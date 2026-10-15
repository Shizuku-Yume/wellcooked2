/*
 * net.c -- the poll-driven link state machine described in net.h.
 */
#define _POSIX_C_SOURCE 200809L

#include <kitchenos/net.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <kitchenos/framing.h>

#define KOS_NET_HOST_MAX 128
#define KOS_NET_ERR_MAX 192
#define KOS_NET_READ_CHUNK 8192

/* How much a single wait may pull from the socket before handing the decoder
 * back to recv_frame(): enough for a maximum-size frame plus its prefix, so a
 * peer that is faster than the caller cannot grow the decoder without bound. */
#define KOS_NET_DECODER_LIMIT ((size_t)FRAMING_MAX_PAYLOAD + FRAMING_HEADER_LEN)

struct kos_net {
    kos_net_mode mode;
    kos_net_state state;

    int listen_fd; /* LISTEN only, -1 otherwise */
    int peer_fd;   /* -1 whenever the link is down */
    int backlog;
    int reconnect_delay_ms;
    int port; /* LISTEN: the port actually bound (port 0 becomes a real one) */

    char host[KOS_NET_HOST_MAX]; /* CONNECT: target text, for error messages */
    struct sockaddr_in target;
    int have_target;
    int connecting;      /* CONNECT: a non-blocking connect() is in flight */
    int64_t retry_at_ms; /* CONNECT while down: earliest next dial (monotonic) */

    framing_decoder *decoder;
    unsigned char *encbuf; /* grown once and reused: one send per frame */
    size_t encbuf_cap;

    char last_error[KOS_NET_ERR_MAX];
};

static void net_error(kos_net *net, const char *fmt, ...)
{
    va_list ap;

    if (net == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(net->last_error, sizeof net->last_error, fmt, ap);
    va_end(ap);
}

/* Keeps errno's own words ("Connection refused") next to the call that produced
 * it, which is the part that makes a log line actionable. */
static void net_errno_error(kos_net *net, int err, const char *fmt, ...)
{
    char what[KOS_NET_ERR_MAX];
    va_list ap;

    if (net == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    /* The precisions bound each half so the whole line provably fits the field:
     * a truncated diagnostic is still worth more than a compiler warning. */
    if (err != 0)
        snprintf(net->last_error, sizeof net->last_error, "%.96s: %.56s (errno=%d)",
                 what, strerror(err), err);
    else
        snprintf(net->last_error, sizeof net->last_error, "%.96s", what);
}

/* Monotonic milliseconds. Losing the clock must not wedge the retry timer, so a
 * counter that only moves forward stands in for time. */
static int64_t now_ms(void)
{
    static int64_t fallback_tick;
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
    return ++fallback_tick;
}

static int net_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

/* A best-effort tweak, not a correctness requirement: the action stream is a
 * control loop, and Nagle would hold a frame back behind the previous ACK. */
static void net_set_nodelay(int fd)
{
    int one = 1;

    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, (socklen_t)sizeof one);
}

static void net_close_peer_fd(kos_net *net)
{
    if (net->peer_fd >= 0) {
        close(net->peer_fd);
        net->peer_fd = -1;
    }
}

static void net_schedule_retry(kos_net *net)
{
    net->retry_at_ms = now_ms() + (int64_t)net->reconnect_delay_ms;
}

/*
 * Every ending of a session funnels through here, so "the link is gone" has one
 * meaning and one set of side effects wherever it comes from. `why` is stored
 * as the reason the link is down; NULL keeps the message the caller already
 * wrote (for instance the errno text from the failed read).
 */
static void net_drop(kos_net *net, const char *why)
{
    if (net == NULL)
        return;
    net_close_peer_fd(net);
    framing_decoder_reset(net->decoder);
    net->state = KOS_NET_DOWN;
    net->connecting = 0;
    if (net->mode == KOS_NET_MODE_CONNECT)
        net_schedule_retry(net);
    if (why != NULL)
        net_error(net, "%s", why);
}

/* A closed socket must surface as EPIPE from send(), not as a signal that kills
 * the Bridge. MSG_NOSIGNAL covers the socket writes; the disposition covers any
 * other write to a pipe whose reader went away. */
static void net_ignore_sigpipe(void)
{
    static int done;
    struct sigaction sa;

    if (done)
        return;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa, NULL) == 0)
        done = 1;
}

