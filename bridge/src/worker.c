/*
 * worker.c -- Chef Worker：mq 收工序 → 宏动作序列 → 导航表 → 输入槽 → 回事件。
 *
 * 它不选单、不规划路径：走位由 navd 统一算，Worker 只把"到哪个工位"写进目标槽、
 * 按计划槽里已放行的路点发 path。到站后把 Scheduler 的 op 展开成宏动作（macro.c），
 * 每一步按世界状态判完成；interact intent 维持 1000 ms，覆盖插件立即按一次、600 ms 后
 * 重按 250 ms 的完整两脉冲窗口；work 持续按住，等待期间发 stop，Bridge 的帧屏障始终拿到有效槽。
 *
 * 时间一律用世界状态的 game_time_ms：暂停时超时不走。navd 崩溃（计划槽写入时钟
 * 落后 500 ms）时先发 stop；正在 goto 的人暂停推进宏动作，等 navd 回来再继续。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/child.h>
#include "worker_timing.h"
#include <kitchenos/kitchen.h>
#include <kitchenos/log.h>
#include <kitchenos/macro.h>
#include <kitchenos/recipes.h>
#include <kitchenos/mq.h>
#include <kitchenos/navtab.h>
#include <kitchenos/shm.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WORKER_LOOP_MS 5
/* 步骤切换后 STOP 的最短驻留时间：Bridge 每帧（~16.7 ms @60fps）才读一次输入槽，
 * 否则 STOP 会被下一次 INTERACT 覆盖，插件把两次同目标按键当成一次连按。 */
#define WORKER_STOP_HOLD_MS 40
#define WORKER_NAV_STALE_MS 500
#define WORKER_INTERACT_WINDOW_MS KOS_WORKER_INTERACT_WINDOW_MS

typedef struct {
    kos_child_runtime rt;
    kos_kitchen kitchen;
    char kitchen_path[KOS_DAEMON_PATH_MAX];
    /* 只读 recipe 边一张表：start_cook 要用"cook 节点的子节点 label"核对锅里
     * 放的是不是这道菜要的原料（S2）。读不到菜谱时 start_cook 退回剥修饰语。 */
    kos_recipes recipes;
    int recipes_loaded;
    mqd_t task_queue;
    mqd_t events_queue;
    int queues_ready;
    int mq_wait_logged;
    int recovery_sent;
    int target_cleared;
    kos_mq_msg current;
    int have_task;
    kos_macro macro;
    uint32_t target_seq;
    int pinned;
    int stale_logged;
    int64_t interact_until_game_ms;
    int interact_window_step;
    /* 步骤切换时先把 STOP 留在槽里至少这么久：Bridge 每帧才读一次槽，
     * 5 ms 工作循环 + 10 ms 槽周期会把 STOP 盖掉，插件收不到"上一个动作结束"，
     * 同一个目标上的下一次 interact 就被当成连按而不再触发（plate 取盘踩过）。 */
    int64_t stop_hold_until_ms;
    kos_navtab *nav;
    int64_t next_nav_attach_ms;
    int nav_wait_logged;
    uint64_t frame_id;
    int64_t next_slot_ms;
    int slot_error_logged;
} worker;

static void send_event(worker *w, int event, const char *detail);

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

static int resolve_kitchen_path(worker *w)
{
    if (w->rt.opts.kitchen_path[0] != '\0') {
        copy_str(w->kitchen_path, sizeof w->kitchen_path, w->rt.opts.kitchen_path);
        return 0;
    }
    if (kos_child_default_config_path("kitchen.example.json", w->kitchen_path,
                                      sizeof w->kitchen_path) != 0) {
        kos_log_error(w->rt.log, "kitchen_missing",
                      "no --kitchen and no config/kitchen.example.json");
        return -1;
    }
    return 0;
}

