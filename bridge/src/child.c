/*
 * child.c -- Scheduler 与 Worker 共用的进程骨架。
 *
 * 这里没有业务代码：只保证两个新进程在"Bridge 还没建共享内存""Supervisor 还没
 * 开始读心跳""信号到达时正睡在 nanosleep 里"这三种真实启动顺序下都能可预测地工作。
 * 挂接失败不是错误状态而是等待状态——Bridge 与 Scheduler 由 Supervisor 同时拉起，
 * 谁先跑起来没有保证。
 *
 * Phase 3 起业务循环由 scheduler.c / worker.c 自己驱动：kos_child_setup() 建好
 * 日志/信号/重试时间，调用方每轮调用 kos_child_poll()，再按自己的节奏 sleep。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/child.h>
#include <kitchenos/policy.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHILD_VERSION "0.1.0"
#define CHILD_DEFAULT_HEARTBEAT_MS 500
#define CHILD_DEFAULT_ATTACH_RETRY_MS 100
#define CHILD_POLL_MS 100

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_stop_signal = 0;
static volatile sig_atomic_t g_sighup = 0;
static volatile sig_atomic_t g_sigusr1 = 0;

static void on_stop(int sig)
{
    g_stop_signal = sig;
    g_stop = 1;
}

static void on_sighup(int sig)
{
    (void)sig;
    g_sighup = 1;
}

static void on_sigusr1(int sig)
{
    (void)sig;
    g_sigusr1 = 1;
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

static int64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    if (ms <= 0) {
        return;
    }
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

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

static const char *kind_name(kos_child_kind kind)
{
    if (kind == KOS_CHILD_SCHEDULER) {
        return "scheduler";
    }
    if (kind == KOS_CHILD_NAVD) {
        return "navd";
    }
    return "worker";
}

void kos_child_opts_init(kos_child_opts *o, kos_child_kind kind)
{
    if (o == NULL) {
        return;
    }
    memset(o, 0, sizeof *o);
    o->kind = kind;
    o->chef_id = (kind == KOS_CHILD_WORKER) ? 0 : -1;
    o->instance = KOS_INSTANCE_NONE;
    o->supervisor_fd = -1;
    o->heartbeat_interval_ms = CHILD_DEFAULT_HEARTBEAT_MS;
    o->attach_retry_ms = CHILD_DEFAULT_ATTACH_RETRY_MS;
    o->chefs = 2;
    o->generation = 1;
    o->tick_ms = 50;
    o->slot_interval_ms = 10;
    o->lease_ttl_ms = 30000;
    copy_str(o->strategy, sizeof o->strategy, "fcfs");
    copy_str(o->dispatch, sizeof o->dispatch, "flexible");
}

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

static int parse_long(const char *text, long min, long max, long *out)
{
    char *end = NULL;
    long value;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min || value > max) {
        return -1;
    }
    *out = value;
    return 0;
}

void kos_child_print_usage(FILE *out, kos_child_kind kind)
{
    fprintf(out,
            "usage: kitchenos-%s --runtime-dir DIR [--state-name NAME] [--sem-name NAME]\n"
            "                      [--log FILE] [--supervisor-fd N]\n"
            "                      [--supervisor-heartbeat-ms M] [--attach-retry-ms M]\n"
            "                      [--kitchen FILE] [--recipes FILE] [--orders FILE]\n"
            "                      [--runs-dir DIR] [--strategy fcfs|edf|llf|hrrn] [--dispatch flexible|zoned] [--chefs N]\n"
            "                      [--tick-ms M] [--slot-interval-ms M] [--instance N]\n"
            "                      [--verbose] [--help] [--version]\n",
            kind_name(kind));
    if (kind == KOS_CHILD_WORKER) {
        fprintf(out, "       kitchenos-worker also takes --chef-id N (0..%d).\n",
                KOS_SHM_MAX_CHEFS - 1);
    }
    if (kind == KOS_CHILD_NAVD) {
        fprintf(out, "       kitchenos-navd also takes --nav-name NAME --nav-sem-name NAME.\n");
    }
    fprintf(out,
            "\n"
            "Scheduler: builds orders/tasks from config/{kitchen,recipes,orders}.example.json\n"
            "and the Bridge's shm world state, dispatches ready tasks over POSIX mq, and\n"
            "writes runs/<ts>/{events.jsonl,decisions.jsonl}. SIGHUP cycles FCFS/EDF/LLF/HRRN.\n"
            "Worker: receives its mq task, writes the target slot of /kitchenos_nav, reads the\n"
            "plan slot, publishes per-frame path/stop into its input slot, and reports task\n"
            "results on /kitchenos_events.\n"
            "navd: plans every chef with PIBT from each world snapshot and releases waypoints\n"
            "through the execution dependency graph; it owns the plan slots.\n"
            "\n"
            "Exit codes: 0 clean shutdown, 1 startup failure, 2 argument error.\n");
}

int kos_child_parse_args(kos_child_opts *o, int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;
        const char *eq;
        char name[64];
        size_t name_len;
        long number;

        if (arg[0] != '-' || arg[1] != '-') {
            return KOS_CHILD_ARG_ERR;
        }
        eq = strchr(arg, '=');
        name_len = (eq != NULL) ? (size_t)(eq - arg) : strlen(arg);
        if (name_len == 0 || name_len >= sizeof name) {
            return KOS_CHILD_ARG_ERR;
        }
        memcpy(name, arg, name_len);
        name[name_len] = '\0';
        if (eq != NULL) {
            value = eq + 1;
            if (value[0] == '\0') {
                return KOS_CHILD_ARG_ERR;
            }
        }

        if (strcmp(name, "--help") == 0) {
            return KOS_CHILD_ARG_HELP;
        } else if (strcmp(name, "--version") == 0) {
            return KOS_CHILD_ARG_VERSION;
        } else if (strcmp(name, "--verbose") == 0) {
            if (value != NULL) {
                return KOS_CHILD_ARG_ERR;
            }
            o->verbose = 1;
        } else if (strcmp(name, "--runtime-dir") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return KOS_CHILD_ARG_ERR;
            }
            copy_str(o->runtime_dir, sizeof o->runtime_dir, v);
        } else if (strcmp(name, "--state-name") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return KOS_CHILD_ARG_ERR;
            }
            o->state_name = v;
        } else if (strcmp(name, "--sem-name") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return KOS_CHILD_ARG_ERR;
            }
            o->sem_name = v;
        } else if (strcmp(name, "--log") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return KOS_CHILD_ARG_ERR;
            }
            copy_str(o->log_path, sizeof o->log_path, v);
        } else if (strcmp(name, "--supervisor-fd") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 0, 65535, &number) != 0) {
                return KOS_CHILD_ARG_ERR;
            }
            o->supervisor_fd = (int)number;
        } else if (strcmp(name, "--supervisor-heartbeat-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 0, 3600000, &number) != 0) {
                return KOS_CHILD_ARG_ERR;
            }
            o->heartbeat_interval_ms = (int)number;
        } else if (strcmp(name, "--attach-retry-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 10, 60000, &number) != 0) {
                return KOS_CHILD_ARG_ERR;
            }
            o->attach_retry_ms = (int)number;
        } else if (strcmp(name, "--chef-id") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (o->kind != KOS_CHILD_WORKER ||
                parse_long(v, 0, KOS_SHM_MAX_CHEFS - 1, &number) != 0) {
                return KOS_CHILD_ARG_ERR;
            }
            o->chef_id = (int)number;
        } else if (strcmp(name, "--kitchen") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return KOS_CHILD_ARG_ERR;
            }
            copy_str(o->kitchen_path, sizeof o->kitchen_path, v);
        } else if (strcmp(name, "--recipes") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return KOS_CHILD_ARG_ERR;
            }
            copy_str(o->recipes_path, sizeof o->recipes_path, v);
        } else if (strcmp(name, "--orders") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return KOS_CHILD_ARG_ERR;
            }
            copy_str(o->orders_path, sizeof o->orders_path, v);
        } else if (strcmp(name, "--runs-dir") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return KOS_CHILD_ARG_ERR;
            }
            copy_str(o->runs_dir, sizeof o->runs_dir, v);
        } else if (strcmp(name, "--strategy") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            kos_policy_kind kind;

            if (v == NULL || kos_policy_kind_from_name(v, &kind) != KOS_OK) {
                return KOS_CHILD_ARG_ERR;
            }
            copy_str(o->strategy, sizeof o->strategy, v);
        } else if (strcmp(name, "--dispatch") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL || (strcmp(v, "flexible") != 0 && strcmp(v, "zoned") != 0 &&
                              strcmp(v, "flex") != 0 && strcmp(v, "zone") != 0)) {
                return KOS_CHILD_ARG_ERR;
            }
            if (strcmp(v, "zone") == 0 || strcmp(v, "zoned") == 0) {
                copy_str(o->dispatch, sizeof o->dispatch, "zoned");
            } else {
                copy_str(o->dispatch, sizeof o->dispatch, "flexible");
            }
        } else if (strcmp(name, "--chefs") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 0, KOS_SHM_MAX_CHEFS, &number) != 0) {
                return KOS_CHILD_ARG_ERR;
            }
            o->chefs = (int)number;
        } else if (strcmp(name, "--instance") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 0, 255, &number) != 0) {
                return KOS_CHILD_ARG_ERR;
            }
            o->instance = (int)number;
        } else if (strcmp(name, "--lease-ttl-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 100, 3600000, &number) != 0) {
                return KOS_CHILD_ARG_ERR;
            }
            o->lease_ttl_ms = (int)number;
        } else if (strcmp(name, "--generation") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 1, 1000000, &number) != 0) {
                return KOS_CHILD_ARG_ERR;
            }
            o->generation = (int)number;
        } else if (strcmp(name, "--tick-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 5, 5000, &number) != 0) {
                return KOS_CHILD_ARG_ERR;
            }
            o->tick_ms = (int)number;
        } else if (strcmp(name, "--slot-interval-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 5, 5000, &number) != 0) {
                return KOS_CHILD_ARG_ERR;
            }
            o->slot_interval_ms = (int)number;
        } else if (strcmp(name, "--nav-name") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL || v[0] == '\0') {
                return KOS_CHILD_ARG_ERR;
            }
            copy_str(o->nav_name, sizeof o->nav_name, v);
        } else if (strcmp(name, "--nav-sem-name") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL || v[0] == '\0') {
                return KOS_CHILD_ARG_ERR;
            }
            copy_str(o->nav_sem_name, sizeof o->nav_sem_name, v);
        } else {
            return KOS_CHILD_ARG_ERR;
        }
    }
    return KOS_CHILD_ARG_OK;
}

int kos_child_default_config_path(const char *filename, char *buf, size_t len)
{
    char exe[KOS_DAEMON_PATH_MAX];
    char candidate[KOS_DAEMON_PATH_MAX];
    char *slash;
    ssize_t n;

    if (filename == NULL || buf == NULL || len == 0) {
        return -1;
    }
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        static const char *const prefixes[] = {"../../config", "../../../config",
                                               "../../../../config"};
        size_t p;

        exe[n] = '\0';
        slash = strrchr(exe, '/');
        if (slash != NULL) {
            *slash = '\0';
            /* build/ 与 build/san/ 深度不同，按候选前缀逐个试；不猜绝对仓库路径。 */
            for (p = 0; p < sizeof prefixes / sizeof prefixes[0]; p++) {
                if (snprintf(candidate, sizeof candidate, "%s/%s/%s", exe, prefixes[p],
                             filename) >= (int)sizeof candidate) {
                    continue;
                }
                if (access(candidate, R_OK) == 0) {
                    copy_str(buf, len, candidate);
                    return 0;
                }
            }
        }
    }
    if (snprintf(candidate, sizeof candidate, "config/%s", filename) < (int)sizeof candidate &&
        access(candidate, R_OK) == 0) {
        copy_str(buf, len, candidate);
        return 0;
    }
    return -1;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    static const int stop_signals[] = {SIGTERM, SIGINT};
    size_t i;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    /* 不设 SA_RESTART：主循环睡在 nanosleep 里，信号必须让它立刻醒来。 */
    sa.sa_flags = 0;
    for (i = 0; i < sizeof stop_signals / sizeof stop_signals[0]; i++) {
        if (sigaction(stop_signals[i], &sa, NULL) != 0) {
            return -1;
        }
    }

    sa.sa_handler = on_sighup;
    if (sigaction(SIGHUP, &sa, NULL) != 0) {
        return -1;
    }
    sa.sa_handler = on_sigusr1;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        return -1;
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
}

