/*
 * resource.c -- 命名信号量、租约与资源分配图。
 *
 * 只有一个进程（Scheduler）执行 acquire/release，因此这里不需要额外的锁；跨进程的
 * 互斥由 sem_t 自己提供。释放路径严格按 task_id 删除租约：删除失败就绝不 sem_post，
 * 这是"double release impossible"的实现点，而不是一句日志约定。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/resource.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

static int set_error(kos_error *err, int code, int sys_errno, const char *what,
                     const char *kind)
{
    if (err != NULL) {
        err->code = code;
        err->sys_errno = sys_errno;
        if (kind != NULL) {
            snprintf(err->context, sizeof err->context, "%.80s('%.80s')", what, kind);
        } else {
            snprintf(err->context, sizeof err->context, "%s", what);
        }
    }
    return code;
}

const char *kos_resource_name(const char *kind, int instance, char *buf, size_t len)
{
    char base[KOS_RES_NAME_MAX];

    if (kind == NULL || buf == NULL || len == 0 || kind[0] == '\0') {
        return NULL;
    }
    if (snprintf(base, sizeof base, "/kitchenos_res_%s", kind) >= (int)sizeof base) {
        return NULL;
    }
    if (kos_name_instance(base, instance, buf, len) != KOS_OK) {
        return NULL;
    }
    return buf;
}

static int kind_count(const kos_kitchen *kitchen, const char *kind)
{
    int count = 0;
    int i;

    for (i = 0; i < kitchen->station_count; i++) {
        if (strcmp(kitchen->stations[i].kind, kind) == 0) {
            count++;
        }
    }
    return count;
}

static int collect_kinds(const kos_kitchen *kitchen, char kinds[][KOS_RES_KIND_MAX],
                         int *count, kos_error *err)
{
    int i;

    *count = 0;
    for (i = 0; i < kitchen->station_count; i++) {
        const char *kind = kitchen->stations[i].kind;
        int j;
        int found = 0;

        for (j = 0; j < *count; j++) {
            if (strcmp(kinds[j], kind) == 0) {
                found = 1;
                break;
            }
        }
        if (found) {
            continue;
        }
        if (*count >= KOS_RES_MAX) {
            return set_error(err, KOS_ERR_RANGE, 0, "资源 kind 数量超过上限", kind);
        }
        copy_str(kinds[*count], KOS_RES_KIND_MAX, kind);
        (*count)++;
    }
    if (*count == 0) {
        return set_error(err, KOS_ERR_EMPTY, 0, "kitchen 没有工位", NULL);
    }
    return KOS_OK;
}

int kos_resources_create(const kos_kitchen *kitchen, int instance, kos_error *err)
{
    char kinds[KOS_RES_MAX][KOS_RES_KIND_MAX];
    int count = 0;
    int i;
    int rc;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (kitchen == NULL) {
        return KOS_ERR_INVAL;
    }
    rc = collect_kinds(kitchen, kinds, &count, err);
    if (rc != KOS_OK) {
        return rc;
    }
    for (i = 0; i < count; i++) {
        char name[KOS_RES_NAME_MAX];
        int capacity = kind_count(kitchen, kinds[i]);
        sem_t *sem;

        if (kos_resource_name(kinds[i], instance, name, sizeof name) == NULL) {
            return set_error(err, KOS_ERR_INVAL, 0, "资源名太长", kinds[i]);
        }
        if (sem_unlink(name) != 0 && errno != ENOENT) {
            return set_error(err, KOS_ERR_IO, errno, "sem_unlink", name);
        }
        sem = sem_open(name, O_CREAT | O_EXCL, 0644, (unsigned)capacity);
        if (sem == SEM_FAILED) {
            return set_error(err, KOS_ERR_IO, errno, "sem_open", name);
        }
        sem_close(sem);
    }
    return KOS_OK;
}

int kos_resources_attach(const kos_kitchen *kitchen, kos_resource_set *set, int instance,
                         kos_error *err)
{
    char kinds[KOS_RES_MAX][KOS_RES_KIND_MAX];
    int count = 0;
    int i;
    int rc;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (kitchen == NULL || set == NULL) {
        return KOS_ERR_INVAL;
    }
    memset(set, 0, sizeof *set);
    rc = collect_kinds(kitchen, kinds, &count, err);
    if (rc != KOS_OK) {
        return rc;
    }
    for (i = 0; i < count; i++) {
        kos_res_slot *item = &set->items[set->count];
        sem_t *sem;

        copy_str(item->kind, sizeof item->kind, kinds[i]);
        if (kos_resource_name(kinds[i], instance, item->name, sizeof item->name) == NULL) {
            kos_resources_close(set);
            return set_error(err, KOS_ERR_INVAL, 0, "资源名太长", kinds[i]);
        }
        item->capacity = kind_count(kitchen, kinds[i]);
        sem = sem_open(item->name, 0);
        if (sem == SEM_FAILED) {
            int saved = errno;

            kos_resources_close(set);
            if (saved == ENOENT) {
                return set_error(err, KOS_ERR_MISSING, saved, "sem_open", item->name);
            }
            return set_error(err, KOS_ERR_IO, saved, "sem_open", item->name);
        }
        item->sem = sem;
        set->count++;
    }
    return KOS_OK;
}

void kos_resources_close(kos_resource_set *set)
{
    int i;

    if (set == NULL) {
        return;
    }
    for (i = 0; i < set->count; i++) {
        if (set->items[i].sem != NULL) {
            sem_close(set->items[i].sem);
            set->items[i].sem = NULL;
        }
    }
    set->count = 0;
}

int kos_resources_unlink(const kos_kitchen *kitchen, int instance, kos_error *err)
{
    char kinds[KOS_RES_MAX][KOS_RES_KIND_MAX];
    int count = 0;
    int i;
    int rc;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (kitchen == NULL) {
        return KOS_ERR_INVAL;
    }
    rc = collect_kinds(kitchen, kinds, &count, err);
    if (rc != KOS_OK) {
        return rc;
    }
    for (i = 0; i < count; i++) {
        char name[KOS_RES_NAME_MAX];

        if (kos_resource_name(kinds[i], instance, name, sizeof name) == NULL) {
            return KOS_ERR_INVAL;
        }
        if (sem_unlink(name) != 0 && errno != ENOENT) {
            return set_error(err, KOS_ERR_IO, errno, "sem_unlink", name);
        }
    }
    return KOS_OK;
}

int kos_resources_index(const kos_resource_set *set, const char *kind)
{
    int i;

    if (set == NULL || kind == NULL) {
        return -1;
    }
    for (i = 0; i < set->count; i++) {
        if (strcmp(set->items[i].kind, kind) == 0) {
            return i;
        }
    }
    return -1;
}

int kos_resource_capacity(const kos_resource_set *set, const char *kind)
{
    int index = kos_resources_index(set, kind);

    return index >= 0 ? set->items[index].capacity : 0;
}

int kos_resource_try_acquire(kos_resource_set *set, const char *kind)
{
    int index = kos_resources_index(set, kind);

    if (index < 0) {
        return KOS_ERR_MISSING;
    }
    for (;;) {
        if (sem_trywait(set->items[index].sem) == 0) {
            return KOS_OK;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            return KOS_ERR_EMPTY;
        }
        return KOS_ERR_IO;
    }
}

int kos_resource_release(kos_resource_set *set, const char *kind)
{
    int index = kos_resources_index(set, kind);

    if (index < 0) {
        return KOS_ERR_MISSING;
    }
    if (sem_post(set->items[index].sem) != 0) {
        return KOS_ERR_IO;
    }
    return KOS_OK;
}

int kos_resource_value(const kos_resource_set *set, const char *kind, int *value)
{
    int index = kos_resources_index(set, kind);
    int local = 0;

    if (index < 0) {
        return KOS_ERR_MISSING;
    }
    if (sem_getvalue(set->items[index].sem, &local) != 0) {
        return KOS_ERR_IO;
    }
    if (local < 0) {
        local = 0; /* Linux 在有人等待时返回负数；对外只表达"可用数量" */
    }
    if (value != NULL) {
        *value = local;
    }
    return KOS_OK;
}

