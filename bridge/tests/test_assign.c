/*
 * test_assign.c -- 派工代价、匈牙利配对、锅链、洗盘判定与决策日志（W7）。
 *
 * 配对用例都把代价放进 travel_s（w_d = 1，其余权重 0），于是矩阵每个元素就是手算
 * 的代价；2 厨师 × 3 工序的最优解另用全排列暴力核对。决策日志的检查启动真实的
 * kitchenos-scheduler（临时地图、kind 带 pid 后缀，资源信号量不撞名），跑几秒后
 * 逐行解析 decisions.jsonl：dispatch 行有 w_d/w_t/w_c/w_u/w_z 且 cost = 五项之和，
 * 三次 SIGHUP 依次切到 EDF → LLF → HRRN。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/assign.h"
#include "kitchenos/child.h"
#include "kitchenos/json.h"
#include "kitchenos/recipes.h"
#include "kitchenos/shm.h"
#include "kitchenos/stand.h"

#include "scheduler_fixture.h"

#include <math.h>
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

static int close_enough(double a, double b)
{
    return fabs(a - b) <= 1e-6 * (1.0 + fabs(a) + fabs(b));
}

static kos_assign_weights travel_only_weights(void)
{
    kos_assign_weights weights;

    memset(&weights, 0, sizeof weights);
    weights.w_d = 1.0;
    return weights;
}

static kos_assign_terms travel_term(double travel_s)
{
    kos_assign_terms terms;

    memset(&terms, 0, sizeof terms);
    terms.travel_s = travel_s;
    return terms;
}

static void test_cost(void)
{
    kos_assign_weights weights = {2.0, 0.5, 3.0, 4.0, 1.0};
    kos_assign_terms terms;
    kos_assign_components parts;

    printf("[test_assign] 1. 代价公式与分量\n");
    memset(&terms, 0, sizeof terms);
    terms.travel_s = 10.0;
    terms.work_s = 4.0;
    terms.critical_s = 5.0;
    terms.urgency = 0.25;
    terms.zone_penalty = 1.0;
    /* 2*10 + 0.5*4 − 3*5 − 4*0.25 + 1*1 = 20 + 2 − 15 − 1 + 1 = 7 */
    expect(close_enough(kos_assign_cost(&weights, &terms), 7.0), "cost_formula");
    kos_assign_breakdown(&weights, &terms, &parts);
    expect(close_enough(parts.w_d, 20.0) && close_enough(parts.w_t, 2.0) &&
               close_enough(parts.w_c, -15.0) && close_enough(parts.w_u, -1.0) &&
               close_enough(parts.w_z, 1.0) && close_enough(parts.cost, 7.0),
           "cost_components_signed");
    terms.critical_x2 = 1;
    /* 锅链：−3*2*5 比原来再少 15 → −8 */
    expect(close_enough(kos_assign_cost(&weights, &terms), -8.0),
           "cost_pot_chain_doubles_wc");

    kos_assign_weights defaults;

    kos_assign_weights_defaults(&defaults);
    expect(close_enough(defaults.w_d, 1.0) && close_enough(defaults.w_t, 0.2) &&
               close_enough(defaults.w_c, 0.5) && close_enough(defaults.w_u, 2.0) &&
               close_enough(defaults.w_z, 1.0),
           "weights_defaults");
    expect(close_enough(kos_assign_travel_s(10, 107), 1.07), "travel_cells_to_seconds");
    expect(close_enough(kos_assign_travel_s(-3, 107), 0.0), "travel_negative_is_zero");
}

