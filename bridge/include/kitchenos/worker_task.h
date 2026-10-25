/*
 * worker_task.h -- 一个工序在 Worker 里的执行状态机（模块契约）。
 *
 * 与真实时间解耦：调用方给 now_ms，所以测试可以在几微秒里跑完"走 10 格 + 交互
 * + 工作 1 秒"。状态机只产出 kos_input_action（move/interact/stop），写共享内存与
 * 发事件是 worker.c 的事。
 */
#ifndef KITCHENOS_WORKER_TASK_H
#define KITCHENOS_WORKER_TASK_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>

#include <kitchenos/kitchen.h>
#include <kitchenos/nav.h>
#include <kitchenos/shm.h>

typedef enum {
    KOS_WORKER_MOVING = 0,
    KOS_WORKER_INTERACTING = 1,
    KOS_WORKER_WORKING = 2,
    KOS_WORKER_DONE = 3,
    KOS_WORKER_FAILED = 4
} kos_worker_phase;

typedef struct {
    const kos_kitchen *kitchen;
    int path[KOS_NAV_MAX_PATH];
    int path_len;
    int path_index;
    int target_cell;
    float x;
    float z;
    int phase;
    int64_t last_ms;
    int64_t phase_end_ms;
    int duration_ms;
    int interact_ms;
    int speed_mm_s;
    char station_id[KOS_KITCHEN_NAME_MAX];
} kos_worker_task;

/*
 * 从 (start_x,start_z) 到工位 station 的站位格规划路径。station 必须存在；目标格由
 * kitchen->stand 的站位格表选出（同一工位有多个时取同一连通区里最近的），不再用
 * "离工位坐标最近的可走格"猜。路径不可达或工位没有站位格时返回错误。
 */
int kos_worker_task_init(kos_worker_task *task, const kos_kitchen *kitchen,
                         const char *station_id, float start_x, float start_z,
                         int duration_ms, int64_t now_ms, kos_error *err);

/*
 * 推进到 now_ms，填充下一步要发布的动作。返回 0 = 还在执行，1 = 完成，-1 = 失败。
 * 完成时 action 是 stop（让厨师站住），调用方发完事件后清任务。
 */
int kos_worker_task_step(kos_worker_task *task, int64_t now_ms, kos_input_action *action);

const char *kos_worker_phase_name(int phase);

#endif /* KITCHENOS_WORKER_TASK_H */
