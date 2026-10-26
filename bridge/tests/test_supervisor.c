/*
 * test_supervisor.c -- Supervisor 的单元与集成测试。
 *
 * 两类证据分开：
 *
 *   1. 纯函数（崩溃窗口、退避、pid 文件锁）用假时间戳和 fork 出的子进程验证。
 *      fcntl 记录锁属于进程而不是 fd，所以"同一进程开两个 fd"证明不了单实例；
 *      第二个持锁者必须真的在另一个进程里。
 *   2. 进程控制用真实的 `kitchenos-supervisor --foreground` 跑：kill -9 一个 Worker
 *      看它是否被拉起、崩溃循环是否按上限放弃、SIGTERM 后子进程是否全消失且 IPC
 *      对象被删。每条断言都对应 spec 里一个 W 编号。
 *
 * 每个用例用独立的 mkdtemp 运行目录与带 pid 的 shm/sem 名；测试进程自己清理，
 * 失败时也不会留下正在跑的 Supervisor。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "kitchenos/child.h"
#include "kitchenos/daemon.h"
#include "kitchenos/kitchen.h"
#include "kitchenos/mq.h"
#include "kitchenos/resource.h"
#include "kitchenos/shm.h"
#include "kitchenos/supervise.h"

#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STARTUP_TIMEOUT_MS 5000
#define RESTART_TIMEOUT_MS 4000
#define EXIT_TIMEOUT_MS 5000

static int g_checks;
static int g_failures;
static char g_sup_bin[KOS_DAEMON_PATH_MAX + 32];
static char g_silent_bin[KOS_DAEMON_PATH_MAX + 32];

static void expect(int cond, const char *what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        g_failures++;
        printf("  FAIL %s\n", what);
    }
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int read_all(const char *path, char *buf, size_t cap)
{
    int fd;
    size_t used = 0;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    while (used + 1 < cap) {
        ssize_t n = read(fd, buf + used, cap - 1 - used);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }
    buf[used] = '\0';
    close(fd);
    return 0;
}

static int log_contains(const char *path, const char *needle)
{
    char body[32768];

    if (read_all(path, body, sizeof body) != 0) {
        return 0;
    }
    return strstr(body, needle) != NULL;
}

static int wait_log(const char *path, const char *needle, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;

    do {
        if (log_contains(path, needle)) {
            return 1;
        }
        sleep_ms(20);
    } while (now_ms() < deadline);
    return log_contains(path, needle);
}

static int wait_file_exists(const char *path, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if (access(path, F_OK) == 0) {
            return 1;
        }
        sleep_ms(10);
    }
    return access(path, F_OK) == 0;
}
static int json_long(const char *start, const char *key, long *out)
{
    char needle[48];
    const char *p;

    snprintf(needle, sizeof needle, "\"%s\":", key);
    p = strstr(start, needle);
    if (p == NULL) {
        return -1;
    }
    return sscanf(p + strlen(needle), "%ld", out) == 1 ? 0 : -1;
}

static int json_string(const char *start, const char *key, char *out, size_t cap)
{
    char needle[48];
    const char *p;

    if (cap < 2) {
        return -1;
    }
    snprintf(needle, sizeof needle, "\"%s\":\"", key);
    p = strstr(start, needle);
    if (p == NULL) {
        return -1;
    }
    return sscanf(p + strlen(needle), "%63[^\"]", out) == 1 ? 0 : -1;
}

typedef struct {
    long pid;
    long spawns;
    long exits;
    char state[64];
} child_status;

/* 0 = 找到了，-1 = 文件/角色暂时不在。status.json 是一行、键序固定，测试按子串读。 */
static int lookup_child(const char *runtime, const char *role, child_status *out)
{
    char path[KOS_DAEMON_PATH_MAX + 32];
    char body[16384];
    char needle[96];
    const char *p;

    snprintf(path, sizeof path, "%s/status.json", runtime);
    if (read_all(path, body, sizeof body) != 0) {
        return -1;
    }
    snprintf(needle, sizeof needle, "\"role\":\"%s\"", role);
    p = strstr(body, needle);
    if (p == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (json_long(p, "pid", &out->pid) != 0 ||
        json_long(p, "spawns", &out->spawns) != 0 ||
        json_long(p, "exits", &out->exits) != 0 ||
        json_string(p, "state", out->state, sizeof out->state) != 0) {
        return -1;
    }
    return 0;
}

static int wait_child_running(const char *runtime, const char *role, long min_spawns,
                              int timeout_ms, long *pid_out)
{
    int64_t deadline = now_ms() + timeout_ms;
    child_status st;

    do {
        if (lookup_child(runtime, role, &st) == 0 && st.pid > 0 &&
            st.spawns >= min_spawns && strcmp(st.state, "running") == 0) {
            if (pid_out != NULL) {
                *pid_out = st.pid;
            }
            return 1;
        }
        sleep_ms(20);
    } while (now_ms() < deadline);
    return 0;
}

static int wait_status_state(const char *runtime, const char *role, const char *state,
                             long min_spawns, int timeout_ms,
                             child_status *out)
{
    int64_t deadline = now_ms() + timeout_ms;
    child_status st;

    do {
        if (lookup_child(runtime, role, &st) == 0 && st.spawns >= min_spawns &&
            strcmp(st.state, state) == 0) {
            if (out != NULL) {
                *out = st;
            }
            return 1;
        }
        sleep_ms(20);
    } while (now_ms() < deadline);
    return 0;
}

static int wait_exit(pid_t pid, int timeout_ms, int *status_out)
{
    int64_t deadline = now_ms() + timeout_ms;

    for (;;) {
        int status = 0;
        pid_t rc = waitpid(pid, &status, WNOHANG);

        if (rc == pid) {
            if (status_out != NULL) {
                *status_out = status;
            }
            return 1;
        }
        if (rc < 0 && errno == ECHILD) {
            return 0;
        }
        if (now_ms() >= deadline) {
            return 0;
        }
        sleep_ms(20);
    }
}

static int pid_gone(pid_t pid, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;

    do {
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            return 1;
        }
        sleep_ms(20);
    } while (now_ms() < deadline);
    return kill(pid, 0) != 0 && errno == ESRCH;
}

