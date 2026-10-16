/*
 * kitchenos-worker-stub -- 一个 Chef Worker 的替身。
 *
 * Bridge 的帧屏障要成立，必须有人往输入槽里写东西，而真正的 Worker 还没实现。
 * 这个程序按固定频率把 "move" 动作发布进指定的一格，于是屏障、就绪信号量和
 * idle 降级三条路径都能在不用游戏、不用 Scheduler、不用 Worker 的前提下被驱动。
 *
 * 它**从不创建**共享段：Bridge 没在跑的时候，没有东西可以被替代，这时报错退出
 * 比悄悄造一个半成品段（等着 Bridge 挂接上去）有用得多。
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <kitchenos/shm.h>

/* 让 move 的目标真的在动：恒定目标看不出"这一帧和上一帧不是同一份输入"。
 * 用三角波而不是 cos/sin —— libm 不在 Makefile 的 LDFLAGS 里，而一个替身
 * 没有任何理由为了一条来回移动的路线去依赖数学库。 */
#define STUB_SPAN 20 /* 单程多少帧 */
#define STUB_X0 (-4.0f)
#define STUB_X1 4.0f

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
            "usage: %s --state-name NAME --sem-name NAME --chefs N --slot K\n"
            "          --hz H --duration S [--kind move|stop|interact|pickup|dash]\n"
            "          [--target X,Y]\n"
            "\n"
            "Publishes one Chef Worker's worth of input into slot K of an existing\n"
            "KitchenOS shared segment. The segment must already exist (start the\n"
            "Bridge first); this program never creates it.\n"
            "\n"
            "move sends a triangular wave between STUB_X0 and STUB_X1 by default;\n"
            "--target X,Y sends one constant target instead (what a real Worker\n"
            "does when it decides 'chef 0 goes to the cutting board').\n",
            argv0);
}

static int parse_int(const char *text, long *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *out = value;
    return 0;
}

static int parse_double(const char *text, double *out)
{
    char *end = NULL;
    double value;

    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *out = value;
    return 0;
}

/* 解析 text 的前 len 个字节（不是 NUL 结尾的片段），给 --target X,Y 用。 */
static int parse_double_prefix(const char *text, size_t len, double *out)
{
    char buf[64];
    char *end = NULL;
    double value;

    if (len == 0 || len >= sizeof buf) {
        return -1;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';

    errno = 0;
    value = strtod(buf, &end);
    if (errno != 0 || end == buf || *end != '\0') {
        return -1;
    }
    *out = value;
    return 0;
}

/* 睡到绝对时刻 next，被信号打断就提前醒来（外层会看到 g_stop）。 */
static void sleep_until(const struct timespec *next)
{
    struct timespec left = *next;

    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &left, &left) == EINTR) {
        if (g_stop) {
            return;
        }
    }
}

static void add_ns(struct timespec *ts, long ns)
{
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec += 1;
    }
}

static double seconds_between(const struct timespec *from, const struct timespec *to)
{
    return (double)(to->tv_sec - from->tv_sec) + (double)(to->tv_nsec - from->tv_nsec) / 1e9;
}

static int ts_before(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec) {
        return a->tv_sec < b->tv_sec;
    }
    return a->tv_nsec < b->tv_nsec;
}

/*
 * The slot's action kind, so a scenario can drive any wire action without a
 * bespoke stub binary. This is the whole vocabulary kos_action_kind has; `none` is
 * not offered, because "publish nothing" is what not publishing means.
 */
