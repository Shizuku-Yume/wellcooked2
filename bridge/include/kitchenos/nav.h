/*
 * nav.h -- 厨房网格的相邻判定（模块契约）。
 *
 * 单厨师 A* 已随 Worker 的旧执行路径删除：走位统一由 navd 的 PIBT 规划（pibt.h），
 * 按执行依赖图放行（adg.h）。这里只剩 stand.c 建距离场和 pibt.c 建邻居表都要用的
 * "mask 是否允许从 from 到 to"。
 */
#ifndef KITCHENOS_NAV_H
#define KITCHENOS_NAV_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/kitchen.h>

/* 两格是否由网格掩码直接相连（距离必须正好一个 step）。 */
int kos_nav_adjacent(const kos_kitchen *kitchen, int from, int to);

#endif /* KITCHENOS_NAV_H */