/* 返回 1 = 写了，0 = 还没到点/没配，-1 = 管道断了（Supervisor 已死）。 */
static int heartbeat_tick(kos_child_runtime *rt, int64_t now)
{
    char byte = '.';
    ssize_t n;

    if (rt->opts.supervisor_fd < 0 || rt->opts.heartbeat_interval_ms <= 0) {
        return 0;
    }
    if (rt->next_heartbeat_ms == 0) {
        rt->next_heartbeat_ms = now;
    }
    if (now < rt->next_heartbeat_ms) {
        return 0;
    }
    n = write(rt->opts.supervisor_fd, &byte, 1);
    if (n == 1) {
        rt->next_heartbeat_ms = now + rt->opts.heartbeat_interval_ms;
        return 1;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        if (errno == EAGAIN) {
            rt->next_heartbeat_ms = now + rt->opts.heartbeat_interval_ms;
        }
        return 0;
    }
    kos_log_warn(rt->log, "heartbeat_lost", "fd=%d error=%s", rt->opts.supervisor_fd,
                 strerror(errno));
    return -1;
}

int kos_child_setup(kos_child_runtime *rt, const kos_child_opts *opts)
{
    char default_name[64];
    kos_error err;
    int rc;
    kos_child_opts saved;

    if (rt == NULL || opts == NULL) {
        return 1;
    }
    /* Copy before clearing rt: callers legitimately pass &rt->opts, and a plain
     * memset would zero the source before it is read. */
    saved = *opts;
    memset(rt, 0, sizeof *rt);
    rt->opts = saved;
    opts = &saved;

    kos_error_clear(&err);
    rc = kos_runtime_dir(opts->runtime_dir[0] != '\0' ? opts->runtime_dir : NULL,
                         rt->runtime_dir, sizeof rt->runtime_dir, &err);
    if (rc != KOS_OK) {
        char text[256];

        kos_error_format(&err, text, sizeof text);
        fprintf(stderr, "kitchenos-%s: 运行目录不可用: %s\n", kind_name(opts->kind), text);
        return 1;
    }

    if (opts->log_path[0] != '\0') {
        copy_str(rt->log_path, sizeof rt->log_path, opts->log_path);
    } else {
        if (opts->kind == KOS_CHILD_SCHEDULER) {
            copy_str(default_name, sizeof default_name, "scheduler.log");
        } else if (opts->kind == KOS_CHILD_NAVD) {
            copy_str(default_name, sizeof default_name, "navd.log");
        } else {
            snprintf(default_name, sizeof default_name, "worker-%d.log", opts->chef_id);
        }
        if (kos_runtime_log_path(rt->runtime_dir, default_name, rt->log_path,
                                 sizeof rt->log_path, &err) != KOS_OK) {
            char text[256];

            kos_error_format(&err, text, sizeof text);
            fprintf(stderr, "kitchenos-%s: 日志路径不可用: %s\n", kind_name(opts->kind),
                    text);
            return 1;
        }
    }

    kos_error_clear(&err);
    if (kos_log_open(rt->log_path, opts->verbose ? KOS_LOG_DEBUG : KOS_LOG_INFO, &rt->log,
                     &err) != KOS_OK) {
        char text[256];

        kos_error_format(&err, text, sizeof text);
        fprintf(stderr, "kitchenos-%s: 打不开日志 %s: %s\n", kind_name(opts->kind),
                rt->log_path, text);
        return 1;
    }
    if (install_signal_handlers() != 0) {
        kos_log_error(rt->log, "signal_install_failed", "error=%s", strerror(errno));
        kos_log_close(rt->log);
        rt->log = NULL;
        return 1;
    }

    rt->next_attach_ms = now_ms();
    rt->next_attach_log_ms = now_ms();
    kos_log_info(rt->log,
                 opts->kind == KOS_CHILD_SCHEDULER
                     ? "scheduler_start"
                     : (opts->kind == KOS_CHILD_NAVD ? "navd_start" : "worker_start"),
                 "pid=%ld chef_id=%d log=%s state=%s sem=%s supervisor_fd=%d "
                 "heartbeat_ms=%d attach_retry_ms=%d kitchen=%s recipes=%s orders=%s "
                 "runs=%s strategy=%s chefs=%d generation=%d lease_ttl_ms=%d "
                 "tick_ms=%d slot_ms=%d version=%s",
                 (long)getpid(), opts->chef_id, rt->log_path,
                 opts->state_name != NULL ? opts->state_name : KOS_SHM_DEFAULT_NAME,
                 opts->sem_name != NULL ? opts->sem_name : KOS_SHM_DEFAULT_SEM_NAME,
                 opts->supervisor_fd, opts->heartbeat_interval_ms, opts->attach_retry_ms,
                 opts->kitchen_path[0] ? opts->kitchen_path : "(default)",
                 opts->recipes_path[0] ? opts->recipes_path : "(default)",
                 opts->orders_path[0] ? opts->orders_path : "(default)",
                 opts->runs_dir[0] ? opts->runs_dir : "(default)", opts->strategy,
                 opts->chefs, opts->generation, opts->lease_ttl_ms, opts->tick_ms,
                 opts->slot_interval_ms, CHILD_VERSION);
    return 0;
}

