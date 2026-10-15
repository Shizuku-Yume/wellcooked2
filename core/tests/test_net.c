/*
 * test_net.c -- the log and net suites, driven without the game and without the
 * Bridge: a plain socket()/connect() peer plays the plugin and an ephemeral port
 * plays the classroom link.
 *
 * What is proven here (numbered as in the task):
 *   1. LISTEN mode on port 0 -- ephemeral port, several frames both ways,
 *      byte-identical payloads, one frame split over two send()s, two frames in
 *      one buffer, and a reconnecting client replacing a dead session.
 *   2. A zero-length frame is an empty *message*: it is delivered with len == 0
 *      and the connection stays up (the caller's parser rejects it, as
 *      TcpLink.cs and the framing contract both imply), and the same connection
 *      still carries the next frame. A length prefix above 1 MiB is the real
 *      desync: it drops the link to DOWN, keeps the listener alive and stays
 *      recoverable.
 *   3. CONNECT mode against a closed port: wait() keeps answering 0, retries
 *      happen, and the link comes up once a listener appears.
 *   4. Peer close is detected as DOWN and a reconnect in LISTEN mode succeeds.
 *   5. The log module: mkdir -p, format, level filtering, one line per event,
 *      and a line that survives SIGKILL (no buffering to lose).
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <kitchenos/framing.h>
#include <kitchenos/log.h>
#include <kitchenos/net.h>

/* ------------------------------------------------------------------ checks */

static int g_checks;
static int g_failures;