static int free_port(void)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    int fd;
    int port = 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0 &&
        getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
        port = ntohs(addr.sin_port);
    }
    close(fd);
    return port;
}

static void object_paths(const char *state, const char *sem, char *state_path,
                         size_t state_cap, char *sem_path, size_t sem_cap)
{
    snprintf(state_path, state_cap, "/dev/shm/%s", state + 1);
    snprintf(sem_path, sem_cap, "/dev/shm/sem.%s", sem + 1);
}

typedef struct {
    char runtime[KOS_DAEMON_PATH_MAX];
    char state[64];
    char sem[64];
    int port;
    pid_t pid;
} sup_run;

static void make_runtime(char *out, size_t cap)
{
    if (out == NULL || cap < 32) {
        return;
    }
    snprintf(out, cap, "/tmp/kos-sup-test-XXXXXX");
    if (mkdtemp(out) == NULL) {
        out[0] = '\0';
    }
}

/*
 * 统一的启动器：参数覆盖每个用例需要的差异，公共项固定成短超时，保证一次
 * `make test` 里这些集成用例总耗时可控。stdout/stderr 落到 runtime 下的文件，
 * 失败时人可以直接看。
 */
static pid_t spawn_supervisor_ext(const char *runtime, const char *state, const char *sem,
                                  int port, int workers, const char *worker_bin,
                                  int no_bridge, int no_scheduler, int crash_limit,
                                  const char *backoff, int hb_ms, int hb_timeout,
                                  int health_ms, const char *runs_dir)
{
    char port_buf[16];
    char workers_buf[16];
    char limit_buf[16];
    char hb_buf[16];
    char hb_timeout_buf[16];
    char health_buf[16];
    char err_path[KOS_DAEMON_PATH_MAX + 32];
    char *argv[48];
    int ac = 0;
    pid_t pid;

    snprintf(port_buf, sizeof port_buf, "%d", port);
    snprintf(workers_buf, sizeof workers_buf, "%d", workers);
    snprintf(limit_buf, sizeof limit_buf, "%d", crash_limit);
    snprintf(hb_buf, sizeof hb_buf, "%d", hb_ms);
    snprintf(hb_timeout_buf, sizeof hb_timeout_buf, "%d", hb_timeout);
    snprintf(health_buf, sizeof health_buf, "%d", health_ms);

    argv[ac++] = g_sup_bin;
    argv[ac++] = (char *)"--foreground";
    argv[ac++] = (char *)"--runtime-dir";
    argv[ac++] = (char *)runtime;
    argv[ac++] = (char *)"--state-name";
    argv[ac++] = (char *)state;
    argv[ac++] = (char *)"--sem-name";
    argv[ac++] = (char *)sem;
    argv[ac++] = (char *)"--bridge-port";
    argv[ac++] = port_buf;
    argv[ac++] = (char *)"--workers";
    argv[ac++] = workers_buf;
    argv[ac++] = (char *)"--health-interval-ms";
    argv[ac++] = health_buf;
    argv[ac++] = (char *)"--supervisor-heartbeat-ms";
    argv[ac++] = hb_buf;
    argv[ac++] = (char *)"--heartbeat-timeout-ms";
    argv[ac++] = hb_timeout_buf;
    argv[ac++] = (char *)"--stop-timeout-ms";
    argv[ac++] = (char *)"800";
    argv[ac++] = (char *)"--crash-limit";
    argv[ac++] = limit_buf;
    argv[ac++] = (char *)"--crash-window-ms";
    argv[ac++] = (char *)"5000";
    argv[ac++] = (char *)"--backoff-ms";
    argv[ac++] = (char *)backoff;
    argv[ac++] = (char *)"--verbose";
    if (no_bridge) {
        argv[ac++] = (char *)"--no-bridge";
    }
    if (no_scheduler) {
        argv[ac++] = (char *)"--no-scheduler";
    }
    if (worker_bin != NULL) {
        argv[ac++] = (char *)"--worker-bin";
        argv[ac++] = (char *)worker_bin;
    }
    if (runs_dir != NULL) {
        argv[ac++] = (char *)"--runs-dir";
        argv[ac++] = (char *)runs_dir;
    }
    argv[ac] = NULL;

    snprintf(err_path, sizeof err_path, "%s/supervisor-stderr.log", runtime);
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        int fd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) {
                close(fd);
            }
        }
        execv(g_sup_bin, argv);
        _exit(127);
    }
    return pid;
}
static pid_t spawn_supervisor(const char *runtime, const char *state, const char *sem,
                              int port, int workers, const char *worker_bin,
                              int no_bridge, int no_scheduler, int crash_limit,
                              const char *backoff, int hb_ms, int hb_timeout,
                              int health_ms)
{
    return spawn_supervisor_ext(runtime, state, sem, port, workers, worker_bin, no_bridge,
                                no_scheduler, crash_limit, backoff, hb_ms, hb_timeout,
                                health_ms, NULL);
}


