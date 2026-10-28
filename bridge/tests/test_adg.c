/*
 * test_adg.c -- 执行依赖图放行与导航表。
 *
 * 1. 用 _3 的真实网格跑 PIBT，得到一组同步无冲突的轨迹；随机打乱各厨师的
 *    执行快慢，只按 kos_adg_releasable 的结果推进，任何时刻都不许两人同格。
 * 2. 导航表自己的读写、单写者槽位、重启后计划序号仍在。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/adg.h"
#include "kitchenos/child.h"
#include "kitchenos/kitchen.h"
#include "kitchenos/navtab.h"
#include "kitchenos/pibt.h"

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

static uint32_t g_rng = 0x243f6a88u;

static uint32_t rng_next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static int load_map(const char *scene, kos_kitchen *kitchen, kos_pibt *pibt)
{
    char relative[128];
    char path[KOS_DAEMON_PATH_MAX];
    kos_error err;

    snprintf(relative, sizeof relative, "maps/%s/kitchen.json", scene);
    if (kos_child_default_config_path(relative, path, sizeof path) != 0) {
        return -1;
    }
    memset(kitchen, 0, sizeof *kitchen);
    kos_error_clear(&err);
    if (kos_kitchen_load(path, kitchen, &err) != KOS_OK) {
        return -1;
    }
    return kos_pibt_init(pibt, kitchen);
}

static int cells_of_component(const kos_kitchen *kitchen, int component, int *cells,
                              int cap)
{
    int count = 0;
    int i;

    for (i = 0; i < kitchen->cell_count && count < cap; i++) {
        if (kitchen->stand.component[i] == component) {
            cells[count++] = i;
        }
    }
    return count;
}

/* 单次执行模拟：随机挑一个已放行且还没走到的人前进一步。 */
static int simulate(const kos_adg *graph, const int *starts, int chef_count,
                    int *bad_round)
{
    int actual[KOS_ADG_MAX_CHEFS];
    int executed[KOS_ADG_MAX_CHEFS];
    int rounds = 0;
    int i;

    for (i = 0; i < chef_count; i++) {
        actual[i] = starts[i];
        executed[i] = 1;
    }
    for (;;) {
        int progress = 0;

        for (i = 0; i < chef_count; i++) {
            int count = 0;

            if (kos_adg_releasable(graph, i, executed, actual, &count) != KOS_OK) {
                return -1;
            }
            if (count > 0 && executed[i] < graph->length[i]) {
                progress = 1;
            }
        }
        if (!progress) {
            break;
        }
        /* 随机顺序试；只放行 kos_adg_releasable 允许的那一步。 */
        for (i = 0; i < chef_count; i++) {
            int pick = (int)(rng_next() % (uint32_t)chef_count);
            int count = 0;
            int j;

            if (kos_adg_releasable(graph, pick, executed, actual, &count) != KOS_OK) {
                return -1;
            }
            if (count <= 0 || executed[pick] >= graph->length[pick]) {
                continue;
            }
            actual[pick] = graph->cells[pick][executed[pick]];
            executed[pick]++;
            for (j = 0; j < chef_count; j++) {
                int k;

                for (k = 0; k < j; k++) {
                    if (actual[j] == actual[k]) {
                        *bad_round = rounds;
                        return 0;
                    }
                }
            }
            progress = 1;
            break;
        }
        rounds++;
        if (rounds > 20000) {
            break;
        }
    }
    for (i = 0; i < chef_count; i++) {
        if (executed[i] != graph->length[i]) {
            return 0;
        }
    }
    return 1;
}