static void check_impl(int ok, const char *expr, int line, const char *fmt, ...)
{
    va_list ap;

    g_checks++;
    if (ok)
        return;
    g_failures++;
    fprintf(stderr, "  FAIL line %d: %s", line, expr);
    if (fmt != NULL) {
        fprintf(stderr, " -- ");
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fprintf(stderr, "\n");
}

#define CHECK(cond) check_impl((cond) ? 1 : 0, #cond, __LINE__, NULL)
#define CHECKF(cond, ...) check_impl((cond) ? 1 : 0, #cond, __LINE__, __VA_ARGS__)

/* ------------------------------------------------------- raw socket helpers */

/* A blocking client socket built from plain POSIX calls: the test must not use
 * kos_net to talk to kos_net, or a bug would cancel itself out. */
static int dial_local(int port)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int listen_local(int port) /* port 0 = let the kernel pick one */
{
    struct sockaddr_in addr;
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int fd_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = (socklen_t)sizeof addr;

    if (fd < 0 || getsockname(fd, (struct sockaddr *)&addr, &len) != 0)
        return -1;
    return (int)ntohs(addr.sin_port);
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

typedef struct {
    int fd;
    framing_decoder *dec;
} raw_peer;

/* 1 = one frame decoded, 0 = nothing within the timeout, -1 = peer gone. */
static int raw_peer_frame(raw_peer *peer, char *buf, size_t cap, size_t *len, int timeout_ms)
{
    for (;;) {
        struct pollfd pfd;
        const char *payload = NULL;
        size_t plen = 0;
        char chunk[4096];
        ssize_t n;
        int rc = framing_decoder_next(peer->dec, &payload, &plen);

        if (rc == FRAMING_OK) {
            if (plen + 1 > cap)
                return -1;
            memcpy(buf, payload, plen);
            buf[plen] = '\0';
            *len = plen;
            return 1;
        }
        if (rc != FRAMING_NEED_MORE)
            return -1;

        pfd.fd = peer->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rc = poll(&pfd, 1, timeout_ms);
        if (rc == 0)
            return 0;
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        n = read(peer->fd, chunk, sizeof chunk);
        if (n == 0)
            return -1;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        if (framing_decoder_feed(peer->dec, chunk, (size_t)n) != FRAMING_OK)
            return -1;
    }
}

/* -------------------------------------------------------- net-side drivers */

/* Drives wait()+recv_frame() until the link state matches, at most ~ms. */
static int wait_for_state(kos_net *net, kos_net_state want, int ms)
{
    char buf[65536];
    size_t len = 0;
    int waited = 0;

    while (kos_net_state_of(net) != want && waited < ms) {
        if (kos_net_wait(net, 20) != 0)
            return 0;
        (void)kos_net_recv_frame(net, buf, sizeof buf, &len);
        waited += 20;
    }
    return kos_net_state_of(net) == want;
}

/* 1 = one frame, 0 = timeout, -1 = fatal or the link went down. */
static int net_take_frame(kos_net *net, char *buf, size_t cap, size_t *len, int ms)
{
    int waited = 0;

    *len = 0;
    while (waited < ms) {
        int rc = kos_net_recv_frame(net, buf, cap, len);

        if (rc == 1)
            return 1;
        if (rc < 0)
            return -1;
        if (kos_net_wait(net, 20) != 0)
            return -1;
        waited += 20;
    }
    return 0;
}

/* -------------------------------------------------------------- 5. the log */

static int ts_shape_ok(const char *line)
{
    static const char pattern[24] = "dddd-dd-ddTdd:dd:dd.ddd";
    int i;

    for (i = 0; i < 23; i++) {
        char c = line[i];

        if (pattern[i] == 'd') {
            if (c < '0' || c > '9')
                return 0;
        } else if (c != pattern[i]) {
            return 0;
        }
    }
    return 1;
}

/* The level field occupies five columns starting at offset 24, left-aligned:
 * "INFO " for a name of four characters, "ERROR" for five. */
static int level_field_ok(const char *line)
{
    static const char *const names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    char field[6];
    int i;

    if (line[23] != ' ' || line[29] != ' ')
        return 0;
    memcpy(field, line + 24, 5);
    field[5] = '\0';
    for (i = 0; i < 4; i++) {
        char padded[6];

        snprintf(padded, sizeof padded, "%-5s", names[i]);
        if (memcmp(field, padded, 5) == 0)
            return 1;
    }
    return 0;
}

/* The timestamp must be local time (mktime round-trips it) and close to now. */
static int ts_close_to_now(const char *line, int slack_s)
{
    struct tm tm;
    time_t parsed;
    int y, mo, d, h, mi, s, ms;

    if (sscanf(line, "%4d-%2d-%2dT%2d:%2d:%2d.%3d", &y, &mo, &d, &h, &mi, &s, &ms) != 7)
        return 0;
    if (ms < 0 || ms > 999)
        return 0;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    parsed = mktime(&tm);
    if (parsed == (time_t)-1)
        return 0;
    return labs((long)(time(NULL) - parsed)) <= slack_s;
}

static size_t read_file(const char *path, char *buf, size_t cap)
{
    FILE *fp = fopen(path, "r");
    size_t got;

    if (fp == NULL)
        return 0;
    got = fread(buf, 1, cap - 1, fp);
    buf[got] = '\0';
    fclose(fp);
    return got;
}

static int count_newlines(const char *text)
{
    int n = 0;

    for (; *text != '\0'; text++)
        if (*text == '\n')
            n++;
    return n;
}

static void test_log_module(void)
{
    char dir[] = "/tmp/kos_log_test_XXXXXX";
    char path[256];
    char second[256];
    char nested[256];
    char text[8192];
    kos_error err;
    kos_log *log = NULL;
    kos_log *tmp = NULL;
    size_t got;
    int lines;

    memset(&err, 0, sizeof err);
    if (mkdtemp(dir) == NULL) {
        CHECKF(0, "mkdtemp(%s): %s", dir, strerror(errno));
        return;
    }
    snprintf(path, sizeof path, "%s/deep/nested/bridge.log", dir);
    snprintf(second, sizeof second, "%s/second.log", dir);
    snprintf(nested, sizeof nested, "%s/deep/nested", dir);

    /* failures must be return codes, and must never hand back a handle */
    CHECK(kos_log_open(NULL, KOS_LOG_INFO, &tmp, &err) == KOS_ERR_INVAL);
    CHECK(tmp == NULL);
    CHECK(kos_log_open(path, (kos_log_level)99, &tmp, &err) == KOS_ERR_INVAL);
    CHECK(kos_log_mirror_stderr(NULL, 1) == KOS_ERR_INVAL);
    /* /dev/null is a file, so no directory can be created under it: the failure
     * has to be reported with an errno, not swallowed */
    memset(&err, 0, sizeof err);
    CHECK(kos_log_open("/dev/null/nested/bridge.log", KOS_LOG_INFO, &tmp, &err) == KOS_ERR_IO);
    CHECK(tmp == NULL);
    CHECKF(err.sys_errno != 0 && err.context[0] != '\0', "no errno/context: code=%d errno=%d \"%s\"",
           err.code, err.sys_errno, err.context);
    kos_log_close(NULL);
    kos_log_write(NULL, KOS_LOG_ERROR, "noop", "x=%d", 1);

    CHECKF(kos_log_open(path, KOS_LOG_INFO, &log, &err) == KOS_OK, "open: %s", err.context);
    CHECK(log != NULL);
    if (log == NULL)
        return;

    /* the directory part of the path must be created, not demanded */
    {
        struct stat st;

        CHECKF(stat(nested, &st) == 0 && S_ISDIR(st.st_mode),
               "mkdir -p did not create %s", nested);
    }

    /* the mirror is what a foreground run shows; the lines below land in the
     * make output as well as in the file */
    CHECK(kos_log_mirror_stderr(log, 1) == KOS_OK);

    kos_log_debug(log, "hidden_debug", "n=%d", 1); /* below min_level: dropped */
    kos_log_info(log, "bridge_start", "pid=%d link=%s port=%d", (int)getpid(), "listen", 12345);
    kos_log_warn(log, "input_slot_missing", "frame=%llu chef=%d", (unsigned long long)7, 3);
    kos_log_error(log, "json_parse_failed", "reason=%s", "expected value");
    /* newlines inside a message must not become extra events */
    kos_log_write(log, KOS_LOG_ERROR, "multi_line", "first\nsecond\rthird");
    kos_log_close(log);

    got = read_file(path, text, sizeof text);
    CHECKF(got > 0, "log file is empty");
    lines = count_newlines(text);
    CHECKF(lines == 4, "expected one line per event (4), got %d", lines);
    CHECKF(strstr(text, "hidden_debug") == NULL, "min_level did not filter DEBUG out");
    CHECKF(strstr(text, "bridge_start pid=") != NULL, "the start event is missing");
    CHECKF(strstr(text, "multi_line first second third") != NULL,
           "newlines were not folded into spaces");
    CHECKF(got > 0 && text[got - 1] == '\n', "the file does not end with a newline");

    {
        const char *line = text;
        const char *nl;
        int idx = 0;

        while (line != NULL && *line != '\0') {
            size_t line_len = strcspn(line, "\n");

            CHECKF(line_len >= 30, "line %d is too short: \"%s\"", idx, line);
            CHECKF(ts_shape_ok(line), "line %d has no YYYY-MM-DDTHH:MM:SS.mmm timestamp: %.23s",
                   idx, line);
            CHECKF(ts_close_to_now(line, 10), "line %d is not local time near now: %.23s",
                   idx, line);
            CHECKF(line[23] == ' ', "line %d has no separator after the timestamp", idx);
            CHECKF(level_field_ok(line), "line %d level field is not 5 left-aligned columns: \"%.6s\"",
                   idx, line + 23);
            idx++;
            nl = strchr(line, '\n');
            line = nl != NULL ? nl + 1 : NULL;
        }
        CHECKF(idx == 4, "expected 4 lines, walked %d", idx);
    }

    /* the exact shape the spec asks for, on the first line */
    {
        static const char expect[] = " INFO  bridge_start pid=";
        const char *nl = strchr(text, '\n');
        size_t head_len = nl != NULL ? (size_t)(nl - text) : strlen(text);
        char head[512];

        CHECK(head_len < sizeof head);
        if (head_len < sizeof head) {
            memcpy(head, text, head_len);
            head[head_len] = '\0';
            CHECKF(head_len > 23 && strncmp(head + 23, expect, sizeof expect - 1) == 0,
                   "first line is not \"<ts>%s...\": %s", expect, head);
        }
    }

    /* filtering at ERROR keeps exactly the error event */
    CHECKF(kos_log_open(second, KOS_LOG_ERROR, &log, &err) == KOS_OK, "open: %s", err.context);
    if (log != NULL) {
        kos_log_info(log, "filtered_out", "n=%d", 1);
        kos_log_error(log, "kept", "n=%d", 2);
        kos_log_close(log);
        got = read_file(second, text, sizeof text);
        CHECK(count_newlines(text) == 1);
        CHECK(strstr(text, "filtered_out") == NULL);
        CHECKF(strstr(text, "kept n=2") != NULL, "the ERROR event did not survive: %s", text);
    }

    /* level names, for the Bridge's --log-level parsing */
    CHECK(strcmp(kos_log_level_name(KOS_LOG_WARN), "WARN") == 0);
    CHECK(strcmp(kos_log_level_name(KOS_LOG_ERROR), "ERROR") == 0);
    CHECK(kos_log_level_from_name("debug") == KOS_LOG_DEBUG);
    CHECK(kos_log_level_from_name("Error") == KOS_LOG_ERROR);
    CHECK(kos_log_level_from_name("nonsense") == -1);
    CHECK(kos_log_level_from_name(NULL) == -1);

    (void)unlink(path);
    (void)unlink(second);
    (void)rmdir(nested);
    {
        char deep[256];

        snprintf(deep, sizeof deep, "%s/deep", dir);
        (void)rmdir(deep);
    }
    (void)rmdir(dir);
}

/* A killed process must leave the log complete: one write() per event means
 * there is no buffer to lose. */
static void test_log_survives_sigkill(void)
{
    char dir[] = "/tmp/kos_log_kill_XXXXXX";
    char path[256];
    char text[4096];
    pid_t pid;
    int status = 0;
    int waited = 0;
    int seen = 0;

    if (mkdtemp(dir) == NULL) {
        CHECKF(0, "mkdtemp(%s): %s", dir, strerror(errno));
        return;
    }
    snprintf(path, sizeof path, "%s/killed.log", dir);

    pid = fork();
    if (pid < 0) {
        CHECKF(0, "fork: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        kos_log *child_log = NULL;
        kos_error child_err;
        struct timespec nap = { 1, 0 };

        memset(&child_err, 0, sizeof child_err);
        if (kos_log_open(path, KOS_LOG_DEBUG, &child_log, &child_err) != KOS_OK)
            _exit(2);
        kos_log_info(child_log, "written_before_sigkill", "pid=%d", (int)getpid());
        for (;;)
            nanosleep(&nap, NULL); /* no flush, no close: the parent kills us */
    }

    while (waited < 2000 && !seen) {
        struct timespec nap = { 0, 20 * 1000 * 1000 };

        nanosleep(&nap, NULL);
        waited += 20;
        if (read_file(path, text, sizeof text) > 0 && strstr(text, "written_before_sigkill") != NULL)
            seen = 1;
    }
    CHECKF(kill(pid, SIGKILL) == 0, "kill: %s", strerror(errno));
    CHECKF(waitpid(pid, &status, 0) == pid, "waitpid: %s", strerror(errno));
    CHECKF(seen, "the event was not on disk before the process was killed");

    (void)read_file(path, text, sizeof text);
    CHECKF(strstr(text, "written_before_sigkill") != NULL,
           "the line did not survive SIGKILL: \"%s\"", text);
    CHECKF(strlen(text) > 0 && text[strlen(text) - 1] == '\n',
           "the surviving line is not terminated");

    (void)unlink(path);
    (void)rmdir(dir);
}

/* ------------------------------------------------------- 1. LISTEN exchange */

static void test_listen_exchange(void)
{
    static const char *inbound[] = { "{\"t\":\"hello\"}", "{\"t\":\"one\"}", "{\"t\":\"two\"}" };
    static const char *outbound[] = { "{\"t\":\"action\"}", "{\"t\":\"idle\",\"chef\":3}" };
    kos_net_opts opts;
    kos_net *net = NULL;
    kos_error err;
    raw_peer peer;
    char buf[4096];
    size_t len = 0;
    int port, client, c2, old_peer, i, rc;

    memset(&err, 0, sizeof err);
    kos_net_opts_init(&opts);
    opts.port = 0; /* ephemeral: the suite must not fight for a fixed port */
    opts.backlog = 8;
    CHECKF(kos_net_open(&opts, &net, &err) == KOS_OK, "open: %s", err.context);
    if (net == NULL)
        return;

    CHECK(kos_net_listen_fd(net) >= 0);
    port = fd_port(kos_net_listen_fd(net));
    printf("  LISTEN mode bound ephemeral port %d\n", port);
    CHECKF(port > 0, "port 0 did not turn into a real port (%d)", port);

    /* nobody connected yet: wait times out, the state stays DOWN, no fatal */
    for (i = 0; i < 3; i++)
        CHECKF(kos_net_wait(net, 10) == 0, "wait went fatal with no peer");
    CHECK(kos_net_state_of(net) == KOS_NET_DOWN);
    CHECK(kos_net_recv_frame(net, buf, sizeof buf, &len) == 0);
    CHECK(kos_net_send_frame(net, "{}", 2) == -1);

    client = dial_local(port);
    CHECKF(client >= 0, "dial 127.0.0.1:%d: %s", port, strerror(errno));
    if (client < 0) {
        kos_net_close(net);
        return;
    }
    CHECKF(wait_for_state(net, KOS_NET_UP, 2000), "the accepted peer never came up");
    CHECK(kos_net_peer_fd(net) >= 0);

    /* one frame split across two send() calls (the "split" case) */
    {
        unsigned char enc[256];
        size_t enc_len = 0;

        CHECK(framing_encode(inbound[0], strlen(inbound[0]), enc, sizeof enc, &enc_len) == FRAMING_OK);
        CHECK(write_all(client, enc, 3) == 0);
        CHECK(write_all(client, enc + 3, enc_len - 3) == 0);
    }
    /* two frames in one buffer (the "coalesced" case) */
    {
        unsigned char enc[512];
        size_t total = 0;
        size_t one = 0;

        CHECK(framing_encode(inbound[1], strlen(inbound[1]), enc, sizeof enc, &one) == FRAMING_OK);
        total = one;
        CHECK(framing_encode(inbound[2], strlen(inbound[2]), enc + total, sizeof enc - total,
                             &one) == FRAMING_OK);
        total += one;
        CHECK(write_all(client, enc, total) == 0);
    }

    for (i = 0; i < 3; i++) {
        rc = net_take_frame(net, buf, sizeof buf, &len, 2000);

        CHECKF(rc == 1, "frame %d never arrived (rc=%d)", i, rc);
        if (rc == 1)
            CHECKF(len == strlen(inbound[i]) && memcmp(buf, inbound[i], len) == 0,
                   "frame %d is not byte-identical: got \"%s\"", i, buf);
    }
    CHECK(kos_net_recv_frame(net, buf, sizeof buf, &len) == 0);

    /* the other direction: what net sends, the raw peer must decode identically */
    peer.fd = client;
    peer.dec = framing_decoder_new();
    CHECK(peer.dec != NULL);
    if (peer.dec != NULL) {
        for (i = 0; i < 2; i++) {
            CHECK(kos_net_send_frame(net, outbound[i], strlen(outbound[i])) == 0);
            rc = raw_peer_frame(&peer, buf, sizeof buf, &len, 2000);
            CHECKF(rc == 1, "the peer did not decode outbound frame %d (rc=%d)", i, rc);
            if (rc == 1)
                CHECKF(len == strlen(outbound[i]) && memcmp(buf, outbound[i], len) == 0,
                       "outbound frame %d differs: got \"%s\"", i, buf);
        }
        framing_decoder_free(peer.dec);
    }

    /* a second client replaces the first: the plugin that reconnects wins, and
     * the session that nobody is using is dropped instead of locking it out */
    old_peer = kos_net_peer_fd(net);
    c2 = dial_local(port);
    CHECK(c2 >= 0);
    if (c2 >= 0) {
        for (i = 0; i < 100 && kos_net_peer_fd(net) == old_peer; i++)
            CHECK(kos_net_wait(net, 20) == 0);
        CHECKF(kos_net_peer_fd(net) != old_peer, "the second connection was not adopted");
        CHECK(kos_net_state_of(net) == KOS_NET_UP);
        {
            unsigned char enc[256];
            size_t enc_len = 0;
            static const char late[] = "{\"t\":\"from_second_client\"}";

            CHECK(framing_encode(late, strlen(late), enc, sizeof enc, &enc_len) == FRAMING_OK);
            CHECK(write_all(c2, enc, enc_len) == 0);
            rc = net_take_frame(net, buf, sizeof buf, &len, 2000);
            CHECKF(rc == 1, "the new session delivered nothing (rc=%d)", rc);
            if (rc == 1)
                CHECKF(len == strlen(late) && memcmp(buf, late, len) == 0, "got \"%s\"", buf);
        }
        close(c2);
    }

    close(client);
    kos_net_close(net);
}

/* --------------------------------- 2a. a zero-length frame is a bad message */

/*
 * The two failure classes must not be confused: a malformed *message* is passed
 * up and the connection lives, while only a lost *stream* costs the session. The
 * reference peer (TcpLink.cs) rejects nothing but out-of-range lengths, so an
 * empty frame reaches its JSON layer on a perfectly healthy connection -- the
 * Bridge must see the same thing.
 */
static void test_zero_length_frame_is_delivered(void)
{
    kos_net_opts opts;
    kos_net *net = NULL;
    kos_error err;
    raw_peer peer;
    char buf[4096];
    size_t len = 123;
    int port, client, rc;

    memset(&err, 0, sizeof err);
    kos_net_opts_init(&opts);
    opts.port = 0;
    CHECKF(kos_net_open(&opts, &net, &err) == KOS_OK, "open: %s", err.context);
    if (net == NULL)
        return;
    port = fd_port(kos_net_listen_fd(net));
    CHECK(port > 0);

    client = dial_local(port);
    CHECK(client >= 0);
    CHECKF(wait_for_state(net, KOS_NET_UP, 2000), "no link for the zero-length case");

    {
        unsigned char zero[FRAMING_HEADER_LEN] = { 0, 0, 0, 0 };

        CHECK(write_all(client, zero, sizeof zero) == 0);
    }

    /* it must come up as a frame of length 0 ... */
    rc = net_take_frame(net, buf, sizeof buf, &len, 2000);
    CHECKF(rc == 1, "a zero-length frame was not delivered (rc=%d)", rc);
    if (rc == 1) {
        CHECKF(len == 0, "expected a frame of length 0, got %zu", len);
        CHECKF(buf[0] == '\0', "the empty frame is not an empty C string");
    }

    /* ... on a connection that is still there */
    CHECKF(kos_net_state_of(net) == KOS_NET_UP, "the empty frame dropped the link: %s",
           kos_net_last_error(net));
    CHECK(kos_net_peer_fd(net) >= 0);
    CHECKF(kos_net_last_error(net)[0] == '\0', "the empty frame recorded an error: %s",
           kos_net_last_error(net));
    printf("  zero-length frame -> delivered as len=0, link still UP\n");

    /* and the same connection keeps carrying frames */
    {
        static const char next[] = "{\"t\":\"after_empty\"}";
        unsigned char enc[256];
        size_t enc_len = 0;

        CHECK(framing_encode(next, strlen(next), enc, sizeof enc, &enc_len) == FRAMING_OK);
        CHECK(write_all(client, enc, enc_len) == 0);
        rc = net_take_frame(net, buf, sizeof buf, &len, 2000);
        CHECKF(rc == 1, "the frame after the empty one never arrived (rc=%d)", rc);
        if (rc == 1)
            CHECKF(len == strlen(next) && memcmp(buf, next, len) == 0, "got \"%s\"", buf);
    }

    /* sending an empty frame is symmetric: the peer decodes a frame of length 0 */
    peer.fd = client;
    peer.dec = framing_decoder_new();
    CHECK(peer.dec != NULL);
    if (peer.dec != NULL) {
        CHECK(kos_net_send_frame(net, "", 0) == 0);
        rc = raw_peer_frame(&peer, buf, sizeof buf, &len, 2000);
        CHECKF(rc == 1, "the peer did not decode our empty frame (rc=%d)", rc);
        if (rc == 1)
            CHECKF(len == 0, "the peer saw %zu bytes, expected 0", len);
        framing_decoder_free(peer.dec);
    }

    close(client);
    kos_net_close(net);
}

/* ------------------------------------------ 2b. a real desync costs the link */

static void test_desync_drops_link(void)
{
    kos_net_opts opts;
    kos_net *net = NULL;
    kos_error err;
    char buf[4096];
    size_t len = 0;
    unsigned char bogus[FRAMING_HEADER_LEN];
    int port, client, i, rc;

    memset(&err, 0, sizeof err);
    kos_net_opts_init(&opts);
    opts.port = 0;
    CHECKF(kos_net_open(&opts, &net, &err) == KOS_OK, "open: %s", err.context);
    if (net == NULL)
        return;
    port = fd_port(kos_net_listen_fd(net));
    CHECK(port > 0);

    /* a length prefix above the 1 MiB cap is a lost stream: no resynchronization
     * is possible, so the session is over */
    client = dial_local(port);
    CHECK(client >= 0);
    CHECKF(wait_for_state(net, KOS_NET_UP, 2000), "no link for the oversize case");
    bogus[0] = 0x00;
    bogus[1] = 0x40; /* 0x00400000 = 4 MiB */
    bogus[2] = 0x00;
    bogus[3] = 0x00;
    CHECK(write_all(client, bogus, sizeof bogus) == 0);
    CHECKF(wait_for_state(net, KOS_NET_DOWN, 1000), "a 4 MiB prefix did not drop the link");
    CHECK(kos_net_peer_fd(net) == -1);
    CHECK(kos_net_listen_fd(net) >= 0);
    CHECKF(kos_net_last_error(net)[0] != '\0', "the drop left no reason behind");
    printf("  4 MiB length prefix -> DOWN: %s\n", kos_net_last_error(net));
    CHECK(kos_net_recv_frame(net, buf, sizeof buf, &len) == 0); /* nothing, no crash */
    for (i = 0; i < 3; i++)
        CHECK(kos_net_wait(net, 10) == 0); /* and no spin */
    close(client);

    /* and the port is still usable afterwards */
    client = dial_local(port);
    CHECK(client >= 0);
    CHECKF(wait_for_state(net, KOS_NET_UP, 2000), "the listener did not survive the desync");
    {
        static const char good[] = "{\"t\":\"after_desync\"}";
        unsigned char enc[256];
        size_t enc_len = 0;

        CHECK(framing_encode(good, strlen(good), enc, sizeof enc, &enc_len) == FRAMING_OK);
        CHECK(write_all(client, enc, enc_len) == 0);
        rc = net_take_frame(net, buf, sizeof buf, &len, 2000);
        CHECKF(rc == 1, "the link did not recover after the desync (rc=%d)", rc);
        if (rc == 1)
            CHECKF(len == strlen(good) && memcmp(buf, good, len) == 0, "got \"%s\"", buf);
    }
    close(client);
    kos_net_close(net);
}

/* ---------------------------------------------- 3. CONNECT retry behaviour */

static void test_connect_retries_until_listener_appears(void)
{
    kos_net_opts opts;
    kos_net *net = NULL;
    kos_error err;
    raw_peer peer;
    char buf[4096];
    size_t len = 0;
    int reserved, srv, port, i, rc, peer_fd;

    /* Reserve a port and close it again: this is "the game is not running yet",
     * and the reserved number is what the listener below will use. */
    reserved = listen_local(0);
    CHECK(reserved >= 0);
    if (reserved < 0)
        return;
    port = fd_port(reserved);
    close(reserved);
    CHECKF(port > 0, "no ephemeral port could be reserved");

    memset(&err, 0, sizeof err);
    kos_net_opts_init(&opts);
    opts.mode = KOS_NET_MODE_CONNECT;
    opts.host = "127.0.0.1";
    opts.port = port;
    opts.reconnect_delay_ms = 50;
    CHECKF(kos_net_open(&opts, &net, &err) == KOS_OK, "open: %s", err.context);
    if (net == NULL)
        return;
    CHECK(kos_net_listen_fd(net) == -1); /* CONNECT mode listens on nothing */

    /* Refused dials are a state, not an exit: wait keeps answering 0. */
    for (i = 0; i < 12; i++)
        CHECKF(kos_net_wait(net, 20) == 0, "wait went fatal while the port was closed");
    CHECKF(kos_net_state_of(net) == KOS_NET_DOWN, "state=%d", (int)kos_net_state_of(net));
    CHECKF(kos_net_last_error(net)[0] != '\0', "a refused dial left no reason behind");
    printf("  CONNECT to a closed port (still retrying): %s\n", kos_net_last_error(net));
    CHECK(kos_net_recv_frame(net, buf, sizeof buf, &len) == 0);
    CHECK(kos_net_send_frame(net, "{}", 2) == -1);
    CHECK(kos_net_peer_fd(net) == -1);

    /* now the game starts listening on that port */
    srv = listen_local(port);
    CHECKF(srv >= 0, "bind 127.0.0.1:%d after the probe: %s", port, strerror(errno));
    if (srv < 0) {
        kos_net_close(net);
        return;
    }
    CHECKF(wait_for_state(net, KOS_NET_UP, 3000), "the retry never reached the new listener");
    CHECK(kos_net_peer_fd(net) >= 0);
    CHECKF(kos_net_last_error(net)[0] == '\0', "a stale error survived the connect: %s",
           kos_net_last_error(net));

    peer_fd = accept(srv, NULL, NULL);
    CHECKF(peer_fd >= 0, "accept: %s", strerror(errno));
    if (peer_fd >= 0) {
        peer.fd = peer_fd;
        peer.dec = framing_decoder_new();
        if (peer.dec != NULL) {
            static const char back[] = "{\"t\":\"state_update\"}";
            unsigned char enc[256];
            size_t enc_len = 0;

            CHECK(framing_encode(back, strlen(back), enc, sizeof enc, &enc_len) == FRAMING_OK);
            CHECK(write_all(peer_fd, enc, enc_len) == 0);
            rc = net_take_frame(net, buf, sizeof buf, &len, 2000);
            CHECKF(rc == 1, "nothing arrived over the connected link (rc=%d)", rc);
            if (rc == 1)
                CHECKF(len == strlen(back) && memcmp(buf, back, len) == 0, "got \"%s\"", buf);

            CHECK(kos_net_send_frame(net, back, strlen(back)) == 0);
            rc = raw_peer_frame(&peer, buf, sizeof buf, &len, 2000);
            CHECKF(rc == 1, "the sent frame did not reach the listener (rc=%d)", rc);
            framing_decoder_free(peer.dec);
        }
        /* the peer closing must be detected and the retry must resume */
        close(peer_fd);
        CHECKF(wait_for_state(net, KOS_NET_DOWN, 2000), "closing the peer did not drop the link");
        CHECK(kos_net_state_of(net) == KOS_NET_DOWN);
        for (i = 0; i < 4; i++)
            CHECKF(kos_net_wait(net, 20) == 0, "wait went fatal after the drop");
    }
    kos_net_close(net);
    close(srv);
}

/* --------------------------------------- 4. disconnect detection + reconnect */

static void test_peer_close_then_reconnect(void)
{
    kos_net_opts opts;
    kos_net *net = NULL;
    kos_error err;
    char buf[4096];
    size_t len = 0;
    int port, client, c2, old_peer, i, rc;

    memset(&err, 0, sizeof err);
    kos_net_opts_init(&opts);
    opts.port = 0;
    CHECKF(kos_net_open(&opts, &net, &err) == KOS_OK, "open: %s", err.context);
    if (net == NULL)
        return;
    port = fd_port(kos_net_listen_fd(net));
    CHECK(port > 0);

    client = dial_local(port);
    CHECK(client >= 0);
    CHECKF(wait_for_state(net, KOS_NET_UP, 2000), "the first peer never came up");

    /* a working exchange first, so the drop below is a change and not a state
     * the link was in all along */
    {
        static const char first[] = "{\"t\":\"before_close\"}";
        unsigned char enc[256];
        size_t enc_len = 0;

        CHECK(framing_encode(first, strlen(first), enc, sizeof enc, &enc_len) == FRAMING_OK);
        CHECK(write_all(client, enc, enc_len) == 0);
        rc = net_take_frame(net, buf, sizeof buf, &len, 2000);
        CHECKF(rc == 1, "the exchange before the close failed (rc=%d)", rc);
    }

    close(client); /* what the plugin does when the game exits */
    CHECKF(wait_for_state(net, KOS_NET_DOWN, 2000), "the close was not detected");
    CHECK(kos_net_peer_fd(net) == -1);
    CHECKF(kos_net_listen_fd(net) >= 0, "the listener died with the peer");
    CHECKF(fd_port(kos_net_listen_fd(net)) == port, "the listener lost its port");
    CHECKF(kos_net_last_error(net)[0] != '\0', "the drop left no reason behind");
    printf("  peer closed -> DOWN: %s\n", kos_net_last_error(net));

    /* the plugin coming back must work without reopening anything */
    client = dial_local(port);
    CHECKF(client >= 0, "reconnect to 127.0.0.1:%d: %s", port, strerror(errno));
    CHECKF(wait_for_state(net, KOS_NET_UP, 2000), "the plugin could not reconnect");
    {
        static const char again[] = "{\"t\":\"after_reconnect\"}";
        unsigned char enc[256];
        size_t enc_len = 0;

        CHECK(framing_encode(again, strlen(again), enc, sizeof enc, &enc_len) == FRAMING_OK);
        CHECK(write_all(client, enc, enc_len) == 0);
        rc = net_take_frame(net, buf, sizeof buf, &len, 2000);
        CHECKF(rc == 1, "the reconnected session delivered nothing (rc=%d)", rc);
        if (rc == 1)
            CHECKF(len == strlen(again) && memcmp(buf, again, len) == 0, "got \"%s\"", buf);
    }

    /* a reconnect that races the old socket must still win */
    old_peer = kos_net_peer_fd(net);
    c2 = dial_local(port);
    CHECK(c2 >= 0);
    if (c2 >= 0) {
        for (i = 0; i < 100 && kos_net_peer_fd(net) == old_peer; i++)
            CHECK(kos_net_wait(net, 20) == 0);
        CHECKF(kos_net_peer_fd(net) != old_peer, "the racing reconnect was not adopted");
        close(c2);
    }

    close(client);
    kos_net_close(net);
}

/* A peer that vanishes without warning must cost a return code, not the process:
 * this is the path where a naive send() raises SIGPIPE and kills the Bridge. */
static void test_send_to_gone_peer_is_not_fatal(void)
{
    kos_net_opts opts;
    kos_net *net = NULL;
    kos_error err;
    char buf[256];
    size_t len = 0;
    int port, client, i, rc = 0;

    memset(&err, 0, sizeof err);
    kos_net_opts_init(&opts);
    opts.port = 0;
    CHECKF(kos_net_open(&opts, &net, &err) == KOS_OK, "open: %s", err.context);
    if (net == NULL)
        return;
    port = fd_port(kos_net_listen_fd(net));
    client = dial_local(port);
    CHECK(client >= 0);
    if (client < 0) {
        kos_net_close(net);
        return;
    }
    CHECKF(wait_for_state(net, KOS_NET_UP, 2000), "no link to send over");

    /* Hard reset (SO_LINGER 0) instead of a polite FIN, so the next write really
     * does hit a reset connection rather than a clean shutdown. No wait() happens
     * between the sends: the point is the write path, not the read path. */
    {
        struct linger lin;

        lin.l_onoff = 1;
        lin.l_linger = 0;
        CHECK(setsockopt(client, SOL_SOCKET, SO_LINGER, &lin, (socklen_t)sizeof lin) == 0);
        close(client);
    }
    for (i = 0; i < 50; i++) {
        struct timespec nap = { 0, 5 * 1000 * 1000 };

        rc = kos_net_send_frame(net, "{\"t\":\"into_the_void\"}", 21);
        if (rc == -1)
            break;
        nanosleep(&nap, NULL);
    }
    CHECKF(rc == -1, "sending to a reset peer kept succeeding");
    CHECKF(kos_net_state_of(net) == KOS_NET_DOWN, "a failed send did not drop the link");
    CHECKF(kos_net_last_error(net)[0] != '\0', "the failed send left no reason behind");
    printf("  send to a reset peer -> -1: %s (process alive)\n", kos_net_last_error(net));

    /* the listener is still good: the plugin reconnecting must work */
    client = dial_local(port);
    CHECK(client >= 0);
    CHECKF(wait_for_state(net, KOS_NET_UP, 2000), "the listener did not survive the failed send");
    CHECK(kos_net_recv_frame(net, buf, sizeof buf, &len) == 0);
    close(client);
    kos_net_close(net);
}

int main(void)
{
    printf("test_net: log + net suites\n");

    test_log_module();
    test_log_survives_sigkill();
    test_listen_exchange();
    test_zero_length_frame_is_delivered();
    test_desync_drops_link();
    test_connect_retries_until_listener_appears();
    test_peer_close_then_reconnect();
    test_send_to_gone_peer_is_not_fatal();

    printf("test_net: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
