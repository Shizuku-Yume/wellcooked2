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

#include "wellcooked/child.h"
#include "wellcooked/json.h"
#include "wellcooked/mq.h"
#include "wellcooked/policy.h"
#include "wellcooked/recipes.h"

#include "scheduler_fixture.h"

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
    expect(kos_policy_other(KOS_POLICY_FCFS) == KOS_POLICY_EDF &&
               kos_policy_other(KOS_POLICY_EDF) == KOS_POLICY_LLF &&
               kos_policy_other(KOS_POLICY_LLF) == KOS_POLICY_HRRN &&
               kos_policy_other(KOS_POLICY_HRRN) == KOS_POLICY_SJF &&
               kos_policy_other(KOS_POLICY_SJF) == KOS_POLICY_FCFS,
           "policy_other_cycles_five");
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

/*
 * LLF / HRRN 用手算顺序证明，且每个用例里 EDF、FCFS 的选择与它不同：
 *   LLF  a: deadline 3000, est 100  → slack 1900 @now=1000（EDF 选 a）
 *        b: deadline 4000, est 3500 → slack -500（LLF 选 b）
 *        c: deadline 9000, est 1000 → slack 7000
 *   HRRN a: arrival 100, est 8000 → (900+8000)/8000 ≈ 1.11（EDF 选 a，deadline 2000）
 *        b: arrival 900, est 100  → (100+100)/100 = 2.0（HRRN 选 b）
 *        c: arrival 100, est 1000 → 1.9（FCFS 选 c）
 */
