/*
 * scheduler.c -- KitchenOS Scheduler。
 *
 * 两级调度：
 *   1. 订单级：policy.h 的 FCFS / EDF 只吃订单视图与资源视图。
 *   2. 工序级：订单展开成 recipes.json 的 DAG；前置完成且资源可预约的节点进就绪
 *      队列，按关键路径剩余工时挑最高的一个，派给空闲厨师。
 *
 * 任务走 POSIX mq（每厨师一条 /kitchenos_tasks_<chef>），结果从 /kitchenos_events
 * 回来。每次派发写 runs/<ts>/decisions.jsonl，状态变化写 events.jsonl；SIGHUP 在
 * FCFS/EDF 之间切换并留下一条 strategy 决策，SIGUSR1 写 checkpoint.json。
 *
 * 这里不碰游戏：世界状态只从 shm 读，动作只由 Worker 写输入槽。没有 Bridge 时
 * 它照样能凭 config/orders.json 的 menu 生成订单并派发，只是没人执行。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/child.h>
#include <kitchenos/json.h>
#include <kitchenos/kitchen.h>
#include <kitchenos/log.h>
#include <kitchenos/metrics.h>
#include <kitchenos/mq.h>
#include <kitchenos/policy.h>
#include <kitchenos/recipes.h>
#include <kitchenos/resource.h>
#include <kitchenos/shm.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SCHED_MAX_ORDERS 32
#define SCHED_MAX_TASKS KOS_RECIPE_MAX_DISH_NODES
#define SCHED_MAX_RESOURCES 32
#define SCHED_LINE_MAX 2048
#define SCHED_WORLD_POLL_MS 100
#define SCHED_ORDER_GAP_MS 700
#define SCHED_ORDER_REWARD_FALLBACK 10
#define SCHED_TASK_TIMEOUT_MS 30000

typedef enum {
    TASK_PENDING = 0,
    TASK_RUNNING = 1,
    TASK_DONE = 2,
    TASK_FAILED = 3
} task_state;

typedef struct {
    int node_index;
    int state;
    int chef_id;
    int avoid_chef; /* 上一任报 BLOCKED 的厨师；改派时先避开他 */
    uint64_t task_id;
    int64_t started_ms;
    int64_t ready_since_ms; /* 第一次进入就绪队列 */
    int64_t wait_started_ms; /* 最近一段资源等待的开始；0 = 未等待 */
    int wait_logged;
} sched_task;

typedef struct {
    char id[KOS_SHM_NAME_MAX];
    char dish[KOS_RECIPE_NAME_MAX];
    int dish_index;
    int64_t arrival_ms;
    int64_t deadline_ms;
    int64_t est_cost_ms;
    int reward;
    int status;
    int from_world;
    int64_t finished_ms;
    int64_t lateness_ms;
    int task_count;
    sched_task tasks[SCHED_MAX_TASKS];
} sched_order;

typedef struct {
    char kind[KOS_RES_KIND_MAX];
    int capacity;
    int64_t acquires;
    int64_t busy_ms;
    int64_t wait_ms;
    int64_t waits;
} sched_res_stat;

typedef struct {
    kos_child_runtime rt;
    kos_kitchen kitchen;
    kos_recipes recipes;
    kos_policy *policy;
    char kitchen_path[KOS_DAEMON_PATH_MAX];
    char recipes_path[KOS_DAEMON_PATH_MAX];
    char orders_path[KOS_DAEMON_PATH_MAX];
    char runs_dir[KOS_DAEMON_PATH_MAX];
    char run_dir[KOS_DAEMON_PATH_MAX + 32];
    char events_path[KOS_DAEMON_PATH_MAX + 48];
    char decisions_path[KOS_DAEMON_PATH_MAX + 48];
    int events_fd;
    int decisions_fd;

    sched_order orders[SCHED_MAX_ORDERS];
    int order_count;
    kos_resource_set resources;
    kos_lease_table leases;
    sched_res_stat res_stats[KOS_RES_MAX];
    int res_stat_count;
    int wanted_kind[KOS_SHM_MAX_CHEFS];
    int64_t dispatch_fcfs;
    int64_t dispatch_edf;
    int64_t worker_restarts;
    int next_chef;
    int blocked_hold[KOS_SHM_MAX_CHEFS]; /* 报过 BLOCKED 的厨师先不接新活 */
    char metrics_path[KOS_DAEMON_PATH_MAX + 48];
    uint64_t next_task_id;
    int64_t started_ms;
    int64_t next_world_poll_ms;
    uint64_t running_by_chef[KOS_SHM_MAX_CHEFS];

    mqd_t tasks[KOS_SHM_MAX_CHEFS];
    mqd_t events;
    int queues_ready;
} scheduler;

static int64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

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

static void error_text(const kos_error *err, char *buf, size_t len)
{
    kos_error_format(err, buf, len);
}

/* ============================================================ JSONL 输出 */

static int open_jsonl(const char *path, kos_error *err)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

    if (fd < 0) {
        if (err != NULL) {
            err->code = KOS_ERR_IO;
            err->sys_errno = errno;
            snprintf(err->context, sizeof err->context, "open('%s')", path);
        }
    }
    return fd;
}

static void write_jsonl(int fd, json_writer *writer)
{
    char line[SCHED_LINE_MAX + 2];
    size_t len;
    size_t off = 0;

    if (fd < 0 || json_writer_error(writer) != JSON_OK) {
        return;
    }
    len = json_writer_length(writer);
    if (len + 1 > sizeof line) {
        return;
    }
    memcpy(line, json_writer_text(writer), len);
    line[len++] = '\n';
    while (off < len) {
        ssize_t n = write(fd, line + off, len - off);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        off += (size_t)n;
    }
}

static void event_common(json_writer *w, scheduler *s, const char *event)
{
    json_writer_object_begin(w);
    json_writer_key(w, "t_ms");
    json_writer_int(w, now_ms() - s->started_ms);
    json_writer_key(w, "event");
    json_writer_string(w, event);
}

static void event_end(scheduler *s, json_writer *w)
{
    json_writer_object_end(w);
    write_jsonl(s->events_fd, w);
}

static void decision_common(json_writer *w, scheduler *s, const char *kind,
                            const char *reason)
{
    json_writer_object_begin(w);
    json_writer_key(w, "t_ms");
    json_writer_int(w, now_ms() - s->started_ms);
    json_writer_key(w, "kind");
    json_writer_string(w, kind);
    json_writer_key(w, "policy");
    json_writer_string(w, kos_policy_name(s->policy));
    json_writer_key(w, "reason");
    json_writer_string(w, reason);
}

/* ============================================================== 资源模型 */

static sched_res_stat *stat_find(scheduler *s, const char *kind)
{
    int i;

    for (i = 0; i < s->res_stat_count; i++) {
        if (strcmp(s->res_stats[i].kind, kind) == 0) {
            return &s->res_stats[i];
        }
    }
    return NULL;
}

static void resource_stats_build(scheduler *s)
{
    int i;

    s->res_stat_count = 0;
    for (i = 0; i < s->resources.count; i++) {
        sched_res_stat *stat = &s->res_stats[s->res_stat_count++];

        memset(stat, 0, sizeof *stat);
        copy_str(stat->kind, sizeof stat->kind, s->resources.items[i].kind);
        stat->capacity = s->resources.items[i].capacity;
    }
}

static int resource_has_token(scheduler *s, const char *kind)
{
    int value = 0;

    if (kind == NULL || kos_resource_value(&s->resources, kind, &value) != KOS_OK) {
        return 0;
    }
    return value > 0;
}

static void resource_event(scheduler *s, const char *event, const char *kind, int chef_id,
                           uint64_t task_id, const char *reason, int64_t wait_ms)
{
    json_writer w;
    char line[SCHED_LINE_MAX];

    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, event);
    json_writer_key(&w, "kind");
    json_writer_string(&w, kind);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef_id);
    json_writer_key(&w, "task_id");
    json_writer_int(&w, (long long)task_id);
    json_writer_key(&w, "reason");
    json_writer_string(&w, reason != NULL ? reason : "");
    if (wait_ms >= 0) {
        json_writer_key(&w, "wait_ms");
        json_writer_int(&w, wait_ms);
    }
    event_end(s, &w);
}

