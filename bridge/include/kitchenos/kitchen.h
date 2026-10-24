/*
 * kitchen.h -- config/kitchen.json 的运行时模型（模块契约）。
 *
 * 只有两样东西是 Worker 和 Scheduler 都需要的：可走网格（坐标 + 邻居掩码）与
 * 工位（kind、位置、料箱给什么）。解析放在这里，A* 放在 nav.h，两边都不读 JSON。
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

typedef struct {
    char scene[KOS_KITCHEN_NAME_MAX];
    float step;
    int cell_count;
    int station_count;
    kos_grid_cell cells[KOS_KITCHEN_MAX_CELLS];
    kos_station stations[KOS_KITCHEN_MAX_STATIONS];
} kos_kitchen;

/* 读整个文件并解析；失败时 err 带文件名/字段。 */
int kos_kitchen_load(const char *path, kos_kitchen *out, kos_error *err);

/* 坐标 → 网格下标（容差 1e-3）；不在网格上返回 -1。 */
int kos_kitchen_cell_index(const kos_kitchen *kitchen, float x, float z);

/* 离 (x,z) 最近的可走格；网格为空返回 -1。平局取下标小的。 */
int kos_kitchen_nearest_cell(const kos_kitchen *kitchen, float x, float z);

int kos_kitchen_count_kind(const kos_kitchen *kitchen, const char *kind);
const kos_station *kos_kitchen_station_by_id(const kos_kitchen *kitchen, const char *id);

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
