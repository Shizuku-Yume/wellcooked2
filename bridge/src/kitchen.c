/*
 * kitchen.c -- 解析 config/kitchen.json 的 grid + stations。
 *
 * 只接受我们自己导出的形状（grid.cells / grid.mask / stations[]），字段缺失就是
 * 启动错误：Worker 没有网格就不会走，Scheduler 没有工位就不会派任务，静默降级
 * 比拒绝启动更难查。上限是固定的，超出记错误而不是截断——截断的网格会让规划
 * 走到一半没路，看起来像寻路算法的 bug。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/kitchen.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kitchenos/json.h>
#include <kitchenos/stand.h>

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if (cap == 0) {
        return;
    }
    if (src != NULL) {
        while (i + 1 < cap && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

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

static char *read_file(const char *path, size_t *len_out, kos_error *err)
{
    FILE *file;
    long size;
    char *text;
    size_t got;

    file = fopen(path, "rb");
    if (file == NULL) {
        if (err != NULL) {
            err->code = KOS_ERR_IO;
            err->sys_errno = errno;
            snprintf(err->context, sizeof err->context, "fopen('%s')", path);
        }
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        size > KOS_KITCHEN_FILE_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        set_error(err, KOS_ERR_IO, "读取 '%s' 大小失败", path);
        return NULL;
    }
    text = malloc((size_t)size + 1);
    if (text == NULL) {
        fclose(file);
        set_error(err, KOS_ERR_NOMEM, "malloc(%ld)", size + 1);
        return NULL;
    }
    got = fread(text, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(text);
        set_error(err, KOS_ERR_IO, "读取 '%s' 只得到 %lu/%ld 字节", path,
                  (unsigned long)got, size);
        return NULL;
    }
    text[size] = '\0';
    *len_out = (size_t)size;
    return text;
}

static int parse_cell(const json_value *entry, kos_grid_cell *cell, kos_error *err)
{
    const json_value *pair;
    const json_value *value;

    if (json_typeof(entry) != JSON_ARRAY || json_array_size(entry) != 2) {
        return set_error(err, KOS_ERR_INVAL, "grid.cells 项不是 [x,z]");
    }
    pair = json_array_get(entry, 0);
    value = json_array_get(entry, 1);
    if (json_typeof(pair) != JSON_NUMBER || json_typeof(value) != JSON_NUMBER) {
        return set_error(err, KOS_ERR_INVAL, "grid.cells 坐标不是数字");
    }
    cell->x = (float)json_number_value(pair);
    cell->z = (float)json_number_value(value);
    cell->mask = 0;
    return KOS_OK;
}

int kos_kitchen_load(const char *path, kos_kitchen *out, kos_error *err)
{
    char *text = NULL;
    size_t len = 0;
    json_value *root = NULL;
    json_error jerr;
    const json_value *grid;
    const json_value *cells;
    const json_value *masks;
    const json_value *stations;
    size_t cell_count;
    size_t i;
    int rc = KOS_ERR_INVAL;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (path == NULL || out == NULL) {
        return set_error(err, KOS_ERR_INVAL, "kos_kitchen_load 参数为空");
    }
    memset(out, 0, sizeof *out);
    text = read_file(path, &len, err);
    if (text == NULL) {
        return KOS_ERR_IO;
    }
    json_error_init(&jerr);
    rc = json_parse_ex(text, len, &root, &jerr);
    free(text);
    if (rc != JSON_OK || json_typeof(root) != JSON_OBJECT) {
        json_free(root);
        return set_error(err, KOS_ERR_INVAL, "'%s' 不是 JSON 对象: %s", path,
                         json_strerror(rc));
    }

    {
        const char *scene = NULL;
        size_t scene_len = 0;

        if (json_object_get_string(root, "scene", &scene, &scene_len) == JSON_OK) {
            copy_str(out->scene, sizeof out->scene, scene);
        }
    }

    if (json_object_get_object(root, "grid", &grid) != JSON_OK ||
        json_object_get_array(grid, "cells", &cells) != JSON_OK ||
        json_object_get_array(grid, "mask", &masks) != JSON_OK) {
        rc = set_error(err, KOS_ERR_INVAL, "'%s' 缺 grid.cells / grid.mask", path);
        goto out;
    }
    {
        double step = 0.0;

        if (json_object_get_double(grid, "step", &step) != JSON_OK || step <= 0.0) {
            rc = set_error(err, KOS_ERR_INVAL, "'%s' 的 grid.step 不合法", path);
            goto out;
        }
        out->step = (float)step;
    }
    cell_count = json_array_size(cells);
    if (cell_count == 0 || cell_count > (size_t)KOS_KITCHEN_MAX_CELLS ||
        json_array_size(masks) != cell_count) {
        rc = set_error(err, KOS_ERR_RANGE, "'%s' cells=%lu mask=%lu（上限 %d）", path,
                       (unsigned long)cell_count, (unsigned long)json_array_size(masks),
                       KOS_KITCHEN_MAX_CELLS);
        goto out;
    }
    for (i = 0; i < cell_count; i++) {
        const json_value *mask = json_array_get(masks, i);

        if (parse_cell(json_array_get(cells, i), &out->cells[i], err) != KOS_OK) {
            rc = err != NULL ? err->code : KOS_ERR_INVAL;
            goto out;
        }
        if (json_typeof(mask) != JSON_NUMBER) {
            rc = set_error(err, KOS_ERR_INVAL, "'%s' grid.mask[%lu] 不是数字", path,
                           (unsigned long)i);
            goto out;
        }
        out->cells[i].mask = (uint8_t)((unsigned)json_number_value(mask) & 0x0fu);
    }
    out->cell_count = (int)cell_count;

    if (json_object_get_array(root, "stations", &stations) != JSON_OK) {
        rc = set_error(err, KOS_ERR_INVAL, "'%s' 缺 stations", path);
        goto out;
    }
    if (json_array_size(stations) > (size_t)KOS_KITCHEN_MAX_STATIONS) {
        rc = set_error(err, KOS_ERR_RANGE, "'%s' stations=%lu 超过上限 %d", path,
                       (unsigned long)json_array_size(stations), KOS_KITCHEN_MAX_STATIONS);
        goto out;
    }
    for (i = 0; i < json_array_size(stations); i++) {
        const json_value *entry = json_array_get(stations, i);
        kos_station *station = &out->stations[out->station_count];
        const json_value *pos;
        const char *text_value = NULL;
        size_t text_len = 0;

        if (json_typeof(entry) != JSON_OBJECT ||
            json_object_get_string(entry, "id", &text_value, &text_len) != JSON_OK) {
            rc = set_error(err, KOS_ERR_INVAL, "'%s' stations[%lu] 缺 id", path,
                           (unsigned long)i);
            goto out;
        }
        copy_str(station->id, sizeof station->id, text_value);
        if (json_object_get_string(entry, "kind", &text_value, &text_len) != JSON_OK) {
            rc = set_error(err, KOS_ERR_INVAL, "工位 %s 缺 kind", station->id);
            goto out;
        }
        copy_str(station->kind, sizeof station->kind, text_value);
        if (json_object_get_string(entry, "gives", &text_value, &text_len) == JSON_OK) {
            copy_str(station->gives, sizeof station->gives, text_value);
        }
        if (json_object_get_string(entry, "step", &text_value, &text_len) == JSON_OK) {
            copy_str(station->step, sizeof station->step, text_value);
        }
        if (json_object_get_array(entry, "pos", &pos) != JSON_OK ||
            json_array_size(pos) != 2 ||
            json_typeof(json_array_get(pos, 0)) != JSON_NUMBER ||
            json_typeof(json_array_get(pos, 1)) != JSON_NUMBER) {
            rc = set_error(err, KOS_ERR_INVAL, "工位 %s 的 pos 不是 [x,z]", station->id);
            goto out;
        }
        station->x = (float)json_number_value(json_array_get(pos, 0));
        station->z = (float)json_number_value(json_array_get(pos, 1));
        out->station_count++;
    }
    if (out->station_count == 0) {
        rc = set_error(err, KOS_ERR_INVAL, "'%s' stations 为空", path);
        goto out;
    }
    /* 站位格是加载契约的一部分：没有站位格的工位在这里就失败，别留到运行时
     * 才表现成"厨师走到工位旁边却永远到不了"。 */
    rc = kos_stand_build(out, err);
    if (rc != KOS_OK) {
        goto out;
    }
    rc = KOS_OK;