/*
 * 释放一个任务持有的租约。租约先按 task_id 删除；删除成功才 sem_post。这样重复的
 * DONE/FAILED、recovery 与 shutdown 同时到达也不可能把同一个令牌还两次。
 */
static int release_lease_for_task(scheduler *s, uint64_t task_id, const char *event_name,
                                  const char *reason)
{
    kos_lease *lease = kos_lease_by_task(&s->leases, task_id);
    sched_res_stat *stat;
    char kind[KOS_RES_KIND_MAX];
    int chef;
    int64_t acquired;
    int64_t now = now_ms();

    if (lease == NULL) {
        return 0;
    }
    copy_str(kind, sizeof kind, lease->kind);
    chef = lease->chef_id;
    acquired = lease->acquired_ms;
    /* 删除租约是释放令牌的唯一凭证：返回 0 代表别的路径已经释放过。 */
    if (kos_lease_remove(&s->leases, task_id) != 1) {
        return 0;
    }
    stat = stat_find(s, kind);
    if (stat != NULL) {
        stat->busy_ms += now - acquired;
    }
    if (kos_resource_release(&s->resources, kind) != KOS_OK) {
        kos_log_warn(s->rt.log, "resource_release_failed", "kind=%s task=%llu", kind,
                     (unsigned long long)task_id);
    }
    resource_event(s, event_name, kind, chef, task_id, reason, 0);
    /* 回收后从下一个厨师开始派发：避免刚丢掉租约的厨师在同一轮立刻抢回令牌，
     * 让另一个空闲厨师有机会继续订单。 */
    if (s->rt.opts.chefs > 0 &&
        (strcmp(reason, "lease-expired") == 0 ||
         strcmp(reason, "worker-recovered") == 0 ||
         strcmp(reason, "task-timeout") == 0)) {
        s->next_chef = (chef + 1) % s->rt.opts.chefs;
    }
    return 1;
}

/* 记录一次资源等待：每段等待只记一次，拿到令牌时再累加等待时长。 */
static void mark_resource_wait(scheduler *s, int chef, const sched_order *order,
                               sched_task *task, const kos_recipe_node *node)
{
    sched_res_stat *stat;

    if (task->wait_logged) {
        return;
    }
    s->wanted_kind[chef] = kos_resources_index(&s->resources, node->resource);
    task->wait_started_ms = now_ms();
    stat = stat_find(s, node->resource);
    if (stat != NULL) {
        stat->waits++;
    }
    {
        json_writer w;
        char line[SCHED_LINE_MAX];

        json_writer_init(&w, line, sizeof line);
        event_common(&w, s, "resource_wait");
        json_writer_key(&w, "kind");
        json_writer_string(&w, node->resource);
        json_writer_key(&w, "chef_id");
        json_writer_int(&w, chef);
        json_writer_key(&w, "order_id");
        json_writer_string(&w, order->id);
        json_writer_key(&w, "node");
        json_writer_string(&w, node->id);
        json_writer_key(&w, "wait_ms");
        json_writer_int(&w, 0);
        event_end(s, &w);
    }
    kos_log_info(s->rt.log, "resource_wait", "kind=%s order=%s node=%s chef=%d",
                 node->resource, order->id, node->id, chef);
    task->wait_logged = 1;
}

/* ============================================================== 订单模型 */

