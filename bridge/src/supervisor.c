/*
 * supervisor.c -- KitchenOS 守护进程。
 *
 * 它只做三件事，任何一件都不属于 Scheduler：
 *   1. 在完整 daemon 环境里保持单实例（pid 文件 fcntl 锁）；
 *   2. 用 fork/exec 拉起 Bridge、Scheduler、N 个 Worker，并用 SIGCHLD 收尸、按 §6.1
 *      的退避重启，窗口内崩溃到上限就放弃这个子进程（Supervisor 自己继续跑）；
 *   3. 用 SIGALRM 做健康检查（心跳管道 + kill(pid,0)），SIGTERM 时按
 *      Workers → Scheduler → Bridge 的顺序停下并清理 IPC。
 *
 * 所有系统调用的失败都走日志与返回码，不 abort；信号处理函数只写
 * volatile sig_atomic_t，真正的工作都在主循环里做。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* realpath() is POSIX but glibc only declares it under XSI; daemonize needs it
 * because chdir("/") happens after every path is resolved. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <kitchenos/child.h>
#include <kitchenos/daemon.h>
#include <kitchenos/kitchen.h>
#include <kitchenos/log.h>
#include <kitchenos/mq.h>
#include <kitchenos/policy.h>
#include <kitchenos/resource.h>
#include <kitchenos/shm.h>
#include <kitchenos/supervise.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SUPERVISOR_VERSION "0.1.0"
#define ERR_BUF 256
#define DEFAULT_WORKERS 2
#define DEFAULT_BRIDGE_PORT 14455
#define DEFAULT_BRIDGE_HOST "127.0.0.1"
#define DEFAULT_HEALTH_MS 2000
#define DEFAULT_HEARTBEAT_MS 500
#define DEFAULT_HEARTBEAT_TIMEOUT_MS 3000
#define DEFAULT_STOP_TIMEOUT_MS 3000
#define LOOP_TICK_MS 50
#define STATUS_BUF 16384

enum { EXIT_OK = 0, EXIT_STARTUP = 1, EXIT_USAGE = 2, EXIT_RUNNING = 3 };

typedef enum { SPEC_BRIDGE = 0, SPEC_SCHEDULER = 1, SPEC_WORKER = 2 } spec_kind;

typedef struct {
    kos_child rec;
    spec_kind kind;
    int chef_id;
    int heartbeat_fd; /* 读端；-1 = 没有 */
    int heartbeat_expected;
    int64_t last_heartbeat_ms;
    int unresponsive_killed;
} sup_child;

typedef struct {
    /* ---- 配置 ---- */
    int foreground;
    int workers;
    int bridge_port;
    const char *bridge_host;
    int no_bridge;
    int no_scheduler;
    int health_interval_ms;
    int heartbeat_interval_ms;
    int heartbeat_timeout_ms;
    int stop_timeout_ms;
    int verbose;
    const char *state_name;
    const char *sem_name;
    const char *kitchen_path;
    const char *recipes_path;
    const char *orders_path;
    const char *runs_dir;
    const char *strategy;
    int lease_ttl_ms;
    const char *bridge_bin_opt;
    const char *scheduler_bin_opt;
    const char *worker_bin_opt;
    kos_supervise_policy policy;

    /* ---- 启动时解析的绝对路径 ---- */
    char runtime_dir[KOS_DAEMON_PATH_MAX];
    char bin_dir[KOS_DAEMON_PATH_MAX];
    char bridge_bin[KOS_DAEMON_PATH_MAX];
    char scheduler_bin[KOS_DAEMON_PATH_MAX];
    char worker_bin[KOS_DAEMON_PATH_MAX];
    char kitchen_file[KOS_DAEMON_PATH_MAX];
    kos_kitchen kitchen;
    int kitchen_loaded;
    char supervisor_log_path[KOS_DAEMON_PATH_MAX];
    char status_path[KOS_DAEMON_PATH_MAX];
    char runs_dir_buf[KOS_DAEMON_PATH_MAX];

    /* ---- 运行时 ---- */
    kos_daemon daemon;
    kos_pidfile pidfile;
    kos_log *log;
    sup_child children[KOS_SUPERVISE_MAX_CHILDREN];
    int child_count;
    int shutting_down;
    int64_t started_ms;
} supervisor;

static supervisor g_sup; /* 一个进程一个 Supervisor */

static volatile sig_atomic_t g_stop_signal = 0;
static volatile sig_atomic_t g_sigchld = 0;
static volatile sig_atomic_t g_sigalrm = 0;
static volatile sig_atomic_t g_sighup = 0;
static volatile sig_atomic_t g_sigusr1 = 0;

/* ============================================================== 工具 */

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

static void error_text(const kos_error *err, char *buf, size_t len)
{
    kos_error_format(err, buf, len);
}

/* ============================================================ 信号 */

static void on_stop(int sig)
{
    g_stop_signal = sig;
}

static void on_sigchld(int sig)
{
    (void)sig;
    g_sigchld = 1;
}

static void on_sigalrm(int sig)
{
    (void)sig;
    g_sigalrm = 1;
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

static int install_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    /* 不设 SA_RESTART：主循环睡在 nanosleep 里，信号必须让它立刻醒来处理标志。 */
    sa.sa_flags = 0;
    if (sigaction(SIGTERM, &sa, NULL) != 0 || sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }

    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) != 0) {
        return -1;
    }

    sa.sa_handler = on_sigalrm;
    sa.sa_flags = 0;
    if (sigaction(SIGALRM, &sa, NULL) != 0) {
        return -1;
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
    sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
}

static int arm_health_timer(int interval_ms)
{
    struct itimerval timer;

    memset(&timer, 0, sizeof timer);
    timer.it_value.tv_sec = interval_ms / 1000;
    timer.it_value.tv_usec = (interval_ms % 1000) * 1000;
    timer.it_interval = timer.it_value;
    return setitimer(ITIMER_REAL, &timer, NULL);
}

/* ========================================================== 子进程表 */

static int find_child_by_pid(const supervisor *s, pid_t pid)
{
    int i;

    for (i = 0; i < s->child_count; i++) {
        if (s->children[i].rec.pid == pid) {
            return i;
        }
    }
    return -1;
}

static int find_child_role(const supervisor *s, const char *role)
{
    int i;

    for (i = 0; i < s->child_count; i++) {
        if (strcmp(s->children[i].rec.role, role) == 0) {
            return i;
        }
    }
    return -1;
}

static void close_heartbeat(sup_child *sc)
{
    if (sc->heartbeat_fd >= 0) {
        close(sc->heartbeat_fd);
        sc->heartbeat_fd = -1;
    }
}

/* ============================================================== 状态 */

static int appendf(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*off >= cap) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *off) {
        return -1;
    }
    *off += (size_t)n;
    return 0;
}

/*
 * status.json 是 Supervisor 的内部快照，不是跨端协议：字段固定、一行、键序固定，
 * 测试与监控脚本都按字符串找。原子写用临时文件 + rename，读的人不会看到半行。
 */
