/*
 * test_monitor.c -- live.json 编码与 wellcooked-monitor 进程。
 *
 * 控制台只看 live.json：字段缺了、计时器/位置的"有效"位被忽略、段不存在时不报
 * ok:false，网页就会画出假画面。这里用固定世界状态证明编码，再起真进程挂一个
 * 测试专用段，证明它写出的文件能被解析且内容与段一致。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "wellcooked/json.h"
#include "wellcooked/monitor.h"
#include "wellcooked/shm.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

static void fill_world(kos_world_state *w)
{
    memset(w, 0, sizeof *w);
    w->score = 120;
    w->game_time_ms = 45250;
    w->has_round_time = 1;
    w->round_time_left = 134.5f;
    w->round_time_limit = 180.0f;
    w->chef_count = 2;
    w->chefs[0].id = 0;
    w->chefs[0].x = 1.5f;
    w->chefs[0].y = -2.0f;
    snprintf(w->chefs[0].holding, sizeof w->chefs[0].holding, "plate");
    snprintf(w->chefs[0].state, sizeof w->chefs[0].state, "moving");
    w->chefs[0].holding_content_count = 2;
    snprintf(w->chefs[0].holding_contents[0], KOS_SHM_INGREDIENT_MAX, "tortilla");
    snprintf(w->chefs[0].holding_contents[1], KOS_SHM_INGREDIENT_MAX, "cooked:rice");
    w->chefs[1].id = 1;
    w->order_count = 1;
    snprintf(w->orders[0].id, sizeof w->orders[0].id, "order_003");
    snprintf(w->orders[0].recipe, sizeof w->orders[0].recipe, "burrito");
    w->orders[0].time_remaining = 37;
    w->orders[0].score = 60;
    w->orders[0].ingredient_count = 1;
    snprintf(w->orders[0].ingredients[0], KOS_SHM_INGREDIENT_MAX, "rice");
    w->resource_count = 2;
    snprintf(w->resources[0].name, sizeof w->resources[0].name, "pot_0");
    snprintf(w->resources[0].kind, sizeof w->resources[0].kind, "cooker");
    w->resources[0].progress = 0.5f;
    w->resources[0].has_pos = 1;
    w->resources[0].pos_x = 3.0f;
    w->resources[0].pos_y = 4.0f;
    w->resources[0].has_burning = 1;
    w->resources[0].burning = 0;
    w->resources[0].chef_id = -1;
    snprintf(w->resources[1].name, sizeof w->resources[1].name, "plate_stack");
    w->resources[1].count = 3;
    w->order_result_count = 1;
    w->order_results[0].seq = 7;
    snprintf(w->order_results[0].outcome, sizeof w->order_results[0].outcome, "delivered");
    w->order_results[0].has_score = 1;
    w->order_results[0].score = 40;
}

static const json_value *nth(const json_value *doc, const char *key, size_t i)
{
    return json_array_get(json_object_get(doc, key), i);
}

static void check_document(const json_value *doc, const char *tag)
{
    const json_value *chef = nth(doc, "chefs", 0);
    const json_value *order = nth(doc, "orders", 0);
    const json_value *pot = nth(doc, "stations", 0);
    const json_value *stack = nth(doc, "stations", 1);
    const json_value *timer = json_object_get(doc, "timer");
    const char *s = NULL;
    long long v = 0;
    double d = 0;
    char what[96];

#define CHECK(cond, name)                                         \
    do {                                                          \
        snprintf(what, sizeof what, "%s_%s", tag, name);          \
        expect(cond, what);                                       \
    } while (0)

    CHECK(json_bool_value(json_object_get(doc, "ok")) == 1, "ok_true");
    CHECK(json_object_get_int(doc, "score", &v) == 0 && v == 120, "score");
    CHECK(json_object_get_int(doc, "t_ms", &v) == 0 && v == 45250, "t_ms");
    CHECK(json_object_get_double(timer, "left_s", &d) == 0 && d == 134.5 &&
              json_object_get_double(timer, "limit_s", &d) == 0 && d == 180.0,
          "timer");
    CHECK(json_array_size(json_object_get(doc, "chefs")) == 2, "chef_count");
    CHECK(json_object_get_double(chef, "z", &d) == 0 && d == -2.0, "chef_y_as_z");
    CHECK(json_object_get_string(chef, "holding", &s, NULL) == 0 && strcmp(s, "plate") == 0,
          "chef_holding");
    CHECK(json_array_size(json_object_get(chef, "holding_contents")) == 2 &&
              strcmp(json_string_value(nth(chef, "holding_contents", 1)), "cooked:rice") == 0,
          "chef_holding_contents");
    CHECK(json_object_get_string(order, "dish", &s, NULL) == 0 && strcmp(s, "burrito") == 0 &&
              json_object_get_int(order, "remaining_s", &v) == 0 && v == 37 &&
              json_object_get_int(order, "score", &v) == 0 && v == 60,
          "order_fields");
    CHECK(json_object_get_double(pot, "x", &d) == 0 && d == 3.0 &&
              json_object_get_double(pot, "z", &d) == 0 && d == 4.0,
          "station_pos_when_known");
    CHECK(json_object_get(stack, "x") == NULL, "station_no_pos_when_unknown");
    CHECK(json_typeof(json_object_get(pot, "burning")) == JSON_BOOL &&
              json_bool_value(json_object_get(pot, "burning")) == 0,
          "station_burning_reported_false");
    CHECK(json_is_null(json_object_get(pot, "cooked")), "station_cooked_unknown_is_null");
    CHECK(json_object_get_int(stack, "count", &v) == 0 && v == 3, "plate_stack_count");
    CHECK(json_object_get_int(nth(doc, "results", 0), "score", &v) == 0 && v == 40,
          "result_score");
#undef CHECK
}

static void test_encoder(void)
{
    static kos_world_state world;
    static char buf[262144];
    size_t len = 0;
    json_value *doc = NULL;

    printf("[test_monitor] 1. 编码\n");
    fill_world(&world);
    expect(kos_monitor_snapshot_json(&world, buf, sizeof buf, &len) == 0 && len > 0,
           "encode_ok");
    expect(json_parse(buf, len, &doc) == 0 && doc != NULL, "encode_parses");
    if (doc) {
        check_document(doc, "encode");
        json_free(doc);
    }

    world.has_round_time = 0;
    world.chef_count = 99; /* 越界计数必须被截到数组上限，而不是读出界 */
    doc = NULL;
    expect(kos_monitor_snapshot_json(&world, buf, sizeof buf, &len) == 0 &&
               json_parse(buf, len, &doc) == 0 &&
               json_is_null(json_object_get(doc, "timer")) &&
               json_array_size(json_object_get(doc, "chefs")) == KOS_SHM_MAX_CHEFS,
           "encode_no_timer_null_and_clamped_counts");
    json_free(doc);
    expect(kos_monitor_snapshot_json(&world, buf, 64, &len) != 0, "encode_small_buffer_fails");
}