static void test_hungarian_two_by_three(void)
{
    kos_assign_weights weights = travel_only_weights();
    kos_assign_terms terms[2 * 3];
    int task_of_chef[2];
    kos_error err;
    /* 手算矩阵：
     *           t0   t1   t2
     *   chef0   10    5    8
     *   chef1    6    9    7
     * 六种配对：(0,0)+(1,1)=19  (0,0)+(1,2)=17  (0,1)+(1,0)=11
     *           (0,1)+(1,2)=12  (0,2)+(1,0)=14  (0,2)+(1,1)=17
     * 最小 11：chef0→t1, chef1→t0。 */
    static const double costs[2][3] = {{10.0, 5.0, 8.0}, {6.0, 9.0, 7.0}};
    int expected[2] = {1, 0};
    int i;

    printf("[test_assign] 2. 2 厨师 × 3 工序的匈牙利配对\n");
    for (i = 0; i < 6; i++) {
        terms[i] = travel_term(costs[i / 3][i % 3]);
    }
    kos_error_clear(&err);
    expect(kos_assign_solve(&weights, 2, 3, terms, task_of_chef, &err) == KOS_OK,
           "solve_two_by_three");
    expect(task_of_chef[0] == expected[0] && task_of_chef[1] == expected[1],
           "solve_matches_hand_computation");

    /* 暴力确认这确实是总代价最小的配对，而不是恰好三个里挑两个。 */
    {
        int a;
        int b;
        double best = 1e300;

        for (a = 0; a < 3; a++) {
            for (b = 0; b < 3; b++) {
                double total;

                if (a == b) {
                    continue;
                }
                total = costs[0][a] + costs[1][b];
                if (total < best) {
                    best = total;
                }
            }
        }
        expect(close_enough(costs[0][task_of_chef[0]] + costs[1][task_of_chef[1]],
                            best),
               "solve_total_is_bruteforce_minimum");
    }
}

static void test_hungarian_more_chefs(void)
{
    kos_assign_weights weights = travel_only_weights();
    kos_assign_terms terms[3 * 2];
    int task_of_chef[3];
    kos_error err;
    /* 3 厨师 × 2 工序：两道工序都必须派出去，第三个厨师空闲。
     *   chef0: 4, 9    chef1: 5, 2    chef2: 8, 7
     * 最小 4+2 = 6：chef0→t0, chef1→t1。 */
    static const double costs[3][2] = {{4.0, 9.0}, {5.0, 2.0}, {8.0, 7.0}};
    int i;

    printf("[test_assign] 3. 厨师多于工序\n");
    for (i = 0; i < 6; i++) {
        terms[i] = travel_term(costs[i / 2][i % 2]);
    }
    kos_error_clear(&err);
    expect(kos_assign_solve(&weights, 3, 2, terms, task_of_chef, &err) == KOS_OK,
           "solve_three_by_two");
    expect(task_of_chef[0] == 0 && task_of_chef[1] == 1 && task_of_chef[2] == -1,
           "solve_assigns_every_task_and_leaves_one_idle");
}

static void test_hungarian_unreachable(void)
{
    kos_assign_weights weights = travel_only_weights();
    kos_assign_terms terms[2 * 2];
    int task_of_chef[2];
    kos_error err;

    printf("[test_assign] 4. 不可达配对不派\n");
    /* chef0 两道都能到：t0=1, t1=100；chef1 只能到 t1=2。最优是 1 + 2。 */
    terms[0] = travel_term(1.0);
    terms[1] = travel_term(100.0);
    terms[2] = travel_term(0.0);
    terms[2].unreachable = 1;
    terms[3] = travel_term(2.0);
    kos_error_clear(&err);
    expect(kos_assign_solve(&weights, 2, 2, terms, task_of_chef, &err) == KOS_OK,
           "unreachable_solve");
    expect(task_of_chef[0] == 0 && task_of_chef[1] == 1,
           "unreachable_pair_not_chosen");
    expect(close_enough(kos_assign_cost(&weights, &terms[2]), KOS_ASSIGN_FORBIDDEN),
           "unreachable_cost_is_forbidden");

    /* chef1 两道都到不了：它必须空着，chef0 只接一道（没有第二个人能接）。 */
    terms[2].unreachable = 1;
    terms[3].unreachable = 1;
    terms[0] = travel_term(5.0);
    terms[1] = travel_term(6.0);
    kos_error_clear(&err);
    expect(kos_assign_solve(&weights, 2, 2, terms, task_of_chef, &err) == KOS_OK,
           "unreachable_solve_all_forbidden");
    expect(task_of_chef[0] == 0 && task_of_chef[1] == -1,
           "unreachable_chef_left_idle");
}

