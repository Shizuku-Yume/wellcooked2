/*
 * stand.c -- 工位站位格与距离场。
 *
 * 工位坐标在台面/墙的几何中心，不在 grid.cells 里；厨师能站的格子离它通常还有一格
 * 台面厚度。所以站位格不能只看"相邻格在不在网格里"（那样四张地图全是 0 个），
 * 而是每个方向取最近的可走格：台面挡住的一侧取不到，有地板的一侧就是厨师该站的位置。
 * 实测四张地图上这个距离都是 1.2 m（两格），正好对应游戏 1.2 m 交互格的一步。
 *
 * 距离场按 mask 建图后逐工位做多源 BFS；不可达（另一条互不相通的厨房条带）保持
 * KOS_STAND_UNREACHABLE，调用方据此拒绝派工，而不是拿一个假的距离。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/stand.h>

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <kitchenos/nav.h>

/* 垂直偏移超过半格就不是"正对着工位"了；0.5 格是相邻格中心的最大偏差。 */
#define STAND_PERP_TOLERANCE_FACTOR 0.5f
/* 跳过工位自己所在的那一格（工位偶尔压在可走格上，实机 sink 偏 0.01 m），
 * 但不跳过与工位相距四分之一格以上的相邻格。 */
#define STAND_MIN_ALONG_FACTOR 0.25f

typedef struct {
    int axis;     /* 0 = x, 1 = z */
    int sign;     /* 站位格在工位的哪一侧 */
    uint8_t facing; /* 站位格看向工位的方向位 */
} stand_direction;

/* 顺序固定：同一张地图每次算出的站位格表相同，测试与复现都靠它。 */
static const stand_direction STAND_DIRECTIONS[] = {
    {0, +1, KOS_GRID_EDGE_NX},
    {0, -1, KOS_GRID_EDGE_PX},
    {1, +1, KOS_GRID_EDGE_NZ},
    {1, -1, KOS_GRID_EDGE_PZ},
};

