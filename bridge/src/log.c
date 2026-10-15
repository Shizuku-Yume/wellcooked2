/*
 * log.c -- one write() per event, O_APPEND, no buffering. See log.h.
 */
#define _POSIX_C_SOURCE 200809L

#include <kitchenos/log.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * One line is composed in this buffer and written with a single write(). A
 * message that does not fit is cut short and marked instead of being split over
 * two writes: two writes could interleave with another writer and would break
 * the one-line-per-event guarantee that grep and the log tests depend on.
 */
#define KOS_LOG_LINE_MAX 8192

/* "YYYY-MM-DDTHH:MM:SS.mmm" is exactly 23 characters. */
#define KOS_LOG_TS_LEN 23

struct kos_log {
    int fd;
    kos_log_level min_level;
    int mirror_stderr;
};

static const char *const level_names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
static const int level_count = (int)(sizeof level_names / sizeof level_names[0]);

static void err_set(kos_error *err, int code, int sys_errno, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL)
        return; /* an optional argument stays optional */
    err->code = code;
    err->sys_errno = sys_errno;
    va_start(ap, fmt);
    vsnprintf(err->context, sizeof err->context, fmt, ap);
    va_end(ap);
}

/* Unknown values cannot be printed as a name, and kos_log_write() has no way to
 * report a bad level, so it is pinned to the nearest legal one. */
static kos_log_level level_clamp(kos_log_level lvl)
{
    if ((int)lvl < 0)
        return KOS_LOG_DEBUG;
    if ((int)lvl >= level_count)
        return KOS_LOG_ERROR;
    return lvl;
}

static void format_timestamp(char *out, size_t cap)
{
    struct timespec ts;
    struct tm tm;
    long ms = 0;

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        ms = ts.tv_nsec / 1000000L;
    } else {
        /* A clock the kernel will not give us must not cost us the line. */
        ts.tv_sec = time(NULL);
    }
    if (localtime_r(&ts.tv_sec, &tm) == NULL) {
        snprintf(out, cap, "1970-01-01T00:00:00.000");
        return;
    }
    /* Local time, to the millisecond: the log is read next to a wall clock.
     * Each field is masked into its real width first. Without that, GCC cannot
     * bound `tm_year + 1900` (an int may need 11 characters), so it computes a
     * worst case far larger than the caller's buffer and -Werror=format-truncation
     * rejects the line. Masked, the format is provably 23 characters plus NUL. */
    snprintf(out, cap, "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
             (unsigned)(tm.tm_year + 1900) % 10000u, (unsigned)(tm.tm_mon + 1) % 100u,
             (unsigned)tm.tm_mday % 100u, (unsigned)tm.tm_hour % 100u,
             (unsigned)tm.tm_min % 100u, (unsigned)tm.tm_sec % 100u,
             (unsigned)ms % 1000u);
}

/*
 * mkdir -p for the directory part of `path`. Deployments create logs/ ahead of
 * time, but a fresh checkout, a new --log-dir or a pid-suffixed test path should
 * not need a manual mkdir before the first run.
 */
static int ensure_parent_dirs(const char *path, kos_error *err)
{
    char dir[4096];
    size_t len;
    size_t i;
    const char *slash = strrchr(path, '/');

    /* Only the directories above the file are ours to create. A path with no
     * slash is relative to the current directory, which already exists. */
    if (slash == NULL || slash == path)
        return KOS_OK;

    len = (size_t)(slash - path);
    if (len >= sizeof dir) {
        err_set(err, KOS_ERR_INVAL, 0, "log path directory is too long (%zu chars)", len);
        return KOS_ERR_INVAL;
    }
    memcpy(dir, path, len);
    dir[len] = '\0';

    for (i = 1; i <= len; i++) {
        char saved;

        if (dir[i] != '/' && dir[i] != '\0')
            continue;
        saved = dir[i];
        dir[i] = '\0'; /* ...so mkdir() sees exactly one component */
        if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
            int saved_errno = errno;

            dir[i] = saved;
            err_set(err, KOS_ERR_IO, saved_errno, "mkdir(%s)", dir);
            return KOS_ERR_IO;
        }
        dir[i] = saved;
    }
    return KOS_OK;
}

/*
 * write() to a log cannot be reported through this module's void API, so a
 * failure ends the line silently; the stderr mirror is what an operator running
 * in the foreground sees when the disk goes away.
 */
static void write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return;
    }
}

