/*
 * zones.c -- 分区配置读取与工位归属。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/zones.h>

#include <errno.h>
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
        size > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
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

int kos_zones_load(const char *path, kos_zones *out, kos_error *err)
{
    char *text;
    size_t len = 0;
    json_value *root = NULL;
    json_error jerr;
    const json_value *handoff_v;
    const json_value *zones_v;
    int rc = KOS_OK;
    size_t z_count;
    size_t i;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (out == NULL) {
        return KOS_ERR_INVAL;
    }
    memset(out, 0, sizeof *out);
    out->handoff.x = 0.0f;
    out->handoff.z = 0.0f;

    text = read_file(path, &len, err);
    if (text == NULL) {
        /* 文件不存在不是致命错误：没有 zones.json 时保留空配置 */
        if (err != NULL && err->sys_errno == ENOENT) {
            kos_error_clear(err);
            return KOS_OK;
        }
        return KOS_ERR_IO;
    }

    json_error_init(&jerr);
    rc = json_parse_ex(text, len, &root, &jerr);
    free(text);
    if (rc != JSON_OK) {
        return set_error(err, KOS_ERR_INVAL, "'%s' JSON 解析失败: %s", path,
                         json_strerror(rc));
    }
    if (json_typeof(root) != JSON_OBJECT) {
        json_free(root);
        return set_error(err, KOS_ERR_INVAL, "'%s' 根节点不是 object", path);
    }

    {
        const char *scene_str = NULL;
        size_t scene_len = 0;
        if (json_object_get_string(root, "scene", &scene_str, &scene_len) == JSON_OK) {
            copy_str(out->scene, sizeof out->scene, scene_str);
        }
    }

    /* handoff */
    if (json_object_get_object(root, "handoff", &handoff_v) == JSON_OK) {
        const char *id_str = NULL;
        const char *kind_str = NULL;
        size_t str_len = 0;
        const json_value *pos_v;

        if (json_object_get_string(handoff_v, "id", &id_str, &str_len) == JSON_OK) {
            copy_str(out->handoff.id, sizeof out->handoff.id, id_str);
        }
        if (json_object_get_string(handoff_v, "kind", &kind_str, &str_len) == JSON_OK) {
            copy_str(out->handoff.kind, sizeof out->handoff.kind, kind_str);
        } else {
            copy_str(out->handoff.kind, sizeof out->handoff.kind, "counter");
        }
        if (json_object_get_array(handoff_v, "pos", &pos_v) == JSON_OK &&
            json_array_size(pos_v) >= 2) {
            out->handoff.x = (float)json_number_value(json_array_get(pos_v, 0));
            out->handoff.z = (float)json_number_value(json_array_get(pos_v, 1));
        }
    }

    /* zones */
    if (json_object_get_array(root, "zones", &zones_v) == JSON_OK) {
        z_count = json_array_size(zones_v);
        if (z_count > KOS_ZONE_MAX) {
            z_count = KOS_ZONE_MAX;
        }
        out->zone_count = (int)z_count;
        for (i = 0; i < z_count; i++) {
            const json_value *zone_v = json_array_get(zones_v, i);
            const char *zid_str = NULL;
            size_t zid_len = 0;
            long long chef_val = 0;
            const json_value *stations_v;
            kos_zone *zone = &out->zones[i];

            if (json_typeof(zone_v) != JSON_OBJECT) {
                continue;
            }
            if (json_object_get_string(zone_v, "id", &zid_str, &zid_len) == JSON_OK) {
                copy_str(zone->id, sizeof zone->id, zid_str);
            }
            if (json_object_get_int(zone_v, "chef", &chef_val) == JSON_OK) {
                zone->chef_id = (int)chef_val;
            } else {
                zone->chef_id = (int)i;
            }
            if (json_object_get_array(zone_v, "stations", &stations_v) == JSON_OK) {
                size_t s_count = json_array_size(stations_v);
                size_t s_idx;

                if (s_count > KOS_ZONE_STATIONS_MAX) {
                    s_count = KOS_ZONE_STATIONS_MAX;
                }
                zone->station_count = (int)s_count;
                for (s_idx = 0; s_idx < s_count; s_idx++) {
                    const json_value *st_v = json_array_get(stations_v, s_idx);
                    if (st_v != NULL && json_typeof(st_v) == JSON_STRING) {
                        copy_str(zone->stations[s_idx], sizeof zone->stations[s_idx],
                                 json_string_value(st_v));
                    }
                }
            }
        }
    }

    out->from_file = 1;
    json_free(root);
    return KOS_OK;
}

int kos_zones_for_kitchen(const char *kitchen_path, kos_zones *out, kos_error *err)
{
    char path[4096];
    const char *slash;

    if (kitchen_path == NULL || out == NULL) {
        return KOS_ERR_INVAL;
    }
    slash = strrchr(kitchen_path, '/');
    if (slash == NULL) {
        snprintf(path, sizeof path, "zones.json");
    } else {
        size_t dir_len = (size_t)(slash - kitchen_path);

        if (dir_len + sizeof("/zones.json") > sizeof path) {
            return set_error(err, KOS_ERR_RANGE, "kitchen 路径过长: %s", kitchen_path);
        }
        memcpy(path, kitchen_path, dir_len);
        path[dir_len] = '\0';
        snprintf(path + dir_len, sizeof path - dir_len, "/zones.json");
    }
    return kos_zones_load(path, out, err);
}

int kos_zones_station_zone(const kos_zones *zones, const char *station_id)
{
    int i;
    int j;

    if (zones == NULL || station_id == NULL || !zones->from_file) {
        return -1;
    }
    if (zones->handoff.id[0] != '\0' && strcmp(zones->handoff.id, station_id) == 0) {
        return -1;
    }
    for (i = 0; i < zones->zone_count; i++) {
        for (j = 0; j < zones->zones[i].station_count; j++) {
            if (strcmp(zones->zones[i].stations[j], station_id) == 0) {
                return i;
            }
        }
    }
    return -1;
}

int kos_zones_chef_zone(const kos_zones *zones, int chef_id)
{
    int i;

    if (zones == NULL || !zones->from_file) {
        return -1;
    }
    for (i = 0; i < zones->zone_count; i++) {
        if (zones->zones[i].chef_id == chef_id) {
            return i;
        }
    }
    return -1;
}

int kos_zones_register_handoff(const kos_zones *zones, kos_kitchen *kitchen, kos_error *err)
{
    kos_station *st;

    if (zones == NULL || kitchen == NULL || !zones->from_file ||
        zones->handoff.id[0] == '\0') {
        return KOS_OK;
    }
    if (kos_kitchen_station_index(kitchen, zones->handoff.id) >= 0) {
        return KOS_OK; /* 已经存在 */
    }
    if (kitchen->station_count >= KOS_KITCHEN_MAX_STATIONS) {
        return set_error(err, KOS_ERR_RANGE, "工位数已达上限");
    }
    st = &kitchen->stations[kitchen->station_count++];
    copy_str(st->id, sizeof st->id, zones->handoff.id);
    copy_str(st->kind, sizeof st->kind,
             zones->handoff.kind[0] != '\0' ? zones->handoff.kind : "counter");
    st->gives[0] = '\0';
    st->x = zones->handoff.x;
    st->z = zones->handoff.z;

    return kos_stand_build(kitchen, err);
}
