/*
 * test_resources.c -- 命名计数信号量、租约唯一释放与死锁环检测。
 *
 * 这里不经过 Scheduler 进程：直接构造只含两个工位的 kitchen，让 resource.c 走
 * sem_open/sem_unlink 的真实路径。double release 的证明不是"日志里没有第二次"，
 * 而是先 sem_wait 再只按租约删除结果 sem_post；第二次删除返回 0，令牌数量保持
 * capacity，不可能变成 capacity + 1。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/kitchen.h"
#include "kitchenos/resource.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_checks;
static int g_failures;

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

static void make_kitchen(kos_kitchen *kitchen, const char *kind)
{
    memset(kitchen, 0, sizeof *kitchen);
    kitchen->station_count = 2;
    snprintf(kitchen->stations[0].kind, sizeof kitchen->stations[0].kind, "%s", kind);
    snprintf(kitchen->stations[1].kind, sizeof kitchen->stations[1].kind, "%s", kind);
    snprintf(kitchen->stations[0].id, sizeof kitchen->stations[0].id, "r0");
    snprintf(kitchen->stations[1].id, sizeof kitchen->stations[1].id, "r1");
}

static int sem_value(const kos_resource_set *set, const char *kind)
{
    int value = -1;

    if (kos_resource_value(set, kind, &value) != KOS_OK) {
        return -1;
    }
    return value;
}

int main(void)
{
    char kind[KOS_RES_KIND_MAX];
    char other[KOS_RES_KIND_MAX];
    char name[KOS_RES_NAME_MAX];
    kos_kitchen kitchen;
    kos_resource_set set;
    kos_error err;
    kos_lease_table leases;
    int64_t now = now_ms();

    snprintf(kind, sizeof kind, "tres_%ld", (long)getpid());
    snprintf(other, sizeof other, "tother_%ld", (long)getpid());
    make_kitchen(&kitchen, kind);

    printf("[test_resources] 1. 创建/attach 容量\n");
    kos_error_clear(&err);
    expect(kos_resources_create(&kitchen, KOS_INSTANCE_NONE, &err) == KOS_OK, "sem_created");
    kos_error_clear(&err);
    expect(kos_resources_attach(&kitchen, &set, KOS_INSTANCE_NONE, &err) == KOS_OK, "sem_attached");
    expect(set.count == 1 && set.items[0].capacity == 2, "one_kind_capacity_two");
    expect(sem_value(&set, kind) == 2, "initial_value_equals_capacity");
    expect(kos_resource_name(kind, KOS_INSTANCE_NONE, name, sizeof name) != NULL &&
               strncmp(name, "/kitchenos_res_", 15) == 0,
           "named_sem_prefix");
    {
        char suffixed[KOS_RES_NAME_MAX];
        char expected[KOS_RES_NAME_MAX];

        snprintf(expected, sizeof expected, "/kitchenos_res_%s_3", kind);
        expect(kos_resource_name(kind, 3, suffixed, sizeof suffixed) != NULL &&
                   strcmp(suffixed, expected) == 0,
               "instance_appends_suffix");
        expect(kos_resource_name(kind, KOS_INSTANCE_NONE, suffixed, sizeof suffixed) != NULL &&
                   strcmp(suffixed, name) == 0,
               "no_instance_keeps_old_name");
    }

    printf("[test_resources] 2. wait_then_acquire\n");
    expect(kos_resource_try_acquire(&set, kind) == KOS_OK, "acquire_first");
    expect(kos_resource_try_acquire(&set, kind) == KOS_OK, "acquire_second");
    expect(kos_resource_try_acquire(&set, kind) == KOS_ERR_EMPTY, "third_acquire_empty");
    expect(sem_value(&set, kind) == 0, "value_zero_when_full");
    expect(kos_resource_release(&set, kind) == KOS_OK, "release_one");
    expect(sem_value(&set, kind) == 1, "value_one_after_release");
    expect(kos_resource_try_acquire(&set, kind) == KOS_OK, "wait_then_acquire");
    expect(sem_value(&set, kind) == 0, "value_zero_again");

    printf("[test_resources] 3. 租约删除是唯一释放凭证\n");
    kos_leases_init(&leases);
    expect(kos_lease_add(&leases, kind, 0, 101, now, now + 1000, 0, 0) != NULL,
           "lease_added");
    expect(kos_lease_add(&leases, kind, 1, 101, now, now + 1000, 0, 0) == NULL,
           "duplicate_task_rejected");
    expect(kos_lease_add(&leases, kind, 0, 102, now, now + 1000, 0, 1) == NULL,
           "duplicate_chef_rejected");
    expect(kos_lease_remove(&leases, 101) == 1, "first_remove_returns_one");
    expect(kos_lease_remove(&leases, 101) == 0, "second_remove_returns_zero");
    (void)kos_resource_release(&set, kind);
    expect(sem_value(&set, kind) == 1, "single_post_for_single_lease");
    (void)kos_resource_release(&set, kind); /* 归还前面测试占用的另一个槽位 */
    expect(sem_value(&set, kind) == 2, "back_to_capacity");

    printf("[test_resources] 4. lease_expiry_reclaims\n");
    kos_leases_init(&leases);
    expect(kos_resource_try_acquire(&set, kind) == KOS_OK, "expiry_acquire");
    expect(kos_lease_add(&leases, kind, 0, 201, now - 2000, now - 1, 0, 0) != NULL,
           "expired_lease_added");
    {
        kos_lease *lease = kos_lease_by_task(&leases, 201);

        expect(lease != NULL && kos_lease_expired(lease, now) == 1, "lease_is_expired");
        expect(kos_lease_remove(&leases, 201) == 1, "expiry_remove_once");
        (void)kos_resource_release(&set, kind);
        expect(sem_value(&set, kind) == 2, "expiry_reclaimed_to_capacity");
        expect(kos_lease_remove(&leases, 201) == 0, "expiry_not_released_twice");
        expect(sem_value(&set, kind) == 2, "no_double_post_after_expiry");
    }

    printf("[test_resources] 5. deadlock_cycle_detected\n");
    {
        int held[2] = {0, 1};
        int wanted[2] = {1, 0};
        int cycle[KOS_RES_MAX];
        int cycle_len = 0;

        expect(kos_deadlock_find(held, wanted, 2, 2, cycle, &cycle_len) == 1,
               "deadlock_cycle_detected");
        expect(cycle_len == 2 && ((cycle[0] == 0 && cycle[1] == 1) ||
                                  (cycle[0] == 1 && cycle[1] == 0)),
               "cycle_contains_both_chefs");
        expect(kos_deadlock_victim(cycle, cycle_len) == 1,
               "victim_is_highest_chef_id");
    }
    {
        int held[2] = {0, -1};
        int wanted[2] = {1, 0};
        int cycle[KOS_RES_MAX];
        int cycle_len = 0;

        expect(kos_deadlock_find(held, wanted, 2, 2, cycle, &cycle_len) == 0,
               "wait_for_free_resource_is_not_cycle");
    }

    printf("[test_resources] 6. unlink 幂等\n");
    kos_resources_close(&set);
    kos_error_clear(&err);
    expect(kos_resources_unlink(&kitchen, KOS_INSTANCE_NONE, &err) == KOS_OK, "unlink_first");
    expect(kos_resources_unlink(&kitchen, KOS_INSTANCE_NONE, &err) == KOS_OK, "unlink_second_idempotent");
    kos_error_clear(&err);
    expect(kos_resources_attach(&kitchen, &set, KOS_INSTANCE_NONE, &err) == KOS_ERR_MISSING,
           "attach_after_unlink_missing");

    /* 另一个 kind 从未创建过，unlink 也不应失败。 */
    {
        kos_kitchen other_kitchen;

        make_kitchen(&other_kitchen, other);
        kos_error_clear(&err);
        expect(kos_resources_unlink(&other_kitchen, KOS_INSTANCE_NONE, &err) == KOS_OK,
               "unlink_absent_ok");
    }

    printf("[test_resources] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
