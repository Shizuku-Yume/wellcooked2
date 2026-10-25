/*
 * bridge.c -- the KitchenOS Bridge process.
 *
 * The bridge is the only process that talks to the game (TCP) and the only
 * writer of the shared world state (shm.h). It owns no policy: it carries the
 * state the plugin reports into /kitchenos_state, turns every state_update into
 * one frame, waits (with an upper bound) for the N Chef Workers to publish their
 * input slots, aggregates those slots into `action` messages and sends them
 * back. Choosing who cooks what is the Scheduler's job; planning a path is navd's
 * job (the Workers only publish target slots and follow released waypoints).
 *
 * Two invariants shape the whole file:
 *
 *   1. The bridge never dies because a peer is missing. No plugin, no
 *      Scheduler, no Workers is a normal operating condition: the loop keeps
 *      running, keeps writing the world state, and answers every frame with
 *      idle (stop) input. Only argument errors and a failed startup end the
 *      process with a non-zero code.
 *
 *   2. The socket is never blocked on the frame barrier. The barrier is
 *      deferred: a state_update closes the previous frame (flushing it), opens
 *      the next one with a deadline, and the loop services it between socket
 *      polls. A slow or absent Worker can therefore delay a frame by at most
 *      --frame-timeout-ms, never wedge the pipeline. This is the reading of
 *      §6.1 ("wait for every Worker's input, with an upper bound") that also
 *      satisfies "when the Scheduler/Workers crash, the Bridge keeps running
 *      and sends idle input".
 *
 * Single-threaded and event-driven on purpose: the Supervisor daemonizes later,
 * so this process must not fork, setsid or install its own PID file (spec:
 * "不管进程守护"). Signals do nothing but set a flag -- see on_signal().
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <kitchenos/framing.h>
#include <kitchenos/json.h>
#include <kitchenos/log.h>
#include <kitchenos/net.h>
#include <kitchenos/protocol.h>
#include <kitchenos/shm.h>

/* ============================================================== 常量 */

/* Announced as the bridge version in log lines; the wire version is
 * PROTO_VERSION and the two move together (protocol.h owns the wire). */
#define BRIDGE_VERSION PROTO_VERSION

#define DEFAULT_HOST "127.0.0.1"
/* 两种模式默认同一个端口：14455。谁在听由 --link 决定，不再按方向分两个号
 * （decisions.md: 2026-10-15 - 端口按组员决策统一成 14455）。 */
#define DEFAULT_LISTEN_PORT 14455
#define DEFAULT_CONNECT_PORT 14455
#define DEFAULT_CHEFS 2
#define DEFAULT_STATE_NAME KOS_SHM_DEFAULT_NAME
#define DEFAULT_SEM_NAME KOS_SHM_DEFAULT_SEM_NAME
#define DEFAULT_LOG_PATH "logs/bridge.log"
#define DEFAULT_FRAME_TIMEOUT_MS 100
/* 槽多久没有新版本就当成缺席（发 idle）。要比游戏帧间隔宽得多，否则一个正常但比游戏慢的
 * Worker 会被反复冻住；也要足够短，Worker 崩了之后厨师不能一直跑下去。 */
#define DEFAULT_INPUT_GRACE_MS 250
/* 5000 来自 game-bridge spec 的失败行为约定（"心跳超时 5 秒判定对端断开"）。存活判定仍然
 * 只看心跳 —— 见 decisions.md: 存活判定只看心跳。 */
#define DEFAULT_HEARTBEAT_TIMEOUT_MS 5000
#define DEFAULT_HEARTBEAT_INTERVAL_MS 1000

/* The loop refuses to sleep longer than this even when nothing is due: it is
 * the bound on how late a signal, a link edge or a freshly published input slot
 * can be noticed. */
#define MAX_POLL_MS 50

/* A state_summary per 60 frames keeps a 60 Hz stream from flooding the log
 * while still proving the state is flowing (frame = one second of game time). */
#define SUMMARY_EVERY_FRAMES 60

/* One action message is a few hundred bytes; 4 KiB is far above any builder's
 * worst case and keeps the hot path allocation-free. */
#define ACTION_BUF 4096
#define ERR_BUF 256

/* ========================================================== 进程状态 */

typedef struct {
    /* ---- configuration (set once by parse_args) ---- */
    kos_net_mode link_mode;
    const char *host;
    int host_given; /* --host was passed: in listen mode it is the bind address */
    int port;
    int port_given;
    int chefs; /* effective slot count, clamped to KOS_SHM_MAX_CHEFS */
    int chefs_requested;
    const char *state_name;
    const char *sem_name;
    const char *log_path;
    int frame_timeout_ms;
    int input_grace_ms; /* 槽多久没新版本才算"这一格缺席"（默认见 DEFAULT_INPUT_GRACE_MS） */
    int heartbeat_timeout_ms; /* 0 disables the plugin liveness check */
    int heartbeat_interval_ms;
    /* The Supervisor's heartbeat pipe (spec: os-core W4). 0 = standalone run. */
    int supervisor_fd;
    int supervisor_heartbeat_ms;
    int64_t last_supervisor_beat_ms;
    long max_frames; /* > 0: exit 0 after this many dispatched frames */
    int verbose;

    /* ---- handles ---- */
    kos_log *log;
    kos_net *net;
    kos_shm *shm;
    kos_shm_opts shm_opts;
    kos_net_state link_state;
    /* The peer fd the last link_up/link_peer_replaced line reported. net.h hands
     * out a new fd when a second connection replaces the first without the link
     * ever going down, and that swap has to be logged (see check_link_edges). */
    int link_peer_fd;

    /* ---- frame barrier ---- */
    int64_t frames;             /* state_update frames seen (logging only) */
    int64_t frames_dispatched;  /* frames answered with actions */
    int pending;                /* a frame is open, waiting for slots/deadline */
    uint64_t pending_frame;     /* its index, for log lines */
    int64_t pending_deadline_ms;
    /* last_frame[k] = newest slot version already consumed for chef k. It is
     * the "consume this frame's input exactly once" bookkeeping of
     * kos_shm_wait_frame_after(). */
    uint64_t last_frame[KOS_SHM_MAX_CHEFS];
    /* slot_fresh_ms[k] = 这一格最近一次"交了新版本"的时刻（单调毫秒）。 */
    int64_t slot_fresh_ms[KOS_SHM_MAX_CHEFS];

    /* ---- plugin liveness ---- */
    int paused;                /* plugin timed out: dispatching suspended */
    int64_t last_heartbeat_ms; /* last heartbeat received (monotonic) */
    int64_t last_heartbeat_sent_ms;

    /* ---- misc ---- */
    const char *stop_reason;
    char frame_buf[FRAMING_MAX_PAYLOAD + 1]; /* one inbound line, NUL included */
} bridge;

/* Signals may only touch these two. The loop turns them into log events and an
 * orderly shutdown; nothing else in the process has to be async-signal-safe. */
static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_stop_signal = 0;

static bridge g_bridge; /* zero-initialised; one process, one bridge */

/* ============================================================== 工具 */

static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

