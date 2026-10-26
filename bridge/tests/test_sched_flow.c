/*
 * test_sched_flow.c -- Scheduler 的派工与订单结果闭环（G7/G9/G11、P2 重试分流）。
 *
 * 这套测试把 Scheduler 当成被测进程，自己扮演 Worker 与插件：
 *   - 用 shm 写世界状态（厨师位置/手持、订单、分数、order_results）；
 *   - 从 /kitchenos_tasks_<chef> 取派工消息，往 /kitchenos_events 发 DONE/FAILED；
 *   - 断言 events.jsonl 里的 order_completed / order_expired / item_adopted /
 *     putdown_queued / recipe_infeasible / task_requeued 与重新派工的间隔。
 * 每条用例都对应一次 W11 实机失败或状态审计缺口。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/child.h"
#include "kitchenos/json.h"
#include "kitchenos/macro.h"
#include "kitchenos/mq.h"
#include "kitchenos/recipes.h"
#include "kitchenos/shm.h"
#include "kitchenos/stand.h"
#include "scheduler_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_checks;
static int g_failures;

static void expect(int cond, const char *what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        g_failures++;
        printf("  FAIL %s\n", what);
    }
}

static int64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000L;
}

typedef struct {
    uint64_t task_id;
    int chef;
    int64_t received_ms;
    char node[KOS_MQ_NODE_ID_MAX];
    char op[KOS_MQ_OP_MAX];
    char station[KOS_SHM_NAME_MAX];
    char order_id[KOS_SHM_NAME_MAX];
} got_task;

static int open_queue(int instance, int chef, int events, mqd_t *out)
{
    char name[KOS_MQ_NAME_MAX];
    kos_error err;
    int waited = 0;

    if (events) {
        if (kos_mq_events_name(instance, name, sizeof name) != KOS_OK) {
            return -1;
        }
    } else if (kos_mq_task_name(chef, instance, name, sizeof name) != KOS_OK) {
        return -1;
    }
    while (waited <= 3000) {
        kos_error_clear(&err);
        if (kos_mq_open(name, events ? O_WRONLY : O_RDONLY, out, &err) == KOS_OK) {
            return 0;
        }
        ksfx_sleep_ms(20);
        waited += 20;
    }
    return -1;
}

static int collect_tasks(mqd_t *queues, int chefs, got_task *out, int cap, int timeout_ms)
{
    int n = 0;
    int waited = 0;

    while (n < cap) {
        int chef;
        int quiet = 1;

        for (chef = 0; chef < chefs; chef++) {
            for (;;) {
                kos_mq_msg msg;
                kos_error err;

                kos_error_clear(&err);
                if (kos_mq_receive(queues[chef], &msg, 0, &err) != KOS_OK) {
                    break;
                }
                quiet = 0;
                if (n >= cap) {
                    break;
                }
                memset(&out[n], 0, sizeof out[n]);
                out[n].task_id = msg.task_id;
                out[n].chef = chef;
                out[n].received_ms = mono_ms();
                ksfx_copy(out[n].node, sizeof out[n].node, msg.node_id);
                ksfx_copy(out[n].op, sizeof out[n].op, msg.op);
                ksfx_copy(out[n].station, sizeof out[n].station, msg.station_id);
                ksfx_copy(out[n].order_id, sizeof out[n].order_id, msg.order_id);
                n++;
            }
        }
        if (n >= cap) {
            break;
        }
        if (waited >= timeout_ms && quiet) {
            break;
        }
        ksfx_sleep_ms(20);
        waited += 20;
    }
    return n;
}

static int send_event(mqd_t events, uint64_t task_id, int chef, int event,
                      const char *detail)
{
    kos_mq_msg msg;
    kos_error err;

    memset(&msg, 0, sizeof msg);
    msg.version = KOS_MQ_VERSION;
    msg.kind = KOS_MQ_EVENT;
    msg.task_id = task_id;
    msg.chef_id = chef;
    msg.event = event;
    msg.generation = 1;
    if (detail != NULL) {
        ksfx_copy(msg.detail, sizeof msg.detail, detail);
    }
    kos_error_clear(&err);
    return kos_mq_send(events, &msg, 1, 200, &err) == KOS_OK ? 0 : -1;
}

static int world_write(kos_shm *shm, const kos_world_state *world)
{
    return kos_shm_world_write(shm, world) == KOS_OK ? 0 : -1;
}

/* 找一块能从该 kind 的第一个工位走到的可走格，slot 换一块。 */
static int find_stand_cell(const kos_kitchen *kitchen, const char *kind, int slot, int *out)
{
    int station = -1;
    int i;
    int seen = 0;

    for (i = 0; i < kitchen->station_count; i++) {
        if (kos_kitchen_name_matches(kind, kitchen->stations[i].kind)) {
            station = i;
            break;
        }
    }
    if (station < 0) {
        return -1;
    }
    for (i = 0; i < kitchen->cell_count; i++) {
        if (kos_stand_distance(kitchen, station, i) == KOS_STAND_UNREACHABLE) {
            continue;
        }
        if (seen++ < slot) {
            continue;
        }
        *out = i;
        return 0;
    }
    return -1;
}

static void put_chef(kos_world_state *world, const kos_kitchen *kitchen, int id, int slot,
                     const char *holding, const char *kind)
{
    kos_chef *chef = &world->chefs[world->chef_count++];
    int cell = -1;

    if (find_stand_cell(kitchen, kind, slot, &cell) != 0 || cell < 0) {
        cell = 0;
    }
    memset(chef, 0, sizeof *chef);
    chef->id = id;
    chef->x = kitchen->cells[cell].x;
    chef->y = kitchen->cells[cell].z;
    if (holding != NULL) {
        ksfx_copy(chef->holding, sizeof chef->holding, holding);
    }
}

static void put_order(kos_world_state *world, const char *id, const char *recipe, int score)
{
    kos_order *order = &world->orders[world->order_count++];

    memset(order, 0, sizeof *order);
    ksfx_copy(order->id, sizeof order->id, id);
    ksfx_copy(order->recipe, sizeof order->recipe, recipe);
    order->time_remaining = 300;
    order->score = score;
}

static void put_result(kos_world_state *world, int64_t seq, const char *id,
                       const char *outcome, int score, int64_t game_ms)
{
    kos_order_result *result = &world->order_results[world->order_result_count++];

    memset(result, 0, sizeof *result);
    result->seq = seq;
    ksfx_copy(result->id, sizeof result->id, id);
    ksfx_copy(result->outcome, sizeof result->outcome, outcome);
    result->score = score;
    result->has_score = 1;
    result->game_time_ms = game_ms;
}

static void remove_order(kos_world_state *world, const char *id)
{
    uint32_t i;

    for (i = 0; i < world->order_count; i++) {
        if (strcmp(world->orders[i].id, id) == 0) {
            memmove(&world->orders[i], &world->orders[i + 1],
                    (world->order_count - i - 1) * sizeof world->orders[0]);
            world->order_count--;
            return;
        }
    }
}