void kos_net_opts_init(kos_net_opts *opts)
{
    if (opts == NULL)
        return;
    memset(opts, 0, sizeof *opts);
    opts->mode = KOS_NET_MODE_LISTEN;
    opts->host = NULL;
    opts->port = KOS_NET_DEFAULT_PORT;
    opts->bind_addr = KOS_NET_DEFAULT_BIND_ADDR;
    opts->backlog = KOS_NET_DEFAULT_BACKLOG;
    opts->reconnect_delay_ms = KOS_NET_DEFAULT_RECONNECT_MS;
}

static void err_set(kos_error *err, int code, int sys_errno, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL)
        return;
    err->code = code;
    err->sys_errno = sys_errno;
    va_start(ap, fmt);
    vsnprintf(err->context, sizeof err->context, fmt, ap);
    va_end(ap);
}

static int net_open_listen(kos_net *net, const kos_net_opts *opts, kos_error *err)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct sockaddr_in bound;
    socklen_t bound_len = (socklen_t)sizeof bound;
    const char *node = (opts->bind_addr != NULL && opts->bind_addr[0] != '\0')
                           ? opts->bind_addr
                           : KOS_NET_DEFAULT_BIND_ADDR;
    char portbuf[8];
    int one = 1;
    int fd = -1;
    int gai;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; /* the plugin speaks IPv4; TcpLink.cs has no v6 path */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    snprintf(portbuf, sizeof portbuf, "%d", opts->port);

    /* getaddrinfo() rather than inet_pton() so a bind address may also be a name,
     * and so a v6-only name fails with a readable error instead of silently. */
    gai = getaddrinfo(node, portbuf, &hints, &res);
    if (gai != 0) {
        err_set(err, KOS_ERR_IO, 0, "getaddrinfo(%s:%s): %s", node, portbuf, gai_strerror(gai));
        return KOS_ERR_IO;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        int saved_errno = errno;

        freeaddrinfo(res);
        err_set(err, KOS_ERR_IO, saved_errno, "socket(AF_INET, SOCK_STREAM)");
        return KOS_ERR_IO;
    }
    /* SO_REUSEADDR is what lets a restart rebind the port while old connections
     * are still in TIME_WAIT -- the SIGTERM check in the spec depends on it. */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof one) != 0) {
        int saved_errno = errno;

        freeaddrinfo(res);
        close(fd);
        err_set(err, KOS_ERR_IO, saved_errno, "setsockopt(SO_REUSEADDR)");
        return KOS_ERR_IO;
    }
    if (net_set_nonblock(fd) != 0) {
        int saved_errno = errno;

        freeaddrinfo(res);
        close(fd);
        err_set(err, KOS_ERR_IO, saved_errno, "fcntl(O_NONBLOCK) on listen socket");
        return KOS_ERR_IO;
    }
    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
        int saved_errno = errno;

        freeaddrinfo(res);
        close(fd);
        err_set(err, KOS_ERR_IO, saved_errno, "bind(%s:%s)", node, portbuf);
        return KOS_ERR_IO;
    }
    freeaddrinfo(res);
    res = NULL;

    if (listen(fd, net->backlog) != 0) {
        int saved_errno = errno;

        close(fd);
        err_set(err, KOS_ERR_IO, saved_errno, "listen(%s:%s, %d)", node, portbuf, net->backlog);
        return KOS_ERR_IO;
    }
    net_set_nodelay(fd);

    /* Port 0 means "any free port"; remembering the bound one keeps diagnostics
     * and future siblings honest about what is actually open. */
    if (getsockname(fd, (struct sockaddr *)&bound, &bound_len) == 0)
        net->port = (int)ntohs(bound.sin_port);

    net->listen_fd = fd;
    net->state = KOS_NET_DOWN; /* up means "peer attached", which needs an accept */
    net->last_error[0] = '\0';
    return KOS_OK;
}

/*
 * One dial attempt. Failures are recorded and retried later; nothing here can
 * fail the Bridge, which is the whole reason CONNECT mode exists.
 */
