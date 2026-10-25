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

#include <kitchenos/assign.h>
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
#include <kitchenos/stand.h>

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
/* 距离场够不到（另一个连通区/厨师格未知）时给的路程惩罚，单位是格。 */
#define SCHED_UNREACHABLE_CELLS 600
#define SCHED_WASH_PRIORITY 32

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
    int opened;    /* 策略已经选中并开工；未开工订单的工序不参与派工 */
    int open_seq;  /* 开工顺序，交付必须按它来 */
    int64_t finished_ms;
    int64_t lateness_ms;
    int task_count;
    sched_task tasks[SCHED_MAX_TASKS];
} sched_order;

/*
 * wash 是 Scheduler 自己生成的维护工序：它不属于任何订单的菜谱 DAG，但和订单工序
 * 走同一个候选列表、同一个匈牙利配对与同一个资源租约，不另起队列。同一时刻最多
 * 一道：干净盘 > 1 或没有脏盘时不会生成。
 */
typedef struct {
    int state; /* 0 = 没有，1 = 待派，2 = 已派 */
    int chef_id;
    uint64_t task_id;
    int64_t ready_ms;
    int64_t started_ms;
} sched_wash;

typedef struct {
    int order_index; /* -1 = wash */
    int task_index;
    int node_index;
    int station_index;
} sched_candidate;

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
    int64_t dispatch_llf;
    int64_t dispatch_hrrn;
    int64_t worker_restarts;
    int blocked_hold[KOS_SHM_MAX_CHEFS]; /* 报过 BLOCKED 的厨师先不接新活 */
    char metrics_path[KOS_DAEMON_PATH_MAX + 48];
    uint64_t next_task_id;
    int64_t started_ms;
    int64_t next_world_poll_ms;
    uint64_t running_by_chef[KOS_SHM_MAX_CHEFS];

    kos_assign_config assign_cfg;
    unsigned char pot_chain[KOS_RECIPE_MAX_NODES];
    kos_world_state world;
    int world_valid;
    int64_t world_read_ms;
    int64_t next_plate_poll_ms;
    sched_wash wash;
    int open_seq;

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

/*
 * 每条决策都带 w_d/w_t/w_c/w_u/w_z 五个分量，非派工决策全写 0：分析脚本读同一组
 * 键，不用按 kind 分支。components 为 NULL 时写零。
 */
