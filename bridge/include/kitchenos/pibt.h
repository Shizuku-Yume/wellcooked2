/*
 * pibt.h -- 一步 PIBT（Priority Inheritance with Backtracking）规划（模块契约）。
 *
 * 只算"这一步每个人去哪个格"，不读 shm、不认识工位。调用方（navd 的测试与
 * 计划窗口）维护计划队列；distance 是每个厨师到目标的距离场（stand.distance
 * 或 kos_pibt_distance 生成的 BFS 表），候选格按它从近到远试。
 *
 * 优先级由调用方算好：换目标后经过的步数 + 紧迫度，大的先；并列按 chef_id
 * 小的先。pinned 的厨师不动也不被推。被更高优先级要格子的厨师会继承优先级先
 * 找下一格；找不到就让对方回退换候选，所有候选都不行就原地不动。
 */
#ifndef KITCHENOS_PIBT_H
#define KITCHENOS_PIBT_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>

#include <kitchenos/kitchen.h>

#define KOS_PIBT_MAX_AGENTS KOS_SHM_MAX_CHEFS

typedef struct {
    const kos_kitchen *kitchen;
    int cell_count;
    uint8_t degree[KOS_KITCHEN_MAX_CELLS];
    int neighbor[KOS_KITCHEN_MAX_CELLS][4];
} kos_pibt;

typedef struct {
    int chef_id;              /* 并列优先级的先后；必须唯一 */
    int cell;                 /* 当前所在格 */
    int goal;                 /* 目标格；没任务时调用方传 cell */
    int priority;             /* 大的先；navd 用 步数 + 紧迫度 */
    int pinned;               /* 正在工位干活：不动，也不让推 */
    int goal_static;          /* 有目标的厨师已到站：当障碍，不再被推开 */
    const uint16_t *distance; /* cell_count 项；KOS_STAND_UNREACHABLE = 不可达 */
} kos_pibt_agent;

/* 建邻居表（每张图一次）。 */
int kos_pibt_init(kos_pibt *pibt, const kos_kitchen *kitchen);

/*
 * 跑一步。next_cells[i] 对应 agents[i]，只可能是 cell 本身或 mask 允许的邻格。
 * 成功返回 0；参数错误/没有 distance 返回 KOS_ERR_INVAL。
 */
int kos_pibt_step(kos_pibt *pibt, const kos_pibt_agent *agents, int count,
                  int *next_cells);

/* 从 goal 出发的 BFS 距离场，不可达为 KOS_STAND_UNREACHABLE。 */
int kos_pibt_distance(const kos_pibt *pibt, int goal, uint16_t *distance, int cap);

/*
 * 同上，但把 blocked[] 里非 0 的格当作障碍（源点除外）。已到站/正在干活的人
 * 占着的格就是障碍：不绕开的话，最近邻被挡住时会在两个格之间来回走。
 */
int kos_pibt_distance_blocked(const kos_pibt *pibt, int goal, const uint8_t *blocked,
                              uint16_t *distance, int cap);

#endif /* KITCHENOS_PIBT_H */