static void test_hungarian_ties(void)
{
    kos_assign_weights weights = travel_only_weights();
    kos_assign_terms terms[2 * 2];
    int task_of_chef[2];
    kos_error err;
    int i;

    printf("[test_assign] 5. 并列时取下标小的配对\n");
    for (i = 0; i < 4; i++) {
        terms[i] = travel_term(1.0);
    }
    kos_error_clear(&err);
    expect(kos_assign_solve(&weights, 2, 2, terms, task_of_chef, &err) == KOS_OK &&
               task_of_chef[0] == 0 && task_of_chef[1] == 1,
           "solve_ties_are_deterministic");

    kos_error_clear(&err);
    expect(kos_assign_solve(&weights, 0, 2, terms, task_of_chef, &err) == KOS_OK,
           "solve_no_chefs_ok");
    kos_error_clear(&err);
    expect(kos_assign_solve(&weights, 2, 0, terms, task_of_chef, &err) == KOS_OK &&
               task_of_chef[0] == -1 && task_of_chef[1] == -1,
           "solve_no_tasks_idle");
}

static int load_example_recipes(kos_recipes *recipes)
{
    char path[KOS_DAEMON_PATH_MAX];
    kos_error err;

    if (kos_child_default_config_path("recipes.example.json", path, sizeof path) != 0) {
        return -1;
    }
    kos_error_clear(&err);
    return kos_recipes_load(path, recipes, &err);
}

static void test_pot_chain(void)
{
    kos_recipes recipes;
    unsigned char chain[KOS_RECIPE_MAX_NODES];
    int cook_fish;
    int fetch_fish;
    int fetch_carrot;
    int deliver_fish;

    printf("[test_assign] 6. 锅链标记\n");
    memset(&recipes, 0, sizeof recipes);
    if (load_example_recipes(&recipes) != KOS_OK) {
        expect(0, "pot_chain_recipes_loaded");
        return;
    }
    memset(chain, 0, sizeof chain);
    kos_assign_pot_chain(&recipes, chain);
    cook_fish = kos_recipes_node_index(&recipes, "cook:SteamedSpecial_Fish");
    fetch_fish = kos_recipes_node_index(&recipes, "fetch:UncookedFish");
    fetch_carrot = kos_recipes_node_index(&recipes, "fetch:Carrot");
    deliver_fish = kos_recipes_node_index(&recipes, "deliver:SteamedSpecial_Fish");
    expect(cook_fish >= 0 && fetch_fish >= 0 && fetch_carrot >= 0 &&
               deliver_fish >= 0,
           "pot_chain_nodes_found");
    expect(chain[cook_fish] == 1, "pot_chain_cook_marked");
    expect(chain[fetch_fish] == 1, "pot_chain_direct_prerequisite_marked");
    expect(chain[fetch_carrot] == 1, "pot_chain_transitive_prerequisite_marked");
    expect(chain[deliver_fish] == 0, "pot_chain_deliver_not_marked");
}

static void test_wash_rule(void)
{
    printf("[test_assign] 7. 洗盘判定\n");
    expect(kos_assign_wash_needed(0, 1), "wash_needed_no_clean_has_dirty");
    expect(kos_assign_wash_needed(1, 2), "wash_needed_one_clean_has_dirty");
    expect(!kos_assign_wash_needed(2, 3), "wash_not_needed_clean_surplus");
    expect(!kos_assign_wash_needed(0, 0), "wash_not_needed_no_dirty");
}

