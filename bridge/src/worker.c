/*
 * worker.c -- Chef Worker：mq 收工序 → 宏动作序列 → 导航表 → 输入槽 → 回事件。
 *
 * 它不选单、不规划路径：走位由 navd 统一算，Worker 只把"到哪个工位"写进目标槽、
 * 按计划槽里已放行的路点发 path。到站后把 Scheduler 的 op 展开成宏动作（macro.c），
 * 每一步按世界状态判完成：interact 只发一小段（约 250 ms）就停，等 holding/锅状态/
 * 进度变化；work 持续按住；等待期间发 stop，Bridge 的帧屏障始终拿到有效槽。
 *
 * 时间一律用世界状态的 game_time_ms：暂停时超时不走。navd 崩溃（计划槽写入时钟
 * 落后 500 ms）时先发 stop；正在 goto 的人暂停推进宏动作，等 navd 回来再继续。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/child.h>
#include <kitchenos/kitchen.h>
#include <kitchenos/log.h>
#include <kitchenos/macro.h>
#include <kitchenos/mq.h>
#include <kitchenos/navtab.h>
#include <kitchenos/shm.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WORKER_LOOP_MS 5
#define WORKER_NAV_STALE_MS 500
#define WORKER_NAV_ATTACH_LOG_MS 2000
#define WORKER_INTERACT_WINDOW_MS 250

typedef struct {
    kos_child_runtime rt;
    kos_kitchen kitchen;
    char kitchen_path[KOS_DAEMON_PATH_MAX];
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
    char text[256];

    if (w->rt.opts.chefs <= 0) {
        return -1;
    }
    if (kos_mq_task_name(w->rt.opts.chef_id, name, sizeof name) != KOS_OK) {
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
    if (kos_mq_open(KOS_MQ_EVENTS_NAME, O_WRONLY, &w->events_queue, &err) != KOS_OK) {
        kos_mq_close(w->task_queue);
        w->task_queue = (mqd_t)-1;
        if (!w->mq_wait_logged) {
            error_text(&err, text, sizeof text);
            kos_log_info(w->rt.log, "mq_wait", "queue=%s error=%s", KOS_MQ_EVENTS_NAME,
                         text);
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
        copy_str(target.station_id, sizeof target.station_id, w->current.station_id);
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

static void publish_macro_action(worker *w, const kos_macro_output *out, int64_t game_ms,
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
        int station = kos_kitchen_station_index(&w->kitchen, out->target_object);

        action.kind = KOS_ACTION_FACE;
        if (station >= 0) {
            /* 插件按资源键（pot_0/cutting_board_0）或 GameObject 名解析 target_object，
             * kitchen.json 的 id 两者都不一定是；face 支持坐标，直接给工位坐标。 */
            action.target_x = w->kitchen.stations[station].x;
            action.target_y = w->kitchen.stations[station].z;
        } else {
            copy_str(action.target_object, sizeof action.target_object,
                     out->target_object);
        }
        publish_action(w, &action);
        return;
    }
    if (out->action == KOS_MACRO_ACT_WORK) {
        action.kind = KOS_ACTION_WORK;
    } else {
        /* 交互只发一小段：ActionDriver 对同一条未完成的按键会去重，但完成后的
         * 每帧重发会重新按键；等状态变化期间发 stop 更接近旧 Worker 的 200 ms 脉冲。 */
        if (w->interact_window_step != w->macro.step_index ||
            game_ms > w->interact_until_game_ms) {
            publish_stop(w);
            return;
        }
        action.kind = KOS_ACTION_INTERACT;
    }
    copy_str(action.target_object, sizeof action.target_object, out->target_object);
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
    if (kos_macro_plan_build(&w->kitchen, msg->op, msg->node_id, msg->station_id,
                             msg->order_id, msg->duration_ms, &plan, &err) != KOS_OK) {
        char detail[KOS_MQ_DETAIL_MAX];

        error_text(&err, text, sizeof text);
        kos_log_warn(w->rt.log, "macro_map_failed",
                     "task=%llu op=%s node=%s error=%s",
                     (unsigned long long)msg->task_id, msg->op, msg->node_id, text);
        snprintf(detail, sizeof detail, "macro-map op=%s", msg->op);
        send_event(w, KOS_EVT_FAILED, detail);
        return -1;
    }
    kos_macro_begin(&w->macro, &plan);
    w->target_seq++;
    w->pinned = 0;
    w->stale_logged = 0;
    w->interact_until_game_ms = 0;
    w->interact_window_step = -1;
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
    }
    if (out->action == KOS_MACRO_ACT_INTERACT &&
        w->interact_window_step != w->macro.step_index) {
        w->interact_window_step = w->macro.step_index;
        w->interact_until_game_ms = game_ms + WORKER_INTERACT_WINDOW_MS;
    }

    if (nav_arrived && !w->pinned) {
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
        if (kos_kitchen_load(w->kitchen_path, &w->kitchen, &err) != KOS_OK) {
            error_text(&err, text, sizeof text);
            kos_log_error(w->rt.log, "kitchen_load_failed", "path=%s error=%s",
                          w->kitchen_path, text);
            kos_child_teardown(&w->rt);
            return 1;
        }
        kos_log_info(w->rt.log, "config_loaded", "kitchen=%s cells=%d stations=%d",
                     w->kitchen_path, w->kitchen.cell_count, w->kitchen.station_count);
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
                    }
                }
            }

            if (task_ended) {
                publish_stop(w);
            } else if (w->have_task) {
                if (have_world) {
                    if (slot_due || force_publish) {
                        publish_macro_action(w, &out, game_ms, &plan, plan_fresh,
                                             plan_matches);
                    }
                } else if (slot_due) {
                    publish_stop(w);
                }
            } else if (slot_due || force_publish) {
                /* 没任务的人也可能被 navd 规划着让路。 */
                if (plan_ok && plan_fresh && plan.status == KOS_NAV_MOVING &&
                    plan.waypoint_count > 0) {
                    publish_path(w, &plan);
                } else {
                    publish_stop(w);
                }
            }
            if (slot_due || force_publish || task_ended) {
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