static void net_connect_start(kos_net *net)
{
    int fd;
    int rc;

    if (!net->have_target)
        return;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        net_errno_error(net, errno, "socket(AF_INET, SOCK_STREAM)");
        net_schedule_retry(net);
        return;
    }
    if (net_set_nonblock(fd) != 0) {
        net_errno_error(net, errno, "fcntl(O_NONBLOCK) on connect socket");
        close(fd);
        net_schedule_retry(net);
        return;
    }
    net_set_nodelay(fd);

    net->peer_fd = fd;
    net->connecting = 1;
    rc = connect(fd, (struct sockaddr *)&net->target, (socklen_t)sizeof net->target);
    if (rc == 0) {
        net->connecting = 0;
        net->state = KOS_NET_UP;
        net->last_error[0] = '\0';
        return;
    }
    if (errno == EINPROGRESS || errno == EINTR) {
        /* Completion (and its SO_ERROR verdict) belongs to kos_net_wait(). */
        net->state = KOS_NET_DOWN;
        return;
    }

    /* Refused or unreachable is the expected state while the game is closed. */
    net_errno_error(net, errno, "connect(%s:%d)", net->host, net->port);
    net_close_peer_fd(net);
    net->connecting = 0;
    net->state = KOS_NET_DOWN;
    net_schedule_retry(net);
}

static int net_open_connect(kos_net *net, const kos_net_opts *opts, kos_error *err)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char portbuf[8];
    int gai;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portbuf, sizeof portbuf, "%d", opts->port);
    snprintf(net->host, sizeof net->host, "%s", opts->host);

    gai = getaddrinfo(opts->host, portbuf, &hints, &res);
    if (gai != 0 || res == NULL) {
        err_set(err, KOS_ERR_IO, 0, "getaddrinfo(%s:%s): %s", opts->host, portbuf,
                gai != 0 ? gai_strerror(gai) : "no address");
        if (res != NULL)
            freeaddrinfo(res);
        return KOS_ERR_IO;
    }
    /* The address is kept, not the addrinfo list: a retry an hour from now must
     * not depend on a name server that may have gone away meanwhile. */
    memcpy(&net->target, res->ai_addr, sizeof net->target);
    net->have_target = 1;
    freeaddrinfo(res);

    net->state = KOS_NET_DOWN;
    net->last_error[0] = '\0';
    net_connect_start(net);
    return KOS_OK;
}

int kos_net_open(const kos_net_opts *opts, kos_net **out, kos_error *err)
{
    kos_net *net;
    int rc;

    if (out == NULL) {
        err_set(err, KOS_ERR_INVAL, 0, "kos_net_open: out is NULL");
        return KOS_ERR_INVAL;
    }
    *out = NULL;
    if (opts == NULL) {
        err_set(err, KOS_ERR_INVAL, 0, "kos_net_open: opts is NULL");
        return KOS_ERR_INVAL;
    }
    if (opts->mode != KOS_NET_MODE_LISTEN && opts->mode != KOS_NET_MODE_CONNECT) {
        err_set(err, KOS_ERR_INVAL, 0, "kos_net_open: mode %d is not a kos_net_mode", (int)opts->mode);
        return KOS_ERR_INVAL;
    }
    if (opts->port < 0 || opts->port > 65535) {
        err_set(err, KOS_ERR_INVAL, 0, "kos_net_open: port %d out of range", opts->port);
        return KOS_ERR_INVAL;
    }
    if (opts->mode == KOS_NET_MODE_CONNECT && (opts->host == NULL || opts->host[0] == '\0')) {
        err_set(err, KOS_ERR_INVAL, 0, "kos_net_open: CONNECT mode needs a host");
        return KOS_ERR_INVAL;
    }

    net = calloc(1, sizeof *net);
    if (net == NULL) {
        err_set(err, KOS_ERR_NOMEM, errno, "calloc(kos_net)");
        return KOS_ERR_NOMEM;
    }
    net->listen_fd = -1;
    net->peer_fd = -1;
    net->mode = opts->mode;
    net->state = KOS_NET_DOWN;
    net->backlog = opts->backlog > 0 ? opts->backlog : KOS_NET_DEFAULT_BACKLOG;
    net->reconnect_delay_ms = opts->reconnect_delay_ms > 0 ? opts->reconnect_delay_ms : 0;
    net->port = opts->port;
    net->decoder = framing_decoder_new();
    if (net->decoder == NULL) {
        free(net);
        err_set(err, KOS_ERR_NOMEM, errno, "framing_decoder_new()");
        return KOS_ERR_NOMEM;
    }

    net_ignore_sigpipe();

    if (opts->mode == KOS_NET_MODE_LISTEN)
        rc = net_open_listen(net, opts, err);
    else
        rc = net_open_connect(net, opts, err);
    if (rc != KOS_OK) {
        kos_net_close(net);
        return rc;
    }

    *out = net;
    return KOS_OK;
}

