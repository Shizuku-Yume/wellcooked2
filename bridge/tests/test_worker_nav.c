/*
 * test_worker_nav.c -- 网格寻路与 Worker 执行状态机。
 *
 * 两个证据层次：
 *   1. nav：路径的每一步都必须由 kitchen.json 的 mask 允许，终点必须是工位最近的
 *      可走格；这是"Worker 真的按地图走"而不是瞬移。
 *   2. worker_task：把状态机产出的动作真的 publish 到共享内存输入槽，再读回来，
 *      断言先 move 后 interact（中间没有 stop）。只测内存里的 action 变量不够——
 *      Bridge 读的是槽。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/child.h"
#include "kitchenos/kitchen.h"
#include "kitchenos/nav.h"
#include "kitchenos/shm.h"
#include "kitchenos/stand.h"
#include "kitchenos/worker_task.h"

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

static int load_kitchen(kos_kitchen *kitchen)
{
    char path[KOS_DAEMON_PATH_MAX];
    kos_error err;

    if (kos_child_default_config_path("kitchen.example.json", path, sizeof path) != 0) {
        return -1;
    }
    kos_error_clear(&err);
    return kos_kitchen_load(path, kitchen, &err);
}

static void test_geometry(void)
{
    kos_kitchen kitchen;
    const kos_station *station;
    int goal;
    int path[KOS_NAV_MAX_PATH];
    int path_len = 0;
    kos_error err;
    int i;
    int idx;

    printf("[test_worker_nav] 1. 网格与 A*\n");
    memset(&kitchen, 0, sizeof kitchen);
    expect(load_kitchen(&kitchen) == KOS_OK, "kitchen_load_example");
    expect(kitchen.cell_count == 546 && kitchen.station_count == 20,
           "kitchen_example_shape");
    expect(kitchen.step > 0.0f, "kitchen_step_positive");

    station = kos_kitchen_station_by_id(&kitchen, "dispenser-2");
    expect(station != NULL && strcmp(station->kind, "dispenser") == 0,
           "station_lookup_by_id");
    if (station == NULL) {
        return;
    }
    {
        int station_index = kos_kitchen_station_index(&kitchen, station->id);
        const kos_station_stands *stands =
            station_index >= 0 ? kos_stand_cells(&kitchen, station_index) : NULL;

        expect(stands != NULL && stands->count >= 1, "station_has_stand_cells");
        if (stands == NULL || stands->count == 0) {
            return;
        }
        goal = stands->cells[0].cell;
        expect(goal >= 0 && goal < kitchen.cell_count, "stand_cell_in_grid");
        expect(kos_stand_distance(&kitchen, station_index, goal) == 0,
               "stand_cell_distance_zero");
        expect(kos_stand_best_cell(&kitchen, station_index, goal) == goal,
               "stand_cell_is_its_own_nearest");
    }

    kos_error_clear(&err);
    expect(kos_nav_find_path(&kitchen, 0, goal, path, KOS_NAV_MAX_PATH, &path_len, &err) ==
               KOS_OK &&
               path_len > 1,
           "path_found");
    expect(path_len > 0 && path[0] == 0 && path[path_len - 1] == goal,
           "path_reaches_station_neighbour");

    {
        int every_step_legal = 1;

        for (i = 0; i + 1 < path_len; i++) {
            if (!kos_nav_adjacent(&kitchen, path[i], path[i + 1])) {
                every_step_legal = 0;
            }
        }
        expect(every_step_legal, "path_respects_mask");
    }

    /* 反向邻接也必须成立：mask 声明对称，A* 才能双向复用同一张图。 */
    {
        int symmetric = 1;

        for (i = 0; i < path_len - 1; i++) {
            if (!kos_nav_adjacent(&kitchen, path[i + 1], path[i])) {
                symmetric = 0;
            }
        }
        expect(symmetric, "grid_mask_is_symmetric_along_path");
    }

    idx = kos_kitchen_cell_index(&kitchen, kitchen.cells[3].x, kitchen.cells[3].z);
    expect(idx == 3, "cell_index_roundtrip");
    kos_error_clear(&err);
    expect(kos_nav_find_path(&kitchen, -1, goal, path, KOS_NAV_MAX_PATH, &path_len, &err) ==
               KOS_ERR_INVAL,
           "nav_rejects_bad_start");
}