void kos_leases_init(kos_lease_table *table)
{
    if (table != NULL) {
        memset(table, 0, sizeof *table);
    }
}

kos_lease *kos_lease_add(kos_lease_table *table, const char *kind, int chef_id,
                         uint64_t task_id, int64_t acquired_ms, int64_t deadline_ms,
                         int order_index, int task_index)
{
    kos_lease *lease;
    int i;

    if (table == NULL || kind == NULL || task_id == 0 || chef_id < 0 ||
        table->count >= KOS_LEASE_MAX) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (table->leases[i].task_id == task_id ||
            table->leases[i].chef_id == chef_id) {
            return NULL;
        }
    }
    lease = &table->leases[table->count++];
    memset(lease, 0, sizeof *lease);
    lease->used = 1;
    copy_str(lease->kind, sizeof lease->kind, kind);
    lease->chef_id = chef_id;
    lease->task_id = task_id;
    lease->acquired_ms = acquired_ms;
    lease->deadline_ms = deadline_ms;
    lease->order_index = order_index;
    lease->task_index = task_index;
    return lease;
}

kos_lease *kos_lease_by_task(kos_lease_table *table, uint64_t task_id)
{
    int i;

    if (table == NULL) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (table->leases[i].task_id == task_id) {
            return &table->leases[i];
        }
    }
    return NULL;
}