static void test_llf_hrrn(void)
{
    kos_order_view orders[3];
    kos_policy *llf;
    kos_policy *hrrn;
    kos_policy *edf;
    kos_policy *fcfs;
    int chosen = -1;

    printf("[test_policy] 2. LLF / HRRN\n");
    memset(orders, 0, sizeof orders);
    snprintf(orders[0].id, sizeof orders[0].id, "a");
    orders[0].arrival_ms = 100;
    orders[0].deadline_ms = 3000;
    orders[0].est_cost_ms = 100;
    orders[0].status = KOS_ORDER_ACTIVE;
    snprintf(orders[1].id, sizeof orders[1].id, "b");
    orders[1].arrival_ms = 200;
    orders[1].deadline_ms = 4000;
    orders[1].est_cost_ms = 3500;
    orders[1].status = KOS_ORDER_ACTIVE;
    snprintf(orders[2].id, sizeof orders[2].id, "c");
    orders[2].arrival_ms = 50;
    orders[2].deadline_ms = 9000;
    orders[2].est_cost_ms = 1000;
    orders[2].status = KOS_ORDER_ACTIVE;

    llf = make_policy(KOS_POLICY_LLF);
    hrrn = make_policy(KOS_POLICY_HRRN);
    edf = make_policy(KOS_POLICY_EDF);
    fcfs = make_policy(KOS_POLICY_FCFS);
    expect(llf != NULL && hrrn != NULL && edf != NULL && fcfs != NULL,
           "policy_create_llf_hrrn");
    if (llf == NULL || hrrn == NULL || edf == NULL || fcfs == NULL) {
        kos_policy_destroy(llf);
        kos_policy_destroy(hrrn);
        kos_policy_destroy(edf);
        kos_policy_destroy(fcfs);
        return;
    }
    expect(kos_policy_pick(llf, orders, 3, NULL, 0, 1000, &chosen) == KOS_OK &&
               chosen == 1,
           "llf_least_slack_first");
    expect(kos_policy_pick(edf, orders, 3, NULL, 0, 1000, &chosen) == KOS_OK &&
               chosen == 0,
           "llf_case_edf_differs");
    expect(strcmp(kos_policy_name(llf), "LLF") == 0, "policy_name_llf");

    memset(orders, 0, sizeof orders);
    snprintf(orders[0].id, sizeof orders[0].id, "a");
    orders[0].arrival_ms = 100;
    orders[0].deadline_ms = 2000;
    orders[0].est_cost_ms = 8000;
    orders[0].status = KOS_ORDER_ACTIVE;
    snprintf(orders[1].id, sizeof orders[1].id, "b");
    orders[1].arrival_ms = 900;
    orders[1].deadline_ms = 5000;
    orders[1].est_cost_ms = 100;
    orders[1].status = KOS_ORDER_ACTIVE;
    snprintf(orders[2].id, sizeof orders[2].id, "c");
    orders[2].arrival_ms = 50;
    orders[2].deadline_ms = 3000;
    orders[2].est_cost_ms = 1000;
    orders[2].status = KOS_ORDER_ACTIVE;

    expect(kos_policy_pick(hrrn, orders, 3, NULL, 0, 1000, &chosen) == KOS_OK &&
               chosen == 1,
           "hrrn_highest_response_ratio");
    expect(kos_policy_pick(edf, orders, 3, NULL, 0, 1000, &chosen) == KOS_OK &&
               chosen == 0,
           "hrrn_case_edf_differs");
    expect(kos_policy_pick(fcfs, orders, 3, NULL, 0, 1000, &chosen) == KOS_OK &&
               chosen == 2,
           "hrrn_case_fcfs_differs");
    expect(strcmp(kos_policy_name(hrrn), "HRRN") == 0, "policy_name_hrrn");

    /* 主键并列时必须退化成 FCFS：LLF 的松弛相同、HRRN 的响应比相同。 */
    memset(orders, 0, sizeof orders);
    snprintf(orders[0].id, sizeof orders[0].id, "late");
    orders[0].arrival_ms = 200;
    orders[0].deadline_ms = 5000;
    orders[0].est_cost_ms = 2000;
    orders[0].status = KOS_ORDER_ACTIVE;
    snprintf(orders[1].id, sizeof orders[1].id, "early");
    orders[1].arrival_ms = 100;
    orders[1].deadline_ms = 4000;
    orders[1].est_cost_ms = 1000;
    orders[1].status = KOS_ORDER_ACTIVE;
    expect(kos_policy_pick(llf, orders, 2, NULL, 0, 1000, &chosen) == KOS_OK &&
               chosen == 1,
           "llf_tie_falls_back_to_fcfs");

    memset(orders, 0, sizeof orders);
    snprintf(orders[0].id, sizeof orders[0].id, "late");
    orders[0].arrival_ms = 500;
    orders[0].deadline_ms = 9000;
    orders[0].est_cost_ms = 500;
    orders[0].status = KOS_ORDER_ACTIVE;
    snprintf(orders[1].id, sizeof orders[1].id, "early");
    orders[1].arrival_ms = 0;
    orders[1].deadline_ms = 9000;
    orders[1].est_cost_ms = 1000;
    orders[1].status = KOS_ORDER_ACTIVE;
    expect(kos_policy_pick(hrrn, orders, 2, NULL, 0, 1000, &chosen) == KOS_OK &&
               chosen == 1,
           "hrrn_tie_falls_back_to_fcfs");

    {
        kos_policy_kind kind = KOS_POLICY_FCFS;

        expect(kos_policy_kind_from_name("llf", &kind) == KOS_OK &&
                   kind == KOS_POLICY_LLF,
               "policy_kind_from_name_llf");
        expect(kos_policy_kind_from_name("HRRN", &kind) == KOS_OK &&
                   kind == KOS_POLICY_HRRN,
               "policy_kind_from_name_hrrn");
    }
    kos_policy_destroy(llf);
    kos_policy_destroy(hrrn);
    kos_policy_destroy(edf);
    kos_policy_destroy(fcfs);
}

/*
 * SJF：估计剩余工时最短的先开，与 FCFS/EDF 的选择都不同；工时并列退化成 FCFS。
 *   a: arrival 100, deadline 2000, est 4000（FCFS、EDF 都选 a）
 *   b: arrival 300, deadline 6000, est 500 （SJF 选 b）
 *   c: arrival 200, deadline 5000, est 1500
 */