static int open_queues(worker *w)
{
    kos_error err;
    char name[KOS_MQ_NAME_MAX];
    char events_name[KOS_MQ_NAME_MAX];
    char text[256];

    if (w->rt.opts.chefs <= 0) {
        return -1;
    }
    if (kos_mq_task_name(w->rt.opts.chef_id, w->rt.opts.instance, name, sizeof name) !=
        KOS_OK) {
        return -1;
    }
    if (kos_mq_events_name(w->rt.opts.instance, events_name, sizeof events_name) != KOS_OK) {
        return -1;
    }
    kos_error_clear(&err);
    if (kos_mq_open(name, O_RDONLY, &w->task_queue, &err) != KOS_OK) {
        if (!w->mq_wait_logged) {
            error_text(&err, text, sizeof text);
            kos_log_info(w->rt.log, "mq_wait", "queue=%s error=%s", name, text);
            w->mq_wait_logged = 1;
        }
        return -1;
    }
    kos_error_clear(&err);
    if (kos_mq_open(events_name, O_WRONLY, &w->events_queue, &err) != KOS_OK) {
        kos_mq_close(w->task_queue);
        w->task_queue = (mqd_t)-1;
        if (!w->mq_wait_logged) {
            error_text(&err, text, sizeof text);
            kos_log_info(w->rt.log, "mq_wait", "queue=%s error=%s", events_name, text);
            w->mq_wait_logged = 1;
        }
        return -1;
    }
    w->queues_ready = 1;
    kos_log_info(w->rt.log, "mq_ready", "task_queue=%s chef=%d", name,
                 w->rt.opts.chef_id);
    return 0;
}

static void close_queues(worker *w)
{
    if (w->task_queue != (mqd_t)-1) {
        kos_mq_close(w->task_queue);
        w->task_queue = (mqd_t)-1;
    }
    if (w->events_queue != (mqd_t)-1) {
        kos_mq_close(w->events_queue);
        w->events_queue = (mqd_t)-1;
    }
    w->queues_ready = 0;
}

static void send_event(worker *w, int event, const char *detail)
{
    kos_mq_msg msg;
    kos_error err;
    char text[256];

    if (w->events_queue == (mqd_t)-1) {
        return;
    }
    msg = w->current;
    msg.version = KOS_MQ_VERSION;
    msg.kind = KOS_MQ_EVENT;
    msg.event = event;
    msg.generation = w->rt.opts.generation;
    msg.chef_id = w->rt.opts.chef_id;
    copy_str(msg.detail, sizeof msg.detail, detail != NULL ? detail : "");
    kos_error_clear(&err);
    if (kos_mq_send(w->events_queue, &msg, 1, 50, &err) != KOS_OK) {
        error_text(&err, text, sizeof text);
        kos_log_warn(w->rt.log, "event_send_failed", "event=%d error=%s", event, text);
    }
}

static const char *current_target_station(const worker *w)
{
    if (w->have_task && w->macro.plan.step_count > 0 &&
        w->macro.step_index < w->macro.plan.step_count) {
        const char *step_station = w->macro.plan.steps[w->macro.step_index].station_id;
        if (step_station != NULL && step_station[0] != '\0') {
            return step_station;
        }
    }
    return w->current.station_id;
}

static void write_target(worker *w, int has_target, int pinned)
{
    kos_nav_target target;

    if (w->nav == NULL) {
        return;
    }
    memset(&target, 0, sizeof target);
    target.has_target = has_target;
    target.pinned = pinned ? 1 : 0;
    target.target_seq = w->target_seq;
    target.urgency = w->have_task ? (uint32_t)(w->current.priority > 0
                                                   ? w->current.priority
                                                   : 0)
                                  : 0;
    if (has_target) {
        copy_str(target.station_id, sizeof target.station_id, current_target_station(w));
    }
    target.station_id[KOS_SHM_NAME_MAX - 1] = '\0';
    (void)kos_navtab_write_target(w->nav, w->rt.opts.chef_id, &target);
}

static void clear_target(worker *w)
{
    w->pinned = 0;
    write_target(w, 0, 0);
}

static int read_world(worker *w, kos_world_state *world)
{
    if (w->rt.shm == NULL) {
        return 0;
    }
    return kos_shm_world_read(w->rt.shm, world, KOS_SHM_READ_ATTEMPTS, NULL) == KOS_OK;
}