void kos_net_close(kos_net *net)
{
    if (net == NULL)
        return;
    net_close_peer_fd(net);
    if (net->listen_fd >= 0) {
        close(net->listen_fd);
        net->listen_fd = -1;
    }
    framing_decoder_free(net->decoder);
    net->decoder = NULL;
    free(net->encbuf);
    net->encbuf = NULL;
    free(net);
}

kos_net_state kos_net_state_of(const kos_net *net)
{
    return net != NULL ? net->state : KOS_NET_DOWN;
}

int kos_net_peer_fd(const kos_net *net)
{
    /* A socket that is still dialing is not a peer: reporting it while the link
     * is DOWN would invite a caller to poll or send on a connection that does
     * not exist yet. */
    if (net == NULL || net->state != KOS_NET_UP)
        return -1;
    return net->peer_fd;
}

int kos_net_listen_fd(const kos_net *net)
{
    return net != NULL ? net->listen_fd : -1;
}

const char *kos_net_last_error(const kos_net *net)
{
    static const char none[] = "";

    return net != NULL ? net->last_error : none;
}

void kos_net_drop(kos_net *net)
{
    if (net == NULL)
        return;
    net_drop(net, "link dropped by the caller");
}

/*
 * Drains whatever the peer already sent into the decoder. EOF, ECONNRESET and
 * friend end the session and return 0; only an unusable decoder is fatal.
 */
static int net_read_peer(kos_net *net)
{
    char chunk[KOS_NET_READ_CHUNK];
    int peer = net->peer_fd;

    if (peer < 0)
        return 0;

    for (;;) {
        ssize_t n;
        int frc;

        /* Stop once a full frame is already buffered: recv_frame() has work to
         * hand out, and this bounds what a desynced or hostile peer can make us
         * hold in memory. */
        if (framing_decoder_buffered(net->decoder) >= KOS_NET_DECODER_LIMIT)
            return 0;

        n = read(peer, chunk, sizeof chunk);
        if (n > 0) {
            frc = framing_decoder_feed(net->decoder, chunk, (size_t)n);
            if (frc != FRAMING_OK) {
                net_error(net, "framing_decoder_feed: rc=%d", frc);
                return -1;
            }
            continue;
        }
        if (n == 0) {
            net_drop(net, "peer closed the connection");
            return 0;
        }
        if (errno == EINTR)
            continue; /* a signal is not this link's business */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0; /* drained */
        /* Any other read error on a TCP socket means this peer is unusable. That
         * is a state change, not a process-level failure. */
        net_errno_error(net, errno, "read(peer fd %d)", peer);
        net_drop(net, NULL);
        return 0;
    }
}

static int net_accept(kos_net *net)
{
    for (;;) {
        int fd = accept(net->listen_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0; /* queue drained */
            /* ECONNABORTED, EMFILE, ENFILE, EPERM: this connection attempt is
             * lost, the listener is not. Never give up the port. */
            net_errno_error(net, errno, "accept(listen fd %d)", net->listen_fd);
            return 0;
        }
        if (net_set_nonblock(fd) != 0) {
            net_errno_error(net, errno, "fcntl(O_NONBLOCK) on accepted fd");
            close(fd);
            continue;
        }
        net_set_nodelay(fd);

        /*
         * One game instance talks to one Bridge, and the newest connection is
         * the one the peer believes in: a plugin that reconnects before we
         * noticed the old socket died would otherwise sit locked out behind a
         * session nobody is using. The decoder is reset because the previous
         * stream's leftover bytes must never be read as this stream's start.
         */
        net_close_peer_fd(net);
        framing_decoder_reset(net->decoder);
        net->peer_fd = fd;
        net->state = KOS_NET_UP;
        net->last_error[0] = '\0';
    }
}