/* Bounded string copy that always NUL-terminates. The shm layout uses fixed
 * char arrays, and a frame from the game may carry arbitrarily long strings, so
 * truncation here is normal and must not be a buffer overrun. */
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if (cap == 0) {
        return;
    }
    if (src != NULL) {
        while (i + 1 < cap && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static int double_is_finite(double v)
{
    /* Deliberately not isfinite(): that is a macro from math.h and pulls the
     * -lm question into a program that otherwise needs no math library. */
    return v == v && v <= 1.0e308 && v >= -1.0e308;
}

static int64_t double_to_ll(double v)
{
    if (!double_is_finite(v)) {
        return 0;
    }
    if (v >= 9.0e18) {
        return INT64_MAX;
    }
    if (v <= -9.0e18) {
        return INT64_MIN;
    }
    return (int64_t)(v < 0 ? v - 0.5 : v + 0.5);
}

static float double_to_float(double v)
{
    if (!double_is_finite(v)) {
        return 0.0f;
    }
    if (v >= 3.0e38) {
        return 3.0e38f;
    }
    if (v <= -3.0e38) {
        return -3.0e38f;
    }
    return (float)v;
}

static const char *net_error_text(const bridge *b)
{
    const char *text = kos_net_last_error(b->net);
    return (text != NULL && text[0] != '\0') ? text : "none";
}

/* ======================================================== 信号处理 */

/*
 * The one and only thing a handler does is set the flag; everything else
 * (logging, unlinking, closing) happens in the main loop, where it is allowed
 * to call into stdio and the other modules.
 */
static void on_signal(int sig)
{
    g_stop_signal = sig;
    g_stop = 1;
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGTERM:
        return "SIGTERM";
    case SIGINT:
        return "SIGINT";
    default:
        return "unknown";
    }
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART: a signal must not turn an in-flight socket wait into an EINTR
     * that the net module would have to special-case. The handler changes no
     * state the loop is reading, and the loop notices the flag within one poll
     * tick (<= MAX_POLL_MS), so restarting is both safe and cheaper. */
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }

    /* SIGPIPE: a write to a socket the plugin already closed must come back as
     * an error from net, never as a signal that kills the bridge mid-frame. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
}

/* ============================================================ 命令行 */

enum { ARG_OK = 0, ARG_ERR = 1, ARG_HELP = 2, ARG_VERSION = 3 };

static void print_usage(FILE *out)
{
    fprintf(out,
            "usage: kitchenos-bridge [--link listen|connect] [--host H] [--port P]\n"
            "                        [--chefs N] [--state-name NAME] [--sem-name NAME]\n"
            "                        [--log FILE] [--frame-timeout-ms M]\n"
            "                        [--heartbeat-timeout-ms M] [--heartbeat-interval-ms M]\n"
            "                        [--supervisor-fd N] [--supervisor-heartbeat-ms M]\n"
            "                        [--max-frames N] [--verbose] [--help] [--version]\n");
}

static void print_help(void)
{
    print_usage(stdout);
    printf("\n"
           "The KitchenOS Bridge: the only process that talks to the game plugin.\n"
           "It maps every state_update to one frame, waits for the Chef Workers' input\n"
           "slots (bounded by --frame-timeout-ms), and answers with one action per chef,\n"
           "falling back to idle \"stop\" for any slot that is missing.\n"
           "\n"
           "Options:\n"
           "  --link listen|connect   listen (default): act as the TCP server the plugin\n"
           "                          dials into. connect: dial the plugin's own server.\n"
           "  --host H                listen: bind address (default 127.0.0.1; pass\n"
           "                          0.0.0.0 for a cross-machine demo). connect: peer\n"
           "                          address to dial (default 127.0.0.1).\n"
           "  --port P                listen: %d (default). connect: %d (default).\n"
           "  --chefs N               number of input slots, 0..%d (default %d).\n"
           "  --state-name NAME       shared world state object (default %s).\n"
           "  --sem-name NAME         input-ready semaphore (default %s).\n"
           "  --log FILE              event log (default %s).\n"
           "  --frame-timeout-ms M    frame barrier upper bound (default %d).\n"
           "  --input-grace-ms M      how long a slot may go without a new version before\n"
           "                          its chef is answered with idle (default %d).\n"
           "  --heartbeat-timeout-ms M  declare the plugin gone after M ms without a\n"
           "                          heartbeat; 0 disables (default %d).\n"
           "                          A timeout suspends dispatching until the plugin\n"
           "                          speaks again (decisions.md: 只停心跳仍发状态属于\n"
           "                          插件失联，暂停下发).\n"
           "  --heartbeat-interval-ms M  send a heartbeat every M ms, 0 disables\n"
           "                          (default %d).\n"
           "  --supervisor-fd N       write one byte to the Supervisor's heartbeat pipe\n"
           "                          every --supervisor-heartbeat-ms (default: none).\n"
           "  --supervisor-heartbeat-ms M  Supervisor heartbeat interval (default 0).\n"
           "  --max-frames N          exit 0 after N dispatched frames (deterministic\n"
           "                          runs); 0 = run until signalled (default).\n"
           "  --verbose               log DEBUG events (per-frame state_update, acks).\n"
           "  --help, --version       print and exit 0.\n"
           "\n"
           "Exit codes: 0 clean shutdown (signal or --max-frames), 1 startup failure,\n"
           "2 argument error. Unknown or incomplete options print the usage line.\n",
           DEFAULT_LISTEN_PORT, DEFAULT_CONNECT_PORT, KOS_SHM_MAX_CHEFS, DEFAULT_CHEFS,
           DEFAULT_STATE_NAME, DEFAULT_SEM_NAME, DEFAULT_LOG_PATH,
           DEFAULT_FRAME_TIMEOUT_MS, DEFAULT_INPUT_GRACE_MS, DEFAULT_HEARTBEAT_TIMEOUT_MS,
           DEFAULT_HEARTBEAT_INTERVAL_MS);
}

/* Value of an option: either the "=..." half of --opt=value or the next argv
 * entry. Returns NULL when the value is missing, which the caller reports as an
 * argument error (exit 2) rather than guessing. */
static const char *take_value(const char *inline_value, int argc, char **argv, int *i)
{
    if (inline_value != NULL) {
        return inline_value[0] != '\0' ? inline_value : NULL;
    }
    if (*i + 1 >= argc) {
        return NULL;
    }
    (*i)++;
    return argv[*i];
}

static int parse_long(const char *value, long min, long max, long *out)
{
    char *end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < min || parsed > max) {
        return -1;
    }
    *out = parsed;
    return 0;
}