static int order_find(scheduler *s, const char *id)
{
    int i;

    for (i = 0; i < s->order_count; i++) {
        if (strcmp(s->orders[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static sched_task *task_find(scheduler *s, uint64_t task_id, int *order_index)
{
    int i;
    int j;

    for (i = 0; i < s->order_count; i++) {
        for (j = 0; j < s->orders[i].task_count; j++) {
            if (s->orders[i].tasks[j].task_id == task_id) {
                if (order_index != NULL) {
                    *order_index = i;
                }
                return &s->orders[i].tasks[j];
            }
        }
    }
    return NULL;
}

static int order_task_by_node(const sched_order *order, int node_index)
{
    int i;

    for (i = 0; i < order->task_count; i++) {
        if (order->tasks[i].node_index == node_index) {
            return i;
        }
    }
    return -1;
}

/*
 * 同一个订单里，某工序的所有子工序都 DONE 才算就绪；跨订单的同名节点不共享状态。
 */
static int order_node_ready(const scheduler *s, const sched_order *order, int node_index)
{
    int e;

    for (e = 0; e < s->recipes.edge_count; e++) {
        int child = s->recipes.edges[e].from;
        int parent = s->recipes.edges[e].to;
        int task_index;

        if (parent != node_index) {
            continue;
        }
        task_index = order_task_by_node(order, child);
        if (task_index < 0 || order->tasks[task_index].state != TASK_DONE) {
            return 0;
        }
    }
    return 1;
}

static int order_has_done_all(const sched_order *order)
{
    int i;

    for (i = 0; i < order->task_count; i++) {
        if (order->tasks[i].state != TASK_DONE) {
            return 0;
        }
    }
    return 1;
}

static void mark_order_finished(sched_order *order, int status, int64_t now)
{
    order->status = status;
    order->finished_ms = now;
    order->lateness_ms = now > order->deadline_ms ? now - order->deadline_ms : 0;
}

static void task_requeue(scheduler *s, int order_index, int task_index, const char *reason)
{
    sched_order *order;
    sched_task *task;
    json_writer w;
    char line[SCHED_LINE_MAX];
    int chef;

    if (order_index < 0 || order_index >= s->order_count) {
        return;
    }
    order = &s->orders[order_index];
    if (task_index < 0 || task_index >= order->task_count) {
        return;
    }
    task = &order->tasks[task_index];
    chef = task->chef_id;
    if (chef >= 0 && chef < KOS_SHM_MAX_CHEFS &&
        s->running_by_chef[chef] == task->task_id) {
        s->running_by_chef[task->chef_id] = 0;
    }
    task->state = TASK_PENDING;
    task->chef_id = -1;
    task->task_id = 0;
    task->ready_since_ms = 0;
    task->wait_started_ms = 0;
    task->wait_logged = 0;
    kos_log_warn(s->rt.log, "task_requeued", "order=%s node=%s reason=%s", order->id,
                 s->recipes.nodes[task->node_index].id, reason);
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, "task_requeued");
    json_writer_key(&w, "order_id");
    json_writer_string(&w, order->id);
    json_writer_key(&w, "node");
    json_writer_string(&w, s->recipes.nodes[task->node_index].id);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef);
    json_writer_key(&w, "reason");
    json_writer_string(&w, reason);
    event_end(s, &w);
}

/* 回收某厨师的所有租约；每条租约只走一次 release_lease_for_task。 */
static int reclaim_leases_for_chef(scheduler *s, int chef, const char *reason)
{
    int count = 0;

    for (;;) {
        kos_lease *lease = kos_lease_by_chef(&s->leases, chef);
        uint64_t task_id;
        int order_index;
        int task_index;

        if (lease == NULL) {
            break;
        }
        task_id = lease->task_id;
        order_index = lease->order_index;
        task_index = lease->task_index;
        (void)release_lease_for_task(s, task_id, "resource_reclaimed", reason);
        task_requeue(s, order_index, task_index, reason);
        count++;
    }
    return count;
}

/* 过期的租约：先记 lease_expired，再按唯一路径归还令牌并重新排队。 */
static void expire_leases(scheduler *s, int64_t now)
{
    for (;;) {
        kos_lease *expired = NULL;
        int i;

        for (i = 0; i < s->leases.count; i++) {
            if (kos_lease_expired(&s->leases.leases[i], now)) {
                expired = &s->leases.leases[i];
                break;
            }
        }
        if (expired == NULL) {
            return;
        }
        {
            uint64_t task_id = expired->task_id;
            int chef = expired->chef_id;
            int order_index = expired->order_index;
            int task_index = expired->task_index;
            char kind[KOS_RES_KIND_MAX];
            int64_t overdue = now - expired->deadline_ms;
            json_writer w;
            char line[SCHED_LINE_MAX];

            copy_str(kind, sizeof kind, expired->kind);
            kos_log_warn(s->rt.log, "lease_expired",
                         "kind=%s chef=%d task=%llu overdue_ms=%lld", kind, chef,
                         (unsigned long long)task_id, (long long)overdue);
            json_writer_init(&w, line, sizeof line);
            event_common(&w, s, "lease_expired");
            json_writer_key(&w, "kind");
            json_writer_string(&w, kind);
            json_writer_key(&w, "chef_id");
            json_writer_int(&w, chef);
            json_writer_key(&w, "task_id");
            json_writer_int(&w, (long long)task_id);
            json_writer_key(&w, "overdue_ms");
            json_writer_int(&w, overdue);
            event_end(s, &w);
            (void)release_lease_for_task(s, task_id, "resource_reclaimed",
                                         "lease-expired");
            task_requeue(s, order_index, task_index, "lease-expired");
        }
    }
}

/*
 * 资源分配图的环检测。当前每个任务只持有一个资源，正常执行不会成环；这里仍然按
 * §6.4 维护 held/wanted 图并保留 victim 规则，供以后多资源任务或真实阻塞等待使用。
 */
static void detect_deadlock(scheduler *s)
{
    int held[KOS_SHM_MAX_CHEFS];
    int wanted[KOS_SHM_MAX_CHEFS];
    int cycle[KOS_RES_MAX];
    int cycle_len = 0;
    int chef;
    int i;

    for (chef = 0; chef < s->rt.opts.chefs; chef++) {
        kos_lease *lease = kos_lease_by_chef(&s->leases, chef);

        held[chef] = lease != NULL ? kos_resources_index(&s->resources, lease->kind) : -1;
        wanted[chef] = s->wanted_kind[chef];
    }
    if (!kos_deadlock_find(held, wanted, s->rt.opts.chefs, s->resources.count, cycle,
                           &cycle_len)) {
        return;
    }
    {
        char text[128];
        size_t off = 0;
        int victim = kos_deadlock_victim(cycle, cycle_len);
        kos_lease *lease;
        json_writer w;
        char line[SCHED_LINE_MAX];

        for (i = 0; i < cycle_len && off + 8 < sizeof text; i++) {
            off += (size_t)snprintf(text + off, sizeof text - off, "%s%d",
                                    i == 0 ? "" : "->", cycle[i]);
        }
        kos_log_warn(s->rt.log, "deadlock_detected", "cycle=%s victim=%d", text, victim);
        json_writer_init(&w, line, sizeof line);
        event_common(&w, s, "deadlock_detected");
        json_writer_key(&w, "cycle");
        json_writer_string(&w, text);
        json_writer_key(&w, "victim");
        json_writer_int(&w, victim);
        event_end(s, &w);
        lease = kos_lease_by_chef(&s->leases, victim);
        if (lease != NULL) {
            uint64_t task_id = lease->task_id;
            int order_index = lease->order_index;
            int task_index = lease->task_index;

            (void)release_lease_for_task(s, task_id, "resource_reclaimed",
                                         "deadlock-victim");
            task_requeue(s, order_index, task_index, "deadlock-victim");
            json_writer_init(&w, line, sizeof line);
            event_common(&w, s, "deadlock_recovered");
            json_writer_key(&w, "victim");
            json_writer_int(&w, victim);
            json_writer_key(&w, "task_id");
            json_writer_int(&w, (long long)task_id);
            event_end(s, &w);
        }
    }
}

static int order_has_ready(scheduler *s, int order_index, int chef)
{
    const sched_order *order = &s->orders[order_index];
    int i;

    for (i = 0; i < order->task_count; i++) {
        int node = order->tasks[i].node_index;

        if (order->tasks[i].state == TASK_PENDING &&
            order->tasks[i].avoid_chef != chef && order_node_ready(s, order, node)) {
            return 1;
        }
    }
    return 0;
}

static int add_order(scheduler *s, const char *id, const char *dish, int64_t arrival_ms,
                     int64_t deadline_ms, int reward, int from_world, int64_t now)
{
    int dish_index;
    int order_index;
    sched_order *order;
    int i;
    json_writer w;
    char line[SCHED_LINE_MAX];

    if (s->order_count >= SCHED_MAX_ORDERS) {
        kos_log_warn(s->rt.log, "order_capacity", "orders=%d id=%s", s->order_count, id);
        return KOS_ERR_RANGE;
    }
    if (order_find(s, id) >= 0) {
        return KOS_ERR_EXISTS;
    }
    dish_index = kos_recipes_dish_index(&s->recipes, dish);
    if (dish_index < 0) {
        kos_log_warn(s->rt.log, "order_unknown_dish", "id=%s dish=%s", id, dish);
        return KOS_ERR_MISSING;
    }
    order_index = s->order_count++;
    order = &s->orders[order_index];
    memset(order, 0, sizeof *order);
    copy_str(order->id, sizeof order->id, id);
    copy_str(order->dish, sizeof order->dish, dish);
    order->dish_index = dish_index;
    order->arrival_ms = arrival_ms;
    order->deadline_ms = deadline_ms;
    order->est_cost_ms = kos_recipe_est_cost_ms(&s->recipes, dish_index);
    order->reward = reward > 0 ? reward : SCHED_ORDER_REWARD_FALLBACK;
    order->status = KOS_ORDER_ACTIVE;
    order->from_world = from_world;
    order->task_count = s->recipes.dishes[dish_index].node_count;
    for (i = 0; i < order->task_count; i++) {
        order->tasks[i].node_index = s->recipes.dishes[dish_index].nodes[i];
        order->tasks[i].state = TASK_PENDING;
        order->tasks[i].chef_id = -1;
        order->tasks[i].avoid_chef = -1;
        order->tasks[i].task_id = 0;
    }

    kos_log_info(s->rt.log, "order_arrived",
                 "id=%s dish=%s tasks=%d arrival_ms=%lld deadline_ms=%lld reward=%d "
                 "source=%s", id, dish, order->task_count, (long long)arrival_ms,
                 (long long)deadline_ms, order->reward, from_world ? "world" : "config");
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, "order_arrived");
    json_writer_key(&w, "order_id");
    json_writer_string(&w, id);
    json_writer_key(&w, "dish");
    json_writer_string(&w, dish);
    json_writer_key(&w, "tasks");
    json_writer_int(&w, order->task_count);
    json_writer_key(&w, "arrival_ms");
    json_writer_int(&w, arrival_ms - s->started_ms);
    json_writer_key(&w, "deadline_ms");
    json_writer_int(&w, deadline_ms - s->started_ms);
    json_writer_key(&w, "source");
    json_writer_string(&w, from_world ? "world" : "config");
    event_end(s, &w);
    (void)now;
    return KOS_OK;
}

/* ============================================================ 配置与启动 */

static int resolve_path(const char *explicit_path, const char *name, char *out, size_t len,
                        int required, kos_log *log)
{
    if (explicit_path != NULL && explicit_path[0] != '\0') {
        copy_str(out, len, explicit_path);
        return 0;
    }
    if (kos_child_default_config_path(name, out, len) == 0) {
        return 0;
    }
    if (required) {
        kos_log_error(log, "config_missing", "file=%s", name);
        return -1;
    }
    out[0] = '\0';
    return 0;
}

static int ensure_dir(const char *path, kos_log *log)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        kos_log_error(log, "mkdir_failed", "path=%s error=%s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int path_join(char *dst, size_t cap, const char *base, const char *name)
{
    size_t blen = strlen(base);
    size_t nlen = strlen(name);

    if (blen + 1 + nlen + 1 > cap) {
        return -1;
    }
    memcpy(dst, base, blen);
    dst[blen] = '/';
    memcpy(dst + blen + 1, name, nlen + 1);
    return 0;
}

static int prepare_run_dir(scheduler *s)
{
    char base[KOS_DAEMON_PATH_MAX + 16];
    char stamp[64];
    char name[96];
    struct tm local;
    time_t now = time(NULL);
    int suffix;

    if (s->runs_dir[0] != '\0') {
        copy_str(base, sizeof base, s->runs_dir);
    } else if (s->rt.opts.runs_dir[0] != '\0') {
        copy_str(base, sizeof base, s->rt.opts.runs_dir);
    } else if (path_join(base, sizeof base, s->rt.runtime_dir, "runs") != 0) {
        kos_log_error(s->rt.log, "run_dir_failed", "runtime path too long");
        return -1;
    }
    if (ensure_dir(base, s->rt.log) != 0) {
        return -1;
    }
    localtime_r(&now, &local);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    for (suffix = 0; suffix < 100; suffix++) {
        if (suffix == 0) {
            copy_str(name, sizeof name, stamp);
        } else {
            snprintf(name, sizeof name, "%s-%d", stamp, suffix);
        }
        if (path_join(s->run_dir, sizeof s->run_dir, base, name) != 0) {
            kos_log_error(s->rt.log, "run_dir_failed", "path too long");
            return -1;
        }
        if (mkdir(s->run_dir, 0755) == 0) {
            break;
        }
        if (errno != EEXIST) {
            kos_log_error(s->rt.log, "run_dir_failed", "path=%s error=%s", s->run_dir,
                          strerror(errno));
            return -1;
        }
    }
    if (path_join(s->events_path, sizeof s->events_path, s->run_dir, "events.jsonl") != 0 ||
        path_join(s->decisions_path, sizeof s->decisions_path, s->run_dir,
                  "decisions.jsonl") != 0 ||
        path_join(s->metrics_path, sizeof s->metrics_path, s->run_dir,
                  "metrics.json") != 0) {
        kos_log_error(s->rt.log, "run_dir_failed", "run file path too long");
        return -1;
    }
    s->events_fd = open_jsonl(s->events_path, NULL);
    s->decisions_fd = open_jsonl(s->decisions_path, NULL);
    if (s->events_fd < 0 || s->decisions_fd < 0) {
        kos_log_error(s->rt.log, "run_file_failed", "dir=%s error=%s", s->run_dir,
                      strerror(errno));
        return -1;
    }
    kos_log_info(s->rt.log, "run_dir", "path=%s events=%s decisions=%s metrics=%s",
                 s->run_dir, s->events_path, s->decisions_path, s->metrics_path);
    return 0;
}

static int open_queues(scheduler *s)
{
    kos_error err;
    int i;
    int rc;

    kos_error_clear(&err);
    rc = kos_mq_unlink_all(s->rt.opts.chefs, &err);
    if (rc != KOS_OK) {
        char text[256];

        error_text(&err, text, sizeof text);
        kos_log_warn(s->rt.log, "mq_stale_cleanup_failed", "error=%s", text);
    }
    kos_error_clear(&err);
    if (kos_mq_create(KOS_MQ_EVENTS_NAME, KOS_MQ_MAX_MESSAGES, &err) != KOS_OK) {
        char text[256];

        error_text(&err, text, sizeof text);
        kos_log_error(s->rt.log, "mq_create_failed", "name=%s error=%s",
                      KOS_MQ_EVENTS_NAME, text);
        return -1;
    }
    for (i = 0; i < s->rt.opts.chefs; i++) {
        char name[KOS_MQ_NAME_MAX];

        if (kos_mq_task_name(i, name, sizeof name) != KOS_OK) {
            return -1;
        }
        kos_error_clear(&err);
        if (kos_mq_create(name, KOS_MQ_MAX_MESSAGES, &err) != KOS_OK) {
            char text[256];

            error_text(&err, text, sizeof text);
            kos_log_error(s->rt.log, "mq_create_failed", "name=%s error=%s", name, text);
            return -1;
        }
    }
    kos_error_clear(&err);
    if (kos_mq_open(KOS_MQ_EVENTS_NAME, O_RDONLY, &s->events, &err) != KOS_OK) {
        char text[256];

        error_text(&err, text, sizeof text);
        kos_log_error(s->rt.log, "mq_open_failed", "name=%s error=%s", KOS_MQ_EVENTS_NAME,
                      text);
        return -1;
    }
    for (i = 0; i < s->rt.opts.chefs; i++) {
        char name[KOS_MQ_NAME_MAX];

        if (kos_mq_task_name(i, name, sizeof name) != KOS_OK) {
            return -1;
        }
        kos_error_clear(&err);
        if (kos_mq_open(name, O_WRONLY, &s->tasks[i], &err) != KOS_OK) {
            char text[256];

            error_text(&err, text, sizeof text);
            kos_log_error(s->rt.log, "mq_open_failed", "name=%s error=%s", name, text);
            return -1;
        }
    }
    s->queues_ready = 1;
    kos_log_info(s->rt.log, "mq_ready", "events=%s task_queues=%d", KOS_MQ_EVENTS_NAME,
                 s->rt.opts.chefs);
    return 0;
}

static void close_queues(scheduler *s)
{
    int i;

    for (i = 0; i < KOS_SHM_MAX_CHEFS; i++) {
        if (s->tasks[i] != (mqd_t)-1) {
            kos_mq_close(s->tasks[i]);
            s->tasks[i] = (mqd_t)-1;
        }
    }
    if (s->events != (mqd_t)-1) {
        kos_mq_close(s->events);
        s->events = (mqd_t)-1;
    }
    if (s->queues_ready) {
        kos_error err;

        kos_error_clear(&err);
        (void)kos_mq_unlink_all(s->rt.opts.chefs, &err);
        s->queues_ready = 0;
    }
}

/* ============================================================== 世界状态 */

static void poll_world_orders(scheduler *s, int64_t now)
{
    kos_world_state world;
    int i;

    if (s->rt.shm == NULL || now < s->next_world_poll_ms) {
        return;
    }
    s->next_world_poll_ms = now + SCHED_WORLD_POLL_MS;
    if (kos_shm_world_read(s->rt.shm, &world, KOS_SHM_READ_ATTEMPTS, NULL) != KOS_OK) {
        return;
    }
    for (i = 0; i < (int)world.order_count; i++) {
        const kos_order *order = &world.orders[i];
        int64_t deadline;

        if (order->id[0] == '\0' || order_find(s, order->id) >= 0) {
            continue;
        }
        deadline = now + (order->time_remaining > 0 ? order->time_remaining * 1000LL : 30000);
        if (add_order(s, order->id, order->recipe, now, deadline, 0, 1, now) == KOS_OK) {
            kos_log_info(s->rt.log, "world_order", "id=%s dish=%s time_remaining=%d",
                         order->id, order->recipe, order->time_remaining);
        }
    }
}

/* ============================================================== 事件处理 */

static void finish_order_if_done(scheduler *s, int order_index)
{
    sched_order *order = &s->orders[order_index];
    json_writer w;
    char line[SCHED_LINE_MAX];

    if (order->status != KOS_ORDER_ACTIVE || !order_has_done_all(order)) {
        return;
    }
    mark_order_finished(order, KOS_ORDER_COMPLETED, now_ms());
    kos_log_info(s->rt.log, "order_completed", "id=%s dish=%s tasks=%d", order->id,
                 order->dish, order->task_count);
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, "order_completed");
    json_writer_key(&w, "order_id");
    json_writer_string(&w, order->id);
    json_writer_key(&w, "dish");
    json_writer_string(&w, order->dish);
    event_end(s, &w);
}

static void handle_worker_event(scheduler *s, const kos_mq_msg *msg)
{
    sched_task *task;
    int order_index = -1;
    sched_order *order;
    json_writer w;
    char line[SCHED_LINE_MAX];
    const char *event_name;

    if (msg->kind == KOS_MQ_EVENT && msg->event == KOS_EVT_RECOVERED) {
        int requeued;

        if (msg->generation > 1) {
            s->worker_restarts++;
        }
        kos_log_info(s->rt.log, "worker_recovered", "chef=%d detail=%s", msg->chef_id,
                     msg->detail);
        requeued = reclaim_leases_for_chef(s, msg->chef_id, "worker-recovered");
        json_writer_init(&w, line, sizeof line);
        event_common(&w, s, "worker_recovered");
        json_writer_key(&w, "chef_id");
        json_writer_int(&w, msg->chef_id);
        json_writer_key(&w, "requeued");
        json_writer_int(&w, requeued);
        event_end(s, &w);
        return;
    }
    if (msg->kind != KOS_MQ_EVENT) {
        kos_log_warn(s->rt.log, "mq_bad_kind", "kind=%u task=%llu", msg->kind,
                     (unsigned long long)msg->task_id);
        return;
    }
    task = task_find(s, msg->task_id, &order_index);
    if (task == NULL || order_index < 0) {
        kos_log_warn(s->rt.log, "event_unknown_task", "task=%llu chef=%d event=%d",
                     (unsigned long long)msg->task_id, msg->chef_id, msg->event);
        return;
    }
    order = &s->orders[order_index];
    event_name = msg->event == KOS_EVT_ACCEPTED
                     ? "task_accepted"
                     : (msg->event == KOS_EVT_DONE
                            ? "task_done"
                            : (msg->event == KOS_EVT_BLOCKED ? "task_blocked" : "task_failed"));

    kos_log_info(s->rt.log, event_name,
                 "order=%s task=%llu chef=%d node=%s detail=%s", order->id,
                 (unsigned long long)msg->task_id, msg->chef_id, msg->node_id,
                 msg->detail);
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, event_name);
    json_writer_key(&w, "order_id");
    json_writer_string(&w, order->id);
    json_writer_key(&w, "task_id");
    json_writer_int(&w, (long long)msg->task_id);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, msg->chef_id);
    json_writer_key(&w, "node");
    json_writer_string(&w, msg->node_id);
    if (msg->detail[0] != '\0') {
        json_writer_key(&w, "detail");
        json_writer_string(&w, msg->detail);
    }
    event_end(s, &w);

    if (msg->event == KOS_EVT_ACCEPTED) {
        return;
    }
    if (task->state != TASK_RUNNING) {
        kos_log_warn(s->rt.log, "event_stale", "task=%llu state=%d",
                     (unsigned long long)msg->task_id, task->state);
        return;
    }

    if (task->chef_id >= 0 && task->chef_id < KOS_SHM_MAX_CHEFS &&
        s->running_by_chef[task->chef_id] == task->task_id) {
        s->running_by_chef[task->chef_id] = 0;
    }
    {
        int node = task->node_index;

        if (msg->event == KOS_EVT_DONE) {
            (void)release_lease_for_task(s, msg->task_id, "resource_released",
                                         "task-done");
            task->state = TASK_DONE;
            finish_order_if_done(s, order_index);
        } else if (msg->event == KOS_EVT_BLOCKED) {
            /* 走位连续 3 次卡住：租约还给资源模型，工序回就绪队列，并记住
             * 别立刻又派给同一个人（他那里仍然走不通）。 */
            int blocked_chef = msg->chef_id;
            int task_index = (int)(task - order->tasks);

            if (blocked_chef >= 0 && blocked_chef < KOS_SHM_MAX_CHEFS) {
                s->blocked_hold[blocked_chef] = 1;
            }
            (void)release_lease_for_task(s, msg->task_id, "resource_reclaimed",
                                         "task-blocked");
            task_requeue(s, order_index, task_index, "worker-blocked");
            if (task_index >= 0 && task_index < order->task_count) {
                order->tasks[task_index].avoid_chef = blocked_chef;
            }
        } else {
            (void)release_lease_for_task(s, msg->task_id, "resource_reclaimed",
                                         "task-failed");
            task->state = TASK_FAILED;
            if (!order_has_done_all(order)) {
                mark_order_finished(order, KOS_ORDER_FAILED, now_ms());
            }
            kos_log_warn(s->rt.log, "task_failed", "order=%s node=%s detail=%s",
                         order->id, s->recipes.nodes[node].id, msg->detail);
        }
    }
}