static int set_error(kos_error *err, int code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int set_error(kos_error *err, int code, const char *fmt, ...)
{
    va_list ap;

    if (err != NULL) {
        err->code = code;
        err->sys_errno = 0;
        va_start(ap, fmt);
        vsnprintf(err->context, sizeof err->context, fmt, ap);
        va_end(ap);
    }
    return code;
}

static void build_adjacency(const kos_kitchen *kitchen,
                            uint8_t degree[KOS_KITCHEN_MAX_CELLS],
                            int neighbor[KOS_KITCHEN_MAX_CELLS][4])
{
    int i;
    int j;

    memset(degree, 0, sizeof(uint8_t) * (size_t)kitchen->cell_count);
    for (i = 0; i < kitchen->cell_count; i++) {
        for (j = 0; j < kitchen->cell_count; j++) {
            if (i == j || !kos_nav_adjacent(kitchen, i, j)) {
                continue;
            }
            /* mask 声明对称；万一数据坏了也不越界，多出来的边丢弃并在测试里抓。 */
            if (degree[i] < 4) {
                neighbor[i][degree[i]++] = j;
            }
        }
    }
}

static void label_components(kos_kitchen *kitchen,
                             const uint8_t degree[KOS_KITCHEN_MAX_CELLS],
                             const int neighbor[KOS_KITCHEN_MAX_CELLS][4])
{
    int queue[KOS_KITCHEN_MAX_CELLS];
    int next_component = 0;
    int i;

    for (i = 0; i < kitchen->cell_count; i++) {
        int head = 0;
        int tail = 0;
        int current;

        if (kitchen->stand.component[i] >= 0) {
            continue;
        }
        kitchen->stand.component[i] = next_component;
        queue[tail++] = i;
        while (head < tail) {
            int n;

            current = queue[head++];
            for (n = 0; n < degree[current]; n++) {
                int other = neighbor[current][n];

                if (kitchen->stand.component[other] < 0) {
                    kitchen->stand.component[other] = next_component;
                    queue[tail++] = other;
                }
            }
        }
        next_component++;
    }
}

static int find_stand_cell(const kos_kitchen *kitchen, const kos_station *station,
                           const stand_direction *direction, float perp_tolerance,
                           float min_along)
{
    int best = -1;
    float best_along = 0.0f;
    int i;

    for (i = 0; i < kitchen->cell_count; i++) {
        float dx = kitchen->cells[i].x - station->x;
        float dz = kitchen->cells[i].z - station->z;
        float along = direction->axis == 0 ? dx : dz;
        float perp = fabsf(direction->axis == 0 ? dz : dx);

        along *= (float)direction->sign;
        if (along <= min_along || perp > perp_tolerance) {
            continue;
        }
        if (best < 0 || along < best_along ||
            (along == best_along && i < best)) {
            best = i;
            best_along = along;
        }
    }
    return best;
}

static void sort_stands(kos_station_stands *stands, const kos_kitchen *kitchen,
                        const kos_station *station)
{
    int i;

    for (i = 1; i < stands->count; i++) {
        kos_stand_cell value = stands->cells[i];
        float value_dx = kitchen->cells[value.cell].x - station->x;
        float value_dz = kitchen->cells[value.cell].z - station->z;
        float value_dist = value_dx * value_dx + value_dz * value_dz;
        int j = i - 1;

        while (j >= 0) {
            const kos_stand_cell *other = &stands->cells[j];
            float dx = kitchen->cells[other->cell].x - station->x;
            float dz = kitchen->cells[other->cell].z - station->z;
            float dist = dx * dx + dz * dz;

            if (dist < value_dist || (dist == value_dist && other->cell < value.cell)) {
                break;
            }
            stands->cells[j + 1] = stands->cells[j];
            j--;
        }
        stands->cells[j + 1] = value;
    }
}

static void fill_distance_field(kos_kitchen *kitchen, int station_index,
                                const uint8_t degree[KOS_KITCHEN_MAX_CELLS],
                                const int neighbor[KOS_KITCHEN_MAX_CELLS][4])
{
    uint16_t *distance = kitchen->stand.distance[station_index];
    const kos_station_stands *stands = &kitchen->stand.station[station_index];
    int queue[KOS_KITCHEN_MAX_CELLS];
    int head = 0;
    int tail = 0;
    int i;

    for (i = 0; i < kitchen->cell_count; i++) {
        distance[i] = KOS_STAND_UNREACHABLE;
    }
    for (i = 0; i < stands->count; i++) {
        int cell = stands->cells[i].cell;

        distance[cell] = 0;
        queue[tail++] = cell;
    }
    while (head < tail) {
        int current = queue[head++];
        int n;

        for (n = 0; n < degree[current]; n++) {
            int other = neighbor[current][n];

            if (distance[other] == KOS_STAND_UNREACHABLE) {
                distance[other] = (uint16_t)(distance[current] + 1);
                queue[tail++] = other;
            }
        }
    }
}

int kos_stand_build(kos_kitchen *kitchen, kos_error *err)
{
    uint8_t degree[KOS_KITCHEN_MAX_CELLS];
    int neighbor[KOS_KITCHEN_MAX_CELLS][4];
    float perp_tolerance;
    float min_along;
    int station_index;
    int i;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (kitchen == NULL || kitchen->cell_count <= 0 || kitchen->station_count <= 0 ||
        kitchen->cell_count > KOS_KITCHEN_MAX_CELLS ||
        kitchen->station_count > KOS_KITCHEN_MAX_STATIONS || kitchen->step <= 0.0f) {
        return set_error(err, KOS_ERR_INVAL, "kos_stand_build 参数或网格不合法");
    }

    memset(&kitchen->stand, 0, sizeof kitchen->stand);
    for (i = 0; i < kitchen->cell_count; i++) {
        kitchen->stand.component[i] = -1;
    }

    perp_tolerance = kitchen->step * STAND_PERP_TOLERANCE_FACTOR + 1e-3f;
    min_along = kitchen->step * STAND_MIN_ALONG_FACTOR;

    for (station_index = 0; station_index < kitchen->station_count; station_index++) {
        const kos_station *station = &kitchen->stations[station_index];
        kos_station_stands *stands = &kitchen->stand.station[station_index];
        size_t d;

        for (d = 0; d < sizeof STAND_DIRECTIONS / sizeof STAND_DIRECTIONS[0]; d++) {
            int cell = find_stand_cell(kitchen, station, &STAND_DIRECTIONS[d],
                                       perp_tolerance, min_along);
            int duplicate = 0;
            int k;

            if (cell < 0) {
                continue;
            }
            for (k = 0; k < stands->count; k++) {
                if (stands->cells[k].cell == cell) {
                    duplicate = 1;
                }
            }
            if (duplicate) {
                continue;
            }
            stands->cells[stands->count].cell = cell;
            stands->cells[stands->count].facing = STAND_DIRECTIONS[d].facing;
            stands->count++;
        }
        if (stands->count == 0) {
            return set_error(err, KOS_ERR_MISSING,
                             "工位 %s（kind=%s）周围没有可站的可走格", station->id,
                             station->kind);
        }
        sort_stands(stands, kitchen, station);
    }

    build_adjacency(kitchen, degree, neighbor);
    label_components(kitchen, degree, neighbor);
    for (station_index = 0; station_index < kitchen->station_count; station_index++) {
        fill_distance_field(kitchen, station_index, degree, neighbor);
    }
    return KOS_OK;
}

const kos_station_stands *kos_stand_cells(const kos_kitchen *kitchen, int station_index)
{
    if (kitchen == NULL || station_index < 0 || station_index >= kitchen->station_count) {
        return NULL;
    }
    return &kitchen->stand.station[station_index];
}

uint16_t kos_stand_distance(const kos_kitchen *kitchen, int station_index, int cell_index)
{
    if (kitchen == NULL || station_index < 0 || station_index >= kitchen->station_count ||
        cell_index < 0 || cell_index >= kitchen->cell_count) {
        return KOS_STAND_UNREACHABLE;
    }
    return kitchen->stand.distance[station_index][cell_index];
}

int kos_stand_best_cell(const kos_kitchen *kitchen, int station_index, int from_cell)
{
    const kos_station_stands *stands;
    int component;
    int best = -1;
    float best_distance = 0.0f;
    int i;

    if (kitchen == NULL || station_index < 0 || station_index >= kitchen->station_count ||
        from_cell < 0 || from_cell >= kitchen->cell_count) {
        return -1;
    }
    stands = &kitchen->stand.station[station_index];
    component = kitchen->stand.component[from_cell];
    if (component < 0) {
        return -1;
    }
    for (i = 0; i < stands->count; i++) {
        const kos_stand_cell *stand = &stands->cells[i];
        float dx;
        float dz;
        float distance;

        if (kitchen->stand.component[stand->cell] != component) {
            continue;
        }
        dx = fabsf(kitchen->cells[stand->cell].x - kitchen->cells[from_cell].x);
        dz = fabsf(kitchen->cells[stand->cell].z - kitchen->cells[from_cell].z);
        distance = dx + dz;
        if (best < 0 || distance < best_distance) {
            best = stand->cell;
            best_distance = distance;
        }
    }
    return best;
}
