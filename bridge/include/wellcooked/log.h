/*
 * log.h -- the Bridge's append-only event log (module contract).
 *
 * Why a module and not fprintf: the acceptance reads logs/bridge.log to decide
 * whether a run happened, so the file has to stay a greppable record of one
 * event per line even when the process is killed mid-run. That rules out stdio
 * buffering: every event is exactly one write() on an O_APPEND fd, so a SIGKILL
 * can lose nothing -- a line is either in the file or it never started.
 *
 * Line format (single spaces, level left-aligned in five columns, then the
 * event token, then the caller's key=value pairs):
 *
 *   2026-10-15T15:29:55.123 INFO  bridge_start pid=1234 link=listen port=14455
 *
 * Single writer by contract: only the Bridge writes this file, so nothing here
 * is locked (standards.md forbids threads in the core).
 */
#ifndef WELLCOOKED_LOG_H
#define WELLCOOKED_LOG_H

/* Feature-test macro before every system header: -std=c99 hides the
 * POSIX.1-2008 declarations without it (same trick, same reason as shm.h). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>

/* kos_error (+ KOS_ERR_*) is the failure contract every module reports through.
 * It is declared in shm.h today; minting a second error struct here would give
 * callers two shapes to translate between for no gain. */
#include <wellcooked/shm.h>

typedef enum {
    KOS_LOG_DEBUG = 0,
    KOS_LOG_INFO,
    KOS_LOG_WARN,
    KOS_LOG_ERROR
} kos_log_level;

typedef struct kos_log kos_log; /* opaque */

/*
 * Open `path` for appending, creating it and any missing parent directory.
 * `min_level` filters at the door: an event below it is dropped before it is
 * formatted, so DEBUG chatter costs nothing in a WARN-level run.
 *
 * On success *out holds an fd that kos_log_close() must release; on failure
 * *out is NULL and err says which call failed and why. Never exits the process.
 */
int kos_log_open(const char *path, kos_log_level min_level, kos_log **out, kos_error *err);

/* Nothing is buffered, so closing is just closing the fd. NULL is a no-op,
 * which lets callers run this on every exit path without checking first. */
void kos_log_close(kos_log *log);

/* Also echo every line to stderr, for foreground runs where nobody tails the
 * file. Off by default. Returns KOS_OK, or KOS_ERR_INVAL for a NULL log. */
int kos_log_mirror_stderr(kos_log *log, int enabled);

/*
 * Write one event. `fmt` is the printf-style format of the key=value tail; for
 * an event that carries no data pass "" (C99 cannot make a variadic argument
 * optional, and making the format mandatory keeps the printf attribute
 * checking the call sites). Embedded newlines in the message are folded to
 * spaces so the one-event-per-line guarantee survives a multi-line payload.
 *
 * Safe with log == NULL: the call is a no-op, so no call site needs a guard.
 */
void kos_log_write(kos_log *log, kos_log_level lvl, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Convenience wrappers: kos_log_info(log, "bridge_start", "pid=%d", (int)getpid()) */
#define kos_log_debug(log, event, ...) kos_log_write((log), KOS_LOG_DEBUG, (event), __VA_ARGS__)
#define kos_log_info(log, event, ...) kos_log_write((log), KOS_LOG_INFO, (event), __VA_ARGS__)
#define kos_log_warn(log, event, ...) kos_log_write((log), KOS_LOG_WARN, (event), __VA_ARGS__)
#define kos_log_error(log, event, ...) kos_log_write((log), KOS_LOG_ERROR, (event), __VA_ARGS__)

/* Level spelling for log lines, --log-level parsing and tests. An unknown level
 * name is -1, which is also what an out-of-range number gets. */
const char *kos_log_level_name(kos_log_level lvl);
int kos_log_level_from_name(const char *name);

#endif /* WELLCOOKED_LOG_H */
