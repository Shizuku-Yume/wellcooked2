/*
 * policy.h -- 订单级调度策略（模块契约）。
 *
 * 这个接口刻意只看得见"订单视图"和"资源视图"：没有游戏 API、没有 socket、没有
 * shm 布局、没有文件路径。策略可以在单元测试里用几个整数证明，也可以被未来的
 * 离散事件模拟器直接复用。
 *
 * 当前实现两种：
 *   FCFS  按到达时间；相同时按下标。
 *   EDF   按截止时间；相同时先按到达时间（所以截止期全相同就退化成 FCFS），再按下标。
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
    KOS_POLICY_EDF = 1
} kos_policy_kind;

typedef struct kos_policy kos_policy; /* opaque */

kos_policy *kos_policy_create(kos_policy_kind kind, kos_error *err);
void kos_policy_destroy(kos_policy *policy);

kos_policy_kind kos_policy_kind_of(const kos_policy *policy);
const char *kos_policy_name(const kos_policy *policy); /* "FCFS" / "EDF" */
void kos_policy_set_kind(kos_policy *policy, kos_policy_kind kind);
kos_policy_kind kos_policy_other(kos_policy_kind kind);
int kos_policy_kind_from_name(const char *name, kos_policy_kind *out); /* 0 = ok */

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