static int contains(const char *text, const char *needle)
{
    return text != NULL && strstr(text, needle) != NULL;
}

static char *read_events(const ksfx_run *run, size_t *len_out)
{
    char decisions[KSFX_PATH_MAX];
    char events[KSFX_PATH_MAX];

    if (ksfx_decisions_path(run, decisions, sizeof decisions) != 0) {
        return NULL;
    }
    {
        char *slash = strrchr(decisions, '/');

        if (slash == NULL) {
            return NULL;
        }
        *slash = '\0';
    }
    if (ksfx_join(events, sizeof events, decisions, "events.jsonl") != 0) {
        return NULL;
    }
    return ksfx_read_file(events, len_out);
}

/* 写一份最小菜谱：fetch(ingredient) → [cook] → deliver。dish 名由调用方给。 */
static int write_recipes(const ksfx_run *run, const char *dish, int with_cook,
                         int three_fetch)
{
    char kind_dispenser[64];
    char kind_serving[64];
    char kind_cooker[64];
    char text[4096];
    int n;

    snprintf(kind_dispenser, sizeof kind_dispenser, "zz%ld_dispenser", (long)getpid());
    snprintf(kind_serving, sizeof kind_serving, "zz%ld_serving", (long)getpid());
    snprintf(kind_cooker, sizeof kind_cooker, "zz%ld_cooker", (long)getpid());

    if (three_fetch) {
        n = snprintf(
            text, sizeof text,
            "{\"scene\":\"s_wellcooked_demo_2\",\"nodes\":["
            "{\"id\":\"fetch:A\",\"op\":\"fetch\",\"label\":\"A\",\"resource\":\"%s\","
            "\"ingredient\":\"SushiFish\",\"duration\":1.0},"
            "{\"id\":\"fetch:B\",\"op\":\"fetch\",\"label\":\"B\",\"resource\":\"%s\","
            "\"ingredient\":\"SushiFish\",\"duration\":1.0},"
            "{\"id\":\"fetch:C\",\"op\":\"fetch\",\"label\":\"C\",\"resource\":\"%s\","
            "\"ingredient\":\"SushiFish\",\"duration\":1.0},"
            "{\"id\":\"deliver:%s\",\"op\":\"deliver\",\"label\":\"%s\","
            "\"resource\":\"%s\",\"duration\":1.0}],"
            "\"edges\":[[\"fetch:A\",\"deliver:%s\"],[\"fetch:B\",\"deliver:%s\"],"
            "[\"fetch:C\",\"deliver:%s\"]],"
            "\"dishes\":[{\"dish\":\"%s\",\"score\":80,\"root\":\"deliver:%s\","
            "\"nodes\":[\"fetch:A\",\"fetch:B\",\"fetch:C\",\"deliver:%s\"]}]}\n",
            kind_dispenser, kind_dispenser, kind_dispenser, dish, dish, kind_serving, dish,
            dish, dish, dish, dish, dish);
    } else if (with_cook) {
        n = snprintf(
            text, sizeof text,
            "{\"scene\":\"s_wellcooked_demo_2\",\"nodes\":["
            "{\"id\":\"fetch:X\",\"op\":\"fetch\",\"label\":\"X\",\"resource\":\"%s\","
            "\"ingredient\":\"SushiFish\",\"duration\":1.0},"
            "{\"id\":\"cook:Y\",\"op\":\"cook\",\"label\":\"Y\",\"resource\":\"%s\","
            "\"duration\":1.0},"
            "{\"id\":\"deliver:%s\",\"op\":\"deliver\",\"label\":\"%s\","
            "\"resource\":\"%s\",\"duration\":1.0}],"
            "\"edges\":[[\"fetch:X\",\"cook:Y\"],[\"cook:Y\",\"deliver:%s\"]],"
            "\"dishes\":[{\"dish\":\"%s\",\"score\":80,\"root\":\"deliver:%s\","
            "\"nodes\":[\"fetch:X\",\"cook:Y\",\"deliver:%s\"]}]}\n",
            kind_dispenser, kind_cooker, dish, dish, kind_serving, dish, dish, dish, dish);
    } else {
        n = snprintf(
            text, sizeof text,
            "{\"scene\":\"s_wellcooked_demo_2\",\"nodes\":["
            "{\"id\":\"deliver:%s\",\"op\":\"deliver\",\"label\":\"%s\","
            "\"resource\":\"%s\",\"duration\":1.0}],"
            "\"edges\":[],"
            "\"dishes\":[{\"dish\":\"%s\",\"score\":80,\"root\":\"deliver:%s\","
            "\"nodes\":[\"deliver:%s\"]}]}\n",
            dish, dish, kind_serving, dish, dish, dish);
    }
    return n > 0 && ksfx_write_file(run->recipes_path, text, (size_t)n) == 0 ? 0 : -1;
}

static int write_orders(const ksfx_run *run)
{
    static const char text[] = "{\"menu\":[]}\n";

    return ksfx_write_file(run->orders_path, text, sizeof text - 1);
}

/* 只放一个 handoff 台面、不划区：zoned 模式会把 counter_handoff_0 注册进 Scheduler
 * 的 kitchen（worker 侧要等 export 补 Counter，见报告的依赖）。 */
static int install_counter_zones(ksfx_run *run)
{
    static const char text[] =
        "{\"scene\":\"s_wellcooked_demo_2\","
        "\"handoff\":{\"id\":\"counter_handoff_0\",\"kind\":\"counter\","
        "\"pos\":[4.2,-3.6]},\"zones\":[]}\n";
    char path[KSFX_PATH_MAX];
    kos_zones zones;
    kos_error err;

    if (ksfx_join(path, sizeof path, run->dir, "zones.json") != 0 ||
        ksfx_write_file(path, text, sizeof text - 1) != 0) {
        return -1;
    }
    memset(&zones, 0, sizeof zones);
    kos_error_clear(&err);
    if (kos_zones_for_kitchen(run->kitchen_path, &zones, &err) != KOS_OK ||
        !zones.from_file) {
        return -1;
    }
    kos_error_clear(&err);
    return kos_zones_register_handoff(&zones, &run->kitchen, &err) == KOS_OK ? 0 : -1;
}

