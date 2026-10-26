/*
 * test_map_configs.c -- 四张自制地图的配置能被 Scheduler / Worker 的加载器读进来。
 *
 * 任务 01 的判据是"四张图都能交给系统控制"，而 Scheduler 与 Worker 共用
 * kitchen.c / recipes.c 这两个加载器。这里用与守护进程完全相同的入口
 * (kos_kitchen_load / kos_recipes_load) 逐个读 config/maps/<scene>/，所以目录少文件、
 * JSON 超出行/列上限、场景写错、菜谱 DAG 引用不存在的节点都会在这里失败，
 * 不用先起游戏或 mq。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/child.h"
#include "kitchenos/kitchen.h"
#include "kitchenos/recipes.h"

#include <stdio.h>
#include <string.h>

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

static int find_config(const char *relative, char *path, size_t len)
{
    return kos_child_default_config_path(relative, path, len) == 0;
}

static void test_one_map(const char *scene)
{
    char relative[128];
    char kitchen_path[KOS_DAEMON_PATH_MAX];
    char recipes_path[KOS_DAEMON_PATH_MAX];
    {
        static kos_kitchen kitchen;
        static kos_recipes recipes;
        kos_error err;
        int i;
        char label[192];

        printf("[test_map_configs] %s\n", scene);

        snprintf(relative, sizeof relative, "maps/%s/kitchen.json", scene);
        snprintf(label, sizeof label, "%s: kitchen.json 存在", scene);
        expect(find_config(relative, kitchen_path, sizeof kitchen_path), label);

        snprintf(relative, sizeof relative, "maps/%s/recipes.json", scene);
        snprintf(label, sizeof label, "%s: recipes.json 存在", scene);
        expect(find_config(relative, recipes_path, sizeof recipes_path), label);

        memset(&kitchen, 0, sizeof kitchen);
        kos_error_clear(&err);
        snprintf(label, sizeof label, "%s: kos_kitchen_load", scene);
        if (kos_kitchen_load(kitchen_path, &kitchen, &err) != KOS_OK) {
            expect(0, label);
            return;
        }
        expect(1, label);
        snprintf(label, sizeof label, "%s: scene 与目录名一致", scene);
        expect(strcmp(kitchen.scene, scene) == 0, label);
        snprintf(label, sizeof label, "%s: 格距 0.6 米", scene);
        expect(kitchen.step > 0.59f && kitchen.step < 0.61f, label);
        snprintf(label, sizeof label, "%s: 有可走格与工位", scene);
        expect(kitchen.cell_count > 0 && kitchen.station_count > 0, label);
        snprintf(label, sizeof label, "%s: 四类关键工位齐全（料箱/出菜口/锅/水槽）", scene);
        expect(kos_kitchen_count_kind(&kitchen, "dispenser") > 0 &&
                   kos_kitchen_count_kind(&kitchen, "serving") > 0 &&
                   kos_kitchen_count_kind(&kitchen, "cooker") > 0 &&
                   kos_kitchen_count_kind(&kitchen, "sink") > 0,
               label);
        snprintf(label, sizeof label, "%s: 网格掩码只用 4 位", scene);
        for (i = 0; i < kitchen.cell_count; i++) {
            if ((kitchen.cells[i].mask & (unsigned char)~0x0fu) != 0) {
                break;
            }
        }
        expect(i == kitchen.cell_count, label);
        snprintf(label, sizeof label, "%s: 工位 id 非空且不重复", scene);
        for (i = 0; i < kitchen.station_count; i++) {
            int j;
            if (kitchen.stations[i].id[0] == '\0' ||
                kitchen.stations[i].kind[0] == '\0') {
                break;
            }
            for (j = i + 1; j < kitchen.station_count; j++) {
                if (strcmp(kitchen.stations[i].id, kitchen.stations[j].id) == 0) {
                    i = kitchen.station_count;
                    break;
                }
            }
        }
        expect(i == kitchen.station_count, label);

        memset(&recipes, 0, sizeof recipes);
        kos_error_clear(&err);
        snprintf(label, sizeof label, "%s: kos_recipes_load", scene);
        if (kos_recipes_load(recipes_path, &recipes, &err) != KOS_OK) {
            expect(0, label);
            return;
        }
        expect(1, label);
        snprintf(label, sizeof label, "%s: 菜谱 scene 与目录名一致", scene);
        expect(strcmp(recipes.scene, scene) == 0, label);
        snprintf(label, sizeof label, "%s: 有工序、边与菜", scene);
        expect(recipes.node_count > 0 && recipes.edge_count > 0 &&
                   recipes.dish_count > 0,
               label);
        snprintf(label, sizeof label, "%s: 每条边指向已有工序", scene);
        for (i = 0; i < recipes.edge_count; i++) {
            if (recipes.edges[i].from < 0 || recipes.edges[i].from >= recipes.node_count ||
                recipes.edges[i].to < 0 || recipes.edges[i].to >= recipes.node_count) {
                break;
            }
        }
        expect(i == recipes.edge_count, label);
        snprintf(label, sizeof label, "%s: 每道菜的节点有序且根有效", scene);
        for (i = 0; i < recipes.dish_count; i++) {
            int j;
            const kos_dish *dish = &recipes.dishes[i];
            if (dish->dish[0] == '\0' || dish->node_count <= 0 ||
                dish->root < 0 || dish->root >= recipes.node_count) {
                break;
            }
            for (j = 0; j < dish->node_count; j++) {
                if (dish->nodes[j] < 0 || dish->nodes[j] >= recipes.node_count) {
                    break;
                }
            }
            if (j != dish->node_count) {
                break;
            }
        }
        expect(i == recipes.dish_count, label);
            /* 每个工序的资源类型都要有对象可派，否则 Scheduler 的派工会落到
             * "recipe_no_station"。`_1` 没有干净盘堆（spec Notes），plate 工序在
             * 洗盘任务（04/05）补上资源模型前是已知缺口，不在这里冒充通过。 */
        snprintf(label, sizeof label, "%s: 每个工序的资源类型都有工位（_1 的 plate_stack 除外）",
                 scene);
        for (i = 0; i < recipes.node_count; i++) {
            const kos_recipe_node *node = &recipes.nodes[i];
            if (node->resource[0] == '\0') {
                break;
            }
            if (strcmp(scene, "s_wellcooked_demo_1") == 0 &&
                strcmp(node->resource, "plate_stack") == 0) {
                continue;
            }
            if (kos_kitchen_count_kind(&kitchen, node->resource) <= 0) {
                break;
            }
        }
        expect(i == recipes.node_count, label);
    }
}

