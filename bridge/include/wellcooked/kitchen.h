/*
 * kitchen.h -- config/kitchen.json 的运行时模型（模块契约）。
 *
 * Worker、Scheduler 和 navd 都需要的只有三样：可走网格（坐标 + 邻居掩码）、工位
 * （kind、位置、料箱给什么）和由网格+工位导出的站位格/距离场（stand.c 在加载时算，
 * 不读 JSON）。解析放在这里，相邻判定在 nav.h，两边都不读 JSON。
 */
#ifndef WELLCOOKED_KITCHEN_H
#define WELLCOOKED_KITCHEN_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>

#include <wellcooked/shm.h>

#define KOS_KITCHEN_MAX_CELLS 2048
/* 普通台面（counter）现在也是工位：`_3` 的环形吧台有 42 张，加上料箱/锅/盘堆就过 64。
 * 这个上限只在进程内使用（kos_kitchen 不进共享内存），放宽不改变段布局。 */
#define KOS_KITCHEN_MAX_STATIONS 128
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
    char step[KOS_KITCHEN_NAME_MAX];  /* cooker 上那口锅做哪种烹饪（Boil/Fry…）；空串 = 不限 */
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
    /* 站位格兜底记录（stand.c 写，调用方读完地图后用自己的 logger 打一行）。
     * 空串 = 每个工位都有正对的站位格；非空 = 至少一个工位用了侧向/远距离兜底，
     * 内容是第一例的一句话说明（含工位 id、距离与上限），count 是总例数。
     * 加载器不碰 stderr：worker/scheduler/navd 各有自己的日志文件。 */
    char stand_warning[192];
    int stand_fallback_count;
} kos_kitchen;

/* 读整个文件并解析；失败时 err 带文件名/字段。 */
int kos_kitchen_load(const char *path, kos_kitchen *out, kos_error *err);

/*
 * kos_kitchen_load + 同目录 zones.json 里的 handoff 台面（counter_handoff_*）。
 * Worker 的放下工序按工位 id 建宏计划，工位不在 kitchen 里会 macro-map 失败；
 * Scheduler 在 zoned 模式已经注册，Worker 也必须看到同一个工位。
 * zones 文件缺失或坏掉不影响地图加载（返回 KOS_OK）。
 */
int kos_kitchen_load_with_zones(const char *path, kos_kitchen *out, kos_error *err);

/* 坐标 → 网格下标（容差 1e-3）；不在网格上返回 -1。 */
int kos_kitchen_cell_index(const kos_kitchen *kitchen, float x, float z);

/* 离 (x,z) 最近的可走格；网格为空返回 -1。平局取下标小的。 */
int kos_kitchen_nearest_cell(const kos_kitchen *kitchen, float x, float z);

int kos_kitchen_count_kind(const kos_kitchen *kitchen, const char *kind);
const kos_station *kos_kitchen_station_by_id(const kos_kitchen *kitchen, const char *id);
/* Build the observable FACE payload for a stable map station ID; unknown IDs fail. */
int kos_kitchen_face_action(const kos_kitchen *kitchen, const char *id,
                            kos_input_action *action);

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

/*
 * 普通台面（counter）：同格还有别的工位（盘堆/灶/出菜口都摆在台面上）就不算，
 * 判定阈值与 `scripts/export-kitchen.py` 合并工位用的 0.15 m 相同。
 */
#define KOS_KITCHEN_STATION_OVERLAP 0.15f

/*
 * 挑一张能当容器的普通台面（卷饼 `place` / `pour` / `pickup` 落这里）。
 * 只考虑有站位格、同格没有别的工位的 counter；`busy` 非空时跳过其中非 0 的下标
 * （别的订单已经占了那张台面）。
 *
 * 分数 = 2 × Σ_steps（到最近同类灶的格数）+ 到最近盘堆（没有盘堆时水槽）的格数：
 * 每份熟料都要端着锅从灶走到台面再走回去，所以灶距离算两倍。同分取 stations[]
 * 下标小的——导出顺序固定，结果可复现。成功返回 0 并写下标，没有可用台面返回
 * KOS_ERR_MISSING。
 */
int kos_kitchen_pick_counter(const kos_kitchen *kitchen, const char *const steps[],
                             int step_count, const unsigned char *busy, int *index);

#endif /* WELLCOOKED_KITCHEN_H */
