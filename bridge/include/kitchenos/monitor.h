#ifndef KITCHENOS_MONITOR_H
#define KITCHENOS_MONITOR_H

#include <kitchenos/shm.h>

/* 将一致的共享内存快照编码为 live.json；返回 0 成功。 */
int kos_monitor_snapshot_json(const kos_world_state *world, char *buf, size_t cap,
                              size_t *len_out);

#endif