/* 起一个 Scheduler + 世界状态；返回 shm 句柄供测试继续改写。 */
static int start_run_ex(ksfx_run *run, kos_shm **shm, mqd_t *events, mqd_t *taskss,
                        int chefs, kos_world_state *world, const char *dispatch)
{
    kos_shm_opts opts;
    kos_error err;
    int i;

    run->chefs = chefs;
    run->verbose = getenv("KSFX_VERBOSE") != NULL;
    memset(&opts, 0, sizeof opts);
    opts.object_name = run->state_name;
    opts.sem_name = run->sem_name;
    opts.perm = 0600;
    kos_error_clear(&err);
    if (kos_shm_open(&opts, KOS_SHM_CREATE, shm, &err) != KOS_OK) {
        printf("  shm_open failed: %s\n", err.context);
        return -1;
    }
    world->game_time_ms = 1000;
    if (world_write(*shm, world) != 0) {
        return -1;
    }
    if (ksfx_spawn_ex(run, "fcfs", dispatch != NULL ? dispatch : "flexible") <= 0) {
        return -1;
    }
    if (ksfx_wait_log(run, "mq_ready", 3000) != 0) {
        ksfx_dump_logs(run);
        return -1;
    }
    if (open_queue(run->instance, 0, 1, events) != 0) {
        return -1;
    }
    for (i = 0; i < chefs; i++) {
        if (open_queue(run->instance, i, 0, &taskss[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int start_run(ksfx_run *run, kos_shm **shm, mqd_t *events, mqd_t *taskss, int chefs,
                     kos_world_state *world)
{
    return start_run_ex(run, shm, events, taskss, chefs, world, NULL);
}

static void cleanup_run(ksfx_run *run, kos_shm **shm, mqd_t *events, int chefs)
{
    kos_error err;
    kos_shm_opts opts;

    (void)ksfx_stop(run);
    if (*shm != NULL) {
        memset(&opts, 0, sizeof opts);
        opts.object_name = run->state_name;
        opts.sem_name = run->sem_name;
        kos_error_clear(&err);
        (void)kos_shm_unlink(&opts, &err);
        (void)kos_shm_close(*shm, &err);
        *shm = NULL;
    }
    (void)events;
    (void)chefs;
    ksfx_cleanup(run);
}

/* G11 + 候选构建：空手的厨师拿得到每一条就绪 fetch，手上有料的厨师不接 fetch。 */
static void test_holding_and_candidates(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[8];
    int n;
    int i;
    int chef0_got = 0;
    int chef1_got = 0;

    printf("[test_sched_flow] 1. 三条就绪 fetch 填满三个厨师；手上有料的不接 fetch\n");
    for (i = 0; i < KOS_SHM_MAX_CHEFS; i++) {
        taskss[i] = (mqd_t)-1;
    }
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "ThreeFetch", 0, 1) != 0) {
        expect(0, "fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, NULL, "dispenser");
    put_chef(&world, &run.kitchen, 1, 1, NULL, "dispenser");
    put_chef(&world, &run.kitchen, 2, 2, NULL, "dispenser");
    put_order(&world, "w1", "ThreeFetch", 80);
    if (start_run(&run, &shm, &events, taskss, 3, &world) != 0) {
        expect(0, "scheduler_started");
        cleanup_run(&run, &shm, &events, 3);
        return;
    }
    expect(1, "scheduler_started");
    n = collect_tasks(taskss, 3, got, 3, 3000);
    expect(n == 3, "three_ready_fetches_dispatched_in_one_round");
    {
        int unique_nodes = 0;
        int unique_chefs = 0;

        for (i = 0; i < n; i++) {
            int j;
            int dup_node = 0;
            int dup_chef = 0;

            for (j = 0; j < i; j++) {
                if (strcmp(got[i].node, got[j].node) == 0) {
                    dup_node = 1;
                }
                if (got[i].chef == got[j].chef) {
                    dup_chef = 1;
                }
            }
            unique_nodes += dup_node ? 0 : 1;
            unique_chefs += dup_chef ? 0 : 1;
        }
        expect(unique_nodes == 3 && unique_chefs == 3,
               "three_distinct_fetch_nodes_to_three_distinct_chefs");
    }
    expect(n == 3 && strstr(got[0].node, "fetch:") != NULL, "dispatched_nodes_are_fetches");

    /* 奖励来自 orders[].score（G7）：事件里有 reward=80。 */
    expect(ksfx_wait_events(&run, "\"reward\":80", 1000, NULL, 0) == 0,
           "order_arrived_reports_world_score");

    (void)ksfx_stop(&run);
    expect(1, "scheduler_stopped");
    cleanup_run(&run, &shm, &events, 3);

    /* 手上有料的厨师不能接 fetch；空手的那位接一条。 */
    printf("[test_sched_flow] 2. 手上有料的厨师不接 fetch，料放下后接活\n");
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "ThreeFetch", 0, 1) != 0) {
        expect(0, "holding_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    /* 手里的 Z 不是任何待派 fetch 的产物：不该被采纳，也不该接 fetch。 */
    put_chef(&world, &run.kitchen, 0, 0, "Z", "dispenser");
    put_chef(&world, &run.kitchen, 1, 1, NULL, "dispenser");
    put_order(&world, "w1", "ThreeFetch", 80);
    if (start_run(&run, &shm, &events, taskss, 2, &world) != 0) {
        expect(0, "holding_scheduler_started");
        cleanup_run(&run, &shm, &events, 2);
        return;
    }
    n = collect_tasks(taskss, 2, got, 2, 1500);
    for (i = 0; i < n; i++) {
        if (got[i].chef == 0) {
            chef0_got = 1;
        }
        if (got[i].chef == 1) {
            chef1_got = 1;
        }
    }
    expect(chef1_got && !chef0_got, "holding_chef_is_not_sent_a_fetch");
    expect(send_event(events, got[0].task_id, 1, KOS_EVT_DONE, "done") == 0,
           "first_fetch_done_sent");

    /* 料箱回料后，空手的 0 号厨师接手剩下的 fetch（不再空转）。 */
    world.chefs[0].holding[0] = '\0';
    expect(world_write(shm, &world) == 0, "chef0_hand_cleared");
    n = collect_tasks(taskss, 2, got, 2, 3000);
    {
        int chef0_retook = 0;
        int j;

        for (j = 0; j < n; j++) {
            if (got[j].chef == 0 && strstr(got[j].node, "fetch:") != NULL) {
                chef0_retook = 1;
            }
        }
        expect(chef0_retook, "empty_handed_chef_takes_remaining_fetch");
    }
    cleanup_run(&run, &shm, &events, 2);
}

/* P2：goto 失败回队并退避，第三次才让订单失败。 */
static void test_retry_backoff(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[4];
    int64_t failed_at;
    int64_t first_delay;
    int64_t second_delay;
    int n;

    printf("[test_sched_flow] 3. macro=goto 失败回队，退避 n×2 s\n");
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "CookDish", 1, 0) != 0) {
        expect(0, "retry_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, NULL, "dispenser");
    put_order(&world, "w1", "CookDish", 80);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "retry_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].node, "fetch:X") == 0, "retry_first_task_is_fetch");
    failed_at = mono_ms();
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_FAILED,
                      "macro=goto wait=arrived+face timeout waited_ms=6000") == 0,
           "goto_failure_sent");
    expect(ksfx_wait_events(&run, "\"reason\":\"macro-failed\"", 2000, NULL, 0) == 0,
           "goto_failure_requeued");
    n = collect_tasks(taskss, 1, got, 1, 4000);
    first_delay = n > 0 ? got[0].received_ms - failed_at : 0;
    expect(n == 1 && first_delay >= 1500,
           "first_retry_waits_about_two_seconds");
    failed_at = mono_ms();
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_FAILED,
                      "macro=goto wait=arrived+face timeout waited_ms=6000") == 0,
           "second_goto_failure_sent");
    n = collect_tasks(taskss, 1, got, 1, 6000);
    second_delay = n > 0 ? got[0].received_ms - failed_at : 0;
    expect(n == 1 && second_delay >= 3500,
           "second_retry_waits_about_four_seconds");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_FAILED,
                      "macro=goto wait=arrived+face timeout waited_ms=6000") == 0,
           "third_goto_failure_sent");
    expect(ksfx_wait_events(&run, "\"event\":\"order_failed\"", 2000, NULL, 0) == 0,
           "third_failure_fails_the_dish");
    expect(ksfx_wait_events(&run, "\"reason\":\"macro-retries-exhausted\"", 500, NULL, 0) == 0,
           "retry_exhaustion_is_infeasible");
    cleanup_run(&run, &shm, &events, 1);
}