static void test_config(void)
{
    char path[128];
    char kitchen[KOS_DAEMON_PATH_MAX];
    kos_assign_config cfg;
    kos_error err;
    static const char text[] =
        "{\"weights\":{\"w_d\":3.5,\"w_t\":0.7},\"cell_ms\":250,\"wash_ms\":4200}\n";
    static const char broken[] = "{\"weights\":\n";

    printf("[test_assign] 8. 调度配置\n");
    snprintf(path, sizeof path, "/tmp/kos-assign-cfg-%ld.json", (long)getpid());
    kos_error_clear(&err);
    expect(kos_assign_config_load(path, &cfg, &err) == KOS_OK && !cfg.from_file &&
               close_enough(cfg.weights.w_d, 1.0) &&
               close_enough(cfg.cell_ms, KOS_ASSIGN_CELL_MS_DEFAULT),
           "config_missing_uses_defaults");
    expect(ksfx_write_file(path, text, sizeof text - 1) == 0, "config_fixture_written");
    kos_error_clear(&err);
    expect(kos_assign_config_load(path, &cfg, &err) == KOS_OK && cfg.from_file &&
               close_enough(cfg.weights.w_d, 3.5) &&
               close_enough(cfg.weights.w_t, 0.7) &&
               close_enough(cfg.weights.w_c, 0.5) && close_enough(cfg.cell_ms, 250) &&
               close_enough(cfg.wash_ms, 4200),
           "config_file_overrides_and_keeps_defaults");
    expect(ksfx_write_file(path, broken, sizeof broken - 1) == 0, "config_broken_written");
    kos_error_clear(&err);
    expect(kos_assign_config_load(path, &cfg, &err) != KOS_OK, "config_broken_rejected");
    {
        static const char bad_weights[] = "{\"weights\":[]}\n";

        expect(ksfx_write_file(path, bad_weights, sizeof bad_weights - 1) == 0,
               "config_bad_weights_written");
        kos_error_clear(&err);
        expect(kos_assign_config_load(path, &cfg, &err) != KOS_OK,
               "config_non_object_weights_rejected");
    }
    (void)unlink(path);

    if (kos_child_default_config_path("maps/s_wellcooked_demo_4/kitchen.json", kitchen,
                                      sizeof kitchen) == 0) {
        kos_error_clear(&err);
        expect(kos_assign_config_for_kitchen(kitchen, &cfg, &err) == KOS_OK &&
                   cfg.from_file && close_enough(cfg.cell_ms, 107) &&
                   close_enough(cfg.weights.w_u, 2.0),
               "config_found_next_to_map_kitchen");
    } else {
        expect(0, "config_map_kitchen_found");
    }
}

/* ================================================== 8. decisions.jsonl ===== */

static int field_is_number(const json_value *object, const char *key)
{
    const json_value *value = json_object_get(object, key);

    return value != NULL && json_typeof(value) == JSON_NUMBER;
}

static int field_string_is(const json_value *object, const char *key, const char *text)
{
    const json_value *value = json_object_get(object, key);

    return value != NULL && json_typeof(value) == JSON_STRING &&
           strcmp(json_string_value(value), text) == 0;
}

