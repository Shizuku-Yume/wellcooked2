/*
 * test_pibt.c -- 四张地图上的 PIBT 可达性与 pa 对照基线的无冲突、确定性。
 *
 * 每个随机用例用同一张图的真实网格：起点与终点都取同一连通区内的可走格，
 * 距离场按终点 BFS。逐步检查 mask、顶点冲突与对换，再检查 _3 上 200 步内
 * 全部到达。同一输入跑两遍的比较锁定"平局按 chef_id / 格下标"的确定性。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/child.h"
#include "kitchenos/kitchen.h"
#include "kitchenos/nav.h"
#include "kitchenos/pa.h"
#include "kitchenos/pibt.h"

#include <stdint.h>
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

static uint32_t g_rng = 0x9e3779b9u;

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

static int in_component(const kos_kitchen *kitchen, int cell, int component)
{
    return cell >= 0 && cell < kitchen->cell_count &&
           kitchen->stand.component[cell] == component;
}

static int pick_distinct(const kos_kitchen *kitchen, int component, int *used, int used_n,
                         int *out)
{
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        int cell = (int)(rng_next() % (uint32_t)kitchen->cell_count);
        int i;
        int seen = 0;

        if (!in_component(kitchen, cell, component)) {
            continue;
        }
        for (i = 0; i < used_n; i++) {
            if (used[i] == cell) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            *out = cell;
            return 0;
        }
    }
    return -1;
}

typedef struct {
    int count;
    int start[KOS_PIBT_MAX_AGENTS];
    int goal[KOS_PIBT_MAX_AGENTS];
    uint16_t dist[KOS_PIBT_MAX_AGENTS][KOS_KITCHEN_MAX_CELLS];
    int steps;
    uint64_t hash;
    int arrived;
    int final_cell[KOS_PIBT_MAX_AGENTS];
} trial_result;

static int run_trial(kos_kitchen *kitchen, kos_pibt *pibt, const trial_result *input,
                     int max_steps, trial_result *out)
{
    kos_pibt_agent agents[KOS_PIBT_MAX_AGENTS];
    int cells[KOS_PIBT_MAX_AGENTS];
    int next[KOS_PIBT_MAX_AGENTS];
    int i;
    int step;
    int anchor[KOS_PIBT_MAX_AGENTS];
    uint16_t live_dist[KOS_PIBT_MAX_AGENTS][KOS_KITCHEN_MAX_CELLS];
    uint64_t hash = 1469598103934665603ull;

    *out = *input;
    out->arrived = 0;
    out->steps = 0;
    out->hash = 0;
    for (i = 0; i < input->count; i++) {
        cells[i] = input->start[i];
        anchor[i] = 0;
    }
    for (step = 0; step < max_steps; step++) {
        int all_at_goal = 1;

        for (i = 0; i < input->count; i++) {
            if (cells[i] != input->goal[i]) {
                all_at_goal = 0;
                break;
            }
        }
        if (all_at_goal) {
            out->arrived = 1;
            break;
        }
        {
            uint8_t blocked[KOS_KITCHEN_MAX_CELLS];

            memset(blocked, 0, sizeof blocked);
            for (i = 0; i < input->count; i++) {
                if (cells[i] == input->goal[i]) {
                    blocked[cells[i]] = 1; /* 到站的人是固定障碍 */
                }
            }
            for (i = 0; i < input->count; i++) {
                (void)kos_pibt_distance_blocked(pibt, input->goal[i], blocked,
                                                live_dist[i],
                                                KOS_KITCHEN_MAX_CELLS);
            }
        }
        for (i = 0; i < input->count; i++) {
            if (cells[i] == input->goal[i]) {
                anchor[i] = step; /* 到站清零；不再被推开 */
            }
            memset(&agents[i], 0, sizeof agents[i]);
            agents[i].chef_id = i;
            agents[i].cell = cells[i];
            agents[i].goal = input->goal[i];
            agents[i].priority = (step - anchor[i]) + 1;
            agents[i].pinned = 0;
            agents[i].goal_static = 1;
            agents[i].distance = live_dist[i];
        }
        if (kos_pibt_step(pibt, agents, input->count, next) != KOS_OK) {
            return -1;
        }
        for (i = 0; i < input->count; i++) {
            int j;

            if (next[i] < 0 || next[i] >= kitchen->cell_count) {
                return -1;
            }
            if (next[i] != cells[i] && !kos_nav_adjacent(kitchen, cells[i], next[i])) {
                return -1;
            }
            for (j = 0; j < i; j++) {
                if (next[i] == next[j]) {
                    return -1; /* 顶点冲突 */
                }
                if (next[i] == cells[j] && next[j] == cells[i]) {
                    return -1; /* 对换 */
                }
            }
            hash = (hash ^ (uint64_t)(uint32_t)next[i]) * 1099511628211ull;
        }
        for (i = 0; i < input->count; i++) {
            cells[i] = next[i];
        }
        out->steps = step + 1;
    }
    for (i = 0; i < input->count; i++) {
        if (cells[i] != input->goal[i]) {
            break;
        }
    }
    if (i == input->count) {
        out->arrived = 1;
    }
    out->hash = hash;
    for (i = 0; i < input->count; i++) {
        out->final_cell[i] = cells[i];
    }
    return 0;
}