static void write_status(supervisor *s, const char *reason)
{
    char body[STATUS_BUF];
    char tmp[KOS_DAEMON_PATH_MAX];
    size_t off = 0;
    int fd;
    int i;
    ssize_t written;

    if (s->log == NULL) {
        return;
    }
    if (appendf(body, sizeof body, &off,
                "{\"pid\":%ld,\"role\":\"supervisor\",\"version\":\"%s\","
                "\"reason\":\"%s\",\"uptime_ms\":%lld,\"children\":[",
                (long)getpid(), SUPERVISOR_VERSION, reason,
                (long long)(now_ms() - s->started_ms)) != 0) {
        kos_log_warn(s->log, "status_write_failed", "reason=overflow");
        return;
    }
    for (i = 0; i < s->child_count; i++) {
        const sup_child *sc = &s->children[i];

        if (appendf(body, sizeof body, &off,
                    "%s{\"role\":\"%s\",\"pid\":%ld,\"state\":\"%s\",\"spawns\":%lld,"
                    "\"exits\":%lld,\"last_exit_ms\":%lld,\"last_code\":%d,"
                    "\"last_signal\":%d}",
                    i == 0 ? "" : ",", sc->rec.role, (long)sc->rec.pid,
                    kos_child_state_name(sc->rec.state), (long long)sc->rec.spawns,
                    (long long)sc->rec.exits, (long long)sc->rec.last_exit_ms,
                    sc->rec.last_exit_code, sc->rec.last_exit_signal) != 0) {
            kos_log_warn(s->log, "status_write_failed", "reason=overflow");
            return;
        }
    }
    if (appendf(body, sizeof body, &off, "]}\n") != 0) {
        kos_log_warn(s->log, "status_write_failed", "reason=overflow");
        return;
    }

    if (snprintf(tmp, sizeof tmp, "%s.tmp", s->status_path) >= (int)sizeof tmp) {
        kos_log_warn(s->log, "status_write_failed", "reason=path-too-long");
        return;
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        kos_log_warn(s->log, "status_write_failed", "path=%s error=%s", tmp,
                     strerror(errno));
        return;
    }
    written = write(fd, body, off);
    if (written < 0 || (size_t)written != off) {
        kos_log_warn(s->log, "status_write_failed", "path=%s error=%s", tmp,
                     strerror(errno));
        close(fd);
        (void)unlink(tmp);
        return;
    }
    close(fd);
    if (rename(tmp, s->status_path) != 0) {
        kos_log_warn(s->log, "status_write_failed", "rename=%s error=%s", s->status_path,
                     strerror(errno));
        (void)unlink(tmp);
    }
}

/* ============================================================ 启动子进程 */

static int child_log_path(const supervisor *s, const sup_child *sc, char *buf, size_t len,
                          kos_error *err)
{
    char name[64];

    if (sc->kind == SPEC_BRIDGE) {
        copy_str(name, sizeof name, "bridge.log");
    } else if (sc->kind == SPEC_SCHEDULER) {
        copy_str(name, sizeof name, "scheduler.log");
    } else {
        snprintf(name, sizeof name, "worker-%d.log", sc->chef_id);
    }
    return kos_runtime_log_path(s->runtime_dir, name, buf, len, err);
}

static void note_decision(supervisor *s, sup_child *sc, int index,
                          kos_supervise_decision decision, int64_t now, const char *why)
{
    (void)index;
    if (s->shutting_down) {
        sc->rec.state = KOS_CHILD_STOPPED;
        return;
    }
    if (decision.action == KOS_SUPERVISE_GIVE_UP) {
        kos_log_warn(s->log, "child_given_up",
                     "role=%s exits=%lld window_ms=%d limit=%d last_code=%d last_signal=%d "
                     "why=%s",
                     sc->rec.role, (long long)sc->rec.exits, s->policy.crash_window_ms,
                     s->policy.crash_limit, sc->rec.last_exit_code,
                     sc->rec.last_exit_signal, why);
    } else if (decision.action == KOS_SUPERVISE_RESTART) {
        kos_log_warn(s->log, "child_restart_scheduled",
                     "role=%s delay_ms=%d failures=%d at_ms=%lld why=%s", sc->rec.role,
                     decision.delay_ms, decision.failures_in_window, (long long)now, why);
    }
    write_status(s, "child_exit");
}

/* fork 之前的失败（日志路径、pipe、fork 本身）也要计入崩溃窗口，否则一个永远建不
 * 出来的子进程会安静地停在 STOPPED，Supervisor 看起来"正常"却什么都没在管。 */
static void record_spawn_failure(supervisor *s, sup_child *sc, int index, const char *stage,
                                 const char *detail)
{
    kos_supervise_decision d;
    int64_t now = now_ms();

    kos_log_error(s->log, "spawn_failed", "role=%s stage=%s error=%s", sc->rec.role, stage,
                  detail);
    d = kos_child_on_exit(&sc->rec, 127 << 8, now, &s->policy);
    if (d.action == KOS_SUPERVISE_RESTART && d.delay_ms == 0) {
        sc->rec.next_restart_ms = now + 100;
    }
    note_decision(s, sc, index, d, now, "spawn_failed");
}

