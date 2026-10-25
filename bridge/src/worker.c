/*
 * worker.c -- Chef Worker：mq 收工序 → 写目标槽 → 读计划槽 → 每帧发 path。
 *
 * 它不再规划路径，也不发 move：走位由 navd 统一算，Worker 只把"要到哪个工位"
 * 写进导航表，然后按计划槽里已放行的路点发 path。到站后 pinned=1（navd 不再
 * 推它），交互、按工序工时等待，结果回 mq。
 *
 * 输入槽是 Worker 独占写的唯一共享状态；世界状态与导航表都只读。没有任务时也
 * 持续写 stop（或被 navd 推着走时写 path），Bridge 的帧屏障永远能拿到一个有效槽。
 * 时间一律用世界状态的 game_time_ms：暂停时交互/工时都不前进，navd 也不会误判
 * 卡住。navd 崩溃后计划槽的写入时钟落后 500 ms，这里先发 stop 等它回来。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/child.h>
#include <kitchenos/kitchen.h>
#include <kitchenos/log.h>
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
#define WORKER_INTERACT_MS 200
#define WORKER_NAV_STALE_MS 500
#define WORKER_NAV_ATTACH_LOG_MS 2000

typedef enum {
    WORKER_IDLE = 0,
    WORKER_NAVIGATING = 1,
    WORKER_INTERACTING = 2,
    WORKER_WORKING = 3
} worker_phase;

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
    worker_phase phase;
    uint32_t target_seq;
    int pinned;
    int stale_logged;
    int64_t interact_end_game_ms;
    int64_t work_end_game_ms;
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

static int start_task(worker *w, const kos_mq_msg *msg)
{
    if (msg->version != KOS_MQ_VERSION || msg->kind != KOS_MQ_TASK ||
        msg->chef_id != w->rt.opts.chef_id) {
        kos_log_warn(w->rt.log, "task_bad_message",
                     "version=%u kind=%u chef=%d expected=%d", msg->version, msg->kind,
                     msg->chef_id, w->rt.opts.chef_id);
        return -1;
    }
    if (kos_kitchen_station_index(&w->kitchen, msg->station_id) < 0) {
        kos_log_warn(w->rt.log, "task_bad_station", "task=%llu station=%s",
                     (unsigned long long)msg->task_id, msg->station_id);
        w->current = *msg;
        send_event(w, KOS_EVT_FAILED, "unknown station");
        return -1;
    }
    w->current = *msg;
    w->target_seq++;
    w->pinned = 0;
    w->phase = WORKER_NAVIGATING;
    w->stale_logged = 0;
    w->have_task = 1;
    write_target(w, 1, 0);
    kos_log_info(w->rt.log, "task_received",
                 "task=%llu order=%s node=%s op=%s station=%s duration_ms=%d target_seq=%u",
                 (unsigned long long)msg->task_id, msg->order_id, msg->node_id, msg->op,
                 msg->station_id, msg->duration_ms, w->target_seq);
    send_event(w, KOS_EVT_ACCEPTED, "accepted");
    return 0;
}

static void finish_task(worker *w, int event, const char *detail)
{
    send_event(w, event, detail);
    clear_target(w);
    w->have_task = 0;
    w->phase = WORKER_IDLE;
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

static void handle_plan(worker *w, int64_t game_ms)
{
    kos_nav_plan plan;
    int fresh;
    kos_input_action action;

    memset(&plan, 0, sizeof plan);
    if (w->nav == NULL || kos_navtab_read_plan(w->nav, w->rt.opts.chef_id, &plan) !=
                              KOS_OK) {
        publish_stop(w);
        return;
    }
    fresh = plan.game_time_ms + WORKER_NAV_STALE_MS >= (uint64_t)game_ms;
    if (!fresh) {
        if (!w->stale_logged) {
            kos_log_warn(w->rt.log, "nav_stale", "chef=%d plan_ms=%llu game_ms=%lld",
                         w->rt.opts.chef_id, (unsigned long long)plan.game_time_ms,
                         (long long)game_ms);
            w->stale_logged = 1;
        }
        publish_stop(w);
        return;
    }
    w->stale_logged = 0;

    if (w->have_task) {
        if (plan.target_seq == w->target_seq && plan.status == KOS_NAV_BLOCKED) {
            char detail[KOS_MQ_DETAIL_MAX];

            snprintf(detail, sizeof detail, "navd blocked: %s", w->current.station_id);
            kos_log_warn(w->rt.log, "task_blocked", "task=%llu station=%s detail=%s",
                         (unsigned long long)w->current.task_id, w->current.station_id,
                         detail);
            finish_task(w, KOS_EVT_BLOCKED, detail);
            publish_stop(w);
            return;
        }
        if (w->phase == WORKER_NAVIGATING && plan.target_seq == w->target_seq &&
            plan.status == KOS_NAV_ARRIVED) {
            w->pinned = 1;
            write_target(w, 1, 1);
            w->phase = WORKER_INTERACTING;
            w->interact_end_game_ms = game_ms + WORKER_INTERACT_MS;
        }
        if (w->phase == WORKER_INTERACTING) {
            memset(&action, 0, sizeof action);
            action.kind = KOS_ACTION_INTERACT;
            copy_str(action.target_object, sizeof action.target_object,
                     w->current.station_id);
            publish_action(w, &action);
            if (game_ms >= w->interact_end_game_ms) {
                w->phase = WORKER_WORKING;
                w->work_end_game_ms =
                    game_ms + (w->current.duration_ms > 0 ? w->current.duration_ms : 1);
            }
            return;
        }
        if (w->phase == WORKER_WORKING) {
            publish_stop(w);
            if (game_ms >= w->work_end_game_ms) {
                kos_log_info(w->rt.log, "task_done", "task=%llu order=%s node=%s",
                             (unsigned long long)w->current.task_id, w->current.order_id,
                             w->current.node_id);
                finish_task(w, KOS_EVT_DONE, "done");
            }
            return;
        }
        if (w->phase == WORKER_NAVIGATING && plan.target_seq == w->target_seq &&
            plan.status == KOS_NAV_MOVING && plan.waypoint_count > 0) {
            publish_path(w, &plan);
            return;
        }
        publish_stop(w);
        return;
    }

    /* 没任务也可能被 navd 规划着让路（目标就是自己所在格，可被推开）。 */
    if (plan.status == KOS_NAV_MOVING && plan.waypoint_count > 0) {
        publish_path(w, &plan);
    } else {
        publish_stop(w);
    }
}

static int worker_main(worker *w, const kos_child_opts *opts)
{
    int i;

    memset(w, 0, sizeof *w);
    w->rt.opts = *opts;
    w->task_queue = (mqd_t)-1;
    w->events_queue = (mqd_t)-1;

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
        int have_world;
        int64_t game_ms = 0;

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

        if (now >= w->next_slot_ms) {
            handle_plan(w, game_ms);
            w->next_slot_ms = now + w->rt.opts.slot_interval_ms;
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
        printf("kitchenos-worker 0.4.0\n");
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