static void publish_action(worker *w, kos_input_action *action)
{
    kos_error err;
    char text[256];

    if (w->rt.shm == NULL) {
        return;
    }
    action->frame_id = ++w->frame_id;
    action->chef_id = w->rt.opts.chef_id;
    kos_error_clear(&err);
    if (kos_shm_slot_publish(w->rt.shm, (uint32_t)w->rt.opts.chef_id, action) != KOS_OK) {
        if (!w->slot_error_logged) {
            error_text(&err, text, sizeof text);
            kos_log_warn(w->rt.log, "slot_publish_failed", "chef=%d error=%s",
                         w->rt.opts.chef_id, text);
            w->slot_error_logged = 1;
        }
    }
}

static void publish_stop(worker *w)
{
    kos_input_action action;

    memset(&action, 0, sizeof action);
    action.kind = KOS_ACTION_STOP;
    publish_action(w, &action);
}

static void publish_path(worker *w, const kos_nav_plan *plan)
{
    kos_input_action action;
    int count = plan->waypoint_count;
    int i;

    if (count < 0) {
        count = 0;
    }
    if (count > KOS_MAX_WAYPOINTS) {
        count = KOS_MAX_WAYPOINTS;
    }
    memset(&action, 0, sizeof action);
    action.kind = KOS_ACTION_PATH;
    action.seq = plan->plan_seq;
    action.waypoint_count = (uint32_t)count;
    for (i = 0; i < count; i++) {
        action.waypoints[i].x = plan->waypoints[i].x;
        action.waypoints[i].y = plan->waypoints[i].y;
    }
    publish_action(w, &action);
}

static void publish_macro_action(worker *w, const kos_macro_output *out,
                                 const kos_nav_plan *plan, int plan_fresh,
                                 int plan_matches)
{
    kos_input_action action;

    if (out->action == KOS_MACRO_ACT_NAVIGATE) {
        if (plan_fresh && plan_matches && plan->status == KOS_NAV_MOVING &&
            plan->waypoint_count > 0) {
            publish_path(w, plan);
        } else {
            publish_stop(w);
        }
        return;
    }
    if (out->action == KOS_MACRO_ACT_STOP) {
        publish_stop(w);
        return;
    }

    memset(&action, 0, sizeof action);
    if (out->action == KOS_MACRO_ACT_FACE) {
        if (kos_kitchen_face_action(&w->kitchen, out->target_object, &action) != KOS_OK) {
            kos_log_warn(w->rt.log, "macro_face_station_missing",
                         "task=%llu station=%s",
                         (unsigned long long)w->current.task_id, out->target_object);
            publish_stop(w);
            return;
        }
    } else {
        if (out->action == KOS_MACRO_ACT_WORK) {
            action.kind = KOS_ACTION_WORK;
        } else {
            /* 窗口由 update_macro 在宏仍要 interact 时续期；这里只管"这一步没要过"。
             * 不在这里判 player 过期：过期的窗口已经续过，判了会让宏剩下的等待时间
             * 全变成 stop（实机 _3 pickup 只按一次的原因之一）。 */
            if (w->interact_window_step != w->macro.step_index) {
                publish_stop(w);
                return;
            }
            action.kind = KOS_ACTION_INTERACT;
        }
        copy_str(action.target_object, sizeof action.target_object, out->target_object);
        /* 插件只认识少数几个场景对象名；把站点坐标一起发过去，插件按坐标判断
         * 是否已朝向目标，避免对着别的工作台按键（实机 _3 拿走了别人的平底锅）。 */
        {
            const kos_station *st = kos_kitchen_station_by_id(&w->kitchen, out->target_object);
            if (st != NULL) {
                action.target_x = st->x;
                action.target_y = st->z;
            } else {
                action.target_x = NAN;
                action.target_y = NAN;
            }
        }
    }
    publish_action(w, &action);
}

static void log_macro_start(worker *w, int step_index)
{
    const kos_macro_step *step;

    if (step_index < 0 || step_index >= w->macro.plan.step_count) {
        return;
    }
    step = &w->macro.plan.steps[step_index];
    kos_log_info(w->rt.log, "macro_start",
                 "task=%llu step=%d/%d macro=%s station=%s item=%s duration_ms=%lld",
                 (unsigned long long)w->current.task_id, step_index + 1,
                 w->macro.plan.step_count, kos_macro_kind_name(step->kind),
                 step->station_id, step->item[0] != '\0' ? step->item : "-",
                 (long long)step->duration_ms);
}