static void unlink_objects(const char *state, const char *sem)
{
    kos_shm_opts opts;
    kos_error err;

    memset(&opts, 0, sizeof opts);
    opts.object_name = state;
    opts.sem_name = sem;
    kos_error_clear(&err);
    (void)kos_shm_unlink(&opts, &err);
}

static void remove_tree(const char *path)
{
    char cmd[KOS_DAEMON_PATH_MAX + 32];

    if (path[0] == '\0') {
        return;
    }
    snprintf(cmd, sizeof cmd, "rm -rf -- '%s'", path);
    (void)system(cmd);
}

/* Supervisor 被 SIGKILL 时不会走 unlink_resources；测试自己清掉全局资源信号量，
 * 否则下一次 make test 会在 /dev/shm 里留下 sem.kitchenos_res_*。 */
static void unlink_resource_sems_instance(int instance)
{
    char path[KOS_DAEMON_PATH_MAX];
    kos_kitchen kitchen;
    kos_error err;

    if (kos_child_default_config_path("kitchen.example.json", path, sizeof path) != 0) {
        return;
    }
    kos_error_clear(&err);
    if (kos_kitchen_load(path, &kitchen, &err) == KOS_OK) {
        (void)kos_resources_unlink(&kitchen, instance, &err);
    }
}

/* 自定义 shm 名的运行由 Supervisor 按 pid 派生实例号（见 supervisor_main），
 * 清理时用同一个公式把带后缀的资源信号量一并删掉。 */
static int supervisor_instance_for_pid(pid_t pid)
{
    if (pid <= 0) {
        return KOS_INSTANCE_NONE;
    }
    return (int)((unsigned long)pid % 65534) + 1;
}

static void unlink_resource_sems(void)
{
    /* 没有 pid 的调用点：默认名与派生名都不知道，只能清默认名；
     * cleanup_run 会用 supervisor_instance_for_pid(pid) 再清一次。 */
    unlink_resource_sems_instance(KOS_INSTANCE_NONE);
}

static void cleanup_run(sup_run *r)
{
    if (r->pid > 0) {
        unlink_resource_sems_instance(supervisor_instance_for_pid(r->pid));
    }
    if (r->pid > 0 && kill(r->pid, 0) == 0) {
        kill(r->pid, SIGKILL);
        (void)waitpid(r->pid, NULL, 0);
    }
    unlink_objects(r->state, r->sem);
    unlink_resource_sems();
    remove_tree(r->runtime);
    r->pid = -1;
}

static void init_run(sup_run *r, const char *tag)
{
    memset(r, 0, sizeof *r);
    make_runtime(r->runtime, sizeof r->runtime);
    snprintf(r->state, sizeof r->state, "/kitchenos_t_%s_%ld", tag, (long)getpid());
    snprintf(r->sem, sizeof r->sem, "/kitchenos_ts_%s_%ld", tag, (long)getpid());
    r->port = free_port();
    r->pid = -1;
}

/* ================================================== 1. 纯策略 ============ */