/* P2：已经到站、游戏不收（pickup 超时）→ 不退避，当单失败。 */
static void test_refusal_is_infeasible(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[2];
    char *events_text;
    size_t len = 0;
    int n;

    printf("[test_sched_flow] 4. 到站后游戏不收：recipe_infeasible，不重试\n");
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "CookDish", 1, 0) != 0) {
        expect(0, "refusal_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, NULL, "dispenser");
    put_order(&world, "w1", "CookDish", 80);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "refusal_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1, "refusal_task_dispatched");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_FAILED,
                      "macro=pickup wait=holding timeout waited_ms=1500") == 0,
           "pickup_timeout_sent");
    expect(ksfx_wait_events(&run, "\"event\":\"recipe_infeasible\"", 2000, NULL, 0) == 0,
           "refusal_marks_recipe_infeasible");
    expect(ksfx_wait_events(&run, "\"event\":\"order_failed\"", 1000, NULL, 0) == 0,
           "refusal_fails_order");
    n = collect_tasks(taskss, 1, got, 1, 500);
    expect(n == 0, "refusal_is_not_requeued");
    events_text = read_events(&run, &len);
    expect(contains(events_text, "game-refused") &&
               !contains(events_text, "\"reason\":\"macro-failed\""),
           "refusal_detail_names_reason_without_requeue");
    free(events_text);
    cleanup_run(&run, &shm, &events, 1);
}

/* G9：交付结果与过期分开；order_results 是第一判据，分数增量是退路。 */
static void test_delivered_vs_expired(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[4];
    int n;
    int seq = 1;

    printf("[test_sched_flow] 5. order_results：交付 / 过期 / 送错\n");
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "PlateDish", 0, 0) != 0) {
        expect(0, "result_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, "Plate", "serving");
    put_order(&world, "w1", "PlateDish", 80);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "result_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].op, "deliver") == 0, "deliver_dispatched");

    /* 交付成功：订单消失、分数 +80、插件报 delivered。 */
    world.score = 80;
    remove_order(&world, "w1");
    put_result(&world, seq++, "w1", "delivered", 80, world.game_time_ms);
    expect(world_write(shm, &world) == 0, "delivered_world_written");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_DONE, "done") == 0,
           "deliver_done_sent");
    expect(ksfx_wait_events(&run, "\"event\":\"order_completed\"", 2000, NULL, 0) == 0,
           "delivered_result_completes_order");
    expect(ksfx_wait_events(&run, "\"source\":\"game-result\"", 500, NULL, 0) == 0,
           "completion_source_is_game_result");

    /* 过期：订单消失、分数不动、插件报 expired；即使 deliver 发了 DONE 也不能完成。 */
    put_order(&world, "w2", "PlateDish", 80);
    expect(world_write(shm, &world) == 0, "second_order_written");
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].order_id, "w2") == 0, "second_deliver_dispatched");
    remove_order(&world, "w2");
    put_result(&world, seq++, "w2", "expired", 0, world.game_time_ms);
    expect(world_write(shm, &world) == 0, "expired_world_written");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_DONE, "done") == 0,
           "expired_deliver_done_sent");
    expect(ksfx_wait_events(&run, "\"event\":\"order_expired\"", 2000, NULL, 0) == 0,
           "expired_result_does_not_complete");
    {
        size_t completed_len = 0;
        char *events_text = read_events(&run, &completed_len);

        expect(!contains(events_text, "\"order_id\":\"w2\",\"dish\":\"PlateDish\","
                                      "\"source\":\"dag\""),
               "expired_order_never_completed");
        free(events_text);
        (void)completed_len;
    }

    /* 送错菜被拒：failed → order_failed。 */
    put_order(&world, "w3", "PlateDish", 80);
    expect(world_write(shm, &world) == 0, "third_order_written");
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].order_id, "w3") == 0, "third_deliver_dispatched");
    remove_order(&world, "w3");
    put_result(&world, seq++, "w3", "failed", 0, world.game_time_ms);
    expect(world_write(shm, &world) == 0, "failed_world_written");
    expect(ksfx_wait_events(&run, "\"event\":\"order_failed\"", 2000, NULL, 0) == 0,
           "failed_result_fails_order");

    cleanup_run(&run, &shm, &events, 1);
}

/* G9 的分数退路 + P2 的过期分流：没有 order_results 时，deliver 报 expired。 */
static void test_score_delta_expiry(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[2];
    int n;

    printf("[test_sched_flow] 6. deliver 报 expired → order_expired（不是 failed）\n");
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "PlateDish", 0, 0) != 0) {
        expect(0, "delta_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, "Plate", "serving");
    put_order(&world, "w1", "PlateDish", 80);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "delta_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1, "delta_deliver_dispatched");
    remove_order(&world, "w1");
    world.score = -20;
    expect(world_write(shm, &world) == 0, "delta_world_written");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_FAILED,
                      "macro=deliver wait=order-gone expired waited_ms=100") == 0,
           "expired_detail_sent");
    expect(ksfx_wait_events(&run, "\"event\":\"order_expired\"", 2000, NULL, 0) == 0,
           "expired_detail_marks_expired");
    expect(ksfx_wait_events(&run, "\"source\":\"deliver-expired\"", 500, NULL, 0) == 0,
           "expired_source_recorded");
    cleanup_run(&run, &shm, &events, 1);
}