int kos_log_open(const char *path, kos_log_level min_level, kos_log **out, kos_error *err)
{
    kos_log *log;
    int fd;
    int rc;

    if (out == NULL) {
        err_set(err, KOS_ERR_INVAL, 0, "kos_log_open: out is NULL");
        return KOS_ERR_INVAL;
    }
    *out = NULL; /* a failed open never leaves a dangling handle behind */
    if (path == NULL || path[0] == '\0') {
        err_set(err, KOS_ERR_INVAL, 0, "kos_log_open: path is NULL or empty");
        return KOS_ERR_INVAL;
    }
    if ((int)min_level < 0 || (int)min_level >= level_count) {
        err_set(err, KOS_ERR_INVAL, 0, "kos_log_open: min_level %d out of range", (int)min_level);
        return KOS_ERR_INVAL;
    }

    rc = ensure_parent_dirs(path, err);
    if (rc != KOS_OK)
        return rc;

    /* O_APPEND so a restart cannot overwrite an earlier run, O_CLOEXEC so the
     * Bridge's future children (Supervisor, Workers) do not inherit the fd. */
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        err_set(err, KOS_ERR_IO, errno, "open(%s, O_WRONLY|O_CREAT|O_APPEND)", path);
        return KOS_ERR_IO;
    }

    log = calloc(1, sizeof *log);
    if (log == NULL) {
        int saved_errno = errno;

        close(fd);
        err_set(err, KOS_ERR_NOMEM, saved_errno, "calloc(kos_log)");
        return KOS_ERR_NOMEM;
    }
    log->fd = fd;
    log->min_level = min_level;
    log->mirror_stderr = 0;
    *out = log;
    return KOS_OK;
}

void kos_log_close(kos_log *log)
{
    if (log == NULL)
        return;
    if (log->fd >= 0)
        close(log->fd);
    free(log);
}

int kos_log_mirror_stderr(kos_log *log, int enabled)
{
    if (log == NULL)
        return KOS_ERR_INVAL;
    log->mirror_stderr = enabled ? 1 : 0;
    return KOS_OK;
}

void kos_log_write(kos_log *log, kos_log_level lvl, const char *event, const char *fmt, ...)
{
    char line[KOS_LOG_LINE_MAX + 2]; /* the extra bytes hold '\n' and NUL */
    char ts[KOS_LOG_TS_LEN + 1];
    const size_t cap = sizeof line;
    size_t used;
    size_t msg_start;
    int truncated = 0;
    int n;

    if (log == NULL || log->fd < 0 || event == NULL)
        return; /* documented no-op: callers never need a guard */
    lvl = level_clamp(lvl);
    if ((int)lvl < (int)log->min_level)
        return;

    format_timestamp(ts, sizeof ts);
    n = snprintf(line, cap, "%s %-5s %s", ts, level_names[lvl], event);
    if (n < 0)
        return;
    if ((size_t)n >= cap - 1) {
        used = cap - 1; /* the event token alone overflowed the line */
        truncated = 1;
    } else {
        used = (size_t)n;
    }

    msg_start = used;
    if (!truncated && fmt != NULL && fmt[0] != '\0') {
        va_list ap;

        if (used + 2 >= cap) {
            truncated = 1;
        } else {
            line[used++] = ' ';
            /* One byte is kept back for the newline the caller never writes. */
            va_start(ap, fmt);
            n = vsnprintf(line + used, cap - used - 1, fmt, ap);
            va_end(ap);
            if (n < 0) {
                line[used] = '\0';
            } else if ((size_t)n >= cap - used - 1) {
                truncated = 1;
                used = cap - 1;
            } else {
                used += (size_t)n;
            }
        }
    }

    /* Newlines would turn one event into several lines, which is exactly what a
     * reader counting events (and the log test) assumes cannot happen. */
    {
        size_t i;

        for (i = msg_start; i < used; i++)
            if (line[i] == '\n' || line[i] == '\r')
                line[i] = ' ';
    }
    if (truncated) {
        static const char marker[] = "...[truncated]";
        size_t marker_len = sizeof marker - 1;

        if (used + marker_len + 1 < cap) {
            memcpy(line + used, marker, marker_len);
            used += marker_len;
        }
    }

    line[used++] = '\n';
    write_all(log->fd, line, used);
    if (log->mirror_stderr)
        write_all(STDERR_FILENO, line, used);
}

const char *kos_log_level_name(kos_log_level lvl)
{
    if ((int)lvl < 0 || (int)lvl >= level_count)
        return "UNKNOWN";
    return level_names[lvl];
}

int kos_log_level_from_name(const char *name)
{
    int i;

    if (name == NULL)
        return -1;
    for (i = 0; i < level_count; i++)
        if (strcasecmp(name, level_names[i]) == 0)
            return i;
    return -1;
}