static int spawn_child(supervisor *s, int index)
{
    sup_child *sc = &s->children[index];
    char *argv[48];
    int ac = 0;
    char port_buf[16];
    char workers_buf[16];
    char chef_buf[16];
    char hb_fd_buf[16];
    char hb_ms_buf[16];
    char generation_buf[32];
    char log_path[KOS_DAEMON_PATH_MAX];
    char bin[KOS_DAEMON_PATH_MAX];
    kos_error err;
    int pipe_fds[2] = {-1, -1};
    int hb_fd = -1;
    pid_t pid;

    if (index < 0 || index >= s->child_count) {
        return -1;
    }

    kos_error_clear(&err);
    if (child_log_path(s, sc, log_path, sizeof log_path, &err) != KOS_OK) {
        char text[ERR_BUF];

        error_text(&err, text, sizeof text);
        record_spawn_failure(s, sc, index, "log_path", text);
        return -1;
    }

    if (sc->kind == SPEC_BRIDGE) {
        copy_str(bin, sizeof bin, s->bridge_bin);
    } else if (sc->kind == SPEC_SCHEDULER) {
        copy_str(bin, sizeof bin, s->scheduler_bin);
    } else {
        copy_str(bin, sizeof bin, s->worker_bin);
    }

    if (sc->heartbeat_expected && s->heartbeat_interval_ms > 0) {
        if (pipe(pipe_fds) != 0) {
            record_spawn_failure(s, sc, index, "pipe", strerror(errno));
            return -1;
        }
        /* 读端给父进程；写端给子进程，不能在 exec 时被关掉。O_NONBLOCK 让父进程在
         * SIGALRM 里 drain 时不会卡住，也让子进程在管道满时得到 EAGAIN 而非挂死。 */
        (void)fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
        (void)fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);
        hb_fd = pipe_fds[1];
        snprintf(hb_fd_buf, sizeof hb_fd_buf, "%d", hb_fd);
        snprintf(hb_ms_buf, sizeof hb_ms_buf, "%d", s->heartbeat_interval_ms);
    }

    argv[ac++] = (char *)(sc->kind == SPEC_BRIDGE
                              ? "kitchenos-bridge"
                              : (sc->kind == SPEC_SCHEDULER ? "kitchenos-scheduler"
                                                            : "kitchenos-worker"));

    if (sc->kind == SPEC_BRIDGE) {
        snprintf(port_buf, sizeof port_buf, "%d", s->bridge_port);
        snprintf(workers_buf, sizeof workers_buf, "%d", s->workers);
        argv[ac++] = (char *)"--host";
        argv[ac++] = (char *)s->bridge_host;
        argv[ac++] = (char *)"--port";
        argv[ac++] = port_buf;
        argv[ac++] = (char *)"--chefs";
        argv[ac++] = workers_buf;
        argv[ac++] = (char *)"--state-name";
        argv[ac++] = (char *)s->state_name;
        argv[ac++] = (char *)"--sem-name";
        argv[ac++] = (char *)s->sem_name;
        argv[ac++] = (char *)"--log";
        argv[ac++] = log_path;
        if (hb_fd >= 0) {
            argv[ac++] = (char *)"--supervisor-fd";
            argv[ac++] = hb_fd_buf;
            argv[ac++] = (char *)"--supervisor-heartbeat-ms";
            argv[ac++] = hb_ms_buf;
        }
        if (s->verbose) {
            argv[ac++] = (char *)"--verbose";
        }
    } else {
        argv[ac++] = (char *)"--runtime-dir";
        argv[ac++] = s->runtime_dir;
        argv[ac++] = (char *)"--state-name";
        argv[ac++] = (char *)s->state_name;
        argv[ac++] = (char *)"--sem-name";
        argv[ac++] = (char *)s->sem_name;
        argv[ac++] = (char *)"--log";
        argv[ac++] = log_path;
        snprintf(workers_buf, sizeof workers_buf, "%d", s->workers);
        argv[ac++] = (char *)"--chefs";
        argv[ac++] = workers_buf;
        if (s->kitchen_path != NULL) {
            argv[ac++] = (char *)"--kitchen";
            argv[ac++] = (char *)s->kitchen_path;
        }
        if (sc->kind == SPEC_SCHEDULER) {
            if (s->recipes_path != NULL) {
                argv[ac++] = (char *)"--recipes";
                argv[ac++] = (char *)s->recipes_path;
            }
            if (s->orders_path != NULL) {
                argv[ac++] = (char *)"--orders";
                argv[ac++] = (char *)s->orders_path;
            }
            if (s->runs_dir != NULL) {
                argv[ac++] = (char *)"--runs-dir";
                argv[ac++] = (char *)s->runs_dir;
            }
            if (s->strategy != NULL) {
                argv[ac++] = (char *)"--strategy";
                argv[ac++] = (char *)s->strategy;
            }
            {
                char lease_buf[16];

                snprintf(lease_buf, sizeof lease_buf, "%d", s->lease_ttl_ms);
                argv[ac++] = (char *)"--lease-ttl-ms";
                argv[ac++] = lease_buf;
            }
        }
        if (sc->kind == SPEC_WORKER) {
            snprintf(chef_buf, sizeof chef_buf, "%d", sc->chef_id);
            snprintf(generation_buf, sizeof generation_buf, "%lld",
                     (long long)sc->rec.spawns + 1);
            argv[ac++] = (char *)"--chef-id";
            argv[ac++] = chef_buf;
            argv[ac++] = (char *)"--generation";
            argv[ac++] = generation_buf;
        }
        if (hb_fd >= 0) {
            argv[ac++] = (char *)"--supervisor-fd";
            argv[ac++] = hb_fd_buf;
            argv[ac++] = (char *)"--supervisor-heartbeat-ms";
            argv[ac++] = hb_ms_buf;
        }
        if (s->verbose) {
            argv[ac++] = (char *)"--verbose";
        }
    }
    argv[ac] = NULL;

    pid = fork();
    if (pid < 0) {
        int saved = errno;

        if (pipe_fds[0] >= 0) {
            close(pipe_fds[0]);
        }
        if (pipe_fds[1] >= 0) {
            close(pipe_fds[1]);
        }
        record_spawn_failure(s, sc, index, "fork", strerror(saved));
        return -1;
    }

    if (pid == 0) {
        char text[ERR_BUF];
        int len;

        if (hb_fd >= 0) {
            close(pipe_fds[0]);
        }
        execv(bin, argv);
        len = snprintf(text, sizeof text, "%s: execv('%s') failed: %s\n", argv[0], bin,
                       strerror(errno));
        if (len > 0) {
            (void)write(STDERR_FILENO, text, (size_t)len);
        }
        _exit(127);
    }

    if (hb_fd >= 0) {
        close(pipe_fds[1]);
        sc->heartbeat_fd = pipe_fds[0];
    } else {
        sc->heartbeat_fd = -1;
    }
    sc->last_heartbeat_ms = now_ms();
    sc->unresponsive_killed = 0;
    kos_child_started(&sc->rec, pid);

    kos_log_info(s->log, "child_spawn",
                 "role=%s pid=%ld spawns=%lld heartbeat_fd=%d bin=%s log=%s",
                 sc->rec.role, (long)pid, (long long)sc->rec.spawns, sc->heartbeat_fd, bin,
                 log_path);
    write_status(s, "child_spawn");
    return 0;
}

static void spawn_due(supervisor *s)
{
    int64_t now = now_ms();
    int i;

    if (s->shutting_down) {
        return;
    }
    for (i = 0; i < s->child_count; i++) {
        sup_child *sc = &s->children[i];

        if (sc->rec.state != KOS_CHILD_PENDING || now < sc->rec.next_restart_ms) {
            continue;
        }
        kos_log_warn(s->log, "child_restart", "role=%s attempt=%lld failures=%d",
                     sc->rec.role, (long long)sc->rec.exits + 1,
                     kos_child_failures_in_window(&sc->rec, now, s->policy.crash_window_ms));
        (void)spawn_child(s, i);
    }
}

static void reap_children(supervisor *s)
{
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        int index;
        sup_child *sc;
        kos_supervise_decision decision;
        int code;
        int sig;
        int64_t now;

        if (pid == 0) {
            break;
        }
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != ECHILD) {
                kos_log_warn(s->log, "waitpid_failed", "error=%s", strerror(errno));
            }
            break;
        }

        index = find_child_by_pid(s, pid);
        if (index < 0) {
            kos_log_warn(s->log, "reaped_unknown", "pid=%ld status=%d", (long)pid, status);
            continue;
        }
        sc = &s->children[index];
        close_heartbeat(sc);
        code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        kos_log_warn(s->log, "child_exit", "role=%s pid=%ld code=%d signal=%d",
                     sc->rec.role, (long)pid, code, sig);

        now = now_ms();
        decision = kos_child_on_exit(&sc->rec, status, now, &s->policy);
        if (s->shutting_down) {
            sc->rec.state = KOS_CHILD_STOPPED;
            continue;
        }
        note_decision(s, sc, index, decision, now, "waitpid");
    }
}

