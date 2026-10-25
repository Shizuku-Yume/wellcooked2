/*
 * zones.h -- 分区流水线配置（zones.json）解析与工位/厨师归属。
 */
#ifndef KITCHENOS_ZONES_H
#define KITCHENOS_ZONES_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <kitchenos/kitchen.h>

#define KOS_ZONE_MAX 8
#define KOS_ZONE_NAME_MAX 32
#define KOS_ZONE_STATIONS_MAX 32

typedef struct {
    char id[KOS_ZONE_NAME_MAX];
    int chef_id;
    int station_count;
    char stations[KOS_ZONE_STATIONS_MAX][KOS_KITCHEN_NAME_MAX];
} kos_zone;

typedef struct {
    char id[KOS_KITCHEN_NAME_MAX];
    char kind[KOS_KITCHEN_NAME_MAX];
    float x;
    float z;
} kos_handoff_station;

typedef struct {
    char scene[KOS_KITCHEN_NAME_MAX];
    int zone_count;
    kos_zone zones[KOS_ZONE_MAX];
    kos_handoff_station handoff;
    int from_file;
} kos_zones;

/*
 * 从文件加载 zones.json。文件不存在返回 KOS_OK，from_file = 0。
 */
int kos_zones_load(const char *path, kos_zones *out, kos_error *err);

/* 在 kitchen.json 同目录找 zones.json；找不到返回 from_file = 0。 */
int kos_zones_for_kitchen(const char *kitchen_path, kos_zones *out, kos_error *err);

/* 工位所属分区下标（0..zone_count-1）；交接台面或未指定返回 -1。 */
int kos_zones_station_zone(const kos_zones *zones, const char *station_id);

/* 厨师所属分区下标（0..zone_count-1）；未指定返回 -1。 */
int kos_zones_chef_zone(const kos_zones *zones, int chef_id);

/* 如果 zones 包含 handoff 且 kitchen 里没有，把 handoff 加入 kitchen 并重建 stand。 */
int kos_zones_register_handoff(const kos_zones *zones, kos_kitchen *kitchen, kos_error *err);

#endif /* KITCHENOS_ZONES_H */