/*
 * 菜谱里喂给这个 cook 节点的原料 label。边是"子 → 父"：cook:FriedChicken 的子节点
 * 是 cut:BurritoChicken（label BurritoChicken），cook:BoiledRice 的是 fetch:Rice
 * （label Rice）。cook 节点只该有一个输入；多于一个说明菜谱形状不对，宁可留空让
 * start_cook 退回保守判据，也不要猜。
 */
static void plan_set_cook_input(worker *w, const char *op, const char *node_id,
                                kos_macro_plan *plan)
{
    int node;
    int found = 0;
    int i;

    if (w == NULL || !w->recipes_loaded || op == NULL || strcmp(op, "cook") != 0) {
        return;
    }
    node = kos_recipes_node_index(&w->recipes, node_id);
    if (node < 0) {
        return;
    }
    for (i = 0; i < w->recipes.edge_count; i++) {
        if (w->recipes.edges[i].to != node) {
            continue;
        }
        if (found) {
            kos_log_warn(w->rt.log, "cook_input_ambiguous",
                         "node=%s edge=%d extra_child=%d", node_id, node,
                         w->recipes.edges[i].from);
            return;
        }
        found = w->recipes.edges[i].from + 1;
    }
    if (found) {
        kos_macro_plan_set_cook_input(plan, w->recipes.nodes[found - 1].label);
    }
}

static int start_task(worker *w, const kos_mq_msg *msg)
{
    kos_error err;
    kos_macro_plan plan;
    char text[256];

    if (msg->version != KOS_MQ_VERSION || msg->kind != KOS_MQ_TASK ||
        msg->chef_id != w->rt.opts.chef_id) {
        kos_log_warn(w->rt.log, "task_bad_message",
                     "version=%u kind=%u chef=%d expected=%d", msg->version, msg->kind,
                     msg->chef_id, w->rt.opts.chef_id);
        return -1;
    }
    w->current = *msg;
    if (kos_kitchen_station_index(&w->kitchen, msg->station_id) < 0) {
        kos_log_warn(w->rt.log, "task_bad_station", "task=%llu station=%s",
                     (unsigned long long)msg->task_id, msg->station_id);
        send_event(w, KOS_EVT_FAILED, "unknown station");
        return -1;
    }
    kos_error_clear(&err);
    if (kos_macro_plan_build_with_source(&w->kitchen, msg->op, msg->node_id, msg->station_id,
                                         msg->source_station_id, msg->order_id, msg->duration_ms,
                                         &plan, &err) != KOS_OK) {
        char detail[KOS_MQ_DETAIL_MAX];

        error_text(&err, text, sizeof text);
        kos_log_warn(w->rt.log, "macro_map_failed",
                     "task=%llu op=%s node=%s error=%s",
                     (unsigned long long)msg->task_id, msg->op, msg->node_id, text);
        snprintf(detail, sizeof detail, "macro-map op=%s", msg->op);
        send_event(w, KOS_EVT_FAILED, detail);
        return -1;
    }
    plan_set_cook_input(w, msg->op, msg->node_id, &plan);
    kos_macro_begin(&w->macro, &plan);
    w->macro.log = w->rt.log;
    w->target_seq++;
    w->pinned = 0;
    w->stale_logged = 0;
    w->interact_until_game_ms = 0;
    w->interact_window_step = -1;
    w->stop_hold_until_ms = 0;
    w->have_task = 1;
    write_target(w, 1, 0);
    kos_log_info(w->rt.log, "task_received",
                 "task=%llu order=%s node=%s op=%s station=%s duration_ms=%d "
                 "macro_steps=%d target_seq=%u",
                 (unsigned long long)msg->task_id, msg->order_id, msg->node_id, msg->op,
                 msg->station_id, msg->duration_ms, w->macro.plan.step_count,
                 w->target_seq);
    log_macro_start(w, 0);
    send_event(w, KOS_EVT_ACCEPTED, "accepted");
    return 0;
}

static void finish_task(worker *w, int event, const char *detail)
{
    send_event(w, event, detail);
    clear_target(w);
    w->have_task = 0;
    memset(&w->macro, 0, sizeof w->macro);
}

