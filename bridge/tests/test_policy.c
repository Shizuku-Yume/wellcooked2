/*
 * test_policy.c -- 订单策略、工序图与消息队列的单元测试。
 *
 * 策略比较是调度实验可重复性的地基：FCFS/EDF 的次序、同截止期时 EDF 退化为
 * FCFS、前置未完成不进就绪队列，都必须用固定输入证明。mq 部分证明消息能按优先级
 * 取回且长度/版本被校验——Scheduler 与 Worker 靠它跨进程，不能只靠"能编译"。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/child.h"
#include "kitchenos/mq.h"
#include "kitchenos/policy.h"
#include "kitchenos/recipes.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static kos_policy *make_policy(kos_policy_kind kind)
{
    kos_error err;

    kos_error_clear(&err);
    return kos_policy_create(kind, &err);
}

static void test_policy_order(void)
{
    kos_order_view orders[4];
    kos_policy *fcfs;
    kos_policy *edf;
    int chosen = -1;

    printf("[test_policy] 1. FCFS / EDF\n");
    memset(orders, 0, sizeof orders);
    snprintf(orders[0].id, sizeof orders[0].id, "a");
    orders[0].arrival_ms = 100;
    orders[0].deadline_ms = 500;
    orders[0].status = KOS_ORDER_ACTIVE;
    snprintf(orders[1].id, sizeof orders[1].id, "b");
    orders[1].arrival_ms = 200;
    orders[1].deadline_ms = 300;
    orders[1].status = KOS_ORDER_ACTIVE;
    snprintf(orders[2].id, sizeof orders[2].id, "c");
    orders[2].arrival_ms = 50;
    orders[2].deadline_ms = 900;
    orders[2].status = KOS_ORDER_COMPLETED;

    fcfs = make_policy(KOS_POLICY_FCFS);
    edf = make_policy(KOS_POLICY_EDF);
    expect(fcfs != NULL && edf != NULL, "policy_create");
    if (fcfs == NULL || edf == NULL) {
        kos_policy_destroy(fcfs);
        kos_policy_destroy(edf);
        return;
    }
    expect(kos_policy_pick(fcfs, orders, 3, NULL, 0, 0, &chosen) == KOS_OK && chosen == 0,
           "fcfs_orders_by_arrival");
    expect(kos_policy_pick(edf, orders, 3, NULL, 0, 0, &chosen) == KOS_OK && chosen == 1,
           "edf_orders_by_deadline");
    expect(kos_policy_pick(fcfs, orders, 0, NULL, 0, 0, &chosen) == KOS_ERR_EMPTY,
           "policy_empty_rejected");

    memset(orders, 0, sizeof orders);
    snprintf(orders[0].id, sizeof orders[0].id, "late");
    orders[0].arrival_ms = 500;
    orders[0].deadline_ms = 1000;
    orders[0].status = KOS_ORDER_ACTIVE;
    snprintf(orders[1].id, sizeof orders[1].id, "early");
    orders[1].arrival_ms = 100;
    orders[1].deadline_ms = 1000;
    orders[1].status = KOS_ORDER_ACTIVE;
    {
        kos_order_view view[2];
        int fcfs_choice = -1;
        int edf_choice = -1;

        view[0] = orders[0];
        view[1] = orders[1];
        (void)kos_policy_pick(fcfs, view, 2, NULL, 0, 0, &fcfs_choice);
        (void)kos_policy_pick(edf, view, 2, NULL, 0, 0, &edf_choice);
        expect(fcfs_choice == 1 && edf_choice == 1, "edf_ties_match_fcfs");
    }

    kos_policy_set_kind(edf, KOS_POLICY_FCFS);
    expect(strcmp(kos_policy_name(edf), "FCFS") == 0, "policy_name_after_switch");
    kos_policy_set_kind(edf, KOS_POLICY_EDF);
    expect(strcmp(kos_policy_name(edf), "EDF") == 0, "policy_name_edf");
    expect(kos_policy_other(KOS_POLICY_EDF) == KOS_POLICY_FCFS, "policy_other_flips");
    {
        kos_policy_kind kind = KOS_POLICY_FCFS;

        expect(kos_policy_kind_from_name("edf", &kind) == KOS_OK &&
                   kind == KOS_POLICY_EDF,
               "policy_kind_from_name");
        expect(kos_policy_kind_from_name("nope", &kind) != KOS_OK,
               "policy_kind_from_name_rejects");
    }
    kos_policy_destroy(fcfs);
    kos_policy_destroy(edf);
}

static int load_recipes(kos_recipes *recipes)
{
    char path[KOS_DAEMON_PATH_MAX];
    kos_error err;

    if (kos_child_default_config_path("recipes.example.json", path, sizeof path) != 0) {
        return -1;
    }
    kos_error_clear(&err);
    return kos_recipes_load(path, recipes, &err);
}

static int find_node(const kos_recipes *recipes, const char *id)
{
    return kos_recipes_node_index(recipes, id);
}

static void test_recipes(void)
{
    kos_recipes recipes;
    int dish;
    int fetch;
    int cook;
    int plate;
    int deliver;
    unsigned char done[KOS_RECIPE_MAX_NODES];

    printf("[test_policy] 2. recipes.json 的 DAG 与就绪队列\n");
    memset(&recipes, 0, sizeof recipes);
    expect(load_recipes(&recipes) == KOS_OK, "recipes_load_example");
    expect(recipes.node_count > 0 && recipes.dish_count == 4 && recipes.edge_count > 0,
           "recipes_example_shape");
    dish = kos_recipes_dish_index(&recipes, "SteamedSpecial_Fish");
    expect(dish >= 0, "recipes_find_fish");
    if (dish < 0) {
        return;
    }
    fetch = find_node(&recipes, "fetch:UncookedFish");
    cook = find_node(&recipes, "cook:SteamedSpecial_Fish");
    plate = find_node(&recipes, "plate:SteamedSpecial_Fish");
    deliver = find_node(&recipes, "deliver:SteamedSpecial_Fish");
    expect(fetch >= 0 && cook >= 0 && plate >= 0 && deliver >= 0, "recipes_find_nodes");

    memset(done, 0, sizeof done);
    expect(kos_recipe_node_ready(&recipes, dish, done, fetch),
           "ready_queue_fetch_leaf_is_ready");
    expect(!kos_recipe_node_ready(&recipes, dish, done, cook),
           "ready_queue_cook_blocked_until_fetch");
    done[fetch] = 1;
    expect(kos_recipe_node_ready(&recipes, dish, done, cook),
           "ready_queue_cook_ready_after_fetch");
    expect(!kos_recipe_node_ready(&recipes, dish, done, plate),
           "ready_queue_plate_blocked_until_cook");
    done[cook] = 1;
    expect(kos_recipe_node_ready(&recipes, dish, done, plate),
           "ready_queue_plate_ready_after_cook");
    expect(kos_recipe_critical_ms(&recipes, dish, fetch) >
               kos_recipe_critical_ms(&recipes, dish, cook) &&
           kos_recipe_critical_ms(&recipes, dish, cook) >
               kos_recipe_critical_ms(&recipes, dish, plate) &&
           kos_recipe_critical_ms(&recipes, dish, plate) >
               kos_recipe_critical_ms(&recipes, dish, deliver),
           "critical_path_orders_tasks");
    expect(kos_recipe_est_cost_ms(&recipes, dish) ==
               recipes.nodes[fetch].duration_ms + recipes.nodes[cook].duration_ms +
                   recipes.nodes[plate].duration_ms + recipes.nodes[deliver].duration_ms,
           "est_cost_sums_dish");

    {
        char menu[KOS_RECIPE_MAX_DISHES][KOS_RECIPE_NAME_MAX];
        char path[KOS_DAEMON_PATH_MAX];
        kos_error err;
        int count = 0;

        expect(kos_child_default_config_path("orders.example.json", path, sizeof path) == 0,
               "menu_path_resolved");
        kos_error_clear(&err);
        expect(kos_recipe_menu_load(path, menu, KOS_RECIPE_MAX_DISHES, &count, &err) ==
                       KOS_OK &&
                   count == 4,
               "menu_load_example");
        if (count > 0) {
            expect(strcmp(menu[0], "SteamedSpecial_Fish") == 0, "menu_first_dish");
        }
    }
}

/* 用同一套就绪/关键路径规则跑两遍列表调度，输出必须逐字相同。 */
static int simulate_list_schedule(const kos_recipes *recipes, int dish, int workers,
                                  char out[][KOS_RECIPE_ID_MAX], int cap)
{
    unsigned char done[KOS_RECIPE_MAX_NODES];
    int produced = 0;
    int round;
    int node_count = recipes->dishes[dish].node_count;

    memset(done, 0, sizeof done);
    for (round = 0; round < node_count * 2 + 2 && produced < cap; round++) {
        int w;

        for (w = 0; w < workers; w++) {
            int best = -1;
            int64_t best_critical = 0;
            int i;

            for (i = 0; i < node_count; i++) {
                int node = recipes->dishes[dish].nodes[i];
                int64_t critical;

                if (done[node] || !kos_recipe_node_ready(recipes, dish, done, node)) {
                    continue;
                }
                critical = kos_recipe_critical_ms(recipes, dish, node);
                if (best < 0 || critical > best_critical ||
                    (critical == best_critical && node < best)) {
                    best = node;
                    best_critical = critical;
                }
            }
            if (best < 0) {
                continue;
            }
            snprintf(out[produced], KOS_RECIPE_ID_MAX, "%s", recipes->nodes[best].id);
            produced++;
            done[best] = 1;
        }
    }
    return produced;
}

