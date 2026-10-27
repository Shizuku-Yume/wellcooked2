/*
 * test_stand.c -- 站位格表与距离场（任务 03 的 W1）。
 *
 * 四张地图走的是和 Scheduler/Worker 完全相同的加载入口 kos_kitchen_load，所以
 * "加载成功"本身就证明每个工位都有站位格；剩下的检查逐项证明：
 *   - 站位格在网格里、朝向工位的方向与工位实际所在的一侧一致；
 *   - 朝工位方向的 mask 位与网格一致：那一格可走时必须能过去，不可走时必须挡住；
 *   - 距离场是真正的 BFS：站位格为 0，每个非 0 格都有 dist-1 的父格，相邻格差 ≤1；
 *   - 互不相通的厨房条带之间是 KOS_STAND_UNREACHABLE，不给假距离；
 *   - 两个工位共用一格时两边都列出来，朝向相反；
 *   - 把某工位四周的可走格全删掉后加载失败，错误里带工位 id。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/child.h"
#include "kitchenos/kitchen.h"
#include "kitchenos/nav.h"
#include "kitchenos/stand.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks;
static int g_failures;

static void expect(int cond, const char *what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        g_failures++;
        printf("  FAIL %s\n", what);
    }
}

static int find_config(const char *relative, char *path, size_t len)
{
    return kos_child_default_config_path(relative, path, len) == 0;
}

static int cell_at(const kos_kitchen *kitchen, float x, float z)
{
    return kos_kitchen_cell_index(kitchen, x, z);
}

static void facing_delta(uint8_t facing, float step, float *dx, float *dz)
{
    *dx = 0.0f;
    *dz = 0.0f;
    if (facing == KOS_GRID_EDGE_PX) {
        *dx = step;
    } else if (facing == KOS_GRID_EDGE_NX) {
        *dx = -step;
    } else if (facing == KOS_GRID_EDGE_PZ) {
        *dz = step;
    } else if (facing == KOS_GRID_EDGE_NZ) {
        *dz = -step;
    }
}

static int facing_bit(uint8_t facing)
{
    return facing == KOS_GRID_EDGE_PX   ? (int)KOS_GRID_EDGE_PX
           : facing == KOS_GRID_EDGE_NX ? (int)KOS_GRID_EDGE_NX
           : facing == KOS_GRID_EDGE_PZ ? (int)KOS_GRID_EDGE_PZ
                                        : (int)KOS_GRID_EDGE_NZ;
}

static int stand_contains(const kos_station_stands *stands, int cell)
{
    int i;

    for (i = 0; i < stands->count; i++) {
        if (stands->cells[i].cell == cell) {
            return 1;
        }
    }
    return 0;
}

static void test_one_map(const char *scene)
{
    static kos_kitchen kitchen;
    char relative[128];
    char kitchen_path[KOS_DAEMON_PATH_MAX];
    kos_error err;
    int i;
    int j;
    int unreachable_stations = 0;
    char label[192];

    printf("[test_stand] %s\n", scene);
    snprintf(relative, sizeof relative, "maps/%s/kitchen.json", scene);
    snprintf(label, sizeof label, "%s: 配置存在", scene);
    if (!find_config(relative, kitchen_path, sizeof kitchen_path)) {
        expect(0, label);
        return;
    }
    memset(&kitchen, 0, sizeof kitchen);
    kos_error_clear(&err);
    snprintf(label, sizeof label, "%s: kos_kitchen_load（站位格在这里算）", scene);
    if (kos_kitchen_load(kitchen_path, &kitchen, &err) != KOS_OK) {
        expect(0, label);
        return;
    }
    expect(1, label);

    snprintf(label, sizeof label, "%s: 距离场元素是 uint16", scene);
    expect(sizeof(kitchen.stand.distance[0][0]) == sizeof(uint16_t), label);
    snprintf(label, sizeof label, "%s: 距离场按 64 工位 × 2048 格静态分配", scene);
    expect(sizeof(kitchen.stand.distance) ==
               (size_t)KOS_KITCHEN_MAX_STATIONS * KOS_KITCHEN_MAX_CELLS *
                   sizeof(uint16_t),
           label);

    for (i = 0; i < kitchen.station_count; i++) {
        const kos_station *station = &kitchen.stations[i];
        const kos_station_stands *stands = kos_stand_cells(&kitchen, i);
        float previous_distance = -1.0f;
        int unreachable = 0;
        int c;

        snprintf(label, sizeof label, "%s/%s: 至少 1 个、至多 4 个站位格", scene,
                 station->id);
        expect(stands != NULL && stands->count >= 1 &&
                   stands->count <= KOS_STAND_MAX_PER_STATION,
               label);
        if (stands == NULL || stands->count == 0) {
            continue;
        }

        for (j = 0; j < stands->count; j++) {
            const kos_stand_cell *stand = &stands->cells[j];
            const kos_grid_cell *cell;
            float dx;
            float dz;
            float dist;
            int neighbor;
            int bit;

            snprintf(label, sizeof label, "%s/%s: 站位格下标在网格里", scene,
                     station->id);
            expect(stand->cell >= 0 && stand->cell < kitchen.cell_count, label);
            if (stand->cell < 0 || stand->cell >= kitchen.cell_count) {
                continue;
            }
            cell = &kitchen.cells[stand->cell];
            dist = (cell->x - station->x) * (cell->x - station->x) +
                   (cell->z - station->z) * (cell->z - station->z);
            if (j == 0) {
                snprintf(label, sizeof label,
                         "%s/%s: nearest stand cell within 1.2 m interaction reach",
                         scene, station->id);
                expect(dist <= 1.2f * 1.2f + 1e-5f, label);
            }
            snprintf(label, sizeof label, "%s/%s: 站位格按距离升序", scene,
                     station->id);
            expect(previous_distance < 0.0f || dist >= previous_distance - 1e-4f,
                   label);
            previous_distance = dist;

            snprintf(label, sizeof label, "%s/%s: 站位格不重复", scene, station->id);
            {
                int k;
                int duplicate = 0;

                for (k = 0; k < j; k++) {
                    if (stands->cells[k].cell == stand->cell) {
                        duplicate = 1;
                    }
                }
                expect(!duplicate, label);
            }

            snprintf(label, sizeof label, "%s/%s: 朝向位合法", scene, station->id);
            expect(stand->facing == KOS_GRID_EDGE_PX ||
                       stand->facing == KOS_GRID_EDGE_NX ||
                       stand->facing == KOS_GRID_EDGE_PZ ||
                       stand->facing == KOS_GRID_EDGE_NZ,
                   label);

            /* 朝向必须是"从站位格指向工位"的那一侧，而不是随机的四个位之一。 */
            snprintf(label, sizeof label, "%s/%s: 工位在朝向的一侧", scene,
                     station->id);
            if (stand->facing == KOS_GRID_EDGE_PX) {
                expect(station->x > cell->x, label);
            } else if (stand->facing == KOS_GRID_EDGE_NX) {
                expect(station->x < cell->x, label);
            } else if (stand->facing == KOS_GRID_EDGE_PZ) {
                expect(station->z > cell->z, label);
            } else {
                expect(station->z < cell->z, label);
            }

            /*
             * mask 只在两个可走格之间才有位。工位在台面里，工位那一格不可走，
             * 所以正常情况下朝向位是 0；如果工位离网格很近、旁边还有一格可走，
             * 那位就必须为 1。两个方向都查，才能证明站位格与网格一致。
             */
            facing_delta(stand->facing, kitchen.step, &dx, &dz);
            neighbor = cell_at(&kitchen, cell->x + dx, cell->z + dz);
            bit = facing_bit(stand->facing);
            snprintf(label, sizeof label, "%s/%s: 朝工位方向的 mask 位与网格一致",
                     scene, station->id);
            if (neighbor >= 0) {
                expect((cell->mask & (uint8_t)bit) != 0, label);
            } else {
                expect((cell->mask & (uint8_t)bit) == 0, label);
            }

            snprintf(label, sizeof label, "%s/%s: 站位格距离场为 0", scene,
                     station->id);
            expect(kos_stand_distance(&kitchen, i, stand->cell) == 0, label);
        }

        /* 距离场：站位格 0；每个可达格都有 dist-1 的父格；相邻可行格差 ≤ 1。 */
        {
            int missing_parent = 0;
            int bad_zero = 0;
            int bad_adjacent = 0;

            for (c = 0; c < kitchen.cell_count; c++) {
                uint16_t dist = kos_stand_distance(&kitchen, i, c);
                int d;

                if (dist == KOS_STAND_UNREACHABLE) {
                    unreachable++;
                    continue;
                }
                if (dist == 0) {
                    if (!stand_contains(stands, c)) {
                        bad_zero++;
                    }
                    continue;
                }
                {
                    int has_parent = 0;

                    for (d = 0; d < 4; d++) {
                        float dx = 0.0f;
                        float dz = 0.0f;
                        int n;

                        facing_delta((uint8_t)(d == 0   ? KOS_GRID_EDGE_PX
                                               : d == 1 ? KOS_GRID_EDGE_NX
                                               : d == 2 ? KOS_GRID_EDGE_PZ
                                                        : KOS_GRID_EDGE_NZ),
                                     kitchen.step, &dx, &dz);
                        n = cell_at(&kitchen, kitchen.cells[c].x + dx,
                                    kitchen.cells[c].z + dz);
                        if (n >= 0 && kos_nav_adjacent(&kitchen, n, c) &&
                            kos_stand_distance(&kitchen, i, n) == dist - 1) {
                            has_parent = 1;
                        }
                    }
                    if (!has_parent) {
                        missing_parent++;
                    }
                }
                for (d = 0; d < 4; d++) {
                    float dx = 0.0f;
                    float dz = 0.0f;
                    int n;
                    uint16_t other;

                    facing_delta((uint8_t)(d == 0   ? KOS_GRID_EDGE_PX
                                           : d == 1 ? KOS_GRID_EDGE_NX
                                           : d == 2 ? KOS_GRID_EDGE_PZ
                                                    : KOS_GRID_EDGE_NZ),
                                 kitchen.step, &dx, &dz);
                    n = cell_at(&kitchen, kitchen.cells[c].x + dx,
                                kitchen.cells[c].z + dz);
                    if (n < 0 || !kos_nav_adjacent(&kitchen, c, n)) {
                        continue;
                    }
                    other = kos_stand_distance(&kitchen, i, n);
                    if (other == KOS_STAND_UNREACHABLE ||
                        (dist > other ? dist - other : other - dist) > 1) {
                        bad_adjacent++;
                    }
                }
            }
            snprintf(label, sizeof label, "%s/%s: 距离场每格有 dist-1 的父格", scene,
                     station->id);
            expect(missing_parent == 0, label);
            snprintf(label, sizeof label, "%s/%s: 距离 0 的格都是站位格", scene,
                     station->id);
            expect(bad_zero == 0, label);
            snprintf(label, sizeof label, "%s/%s: 相邻可达格差 ≤ 1", scene,
                     station->id);
            expect(bad_adjacent == 0, label);
        }
        if (unreachable > 0) {
            unreachable_stations++;
            /* 跨连通区的站位格不可达：不能拿一个假距离让人去走。 */
            for (c = 0; c < kitchen.cell_count; c++) {
                if (kos_stand_distance(&kitchen, i, c) == KOS_STAND_UNREACHABLE) {
                    snprintf(label, sizeof label,
                             "%s/%s: 不可达格里挑不出站位格", scene, station->id);
                    expect(kos_stand_best_cell(&kitchen, i, c) < 0, label);
                    break;
                }
            }
        }
    }

    /* `_2` 按设计是两条互不相通的条带，每侧工位都看不到另一侧；其余三张图一个连通区。 */
    snprintf(label, sizeof label, "%s: 多连通区时距离场保留 UNREACHABLE", scene);
    if (strcmp(scene, "s_wellcooked_demo_2") == 0) {
        expect(unreachable_stations > 0, label);
    } else {
        expect(unreachable_stations == 0, label);
    }
}

