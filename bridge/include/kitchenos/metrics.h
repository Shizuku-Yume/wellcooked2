#ifndef KITCHENOS_METRICS_H
#define KITCHENOS_METRICS_H

#include <kitchenos/recipes.h>
#include <kitchenos/resource.h>

#include <stddef.h>
#include <stdint.h>

#define KOS_METRICS_MAX_ORDERS 32
#define KOS_METRICS_NAME_MAX 64

typedef struct {
    char id[KOS_METRICS_NAME_MAX];
    char dish[KOS_RECIPE_NAME_MAX];
    char status[16];
    int completed;
    int64_t arrival_ms;
    int64_t deadline_ms;
    int64_t finished_ms;
    int64_t lateness_ms;
} kos_metrics_order;

typedef struct {
    char kind[KOS_RES_KIND_MAX];
    int capacity;
    int in_use;
    int64_t acquires;
    int64_t busy_ms;
    int64_t wait_ms;
    int64_t waits;
    int utilization_bp; /* busy_ms / (elapsed * capacity)，万分比 */
} kos_metrics_resource;

typedef struct {
    int64_t t_ms;
    const char *reason;
    int orders_total;
    int orders_completed;
    int orders_failed;
    int orders_active;
    int order_count;
    kos_metrics_order orders[KOS_METRICS_MAX_ORDERS];
    int resource_count;
    kos_metrics_resource resources[KOS_RES_MAX];
    int64_t dispatch_fcfs;
    int64_t dispatch_edf;
    int64_t dispatch_llf;
    int64_t dispatch_hrrn;
    int64_t worker_restarts;
} kos_metrics_snapshot;

/* 原子写入 metrics.json：先写 <path>.tmp，再 rename。 */
int kos_metrics_write(const char *path, const kos_metrics_snapshot *snapshot,
                      char *err, size_t err_size);

#endif
