/*
 * nav.c -- 网格相邻判定。
 *
 * mask 与坐标必须同时成立：mask 说"这边通"，坐标也必须正好差一个 step。stand.c
 * 建距离场、pibt.c 建邻居表都用这一条；两边数据不一致时测试会直接抓出来。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <wellcooked/nav.h>

#include <math.h>

int kos_nav_adjacent(const kos_kitchen *kitchen, int from, int to)
{
    const kos_grid_cell *a;
    const kos_grid_cell *b;
    float dx;
    float dz;

    if (kitchen == NULL || from < 0 || to < 0 || from >= kitchen->cell_count ||
        to >= kitchen->cell_count) {
        return 0;
    }
    a = &kitchen->cells[from];
    b = &kitchen->cells[to];
    dx = b->x - a->x;
    dz = b->z - a->z;
    if (fabsf(dx) < 1e-3f && fabsf(dz) < 1e-3f) {
        return 0;
    }
    if (fabsf(dx) > 1e-3f && fabsf(dz) > 1e-3f) {
        return 0;
    }
    if (fabsf(dx) > kitchen->step + 1e-3f || fabsf(dz) > kitchen->step + 1e-3f) {
        return 0;
    }
    if (dx > 0.0f) {
        return (a->mask & KOS_GRID_EDGE_PX) != 0;
    }
    if (dx < 0.0f) {
        return (a->mask & KOS_GRID_EDGE_NX) != 0;
    }
    if (dz > 0.0f) {
        return (a->mask & KOS_GRID_EDGE_PZ) != 0;
    }
    return (a->mask & KOS_GRID_EDGE_NZ) != 0;
}