static void test_policy_unit(void)
{
    kos_supervise_policy p;
    kos_child c;
    kos_supervise_decision d;

    printf("[test_supervisor] 1. 崩溃窗口与退避策略\n");
    kos_supervise_policy_init(&p);
    expect(p.crash_limit == 5 && p.crash_window_ms == 60000 &&
               p.backoff_count == 3 && p.backoff_ms[0] == 0 && p.backoff_ms[1] == 2000 &&
               p.backoff_ms[2] == 10000,
           "backoff_schedule_matches_design");
    expect(kos_supervise_delay_for(&p, 1) == 0, "first_failure_restarts_immediately");
    expect(kos_supervise_delay_for(&p, 2) == 2000, "second_failure_waits_2s");
    expect(kos_supervise_delay_for(&p, 3) == 10000, "third_failure_waits_10s");
    expect(kos_supervise_delay_for(&p, 9) == 10000, "last_backoff_repeats");

    {
        kos_error err;

        memset(&p, 0, sizeof p);
        kos_error_clear(&err);
        expect(kos_supervise_policy_parse_backoff("0,50,100", &p, &err) == KOS_OK &&
                   p.backoff_count == 3 && p.backoff_ms[2] == 100,
               "backoff_list_parsed");
        expect(kos_supervise_policy_parse_backoff("0,", &p, &err) != KOS_OK,
               "backoff_trailing_comma_rejected");
        expect(kos_supervise_policy_parse_backoff("nope", &p, &err) != KOS_OK,
               "backoff_non_number_rejected");
    }

    {
        kos_supervise_policy q;

        kos_supervise_policy_init(&q);
        q.crash_limit = 3;
        q.crash_window_ms = 1000;
        kos_child_init(&c, "worker-0", 0);
        d = kos_child_on_exit(&c, 0, 1000, &q);
        expect(d.action == KOS_SUPERVISE_RESTART && d.failures_in_window == 1,
               "window_counts_first_exit");
        d = kos_child_on_exit(&c, 0, 1100, &q);
        expect(d.action == KOS_SUPERVISE_RESTART && d.failures_in_window == 2,
               "window_counts_second_exit");
        d = kos_child_on_exit(&c, 0, 2200, &q);
        expect(d.action == KOS_SUPERVISE_RESTART && d.failures_in_window == 1,
               "crash_window_resets");
    }

    {
        kos_supervise_policy q;
        int i;
        int gave_up = 0;

        kos_supervise_policy_init(&q);
        q.crash_limit = 3;
        q.crash_window_ms = 10000;
        kos_child_init(&c, "worker-0", 0);
        for (i = 1; i <= 3; i++) {
            d = kos_child_on_exit(&c, i << 8, (int64_t)i * 100, &q);
            if (d.action == KOS_SUPERVISE_GIVE_UP && c.state == KOS_CHILD_GIVEN_UP) {
                gave_up = 1;
            }
        }
        expect(gave_up && c.exits == 3, "crash_limit_gives_up");
    }
}

/* ================================================== 2. pid 文件锁 ========= */

static void test_pidfile(void)
{
    char dir[] = "/tmp/kos-pid-test-XXXXXX";
    char path[KOS_DAEMON_PATH_MAX];
    kos_pidfile pf;
    kos_error err;
    pid_t pid;
    int status = 0;

    printf("[test_supervisor] 2. pid 文件 fcntl 锁\n");
    if (mkdtemp(dir) == NULL) {
        expect(0, "pidfile_temp_dir");
        return;
    }
    snprintf(path, sizeof path, "%s/kitchend.pid", dir);
    kos_error_clear(&err);
    expect(kos_pidfile_acquire(path, &pf, &err) == KOS_OK, "pidfile_acquire");

    /* fcntl 记录锁属于进程：第二个持锁者必须真的在另一个进程里。 */
    pid = fork();
    if (pid == 0) {
        kos_pidfile other;
        kos_error child_err;

        kos_error_clear(&child_err);
        _exit(kos_pidfile_acquire(path, &other, &child_err) == KOS_ERR_EXISTS ? 0 : 1);
    }
    if (pid > 0) {
        (void)waitpid(pid, &status, 0);
        expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
               "pidfile_acquire_blocks_second");
    } else {
        expect(0, "pidfile_acquire_blocks_second");
    }

    kos_pidfile_release(&pf, 0);
    pid = fork();
    if (pid == 0) {
        kos_pidfile other;
        kos_error child_err;
        int ok;

        kos_error_clear(&child_err);
        ok = kos_pidfile_acquire(path, &other, &child_err) == KOS_OK;
        if (ok) {
            kos_pidfile_release(&other, 1);
        }
        _exit(ok ? 0 : 1);
    }
    if (pid > 0) {
        (void)waitpid(pid, &status, 0);
        expect(WIFEXITED(status) && WEXITSTATUS(status) == 0 && access(path, F_OK) != 0,
               "pidfile_release_allows_next");
    } else {
        expect(0, "pidfile_release_allows_next");
    }
    remove_tree(dir);
}

/* ================================================== 3. 完整生命周期 ======= */