static int parse_args(int argc, char **argv, bridge *b)
{
    int i;

    b->link_mode = KOS_NET_MODE_LISTEN;
    b->host = DEFAULT_HOST;
    b->host_given = 0;
    b->port = DEFAULT_LISTEN_PORT;
    b->port_given = 0;
    b->chefs = DEFAULT_CHEFS;
    b->chefs_requested = DEFAULT_CHEFS;
    b->state_name = DEFAULT_STATE_NAME;
    b->sem_name = DEFAULT_SEM_NAME;
    b->log_path = DEFAULT_LOG_PATH;
    b->frame_timeout_ms = DEFAULT_FRAME_TIMEOUT_MS;
    b->input_grace_ms = DEFAULT_INPUT_GRACE_MS;
    b->heartbeat_timeout_ms = DEFAULT_HEARTBEAT_TIMEOUT_MS;
    b->heartbeat_interval_ms = DEFAULT_HEARTBEAT_INTERVAL_MS;
    b->supervisor_fd = -1;
    b->supervisor_heartbeat_ms = 0;
    b->max_frames = 0;
    b->verbose = 0;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;
        const char *eq;
        char name[64];
        size_t name_len;
        long number;

        if (arg[0] != '-' || arg[1] != '-') {
            return ARG_ERR;
        }
        eq = strchr(arg, '=');
        name_len = (eq != NULL) ? (size_t)(eq - arg) : strlen(arg);
        if (name_len == 0 || name_len >= sizeof name) {
            return ARG_ERR;
        }
        memcpy(name, arg, name_len);
        name[name_len] = '\0';
        if (eq != NULL) {
            value = eq + 1;
            if (value[0] == '\0') {
                return ARG_ERR; /* --opt= with nothing after it */
            }
        }

        if (strcmp(name, "--help") == 0) {
            return ARG_HELP;
        } else if (strcmp(name, "--version") == 0) {
            return ARG_VERSION;
        } else if (strcmp(name, "--verbose") == 0) {
            if (value != NULL) {
                return ARG_ERR;
            }
            b->verbose = 1;
        } else if (strcmp(name, "--link") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (v == NULL) {
                return ARG_ERR;
            }
            if (strcmp(v, "listen") == 0) {
                b->link_mode = KOS_NET_MODE_LISTEN;
            } else if (strcmp(v, "connect") == 0) {
                b->link_mode = KOS_NET_MODE_CONNECT;
            } else {
                return ARG_ERR;
            }
        } else if (strcmp(name, "--host") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (v == NULL) {
                return ARG_ERR;
            }
            b->host = v;
            b->host_given = 1;
        } else if (strcmp(name, "--port") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (parse_long(v, 1, 65535, &number) != 0) {
                return ARG_ERR;
            }
            b->port = (int)number;
            b->port_given = 1;
        } else if (strcmp(name, "--chefs") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (parse_long(v, 0, 1024, &number) != 0) {
                return ARG_ERR;
            }
            b->chefs_requested = (int)number;
            b->chefs = (number > KOS_SHM_MAX_CHEFS) ? KOS_SHM_MAX_CHEFS : (int)number;
        } else if (strcmp(name, "--state-name") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (v == NULL) {
                return ARG_ERR;
            }
            b->state_name = v;
        } else if (strcmp(name, "--sem-name") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (v == NULL) {
                return ARG_ERR;
            }
            b->sem_name = v;
        } else if (strcmp(name, "--log") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (v == NULL) {
                return ARG_ERR;
            }
            b->log_path = v;
        } else if (strcmp(name, "--input-grace-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (parse_long(v, 0, 600000, &number) != 0) {
                return ARG_ERR;
            }
            b->input_grace_ms = (int)number;
        } else if (strcmp(name, "--frame-timeout-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (parse_long(v, 0, 600000, &number) != 0) {
                return ARG_ERR;
            }
            b->frame_timeout_ms = (int)number;
        } else if (strcmp(name, "--heartbeat-timeout-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (parse_long(v, 0, 3600000, &number) != 0) {
                return ARG_ERR;
            }
            b->heartbeat_timeout_ms = (int)number;
        } else if (strcmp(name, "--heartbeat-interval-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (parse_long(v, 0, 3600000, &number) != 0) {
                return ARG_ERR;
            }
            b->heartbeat_interval_ms = (int)number;
        } else if (strcmp(name, "--supervisor-fd") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (parse_long(v, 0, 65535, &number) != 0) {
                return ARG_ERR;
            }
            b->supervisor_fd = (int)number;
        } else if (strcmp(name, "--supervisor-heartbeat-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (parse_long(v, 0, 3600000, &number) != 0) {
                return ARG_ERR;
            }
            b->supervisor_heartbeat_ms = (int)number;
        } else if (strcmp(name, "--max-frames") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            if (parse_long(v, 0, 1000000000L, &number) != 0) {
                return ARG_ERR;
            }
            b->max_frames = number;
        } else {
            return ARG_ERR;
        }
    }

    /* The port default depends on the topology, and --link may come after
     * --port on the command line, so resolve it once everything is parsed. */
    if (!b->port_given) {
        b->port = (b->link_mode == KOS_NET_MODE_LISTEN) ? DEFAULT_LISTEN_PORT
                                                        : DEFAULT_CONNECT_PORT;
    }
    return ARG_OK;
}

/* ==================================================== 世界状态映射 */

/*
 * proto_state_update (PROTO_MAX_*) -> kos_world_state (KOS_SHM_MAX_*). The two
 * caps differ on purpose: the protocol tolerates whatever the game sends, the
 * shared layout is fixed. Everything past the layout's cap is dropped, and the
 * counts are what readers use, so a truncated snapshot stays self-consistent.
 */
static void world_from_state_update(const proto_state_update *up, kos_world_state *world)
{
    size_t chefs;
    size_t orders;
    size_t resources;
    size_t i;
    size_t k;

    kos_world_state_reset(world);
    world->score = double_to_ll(up->score);
    world->game_time = double_to_ll(up->game_time);
    world->game_time_ms = up->game_time_ms;

    chefs = up->chef_count;
    if (chefs > KOS_SHM_MAX_CHEFS) {
        chefs = KOS_SHM_MAX_CHEFS;
    }
    world->chef_count = (uint32_t)chefs;
    for (i = 0; i < chefs; i++) {
        const proto_chef *src = &up->chefs[i];
        kos_chef *dst = &world->chefs[i];

        dst->id = (int32_t)src->id;
        dst->x = double_to_float(src->x);
        dst->y = double_to_float(src->y);
        dst->facing = double_to_float(src->facing);
        copy_str(dst->holding, sizeof dst->holding, src->holding);
        copy_str(dst->state, sizeof dst->state, src->state);
    }

    orders = up->order_count;
    if (orders > KOS_SHM_MAX_ORDERS) {
        orders = KOS_SHM_MAX_ORDERS;
    }
    world->order_count = (uint32_t)orders;
    for (i = 0; i < orders; i++) {
        const proto_order *src = &up->orders[i];
        kos_order *dst = &world->orders[i];

        copy_str(dst->id, sizeof dst->id, src->id);
        copy_str(dst->recipe, sizeof dst->recipe, src->recipe);
        dst->time_remaining = (int32_t)double_to_ll(src->time_remaining);
        k = src->ingredient_count;
        if (k > KOS_SHM_MAX_ORDER_INGREDIENTS) {
            k = KOS_SHM_MAX_ORDER_INGREDIENTS;
        }
        dst->ingredient_count = (int32_t)k;
        for (size_t j = 0; j < k; j++) {
            copy_str(dst->ingredients[j], sizeof dst->ingredients[j], src->ingredients[j]);
        }
    }

    /* resources are keyed by name on the wire (pot_0, cutting_board_1,
     * plate_stack, and whatever the game grows later); the layout keeps the key
     * verbatim in `name` and classifies by prefix only, so an unknown key is
     * carried through instead of rejected. */
    resources = up->resource_count;
    if (resources > KOS_SHM_MAX_RESOURCES) {
        resources = KOS_SHM_MAX_RESOURCES;
    }
    world->resource_count = (uint32_t)resources;
    for (i = 0; i < resources; i++) {
        const proto_resource *src = &up->resources[i];
        kos_resource *dst = &world->resources[i];

        copy_str(dst->name, sizeof dst->name, src->key);
        copy_str(dst->state, sizeof dst->state, src->state);
        copy_str(dst->content, sizeof dst->content, src->content);
        dst->time_left = src->has_time_left ? double_to_float(src->time_left) : 0.0f;
        dst->chef_id = src->has_chef_id ? (int32_t)src->chef_id : -1;
        dst->count = src->has_count ? (int32_t)src->count : 0;
        dst->progress = src->has_progress ? double_to_float(src->progress) : 0.0f;
    }
}