out:
    json_free(root);
    if (rc != KOS_OK) {
        memset(out, 0, sizeof *out);
    }
    return rc;
}

int kos_kitchen_cell_index(const kos_kitchen *kitchen, float x, float z)
{
    int i;

    if (kitchen == NULL) {
        return -1;
    }
    for (i = 0; i < kitchen->cell_count; i++) {
        if (fabsf(kitchen->cells[i].x - x) < 1e-3f &&
            fabsf(kitchen->cells[i].z - z) < 1e-3f) {
            return i;
        }
    }
    return -1;
}

int kos_kitchen_nearest_cell(const kos_kitchen *kitchen, float x, float z)
{
    int best = -1;
    float best_dist = 0.0f;
    int i;

    if (kitchen == NULL) {
        return -1;
    }
    for (i = 0; i < kitchen->cell_count; i++) {
        float dx = kitchen->cells[i].x - x;
        float dz = kitchen->cells[i].z - z;
        float dist = dx * dx + dz * dz;

        if (best < 0 || dist < best_dist) {
            best = i;
            best_dist = dist;
        }
    }
    return best;
}

int kos_kitchen_count_kind(const kos_kitchen *kitchen, const char *kind)
{
    int count = 0;
    int i;

    if (kitchen == NULL || kind == NULL) {
        return 0;
    }
    for (i = 0; i < kitchen->station_count; i++) {
        if (strcmp(kitchen->stations[i].kind, kind) == 0) {
            count++;
        }
    }
    return count;
}

