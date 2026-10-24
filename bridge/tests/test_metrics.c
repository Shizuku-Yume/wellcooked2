/*
 * test_metrics.c -- metrics.json 的原子写入与字段完整性。
 *
 * run_dir_layout: 在临时目录里摆出 runs/<ts>/{events,decisions,metrics}.json(l)
 * 的形状，metrics 由真实写出函数生成；脚本只需要这三个文件就能读这一局。
 * metrics_fields_complete: 用项目自己的 JSON parser 回读，逐字段断言，避免
 * "文件存在但字段改名"这种假绿。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/json.h"
#include "kitchenos/metrics.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static int read_all(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t used = 0;

    if (fd < 0) {
        return -1;
    }
    while (used + 1 < cap) {
        ssize_t n = read(fd, buf + used, cap - 1 - used);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }
    close(fd);
    buf[used] = '\0';
    return 0;
}

static int touch(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    if (fd < 0) {
        return -1;
    }
    close(fd);
    return 0;
}

static void fill_snapshot(kos_metrics_snapshot *snapshot, const char *reason)
{
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->t_ms = 19000;
    snapshot->reason = reason;
    snapshot->orders_total = 2;
    snapshot->orders_completed = 1;
    snapshot->orders_failed = 1;
    snapshot->orders_active = 0;
    snapshot->order_count = 2;
    snprintf(snapshot->orders[0].id, sizeof snapshot->orders[0].id, "cfg-00");
    snprintf(snapshot->orders[0].dish, sizeof snapshot->orders[0].dish, "Fish");
    snprintf(snapshot->orders[0].status, sizeof snapshot->orders[0].status, "completed");
    snapshot->orders[0].completed = 1;
    snapshot->orders[0].arrival_ms = 0;
    snapshot->orders[0].deadline_ms = 18000;
    snapshot->orders[0].finished_ms = 19500;
    snapshot->orders[0].lateness_ms = 1500;
    snprintf(snapshot->orders[1].id, sizeof snapshot->orders[1].id, "cfg-01");
    snprintf(snapshot->orders[1].dish, sizeof snapshot->orders[1].dish, "Meat");
    snprintf(snapshot->orders[1].status, sizeof snapshot->orders[1].status, "failed");
    snapshot->orders[1].arrival_ms = 700;
    snapshot->orders[1].deadline_ms = 20000;
    snapshot->orders[1].finished_ms = 12000;
    snapshot->orders[1].lateness_ms = 0;

    snapshot->resource_count = 2;
    snprintf(snapshot->resources[0].kind, sizeof snapshot->resources[0].kind, "cooker");
    snapshot->resources[0].capacity = 1;
    snapshot->resources[0].in_use = 0;
    snapshot->resources[0].acquires = 7;
    snapshot->resources[0].busy_ms = 9000;
    snapshot->resources[0].wait_ms = 2500;
    snapshot->resources[0].waits = 3;
    snapshot->resources[0].utilization_bp = 4736;
    snprintf(snapshot->resources[1].kind, sizeof snapshot->resources[1].kind, "board");
    snapshot->resources[1].capacity = 2;
    snapshot->resources[1].in_use = 1;
    snapshot->resources[1].acquires = 4;
    snapshot->resources[1].busy_ms = 4000;
    snapshot->resources[1].wait_ms = 0;
    snapshot->resources[1].waits = 0;
    snapshot->resources[1].utilization_bp = 1052;

    snapshot->dispatch_fcfs = 5;
    snapshot->dispatch_edf = 6;
    snapshot->worker_restarts = 1;
}

int main(void)
{
    char root[] = "/tmp/kos-metrics-XXXXXX";
    char run_dir[512];
    char metrics[600];
    char events[600];
    char decisions[600];
    char body[16384];
    char err[256];
    kos_metrics_snapshot snapshot;
    json_value *doc = NULL;
    const json_value *orders;
    const json_value *resources;
    const json_value *dispatch;
    const json_value *array;
    long long number = 0;
    const char *text = NULL;
    size_t text_len = 0;
    int all_ok;

    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(run_dir, sizeof run_dir, "%s/runs/20261024-120000", root);
    {
        char runs[512];

        snprintf(runs, sizeof runs, "%s/runs", root);
        if (mkdir(runs, 0755) != 0 && errno != EEXIST) {
            perror("mkdir runs");
            return 2;
        }
    }
    if (mkdir(run_dir, 0755) != 0 && errno != EEXIST) {
        perror("mkdir run_dir");
        return 2;
    }
    snprintf(events, sizeof events, "%s/events.jsonl", run_dir);
    snprintf(decisions, sizeof decisions, "%s/decisions.jsonl", run_dir);
    snprintf(metrics, sizeof metrics, "%s/metrics.json", run_dir);

    printf("[test_metrics] 1. run_dir_layout\n");
    expect(touch(events) == 0 && touch(decisions) == 0, "events_decisions_created");
    fill_snapshot(&snapshot, "shutdown");
    expect(kos_metrics_write(metrics, &snapshot, err, sizeof err) == 0,
           "metrics_written");
    expect(access(events, R_OK) == 0 && access(decisions, R_OK) == 0 &&
               access(metrics, R_OK) == 0,
           "run_dir_has_three_files");

    printf("[test_metrics] 2. metrics_fields_complete\n");
    expect(read_all(metrics, body, sizeof body) == 0, "metrics_readable");
    expect(json_parse_ex(body, strlen(body), &doc, NULL) == JSON_OK, "metrics_json_parses");
    expect(json_object_get_int(doc, "schema", &number) != JSON_OK &&
               json_object_get_string(doc, "schema", &text, &text_len) == JSON_OK &&
               strcmp(text, "kitchenos.metrics.v1") == 0,
           "schema_field");
    expect(json_object_get_int(doc, "t_ms", &number) == JSON_OK && number == 19000,
           "t_ms_field");
    expect(json_object_get_string(doc, "reason", &text, &text_len) == JSON_OK &&
               strcmp(text, "shutdown") == 0,
           "reason_field");
    expect(json_object_get_object(doc, "orders", &orders) == JSON_OK, "orders_object");
    expect(json_object_get_int(orders, "total", &number) == JSON_OK && number == 2,
           "orders_total");
    expect(json_object_get_int(orders, "completed", &number) == JSON_OK && number == 1,
           "orders_completed");
    expect(json_object_get_int(orders, "failed", &number) == JSON_OK && number == 1,
           "orders_failed");
    expect(json_object_get_int(orders, "active", &number) == JSON_OK && number == 0,
           "orders_active");
    expect(json_object_get_array(orders, "per_order", &array) == JSON_OK &&
               json_array_size(array) == 2,
           "per_order_array");
    expect(json_object_get_int(json_array_get(array, 0), "lateness_ms", &number) ==
                   JSON_OK &&
               number == 1500,
           "per_order_lateness");
    expect(json_object_get_int(json_array_get(array, 0), "finished_ms", &number) ==
                   JSON_OK &&
               number == 19500,
           "per_order_finished");
    expect(json_object_get_array(doc, "resources", &resources) == JSON_OK &&
               json_array_size(resources) == 2,
           "resources_array");
    expect(json_object_get_string(json_array_get(resources, 0), "kind", &text,
                                  &text_len) == JSON_OK &&
               strcmp(text, "cooker") == 0,
           "resource_kind");
    expect(json_object_get_int(json_array_get(resources, 0), "capacity", &number) ==
                   JSON_OK &&
               number == 1,
           "resource_capacity");
    expect(json_object_get_int(json_array_get(resources, 0), "in_use", &number) ==
                   JSON_OK &&
               number == 0,
           "resource_in_use");
    expect(json_object_get_int(json_array_get(resources, 0), "acquires", &number) ==
                   JSON_OK &&
               number == 7,
           "resource_acquires");
    expect(json_object_get_int(json_array_get(resources, 0), "wait_ms", &number) ==
                   JSON_OK &&
               number == 2500,
           "resource_wait_ms");
    expect(json_object_get_int(json_array_get(resources, 0), "utilization_bp", &number) ==
                   JSON_OK &&
               number == 4736,
           "resource_utilization");
    expect(json_object_get_object(doc, "dispatch", &dispatch) == JSON_OK, "dispatch_object");
    expect(json_object_get_int(dispatch, "fcfs", &number) == JSON_OK && number == 5,
           "dispatch_fcfs");
    expect(json_object_get_int(dispatch, "edf", &number) == JSON_OK && number == 6,
           "dispatch_edf");
    expect(json_object_get_int(doc, "worker_restarts", &number) == JSON_OK && number == 1,
           "worker_restarts");
    json_free(doc);
    doc = NULL;

    printf("[test_metrics] 3. 原子替换\n");
    expect(access("/tmp", F_OK) == 0, "tmp_available");
    {
        char tmp_path[640];

        snprintf(tmp_path, sizeof tmp_path, "%s.tmp", metrics);
        expect(access(tmp_path, F_OK) != 0, "no_tmp_file_left");
    }
    fill_snapshot(&snapshot, "sigusr1");
    snapshot.orders_completed = 2;
    snapshot.orders_failed = 0;
    {
        struct stat before;
        struct stat after;

        expect(stat(metrics, &before) == 0, "metrics_stat_before");
        expect(kos_metrics_write(metrics, &snapshot, err, sizeof err) == 0,
               "metrics_replaced");
        expect(stat(metrics, &after) == 0, "metrics_stat_after");
        expect(before.st_ino != after.st_ino, "atomic_rename_replaces_inode");
    }
    expect(read_all(metrics, body, sizeof body) == 0, "metrics_reread");
    expect(json_parse_ex(body, strlen(body), &doc, NULL) == JSON_OK, "metrics_reparse");
    expect(json_object_get_string(doc, "reason", &text, &text_len) == JSON_OK &&
               strcmp(text, "sigusr1") == 0,
           "reason_updated");
    expect(json_object_get_object(doc, "orders", &orders) == JSON_OK &&
               json_object_get_int(orders, "completed", &number) == JSON_OK &&
               number == 2,
           "contents_fully_replaced");
    json_free(doc);
    doc = NULL;

    all_ok = g_failures == 0;
    unlink(metrics);
    unlink(events);
    unlink(decisions);
    rmdir(run_dir);
    {
        char runs[512];

        snprintf(runs, sizeof runs, "%s/runs", root);
        rmdir(runs);
    }
    rmdir(root);
    printf("[test_metrics] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return all_ok ? 0 : 1;
}
