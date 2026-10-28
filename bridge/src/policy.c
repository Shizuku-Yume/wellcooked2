/*
 * policy.c -- FCFS / EDF / LLF / HRRN / SJF。见 policy.h 的接口约束。
 *
 * 所有比较都用 (主键, 次键, 下标) 的三段式，保证同一份输入永远挑出同一个订单；
 * 调度实验的可重复性依赖这一点，不能交给"数组刚好是有序的"这种巧合。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <wellcooked/policy.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct kos_policy {
    kos_policy_kind kind;
};

kos_policy *kos_policy_create(kos_policy_kind kind, kos_error *err)
{
    kos_policy *policy;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (kind != KOS_POLICY_FCFS && kind != KOS_POLICY_EDF &&
        kind != KOS_POLICY_LLF && kind != KOS_POLICY_HRRN && kind != KOS_POLICY_SJF) {
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

const char *kos_policy_kind_name(kos_policy_kind kind)
{
    switch (kind) {
    case KOS_POLICY_EDF:
        return "EDF";
    case KOS_POLICY_LLF:
        return "LLF";
    case KOS_POLICY_HRRN:
        return "HRRN";
    case KOS_POLICY_SJF:
        return "SJF";
    case KOS_POLICY_FCFS:
    default:
        return "FCFS";
    }
}

const char *kos_policy_name(const kos_policy *policy)
{
    return kos_policy_kind_name(kos_policy_kind_of(policy));
}

void kos_policy_set_kind(kos_policy *policy, kos_policy_kind kind)
{
    if (policy != NULL &&
        (kind == KOS_POLICY_FCFS || kind == KOS_POLICY_EDF || kind == KOS_POLICY_LLF ||
         kind == KOS_POLICY_HRRN || kind == KOS_POLICY_SJF)) {
        policy->kind = kind;
    }
}

kos_policy_kind kos_policy_other(kos_policy_kind kind)
{
    switch (kind) {
    case KOS_POLICY_FCFS:
        return KOS_POLICY_EDF;
    case KOS_POLICY_EDF:
        return KOS_POLICY_LLF;
    case KOS_POLICY_LLF:
        return KOS_POLICY_HRRN;
    case KOS_POLICY_HRRN:
        return KOS_POLICY_SJF;
    case KOS_POLICY_SJF:
    default:
        return KOS_POLICY_FCFS;
    }
}

int kos_policy_kind_from_name(const char *name, kos_policy_kind *out)
{
    static const struct {
        const char *name;
        kos_policy_kind kind;
    } names[] = {
        {"fcfs", KOS_POLICY_FCFS}, {"edf", KOS_POLICY_EDF}, {"llf", KOS_POLICY_LLF},
        {"hrrn", KOS_POLICY_HRRN}, {"sjf", KOS_POLICY_SJF},
    };
    size_t i;

    if (name == NULL || out == NULL) {
        return KOS_ERR_INVAL;
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcasecmp(name, names[i].name) == 0) {
            *out = names[i].kind;
            return KOS_OK;
        }
    }
    return KOS_ERR_INVAL;
}

const char *kos_task_policy_name(kos_task_policy policy)
{
    switch (policy) {
    case KOS_TASK_POLICY_FIFO:
        return "fifo";
    case KOS_TASK_POLICY_CP:
        return "cp";
    case KOS_TASK_POLICY_COST:
    default:
        return "cost";
    }
}

int kos_task_policy_from_name(const char *name, kos_task_policy *out)
{
    if (name == NULL || out == NULL) {
        return KOS_ERR_INVAL;
    }
    if (strcasecmp(name, "fifo") == 0) {
        *out = KOS_TASK_POLICY_FIFO;
    } else if (strcasecmp(name, "cp") == 0) {
        *out = KOS_TASK_POLICY_CP;
    } else if (strcasecmp(name, "cost") == 0) {
        *out = KOS_TASK_POLICY_COST;
    } else {
        return KOS_ERR_INVAL;
    }
    return KOS_OK;
}

int kos_policy_parse_request(const char *text, kos_policy_kind *order, int *has_order,
                             kos_task_policy *task, int *has_task)
{
    kos_policy_kind o = KOS_POLICY_FCFS;
    kos_task_policy t = KOS_TASK_POLICY_COST;
    int got_o = 0;
    int got_t = 0;
    const char *p = text;

    if (text == NULL || order == NULL || has_order == NULL || task == NULL ||
        has_task == NULL) {
        return KOS_ERR_INVAL;
    }
    for (;;) {
        char word[48];
        size_t n = 0;
        char *eq;

        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        while (p[n] != '\0' && p[n] != ' ' && p[n] != '\t' && p[n] != '\r' && p[n] != '\n') {
            n++;
        }
        if (n >= sizeof word) {
            return KOS_ERR_INVAL;
        }
        memcpy(word, p, n);
        word[n] = '\0';
        p += n;
        eq = strchr(word, '=');
        if (eq == NULL) {
            return KOS_ERR_INVAL;
        }
        *eq = '\0';
        if (strcasecmp(word, "order") == 0 && !got_o) {
            if (kos_policy_kind_from_name(eq + 1, &o) != KOS_OK) {
                return KOS_ERR_INVAL;
            }
            got_o = 1;
        } else if (strcasecmp(word, "task") == 0 && !got_t) {
            if (kos_task_policy_from_name(eq + 1, &t) != KOS_OK) {
                return KOS_ERR_INVAL;
            }
            got_t = 1;
        } else {
            return KOS_ERR_INVAL;
        }
    }
    if (!got_o && !got_t) {
        return KOS_ERR_INVAL;
    }
    *has_order = got_o;
    *has_task = got_t;
    if (got_o) {
        *order = o;
    }
    if (got_t) {
        *task = t;
    }
    return KOS_OK;
}

/*
 * FCFS 是五种策略共同的次级键：主键并列时都退化为"先到先做"，再按数组下标定序。
 * 这样同一份输入永远挑出同一个订单，调度实验可重复。
 */
