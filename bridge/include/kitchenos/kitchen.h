/*
 * kitchen.h -- config/kitchen.json 的运行时模型（模块契约）。
 *
 * Worker、Scheduler 和 navd 都需要的只有三样：可走网格（坐标 + 邻居掩码）、工位
 * （kind、位置、料箱给什么）和由网格+工位导出的站位格/距离场（stand.c 在加载时算，
 * 不读 JSON）。解析放在这里，A* 放在 nav.h，两边都不读 JSON。
 */
#ifndef KITCHENOS_KITCHEN_H
#define KITCHENOS_KITCHEN_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>

#include <kitchenos/shm.h>

#define KOS_KITCHEN_MAX_CELLS 2048
#define KOS_KITCHEN_MAX_STATIONS 64
#define KOS_KITCHEN_NAME_MAX 32
#define KOS_KITCHEN_FILE_MAX (4 * 1024 * 1024)

#define KOS_GRID_EDGE_PX 1u /* +x */
#define KOS_GRID_EDGE_NX 2u /* -x */
#define KOS_GRID_EDGE_PZ 4u /* +z */
#define KOS_GRID_EDGE_NZ 8u /* -z */

typedef struct {
    float x;
    float z;
    uint8_t mask; /* edge_bits: 1:+x 2:-x 4:+z 8:-z */
} kos_grid_cell;

typedef struct {
    char id[KOS_KITCHEN_NAME_MAX];
    char kind[KOS_KITCHEN_NAME_MAX];
    char gives[KOS_KITCHEN_NAME_MAX]; /* 空串 = 这个工位不发料 */
    float x;
    float z;
} kos_station;

/*
 * 站位格：工位朝向的可走格，加载 kitchen.json 时由 stand.c 算出。
 *
 * 工位本身在台面/墙里，不在 grid.cells 中，所以"朝向工位的边"在 mask 里一定是 0
 * （那一格不可走）。站位格取每个方向离工位最近的可行格；朝向存的是站位格看向
 * 工位的方向位，与 grid.mask 的位同一套，Worker 用它转身。
 */
#define KOS_STAND_MAX_PER_STATION 4
#define KOS_STAND_UNREACHABLE UINT16_MAX

typedef struct {
    int cell;        /* kitchen.cells[] 下标 */
    uint8_t facing;  /* KOS_GRID_EDGE_PX/NX/PZ/NZ：站位格看向工位的方向 */
} kos_stand_cell;

typedef struct {
    int count;
    kos_stand_cell cells[KOS_STAND_MAX_PER_STATION]; /* 按到工位的距离升序 */
} kos_station_stands;

/*
 * 距离场：distance[station][cell] 是从该工位任一站位格出发的 BFS 步数；
 * 站位格为 0，不可达（另一个连通区）为 KOS_STAND_UNREACHABLE。
 * 定长数组按上限静态分配（64 × 2048 × 2 B = 256 KB），与 kitchen 一起加载，
 * 不动态分配：Scheduler 估路程和 navd 排候选格用的是同一份。
 */
typedef struct {
    kos_station_stands station[KOS_KITCHEN_MAX_STATIONS];
    uint16_t distance[KOS_KITCHEN_MAX_STATIONS][KOS_KITCHEN_MAX_CELLS];
    int component[KOS_KITCHEN_MAX_CELLS]; /* 连通块编号；stand 模块内部用来判断可达 */
} kos_stand_map;

typedef struct {
    char scene[KOS_KITCHEN_NAME_MAX];
    float step;
    int cell_count;
    int station_count;
    kos_grid_cell cells[KOS_KITCHEN_MAX_CELLS];
    kos_station stations[KOS_KITCHEN_MAX_STATIONS];
    kos_stand_map stand;
} kos_kitchen;

/* 读整个文件并解析；失败时 err 带文件名/字段。 */
int kos_kitchen_load(const char *path, kos_kitchen *out, kos_error *err);

/* 坐标 → 网格下标（容差 1e-3）；不在网格上返回 -1。 */
int kos_kitchen_cell_index(const kos_kitchen *kitchen, float x, float z);

/* 离 (x,z) 最近的可走格；网格为空返回 -1。平局取下标小的。 */
int kos_kitchen_nearest_cell(const kos_kitchen *kitchen, float x, float z);

int kos_kitchen_count_kind(const kos_kitchen *kitchen, const char *kind);
const kos_station *kos_kitchen_station_by_id(const kos_kitchen *kitchen, const char *id);

/* 工位 id → stations[] 下标；找不到返回 -1。 */
int kos_kitchen_station_index(const kos_kitchen *kitchen, const char *id);

/*
 * 选一个 kind 匹配的工位。ingredient_hint 非空时优先选 gives 与它（大小写不敏感、
 * 允许子串，例如 "fish" 命中 "Fish"）匹配的；没有匹配再退回该 kind 的第一个。
 * salt 用来把不同任务摊到同类工位上（取 salt % 候选数）。成功返回 0 并写下标。
 */
int kos_kitchen_pick_station(const kos_kitchen *kitchen, const char *kind,
                             const char *ingredient_hint, unsigned salt, int *index);

/* 两个名字是否匹配：忽略大小写，任一方包含另一方即算。给上面选料箱用。 */
int kos_kitchen_name_matches(const char *hint, const char *candidate);

#endif /* KITCHENOS_KITCHEN_H */