static void test_adg_random_speeds(void)
{
    static kos_kitchen kitchen;
    static kos_pibt pibt;
    static kos_adg graph;
    static int trajectory[KOS_ADG_MAX_CHEFS][KOS_ADG_MAX_STEPS];
    int starts[KOS_ADG_MAX_CHEFS];
    uint16_t dist[KOS_ADG_MAX_CHEFS][KOS_KITCHEN_MAX_CELLS];
    const int *sequences[KOS_ADG_MAX_CHEFS];
    int lengths[KOS_ADG_MAX_CHEFS];
    int cells[KOS_KITCHEN_MAX_CELLS];
    int component_cells;
    int chef_count = 4;
    int cell_count;
    int i;
    int trial;
    int failed = 0;
    int checked = 0;

    printf("[test_adg] 1. 执行快慢打乱后的放行\n");
    expect(load_map("s_wellcooked_demo_3", &kitchen, &pibt) == KOS_OK,
           "_3 kitchen + pibt");
    if (kitchen.cell_count == 0) {
        return;
    }
    cell_count = cells_of_component(&kitchen, 0, cells, KOS_KITCHEN_MAX_CELLS);
    component_cells = cell_count;
    expect(component_cells >= chef_count * 2, "_3 主连通区有足够格");

    for (trial = 0; trial < 50; trial++) {
        kos_pibt_agent agents[KOS_PIBT_MAX_AGENTS];
        int next[KOS_PIBT_MAX_AGENTS];
        int current[KOS_PIBT_MAX_AGENTS];
        int goals[KOS_PIBT_MAX_AGENTS];
        int step;
        int rc;
        int bad_round = -1;

        for (i = 0; i < chef_count; i++) {
            int attempt;

            current[i] = cells[rng_next() % (uint32_t)component_cells];
            for (attempt = 0; attempt < 100; attempt++) {
                int candidate = cells[rng_next() % (uint32_t)component_cells];
                int j;
                int seen = 0;

                for (j = 0; j < i; j++) {
                    if (candidate == current[j]) {
                        seen = 1;
                    }
                }
                if (!seen) {
                    current[i] = candidate;
                    break;
                }
            }
            for (attempt = 0; attempt < 100; attempt++) {
                int candidate = cells[rng_next() % (uint32_t)component_cells];
                int j;
                int seen = 0;

                for (j = 0; j < i; j++) {
                    if (candidate == goals[j]) {
                        seen = 1;
                    }
                }
                if (!seen) {
                    goals[i] = candidate;
                    break;
                }
            }
            starts[i] = current[i];
            trajectory[i][0] = current[i];
            lengths[i] = 1;
            kos_pibt_distance(&pibt, goals[i], dist[i], KOS_KITCHEN_MAX_CELLS);
        }
        for (step = 0; step < 120; step++) {
            for (i = 0; i < chef_count; i++) {
                memset(&agents[i], 0, sizeof agents[i]);
                agents[i].chef_id = i;
                agents[i].cell = current[i];
                agents[i].goal = goals[i];
                agents[i].priority = chef_count - i;
                agents[i].distance = dist[i];
            }
            rc = kos_pibt_step(&pibt, agents, chef_count, next);
            if (rc != KOS_OK) {
                failed++;
                break;
            }
            for (i = 0; i < chef_count; i++) {
                current[i] = next[i];
                if (lengths[i] < KOS_ADG_MAX_STEPS) {
                    trajectory[i][lengths[i]++] = current[i];
                }
            }
        }
        if (rc != KOS_OK) {
            continue;
        }
        for (i = 0; i < chef_count; i++) {
            sequences[i] = trajectory[i];
        }
        if (kos_adg_build(&graph, sequences, lengths, chef_count) != KOS_OK) {
            failed++;
            continue;
        }
        for (i = 0; i < 20; i++) {
            int result = simulate(&graph, starts, chef_count, &bad_round);

            checked++;
            if (result != 1) {
                failed++;
                if (bad_round >= 0) {
                    printf("       碰撞: trial=%d round=%d\n", trial, bad_round);
                }
                break;
            }
        }
    }
    expect(failed == 0,
           failed == 0 ? "随机执行快慢下 1000 次模拟无同格" : "放行序列出现同格/未走完");
    expect(checked > 0, "执行依赖图模拟确实跑过");
}