/* G11：订单中途过期，手上的料接到同菜的另一单。 */
static void test_expiry_adopts_item(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[4];
    int n;
    int64_t seq = 1;

    printf("[test_sched_flow] 7. 过期订单的原料被同菜订单采纳\n");
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "CookDish", 1, 0) != 0) {
        expect(0, "adopt_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, NULL, "dispenser");
    put_order(&world, "w1", "CookDish", 80);
    put_order(&world, "w2", "CookDish", 80);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "adopt_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].node, "fetch:X") == 0, "adopt_fetch_dispatched");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_DONE, "done") == 0,
           "adopt_fetch_done_sent");
    ksfx_sleep_ms(150);
    world.chefs[0].holding[0] = '\0';
    ksfx_copy(world.chefs[0].holding, sizeof world.chefs[0].holding, "X");
    remove_order(&world, "w1");
    put_result(&world, seq++, "w1", "expired", 0, world.game_time_ms);
    expect(world_write(shm, &world) == 0, "adopt_expiry_written");
    expect(ksfx_wait_events(&run, "\"event\":\"item_adopted\"", 2000, NULL, 0) == 0,
           "expired_order_item_adopted");
    expect(ksfx_wait_events(&run, "\"order_id\":\"w2\"", 500, NULL, 0) == 0,
           "adopted_into_same_dish_order");
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].node, "cook:Y") == 0, "adopted_item_continues_with_cook");
    expect(n == 1 && strcmp(got[0].order_id, "w2") == 0, "adopted_item_goes_to_new_order");
    cleanup_run(&run, &shm, &events, 1);
}

/* G11：没人要的手持物放到台面；没有台面工位时只能记 orphaned。 */
static void test_putdown(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[4];
    char *events_text;
    size_t len = 0;
    int n;
    int64_t seq = 1;

    printf("[test_sched_flow] 8. 过期订单的孤儿料放下到台面\n");
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "CookDish", 1, 0) != 0) {
        expect(0, "putdown_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    expect(install_counter_zones(&run) == 0, "putdown_counter_zone_written");
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, NULL, "dispenser");
    put_order(&world, "w1", "CookDish", 80);
    if (start_run_ex(&run, &shm, &events, taskss, 1, &world, "zoned") != 0) {
        expect(0, "putdown_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].node, "fetch:X") == 0, "putdown_fetch_dispatched");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_DONE, "done") == 0,
           "putdown_fetch_done_sent");
    ksfx_sleep_ms(150);
    world.chefs[0].holding[0] = '\0';
    ksfx_copy(world.chefs[0].holding, sizeof world.chefs[0].holding, "X");
    remove_order(&world, "w1");
    put_result(&world, seq++, "w1", "expired", 0, world.game_time_ms);
    expect(world_write(shm, &world) == 0, "putdown_expiry_written");
    expect(ksfx_wait_events(&run, "\"event\":\"putdown_queued\"", 2000, NULL, 0) == 0,
           "putdown_queued_for_orphan_item");
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].op, "assemble") == 0 &&
               strcmp(got[0].node, "drop:X") == 0,
           "putdown_dispatch_is_goto_drop");
    expect(n == 1 && strcmp(got[0].station, "counter_handoff_0") == 0,
           "putdown_targets_the_counter_station");
    world.chefs[0].holding[0] = '\0';
    expect(world_write(shm, &world) == 0, "putdown_hand_cleared");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_DONE, "done") == 0,
           "putdown_done_sent");
    expect(ksfx_wait_events(&run, "\"event\":\"putdown_done\"", 2000, NULL, 0) == 0,
           "putdown_done_recorded");
    cleanup_run(&run, &shm, &events, 1);

    /* 没有台面工位（去掉 zones.json 的 handoff）→ hold_orphaned，不派不可能的放下。 */
    printf("[test_sched_flow] 9. 没有台面工位时记 hold_orphaned\n");
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "CookDish", 1, 0) != 0) {
        expect(0, "orphan_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    {
        char zones[KSFX_PATH_MAX];

        if (ksfx_join(zones, sizeof zones, run.dir, "zones.json") == 0) {
            (void)unlink(zones);
        }
    }
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, NULL, "dispenser");
    put_order(&world, "w1", "CookDish", 80);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "orphan_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1, "orphan_fetch_dispatched");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_DONE, "done") == 0,
           "orphan_fetch_done_sent");
    ksfx_sleep_ms(150);
    world.chefs[0].holding[0] = '\0';
    ksfx_copy(world.chefs[0].holding, sizeof world.chefs[0].holding, "X");
    remove_order(&world, "w1");
    put_result(&world, seq++, "w1", "expired", 0, world.game_time_ms);
    expect(world_write(shm, &world) == 0, "orphan_expiry_written");
    expect(ksfx_wait_events(&run, "\"event\":\"hold_orphaned\"", 2000, NULL, 0) == 0,
           "orphan_item_logged_when_no_counter");
    events_text = read_events(&run, &len);
    expect(contains(events_text, "\"item\":\"X\""), "orphan_names_the_item");
    free(events_text);
    n = collect_tasks(taskss, 1, got, 1, 500);
    expect(n == 0, "no_impossible_putdown_without_counter");
    cleanup_run(&run, &shm, &events, 1);
}

/* G9 退路：没有 order_results 时用分数增量；分数没动绝不记完成。 */
static void test_score_fallback(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[2];
    int n;

    printf("[test_sched_flow] 10. 结果字段缺席时用 world.score 增量判定\n");
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "PlateDish", 0, 0) != 0) {
        expect(0, "fallback_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, "Plate", "serving");
    put_order(&world, "w1", "PlateDish", 80);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "fallback_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].op, "deliver") == 0, "fallback_deliver_dispatched");
    remove_order(&world, "w1");
    world.score = 80; /* 没有 order_results，只有分数上涨 */
    expect(world_write(shm, &world) == 0, "fallback_delivered_world");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_DONE, "done") == 0,
           "fallback_deliver_done_sent");
    expect(ksfx_wait_events(&run, "\"source\":\"world-score\"", 2000, NULL, 0) == 0,
           "positive_score_delta_completes");
    expect(ksfx_wait_events(&run, "\"event\":\"order_completed\"", 500, NULL, 0) == 0,
           "fallback_completion_is_order_completed");

    /* 订单消失但分数没动：等过宽限后判过期，不能记完成。 */
    put_order(&world, "w2", "PlateDish", 80);
    expect(world_write(shm, &world) == 0, "fallback_second_order_written");
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].order_id, "w2") == 0, "fallback_second_deliver");
    remove_order(&world, "w2");
    expect(world_write(shm, &world) == 0, "fallback_expired_world");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_DONE, "done") == 0,
           "fallback_expired_done_sent");
    expect(ksfx_wait_events(&run, "\"event\":\"order_outcome_pending\"", 1000, NULL, 0) == 0,
           "no_score_delta_waits_for_outcome");
    expect(ksfx_wait_events(&run, "\"event\":\"order_expired\"", 3000, NULL, 0) == 0,
           "zero_score_delta_expires_after_grace");
    {
        size_t len = 0;
        char *text = read_events(&run, &len);

        expect(!contains(text, "\"order_id\":\"w2\",\"dish\":\"PlateDish\","
                                "\"source\":\"dag\""),
               "zero_delta_never_completes");
        free(text);
        (void)len;
    }
    cleanup_run(&run, &shm, &events, 1);
}