/* ======================================================== 帧屏障 */

/*
 * Probe the input slots without blocking.
 *
 * timeout_ms = 0 is the shm module's documented "just look": it advances
 * last_frame[k] for the slots that were ready, which is exactly "consume this
 * frame's input once", and reports the rest through the mask. Nothing here can
 * block the socket.
 */
static uint64_t probe_slots(bridge *b)
{
    uint64_t missing = 0;
    uint32_t k;

    if (b->chefs <= 0) {
        /* No slots to wait for: every frame is trivially complete. shm.h
         * rejects chef_count == 0, so this must not reach the barrier. */
        return 0;
    }

    /*
     * 只看，不改水位。
     *
     * 这里**不能**用 kos_shm_wait_frame_after()：它会把"比上次更新"的那一格
     * 消费掉（推进 last_frame[k]），而本函数在屏障窗口内会被事件循环反复调用，
     * 于是每次探测都把 Worker 刚写进来的那一版吃掉，等到真正下发这一帧时，
     * 槽看起来又"没更新"了 —— 实测（2026-10-16，60Hz 与 300Hz 两种 stub 发布
     * 率）0 号槽被判缺失的帧占 88%～98%，插件收到大量合成的 stop，持续动作
     * （move）被打断。消费必须只发生在 dispatch_pending() 真正取用那一帧时。
     *
     * 另外 timeout=0 会让 slot_snapshot_until() 的"序号是奇数"分支直接返回
     * BUSY（deadline 已经过期，不重试），所以旧写法连"只是看一眼"都做不到。
     */
    for (k = 0; k < (uint32_t)b->chefs; k++) {
        kos_input_action action;

        int rc = kos_shm_slot_read(b->shm, k, &action);

        if (rc == KOS_OK && action.frame_id > b->last_frame[k]) {
            b->slot_fresh_ms[k] = now_ms(); /* 这一格刚交了新的一版 */
            continue;
        }
        /*
         * 没交新版本 ≠ 这个 Worker 死了。
         *
         * 桥每帧要求"槽比上次更新"，可 Worker 的发布节拍是它自己的：只要它比游戏帧率慢一点，
         * 就会有帧落在两次发布之间（实测 120Hz 的 Worker 对 117fps 的游戏，约一半的帧如此）。
         * 那种帧不该把厨师冻住 —— 保留它最后交的那一版意图，只有超过 --input-grace-ms
         * 都没动静（Worker 崩了/退出了）才降级成 idle，这才是 decisions.md 2026-10-15
         * 那条"Worker 崩溃时 Bridge 继续发 idle"要的效果。
         */
        if (now_ms() - b->slot_fresh_ms[k] > b->input_grace_ms) {
            missing |= (uint64_t)1u << k;
        }
    }
    return missing;
}

static void format_chef_list(uint64_t mask, int chefs, char *out, size_t cap)
{
    size_t used = 0;
    int k;

    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    for (k = 0; k < chefs && k < 64; k++) {
        char num[16];
        int n;

        if ((mask & ((uint64_t)1 << (unsigned)k)) == 0) {
            continue;
        }
        n = snprintf(num, sizeof num, "%s%d", used > 0 ? "," : "", k);
        if (n < 0 || used + (size_t)n + 1 > cap) {
            break; /* truncated on purpose: the mask is logged next to it */
        }
        memcpy(out + used, num, (size_t)n);
        used += (size_t)n;
        out[used] = '\0';
    }
}

/*
 * Translate one worker slot into the plugin's action message. move and interact
 * carry data; stop, pickup and dash are just a name the plugin switches on. `none`
 * is not an action at all -- it means this Worker has nothing to say this frame --
 * so it degrades to idle stop, exactly like a missing slot.
 */
static int build_action_from_slot(int chef, const kos_input_action *action,
                                  char *out, size_t cap, size_t *len)
{
    /* A corrupt slot may omit the terminator; `strlen` on it would read past the
     * payload and leak/crash. Treat that like an unusable action. */
    if (memchr(action->target_object, '\0', sizeof action->target_object) == NULL)
        return PROTO_ERR_RANGE;

    switch (action->kind) {
    case KOS_ACTION_MOVE:
        if (!double_is_finite(action->target_x) || !double_is_finite(action->target_y)) {
            return PROTO_ERR_RANGE; /* no usable target: fall back to idle */
        }
        return protocol_build_action_move(chef, action->target_x, action->target_y,
                                          out, cap, len);
    case KOS_ACTION_INTERACT:
        if (action->target_object[0] == '\0') {
            return PROTO_ERR_MISSING; /* interact with nothing is not a message */
        }
        return protocol_build_action_interact(chef, action->target_object, out, cap, len);
    case KOS_ACTION_PICKUP:
        return protocol_build_action_pickup(chef, out, cap, len);
    case KOS_ACTION_DASH:
        return protocol_build_action_dash(chef, out, cap, len);
    case KOS_ACTION_PATH: {
        proto_waypoint wps[PROTO_MAX_WAYPOINTS];
        size_t count = action->waypoint_count;
        if (count == 0 || count > PROTO_MAX_WAYPOINTS) {
            return PROTO_ERR_RANGE;
        }
        for (size_t w = 0; w < count; w++) {
            if (!double_is_finite(action->waypoints[w].x)
                || !double_is_finite(action->waypoints[w].y)) {
                return PROTO_ERR_RANGE;
            }
            wps[w].x = action->waypoints[w].x;
            wps[w].y = action->waypoints[w].y;
        }
        return protocol_build_action_path(chef, (long long)action->seq, wps, count, out, cap, len);
    }
    case KOS_ACTION_FACE: {
        /* target_object 优先：Worker 只能用一个空串表示"没有具名目标"，
         * 坐标 0,0 是个合法位置，不能拿它当"没给坐标"。 */
        int has_target = 0;
        if (action->target_object[0] == '\0'
            && double_is_finite(action->target_x) && double_is_finite(action->target_y)) {
            has_target = 1;
        }
        if (!has_target && action->target_object[0] == '\0') {
            return PROTO_ERR_MISSING;
        }
        return protocol_build_action_face(chef, has_target, action->target_x, action->target_y,
                                          action->target_object, out, cap, len);
    }
    case KOS_ACTION_WORK: {
        if (action->target_object[0] == '\0') {
            return PROTO_ERR_MISSING;
        }
        return protocol_build_action_work(chef, action->target_object, out, cap, len);
    }
    case KOS_ACTION_STOP:
    case KOS_ACTION_NONE:
        return PROTO_ERR_TYPE; /* the idle path: the caller sends `stop` itself */
    default:
        return PROTO_ERR_TYPE; /* unknown kind (torn or corrupt slot) -> idle */
    }
}

/*
 * Close the open frame: one action per chef in [0, chefs). A slot that was
 * ready is translated; a missing slot (or one whose contents cannot be turned
 * into a message) gets idle `stop`, which is what keeps a crashed Worker from
 * stopping the game.
 */