static int net_wait_listen(kos_net *net, int timeout_ms)
{
    struct pollfd fds[2];
    int nfds = 0;
    int i;
    int rc;

    if (net->listen_fd < 0) {
        net_error(net, "the listening socket is not open");
        return -1;
    }
    fds[nfds].fd = net->listen_fd;
    fds[nfds].events = POLLIN;
    fds[nfds].revents = 0;
    nfds++;
    if (net->peer_fd >= 0) {
        fds[nfds].fd = net->peer_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }

    rc = poll(fds, (nfds_t)nfds, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR)
            return 0; /* let the caller see its signal flag */
        net_errno_error(net, errno, "poll(listen fd %d)", net->listen_fd);
        return -1;
    }

    for (i = 0; i < nfds; i++) {
        if (fds[i].revents == 0)
            continue;
        if (fds[i].fd == net->listen_fd) {
            if (fds[i].revents & POLLNVAL) {
                net_error(net, "the listening fd is not open (POLLNVAL)");
                return -1;
            }
            if (fds[i].revents & POLLERR) {
                net_error(net, "the listening socket reported POLLERR");
                return -1;
            }
            if ((fds[i].revents & POLLIN) && net_accept(net) != 0)
                return -1;
        } else if (fds[i].fd == net->peer_fd) {
            /* Guarded by identity: an accept earlier in this loop may already
             * have replaced the peer, and the replaced socket's bytes belong to
             * a session that is over. */
            if (net_read_peer(net) != 0)
                return -1;
        }
    }
    return 0;
}

/* SO_ERROR is the only truthful verdict on a non-blocking connect that poll()
 * reported as writable. */
static int net_connect_finish(kos_net *net)
{
    int soerr = 0;
    socklen_t len = (socklen_t)sizeof soerr;
    char msg[KOS_NET_ERR_MAX];

    if (getsockopt(net->peer_fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0) {
        net_errno_error(net, errno, "getsockopt(SO_ERROR)");
        net_drop(net, NULL);
        return 0;
    }
    if (soerr != 0) {
        snprintf(msg, sizeof msg, "connect(%s:%d): %s (errno=%d)",
                 net->host, net->port, strerror(soerr), soerr);
        net_drop(net, msg); /* also schedules the next attempt */
        return 0;
    }
    net->connecting = 0;
    net->state = KOS_NET_UP;
    net->last_error[0] = '\0';
    return 0;
}

static int net_wait_connect(kos_net *net, int timeout_ms)
{
    struct pollfd pfd;
    int64_t now;
    int64_t sleep_ms;
    int rc;

    if (!net->have_target) {
        net_error(net, "no target address to connect to");
        return -1;
    }

    if (net->state == KOS_NET_UP && net->peer_fd >= 0) {
        pfd.fd = net->peer_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR)
                return 0;
            net_errno_error(net, errno, "poll(peer fd %d)", net->peer_fd);
            return -1;
        }
        if (rc == 0)
            return 0;
        return net_read_peer(net);
    }

    if (net->connecting && net->peer_fd >= 0) {
        pfd.fd = net->peer_fd;
        pfd.events = POLLOUT; /* writable or errored: both mean "the dial settled" */
        pfd.revents = 0;
        rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR)
                return 0;
            net_errno_error(net, errno, "poll(connecting fd %d)", net->peer_fd);
            return -1;
        }
        if (rc == 0)
            return 0; /* still dialing */
        return net_connect_finish(net);
    }

    /* Down and not dialing: the only thing to wait for is the retry deadline. */
    now = now_ms();
    if (now >= net->retry_at_ms) {
        net_connect_start(net);
        return 0;
    }
    sleep_ms = net->retry_at_ms - now;
    if (timeout_ms >= 0 && (int64_t)timeout_ms < sleep_ms)
        sleep_ms = (int64_t)timeout_ms;
    if (sleep_ms > 0) {
        /* A bare poll() is a sleep that still answers to signals, unlike a
         * sleep loop that would hold SIGTERM off for a whole delay. */
        rc = poll(NULL, 0, (int)sleep_ms);
        if (rc < 0 && errno != EINTR) {
            net_errno_error(net, errno, "poll(retry delay)");
            return -1;
        }
    }
    if (now_ms() >= net->retry_at_ms)
        net_connect_start(net);
    return 0;
}

int kos_net_wait(kos_net *net, int timeout_ms)
{
    if (net == NULL)
        return -1;
    if (net->mode == KOS_NET_MODE_LISTEN)
        return net_wait_listen(net, timeout_ms);
    return net_wait_connect(net, timeout_ms);
}