static int run_pa_trial(kos_pa *pa, const trial_result *input, int max_steps,
                        trial_result *out)
{
    uint8_t blocked[KOS_KITCHEN_MAX_CELLS];
    kos_pa_agent agents[KOS_PA_MAX_AGENTS];
    int cells[KOS_PA_MAX_AGENTS];
    int goal[KOS_PA_MAX_AGENTS];
    int paths[KOS_PA_MAX_AGENTS][KOS_PA_MAX_HORIZON];
    int lengths[KOS_PA_MAX_AGENTS];
    int anchor[KOS_PA_MAX_AGENTS];
    int step = 0;
    int i;
    uint64_t hash = 1469598103934665603ull;

    *out = *input;
    out->arrived = 0;
    out->steps = 0;
    out->hash = 0;
    for (i = 0; i < input->count; i++) {
        cells[i] = input->start[i];
        goal[i] = input->goal[i];
        anchor[i] = 0;
    }
    for (;;) {
        int all_at_goal = 1;
        int longest = 0;
        int t;

        for (i = 0; i < input->count; i++) {
            if (cells[i] != goal[i]) {
                all_at_goal = 0;
            }
        }
        if (all_at_goal) {
            out->arrived = 1;
            break;
        }
        if (step >= max_steps) {
            break;
        }
        memset(blocked, 0, sizeof blocked);
        for (i = 0; i < input->count; i++) {
            if (cells[i] == goal[i]) {
                blocked[cells[i]] = 1;
                anchor[i] = step;
            }
            memset(&agents[i], 0, sizeof agents[i]);
            agents[i].chef_id = i;
            agents[i].cell = cells[i];
            agents[i].goal = goal[i];
            agents[i].priority = (step - anchor[i]) + 1;
            agents[i].pinned = 0;
            agents[i].goal_static = 1;
        }
        if (kos_pa_plan(pa, agents, input->count, KOS_PA_MAX_HORIZON, blocked,
                        lengths, paths) != KOS_OK) {
            return -1;
        }
        for (i = 0; i < input->count; i++) {
            if (lengths[i] > longest) {
                longest = lengths[i];
            }
        }
        if (longest == 0) {
            break; /* 谁都动不了：不是冲突，只是这组输入走不动了 */
        }
        for (t = 0; t < longest && step < max_steps; t++) {
            int next[KOS_PA_MAX_AGENTS];

            for (i = 0; i < input->count; i++) {
                int j;

                next[i] = t < lengths[i] ? paths[i][t] : cells[i];
                if (next[i] < 0 || next[i] >= pa->cell_count) {
                    return -1;
                }
                if (next[i] != cells[i] &&
                    !kos_nav_adjacent(pa->kitchen, cells[i], next[i])) {
                    return -1;
                }
                for (j = 0; j < i; j++) {
                    if (next[i] == next[j]) {
                        return -1; /* 顶点冲突 */
                    }
                    if (next[i] == cells[j] && next[j] == cells[i]) {
                        return -1; /* 对换 */
                    }
                }
                hash = (hash ^ (uint64_t)(uint32_t)next[i]) * 1099511628211ull;
            }
            for (i = 0; i < input->count; i++) {
                cells[i] = next[i];
            }
            step++;
            for (i = 0; i < input->count; i++) {
                if (cells[i] != goal[i]) {
                    break;
                }
            }
            if (i == input->count) {
                out->arrived = 1;
                break;
            }
        }
        if (out->arrived) {
            break;
        }
    }
    out->steps = step;
    out->hash = hash;
    for (i = 0; i < input->count; i++) {
        out->final_cell[i] = cells[i];
    }
    return 0;
}