static void dispatch_pending(bridge *b, uint64_t missing, int timed_out)
{
    char buf[ACTION_BUF];
    char list[64];
    uint64_t mask = missing;
    int moved = 0;
    int interacted = 0;
    int picked_up = 0;
    int dashed = 0;
    int pathed = 0;
    int faced = 0;
    int worked = 0;
    int idle = 0;
    int sent = 0;
    int k;

    for (k = 0; k < b->chefs; k++) {
        kos_input_action action;
        size_t len = 0;
        int built;

        memset(&action, 0, sizeof action);
        if ((mask & ((uint64_t)1 << (unsigned)k)) != 0) {
            built = protocol_build_action_stop(k, buf, sizeof buf, &len);
            idle++;
        } else if (kos_shm_slot_read(b->shm, (uint32_t)k, &action) != KOS_OK) {
            /* fall through to the unreadable branch below */
            /* Ready by frame_id but unreadable now: the slot was overwritten or
             * torn. Idle is the only honest answer for this frame. */
            built = protocol_build_action_stop(k, buf, sizeof buf, &len);
            idle++;
        } else {
            /* 消费这一版：本帧的输入只取用一次，下一次探测要求更新的一版。 */
            b->last_frame[k] = action.frame_id;
            built = build_action_from_slot(k, &action, buf, sizeof buf, &len);
            if (built != PROTO_OK) {
                /* stop/none mean "nothing to do this frame": routine, and DEBUG is
                 * the right level, otherwise a quiet Worker would fill the log.
                 * Anything else failing here is the Worker's problem -- move or
                 * interact with unusable parameters, or a kind no builder knows --
                 * and the input is replaced by idle, so it has to be visible
                 * without turning on --verbose. */
                if (action.kind == KOS_ACTION_STOP || action.kind == KOS_ACTION_NONE) {
                    kos_log_debug(b->log, "msg_ignored",
                                  "reason=slot-action kind=%s chef=%d detail=%s",
                                  kos_action_kind_name(action.kind), k,
                                  protocol_strerror(built));
                } else {
                    kos_log_warn(b->log, "msg_ignored",
                                 "reason=slot-action-rejected kind=%s chef=%d detail=%s "
                                 "(fell back to idle stop)",
                                 kos_action_kind_name(action.kind), k,
                                 protocol_strerror(built));
                }
                built = protocol_build_action_stop(k, buf, sizeof buf, &len);
                idle++;
            } else if (action.kind == KOS_ACTION_MOVE) {
                moved++;
            } else if (action.kind == KOS_ACTION_INTERACT) {
                interacted++;
            } else if (action.kind == KOS_ACTION_PICKUP) {
                picked_up++;
            } else if (action.kind == KOS_ACTION_DASH) {
                dashed++;
            } else if (action.kind == KOS_ACTION_PATH) {
                pathed++;
            } else if (action.kind == KOS_ACTION_FACE) {
                faced++;
            } else if (action.kind == KOS_ACTION_WORK) {
                worked++;
            }
        }
        if (built != PROTO_OK) {
            kos_log_error(b->log, "frame_error", "stage=build_action chef=%d rc=%s",
                          k, protocol_strerror(built));
            continue;
        }
        /* net.h: 0 = the whole frame went out, -1 = it did not (and the link
         * was dropped, because a partial frame cannot be resynchronized). */
        if (kos_net_send_frame(b->net, buf, len) < 0) {
            kos_log_warn(b->log, "frame_error",
                         "stage=send_action chef=%d frame=%" PRIu64 " reason=%s",
                         k, b->pending_frame, net_error_text(b));
        } else {
            sent++;
        }
    }

    if (idle > 0 && mask != 0) {
        format_chef_list(mask, b->chefs, list, sizeof list);
        kos_log_warn(b->log, "slots_missing",
                     "frame=%" PRIu64 " chefs=%d mask=0x%" PRIx64 " missing=%s timed_out=%d",
                     b->pending_frame, b->chefs, mask, list, timed_out);
    }
    kos_log_info(b->log, "actions_sent",
                 "frame=%" PRIu64 " chefs=%d sent=%d idle=%d move=%d interact=%d "
                 "pickup=%d dash=%d path=%d face=%d work=%d timed_out=%d",
                 b->pending_frame, b->chefs, sent, idle, moved, interacted, picked_up, dashed,
                 pathed, faced, worked, timed_out);

    b->pending = 0;
    b->frames_dispatched++;
    if (b->max_frames > 0 && b->frames_dispatched >= b->max_frames) {
        b->stop_reason = "max_frames";
        g_stop = 1;
    }
}

static void service_pending_frame(bridge *b)
{
    uint64_t missing;

    if (!b->pending) {
        return;
    }
    if (b->paused) {
        /* Dispatching is suspended (plugin timed out): drop the open frame
         * instead of letting a backlog of them fire when the plugin returns. */
        b->pending = 0;
        return;
    }

    missing = probe_slots(b);
    if (missing != 0 && now_ms() < b->pending_deadline_ms) {
        return; /* still inside the barrier window: give the workers their time */
    }
    dispatch_pending(b, missing, missing != 0);
}

/* ======================================================== 消息处理 */

static void join_actions(const proto_hello *hello, char *out, size_t cap)
{
    size_t used = 0;
    size_t i;

    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    for (i = 0; i < hello->action_count; i++) {
        size_t n = strlen(hello->actions[i]);

        if (used + n + 2 > cap) {
            break;
        }
        if (used > 0) {
            out[used++] = ',';
        }
        memcpy(out + used, hello->actions[i], n);
        used += n;
        out[used] = '\0';
    }
}

/*
 * One state_update = one frame.
 *
 * The world snapshot is published first so a reader never waits behind the
 * barrier; then the previously open frame is flushed (a slow worker must not be
 * able to hold the pipeline), and only then is the new frame opened, with its
 * deadline and with the readiness semaphore drained so that stale posts cannot
 * wake the next frame's wait.
 */
static void on_state_update(bridge *b, const proto_state_update *up, const proto_diag *diag)
{
    kos_world_state world;
    int64_t now;

    world_from_state_update(up, &world);
    if (kos_shm_world_write(b->shm, &world) != KOS_OK) {
        kos_log_warn(b->log, "frame_error", "stage=shm_world_write frame=%" PRId64
                                            " state=%s",
                     b->frames + 1, b->state_name);
    }

    b->frames++;
    now = now_ms();

    if (b->paused) {
        b->pending = 0;
    } else {
        if (b->pending) {
            uint64_t missing = probe_slots(b);
            dispatch_pending(b, missing, 0);
        }
        b->pending = 1;
        b->pending_frame = (uint64_t)b->frames;
        b->pending_deadline_ms = now + b->frame_timeout_ms;
        kos_shm_drain_ready(b->shm);
    }

    kos_log_debug(b->log, "state_update",
                  "frame=%" PRId64 " chefs=%zu orders=%zu resources=%zu score=%" PRId64
                  " game_time=%" PRId64 " game_time_ms=%" PRId64
                  " dropped=%zu truncated=%zu pending=%d",
                  b->frames, up->chef_count, up->order_count, up->resource_count,
                  double_to_ll(up->score), double_to_ll(up->game_time),
                  (int64_t)up->game_time_ms,
                  diag != NULL ? diag->dropped_items : 0,
                  diag != NULL ? diag->truncated_strings : 0, b->pending);

    if (b->frames % SUMMARY_EVERY_FRAMES == 0) {
        kos_log_info(b->log, "state_summary",
                     "frames=%" PRId64 " chefs=%zu orders=%zu score=%" PRId64
                     " game_time=%" PRId64 " paused=%d",
                     b->frames, up->chef_count, up->order_count,
                     double_to_ll(up->score), double_to_ll(up->game_time), b->paused);
    }
}

/*
 * End a dispatch suspension. The suspension has two possible ends -- the plugin
 * speaks again on the same socket, or it reconnects after the half-open session
 * was dropped -- and both are worth exactly one plugin_resumed line.
 */