static void decision_common(json_writer *w, scheduler *s, const char *kind,
                            const char *reason, const kos_assign_components *components)
{
    kos_assign_components zero;

    memset(&zero, 0, sizeof zero);
    if (components == NULL) {
        components = &zero;
    }
    json_writer_object_begin(w);
    json_writer_key(w, "t_ms");
    json_writer_int(w, now_ms() - s->started_ms);
    json_writer_key(w, "kind");
    json_writer_string(w, kind);
    json_writer_key(w, "policy");
    json_writer_string(w, kos_policy_name(s->policy));
    json_writer_key(w, "reason");
    json_writer_string(w, reason);
    json_writer_key(w, "w_d");
    json_writer_double(w, components->w_d);
    json_writer_key(w, "w_t");
    json_writer_double(w, components->w_t);
    json_writer_key(w, "w_c");
    json_writer_double(w, components->w_c);
    json_writer_key(w, "w_u");
    json_writer_double(w, components->w_u);
    json_writer_key(w, "w_z");
    json_writer_double(w, components->w_z);
    json_writer_key(w, "cost");
    json_writer_double(w, components->cost);
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

/* 洗盘任务没有订单可回队；租约被回收或 Worker 崩溃时直接丢掉，下一轮按盘子数量重生成。 */
static void deactivate_wash(scheduler *s, const char *reason)
{
    if (s->wash.state == 0) {
        return;
    }
    kos_log_warn(s->rt.log, "wash_dropped", "reason=%s task=%llu chef=%d", reason,
                 (unsigned long long)s->wash.task_id, s->wash.chef_id);
    memset(&s->wash, 0, sizeof s->wash);
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
        if (task_id != 0 && task_id == s->wash.task_id) {
            deactivate_wash(s, reason);
        } else {
            task_requeue(s, order_index, task_index, reason);
        }
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
            if (task_id != 0 && task_id == s->wash.task_id) {
                deactivate_wash(s, "lease-expired");
            } else {
                task_requeue(s, order_index, task_index, "lease-expired");
            }
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
            if (task_id != 0 && task_id == s->wash.task_id) {
                deactivate_wash(s, "deadlock-victim");
            } else {
                task_requeue(s, order_index, task_index, "deadlock-victim");
            }
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

static int order_has_ready(scheduler *s, int order_index)
{
    const sched_order *order = &s->orders[order_index];
    int i;

    for (i = 0; i < order->task_count; i++) {
        int node = order->tasks[i].node_index;

        if (order->tasks[i].state == TASK_PENDING &&
            order_node_ready(s, order, node)) {
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

/*
 * 一次派工要看厨师坐标和盘子计数，主循环里的订单轮询也要看世界状态；都走这一份
 * 缓存，同一个 SCHED_WORLD_POLL_MS 窗口里只拷一次 16 KB 段。读失败时沿用上一份
 * 快照（游戏暂停/C 侧刚启动时也还能按旧位置估路程）。
 */
static const kos_world_state *scheduler_world(scheduler *s, int64_t now)
{
    kos_world_state next;

    if (s->rt.shm == NULL) {
        return NULL;
    }
    if (s->world_valid && now - s->world_read_ms < SCHED_WORLD_POLL_MS) {
        return &s->world;
    }
    if (kos_shm_world_read(s->rt.shm, &next, KOS_SHM_READ_ATTEMPTS, NULL) != KOS_OK) {
        return s->world_valid ? &s->world : NULL;
    }
    s->world = next;
    s->world_valid = 1;
    s->world_read_ms = now;
    return &s->world;
}

static void poll_world_orders(scheduler *s, int64_t now)
{
    const kos_world_state *world;
    int i;

    if (s->rt.shm == NULL || now < s->next_world_poll_ms) {
        return;
    }
    s->next_world_poll_ms = now + SCHED_WORLD_POLL_MS;
    world = scheduler_world(s, now);
    if (world == NULL) {
        return;
    }
    for (i = 0; i < (int)world->order_count; i++) {
        const kos_order *order = &world->orders[i];
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

static void handle_wash_event(scheduler *s, const kos_mq_msg *msg)
{
    const char *event_name = msg->event == KOS_EVT_DONE ? "wash_done" : "wash_failed";
    json_writer w;
    char line[SCHED_LINE_MAX];

    if (s->wash.chef_id >= 0 && s->wash.chef_id < KOS_SHM_MAX_CHEFS &&
        s->running_by_chef[s->wash.chef_id] == msg->task_id) {
        s->running_by_chef[s->wash.chef_id] = 0;
    }
    if (msg->event != KOS_EVT_ACCEPTED) {
        (void)release_lease_for_task(s, msg->task_id, "resource_reclaimed",
                                     msg->event == KOS_EVT_DONE ? "wash-done"
                                                                : "wash-failed");
    }
    kos_log_info(s->rt.log, event_name, "task=%llu chef=%d detail=%s",
                 (unsigned long long)msg->task_id, msg->chef_id, msg->detail);
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, event_name);
    json_writer_key(&w, "task_id");
    json_writer_int(&w, (long long)msg->task_id);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, msg->chef_id);
    if (msg->detail[0] != '\0') {
        json_writer_key(&w, "detail");
        json_writer_string(&w, msg->detail);
    }
    event_end(s, &w);
    if (msg->event != KOS_EVT_ACCEPTED) {
        memset(&s->wash, 0, sizeof s->wash);
    }
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
    if (s->wash.state == 2 && msg->task_id != 0 && msg->task_id == s->wash.task_id) {
        handle_wash_event(s, msg);
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

static unsigned task_priority(const sched_order *order, int64_t now)
{
    int64_t slack = order->deadline_ms - now;
    int seconds = slack > 0 ? (int)(slack / 1000) : 0;

    if (seconds > 31) {
        seconds = 31;
    }
    return (unsigned)(32 - seconds);
}

static int candidate_duration_ms(const scheduler *s, const sched_candidate *cand)
{
    if (cand->order_index < 0) {
        return (int)(s->assign_cfg.wash_ms + 0.5);
    }
    return s->recipes.nodes[cand->node_index].duration_ms;
}

static const char *candidate_resource(const scheduler *s, const sched_candidate *cand)
{
    /* wash 的目标是水槽工位；资源 kind 取工位自己的 kind，地图把水槽叫 wash_sink
     * 时租约表与 metrics 也跟着用那个名字。 */
    return cand->order_index < 0
               ? s->kitchen.stations[cand->station_index].kind
               : s->recipes.nodes[cand->node_index].resource;
}

/* 水槽工位：kind 与 "sink" 互相包含即可，导出的地图目前都正好叫 sink。 */
static int sink_station_index(const kos_kitchen *kitchen)
{
    int i;

    for (i = 0; i < kitchen->station_count; i++) {
        if (kos_kitchen_name_matches("sink", kitchen->stations[i].kind)) {
            return i;
        }
    }
    return -1;
}

static int64_t order_remaining_critical_ms(const scheduler *s, const sched_order *order)
{
    int64_t best = 0;
    int i;

    for (i = 0; i < order->task_count; i++) {
        int64_t critical;
        int state = order->tasks[i].state;

        if (state == TASK_DONE || state == TASK_FAILED) {
            continue;
        }
        critical = kos_recipe_critical_ms(&s->recipes, order->dish_index,
                                          order->tasks[i].node_index);
        if (critical > best) {
            best = critical;
        }
    }
    return best;
}

/*
 * 小费倍率要求按开工顺序交付：开工更早的订单还没做完时，后面订单的 deliver 不进
 * 就绪队列。失败/完成的更早订单不再挡路。
 */
static int order_delivery_allowed(const scheduler *s, const sched_order *order)
{
    int i;

    if (order->open_seq <= 0) {
        return 1;
    }
    for (i = 0; i < s->order_count; i++) {
        const sched_order *earlier = &s->orders[i];

        if (earlier == order || earlier->status != KOS_ORDER_ACTIVE || !earlier->opened ||
            earlier->open_seq <= 0) {
            continue;
        }
        if (earlier->open_seq < order->open_seq && !order_has_done_all(earlier)) {
            return 0;
        }
    }
    return 1;
}

static int task_station_index(const scheduler *s, int order_index, int task_index,
                              int *station_out)
{
    const sched_order *order = &s->orders[order_index];
    const kos_recipe_node *node =
        &s->recipes.nodes[order->tasks[task_index].node_index];
    const char *hint = node->ingredient[0] != '\0' ? node->ingredient : node->label;

    return kos_kitchen_pick_station(&s->kitchen, node->resource, hint,
                                    (unsigned)(order_index * KOS_RECIPE_MAX_DISH_NODES +
                                               task_index),
                                    station_out);
}

/*
 * 至少有一个空闲厨师能从自己的格子走到该工位。厨师格未知（还没有世界状态）时按
 * 可达处理：那时所有路程都是 0，交给后面的派工流程。互不相通的厨房条带之间
 * 距离场返回 UINT16_MAX，这种工序不该占住厨师或资源令牌。
 */
static int station_reachable(const scheduler *s, int station, const int *chef_cells,
                             int chef_count)
{
    int i;

    for (i = 0; i < chef_count; i++) {
        if (chef_cells[i] < 0 ||
            kos_stand_distance(&s->kitchen, station, chef_cells[i]) !=
                KOS_STAND_UNREACHABLE) {
            return 1;
        }
    }
    return 0;
}

static int task_dispatch_ready(const scheduler *s, const sched_order *order, int task_index)
{
    const sched_task *task = &order->tasks[task_index];
    const kos_recipe_node *node;

    if (task->state != TASK_PENDING ||
        !order_node_ready(s, order, task->node_index)) {
        return 0;
    }
    node = &s->recipes.nodes[task->node_index];
    if (strcmp(node->op, "deliver") == 0 && !order_delivery_allowed(s, order)) {
        return 0;
    }
    return 1;
}

/* 二分图匹配（Kuhn）：把一个任务分给一个空闲厨师；chef 侧最多 8 个。 */
static int match_task_to_chef(int task, const int *mask, int *chef_task,
                              unsigned char *seen, int chef_count)
{
    int chef;

    for (chef = 0; chef < chef_count; chef++) {
        if ((mask[task] & (1 << chef)) == 0 || seen[chef]) {
            continue;
        }
        seen[chef] = 1;
        if (chef_task[chef] < 0 ||
            match_task_to_chef(chef_task[chef], mask, chef_task, seen, chef_count)) {
            chef_task[chef] = task;
            return 1;
        }
    }
    return 0;
}

/*
 * 已开工订单里"现在真的能派给不同厨师"的工序数：依赖就绪、至少一个空闲厨师走得到、
 * 资源 kind 本轮还有令牌，然后做一次二分图最大匹配。只数"任意厨师可达"会把两个只有
 * 同一个厨师够得着的工序当成两份工作：匈牙利之后另一个厨师仍然空转，也不会开下一单
 * （_2 的两条互不相通条带就会这样）。匹配数小于空闲厨师数时才该按策略开下一单。
 */
static int opened_ready_count(scheduler *s, int64_t now, const int *chef_cells,
                              int chef_count)
{
    char kind_names[KOS_ASSIGN_MAX_TASKS][KOS_RES_KIND_MAX];
    int kind_free[KOS_ASSIGN_MAX_TASKS];
    int kind_n = 0;
    int masks[KOS_ASSIGN_MAX_TASKS];
    int task_count = 0;
    int chef_task[KOS_ASSIGN_MAX_CHEFS];
    int matched = 0;
    int i;
    int j;
    int k;

    for (i = 0; i < KOS_ASSIGN_MAX_CHEFS; i++) {
        chef_task[i] = -1;
    }
    for (i = 0; i < s->order_count; i++) {
        const sched_order *order = &s->orders[i];

        if (order->status != KOS_ORDER_ACTIVE || !order->opened ||
            now < order->arrival_ms) {
            continue;
        }
        for (j = 0; j < order->task_count; j++) {
            const char *kind;
            int station;
            int value = 0;
            int mask = 0;

            if (task_count >= KOS_ASSIGN_MAX_TASKS) {
                goto match;
            }
            if (!task_dispatch_ready(s, order, j)) {
                continue;
            }
            if (task_station_index(s, i, j, &station) != KOS_OK) {
                continue;
            }
            for (k = 0; k < KOS_ASSIGN_MAX_CHEFS && k < chef_count; k++) {
                if (chef_cells[k] < 0 ||
                    kos_stand_distance(&s->kitchen, station, chef_cells[k]) !=
                        KOS_STAND_UNREACHABLE) {
                    mask |= 1 << k;
                }
            }
            if (mask == 0) {
                continue;
            }
            kind = s->recipes.nodes[order->tasks[j].node_index].resource;
            for (k = 0; k < kind_n; k++) {
                if (strcmp(kind_names[k], kind) == 0) {
                    break;
                }
            }
            if (k == kind_n) {
                copy_str(kind_names[k], sizeof kind_names[k], kind);
                if (kos_resource_value(&s->resources, kind, &value) != KOS_OK) {
                    value = 0;
                }
                kind_free[k] = value;
                kind_n++;
            }
            if (kind_free[k] <= 0) {
                continue;
            }
            kind_free[k]--;
            masks[task_count++] = mask;
        }
    }
match:
    for (i = 0; i < task_count; i++) {
        unsigned char seen[KOS_ASSIGN_MAX_CHEFS];

        memset(seen, 0, sizeof seen);
        if (match_task_to_chef(i, masks, chef_task, seen, chef_count)) {
            matched++;
        }
    }
    return matched;
}

static int open_next_order(scheduler *s, int64_t now, const int *chef_cells,
                          int chef_count)
{
    kos_order_view views[SCHED_MAX_ORDERS];
    int map[SCHED_MAX_ORDERS];
    int view_count = 0;
    int chosen = -1;
    int i;
    int j;

    for (i = 0; i < s->order_count; i++) {
        sched_order *candidate = &s->orders[i];
        int has_feasible = 0;

        if (candidate->status != KOS_ORDER_ACTIVE || candidate->opened ||
            now < candidate->arrival_ms || !order_has_ready(s, i)) {
            continue;
        }
        for (j = 0; j < candidate->task_count; j++) {
            int station;

            if (!task_dispatch_ready(s, candidate, j)) {
                continue;
            }
            /* 找不到工位也算"能开"：订单会在 build_candidates() 里走
             * fail_task_no_station 并标记 FAILED，而不是永远停在未开工状态。
             * 只有"有工位但当前空闲厨师都到不了"才留到厨师换位置后再试。 */
            if (task_station_index(s, i, j, &station) != KOS_OK ||
                station_reachable(s, station, chef_cells, chef_count)) {
                has_feasible = 1;
                break;
            }
        }
        if (!has_feasible) {
            continue;
        }
        memset(&views[view_count], 0, sizeof views[view_count]);
        copy_str(views[view_count].id, sizeof views[view_count].id, candidate->id);
        copy_str(views[view_count].dish, sizeof views[view_count].dish, candidate->dish);
        views[view_count].arrival_ms = candidate->arrival_ms;
        views[view_count].deadline_ms = candidate->deadline_ms;
        views[view_count].est_cost_ms = order_remaining_critical_ms(s, candidate);
        views[view_count].reward = candidate->reward;
        views[view_count].status = candidate->status;
        map[view_count] = i;
        view_count++;
    }
    if (view_count == 0 ||
        kos_policy_pick(s->policy, views, view_count, NULL, 0, now, &chosen) != KOS_OK) {
        return KOS_ERR_EMPTY;
    }
    {
        sched_order *order = &s->orders[map[chosen]];
        json_writer w;
        char line[SCHED_LINE_MAX];
        int64_t critical = order_remaining_critical_ms(s, order);

        order->opened = 1;
        order->open_seq = ++s->open_seq;
        kos_log_info(s->rt.log, "order_opened",
                     "order=%s dish=%s open_seq=%d policy=%s crit_ms=%lld slack_ms=%lld",
                     order->id, order->dish, order->open_seq, kos_policy_name(s->policy),
                     (long long)critical, (long long)(order->deadline_ms - now));
        json_writer_init(&w, line, sizeof line);
        event_common(&w, s, "order_opened");
        json_writer_key(&w, "order_id");
        json_writer_string(&w, order->id);
        json_writer_key(&w, "dish");
        json_writer_string(&w, order->dish);
        json_writer_key(&w, "open_seq");
        json_writer_int(&w, order->open_seq);
        event_end(s, &w);

        json_writer_init(&w, line, sizeof line);
        decision_common(&w, s, "order_open", "policy", NULL);
        json_writer_key(&w, "order_id");
        json_writer_string(&w, order->id);
        json_writer_key(&w, "dish");
        json_writer_string(&w, order->dish);
        json_writer_key(&w, "open_seq");
        json_writer_int(&w, order->open_seq);
        json_writer_key(&w, "critical_ms");
        json_writer_int(&w, (long long)critical);
        json_writer_key(&w, "slack_ms");
        json_writer_int(&w, (long long)(order->deadline_ms - now));
        json_writer_key(&w, "ready_orders");
        json_writer_int(&w, view_count);
        json_writer_object_end(&w);
        write_jsonl(s->decisions_fd, &w);
    }
    return KOS_OK;
}

static void fail_task_no_station(scheduler *s, sched_order *order, int task_index,
                                 int64_t now)
{
    const kos_recipe_node *node = &s->recipes.nodes[order->tasks[task_index].node_index];
    json_writer w;
    char line[SCHED_LINE_MAX];

    kos_log_warn(s->rt.log, "task_no_station", "node=%s resource=%s", node->id,
                 node->resource);
    order->tasks[task_index].state = TASK_FAILED;
    mark_order_finished(order, KOS_ORDER_FAILED, now);
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, "task_failed");
    json_writer_key(&w, "order_id");
    json_writer_string(&w, order->id);
    json_writer_key(&w, "node");
    json_writer_string(&w, node->id);
    json_writer_key(&w, "detail");
    json_writer_string(&w, "no station for resource");
    event_end(s, &w);
}

/*
 * 候选 = 已开工订单的就绪工序 + 至多一道 wash。未开工订单的工序不在这里；由
 * open_next_order() 按策略一单一单开。资源令牌在候选阶段就按 kind 预扣（同一轮
 * 里不会把两个厨师派给只剩一个令牌的资源），真正派发时再原子获取一次。
 */
static int build_candidates(scheduler *s, int64_t now, const int *chef_cells,
                            int chef_count, sched_candidate *out, int cap)
{
    int n = 0;
    int i;
    int j;

    for (i = 0; i < s->order_count; i++) {
        sched_order *order = &s->orders[i];

        if (order->status != KOS_ORDER_ACTIVE || !order->opened ||
            now < order->arrival_ms) {
            continue;
        }
        for (j = 0; j < order->task_count; j++) {
            sched_task *task = &order->tasks[j];
            int station;

            if (!task_dispatch_ready(s, order, j)) {
                continue;
            }
            if (task->ready_since_ms == 0) {
                task->ready_since_ms = now;
            }
            if (n >= cap) {
                return n;
            }
            if (task_station_index(s, i, j, &station) != KOS_OK) {
                fail_task_no_station(s, order, j, now);
                break;
            }
            if (!station_reachable(s, station, chef_cells, chef_count)) {
                continue;
            }
            out[n].order_index = i;
            out[n].task_index = j;
            out[n].node_index = task->node_index;
            out[n].station_index = station;
            n++;
        }
    }
    if (s->wash.state == 1 && n < cap) {
        int station = sink_station_index(&s->kitchen);

        if (station >= 0 && station_reachable(s, station, chef_cells, chef_count)) {
            out[n].order_index = -1;
            out[n].task_index = -1;
            out[n].node_index = -1;
            out[n].station_index = station;
            n++;
        }
    }
    return n;
}

/*
 * 策略给的订单紧迫度：把已开工的活跃订单按当前策略从最紧急排到最不紧急，映射到
 * 0..1（1 最紧急）。只有一单时所有工序同为 1.0，w_u 不区分配对；这是有意的——
 * 订单级策略只决定开工顺序。
 */
static void order_urgency_ranks(scheduler *s, int64_t now,
                                double urgency[SCHED_MAX_ORDERS])
{
    kos_order_view pending[SCHED_MAX_ORDERS];
    int map[SCHED_MAX_ORDERS];
    int count = 0;
    int rank;
    int i;

    memset(urgency, 0, sizeof(double) * SCHED_MAX_ORDERS);
    for (i = 0; i < s->order_count; i++) {
        sched_order *order = &s->orders[i];

        if (order->status != KOS_ORDER_ACTIVE || !order->opened ||
            now < order->arrival_ms) {
            continue;
        }
        memset(&pending[count], 0, sizeof pending[count]);
        copy_str(pending[count].id, sizeof pending[count].id, order->id);
        copy_str(pending[count].dish, sizeof pending[count].dish, order->dish);
        pending[count].arrival_ms = order->arrival_ms;
        pending[count].deadline_ms = order->deadline_ms;
        pending[count].est_cost_ms = order_remaining_critical_ms(s, order);
        pending[count].reward = order->reward;
        pending[count].status = order->status;
        map[count] = i;
        count++;
    }
    for (rank = 0; rank < count; rank++) {
        kos_order_view views[SCHED_MAX_ORDERS];
        int vmap[SCHED_MAX_ORDERS];
        int chosen = -1;
        int left = count - rank;

        for (i = 0; i < left; i++) {
            views[i] = pending[rank + i];
            vmap[i] = map[rank + i];
        }
        if (kos_policy_pick(s->policy, views, left, NULL, 0, now, &chosen) != KOS_OK ||
            chosen < 0 || chosen >= left) {
            break;
        }
        urgency[vmap[chosen]] =
            count > 1 ? 1.0 - (double)rank / (double)(count - 1) : 1.0;
        if (chosen != 0) {
            kos_order_view swap_view = pending[rank];
            int swap_map = map[rank];

            pending[rank] = pending[rank + chosen];
            pending[rank + chosen] = swap_view;
            map[rank] = map[rank + chosen];
            map[rank + chosen] = swap_map;
        }
    }
}

static int plate_count(const kos_world_state *world, const char *name)
{
    int sum = 0;
    uint32_t i;

    for (i = 0; i < world->resource_count && i < KOS_SHM_MAX_RESOURCES; i++) {
        if (strcmp(world->resources[i].name, name) == 0 &&
            world->resources[i].count > 0) {
            sum += world->resources[i].count;
        }
    }
    return sum;
}

/*
 * 盘子是有界缓冲：干净盘 ≤ 1 且有脏盘时生成一道 wash。生成条件里的两个计数都从
 * 世界状态读（plate_stack / dirty_plates 的 count）；读不到脏盘字段时不会生成，
 * 现有地图照常跑。wash 走同一个候选列表，不另起队列。
 */
static void maybe_generate_wash(scheduler *s, int64_t now)
{
    const kos_world_state *world;
    int clean;
    int dirty;

    if (now < s->next_plate_poll_ms) {
        return;
    }
    s->next_plate_poll_ms = now + SCHED_WORLD_POLL_MS;
    world = scheduler_world(s, now);
    if (world == NULL) {
        return;
    }
    clean = plate_count(world, "plate_stack");
    dirty = plate_count(world, "dirty_plates");
    if (s->wash.state == 1 && !kos_assign_wash_needed(clean, dirty)) {
        kos_log_info(s->rt.log, "wash_cancelled", "clean=%d dirty=%d", clean, dirty);
        memset(&s->wash, 0, sizeof s->wash);
    }
    if (s->wash.state != 0 || !kos_assign_wash_needed(clean, dirty)) {
        return;
    }
    if (sink_station_index(&s->kitchen) < 0) {
        return;
    }
    s->wash.state = 1;
    s->wash.chef_id = -1;
    s->wash.task_id = 0;
    s->wash.ready_ms = now;
    kos_log_info(s->rt.log, "wash_generated", "clean=%d dirty=%d", clean, dirty);
    {
        json_writer w;
        char line[SCHED_LINE_MAX];

        json_writer_init(&w, line, sizeof line);
        event_common(&w, s, "wash_generated");
        json_writer_key(&w, "clean_plates");
        json_writer_int(&w, clean);
        json_writer_key(&w, "dirty_plates");
        json_writer_int(&w, dirty);
        event_end(s, &w);
    }
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
    sched_candidate all[KOS_ASSIGN_MAX_TASKS];
    sched_candidate pool[KOS_ASSIGN_MAX_TASKS];
    /* 紧凑排：kos_assign_solve 以实际 pool_count 为行距读 chef-major 矩阵。 */
    kos_assign_terms terms[KOS_ASSIGN_MAX_CHEFS * KOS_ASSIGN_MAX_TASKS];
    int travel_cells[KOS_ASSIGN_MAX_CHEFS * KOS_ASSIGN_MAX_TASKS];
    int idle[KOS_ASSIGN_MAX_CHEFS];
    int chef_cells[KOS_ASSIGN_MAX_CHEFS];
    int task_of_chef[KOS_ASSIGN_MAX_CHEFS];
    double urgency[SCHED_MAX_ORDERS];
    const kos_world_state *world;
    char kind_names[KOS_ASSIGN_MAX_TASKS][KOS_RES_KIND_MAX];
    int kind_free[KOS_ASSIGN_MAX_TASKS];
    int kind_n = 0;
    int idle_count = 0;
    int cand_count;
    int pool_count = 0;
    int i;
    int j;

    if (s->rt.opts.chefs <= 0) {
        return;
    }
    for (i = 0; i < s->rt.opts.chefs && i < KOS_ASSIGN_MAX_CHEFS; i++) {
        if (s->blocked_hold[i] && !has_avoid_task(s, i)) {
            /* 被重排的工序已经完成/失败，扣留自然解除，避免这个厨师永远没活。 */
            s->blocked_hold[i] = 0;
        }
        if (s->running_by_chef[i] == 0 && !s->blocked_hold[i]) {
            idle[idle_count++] = i;
        }
    }
    if (idle_count == 0) {
        return;
    }
    world = scheduler_world(s, now);
    for (i = 0; i < idle_count; i++) {
        int chef = idle[i];

        chef_cells[i] = -1;
        if (world != NULL && chef < (int)world->chef_count &&
            chef < KOS_SHM_MAX_CHEFS) {
            chef_cells[i] = kos_kitchen_nearest_cell(&s->kitchen,
                                                     world->chefs[chef].x,
                                                     world->chefs[chef].y);
        }
    }
    /* 已开工订单没有足够多的可派工序填满空闲厨师时，才按策略开下一单。 */
    for (;;) {
        int ready = opened_ready_count(s, now, chef_cells, idle_count);

        if (ready >= idle_count ||
            open_next_order(s, now, chef_cells, idle_count) != KOS_OK) {
            break;
        }
    }
    cand_count = build_candidates(s, now, chef_cells, idle_count, all,
                                  KOS_ASSIGN_MAX_TASKS);
    if (cand_count == 0) {
        return;
    }
    /* 按 kind 预扣本轮可用令牌，避免把两个厨师派给同一个只剩一个令牌的资源。 */
    for (i = 0; i < cand_count; i++) {
        const char *kind = candidate_resource(s, &all[i]);
        int k = -1;
        int value = 0;

        for (j = 0; j < kind_n; j++) {
            if (strcmp(kind_names[j], kind) == 0) {
                k = j;
                break;
            }
        }
        if (k < 0) {
            k = kind_n++;
            copy_str(kind_names[k], sizeof kind_names[k], kind);
            if (kos_resource_value(&s->resources, kind, &value) != KOS_OK) {
                value = 0;
            }
            kind_free[k] = value;
        }
        if (kind_free[k] > 0) {
            kind_free[k]--;
            pool[pool_count++] = all[i];
        }
    }
    if (pool_count == 0) {
        /* 所有就绪工序都在等资源：至少给第一个等待者记一次 wait，metrics 才有数。 */
        for (i = 0; i < cand_count; i++) {
            if (all[i].order_index >= 0) {
                sched_order *order = &s->orders[all[i].order_index];
                sched_task *task = &order->tasks[all[i].task_index];

                mark_resource_wait(s, idle[0], order, task,
                                   &s->recipes.nodes[task->node_index]);
                break;
            }
        }
        return;
    }

    order_urgency_ranks(s, now, urgency);
    for (i = 0; i < idle_count; i++) {
        int cell = chef_cells[i];

        for (j = 0; j < pool_count; j++) {
            int index = i * pool_count + j;
            kos_assign_terms *term = &terms[index];
            int station = pool[j].station_index;

            memset(term, 0, sizeof *term);
            travel_cells[index] = 0;
            if (pool[j].order_index >= 0) {
                sched_task *pooled =
                    &s->orders[pool[j].order_index].tasks[pool[j].task_index];

                if (pooled->avoid_chef == idle[i]) {
                    /* 这个厨师上一轮走到一半卡住；先派给别人，别原地重试。 */
                    term->unreachable = 1;
                    travel_cells[index] = SCHED_UNREACHABLE_CELLS;
                }
            }
            if (cell >= 0) {
                uint16_t distance = kos_stand_distance(&s->kitchen, station, cell);

                if (distance == KOS_STAND_UNREACHABLE) {
                    /* 两个互不相通的厨房区之间没有路；不派这道，宁可让厨师空着。 */
                    term->unreachable = 1;
                    travel_cells[index] = SCHED_UNREACHABLE_CELLS;
                } else {
                    travel_cells[index] = distance;
                    term->travel_s = kos_assign_travel_s(travel_cells[index],
                                                         s->assign_cfg.cell_ms);
                }
            }
            term->work_s = candidate_duration_ms(s, &pool[j]) / 1000.0;
            if (pool[j].order_index < 0) {
                term->critical_s = s->assign_cfg.wash_ms / 1000.0;
                term->urgency = 1.0;
            } else {
                term->critical_s =
                    kos_recipe_critical_ms(&s->recipes,
                                           s->orders[pool[j].order_index].dish_index,
                                           pool[j].node_index) /
                    1000.0;
                term->urgency = urgency[pool[j].order_index];
                term->critical_x2 = s->pot_chain[pool[j].node_index] ? 1 : 0;
            }
        }
    }
    {
        kos_error err;

        kos_error_clear(&err);
        if (kos_assign_solve(&s->assign_cfg.weights, idle_count, pool_count,
                             terms, task_of_chef, &err) != KOS_OK) {
            char text[256];

            error_text(&err, text, sizeof text);
            kos_log_warn(s->rt.log, "assign_failed", "error=%s", text);
            return;
        }
    }

    for (i = 0; i < idle_count; i++) {
        int chef = idle[i];
        int pick = task_of_chef[i];
        sched_candidate *cand;
        const kos_recipe_node *node = NULL;
        sched_order *order = NULL;
        sched_task *task = NULL;
        const char *kind;
        const char *order_id;
        const char *dish = "";
        const char *node_id;
        int duration;
        int critical_ms = 0;
        int64_t slack_ms = 0;
        int priority;
        const kos_station *station;
        kos_assign_components components;
        kos_mq_msg msg;
        kos_error err;
        char line[SCHED_LINE_MAX];
        json_writer w;
        int rc;

        if (pick < 0 || pick >= pool_count) {
            continue;
        }
        cand = &pool[pick];
        if (cand->order_index >= 0) {
            order = &s->orders[cand->order_index];
            task = &order->tasks[cand->task_index];
            node = &s->recipes.nodes[cand->node_index];
            kind = node->resource;
            order_id = order->id;
            dish = order->dish;
            node_id = node->id;
            duration = node->duration_ms;
            critical_ms = (int)kos_recipe_critical_ms(&s->recipes, order->dish_index,
                                                      cand->node_index);
            slack_ms = order->deadline_ms - now;
            priority = (int)task_priority(order, now);
        } else {
            order_id = "wash";
            node_id = "wash";
            duration = candidate_duration_ms(s, cand);
            priority = SCHED_WASH_PRIORITY;
        }
        station = &s->kitchen.stations[cand->station_index];
        kind = candidate_resource(s, cand);
        rc = kos_resource_try_acquire(&s->resources, kind);
        if (rc == KOS_ERR_EMPTY) {
            if (order != NULL) {
                mark_resource_wait(s, chef, order, task,
                                   &s->recipes.nodes[task->node_index]);
            }
            continue;
        }
        if (rc != KOS_OK) {
            kos_log_warn(s->rt.log, "resource_acquire_failed", "kind=%s rc=%d", kind, rc);
            continue;
        }
        s->wanted_kind[chef] = -1;

        memset(&msg, 0, sizeof msg);
        msg.version = KOS_MQ_VERSION;
        msg.kind = KOS_MQ_TASK;
        msg.task_id = ++s->next_task_id;
        msg.chef_id = chef;
        msg.priority = priority;
        msg.duration_ms = duration;
        msg.target_x = station->x;
        msg.target_y = station->z;
        copy_str(msg.order_id, sizeof msg.order_id, order_id);
        copy_str(msg.dish, sizeof msg.dish, dish);
        copy_str(msg.node_id, sizeof msg.node_id, node_id);
        copy_str(msg.op, sizeof msg.op, cand->order_index < 0 ? "wash" : node->op);
        copy_str(msg.resource, sizeof msg.resource, kind);
        copy_str(msg.station_id, sizeof msg.station_id, station->id);

        kos_error_clear(&err);
        if (kos_mq_send(s->tasks[chef], &msg, (unsigned)msg.priority, 20, &err) != KOS_OK) {
            char text[256];

            error_text(&err, text, sizeof text);
            kos_log_warn(s->rt.log, "task_send_failed", "order=%s node=%s chef=%d error=%s",
                         order_id, node_id, chef, text);
            (void)kos_resource_release(&s->resources, kind);
            continue;
        }
        {
            int64_t wait_ms = task != NULL && task->wait_started_ms > 0
                                  ? now - task->wait_started_ms
                                  : 0;
            sched_res_stat *stat = stat_find(s, kind);
            int64_t deadline = now + s->rt.opts.lease_ttl_ms;
            int was_waiting = task != NULL && task->wait_logged;

            if (kos_lease_add(&s->leases, kind, chef, msg.task_id, now, deadline,
                              order != NULL ? cand->order_index : -1,
                              order != NULL ? cand->task_index : -1) == NULL) {
                kos_log_error(s->rt.log, "lease_add_failed", "kind=%s chef=%d task=%llu",
                              kind, chef, (unsigned long long)msg.task_id);
                (void)kos_resource_release(&s->resources, kind);
                continue;
            }
            if (stat != NULL) {
                stat->acquires++;
                if (was_waiting && wait_ms > 0) {
                    stat->wait_ms += wait_ms;
                }
            }
            resource_event(s, "resource_acquired", kind, chef, msg.task_id, "dispatch",
                           wait_ms);
            if (task != NULL) {
                task->wait_logged = 0;
                task->wait_started_ms = 0;
            }
        }
        s->running_by_chef[chef] = msg.task_id;
        if (order != NULL) {
            if (task->avoid_chef >= 0 && task->avoid_chef != chef &&
                task->avoid_chef < KOS_SHM_MAX_CHEFS) {
                s->blocked_hold[task->avoid_chef] = 0;
            }
            task->avoid_chef = -1;
            task->state = TASK_RUNNING;
            task->chef_id = chef;
            task->task_id = msg.task_id;
            task->started_ms = now;
        } else {
            s->wash.state = 2;
            s->wash.chef_id = chef;
            s->wash.task_id = msg.task_id;
            s->wash.started_ms = now;
        }
        switch (kos_policy_kind_of(s->policy)) {
        case KOS_POLICY_EDF:
            s->dispatch_edf++;
            break;
        case KOS_POLICY_LLF:
            s->dispatch_llf++;
            break;
        case KOS_POLICY_HRRN:
            s->dispatch_hrrn++;
            break;
        case KOS_POLICY_FCFS:
        default:
            s->dispatch_fcfs++;
            break;
        }

        kos_assign_breakdown(&s->assign_cfg.weights,
                             &terms[i * pool_count + pick], &components);
        kos_log_info(s->rt.log, "task_dispatched",
                     "order=%s node=%s chef=%d task=%llu priority=%d crit_ms=%d "
                     "cost=%.3f travel_cells=%d policy=%s", order_id, node_id, chef,
                     (unsigned long long)msg.task_id, priority, critical_ms,
                     components.cost, travel_cells[i * pool_count + pick],
                     kos_policy_name(s->policy));

        json_writer_init(&w, line, sizeof line);
        decision_common(&w, s, "dispatch", "list-scheduling", &components);
        json_writer_key(&w, "order_id");
        json_writer_string(&w, order_id);
        json_writer_key(&w, "dish");
        json_writer_string(&w, dish);
        json_writer_key(&w, "task_id");
        json_writer_int(&w, (long long)msg.task_id);
        json_writer_key(&w, "node");
        json_writer_string(&w, node_id);
        json_writer_key(&w, "chef_id");
        json_writer_int(&w, chef);
        json_writer_key(&w, "priority");
        json_writer_int(&w, priority);
        json_writer_key(&w, "critical_ms");
        json_writer_int(&w, critical_ms);
        json_writer_key(&w, "slack_ms");
        json_writer_int(&w, (long long)slack_ms);
        json_writer_key(&w, "ready_tasks");
        json_writer_int(&w, cand_count);
        json_writer_key(&w, "station");
        json_writer_string(&w, station->id);
        json_writer_key(&w, "travel_cells");
        json_writer_int(&w, travel_cells[i * pool_count + pick]);
        json_writer_key(&w, "cell_ms");
        json_writer_double(&w, s->assign_cfg.cell_ms);
        json_writer_key(&w, "travel_s");
        json_writer_double(&w, terms[i * pool_count + pick].travel_s);
        json_writer_key(&w, "work_s");
        json_writer_double(&w, terms[i * pool_count + pick].work_s);
        json_writer_key(&w, "critical_s");
        json_writer_double(&w, terms[i * pool_count + pick].critical_s);
        json_writer_key(&w, "urgency");
        json_writer_double(&w, terms[i * pool_count + pick].urgency);
        json_writer_key(&w, "zone");
        json_writer_double(&w, terms[i * pool_count + pick].zone_penalty);
        json_writer_key(&w, "weight_d");
        json_writer_double(&w, s->assign_cfg.weights.w_d);
        json_writer_key(&w, "weight_t");
        json_writer_double(&w, s->assign_cfg.weights.w_t);
        json_writer_key(&w, "weight_c");
        json_writer_double(&w, terms[i * pool_count + pick].critical_x2
                                   ? s->assign_cfg.weights.w_c * KOS_ASSIGN_CRITICAL_BOOST
                                   : s->assign_cfg.weights.w_c);
        json_writer_key(&w, "weight_u");
        json_writer_double(&w, s->assign_cfg.weights.w_u);
        json_writer_key(&w, "weight_z");
        json_writer_double(&w, s->assign_cfg.weights.w_z);
        json_writer_key(&w, "open_orders");
        json_writer_int(&w, s->open_seq);
        json_writer_object_end(&w);
        write_jsonl(s->decisions_fd, &w);

        json_writer_init(&w, line, sizeof line);
        event_common(&w, s, "task_dispatched");
        json_writer_key(&w, "order_id");
        json_writer_string(&w, order_id);
        json_writer_key(&w, "task_id");
        json_writer_int(&w, (long long)msg.task_id);
        json_writer_key(&w, "chef_id");
        json_writer_int(&w, chef);
        json_writer_key(&w, "node");
        json_writer_string(&w, node_id);
        json_writer_key(&w, "station");
        json_writer_string(&w, station->id);
        event_end(s, &w);
    }
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
    kos_assign_pot_chain(&s->recipes, s->pot_chain);
    kos_error_clear(&err);
    if (kos_assign_config_for_kitchen(s->kitchen_path, &s->assign_cfg, &err) != KOS_OK) {
        char text[256];

        error_text(&err, text, sizeof text);
        kos_log_warn(s->rt.log, "schedule_config_failed", "path=%s error=%s",
                     s->kitchen_path, text);
        kos_assign_config_defaults(&s->assign_cfg);
    }
    kos_log_info(s->rt.log, "config_reload_applied",
                 "recipes=%s nodes=%d dishes=%d kitchen=%s cells=%d weights=%.2f/%.2f/"
                 "%.2f/%.2f/%.2f cell_ms=%.1f wash_ms=%.0f", s->recipes_path,
                 s->recipes.node_count, s->recipes.dish_count, s->kitchen_path,
                 s->kitchen.cell_count, s->assign_cfg.weights.w_d,
                 s->assign_cfg.weights.w_t, s->assign_cfg.weights.w_c,
                 s->assign_cfg.weights.w_u, s->assign_cfg.weights.w_z,
                 s->assign_cfg.cell_ms, s->assign_cfg.wash_ms);
}

static void strategy_switch(scheduler *s)
{
    kos_policy_kind from = kos_policy_kind_of(s->policy);
    kos_policy_kind to = kos_policy_other(from);
    json_writer w;
    char line[SCHED_LINE_MAX];

    kos_policy_set_kind(s->policy, to);
    kos_log_info(s->rt.log, "strategy_switch", "from=%s to=%s reason=SIGHUP",
                 kos_policy_kind_name(from), kos_policy_kind_name(to));
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, "strategy_switch");
    json_writer_key(&w, "from");
    json_writer_string(&w, kos_policy_kind_name(from));
    json_writer_key(&w, "to");
    json_writer_string(&w, kos_policy_kind_name(to));
    event_end(s, &w);

    json_writer_init(&w, line, sizeof line);
    decision_common(&w, s, "strategy", "SIGHUP", NULL);
    json_writer_key(&w, "from");
    json_writer_string(&w, kos_policy_kind_name(from));
    json_writer_key(&w, "to");
    json_writer_string(&w, kos_policy_kind_name(to));
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
    snapshot.dispatch_llf = s->dispatch_llf;
    snapshot.dispatch_hrrn = s->dispatch_hrrn;
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
                 "edges=%d", s->kitchen_path, s->kitchen.cell_count,
                 s->kitchen.station_count, s->recipes_path, s->recipes.node_count,
                 s->recipes.dish_count, s->recipes.edge_count);
    kos_assign_pot_chain(&s->recipes, s->pot_chain);
    kos_error_clear(&err);
    if (kos_assign_config_for_kitchen(s->kitchen_path, &s->assign_cfg, &err) != KOS_OK) {
        char text[256];

        error_text(&err, text, sizeof text);
        kos_log_warn(s->rt.log, "schedule_config_failed", "kitchen=%s error=%s",
                     s->kitchen_path, text);
        kos_assign_config_defaults(&s->assign_cfg);
    }
    kos_log_info(s->rt.log, "schedule_config",
                 "source=%s weights=%.2f/%.2f/%.2f/%.2f/%.2f cell_ms=%.1f wash_ms=%.0f",
                 s->assign_cfg.from_file ? "file" : "defaults",
                 s->assign_cfg.weights.w_d, s->assign_cfg.weights.w_t,
                 s->assign_cfg.weights.w_c, s->assign_cfg.weights.w_u,
                 s->assign_cfg.weights.w_z, s->assign_cfg.cell_ms,
                 s->assign_cfg.wash_ms);
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
        } else if (rc == KOS_ERR_EMPTY) {
            /* 真游戏只认游戏自己出的单：空 menu 明确表示"不造配置订单"。 */
            kos_log_info(s->rt.log, "menu_empty",
                         "path=%s orders will only come from the shm world state",
                         s->orders_path);
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
        maybe_generate_wash(s, now);

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
        if (s->wash.state == 2 &&
            now - s->wash.started_ms >
                (int64_t)(s->assign_cfg.wash_ms + 0.5) + SCHED_TASK_TIMEOUT_MS) {
            uint64_t task_id = s->wash.task_id;
            int chef = s->wash.chef_id;

            kos_log_warn(s->rt.log, "wash_timeout", "task=%llu chef=%d",
                         (unsigned long long)task_id, chef);
            if (chef >= 0 && chef < KOS_SHM_MAX_CHEFS &&
                s->running_by_chef[chef] == task_id) {
                s->running_by_chef[chef] = 0;
            }
            (void)release_lease_for_task(s, task_id, "resource_reclaimed",
                                         "wash-timeout");
            memset(&s->wash, 0, sizeof s->wash);
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