static void test_random_maps(void)
{
    static const char *const scenes[] = {
        "s_wellcooked_demo_1",
        "s_wellcooked_demo_2",
        "s_wellcooked_demo_3",
        "s_wellcooked_demo_4",
    };
    size_t scene_index;

    for (scene_index = 0; scene_index < sizeof scenes / sizeof scenes[0];
         scene_index++) {
        static kos_kitchen kitchen;
        static kos_pibt pibt;
        static kos_pa pa;
        int component_cells[KOS_KITCHEN_MAX_CELLS];
        int component_ids[KOS_KITCHEN_MAX_CELLS];
        int component_sizes[KOS_KITCHEN_MAX_CELLS];
        int component_count = 0;
        int trial;
        int failed_mask = 0;
        int failed_conflict = 0;
        int failed_arrival = 0;
        int failed_pa = 0;
        int pa_arrived = 0;
        char label[192];

        printf("[test_pibt] %s\n", scenes[scene_index]);
        snprintf(label, sizeof label, "%s: kitchen + pibt 初始化", scenes[scene_index]);
        expect(load_map(scenes[scene_index], &kitchen, &pibt) == KOS_OK, label);
        if (g_failures > 0 && kitchen.cell_count == 0) {
            continue;
        }
        snprintf(label, sizeof label, "%s: pa 初始化", scenes[scene_index]);
        expect(kos_pa_init(&pa, &kitchen) == KOS_OK, label);
        memset(component_sizes, 0, sizeof component_sizes);
        for (trial = 0; trial < kitchen.cell_count; trial++) {
            int comp = kitchen.stand.component[trial];
            int i;
            int found = -1;

            for (i = 0; i < component_count; i++) {
                if (component_ids[i] == comp) {
                    found = i;
                    break;
                }
            }
            if (found < 0) {
                component_ids[component_count++] = comp;
                found = component_count - 1;
            }
            component_sizes[found]++;
        }
        (void)component_cells;

        for (trial = 0; trial < 100; trial++) {
            trial_result input;
            trial_result first;
            trial_result second;
            int chef_count = 2 + (int)(rng_next() % 3u);
            int component;
            int used[KOS_PIBT_MAX_AGENTS * 2];
            int i;
            int rc;

            do {
                component = component_ids[rng_next() % (uint32_t)component_count];
            } while (component < 0);
            memset(&input, 0, sizeof input);
            input.count = chef_count;
            for (i = 0; i < chef_count * 2; i++) {
                int cell;

                if (pick_distinct(&kitchen, component, used, i, &cell) != 0) {
                    break;
                }
                used[i] = cell;
                if (i < chef_count) {
                    input.start[i] = cell;
                } else {
                    input.goal[i - chef_count] = cell;
                }
            }
            if (i < chef_count * 2) {
                continue;
            }
            for (i = 0; i < chef_count; i++) {
                if (kos_pibt_distance(&pibt, input.goal[i], input.dist[i],
                                      KOS_KITCHEN_MAX_CELLS) != KOS_OK) {
                    failed_mask++;
                }
            }
            rc = run_trial(&kitchen, &pibt, &input, 400, &first);
            if (rc != 0) {
                failed_mask++;
                continue;
            }
            if (strcmp(scenes[scene_index], "s_wellcooked_demo_3") == 0 &&
                (!first.arrived || first.steps > 200)) {
                failed_arrival++;
                printf("       200 步未到达: trial=%d chefs=%d arrived=%d steps=%d",
                       trial, chef_count, first.arrived, first.steps);
                for (i = 0; i < chef_count; i++) {
                    printf(" [%d->%d final %d]", input.start[i], input.goal[i],
                           first.final_cell[i]);
                }
                printf("\n");
            }
            rc = run_trial(&kitchen, &pibt, &input, 400, &second);
            if (rc != 0 || second.hash != first.hash ||
                second.steps != first.steps) {
                failed_conflict++;
            }
            /* pa 是同优先级顺序的时空 A*：用同样的输入跑两遍，锁住无冲突与确定性。
             * 它不承诺 _3 上一定到达（那是 PIBT 的保证），只统计到达数。 */
            rc = run_pa_trial(&pa, &input, 200, &first);
            if (rc != 0) {
                failed_pa++;
                continue;
            }
            pa_arrived += first.arrived ? 1 : 0;
            rc = run_pa_trial(&pa, &input, 200, &second);
            if (rc != 0 || second.hash != first.hash ||
                second.steps != first.steps) {
                failed_pa++;
            }
        }
        snprintf(label, sizeof label, "%s: 100 组随机起终点每步满足 mask 且无冲突",
                 scenes[scene_index]);
        expect(failed_mask == 0 && failed_conflict == 0,
               failed_mask == 0 && failed_conflict == 0
                   ? label
                   : "随机用例失败：mask/冲突或同输入不一致");
        snprintf(label, sizeof label, "%s: pa 100 组随机起终点每步无冲突且同输入一致",
                 scenes[scene_index]);
        expect(failed_pa == 0,
               failed_pa == 0 ? label
                              : "pa 随机用例失败：越界/冲突或同输入不一致");
        printf("[test_pibt] %s: pa 到达 %d/100 组（200 步内）\n",
               scenes[scene_index], pa_arrived);
        if (strcmp(scenes[scene_index], "s_wellcooked_demo_3") == 0) {
            snprintf(label, sizeof label, "%s: _3 每组 200 步内全部到达", scenes[scene_index]);
            expect(failed_arrival == 0,
                   failed_arrival == 0 ? label : "有随机用例 200 步内没全部到达");
        }
    }
}