/* ============================================================ 健康检查 */

static void health_tick(supervisor *s)
{
    int64_t now = now_ms();
    int running = 0;
    int pending = 0;
    int given_up = 0;
    int64_t spawns = 0;
    int i;

    for (i = 0; i < s->child_count; i++) {
        sup_child *sc = &s->children[i];

        if (sc->rec.state == KOS_CHILD_PENDING) {
            pending++;
            continue;
        }
        if (sc->rec.state == KOS_CHILD_GIVEN_UP) {
            given_up++;
            continue;
        }
        if (!kos_child_alive(&sc->rec)) {
            continue;
        }
        running++;
        spawns += sc->rec.spawns;

        if (sc->heartbeat_expected && sc->heartbeat_fd >= 0) {
            char buf[256];
            int got = 0;
            ssize_t n;

            for (;;) {
                n = read(sc->heartbeat_fd, buf, sizeof buf);
                if (n > 0) {
                    got = 1;
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            if (got) {
                sc->last_heartbeat_ms = now;
            }
            if (s->heartbeat_timeout_ms > 0 && !sc->unresponsive_killed &&
                now - sc->last_heartbeat_ms > s->heartbeat_timeout_ms) {
                kos_log_warn(s->log, "child_unresponsive",
                             "role=%s pid=%ld silent_ms=%lld timeout_ms=%d",
                             sc->rec.role, (long)sc->rec.pid,
                             (long long)(now - sc->last_heartbeat_ms),
                             s->heartbeat_timeout_ms);
                if (kill(sc->rec.pid, SIGKILL) != 0 && errno != ESRCH) {
                    kos_log_warn(s->log, "child_kill_failed", "role=%s pid=%ld error=%s",
                                 sc->rec.role, (long)sc->rec.pid, strerror(errno));
                }
                sc->unresponsive_killed = 1;
            }
        } else if (kill(sc->rec.pid, 0) != 0 && errno == ESRCH) {
            /* SIGCHLD 应该已经排队；这里只记录，收尸仍在 reap_children 里做。 */
            kos_log_warn(s->log, "health_missing", "role=%s pid=%ld", sc->rec.role,
                         (long)sc->rec.pid);
        }
    }

    kos_log_debug(s->log, "health_tick",
                  "children=%d running=%d pending=%d given_up=%d spawns=%lld",
                  s->child_count, running, pending, given_up, (long long)spawns);
}

/* ============================================================ 信号转发 */

static void forward_sighup(supervisor *s)
{
    int index = find_child_role(s, "scheduler");

    if (index >= 0 && kos_child_alive(&s->children[index].rec)) {
        pid_t pid = s->children[index].rec.pid;

        if (kill(pid, SIGHUP) == 0) {
            kos_log_info(s->log, "sighup_forwarded", "role=scheduler pid=%ld", (long)pid);
            return;
        }
        kos_log_warn(s->log, "sighup_forward_failed", "pid=%ld error=%s", (long)pid,
                     strerror(errno));
        return;
    }
    kos_log_warn(s->log, "sighup_no_scheduler", "role=scheduler not-running");
}

static void forward_sigusr1(supervisor *s)
{
    int index = find_child_role(s, "scheduler");

    if (index >= 0 && kos_child_alive(&s->children[index].rec)) {
        pid_t pid = s->children[index].rec.pid;

        if (kill(pid, SIGUSR1) == 0) {
            kos_log_info(s->log, "sigusr1_forwarded", "role=scheduler pid=%ld",
                         (long)pid);
            return;
        }
        kos_log_warn(s->log, "sigusr1_forward_failed", "pid=%ld error=%s", (long)pid,
                     strerror(errno));
        return;
    }
    kos_log_warn(s->log, "sigusr1_no_scheduler", "role=scheduler not-running");
}

/* ============================================================== 关闭 */

static void unlink_resources(supervisor *s)
{
    kos_resource_set set;
    kos_error err;
    char text[ERR_BUF];
    int i;
    int rc;

    if (!s->kitchen_loaded) {
        return;
    }
    kos_error_clear(&err);
    if (kos_resources_attach(&s->kitchen, &set, &err) == KOS_OK) {
        for (i = 0; i < set.count; i++) {
            int value = -1;
            int rc_value = kos_resource_value(&set, set.items[i].kind, &value);

            kos_log_info(s->log, "resource_values",
                         "kind=%s value=%d capacity=%d result=%s", set.items[i].kind,
                         rc_value == KOS_OK ? value : -1, set.items[i].capacity,
                         rc_value == KOS_OK ? "ok" : "failed");
        }
        kos_resources_close(&set);
    } else {
        error_text(&err, text, sizeof text);
        kos_log_warn(s->log, "resource_values_failed", "error=%s", text);
    }
    kos_error_clear(&err);
    rc = kos_resources_unlink(&s->kitchen, &err);
    if (rc == KOS_OK) {
        kos_log_info(s->log, "resource_unlink", "prefix=/kitchenos_res_ result=ok");
    } else {
        error_text(&err, text, sizeof text);
        kos_log_warn(s->log, "resource_unlink",
                     "prefix=/kitchenos_res_ result=failed error=%s", text);
    }
}

static void unlink_ipc(supervisor *s)
{
    kos_shm_opts opts;
    kos_error err;
    int rc;

    memset(&opts, 0, sizeof opts);
    opts.object_name = s->state_name;
    opts.sem_name = s->sem_name;
    opts.perm = 0;

    kos_error_clear(&err);
    rc = kos_shm_unlink(&opts, &err);
    if (rc == KOS_OK) {
        kos_log_info(s->log, "ipc_unlink", "object=%s sem=%s result=ok", s->state_name,
                     s->sem_name);
    } else {
        char text[ERR_BUF];

        error_text(&err, text, sizeof text);
        kos_log_warn(s->log, "ipc_unlink", "object=%s sem=%s result=failed error=%s",
                     s->state_name, s->sem_name, text);
    }

    kos_error_clear(&err);
    rc = kos_mq_unlink_all(s->workers, &err);
    if (rc == KOS_OK) {
        kos_log_info(s->log, "mq_unlink", "workers=%d result=ok", s->workers);
    } else {
        char text[ERR_BUF];

        error_text(&err, text, sizeof text);
        kos_log_warn(s->log, "mq_unlink", "workers=%d result=failed error=%s", s->workers,
                     text);
    }
    unlink_resources(s);
}

static void stop_one(supervisor *s, sup_child *sc)
{
    int64_t deadline;
    int status = 0;
    pid_t pid;

    if (!kos_child_alive(&sc->rec)) {
        if (sc->rec.state == KOS_CHILD_PENDING || sc->rec.state == KOS_CHILD_RUNNING) {
            sc->rec.state = KOS_CHILD_STOPPED;
        }
        sc->rec.pid = 0;
        close_heartbeat(sc);
        return;
    }

    pid = sc->rec.pid;
    kos_log_info(s->log, "child_stop_sent", "role=%s pid=%ld signal=SIGTERM",
                 sc->rec.role, (long)pid);
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
        kos_log_warn(s->log, "child_stop_failed", "role=%s pid=%ld error=%s", sc->rec.role,
                     (long)pid, strerror(errno));
    }

    deadline = now_ms() + s->stop_timeout_ms;
    while (now_ms() < deadline) {
        pid_t rc = waitpid(pid, &status, WNOHANG);

        if (rc == pid) {
            kos_log_info(s->log, "child_stopped", "role=%s pid=%ld code=%d signal=%d",
                         sc->rec.role, (long)pid,
                         WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                         WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            sc->rec.state = KOS_CHILD_STOPPED;
            sc->rec.pid = 0;
            close_heartbeat(sc);
            write_status(s, "child_stopped");
            return;
        }
        if (rc < 0 && errno == ECHILD) {
            sc->rec.state = KOS_CHILD_STOPPED;
            sc->rec.pid = 0;
            close_heartbeat(sc);
            return;
        }
        sleep_ms(20);
    }

    kos_log_warn(s->log, "child_kill", "role=%s pid=%ld reason=stop-timeout", sc->rec.role,
                 (long)pid);
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        kos_log_warn(s->log, "child_kill_failed", "role=%s pid=%ld error=%s", sc->rec.role,
                     (long)pid, strerror(errno));
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* 等内核收尸；SIGKILL 之后不会太久。 */
    }
    kos_log_warn(s->log, "child_killed", "role=%s pid=%ld signal=%d", sc->rec.role,
                 (long)pid, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    sc->rec.state = KOS_CHILD_STOPPED;
    sc->rec.pid = 0;
    close_heartbeat(sc);
    write_status(s, "child_killed");
}

static int shutdown_all(supervisor *s)
{
    int i;

    s->shutting_down = 1;
    kos_log_info(s->log, "shutdown_begin", "signal=%s children=%d",
                 g_stop_signal != 0 ? signal_name((int)g_stop_signal) : "internal",
                 s->child_count);

    /* Workers 先停（它们在生产输入槽），再 Scheduler（决策来源），最后 Bridge
     * （唯一与游戏通信的进程）。倒序也是"最后拉起的先停"，与依赖方向一致。 */
    for (i = s->child_count - 1; i >= 0; i--) {
        if (s->children[i].kind == SPEC_WORKER) {
            stop_one(s, &s->children[i]);
        }
    }
    i = find_child_role(s, "scheduler");
    if (i >= 0) {
        stop_one(s, &s->children[i]);
    }
    i = find_child_role(s, "bridge");
    if (i >= 0) {
        stop_one(s, &s->children[i]);
    }
    for (i = 0; i < s->child_count; i++) {
        stop_one(s, &s->children[i]);
    }

    unlink_ipc(s);
    write_status(s, "stopped");

    {
        int64_t spawns = 0;
        int64_t exits = 0;

        for (i = 0; i < s->child_count; i++) {
            spawns += s->children[i].rec.spawns;
            exits += s->children[i].rec.exits;
        }
        kos_log_info(s->log, "supervisor_stop", "reason=%s uptime_ms=%lld spawns=%lld "
                     "exits=%lld",
                     g_stop_signal != 0 ? signal_name((int)g_stop_signal) : "internal",
                     (long long)(now_ms() - s->started_ms), (long long)spawns,
                     (long long)exits);
    }

    kos_pidfile_release(&s->pidfile, 1);
    kos_log_info(s->log, "pidfile_removed", "path=%s", s->pidfile.path);
    kos_log_close(s->log);
    s->log = NULL;
    kos_daemon_ready(&s->daemon, EXIT_OK); /* foreground 下是空操作 */
    return EXIT_OK;
}

/* ============================================================ 启动解析 */

static const char *self_dir(char *out, size_t len)
{
    char exe[KOS_DAEMON_PATH_MAX];
    char dir[KOS_DAEMON_PATH_MAX];
    char *slash;
    ssize_t n;

    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) {
        return NULL;
    }
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL) {
        return NULL;
    }
    *slash = '\0';
    if (realpath(exe, dir) == NULL) {
        return NULL;
    }
    copy_str(out, len, dir);
    return out;
}