static void drain_events(scheduler *s)
{
    for (;;) {
        kos_mq_msg msg;
        kos_error err;

        if (s->events == (mqd_t)-1) {
            return;
        }
        kos_error_clear(&err);
        if (kos_mq_receive(s->events, &msg, 0, &err) != KOS_OK) {
            return;
        }
        handle_worker_event(s, &msg);
    }
}

/* ============================================================== 调度决策 */

static int best_ready_task(scheduler *s, int order_index, int64_t now, int chef)
{
    sched_order *order = &s->orders[order_index];
    int best = -1;
    int64_t best_critical = 0;
    int i;

    for (i = 0; i < order->task_count; i++) {
        int node = order->tasks[i].node_index;
        int64_t critical;

        if (order->tasks[i].state != TASK_PENDING ||
            order->tasks[i].avoid_chef == chef ||
            !order_node_ready(s, order, node)) {
            continue;
        }
        if (order->tasks[i].ready_since_ms == 0) {
            order->tasks[i].ready_since_ms = now;
        }
        critical = kos_recipe_critical_ms(&s->recipes, order->dish_index, node);
        if (best < 0 || critical > best_critical ||
            (critical == best_critical && node < order->tasks[best].node_index)) {
            best = i;
            best_critical = critical;
        }
    }
    return best;
}