static void test_navtab(void)
{
    kos_navtab_opts opts;
    kos_navtab *table = NULL;
    kos_navtab *again = NULL;
    kos_nav_target target;
    kos_nav_target read_target;
    kos_nav_plan plan;
    kos_nav_plan read_plan;
    char object[96];
    char sem[96];
    char nav_name[96];
    char nav_sem[96];
    kos_error err;

    printf("[test_adg] 2. 导航表\n");
    snprintf(object, sizeof object, "/kitchenos_test_nav_%ld", (long)getpid());
    snprintf(sem, sizeof sem, "/kitchenos_test_navlock_%ld", (long)getpid());
    kos_navtab_opts_init(&opts);
    opts.object_name = object;
    opts.sem_name = sem;
    kos_error_clear(&err);
    (void)kos_navtab_unlink(&opts, &err);
    expect(kos_navtab_create(&opts, &table, &err) == KOS_OK, "导航表创建");
    expect(table != NULL, "创建返回句柄");
    if (table == NULL) {
        return;
    }

    memset(&target, 0, sizeof target);
    target.has_target = 1;
    target.pinned = 0;
    target.target_seq = 7;
    target.urgency = 3;
    snprintf(target.station_id, sizeof target.station_id, "dispenser-0");
    expect(kos_navtab_write_target(table, 0, &target) == KOS_OK, "目标槽写入");
    expect(kos_navtab_read_target(table, 0, &read_target) == KOS_OK &&
               read_target.has_target == 1 && read_target.target_seq == 7 &&
               read_target.urgency == 3 &&
               strcmp(read_target.station_id, "dispenser-0") == 0,
           "目标槽读回一致");

    memset(&plan, 0, sizeof plan);
    plan.plan_seq = 11;
    plan.status = KOS_NAV_MOVING;
    plan.target_seq = 7;
    plan.waypoint_count = 2;
    plan.waypoints[0].x = 1.2f;
    plan.waypoints[0].y = -0.6f;
    plan.waypoints[1].x = 1.8f;
    plan.waypoints[1].y = -0.6f;
    plan.game_time_ms = 12345;
    expect(kos_navtab_write_plan(table, 0, &plan) == KOS_OK, "计划槽写入");
    expect(kos_navtab_read_plan(table, 0, &read_plan) == KOS_OK &&
               read_plan.plan_seq == 11 && read_plan.status == KOS_NAV_MOVING &&
               read_plan.waypoint_count == 2 &&
               read_plan.waypoints[1].x == 1.8f,
           "计划槽读回一致");
    expect(kos_navtab_write_plan(table, KOS_SHM_MAX_CHEFS, &plan) == KOS_ERR_INVAL,
           "越界厨师号被拒");
    expect(kos_navtab_write_target(table, -1, &target) == KOS_ERR_INVAL,
           "负厨师号被拒");

    expect(kos_navtab_open(&opts, &again, &err) == KOS_OK, "第二个句柄挂接");
    if (again != NULL) {
        expect(kos_navtab_read_plan(again, 0, &read_plan) == KOS_OK &&
                   read_plan.plan_seq == 11,
               "挂接后仍读到计划序号（模拟 navd 重启）");
        kos_navtab_close(again);
    }
    kos_navtab_close(table);
    expect(kos_navtab_unlink(&opts, &err) == KOS_OK, "导航表删除");
    expect(kos_navtab_open(&opts, &table, &err) == KOS_ERR_MISSING, "删除后挂接失败");

    expect(kos_navtab_default_names(KOS_SHM_DEFAULT_NAME, nav_name, sizeof nav_name,
                                    nav_sem, sizeof nav_sem) == 0 &&
               strcmp(nav_name, KOS_NAV_DEFAULT_NAME) == 0 &&
               strcmp(nav_sem, KOS_NAV_DEFAULT_SEM_NAME) == 0,
           "默认状态名派生默认导航表名");
    expect(kos_navtab_default_names("/kitchenos_x", nav_name, sizeof nav_name, nav_sem,
                                    sizeof nav_sem) == 0 &&
               strcmp(nav_name, "/kitchenos_x_nav") == 0 &&
               strcmp(nav_sem, "/kitchenos_x_nav_lock") == 0,
           "自定义状态名派生独立导航表名");
}

int main(void)
{
    printf("[test_adg] 开始：依赖图放行与导航表\n");
    test_adg_random_speeds();
    test_navtab();
    printf("[test_adg] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