static void test_decisions_log(void)
{
    ksfx_run run;
    char decisions[KSFX_PATH_MAX];
    FILE *file;
    char *line = NULL;
    size_t cap = 0;
    int dispatch_count = 0;
    int all_have_components = 1;
    int sums_match = 1;
    int saw_edf = 0;
    int saw_llf = 0;
    int saw_hrrn = 0;
    int lines = 0;

    printf("[test_assign] 9. decisions.jsonl 的分量与 SIGHUP\n");
    if (ksfx_init(&run) != 0) {
        expect(0, "fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    if (ksfx_spawn(&run, "fcfs") <= 0) {
        expect(0, "scheduler_spawned");
        ksfx_cleanup(&run);
        return;
    }
    if (ksfx_wait_decisions(&run, "\"dispatch\"", 3000, decisions,
                            sizeof decisions) != 0) {
        ksfx_dump_logs(&run);
        expect(0, "dispatch_decision_written");
        (void)ksfx_stop(&run);
        ksfx_cleanup(&run);
        return;
    }
    expect(1, "dispatch_decision_written");
    ksfx_sleep_ms(100);
    (void)kill(run.pid, SIGHUP);
    ksfx_sleep_ms(150);
    (void)kill(run.pid, SIGHUP);
    ksfx_sleep_ms(150);
    (void)kill(run.pid, SIGHUP);
    ksfx_sleep_ms(250);
    expect(ksfx_stop(&run) == 0, "scheduler_stopped");

    file = fopen(decisions, "r");
    if (file == NULL) {
        expect(0, "decisions_readable");
        ksfx_cleanup(&run);
        return;
    }
    while (getline(&line, &cap, file) >= 0) {
        json_value *root = NULL;
        json_error jerr;
        int rc;

        if (line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        json_error_init(&jerr);
        rc = json_parse_ex(line, strlen(line), &root, &jerr);
        if (rc != JSON_OK || json_typeof(root) != JSON_OBJECT) {
            all_have_components = 0;
            json_free(root);
            continue;
        }
        lines++;
        if (!field_is_number(root, "w_d") || !field_is_number(root, "w_t") ||
            !field_is_number(root, "w_c") || !field_is_number(root, "w_u") ||
            !field_is_number(root, "w_z")) {
            all_have_components = 0;
        }
        if (field_string_is(root, "kind", "dispatch")) {
            double w_d = 0.0;
            double w_t = 0.0;
            double w_c = 0.0;
            double w_u = 0.0;
            double w_z = 0.0;
            double cost = 0.0;

            dispatch_count++;
            if (json_object_get_double(root, "w_d", &w_d) == JSON_OK &&
                json_object_get_double(root, "w_t", &w_t) == JSON_OK &&
                json_object_get_double(root, "w_c", &w_c) == JSON_OK &&
                json_object_get_double(root, "w_u", &w_u) == JSON_OK &&
                json_object_get_double(root, "w_z", &w_z) == JSON_OK &&
                json_object_get_double(root, "cost", &cost) == JSON_OK) {
                if (!close_enough(cost, w_d + w_t + w_c + w_u + w_z)) {
                    sums_match = 0;
                }
            } else {
                sums_match = 0;
            }
            if (!field_is_number(root, "travel_cells") ||
                !field_is_number(root, "weight_c") ||
                !field_is_number(root, "urgency")) {
                all_have_components = 0;
            }
        }
        if (field_string_is(root, "kind", "strategy")) {
            if (field_string_is(root, "to", "EDF")) {
                saw_edf = 1;
            }
            if (field_string_is(root, "to", "LLF")) {
                saw_llf = 1;
            }
            if (field_string_is(root, "to", "HRRN")) {
                saw_hrrn = 1;
            }
        }
        json_free(root);
    }
    free(line);
    fclose(file);

    expect(lines > 0, "decisions_has_lines");
    expect(dispatch_count > 0, "decisions_has_dispatch");
    expect(all_have_components, "every_decision_has_component_fields");
    expect(sums_match, "dispatch_cost_equals_component_sum");
    expect(saw_edf && saw_llf && saw_hrrn, "sighup_cycles_to_edf_llf_hrrn");
    ksfx_cleanup(&run);
}


/*
 * 世界状态里有厨师坐标和脏盘时，路程来自 stand 的距离场，wash 由 Scheduler 自己
 * 生成并走正常候选列表。空菜单让 wash 成为唯一候选，这样不用等订单工序超时。
 */
static void test_distance_and_wash(void)
{
    ksfx_run run;
    kos_shm_opts opts;
    kos_shm *shm = NULL;
    kos_world_state world;
    kos_error err;
    char decisions[KSFX_PATH_MAX];
    char run_dir[KSFX_PATH_MAX];
    char events[KSFX_PATH_MAX];
    char *text;
    char *slash;
    size_t len = 0;
    FILE *file;
    char *line = NULL;
    size_t cap = 0;
    int sink_station = -1;
    int chef_cell = -1;
    int wash_dispatched = 0;
    int travel_positive = 0;
    int i;

    printf("[test_assign] 10. 距离场路程与 wash 生成\n");
    if (ksfx_init_ex(&run, 0) != 0) {
        expect(0, "wash_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    for (i = 0; i < run.kitchen.station_count; i++) {
        if (kos_kitchen_name_matches("sink", run.kitchen.stations[i].kind)) {
            sink_station = i;
            break;
        }
    }
    expect(sink_station >= 0, "wash_sink_station_found");
    if (sink_station < 0) {
        ksfx_cleanup(&run);
        return;
    }
    for (i = 0; i < run.kitchen.cell_count; i++) {
        uint16_t distance = kos_stand_distance(&run.kitchen, sink_station, i);

        if (distance > 0 && distance < 100) {
            chef_cell = i;
            break;
        }
    }
    expect(chef_cell >= 0, "wash_chef_cell_found");
    if (chef_cell < 0) {
        ksfx_cleanup(&run);
        return;
    }

    memset(&opts, 0, sizeof opts);
    opts.object_name = run.state_name;
    opts.sem_name = run.sem_name;
    opts.perm = 0600;
    kos_error_clear(&err);
    if (kos_shm_open(&opts, KOS_SHM_CREATE, &shm, &err) != KOS_OK) {
        expect(0, "wash_shm_created");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    world.chef_count = 2;
    world.chefs[0].id = 0;
    world.chefs[0].x = run.kitchen.cells[chef_cell].x;
    world.chefs[0].y = run.kitchen.cells[chef_cell].z;
    world.chefs[1].id = 1;
    world.chefs[1].x = run.kitchen.cells[chef_cell].x;
    world.chefs[1].y = run.kitchen.cells[chef_cell].z;
    world.resource_count = 2;
    snprintf(world.resources[0].name, sizeof world.resources[0].name, "plate_stack");
    snprintf(world.resources[0].state, sizeof world.resources[0].state, "free");
    world.resources[0].count = 0;
    snprintf(world.resources[1].name, sizeof world.resources[1].name, "dirty_plates");
    snprintf(world.resources[1].state, sizeof world.resources[1].state, "occupied");
    world.resources[1].count = 2;
    expect(kos_shm_world_write(shm, &world) == KOS_OK, "wash_world_written");

    if (ksfx_spawn(&run, "fcfs") <= 0) {
        expect(0, "wash_scheduler_spawned");
        (void)kos_shm_unlink(&opts, &err);
        (void)kos_shm_close(shm, &err);
        ksfx_cleanup(&run);
        return;
    }
    if (ksfx_wait_decisions(&run, "wash", 3000, decisions, sizeof decisions) != 0) {
        ksfx_dump_logs(&run);
        expect(0, "wash_decision_written");
        (void)ksfx_stop(&run);
        (void)kos_shm_unlink(&opts, &err);
        (void)kos_shm_close(shm, &err);
        ksfx_cleanup(&run);
        return;
    }
    expect(1, "wash_decision_written");
    expect(ksfx_stop(&run) == 0, "wash_scheduler_stopped");

    file = fopen(decisions, "r");
    if (file != NULL) {
        while (getline(&line, &cap, file) >= 0) {
            json_value *root = NULL;
            json_error jerr;
            const char *kind = NULL;
            const char *node = NULL;

            json_error_init(&jerr);
            if (line[0] == '\n' ||
                json_parse_ex(line, strlen(line), &root, &jerr) != JSON_OK ||
                json_typeof(root) != JSON_OBJECT) {
                json_free(root);
                continue;
            }
            (void)json_object_get_string(root, "kind", &kind, NULL);
            (void)json_object_get_string(root, "node", &node, NULL);
            if (kind != NULL && node != NULL && strcmp(kind, "dispatch") == 0 &&
                strcmp(node, "wash") == 0) {
                const json_value *value = json_object_get(root, "travel_cells");

                wash_dispatched = 1;
                if (value != NULL && json_typeof(value) == JSON_NUMBER &&
                    json_number_value(value) > 0) {
                    travel_positive = 1;
                }
            }
            json_free(root);
        }
        free(line);
        fclose(file);
    }
    expect(wash_dispatched, "wash_dispatched_for_dirty_plates");
    if (!travel_positive) {
        ksfx_dump_file("decisions", decisions);
        ksfx_dump_logs(&run);
    }
    expect(travel_positive, "wash_cost_uses_distance_field");

    ksfx_copy(run_dir, sizeof run_dir, decisions);
    slash = strrchr(run_dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (ksfx_join(events, sizeof events, run_dir, "events.jsonl") == 0) {
            text = ksfx_read_file(events, &len);
            expect(text != NULL && strstr(text, "wash_generated") != NULL,
                   "wash_generated_event");
            free(text);
        }
    }
    (void)kos_shm_unlink(&opts, &err);
    (void)kos_shm_close(shm, &err);
    ksfx_cleanup(&run);
}


/*
 * 没有对应工位的工序要尽早让订单失败，而不是留在未开工状态：open_next_order 把
 * "找不到工位"也算可开，随后 build_candidates 的 fail_task_no_station 负责收尾。
 * 这条路径在 `_1`（没有 plate_stack 工位）会用到。
 */
static void test_missing_station_fails_order(void)
{
    ksfx_run run;
    char events[KSFX_PATH_MAX];
    char *text;
    size_t len = 0;
    static const char recipes_text[] =
        "{\"scene\":\"s_wellcooked_demo_2\","
        "\"nodes\":[{\"id\":\"fetch:Missing\",\"op\":\"fetch\",\"label\":\"Missing\","
        "\"resource\":\"no_such_resource\",\"duration\":1.0}],"
        "\"edges\":[],"
        "\"dishes\":[{\"dish\":\"Missing\",\"score\":1,\"root\":\"fetch:Missing\","
        "\"nodes\":[\"fetch:Missing\"]}]}\n";
    static const char orders_text[] =
        "{\"menu\":[{\"recipes\":[\"Missing\"]}]}\n";

    printf("[test_assign] 11. 缺工位的订单失败\n");
    if (ksfx_init_ex(&run, 0) != 0) {
        expect(0, "missing_station_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    if (ksfx_write_file(run.recipes_path, recipes_text, sizeof recipes_text - 1) != 0 ||
        ksfx_write_file(run.orders_path, orders_text, sizeof orders_text - 1) != 0) {
        expect(0, "missing_station_fixture_written");
        ksfx_cleanup(&run);
        return;
    }
    if (ksfx_spawn(&run, "fcfs") <= 0) {
        expect(0, "missing_station_scheduler_spawned");
        ksfx_cleanup(&run);
        return;
    }
    expect(ksfx_wait_events(&run, "no station for resource", 3000, events,
                            sizeof events) == 0,
           "missing_station_event_written");
    expect(ksfx_stop(&run) == 0, "missing_station_scheduler_stopped");
    text = ksfx_read_file(events, &len);
    expect(text != NULL && strstr(text, "\"task_failed\"") != NULL &&
               strstr(text, "\"order_id\":\"cfg-00\"") != NULL,
           "missing_station_order_failed");
    free(text);
    ksfx_cleanup(&run);
}

int main(void)
{
    printf("[test_assign] 开始：派工代价、匈牙利配对、锅链/洗盘、决策日志\n");
    test_cost();
    test_hungarian_two_by_three();
    test_hungarian_more_chefs();
    test_hungarian_unreachable();
    test_hungarian_ties();
    test_pot_chain();
    test_wash_rule();
    test_config();
    test_decisions_log();
    test_distance_and_wash();
    test_missing_station_fails_order();
    printf("[test_assign] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