static void test_lifecycle(void)
{
    sup_run r;
    char supervisor_log[KOS_DAEMON_PATH_MAX + 32];
    char scheduler_log[KOS_DAEMON_PATH_MAX + 32];
    char state_path[KOS_DAEMON_PATH_MAX];
    char sem_path[KOS_DAEMON_PATH_MAX];
    long bridge_pid = 0;
    long sched_pid = 0;
    long worker_pid = 0;
    long worker1_pid = 0;
    long old_worker;
    long new_worker = 0;
    child_status st;
    int status = 0;
    int second_status = 0;
    pid_t second;
    char text[4096];
    char err_text[2048];
    char log_text[32768];
    const char *p_worker;
    const char *p_sched;
    const char *p_bridge;

    printf("[test_supervisor] 3. 完整进程树：重启、信号、清理\n");
    init_run(&r, "life");
    snprintf(supervisor_log, sizeof supervisor_log, "%s/logs/supervisor.log", r.runtime);
    snprintf(scheduler_log, sizeof scheduler_log, "%s/logs/scheduler.log", r.runtime);

    r.pid = spawn_supervisor(r.runtime, r.state, r.sem, r.port, 2, NULL, 0, 0, 5, "0,50,100",
                             50, 800, 100);
    expect(r.pid > 0, "supervisor_spawned");
    if (r.pid <= 0) {
        cleanup_run(&r);
        return;
    }

    expect(wait_child_running(r.runtime, "bridge", 1, STARTUP_TIMEOUT_MS, &bridge_pid),
           "bridge_running");
    expect(wait_child_running(r.runtime, "scheduler", 1, STARTUP_TIMEOUT_MS, &sched_pid),
           "scheduler_running");
    expect(wait_child_running(r.runtime, "worker-0", 1, STARTUP_TIMEOUT_MS, &worker_pid),
           "worker_running");
    expect(wait_child_running(r.runtime, "worker-1", 1, STARTUP_TIMEOUT_MS, &worker1_pid),
           "second_worker_running");

    object_paths(r.state, r.sem, state_path, sizeof state_path, sem_path, sizeof sem_path);
    expect(wait_file_exists(state_path, 2000), "bridge_creates_shm_segment");
    expect(wait_file_exists(sem_path, 2000), "bridge_creates_ready_semaphore");
    expect(wait_log(supervisor_log, "health_tick", 2000), "health_tick_logged");
    expect(log_contains(scheduler_log, "shm_attached"), "scheduler_attaches_shm");
    expect(log_contains(scheduler_log, "world_snapshot"), "scheduler_reads_world_state");

    kill(r.pid, SIGUSR1);
    expect(wait_log(supervisor_log, "status_dump", 2000), "status_dump_on_sigusr1");
    expect(lookup_child(r.runtime, "worker-0", &st) == 0, "status_file_readable");

    kill(r.pid, SIGHUP);
    expect(wait_log(supervisor_log, "sighup_forwarded", 2000), "sighup_forwarded");
    expect(wait_log(scheduler_log, "strategy_switch", 2000),
           "sighup_reaches_scheduler");

    old_worker = worker_pid;
    kill((pid_t)old_worker, SIGKILL);
    expect(wait_child_running(r.runtime, "worker-0", 2, RESTART_TIMEOUT_MS, &new_worker) &&
               new_worker != old_worker,
           "restart_after_sigkill");
    expect(wait_log(supervisor_log, "child_exit role=worker-0", 2000) &&
               log_contains(supervisor_log, "signal=9"),
           "restart_logs_exit_signal");
    expect(log_contains(supervisor_log, "child_restart role=worker-0"), "restart_logged");
    expect(lookup_child(r.runtime, "worker-0", &st) == 0 && st.exits == 1 &&
               st.spawns == 2,
           "status_counts_exits_and_spawns");

    /* 同一运行目录的第二个实例必须在碰日志之前被 pid 锁挡住。 */
    second = spawn_supervisor(r.runtime, r.state, r.sem, r.port, 2, NULL, 0, 0, 5,
                              "0,50,100", 50, 800, 100);
    expect(second > 0 && wait_exit(second, 3000, &second_status), "second_supervisor_exits");
    expect(WIFEXITED(second_status) && WEXITSTATUS(second_status) == 3,
           "second_supervisor_refused");
    snprintf(text, sizeof text, "%s/supervisor-stderr.log", r.runtime);
    expect(read_all(text, err_text, sizeof err_text) == 0 &&
               strstr(err_text, "已在运行") != NULL,
           "second_supervisor_reports_reason");
    expect(kill(r.pid, 0) == 0, "first_supervisor_survives_second_attempt");

    /* 关闭顺序：日志里 worker → scheduler → bridge 的 first-stop 必须按此顺序出现。 */
    kill(r.pid, SIGTERM);
    expect(wait_exit(r.pid, EXIT_TIMEOUT_MS, &status), "sigterm_supervisor_exits");
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "sigterm_exit_code_zero");
    r.pid = -1;

    expect(pid_gone((pid_t)bridge_pid, 1000) && pid_gone((pid_t)sched_pid, 1000) &&
               pid_gone((pid_t)old_worker, 1000) && pid_gone((pid_t)worker1_pid, 1000) &&
               pid_gone((pid_t)new_worker, 1000),
           "sigterm_children_gone");
    expect(access(state_path, F_OK) != 0 && access(sem_path, F_OK) != 0,
           "sigterm_unlinks_ipc");
    snprintf(text, sizeof text, "%s/kitchend.pid", r.runtime);
    expect(access(text, F_OK) != 0, "sigterm_removes_pidfile");

    if (read_all(supervisor_log, log_text, sizeof log_text) == 0) {
        p_worker = strstr(log_text, "child_stop_sent role=worker-1");
        p_sched = strstr(log_text, "child_stop_sent role=scheduler");
        p_bridge = strstr(log_text, "child_stop_sent role=bridge");
        expect(p_worker != NULL && p_sched != NULL && p_bridge != NULL &&
                   p_worker < p_sched && p_sched < p_bridge,
               "shutdown_order_workers_first");
        expect(strstr(log_text, "ipc_unlink") != NULL, "shutdown_logs_ipc_unlink");
    } else {
        expect(0, "shutdown_order_workers_first");
        expect(0, "shutdown_logs_ipc_unlink");
    }

    cleanup_run(&r);
}

/* ================================================== 4. 崩溃循环 =========== */

