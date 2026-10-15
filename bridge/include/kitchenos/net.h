/*
 * net.h -- poll-driven TCP link to the game plugin (module contract).
 *
 * The Bridge has to survive a game that is not running, a plugin that dies
 * between two frames and a plugin that comes back minutes later, so this module
 * is a state machine rather than a connect() wrapper: every socket is
 * non-blocking, every failure is a return code or a state change, and both sides
 * of the same protocol -- Bridge listening, plugin connecting -- are served by
 * one object.
 *
 * Framing is not this module's job. Bytes read from the socket go into a
 * framing_decoder (see framing.h) and come back out of kos_net_recv_frame(); no
 * terminator is looked for here. A line longer than FRAMING_MAX_PAYLOAD means
 * the peer is not speaking this protocol, and there is nothing to
 * resynchronize *to* -- a terminator inside a payload is indistinguishable from
 * the one that ends it -- so the link drops and the peer has to reconnect. A
 * zero-length line is not that case: it is a legal frame with an empty payload,
 * i.e. a malformed *message* -- the caller's parser rejects it while the
 * connection stays up, which is how the reference peer behaves too
 * (TcpLink.cs splits on the terminator without judging the payload and lets its
 * JSON layer reject the empty string).
 *
 * Single-threaded by contract: the caller drives everything through
 * kos_net_wait() (standards.md: no threads in the core).
 */
#ifndef KITCHENOS_NET_H
#define KITCHENOS_NET_H

/* Before every system header, as in shm.h: -std=c99 hides the POSIX.1-2008
 * declarations otherwise. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>

#include <kitchenos/shm.h> /* kos_error + KOS_ERR_*, the shared failure contract */

#define KOS_NET_DEFAULT_PORT 12345
#define KOS_NET_DEFAULT_BIND_ADDR "0.0.0.0"
#define KOS_NET_DEFAULT_BACKLOG 5
#define KOS_NET_DEFAULT_RECONNECT_MS 1000

typedef enum {
    KOS_NET_MODE_LISTEN = 0, /* Bridge is the server: the plugin dials in */
    KOS_NET_MODE_CONNECT = 1 /* Bridge is the client: the plugin listens */
} kos_net_mode;

typedef enum {
    KOS_NET_DOWN = 0, /* no usable peer: connecting, retrying, or waiting to accept */
    KOS_NET_UP = 1    /* a peer socket exists and frames may be exchanged */
} kos_net_state;

typedef struct {
    kos_net_mode mode;
    const char *host;   /* CONNECT only, required: an address or name to dial */
    int port;           /* 0 in LISTEN mode means "any free port" (tests, demos) */
    const char *bind_addr; /* LISTEN only; NULL means KOS_NET_DEFAULT_BIND_ADDR */
    int backlog;        /* LISTEN only; <= 0 means KOS_NET_DEFAULT_BACKLOG */
    int reconnect_delay_ms; /* CONNECT: pause between dials, 0 means immediately */
} kos_net_opts;

/* LISTEN, port 12345, bind 0.0.0.0, backlog 5, reconnect after 1000 ms. host is
 * left NULL because CONNECT is the only mode that reads it and silently dialing
 * localhost on a forgotten host would hide a configuration mistake. */
void kos_net_opts_init(kos_net_opts *opts);

typedef struct kos_net kos_net; /* opaque */

/*
 * Open the link. LISTEN mode binds and listens (the accepted peer arrives inside
 * kos_net_wait); CONNECT mode resolves the target and makes the first of many
 * dial attempts.
 *
 * A refused dial is not an open failure in CONNECT mode: the object is created,
 * stays KOS_NET_DOWN and keeps retrying, because the Bridge must run while the
 * game is closed. KOS_ERR_IO here means the local socket could not be created or
 * the LISTEN port could not be bound, and err names the call and the errno.
 */
int kos_net_open(const kos_net_opts *opts, kos_net **out, kos_error *err);

/* Close every fd, free the decoder and the buffers. Never blocks, NULL-safe. */
void kos_net_close(kos_net *net);

kos_net_state kos_net_state_of(const kos_net *net);

/*
 * Serve whichever fd matters right now -- the listening socket, the peer socket,
 * or the retry timer while CONNECT is down -- for at most timeout_ms
 * (0 = poll only, < 0 = wait until something happens). Inside one call it
 * accepts a pending connection, finishes a non-blocking connect (SO_ERROR),
 * retries a failed dial after the delay and drains available bytes into the
 * decoder.
 *
 * Returns 0 on any progress or timeout, and -1 only when the object itself is
 * unusable (a broken listener, a failed poll, out of memory). A dead peer is a
 * state change to KOS_NET_DOWN, never an error: a caller that only checks for -1
 * keeps running through every network failure, which is the point.
 *
 * EINTR returns 0 immediately instead of restarting the wait, so a SIGTERM
 * handler that only sets a flag still gets control back promptly.
 */
int kos_net_wait(kos_net *net, int timeout_ms);

/*
 * Hand out the next complete frame from what kos_net_wait() read.
 *   1  -> one frame copied into buf (always NUL-terminated) and its length in
 *         *len; a zero-length frame is reported as len == 0 with buf == ""
 *   0  -> nothing complete buffered (or the link is down): call again after the
 *         next wait; a caller loops until it sees 0
 *  -1  -> fatal for this call: the buffer cannot hold a legal frame, the decoder
 *         is out of memory. The link is left alone in this case.
 *
 * Peer EOF and a framing desync (a line over FRAMING_MAX_PAYLOAD -- the one
 * failure a reader cannot tell apart from payload bytes) are handled here rather
 * than reported: the link drops to KOS_NET_DOWN, the peer fd is closed, the
 * decoder is reset, and the answer is 0. An EOF therefore discards bytes that
 * were buffered but never pulled -- the session they belong to is over, and the
 * next peer's stream must not be parsed as a continuation of it. A *malformed
 * message*, by contrast, is delivered and left to the caller's parser: the
 * connection stays up, as it must.
 */
int kos_net_recv_frame(kos_net *net, char *buf, size_t cap, size_t *len);

/*
 * Encode one frame with framing_encode() and send it.
 * Returns 0 when the whole frame went out, -1 otherwise. Anything that leaves a
 * partial line on the wire (EPIPE, ECONNRESET, EAGAIN, a short send) drops the
 * link first: the peer is left holding a line whose terminator will never come,
 * and the next frame would be read as its continuation, so an honest disconnect
 * beats a silent desync. A payload over FRAMING_MAX_PAYLOAD, or one carrying a
 * raw terminator, is a local bug and returns -1 without touching the link.
 * Never blocks and never raises SIGPIPE.
 */
int kos_net_send_frame(kos_net *net, const char *json, size_t len);

/* Force the session down: for the Bridge's heartbeat timeout, which knows the
 * peer is gone before the socket does (a half-open connection never reports
 * EOF). LISTEN keeps the port and waits for the next connection; CONNECT retries
 * after reconnect_delay_ms. NULL-safe, never blocks. */
void kos_net_drop(kos_net *net);

/* Fds for callers that want to poll, log or `ss -tnp` them; -1 when absent.
 * The peer fd is reported only while the link is UP: a half-open socket still
 * being dialed is not something a caller may read or write. */
int kos_net_peer_fd(const kos_net *net);
int kos_net_listen_fd(const kos_net *net);

/* Why the link is down, or "" while it is up. Never NULL. */
const char *kos_net_last_error(const kos_net *net);

#endif /* KITCHENOS_NET_H */