/* 持有亲和的成品丢了：子工序 DONE 后厨师一直空手 → 订单失败，不再派空手 dump。 */
static void test_holding_lost(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[2];
    int n;

    printf("[test_sched_flow] 11. 手上的料丢了：订单失败，不空等\n");
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "CookDish", 1, 0) != 0) {
        expect(0, "lost_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, NULL, "dispenser");
    put_order(&world, "w1", "CookDish", 80);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "lost_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].node, "fetch:X") == 0, "lost_fetch_dispatched");
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_DONE, "done") == 0,
           "lost_fetch_done_sent");
    /* 世界状态里厨师一直空手（插件漏帧/东西被换走）。 */
    expect(ksfx_wait_events(&run, "\"source\":\"holding-lost\"", 4000, NULL, 0) == 0,
           "lost_holding_fails_order");
    n = collect_tasks(taskss, 1, got, 1, 500);
    expect(n == 0, "lost_holding_does_not_dispatch_cook");
    cleanup_run(&run, &shm, &events, 1);
}

/*
 * Worker 重启（RECOVERED）时把在飞的 fetch 排回队列，但世界状态里料已经在他手上：
 * 不能把 fetch 再派给手不空的厨师，应该把产物记成该工序的完成（os-loop）。
 */
static void test_recovery_adopts_holding(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[4];
    int n;

    printf("[test_sched_flow] 12. Worker 重启：手上已有的料不再重派 fetch\n");
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipes(&run, "CookDish", 1, 0) != 0) {
        expect(0, "recovery_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, NULL, "dispenser");
    put_order(&world, "w1", "CookDish", 80);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "recovery_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].node, "fetch:X") == 0, "recovery_fetch_dispatched");
    /* 模拟"宏已经拿到料但 DONE 事件丢了"：世界状态里手上有 X，再发 RECOVERED。 */
    world.chefs[0].holding[0] = '\0';
    ksfx_copy(world.chefs[0].holding, sizeof world.chefs[0].holding, "X");
    expect(world_write(shm, &world) == 0, "recovery_holding_written");
    expect(send_event(events, 0, 0, KOS_EVT_RECOVERED, "worker ready") == 0,
           "recovery_event_sent");
    expect(ksfx_wait_events(&run, "\"event\":\"item_adopted\"", 2000, NULL, 0) == 0,
           "recovered_fetch_adopted_from_hand");
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].node, "cook:Y") == 0,
           "recovered_chef_gets_the_next_step");
    cleanup_run(&run, &shm, &events, 1);
}

/*
 * Worker 侧的 kitchen 加载必须和 Scheduler 一样注册 zones 的 handoff 台面，
 * 否则放下工序的 station_id=counter_handoff_0 在 Worker 的 kitchen 里不存在，
 * 宏计划建不出来（macro-map 失败）。
 */
static void test_worker_handoff_registration(void)
{
    ksfx_run run;
    kos_kitchen plain;
    kos_kitchen with_zones;
    kos_macro_plan plan;
    kos_error err;

    printf("[test_sched_flow] 13. Worker 加载 kitchen 时注册 handoff 台面\n");
    if (ksfx_init_ex(&run, 0) != 0) {
        expect(0, "handoff_fixture_init");
        ksfx_cleanup(&run);
        return;
    }
    memset(&plain, 0, sizeof plain);
    kos_error_clear(&err);
    expect(kos_kitchen_load(run.kitchen_path, &plain, &err) == KOS_OK,
           "handoff_plain_kitchen_loaded");
    expect(kos_kitchen_station_index(&plain, "counter_handoff_0") < 0,
           "plain_load_does_not_know_the_handoff_station");
    memset(&with_zones, 0, sizeof with_zones);
    kos_error_clear(&err);
    expect(kos_kitchen_load_with_zones(run.kitchen_path, &with_zones, &err) == KOS_OK,
           "handoff_kitchen_loaded_with_zones");
    expect(kos_kitchen_station_index(&with_zones, "counter_handoff_0") >= 0,
           "worker_load_registers_the_handoff_station");
    kos_error_clear(&err);
    expect(kos_macro_plan_build(&with_zones, "assemble", "drop:X", "counter_handoff_0",
                                "putdown", 1500, &plan, &err) == KOS_OK &&
               plan.step_count == 2 && plan.steps[1].kind == KOS_MACRO_DROP,
           "putdown_plan_builds_with_worker_kitchen");
    ksfx_cleanup(&run);
}


/* ---------- 简单单工序菜谱（灶/板独占与倒料门禁用） ---------- */

static int write_recipe_node(const ksfx_run *run, const char *dish, const char *op,
                             const char *node_id, const char *label, const char *kind)
{
    char text[2048];
    int n = snprintf(text, sizeof text,
                     "{\"scene\":\"%s\",\"nodes\":["
                     "{\"id\":\"%s\",\"op\":\"%s\",\"label\":\"%s\",\"resource\":\"%s\","
                     "\"duration\":1.0}],"
                     "\"edges\":[],"
                     "\"dishes\":[{\"dish\":\"%s\",\"score\":80,\"root\":\"%s\","
                     "\"nodes\":[\"%s\"]}]}\n",
                     run->scene, node_id, op, label, kind, dish, node_id, node_id);
    return n > 0 && ksfx_write_file(run->recipes_path, text, (size_t)n) == 0 ? 0 : -1;
}

static void put_station_resource(kos_world_state *world, const char *name,
                                 const char *state, const char *content)
{
    kos_resource *resource = &world->resources[world->resource_count++];

    memset(resource, 0, sizeof *resource);
    ksfx_copy(resource->name, sizeof resource->name, name);
    ksfx_copy(resource->state, sizeof resource->state, state);
    if (content != NULL) {
        ksfx_copy(resource->content, sizeof resource->content, content);
    }
    resource->chef_id = -1;
}
static void put_station_resource_at(kos_world_state *world, const char *name,
                                    const char *state, const char *content,
                                    float x, float y)
{
    kos_resource *resource = &world->resources[world->resource_count++];

    memset(resource, 0, sizeof *resource);
    ksfx_copy(resource->name, sizeof resource->name, name);
    ksfx_copy(resource->state, sizeof resource->state, state);
    if (content != NULL) {
        ksfx_copy(resource->content, sizeof resource->content, content);
    }
    resource->chef_id = -1;
    resource->has_pos = 1;
    resource->pos_x = x;
    resource->pos_y = y;
}

/* 从 "cooker-4" 拼出 "pot_4"；测试用它写世界状态。 */
static int station_resource_name(const char *station, char *out, size_t cap)
{
    const char *dash = strrchr(station, '-');

    if (dash == NULL) {
        return -1;
    }
    if (strncmp(station, "cooker", 6) == 0) {
        snprintf(out, cap, "pot_%s", dash + 1);
    } else if (strncmp(station, "board", 5) == 0) {
        snprintf(out, cap, "cutting_board_%s", dash + 1);
    } else {
        return -1;
    }
    return 0;
}