int kos_child_poll(kos_child_runtime *rt)
{
    int64_t now;
    kos_shm_opts shm_opts;
    kos_error err;
    int rc;

    if (rt == NULL || rt->log == NULL) {
        return 1;
    }
    if (g_stop) {
        return 1;
    }

    now = now_ms();
    if (heartbeat_tick(rt, now) < 0) {
        rt->heartbeat_dead = 1;
        return 1;
    }

    if (rt->shm == NULL && now >= rt->next_attach_ms) {
        memset(&shm_opts, 0, sizeof shm_opts);
        shm_opts.object_name = rt->opts.state_name;
        shm_opts.sem_name = rt->opts.sem_name;
        shm_opts.perm = 0;
        kos_error_clear(&err);
        rc = kos_shm_open(&shm_opts, KOS_SHM_ATTACH, &rt->shm, &err);
        if (rc == KOS_OK) {
            kos_world_state world;

            kos_log_info(rt->log, "shm_attached", "object=%s sem=%s",
                         rt->opts.state_name != NULL ? rt->opts.state_name
                                                     : KOS_SHM_DEFAULT_NAME,
                         rt->opts.sem_name != NULL ? rt->opts.sem_name
                                                   : KOS_SHM_DEFAULT_SEM_NAME);
            if (kos_shm_world_read(rt->shm, &world, KOS_SHM_READ_ATTEMPTS, NULL) == KOS_OK) {
                kos_log_info(rt->log, "world_snapshot",
                             "chefs=%u orders=%u resources=%u score=%lld game_time=%lld",
                             world.chef_count, world.order_count, world.resource_count,
                             (long long)world.score, (long long)world.game_time);
            }
        } else {
            if (now >= rt->next_attach_log_ms) {
                char text[256];

                kos_error_format(&err, text, sizeof text);
                kos_log_warn(rt->log, "shm_wait", "object=%s error=%s",
                             rt->opts.state_name != NULL ? rt->opts.state_name
                                                         : KOS_SHM_DEFAULT_NAME,
                             text);
                rt->next_attach_log_ms = now + 5000;
            }
            rt->next_attach_ms = now + rt->opts.attach_retry_ms;
        }
    }
    return g_stop ? 1 : 0;
}