static void resume_plugin(bridge *b, const char *how)
{
    if (!b->paused) {
        return;
    }
    b->paused = 0;
    kos_log_info(b->log, "plugin_resumed",
                 "reason=%s silent_ms=%" PRId64 " timeout_ms=%d",
                 how, now_ms() - b->last_heartbeat_ms, b->heartbeat_timeout_ms);
}

static void on_heartbeat(bridge *b)
{
    resume_plugin(b, "heartbeat");
    b->last_heartbeat_ms = now_ms();
}

static void handle_message(bridge *b, const char *buf, size_t len)
{
    static proto_state_update state_update; /* ~25 KiB: not a stack object */
    static proto_config config;
    json_error jerr;
    json_value *root = NULL;
    const json_value *type_value;
    const char *type_name;
    proto_msg_type type;
    proto_diag diag;
    int rc;

    json_error_init(&jerr);
    rc = json_parse_ex(buf, len, &root, &jerr);
    if (rc != JSON_OK || root == NULL) {
        /* §5.3: an unparsable frame is logged and that message alone is
         * dropped. The connection stays up -- the plugin's next frame may be
         * perfectly fine. */
        kos_log_warn(b->log, "parse_error",
                     "reason=%s detail=\"%s\" offset=%zu len=%zu",
                     json_strerror(rc), jerr.detail, jerr.offset, len);
        json_free(root);
        return;
    }
    if (json_typeof(root) != JSON_OBJECT) {
        kos_log_warn(b->log, "parse_error", "reason=not-an-object len=%zu", len);
        json_free(root);
        return;
    }
    type_value = json_object_get(root, "type");
    type_name = json_string_value(type_value);
    if (type_value == NULL || type_name == NULL) {
        kos_log_warn(b->log, "parse_error", "reason=missing-type len=%zu", len);
        json_free(root);
        return;
    }

    type = protocol_message_type(root);
    memset(&diag, 0, sizeof diag);

    switch (type) {
    case PROTO_MSG_STATE_UPDATE:
        rc = protocol_parse_state_update(root, &state_update, &diag);
        if (rc != PROTO_OK) {
            /* The one missing field the whole Linux side times on deserves a
             * message a person can act on: the generic "required field is
             * missing" does not say which field, and its usual cause is an
             * old plugin build. */
            if (rc == PROTO_ERR_MISSING && json_object_get(root, "game_time_ms") == NULL) {
                kos_log_warn(b->log, "parse_error",
                             "type=state_update reason=missing game_time_ms (required since "
                             "shm version %u) dropped=1",
                             (unsigned)KOS_SHM_VERSION);
            } else {
                kos_log_warn(b->log, "parse_error", "type=state_update reason=%s",
                             protocol_strerror(rc));
            }
            break;
        }
        on_state_update(b, &state_update, &diag);
        break;

    case PROTO_MSG_HEARTBEAT: {
        proto_heartbeat heartbeat;

        rc = protocol_parse_heartbeat(root, &heartbeat);
        if (rc != PROTO_OK) {
            kos_log_warn(b->log, "parse_error", "type=heartbeat reason=%s",
                         protocol_strerror(rc));
            break;
        }
        on_heartbeat(b);
        break;
    }

    case PROTO_MSG_HELLO: {
        proto_hello hello;
        char actions[PROTO_MAX_ACTIONS * PROTO_KEY_MAX];

        rc = protocol_parse_hello(root, &hello, &diag);
        if (rc != PROTO_OK) {
            kos_log_warn(b->log, "parse_error", "type=hello reason=%s",
                         protocol_strerror(rc));
            break;
        }
        join_actions(&hello, actions, sizeof actions);
        kos_log_info(b->log, "plugin_hello", "instance=%s version=%s actions=%s count=%zu",
                     hello.instance, hello.version, actions, hello.action_count);
        break;
    }

    case PROTO_MSG_ECHO: {
        proto_echo echo;
        char reply[PROTO_STR_MAX + 64];
        size_t reply_len = 0;

        rc = protocol_parse_echo(root, &echo);
        if (rc != PROTO_OK) {
            kos_log_warn(b->log, "parse_error", "type=echo reason=%s",
                         protocol_strerror(rc));
            break;
        }
        /* §5.3 puts echo first in the integration order: the plugin sends a
         * token, this side sends it straight back. */
        rc = protocol_build_echo_reply(echo.token, reply, sizeof reply, &reply_len);
        if (rc != PROTO_OK) {
            kos_log_error(b->log, "frame_error", "stage=build_echo_reply rc=%s",
                          protocol_strerror(rc));
            break;
        }
        if (kos_net_send_frame(b->net, reply, reply_len) < 0) {
            kos_log_warn(b->log, "frame_error", "stage=send_echo_reply reason=%s",
                         net_error_text(b));
        } else {
            kos_log_debug(b->log, "echo_reply_sent", "token=%s len=%zu", echo.token,
                          reply_len);
        }
        break;
    }

    case PROTO_MSG_ACK: {
        proto_ack ack;

        rc = protocol_parse_ack(root, &ack, &diag);
        if (rc != PROTO_OK) {
            kos_log_warn(b->log, "parse_error", "type=ack reason=%s",
                         protocol_strerror(rc));
            break;
        }
        /* The plugin acks every action; a nack is its business, not an error
         * for us, so this never escalates past DEBUG. */
        if (ack.has_seq) {
            kos_log_debug(b->log, "plugin_ack",
                          "chef=%" PRId64 " action=%s ok=%d seq=%" PRId64 " detail=%s",
                          (int64_t)ack.chef_id, ack.action, ack.ok, (int64_t)ack.seq, ack.detail);
        } else {
            kos_log_debug(b->log, "plugin_ack",
                          "chef=%" PRId64 " action=%s ok=%d detail=%s",
                          (int64_t)ack.chef_id, ack.action, ack.ok, ack.detail);
        }
        break;
    }

    case PROTO_MSG_CONFIG: {
        rc = protocol_parse_config(root, &config, &diag);
        if (rc != PROTO_OK) {
            kos_log_warn(b->log, "parse_error", "type=config reason=%s",
                         protocol_strerror(rc));
            break;
        }
        /* Recorded, deliberately not acted upon: choosing a strategy is the
         * Scheduler's decision, the bridge only executes (spec "Bride 只搬运"). */
        kos_log_info(b->log, "config_received", "strategy=%s orders=%zu dropped=%zu",
                     config.strategy, config.order_count, diag.dropped_items);
        break;
    }

    case PROTO_MSG_STATE_REQUEST:
    case PROTO_MSG_ACTION:
    case PROTO_MSG_ECHO_REPLY:
    case PROTO_MSG_UNKNOWN:
    default:
        kos_log_debug(b->log, "msg_ignored", "type=%s len=%zu", type_name, len);
        break;
    }

    json_free(root);
}

/* ============================================================ 链路 */

static void on_link_up(bridge *b)
{
    resume_plugin(b, "link_up"); /* logs while last_heartbeat_ms is still stale */
    b->last_heartbeat_ms = now_ms();
    b->last_heartbeat_sent_ms = 0; /* send one on the next tick */
    b->pending = 0;
    b->link_peer_fd = kos_net_peer_fd(b->net);
    kos_log_info(b->log, "link_up", "mode=%s host=%s port=%d peer_fd=%d listen_fd=%d",
                 b->link_mode == KOS_NET_MODE_LISTEN ? "listen" : "connect", b->host,
                 b->port, b->link_peer_fd, kos_net_listen_fd(b->net));
}

