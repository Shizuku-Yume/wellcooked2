/*
 * policy.h -- 订单级调度策略（模块契约）。
 *
 * 这个接口刻意只看得见"订单视图"和"资源视图"：没有游戏 API、没有 socket、没有
 * shm 布局、没有文件路径。策略可以在单元测试里用几个整数证明，也可以被未来的
 * 离散事件模拟器直接复用。
 *
 * 当前实现五种：
 *   FCFS  按到达时间；相同时按下标。
 *   EDF   按截止时间；相同时先按到达时间（所以截止期全相同就退化成 FCFS），再按下标。
 *   LLF   按松弛（截止期 − 现在 − 估计剩余工时）升序；松弛相同退化为 FCFS。
 *   HRRN  按响应比（(等待 + 估计工时) / 估计工时）降序；比值相同退化为 FCFS。
 *   SJF   按估计剩余工时升序；工时相同退化为 FCFS。
 *
 * 工序级排序（kos_task_policy）只是名字和解析放在这里，排序本身在 Scheduler 的
 * 派工里：FIFO 按开工顺序 + 工序下标，CP 按关键路径，COST 是五项加权代价。
 *
 * est_cost_ms 由调用方填"从该订单现在这道工序到做完还要多久"的估计：未开工订单是
 * 菜谱关键路径，已开工订单是未完成节点的最长剩余路径。策略不自己估工时。
 */
#ifndef KITCHENOS_POLICY_H
#define KITCHENOS_POLICY_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>

#include <kitchenos/shm.h>

#define KOS_POLICY_NAME_MAX 16

typedef enum {
    KOS_ORDER_ACTIVE = 0,
    KOS_ORDER_COMPLETED = 1,
    KOS_ORDER_EXPIRED = 2,
    KOS_ORDER_FAILED = 3
} kos_order_status;

typedef struct {
    char id[KOS_SHM_NAME_MAX];
    char dish[KOS_SHM_NAME_MAX];
    int64_t arrival_ms;
    int64_t deadline_ms;
    int64_t est_cost_ms;
    int reward;
    int status; /* kos_order_status */
} kos_order_view;

typedef struct {
    char kind[KOS_SHM_NAME_MAX];
    int capacity;
    int used;
} kos_resource_view;

typedef enum {
    KOS_POLICY_FCFS = 0,
    KOS_POLICY_EDF = 1,
    KOS_POLICY_LLF = 2,
    KOS_POLICY_HRRN = 3,
    KOS_POLICY_SJF = 4
} kos_policy_kind;

typedef enum {
    KOS_TASK_POLICY_COST = 0, /* 默认：五项加权代价 + 匹配 */
    KOS_TASK_POLICY_FIFO = 1, /* 开工顺序 + 工序下标，不看路程 */
    KOS_TASK_POLICY_CP = 2    /* 剩余关键路径最长的先做 */
} kos_task_policy;

typedef struct kos_policy kos_policy; /* opaque */

kos_policy *kos_policy_create(kos_policy_kind kind, kos_error *err);
void kos_policy_destroy(kos_policy *policy);

kos_policy_kind kos_policy_kind_of(const kos_policy *policy);
/* "FCFS" / "EDF" / "LLF" / "HRRN" / "SJF"；未知 kind 返回 "FCFS"。 */
const char *kos_policy_name(const kos_policy *policy);
const char *kos_policy_kind_name(kos_policy_kind kind);
void kos_policy_set_kind(kos_policy *policy, kos_policy_kind kind);
/* SIGHUP 的轮换顺序：FCFS → EDF → LLF → HRRN → SJF → FCFS。 */
kos_policy_kind kos_policy_other(kos_policy_kind kind);
/* 名字不分大小写；0 = ok。 */
int kos_policy_kind_from_name(const char *name, kos_policy_kind *out);

/* "fifo" / "cp" / "cost"；未知值返回 "cost"。 */
const char *kos_task_policy_name(kos_task_policy policy);
int kos_task_policy_from_name(const char *name, kos_task_policy *out); /* 0 = ok，不分大小写 */

/*
 * strategy.request 的一行：`order=<名> task=<名>`，两个键都可选、顺序任意、空白分隔，
 * 名字不分大小写。成功返回 0，并把出现的键写进 *order / *task，对应的 *has_* 置 1；
 * 未知键、未知名字、两个键都没有，返回 KOS_ERR_INVAL，输出不动。
 */
int kos_policy_parse_request(const char *text, kos_policy_kind *order, int *has_order,
                             kos_task_policy *task, int *has_task);

/*
 * 从 orders 里挑一个（只看 status == KOS_ORDER_ACTIVE 的项）。成功返回 0 并写
 * *chosen（orders 数组下标）；没有可选项返回 KOS_ERR_EMPTY；参数错误返回
 * KOS_ERR_INVAL。`resources` 目前只作为接口的一部分（资源可用性由调用方在构造
 * 候选列表时过滤），策略本身不修改它。
 */
int kos_policy_pick(const kos_policy *policy, const kos_order_view *orders, int order_count,
                    const kos_resource_view *resources, int resource_count,
                    int64_t now_ms, int *chosen);

#endif /* KITCHENOS_POLICY_H */
