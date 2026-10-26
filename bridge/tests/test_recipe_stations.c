/*
 * test_recipe_stations.c -- 用 `_3` 的实机夹具核对每道菜的每道工序都有工位可派。
 *
 * 夹具在 `scripts/fixtures/s_wellcooked_demo_3/`：state/menu/terrain 是实机
 * 20261027-033435 的原样导出，普通台面的 `ClientAttachStation` 记录按 manual.log 的
 * 对象名与 layout_3.tsv 的坐标补齐（那次跑的是旧插件，状态里没有它们）；kitchen.json
 * 与 recipes.json 由 `scripts/export-kitchen.py` 从这三份文件生成，和 Runner 进关后
 * 走的是同一个生成器。
 *
 * 锁住的三件事：
 *   1. 5 道菜的每个节点都能在这张厨房里找到工位（`recipe_no_station` 的反面）；
 *   2. `cook` 节点的 step 有同类型的灶；
 *   3. 卷饼的台面由 `kos_kitchen_pick_counter` 按"离灶与盘堆最近"确定性挑选，
 *      不是按订单序号取模。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/kitchen.h"
#include "kitchenos/recipes.h"
#include "kitchenos/stand.h"

#include <stdio.h>
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

/*
 * 夹具不在 config/ 下，`kos_child_default_config_path` 不适用：从可执行文件位置
 * （bridge/build 或 bridge/build/san）往上找仓库根。
 */
static int find_fixture(const char *name, char *path, size_t len)
{
    static const char *const prefixes[] = {"../..", "../../..", "../../../.."};
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *slash;
    size_t p;

    if (n <= 0) {
        return -1;
    }
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash != NULL) {
        *slash = '\0';
    }
    for (p = 0; p < sizeof prefixes / sizeof prefixes[0]; p++) {
        if (snprintf(path, len, "%s/%s/scripts/fixtures/s_wellcooked_demo_3/%s", exe,
                     prefixes[p], name) >= (int)len) {
            continue;
        }
        if (access(path, R_OK) == 0) {
            return 0;
        }
    }
    return -1;
}

/* cook 的 step 与 cookers 的 step 匹配规则与 scheduler.c 的 cooker_step_matches 相同。 */
static int has_cooker_step(const kos_kitchen *kitchen, const char *step)
{
    int i;

    for (i = 0; i < kitchen->station_count; i++) {
        const kos_station *station = &kitchen->stations[i];

        if (strcmp(station->kind, "cooker") != 0) {
            continue;
        }
        if (step[0] == '\0' || station->step[0] == '\0' ||
            strcmp(station->step, step) == 0) {
            return 1;
        }
    }
    return 0;
}

static int node_has_station(const kos_kitchen *kitchen, const kos_recipe_node *node)
{
    if (node->resource[0] == '\0' || strcmp(node->op, "drop") == 0) {
        return 1;
    }
    if (strcmp(node->op, "plate") == 0 &&
        kos_kitchen_count_kind(kitchen, "plate_stack") == 0) {
        return kos_kitchen_count_kind(kitchen, "sink") > 0;
    }
    return kos_kitchen_count_kind(kitchen, node->resource) > 0;
}

