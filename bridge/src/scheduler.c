/*
 * scheduler.c -- WellCooked Scheduler。
 *
 * 两级调度：
 *   1. 订单级：policy.h 的 FCFS / EDF / LLF / HRRN / SJF 只吃订单视图与资源视图。
 *   2. 工序级：订单展开成 recipes.json 的 DAG；前置完成且资源可预约的节点进就绪
 *      队列，按 --task-policy 排：cost（五项加权代价 + 匈牙利匹配，默认）、
 *      cp（剩余关键路径长的先做）、fifo（开工顺序 + 工序下标）。
 *
 * 任务走 POSIX mq（每厨师一条 /wellcooked_tasks_<chef>），结果从 /wellcooked_events
 * 回来。每次派发写 runs/<ts>/decisions.jsonl，状态变化写 events.jsonl；SIGHUP 时
 * 若 <runtime>/strategy.request 存在就按它切两级策略，否则订单级轮换，都留下一条
 * strategy 决策并重写 <runtime>/policy.json；SIGUSR1 写 checkpoint.json。
 *
 * 这里不碰游戏：世界状态只从 shm 读，动作只由 Worker 写输入槽。没有 Bridge 时
 * 它照样能凭 config/orders.json 的 menu 生成订单并派发，只是没人执行。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <wellcooked/assign.h>
#include <wellcooked/child.h>
#include <wellcooked/json.h>
#include <wellcooked/kitchen.h>
#include <wellcooked/log.h>
#include <wellcooked/metrics.h>
#include <wellcooked/mq.h>
#include <wellcooked/policy.h>
#include <wellcooked/recipes.h>
#include <wellcooked/resource.h>
#include <wellcooked/shm.h>
#include <wellcooked/stand.h>
#include <wellcooked/zones.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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
#define SCHED_ORDER_REWARD_UNKNOWN 0 /* 订单没有分值：策略按 0 处理，不参与奖励比较 */
#define SCHED_TASK_TIMEOUT_MS 30000
/* 距离场够不到（另一个连通区/厨师格未知）时给的路程惩罚，单位是格。 */
#define SCHED_UNREACHABLE_CELLS 600
#define SCHED_WASH_PRIORITY 32
#define SCHED_TASK_MAX_RETRIES 3 /* 同一工序宏动作最多失败 3 次 */
/* 第 n 次宏失败回队后等 n × 2 s 再派（P2 重试风暴的退避）。 */
#define SCHED_TASK_RETRY_BACKOFF_MS 2000
/* 交付动作做完后等游戏把这一单的分数记上；超过这个窗口才按分数增量判断成败。
 * fake_plugin 在 0.5 s 后才改分数，实机同一帧，1.5 s 两边都够。 */
#define SCHED_OUTCOME_GRACE_MS 1500
/* 子工序 DONE 后原料应在这个时间内出现在厨师手上；超过还空手就是"手上东西丢了"。 */
#define SCHED_HOLDING_LOSS_MS 1000
/* 放下工序的工时估计（goto + 一次交互），只用于派工代价。 */
#define SCHED_PUTDOWN_MS 1500
/* 更早订单剩余关键路径超过这个值就不再按开工顺序挡后面订单的 deliver。 */
#define SCHED_DELIVERY_WAIT_MAX_MS 6000
/* 料备齐后为了按序交付最多等这么久，超时就乱序装盘出餐（宁可断小费也不让人端着料干站）。 */
#define SCHED_DELIVERY_HOLD_MAX_MS 8000
/* 派工代价里倒料按这么长的关键路径算（秒，再 ×2），必须压过锅链上游 fetch/cut 的
 * 约 14.7 s 加路程差：锅熟后 12 s 就焦。 */
#define SCHED_POUR_CRITICAL_S 30.0

enum {
    SCHED_OUTCOME_UNKNOWN = 0,
    SCHED_OUTCOME_DELIVERED = 1,
    SCHED_OUTCOME_EXPIRED = 2,
    SCHED_OUTCOME_FAILED = 3 /* 游戏判送错菜（OnFoodDelivered(false)） */
};

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
    int fail_count; /* 宏动作失败后回队的次数；到 SCHED_TASK_MAX_RETRIES 才让订单失败 */
    uint64_t task_id;
    int64_t started_ms;
    int64_t ready_since_ms; /* 第一次进入就绪队列 */
    int64_t wait_started_ms; /* 最近一段资源等待的开始；0 = 未等待 */
    int wait_logged;
    int64_t not_before_ms; /* 宏失败回队后的退避截止；0 = 立即可派 */
    int64_t done_ms;       /* DONE 的时刻；持有亲和丢失判定的宽限起点 */
    int64_t score_base;    /* deliver 派发时的 world.score；G9 的分数锚点 */
    int score_base_valid;
    char station_id[KOS_SHM_NAME_MAX];
    int cooker_reserved;
    int64_t cooker_reserved_ms;
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
    int reject_event_pending; /* 启动期被拒的单，events.jsonl 还没打开，等打开后补写 */
    /* G9：完成/过期以游戏结果为准。world 订单的 DAG 走完只说明动作做完；
     * 分数增量为正才算交付，为负或动作没做完订单就消失则算过期。 */
    int outcome;
    int64_t reward_observed; /* 交付时实际观察到的分数增量 */
    int64_t score_anchor;    /* 订单到达、以及 deliver 派发时刷新的分数锚点 */
    int seen_in_world;       /* 至少有一帧在世界状态里见过；防止启动空帧误判过期 */
    int outcome_pending_logged;
    int64_t outcome_check_ms; /* 交付动作 DONE 后等游戏记分的截止 */
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

/* 手上拿着的东西没人要时的放下工序（不属于任何订单，和 wash 一样是服务任务）。 */
typedef struct {
    int state; /* 0 = 没有，1 = 待派，2 = 已派 */
    int chef_id;
    uint64_t task_id;
    int64_t ready_ms;
    int64_t started_ms;
    char item[KOS_SHM_NAME_MAX];
    int station_index;
} sched_putdown;

typedef struct {
    int order_index; /* -1 = wash, -2 = putdown */
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
    kos_task_policy task_policy;
    int64_t policy_since_ms;
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
    int64_t dispatch_sjf;
    int64_t worker_restarts;
    int blocked_hold[KOS_SHM_MAX_CHEFS]; /* 报过 BLOCKED 的厨师先不接新活 */
    int hold_stuck[KOS_SHM_MAX_CHEFS];   /* 手里那件东西没人要、也没台面可放 */
    int putdown_skip[KOS_SHM_MAX_CHEFS]; /* 上次放下失败的台面工位下标 + 1；0 = 无 */
    char metrics_path[KOS_DAEMON_PATH_MAX + 48];
    uint64_t next_task_id;
    int64_t started_ms;
    int64_t next_world_poll_ms;
    uint64_t running_by_chef[KOS_SHM_MAX_CHEFS];

    kos_assign_config assign_cfg;
    unsigned char pot_chain[KOS_RECIPE_MAX_NODES];
    /* 每道菜能不能被这套 kitchen 做出来（启动时算一次；见 recipe_feasibility_check）。 */
    unsigned char dish_feasible[KOS_RECIPE_MAX_DISHES];
    kos_world_state world;
    int world_valid;
    int round_started;
    int64_t world_read_ms;
    int64_t next_plate_poll_ms;
    sched_wash wash;
    sched_putdown putdown;
    int open_seq;
    int64_t result_seq_seen; /* order_results 去重游标（G9） */
    int result_seq_valid;

    mqd_t tasks[KOS_SHM_MAX_CHEFS];
    mqd_t events;
    int queues_ready;
    int zoned;
    kos_zones zones;
} scheduler;
static int cook_has_pour_successor(const scheduler *s, int cook_node);
static void release_pour_source(scheduler *s, sched_order *order,
                                const kos_recipe_node *pour_node);
static int pour_source_cook_task(const scheduler *s, const sched_order *order, int pour_node);
static int cook_has_plate_successor(const scheduler *s, int cook_node);
static void release_plate_source(scheduler *s, sched_order *order,
                                 const kos_recipe_node *plate_node);
static int plate_cook_task(const scheduler *s, const sched_order *order, int plate_node);
static int sink_station_index(const kos_kitchen *kitchen);
static int plate_count(const kos_world_state *world, const char *name);
static void salvage_expired_holdings(scheduler *s, const sched_order *order, int64_t now);
static int cooker_step_matches(const kos_station *station, const char *step);
static void recipe_feasibility_check(scheduler *s);
static void finish_order_if_done(scheduler *s, int order_index);

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
 * 配置订单在 load_config 里就建好了，那时 events.jsonl 还没打开；被 recipe_infeasible
 * 拒掉的单先把标记挂起，等 run dir 就绪后一次性补写事件。
 */