static unsigned task_priority(const sched_order *order, int64_t now)
{
    int64_t slack = order->deadline_ms - now;
    int seconds = slack > 0 ? (int)(slack / 1000) : 0;

    if (seconds > 31) {
        seconds = 31;
    }
    return (unsigned)(32 - seconds);
}

static int dispatch_one(scheduler *s, int chef, int64_t now)
{
    kos_order_view views[SCHED_MAX_ORDERS];
    int map[SCHED_MAX_ORDERS];
    int view_count = 0;
    int chosen = -1;
    int order_index;
    int task_index;
    sched_order *order;
    sched_task *task;
    kos_recipe_node *node;
    const char *station_hint;
    int station_index = -1;
    const kos_station *station;
    char station_id[KOS_KITCHEN_NAME_MAX] = "";
    float station_x = 0.0f;
    float station_z = 0.0f;
    kos_mq_msg msg;
    kos_error err;
    char line[SCHED_LINE_MAX];
    json_writer w;
    int i;

    for (i = 0; i < s->order_count; i++) {
        sched_order *candidate = &s->orders[i];

        if (candidate->status != KOS_ORDER_ACTIVE || now < candidate->arrival_ms ||
            !order_has_ready(s, i, chef)) {
            continue;
        }
        memset(&views[view_count], 0, sizeof views[view_count]);
        copy_str(views[view_count].id, sizeof views[view_count].id, candidate->id);
        copy_str(views[view_count].dish, sizeof views[view_count].dish, candidate->dish);
        views[view_count].arrival_ms = candidate->arrival_ms;
        views[view_count].deadline_ms = candidate->deadline_ms;
        views[view_count].est_cost_ms = candidate->est_cost_ms;
        views[view_count].reward = candidate->reward;
        views[view_count].status = candidate->status;
        map[view_count] = i;
        view_count++;
    }
    if (view_count == 0) {
        /* 所有订单都在等资源：仍要记录等待者，否则 metrics 的 waits 永远是 0。 */
        for (i = 0; i < s->order_count; i++) {
            sched_order *blocked = &s->orders[i];
            int j;

            if (blocked->status != KOS_ORDER_ACTIVE || now < blocked->arrival_ms) {
                continue;
            }
            for (j = 0; j < blocked->task_count; j++) {
                int node_index = blocked->tasks[j].node_index;
                const kos_recipe_node *blocked_node = &s->recipes.nodes[node_index];

                if (blocked->tasks[j].state != TASK_PENDING ||
                    !order_node_ready(s, blocked, node_index) ||
                    resource_has_token(s, blocked_node->resource)) {
                    continue;
                }
                mark_resource_wait(s, chef, blocked, &blocked->tasks[j], blocked_node);
                return -1;
            }
        }
        return 0;
    }
    if (kos_policy_pick(s->policy, views, view_count, NULL, 0, now, &chosen) != KOS_OK) {
        return 0;
    }
    order_index = map[chosen];
    order = &s->orders[order_index];
    task_index = best_ready_task(s, order_index, now, chef);
    if (task_index < 0) {
        return 0;
    }
    task = &order->tasks[task_index];
    node = &s->recipes.nodes[task->node_index];

    station_hint = node->ingredient[0] != '\0' ? node->ingredient : node->label;
    if (kos_kitchen_pick_station(&s->kitchen, node->resource, station_hint,
                                 (unsigned)(s->next_task_id + (uint64_t)chef), &station_index) !=
        KOS_OK) {
        kos_log_warn(s->rt.log, "task_no_station", "node=%s resource=%s", node->id,
                     node->resource);
        task->state = TASK_FAILED;
        order->status = KOS_ORDER_FAILED;
        json_writer_init(&w, line, sizeof line);
        event_common(&w, s, "task_failed");
        json_writer_key(&w, "order_id");
        json_writer_string(&w, order->id);
        json_writer_key(&w, "node");
        json_writer_string(&w, node->id);
        json_writer_key(&w, "detail");
        json_writer_string(&w, "no station for resource");
        event_end(s, &w);
        return 0;
    }
    station = &s->kitchen.stations[station_index];
    copy_str(station_id, sizeof station_id, station->id);
    station_x = station->x;
    station_z = station->z;

    {
        int rc = kos_resource_try_acquire(&s->resources, node->resource);

        if (rc == KOS_ERR_EMPTY) {
            mark_resource_wait(s, chef, order, task, node);
            return -1;
        }
        if (rc != KOS_OK) {
            kos_log_warn(s->rt.log, "resource_acquire_failed", "kind=%s rc=%d",
                         node->resource, rc);
            return 0;
        }
        s->wanted_kind[chef] = -1;
    }

    memset(&msg, 0, sizeof msg);
    msg.version = KOS_MQ_VERSION;
    msg.kind = KOS_MQ_TASK;
    msg.task_id = ++s->next_task_id;
    msg.chef_id = chef;
    msg.priority = (int)task_priority(order, now);
    msg.duration_ms = node->duration_ms;
    msg.target_x = station_x;
    msg.target_y = station_z;
    copy_str(msg.order_id, sizeof msg.order_id, order->id);
    copy_str(msg.dish, sizeof msg.dish, order->dish);
    copy_str(msg.node_id, sizeof msg.node_id, node->id);
    copy_str(msg.op, sizeof msg.op, node->op);
    copy_str(msg.resource, sizeof msg.resource, node->resource);
    copy_str(msg.station_id, sizeof msg.station_id, station_id);

    kos_error_clear(&err);
    if (kos_mq_send(s->tasks[chef], &msg, (unsigned)msg.priority, 20, &err) != KOS_OK) {
        char text[256];

        error_text(&err, text, sizeof text);
        kos_log_warn(s->rt.log, "task_send_failed", "order=%s node=%s chef=%d error=%s",
                     order->id, node->id, chef, text);
        (void)kos_resource_release(&s->resources, node->resource);
        return -1;
    }

    {
        int64_t wait_ms = task->wait_started_ms > 0 ? now - task->wait_started_ms : 0;
        sched_res_stat *stat = stat_find(s, node->resource);
        int64_t deadline = now + s->rt.opts.lease_ttl_ms;
        int was_waiting = task->wait_logged;

        if (kos_lease_add(&s->leases, node->resource, chef, msg.task_id, now, deadline,
                          order_index, task_index) == NULL) {
            kos_log_error(s->rt.log, "lease_add_failed", "kind=%s chef=%d task=%llu",
                          node->resource, chef, (unsigned long long)msg.task_id);
            (void)kos_resource_release(&s->resources, node->resource);
            return -1;
        }
        if (stat != NULL) {
            stat->acquires++;
            if (was_waiting && wait_ms > 0) {
                stat->wait_ms += wait_ms;
            }
        }
        resource_event(s, "resource_acquired", node->resource, chef, msg.task_id,
                       "dispatch", wait_ms);
        task->wait_logged = 0;
        task->wait_started_ms = 0;
    }

    task->state = TASK_RUNNING;
    if (task->avoid_chef >= 0 && task->avoid_chef != chef &&
        task->avoid_chef < KOS_SHM_MAX_CHEFS) {
        s->blocked_hold[task->avoid_chef] = 0;
    }
    task->chef_id = chef;
    task->avoid_chef = -1;
    task->task_id = msg.task_id;
    task->started_ms = now;
    s->running_by_chef[chef] = msg.task_id;

    if (kos_policy_kind_of(s->policy) == KOS_POLICY_EDF) {
        s->dispatch_edf++;
    } else {
        s->dispatch_fcfs++;
    }

    kos_log_info(s->rt.log, "task_dispatched",
                 "order=%s node=%s chef=%d task=%llu priority=%d crit_ms=%lld "
                 "policy=%s", order->id, node->id, chef,
                 (unsigned long long)msg.task_id, msg.priority,
                 (long long)kos_recipe_critical_ms(&s->recipes, order->dish_index,
                                                   task->node_index),
                 kos_policy_name(s->policy));

    json_writer_init(&w, line, sizeof line);
    decision_common(&w, s, "dispatch", "list-scheduling");
    json_writer_key(&w, "order_id");
    json_writer_string(&w, order->id);
    json_writer_key(&w, "dish");
    json_writer_string(&w, order->dish);
    json_writer_key(&w, "task_id");
    json_writer_int(&w, (long long)msg.task_id);
    json_writer_key(&w, "node");
    json_writer_string(&w, node->id);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef);
    json_writer_key(&w, "priority");
    json_writer_int(&w, msg.priority);
    json_writer_key(&w, "critical_ms");
    json_writer_int(&w, (long long)kos_recipe_critical_ms(&s->recipes, order->dish_index,
                                                          task->node_index));
    json_writer_key(&w, "slack_ms");
    json_writer_int(&w, (long long)(order->deadline_ms - now));
    json_writer_key(&w, "ready_orders");
    json_writer_int(&w, view_count);
    json_writer_object_end(&w);
    write_jsonl(s->decisions_fd, &w);

    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, "task_dispatched");
    json_writer_key(&w, "order_id");
    json_writer_string(&w, order->id);
    json_writer_key(&w, "task_id");
    json_writer_int(&w, (long long)msg.task_id);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef);
    json_writer_key(&w, "node");
    json_writer_string(&w, node->id);
    json_writer_key(&w, "station");
    json_writer_string(&w, station_id);
    event_end(s, &w);
    return 1;
}