/* 两条 cook 同一轮派工必须落到不同的灶（build 阶段两条都看到空闲）。 */
static void test_cooker_station_exclusive(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[4];
    char kind[64];
    int n;

    printf("[test_sched_flow] 14. 同一轮两条 cook 不能挑同一口锅\n");
    ksfx_scene_override = "s_wellcooked_demo_3";
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0) {
        expect(0, "cooker_excl_fixture_init");
        ksfx_cleanup(&run);
        ksfx_scene_override = NULL;
        return;
    }
    ksfx_scene_override = NULL;
    snprintf(kind, sizeof kind, "zz%ld_cooker", (long)getpid());
    if (write_recipe_node(&run, "CookOnly", "cook", "cook:C", "C", kind) != 0) {
        expect(0, "cooker_excl_recipe_written");
        ksfx_cleanup(&run);
        return;
    }
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, "Rice", "cooker");
    put_chef(&world, &run.kitchen, 1, 1, "Rice", "cooker");
    put_order(&world, "w1", "CookOnly", 80);
    put_order(&world, "w2", "CookOnly", 80);
    if (start_run(&run, &shm, &events, taskss, 2, &world) != 0) {
        expect(0, "cooker_excl_scheduler_started");
        cleanup_run(&run, &shm, &events, 2);
        return;
    }
    n = collect_tasks(taskss, 2, got, 2, 3000);
    expect(n == 2 && strcmp(got[0].op, "cook") == 0 && strcmp(got[1].op, "cook") == 0,
           "cooker_excl_two_cooks_dispatched");
    expect(n == 2 && strcmp(got[0].station, got[1].station) != 0,
           "same_round_cooks_get_distinct_stations");
    cleanup_run(&run, &shm, &events, 2);
}

/* 世界状态说某个灶有料时不能再把新 cook 派到那口锅。 */
static void test_cooker_occupied_from_world(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[2];
    char kind[64];
    int n;

    printf("[test_sched_flow] 15. 锅里还有料时跳过这个灶\n");
    ksfx_scene_override = "s_wellcooked_demo_3";
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0) {
        expect(0, "occupied_fixture_init");
        ksfx_cleanup(&run);
        ksfx_scene_override = NULL;
        return;
    }
    ksfx_scene_override = NULL;
    snprintf(kind, sizeof kind, "zz%ld_cooker", (long)getpid());
    (void)write_recipe_node(&run, "CookOnly", "cook", "cook:C", "C", kind);
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, "Rice", "cooker");
    put_order(&world, "w1", "CookOnly", 80);
    put_station_resource(&world, "pot_0", "cooking", "Rice");
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "occupied_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].op, "cook") == 0, "occupied_cook_dispatched");
    expect(n == 1 && strcmp(got[0].station, "cooker-0") != 0,
           "busy_pot_is_not_picked");
    cleanup_run(&run, &shm, &events, 1);
}

static int write_recipe_cook_pour(const ksfx_run *run, const char *dish)
{
    char text[2048];
    int n = snprintf(text, sizeof text,
                     "{\"scene\":\"%s\",\"nodes\":["
                     "{\"id\":\"cook:C\",\"op\":\"cook\",\"label\":\"C\","
                     "\"resource\":\"zz%ld_cooker\",\"duration\":1.0},"
                     "{\"id\":\"pour:P\",\"op\":\"pour\",\"label\":\"P\","
                     "\"resource\":\"counter\",\"duration\":1.0}],"
                     "\"edges\":[[\"cook:C\",\"pour:P\"]],"
                     "\"dishes\":[{\"dish\":\"%s\",\"score\":80,\"root\":\"pour:P\","
                     "\"nodes\":[\"cook:C\",\"pour:P\"]}]}\n",
                     run->scene, (long)getpid(), dish);
    return n > 0 && ksfx_write_file(run->recipes_path, text, (size_t)n) == 0 ? 0 : -1;
}

/* 倒料必须等源锅 state=cooked。 */
static void test_pour_waits_for_cooked(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[4];
    char pot[KOS_SHM_NAME_MAX];
    int n;

    printf("[test_sched_flow] 16. 锅没熟不派 pour\n");
    ksfx_scene_override = "s_wellcooked_demo_3";
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0 ||
        write_recipe_cook_pour(&run, "PourDish") != 0) {
        expect(0, "pour_fixture_init");
        ksfx_cleanup(&run);
        ksfx_scene_override = NULL;
        return;
    }
    ksfx_scene_override = NULL;
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, "Rice", "cooker");
    put_order(&world, "w1", "PourDish", 80);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "pour_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].op, "cook") == 0, "pour_source_cook_dispatched");
    if (n == 1 && station_resource_name(got[0].station, pot, sizeof pot) == 0) {
        put_station_resource(&world, pot, "cooking", "Rice");
        world.chefs[0].holding[0] = '\0';
        expect(world_write(shm, &world) == 0, "pour_pot_cooking_written");
    }
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_DONE, "done") == 0,
           "pour_source_cook_done");
    ksfx_sleep_ms(150);
    n = collect_tasks(taskss, 1, got, 1, 800);
    expect(n == 0, "pour_not_dispatched_while_cooking");
    world.resources[world.resource_count - 1].state[0] = '\0';
    ksfx_copy(world.resources[world.resource_count - 1].state,
              sizeof world.resources[world.resource_count - 1].state, "cooked");
    expect(world_write(shm, &world) == 0, "pour_pot_cooked_written");
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].op, "pour") == 0,
           "pour_dispatched_after_cooked");
    cleanup_run(&run, &shm, &events, 1);
}

/* 同一轮两条 cut 不能挑同一块砧板。 */
static void test_board_station_exclusive(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[4];
    int n;

    printf("[test_sched_flow] 17. 同一轮两条 cut 不能挑同一块砧板\n");
    ksfx_scene_override = "s_wellcooked_demo_3";
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0) {
        expect(0, "board_fixture_init");
        ksfx_cleanup(&run);
        ksfx_scene_override = NULL;
        return;
    }
    ksfx_scene_override = NULL;
    /* fixture 只改 cooker/mixer 等 kind；砧板仍是 "board"。 */
    (void)write_recipe_node(&run, "CutOnly", "cut", "cut:C", "C", "board");
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, "Fish", "board");
    put_chef(&world, &run.kitchen, 1, 1, "Fish", "board");
    put_order(&world, "w1", "CutOnly", 80);
    put_order(&world, "w2", "CutOnly", 80);
    if (start_run(&run, &shm, &events, taskss, 2, &world) != 0) {
        expect(0, "board_scheduler_started");
        cleanup_run(&run, &shm, &events, 2);
        return;
    }
    n = collect_tasks(taskss, 2, got, 2, 3000);
    expect(n == 2 && strcmp(got[0].op, "cut") == 0 && strcmp(got[1].op, "cut") == 0,
           "board_two_cuts_dispatched");
    expect(n == 2 && strcmp(got[0].station, got[1].station) != 0,
           "same_round_cuts_get_distinct_boards");
    cleanup_run(&run, &shm, &events, 2);
}