static void test_face_target_coordinate(void)
{
    static kos_kitchen kitchen;
    kos_input_action action;
    kos_error err;
    const kos_station *station;
    char path[KOS_DAEMON_PATH_MAX];

    expect(find_config("maps/s_wellcooked_demo_2/kitchen.json", path, sizeof path),
           "demo2 face coordinate kitchen path");
    kos_error_clear(&err);
    expect(kos_kitchen_load(path, &kitchen, &err) == KOS_OK,
           "demo2 face coordinate kitchen loads");
    station = kos_kitchen_station_by_id(&kitchen, "dispenser-4");
    expect(station != NULL, "dispenser-4 is a stable map station ID");
    expect(kos_kitchen_face_action(&kitchen, "dispenser-4", &action) == KOS_OK,
           "face action accepts dispenser-4 map ID");
    if (station != NULL) {
        expect(action.kind == KOS_ACTION_FACE && action.target_x == station->x &&
                   action.target_y == station->z,
               "face action carries dispenser-4 map x/z coordinates");
    }
    expect(kos_kitchen_face_action(&kitchen, "dispenser-missing", &action) != KOS_OK,
           "unknown station ID cannot produce face action");
}

int main(void)
{
    static const char *const scenes[] = {
        "s_wellcooked_demo_1",
        "s_wellcooked_demo_2",
        "s_wellcooked_demo_3",
        "s_wellcooked_demo_4",
    };
    size_t i;

    test_face_target_coordinate();
    printf("[test_map_configs] 开始：四张地图的 kitchen/recipes 加载\n");
    for (i = 0; i < sizeof scenes / sizeof scenes[0]; i++) {
        test_one_map(scenes[i]);
    }
    printf("[test_map_configs] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