int kos_net_recv_frame(kos_net *net, char *buf, size_t cap, size_t *len)
{
    const char *payload = NULL;
    size_t plen = 0;
    int rc;

    if (len != NULL)
        *len = 0;
    if (net == NULL)
        return -1;
    if (buf == NULL || cap == 0) {
        net_error(net, "recv_frame: buffer is NULL or has no room");
        return -1;
    }
    if (net->state != KOS_NET_UP || net->peer_fd < 0)
        return 0; /* a down link has nothing to hand out */

    rc = framing_decoder_next(net->decoder, &payload, &plen);
    if (rc == FRAMING_NEED_MORE)
        return 0;
    if (rc == FRAMING_ERR_LENGTH) {
        /* Out of range (negative, or above the 1 MiB cap): the stream lost sync. */
        net_drop(net, "framing desync: length prefix out of range");
        return 0;
    }
    if (rc != FRAMING_OK) {
        net_error(net, "framing_decoder_next: rc=%d", rc);
        return -1;
    }
    /*
     * A zero-length frame is a legal frame with an empty payload, and it is a
     * malformed *message*, not a lost stream: the reference peer (TcpLink.cs)
     * only rejects lengths outside [0, MaxFrameBytes] and hands the empty
     * payload up for its JSON layer to refuse, and framing.h promises len == 0
     * here. The two failure classes are not interchangeable -- the course
     * requirements drop a bad message and keep the connection, and only a length
     * prefix that cannot be resynchronized costs the session. So this returns a
     * frame of length 0 and lets the caller's parser reject it.
     */
    if (plen >= cap) {
        /* A caller that cannot hold a legal frame is a programming error, and
         * truncating JSON silently would be worse than refusing. The link is
         * left alone. */
        net_error(net, "frame of %zu bytes does not fit a %zu byte buffer", plen, cap);
        return -1;
    }

    memcpy(buf, payload, plen);
    buf[plen] = '\0';
    if (len != NULL)
        *len = plen;
    return 1;
}

int kos_net_send_frame(kos_net *net, const char *json, size_t len)
{
    size_t need;
    size_t written = 0;
    ssize_t n;
    int rc;

    if (net == NULL)
        return -1;
    if (json == NULL && len > 0) {
        net_error(net, "send_frame: payload is NULL but len=%zu", len);
        return -1;
    }
    if (net->state != KOS_NET_UP || net->peer_fd < 0) {
        net_error(net, "send_frame: link is down, %zu bytes dropped", len);
        return -1;
    }
    if (len > (size_t)FRAMING_MAX_PAYLOAD) {
        /* Our own bug, not the peer's: keep the session usable. */
        net_error(net, "send_frame: %zu bytes exceeds the %u byte frame limit",
                  len, (unsigned)FRAMING_MAX_PAYLOAD);
        return -1;
    }

    need = framing_encoded_size(len);
    if (net->encbuf_cap < need) {
        unsigned char *grown = realloc(net->encbuf, need);

        if (grown == NULL) {
            net_error(net, "send_frame: out of memory for %zu bytes", need);
            return -1;
        }
        net->encbuf = grown;
        net->encbuf_cap = need;
    }

    rc = framing_encode(json, len, net->encbuf, net->encbuf_cap, &written);
    if (rc != FRAMING_OK || written != need) {
        net_error(net, "framing_encode: rc=%d", rc);
        return -1;
    }

    for (;;) {
        n = send(net->peer_fd, net->encbuf, written, MSG_NOSIGNAL);
        if (n >= 0)
            break;
        if (errno == EINTR)
            continue; /* nothing left the socket, so the retry is safe */
        /* EPIPE/ECONNRESET/ENOTCONN and EAGAIN alike mean the frame did not go
         * out and the link is not usable. */
        net_errno_error(net, errno, "send(fd %d, %zu bytes)", net->peer_fd, written);
        net_drop(net, NULL);
        return -1;
    }
    if ((size_t)n != written) {
        /* A partial frame cannot be resumed: the peer is left mid-prefix with no
         * way to resynchronize, so an honest reconnect beats a silent desync. */
        net_error(net, "send: short write (%zd of %zu bytes)", n, written);
        net_drop(net, NULL);
        return -1;
    }
    return 0;
}