static void test_crash_loop(void)
{
    sup_run r;
    char supervisor_log[KOS_DAEMON_PATH_MAX + 32];
    child_status st;
    int status = 0;

    printf("[test_supervisor] 4. 崩溃循环达到上限后放弃\n");
    memset(&st, 0, sizeof st);
    init_run(&r, "loop");
    snprintf(supervisor_log, sizeof supervisor_log, "%s/logs/supervisor.log", r.runtime);

    /* /bin/true 收到 Worker 参数后立刻退出 0：对守护进程来说这就是崩溃。 */
    r.pid = spawn_supervisor(r.runtime, r.state, r.sem, r.port, 1, "/bin/true", 1, 1, 2,
                             "0,50", 50, 1000, 100);
    expect(r.pid > 0, "crash_loop_supervisor_spawned");
    if (r.pid <= 0) {
        cleanup_run(&r);
        return;
    }
    expect(wait_log(supervisor_log, "child_given_up role=worker-0", RESTART_TIMEOUT_MS),
           "crash_loop_gives_up");
    expect(wait_status_state(r.runtime, "worker-0", "given_up", 2, 2000, &st),
           "crash_loop_status_given_up");
    expect(st.exits == 2 && st.spawns == 2, "crash_loop_stops_spawning");
    expect(kill(r.pid, 0) == 0, "supervisor_survives_given_up_child");

    kill(r.pid, SIGTERM);
    expect(wait_exit(r.pid, EXIT_TIMEOUT_MS, &status) && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0,
           "crash_loop_shutdown_clean");
    r.pid = -1;
    cleanup_run(&r);
}

/* ================================================== 5. 心跳超时 =========== */

static void test_silent_child(void)
{
    sup_run r;
    char supervisor_log[KOS_DAEMON_PATH_MAX + 32];
    long first = 0;
    long second = 0;
    int status = 0;

    printf("[test_supervisor] 5. 僵尸活进程：心跳超时后重启\n");
    init_run(&r, "silent");
    snprintf(supervisor_log, sizeof supervisor_log, "%s/logs/supervisor.log", r.runtime);

    r.pid = spawn_supervisor(r.runtime, r.state, r.sem, r.port, 1, g_silent_bin, 1, 1, 3,
                             "0,50", 30, 200, 50);
    expect(r.pid > 0, "silent_child_supervisor_spawned");
    if (r.pid <= 0) {
        cleanup_run(&r);
        return;
    }
    expect(wait_child_running(r.runtime, "worker-0", 1, STARTUP_TIMEOUT_MS, &first),
           "silent_child_running");
    expect(wait_log(supervisor_log, "child_unresponsive role=worker-0", 3000),
           "silent_child_restarted");
    expect(wait_child_running(r.runtime, "worker-0", 2, RESTART_TIMEOUT_MS, &second) &&
               second != first,
           "silent_child_new_pid");

    kill(r.pid, SIGTERM);
    expect(wait_exit(r.pid, EXIT_TIMEOUT_MS, &status) && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0,
           "silent_child_shutdown_clean");
    r.pid = -1;
    cleanup_run(&r);
}

/* ================================================== 6. 无 Bridge 清理 ===== */

static void test_cleanup_without_bridge(void)
{
    sup_run r;
    char supervisor_log[KOS_DAEMON_PATH_MAX + 32];
    char state_path[KOS_DAEMON_PATH_MAX];
    char sem_path[KOS_DAEMON_PATH_MAX];
    kos_shm_opts opts;
    kos_error err;
    kos_shm *shm = NULL;
    int status = 0;

    printf("[test_supervisor] 6. Bridge 不在场时 Supervisor 仍负责清理 IPC\n");
    init_run(&r, "clean");
    snprintf(supervisor_log, sizeof supervisor_log, "%s/logs/supervisor.log", r.runtime);

    memset(&opts, 0, sizeof opts);
    opts.object_name = r.state;
    opts.sem_name = r.sem;
    opts.perm = 0;
    kos_error_clear(&err);
    expect(kos_shm_open(&opts, KOS_SHM_CREATE, &shm, &err) == KOS_OK, "precreate_shm");
    if (shm != NULL) {
        kos_error_clear(&err);
        (void)kos_shm_close(shm, &err);
    }

    r.pid = spawn_supervisor(r.runtime, r.state, r.sem, r.port, 1, NULL, 1, 1, 5, "0,50",
                             50, 1000, 100);
    expect(r.pid > 0, "cleanup_supervisor_spawned");
    if (r.pid <= 0) {
        cleanup_run(&r);
        return;
    }
    expect(wait_child_running(r.runtime, "worker-0", 1, STARTUP_TIMEOUT_MS, NULL),
           "worker_attaches_to_preexisting_shm");

    kill(r.pid, SIGTERM);
    expect(wait_exit(r.pid, EXIT_TIMEOUT_MS, &status) && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0,
           "cleanup_supervisor_exits_zero");
    r.pid = -1;

    object_paths(r.state, r.sem, state_path, sizeof state_path, sem_path, sizeof sem_path);
    expect(access(state_path, F_OK) != 0 && access(sem_path, F_OK) != 0,
           "cleanup_without_bridge");
    expect(log_contains(supervisor_log, "ipc_unlink"), "cleanup_logged");
    cleanup_run(&r);
}

/* ================================================== 7. 孤儿退出 =========== */

