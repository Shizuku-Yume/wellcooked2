/*
 * resource.h -- 命名计数信号量、租约表与资源分配图（模块契约）。
 *
 * 三类东西放在一起，因为它们回答同一个问题："这个厨师现在拿着什么，别人还要等多久？"
 *
 *   1. 每个 kitchen.json 的工位 kind 一个 POSIX 计数信号量 `/wellcooked_res_<kind>`，
 *      容量 = 该 kind 的工位数。由 Supervisor 创建与 unlink，Scheduler attach 后用作
 *      唯一的所有权凭证（不再另存进程内计数器）。
 *   2. 租约表：谁（chef）因为哪个任务（task_id）拿了哪个 kind、什么时候拿、什么时候
 *      到期。释放必须先按 task_id 找到并删除租约，所以同一个任务不可能 release 两次。
 *   3. 资源分配图：chef → 持有的资源 → 该资源的持有者 ↔ chef 想要的资源。检测到环时
 *      返回环上的 chef 序列；victim 规则由 kos_deadlock_victim() 固定。
 */
#ifndef WELLCOOKED_RESOURCE_H
#define WELLCOOKED_RESOURCE_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <semaphore.h>
#include <stdint.h>

#include <wellcooked/kitchen.h>
#include <wellcooked/shm.h>

#define KOS_RES_KIND_MAX 32
#define KOS_RES_NAME_MAX 64
#define KOS_RES_MAX 32
#define KOS_LEASE_MAX 64

typedef struct {
    char kind[KOS_RES_KIND_MAX];
    char name[KOS_RES_NAME_MAX];
    int capacity;
    sem_t *sem;
} kos_res_slot;

typedef struct {
    kos_res_slot items[KOS_RES_MAX];
    int count;
} kos_resource_set;

/* 从 kitchen 的 stations 统计每个 kind 的工位数并创建信号量；同名旧对象先 unlink。
 * 成功只留下命名对象（句柄立即关闭），Scheduler 之后再 attach。 */
int kos_resources_create(const kos_kitchen *kitchen, int instance, kos_error *err);

/* 打开所有 kind 的信号量。缺任何一个都返回 KOS_ERR_MISSING（Supervisor 应先 create）。 */
int kos_resources_attach(const kos_kitchen *kitchen, kos_resource_set *set, int instance,
                         kos_error *err);
void kos_resources_close(kos_resource_set *set);

/* 删除 kitchen 里出现的所有 kind 的名字；已不存在算成功（幂等）。 */
int kos_resources_unlink(const kos_kitchen *kitchen, int instance, kos_error *err);

int kos_resources_index(const kos_resource_set *set, const char *kind);
int kos_resource_capacity(const kos_resource_set *set, const char *kind);

/* 0 = 拿到一个令牌；KOS_ERR_EMPTY = 容量已满；其他 = 系统错误。 */
int kos_resource_try_acquire(kos_resource_set *set, const char *kind);
int kos_resource_release(kos_resource_set *set, const char *kind);
int kos_resource_value(const kos_resource_set *set, const char *kind, int *value);

typedef struct {
    int used;
    char kind[KOS_RES_KIND_MAX];
    int chef_id;
    uint64_t task_id;
    int64_t acquired_ms;
    int64_t deadline_ms;
    int order_index;
    int task_index;
} kos_lease;

typedef struct {
    kos_lease leases[KOS_LEASE_MAX];
    int count;
} kos_lease_table;

void kos_leases_init(kos_lease_table *table);

/*
 * 添加租约。同一个 task_id 或同一个 chef 已有租约时返回 NULL（调用方必须把刚拿到的
 * 信号量还回去）。deadline_ms 是单调时间的绝对毫秒。
 */
kos_lease *kos_lease_add(kos_lease_table *table, const char *kind, int chef_id,
                         uint64_t task_id, int64_t acquired_ms, int64_t deadline_ms,
                         int order_index, int task_index);

kos_lease *kos_lease_by_task(kos_lease_table *table, uint64_t task_id);
kos_lease *kos_lease_by_chef(kos_lease_table *table, int chef_id);
int kos_lease_expired(const kos_lease *lease, int64_t now_ms);

/* 1 = 删除了一条；0 = 本来就没有。调用方只有拿到 1 才能 sem_post。 */
int kos_lease_remove(kos_lease_table *table, uint64_t task_id);

/*
 * 资源分配图环检测。
 * held[chef]   = 持有的资源下标，-1 表示没有；
 * wanted[chef] = 正在等待的资源下标，-1 表示没有。
 * 有环返回 1 并写 cycle（chef 下标，不重复起点，长度 *cycle_len）；无环返回 0。
 */
int kos_deadlock_find(const int *held, const int *wanted, int chef_count,
                      int resource_count, int *cycle, int *cycle_len);

/* victim 规则：环上编号最大的厨师（最近被调度的那个），丢掉它的租约代价最小。 */
int kos_deadlock_victim(const int *cycle, int cycle_len);

/* instance >= 0 时名字加 `_<instance>`；KOS_INSTANCE_NONE 保持旧名字。 */
const char *kos_resource_name(const char *kind, int instance, char *buf, size_t len);

#endif /* WELLCOOKED_RESOURCE_H */
