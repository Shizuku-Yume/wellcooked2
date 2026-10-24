/*
 * policy.c -- FCFS / EDF。见 policy.h 的接口约束。
 *
 * 所有比较都用 (主键, 次键, 下标) 的三段式，保证同一份输入永远挑出同一个订单；
 * 调度实验的可重复性依赖这一点，不能交给"数组刚好是有序的"这种巧合。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/policy.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kos_policy {
    kos_policy_kind kind;
};

kos_policy *kos_policy_create(kos_policy_kind kind, kos_error *err)
{
    kos_policy *policy;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (kind != KOS_POLICY_FCFS && kind != KOS_POLICY_EDF) {
        if (err != NULL) {
            err->code = KOS_ERR_INVAL;
            snprintf(err->context, sizeof err->context, "未知策略 %d", (int)kind);
        }
        return NULL;
    }
    policy = calloc(1, sizeof *policy);
    if (policy == NULL) {
        if (err != NULL) {
            err->code = KOS_ERR_NOMEM;
            snprintf(err->context, sizeof err->context, "calloc(kos_policy)");
        }
        return NULL;
    }
    policy->kind = kind;
    return policy;
}

void kos_policy_destroy(kos_policy *policy)
{
    free(policy);
}

kos_policy_kind kos_policy_kind_of(const kos_policy *policy)
{
    return policy != NULL ? policy->kind : KOS_POLICY_FCFS;
}

const char *kos_policy_name(const kos_policy *policy)
{
    return kos_policy_kind_of(policy) == KOS_POLICY_EDF ? "EDF" : "FCFS";
}

void kos_policy_set_kind(kos_policy *policy, kos_policy_kind kind)
{
    if (policy != NULL && (kind == KOS_POLICY_FCFS || kind == KOS_POLICY_EDF)) {
        policy->kind = kind;
    }
}

kos_policy_kind kos_policy_other(kos_policy_kind kind)
{
    return kind == KOS_POLICY_FCFS ? KOS_POLICY_EDF : KOS_POLICY_FCFS;
}

int kos_policy_kind_from_name(const char *name, kos_policy_kind *out)
{
    if (name == NULL || out == NULL) {
        return KOS_ERR_INVAL;
    }
    if (strcmp(name, "fcfs") == 0 || strcmp(name, "FCFS") == 0) {
        *out = KOS_POLICY_FCFS;
        return KOS_OK;
    }
    if (strcmp(name, "edf") == 0 || strcmp(name, "EDF") == 0) {
        *out = KOS_POLICY_EDF;
        return KOS_OK;
    }
    return KOS_ERR_INVAL;
}

/* 返回 <0 / 0 / >0，语义与 qsort 比较器一致。 */
static int compare_orders(kos_policy_kind kind, const kos_order_view *a,
                          const kos_order_view *b, int a_index, int b_index)
{
    if (kind == KOS_POLICY_EDF) {
        if (a->deadline_ms != b->deadline_ms) {
            return a->deadline_ms < b->deadline_ms ? -1 : 1;
        }
        /* 截止期相同：EDF 退化为 FCFS，这样"时限一致时两者顺序相同"是可证明的。 */
        if (a->arrival_ms != b->arrival_ms) {
            return a->arrival_ms < b->arrival_ms ? -1 : 1;
        }
    } else {
        if (a->arrival_ms != b->arrival_ms) {
            return a->arrival_ms < b->arrival_ms ? -1 : 1;
        }
    }
    if (a_index == b_index) {
        return 0;
    }
    return a_index < b_index ? -1 : 1;
}

int kos_policy_pick(const kos_policy *policy, const kos_order_view *orders, int order_count,
                    const kos_resource_view *resources, int resource_count,
                    int64_t now_ms, int *chosen)
{
    kos_policy_kind kind;
    int best = -1;
    int i;

    (void)resources;
    (void)resource_count;
    (void)now_ms;
    if (policy == NULL || orders == NULL || chosen == NULL || order_count <= 0) {
        return order_count == 0 ? KOS_ERR_EMPTY : KOS_ERR_INVAL;
    }
    kind = policy->kind;
    for (i = 0; i < order_count; i++) {
        if (orders[i].status != KOS_ORDER_ACTIVE) {
            continue;
        }
        if (best < 0 || compare_orders(kind, &orders[i], &orders[best], i, best) < 0) {
            best = i;
        }
    }
    if (best < 0) {
        return KOS_ERR_EMPTY;
    }
    *chosen = best;
    return KOS_OK;
}