static void test_orphans(void)
{
    sup_run r;
    long pids[3] = {0, 0, 0};
    const char *roles[3] = {"bridge", "scheduler", "worker-0"};
    char state_path[KOS_DAEMON_PATH_MAX];
    char sem_path[KOS_DAEMON_PATH_MAX];
    char pid_path[KOS_DAEMON_PATH_MAX + 32];
    int i;
    int all_gone = 1;

    printf("[test_supervisor] 7. Supervisor 被 SIGKILL 后子进程因心跳断开退出\n");
    init_run(&r, "orph");
    r.pid = spawn_supervisor(r.runtime, r.state, r.sem, r.port, 1, NULL, 0, 0, 5,
                             "0,50,100", 50, 800, 100);
    expect(r.pid > 0, "orphan_supervisor_spawned");
    if (r.pid <= 0) {
        cleanup_run(&r);
        return;
    }
    for (i = 0; i < 3; i++) {
        expect(wait_child_running(r.runtime, roles[i], 1, STARTUP_TIMEOUT_MS, &pids[i]),
               i == 0 ? "orphan_bridge_running"
                      : (i == 1 ? "orphan_scheduler_running" : "orphan_worker_running"));
    }

    kill(r.pid, SIGKILL);
    (void)waitpid(r.pid, NULL, 0);
    r.pid = -1;
    for (i = 0; i < 3; i++) {
        if (pids[i] > 0 && !pid_gone((pid_t)pids[i], 3000)) {
            all_gone = 0;
            kill((pid_t)pids[i], SIGKILL);
        }
    }
    expect(all_gone, "orphans_exit_when_pipe_breaks");

    /* SIGKILL 不走关闭路径，IPC 与 pid 文件只能由测试自己擦掉。 */
    snprintf(pid_path, sizeof pid_path, "%s/kitchend.pid", r.runtime);
    (void)unlink(pid_path);
    unlink_objects(r.state, r.sem);
    object_paths(r.state, r.sem, state_path, sizeof state_path, sem_path, sizeof sem_path);
    expect(access(state_path, F_OK) != 0 && access(sem_path, F_OK) != 0,
           "orphan_manual_cleanup");
    cleanup_run(&r);
}

/* ================================================== 8. --runs-dir 指定目录生效 ======= */

static void test_runs_dir(void)
{
    sup_run r;
    char custom_runs[KOS_DAEMON_PATH_MAX + 32];
    char scheduler_log[KOS_DAEMON_PATH_MAX + 32];
    char default_runs[KOS_DAEMON_PATH_MAX + 32];
    long sched_pid = 0;
    int status = 0;
    DIR *d;
    struct dirent *ent;
    int found_run = 0;
    char found_run_name[256];

    printf("[test_supervisor] 8. --runs-dir 指定目录生效\n");
    init_run(&r, "runsdir");
    snprintf(custom_runs, sizeof custom_runs, "%s/custom_runs", r.runtime);
    snprintf(scheduler_log, sizeof scheduler_log, "%s/logs/scheduler.log", r.runtime);
    snprintf(default_runs, sizeof default_runs, "%s/runs", r.runtime);

    r.pid = spawn_supervisor_ext(r.runtime, r.state, r.sem, r.port, 0, NULL, 1, 0, 5,
                                 "0,50", 50, 1000, 100, custom_runs);
    expect(r.pid > 0, "runs_dir_supervisor_spawned");
    if (r.pid <= 0) {
        cleanup_run(&r);
        return;
    }

    expect(wait_child_running(r.runtime, "scheduler", 1, STARTUP_TIMEOUT_MS, &sched_pid),
           "runs_dir_scheduler_running");
    expect(wait_log(scheduler_log, "run_dir path=", 2000), "run_dir_logged");

    /* 检查日志中记录的 run_dir 路径确实以 custom_runs 开头 */
    {
        char line[2048];
        int has_path = 0;
        FILE *fp = fopen(scheduler_log, "r");
        if (fp != NULL) {
            while (fgets(line, sizeof line, fp) != NULL) {
                if (strstr(line, "run_dir path=") != NULL &&
                    strstr(line, custom_runs) != NULL) {
                    has_path = 1;
                    break;
                }
            }
            fclose(fp);
        }
        expect(has_path, "run_dir_uses_custom_runs_path");
    }

    /* 检查 custom_runs 目录下生成了时间戳目录且包含 events.jsonl, decisions.jsonl */
    d = opendir(custom_runs);
    expect(d != NULL, "custom_runs_dir_exists");
    if (d != NULL) {
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] != '.') {
                found_run = 1;
                snprintf(found_run_name, sizeof found_run_name, "%s", ent->d_name);
                break;
            }
        }
        closedir(d);
    }
    expect(found_run, "run_subdir_created_in_custom_runs");

    if (found_run) {
        char file_path[KOS_DAEMON_PATH_MAX + 320];
        snprintf(file_path, sizeof file_path, "%s/%s/events.jsonl", custom_runs, found_run_name);
        expect(access(file_path, F_OK) == 0, "events_jsonl_in_custom_runs");
        snprintf(file_path, sizeof file_path, "%s/%s/decisions.jsonl", custom_runs, found_run_name);
        expect(access(file_path, F_OK) == 0, "decisions_jsonl_in_custom_runs");
    }

    /* 默认的 <runtime>/runs 目录由 runtime_dir 预建，但不应包含任何运行子目录 */
    {
        int default_has_runs = 0;

        d = opendir(default_runs);
        if (d != NULL) {
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] != '.') {
                    default_has_runs = 1;
                    break;
                }
            }
            closedir(d);
        }
        expect(default_has_runs == 0, "default_runtime_runs_has_no_subdirs");
    }

    kill(r.pid, SIGTERM);
    expect(wait_exit(r.pid, EXIT_TIMEOUT_MS, &status), "sigterm_runs_dir_supervisor_exits");
    r.pid = -1;
    cleanup_run(&r);
}