static void make_line(kos_kitchen *kitchen)
{
    memset(kitchen, 0, sizeof *kitchen);
    kitchen->step = 0.6f;
    kitchen->cell_count = 3;
    kitchen->cells[0].x = 0.0f;
    kitchen->cells[0].z = 0.0f;
    kitchen->cells[0].mask = KOS_GRID_EDGE_PX;
    kitchen->cells[1].x = 0.6f;
    kitchen->cells[1].z = 0.0f;
    kitchen->cells[1].mask = KOS_GRID_EDGE_NX | KOS_GRID_EDGE_PX;
    kitchen->cells[2].x = 1.2f;
    kitchen->cells[2].z = 0.0f;
    kitchen->cells[2].mask = KOS_GRID_EDGE_NX;
    kitchen->stand.component[0] = 0;
    kitchen->stand.component[1] = 0;
    kitchen->stand.component[2] = 0;
}

static void test_priority_and_pinned(void)
{
    static kos_kitchen kitchen;
    static kos_pibt pibt;
    static uint16_t dist[4][KOS_KITCHEN_MAX_CELLS];
    kos_pibt_agent agents[2];
    int next[2];

    printf("[test_pibt] 2. 优先级、pinned 与推动\n");
    make_line(&kitchen);
    expect(kos_pibt_init(&pibt, &kitchen) == KOS_OK, "三格直线初始化");
    kos_pibt_distance(&pibt, 1, dist[0], KOS_KITCHEN_MAX_CELLS);
    kos_pibt_distance(&pibt, 1, dist[1], KOS_KITCHEN_MAX_CELLS);
    kos_pibt_distance(&pibt, 2, dist[2], KOS_KITCHEN_MAX_CELLS);

    memset(agents, 0, sizeof agents);
    agents[0].chef_id = 0;
    agents[0].cell = 0;
    agents[0].goal = 1;
    agents[0].priority = 5;
    agents[0].distance = dist[0];
    agents[1].chef_id = 1;
    agents[1].cell = 2;
    agents[1].goal = 1;
    agents[1].priority = 5;
    agents[1].distance = dist[1];
    expect(kos_pibt_step(&pibt, agents, 2, next) == KOS_OK && next[0] == 1 && next[1] == 2,
           "并列优先级 chef_id 小者先");

    agents[0].priority = 1;
    agents[1].priority = 5;
    expect(kos_pibt_step(&pibt, agents, 2, next) == KOS_OK && next[0] == 0 && next[1] == 1,
           "优先级大者先");

    agents[0].cell = 0;
    agents[0].goal = 2;
    agents[0].priority = 5;
    agents[0].distance = dist[2];
    agents[1].cell = 1;
    agents[1].goal = 1;
    agents[1].priority = 1;
    agents[1].pinned = 1;
    agents[1].distance = dist[0];
    expect(kos_pibt_step(&pibt, agents, 2, next) == KOS_OK && next[0] == 0 && next[1] == 1,
           "pinned 当障碍不推");

    agents[0].cell = 0;
    agents[0].goal = 2;
    agents[0].priority = 5;
    agents[0].distance = dist[2];
    agents[1].cell = 1;
    agents[1].goal = 1;
    agents[1].priority = 0;
    agents[1].pinned = 0;
    agents[1].distance = dist[0];
    expect(kos_pibt_step(&pibt, agents, 2, next) == KOS_OK && next[0] == 1 && next[1] == 2,
           "没任务的人被推开让路");
}

int main(void)
{
    printf("[test_pibt] 开始：PIBT 规划\n");
    test_random_maps();
    test_priority_and_pinned();
    printf("[test_pibt] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