static int has_avoid_task(const scheduler *s, int chef)
{
    int i;

    for (i = 0; i < s->order_count; i++) {
        const sched_order *order = &s->orders[i];
        int j;

        if (order->status != KOS_ORDER_ACTIVE) {
            continue;
        }
        for (j = 0; j < order->task_count; j++) {
            if (order->tasks[j].state == TASK_PENDING &&
                order->tasks[j].avoid_chef == chef) {
                return 1;
            }
        }
    }
    return 0;
}

static void dispatch_ready(scheduler *s, int64_t now)
{
    int offset;

    if (s->rt.opts.chefs <= 0) {
        return;
    }
    /* 轮转起点，避免每次空闲都从 chef 0 开始而让后面的厨师长期饿着。 */
    for (offset = 0; offset < s->rt.opts.chefs; offset++) {
        int chef = (s->next_chef + offset) % s->rt.opts.chefs;
        int rc;

        if (s->blocked_hold[chef] && !has_avoid_task(s, chef)) {
            /* 被重排的工序已经完成/失败，扣留自然解除，避免这个厨师永远没活。 */
            s->blocked_hold[chef] = 0;
        }
        if (s->running_by_chef[chef] != 0 || s->blocked_hold[chef]) {
            continue;
        }
        rc = dispatch_one(s, chef, now);
        if (rc == 0) {
            break;
        }
        /* rc < 0: 这个厨师要的资源暂时没有；继续看下一个厨师，可能用别的资源。 */
    }
    s->next_chef = (s->next_chef + 1) % s->rt.opts.chefs;
}

/* ============================================================== 信号动作 */

/*
 * SIGHUP 的"重读配置"部分：只有没有活跃订单时才替换 DAG 与网格。订单引用的是
 * dish 下标，运行中换表会让已派发的任务指向另一道菜；宁可记录 deferred，也不要
 * 让一个正在执行的节点突然失去定义。
 */
static void reload_config_on_sighup(scheduler *s)
{
    kos_recipes recipes;
    kos_kitchen kitchen;
    kos_error err;
    int active = 0;
    int i;

    for (i = 0; i < s->order_count; i++) {
        if (s->orders[i].status == KOS_ORDER_ACTIVE) {
            active++;
        }
    }
    memset(&recipes, 0, sizeof recipes);
    memset(&kitchen, 0, sizeof kitchen);
    kos_error_clear(&err);
    if (kos_recipes_load(s->recipes_path, &recipes, &err) != KOS_OK) {
        kos_log_warn(s->rt.log, "config_reload_failed", "recipes=%s", s->recipes_path);
        return;
    }
    kos_error_clear(&err);
    if (kos_kitchen_load(s->kitchen_path, &kitchen, &err) != KOS_OK) {
        kos_log_warn(s->rt.log, "config_reload_failed", "kitchen=%s", s->kitchen_path);
        return;
    }
    if (active > 0) {
        kos_log_info(s->rt.log, "config_reload_deferred", "active_orders=%d", active);
        return;
    }
    s->recipes = recipes;
    s->kitchen = kitchen;
    kos_log_info(s->rt.log, "config_reload_applied",
                 "recipes=%s nodes=%d dishes=%d kitchen=%s cells=%d", s->recipes_path,
                 s->recipes.node_count, s->recipes.dish_count, s->kitchen_path,
                 s->kitchen.cell_count);
}