static void resolve_binaries(void)
{
    char exe[KOS_DAEMON_PATH_MAX];
    char *slash;
    ssize_t n;

    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) {
        return;
    }
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL) {
        return;
    }
    *slash = '\0';
    snprintf(g_sup_bin, sizeof g_sup_bin, "%s/kitchenos-supervisor", exe);
    snprintf(g_silent_bin, sizeof g_silent_bin, "%s/kitchenos-silent-child", exe);
}

/* 取出 scheduler.log 里 mq_ready 的事件队列名。 */
static int read_events_queue(const char *log_path, char *out, size_t cap)
{
    char body[32768];
    const char *hit;
    size_t len = 0;

    if (read_all(log_path, body, sizeof body) != 0) {
        return -1;
    }
    hit = strstr(body, "mq_ready events=");
    if (hit == NULL) {
        return -1;
    }
    hit += strlen("mq_ready events=");
    while (hit[len] != '\0' && hit[len] != ' ' && hit[len] != '\n' && len + 1 < cap) {
        len++;
    }
    memcpy(out, hit, len);
    out[len] = '\0';
    return len > 0 ? 0 : -1;
}

/*
 * 自定义 shm 名的两棵树（场景 harness 就是这种用法）必须拿到不同的 mq 名；
 * 否则第二棵树会把第一棵的事件收走，os-loop 的 RECOVERED 就是这样丢的。
 * 修复前两个 scheduler.log 都写 events=/kitchenos_events。
 */
static void test_custom_names_isolate_mq(void)
{
    sup_run a;
    sup_run b;
    char log_a[KOS_DAEMON_PATH_MAX + 32];
    char log_b[KOS_DAEMON_PATH_MAX + 32];
    char queue_a[64];
    char queue_b[64];
    int status = 0;

    printf("[test_supervisor] 9. 自定义 shm 名的运行隔离 mq\n");
    init_run(&a, "mqiso_a");
    init_run(&b, "mqiso_b");
    snprintf(log_a, sizeof log_a, "%s/logs/scheduler.log", a.runtime);
    snprintf(log_b, sizeof log_b, "%s/logs/scheduler.log", b.runtime);
    a.pid = spawn_supervisor(a.runtime, a.state, a.sem, a.port, 1, NULL, 1, 0, 5,
                             "0,50,100", 50, 800, 100);
    b.pid = spawn_supervisor(b.runtime, b.state, b.sem, b.port, 1, NULL, 1, 0, 5,
                             "0,50,100", 50, 800, 100);
    expect(a.pid > 0 && b.pid > 0, "two_supervisors_spawned");
    expect(wait_child_running(a.runtime, "scheduler", 1, STARTUP_TIMEOUT_MS, NULL),
           "first_scheduler_running");
    expect(wait_child_running(b.runtime, "scheduler", 1, STARTUP_TIMEOUT_MS, NULL),
           "second_scheduler_running");
    expect(wait_log(log_a, "mq_ready events=", 3000), "first_mq_ready_logged");
    expect(wait_log(log_b, "mq_ready events=", 3000), "second_mq_ready_logged");
    expect(read_events_queue(log_a, queue_a, sizeof queue_a) == 0 &&
               read_events_queue(log_b, queue_b, sizeof queue_b) == 0,
           "event_queue_names_readable");
    if (queue_a[0] != '\0') {
        printf("       first events queue: %s\n", queue_a);
    }
    if (queue_b[0] != '\0') {
        printf("       second events queue: %s\n", queue_b);
    }
    expect(strcmp(queue_a, "/kitchenos_events") != 0 &&
               strcmp(queue_b, "/kitchenos_events") != 0,
           "custom_names_use_a_suffixed_event_queue");
    expect(strcmp(queue_a, queue_b) != 0, "two_runs_do_not_share_event_queue");
    (void)kill(a.pid, SIGTERM);
    (void)kill(b.pid, SIGTERM);
    expect(wait_exit(a.pid, 3000, &status) && wait_exit(b.pid, 3000, &status),
           "both_supervisors_stopped");
    a.pid = -1;
    b.pid = -1;
    cleanup_run(&a);
    cleanup_run(&b);
}

int main(void)
{
    printf("[test_supervisor] 开始：Supervisor 守护进程、重启策略、清理\n");
    resolve_binaries();

    test_policy_unit();
    test_pidfile();
    test_lifecycle();
    test_crash_loop();
    test_silent_child();
    test_cleanup_without_bridge();
    test_orphans();
    test_runs_dir();
    test_custom_names_isolate_mq();

    printf("[test_supervisor] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
