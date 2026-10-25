/*
 * child.h -- Scheduler 与 Chef Worker 共用的进程骨架（模块契约）。
 *
 * 两个进程的"外围"完全相同：解析同一组参数、打开自己的日志、在 Bridge 还没建段时
 * 重试挂接 /kitchenos_state、向 Supervisor 的心跳管道写字节、把 SIGTERM/SIGHUP 变成
 * 日志事件后退出。业务循环（调度 / 路径）各自不同，所以这里只提供可轮询的启动、
 * 单步与收尾，而不是一个替调用方决定循环节奏的 run()。
 *
 * 心跳管道断开（Supervisor 已死）时进程主动退出：守护进程被 SIGKILL 时，孤儿子进程
 * 继续占着共享内存与资源信号量比直接退出更糟。
 */
#ifndef KITCHENOS_CHILD_H
#define KITCHENOS_CHILD_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>

#include <kitchenos/daemon.h>
#include <kitchenos/log.h>
#include <kitchenos/shm.h>

enum {
    KOS_CHILD_ARG_OK = 0,
    KOS_CHILD_ARG_ERR = 1,
    KOS_CHILD_ARG_HELP = 2,
    KOS_CHILD_ARG_VERSION = 3
};

typedef enum {
    KOS_CHILD_SCHEDULER = 0,
    KOS_CHILD_WORKER = 1,
    KOS_CHILD_NAVD = 2
} kos_child_kind;

typedef struct {
    kos_child_kind kind;
    int chef_id; /* Worker: 0..KOS_SHM_MAX_CHEFS-1；Scheduler: -1 */
    char runtime_dir[KOS_DAEMON_PATH_MAX];
    char log_path[KOS_DAEMON_PATH_MAX]; /* 空 = <runtime>/logs/<默认名> */
    const char *state_name;             /* NULL = shm.h 默认值 */
    const char *sem_name;
    int supervisor_fd;         /* 心跳管道写端；-1 = 没有 */
    int heartbeat_interval_ms; /* 0 = 不发心跳 */
    int attach_retry_ms;
    int verbose;

    /* 两个进程共用同一份 CLI；各自只读自己需要的字段。 */
    char kitchen_path[KOS_DAEMON_PATH_MAX];
    char recipes_path[KOS_DAEMON_PATH_MAX];
    char orders_path[KOS_DAEMON_PATH_MAX];
    char runs_dir[KOS_DAEMON_PATH_MAX];
    char strategy[16]; /* "fcfs" / "edf" / "llf" / "hrrn" */
    int chefs;         /* Scheduler: 输入槽数量（= Worker 数） */
    int generation;    /* Worker: 第几次拉起；>1 时才发 RECOVERED */
    int tick_ms;       /* Scheduler: 调度轮询周期 */
    int slot_interval_ms; /* Worker: 写输入槽的周期 */
    int lease_ttl_ms;    /* Scheduler: 资源租约时长 */
    char nav_name[KOS_SHM_PATH_MAX];     /* Worker/navd: 导航表对象名 */
    char nav_sem_name[KOS_SHM_PATH_MAX]; /* Worker/navd: 导航表锁名 */
    char nav[8];                         /* navd: "pibt"（默认）或 "pa" 对照基线 */
} kos_child_opts;

typedef struct {
    kos_child_opts opts;
    char runtime_dir[KOS_DAEMON_PATH_MAX];
    char log_path[KOS_DAEMON_PATH_MAX];
    kos_log *log;
    kos_shm *shm;
    int64_t next_heartbeat_ms;
    int64_t next_attach_ms;
    int64_t next_attach_log_ms;
    int heartbeat_dead;
} kos_child_runtime;

void kos_child_opts_init(kos_child_opts *o, kos_child_kind kind);

/*
 * 接受两种进程共有的一组开关：
 *   --runtime-dir DIR  --state-name NAME  --sem-name NAME  --log FILE
 *   --supervisor-fd N  --supervisor-heartbeat-ms M  --attach-retry-ms M
 *   --kitchen FILE  --recipes FILE  --orders FILE  --runs-dir DIR
 *   --strategy fcfs|edf|llf|hrrn  --chefs N  --generation N  --lease-ttl-ms M
 *   --tick-ms M  --slot-interval-ms M  --nav-name NAME  --nav-sem-name NAME
 *   --nav pibt|pa  --verbose  --help  --version
 * Worker 另加 --chef-id N（缺省 0）。未知参数返回 KOS_CHILD_ARG_ERR。
 */
int kos_child_parse_args(kos_child_opts *o, int argc, char **argv);

void kos_child_print_usage(FILE *out, kos_child_kind kind);

/*
 * 启动骨架：解析默认运行目录、开日志、装信号、初始化重试时间。返回 0 成功，1 失败
 * （已把原因写到 stderr/日志）。之后调用方自己驱动 kos_child_poll()。
 */
int kos_child_setup(kos_child_runtime *rt, const kos_child_opts *opts);

/* 处理心跳与 shm 挂接重试；返回 1 = 应当停止（SIGTERM/SIGINT 或心跳断裂）。 */
int kos_child_poll(kos_child_runtime *rt);

/* 睡到下一个心跳/挂接期限，最多 max_ms；信号会提前打断。 */
void kos_child_sleep(kos_child_runtime *rt, int max_ms);

/* 记录停止原因、关闭 shm 与日志。重复调用安全。 */
void kos_child_teardown(kos_child_runtime *rt);

/* SIGHUP / SIGUSR1 各是一次性的：取走并清标志，调用方决定怎么处理。 */
int kos_child_take_sighup(void);
int kos_child_take_sigusr1(void);
int kos_child_stop_signal(void);

static inline kos_log *kos_child_log(kos_child_runtime *rt)
{
    return rt != NULL ? rt->log : NULL;
}

static inline kos_shm *kos_child_shm(kos_child_runtime *rt)
{
    return rt != NULL ? rt->shm : NULL;
}

/*
 * config/<filename> 的默认位置：可执行文件目录的 ../../config（仓库布局
 * bridge/build 或 bridge/build/san），找不到再试当前目录的 config/。返回 0 成功。
 */
int kos_child_default_config_path(const char *filename, char *buf, size_t len);

/* 兼容 Phase 2 的最小骨架：setup → poll 循环 → teardown。现在两个真实进程都用
 * kos_child_setup() 自己驱动，这个入口只留给以后的简单子进程。 */
int kos_child_run(kos_child_opts *o);

#endif /* KITCHENOS_CHILD_H */
