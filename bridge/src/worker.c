/*
 * worker.c -- Chef Worker：mq 收工序 → A* → 每帧写自己的输入槽 → 回事件。
 *
 * 它是执行器，不选单、不算优先级。输入槽是 Worker 独占写的唯一共享状态；世界状态
 * 只读（厨师位置从里面取）。没有任务时持续写 stop，所以 Bridge 的帧屏障永远能拿到
 * 一个有效槽，不会因为 Worker 空转而把厨师判成缺席。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/child.h>
#include <kitchenos/kitchen.h>
#include <kitchenos/log.h>
#include <kitchenos/mq.h>
#include <kitchenos/shm.h>
#include <kitchenos/worker_task.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WORKER_LOOP_MS 5

typedef struct {
    kos_child_runtime rt;
    kos_kitchen kitchen;
    char kitchen_path[KOS_DAEMON_PATH_MAX];
    mqd_t task_queue;
    mqd_t events_queue;
    int queues_ready;
    int mq_wait_logged;
    int recovery_sent;
    kos_mq_msg current;
    kos_worker_task task;
    int have_task;
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
        kos_log_error(w->rt.log, "kitchen_missing", "no --kitchen and no config/kitchen.example.json");
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
    if (w->rt.opts.generation > 1 && !w->recovery_sent) {
        /* Startup == "I have no task in hand"; the Scheduler turns this into a
         * requeue of whatever was RUNNING for this chef before the crash. The first
         * generation stays quiet: it has no prior task, and racing a just-dispatched
         * task with a requeue would only churn ids. */
        send_event(w, KOS_EVT_RECOVERED, "worker ready");
        kos_log_info(w->rt.log, "worker_recovered_sent", "chef=%d generation=%d",
                     w->rt.opts.chef_id, w->rt.opts.generation);
        w->recovery_sent = 1;
    }
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

static void chef_start_position(worker *w, float *x, float *z)
{
    kos_world_state world;
    int i;

    *x = w->kitchen.cells[0].x;
    *z = w->kitchen.cells[0].z;
    if (w->rt.shm != NULL &&
        kos_shm_world_read(w->rt.shm, &world, KOS_SHM_READ_ATTEMPTS, NULL) == KOS_OK) {
        for (i = 0; i < (int)world.chef_count; i++) {
            if (world.chefs[i].id == w->rt.opts.chef_id) {
                *x = world.chefs[i].x;
                *z = world.chefs[i].y; /* shm 的 y 是厨房平面的另一个轴 */
                return;
            }
        }
    }
    if (w->rt.opts.chef_id > 0 && w->kitchen.cell_count > 1) {
        *x = w->kitchen.cells[w->kitchen.cell_count - 1].x;
        *z = w->kitchen.cells[w->kitchen.cell_count - 1].z;
    }
}

static int start_task(worker *w, const kos_mq_msg *msg)
{
    kos_error err;
    float start_x;
    float start_z;
    int rc;
    char text[256];

    if (msg->version != KOS_MQ_VERSION || msg->kind != KOS_MQ_TASK ||
        msg->chef_id != w->rt.opts.chef_id) {
        kos_log_warn(w->rt.log, "task_bad_message",
                     "version=%u kind=%u chef=%d expected=%d", msg->version, msg->kind,
                     msg->chef_id, w->rt.opts.chef_id);
        return -1;
    }
    w->current = *msg;
    chef_start_position(w, &start_x, &start_z);
    kos_error_clear(&err);
    rc = kos_worker_task_init(&w->task, &w->kitchen, msg->station_id, msg->target_x,
                              msg->target_y, start_x, start_z, msg->duration_ms, now_ms(),
                              &err);
    if (rc != KOS_OK) {
        error_text(&err, text, sizeof text);
        kos_log_warn(w->rt.log, "task_path_failed", "task=%llu node=%s error=%s",
                     (unsigned long long)msg->task_id, msg->node_id, text);
        send_event(w, KOS_EVT_FAILED, "path not found");
        w->have_task = 0;
        return -1;
    }
    w->have_task = 1;
    kos_log_info(w->rt.log, "task_received",
                 "task=%llu order=%s node=%s op=%s station=%s duration_ms=%d start=%.2f,%.2f",
                 (unsigned long long)msg->task_id, msg->order_id, msg->node_id, msg->op,
                 msg->station_id, msg->duration_ms, (double)start_x, (double)start_z);
    send_event(w, KOS_EVT_ACCEPTED, "accepted");
    return 0;
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

static void publish_idle(worker *w)
{
    kos_input_action action;

    memset(&action, 0, sizeof action);
    action.kind = KOS_ACTION_STOP;
    publish_action(w, &action);
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

        if (kos_child_take_sighup()) {
            kos_log_info(w->rt.log, "signal", "name=SIGHUP action=ignored");
        }
        if (kos_child_take_sigusr1()) {
            kos_log_info(w->rt.log, "signal", "name=SIGUSR1 action=ignored");
        }
        if (!w->queues_ready) {
            (void)open_queues(w);
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

        if (w->have_task) {
            kos_input_action action;
            int rc = kos_worker_task_step(&w->task, now, &action);

            if (now >= w->next_slot_ms || rc != 0) {
                publish_action(w, &action);
                w->next_slot_ms = now + w->rt.opts.slot_interval_ms;
            }
            if (rc == 1) {
                kos_log_info(w->rt.log, "task_done", "task=%llu order=%s node=%s",
                             (unsigned long long)w->current.task_id, w->current.order_id,
                             w->current.node_id);
                send_event(w, KOS_EVT_DONE, "done");
                w->have_task = 0;
                publish_idle(w);
                w->next_slot_ms = now + w->rt.opts.slot_interval_ms;
            } else if (rc < 0) {
                send_event(w, KOS_EVT_FAILED, "task failed");
                w->have_task = 0;
                publish_idle(w);
            }
        } else if (now >= w->next_slot_ms) {
            publish_idle(w);
            w->next_slot_ms = now + w->rt.opts.slot_interval_ms;
        }

        kos_child_sleep(&w->rt, WORKER_LOOP_MS);
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
        printf("kitchenos-worker 0.3.0\n");
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
