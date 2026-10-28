/*
 * stand.h -- 工位的站位格与全图距离场（模块契约）。
 *
 * 站位格在加载 kitchen.json 时算好（kos_kitchen_load 调 kos_stand_build），下游只读：
 * Worker 用它选走到哪个站位格并转身，navd 用它给候选格排序，Scheduler 用距离场估
 * 路程。工位在台面/墙里，grid.cells 里没有它的位置，所以"朝向工位的 mask 位"必定
 * 是 0；站位格取的是该方向最近的可行格（实测四张地图都是 1.2 m，即两格，正好是游戏
 * 1.2 m 交互格的一步）。
 *
 * 一个工位没有站位格（四周都不可走）时 kos_stand_build 报 KOS_ERR_MISSING，错误里带
 * 工位 id：这种数据错误留到运行时只会表现成"厨师永远走不到工位"。
 */
#ifndef WELLCOOKED_STAND_H
#define WELLCOOKED_STAND_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <wellcooked/kitchen.h>

/*
 * 算全部工位的站位格与距离场，写进 kitchen->stand。
 *
 * 每个方向（+x/-x/+z/-z）取离工位最近、且垂直偏移不超过半格的可走格；最多 4 个，
 * 按到工位的距离排序。工位两侧若分属不同连通区，两侧的站位格都会列出；调用方用
 * kos_stand_distance / kos_stand_best_cell 判断从某格出发是否可达，不要假设同一
 * 工位的站位格一定连通。
 * 参数/网格错误返回 KOS_ERR_INVAL；某个工位一个站位格都没有返回 KOS_ERR_MISSING，
 * err->context 里带工位 id。
 */
int kos_stand_build(kos_kitchen *kitchen, kos_error *err);

/* 某个工位的站位格表；下标越界返回 NULL。 */
const kos_station_stands *kos_stand_cells(const kos_kitchen *kitchen, int station_index);

/* distance[station][cell]；参数越界返回 KOS_STAND_UNREACHABLE。 */
uint16_t kos_stand_distance(const kos_kitchen *kitchen, int station_index, int cell_index);

/*
 * 从 from_cell 出发最合适的站位格：必须在同一连通区，按曼哈顿步数取最近（并列取
 * 站位格表里靠前的）。工位下标越界、from_cell 越界或该工位全部站位格都不可达时返回 -1。
 */
int kos_stand_best_cell(const kos_kitchen *kitchen, int station_index, int from_cell);

#endif /* WELLCOOKED_STAND_H */