static int parse_kind(const char *text, kos_action_kind *out)
{
    static const struct {
        const char *name;
        kos_action_kind kind;
    } table[] = {
        {"move", KOS_ACTION_MOVE},
        {"stop", KOS_ACTION_STOP},
        {"interact", KOS_ACTION_INTERACT},
        {"pickup", KOS_ACTION_PICKUP},
        {"dash", KOS_ACTION_DASH},
    };
    size_t i;

    for (i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(text, table[i].name) == 0) {
            *out = table[i].kind;
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *state_name = NULL;
    const char *sem_name = NULL;
    long chefs = 2;
    long slot = 0;
    double hz = 60.0;
    double duration = 4.0;
    kos_action_kind kind = KOS_ACTION_MOVE;
    int fixed_target = 0;
    double target_x = 0.0;
    double target_y = 0.0;

    kos_shm_opts opts;
    kos_shm *shm = NULL;
    kos_error err;
    struct sigaction sa;
    struct timespec next;
    struct timespec start;
    long period_ns;
    uint64_t frame = 0;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *value;

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        }
        /* 其余选项都带值：缺值当场报错，不要退回默认值假装一切正常。 */
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: '%s' needs a value\n", argv[0], a);
            return 2;
        }
        value = argv[++i];

        if (strcmp(a, "--state-name") == 0) {
            state_name = value;
        } else if (strcmp(a, "--sem-name") == 0) {
            sem_name = value;
        } else if (strcmp(a, "--chefs") == 0) {
            if (parse_int(value, &chefs) != 0) {
                fprintf(stderr, "%s: --chefs wants an integer\n", argv[0]);
                return 2;
            }
        } else if (strcmp(a, "--slot") == 0) {
            if (parse_int(value, &slot) != 0) {
                fprintf(stderr, "%s: --slot wants an integer\n", argv[0]);
                return 2;
            }
        } else if (strcmp(a, "--hz") == 0) {
            if (parse_double(value, &hz) != 0) {
                fprintf(stderr, "%s: --hz wants a number\n", argv[0]);
                return 2;
            }
        } else if (strcmp(a, "--duration") == 0) {
            if (parse_double(value, &duration) != 0) {
                fprintf(stderr, "%s: --duration wants a number\n", argv[0]);
                return 2;
            }
        } else if (strcmp(a, "--target") == 0) {
            /* "X,Y"：一个固定目标点。逗号分隔，两边都要能解析成有限数。 */
            const char *comma = strchr(value, ',');
            if (comma == NULL
                || parse_double_prefix(value, comma - value, &target_x) != 0
                || parse_double(comma + 1, &target_y) != 0) {
                fprintf(stderr, "%s: --target wants X,Y\n", argv[0]);
                return 2;
            }
            fixed_target = 1;
        } else if (strcmp(a, "--kind") == 0) {
            if (parse_kind(value, &kind) != 0) {
                fprintf(stderr, "%s: --kind must be move, stop, interact, pickup or dash\n",
                        argv[0]);
                return 2;
            }
        } else {
            fprintf(stderr, "%s: unknown argument '%s'\n", argv[0], a);
            usage(stderr, argv[0]);
            return 2;
        }
    }

    if (slot < 0 || slot >= KOS_SHM_MAX_CHEFS) {
        fprintf(stderr, "%s: --slot %ld is outside 0..%d\n", argv[0], slot,
                KOS_SHM_MAX_CHEFS - 1);
        return 2;
    }
    if (chefs > 0 && slot >= chefs) {
        fprintf(stderr, "%s: --slot %ld is not one of the %ld chefs\n", argv[0], slot, chefs);
        return 2;
    }
    if (hz <= 0.0 || hz > 1000.0) {
        fprintf(stderr, "%s: --hz must be within 0..1000\n", argv[0]);
        return 2;
    }
    if (duration < 0.0) {
        fprintf(stderr, "%s: --duration must not be negative\n", argv[0]);
        return 2;
    }
    period_ns = (long)(1000000000.0 / hz);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART keeps clock_nanosleep from being restarted mid-frame; we want the
     * EINTR so a SIGTERM ends the loop promptly instead of after the next period. */
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    kos_shm_opts_init(&opts);
    if (state_name != NULL) {
        opts.object_name = state_name;
    }
    if (sem_name != NULL) {
        opts.sem_name = sem_name;
    }

    rc = kos_shm_open(&opts, KOS_SHM_ATTACH, &shm, &err);
    if (rc != KOS_OK) {
        char detail[256];
        kos_error_format(&err, detail, sizeof detail);
        fprintf(stderr, "%s: cannot attach to '%s': %s\n", argv[0],
                opts.object_name != NULL ? opts.object_name : KOS_SHM_DEFAULT_NAME, detail);
        fprintf(stderr, "  (the shared segment is created by the Bridge; start it first)\n");
        return 1;
    }

    fprintf(stdout, "worker-stub slot=%ld chefs=%ld hz=%.1f duration=%.1fs kind=%s\n", slot, chefs,
            hz, duration, kos_action_kind_name(kind));
    fflush(stdout);

    clock_gettime(CLOCK_MONOTONIC, &start);
    next = start;
    add_ns(&next, period_ns);

    while (!g_stop) {
        kos_input_action action;
        struct timespec now;

        /* 退出判据是**真实流逝的时间**，不是"发够 duration*hz 帧"。帧数可以靠突发
         * 追回来，真实时间不能；按帧数计时会让一个被抢占的 stub 提前退出，于是
         * "这段时间里那格有没有新输入"就取决于机器忙不忙。 */
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (duration > 0.0 && seconds_between(&start, &now) >= duration) {
            break;
        }

        /* 三角波：x 在 STUB_X0..STUB_X1 之间来回，y 带上槽号，于是相邻帧的输入
         * 互不相同，日志里 idle（stop）与真实输入（move）一眼可辨。 */
        {
            long step = (long)(frame % (uint64_t)(2 * STUB_SPAN));
            float t = (step <= STUB_SPAN) ? (float)step / STUB_SPAN
                                          : (float)(2 * STUB_SPAN - step) / STUB_SPAN;

            memset(&action, 0, sizeof action);
            action.frame_id = frame + 1; /* 0 保留给"从没写过" */
            action.chef_id = (int32_t)slot;
            action.kind = kind;
            action.target_x = fixed_target ? (float)target_x : STUB_X0 + t * (STUB_X1 - STUB_X0);
            action.target_y = fixed_target ? (float)target_y : (float)slot;
            action.target_object[0] = '\0';
            if (kind == KOS_ACTION_INTERACT) {
                /* interact 必须带一个非空目标，否则 Bridge 只能把它降级成 idle */
                snprintf(action.target_object, sizeof action.target_object,
                         "cutting_board_%ld", slot);
            }
        }

        rc = kos_shm_slot_publish(shm, (uint32_t)slot, &action);
        if (rc != KOS_OK) {
            char detail[256];
            kos_error_format(&err, detail, sizeof detail);
            fprintf(stderr, "%s: publish frame %llu into slot %ld failed (rc=%d) %s\n", argv[0],
                    (unsigned long long)(frame + 1), slot, rc, detail);
            kos_shm_close(shm, NULL);
            return 1;
        }

        frame++;
        sleep_until(&next);

        /* 错过的那一拍直接跳过，不补发。落后就连发会把 stub 变成一台压力机，
         * 而它在这里的角色是"一个有固定写槽频率的 Worker"。 */
        clock_gettime(CLOCK_MONOTONIC, &now);
        add_ns(&next, period_ns);
        if (ts_before(&next, &now)) {
            next = now;
            add_ns(&next, period_ns);
        }
    }

    printf("worker-stub slot=%ld published %llu frames\n", slot, (unsigned long long)frame);
    fflush(stdout);

    /* 只解除映射：段和信号量归 Bridge，替身无权删除它们。 */
    kos_shm_close(shm, NULL);
    return 0;
}