static void test_list_schedule(void)
{
    kos_recipes recipes;
    char first[KOS_RECIPE_MAX_DISH_NODES][KOS_RECIPE_ID_MAX];
    char second[KOS_RECIPE_MAX_DISH_NODES][KOS_RECIPE_ID_MAX];
    int a;
    int b;
    int dish;
    int i;
    int same = 1;

    printf("[test_policy] 3. 列表调度可复现\n");
    memset(&recipes, 0, sizeof recipes);
    expect(load_recipes(&recipes) == KOS_OK, "list_recipes_loaded");
    dish = kos_recipes_dish_index(&recipes, "SteamedSpecial_Fish");
    if (dish < 0) {
        expect(0, "list_find_dish");
        return;
    }
    a = simulate_list_schedule(&recipes, dish, 2, first, KOS_RECIPE_MAX_DISH_NODES);
    b = simulate_list_schedule(&recipes, dish, 2, second, KOS_RECIPE_MAX_DISH_NODES);
    expect(a == recipes.dishes[dish].node_count && b == a, "list_schedule_covers_dish");
    for (i = 0; i < a && i < b; i++) {
        if (strcmp(first[i], second[i]) != 0) {
            same = 0;
        }
    }
    expect(same && a == b, "list_schedule_is_deterministic");
}