static int resolve_bin(const char *dir, const char *override, const char *name, char *out,
                       size_t len, kos_error *err)
{
    char candidate[KOS_DAEMON_PATH_MAX];
    char resolved[KOS_DAEMON_PATH_MAX];

    if (override != NULL) {
        copy_str(candidate, sizeof candidate, override);
    } else if (snprintf(candidate, sizeof candidate, "%s/%s", dir, name) >=
               (int)sizeof candidate) {
        err->code = KOS_ERR_INVAL;
        snprintf(err->context, sizeof err->context, "二进制路径过长: %.120s", name);
        return KOS_ERR_INVAL;
    }
    if (realpath(candidate, resolved) == NULL) {
        err->code = KOS_ERR_MISSING;
        err->sys_errno = errno;
        snprintf(err->context, sizeof err->context, "realpath('%.120s')", candidate);
        return KOS_ERR_MISSING;
    }
    if (access(resolved, X_OK) != 0) {
        err->code = KOS_ERR_IO;
        err->sys_errno = errno;
        snprintf(err->context, sizeof err->context, "access('%.120s', X_OK)", resolved);
        return KOS_ERR_IO;
    }
    if (strlen(resolved) + 1 > len) {
        err->code = KOS_ERR_INVAL;
        snprintf(err->context, sizeof err->context, "二进制路径缓冲区太小: %.120s", resolved);
        return KOS_ERR_INVAL;
    }
    copy_str(out, len, resolved);
    return KOS_OK;
}

static void print_usage(FILE *out)
{
    fprintf(out,
            "usage: kitchenos-supervisor [--foreground] [--runtime-dir DIR]\n"
            "                            [--workers N] [--bridge-host H] [--bridge-port P]\n"
            "                            [--no-bridge] [--no-scheduler]\n"
            "                            [--bin-dir DIR] [--bridge-bin FILE]\n"
            "                            [--scheduler-bin FILE] [--worker-bin FILE]\n"
            "                            [--state-name NAME] [--sem-name NAME]\n"
            "                            [--health-interval-ms M]\n"
            "                            [--supervisor-heartbeat-ms M]\n"
            "                            [--heartbeat-timeout-ms M]\n"
            "                            [--stop-timeout-ms M] [--crash-limit N]\n"
            "                            [--crash-window-ms M] [--backoff-ms LIST]\n"
            "                            [--verbose] [--help] [--version]\n");
}