static void test_sjf(void)
{
    kos_order_view orders[3];
    kos_policy *sjf;
    kos_policy *fcfs;
    kos_policy *edf;
    kos_policy_kind kind = KOS_POLICY_FCFS;
    int chosen = -1;

    printf("[test_policy] 2b. SJF\n");
    memset(orders, 0, sizeof orders);
    snprintf(orders[0].id, sizeof orders[0].id, "a");
    orders[0].arrival_ms = 100;
    orders[0].deadline_ms = 2000;
    orders[0].est_cost_ms = 4000;
    orders[0].status = KOS_ORDER_ACTIVE;
    snprintf(orders[1].id, sizeof orders[1].id, "b");
    orders[1].arrival_ms = 300;
    orders[1].deadline_ms = 6000;
    orders[1].est_cost_ms = 500;
    orders[1].status = KOS_ORDER_ACTIVE;
    snprintf(orders[2].id, sizeof orders[2].id, "c");
    orders[2].arrival_ms = 200;
    orders[2].deadline_ms = 5000;
    orders[2].est_cost_ms = 1500;
    orders[2].status = KOS_ORDER_ACTIVE;

    sjf = make_policy(KOS_POLICY_SJF);
    fcfs = make_policy(KOS_POLICY_FCFS);
    edf = make_policy(KOS_POLICY_EDF);
    expect(sjf != NULL && fcfs != NULL && edf != NULL, "policy_create_sjf");
    if (sjf == NULL || fcfs == NULL || edf == NULL) {
        kos_policy_destroy(sjf);
        kos_policy_destroy(fcfs);
        kos_policy_destroy(edf);
        return;
    }
    expect(kos_policy_pick(sjf, orders, 3, NULL, 0, 1000, &chosen) == KOS_OK && chosen == 1,
           "sjf_shortest_estimate_first");
    expect(kos_policy_pick(fcfs, orders, 3, NULL, 0, 1000, &chosen) == KOS_OK && chosen == 0,
           "sjf_case_fcfs_differs");
    expect(kos_policy_pick(edf, orders, 3, NULL, 0, 1000, &chosen) == KOS_OK && chosen == 0,
           "sjf_case_edf_differs");

    orders[1].status = KOS_ORDER_COMPLETED;
    expect(kos_policy_pick(sjf, orders, 3, NULL, 0, 1000, &chosen) == KOS_OK && chosen == 2,
           "sjf_skips_completed");

    orders[1].status = KOS_ORDER_ACTIVE;
    orders[1].est_cost_ms = 1500;
    expect(kos_policy_pick(sjf, orders, 3, NULL, 0, 1000, &chosen) == KOS_OK && chosen == 2,
           "sjf_tie_falls_back_to_fcfs");

    expect(strcmp(kos_policy_name(sjf), "SJF") == 0, "policy_name_sjf");
    expect(kos_policy_kind_from_name("Sjf", &kind) == KOS_OK && kind == KOS_POLICY_SJF,
           "policy_kind_from_name_sjf");
    kos_policy_destroy(sjf);
    kos_policy_destroy(fcfs);
    kos_policy_destroy(edf);
}