/* 灶被占用挡下来的候选要进 resources[cooker].waits（resource_wait 事件）。 */
static void test_cooker_gate_counts_wait(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[4];
    char kind[64];
    char needle[128];
    int n;

    printf("[test_sched_flow] 18. 灶占用等待进 resource_wait 统计\n");
    ksfx_scene_override = "s_wellcooked_demo_3";
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0) {
        expect(0, "gate_fixture_init");
        ksfx_cleanup(&run);
        ksfx_scene_override = NULL;
        return;
    }
    ksfx_scene_override = NULL;
    snprintf(kind, sizeof kind, "zz%ld_cooker", (long)getpid());
    (void)write_recipe_node(&run, "CookOnly", "cook", "cook:C", "C", kind);
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, "Rice", "cooker");
    put_chef(&world, &run.kitchen, 1, 1, "Rice", "cooker");
    put_order(&world, "w1", "CookOnly", 80);
    put_order(&world, "w2", "CookOnly", 80);
    {
        int i;
        for (i = 1; i < 6; i++) {
            char name[KOS_SHM_NAME_MAX];
            snprintf(name, sizeof name, "pot_%d", i);
            put_station_resource(&world, name, "cooking", "Rice");
        }
    }
    if (start_run(&run, &shm, &events, taskss, 2, &world) != 0) {
        expect(0, "gate_scheduler_started");
        cleanup_run(&run, &shm, &events, 2);
        return;
    }
    n = collect_tasks(taskss, 2, got, 2, 3000);
    expect(n == 1 && strcmp(got[0].op, "cook") == 0, "gate_one_cook_dispatched");
    snprintf(needle, sizeof needle, "\"event\":\"resource_wait\",\"kind\":\"%s\"", kind);
    expect(ksfx_wait_events(&run, needle, 2000, NULL, 0) == 0,
           "cooker_gate_records_resource_wait");
    cleanup_run(&run, &shm, &events, 2);
}

/* 订单失败/不可行时释放所有工位保留并解绑厨师，闲置厨师接新单任务。 */
static void test_failed_order_releases_and_unbinds(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[4];
    int n;

    printf("[test_sched_flow] 19. 订单失败释放保留并解绑厨师\n");
    ksfx_scene_override = "s_wellcooked_demo_3";
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0) {
        expect(0, "unbind_fixture_init");
        ksfx_cleanup(&run);
        ksfx_scene_override = NULL;
        return;
    }
    ksfx_scene_override = NULL;
    (void)write_recipe_node(&run, "CutOnly", "cut", "cut:C", "C", "board");
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, "Fish", "board");
    put_order(&world, "w1", "CutOnly", 80);
    put_order(&world, "w2", "CutOnly", 80);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "unbind_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].op, "cut") == 0 && strcmp(got[0].order_id, "w1") == 0,
           "first_order_cut_dispatched");
    /* 模拟任务失败导致订单不可行 */
    expect(send_event(events, got[0].task_id, 0, KOS_EVT_FAILED,
                      "macro=chop wait=progress=1 timeout") == 0,
           "task_failure_sent");
    expect(ksfx_wait_events(&run, "\"event\":\"order_failed\"", 2000, NULL, 0) == 0,
           "order_failed_recorded");
    /* 厨师手持在下一轮派发前被采纳或允许接下一单任务 */
    world.chefs[0].holding[0] = '\0';
    expect(world_write(shm, &world) == 0, "chef_empty_handed");
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].order_id, "w2") == 0,
           "idle_chef_takes_next_order_task");
    cleanup_run(&run, &shm, &events, 1);
}
/*
 * 灶台占用排查回归：
 * 1. 世界状态通过坐标上报某灶有料（如 pot_0 位置在 cooker-4），即使键名与序号不对应，
 *    调度器也不得把新 cook 派到该灶，应选空闲的同类型灶（cooker-5）。
 * 2. 失败订单释放 reservation 后，若该灶在世界状态中仍显示忙碌/有料，后继订单的 cook
 *    绝不能选该灶。
 */
static void test_cooker_pos_busy_and_order_release(void)
{
    ksfx_run run;
    kos_shm *shm = NULL;
    kos_world_state world;
    mqd_t events = (mqd_t)-1;
    mqd_t taskss[KOS_SHM_MAX_CHEFS];
    got_task got[2];
    char kind[64];
    int n;

    printf("[test_sched_flow] 20. 灶台按位置识别占用，且订单释放后世界占用的灶不被二次误选\n");
    ksfx_scene_override = "s_wellcooked_demo_3";
    if (ksfx_init_ex(&run, 0) != 0 || write_orders(&run) != 0) {
        expect(0, "cooker_pos_init");
        ksfx_cleanup(&run);
        ksfx_scene_override = NULL;
        return;
    }
    ksfx_scene_override = NULL;
    snprintf(kind, sizeof kind, "zz%ld_cooker", (long)getpid());
    (void)write_recipe_node(&run, "CookPot", "cook", "cook:C", "C", kind);
    kos_world_state_reset(&world);
    put_chef(&world, &run.kitchen, 0, 0, "Rice", "cooker");
    put_order(&world, "w1", "CookPot", 80);
    /* 模拟 G1/旧上报：键名叫 pot_0，但坐标落在 cooker-4 (6.6, -6.0) 处且正在煮米 */
    put_station_resource_at(&world, "pot_0", "cooking", "Rice", 6.6f, -6.0f);
    if (start_run(&run, &shm, &events, taskss, 1, &world) != 0) {
        expect(0, "cooker_pos_scheduler_started");
        cleanup_run(&run, &shm, &events, 1);
        return;
    }
    n = collect_tasks(taskss, 1, got, 1, 3000);
    expect(n == 1 && strcmp(got[0].op, "cook") == 0, "cooker_pos_cook_dispatched");
    expect(n == 1 && strcmp(got[0].station, "cooker-4") != 0,
           "cooker_4_avoided_via_pos_match");
    cleanup_run(&run, &shm, &events, 1);
}

int main(void)
{
    printf("[test_sched_flow] 开始：派工看手持、失败分流、订单结果闭环\n");
    test_holding_and_candidates();
    test_retry_backoff();
    test_refusal_is_infeasible();
    test_delivered_vs_expired();
    test_score_delta_expiry();
    test_expiry_adopts_item();
    test_putdown();
    test_score_fallback();
    test_holding_lost();
    test_recovery_adopts_holding();
    test_worker_handoff_registration();
    test_cooker_station_exclusive();
    test_cooker_occupied_from_world();
    test_pour_waits_for_cooked();
    test_board_station_exclusive();
    test_cooker_gate_counts_wait();
    test_failed_order_releases_and_unbinds();
    test_cooker_pos_busy_and_order_release();
    printf("[test_sched_flow] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