void kos_child_sleep(kos_child_runtime *rt, int max_ms)
{
    int64_t now = now_ms();
    int64_t wait = max_ms > 0 ? max_ms : 1;

    if (rt != NULL) {
        if (rt->next_heartbeat_ms > now && rt->next_heartbeat_ms - now < wait) {
            wait = rt->next_heartbeat_ms - now;
        }
        if (rt->shm == NULL && rt->next_attach_ms > now &&
            rt->next_attach_ms - now < wait) {
            wait = rt->next_attach_ms - now;
        }
    }
    if (wait < 1) {
        wait = 1;
    }
    sleep_ms((int)wait);
}

void kos_child_teardown(kos_child_runtime *rt)
{
    const char *reason;

    if (rt == NULL) {
        return;
    }
    if (rt->log != NULL) {
        if (g_stop_signal != 0) {
            kos_log_info(rt->log, "signal", "name=%s", signal_name((int)g_stop_signal));
        }
        if (rt->heartbeat_dead) {
            kos_log_info(rt->log, "supervisor_gone", "reason=heartbeat-pipe-broken");
        }
        reason = rt->heartbeat_dead ? "supervisor-gone"
                                    : (g_stop_signal != 0 ? "signal" : "loop-end");
        kos_log_info(rt->log,
                     rt->opts.kind == KOS_CHILD_SCHEDULER ? "scheduler_stop"
                                                          : "worker_stop",
                     "reason=%s pid=%ld chef_id=%d", reason, (long)getpid(),
                     rt->opts.chef_id);
    }
    if (rt->shm != NULL) {
        kos_error err;

        kos_error_clear(&err);
        (void)kos_shm_close(rt->shm, &err);
        rt->shm = NULL;
    }
    if (rt->log != NULL) {
        kos_log_close(rt->log);
        rt->log = NULL;
    }
}