static void print_help(void)
{
    print_usage(stdout);
    printf("\n"
           "KitchenOS Supervisor: the daemon that owns the process tree and nothing else.\n"
           "\n"
           "Options:\n"
           "  --foreground             stay in the foreground (tests, debugging)\n"
           "  --runtime-dir DIR        pid/logs/status directory (default: $XDG_RUNTIME_DIR/\n"
           "                           kitchenos, else /tmp/kitchenos-<uid>)\n"
           "  --workers N              number of Chef Worker processes (default %d, max %d)\n"
           "  --kitchen FILE           kitchen.json passed to Scheduler and Workers\n"
           "  --recipes FILE           recipes.json passed to Scheduler\n"
           "  --orders FILE            orders.json menu passed to Scheduler\n"
           "  --runs-dir DIR           Scheduler run logs (default <runtime>/runs)\n"
           "  --strategy fcfs|edf|llf|hrrn  initial order policy (default fcfs; SIGHUP cycles)\n"
           "  --lease-ttl-ms M         resource lease lifetime (default 30000)\n"
           "  --bridge-host H          Bridge bind address (default %s)\n"
           "  --bridge-port P          Bridge TCP port (default %d)\n"
           "  --no-bridge              do not start the Bridge\n"
           "  --no-scheduler           do not start the Scheduler\n"
           "  --bin-dir DIR            where the child binaries live (default: this binary's dir)\n"
           "  --bridge-bin FILE        explicit Bridge binary\n"
           "  --scheduler-bin FILE     explicit Scheduler binary\n"
           "  --worker-bin FILE        explicit Worker binary\n"
           "  --health-interval-ms M   SIGALRM health tick period (default %d)\n"
           "  --supervisor-heartbeat-ms M  child heartbeat interval (default %d)\n"
           "  --heartbeat-timeout-ms M declare a child unresponsive after M ms without a\n"
           "                           heartbeat byte; 0 disables (default %d)\n"
           "  --stop-timeout-ms M      SIGTERM grace per child before SIGKILL (default %d)\n"
           "  --crash-limit N          exits within the window before giving up (default %d)\n"
           "  --crash-window-ms M      sliding crash window (default %d)\n"
           "  --backoff-ms LIST        comma-separated restart delays (default 0,2000,10000)\n"
           "  --verbose                log DEBUG events (health ticks)\n"
           "  --help, --version        print and exit 0\n"
           "\n"
           "Exit codes: 0 clean shutdown, 1 startup failure, 2 argument error,\n"
           "3 another instance holds the pid file lock.\n",
           DEFAULT_WORKERS, KOS_SHM_MAX_CHEFS, DEFAULT_BRIDGE_HOST, DEFAULT_BRIDGE_PORT,
           DEFAULT_HEALTH_MS, DEFAULT_HEARTBEAT_MS, DEFAULT_HEARTBEAT_TIMEOUT_MS,
           DEFAULT_STOP_TIMEOUT_MS, 5, 60000);
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

enum { ARG_OK = 0, ARG_ERR = 1, ARG_HELP = 2, ARG_VERSION = 3 };

static int parse_args(int argc, char **argv, supervisor *s)
{
    int i;

    memset(s, 0, sizeof *s);
    s->foreground = 0;
    s->workers = DEFAULT_WORKERS;
    s->bridge_port = DEFAULT_BRIDGE_PORT;
    s->bridge_host = DEFAULT_BRIDGE_HOST;
    s->health_interval_ms = DEFAULT_HEALTH_MS;
    s->heartbeat_interval_ms = DEFAULT_HEARTBEAT_MS;
    s->heartbeat_timeout_ms = DEFAULT_HEARTBEAT_TIMEOUT_MS;
    s->stop_timeout_ms = DEFAULT_STOP_TIMEOUT_MS;
    s->state_name = KOS_SHM_DEFAULT_NAME;
    s->sem_name = KOS_SHM_DEFAULT_SEM_NAME;
    s->strategy = "fcfs";
    s->lease_ttl_ms = 30000;
    s->daemon.ready_fd = -1;
    s->pidfile.fd = -1;
    kos_supervise_policy_init(&s->policy);

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
                return ARG_ERR;
            }
        }

        if (strcmp(name, "--help") == 0) {
            return ARG_HELP;
        } else if (strcmp(name, "--version") == 0) {
            return ARG_VERSION;
        } else if (strcmp(name, "--foreground") == 0) {
            if (value != NULL) {
                return ARG_ERR;
            }
            s->foreground = 1;
        } else if (strcmp(name, "--no-bridge") == 0) {
            if (value != NULL) {
                return ARG_ERR;
            }
            s->no_bridge = 1;
        } else if (strcmp(name, "--no-scheduler") == 0) {
            if (value != NULL) {
                return ARG_ERR;
            }
            s->no_scheduler = 1;
        } else if (strcmp(name, "--verbose") == 0) {
            if (value != NULL) {
                return ARG_ERR;
            }
            s->verbose = 1;
        } else if (strcmp(name, "--runtime-dir") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            copy_str(s->runtime_dir, sizeof s->runtime_dir, v);
        } else if (strcmp(name, "--bin-dir") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            copy_str(s->bin_dir, sizeof s->bin_dir, v);
        } else if (strcmp(name, "--bridge-bin") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            s->bridge_bin_opt = v;
        } else if (strcmp(name, "--scheduler-bin") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            s->scheduler_bin_opt = v;
        } else if (strcmp(name, "--worker-bin") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            s->worker_bin_opt = v;
        } else if (strcmp(name, "--bridge-host") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            s->bridge_host = v;
        } else if (strcmp(name, "--state-name") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            s->state_name = v;
        } else if (strcmp(name, "--sem-name") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            s->sem_name = v;
        } else if (strcmp(name, "--kitchen") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            s->kitchen_path = v;
        } else if (strcmp(name, "--recipes") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            s->recipes_path = v;
        } else if (strcmp(name, "--orders") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            s->orders_path = v;
        } else if (strcmp(name, "--runs-dir") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (v == NULL) {
                return ARG_ERR;
            }
            s->runs_dir = v;
        } else if (strcmp(name, "--lease-ttl-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 100, 3600000, &number) != 0) {
                return ARG_ERR;
            }
            s->lease_ttl_ms = (int)number;
        } else if (strcmp(name, "--strategy") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            kos_policy_kind kind;

            if (v == NULL || kos_policy_kind_from_name(v, &kind) != KOS_OK) {
                return ARG_ERR;
            }
            s->strategy = v;
        } else if (strcmp(name, "--workers") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 0, KOS_SHM_MAX_CHEFS, &number) != 0) {
                return ARG_ERR;
            }
            s->workers = (int)number;
        } else if (strcmp(name, "--bridge-port") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 1, 65535, &number) != 0) {
                return ARG_ERR;
            }
            s->bridge_port = (int)number;
        } else if (strcmp(name, "--health-interval-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 10, 3600000, &number) != 0) {
                return ARG_ERR;
            }
            s->health_interval_ms = (int)number;
        } else if (strcmp(name, "--supervisor-heartbeat-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 0, 3600000, &number) != 0) {
                return ARG_ERR;
            }
            s->heartbeat_interval_ms = (int)number;
        } else if (strcmp(name, "--heartbeat-timeout-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 0, 3600000, &number) != 0) {
                return ARG_ERR;
            }
            s->heartbeat_timeout_ms = (int)number;
        } else if (strcmp(name, "--stop-timeout-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 100, 60000, &number) != 0) {
                return ARG_ERR;
            }
            s->stop_timeout_ms = (int)number;
        } else if (strcmp(name, "--crash-limit") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 1, KOS_SUPERVISE_HISTORY, &number) != 0) {
                return ARG_ERR;
            }
            s->policy.crash_limit = (int)number;
        } else if (strcmp(name, "--crash-window-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);

            if (parse_long(v, 100, 3600000, &number) != 0) {
                return ARG_ERR;
            }
            s->policy.crash_window_ms = (int)number;
        } else if (strcmp(name, "--backoff-ms") == 0) {
            const char *v = take_value(value, argc, argv, &i);
            kos_error err;

            if (v == NULL ||
                kos_supervise_policy_parse_backoff(v, &s->policy, &err) != KOS_OK) {
                return ARG_ERR;
            }
        } else {
            return ARG_ERR;
        }
    }
    return ARG_OK;
}