static int compare_fcfs(const kos_order_view *a, const kos_order_view *b, int a_index,
                        int b_index)
{
    if (a->arrival_ms != b->arrival_ms) {
        return a->arrival_ms < b->arrival_ms ? -1 : 1;
    }
    if (a_index == b_index) {
        return 0;
    }
    return a_index < b_index ? -1 : 1;
}

/* 返回 <0 / 0 / >0，语义与 qsort 比较器一致。 */
static int compare_orders(kos_policy_kind kind, const kos_order_view *a,
                          const kos_order_view *b, int a_index, int b_index,
                          int64_t now_ms)
{
    switch (kind) {
    case KOS_POLICY_EDF:
        if (a->deadline_ms != b->deadline_ms) {
            return a->deadline_ms < b->deadline_ms ? -1 : 1;
        }
        break;
    case KOS_POLICY_SJF:
        /* 估计剩余工时短的先开；未开工单就是菜谱关键路径。 */
        if (a->est_cost_ms != b->est_cost_ms) {
            return a->est_cost_ms < b->est_cost_ms ? -1 : 1;
        }
        break;
    case KOS_POLICY_LLF: {
        /* 松弛 = 截止期 − 现在 − 剩余关键路径工时；越小越紧急。
         * __int128 只是比较用的宽中间量，接口仍是 int64 毫秒。 */
        __int128 slack_a = (__int128)a->deadline_ms - now_ms - a->est_cost_ms;
        __int128 slack_b = (__int128)b->deadline_ms - now_ms - b->est_cost_ms;

        if (slack_a != slack_b) {
            return slack_a < slack_b ? -1 : 1;
        }
        break;
    }
    case KOS_POLICY_HRRN: {
        /* 响应比 = (等待 + 估计工时) / 估计工时，用交叉相乘比较，避免浮点并列；
         * 乘积用 __int128，极端等待时间也不会回绕。 */
        int64_t est_a = a->est_cost_ms > 0 ? a->est_cost_ms : 1;
        int64_t est_b = b->est_cost_ms > 0 ? b->est_cost_ms : 1;
        int64_t wait_a = now_ms > a->arrival_ms ? now_ms - a->arrival_ms : 0;
        int64_t wait_b = now_ms > b->arrival_ms ? now_ms - b->arrival_ms : 0;
        __int128 left = ((__int128)wait_a + est_a) * est_b;
        __int128 right = ((__int128)wait_b + est_b) * est_a;

        if (left != right) {
            return left > right ? -1 : 1;
        }
        break;
    }
    case KOS_POLICY_FCFS:
    default:
        /* FCFS 的主键就是到达时间，必须排在奖励之前，否则就不再是 FCFS。 */
        if (a->arrival_ms != b->arrival_ms) {
            return a->arrival_ms < b->arrival_ms ? -1 : 1;
        }
        break;
    }
    /* 奖励是策略主键之外的次键：四种策略各自的语义（到达/截止/松弛/响应比）不变，
     * 主键分不出先后时先做分值高的单。G7：分值来自订单（插件上报或菜谱 dish score），
     * 不再固定 10；0 = 未知，不参与比较。 */
    if (a->reward != b->reward && a->reward > 0 && b->reward > 0) {
        return a->reward > b->reward ? -1 : 1;
    }
    return compare_fcfs(a, b, a_index, b_index);
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
    if (policy == NULL || orders == NULL || chosen == NULL || order_count <= 0) {
        return order_count == 0 ? KOS_ERR_EMPTY : KOS_ERR_INVAL;
    }
    kind = policy->kind;
    for (i = 0; i < order_count; i++) {
        if (orders[i].status != KOS_ORDER_ACTIVE) {
            continue;
        }
        if (best < 0 ||
            compare_orders(kind, &orders[i], &orders[best], i, best, now_ms) < 0) {
            best = i;
        }
    }
    if (best < 0) {
        return KOS_ERR_EMPTY;
    }
    *chosen = best;
    return KOS_OK;
}