static void flush_rejected_order_events(scheduler *s)
{
    int i;

    for (i = 0; i < s->order_count; i++) {
        sched_order *order = &s->orders[i];
        json_writer w;
        char line[SCHED_LINE_MAX];

        if (!order->reject_event_pending) {
            continue;
        }
        if (s->events_fd < 0) {
            continue;
        }
        order->reject_event_pending = 0;
        json_writer_init(&w, line, sizeof line);
        event_common(&w, s, "order_rejected");
        json_writer_key(&w, "order_id");
        json_writer_string(&w, order->id);
        json_writer_key(&w, "dish");
        json_writer_string(&w, order->dish);
        json_writer_key(&w, "reason");
        json_writer_string(&w, "recipe_infeasible");
        event_end(s, &w);
    }
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
    json_writer_key(w, "task_policy");
    json_writer_string(w, kos_task_policy_name(s->task_policy));
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
/* Cook DONE releases the chef lease but transfers the already-held cooker token to the order. */
static int detach_lease_keep_token(scheduler *s, uint64_t task_id)
{
    kos_lease *lease = kos_lease_by_task(&s->leases, task_id);
    sched_res_stat *stat;
    char kind[KOS_RES_KIND_MAX];
    int64_t held_since;

    if (lease == NULL) return 0;
    copy_str(kind, sizeof kind, lease->kind);
    held_since = lease->acquired_ms;
    if (kos_lease_remove(&s->leases, task_id) != 1) return 0;
    stat = stat_find(s, kind);
    if (stat != NULL) stat->busy_ms += now_ms() - held_since;
    return 1;
}

static void release_order_cook_reservations(scheduler *s, sched_order *order)
{
    int i;
    for (i = 0; i < order->task_count; i++) {
        sched_task *task = &order->tasks[i];
        if (task->cooker_reserved) {
            const kos_recipe_node *node = &s->recipes.nodes[task->node_index];
            (void)kos_resource_release(&s->resources, node->resource);
            task->cooker_reserved = 0;
            task->station_id[0] = '\0';
        }
    }
}


/* 记录一次资源等待：每段等待只记一次，拿到令牌时再累加等待时长。 */
static void mark_resource_wait(scheduler *s, int chef, const sched_order *order,
                               sched_task *task, const kos_recipe_node *node)
{
    sched_res_stat *stat;
    const char *kind = node->resource;

    if (strcmp(node->op, "plate") == 0 &&
        kos_kitchen_count_kind(&s->kitchen, node->resource) == 0) {
        int sink = sink_station_index(&s->kitchen);

        if (sink >= 0) {
            kind = s->kitchen.stations[sink].kind;
        }
    }

    if (task->wait_logged) {
        return;
    }
    if (chef >= 0 && chef < KOS_SHM_MAX_CHEFS) {
        s->wanted_kind[chef] = kos_resources_index(&s->resources, kind);
    }
    task->wait_started_ms = now_ms();
    stat = stat_find(s, kind);
    if (stat != NULL) {
        stat->waits++;
    }
    {
        json_writer w;
        char line[SCHED_LINE_MAX];

        json_writer_init(&w, line, sizeof line);
        event_common(&w, s, "resource_wait");
        json_writer_key(&w, "kind");
        json_writer_string(&w, kind);
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
                 kind, order->id, node->id, chef);
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

/* 宏动作以 pickup 收尾的工序：做完后产物在这个厨师手里。 */
static int op_leaves_item_in_hand(const char *op)
{
    return strcmp(op, "fetch") == 0 || strcmp(op, "cut") == 0 || strcmp(op, "mix") == 0 ||
           strcmp(op, "pickup") == 0 || strcmp(op, "plate") == 0;
}

static const kos_chef *world_chef(const kos_world_state *world, int chef);

/*
 * 手上端着装有这道 pour 原料的锅的厨师（插件的 carried_<chefId>），没有返回 -1。
 * 只看容器内容：锅名会随 prefab 变，内容才是这道工序要的东西。
 */
static int pour_pot_carrier(const scheduler *s, const kos_recipe_node *node)
{
    const char *want = node->ingredient;
    int i;

    if (!s->world_valid) {
        return -1;
    }
    if (want[0] == '\0') {
        const char *colon = strchr(node->id, ':');

        want = colon != NULL ? colon + 1 : "";
    }
    if (want[0] == '\0') {
        return -1;
    }
    for (i = 0; i < (int)s->world.resource_count; i++) {
        const kos_resource *res = &s->world.resources[i];
        int chef;
        int k;

        if (strncmp(res->name, "carried_", 8) != 0) {
            continue;
        }
        chef = atoi(res->name + 8);
        if (chef < 0 || chef >= s->rt.opts.chefs) {
            continue;
        }
        /* 只认锅：端着的玉米饼/盘子里也有 cooked:Rice（实机 20261028-022924 把卷饼
         * 当成米锅，pour:Rice 派给拿卷饼的人连失败 5 次）。 */
        {
            const kos_chef *holder = world_chef(&s->world, chef);

            if (holder == NULL || !(kos_kitchen_name_matches("pot", holder->holding) ||
                                    kos_kitchen_name_matches("pan", holder->holding))) {
                continue;
            }
        }
        if (res->content[0] != '\0' && kos_kitchen_name_matches(want, res->content)) {
            return chef;
        }
        for (k = 0; k < (int)res->content_count && k < KOS_SHM_MAX_CONTENTS; k++) {
            if (kos_kitchen_name_matches(want, res->contents[k])) {
                return chef;
            }
        }
    }
    return -1;
}

/*
 * 手里拿着这道工序原料的厨师：某个已完成的子工序把产物留在了他手上，后继只能由他做，
 * 换成空手的人去 drop/deliver 会空等到超时（_3 实测 deliver 被派给没端盘子的人）。
 * 两个子工序分别在两个厨师手里时一道工序满足不了两人，返回 -1 不绑定。
 */
static int task_holder_chef(const scheduler *s, const sched_order *order, int task_index)
{
    int node_index = order->tasks[task_index].node_index;
    int holder = -1;
    int e;

    for (e = 0; e < s->recipes.edge_count; e++) {
        int child_task;
        const sched_task *child;

        if (s->recipes.edges[e].to != node_index) {
            continue;
        }
        child_task = order_task_by_node(order, s->recipes.edges[e].from);
        if (child_task < 0) {
            continue;
        }
        child = &order->tasks[child_task];
        if (child->state != TASK_DONE || child->chef_id < 0 ||
            !op_leaves_item_in_hand(s->recipes.nodes[child->node_index].op)) {
            continue;
        }
        if (holder >= 0 && holder != child->chef_id) {
            return -1;
        }
        holder = child->chef_id;
    }
    /* 失败回队过的工序：宏第一步已把手里的东西放到工位上（装盘放到盘堆、切菜放到
     * 砧板），持有者空手了就不再绑定他——否则空手的他接不了、别人也接不了，整单
     * 卡死（实机 20261028-014743：order_001 装盘重试永不派出，后续订单全部停摆）。 */
    if (holder >= 0 && order->tasks[task_index].fail_count > 0 && s->world_valid) {
        const kos_chef *chef = world_chef(&s->world, holder);

        if (chef != NULL && chef->holding[0] == '\0') {
            return -1;
        }
    }
    /* 倒料端着锅走到一半被 navd 判卡住（实机 20261028-020415 order_003 pour:Rice）：
     * 锅在他手上、灶上空了，别人端不到锅，只能由他接着倒。 */
    if (holder < 0 && strcmp(s->recipes.nodes[node_index].op, "pour") == 0) {
        int carrier = pour_pot_carrier(s, &s->recipes.nodes[node_index]);

        if (carrier >= 0) {
            return carrier;
        }
    }
    return holder;
}

/* bound[chef] = 1：这个厨师手里拿着某道待派工序的原料，只能接那道工序。 */
static void holding_chefs(const scheduler *s, int bound[KOS_SHM_MAX_CHEFS])
{
    int i;
    int j;

    memset(bound, 0, sizeof(int) * KOS_SHM_MAX_CHEFS);
    for (i = 0; i < s->order_count; i++) {
        const sched_order *order = &s->orders[i];

        if (order->status != KOS_ORDER_ACTIVE) {
            continue;
        }
        for (j = 0; j < order->task_count; j++) {
            int holder;

            if (order->tasks[j].state != TASK_PENDING) {
                continue;
            }
            holder = task_holder_chef(s, order, j);
            if (holder >= 0 && holder < KOS_SHM_MAX_CHEFS) {
                bound[holder] = 1;
            }
        }
    }
}

/* 归一名比较，与 macro.c 的 name_eq 同一规则（忽略大小写，'-' 与 '_' 等价）。 */
static int sched_name_eq(const char *a, const char *b)
{
    size_t i = 0;

    if (a == NULL || b == NULL) {
        return 0;
    }
    for (;;) {
        char ca = a[i];
        char cb = b[i];

        if (ca == '-' || ca == '_') {
            ca = '_';
        }
        if (cb == '-' || cb == '_') {
            cb = '_';
        }
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
        i++;
    }
}

/*
 * 世界状态里厨师手上拿着什么。返回 NULL = 不知道（没有世界状态/该厨师不在帧里），
 * 空串 = 空手，其它 = 物品名。NULL 与 "" 必须分开：不知道时保持旧的图亲和规则，
 * 不能把"没读到"当成"空手"而卡住所有派工。
 */
static const char *chef_holding(const kos_world_state *world, int chef)
{
    int i;

    if (world == NULL) {
        return NULL;
    }
    for (i = 0; i < (int)world->chef_count && i < KOS_SHM_MAX_CHEFS; i++) {
        if (world->chefs[i].id == chef) {
            return world->chefs[i].holding;
        }
    }
    return NULL;
}

/*
 * 消费手上物品的工序：先把料放下/开火/端去交付，手上必须是同一件东西。
 * plate（把手里整份菜放到盘堆）与 place（把玉米饼放到台面当容器）都是"手上有东西
 * 才能做"的工序；漏掉它们，厨师切完/拿到玉米饼后再也接不到下一道工序（G11 的死角，
 * 实机 demo3 的 sushi/burrito 链都卡在这里）。pour 不在此列：pour 的宏先端锅，
 * 派工时厨师必须是空手。
 */
static int op_consumes_holding(const char *op)
{
    return op != NULL && (strcmp(op, "cook") == 0 || strcmp(op, "cut") == 0 ||
                          strcmp(op, "mix") == 0 || strcmp(op, "assemble") == 0 ||
                          strcmp(op, "deliver") == 0 || strcmp(op, "plate") == 0 ||
                          strcmp(op, "place") == 0);
}

/*
 * 厨师能不能接这道工序（G11）。规则：
 *   - 手上不空：只接消费手上东西的工序，而且图上的持有亲和必须就是他
 *     （原料在他手里，换成空手的人去 drop 会"已放下"直接完成、去 fetch 只能超时）。
 *   - 空手：只接取料/拿盘/洗盘；拿着料却空手的持有者不能接 drop/开火，
 *     否则宏会对着空锅按交互直到超时。
 *   - 世界状态读不到时退回旧规则（只看 holder/bound），离线测试照旧。
 */
static int chef_can_take_task(const kos_world_state *world, int chef, const char *op,
                              int holder, int bound)
{
    const char *holding = chef_holding(world, chef);

    if (holding == NULL) {
        return holder >= 0 ? holder == chef : !bound;
    }
    if (holding[0] == '\0') {
        if (holder >= 0 || bound) {
            return 0;
        }
        /* 煮/煎类热菜在锅里（holder < 0），装盘需要空手去盘堆拿盘；
         * 切好/做好的冷菜在手上（holder >= 0），由上面的 holder 判定锁定制作者。 */
        if (strcmp(op, "plate") == 0) {
            return 1;
        }
        return !op_consumes_holding(op);
    }
    /* 端着源锅的人续做 pour（task_holder_chef 把 holder 设成了他）。 */
    if (strcmp(op, "pour") == 0) {
        return holder == chef;
    }
    /* 热菜装盘原料在灶上（holder < 0），手上拿着别的东西的厨师不能接装盘 */
    if (holder < 0 && strcmp(op, "plate") == 0) {
        return 0;
    }
    /* holder < 0 说明图上没有唯一持有者（deliver 只有盘子、或两个子工序在两个人手里）：
     * 这时允许手上有东西的厨师去试，内容核对会把放错的料判成 content-mismatch。
     * 但手里拿着另一道待派工序原料的人（bound）不能去：实机 20261028-064643 厨师 0
     * 端着切好的虾被派去切鸡肉，把虾放上 board-0 后两单都卡死（order_006 item-lost）。 */
    return op_consumes_holding(op) && (holder == chef || (holder < 0 && !bound));
}

/*
 * 手上物品是否就是某个节点的产物。fetch/cut/mix 的节点名后缀就是产物名
 * （fetch:Tomato、cut:ChoppedTomato）；plate 的产物是盘上那道菜，靠
 * holding_contents 匹配（G4 之后插件会发盘内容）。
 */
static int node_product_is_held(const sched_order *order, int task_index,
                                const kos_recipes *recipes, const kos_chef *chef)
{
    const kos_recipe_node *node = &recipes->nodes[order->tasks[task_index].node_index];
    const char *colon;

    if (chef == NULL || chef->holding[0] == '\0') {
        return 0;
    }
    if (strcmp(node->op, "fetch") == 0 || strcmp(node->op, "cut") == 0 ||
        strcmp(node->op, "mix") == 0) {
        colon = strchr(node->id, ':');
        return colon != NULL && sched_name_eq(colon + 1, chef->holding);
    }
    if (strcmp(node->op, "plate") == 0) {
        int i;

        if (!sched_name_eq(chef->holding, "Plate")) {
            return 0;
        }
        for (i = 0; i < (int)chef->holding_content_count && i < KOS_SHM_MAX_CONTENTS; i++) {
            if (sched_name_eq(chef->holding_contents[i], node->label) ||
                sched_name_eq(chef->holding_contents[i], order->dish)) {
                return 1;
            }
        }
        /* 内容读不到时不能靠猜：盘里是哪道菜未验证，不做采纳。 */
    }
    return 0;
}

static const kos_chef *world_chef(const kos_world_state *world, int chef)
{
    int i;

    if (world == NULL) {
        return NULL;
    }
    for (i = 0; i < (int)world->chef_count && i < KOS_SHM_MAX_CHEFS; i++) {
        if (world->chefs[i].id == chef) {
            return &world->chefs[i];
        }
    }
    return NULL;
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
    int i;

    order->status = status;
    order->finished_ms = now;
    order->lateness_ms = now > order->deadline_ms ? now - order->deadline_ms : 0;
    for (i = 0; i < order->task_count; i++) {
        order->tasks[i].wait_logged = 0;
        order->tasks[i].wait_started_ms = 0;
    }
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
    if (order->status != KOS_ORDER_ACTIVE) {
        /* 订单已经以交付/过期/失败收尾：租约到期或 Worker 恢复不能再把它排回来。 */
        if (task->chef_id >= 0 && task->chef_id < KOS_SHM_MAX_CHEFS &&
            s->running_by_chef[task->chef_id] == task->task_id) {
            s->running_by_chef[task->chef_id] = 0;
        }
        task->chef_id = -1;
        task->task_id = 0;
        return;
    }
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

/* 放下工序失败一次就放弃：再派一次还是同一个动作（P2 不重试风暴）。 */
static void deactivate_putdown(scheduler *s, const char *reason)
{
    if (s->putdown.state == 0) {
        return;
    }
    kos_log_warn(s->rt.log, "putdown_dropped", "reason=%s task=%llu chef=%d", reason,
                 (unsigned long long)s->putdown.task_id, s->putdown.chef_id);
    memset(&s->putdown, 0, sizeof s->putdown);
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
        } else if (task_id != 0 && task_id == s->putdown.task_id) {
            deactivate_putdown(s, reason);
        } else {
            task_requeue(s, order_index, task_index, reason);
            if (order_index >= 0 && order_index < s->order_count &&
                task_index >= 0 && task_index < s->orders[order_index].task_count) {
                /* 世界状态缓存最多 100 ms 旧：先等一帧，让"料其实已经在他手上"
                 * 被看到，再由第 12 条用例里的采纳路径接手，而不是重派一次 fetch。 */
                s->orders[order_index].tasks[task_index].not_before_ms =
                    now_ms() + SCHED_WORLD_POLL_MS + 50;
            }
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
            } else if (task_id != 0 && task_id == s->putdown.task_id) {
                deactivate_putdown(s, "lease-expired");
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
            } else if (task_id != 0 && task_id == s->putdown.task_id) {
                deactivate_putdown(s, "deadlock-victim");
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
    /* G7：优先用游戏上报的每单分值（orders[].score，协议已定义）；没有时用
     * 菜单导出的 dish score；两者都没有记 0（未知），不再固定 10。 */
    if (reward <= 0 && s->recipes.dishes[dish_index].score > 0) {
        reward = s->recipes.dishes[dish_index].score;
    }
    order->reward = reward > 0 ? reward : SCHED_ORDER_REWARD_UNKNOWN;
    order->status = KOS_ORDER_ACTIVE;
    order->from_world = from_world;
    order->task_count = s->recipes.dishes[dish_index].node_count;
    for (i = 0; i < order->task_count; i++) {
        order->tasks[i].node_index = s->recipes.dishes[dish_index].nodes[i];
        order->tasks[i].state = TASK_PENDING;
        order->tasks[i].chef_id = -1;
        order->tasks[i].avoid_chef = -1;
        order->tasks[i].fail_count = 0;
        order->tasks[i].task_id = 0;
    }
    if (!s->dish_feasible[dish_index]) {
        /* 工序图里有这套厨房做不出来的节点：留着单子但直接判失败，别再派下去等超时。 */
        order->status = KOS_ORDER_FAILED;
        order->finished_ms = now;
        order->reject_event_pending = 1;
        kos_log_error(s->rt.log, "order_rejected", "id=%s dish=%s reason=recipe_infeasible",
                      id, dish);
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
    json_writer_key(&w, "reward");
    json_writer_int(&w, order->reward);
    json_writer_key(&w, "source");
    json_writer_string(&w, from_world ? "world" : "config");
    event_end(s, &w);
    flush_rejected_order_events(s);
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
    char events_name[KOS_MQ_NAME_MAX];
    int i;
    int rc;

    if (kos_mq_events_name(s->rt.opts.instance, events_name, sizeof events_name) != KOS_OK) {
        kos_log_error(s->rt.log, "mq_name_failed", "instance=%d", s->rt.opts.instance);
        return -1;
    }
    /*
     * 第一次启动清掉上一局残留的队列再建。Supervisor 重新拉起（generation > 1）时不能删：
     * Worker 手里还开着旧队列的描述符，删了再建等于换了一条 Worker 永远读不到的新队列
     * （实机 20261028-133741：kill -9 后派了 16 个任务，Worker 一个都没收到）。
     */
    if (s->rt.opts.generation <= 1) {
        kos_error_clear(&err);
        rc = kos_mq_unlink_all(s->rt.opts.chefs, s->rt.opts.instance, &err);
        if (rc != KOS_OK) {
            char text[256];

            error_text(&err, text, sizeof text);
            kos_log_warn(s->rt.log, "mq_stale_cleanup_failed", "error=%s", text);
        }
    }
    kos_error_clear(&err);
    rc = kos_mq_create(events_name, KOS_MQ_MAX_MESSAGES, &err);
    if (rc != KOS_OK && !(rc == KOS_ERR_EXISTS && s->rt.opts.generation > 1)) {
        char text[256];

        error_text(&err, text, sizeof text);
        kos_log_error(s->rt.log, "mq_create_failed", "name=%s error=%s", events_name, text);
        return -1;
    }
    for (i = 0; i < s->rt.opts.chefs; i++) {
        char name[KOS_MQ_NAME_MAX];

        if (kos_mq_task_name(i, s->rt.opts.instance, name, sizeof name) != KOS_OK) {
            return -1;
        }
        kos_error_clear(&err);
        rc = kos_mq_create(name, KOS_MQ_MAX_MESSAGES, &err);
        if (rc != KOS_OK && !(rc == KOS_ERR_EXISTS && s->rt.opts.generation > 1)) {
            char text[256];

            error_text(&err, text, sizeof text);
            kos_log_error(s->rt.log, "mq_create_failed", "name=%s error=%s", name, text);
            return -1;
        }
    }
    kos_error_clear(&err);
    if (kos_mq_open(events_name, O_RDONLY, &s->events, &err) != KOS_OK) {
        char text[256];

        error_text(&err, text, sizeof text);
        kos_log_error(s->rt.log, "mq_open_failed", "name=%s error=%s", events_name, text);
        return -1;
    }
    for (i = 0; i < s->rt.opts.chefs; i++) {
        char name[KOS_MQ_NAME_MAX];

        if (kos_mq_task_name(i, s->rt.opts.instance, name, sizeof name) != KOS_OK) {
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
    kos_log_info(s->rt.log, "mq_ready", "events=%s task_queues=%d", events_name,
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
        (void)kos_mq_unlink_all(s->rt.opts.chefs, s->rt.opts.instance, &err);
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

/* ===================================================== 订单结果与过期收尾 */

static int world_order_present(const kos_world_state *world, const char *id)
{
    int i;

    for (i = 0; i < (int)world->order_count && i < KOS_SHM_MAX_ORDERS; i++) {
        if (strcmp(world->orders[i].id, id) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * 订单以某种结果结束。租约留给正在执行的宏自己收尾（RUNNING 的任务不能凭空
 * 取消，Worker 也不支持中途撤销）；PENDING 的任务直接作废，cook 的锅令牌在这里
 * 归还，避免过期订单一直占着灶台。
 */
static void finish_order_with_outcome(scheduler *s, int order_index, int status,
                                      const char *source, int64_t now)
{
    sched_order *order = &s->orders[order_index];
    json_writer w;
    char line[SCHED_LINE_MAX];
    const char *event_name;
    int i;

    if (order->status != KOS_ORDER_ACTIVE) {
        return;
    }
    release_order_cook_reservations(s, order);
    for (i = 0; i < order->task_count; i++) {
        sched_task *task = &order->tasks[i];

        task->station_id[0] = '\0';
        if (task->wait_logged && task->wait_started_ms > 0) {
            int64_t wait_ms = now - task->wait_started_ms;
            if (wait_ms > 0) {
                const kos_recipe_node *node = &s->recipes.nodes[task->node_index];
                sched_res_stat *stat = stat_find(s, node->resource);
                if (stat != NULL) {
                    stat->wait_ms += wait_ms;
                }
            }
            task->wait_logged = 0;
            task->wait_started_ms = 0;
        }
        if (task->state == TASK_PENDING) {
            task->state = TASK_FAILED;
        }
    }
    mark_order_finished(order, status, now);
    event_name = status == KOS_ORDER_COMPLETED
                     ? "order_completed"
                     : (status == KOS_ORDER_EXPIRED ? "order_expired" : "order_failed");
    kos_log_info(s->rt.log, event_name,
                 "id=%s dish=%s source=%s outcome=%d reward=%lld tasks=%d", order->id,
                 order->dish, source != NULL ? source : "", order->outcome,
                 (long long)order->reward_observed, order->task_count);
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, event_name);
    json_writer_key(&w, "order_id");
    json_writer_string(&w, order->id);
    json_writer_key(&w, "dish");
    json_writer_string(&w, order->dish);
    json_writer_key(&w, "source");
    json_writer_string(&w, source != NULL ? source : "");
    json_writer_key(&w, "outcome");
    json_writer_string(&w, order->outcome == SCHED_OUTCOME_DELIVERED
                              ? "delivered"
                              : (order->outcome == SCHED_OUTCOME_EXPIRED
                                     ? "expired"
                                     : (order->outcome == SCHED_OUTCOME_FAILED ? "failed"
                                                                               : "unknown")));
    json_writer_key(&w, "reward");
    json_writer_int(&w, order->reward_observed);
    event_end(s, &w);
}

static int counter_station_index(const kos_kitchen *kitchen)
{
    int i;

    for (i = 0; i < kitchen->station_count; i++) {
        if (kos_kitchen_name_matches("counter", kitchen->stations[i].kind)) {
            return i;
        }
    }
    return -1;
}

/* 台面格上还摆着别的工位（盘堆、水槽……）时，往上放东西会落进那个工位：
 * 实机 20261028-030252 把没人要的鱼放到 counter-18，等于盖在了干净盘堆顶上的盘子里，
 * 后面的装盘全部失败。 */
static int counter_shares_cell(const kos_kitchen *kitchen, int counter)
{
    const kos_station *st = &kitchen->stations[counter];
    int i;

    for (i = 0; i < kitchen->station_count; i++) {
        const kos_station *other = &kitchen->stations[i];

        if (i == counter || kos_kitchen_name_matches("counter", other->kind)) {
            continue;
        }
        if (fabsf(other->x - st->x) < 0.1f && fabsf(other->z - st->z) < 0.1f) {
            return 1;
        }
    }
    return 0;
}

/* 台面上已经摆着东西（玉米饼、盘子……）或者有活跃订单的 place 工序把它当容器：
 * 往上放手里的料要么被拒，要么混进别人的卷饼。实机 20261028-043613 worker-0
 * 在摆着 Tortilla 的 counter-13/counter-9 之间来回放 ChoppedMushroom 失败 16 次。 */
static int counter_occupied(const scheduler *s, int counter)
{
    const kos_station *st = &s->kitchen.stations[counter];
    int i;
    int j;

    if (s->world_valid) {
        for (i = 0; i < (int)s->world.resource_count && i < KOS_SHM_MAX_RESOURCES; i++) {
            const kos_resource *r = &s->world.resources[i];

            if (!r->has_pos || fabsf(r->pos_x - st->x) >= 0.3f ||
                fabsf(r->pos_y - st->z) >= 0.3f) {
                continue;
            }
            if (r->item[0] != '\0' || r->content[0] != '\0' || r->content_count > 0 ||
                (r->kind[0] != '\0' && strcmp(r->kind, st->kind) != 0)) {
                return 1;
            }
        }
    }
    for (i = 0; i < s->order_count; i++) {
        const sched_order *order = &s->orders[i];

        if (order->status != KOS_ORDER_ACTIVE) {
            continue;
        }
        for (j = 0; j < order->task_count; j++) {
            const sched_task *task = &order->tasks[j];

            if (task->state != TASK_PENDING && task->station_id[0] != '\0' &&
                strcmp(task->station_id, st->id) == 0 &&
                strcmp(s->recipes.nodes[task->node_index].op, "place") == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* 离厨师最近的台面（跳过上次放失败的那个）。只有一个台面或拿不到厨师位置时
 * 退回第一个台面。 */
static int counter_station_near(scheduler *s, int chef)
{
    const kos_chef *c = world_chef(&s->world, chef);
    int skip = (chef >= 0 && chef < KOS_SHM_MAX_CHEFS) ? s->putdown_skip[chef] - 1 : -1;
    int best = -1;
    float best_d = 0.0f;
    int i;

    if (c == NULL) {
        return counter_station_index(&s->kitchen);
    }
    for (i = 0; i < s->kitchen.station_count; i++) {
        const kos_station *st = &s->kitchen.stations[i];
        float dx, dz, d;

        if (i == skip || !kos_kitchen_name_matches("counter", st->kind) ||
            counter_shares_cell(&s->kitchen, i) || counter_occupied(s, i)) {
            continue;
        }
        dx = st->x - c->x;
        dz = st->z - c->y;
        d = dx * dx + dz * dz;
        if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best >= 0 ? best : counter_station_index(&s->kitchen);
}

/* 把手上没人要的东西放下。没有普通台面工位（kitchen.json 目前不导出 Counter）时
 * 只能记 orphaned，等工位表补上后这条路径才有地方放。 */
static void queue_putdown(scheduler *s, int chef, const char *item, const char *reason,
                          int64_t now)
{
    int station = counter_station_near(s, chef);
    json_writer w;
    char line[SCHED_LINE_MAX];
    const char *event_name;

    if (s->putdown.state != 0 && s->putdown.chef_id == chef) {
        /* 同一厨师的同一件手持物已经在放下队列里。salvage_expired_holdings 会对
         * 他做过的每道 fetch/cut/plate 各调一次（实机 20261027-200237
         * scheduler.log:58-60）：第二次不能把他记成 hold_stuck，否则他被摘出空闲表、
         * 自己那条放下工序永远派不出去，放下槽位也一直占着。 */
        return;
    }
    if (station < 0 || s->putdown.state != 0) {
        event_name = "hold_orphaned";
        if (chef >= 0 && chef < KOS_SHM_MAX_CHEFS) {
            /* 没地方放：先把人从空闲表里摘掉，避免每轮都拿"该派活"去开新订单。 */
            s->hold_stuck[chef] = 1;
        }
        kos_log_warn(s->rt.log, event_name, "chef=%d item=%s reason=%s station=%d",
                     chef, item != NULL ? item : "", reason != NULL ? reason : "",
                     station);
    } else {
        memset(&s->putdown, 0, sizeof s->putdown);
        s->putdown.state = 1;
        s->putdown.chef_id = chef;
        s->putdown.station_index = station;
        s->putdown.ready_ms = now;
        copy_str(s->putdown.item, sizeof s->putdown.item, item);
        if (chef >= 0 && chef < KOS_SHM_MAX_CHEFS) {
            s->hold_stuck[chef] = 0; /* 有地方放就不再扣留这个厨师 */
        }
        event_name = "putdown_queued";
        kos_log_info(s->rt.log, event_name, "chef=%d item=%s station=%s reason=%s", chef,
                     item != NULL ? item : "", s->kitchen.stations[station].id,
                     reason != NULL ? reason : "");
    }
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, event_name);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef);
    json_writer_key(&w, "item");
    json_writer_string(&w, item != NULL ? item : "");
    json_writer_key(&w, "reason");
    json_writer_string(&w, reason != NULL ? reason : "");
    event_end(s, &w);
}

/* 节点祖先里还有没有正在执行的工序；有就不能采纳（会和正在跑的动作抢东西）。 */
static int ancestors_adoptable(const scheduler *s, const sched_order *order, int node,
                               unsigned char *seen)
{
    int e;

    for (e = 0; e < s->recipes.edge_count; e++) {
        int child_task;
        const sched_task *child;

        if (s->recipes.edges[e].to != node) {
            continue;
        }
        child_task = order_task_by_node(order, s->recipes.edges[e].from);
        if (child_task < 0) {
            continue;
        }
        child = &order->tasks[child_task];
        if (child->state == TASK_DONE) {
            continue;
        }
        if (child->state == TASK_RUNNING) {
            return 0;
        }
        if (seen[child_task]) {
            continue;
        }
        seen[child_task] = 1;
        if (!ancestors_adoptable(s, order, child->node_index, seen)) {
            return 0;
        }
    }
    return 1;
}

/* 采纳一件成品等于这条链上的工序都已经有产物，全部记 DONE；锅令牌一并归还。 */
static void mark_ancestors_done(scheduler *s, sched_order *order, int node, int64_t now,
                                unsigned char *seen)
{
    int e;

    for (e = 0; e < s->recipes.edge_count; e++) {
        int child_task;
        sched_task *child;

        if (s->recipes.edges[e].to != node) {
            continue;
        }
        child_task = order_task_by_node(order, s->recipes.edges[e].from);
        if (child_task < 0 || order->tasks[child_task].state == TASK_DONE ||
            seen[child_task]) {
            continue;
        }
        seen[child_task] = 1;
        child = &order->tasks[child_task];
        mark_ancestors_done(s, order, child->node_index, now, seen);
        child->state = TASK_DONE;
        child->chef_id = -1;
        child->done_ms = now;
        if (child->cooker_reserved) {
            (void)kos_resource_release(&s->resources, "cooker");
            child->cooker_reserved = 0;
            child->station_id[0] = '\0';
        }
    }
}

/*
 * G11：厨师手里那件东西如果正是另一张订单待做工序的产物，就直接记到那张订单
 * 名下（连同它的祖先链），让这个厨师接着往下做，而不是把东西丢在地上重做。
 * 匹配按产物名（fetch/cut/mix）或盘内容（plate），不跨菜乱认。
 */
static int chef_holding_for_active_order(const scheduler *s, int chef)
{
    int i, j;

    for (i = 0; i < s->order_count; i++) {
        const sched_order *order = &s->orders[i];

        if (order->status != KOS_ORDER_ACTIVE) {
            continue;
        }
        for (j = 0; j < order->task_count; j++) {
            if (task_holder_chef(s, order, j) == chef) {
                return 1;
            }
        }
    }
    return 0;
}

static int try_adopt_holding(scheduler *s, int chef, const char *reason, int64_t now)
{
    const kos_chef *state = world_chef(&s->world, chef);
    int i;

    if (state == NULL || state->holding[0] == '\0') {
        return 0;
    }
    if (chef_holding_for_active_order(s, chef)) {
        return 0;
    }
    for (i = 0; i < s->order_count; i++) {
        sched_order *order = &s->orders[i];
        int j;

        if (order->status != KOS_ORDER_ACTIVE) {
            continue;
        }
        for (j = 0; j < order->task_count; j++) {
            unsigned char seen[KOS_RECIPE_MAX_DISH_NODES];
            sched_task *task = &order->tasks[j];
            json_writer w;
            char line[SCHED_LINE_MAX];

            if (task->state != TASK_PENDING ||
                !node_product_is_held(order, j, &s->recipes, state)) {
                continue;
            }
            memset(seen, 0, sizeof seen);
            if (!ancestors_adoptable(s, order, task->node_index, seen)) {
                continue;
            }
            memset(seen, 0, sizeof seen);
            mark_ancestors_done(s, order, task->node_index, now, seen);
            task->state = TASK_DONE;
            task->chef_id = chef;
            task->done_ms = now;
            kos_log_info(s->rt.log, "item_adopted",
                         "order=%s node=%s chef=%d item=%s reason=%s", order->id,
                         s->recipes.nodes[task->node_index].id, chef, state->holding,
                         reason != NULL ? reason : "");
            json_writer_init(&w, line, sizeof line);
            event_common(&w, s, "item_adopted");
            json_writer_key(&w, "order_id");
            json_writer_string(&w, order->id);
            json_writer_key(&w, "node");
            json_writer_string(&w, s->recipes.nodes[task->node_index].id);
            json_writer_key(&w, "chef_id");
            json_writer_int(&w, chef);
            json_writer_key(&w, "item");
            json_writer_string(&w, state->holding);
            event_end(s, &w);
            finish_order_if_done(s, i);
            return 1;
        }
    }
    return 0;
}

/* 订单过期后处理它留在厨师手上的东西：先给同菜的另一单，没人要就放下。 */
static void salvage_chef_holding(scheduler *s, int chef, const char *reason, int64_t now)
{
    const kos_chef *state;

    if (!s->world_valid) {
        return;
    }
    state = world_chef(&s->world, chef);
    if (state == NULL || state->holding[0] == '\0') {
        return;
    }
    /* 手上的东西属于另一张活跃订单（实机 20261027-220205：order_004 失败时把
     * chef0 给 order_005 拿的 Rice 放下了，order_005 随即 holding-lost）。 */
    if (chef_holding_for_active_order(s, chef)) {
        return;
    }
    if (try_adopt_holding(s, chef, reason, now)) {
        return;
    }
    queue_putdown(s, chef, state->holding, reason, now);
}

static void salvage_expired_holdings(scheduler *s, const sched_order *order, int64_t now)
{
    int j;

    for (j = 0; j < order->task_count; j++) {
        const sched_task *task = &order->tasks[j];

        if (task->state != TASK_DONE || task->chef_id < 0 ||
            !op_leaves_item_in_hand(s->recipes.nodes[task->node_index].op)) {
            continue;
        }
        if (task->chef_id < KOS_SHM_MAX_CHEFS &&
            s->running_by_chef[task->chef_id] != 0) {
            continue; /* 他正在做后续工序，事件回来时再收 */
        }
        salvage_chef_holding(s, task->chef_id, "order-expired", now);
    }
}

/*
 * G9：插件把最近结束的订单放在 order_results[] 里每帧重发，按 seq 去重。这是
 * 完成/过期的第一手判据；world.score 增量只是它没上报时的退路。seq 比上次见到
 * 的小说明插件重启过，游标归零。
 */
static void handle_order_result(scheduler *s, const kos_order_result *result, int64_t now)
{
    int order_index = order_find(s, result->id);
    sched_order *order;

    if (order_index < 0) {
        kos_log_debug(s->rt.log, "order_result_unmatched", "id=%s outcome=%s seq=%lld",
                      result->id, result->outcome, (long long)result->seq);
        return;
    }
    order = &s->orders[order_index];
    if (order->status != KOS_ORDER_ACTIVE) {
        return;
    }
    if (strcmp(result->outcome, "delivered") == 0) {
        order->outcome = SCHED_OUTCOME_DELIVERED;
        order->reward_observed = result->has_score ? result->score : 0;
        finish_order_with_outcome(s, order_index, KOS_ORDER_COMPLETED, "game-result", now);
        return;
    }
    if (strcmp(result->outcome, "expired") == 0) {
        order->outcome = SCHED_OUTCOME_EXPIRED;
        finish_order_with_outcome(s, order_index, KOS_ORDER_EXPIRED, "game-result", now);
        salvage_expired_holdings(s, order, now);
        return;
    }
    order->outcome = SCHED_OUTCOME_FAILED;
    finish_order_with_outcome(s, order_index, KOS_ORDER_FAILED, "game-result", now);
    salvage_expired_holdings(s, order, now);
}

static void poll_order_results(scheduler *s, const kos_world_state *world, int64_t now)
{
    int64_t newest = 0;
    uint32_t i;

    for (i = 0; i < world->order_result_count && i < KOS_SHM_MAX_ORDER_RESULTS; i++) {
        if (world->order_results[i].seq > newest) {
            newest = world->order_results[i].seq;
        }
    }
    if (s->result_seq_valid && newest > 0 && newest < s->result_seq_seen) {
        kos_log_info(s->rt.log, "order_result_seq_reset", "seen=%lld newest=%lld",
                     (long long)s->result_seq_seen, (long long)newest);
        s->result_seq_seen = 0;
    }
    for (i = 0; i < world->order_result_count && i < KOS_SHM_MAX_ORDER_RESULTS; i++) {
        const kos_order_result *result = &world->order_results[i];

        if (result->seq <= 0 || result->seq <= s->result_seq_seen) {
            continue;
        }
        /* 上一局残留的结果：游戏时钟比本帧还晚，/ 或已经过去很久。 */
        if (result->game_time_ms > world->game_time_ms + 500) {
            continue;
        }
        handle_order_result(s, result, now);
    }
    if (newest > s->result_seq_seen) {
        s->result_seq_seen = newest;
    }
    s->result_seq_valid = 1;
}

/*
 * 订单从世界状态里消失且没等到结果事件时的兜底（协议字段还没上报的老 Bridge、
 * 或结果丢帧）。全部工序 DONE 时看分数增量：涨了算交付，其余都算过期——宁可
 * 少记一单完成，也不把扣分记成完成（G9）。等 1.5 s 是为了让分数/结果同一帧
 * 之后到齐。
 */
static void reconcile_world_orders(scheduler *s, const kos_world_state *world, int64_t now)
{
    int i;

    if (world->chef_count == 0) {
        return; /* 插件刚连上/不在关卡：空帧不是"订单过期" */
    }
    for (i = 0; i < s->order_count; i++) {
        sched_order *order = &s->orders[i];
        int64_t delta;

        if (order->status != KOS_ORDER_ACTIVE || !order->from_world) {
            continue;
        }
        if (world_order_present(world, order->id)) {
            order->seen_in_world = 1;
            order->outcome_check_ms = 0;
            continue;
        }
        if (!order->seen_in_world) {
            continue;
        }
        if (order->outcome_check_ms == 0) {
            order->outcome_check_ms = now + SCHED_OUTCOME_GRACE_MS;
            continue;
        }
        if (now < order->outcome_check_ms) {
            continue;
        }
        delta = world->score - order->score_anchor;
        if (order_has_done_all(order) && delta > 0) {
            order->outcome = SCHED_OUTCOME_DELIVERED;
            order->reward_observed = delta;
            finish_order_with_outcome(s, i, KOS_ORDER_COMPLETED, "world-score", now);
        } else {
            order->outcome = SCHED_OUTCOME_EXPIRED;
            finish_order_with_outcome(s, i, KOS_ORDER_EXPIRED, "world-order-gone", now);
            salvage_expired_holdings(s, order, now);
        }
    }
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
    for (i = 0; i < (int)world->order_count && i < KOS_SHM_MAX_ORDERS; i++) {
        const kos_order *order = &world->orders[i];
        int64_t deadline;

        if (order->id[0] == '\0' || order_find(s, order->id) >= 0) {
            continue;
        }
        deadline = now + (order->time_remaining > 0 ? order->time_remaining * 1000LL : 30000);
        if (add_order(s, order->id, order->recipe, now, deadline, order->score, 1, now) ==
            KOS_OK) {
            int index = order_find(s, order->id);

            if (index >= 0) {
                s->orders[index].score_anchor = world->score;
            }
            kos_log_info(s->rt.log, "world_order",
                         "id=%s dish=%s time_remaining=%d reward=%d", order->id,
                         order->recipe, order->time_remaining,
                         index >= 0 ? s->orders[index].reward : 0);
        }
    }
    poll_order_results(s, world, now);
    reconcile_world_orders(s, world, now);
}

/* ============================================================== 事件处理 */

static void finish_order_if_done(scheduler *s, int order_index)
{
    sched_order *order = &s->orders[order_index];

    if (order->status != KOS_ORDER_ACTIVE || !order_has_done_all(order)) {
        return;
    }
    /* world 订单的 DAG 走完只说明动作都做了；完成/过期以游戏结果为准（G9）。 */
    if (order->from_world && order->outcome != SCHED_OUTCOME_DELIVERED) {
        if (!order->outcome_pending_logged) {
            json_writer w;
            char line[SCHED_LINE_MAX];

            order->outcome_pending_logged = 1;
            kos_log_info(s->rt.log, "order_outcome_pending",
                         "id=%s dish=%s outcome=%d", order->id, order->dish,
                         order->outcome);
            json_writer_init(&w, line, sizeof line);
            event_common(&w, s, "order_outcome_pending");
            json_writer_key(&w, "order_id");
            json_writer_string(&w, order->id);
            event_end(s, &w);
        }
        return;
    }
    finish_order_with_outcome(s, order_index, KOS_ORDER_COMPLETED,
                              order->from_world ? "world-score" : "dag", now_ms());
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

static void emit_recipe_infeasible(scheduler *s, const sched_order *order,
                                   const sched_task *task, const char *reason,
                                   const char *detail)
{
    json_writer w;
    char line[SCHED_LINE_MAX];

    kos_log_warn(s->rt.log, "recipe_infeasible", "order=%s node=%s reason=%s detail=%s",
                 order->id, s->recipes.nodes[task->node_index].id, reason, detail);
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, "recipe_infeasible");
    json_writer_key(&w, "order_id");
    json_writer_string(&w, order->id);
    json_writer_key(&w, "node");
    json_writer_string(&w, s->recipes.nodes[task->node_index].id);
    json_writer_key(&w, "reason");
    json_writer_string(&w, reason);
    json_writer_key(&w, "detail");
    json_writer_string(&w, detail != NULL ? detail : "");
    event_end(s, &w);
}

/* P2 的失败分流。detail 由 macro.c 的 fail_step 写成
 * "macro=<动作> wait=<条件> <原因> waited_ms=<n>"，动作与原因共同决定处理方式。 */
enum {
    SCHED_FAIL_REQUEUE = 0, /* 走位问题/世界变了：回队再派 */
    SCHED_FAIL_INFEASIBLE,  /* 已到站而游戏不收：工序图不可行，当单失败 */
    SCHED_FAIL_ITEM_LOST,   /* 手上的东西没了 */
    SCHED_FAIL_EXPIRED,     /* 订单在交付前消失 */
    SCHED_FAIL_ORDER        /* 宏映射等硬错误 */
};

static int macro_fail_disposition(const char *detail)
{
    if (detail == NULL) {
        return SCHED_FAIL_ORDER;
    }
    if (strstr(detail, "wait=order-gone") != NULL && strstr(detail, "expired") != NULL) {
        return SCHED_FAIL_EXPIRED;
    }
    if (strstr(detail, "holding-lost") != NULL) {
        return SCHED_FAIL_ITEM_LOST;
    }
    /* 没到站：走位/让路问题，重试有意义。 */
    if (strstr(detail, "macro=goto") != NULL) {
        return SCHED_FAIL_REQUEUE;
    }
    /* 明确的模型/内容错误：重试只会重复同样的动作。 */
    if (strstr(detail, "no-station-resource") != NULL ||
        strstr(detail, "burning") != NULL ||
        strstr(detail, "content-mismatch") != NULL ||
        strstr(detail, "unknown-kind") != NULL) {
        return SCHED_FAIL_INFEASIBLE;
    }
    /* 工位上的东西被拿走了：世界变了，重试可能拿到另一份。 */
    if (strstr(detail, "source-gone") != NULL || strstr(detail, "content-gone") != NULL) {
        return SCHED_FAIL_REQUEUE;
    }
    /* 派到时厨师手上还有东西（上一单的盘子/原料没放下）：换个人再派，不是这道菜做不了。
     * 实机 20261027-232216 order_004/005 的 fetch:Rice 因此直接判单失败，米饭锅烧糊。 */
    if (strstr(detail, "hand-occupied") != NULL) {
        return SCHED_FAIL_REQUEUE;
    }
    /* 到站后等到超时：实机 20261027-190659 里 chop/drop 超时多半是焦点或板上残留这类瞬时状态
     * （scheduler.log:80/88/351 三单因此直接判死，锅留在火上烧糊）。按重试走退避，
     * 连续 SCHED_TASK_MAX_RETRIES 次才判这道菜失败。 */
    if (strstr(detail, "timeout") != NULL) {
        return SCHED_FAIL_REQUEUE;
    }
    /* 到站但游戏选中的交互对象一直不是目标（另一个厨师挡在面前、站位格选错）：
     * 和超时一样是瞬时状态。实机 20261027-200237 scheduler.log:55 出菜口
     * deliver 因 focus 直接判单失败，厨师端着菜站到收工。 */
    if (strstr(detail, " focus ") != NULL) {
        return SCHED_FAIL_REQUEUE;
    }
    return SCHED_FAIL_ORDER;
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
    if (s->putdown.state == 2 && msg->task_id != 0 && msg->task_id == s->putdown.task_id) {
        if (msg->chef_id >= 0 && msg->chef_id < KOS_SHM_MAX_CHEFS &&
            s->running_by_chef[msg->chef_id] == msg->task_id) {
            s->running_by_chef[msg->chef_id] = 0;
        }
        if (msg->event != KOS_EVT_ACCEPTED) {
            const char *event_name =
                msg->event == KOS_EVT_DONE ? "putdown_done" : "putdown_failed";

            (void)release_lease_for_task(s, msg->task_id, "resource_reclaimed",
                                         msg->event == KOS_EVT_DONE ? "putdown-done"
                                                                    : "putdown-failed");
            kos_log_info(s->rt.log, event_name, "task=%llu chef=%d detail=%s",
                         (unsigned long long)msg->task_id, msg->chef_id, msg->detail);
            json_writer_init(&w, line, sizeof line);
            event_common(&w, s, event_name);
            json_writer_key(&w, "task_id");
            json_writer_int(&w, (long long)msg->task_id);
            json_writer_key(&w, "chef_id");
            json_writer_int(&w, msg->chef_id);
            json_writer_key(&w, "item");
            json_writer_string(&w, s->putdown.item);
            event_end(s, &w);
            if (msg->chef_id >= 0 && msg->chef_id < KOS_SHM_MAX_CHEFS) {
                if (msg->event == KOS_EVT_DONE) {
                    s->putdown_skip[msg->chef_id] = 0;
                } else {
                    /* 放失败了东西还在手上（实机 20261027-212429：锅放不到远处的
                     * counter-0，之后再没人管，端着空锅走一整局）。记下失败的台面，
                     * 交给 hold-stuck-retry 换最近的另一个台面重排；手空了自然解除。 */
                    s->putdown_skip[msg->chef_id] = s->putdown.station_index + 1;
                    s->hold_stuck[msg->chef_id] = 1;
                }
            }
            memset(&s->putdown, 0, sizeof s->putdown);
        }
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
    if (order->status != KOS_ORDER_ACTIVE) {
        /* 订单已收尾（结果/过期先到）：只把租约还掉，不再改订单状态。 */
        (void)release_lease_for_task(s, msg->task_id, "resource_reclaimed", "order-closed");
        if (msg->event == KOS_EVT_DONE) {
            task->state = TASK_DONE;
            task->done_ms = now_ms();
            if (op_leaves_item_in_hand(s->recipes.nodes[task->node_index].op)) {
                salvage_chef_holding(s, msg->chef_id, "order-closed", now_ms());
            }
        } else {
            task->state = TASK_FAILED;
        }
        return;
    }
    {
        int node = task->node_index;
        int64_t now = now_ms();
        const char *op = s->recipes.nodes[node].op;

        if (msg->event == KOS_EVT_DONE) {
            if (order->status == KOS_ORDER_ACTIVE &&
                strcmp(op, "cook") == 0 &&
                (cook_has_pour_successor(s, node) ||
                 cook_has_plate_successor(s, node))) {
                task->cooker_reserved = detach_lease_keep_token(s, msg->task_id);
                if (!task->cooker_reserved) {
                    (void)release_lease_for_task(s, msg->task_id, "resource_reclaimed",
                                                 "reservation-transfer-failed");
                    task->state = TASK_FAILED;
                    finish_order_with_outcome(s, order_index, KOS_ORDER_FAILED,
                                              "reservation-transfer-failed", now);
                    return;
                }
                task->cooker_reserved_ms = now;
            } else {
                (void)release_lease_for_task(s, msg->task_id, "resource_released", "task-done");
            }
            task->state = TASK_DONE;
            task->done_ms = now;
            task->not_before_ms = 0;
            if (strcmp(op, "pour") == 0 || strcmp(op, "plate") == 0) {
                release_pour_source(s, order, &s->recipes.nodes[node]);
            }
            if (strcmp(op, "plate") == 0) {
                release_plate_source(s, order, &s->recipes.nodes[node]);
            }
            if (strcmp(op, "deliver") == 0) {
                const kos_world_state *world = scheduler_world(s, now);

                if (order->from_world && world != NULL && task->score_base_valid) {
                    int64_t delta = world->score - task->score_base;

                    if (delta > 0) {
                        order->outcome = SCHED_OUTCOME_DELIVERED;
                        order->reward_observed = delta;
                    } else if (order->outcome_check_ms == 0) {
                        /* 分数还没记上（或本局没上报）：等结果事件/分数变化再判，
                         * 不能在这里把一单没记分的交付当完成（G9）。 */
                        order->outcome_check_ms = now + SCHED_OUTCOME_GRACE_MS;
                    }
                }
            }
            finish_order_if_done(s, order_index);
        } else if (msg->event == KOS_EVT_BLOCKED) {
            /* 走位连续 3 次卡住：租约还给资源模型，工序回就绪队列，并记住
             * 别立刻又派给同一个人（他那里仍然走不通）。原料在他手里时别人做不了，
             * 不避开也不扣留，让他原地重试。 */
            int blocked_chef = msg->chef_id;
            int task_index = (int)(task - order->tasks);
            int holder = task_holder_chef(s, order, task_index);

            (void)release_lease_for_task(s, msg->task_id, "resource_reclaimed",
                                         "task-blocked");
            task_requeue(s, order_index, task_index, "worker-blocked");
            if (holder < 0 || holder != blocked_chef) {
                if (blocked_chef >= 0 && blocked_chef < KOS_SHM_MAX_CHEFS) {
                    s->blocked_hold[blocked_chef] = 1;
                }
                order->tasks[task_index].avoid_chef = blocked_chef;
            }
        } else if (strncmp(msg->detail, "macro=", 6) == 0) {
            int task_index = (int)(task - order->tasks);
            int disposition = macro_fail_disposition(msg->detail);

            (void)release_lease_for_task(s, msg->task_id, "resource_reclaimed",
                                         "task-failed");
            if (disposition == SCHED_FAIL_EXPIRED) {
                order->outcome = SCHED_OUTCOME_EXPIRED;
                task->state = TASK_FAILED;
                finish_order_with_outcome(s, order_index, KOS_ORDER_EXPIRED,
                                          "deliver-expired", now);
                /* 交付的菜还在这个厨师手上，给同菜的另一单或放下。 */
                salvage_chef_holding(s, msg->chef_id, "order-expired", now);
            } else if (disposition == SCHED_FAIL_ITEM_LOST) {
                task->state = TASK_FAILED;
                kos_log_warn(s->rt.log, "task_item_lost", "order=%s node=%s detail=%s",
                             order->id, s->recipes.nodes[node].id, msg->detail);
                finish_order_with_outcome(s, order_index, KOS_ORDER_FAILED, "item-lost",
                                          now);
            } else if (disposition == SCHED_FAIL_REQUEUE) {
                task->fail_count++;
                if (task->fail_count < SCHED_TASK_MAX_RETRIES) {
                    task_requeue(s, order_index, task_index, "macro-failed");
                    /* 手上有东西接不了：下一轮换个空手的人。 */
                    if (strstr(msg->detail, "hand-occupied") != NULL) {
                        task->avoid_chef = msg->chef_id;
                    }
                    /* 台面放不下（上面还躺着失败订单留下的玉米饼）：重派换一张。 */
                    if (strcmp(op, "place") == 0) {
                        task->station_id[0] = '\0';
                    }
                    /* 倒料不退避：锅熟后约 12 s 就焦，2 s 退避期间空闲厨师会去拿别的活
                     * （实机 20261028-120417 米饭锅 goto 超时后退避，锅差点烧）。 */
                    task->not_before_ms =
                        strcmp(op, "pour") == 0
                            ? now
                            : now + (int64_t)task->fail_count * SCHED_TASK_RETRY_BACKOFF_MS;
                    kos_log_info(s->rt.log, "task_retry_backoff",
                                 "order=%s node=%s attempt=%d not_before_ms=%lld",
                                 order->id, s->recipes.nodes[node].id, task->fail_count,
                                 (long long)(task->not_before_ms - now));
                } else {
                    task->state = TASK_FAILED;
                    emit_recipe_infeasible(s, order, task, "macro-retries-exhausted",
                                           msg->detail);
                    finish_order_with_outcome(s, order_index, KOS_ORDER_FAILED,
                                              "macro-retries-exhausted", now);
                    salvage_expired_holdings(s, order, now);
                }
            } else if (disposition == SCHED_FAIL_INFEASIBLE) {
                task->state = TASK_FAILED;
                emit_recipe_infeasible(s, order, task, "game-refused", msg->detail);
                finish_order_with_outcome(s, order_index, KOS_ORDER_FAILED,
                                          "recipe-infeasible", now);
                salvage_expired_holdings(s, order, now);
            } else {
                task->state = TASK_FAILED;
                kos_log_warn(s->rt.log, "task_failed", "order=%s node=%s detail=%s",
                             order->id, s->recipes.nodes[node].id, msg->detail);
                finish_order_with_outcome(s, order_index, KOS_ORDER_FAILED, "macro-error",
                                          now);
                salvage_expired_holdings(s, order, now);
            }
        } else {
            (void)release_lease_for_task(s, msg->task_id, "resource_reclaimed",
                                         "task-failed");
            task->state = TASK_FAILED;
            kos_log_warn(s->rt.log, "task_failed", "order=%s node=%s detail=%s",
                         order->id, s->recipes.nodes[node].id, msg->detail);
            finish_order_with_outcome(s, order_index, KOS_ORDER_FAILED, "task-failed", now);
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
    if (cand->order_index == -2) {
        return SCHED_PUTDOWN_MS;
    }
    if (cand->order_index < 0) {
        return (int)(s->assign_cfg.wash_ms + 0.5);
    }
    return s->recipes.nodes[cand->node_index].duration_ms;
}

static const char *candidate_resource(const scheduler *s, const sched_candidate *cand)
{
    if (cand->order_index < 0) {
        return s->kitchen.stations[cand->station_index].kind;
    }
    {
        const kos_recipe_node *node = &s->recipes.nodes[cand->node_index];

        if (strcmp(node->op, "plate") == 0 &&
            kos_kitchen_count_kind(&s->kitchen, node->resource) == 0) {
            return s->kitchen.stations[cand->station_index].kind;
        }
        return node->resource;
    }
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
/* 工位 id 末尾的序号（cooker-4 → 4）；没有数字返回 -1。 */
static int station_ordinal(const char *id)
{
    const char *p;
    const char *digits = NULL;
    int value = 0;
    int count = 0;

    if (id == NULL) {
        return -1;
    }
    for (p = id; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            if (digits == NULL) {
                digits = p;
            }
            value = value * 10 + (*p - '0');
            count++;
        } else {
            digits = NULL;
            value = 0;
            count = 0;
        }
    }
    return count > 0 ? value : -1;
}

/*
 * 工位 id → 世界状态里的资源（cooker-4 ↔ pot_4、board-1 ↔ cutting_board_1、
 * sink-0 ↔ sink_0）。G1 契约下先按位置认（世界状态里的 pos 与工位 pos 对齐，
 * 容差 0.3 m），再退回精确名与序号归一名。
 */
static const kos_resource *station_world_resource(const scheduler *s, const char *station_id)
{
    int i;
    int ordinal;
    char name[KOS_SHM_NAME_MAX];
    const char *prefix = NULL;
    int station_index;

    if (station_id == NULL || station_id[0] == '\0' || !s->world_valid) {
        return NULL;
    }
    station_index = kos_kitchen_station_index(&s->kitchen, station_id);
    if (station_index >= 0) {
        const kos_station *station = &s->kitchen.stations[station_index];
        const kos_resource *nearest = NULL;
        float nearest_d2 = 0.3f * 0.3f;

        for (i = 0; i < (int)s->world.resource_count && i < KOS_SHM_MAX_RESOURCES; i++) {
            float dx, dy, d2;

            if (!s->world.resources[i].has_pos) {
                continue;
            }
            if (s->world.resources[i].kind[0] != '\0' &&
                strcmp(s->world.resources[i].kind, station->kind) != 0) {
                continue;
            }
            dx = s->world.resources[i].pos_x - station->x;
            dy = s->world.resources[i].pos_y - station->z;
            d2 = dx * dx + dy * dy;
            if (d2 <= nearest_d2) {
                nearest_d2 = d2;
                nearest = &s->world.resources[i];
            }
        }
        if (nearest != NULL) {
            return nearest;
        }
    }
    for (i = 0; i < (int)s->world.resource_count && i < KOS_SHM_MAX_RESOURCES; i++) {
        if (strcmp(s->world.resources[i].name, station_id) == 0) {
            return &s->world.resources[i];
        }
    }
    ordinal = station_ordinal(station_id);
    if (ordinal < 0) {
        return NULL;
    }
    if (strncmp(station_id, "cooker", 6) == 0) {
        prefix = "pot";
    } else if (strncmp(station_id, "board", 5) == 0) {
        prefix = "cutting_board";
    } else if (strncmp(station_id, "sink", 4) == 0) {
        prefix = "sink";
    }
    if (prefix == NULL) {
        return NULL;
    }
    snprintf(name, sizeof name, "%s_%d", prefix, ordinal);
    for (i = 0; i < (int)s->world.resource_count && i < KOS_SHM_MAX_RESOURCES; i++) {
        if (strcmp(s->world.resources[i].name, name) == 0) {
            return &s->world.resources[i];
        }
    }
    return NULL;
}

/*
 * 工位上还有东西（锅里有料/正在煮、板上放着料）。新工序不能选它：
 * 游戏会以 pot-occupied/board-occupied 拒绝（demo3 实机与 sim 都复现）。
 * 世界状态读不到时不拦（老 Bridge 没有这些字段）。
 */
static int world_station_busy(const scheduler *s, const char *station_id)
{
    const kos_resource *resource = station_world_resource(s, station_id);

    if (resource == NULL) {
        return 0;
    }
    if (resource->state[0] == '\0' || strcmp(resource->state, "free") == 0 ||
        strcmp(resource->state, "empty") == 0) {
        return 0;
    }
    if (resource->item[0] != '\0' || resource->content[0] != '\0' ||
        resource->content_count > 0) {
        return 1;
    }
    return strcmp(resource->state, "cooking") == 0 ||
           strcmp(resource->state, "cooked") == 0 ||
           strcmp(resource->state, "burning") == 0 ||
           strcmp(resource->state, "occupied") == 0;
}

/* 砧板/搅拌台按运行中的工序占用；产出的料被拿走（任务 DONE）就空出来。 */
static int board_station_reserved(const scheduler *s, const char *station_id)
{
    int i;
    int j;

    for (i = 0; i < s->order_count; i++) {
        const sched_order *order = &s->orders[i];

        for (j = 0; j < order->task_count; j++) {
            const sched_task *task = &order->tasks[j];
            const kos_recipe_node *node = &s->recipes.nodes[task->node_index];

            if ((strcmp(node->op, "cut") == 0 || strcmp(node->op, "mix") == 0) &&
                task->station_id[0] != '\0' &&
                strcmp(task->station_id, station_id) == 0 &&
                task->state == TASK_RUNNING) {
                return 1;
            }
        }
    }
    return 0;
}

/* 盘堆一次只给一个装盘厨师用：两个人同时到盘堆放料/取盘会互相抢盘子、在站位格撞在一起
 * （实机 _3：两个装盘任务同时到 plate_stack-0，都卡住）。失败回队、等重试的装盘也占着：
 * 它的料已经放在盘堆上，别人此时来装盘会撞上还站在那儿的厨师（20261027-232216）。
 * 装好盘、还没出餐的那单也占着：盘堆到出菜口是一条单行道，像栈一样后进的人
 * 会把先进的人堵在里面（实机 20261028-063515 装盘与出餐对头卡死）。
 * `self` 是正在问的那道工序，自己不挡自己。 */
static int plate_station_reserved(const scheduler *s, const char *station_id,
                                  const sched_task *self)
{
    int i;
    int j;

    for (i = 0; i < s->order_count; i++) {
        const sched_order *order = &s->orders[i];

        if (order->status != KOS_ORDER_ACTIVE) {
            continue;
        }
        int plated = 0;
        int delivered = 1;

        for (j = 0; j < order->task_count; j++) {
            const sched_task *task = &order->tasks[j];
            const kos_recipe_node *node = &s->recipes.nodes[task->node_index];
            int holds = task->state == TASK_RUNNING ||
                        (task->state == TASK_PENDING && task->fail_count > 0);

            if (strcmp(node->op, "plate") == 0 && task->station_id[0] != '\0' &&
                strcmp(task->station_id, station_id) == 0) {
                if (task != self && holds) {
                    return 1;
                }
                if (task->state == TASK_DONE) {
                    plated = 1;
                }
            }
            if (strcmp(node->op, "deliver") == 0 && task->state != TASK_DONE &&
                task->state != TASK_FAILED) {
                delivered = 0;
            }
        }
        if (plated && !delivered) {
            return 1;
        }
    }
    return 0;
}

static int pick_board_station(const scheduler *s, const char *kind, unsigned salt,
                              int *index)
{
    int candidates[KOS_KITCHEN_MAX_STATIONS];
    int count = 0;
    int i;
    int offset;

    for (i = 0; i < s->kitchen.station_count; i++) {
        if (strcmp(s->kitchen.stations[i].kind, kind) == 0) {
            candidates[count++] = i;
        }
    }
    if (count == 0) {
        return KOS_ERR_MISSING;
    }
    for (offset = 0; offset < count; offset++) {
        int candidate = candidates[(salt + (unsigned)offset) % (unsigned)count];
        const kos_station *station = &s->kitchen.stations[candidate];

        if (board_station_reserved(s, station->id) ||
            world_station_busy(s, station->id)) {
            continue;
        }
        *index = candidate;
        return KOS_OK;
    }
    return KOS_ERR_EMPTY;
}

/* 切失败回队的 cut：原料已经放在上次那块板上（世界状态 occupied+item），
 * world_station_busy 会让它永远选不到板而 resource_wait（实机 20261027-234924 切鸡肉 2042 次）。
 * 板上正是这道工序的料、且没别的切/搅工序在跑，就钉回那块板：drop 手空跳过，直接 chop。 */
static int cut_retry_station(const scheduler *s, const sched_task *task,
                             const kos_recipe_node *node, int *index)
{
    const kos_resource *resource;
    int idx;

    const char *want = node->ingredient[0] != '\0' ? node->ingredient : node->label;

    /* recipes.json 的 cut 节点不带 ingredient，原料名就是 label（cut:BurritoChicken）。
     * 旧代码只认 ingredient，这条重试复用从来没生效过。 */
    if (task->fail_count <= 0 || task->station_id[0] == '\0' || want[0] == '\0') {
        return KOS_ERR_EMPTY;
    }
    idx = kos_kitchen_station_index(&s->kitchen, task->station_id);
    if (idx < 0 || strcmp(s->kitchen.stations[idx].kind, node->resource) != 0 ||
        board_station_reserved(s, task->station_id)) {
        return KOS_ERR_EMPTY;
    }
    resource = station_world_resource(s, task->station_id);
    if (resource == NULL ||
        !(sched_name_eq(resource->item, want) || sched_name_eq(resource->content, want))) {
        return KOS_ERR_EMPTY;
    }
    *index = idx;
    return KOS_OK;
}

static int cooker_station_reserved(const scheduler *s, const char *station_id)
{
    int i;
    int j;

    for (i = 0; i < s->order_count; i++) {
        const sched_order *order = &s->orders[i];

        for (j = 0; j < order->task_count; j++) {
            const sched_task *task = &order->tasks[j];
            const kos_recipe_node *node = &s->recipes.nodes[task->node_index];

            if (strcmp(node->op, "cook") == 0 && task->station_id[0] != '\0' &&
                strcmp(task->station_id, station_id) == 0 &&
                (task->state == TASK_RUNNING || task->cooker_reserved)) {
                return 1;
            }
        }
    }
    return 0;
}

/* 游戏只让 AllowedCookingSteps 含锅的烹饪类型的料放进去（CookableContainer.AllowItemPlacement）：
 * 节点或工位任一方没写 step 时不限，两边都写了就必须相同（实机 `_3`：米被派到炸锅放不进去）。 */
static int cooker_step_matches(const kos_station *station, const char *step)
{
    return step == NULL || step[0] == '\0' || station->step[0] == '\0' ||
           strcmp(station->step, step) == 0;
}

/* 灶台按资源 kind 找，不写死 "cooker"：kind 由 kitchen.json 决定（测试夹具会给
 * 同名 kind 加进程后缀避免信号量撞名），配方里 cook 节点的 resource 与它一致。 */
static int pick_cooker_station(const scheduler *s, const char *kind, const char *step,
                               unsigned salt, int *index)
{
    int offset;
    int matching = 0;

    for (offset = 0; offset < s->kitchen.station_count; offset++) {
        int candidate = (int)((salt + (unsigned)offset) % (unsigned)s->kitchen.station_count);
        const kos_station *station = &s->kitchen.stations[candidate];

        if (strcmp(station->kind, kind) != 0 || !cooker_step_matches(station, step)) {
            continue;
        }
        matching++;
        if (!cooker_station_reserved(s, station->id) &&
            !world_station_busy(s, station->id)) {
            *index = candidate;
            return KOS_OK;
        }
    }
    return matching > 0 ? KOS_ERR_EMPTY : KOS_ERR_MISSING;
}
static void release_plate_source(scheduler *s, sched_order *order,
                                 const kos_recipe_node *plate_node)
{
    int cook_task_index = plate_cook_task(s, order,
                                          (int)(plate_node - s->recipes.nodes));
    sched_task *cook_task;

    if (cook_task_index < 0) {
        return;
    }
    cook_task = &order->tasks[cook_task_index];
    if (cook_task->cooker_reserved) {
        (void)kos_resource_release(&s->resources, "cooker");
        cook_task->cooker_reserved = 0;
        cook_task->station_id[0] = '\0';
    }
}

static int cook_has_plate_successor(const scheduler *s, int cook_node)
{
    int pending[KOS_RECIPE_MAX_NODES];
    unsigned char seen[KOS_RECIPE_MAX_NODES] = {0};
    int count = 0;

    pending[count++] = cook_node;
    seen[cook_node] = 1;
    while (count > 0) {
        int current = pending[--count];
        int edge;

        for (edge = 0; edge < s->recipes.edge_count; edge++) {
            int parent = s->recipes.edges[edge].to;

            if (s->recipes.edges[edge].from != current || parent < 0 ||
                parent >= s->recipes.node_count || seen[parent]) {
                continue;
            }
            if (strcmp(s->recipes.nodes[parent].op, "plate") == 0) {
                return 1;
            }
            seen[parent] = 1;
            if (count < KOS_RECIPE_MAX_NODES) {
                pending[count++] = parent;
            }
        }
    }
    return 0;
}

/* Find the unique reserved cook ancestor of a plate node; -1 means a cold plate. */
static int plate_cook_task(const scheduler *s, const sched_order *order, int plate_node)
{
    int pending[KOS_RECIPE_MAX_NODES];
    unsigned char seen[KOS_RECIPE_MAX_NODES] = {0};
    int count = 0;
    int found = -1;

    pending[count++] = plate_node;
    while (count > 0) {
        int current = pending[--count];
        int edge;

        for (edge = 0; edge < s->recipes.edge_count; edge++) {
            const kos_recipe_edge *recipe_edge = &s->recipes.edges[edge];
            int task_index;
            int ancestor;

            if (recipe_edge->to != current || recipe_edge->from < 0 ||
                recipe_edge->from >= s->recipes.node_count || seen[recipe_edge->from]) {
                continue;
            }
            seen[recipe_edge->from] = 1;
            task_index = order_task_by_node(order, recipe_edge->from);
            if (task_index < 0) {
                continue;
            }
            ancestor = order->tasks[task_index].node_index;
            if (strcmp(s->recipes.nodes[ancestor].op, "cook") == 0 &&
                order->tasks[task_index].cooker_reserved) {
                if (found >= 0 && found != task_index) {
                    return -2;
                }
                found = task_index;
            } else if (count < KOS_RECIPE_MAX_NODES) {
                pending[count++] = ancestor;
            }
        }
    }
    return found;
}


/*
 * cook 做完后把"锅"留住（令牌不还），留给倒料工序端走；倒料完成再还。
 * 只有 cook 的直接后继里真有 pour 才留，否则锅会一直被占住。
 */
static int cook_has_pour_successor(const scheduler *s, int cook_node)
{
    int edge;

    for (edge = 0; edge < s->recipes.edge_count; edge++) {
        int parent = s->recipes.edges[edge].to;

        if (s->recipes.edges[edge].from != cook_node || parent < 0 ||
            parent >= s->recipes.node_count) {
            continue;
        }
        if (strcmp(s->recipes.nodes[parent].op, "pour") == 0 ||
            strcmp(s->recipes.nodes[parent].op, "plate") == 0) {
            return 1;
        }
    }
    return 0;
}

/* pour 的源锅：它那个还占着锅的 cook 直接子工序。0 个 = -1，多个 = -2。 */
static int pour_source_cook_task(const scheduler *s, const sched_order *order, int pour_node)
{
    int found = -1;
    int edge;

    for (edge = 0; edge < s->recipes.edge_count; edge++) {
        const kos_recipe_edge *recipe_edge = &s->recipes.edges[edge];
        int task_index;
        const kos_recipe_node *child;

        if (recipe_edge->to != pour_node || recipe_edge->from < 0 ||
            recipe_edge->from >= s->recipes.node_count) {
            continue;
        }
        task_index = order_task_by_node(order, recipe_edge->from);
        if (task_index < 0) {
            continue;
        }
        child = &s->recipes.nodes[order->tasks[task_index].node_index];
        if (strcmp(child->op, "cook") == 0 && order->tasks[task_index].cooker_reserved) {
            if (found >= 0 && found != task_index) {
                return -2;
            }
            found = task_index;
        }
    }
    return found;
}

static void release_pour_source(scheduler *s, sched_order *order,
                                const kos_recipe_node *pour_node)
{
    int cook_task_index = pour_source_cook_task(s, order,
                                                (int)(pour_node - s->recipes.nodes));
    sched_task *cook_task;

    if (cook_task_index < 0) {
        return;
    }
    cook_task = &order->tasks[cook_task_index];
    if (cook_task->cooker_reserved) {
        const kos_recipe_node *cook_node = &s->recipes.nodes[cook_task->node_index];
        (void)kos_resource_release(&s->resources, cook_node->resource);
        cook_task->cooker_reserved = 0;
        cook_task->station_id[0] = '\0';
    }
}

/*
 * 容器工位（玉米饼所在的那张台面）：place 工序把它放在哪，后续 pour / pickup 就得回哪。
 * 资源令牌只按 kind 计数，不区分具体台面，所以位置必须从 place 任务上带过来。
 */
static int container_station_from(const scheduler *s, const sched_order *order,
                                   int node_index, int depth, int *station_out)
{
    int edge;

    if (depth <= 0) {
        return KOS_ERR_MISSING;
    }
    for (edge = 0; edge < s->recipes.edge_count; edge++) {
        const kos_recipe_edge *recipe_edge = &s->recipes.edges[edge];
        const kos_recipe_node *parent;
        int task_index;

        if (recipe_edge->to != node_index || recipe_edge->from < 0 ||
            recipe_edge->from >= s->recipes.node_count) {
            continue;
        }
        task_index = order_task_by_node(order, recipe_edge->from);
        if (task_index < 0) {
            continue;
        }
        parent = &s->recipes.nodes[order->tasks[task_index].node_index];
        if (strcmp(parent->op, "place") == 0) {
            int index;

            if (order->tasks[task_index].state != TASK_DONE ||
                order->tasks[task_index].station_id[0] == '\0') {
                continue;
            }
            index = kos_kitchen_station_index(&s->kitchen,
                                              order->tasks[task_index].station_id);
            if (index >= 0) {
                *station_out = index;
                return KOS_OK;
            }
            continue;
        }
        /* 卷饼流程里 place 与 pickup 之间夹着 pour，容器工位要沿着 pour 往上取一层。 */
        if (strcmp(parent->op, "pour") == 0 &&
            container_station_from(s, order, recipe_edge->from, depth - 1, station_out) ==
                KOS_OK) {
            return KOS_OK;
        }
    }
    return KOS_ERR_MISSING;
}

static int container_station_index(const scheduler *s, const sched_order *order, int node_index,
                                   int *station_out)
{
    return container_station_from(s, order, node_index, 8, station_out);
}

/* 这道菜的 `cook` 工序要用哪几种烹饪类型：台面离这些灶越近，倒料来回越短。 */
static int dish_cook_steps(const kos_recipes *recipes, const kos_dish *dish,
                           const char *steps[], int max)
{
    int count = 0;
    int i;
    int j;

    for (i = 0; i < dish->node_count && count < max; i++) {
        const kos_recipe_node *node = &recipes->nodes[dish->nodes[i]];

        if (strcmp(node->op, "cook") != 0 || node->step[0] == '\0') {
            continue;
        }
        for (j = 0; j < count; j++) {
            if (strcmp(steps[j], node->step) == 0) {
                break;
            }
        }
        if (j == count) {
            steps[count++] = node->step;
        }
    }
    return count;
}

/*
 * 卷饼台面：优先离这道菜的灶与盘堆最近的那张，不再按 salt 摊。重派时沿用第一次
 * 挑好的台面——pour/pickup 已经按它记录了容器位置。别的活跃订单占着的台面跳过
 * （一张台面只放一只玉米饼）。
 */
static int place_station_index(const scheduler *s, int order_index, int task_index,
                               int *station_out)
{
    const sched_order *order = &s->orders[order_index];
    const sched_task *task = &order->tasks[task_index];
    unsigned char busy[KOS_KITCHEN_MAX_STATIONS];
    const char *steps[8];
    int step_count;
    int i;
    int j;

    if (task->station_id[0] != '\0') {
        int existing = kos_kitchen_station_index(&s->kitchen, task->station_id);

        if (existing >= 0) {
            *station_out = existing;
            return KOS_OK;
        }
    }
    memset(busy, 0, sizeof busy);
    for (i = 0; i < s->order_count; i++) {
        const sched_order *other = &s->orders[i];

        if (other->status != KOS_ORDER_ACTIVE) {
            continue;
        }
        for (j = 0; j < other->task_count; j++) {
            const sched_task *peer = &other->tasks[j];
            int index;

            if (peer == task || peer->station_id[0] == '\0' ||
                peer->state == TASK_FAILED ||
                strcmp(s->recipes.nodes[peer->node_index].op, "place") != 0) {
                continue;
            }
            index = kos_kitchen_station_index(&s->kitchen, peer->station_id);
            if (index >= 0) {
                busy[index] = 1;
            }
        }
    }
    /* 世界里已经放着东西的台面也跳过：失败订单的玉米饼不会自己消失，
     * 实机 20261027-220205 order_004 往 counter-16 放了三次都放不下。 */
    for (i = 0; i < s->kitchen.station_count && i < KOS_KITCHEN_MAX_STATIONS; i++) {
        if (!busy[i] && strcmp(s->kitchen.stations[i].kind, "counter") == 0 &&
            world_station_busy(s, s->kitchen.stations[i].id)) {
            busy[i] = 1;
        }
    }
    step_count = dish_cook_steps(&s->recipes, &s->recipes.dishes[order->dish_index], steps,
                                 (int)(sizeof steps / sizeof steps[0]));
    return kos_kitchen_pick_counter(&s->kitchen, steps, step_count, busy, station_out);
}


/* pickup 的容器来自 place；卷饼流程里中间隔着 pour，所以按祖先找而不是只看直接父工序。 */
static int recipe_has_place_ancestor(const kos_recipes *recipes, int node_index, int depth)
{
    int edge;

    if (depth <= 0) {
        return 0;
    }
    for (edge = 0; edge < recipes->edge_count; edge++) {
        const kos_recipe_edge *recipe_edge = &recipes->edges[edge];

        if (recipe_edge->to != node_index || recipe_edge->from < 0 ||
            recipe_edge->from >= recipes->node_count) {
            continue;
        }
        if (strcmp(recipes->nodes[recipe_edge->from].op, "place") == 0 ||
            recipe_has_place_ancestor(recipes, recipe_edge->from, depth - 1)) {
            return 1;
        }
    }
    return 0;
}

/* 这道菜的某个节点在这套 kitchen 里有没有落点。返回 NULL = 可行；否则是原因。 */
static const char *node_infeasible_reason(const scheduler *s, const kos_dish *dish, int node_index)
{
    const kos_recipe_node *node = &s->recipes.nodes[node_index];
    int i;

    if (node->resource[0] != '\0' &&
        !(strcmp(node->op, "drop") == 0 ||
          (strcmp(node->op, "fetch") == 0 && strcmp(node->resource, "counter") == 0))) {
        if (strcmp(node->op, "plate") == 0 &&
            kos_kitchen_count_kind(&s->kitchen, "plate_stack") == 0) {
            if (sink_station_index(&s->kitchen) < 0) {
                return "no-sink-for-plate";
            }
        } else if (kos_kitchen_count_kind(&s->kitchen, node->resource) <= 0) {
            return "no-station";
        }
    }
    if (strcmp(node->op, "cook") == 0 && node->step[0] != '\0') {
        int matching = 0;

        for (i = 0; i < s->kitchen.station_count; i++) {
            const kos_station *station = &s->kitchen.stations[i];

            if (strcmp(station->kind, "cooker") == 0 &&
                cooker_step_matches(station, node->step)) {
                matching++;
            }
        }
        if (matching == 0) {
            return "no-cooker-step";
        }
    }
    if (strcmp(node->op, "pour") == 0) {
        int has_cook = 0;

        for (i = 0; i < s->recipes.edge_count; i++) {
            if (s->recipes.edges[i].to != node_index) {
                continue;
            }
            if (strcmp(s->recipes.nodes[s->recipes.edges[i].from].op, "cook") == 0) {
                has_cook = 1;
                break;
            }
        }
        if (!has_cook) {
            return "pour-without-cook";
        }
    }
    if (strcmp(node->op, "pickup") == 0 &&
        !recipe_has_place_ancestor(&s->recipes, node_index, 8)) {
        return "pickup-without-place";
    }
    if (strcmp(node->op, "place") == 0) {
        const char *steps[8];
        int step_count = dish_cook_steps(&s->recipes, dish, steps,
                                        (int)(sizeof steps / sizeof steps[0]));
        int dummy = -1;

        /* 台面存在不等于能用：盘堆/灶底下那张放不了玉米饼，角上走不到的也被导出
         * 丢掉了（scripts/export-kitchen.py 的 drop_stations_without_stand）。 */
        if (kos_kitchen_pick_counter(&s->kitchen, steps, step_count, NULL, &dummy) != KOS_OK) {
            return "no-free-counter";
        }
    }
    return NULL;
}

/*
 * 启动/重读配置时把每道菜的节点对照 kitchen 过一遍：工位类型、锅的烹饪类型、
 * pour 的源锅、pickup 的容器来源。不满足的菜直接标成不可行——派下去只会等超时，
 * 日志里也看不出"这道菜本来就做不出来"。
 */
static void recipe_feasibility_check(scheduler *s)
{
    int d;

    for (d = 0; d < s->recipes.dish_count; d++) {
        const kos_dish *dish = &s->recipes.dishes[d];
        int feasible = 1;
        int i;

        for (i = 0; i < dish->node_count; i++) {
            const char *reason = node_infeasible_reason(s, dish, dish->nodes[i]);

            if (reason != NULL) {
                const kos_recipe_node *node = &s->recipes.nodes[dish->nodes[i]];

                feasible = 0;
                kos_log_error(s->rt.log, "recipe_infeasible",
                              "dish=%s node=%s op=%s resource=%s reason=%s", dish->dish,
                              node->id, node->op, node->resource, reason);
            }
        }
        s->dish_feasible[d] = (unsigned char)feasible;
    }
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
 * 小费倍率要求按开工顺序交付：开工更早的订单快做完时，后面订单不开始装盘
 * （装了盘就直接出餐，见 task_dispatch_ready）。失败/完成的更早订单不再挡路；
 * 剩余关键路径还很长（卡住或刚开工）的更早订单也不挡，免得端着料干等。
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
        if (earlier->open_seq < order->open_seq && !order_has_done_all(earlier) &&
            order_remaining_critical_ms(s, earlier) <= SCHED_DELIVERY_WAIT_MAX_MS) {
            return 0;
        }
    }
    return 1;
}

/*
 * 交付顺序闸门的上游版：更早的订单快做完（会挡装盘）、而这一单剩下的关键路径比它
 * 还短时，先别取新料——取了也只能端着等。实机 20261028-074032：order_007/008 的
 * 鱼和虾切好后被前面三单卷饼挡着装盘，两个厨师各端着 41 s 什么也干不了。
 * 只拦 fetch（开一条新料链）；已经在手里的料照常往下做。
 */
static int order_start_allowed(const scheduler *s, const sched_order *order)
{
    int64_t mine;
    int i;

    if (order->open_seq <= 0) {
        return 1;
    }
    mine = order_remaining_critical_ms(s, order);
    for (i = 0; i < s->order_count; i++) {
        const sched_order *earlier = &s->orders[i];
        int64_t theirs;

        if (earlier == order || earlier->status != KOS_ORDER_ACTIVE || !earlier->opened ||
            earlier->open_seq <= 0 || earlier->open_seq >= order->open_seq ||
            order_has_done_all(earlier)) {
            continue;
        }
        theirs = order_remaining_critical_ms(s, earlier);
        if (theirs <= SCHED_DELIVERY_WAIT_MAX_MS && mine < theirs) {
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

    if (s->zoned && s->zones.from_file &&
        (strcmp(node->op, "drop") == 0 ||
         (strcmp(node->op, "fetch") == 0 && strcmp(node->resource, "counter") == 0))) {
        int idx = kos_kitchen_station_index(&s->kitchen, s->zones.handoff.id);
        if (idx >= 0) {
            *station_out = idx;
            return KOS_OK;
        }
        return KOS_ERR_MISSING;
    }
    if (strcmp(node->op, "cook") == 0) {
        return pick_cooker_station(s, node->resource, node->step,
                                   (unsigned)(order_index * KOS_RECIPE_MAX_DISH_NODES +
                                              task_index),
                                   station_out);
    }
    if (strcmp(node->op, "cut") == 0 || strcmp(node->op, "mix") == 0) {
        /* 砧板/搅拌台和灶一样要占住：同一轮里两条 cut 不能都挑 board-1。 */
        if (cut_retry_station(s, &order->tasks[task_index], node, station_out) == KOS_OK) {
            return KOS_OK;
        }
        return pick_board_station(s, node->resource,
                                  (unsigned)(order_index * KOS_RECIPE_MAX_DISH_NODES +
                                             task_index),
                                  station_out);
    }
    if (strcmp(node->op, "plate") == 0 &&
        kos_kitchen_count_kind(&s->kitchen, node->resource) == 0) {
        int sink;

        if (s->world_valid && plate_count(&s->world, "plate_stack") == 0) {
            return KOS_ERR_EMPTY;
        }
        sink = sink_station_index(&s->kitchen);
        if (sink < 0) {
            return KOS_ERR_MISSING;
        }
        *station_out = sink;
        return KOS_OK;
    }
    if (strcmp(node->op, "place") == 0) {
        return place_station_index(s, order_index, task_index, station_out);
    }
    if (strcmp(node->op, "pour") == 0 || strcmp(node->op, "pickup") == 0) {
        int pinned;

        if (container_station_index(s, order, order->tasks[task_index].node_index,
                                    &pinned) == KOS_OK) {
            *station_out = pinned;
            return KOS_OK;
        }
    }
    return kos_kitchen_pick_station(&s->kitchen, node->resource, hint,
                                    (unsigned)(order_index * KOS_RECIPE_MAX_DISH_NODES +
                                               task_index),
                                    station_out);
}

/* 装盘工位被另一个装盘任务占着（运行中或失败待重试）时，这一轮不派（下轮再试）。 */
static int task_station_index_checked(const scheduler *s, int order_index, int task_index,
                                      int *station_out)
{
    const sched_task *task = &s->orders[order_index].tasks[task_index];
    const kos_recipe_node *node = &s->recipes.nodes[task->node_index];
    int rc = task_station_index(s, order_index, task_index, station_out);

    if (rc == KOS_OK && strcmp(node->op, "plate") == 0 &&
        plate_station_reserved(s, s->kitchen.stations[*station_out].id, task)) {
        return KOS_ERR_EMPTY;
    }
    return rc;
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

/* 锅链上的 fetch/cut/mix 往上找它最终喂给的那道 cook；没有（不在锅链）返回 -1。 */
static int pot_chain_cook_task(const scheduler *s, const sched_order *order, int task_index)
{
    int node = order->tasks[task_index].node_index;
    int hops;

    for (hops = 0; hops < s->recipes.node_count; hops++) {
        int edge;
        int parent = -1;

        if (strcmp(s->recipes.nodes[node].op, "cook") == 0) {
            return order_task_by_node(order, node);
        }
        for (edge = 0; edge < s->recipes.edge_count; edge++) {
            int to = s->recipes.edges[edge].to;

            if (s->recipes.edges[edge].from == node && to >= 0 &&
                to < s->recipes.node_count && s->pot_chain[to]) {
                parent = to;
                break;
            }
        }
        if (parent < 0) {
            return -1;
        }
        node = parent;
    }
    return -1;
}

/* 这道还没下锅的 cook 有没有上游已经开工（料在路上/在手里/在板上）。 */
static int pot_chain_cook_committed(const scheduler *s, const sched_order *order,
                                    int cook_task)
{
    int k;

    if (order->tasks[cook_task].state != TASK_PENDING) {
        return 0;
    }
    for (k = 0; k < order->task_count; k++) {
        const sched_task *task = &order->tasks[k];

        if (k == cook_task || !s->pot_chain[task->node_index] ||
            (task->state != TASK_RUNNING && task->state != TASK_DONE)) {
            continue;
        }
        if (pot_chain_cook_task(s, order, k) == cook_task) {
            return 1;
        }
    }
    return 0;
}

/*
 * 锅链开新料的闸：已经在路上的锅链料（上游开工、还没下锅）每份都要一口空灶，
 * 空灶不够就不再开新的。实机 20261028-071415：灶上全是熟/糊锅等空手倒料，三个厨师
 * 却各自端着生蘑菇/肉/米等灶，没人腾得出手倒，环形等待。这道 cook 自己已经有料
 * 在路上（同一锅的第二份料）不拦。老 Bridge（无世界状态）不拦。
 */
static int pot_chain_start_allowed(const scheduler *s, const sched_order *order,
                                   int task_index)
{
    const kos_recipe_node *cook_node;
    int cook_task;
    int free_cookers = 0;
    int committed = 0;
    int i;
    int j;

    if (!s->world_valid || !s->pot_chain[order->tasks[task_index].node_index]) {
        return 1;
    }
    cook_task = pot_chain_cook_task(s, order, task_index);
    if (cook_task < 0 || cook_task == task_index ||
        order->tasks[cook_task].state != TASK_PENDING ||
        pot_chain_cook_committed(s, order, cook_task)) {
        return 1;
    }
    cook_node = &s->recipes.nodes[order->tasks[cook_task].node_index];
    for (i = 0; i < s->kitchen.station_count; i++) {
        const kos_station *station = &s->kitchen.stations[i];

        if (strcmp(station->kind, cook_node->resource) == 0 &&
            cooker_step_matches(station, cook_node->step) &&
            !cooker_station_reserved(s, station->id) &&
            !world_station_busy(s, station->id)) {
            free_cookers++;
        }
    }
    for (i = 0; i < s->order_count; i++) {
        const sched_order *other = &s->orders[i];

        if (other->status != KOS_ORDER_ACTIVE || !other->opened) {
            continue;
        }
        for (j = 0; j < other->task_count; j++) {
            const kos_recipe_node *node = &s->recipes.nodes[other->tasks[j].node_index];

            if (strcmp(node->op, "cook") == 0 &&
                strcmp(node->resource, cook_node->resource) == 0 &&
                (node->step[0] == '\0' || cook_node->step[0] == '\0' ||
                 strcmp(node->step, cook_node->step) == 0) &&
                pot_chain_cook_committed(s, other, j)) {
                committed++;
            }
        }
    }
    return committed < free_cookers;
}

static int pour_pot_ready(const scheduler *s, const sched_order *order, int task_index)
{
    const kos_resource *resource;
    int cook_task;

    if (!s->world_valid) {
        return 1;
    }
    cook_task = pour_source_cook_task(s, order, order->tasks[task_index].node_index);
    if (cook_task < 0 || order->tasks[cook_task].station_id[0] == '\0') {
        return 1; /* 让派发路径报 pour_source_missing，而不是永远卡在就绪外 */
    }
    /* 锅已端在某人手上（倒料走到一半被卡住回队）：灶位此时报空，按灶位状态会永远
     * 不就绪；锅里有这味料就交给持锅人续做（task_holder_chef 绑定他）。 */
    if (pour_pot_carrier(s, &s->recipes.nodes[order->tasks[task_index].node_index]) >= 0) {
        return 1;
    }
    resource = station_world_resource(s, order->tasks[cook_task].station_id);
    if (resource == NULL || resource->state[0] == '\0') {
        return 1;
    }
    return strcmp(resource->state, "cooked") == 0;
}


static int task_dispatch_ready(const scheduler *s, const sched_order *order, int task_index,
                               int64_t now)
{
    const sched_task *task = &order->tasks[task_index];
    const kos_recipe_node *node;

    if (task->state != TASK_PENDING ||
        !order_node_ready(s, order, task->node_index)) {
        return 0;
    }
    /* P2 退避：同一个工序第 n 次宏失败后 n × 2 s 内不进候选。 */
    if (task->not_before_ms > 0 && now < task->not_before_ms) {
        return 0;
    }
    node = &s->recipes.nodes[task->node_index];
    if (strcmp(node->op, "pour") == 0 && !pour_pot_ready(s, order, task_index)) {
        return 0;
    }
    /* 交付顺序闸门卡在装盘，不卡交付：装好盘的人必须立刻出餐，端着成品停在盘堆
     * 站位格会把装盘/出餐那条单行道堵死（实机 20261028-063515：order_007 装好盘
     * 等 order_006，厨师 0 停在盘堆前，厨师 1 装盘 goto 超时 10.3 s + 7.1 s）。
     * 料备齐后最多等 SCHED_DELIVERY_HOLD_MAX_MS：前面卷饼的剩余关键路径不算等锅/
     * 等人倒料，实机 20261028-080123 一个厨师端着切好的鱼等了 56 s，丢的产能比
     * 小费倍率值钱。 */
    if (strcmp(node->op, "plate") == 0 && !order_delivery_allowed(s, order)) {
        int64_t ready_ms = 0;
        int e;

        for (e = 0; e < s->recipes.edge_count; e++) {
            int child_task;

            if (s->recipes.edges[e].to != task->node_index) {
                continue;
            }
            child_task = order_task_by_node(order, s->recipes.edges[e].from);
            if (child_task >= 0 && order->tasks[child_task].done_ms > ready_ms) {
                ready_ms = order->tasks[child_task].done_ms;
            }
        }
        if (ready_ms == 0 || now - ready_ms < SCHED_DELIVERY_HOLD_MAX_MS) {
            return 0;
        }
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
 * 手里拿着原料的厨师只算进那道工序的匹配（task_holder_chef）。
 */
static int opened_ready_count(scheduler *s, int64_t now, const int *idle,
                              const int *chef_cells, int chef_count,
                              const kos_world_state *world)
{
    int bound[KOS_SHM_MAX_CHEFS];
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

    holding_chefs(s, bound);
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
            if (!task_dispatch_ready(s, order, j, now)) {
                continue;
            }
            if (task_station_index_checked(s, i, j, &station) != KOS_OK) {
                continue;
            }
            if (s->zoned && s->zones.from_file) {
                const kos_recipe_node *rnode = &s->recipes.nodes[order->tasks[j].node_index];
                int cand_zone = -1;
                if (strcmp(rnode->op, "drop") == 0 ||
                    (strcmp(rnode->op, "fetch") == 0 && strcmp(rnode->resource, "counter") == 0)) {
                    cand_zone = rnode->zone;
                } else {
                    cand_zone = kos_zones_station_zone(&s->zones, s->kitchen.stations[station].id);
                }
                for (k = 0; k < KOS_ASSIGN_MAX_CHEFS && k < chef_count; k++) {
                    int chef_zone = kos_zones_chef_zone(&s->zones, k);
                    if (chef_zone >= 0 && cand_zone >= 0 && chef_zone != cand_zone) {
                        continue;
                    }
                    if (chef_cells[k] < 0 ||
                        kos_stand_distance(&s->kitchen, station, chef_cells[k]) !=
                            KOS_STAND_UNREACHABLE) {
                        mask |= 1 << k;
                    }
                }
            } else {
                for (k = 0; k < KOS_ASSIGN_MAX_CHEFS && k < chef_count; k++) {
                    if (chef_cells[k] < 0 ||
                        kos_stand_distance(&s->kitchen, station, chef_cells[k]) !=
                            KOS_STAND_UNREACHABLE) {
                        mask |= 1 << k;
                    }
                }
            }
            {
                int holder = task_holder_chef(s, order, j);
                const kos_recipe_node *rnode = &s->recipes.nodes[order->tasks[j].node_index];

                for (k = 0; k < KOS_ASSIGN_MAX_CHEFS && k < chef_count; k++) {
                    int chef = idle[k];

                    if (!chef_can_take_task(world, chef, rnode->op, holder, bound[chef])) {
                        mask &= ~(1 << k);
                    }
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
                    value = KOS_SHM_MAX_CHEFS;
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
            int station_rc;

            if (!task_dispatch_ready(s, candidate, j, now)) {
                continue;
            }
            station_rc = task_station_index_checked(s, i, j, &station);
            if (station_rc == KOS_ERR_EMPTY) {
                /* A known resource is temporarily unavailable; keep the order waiting. */
                has_feasible = 1;
                break;
            }
            /* Missing stations are opened so build_candidates() reports task failure. */
            if (station_rc != KOS_OK ||
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
                     "order=%s dish=%s open_seq=%d policy=%s task_policy=%s crit_ms=%lld "
                     "slack_ms=%lld",
                     order->id, order->dish, order->open_seq, kos_policy_name(s->policy),
                     kos_task_policy_name(s->task_policy),
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
    release_order_cook_reservations(s, order);
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
        /* Iterate pour tasks first: cooked food can burn while ordinary ready work waits. */
        for (j = 0; j < order->task_count; j++) {
            sched_task *task = &order->tasks[j];
            int station;

            if (strcmp(s->recipes.nodes[task->node_index].op, "pour") != 0) {
                continue;
            }
            if (!task_dispatch_ready(s, order, j, now)) {
                continue;
            }
            if (task->ready_since_ms == 0) {
                task->ready_since_ms = now;
            }
            if (n >= cap) {
                return n;
            }
            {
                int station_rc = task_station_index_checked(s, i, j, &station);
                if (station_rc == KOS_ERR_EMPTY) {
                    mark_resource_wait(s, -1, order, task,
                                       &s->recipes.nodes[task->node_index]);
                    kos_log_debug(s->rt.log, "cooker_station_wait", "order=%s node=%s",
                                  order->id, s->recipes.nodes[task->node_index].id);
                    continue;
                }
                if (station_rc != KOS_OK) {
                    fail_task_no_station(s, order, j, now);
                    break;
                }
            }
            if (!station_reachable(s, station, chef_cells, chef_count)) {
                kos_log_debug(s->rt.log, "task_station_unreachable",
                              "order=%s node=%s station=%d idle=%d chef_cells=%d,%d",
                              order->id, s->recipes.nodes[task->node_index].id, station,
                              chef_count, chef_count > 0 ? chef_cells[0] : -1,
                              chef_count > 1 ? chef_cells[1] : -1);
                continue;
            }
            out[n].order_index = i;
            out[n].task_index = j;
            out[n].node_index = task->node_index;
            out[n].station_index = station;
            n++;
        }
        for (j = 0; j < order->task_count; j++) {
            sched_task *task = &order->tasks[j];
            int station;
            if (strcmp(s->recipes.nodes[task->node_index].op, "pour") == 0 ||
                !task_dispatch_ready(s, order, j, now)) {
                continue;
            }
            if (!pot_chain_start_allowed(s, order, j)) {
                continue;
            }
            if (strcmp(s->recipes.nodes[task->node_index].op, "fetch") == 0 &&
                !order_start_allowed(s, order)) {
                continue;
            }
            if (task->ready_since_ms == 0) {
                task->ready_since_ms = now;
            }
            if (n >= cap) {
                return n;
            }
            {
                int station_rc = task_station_index_checked(s, i, j, &station);
                if (station_rc == KOS_ERR_EMPTY) {
                    /* 灶/板都被占着：这也是资源竞争，要进 waits 统计（OS 课设证据）。 */
                    mark_resource_wait(s, -1, order, task,
                                       &s->recipes.nodes[task->node_index]);
                    kos_log_debug(s->rt.log, "cooker_station_wait", "order=%s node=%s",
                                  order->id, s->recipes.nodes[task->node_index].id);
                    continue;
                }
                if (station_rc != KOS_OK) {
                    fail_task_no_station(s, order, j, now);
                    break;
                }
            }
            if (!station_reachable(s, station, chef_cells, chef_count)) {
                kos_log_debug(s->rt.log, "task_station_unreachable",
                              "order=%s node=%s station=%d idle=%d chef_cells=%d,%d",
                              order->id, s->recipes.nodes[task->node_index].id, station,
                              chef_count, chef_count > 0 ? chef_cells[0] : -1,
                              chef_count > 1 ? chef_cells[1] : -1);
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
    if (s->putdown.state == 1 && n < cap) {
        int station = s->putdown.station_index;

        if (station >= 0 && station < s->kitchen.station_count &&
            station_reachable(s, station, chef_cells, chef_count)) {
            out[n].order_index = -2;
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

/*
 * 持有亲和的子工序 DONE 后，产物必须出现在这个厨师手上。超过宽限还空手，说明
 * 东西丢了（被换走/掉地/插件漏帧）：把订单判死，别再派下去让宏对着空工位按交互
 * （实机 worker-2 的 `macro=drop wait=empty-hand timeout`）。
 */
static int64_t holder_child_done_ms(const scheduler *s, const sched_order *order, int node,
                                    int chef)
{
    int64_t done_ms = 0;
    int e;

    for (e = 0; e < s->recipes.edge_count; e++) {
        int child_task;
        const sched_task *child;

        if (s->recipes.edges[e].to != node) {
            continue;
        }
        child_task = order_task_by_node(order, s->recipes.edges[e].from);
        if (child_task < 0) {
            continue;
        }
        child = &order->tasks[child_task];
        if (child->state != TASK_DONE || child->chef_id != chef ||
            !op_leaves_item_in_hand(s->recipes.nodes[child->node_index].op)) {
            continue;
        }
        if (child->done_ms > done_ms) {
            done_ms = child->done_ms;
        }
    }
    return done_ms;
}

static void reconcile_holding_loss(scheduler *s, int64_t now)
{
    int i;

    if (!s->world_valid) {
        return;
    }
    for (i = 0; i < s->order_count; i++) {
        sched_order *order = &s->orders[i];
        int j;

        if (order->status != KOS_ORDER_ACTIVE) {
            continue;
        }
        for (j = 0; j < order->task_count; j++) {
            sched_task *task = &order->tasks[j];
            const kos_chef *chef;
            int64_t done_ms;
            int holder;

            /* 失败回队过的工序：宏的第一步就是把手里的东西放到工位上（装盘先放到盘堆、
             * 切菜先放到砧板），之后超时空手是正常的，东西在工位上，重试会接着做。
             * 实机 20261027-232216：装盘取盘超时一次就被判 holding-lost，整单判死。 */
            if (task->state != TASK_PENDING || task->fail_count > 0) {
                continue;
            }
            holder = task_holder_chef(s, order, j);
            if (holder < 0 || holder >= s->rt.opts.chefs) {
                continue;
            }
            chef = world_chef(&s->world, holder);
            if (chef == NULL || chef->holding[0] != '\0') {
                continue;
            }
            done_ms = holder_child_done_ms(s, order, task->node_index, holder);
            if (done_ms == 0 || now - done_ms < SCHED_HOLDING_LOSS_MS) {
                continue;
            }
            kos_log_warn(s->rt.log, "task_item_lost",
                         "order=%s node=%s chef=%d child_done_ms=%lld", order->id,
                         s->recipes.nodes[task->node_index].id, holder,
                         (long long)(now - done_ms));
            task->state = TASK_FAILED;
            finish_order_with_outcome(s, i, KOS_ORDER_FAILED, "holding-lost", now);
            salvage_expired_holdings(s, order, now);
            break;
        }
    }
}

/* fifo / cp 的派工键：服务工序（洗盘、放下）先于订单工序，它们不派就会卡住别的活。 */
static int task_rank_before(const scheduler *s, const sched_candidate *a,
                            const sched_candidate *b)
{
    int seq_a, seq_b;
    int pour_a = a->order_index >= 0 &&
                 strcmp(s->recipes.nodes[a->node_index].op, "pour") == 0;
    int pour_b = b->order_index >= 0 &&
                 strcmp(s->recipes.nodes[b->node_index].op, "pour") == 0;

    /* 熟锅倒料排在一切之前（含洗盘/放下）：等一轮就可能烧焦起火。 */
    if (pour_a != pour_b) {
        return pour_a;
    }
    if ((a->order_index < 0) != (b->order_index < 0)) {
        return a->order_index < 0;
    }
    if (a->order_index < 0) {
        return a->order_index < b->order_index; /* putdown(-2) 先于 wash(-1) */
    }
    if (s->task_policy == KOS_TASK_POLICY_CP) {
        int64_t ca = kos_recipe_critical_ms(&s->recipes, s->orders[a->order_index].dish_index,
                                            a->node_index);
        int64_t cb = kos_recipe_critical_ms(&s->recipes, s->orders[b->order_index].dish_index,
                                            b->node_index);

        if (ca != cb) {
            return ca > cb;
        }
    }
    seq_a = s->orders[a->order_index].open_seq;
    seq_b = s->orders[b->order_index].open_seq;
    if (seq_a != seq_b) {
        return seq_a < seq_b;
    }
    return a->task_index < b->task_index;
}

/*
 * 工序级 fifo / cp：就绪池按键排好，依次交给一个能做的空闲厨师。fifo 取下标最小的
 * 那个（不看路程），cp 取路程最近的那个。能不能做沿用代价矩阵里的 unreachable，
 * 所以锅熟才倒、手里东西要对、分区限制这些闸门三档完全一样。
 */
static void greedy_assign(const scheduler *s, const sched_candidate *pool, int pool_count,
                          int idle_count, const kos_assign_terms *terms,
                          const int *travel_cells, int *task_of_chef)
{
    int order[KOS_ASSIGN_MAX_TASKS];
    int i, j;

    for (i = 0; i < idle_count; i++) {
        task_of_chef[i] = -1;
    }
    for (j = 0; j < pool_count; j++) {
        int k = j;

        while (k > 0 && task_rank_before(s, &pool[j], &pool[order[k - 1]])) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = j;
    }
    for (j = 0; j < pool_count; j++) {
        int task = order[j];
        int best = -1;

        for (i = 0; i < idle_count; i++) {
            int index = i * pool_count + task;

            if (task_of_chef[i] >= 0 || terms[index].unreachable) {
                continue;
            }
            if (best < 0) {
                best = i;
                /* fifo 不看路程，但倒料例外：取最近的人，少走一步少一分烧焦风险。 */
                if (s->task_policy == KOS_TASK_POLICY_FIFO &&
                    (pool[task].order_index < 0 ||
                     strcmp(s->recipes.nodes[pool[task].node_index].op, "pour") != 0)) {
                    break;
                }
            } else if (travel_cells[index] < travel_cells[best * pool_count + task]) {
                best = i;
            }
        }
        if (best >= 0) {
            task_of_chef[best] = task;
        }
    }
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
    int bound[KOS_SHM_MAX_CHEFS];
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
    world = scheduler_world(s, now);
    if (world == NULL || !world->has_round_time || world->round_time_left <= 0.0f) {
        return;
    }
    if (!s->round_started && world->round_time_left >= world->round_time_limit && world->game_time_ms <= 0) {
        return;
    }
    if (!s->round_started) {
        s->round_started = 1;
        kos_log_info(s->rt.log, "round_started", "game_time_ms=%lld",
                     (long long)world->game_time_ms);
    }
    /* 手上已经有某个待派工序的产物（上一次宏做完了，DONE/租约在崩溃/丢帧里没了）：
     * 直接把工序记成 DONE，别再把同一道工序派给他——他手不空，fetch 只会超时。
     * 这是 Worker 重启后 RECOVERED 重排走不通的那条路（os-loop）。 */
    if (world != NULL) {
        for (i = 0; i < s->rt.opts.chefs && i < KOS_ASSIGN_MAX_CHEFS; i++) {
            if (s->running_by_chef[i] != 0) {
                continue;
            }
            (void)try_adopt_holding(s, i, "holding-matches", now);
        }
    }
    for (i = 0; i < s->rt.opts.chefs && i < KOS_ASSIGN_MAX_CHEFS; i++) {
        if (s->blocked_hold[i] && !has_avoid_task(s, i)) {
            /* 被重排的工序已经完成/失败，扣留自然解除，避免这个厨师永远没活。 */
            s->blocked_hold[i] = 0;
        }
        if (s->hold_stuck[i]) {
            const kos_chef *stuck = world_chef(&s->world, i);

            /* 手里没人要的东西一旦没了（别人拿走/终于放下），恢复接活。 */
            if (stuck != NULL && stuck->holding[0] == '\0') {
                s->hold_stuck[i] = 0;
            } else if (s->world_valid && stuck != NULL &&
                       try_adopt_holding(s, i, "hold-stuck", now)) {
                /* 新开了一张同菜的订单，手里的东西有用了。 */
                s->hold_stuck[i] = 0;
            } else if (s->world_valid && stuck != NULL && s->putdown.state == 0 &&
                       counter_station_index(&s->kitchen) >= 0) {
                /* 当初放下槽位被别人占着才记的 orphaned：槽位空了就重新排队，
                 * 不然这个厨师拿着东西永远站着（实机 20261027-200237 的 Tortilla）。 */
                queue_putdown(s, i, stuck->holding, "hold-stuck-retry", now);
                if (s->hold_stuck[i]) {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (s->running_by_chef[i] == 0 && !s->blocked_hold[i]) {
            idle[idle_count++] = i;
        }
    }
    if (idle_count == 0) {
        return;
    }
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
        int ready = opened_ready_count(s, now, idle, chef_cells, idle_count, world);

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
    /* A cook reservation is intentionally held until its pour/plate releases it.  Do not
     * pre-filter later non-cook work by the same kind: pour needs the held source cooker,
     * while pickup/plate/deliver use other resources.  The real acquire below is authoritative. */
    for (i = 0; i < cand_count; i++) {
        const char *kind = candidate_resource(s, &all[i]);
        const kos_recipe_node *node = all[i].order_index >= 0
                                          ? &s->recipes.nodes[all[i].node_index]
                                          : NULL;

        if (node != NULL && (strcmp(node->op, "pour") == 0 ||
                             strcmp(node->op, "pickup") == 0 ||
                             strcmp(node->op, "plate") == 0 ||
                             strcmp(node->op, "deliver") == 0)) {
            pool[pool_count++] = all[i];
            continue;
        }
        {
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
                    value = KOS_SHM_MAX_CHEFS;
                }
                kind_free[k] = value;
            }
            if (kind_free[k] > 0) {
                kind_free[k]--;
                pool[pool_count++] = all[i];
            }
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
    holding_chefs(s, bound);
    for (i = 0; i < idle_count; i++) {
        int cell = chef_cells[i];
        int chef = idle[i];
        for (j = 0; j < pool_count; j++) {
            int index = i * pool_count + j;
            kos_assign_terms *term = &terms[index];
            int station = pool[j].station_index;
            int holder = -1;

            memset(term, 0, sizeof *term);
            travel_cells[index] = 0;
            if (pool[j].order_index >= 0) {
                sched_task *pooled =
                    &s->orders[pool[j].order_index].tasks[pool[j].task_index];

                holder = task_holder_chef(s, &s->orders[pool[j].order_index],
                                          pool[j].task_index);
                if (pooled->avoid_chef == idle[i]) {
                    /* 这个厨师上一轮走到一半卡住；先派给别人，别原地重试。 */
                    term->unreachable = 1;
                    travel_cells[index] = SCHED_UNREACHABLE_CELLS;
                }
            }
            {
                const char *op = pool[j].order_index >= 0
                                     ? s->recipes.nodes[pool[j].node_index].op
                                     : (pool[j].order_index == -2 ? "assemble" : "wash");
                int putdown = pool[j].order_index == -2;

                if (putdown ? s->putdown.chef_id != chef
                            : !chef_can_take_task(world, chef, op, holder, bound[chef])) {
                    /* G11：手里不是这件东西就派去 drop/fetch 只会空等到超时；
                     * 放下工序只派给东西在他手上的那个厨师。 */
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
            if (pool[j].order_index == -2) {
                term->critical_s = SCHED_PUTDOWN_MS / 1000.0;
                term->urgency = 1.0;
            } else if (pool[j].order_index < 0) {
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
                if (strcmp(s->recipes.nodes[pool[j].node_index].op, "pour") == 0) {
                    /* 倒料只剩 0.8 s 关键路径，按原式会输给锅链上游的 fetch/cut
                     * （×2 后约 −13）：实机 20261028-042654 熟锅等了 10 s 才派、
                     * 烧焦起火。锅熟后 12 s 就焦，倒料按锅链最高优先级算。 */
                    term->critical_s = SCHED_POUR_CRITICAL_S;
                    term->critical_x2 = 1;
                }
                if (s->orders[pool[j].order_index].tasks[pool[j].task_index].avoid_chef == chef) {
                    term->unreachable = 1;
                }
            }
            if (s->zoned && s->zones.from_file) {
                int chef_zone = kos_zones_chef_zone(&s->zones, chef);
                int cand_zone = -1;
                if (pool[j].order_index >= 0) {
                    const kos_recipe_node *rnode = &s->recipes.nodes[pool[j].node_index];
                    if (strcmp(rnode->op, "drop") == 0 ||
                        (strcmp(rnode->op, "fetch") == 0 && strcmp(rnode->resource, "counter") == 0)) {
                        cand_zone = rnode->zone;
                    } else {
                        cand_zone = kos_zones_station_zone(&s->zones, s->kitchen.stations[station].id);
                    }
                } else {
                    cand_zone = kos_zones_station_zone(&s->zones, s->kitchen.stations[station].id);
                }
                if (chef_zone >= 0 && cand_zone >= 0 && chef_zone != cand_zone) {
                    term->unreachable = 1;
                    term->zone_penalty = 1.0;
                } else {
                    term->zone_penalty = 0.0;
                }
            }
        }
    }
    if (s->task_policy == KOS_TASK_POLICY_COST) {
        kos_error err;

        kos_error_clear(&err);
        if (kos_assign_solve(&s->assign_cfg.weights, idle_count, pool_count,
                             terms, task_of_chef, &err) != KOS_OK) {
            char text[256];

            error_text(&err, text, sizeof text);
            kos_log_warn(s->rt.log, "assign_failed", "error=%s", text);
            return;
        }
    } else {
        greedy_assign(s, pool, pool_count, idle_count, terms, travel_cells, task_of_chef);
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
        char source_station_id[KOS_SHM_NAME_MAX] = "";
        char putdown_node[KOS_MQ_NODE_ID_MAX] = "";
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
        } else if (cand->order_index == -2) {
            /* 放下工序：worker 按 assemble 展开成 goto + drop，node 名只用于日志。 */
            snprintf(putdown_node, sizeof putdown_node, "drop:%s", s->putdown.item);
            order_id = "putdown";
            dish = "";
            node_id = putdown_node;
            duration = SCHED_PUTDOWN_MS;
            priority = SCHED_WASH_PRIORITY;
        } else {
            order_id = "wash";
            node_id = "wash";
            duration = candidate_duration_ms(s, cand);
            priority = SCHED_WASH_PRIORITY;
        }
        station = &s->kitchen.stations[cand->station_index];
        if (node != NULL &&
            (strcmp(node->op, "cook") == 0 || strcmp(node->op, "cut") == 0 ||
             strcmp(node->op, "mix") == 0)) {
            unsigned salt = (unsigned)(cand->order_index * KOS_RECIPE_MAX_DISH_NODES +
                                       cand->task_index);
            int repick = -1;
            int repick_rc;

            if (strcmp(node->op, "cook") == 0) {
                repick_rc = pick_cooker_station(s, node->resource, node->step, salt,
                                                &repick);
            } else if (cut_retry_station(s, task, node, &repick) == KOS_OK) {
                repick_rc = KOS_OK;
            } else {
                repick_rc = pick_board_station(s, node->resource, salt, &repick);
            }
            if (repick_rc == KOS_ERR_EMPTY) {
                mark_resource_wait(s, chef, order, task, node);
                continue;
            }
            if (repick_rc == KOS_OK) {
                cand->station_index = repick;
                station = &s->kitchen.stations[repick];
            }
        }
        if (node != NULL && strcmp(node->op, "pour") == 0) {
            int cook_task_index = pour_source_cook_task(s, order, cand->node_index);

            if (cook_task_index == -2) {
                kos_log_warn(s->rt.log, "pour_ambiguous_cooker", "order=%s node=%s",
                             order->id, node->id);
                task->state = TASK_FAILED;
                mark_order_finished(order, KOS_ORDER_FAILED, now);
                release_order_cook_reservations(s, order);
                continue;
            }
            if (cook_task_index < 0) {
                /* cook 没有把锅留住（图里 pour 不是 cook 的直接后继，或锅已被回收）：
                 * 端不到锅，派下去只会空转到超时。 */
                kos_log_warn(s->rt.log, "pour_source_missing", "order=%s node=%s",
                             order->id, node->id);
                task->state = TASK_FAILED;
                mark_order_finished(order, KOS_ORDER_FAILED, now);
                release_order_cook_reservations(s, order);
                continue;
            }
            copy_str(source_station_id, sizeof source_station_id,
                     order->tasks[cook_task_index].station_id);
        }
        if (node != NULL &&
            (strcmp(node->op, "cook") == 0 || strcmp(node->op, "place") == 0 ||
             strcmp(node->op, "cut") == 0 || strcmp(node->op, "mix") == 0)) {
            /* cook 留锅要它；place 的台面是 pour/pickup 的容器位置，必须跟着任务走。 */
            copy_str(task->station_id, sizeof task->station_id, station->id);
        }
        kind = candidate_resource(s, cand);
        rc = kos_resource_try_acquire(&s->resources, kind);
        if (rc == KOS_ERR_EMPTY) {
            if (order != NULL) {
                mark_resource_wait(s, chef, order, task,
                                   &s->recipes.nodes[task->node_index]);
            }
            continue;
        }
        if (rc != KOS_OK && rc != KOS_ERR_MISSING) {
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
        copy_str(msg.op, sizeof msg.op,
                 cand->order_index == -2 ? "assemble"
                                         : (cand->order_index < 0 ? "wash" : node->op));
        copy_str(msg.resource, sizeof msg.resource, kind);
        copy_str(msg.station_id, sizeof msg.station_id, station->id);
        copy_str(msg.source_station_id, sizeof msg.source_station_id, source_station_id);

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

            kos_error_clear(&err);
            if (kos_mq_send(s->tasks[chef], &msg, (unsigned)msg.priority, 20, &err) != KOS_OK) {
                char text[256];

                error_text(&err, text, sizeof text);
                kos_log_warn(s->rt.log, "task_send_failed", "order=%s node=%s chef=%d error=%s",
                             order_id, node_id, chef, text);
                (void)kos_lease_remove(&s->leases, msg.task_id);
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
            task->not_before_ms = 0;
            if (strcmp(node->op, "deliver") == 0) {
                const kos_world_state *dispatch_world = scheduler_world(s, now);

                task->score_base_valid = dispatch_world != NULL;
                task->score_base = dispatch_world != NULL ? dispatch_world->score : 0;
                if (dispatch_world != NULL) {
                    /* 这一单从这一刻起的分数变化才算它的（G9）。 */
                    order->score_anchor = dispatch_world->score;
                }
            }
        } else if (cand->order_index == -2) {
            s->putdown.state = 2;
            s->putdown.chef_id = chef;
            s->putdown.task_id = msg.task_id;
            s->putdown.started_ms = now;
        } else {
            s->wash.state = 2;
            s->wash.chef_id = chef;
            s->wash.task_id = msg.task_id;
            s->wash.started_ms = now;
        }
        switch (kos_policy_kind_of(s->policy)) {
        case KOS_POLICY_SJF:
            s->dispatch_sjf++;
            break;
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
                     "cost=%.3f travel_cells=%d policy=%s task_policy=%s", order_id, node_id,
                     chef, (unsigned long long)msg.task_id, priority, critical_ms,
                     components.cost, travel_cells[i * pool_count + pick],
                     kos_policy_name(s->policy), kos_task_policy_name(s->task_policy));

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
        json_writer_key(&w, "op");
        json_writer_string(&w, msg.op);
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
    recipe_feasibility_check(s);
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

/* <runtime>/policy.json：控制台读的"当前策略"，每次启动和切换后原子重写。 */
static void write_policy_file(scheduler *s)
{
    char path[KOS_DAEMON_PATH_MAX + 32];
    char tmp[KOS_DAEMON_PATH_MAX + 48];
    char text[160];
    int len;
    int fd;

    if (path_join(path, sizeof path, s->rt.runtime_dir, "policy.json") != 0 ||
        path_join(tmp, sizeof tmp, s->rt.runtime_dir, ".policy.json.tmp") != 0) {
        return;
    }
    len = snprintf(text, sizeof text, "{\"order\":\"%s\",\"task\":\"%s\",\"since_ms\":%lld}\n",
                   kos_policy_name(s->policy), kos_task_policy_name(s->task_policy),
                   (long long)s->policy_since_ms);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        kos_log_warn(s->rt.log, "policy_file_failed", "path=%s error=%s", tmp, strerror(errno));
        return;
    }
    if (write(fd, text, (size_t)len) != len || close(fd) != 0) {
        kos_log_warn(s->rt.log, "policy_file_failed", "path=%s error=%s", tmp, strerror(errno));
        (void)unlink(tmp);
        return;
    }
    if (rename(tmp, path) != 0) {
        kos_log_warn(s->rt.log, "policy_file_failed", "path=%s error=%s", path, strerror(errno));
        (void)unlink(tmp);
    }
}

/*
 * Supervisor 重新拉起（generation > 1）时，从 policy.json 读回上一任最后生效的策略，
 * 否则运行中切过的策略会被启动参数覆盖（实机 20261028-123754：切 SJF/fifo 后 kill -9，
 * 新 Scheduler 回到 FCFS/cost）。第一次启动不读：runtime 目录可能留着上一局的文件。
 */
static void restore_policy_file(scheduler *s)
{
    char path[KOS_DAEMON_PATH_MAX + 32];
    char text[256];
    json_value *doc = NULL;
    const char *order = NULL;
    const char *task = NULL;
    long long since = 0;
    kos_policy_kind kind;
    kos_task_policy task_policy;
    FILE *file;
    size_t n;

    if (s->rt.opts.generation <= 1 ||
        path_join(path, sizeof path, s->rt.runtime_dir, "policy.json") != 0) {
        return;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return;
    }
    n = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[n] = '\0';
    if (json_parse(text, n, &doc) != JSON_OK ||
        json_object_get_string(doc, "order", &order, NULL) != JSON_OK ||
        json_object_get_string(doc, "task", &task, NULL) != JSON_OK ||
        kos_policy_kind_from_name(order, &kind) != KOS_OK ||
        kos_task_policy_from_name(task, &task_policy) != KOS_OK) {
        kos_log_warn(s->rt.log, "policy_restore_failed", "path=%s", path);
        json_free(doc);
        return;
    }
    (void)json_object_get_int(doc, "since_ms", &since);
    kos_policy_set_kind(s->policy, kind);
    s->task_policy = task_policy;
    s->policy_since_ms = since;
    kos_log_info(s->rt.log, "policy_restored", "order=%s task=%s generation=%d",
                 kos_policy_kind_name(kind), kos_task_policy_name(task_policy),
                 s->rt.opts.generation);
    json_free(doc);
}

/*
 * 读 <runtime>/strategy.request 并删除。返回 1 = 文件存在且合法（order 与 task 已填好），
 * 0 = 文件不存在，-1 = 文件存在但内容不合法（已写 WARN）。
 */
static int take_strategy_request(scheduler *s, kos_policy_kind *order, kos_task_policy *task)
{
    char path[KOS_DAEMON_PATH_MAX + 32];
    char text[256];
    FILE *file;
    size_t n;
    int has_order = 0;
    int has_task = 0;

    if (path_join(path, sizeof path, s->rt.runtime_dir, "strategy.request") != 0) {
        return 0;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }
    n = fread(text, 1, sizeof text - 1, file);
    text[n] = '\0';
    if (!feof(file)) {
        n = sizeof text; /* 超长：按坏内容处理 */
    }
    fclose(file);
    (void)unlink(path);
    if (n >= sizeof text - 1 ||
        kos_policy_parse_request(text, order, &has_order, task, &has_task) != KOS_OK) {
        text[sizeof text - 1] = '\0';
        text[strcspn(text, "\r\n")] = '\0';
        kos_log_warn(s->rt.log, "strategy_request_invalid", "path=%s content=%s", path, text);
        return -1;
    }
    return 1;
}

static void strategy_switch(scheduler *s)
{
    kos_policy_kind from = kos_policy_kind_of(s->policy);
    kos_task_policy from_task = s->task_policy;
    kos_policy_kind to = from;
    kos_task_policy to_task = from_task;
    const char *reason = "SIGHUP";
    json_writer w;
    char line[SCHED_LINE_MAX];
    int request = take_strategy_request(s, &to, &to_task);

    if (request < 0) {
        return;
    }
    if (request > 0) {
        reason = "request";
    } else {
        to = kos_policy_other(from);
    }
    kos_policy_set_kind(s->policy, to);
    s->task_policy = to_task;
    s->policy_since_ms = now_ms() - s->started_ms;
    kos_log_info(s->rt.log, "strategy_switch",
                 "from=%s to=%s task_from=%s task_to=%s reason=%s",
                 kos_policy_kind_name(from), kos_policy_kind_name(to),
                 kos_task_policy_name(from_task), kos_task_policy_name(to_task), reason);
    json_writer_init(&w, line, sizeof line);
    event_common(&w, s, "strategy_switch");
    json_writer_key(&w, "from");
    json_writer_string(&w, kos_policy_kind_name(from));
    json_writer_key(&w, "to");
    json_writer_string(&w, kos_policy_kind_name(to));
    json_writer_key(&w, "task_from");
    json_writer_string(&w, kos_task_policy_name(from_task));
    json_writer_key(&w, "task_to");
    json_writer_string(&w, kos_task_policy_name(to_task));
    event_end(s, &w);

    json_writer_init(&w, line, sizeof line);
    decision_common(&w, s, "strategy", reason, NULL);
    json_writer_key(&w, "from");
    json_writer_string(&w, kos_policy_kind_name(from));
    json_writer_key(&w, "to");
    json_writer_string(&w, kos_policy_kind_name(to));
    json_writer_key(&w, "task_from");
    json_writer_string(&w, kos_task_policy_name(from_task));
    json_writer_key(&w, "task_to");
    json_writer_string(&w, kos_task_policy_name(to_task));
    json_writer_object_end(&w);
    write_jsonl(s->decisions_fd, &w);
    write_policy_file(s);
}

static const char *order_status_name(int status)
{
    switch (status) {
    case KOS_ORDER_COMPLETED:
        return "completed";
    case KOS_ORDER_FAILED:
        return "failed";
    case KOS_ORDER_EXPIRED:
        return "expired";
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
    snapshot.dispatch_sjf = s->dispatch_sjf;
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
        } else if (src->status == KOS_ORDER_FAILED ||
                   src->status == KOS_ORDER_EXPIRED) {
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

    /* 仍在等待的 pending 任务也要把已经等过的时间算进本次快照。已结束订单不计入。 */
    for (i = 0; i < s->order_count; i++) {
        int j;

        if (s->orders[i].status != KOS_ORDER_ACTIVE) {
            continue;
        }
        for (j = 0; j < s->orders[i].task_count; j++) {
            const sched_task *task = &s->orders[i].tasks[j];
            int k;

            if (task->state != TASK_PENDING || !task->wait_logged || task->wait_started_ms <= 0) {
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
    if (s->zoned) {
        int zones_rc;

        kos_error_clear(&err);
        zones_rc = kos_zones_for_kitchen(s->kitchen_path, &s->zones, &err);
        if (zones_rc != KOS_OK || !s->zones.from_file) {
            char text[256];

            if (zones_rc == KOS_OK) {
                snprintf(text, sizeof text, "zones.json is required for zoned dispatch");
            } else {
                error_text(&err, text, sizeof text);
            }
            kos_log_error(s->rt.log, "zoned_config_required", "kitchen=%s error=%s",
                          s->kitchen_path, text);
            return -1;
        }
        if (kos_zones_register_handoff(&s->zones, &s->kitchen, &err) != KOS_OK ||
            kos_recipes_apply_zones(&s->recipes, &s->kitchen, &s->zones, &err) != KOS_OK) {
            char text[256];

            error_text(&err, text, sizeof text);
            kos_log_error(s->rt.log, "zoned_config_invalid", "kitchen=%s error=%s",
                          s->kitchen_path, text);
            return -1;
        }
        kos_assign_pot_chain(&s->recipes, s->pot_chain);
        kos_log_info(s->rt.log, "zoned_pipeline_ready", "zones=%d handoff=%s",
                     s->zones.zone_count, s->zones.handoff.id);
    }
    kos_assign_pot_chain(&s->recipes, s->pot_chain);
    recipe_feasibility_check(s);
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
    s->zoned = (strcmp(opts->dispatch, "zoned") == 0 || strcmp(opts->dispatch, "zone") == 0);
    if (kos_task_policy_from_name(opts->task_policy, &s->task_policy) != KOS_OK) {
        s->task_policy = KOS_TASK_POLICY_COST;
    }
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
    /* 重启后队列沿用，里面可能还有上一任的 DONE/FAILED；按代错开 id，旧事件只会落到
     * event_unknown_task，不会被当成新任务的结果。 */
    s->next_task_id = opts->generation > 1 ? (uint64_t)(opts->generation - 1) << 32 : 0;

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
    if (kos_resources_attach(&s->kitchen, &s->resources, s->rt.opts.instance, &err) != KOS_OK) {
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
    flush_rejected_order_events(s);
    if (open_queues(s) != 0) {
        close_queues(s);
        kos_policy_destroy(s->policy);
        kos_child_teardown(&s->rt);
        return 1;
    }

    restore_policy_file(s);
    kos_log_info(s->rt.log, "scheduler_ready",
                 "policy=%s task_policy=%s chefs=%d orders=%d run_dir=%s",
                 kos_policy_name(s->policy), kos_task_policy_name(s->task_policy),
                 s->rt.opts.chefs, s->order_count, s->run_dir);
    write_policy_file(s);

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
                    finish_order_with_outcome(s, i, KOS_ORDER_FAILED, "task-timeout", now);
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
        if (s->putdown.state == 2 &&
            now - s->putdown.started_ms > SCHED_PUTDOWN_MS + SCHED_TASK_TIMEOUT_MS) {
            uint64_t task_id = s->putdown.task_id;
            int chef = s->putdown.chef_id;

            kos_log_warn(s->rt.log, "putdown_timeout", "task=%llu chef=%d",
                         (unsigned long long)task_id, chef);
            if (chef >= 0 && chef < KOS_SHM_MAX_CHEFS &&
                s->running_by_chef[chef] == task_id) {
                s->running_by_chef[chef] = 0;
            }
            (void)release_lease_for_task(s, task_id, "resource_reclaimed",
                                         "putdown-timeout");
            deactivate_putdown(s, "putdown-timeout");
        }
        reconcile_holding_loss(s, now);
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
        printf("wellcooked-scheduler 0.3.0\n");
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