const kos_station *kos_kitchen_station_by_id(const kos_kitchen *kitchen, const char *id)
{
    int i;

    if (kitchen == NULL || id == NULL) {
        return NULL;
    }
    for (i = 0; i < kitchen->station_count; i++) {
        if (strcmp(kitchen->stations[i].id, id) == 0) {
            return &kitchen->stations[i];
        }
    }
    return NULL;
}

int kos_kitchen_face_action(const kos_kitchen *kitchen, const char *id,
                            kos_input_action *action)
{
    const kos_station *station = kos_kitchen_station_by_id(kitchen, id);

    if (station == NULL || action == NULL) {
        return KOS_ERR_INVAL;
    }
    memset(action, 0, sizeof *action);
    action->kind = KOS_ACTION_FACE;
    action->target_x = station->x;
    action->target_y = station->z;
    return KOS_OK;
}

int kos_kitchen_station_index(const kos_kitchen *kitchen, const char *id)
{
    int i;

    if (kitchen == NULL || id == NULL) {
        return -1;
    }
    for (i = 0; i < kitchen->station_count; i++) {
        if (strcmp(kitchen->stations[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static int lowered_contains(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    if (nlen == 0) {
        return 1;
    }
    for (i = 0; haystack[i] != '\0'; i++) {
        size_t j;

        for (j = 0; j < nlen && haystack[i + j] != '\0'; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j])) {
                break;
            }
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

int kos_kitchen_name_matches(const char *hint, const char *candidate)
{
    if (hint == NULL || candidate == NULL || hint[0] == '\0' || candidate[0] == '\0') {
        return 0;
    }
    return lowered_contains(hint, candidate) || lowered_contains(candidate, hint);
}

int kos_kitchen_pick_station(const kos_kitchen *kitchen, const char *kind,
                             const char *ingredient_hint, unsigned salt, int *index)
{
    int candidates[KOS_KITCHEN_MAX_STATIONS];
    int count = 0;
    int i;

    if (kitchen == NULL || kind == NULL || index == NULL) {
        return KOS_ERR_INVAL;
    }
    for (i = 0; i < kitchen->station_count; i++) {
        if (strcmp(kitchen->stations[i].kind, kind) == 0) {
            candidates[count++] = i;
        }
    }
    if (count == 0) {
        return KOS_ERR_MISSING;
    }
    if (ingredient_hint != NULL && ingredient_hint[0] != '\0') {
        int matched[KOS_KITCHEN_MAX_STATIONS];
        int matched_count = 0;

        for (i = 0; i < count; i++) {
            if (kos_kitchen_name_matches(ingredient_hint,
                                         kitchen->stations[candidates[i]].gives)) {
                matched[matched_count++] = candidates[i];
            }
        }
        if (matched_count > 0) {
            *index = matched[salt % (unsigned)matched_count];
            return KOS_OK;
        }
    }
    *index = candidates[salt % (unsigned)count];
    return KOS_OK;
}