static void strategy_switch(scheduler *s)
{
    kos_policy_kind from = kos_policy_kind_of(s->policy);
    kos_policy_kind to = kos_policy_other(from);
    json_writer w;
    char line[SCHED_LINE_MAX];

    kos_policy_set_kind(s->policy, to);
    kos_log_info(s->rt.log, "strategy_switch", "from=%s to=%s reason=SIGHUP",
                 from == KOS_POLICY_EDF ? "EDF" : "FCFS",
                 to == KOS_POLICY_EDF ? "EDF" : "FCFS");
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, "strategy_switch");
    json_writer_key(&w, "from");
    json_writer_string(&w, from == KOS_POLICY_EDF ? "EDF" : "FCFS");
    json_writer_key(&w, "to");
    json_writer_string(&w, to == KOS_POLICY_EDF ? "EDF" : "FCFS");
    event_end(s, &w);

    json_writer_init(&w, line, sizeof line);
    decision_common(&w, s, "strategy", "SIGHUP");
    json_writer_key(&w, "from");
    json_writer_string(&w, from == KOS_POLICY_EDF ? "EDF" : "FCFS");
    json_writer_key(&w, "to");
    json_writer_string(&w, to == KOS_POLICY_EDF ? "EDF" : "FCFS");
    json_writer_object_end(&w);
    write_jsonl(s->decisions_fd, &w);
}

static const char *order_status_name(int status)
{
    switch (status) {
    case KOS_ORDER_COMPLETED:
        return "completed";
    case KOS_ORDER_FAILED:
        return "failed";
    default:
        return "active";
    }
}

static void write_metrics_snapshot(scheduler *s, const char *reason)
{
    kos_metrics_snapshot snapshot;
    char err[256];
    int64_t elapsed = now_ms() - s->started_ms;
    int i;

    if (s->metrics_path[0] == '\0') {
        return;
    }
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.t_ms = elapsed;
    snapshot.reason = reason;
    snapshot.orders_total = s->order_count;
    snapshot.dispatch_fcfs = s->dispatch_fcfs;
    snapshot.dispatch_edf = s->dispatch_edf;
    snapshot.worker_restarts = s->worker_restarts;

    for (i = 0; i < s->order_count && i < KOS_METRICS_MAX_ORDERS; i++) {
        kos_metrics_order *dst = &snapshot.orders[snapshot.order_count++];
        const sched_order *src = &s->orders[i];

        copy_str(dst->id, sizeof dst->id, src->id);
        copy_str(dst->dish, sizeof dst->dish, src->dish);
        copy_str(dst->status, sizeof dst->status, order_status_name(src->status));
        dst->completed = src->status == KOS_ORDER_COMPLETED;
        dst->arrival_ms = src->arrival_ms - s->started_ms;
        dst->deadline_ms = src->deadline_ms - s->started_ms;
        dst->finished_ms = src->finished_ms > 0 ? src->finished_ms - s->started_ms : 0;
        dst->lateness_ms = src->lateness_ms;
        if (src->status == KOS_ORDER_COMPLETED) {
            snapshot.orders_completed++;
        } else if (src->status == KOS_ORDER_FAILED) {
            snapshot.orders_failed++;
        } else {
            snapshot.orders_active++;
        }
    }
    for (i = 0; i < s->res_stat_count && i < KOS_RES_MAX; i++) {
        kos_metrics_resource *dst = &snapshot.resources[snapshot.resource_count++];
        const sched_res_stat *src = &s->res_stats[i];
        int free_tokens = 0;

        copy_str(dst->kind, sizeof dst->kind, src->kind);
        dst->capacity = src->capacity;
        if (kos_resource_value(&s->resources, src->kind, &free_tokens) == KOS_OK) {
            dst->in_use = src->capacity - free_tokens;
        }
        dst->acquires = src->acquires;
        dst->busy_ms = src->busy_ms;
        dst->wait_ms = src->wait_ms;
        dst->waits = src->waits;
        if (elapsed > 0 && src->capacity > 0) {
            int64_t denom = elapsed * src->capacity;
            dst->utilization_bp = (int)(src->busy_ms * 10000 / denom);
        }
    }

    /* 仍在等待的 pending 任务也要把已经等过的时间算进本次快照。 */
    for (i = 0; i < s->order_count; i++) {
        int j;

        for (j = 0; j < s->orders[i].task_count; j++) {
            const sched_task *task = &s->orders[i].tasks[j];
            int k;

            if (!task->wait_logged || task->wait_started_ms <= 0) {
                continue;
            }
            for (k = 0; k < snapshot.resource_count; k++) {
                if (strcmp(snapshot.resources[k].kind,
                           s->recipes.nodes[task->node_index].resource) == 0) {
                    snapshot.resources[k].wait_ms += now_ms() - task->wait_started_ms;
                    break;
                }
            }
        }
    }

    if (kos_metrics_write(s->metrics_path, &snapshot, err, sizeof err) != 0) {
        kos_log_warn(s->rt.log, "metrics_write_failed", "path=%s error=%s",
                     s->metrics_path, err);
        return;
    }
    kos_log_info(s->rt.log, "metrics_written", "path=%s reason=%s orders=%d resources=%d",
                 s->metrics_path, reason, snapshot.orders_total,
                 snapshot.resource_count);
}

/* ============================================================== 启动配置 */

static int load_config(scheduler *s)
{
    kos_error err;
    char default_recipes[KOS_DAEMON_PATH_MAX];
    char default_kitchen[KOS_DAEMON_PATH_MAX];
    int rc;

    if (resolve_path(s->rt.opts.recipes_path, "recipes.example.json", default_recipes,
                     sizeof default_recipes, 1, s->rt.log) != 0) {
        return -1;
    }
    if (resolve_path(s->rt.opts.kitchen_path, "kitchen.example.json", default_kitchen,
                     sizeof default_kitchen, 1, s->rt.log) != 0) {
        return -1;
    }
    copy_str(s->recipes_path, sizeof s->recipes_path, default_recipes);
    copy_str(s->kitchen_path, sizeof s->kitchen_path, default_kitchen);

    kos_error_clear(&err);
    if (kos_recipes_load(s->recipes_path, &s->recipes, &err) != KOS_OK) {
        char text[256];

        error_text(&err, text, sizeof text);
        kos_log_error(s->rt.log, "recipes_load_failed", "path=%s error=%s",
                      s->recipes_path, text);
        return -1;
    }
    kos_error_clear(&err);
    if (kos_kitchen_load(s->kitchen_path, &s->kitchen, &err) != KOS_OK) {
        char text[256];

        error_text(&err, text, sizeof text);
        kos_log_error(s->rt.log, "kitchen_load_failed", "path=%s error=%s",
                      s->kitchen_path, text);
        return -1;
    }
    kos_log_info(s->rt.log, "config_loaded",
                 "kitchen=%s cells=%d stations=%d recipes=%s nodes=%d dishes=%d "
                 "edges=%d resources=%d", s->kitchen_path, s->kitchen.cell_count,
                 s->kitchen.station_count, s->recipes_path, s->recipes.node_count,
                 s->recipes.dish_count, s->recipes.edge_count, s->resources.count);
    {
        int i;

        for (i = 0; i < s->recipes.node_count; i++) {
            int dummy = -1;
            const kos_recipe_node *node = &s->recipes.nodes[i];
            const char *hint = node->ingredient[0] != '\0' ? node->ingredient : node->label;

            if (kos_kitchen_pick_station(&s->kitchen, node->resource, hint, 0, &dummy) !=
                KOS_OK) {
                kos_log_error(s->rt.log, "recipe_no_station", "node=%s resource=%s",
                              node->id, node->resource);
            }
        }
    }

    if (resolve_path(s->rt.opts.orders_path, "orders.example.json", s->orders_path,
                     sizeof s->orders_path, 0, s->rt.log) == 0 &&
        s->orders_path[0] != '\0') {
        char menu[KOS_RECIPE_MAX_DISHES][KOS_RECIPE_NAME_MAX];
        int menu_count = 0;

        kos_error_clear(&err);
        rc = kos_recipe_menu_load(s->orders_path, menu, KOS_RECIPE_MAX_DISHES, &menu_count,
                                  &err);
        if (rc == KOS_OK) {
            int wanted = s->rt.opts.chefs + 3;
            int i;

            if (wanted < 5) {
                wanted = 5;
            }
            for (i = 0; i < wanted; i++) {
                char id[KOS_SHM_NAME_MAX];
                int64_t arrival = s->started_ms + (int64_t)i * SCHED_ORDER_GAP_MS;
                int dish_index = kos_recipes_dish_index(&s->recipes, menu[i % menu_count]);
                int64_t est = kos_recipe_est_cost_ms(&s->recipes, dish_index);
                int64_t deadline = arrival + 20000 + est * 2 + (i % 2) * 8000;

                snprintf(id, sizeof id, "cfg-%02d", i);
                (void)add_order(s, id, menu[i % menu_count], arrival, deadline,
                                dish_index >= 0 ? s->recipes.dishes[dish_index].score : 0,
                                0, s->started_ms);
            }
            kos_log_info(s->rt.log, "menu_loaded", "path=%s dishes=%d config_orders=%d",
                         s->orders_path, menu_count, wanted);
        } else {
            char text[256];

            error_text(&err, text, sizeof text);
            kos_log_warn(s->rt.log, "menu_load_failed", "path=%s error=%s",
                         s->orders_path, text);
        }
    } else {
        kos_log_warn(s->rt.log, "menu_absent",
                     "orders file not found; orders will only come from the shm world state");
    }
    return 0;
}