static void maybe_recover(worker *w)
{
    if (w->recovery_sent || !w->queues_ready || w->nav == NULL) {
        return;
    }
    if (w->rt.opts.generation <= 1) {
        return;
    }
    if (!w->target_cleared) {
        /* Worker 崩溃后 navd 还保留着旧目标；先清空再宣布 RECOVERED，Scheduler
         * 重排这道工序之前这个人不会继续往旧工位走。 */
        write_target(w, 0, 0);
        w->target_cleared = 1;
    }
    send_event(w, KOS_EVT_RECOVERED, "worker ready");
    kos_log_info(w->rt.log, "worker_recovered_sent", "chef=%d generation=%d",
                 w->rt.opts.chef_id, w->rt.opts.generation);
    w->recovery_sent = 1;
}

static int worker_plan(worker *w, kos_nav_plan *plan)
{
    memset(plan, 0, sizeof *plan);
    if (w->nav == NULL) {
        return -1;
    }
    if (kos_navtab_read_plan(w->nav, w->rt.opts.chef_id, plan) != KOS_OK) {
        return -1;
    }
    return 0;
}

static int current_step_is_goto(const worker *w)
{
    if (!w->have_task || w->macro.step_index >= w->macro.plan.step_count) {
        return 0;
    }
    return w->macro.plan.steps[w->macro.step_index].kind == KOS_MACRO_GOTO;
}

/* 推进宏动作；返回 1 表示步骤变了、这一帧要立刻发布新动作。 */
static int advance_macro(worker *w, const kos_world_state *world, int64_t game_ms,
                         const kos_nav_plan *plan, int plan_fresh, int plan_matches,
                         int nav_stale, kos_macro_output *out)
{
    int prev_index = w->macro.step_index;
    int prev_started = w->macro.step_started;
    int64_t prev_start_ms = w->macro.step_start_ms;
    int prev_kind = prev_index < w->macro.plan.step_count
                        ? w->macro.plan.steps[prev_index].kind
                        : -1;
    int nav_arrived = plan_fresh && plan_matches && plan->status == KOS_NAV_ARRIVED;
    int rc;

    memset(out, 0, sizeof *out);
    out->action = KOS_MACRO_ACT_STOP;

    /* navd 不在时正在 goto 的人不要推进超时（W5 的崩溃恢复要求回来后继续走）。 */
    if (nav_stale && current_step_is_goto(w)) {
        return 0;
    }

    w->macro.log = w->rt.log;
    rc = kos_macro_advance(&w->macro, &w->kitchen, world, w->rt.opts.chef_id,
                           nav_arrived, game_ms, out);

    if ((w->macro.step_index != prev_index || rc == 1) && prev_kind >= 0 && rc >= 0) {
        int64_t waited = prev_started ? game_ms - prev_start_ms : 0;

        if (waited < 0) {
            waited = 0;
        }
        kos_log_info(w->rt.log, "macro_done",
                     "task=%llu step=%d/%d macro=%s waited_ms=%lld",
                     (unsigned long long)w->current.task_id, prev_index + 1,
                     w->macro.plan.step_count, kos_macro_kind_name(prev_kind),
                     (long long)waited);
    }

    if (rc == 1) {
        kos_log_info(w->rt.log, "task_done", "task=%llu order=%s node=%s",
                     (unsigned long long)w->current.task_id, w->current.order_id,
                     w->current.node_id);
        finish_task(w, KOS_EVT_DONE, "done");
        return 2;
    }
    if (rc < 0) {
        const char *detail = kos_macro_fail_detail(&w->macro);

        kos_log_warn(w->rt.log, "macro_failed", "task=%llu op=%s detail=%s",
                     (unsigned long long)w->current.task_id, w->current.op,
                     detail != NULL && detail[0] != '\0' ? detail : "unknown");
        if (detail == NULL || detail[0] == '\0') {
            detail = "macro failed";
        }
        finish_task(w, KOS_EVT_FAILED, detail);
        return 2;
    }

    if (w->macro.step_index != prev_index) {
        log_macro_start(w, w->macro.step_index);
        w->interact_window_step = -1;
        w->interact_until_game_ms = 0;
        if (w->macro.step_index < w->macro.plan.step_count) {
            const kos_macro_step *new_step = &w->macro.plan.steps[w->macro.step_index];
            const kos_macro_step *old_step = &w->macro.plan.steps[prev_index];
            if (strcmp(new_step->station_id, old_step->station_id) != 0 ||
                new_step->kind == KOS_MACRO_GOTO) {
                w->target_seq++;
                w->pinned = 0;
                write_target(w, 1, 0);
            }
        }
    }
    if (kos_worker_nav_should_replan(out->action, w->pinned)) {
        /* 宏认为还没到（离站位格太远/焦点不对），但 worker 把它 pin 住了：
         * 解 pin 并重写目标，navd 才会重新规划。 */
        w->pinned = 0;
        write_target(w, 1, 0);
        kos_log_info(w->rt.log, "nav_replan",
                     "task=%llu station=%s reason=macro-not-arrived",
                     (unsigned long long)w->current.task_id, w->current.station_id);
    }
    if (out->action == KOS_MACRO_ACT_INTERACT &&
        kos_worker_interact_should_rearm(w->interact_window_step, w->macro.step_index,
                                         game_ms, w->interact_until_game_ms)) {
        /* 窗口用完就续：宏还在等游戏状态变化，插件靠 1 s 的重发窗口自己去重，
         * 超出窗口的意图每 1 s 会再按一轮（PulseInterval 0.6 s × 2）。 */
        w->interact_window_step = w->macro.step_index;
        w->interact_until_game_ms = game_ms + WORKER_INTERACT_WINDOW_MS;
    }

    /* 宏要求重新走位时不能马上又 pin 回去：navd 这一帧还报 ARRIVED，同一帧 unpin→pin
     * 让它永远不重规划，厨师原地每 5 ms 打一条 nav_replan（实机 20261028-071415
     * pour 回灶 drop 失焦 1.5 s 超时，空锅端到收工）。 */
    if (w->macro.step_index == prev_index && nav_arrived && !w->pinned &&
        out->action != KOS_MACRO_ACT_NAVIGATE) {
        w->pinned = 1;
        write_target(w, 1, 1);
    }
    return w->macro.step_index != prev_index;
}