/*
 * net.h folds "the stream cannot be framed any more" into its last-error text
 * and drops the link; the cause itself is a frame-level failure, so it is also
 * reported under the frame_error token instead of only as a link_down reason.
 * This is the one place where the bridge classifies net's human-readable text:
 * there is no error code for it, and guessing wrong costs a log line, not
 * behaviour -- the drop has already happened either way.
 */
static int reason_is_framing_failure(const char *reason)
{
    return reason != NULL && strstr(reason, "framing") != NULL;
}

static void on_link_down(bridge *b)
{
    const char *reason = net_error_text(b);

    b->pending = 0;
    b->link_peer_fd = -1;
    if (reason_is_framing_failure(reason)) {
        kos_log_warn(b->log, "frame_error", "stage=framing reason=%s", reason);
    }
    kos_log_info(b->log, "link_down", "mode=%s reason=%s",
                 b->link_mode == KOS_NET_MODE_LISTEN ? "listen" : "connect", reason);
}

/*
 * Exactly one log line per transition, however many loop iterations it takes.
 *
 * A peer can also change without any transition: net_accept() (net.c) closes the
 * previous peer and adopts the newest connection, all inside one kos_net_wait(),
 * so the link never leaves UP and the state change checked below cannot see it. That swap
 * is still an IPC disconnect followed by an IPC connect, which this process is
 * required to log, so the peer fd is tracked separately and reported here. The
 * line is informational: the bridge's own state (open frame, heartbeat clock,
 * pause flag) deliberately stays as it is -- a reconnect must not cost the game
 * a frame, and the heartbeat timeout path is what drops a silent session.
 */
static void check_link_edges(bridge *b)
{
    kos_net_state state = kos_net_state_of(b->net);
    int peer_fd;

    if (state == b->link_state) {
        if (state == KOS_NET_UP) {
            peer_fd = kos_net_peer_fd(b->net);
            if (peer_fd >= 0 && peer_fd != b->link_peer_fd) {
                kos_log_info(b->log, "link_peer_replaced",
                             "mode=%s old_peer_fd=%d new_peer_fd=%d "
                             "rule=newest-connection-wins",
                             b->link_mode == KOS_NET_MODE_LISTEN ? "listen" : "connect",
                             b->link_peer_fd, peer_fd);
                b->link_peer_fd = peer_fd;
            }
        }
        return;
    }
    b->link_state = state;
    if (state == KOS_NET_UP) {
        on_link_up(b);
    } else {
        on_link_down(b);
    }
}

static void send_heartbeat(bridge *b)
{
    char buf[64];
    size_t len = 0;

    if (protocol_build_heartbeat((long long)time(NULL), buf, sizeof buf, &len) != PROTO_OK) {
        kos_log_error(b->log, "frame_error", "stage=build_heartbeat");
        return;
    }
    b->last_heartbeat_sent_ms = now_ms();
    if (kos_net_send_frame(b->net, buf, len) < 0) {
        kos_log_warn(b->log, "frame_error", "stage=send_heartbeat reason=%s",
                     net_error_text(b));
    } else {
        kos_log_debug(b->log, "heartbeat_sent", "len=%zu interval_ms=%d", len,
                      b->heartbeat_interval_ms);
    }
}

/*
 * Liveness is judged on heartbeats, which is what §5.3 says ("超时未收到心跳：
 * 调度器认为插件断开，暂停调度"). Counting any inbound message would let a
 * plugin that streams state but has stopped answering mask a half-dead link,
 * and the plugin's own TcpLink uses the same heartbeat-only rule.
 */
static void check_plugin_timeout(bridge *b)
{
    int64_t now;

    if (b->heartbeat_timeout_ms <= 0) {
        return;
    }
    now = now_ms();
    if (!b->paused && now - b->last_heartbeat_ms > b->heartbeat_timeout_ms) {
        b->paused = 1;
        b->pending = 0; /* stop dispatching, per §5.3 */
        kos_log_warn(b->log, "plugin_timeout",
                     "silent_ms=%" PRId64 " timeout_ms=%d dispatched=%" PRId64,
                     now - b->last_heartbeat_ms, b->heartbeat_timeout_ms,
                     b->frames_dispatched);
        /* A half-open connection never reports EOF, so the socket has to be torn
         * down here; net.h provides drop() for exactly this decision. LISTEN
         * keeps the port and waits for the plugin's next connection, CONNECT
         * starts redialing, and either way the link_up edge clears `paused`. */
        kos_net_drop(b->net);
    }
}

/* Smallest deadline the loop has to honour, capped so it stays responsive.
 * Never negative: a passed deadline means "service this now", not "sleep -3ms". */
static int next_wakeup_ms(const bridge *b)
{
    int64_t now = now_ms();
    int64_t wait = MAX_POLL_MS;

    if (b->link_state == KOS_NET_UP) {
        if (b->heartbeat_interval_ms > 0) {
            int64_t due = (int64_t)b->heartbeat_interval_ms - (now - b->last_heartbeat_sent_ms);
            if (due < wait) {
                wait = due;
            }
        }
        if (!b->paused && b->heartbeat_timeout_ms > 0) {
            int64_t left = (int64_t)b->heartbeat_timeout_ms - (now - b->last_heartbeat_ms);
            if (left < wait) {
                wait = left;
            }
        }
    }
    if (b->pending) {
        int64_t left = b->pending_deadline_ms - now;
        if (left < wait) {
            wait = left;
        }
    }
    if (wait < 0) {
        wait = 0;
    }
    if (wait > MAX_POLL_MS) {
        wait = MAX_POLL_MS;
    }
    return (int)wait;
}

/* ======================================================= 启动 / 收尾 */

static void log_startup_error(bridge *b, const char *stage, const kos_error *err)
{
    char text[ERR_BUF];

    kos_error_format(err, text, sizeof text);
    kos_log_error(b->log, "startup_error", "stage=%s error=%s", stage, text);
}

static int open_shared_state(bridge *b)
{
    kos_error err;
    int rc;

    memset(&b->shm_opts, 0, sizeof b->shm_opts);
    b->shm_opts.object_name = b->state_name;
    b->shm_opts.sem_name = b->sem_name;
    b->shm_opts.dir = NULL; /* shm.h default: /dev/shm (overridable by env) */
    b->shm_opts.perm = 0;

    kos_error_clear(&err);
    rc = kos_shm_open(&b->shm_opts, KOS_SHM_CREATE, &b->shm, &err);
    if (rc == KOS_ERR_EXISTS) {
        /* A stale segment from a crashed run. Refusing to start would make the
         * Supervisor's restart policy useless, and shm.h marks RECREATE as the
         * mode meant for exactly this. */
        kos_log_warn(b->log, "shm_recreate", "reason=stale-segment object=%s",
                     b->state_name);
        kos_error_clear(&err);
        rc = kos_shm_open(&b->shm_opts, KOS_SHM_RECREATE, &b->shm, &err);
    }
    if (rc != KOS_OK) {
        log_startup_error(b, "shm_open", &err);
        return -1;
    }
    kos_log_info(b->log, "shm_ready", "object=%s sem=%s chefs=%d requested=%d",
                 b->shm_opts.object_name, b->shm_opts.sem_name, b->chefs,
                 b->chefs_requested);
    return 0;
}