static char g_monitor_bin[4096];

static void resolve_binary(void)
{
    char exe[4000]; /* 比 g_monitor_bin 小，拼上可执行名不会截断 */
    char *slash;
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);

    if (n <= 0) {
        return;
    }
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL) {
        return;
    }
    *slash = '\0';
    snprintf(g_monitor_bin, sizeof g_monitor_bin, "%s/wellcooked-monitor", exe);
}

static pid_t spawn_monitor(const char *dir, const char *state_name, const char *sem_name)
{
    pid_t pid = fork();

    if (pid == 0) {
        setenv("WELLCOOKED_SEM_NAME", sem_name, 1);
        execl(g_monitor_bin, "wellcooked-monitor", "--runtime-dir", dir, "--state-name",
              state_name, "--interval-ms", "50", (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* 等 live.json 出现并解析；最多 3 秒。成功返回文档（调用方 json_free）。 */
static json_value *wait_live(const char *dir)
{
    char path[4200];
    int tries;

    snprintf(path, sizeof path, "%s/live.json", dir);
    for (tries = 0; tries < 60; tries++) {
        struct timespec ts = {0, 50 * 1000000L};
        FILE *f = fopen(path, "rb");

        if (f != NULL) {
            static char text[262144];
            size_t len = fread(text, 1, sizeof text, f);
            json_value *doc = NULL;

            fclose(f);
            if (len > 0 && json_parse(text, len, &doc) == 0) {
                return doc;
            }
        }
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static int stop_monitor(pid_t pid)
{
    int status = 0;

    kill(pid, SIGTERM);
    if (waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_process(void)
{
    static kos_world_state world;
    char dir[] = "/tmp/kos_monitor_test_XXXXXX";
    char shm_name[KOS_SHM_PATH_MAX];
    char sem_name[KOS_SHM_PATH_MAX];
    char missing[KOS_SHM_PATH_MAX];
    char path[4200];
    kos_shm_opts opts;
    kos_shm *shm = NULL;
    kos_error err;
    json_value *doc;
    pid_t pid;

    printf("[test_monitor] 2. wellcooked-monitor 进程\n");
    resolve_binary();
    expect(access(g_monitor_bin, X_OK) == 0, "monitor_binary_built");
    if (mkdtemp(dir) == NULL) {
        expect(0, "mkdtemp");
        return;
    }
    snprintf(shm_name, sizeof shm_name, "/wellcooked_test_monitor_%ld", (long)getpid());
    snprintf(sem_name, sizeof sem_name, "/wellcooked_test_sem_monitor_%ld", (long)getpid());
    snprintf(missing, sizeof missing, "/wellcooked_test_monitor_missing_%ld", (long)getpid());

    /* 段不存在：照样写 live.json，但 ok=false，网页据此显示"等待游戏"。 */
    pid = spawn_monitor(dir, missing, sem_name);
    doc = wait_live(dir);
    expect(doc != NULL && json_typeof(json_object_get(doc, "ok")) == JSON_BOOL &&
               json_bool_value(json_object_get(doc, "ok")) == 0,
           "missing_shm_writes_ok_false");
    json_free(doc);
    expect(stop_monitor(pid) == 0, "missing_shm_sigterm_exits_zero");
    snprintf(path, sizeof path, "%s/live.json", dir);
    unlink(path);

    kos_shm_opts_init(&opts);
    opts.object_name = shm_name;
    opts.sem_name = sem_name;
    opts.dir = KOS_SHM_DEFAULT_DIR;
    kos_error_clear(&err);
    expect(kos_shm_open(&opts, KOS_SHM_RECREATE, &shm, &err) == KOS_OK, "create_test_segment");
    if (shm != NULL) {
        fill_world(&world);
        expect(kos_shm_world_write(shm, &world) == KOS_OK, "write_world");
        pid = spawn_monitor(dir, shm_name, sem_name);
        doc = wait_live(dir);
        expect(doc != NULL, "live_json_written");
        if (doc != NULL) {
            check_document(doc, "process");
            json_free(doc);
        }
        expect(stop_monitor(pid) == 0, "sigterm_exits_zero");
        kos_shm_close(shm, &err);
    }
    kos_shm_unlink(&opts, &err);
    unlink(path);
    rmdir(dir);
}

int main(void)
{
    printf("[test_monitor] 开始：live.json 编码与监视进程\n");
    test_encoder();
    test_process();
    printf("[test_monitor] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