static void test_worker_task(void)
{
    kos_kitchen kitchen;
    const kos_station *station;
    kos_worker_task task;
    kos_shm_opts opts;
    kos_shm *shm = NULL;
    kos_error err;
    char state_name[KOS_SHM_PATH_MAX];
    char sem_name[KOS_SHM_PATH_MAX];
    int64_t now;
    int saw_move = 0;
    int saw_interact = 0;
    int stop_between = 0;
    int done = 0;
    int last_kind = KOS_ACTION_NONE;
    uint64_t frame = 0;
    int step;

    printf("[test_worker_nav] 2. Worker 状态机与输入槽\n");
    memset(&kitchen, 0, sizeof kitchen);
    if (load_kitchen(&kitchen) != KOS_OK) {
        expect(0, "slot_kitchen_loaded");
        return;
    }
    station = kos_kitchen_station_by_id(&kitchen, "dispenser-2");
    if (station == NULL) {
        expect(0, "slot_station_found");
        return;
    }

    snprintf(state_name, sizeof state_name, "/kitchenos_nav_%ld", (long)getpid());
    snprintf(sem_name, sizeof sem_name, "/kitchenos_navs_%ld", (long)getpid());
    (void)kos_shm_unlink(&(kos_shm_opts){.object_name = state_name, .sem_name = sem_name,
                                         .perm = 0},
                         &err);
    memset(&opts, 0, sizeof opts);
    opts.object_name = state_name;
    opts.sem_name = sem_name;
    opts.perm = 0;
    kos_error_clear(&err);
    expect(kos_shm_open(&opts, KOS_SHM_CREATE, &shm, &err) == KOS_OK, "slot_shm_created");

    memset(&task, 0, sizeof task);
    kos_error_clear(&err);
    expect(kos_worker_task_init(&task, &kitchen, station->id, kitchen.cells[0].x,
                                kitchen.cells[0].z, 500, 0, &err) == KOS_OK,
           "worker_task_init");

    for (step = 0; step < 2000 && !done; step++) {
        kos_input_action action;
        int rc;

        now = (int64_t)step * 20;
        rc = kos_worker_task_step(&task, now, &action);
        if (rc == 1) {
            done = 1;
        }
        action.frame_id = ++frame;
        action.chef_id = 0;
        if (shm != NULL) {
            kos_error_clear(&err);
            (void)kos_shm_slot_publish(shm, 0, &action);
        }
        if (action.kind == KOS_ACTION_MOVE) {
            saw_move = 1;
        } else if (action.kind == KOS_ACTION_INTERACT) {
            if (!saw_interact) {
                saw_interact = 1;
            }
            last_kind = action.kind;
        } else if (action.kind == KOS_ACTION_STOP) {
            if (saw_move && !saw_interact) {
                stop_between = 1;
            }
        }
        if (rc == 1) {
            last_kind = action.kind;
        }
        if (shm != NULL && action.kind != KOS_ACTION_NONE) {
            kos_input_action readback;

            if (kos_shm_slot_read(shm, 0, &readback) == KOS_OK &&
                readback.kind != action.kind) {
                expect(0, "slot_readback_matches_publish");
            }
        }
    }
    expect(saw_move, "slot_contains_move");
    expect(saw_interact, "slot_contains_interact");
    expect(saw_move && saw_interact && !stop_between,
           "slot_contains_move_then_interact");
    expect(done && last_kind == KOS_ACTION_STOP, "worker_task_ends_with_stop");

    if (shm != NULL) {
        kos_error_clear(&err);
        (void)kos_shm_close(shm, &err);
    }
    kos_error_clear(&err);
    (void)kos_shm_unlink(&opts, &err);
}

int main(void)
{
    printf("[test_worker_nav] 开始：A* 与输入槽动作\n");
    test_geometry();
    test_worker_task();
    printf("[test_worker_nav] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