static void test_task_policy_and_request(void)
{
    kos_task_policy task = KOS_TASK_POLICY_COST;
    kos_policy_kind order = KOS_POLICY_FCFS;
    int has_order = -1;
    int has_task = -1;

    printf("[test_policy] 2c. task policy / strategy.request\n");
    expect(kos_task_policy_from_name("FIFO", &task) == KOS_OK && task == KOS_TASK_POLICY_FIFO &&
               kos_task_policy_from_name("cp", &task) == KOS_OK && task == KOS_TASK_POLICY_CP &&
               kos_task_policy_from_name("cost", &task) == KOS_OK &&
               task == KOS_TASK_POLICY_COST,
           "task_policy_from_name");
    expect(kos_task_policy_from_name("greedy", &task) != KOS_OK, "task_policy_rejects_unknown");
    expect(strcmp(kos_task_policy_name(KOS_TASK_POLICY_FIFO), "fifo") == 0 &&
               strcmp(kos_task_policy_name(KOS_TASK_POLICY_CP), "cp") == 0 &&
               strcmp(kos_task_policy_name((kos_task_policy)99), "cost") == 0,
           "task_policy_names");

    expect(kos_policy_parse_request("task=cp  order=SJF\n", &order, &has_order, &task,
                                    &has_task) == KOS_OK &&
               has_order == 1 && order == KOS_POLICY_SJF && has_task == 1 &&
               task == KOS_TASK_POLICY_CP,
           "request_both_keys_any_order");

    order = KOS_POLICY_LLF;
    expect(kos_policy_parse_request("task=fifo", &order, &has_order, &task, &has_task) ==
                   KOS_OK &&
               has_order == 0 && order == KOS_POLICY_LLF && has_task == 1 &&
               task == KOS_TASK_POLICY_FIFO,
           "request_task_only_keeps_order");

    task = KOS_TASK_POLICY_CP;
    expect(kos_policy_parse_request("order=hrrn", &order, &has_order, &task, &has_task) ==
                   KOS_OK &&
               has_order == 1 && order == KOS_POLICY_HRRN && has_task == 0 &&
               task == KOS_TASK_POLICY_CP,
           "request_order_only_keeps_task");

    order = KOS_POLICY_LLF;
    expect(kos_policy_parse_request("order=bogus task=cp", &order, &has_order, &task,
                                    &has_task) != KOS_OK &&
               order == KOS_POLICY_LLF,
           "request_bad_order_rejected_untouched");
    expect(kos_policy_parse_request("", &order, &has_order, &task, &has_task) != KOS_OK,
           "request_empty_rejected");
    expect(kos_policy_parse_request("sjf", &order, &has_order, &task, &has_task) != KOS_OK,
           "request_bare_word_rejected");
    expect(kos_policy_parse_request("order=sjf order=fcfs", &order, &has_order, &task,
                                    &has_task) != KOS_OK,
           "request_duplicate_key_rejected");
    expect(kos_policy_parse_request("speed=fast", &order, &has_order, &task, &has_task) !=
               KOS_OK,
           "request_unknown_key_rejected");
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
    snprintf(name, sizeof name, "/wellcooked_test_%ld", (long)getpid());
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


/*
 * SIGHUP 的轮换在真实 Scheduler 上验证：以 llf 启动，一次 HUP 必须切到 hrrn，
 * 之后派工决策里的 policy 字段跟着变。订单/工序来自临时地图副本，资源信号量名
 * 带 pid 后缀，不会碰到别的测试。
 */
static void test_sighup_switches_policy(void)
{
    ksfx_run run;
    char decisions[KSFX_PATH_MAX];
    int saw_llf_dispatch = 0;
    int saw_hrrn_switch = 0;
    FILE *file;
    char *line = NULL;
    size_t cap = 0;

    printf("[test_policy] 5. SIGHUP 切到 LLF / HRRN\n");
    if (ksfx_init(&run) != 0) {
        expect(0, "sighup_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    if (ksfx_spawn(&run, "llf") <= 0) {
        expect(0, "sighup_scheduler_spawned");
        ksfx_cleanup(&run);
        return;
    }
    if (ksfx_wait_decisions(&run, "\"dispatch\"", 3000, decisions,
                            sizeof decisions) != 0) {
        ksfx_dump_logs(&run);
        expect(0, "sighup_dispatch_written");
        (void)ksfx_stop(&run);
        ksfx_cleanup(&run);
        return;
    }
    expect(1, "sighup_dispatch_written");
    (void)kill(run.pid, SIGHUP);
    expect(ksfx_wait_decisions(&run, "HRRN", 2000, decisions, sizeof decisions) == 0,
           "sighup_switches_to_hrrn");
    expect(ksfx_stop(&run) == 0, "sighup_scheduler_stopped");

    file = fopen(decisions, "r");
    if (file == NULL) {
        expect(0, "sighup_decisions_readable");
        ksfx_cleanup(&run);
        return;
    }
    while (getline(&line, &cap, file) >= 0) {
        json_value *root = NULL;
        json_error jerr;
        const char *kind = NULL;
        const char *policy = NULL;
        const char *to = NULL;

        json_error_init(&jerr);
        if (line[0] == '\n' ||
            json_parse_ex(line, strlen(line), &root, &jerr) != JSON_OK ||
            json_typeof(root) != JSON_OBJECT) {
            json_free(root);
            continue;
        }
        (void)json_object_get_string(root, "kind", &kind, NULL);
        (void)json_object_get_string(root, "policy", &policy, NULL);
        (void)json_object_get_string(root, "to", &to, NULL);
        if (kind != NULL && strcmp(kind, "dispatch") == 0 && policy != NULL &&
            strcmp(policy, "LLF") == 0) {
            saw_llf_dispatch = 1;
        }
        if (kind != NULL && strcmp(kind, "strategy") == 0 && to != NULL &&
            strcmp(to, "HRRN") == 0) {
            saw_hrrn_switch = 1;
        }
        json_free(root);
    }
    free(line);
    fclose(file);
    expect(saw_llf_dispatch, "sighup_started_on_llf");
    expect(saw_hrrn_switch, "sighup_decision_records_hrrn");
    ksfx_cleanup(&run);
}

/*
 * G7：每单分值进入四种策略。主键（到达/截止/松弛/响应比）仍是第一判据；主键
 * 分不出先后时，80 分的单先于 20 分的单。0 = 未知，不参与奖励比较。
 */
static void test_reward_tiebreak(void)
{
    static const kos_policy_kind kinds[] = {KOS_POLICY_FCFS, KOS_POLICY_EDF,
                                            KOS_POLICY_LLF, KOS_POLICY_HRRN};
    kos_order_view orders[2];
    int k;

    printf("[test_policy] 3. 奖励作为策略次键（G7）\n");
    memset(orders, 0, sizeof orders);
    snprintf(orders[0].id, sizeof orders[0].id, "sushi");
    orders[0].arrival_ms = 100;
    orders[0].deadline_ms = 5000;
    orders[0].est_cost_ms = 1000;
    orders[0].reward = 20;
    orders[0].status = KOS_ORDER_ACTIVE;
    snprintf(orders[1].id, sizeof orders[1].id, "burrito");
    orders[1].arrival_ms = 100;
    orders[1].deadline_ms = 5000;
    orders[1].est_cost_ms = 1000;
    orders[1].reward = 80;
    orders[1].status = KOS_ORDER_ACTIVE;
    for (k = 0; k < (int)(sizeof kinds / sizeof kinds[0]); k++) {
        kos_policy *policy = make_policy(kinds[k]);
        int chosen = -1;
        char label[64];

        expect(policy != NULL &&
                   kos_policy_pick(policy, orders, 2, NULL, 0, 1000, &chosen) == KOS_OK &&
                   chosen == 1,
               (snprintf(label, sizeof label, "%s_prefers_higher_reward",
                         kos_policy_kind_name(kinds[k])),
                label));
        kos_policy_destroy(policy);
    }

    /* 主键优先：早到 1 ms 的低分单仍然先做（FCFS 语义不能被奖励改掉）。 */
    orders[0].arrival_ms = 99;
    {
        kos_policy *fcfs = make_policy(KOS_POLICY_FCFS);
        int chosen = -1;

        expect(fcfs != NULL &&
                   kos_policy_pick(fcfs, orders, 2, NULL, 0, 1000, &chosen) == KOS_OK &&
                   chosen == 0,
               "fcfs_arrival_still_beats_reward");
        kos_policy_destroy(fcfs);
    }
    /* EDF 同理：更早的截止期优先。 */
    orders[0].arrival_ms = 100;
    orders[1].deadline_ms = 4000;
    {
        kos_policy *edf = make_policy(KOS_POLICY_EDF);
        int chosen = -1;

        expect(edf != NULL &&
                   kos_policy_pick(edf, orders, 2, NULL, 0, 1000, &chosen) == KOS_OK &&
                   chosen == 1,
               "edf_deadline_still_beats_reward");
        kos_policy_destroy(edf);
    }
    /* 奖励未知（0）不改变原有 FCFS 次序。 */
    orders[1].reward = 0;
    {
        kos_policy *fcfs = make_policy(KOS_POLICY_FCFS);
        int chosen = -1;

        expect(fcfs != NULL &&
                   kos_policy_pick(fcfs, orders, 2, NULL, 0, 1000, &chosen) == KOS_OK &&
                   chosen == 0,
               "unknown_reward_keeps_fcfs_order");
        kos_policy_destroy(fcfs);
    }
}

int main(void)
{
    printf("[test_policy] 开始：订单策略、工序 DAG、mq\n");
    test_policy_order();
    test_llf_hrrn();
    test_sjf();
    test_task_policy_and_request();
    test_recipes();
    test_list_schedule();
    test_reward_tiebreak();
    test_mq();
    test_sighup_switches_policy();
    printf("[test_policy] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