static int write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    size_t len = strlen(text);

    if (file == NULL) {
        return -1;
    }
    if (fwrite(text, 1, len, file) != len) {
        fclose(file);
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

static void test_synthetic(void)
{
    char dir[] = "/tmp/kitchenos-stand-XXXXXX";
    char shared_path[256];
    char no_stand_path[256];
    char control_path[256];
    char line_path[256];
    char far_path[256];
    char edge_path[256];
    char diagonal_path[256];
    static kos_kitchen kitchen;
    kos_error err;
    int rc;

    printf("[test_stand] 合成厨房：共用站位格、无站位格报错、距离场数值\n");
    if (mkdtemp(dir) == NULL) {
        expect(0, "mkdtemp");
        return;
    }
    snprintf(shared_path, sizeof shared_path, "%s/shared.json", dir);
    snprintf(no_stand_path, sizeof no_stand_path, "%s/no-stand.json", dir);
    snprintf(control_path, sizeof control_path, "%s/control.json", dir);
    snprintf(line_path, sizeof line_path, "%s/line.json", dir);
    snprintf(far_path, sizeof far_path, "%s/far.json", dir);
    snprintf(edge_path, sizeof edge_path, "%s/edge.json", dir);
    snprintf(diagonal_path, sizeof diagonal_path, "%s/diagonal.json", dir);

    /*
     * 一个可走格，两个工位分别在它上下：两个工位共用同一站位格，朝向相反。
     */
    expect(write_file(shared_path,
                      "{\"scene\":\"shared\",\"grid\":{\"step\":0.6,"
                      "\"cells\":[[0,0]],\"mask\":[0]},"
                      "\"stations\":["
                      "{\"id\":\"below-0\",\"kind\":\"sink\",\"pos\":[0,-0.6]},"
                      "{\"id\":\"above-0\",\"kind\":\"sink\",\"pos\":[0,0.6]}]}") == 0,
           "shared.json 写入");
    memset(&kitchen, 0, sizeof kitchen);
    kos_error_clear(&err);
    rc = kos_kitchen_load(shared_path, &kitchen, &err);
    expect(rc == KOS_OK, "两个工位共用一个可走格时加载成功");
    if (rc == KOS_OK) {
        const kos_station_stands *below = kos_stand_cells(&kitchen, 0);
        const kos_station_stands *above = kos_stand_cells(&kitchen, 1);

        expect(below != NULL && below->count == 1 && below->cells[0].cell == 0 &&
                   below->cells[0].facing == KOS_GRID_EDGE_NZ,
               "下面的工位朝 -z 看");
        expect(above != NULL && above->count == 1 && above->cells[0].cell == 0 &&
                   above->cells[0].facing == KOS_GRID_EDGE_PZ,
               "上面的工位朝 +z 看");
        expect(kos_stand_distance(&kitchen, 0, 0) == 0 &&
                   kos_stand_distance(&kitchen, 1, 0) == 0,
               "共用格的工位距离场都是 0");
        expect(kos_stand_cells(&kitchen, -1) == NULL &&
                   kos_stand_cells(&kitchen, 2) == NULL,
               "工位下标越界返回 NULL");
        expect(kos_stand_distance(&kitchen, 0, 1) == KOS_STAND_UNREACHABLE &&
                   kos_stand_best_cell(&kitchen, 0, 1) < 0,
               "格下标越界返回不可达");
    }

    /*
     * 唯一一格与工位既不同行也不同列（垂直偏移 > 半格）：没有站位格，加载失败，
     * 错误里带工位 id。把工位移到同一行它就能加载——证明失败来自几何而不是坏 JSON。
     */
    expect(write_file(no_stand_path,
                      "{\"scene\":\"boxed\",\"grid\":{\"step\":0.6,"
                      "\"cells\":[[0,0]],\"mask\":[0]},"
                      "\"stations\":[{\"id\":\"boxed-0\",\"kind\":\"sink\","
                      "\"pos\":[1.8,0.9]}]}") == 0,
           "no-stand.json 写入");
    memset(&kitchen, 0, sizeof kitchen);
    kos_error_clear(&err);
    rc = kos_kitchen_load(no_stand_path, &kitchen, &err);
    expect(rc == KOS_ERR_MISSING, "没有站位格的工位让加载失败");
    expect(strstr(err.context, "boxed-0") != NULL, "错误里带工位 id");

    expect(write_file(control_path,
                      "{\"scene\":\"control\",\"grid\":{\"step\":0.6,"
                      "\"cells\":[[0,0]],\"mask\":[0]},"
                      "\"stations\":[{\"id\":\"boxed-0\",\"kind\":\"sink\","
                      "\"pos\":[1.8,0.0]}]}") == 0,
           "control.json 写入");
    memset(&kitchen, 0, sizeof kitchen);
    kos_error_clear(&err);
    expect(kos_kitchen_load(control_path, &kitchen, &err) == KOS_OK,
           "移到同一行后能算出站位格（失败不是 JSON 造成）");
    expect(kitchen.stand_fallback_count == 1 && kitchen.stand_warning[0] != '\0',
           "1.8 m 的远距离兜底被记录（不用 stderr）");
    expect(strstr(kitchen.stand_warning, "boxed-0") != NULL &&
               strstr(kitchen.stand_warning, "dist=1.80m") != NULL,
           "兜底记录里有工位 id 与实际距离");

    /*
     * 站位格兜底的上限是交互半径：2.4 m 之外按不到工位，宁可加载失败，
     * 也不要把"看起来能站"的格当成可交互位置（厨师会站在那儿一直按不到）。
     */
    expect(write_file(far_path,
                      "{\"scene\":\"far\",\"grid\":{\"step\":0.6,"
                      "\"cells\":[[0,0]],\"mask\":[0]},"
                      "\"stations\":[{\"id\":\"far-0\",\"kind\":\"counter\","
                      "\"pos\":[2.4,0.0]}]}") == 0,
           "far.json 写入");
    memset(&kitchen, 0, sizeof kitchen);
    kos_error_clear(&err);
    rc = kos_kitchen_load(far_path, &kitchen, &err);
    expect(rc == KOS_ERR_MISSING, "2.4 m 外的工位没有站位格，加载失败");
    expect(strstr(err.context, "far-0") != NULL, "超距错误里带工位 id");

    /*
     * 侧向偏移 0.7 m：距离只有 1.22 m，但游戏面前那一格不是这个工位
     * （实机 20261027-131838 的料箱/盘堆错位）。旧的 1.5 格侧向兜底会接受它，
     * 现在必须失败——站位格与工位必须在同一条正对线上。
     */
    expect(write_file(diagonal_path,
                      "{\"scene\":\"diagonal\",\"grid\":{\"step\":0.6,"
                      "\"cells\":[[0,0]],\"mask\":[0]},"
                      "\"stations\":[{\"id\":\"diag-0\",\"kind\":\"dispenser\","
                      "\"pos\":[0.7,-1.0]}]}") == 0,
           "diagonal.json 写入");
    memset(&kitchen, 0, sizeof kitchen);
    kos_error_clear(&err);
    rc = kos_kitchen_load(diagonal_path, &kitchen, &err);
    expect(rc == KOS_ERR_MISSING, "侧向偏移 0.7 m 的工位没有站位格（不再用侧向兜底）");
    expect(strstr(err.context, "diag-0") != NULL, "侧向偏移错误里带工位 id");

    /* 2.0 m 整（上限边上）：仍然接受，并记录兜底。 */
    expect(write_file(edge_path,
                      "{\"scene\":\"edge\",\"grid\":{\"step\":0.6,"
                      "\"cells\":[[0,0]],\"mask\":[0]},"
                      "\"stations\":[{\"id\":\"edge-0\",\"kind\":\"counter\","
                      "\"pos\":[2.0,0.0]}]}") == 0,
           "edge.json 写入");
    memset(&kitchen, 0, sizeof kitchen);
    kos_error_clear(&err);
    rc = kos_kitchen_load(edge_path, &kitchen, &err);
    expect(rc == KOS_OK, "正好 2.0 m（交互半径上限）仍算可站");
    if (rc == KOS_OK) {
        expect(kitchen.stand_fallback_count == 1, "2.0 m 的站位格记为兜底");
    }

    /*
     * 一维三格，工位在中间格下方：距离场必须是 1 / 0 / 1。
     */
    expect(write_file(line_path,
                      "{\"scene\":\"line\",\"grid\":{\"step\":0.6,"
                      "\"cells\":[[0,0],[0.6,0],[1.2,0]],\"mask\":[1,3,2]},"
                      "\"stations\":[{\"id\":\"middle-0\",\"kind\":\"sink\","
                      "\"pos\":[0.6,-0.6]}]}") == 0,
           "line.json 写入");
    memset(&kitchen, 0, sizeof kitchen);
    kos_error_clear(&err);
    rc = kos_kitchen_load(line_path, &kitchen, &err);
    expect(rc == KOS_OK, "一维厨房加载");
    if (rc == KOS_OK) {
        const kos_station_stands *stands = kos_stand_cells(&kitchen, 0);

        expect(stands != NULL && stands->count == 1 && stands->cells[0].cell == 1,
               "一维厨房只取正对的一格");
        expect(kos_stand_distance(&kitchen, 0, 0) == 1 &&
                   kos_stand_distance(&kitchen, 0, 1) == 0 &&
                   kos_stand_distance(&kitchen, 0, 2) == 1,
               "一维距离场 = 1/0/1");
        expect(kos_stand_best_cell(&kitchen, 0, 0) == 1,
               "从左端出发选中间站位格");
    }

    unlink(shared_path);
    unlink(no_stand_path);
    unlink(control_path);
    unlink(line_path);
    unlink(far_path);
    unlink(edge_path);
    unlink(diagonal_path);
    rmdir(dir);
}
static void test_demo3_cooker_stands(void)
{
    static kos_kitchen kitchen;
    char kitchen_path[KOS_DAEMON_PATH_MAX];
    kos_error err;
    int i;
    int j;
    int k;

    printf("[test_stand] demo3 全部工位站位格几何断言（<= 2.0m；cooker <= 1.2m 且更近自身工位）\n");
    if (!find_config("maps/s_wellcooked_demo_3/kitchen.json", kitchen_path, sizeof kitchen_path)) {
        expect(0, "demo3: 找不到 kitchen.json");
        return;
    }
    memset(&kitchen, 0, sizeof kitchen);
    kos_error_clear(&err);
    if (kos_kitchen_load(kitchen_path, &kitchen, &err) != KOS_OK) {
        expect(0, "demo3: 加载 kitchen.json 失败");
        return;
    }

    expect(kitchen.stand_fallback_count == 0,
           "demo3 的每个工位都有正对站位格（没有用到兜底）");
    for (i = 0; i < kitchen.station_count; i++) {
        const kos_station *station = &kitchen.stations[i];
        const kos_station_stands *stands;
        char label[256];

        stands = kos_stand_cells(&kitchen, i);
        snprintf(label, sizeof label, "demo3/%s(kind=%s): 有站位格", station->id,
                 station->kind);
        expect(stands != NULL && stands->count > 0, label);
        if (stands == NULL) {
            continue;
        }

        for (j = 0; j < stands->count; j++) {
            int cell_idx = stands->cells[j].cell;
            float cx = kitchen.cells[cell_idx].x;
            float cz = kitchen.cells[cell_idx].z;
            float dx = cx - station->x;
            float dz = cz - station->z;
            float dist = sqrtf(dx * dx + dz * dz);

            snprintf(label, sizeof label,
                     "demo3/%s(kind=%s): 站位格 cell=%d 在交互半径内 <= 2.0m (实际 %.2fm)",
                     station->id, station->kind, cell_idx, dist);
            expect(dist <= 2.0f + 1e-3f, label);
            /* 站位格必须正对工位：侧向偏移 <= 半格，否则游戏面前那一格是别人。 */
            {
                float lateral = stands->cells[j].facing == KOS_GRID_EDGE_PX
                                    ? fabsf(cz - station->z)
                                : stands->cells[j].facing == KOS_GRID_EDGE_NX
                                    ? fabsf(cz - station->z)
                                    : fabsf(cx - station->x);

                snprintf(label, sizeof label,
                         "demo3/%s(kind=%s): 站位格 cell=%d 侧向偏移 <= 0.3m (实际 %.2fm)",
                         station->id, station->kind, cell_idx, lateral);
                expect(lateral <= 0.3f + 1e-3f, label);
            }

            if (strcmp(station->kind, "cooker") != 0) {
                continue;
            }
            snprintf(label, sizeof label, "demo3/%s: 灶台站位格 cell=%d 距离 <= 1.2m (实际 %.2fm)",
                     station->id, cell_idx, dist);
            expect(dist <= 1.2f + 1e-3f, label);

            for (k = 0; k < kitchen.station_count; k++) {
                const kos_station *other = &kitchen.stations[k];
                float odx;
                float odz;
                float odist;

                if (k == i) {
                    continue;
                }
                odx = cx - other->x;
                odz = cz - other->z;
                odist = sqrtf(odx * odx + odz * odz);

                if (strcmp(other->kind, "cooker") == 0) {
                    snprintf(label, sizeof label,
                             "demo3/%s: 站位格 cell=%d (距离 %.2fm) 比 %s (距离 %.2fm) 更近",
                             station->id, cell_idx, dist, other->id, odist);
                    expect(dist < odist - 1e-3f, label);
                }
                snprintf(label, sizeof label,
                         "demo3/%s: 站位格 cell=%d 不比其他工位 %s 更远",
                         station->id, cell_idx, other->id);
                expect(dist <= odist + 1e-3f, label);
            }
        }
    }
}


int main(void)
{
    static const char *const scenes[] = {
        "s_wellcooked_demo_1",
        "s_wellcooked_demo_2",
        "s_wellcooked_demo_3",
        "s_wellcooked_demo_4",
    };
    size_t i;

    printf("[test_stand] 开始：站位格表与距离场\n");
    for (i = 0; i < sizeof scenes / sizeof scenes[0]; i++) {
        test_one_map(scenes[i]);
    }
    test_synthetic();
    test_demo3_cooker_stands();
    printf("[test_stand] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
