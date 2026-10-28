/*
 * navtab.h -- /wellcooked_nav：Worker 与 navd 之间的导航表（模块契约）。
 *
 * 独立于 16 KB 的世界状态段：状态段是 Bridge 写的只读快照，导航表是双向的控制面。
 * 每个厨师两个槽，各只有一个写者：
 *   - 目标槽：Worker 写（目标工位、目标序号、紧迫度、pinned），navd 只读；
 *   - 计划槽：navd 写（已放行的路点、计划序号、状态、写入时的游戏时钟），Worker 只读。
 * 槽内用 seqlock 自证自洽，另有一把命名信号量 /wellcooked_nav_lock 协调写者；读侧
 * 发现序号为奇数或校验不符时重试，所以写者在临界区里被 SIGKILL 也不会让读者拿到
 * 半份数据。信号量本身只做协调，写者用超时获取并在超时后接管（见 navtab.c），
 * 因为 POSIX 命名信号量没有 robust mutex 语义。
 *
 * 名字可以由 Supervisor 按运行实例改写（多开时每个进程树一份），默认值就是
 * /wellcooked_nav 与 /wellcooked_nav_lock。
 */
#ifndef WELLCOOKED_NAVTAB_H
#define WELLCOOKED_NAVTAB_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>

#include <wellcooked/shm.h>

#define KOS_NAV_MAGIC 0x4b4f5332u /* "KOS2" */
#define KOS_NAV_VERSION 1u
#define KOS_NAV_MAX_PLAN 8
#define KOS_NAV_DEFAULT_NAME "/wellcooked_nav"
#define KOS_NAV_DEFAULT_SEM_NAME "/wellcooked_nav_lock"
#define KOS_NAV_READ_ATTEMPTS 64
/* 卡住与重规划判定窗口：navd 发现位置未变化达到该时长则触发一次重规划；
 * 连续达到次数上限后置 KOS_NAV_BLOCKED 供 Worker 上报 Scheduler 改派。
 * 共享此常量以确保宏动作层（macro.c）的超时计算严格覆盖判定窗口。 */
#define KOS_NAV_STUCK_MS 2000
#define KOS_NAV_STUCK_LIMIT 3
#define KOS_NAV_BLOCKED_WINDOW_MS ((int64_t)KOS_NAV_STUCK_LIMIT * KOS_NAV_STUCK_MS)


/* 计划槽状态。数值写进共享内存，增删要同时改 navd 与 Worker。 */
typedef enum {
    KOS_NAV_IDLE = 0,    /* 没目标，也没有要让的路 */
    KOS_NAV_MOVING = 1,  /* 有已放行的路点 */
    KOS_NAV_ARRIVED = 2, /* 已到目标工位的站位格（实际坐标 ≤ 0.3 m） */
    KOS_NAV_STUCK = 3,   /* 本轮判定卡住，正在重规划 */
    KOS_NAV_BLOCKED = 4  /* 连续 3 次卡住；Worker 报 BLOCKED 后 Scheduler 改派 */
} kos_nav_status;

typedef struct {
    int32_t has_target;                 /* 0 = 没任务，navd 可把这人当可推开的路人 */
    int32_t pinned;                     /* 正在站位格干活，navd 当障碍，不推 */
    uint32_t target_seq;                /* Worker 每次换目标加一；navd 原样回显 */
    uint32_t urgency;                   /* Scheduler 的订单排名，PIBT 优先级用 */
    char station_id[KOS_SHM_NAME_MAX];  /* 目标工位 id；空串 = 没有目标 */
} kos_nav_target;

typedef struct {
    uint64_t seq; /* seqlock：奇 = 正在写。0 = 从未写过 */
    kos_nav_target target;
} kos_nav_target_slot;

typedef struct {
    uint32_t plan_seq;       /* 内容变化才加一；不变时只刷新 game_time_ms */
    int32_t status;          /* kos_nav_status */
    uint32_t target_seq;     /* 本计划对应的目标槽序号 */
    int32_t waypoint_count;  /* 下面 waypoints[] 的有效前缀，0..KOS_NAV_MAX_PLAN */
    kos_waypoint waypoints[KOS_NAV_MAX_PLAN]; /* 已放行且尚未走到的格中心 */
    uint64_t game_time_ms;   /* navd 写这一版时的游戏时钟；Worker 用它判 navd 是否还活着 */
    uint32_t replans;        /* 本目标累计重规划次数 */
    uint32_t stuck_count;    /* 连续卡住次数，0..3 */
} kos_nav_plan;

typedef struct {
    uint64_t seq;
    kos_nav_plan plan;
} kos_nav_plan_slot;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t target_slot_size;
    uint32_t plan_slot_size;
    uint32_t max_chefs;
    int64_t creator_pid;
    int64_t created_at_ms;
    uint32_t reserved[4];
    kos_nav_target_slot targets[KOS_SHM_MAX_CHEFS];
    kos_nav_plan_slot plans[KOS_SHM_MAX_CHEFS];
} kos_nav_segment;

typedef struct {
    const char *object_name; /* NULL = KOS_NAV_DEFAULT_NAME（可被 WELLCOOKED_NAV_NAME 覆盖） */
    const char *sem_name;    /* NULL = KOS_NAV_DEFAULT_SEM_NAME（同理） */
    int perm;                /* 0 = 0600 */
} kos_navtab_opts;

typedef struct kos_navtab {
    int fd;
    sem_t *sem;
    kos_nav_segment *seg;
    size_t map_size;
    int created;
    char object_name[KOS_SHM_PATH_MAX];
    char sem_name[KOS_SHM_PATH_MAX];
} kos_navtab;

/* 先清空，再按 env → 常量填 object_name / sem_name。 */
void kos_navtab_opts_init(kos_navtab_opts *opts);

/* 新建导航表；同名对象已存在返回 KOS_ERR_EXISTS（清理残留是 Supervisor 的事）。 */
int kos_navtab_create(const kos_navtab_opts *opts, kos_navtab **out, kos_error *err);

/* 挂接已存在的导航表。 */
int kos_navtab_open(const kos_navtab_opts *opts, kos_navtab **out, kos_error *err);

int kos_navtab_close(kos_navtab *tab);

/* 删除共享内存对象与信号量；已经不存在的也算成功。 */
int kos_navtab_unlink(const kos_navtab_opts *opts, kos_error *err);

/* 以下读写都做下标检查；tab 为 NULL 返回 KOS_ERR_INVAL。 */
int kos_navtab_read_target(kos_navtab *tab, int chef, kos_nav_target *out);
int kos_navtab_write_target(kos_navtab *tab, int chef, const kos_nav_target *target);
int kos_navtab_read_plan(kos_navtab *tab, int chef, kos_nav_plan *out);
int kos_navtab_write_plan(kos_navtab *tab, int chef, const kos_nav_plan *plan);

const char *kos_nav_status_name(int status);

/*
 * Supervisor 多开时按世界状态段名派生导航表名：默认名不变，自定义名加 _nav /
 * _nav_lock 后缀。buf 太短返回 -1。
 */
int kos_navtab_default_names(const char *state_name, char *nav_name, size_t nav_len,
                             char *sem_name, size_t sem_len);

#endif /* WELLCOOKED_NAVTAB_H */