int main(void)
{
    char kitchen_path[1200];
    char recipes_path[1200];
    kos_kitchen kitchen;
    kos_recipes recipes;
    kos_error err;
    int place_station = -1;
    int second_station = -1;
    int i;
    int d;

    printf("[test_recipe_stations] 实机 `_3` 夹具\n");
    expect(find_fixture("kitchen.json", kitchen_path, sizeof kitchen_path) == 0,
           "fixture_kitchen_found");
    expect(find_fixture("recipes.json", recipes_path, sizeof recipes_path) == 0,
           "fixture_recipes_found");
    if (g_failures > 0) {
        printf("recipe_stations: %d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    kos_error_clear(&err);
    expect(kos_kitchen_load(kitchen_path, &kitchen, &err) == KOS_OK,
           "fixture_kitchen_loads");
    kos_error_clear(&err);
    expect(kos_recipes_load(recipes_path, &recipes, &err) == KOS_OK,
           "fixture_recipes_load");
    if (g_failures > 0) {
        printf("recipe_stations: %d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    /* 实机 `_3` 的工位数：42 张普通台面里 4 张在走不到的角上由导出丢掉，
     * 所以是 38 张 counter；盘堆/砧板/灶来自那次状态导出。 */
    expect(kos_kitchen_count_kind(&kitchen, "counter") == 38, "counter_count_38");
    expect(kos_kitchen_count_kind(&kitchen, "board") == 2, "board_count_2");
    expect(kos_kitchen_count_kind(&kitchen, "cooker") == 6, "cooker_count_6");
    expect(kos_kitchen_count_kind(&kitchen, "plate_stack") == 1, "plate_stack_count_1");
    expect(recipes.dish_count == 5, "dish_count_5");

    for (i = 0; i < recipes.node_count; i++) {
        const kos_recipe_node *node = &recipes.nodes[i];
        char what[256];

        snprintf(what, sizeof what, "node_has_station:%s", node->id);
        expect(node_has_station(&kitchen, node), what);
        if (strcmp(node->op, "cook") == 0) {
            snprintf(what, sizeof what, "node_cooker_step:%s", node->id);
            expect(has_cooker_step(&kitchen, node->step), what);
        }
    }

    /* 卷饼三道菜的灶类型相同（FryingPan + Pot），台面选择必须一致且可复现。 */
    {
        static const char *const steps[] = {"FryingPan", "Pot"};

        expect(kos_kitchen_pick_counter(&kitchen, steps, 2, NULL, &place_station) == KOS_OK,
               "place_counter_picked");
        expect(kos_kitchen_pick_counter(&kitchen, steps, 2, NULL, &second_station) == KOS_OK &&
                   second_station == place_station,
               "place_counter_deterministic");
        if (place_station >= 0) {
            const kos_station *station = &kitchen.stations[place_station];
            unsigned char busy[KOS_KITCHEN_MAX_STATIONS] = {0};
            int other = -1;

            printf("  info 卷饼台面 = %s @(%.2f,%.2f)\n", station->id, station->x, station->z);
            expect(strcmp(station->kind, "counter") == 0, "place_counter_kind_counter");
            expect(station->id[0] != '\0', "place_counter_has_id");
            expect(kos_stand_cells(&kitchen, place_station) != NULL &&
                       kos_stand_cells(&kitchen, place_station)->count > 0,
                   "place_counter_has_stand");
            /* 盘堆底下那张台面不能选（放不下玉米饼）。 */
            expect(!(station->x > 2.9f && station->x < 3.1f &&
                     station->z > 1.1f && station->z < 1.3f),
                   "place_counter_not_under_plate_stack");
            /* 别的订单占住它时挑下一张，而不是重复同一张。 */
            busy[place_station] = 1;
            expect(kos_kitchen_pick_counter(&kitchen, steps, 2, busy, &other) == KOS_OK &&
                       other >= 0 && other != place_station,
                   "place_counter_skips_busy");
        } else {
            expect(0, "place_counter_selected");
        }
    }

    /* 每一道菜的每个节点都按菜的视图再查一遍：DAG 里的节点引用必须存在。 */
    for (d = 0; d < recipes.dish_count; d++) {
        const kos_dish *dish = &recipes.dishes[d];
        char what[256];
        int ok = dish->node_count > 0;

        for (i = 0; i < dish->node_count; i++) {
            if (dish->nodes[i] < 0 || dish->nodes[i] >= recipes.node_count) {
                ok = 0;
            }
        }
        snprintf(what, sizeof what, "dish_nodes_exist:%s", dish->dish);
        expect(ok, what);
    }

    printf("recipe_stations: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