int kos_child_take_sighup(void)
{
    int pending = g_sighup != 0;

    g_sighup = 0;
    return pending;
}

int kos_child_take_sigusr1(void)
{
    int pending = g_sigusr1 != 0;

    g_sigusr1 = 0;
    return pending;
}

int kos_child_stop_signal(void)
{
    return (int)g_stop_signal;
}

int kos_child_run(kos_child_opts *o)
{
    kos_child_runtime rt;

    if (o == NULL) {
        return 1;
    }
    if (kos_child_setup(&rt, o) != 0) {
        return 1;
    }
    while (!kos_child_poll(&rt)) {
        if (kos_child_take_sighup()) {
            if (o->kind == KOS_CHILD_SCHEDULER) {
                kos_log_info(rt.log, "signal", "name=SIGHUP action=reload");
            } else {
                kos_log_info(rt.log, "signal", "name=SIGHUP action=ignored");
            }
        }
        if (kos_child_take_sigusr1()) {
            if (o->kind == KOS_CHILD_SCHEDULER) {
                kos_log_info(rt.log, "signal", "name=SIGUSR1 action=checkpoint");
            } else {
                kos_log_info(rt.log, "signal", "name=SIGUSR1 action=ignored");
            }
        }
        kos_child_sleep(&rt, CHILD_POLL_MS);
    }
    kos_child_teardown(&rt);
    return 0;
}