kos_lease *kos_lease_by_chef(kos_lease_table *table, int chef_id)
{
    int i;

    if (table == NULL) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (table->leases[i].chef_id == chef_id) {
            return &table->leases[i];
        }
    }
    return NULL;
}

int kos_lease_expired(const kos_lease *lease, int64_t now_ms)
{
    return lease != NULL && lease->deadline_ms > 0 && now_ms >= lease->deadline_ms;
}

int kos_lease_remove(kos_lease_table *table, uint64_t task_id)
{
    int i;

    if (table == NULL || task_id == 0) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (table->leases[i].task_id == task_id) {
            int last = table->count - 1;

            if (i != last) {
                table->leases[i] = table->leases[last];
            }
            memset(&table->leases[last], 0, sizeof table->leases[last]);
            table->count--;
            return 1;
        }
    }
    return 0;
}

int kos_deadlock_find(const int *held, const int *wanted, int chef_count,
                      int resource_count, int *cycle, int *cycle_len)
{
    int owner[KOS_RES_MAX];
    int next[KOS_RES_MAX];
    int state[KOS_RES_MAX];
    int stack[KOS_RES_MAX];
    int start;
    int i;

    if (held == NULL || wanted == NULL || cycle == NULL || cycle_len == NULL ||
        chef_count < 0 || chef_count > KOS_RES_MAX ||
        resource_count < 0 || resource_count > KOS_RES_MAX) {
        return 0;
    }
    *cycle_len = 0;
    for (i = 0; i < resource_count; i++) {
        owner[i] = -1;
    }
    for (i = 0; i < chef_count; i++) {
        state[i] = 0;
        if (held[i] >= 0 && held[i] < resource_count) {
            if (owner[held[i]] >= 0) {
                return 0; /* 同一个资源被两个 chef 声明持有：数据坏了，不猜测 */
            }
            owner[held[i]] = i;
        }
    }
    for (i = 0; i < chef_count; i++) {
        next[i] = -1;
        if (wanted[i] >= 0 && wanted[i] < resource_count) {
            next[i] = owner[wanted[i]];
        }
    }

    for (start = 0; start < chef_count; start++) {
        int depth = 0;
        int node = start;

        if (state[start] != 0) {
            continue;
        }
        while (node >= 0) {
            int j;

            if (state[node] == 2) {
                break;
            }
            if (state[node] == 1) {
                int pos = 0;

                while (pos < depth && stack[pos] != node) {
                    pos++;
                }
                *cycle_len = depth - pos;
                for (j = 0; j < *cycle_len; j++) {
                    cycle[j] = stack[pos + j];
                }
                return 1;
            }
            state[node] = 1;
            stack[depth++] = node;
            node = next[node];
        }
        for (i = 0; i < depth; i++) {
            state[stack[i]] = 2;
        }
    }
    return 0;
}

int kos_deadlock_victim(const int *cycle, int cycle_len)
{
    int victim = -1;
    int i;

    if (cycle == NULL || cycle_len <= 0) {
        return -1;
    }
    for (i = 0; i < cycle_len; i++) {
        if (cycle[i] > victim) {
            victim = cycle[i];
        }
    }
    return victim;
}
