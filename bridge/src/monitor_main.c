/*
 * kitchenos-monitor：只读挂接世界状态共享内存，周期性把一致快照写成
 * <runtime>/live.json（原子 rename），给 Web 控制台画实时画面。
 * 不写共享内存、不碰输入槽；段不存在时写 {"ok":false,...} 并继续重试挂接。
 *
 *   kitchenos-monitor --runtime-dir DIR [--state-name /name] [--interval-ms 200]
 */
#include <kitchenos/monitor.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MONITOR_JSON_CAP 262144

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static int write_live(const char *dir, const char *text, size_t len)
{
    char path[4096];
    char tmp[4096];
    int fd;
    ssize_t n;

    if (snprintf(path, sizeof path, "%s/live.json", dir) >= (int)sizeof path ||
        snprintf(tmp, sizeof tmp, "%s/.live.json.tmp.%ld", dir, (long)getpid()) >=
            (int)sizeof tmp) {
        return -1;
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }
    n = write(fd, text, len);
    if (n != (ssize_t)len) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) < 0 || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void write_unavailable(const char *dir, const char *reason)
{
    char text[256];
    int len = snprintf(text, sizeof text, "{\"ok\":false,\"reason\":\"%s\"}", reason);

    if (len > 0 && len < (int)sizeof text) {
        write_live(dir, text, (size_t)len);
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: kitchenos-monitor --runtime-dir DIR [--state-name /name] "
                    "[--interval-ms N]\n");
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    const char *state_name = NULL;
    long interval = 200;
    kos_shm_opts opts;
    kos_shm *shm = NULL;
    kos_world_state *world;
    kos_error err;
    char *json;
    struct sigaction sa;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--runtime-dir") && i + 1 < argc) {
            dir = argv[++i];
        } else if (!strcmp(argv[i], "--state-name") && i + 1 < argc) {
            state_name = argv[++i];
        } else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc) {
            interval = strtol(argv[++i], NULL, 10);
        } else {
            usage();
            return 2;
        }
    }
    if (!dir || interval < 10 || interval > 10000) {
        usage();
        return 2;
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    json = malloc(MONITOR_JSON_CAP);
    world = malloc(sizeof *world);
    if (!json || !world) {
        free(json);
        free(world);
        return 1;
    }
    kos_shm_opts_init(&opts);
    if (state_name) {
        opts.object_name = state_name;
    }

    while (!stop_requested) {
        struct timespec ts;
        size_t len = 0;
        int rc;

        if (!shm) {
            kos_error_clear(&err);
            if (kos_shm_open(&opts, KOS_SHM_ATTACH, &shm, &err) != KOS_OK) {
                shm = NULL;
                write_unavailable(dir, "shm unavailable");
            }
        }
        if (shm) {
            rc = kos_shm_world_read(shm, world, 8, NULL);
            if (rc == KOS_OK) {
                if (kos_monitor_snapshot_json(world, json, MONITOR_JSON_CAP, &len) == 0) {
                    write_live(dir, json, len);
                } else {
                    write_unavailable(dir, "encode failed");
                }
            } else if (rc == KOS_ERR_EMPTY) {
                write_unavailable(dir, "no world state yet");
            } else if (rc != KOS_ERR_BUSY) {
                /* 段被 Bridge 重建或删掉：放手，下一轮重新挂接。 */
                kos_shm_close(shm, &err);
                shm = NULL;
                write_unavailable(dir, "shm unavailable");
            }
        }
        ts.tv_sec = interval / 1000;
        ts.tv_nsec = (interval % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    if (shm) {
        kos_shm_close(shm, &err);
    }
    free(world);
    free(json);
    return 0;
}