static int worker_main(worker *w, const kos_child_opts *opts)
{
    int i;

    memset(w, 0, sizeof *w);
    w->rt.opts = *opts;
    w->task_queue = (mqd_t)-1;
    w->events_queue = (mqd_t)-1;
    w->interact_until_game_ms = 0;
    w->interact_window_step = -1;

    if (kos_child_setup(&w->rt, &w->rt.opts) != 0) {
        return 1;
    }
    if (resolve_kitchen_path(w) != 0) {
        kos_child_teardown(&w->rt);
        return 1;
    }
    {
        kos_error err;
        char text[256];

        kos_error_clear(&err);
        /* 与 Scheduler 同一份工位表：zones 的 handoff 台面也要在，放下工序才能建计划。 */
        if (kos_kitchen_load_with_zones(w->kitchen_path, &w->kitchen, &err) != KOS_OK) {
            error_text(&err, text, sizeof text);
            kos_log_error(w->rt.log, "kitchen_load_failed", "path=%s error=%s",
                          w->kitchen_path, text);
            kos_child_teardown(&w->rt);
            return 1;
        }
        kos_log_info(w->rt.log, "config_loaded", "kitchen=%s cells=%d stations=%d",
                     w->kitchen_path, w->kitchen.cell_count, w->kitchen.station_count);
        /* 站位格兜底是加载器记下来的：它自己不碰 stderr，由拿到地图的进程打日志。 */
        if (w->kitchen.stand_warning[0] != '\0') {
            kos_log_warn(w->rt.log, "stand_fallback", "%s count=%d",
                         w->kitchen.stand_warning, w->kitchen.stand_fallback_count);
        }
    }
    {
        char recipes_path[KOS_DAEMON_PATH_MAX];
        const char *path = w->rt.opts.recipes_path;
        kos_error err;
        char text[256];

        if (path[0] == '\0') {
            /* 先配 kitchen 同目录的 recipes.json（地图导出是 config/maps/<scene>/
             * kitchen.json + recipes.json 成对出现），再退回默认 recipes.example.json。
             * 拿 A 图的菜谱去核对 B 图锅里放的东西会得到假的 content-mismatch。 */
            const char *slash = strrchr(w->kitchen_path, '/');

            if (slash != NULL) {
                size_t dir_len = (size_t)(slash - w->kitchen_path);

                if (dir_len + sizeof "/recipes.json" <= sizeof recipes_path) {
                    memcpy(recipes_path, w->kitchen_path, dir_len);
                    snprintf(recipes_path + dir_len, sizeof recipes_path - dir_len,
                             "/recipes.json");
                    if (access(recipes_path, R_OK) == 0) {
                        path = recipes_path;
                    }
                }
            }
            if (path[0] == '\0' &&
                kos_child_default_config_path("recipes.example.json", recipes_path,
                                              sizeof recipes_path) == 0) {
                path = recipes_path;
            }
        }
        if (path[0] != '\0') {
            kos_error_clear(&err);
            if (kos_recipes_load(path, &w->recipes, &err) == KOS_OK) {
                w->recipes_loaded = 1;
                kos_log_info(w->rt.log, "recipes_loaded",
                             "path=%s nodes=%d edges=%d dishes=%d", path,
                             w->recipes.node_count, w->recipes.edge_count,
                             w->recipes.dish_count);
            } else {
                error_text(&err, text, sizeof text);
                kos_log_warn(w->rt.log, "recipes_load_failed",
                             "path=%s error=%s (start_cook falls back to modifier strip)",
                             path, text);
            }
        }
    }

    w->next_slot_ms = 0;
    while (!kos_child_poll(&w->rt)) {
        int64_t now = now_ms();
        kos_world_state world;
        kos_nav_plan plan;
        int have_world;
        int plan_ok;
        int plan_fresh = 0;
        int plan_matches = 0;
        int nav_stale = 0;
        int64_t game_ms = 0;
        int force_publish = 0;

        if (kos_child_take_sighup()) {
            kos_log_info(w->rt.log, "signal", "name=SIGHUP action=ignored");
        }
        if (kos_child_take_sigusr1()) {
            kos_log_info(w->rt.log, "signal", "name=SIGUSR1 action=ignored");
        }
        if (w->nav == NULL && now >= w->next_nav_attach_ms) {
            kos_navtab_opts nav_opts;
            kos_error err;
            char text[256];

            kos_navtab_opts_init(&nav_opts);
            if (w->rt.opts.nav_name[0] != '\0') {
                nav_opts.object_name = w->rt.opts.nav_name;
            }
            if (w->rt.opts.nav_sem_name[0] != '\0') {
                nav_opts.sem_name = w->rt.opts.nav_sem_name;
            }
            kos_error_clear(&err);
            if (kos_navtab_open(&nav_opts, &w->nav, &err) == KOS_OK) {
                kos_log_info(w->rt.log, "nav_attached", "name=%s",
                             w->rt.opts.nav_name[0] ? w->rt.opts.nav_name
                                                    : KOS_NAV_DEFAULT_NAME);
                if (w->have_task) {
                    write_target(w, 1, w->pinned);
                }
            } else {
                if (!w->nav_wait_logged) {
                    error_text(&err, text, sizeof text);
                    kos_log_info(w->rt.log, "nav_wait", "error=%s", text);
                    w->nav_wait_logged = 1;
                }
                w->next_nav_attach_ms = now + 100;
            }
        }
        if (!w->queues_ready) {
            (void)open_queues(w);
        }
        maybe_recover(w);

        have_world = read_world(w, &world);
        if (have_world) {
            game_ms = world.game_time_ms;
        }

        if (!w->have_task && w->queues_ready) {
            for (i = 0; i < 4; i++) {
                kos_mq_msg msg;
                kos_error err;

                kos_error_clear(&err);
                if (kos_mq_receive(w->task_queue, &msg, 0, &err) != KOS_OK) {
                    break;
                }
                if (start_task(w, &msg) == 0) {
                    now = now_ms();
                    break;
                }
            }
        }

        plan_ok = worker_plan(w, &plan) == 0;
        if (plan_ok) {
            plan_fresh = plan.game_time_ms + WORKER_NAV_STALE_MS >= (uint64_t)game_ms;
            plan_matches = plan.target_seq == w->target_seq;
            if (!plan_fresh) {
                nav_stale = 1;
                if (!w->stale_logged) {
                    kos_log_warn(w->rt.log, "nav_stale",
                                 "chef=%d plan_ms=%llu game_ms=%lld",
                                 w->rt.opts.chef_id,
                                 (unsigned long long)plan.game_time_ms,
                                 (long long)game_ms);
                    w->stale_logged = 1;
                }
            } else {
                w->stale_logged = 0;
            }
        } else {
            plan_matches = 0;
            nav_stale = 1;
        }

        {
            int slot_due = now >= w->next_slot_ms;
            int publish_allowed = now >= w->stop_hold_until_ms;
            int task_ended = 0;
            kos_macro_output out;

            memset(&out, 0, sizeof out);
            out.action = KOS_MACRO_ACT_STOP;

            if (w->have_task && have_world) {
                if (plan_ok && plan_fresh && plan_matches &&
                    plan.status == KOS_NAV_BLOCKED) {
                    char detail[KOS_MQ_DETAIL_MAX];

                    snprintf(detail, sizeof detail, "navd blocked: %s",
                             w->current.station_id);
                    kos_log_warn(w->rt.log, "task_blocked",
                                 "task=%llu station=%s detail=%s",
                                 (unsigned long long)w->current.task_id,
                                 w->current.station_id, detail);
                    finish_task(w, KOS_EVT_BLOCKED, detail);
                    task_ended = 1;
                } else {
                    int adv = advance_macro(w, &world, game_ms, &plan, plan_fresh,
                                            plan_matches, nav_stale, &out);

                    if (adv == 2) {
                        task_ended = 1;
                    } else if (adv == 1) {
                        force_publish = 1;
                        /* 让 STOP 至少活过两帧 Bridge（16.7 ms/帧 @60fps）。 */
                        w->stop_hold_until_ms = now + WORKER_STOP_HOLD_MS;
                    }
                }
            }

            if (task_ended) {
                publish_stop(w);
            } else if (w->have_task) {
                if (have_world) {
                    if (force_publish || (slot_due && publish_allowed)) {
                        publish_macro_action(w, &out, &plan, plan_fresh,
                                             plan_matches);
                    }
                } else if (slot_due && publish_allowed) {
                    publish_stop(w);
                }
            } else if (force_publish || (slot_due && publish_allowed)) {
                /* 没任务的人也可能被 navd 规划着让路。 */
                if (have_world && world.has_round_time && world.round_time_left > 0.0f &&
                    (world.round_time_left < world.round_time_limit || world.game_time_ms > 0) &&
                    plan_ok && plan_fresh &&
                    plan.status == KOS_NAV_MOVING && plan.waypoint_count > 0) {
                    publish_path(w, &plan);
                } else {
                    publish_stop(w);
                }
            }
            if (force_publish || (slot_due && publish_allowed) || task_ended) {
                w->next_slot_ms = now + w->rt.opts.slot_interval_ms;
            }
        }
        kos_child_sleep(&w->rt, WORKER_LOOP_MS);
    }

    if (w->nav != NULL) {
        kos_navtab_close(w->nav);
        w->nav = NULL;
    }
    close_queues(w);
    kos_child_teardown(&w->rt);
    return 0;
}

int main(int argc, char **argv)
{
    kos_child_opts opts;
    worker *w;
    int parsed;
    int rc;

    kos_child_opts_init(&opts, KOS_CHILD_WORKER);
    parsed = kos_child_parse_args(&opts, argc, argv);
    if (parsed == KOS_CHILD_ARG_HELP) {
        kos_child_print_usage(stdout, KOS_CHILD_WORKER);
        return 0;
    }
    if (parsed == KOS_CHILD_ARG_VERSION) {
        printf("kitchenos-worker 0.5.0\n");
        return 0;
    }
    if (parsed != KOS_CHILD_ARG_OK) {
        kos_child_print_usage(stderr, KOS_CHILD_WORKER);
        return 2;
    }
    w = calloc(1, sizeof *w);
    if (w == NULL) {
        return 1;
    }
    rc = worker_main(w, &opts);
    free(w);
    return rc;
}
