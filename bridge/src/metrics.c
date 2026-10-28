#include <wellcooked/json.h>
#include <wellcooked/metrics.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define METRICS_BUF_MAX 16384

static void metrics_error(char *err, size_t err_size, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || err_size == 0) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
}

static int write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, data, len);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        data += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static const char *completed_text(const kos_metrics_order *order)
{
    if (order->status[0] != '\0') {
        return order->status;
    }
    return order->completed ? "completed" : "active";
}

int kos_metrics_write(const char *path, const kos_metrics_snapshot *snapshot, char *err,
                      size_t err_size)
{
    char body[METRICS_BUF_MAX];
    char tmp[1024];
    json_writer writer;
    int fd;
    int i;
    size_t len;

    if (path == NULL || snapshot == NULL) {
        metrics_error(err, err_size, "invalid arguments");
        return -1;
    }
    if (json_writer_init(&writer, body, sizeof body) != 0) {
        metrics_error(err, err_size, "json buffer init failed");
        return -1;
    }
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        metrics_error(err, err_size, "path too long");
        return -1;
    }

    json_writer_object_begin(&writer);
    json_writer_key(&writer, "schema");
    json_writer_string(&writer, "wellcooked.metrics.v1");
    json_writer_key(&writer, "t_ms");
    json_writer_int(&writer, snapshot->t_ms);
    json_writer_key(&writer, "reason");
    json_writer_string(&writer, snapshot->reason != NULL ? snapshot->reason : "");

    json_writer_key(&writer, "orders");
    json_writer_object_begin(&writer);
    json_writer_key(&writer, "total");
    json_writer_int(&writer, snapshot->orders_total);
    json_writer_key(&writer, "completed");
    json_writer_int(&writer, snapshot->orders_completed);
    json_writer_key(&writer, "failed");
    json_writer_int(&writer, snapshot->orders_failed);
    json_writer_key(&writer, "active");
    json_writer_int(&writer, snapshot->orders_active);
    json_writer_key(&writer, "per_order");
    json_writer_array_begin(&writer);
    for (i = 0; i < snapshot->order_count; i++) {
        const kos_metrics_order *order = &snapshot->orders[i];

        json_writer_object_begin(&writer);
        json_writer_key(&writer, "id");
        json_writer_string(&writer, order->id);
        json_writer_key(&writer, "dish");
        json_writer_string(&writer, order->dish);
        json_writer_key(&writer, "status");
        json_writer_string(&writer, completed_text(order));
        json_writer_key(&writer, "completed");
        json_writer_bool(&writer, order->completed);
        json_writer_key(&writer, "arrival_ms");
        json_writer_int(&writer, order->arrival_ms);
        json_writer_key(&writer, "deadline_ms");
        json_writer_int(&writer, order->deadline_ms);
        json_writer_key(&writer, "finished_ms");
        json_writer_int(&writer, order->finished_ms);
        json_writer_key(&writer, "lateness_ms");
        json_writer_int(&writer, order->lateness_ms);
        json_writer_object_end(&writer);
    }
    json_writer_array_end(&writer);
    json_writer_object_end(&writer);

    json_writer_key(&writer, "resources");
    json_writer_array_begin(&writer);
    for (i = 0; i < snapshot->resource_count; i++) {
        const kos_metrics_resource *res = &snapshot->resources[i];

        json_writer_object_begin(&writer);
        json_writer_key(&writer, "kind");
        json_writer_string(&writer, res->kind);
        json_writer_key(&writer, "capacity");
        json_writer_int(&writer, res->capacity);
        json_writer_key(&writer, "in_use");
        json_writer_int(&writer, res->in_use);
        json_writer_key(&writer, "acquires");
        json_writer_int(&writer, res->acquires);
        json_writer_key(&writer, "busy_ms");
        json_writer_int(&writer, res->busy_ms);
        json_writer_key(&writer, "wait_ms");
        json_writer_int(&writer, res->wait_ms);
        json_writer_key(&writer, "waits");
        json_writer_int(&writer, res->waits);
        json_writer_key(&writer, "utilization_bp");
        json_writer_int(&writer, res->utilization_bp);
        json_writer_object_end(&writer);
    }
    json_writer_array_end(&writer);

    json_writer_key(&writer, "dispatch");
    json_writer_object_begin(&writer);
    json_writer_key(&writer, "fcfs");
    json_writer_int(&writer, snapshot->dispatch_fcfs);
    json_writer_key(&writer, "edf");
    json_writer_int(&writer, snapshot->dispatch_edf);
    json_writer_key(&writer, "llf");
    json_writer_int(&writer, snapshot->dispatch_llf);
    json_writer_key(&writer, "hrrn");
    json_writer_int(&writer, snapshot->dispatch_hrrn);
    json_writer_key(&writer, "sjf");
    json_writer_int(&writer, snapshot->dispatch_sjf);
    json_writer_object_end(&writer);
    json_writer_key(&writer, "worker_restarts");
    json_writer_int(&writer, snapshot->worker_restarts);
    json_writer_object_end(&writer);

    if (json_writer_error(&writer)) {
        metrics_error(err, err_size, "json writer overflow");
        return -1;
    }
    len = json_writer_length(&writer);

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        metrics_error(err, err_size, "open %s: %s", tmp, strerror(errno));
        return -1;
    }
    if (write_all(fd, body, len) != 0 || fsync(fd) != 0) {
        metrics_error(err, err_size, "write %s: %s", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) != 0) {
        metrics_error(err, err_size, "close %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        metrics_error(err, err_size, "rename %s -> %s: %s", tmp, path,
                      strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}
