/*
 * worker_task.c -- move → interact → work → done 的小状态机。
 *
 * 移动速度固定 3 m/s，交互 200 ms；这些是"执行器"的常量，不是游戏物理。真实游戏
 * 会拒绝走过头/交互失败，那类反馈要等真机阶段，不能在这里假造成功语义。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/worker_task.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <kitchenos/stand.h>

#define WORKER_SPEED_MM_S 3000
#define WORKER_INTERACT_MS 200
#define WORKER_ARRIVE_EPS 0.02f

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if (cap == 0) {
        return;
    }
    if (src != NULL) {
        while (i + 1 < cap && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

const char *kos_worker_phase_name(int phase)
{
    switch (phase) {
    case KOS_WORKER_MOVING:
        return "moving";
    case KOS_WORKER_INTERACTING:
        return "interacting";
    case KOS_WORKER_WORKING:
        return "working";
    case KOS_WORKER_DONE:
        return "done";
    case KOS_WORKER_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static void fill_move(kos_input_action *action, const kos_worker_task *task, int cell)
{
    memset(action, 0, sizeof *action);
    action->kind = KOS_ACTION_MOVE;
    action->target_x = task->kitchen->cells[cell].x;
    action->target_y = task->kitchen->cells[cell].z;
}

static void fill_interact(kos_input_action *action, const kos_worker_task *task)
{
    memset(action, 0, sizeof *action);
    action->kind = KOS_ACTION_INTERACT;
    copy_str(action->target_object, sizeof action->target_object, task->station_id);
}

static void fill_stop(kos_input_action *action)
{
    memset(action, 0, sizeof *action);
    action->kind = KOS_ACTION_STOP;
}

int kos_worker_task_init(kos_worker_task *task, const kos_kitchen *kitchen,
                         const char *station_id, float start_x, float start_z,
                         int duration_ms, int64_t now_ms, kos_error *err)
{
    int start_cell;
    int station_index;
    int goal_cell;
    int rc;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (task == NULL || kitchen == NULL || station_id == NULL) {
        return KOS_ERR_INVAL;
    }
    memset(task, 0, sizeof *task);
    task->kitchen = kitchen;
    copy_str(task->station_id, sizeof task->station_id, station_id);
    task->duration_ms = duration_ms > 0 ? duration_ms : 100;
    task->interact_ms = WORKER_INTERACT_MS;
    task->speed_mm_s = WORKER_SPEED_MM_S;
    task->last_ms = now_ms;
    task->x = start_x;
    task->z = start_z;

    station_index = kos_kitchen_station_index(kitchen, station_id);
    if (station_index < 0) {
        if (err != NULL) {
            err->code = KOS_ERR_MISSING;
            snprintf(err->context, sizeof err->context, "未知工位 %s", station_id);
        }
        task->phase = KOS_WORKER_FAILED;
        return KOS_ERR_MISSING;
    }
    start_cell = kos_kitchen_nearest_cell(kitchen, start_x, start_z);
    if (start_cell < 0) {
        if (err != NULL) {
            err->code = KOS_ERR_EMPTY;
            snprintf(err->context, sizeof err->context, "厨房网格为空");
        }
        task->phase = KOS_WORKER_FAILED;
        return KOS_ERR_EMPTY;
    }
    goal_cell = kos_stand_best_cell(kitchen, station_index, start_cell);
    if (goal_cell < 0) {
        if (err != NULL) {
            err->code = KOS_ERR_EMPTY;
            snprintf(err->context, sizeof err->context,
                     "工位 %s 没有与厨师同一连通区的站位格", station_id);
        }
        task->phase = KOS_WORKER_FAILED;
        return KOS_ERR_EMPTY;
    }
    task->target_cell = goal_cell;
    task->x = kitchen->cells[start_cell].x;
    task->z = kitchen->cells[start_cell].z;
    rc = kos_nav_find_path(kitchen, start_cell, goal_cell, task->path, KOS_NAV_MAX_PATH,
                           &task->path_len, err);
    if (rc != KOS_OK) {
        task->phase = KOS_WORKER_FAILED;
        return rc;
    }
    task->path_index = 0;
    if (task->path_len <= 1) {
        task->phase = KOS_WORKER_INTERACTING;
        task->phase_end_ms = now_ms + task->interact_ms;
    } else {
        task->phase = KOS_WORKER_MOVING;
    }
    return KOS_OK;
}

int kos_worker_task_step(kos_worker_task *task, int64_t now_ms, kos_input_action *action)
{
    float budget;

    if (task == NULL || action == NULL || task->kitchen == NULL) {
        return -1;
    }
    if (task->phase == KOS_WORKER_DONE) {
        fill_stop(action);
        return 1;
    }
    if (task->phase == KOS_WORKER_FAILED) {
        fill_stop(action);
        return -1;
    }

    if (now_ms < task->last_ms) {
        now_ms = task->last_ms;
    }

    if (task->phase == KOS_WORKER_MOVING) {
        budget = (float)(now_ms - task->last_ms) * (float)task->speed_mm_s / 1000000.0f;

        while (budget > 0.0f && task->path_index + 1 < task->path_len) {
            int next = task->path[task->path_index + 1];
            float dx = task->kitchen->cells[next].x - task->x;
            float dz = task->kitchen->cells[next].z - task->z;
            float dist = sqrtf(dx * dx + dz * dz);

            if (dist <= WORKER_ARRIVE_EPS) {
                task->x = task->kitchen->cells[next].x;
                task->z = task->kitchen->cells[next].z;
                task->path_index++;
                continue;
            }
            if (dist <= budget) {
                task->x = task->kitchen->cells[next].x;
                task->z = task->kitchen->cells[next].z;
                task->path_index++;
                budget -= dist;
                continue;
            }
            task->x += dx / dist * budget;
            task->z += dz / dist * budget;
            budget = 0.0f;
        }
        task->last_ms = now_ms;

        if (task->path_index + 1 >= task->path_len) {
            task->phase = KOS_WORKER_INTERACTING;
            task->phase_end_ms = now_ms + task->interact_ms;
            fill_interact(action, task);
            return 0;
        }
        fill_move(action, task, task->path[task->path_index + 1]);
        return 0;
    }

    task->last_ms = now_ms;
    if (task->phase == KOS_WORKER_INTERACTING) {
        fill_interact(action, task);
        if (now_ms >= task->phase_end_ms) {
            task->phase = KOS_WORKER_WORKING;
            task->phase_end_ms = now_ms + task->duration_ms;
        }
        return 0;
    }
    if (task->phase == KOS_WORKER_WORKING) {
        fill_stop(action);
        if (now_ms >= task->phase_end_ms) {
            task->phase = KOS_WORKER_DONE;
            return 1;
        }
        return 0;
    }
    fill_stop(action);
    return -1;
}
