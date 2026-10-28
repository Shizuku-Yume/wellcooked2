/*
 * pa.h -- 优先级时空 A*（对照基线）规划（模块契约）。
 *
 * 与 pibt.h 同一层：只算"接下来的路点"，不读 shm、不认识工位。navd 用同一个
 * 目标/优先级调用它，放行仍走执行依赖图（adg.h）。预约表是 kos_pa 里的内存
 * 数组，不新增共享内存：一局只由 navd 一个进程写。
 *
 * 规划顺序与 PIBT 相同：已到目标的人优先级清零，其余按 (priority, chef_id)。
 * 依次为每个人在时间展开图上做 A*（四邻域 + 原地等待），状态 (格, 时间)；
 * 已经规划过的人按时间占住格子（含停留到最后一步之后），后来者避开这些格与
 * 对换。还没轮到的人不预约当前的格：高优先级的人可以规划经过它，轮到它时再
 * 改道。终点必须是"后续时刻没人用"的格（计划走完人就停在那里），因此换道
 * 可能比 PIBT 更保守；这是对照基线，不承担 PIBT 的可达性保证。
 */
#ifndef WELLCOOKED_PA_H
#define WELLCOOKED_PA_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>

#include <wellcooked/kitchen.h>
#include <wellcooked/shm.h>

#define KOS_PA_MAX_AGENTS KOS_SHM_MAX_CHEFS
#define KOS_PA_MAX_HORIZON 8

typedef struct {
    int chef_id;       /* 并列优先级的先后；必须唯一 */
    int cell;          /* 当前所在格 */
    int goal;          /* 目标格；没任务时调用方传 cell */
    int priority;      /* 大的先；navd 用 步数 + 紧迫度 */
    int pinned;        /* 正在工位干活：固定障碍 */
    int goal_static;   /* 有目标：到站后当固定障碍，不再被推开 */
} kos_pa_agent;

typedef struct {
    const kos_kitchen *kitchen;
    int cell_count;
    uint8_t degree[KOS_KITCHEN_MAX_CELLS];
    int neighbor[KOS_KITCHEN_MAX_CELLS][4];

    /* 以下数组只在本进程内使用，plan() 每次调用重置。 */
    uint8_t reserved[KOS_PA_MAX_HORIZON + 1][KOS_KITCHEN_MAX_CELLS];
    int32_t parent[(KOS_PA_MAX_HORIZON + 1) * KOS_KITCHEN_MAX_CELLS];
    uint8_t seen[(KOS_PA_MAX_HORIZON + 1) * KOS_KITCHEN_MAX_CELLS];
    int32_t heap_state[(KOS_PA_MAX_HORIZON + 1) * KOS_KITCHEN_MAX_CELLS];
    int heap_f[(KOS_PA_MAX_HORIZON + 1) * KOS_KITCHEN_MAX_CELLS];
    uint16_t heuristic[KOS_KITCHEN_MAX_CELLS];
} kos_pa;

/* 建邻居表（每张图一次）。 */
int kos_pa_init(kos_pa *pa, const kos_kitchen *kitchen);

/*
 * 一次规划推进：按优先级给 count 个厨师各算一条最多 horizon 步的路径。
 * paths 是 [KOS_PA_MAX_AGENTS][KOS_PA_MAX_HORIZON]，第 i 行前 lengths[i] 个
 * 格有效（不含起点；原地等待表现为重复上一格）。blocked[] 里非 0 的格与
 * pinned/到站者一样当固定障碍；可为 NULL。
 *
 * 成功返回 KOS_OK；参数错误返回 KOS_ERR_INVAL。
 */
int kos_pa_plan(kos_pa *pa, const kos_pa_agent *agents, int count, int horizon,
                const uint8_t *blocked, int *lengths,
                int paths[KOS_PA_MAX_AGENTS][KOS_PA_MAX_HORIZON]);

#endif /* WELLCOOKED_PA_H */
