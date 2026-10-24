/*
 * nav.h -- 厨房网格上的 A*（模块契约）。
 *
 * 输入只有 kitchen.h 的网格与两个下标，输出是格子下标序列。不读 shm、不认识
 * 厨师和工位——把"到哪个工位"翻译成目标格是 worker_task 的事。
 */
#ifndef KITCHENOS_NAV_H
#define KITCHENOS_NAV_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/kitchen.h>
#include <kitchenos/shm.h>

#define KOS_NAV_MAX_PATH KOS_KITCHEN_MAX_CELLS

/* 两格是否由网格掩码直接相连（距离必须正好一个 step）。 */
int kos_nav_adjacent(const kos_kitchen *kitchen, int from, int to);

/*
 * A*：路径包含起点与终点。成功返回 0 并把长度写进 *path_len；无可达路径返回
 * KOS_ERR_EMPTY；参数/缓冲区错误返回 KOS_ERR_INVAL / KOS_ERR_RANGE。
 * 平局按格子下标走，所以同一张图永远给同一条路径（便于复现与测试）。
 */
int kos_nav_find_path(const kos_kitchen *kitchen, int start, int goal, int *path,
                      int path_cap, int *path_len, kos_error *err);

#endif /* KITCHENOS_NAV_H */