static int build_children(supervisor *s)
{
    int i;

    s->child_count = 0;
    if (!s->no_bridge) {
        sup_child *sc = &s->children[s->child_count];

        kos_child_init(&sc->rec, "bridge", 0);
        sc->kind = SPEC_BRIDGE;
        sc->chef_id = -1;
        sc->heartbeat_fd = -1;
        sc->heartbeat_expected = 1;
        s->child_count++;
    }
    if (!s->no_scheduler) {
        sup_child *sc = &s->children[s->child_count];

        kos_child_init(&sc->rec, "scheduler", 0);
        sc->kind = SPEC_SCHEDULER;
        sc->chef_id = -1;
        sc->heartbeat_fd = -1;
        sc->heartbeat_expected = 1;
        s->child_count++;
    }
    for (i = 0; i < s->workers; i++) {
        sup_child *sc = &s->children[s->child_count];
        char role[KOS_SUPERVISE_ROLE_MAX];

        snprintf(role, sizeof role, "worker-%d", i);
        kos_child_init(&sc->rec, role, 0);
        sc->kind = SPEC_WORKER;
        sc->chef_id = i;
        sc->heartbeat_fd = -1;
        sc->heartbeat_expected = 1;
        s->child_count++;
    }
    return 0;
}

static int supervisor_main(supervisor *s)
{
    kos_error err;
    char dir[KOS_DAEMON_PATH_MAX];
    char text[ERR_BUF];
    int i;

    /* daemon 之后 chdir("/")，所有路径必须现在就是绝对的。 */
    kos_error_clear(&err);
    if (kos_runtime_dir(s->runtime_dir[0] != '\0' ? s->runtime_dir : NULL, s->runtime_dir,
                        sizeof s->runtime_dir, &err) != KOS_OK) {
        error_text(&err, text, sizeof text);
        fprintf(stderr, "kitchenos-supervisor: 运行目录不可用: %s\n", text);
        return EXIT_STARTUP;
    }
    if (kos_runtime_log_path(s->runtime_dir, "supervisor.log", s->supervisor_log_path,
                             sizeof s->supervisor_log_path, &err) != KOS_OK ||
        snprintf(s->status_path, sizeof s->status_path, "%s/status.json",
                 s->runtime_dir) >= (int)sizeof s->status_path) {
        fprintf(stderr, "kitchenos-supervisor: 状态/日志路径不可用\n");
        return EXIT_STARTUP;
    }

    if (s->bin_dir[0] != '\0') {
        char resolved[KOS_DAEMON_PATH_MAX];

        if (realpath(s->bin_dir, resolved) == NULL) {
            fprintf(stderr, "kitchenos-supervisor: bin-dir 不存在: %s\n", s->bin_dir);
            return EXIT_STARTUP;
        }
        copy_str(s->bin_dir, sizeof s->bin_dir, resolved);
    } else if (self_dir(dir, sizeof dir) == NULL) {
        fprintf(stderr, "kitchenos-supervisor: 无法确定自身目录（/proc/self/exe）\n");
        return EXIT_STARTUP;
    } else {
        copy_str(s->bin_dir, sizeof s->bin_dir, dir);
    }

    if (!s->no_bridge &&
        resolve_bin(s->bin_dir, s->bridge_bin_opt, "kitchenos-bridge", s->bridge_bin,
                    sizeof s->bridge_bin, &err) != KOS_OK) {
        error_text(&err, text, sizeof text);
        fprintf(stderr, "kitchenos-supervisor: Bridge 不可用: %s\n", text);
        return EXIT_STARTUP;
    }
    if (!s->no_scheduler &&
        resolve_bin(s->bin_dir, s->scheduler_bin_opt, "kitchenos-scheduler",
                    s->scheduler_bin, sizeof s->scheduler_bin, &err) != KOS_OK) {
        error_text(&err, text, sizeof text);
        fprintf(stderr, "kitchenos-supervisor: Scheduler 不可用: %s\n", text);
        return EXIT_STARTUP;
    }
    if (s->workers > 0 &&
        resolve_bin(s->bin_dir, s->worker_bin_opt, "kitchenos-worker", s->worker_bin,
                    sizeof s->worker_bin, &err) != KOS_OK) {
        error_text(&err, text, sizeof text);
        fprintf(stderr, "kitchenos-supervisor: Worker 不可用: %s\n", text);
        return EXIT_STARTUP;
    }

    if (s->kitchen_path != NULL) {
        char resolved[KOS_DAEMON_PATH_MAX];

        if (realpath(s->kitchen_path, resolved) == NULL) {
            fprintf(stderr, "kitchenos-supervisor: kitchen 文件不存在: %s\n",
                    s->kitchen_path);
            return EXIT_STARTUP;
        }
        copy_str(s->kitchen_file, sizeof s->kitchen_file, resolved);
        s->kitchen_path = s->kitchen_file;
    } else if (kos_child_default_config_path("kitchen.example.json", s->kitchen_file,
                                              sizeof s->kitchen_file) != 0) {
        fprintf(stderr, "kitchenos-supervisor: 找不到 kitchen.example.json\n");
        return EXIT_STARTUP;
    } else {
        s->kitchen_path = s->kitchen_file;
    }
    if (s->runs_dir != NULL) {
        if (s->runs_dir[0] == '/') {
            copy_str(s->runs_dir_buf, sizeof s->runs_dir_buf, s->runs_dir);
        } else {
            char cwd[KOS_DAEMON_PATH_MAX];

            if (getcwd(cwd, sizeof cwd) == NULL) {
                fprintf(stderr, "kitchenos-supervisor: 无法获取当前工作目录\n");
                return EXIT_STARTUP;
            }
            if (snprintf(s->runs_dir_buf, sizeof s->runs_dir_buf, "%s/%s", cwd, s->runs_dir) >=
                (int)sizeof s->runs_dir_buf) {
                fprintf(stderr, "kitchenos-supervisor: runs-dir 路径过长\n");
                return EXIT_STARTUP;
            }
        }
        s->runs_dir = s->runs_dir_buf;
    }

    if (!s->foreground) {
        kos_error_clear(&err);
        if (kos_daemonize(&s->daemon, &err) != KOS_OK) {
            error_text(&err, text, sizeof text);
            fprintf(stderr, "kitchenos-supervisor: daemonize 失败: %s\n", text);
            return EXIT_STARTUP;
        }
    }

    kos_error_clear(&err);
    {
        char pid_path[KOS_DAEMON_PATH_MAX + 32];
        int rc;

        snprintf(pid_path, sizeof pid_path, "%s/%s", s->runtime_dir, KOS_PIDFILE_NAME);
        rc = kos_pidfile_acquire(pid_path, &s->pidfile, &err);
        if (rc == KOS_ERR_EXISTS) {
            fprintf(stderr, "kitchenos-supervisor: 已在运行（%s 被锁定）\n", pid_path);
            kos_daemon_ready(&s->daemon, EXIT_RUNNING);
            return EXIT_RUNNING;
        }
        if (rc != KOS_OK) {
            error_text(&err, text, sizeof text);
            fprintf(stderr, "kitchenos-supervisor: pid 文件不可用: %s\n", text);
            kos_daemon_ready(&s->daemon, EXIT_STARTUP);
            return EXIT_STARTUP;
        }
    }

    kos_error_clear(&err);
    if (kos_log_open(s->supervisor_log_path, s->verbose ? KOS_LOG_DEBUG : KOS_LOG_INFO,
                     &s->log, &err) != KOS_OK) {
        error_text(&err, text, sizeof text);
        fprintf(stderr, "kitchenos-supervisor: 打不开日志 %s: %s\n", s->supervisor_log_path,
                text);
        kos_pidfile_release(&s->pidfile, 1);
        kos_daemon_ready(&s->daemon, EXIT_STARTUP);
        return EXIT_STARTUP;
    }

    kos_daemon_ready(&s->daemon, EXIT_OK);

    kos_error_clear(&err);
    if (kos_kitchen_load(s->kitchen_path, &s->kitchen, &err) != KOS_OK) {
        error_text(&err, text, sizeof text);
        kos_log_error(s->log, "kitchen_load_failed", "path=%s error=%s",
                      s->kitchen_path, text);
        kos_pidfile_release(&s->pidfile, 1);
        kos_log_close(s->log);
        return EXIT_STARTUP;
    }
    s->kitchen_loaded = 1;
    kos_error_clear(&err);
    if (kos_resources_create(&s->kitchen, &err) != KOS_OK) {
        error_text(&err, text, sizeof text);
        kos_log_error(s->log, "resource_create_failed", "error=%s", text);
        kos_pidfile_release(&s->pidfile, 1);
        kos_log_close(s->log);
        return EXIT_STARTUP;
    }
    for (i = 0; i < s->kitchen.station_count; i++) {
        int j;
        int seen = 0;

        for (j = 0; j < i; j++) {
            if (strcmp(s->kitchen.stations[j].kind, s->kitchen.stations[i].kind) == 0) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            int capacity = 0;

            for (j = 0; j < s->kitchen.station_count; j++) {
                if (strcmp(s->kitchen.stations[j].kind, s->kitchen.stations[i].kind) ==
                    0) {
                    capacity++;
                }
            }
            kos_log_info(s->log, "resource_created", "kind=%s capacity=%d",
                         s->kitchen.stations[i].kind, capacity);
        }
    }

    s->started_ms = now_ms();
    kos_log_info(s->log, "supervisor_start",
                 "pid=%ld mode=%s runtime=%s log=%s status=%s workers=%d bridge=%s "
                 "scheduler=%s health_ms=%d heartbeat_ms=%d heartbeat_timeout_ms=%d "
                 "stop_timeout_ms=%d crash_limit=%d crash_window_ms=%d version=%s",
                 (long)getpid(), s->foreground ? "foreground" : "daemon",
                 s->runtime_dir, s->supervisor_log_path, s->status_path, s->workers,
                 s->no_bridge ? "off" : s->bridge_bin,
                 s->no_scheduler ? "off" : s->scheduler_bin, s->health_interval_ms,
                 s->heartbeat_interval_ms, s->heartbeat_timeout_ms, s->stop_timeout_ms,
                 s->policy.crash_limit, s->policy.crash_window_ms, SUPERVISOR_VERSION);

    if (install_handlers() != 0) {
        kos_log_error(s->log, "signal_install_failed", "error=%s", strerror(errno));
        kos_pidfile_release(&s->pidfile, 1);
        kos_log_close(s->log);
        return EXIT_STARTUP;
    }
    if (arm_health_timer(s->health_interval_ms) != 0) {
        kos_log_error(s->log, "setitimer_failed", "error=%s", strerror(errno));
        kos_pidfile_release(&s->pidfile, 1);
        kos_log_close(s->log);
        return EXIT_STARTUP;
    }

    build_children(s);
    for (i = 0; i < s->child_count; i++) {
        (void)spawn_child(s, i);
    }
    write_status(s, "started");

    while (g_stop_signal == 0) {
        if (g_sigchld) {
            g_sigchld = 0;
            reap_children(s);
        }
        if (g_sigalrm) {
            g_sigalrm = 0;
            health_tick(s);
        }
        if (g_sighup) {
            g_sighup = 0;
            forward_sighup(s);
        }
        if (g_sigusr1) {
            g_sigusr1 = 0;
            kos_log_info(s->log, "status_dump", "reason=signal:SIGUSR1");
            write_status(s, "signal:SIGUSR1");
            forward_sigusr1(s);
        }
        spawn_due(s);
        sleep_ms(LOOP_TICK_MS);
    }

    return shutdown_all(s);
}

int main(int argc, char **argv)
{
    int parsed = parse_args(argc, argv, &g_sup);

    if (parsed == ARG_HELP) {
        print_help();
        return EXIT_OK;
    }
    if (parsed == ARG_VERSION) {
        printf("kitchenos-supervisor %s\n", SUPERVISOR_VERSION);
        return EXIT_OK;
    }
    if (parsed != ARG_OK) {
        print_usage(stderr);
        return EXIT_USAGE;
    }
    return supervisor_main(&g_sup);
}
