/*
 * nav.c -- 0.6 m 网格上的 A*。
 *
 * 546 个格子的规模不值得堆/哈希：每轮线性挑 f 最小的开放节点足够快，而且平局按
 * 下标决定，路径天然可复现。邻居只按当前格的 mask 生成，目标格必须存在；mask 与
 * 实际坐标不一致的图会被 path_respects_mask 测试抓出来。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/nav.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static float heuristic(const kos_kitchen *kitchen, int from, int to)
{
    float dx = fabsf(kitchen->cells[from].x - kitchen->cells[to].x);
    float dz = fabsf(kitchen->cells[from].z - kitchen->cells[to].z);
    float step = kitchen->step > 0.0f ? kitchen->step : 1.0f;

    return (dx + dz) / step;
}

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

static int neighbor_index(const kos_kitchen *kitchen, int from, unsigned bit, float step,
                          int *out)
{
    float x = kitchen->cells[from].x;
    float z = kitchen->cells[from].z;
    int index;

    if ((kitchen->cells[from].mask & bit) == 0) {
        return 0;
    }
    if (bit == KOS_GRID_EDGE_PX) {
        x += step;
    } else if (bit == KOS_GRID_EDGE_NX) {
        x -= step;
    } else if (bit == KOS_GRID_EDGE_PZ) {
        z += step;
    } else {
        z -= step;
    }
    index = kos_kitchen_cell_index(kitchen, x, z);
    if (index < 0) {
        return 0;
    }
    *out = index;
    return 1;
}

int kos_nav_find_path(const kos_kitchen *kitchen, int start, int goal, int *path,
                      int path_cap, int *path_len, kos_error *err)
{
    int g[KOS_KITCHEN_MAX_CELLS];
    int parent[KOS_KITCHEN_MAX_CELLS];
    unsigned char closed[KOS_KITCHEN_MAX_CELLS];
    unsigned char open[KOS_KITCHEN_MAX_CELLS];
    int open_count = 0;
    int i;
    int current;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (kitchen == NULL || path == NULL || path_len == NULL || path_cap <= 0 ||
        start < 0 || goal < 0 || start >= kitchen->cell_count ||
        goal >= kitchen->cell_count) {
        if (err != NULL) {
            err->code = KOS_ERR_INVAL;
            snprintf(err->context, sizeof err->context, "kos_nav_find_path 参数不合法");
        }
        return KOS_ERR_INVAL;
    }
    *path_len = 0;
    for (i = 0; i < kitchen->cell_count; i++) {
        g[i] = -1;
        parent[i] = -1;
        closed[i] = 0;
        open[i] = 0;
    }

    g[start] = 0;
    open[start] = 1;
    open_count = 1;

    while (open_count > 0) {
        int best = -1;
        float best_f = 0.0f;

        for (i = 0; i < kitchen->cell_count; i++) {
            float f;

            if (!open[i] || closed[i]) {
                continue;
            }
            f = (float)g[i] + heuristic(kitchen, i, goal);
            if (best < 0 || f < best_f || (f == best_f && i < best)) {
                best = i;
                best_f = f;
            }
        }
        if (best < 0) {
            break;
        }
        current = best;
        open[current] = 0;
        closed[current] = 1;
        open_count--;
        if (current == goal) {
            break;
        }

        {
            static const unsigned bits[] = {KOS_GRID_EDGE_PX, KOS_GRID_EDGE_NX,
                                            KOS_GRID_EDGE_PZ, KOS_GRID_EDGE_NZ};
            size_t b;

            for (b = 0; b < sizeof bits / sizeof bits[0]; b++) {
                int next;

                if (!neighbor_index(kitchen, current, bits[b], kitchen->step, &next) ||
                    closed[next]) {
                    continue;
                }
                if (g[next] < 0 || g[current] + 1 < g[next]) {
                    g[next] = g[current] + 1;
                    parent[next] = current;
                    if (!open[next]) {
                        open[next] = 1;
                        open_count++;
                    }
                }
            }
        }
    }

    if (!closed[goal] || g[goal] < 0) {
        return KOS_ERR_EMPTY;
    }

    /* 反向走 parent 得到长度，再正向填。 */
    {
        int length = 0;
        int node = goal;

        while (node >= 0) {
            length++;
            if (node == start) {
                break;
            }
            node = parent[node];
        }
        if (length > path_cap) {
            if (err != NULL) {
                err->code = KOS_ERR_RANGE;
                snprintf(err->context, sizeof err->context,
                         "路径长度 %d 超过缓冲区 %d", length, path_cap);
            }
            return KOS_ERR_RANGE;
        }
        node = goal;
        for (i = length - 1; i >= 0; i--) {
            path[i] = node;
            node = parent[node];
        }
        *path_len = length;
    }
    return KOS_OK;
}