static int open_link(bridge *b)
{
    kos_net_opts opts;
    kos_error err;

    kos_net_opts_init(&opts);
    opts.mode = b->link_mode;
    opts.host = b->host;
    opts.port = b->port;
    /* listen mode: --host is the bind address only when the user asked for one;
     * otherwise the net module's default (127.0.0.1) applies. Loopback is enough
     * for the real-game path -- WSL2's localhost forwarding reaches a listener
     * bound only to the VM's loopback, which was measured rather than assumed --
     * and the link carries no authentication, so binding every interface hands
     * "who gets to be the plugin" to anyone on the LAN. --host 0.0.0.0 is the
     * escape hatch for a cross-machine demo. */
    if (b->link_mode == KOS_NET_MODE_LISTEN && b->host_given) {
        opts.bind_addr = b->host;
    }

    kos_error_clear(&err);
    if (kos_net_open(&opts, &b->net, &err) != KOS_OK) {
        log_startup_error(b, "net_open", &err);
        return -1;
    }
    kos_log_info(b->log, "net_ready", "mode=%s host=%s port=%d listen_fd=%d",
                 b->link_mode == KOS_NET_MODE_LISTEN ? "listen" : "connect", b->host,
                 b->port, kos_net_listen_fd(b->net));
    return 0;
}

/*
 * One byte per interval tells the Supervisor this process is still running its loop.
 * A broken pipe means the Supervisor is gone; the Bridge then exits like any other
 * child instead of becoming an orphan holding /kitchenos_state. Only active when the
 * Supervisor passed the fd, so a standalone `kitchenos-bridge` is unchanged.
 */
static void beat_supervisor(bridge *b)
{
    char byte = '.';
    int64_t now;
    ssize_t n;

    if (b->supervisor_fd < 0 || b->supervisor_heartbeat_ms <= 0) {
        return;
    }
    now = now_ms();
    if (now - b->last_supervisor_beat_ms < b->supervisor_heartbeat_ms) {
        return;
    }
    n = write(b->supervisor_fd, &byte, 1);
    b->last_supervisor_beat_ms = now;
    if (n == 1 || (n < 0 && errno == EAGAIN)) {
        return;
    }
    kos_log_warn(b->log, "supervisor_gone", "fd=%d error=%s", b->supervisor_fd,
                 strerror(errno));
    b->stop_reason = "supervisor_gone";
    g_stop = 1;
}

static void run_loop(bridge *b)
{
    b->link_state = KOS_NET_DOWN; /* the first real transition logs link_up */
    b->last_supervisor_beat_ms = now_ms();

    while (!g_stop) {
        int timeout = next_wakeup_ms(b);
        int wait_rc = kos_net_wait(b->net, timeout);

        if (wait_rc < 0) {
            kos_log_warn(b->log, "net_error", "stage=wait reason=%s", net_error_text(b));
        }
        check_link_edges(b);

        for (;;) {
            size_t len = 0;
            int rc;

            if (g_stop) {
                break;
            }
            rc = kos_net_recv_frame(b->net, b->frame_buf, sizeof b->frame_buf, &len);
            if (rc != 1) {
                if (rc < 0) {
                    /* net drops the link itself on EOF/desync; a fatal return is
                     * reported here and the loop starts the next iteration with
                     * whatever state net is in. */
                    kos_log_warn(b->log, "frame_error", "stage=recv_frame reason=%s",
                                 net_error_text(b));
                }
                break;
            }
            handle_message(b, b->frame_buf, len);
        }

        if (kos_net_state_of(b->net) == KOS_NET_UP) {
            int64_t now = now_ms();

            if (b->heartbeat_interval_ms > 0 &&
                now - b->last_heartbeat_sent_ms >= b->heartbeat_interval_ms) {
                send_heartbeat(b);
            }
            check_plugin_timeout(b);
        }

        service_pending_frame(b);
        beat_supervisor(b);
    }
}

static const char *stop_reason(const bridge *b)
{
    if (g_stop_signal == SIGTERM || g_stop_signal == SIGINT) {
        return "signal";
    }
    if (b->stop_reason != NULL) {
        return b->stop_reason;
    }
    return "loop_end";
}

static int bridge_shutdown(bridge *b, int exit_code)
{
    kos_error err;
    const char *reason = stop_reason(b);

    if (g_stop_signal != 0) {
        kos_log_info(b->log, "signal", "name=%s", signal_name((int)g_stop_signal));
    }
    kos_log_info(b->log, "bridge_stop",
                 "reason=%s frames=%" PRId64 " dispatched=%" PRId64 " paused=%d",
                 reason, b->frames, b->frames_dispatched, b->paused);

    if (b->net != NULL) {
        kos_net_close(b->net);
        b->net = NULL;
    }
    if (b->shm != NULL) {
        kos_error_clear(&err);
        kos_shm_close(b->shm, &err);
        b->shm = NULL;
    }
    /* Idempotent by contract: unlinking what is already gone is success, so a
     * restart can always bind the port and recreate the segment. */
    kos_error_clear(&err);
    if (kos_shm_unlink(&b->shm_opts, &err) != KOS_OK) {
        char text[ERR_BUF];

        kos_error_format(&err, text, sizeof text);
        kos_log_warn(b->log, "shm_unlink_failed", "object=%s error=%s",
                     b->state_name, text);
    }

    kos_log_close(b->log);
    b->log = NULL;
    return exit_code;
}

static int bridge_main(bridge *b)
{
    kos_error err;

    if (install_signal_handlers() != 0) {
        fprintf(stderr, "kitchenos-bridge: cannot install signal handlers: %s\n",
                strerror(errno));
        return 1;
    }

    kos_error_clear(&err);
    if (kos_log_open(b->log_path, b->verbose ? KOS_LOG_DEBUG : KOS_LOG_INFO, &b->log,
                     &err) != KOS_OK) {
        char text[ERR_BUF];

        kos_error_format(&err, text, sizeof text);
        fprintf(stderr, "kitchenos-bridge: cannot open log %s: %s\n", b->log_path, text);
        return 1;
    }

    kos_log_info(b->log, "bridge_start",
                 "link=%s host=%s port=%d chefs=%d requested=%d state=%s sem=%s log=%s "
                 "frame_timeout_ms=%d input_grace_ms=%d heartbeat_timeout_ms=%d heartbeat_interval_ms=%d "
                 "max_frames=%ld pid=%ld",
                 b->link_mode == KOS_NET_MODE_LISTEN ? "listen" : "connect", b->host,
                 b->port, b->chefs, b->chefs_requested, b->state_name, b->sem_name,
                 b->log_path, b->frame_timeout_ms, b->input_grace_ms, b->heartbeat_timeout_ms,
                 b->heartbeat_interval_ms, b->max_frames, (long)getpid());
    if (b->chefs_requested != b->chefs) {
        kos_log_warn(b->log, "chefs_clamped", "requested=%d used=%d limit=%d",
                     b->chefs_requested, b->chefs, KOS_SHM_MAX_CHEFS);
    }

    if (open_shared_state(b) != 0) {
        return bridge_shutdown(b, 1);
    }
    if (open_link(b) != 0) {
        return bridge_shutdown(b, 1);
    }

    run_loop(b);
    return bridge_shutdown(b, 0);
}

int main(int argc, char **argv)
{
    int rc = parse_args(argc, argv, &g_bridge);

    if (rc == ARG_HELP) {
        print_help();
        return 0;
    }
    if (rc == ARG_VERSION) {
        printf("kitchenos-bridge %s\n", BRIDGE_VERSION);
        return 0;
    }
    if (rc != ARG_OK) {
        print_usage(stderr);
        return 2;
    }

    return bridge_main(&g_bridge);
}