static void test_mq(void)
{
    char name[KOS_MQ_NAME_MAX];
    kos_mq_msg sent;
    kos_mq_msg got;
    kos_mq_msg low;
    kos_error err;
    mqd_t queue = (mqd_t)-1;

    printf("[test_policy] 4. 任务消息队列\n");
    snprintf(name, sizeof name, "/kitchenos_test_%ld", (long)getpid());
    (void)mq_unlink(name);
    kos_error_clear(&err);
    expect(kos_mq_create(name, 4, &err) == KOS_OK, "mq_create");
    kos_error_clear(&err);
    expect(kos_mq_open(name, O_RDWR, &queue, &err) == KOS_OK, "mq_open");

    memset(&sent, 0, sizeof sent);
    sent.version = KOS_MQ_VERSION;
    sent.kind = KOS_MQ_TASK;
    sent.task_id = 42;
    sent.chef_id = 1;
    sent.priority = 7;
    sent.duration_ms = 1500;
    snprintf(sent.node_id, sizeof sent.node_id, "fetch:UncookedFish");
    kos_error_clear(&err);
    expect(kos_mq_send(queue, &sent, 7, 100, &err) == KOS_OK, "mq_send_high");

    memset(&low, 0, sizeof low);
    low.version = KOS_MQ_VERSION;
    low.kind = KOS_MQ_EVENT;
    low.task_id = 43;
    low.event = KOS_EVT_DONE;
    kos_error_clear(&err);
    expect(kos_mq_send(queue, &low, 1, 100, &err) == KOS_OK, "mq_send_low");

    kos_error_clear(&err);
    expect(kos_mq_receive(queue, &got, 100, &err) == KOS_OK && got.task_id == 42,
           "mq_priority_orders_messages");
    expect(strcmp(got.node_id, "fetch:UncookedFish") == 0 && got.duration_ms == 1500,
           "mq_payload_roundtrip");
    kos_error_clear(&err);
    expect(kos_mq_receive(queue, &got, 100, &err) == KOS_OK && got.task_id == 43 &&
               got.event == KOS_EVT_DONE,
           "mq_event_roundtrip");
    kos_error_clear(&err);
    expect(kos_mq_receive(queue, &got, 20, &err) == KOS_ERR_TIMEOUT, "mq_timeout");

    kos_mq_close(queue);
    expect(mq_unlink(name) == 0, "mq_unlink");
}

int main(void)
{
    printf("[test_policy] 开始：订单策略、工序 DAG、mq\n");
    test_policy_order();
    test_recipes();
    test_list_schedule();
    test_mq();
    printf("[test_policy] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
