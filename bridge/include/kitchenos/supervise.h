/*
 * supervise.h -- 子进程状态机（模块契约）。
 *
 * 这里没有任何系统调用：崩溃窗口、退避延迟、崩溃上限与"该不该再拉起"都是纯函数，
 * 输入是显式的单调毫秒时间。Supervisor 只负责 fork/exec、收尸和日志，因此重启策略
 * 可以在单元测试里用几个假时间戳证明，而不是靠 sleep 去赌。
 *
 * 默认策略取自课程文档 §6.1：第一次立即重启；窗口内第二次等 2 s；第三次起等 10 s；
 * 一分钟后窗口清零；窗口内第 5 次退出后停止自动重启并告警。参数全部可配，测试用
 * 几十毫秒窗口在几秒内复现崩溃循环。
 */
#ifndef KITCHENOS_SUPERVISE_H
#define KITCHENOS_SUPERVISE_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <sys/types.h>

#include <kitchenos/shm.h>

#define KOS_SUPERVISE_MAX_CHILDREN 16 /* 1 Bridge + 1 Scheduler + 8 Workers 的余量 */
#define KOS_SUPERVISE_MAX_BACKOFF 8
#define KOS_SUPERVISE_HISTORY 16
#define KOS_SUPERVISE_ROLE_MAX 24

typedef struct {
    int crash_limit;   /* 窗口内允许的退出次数；达到即放弃，1..KOS_SUPERVISE_HISTORY */
    int crash_window_ms;
    int backoff_ms[KOS_SUPERVISE_MAX_BACKOFF];
    int backoff_count; /* >= 1；列表最后一项重复使用 */
} kos_supervise_policy;

/* 默认值：5 次 / 60000 ms，退避 0,2000,10000。 */
void kos_supervise_policy_init(kos_supervise_policy *p);

/* 解析 "0,2000,10000"：逗号分隔的非负毫秒，最多 8 项，至少 1 项。 */
int kos_supervise_policy_parse_backoff(const char *list, kos_supervise_policy *p,
                                       kos_error *err);

/* 第 failures_in_window 次失败对应的延迟；<= 1 时为 0（立即重启）。 */
int kos_supervise_delay_for(const kos_supervise_policy *p, int failures_in_window);

enum {
    KOS_CHILD_RUNNING = 0,
    KOS_CHILD_PENDING = 1,  /* 已退出，正在等退避到期 */
    KOS_CHILD_GIVEN_UP = 2, /* 崩溃循环上限，不再拉起 */
    KOS_CHILD_STOPPED = 3,  /* 由 SIGTERM 关闭流程停掉 */
    KOS_CHILD_DISABLED = 4  /* 配置里就没有这个子进程 */
};

typedef struct {
    char role[KOS_SUPERVISE_ROLE_MAX]; /* "bridge" / "scheduler" / "worker-0" */
    pid_t pid;                          /* 0 = 当前没有活着的进程 */
    int state;
    int64_t spawns; /* 累计 fork 成功次数 */
    int64_t exits;  /* 累计退出次数 */
    int64_t exit_times[KOS_SUPERVISE_HISTORY]; /* 环形历史，单调毫秒 */
    int exit_count;  /* 写进环里的总次数（环形下标 = exit_count % HISTORY） */
    int64_t next_restart_ms;
    int64_t last_exit_ms;
    int last_exit_code;   /* WIFEXITED 时的退出码，否则 -1 */
    int last_exit_signal; /* WIFSIGNALED 时的信号，否则 0 */
} kos_child;

enum {
    KOS_SUPERVISE_NONE = 0,
    KOS_SUPERVISE_RESTART = 1,
    KOS_SUPERVISE_GIVE_UP = 2
};

typedef struct {
    int action;
    int delay_ms;
    int failures_in_window;
} kos_supervise_decision;

void kos_child_init(kos_child *c, const char *role, pid_t pid);

/* fork/exec 成功后调用：pid 生效、state=RUNNING、spawns++。 */
void kos_child_started(kos_child *c, pid_t pid);

/*
 * 记录一次退出并给出下一步。`status` 是 waitpid 的原始状态；now_ms 由调用方给，
 * 便于测试构造窗口边界。GIVE_UP 时 state 变 GIVEN_UP 且 next_restart_ms=0；
 * RESTART 时 state=PENDING 且 next_restart_ms=now+delay（delay 可为 0）。
 */
kos_supervise_decision kos_child_on_exit(kos_child *c, int status, int64_t now_ms,
                                         const kos_supervise_policy *p);

/* 当前窗口内的退出次数（只数环形历史里 now-window 之后的）。 */
int kos_child_failures_in_window(const kos_child *c, int64_t now_ms, int window_ms);

/* state == RUNNING && pid > 0。 */
int kos_child_alive(const kos_child *c);

const char *kos_child_state_name(int state);

#endif /* KITCHENOS_SUPERVISE_H */