static int scheduler_main(scheduler *s, const kos_child_opts *opts)
{
    kos_error err;
    int i;

    memset(s, 0, sizeof *s);
    s->rt.opts = *opts;
    if (opts->runs_dir[0] != '\0') {
        copy_str(s->runs_dir, sizeof s->runs_dir, opts->runs_dir);
    }
    s->events_fd = -1;
    s->decisions_fd = -1;
    s->events = (mqd_t)-1;
    for (i = 0; i < KOS_SHM_MAX_CHEFS; i++) {
        s->tasks[i] = (mqd_t)-1;
    }

    if (kos_child_setup(&s->rt, &s->rt.opts) != 0) {
        return 1;
    }
    s->started_ms = now_ms();
    s->next_task_id = 0;

    {
        kos_policy_kind kind;

        if (kos_policy_kind_from_name(s->rt.opts.strategy, &kind) != KOS_OK) {
            kos_log_error(s->rt.log, "strategy_invalid", "value=%s", s->rt.opts.strategy);
            kos_child_teardown(&s->rt);
            return 1;
        }
        kos_error_clear(&err);
        s->policy = kos_policy_create(kind, &err);
        if (s->policy == NULL) {
            char text[256];

            error_text(&err, text, sizeof text);
            kos_log_error(s->rt.log, "policy_create_failed", "error=%s", text);
            kos_child_teardown(&s->rt);
            return 1;
        }
    }

    if (load_config(s) != 0) {
        kos_policy_destroy(s->policy);
        kos_child_teardown(&s->rt);
        return 1;
    }
    kos_error_clear(&err);
    if (kos_resources_attach(&s->kitchen, &s->resources, &err) != KOS_OK) {
        char text[256];

        error_text(&err, text, sizeof text);
        kos_log_error(s->rt.log, "resource_attach_failed", "error=%s", text);
        kos_policy_destroy(s->policy);
        kos_child_teardown(&s->rt);
        return 1;
    }
    resource_stats_build(s);
    if (prepare_run_dir(s) != 0) {
        kos_resources_close(&s->resources);
        kos_policy_destroy(s->policy);
        kos_child_teardown(&s->rt);
        return 1;
    }
    if (open_queues(s) != 0) {
        close_queues(s);
        kos_policy_destroy(s->policy);
        kos_child_teardown(&s->rt);
        return 1;
    }

    kos_log_info(s->rt.log, "scheduler_ready",
                 "policy=%s chefs=%d orders=%d run_dir=%s", kos_policy_name(s->policy),
                 s->rt.opts.chefs, s->order_count, s->run_dir);

    while (!kos_child_poll(&s->rt)) {
        int64_t now = now_ms();

        if (kos_child_take_sighup()) {
            reload_config_on_sighup(s);
            strategy_switch(s);
        }
        if (kos_child_take_sigusr1()) {
            write_metrics_snapshot(s, "sigusr1");
        }
        poll_world_orders(s, now);
        drain_events(s);
        expire_leases(s, now);
        detect_deadlock(s);

        for (i = 0; i < s->order_count; i++) {
            int j;

            if (s->orders[i].status != KOS_ORDER_ACTIVE) {
                continue;
            }
            for (j = 0; j < s->orders[i].task_count; j++) {
                sched_task *task = &s->orders[i].tasks[j];

                if (task->state == TASK_RUNNING &&
                    now - task->started_ms >
                        s->recipes.nodes[task->node_index].duration_ms +
                            SCHED_TASK_TIMEOUT_MS) {
                    kos_log_warn(s->rt.log, "task_timeout", "order=%s node=%s task=%llu",
                                 s->orders[i].id,
                                 s->recipes.nodes[task->node_index].id,
                                 (unsigned long long)task->task_id);
                    if (task->chef_id >= 0 && task->chef_id < KOS_SHM_MAX_CHEFS &&
                        s->running_by_chef[task->chef_id] == task->task_id) {
                        s->running_by_chef[task->chef_id] = 0;
                    }
                    (void)release_lease_for_task(s, task->task_id,
                                                 "resource_reclaimed", "task-timeout");
                    task->state = TASK_FAILED;
                    mark_order_finished(&s->orders[i], KOS_ORDER_FAILED, now);
                }
            }
        }
        dispatch_ready(s, now);
        kos_child_sleep(&s->rt, s->rt.opts.tick_ms);
    }

    while (s->leases.count > 0) {
        kos_lease *lease = &s->leases.leases[0];

        (void)release_lease_for_task(s, lease->task_id, "resource_reclaimed",
                                     "scheduler-shutdown");
    }
    write_metrics_snapshot(s, "shutdown");
    close_queues(s);
    {
        int completed = 0;

        for (i = 0; i < s->order_count; i++) {
            if (s->orders[i].status == KOS_ORDER_COMPLETED) {
                completed++;
            }
        }
        kos_log_info(s->rt.log, "scheduler_stop", "policy=%s orders=%d completed=%d",
                     kos_policy_name(s->policy), s->order_count, completed);
    }
    if (s->events_fd >= 0) {
        close(s->events_fd);
        s->events_fd = -1;
    }
    if (s->decisions_fd >= 0) {
        close(s->decisions_fd);
        s->decisions_fd = -1;
    }
    kos_resources_close(&s->resources);
    kos_policy_destroy(s->policy);
    s->policy = NULL;
    kos_child_teardown(&s->rt);
    return 0;
}

int main(int argc, char **argv)
{
    kos_child_opts opts;
    int parsed;
    scheduler *s;

    kos_child_opts_init(&opts, KOS_CHILD_SCHEDULER);
    parsed = kos_child_parse_args(&opts, argc, argv);
    if (parsed == KOS_CHILD_ARG_HELP) {
        kos_child_print_usage(stdout, KOS_CHILD_SCHEDULER);
        return 0;
    }
    if (parsed == KOS_CHILD_ARG_VERSION) {
        printf("kitchenos-scheduler 0.3.0\n");
        return 0;
    }
    if (parsed != KOS_CHILD_ARG_OK) {
        kos_child_print_usage(stderr, KOS_CHILD_SCHEDULER);
        return 2;
    }
    s = calloc(1, sizeof *s);
    if (s == NULL) {
        return 1;
    }
    {
        int rc = scheduler_main(s, &opts);

        free(s);
        return rc;
    }
}
