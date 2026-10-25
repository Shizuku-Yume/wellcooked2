/*
 * test_shm.c —— 共享世界状态与输入槽的单元测试。
 *
 * 这个文件想证明的不是"函数返回值对"，而是三件在并发下才成立的事：
 *
 *   1. seqlock 真的挡住了撕裂：父进程不停改写世界状态，若干子进程同时读，
 *      读者拿到的每一份快照都必须自洽（载荷里的字段全部由同一帧推出）。
 *      为了说明这个断言不是空话，前面先做一次**反证**：父进程故意不走 seqlock
 *      地改一个字段，读者必须报出不一致 —— 会失败的自检才叫自检。
 *   2. 输入槽 + 信号量这条路径能判帧：等 N 格同一 frame_id 会成功，
 *      少一格会超时并点名是哪一格。
 *   3. 清理干净：unlink 之后重新 create + attach 能拿到全新的段。
 *
 * 每个用例用带 pid 的独立名字，跑测试不会碰到别人的段，崩溃残留也不会互相撞车。
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/shm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define READERS 4        /* 并发读的进程数：比 CPU 少几个，别把机器挤爆 */
#define WRITE_FRAMES 600u /* 正证阶段写多少帧 */
#define WRITE_PERIOD_MS 1 /* 出帧间隔：真实 Bridge 是 60 Hz（16 ms），这里快 16 倍 */
#define TEAR_WAIT_MS 2000 /* 反证阶段等读者报出不一致的上限 */

static int g_checks;
static int g_failures;

static void expect(int cond, const char *what)
{
    g_checks++;
    if (cond)
        printf("  ok   %s\n", what);
    else {
        g_failures++;
        printf("  FAIL %s\n", what);
    }
}

static void show_error(const char *where, const kos_error *err)
{
    char buf[256];

    kos_error_format(err, buf, sizeof buf);
    printf("       %s: %s\n", where, buf);
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* "/dev/shm" + "/kitchenos_test_x" —— 手写拼接，长度自己算得清。 */
static void object_path(char *dst, size_t cap, const char *name)
{
    size_t dl = strlen(KOS_SHM_DEFAULT_DIR);
    size_t nl = strlen(name);

    if (dl + nl + 1 > cap) {
        dst[0] = '\0';
        return;
    }
    memcpy(dst, KOS_SHM_DEFAULT_DIR, dl);
    memcpy(dst + dl, name, nl);
    dst[dl + nl] = '\0';
}

static int object_exists(const char *name)
{
    char path[KOS_SHM_PATH_MAX * 2];

    object_path(path, sizeof path, name);
    return access(path, F_OK) == 0;
}

/* 每次用例一套独立名字：隔离靠名字，因为 POSIX 命名对象只有 /dev/shm 一个落点。 */
static void make_names(char *shm_name, char *sem_name, const char *tag)
{
    snprintf(shm_name, KOS_SHM_PATH_MAX, "/kitchenos_test_%s_%ld", tag, (long)getpid());
    snprintf(sem_name, KOS_SHM_PATH_MAX, "/kitchenos_test_sem_%s_%ld", tag, (long)getpid());
}

static void fill_opts(kos_shm_opts *opts, const char *shm_name, const char *sem_name)
{
    kos_shm_opts_init(opts);
    opts->object_name = shm_name;
    opts->sem_name = sem_name;
    opts->dir = KOS_SHM_DEFAULT_DIR;
}

/* err_out 可空：要检查错误文本的用例才传，避免读到上一次调用留下的旧内容。 */
static int open_segment(const char *shm_name, const char *sem_name, kos_shm_mode mode,
                        kos_shm **out, kos_error *err_out)
{
    kos_shm_opts opts;
    kos_error err;
    int rc;

    fill_opts(&opts, shm_name, sem_name);
    rc = kos_shm_open(&opts, mode, out, &err);
    if (rc != KOS_OK)
        show_error("kos_shm_open", &err);
    if (err_out)
        *err_out = err;
    return rc;
}

/* 用例自己收拾自己建的段：/dev/shm 是 tmpfs，漏出来的对象会一直占着内存，
 * 而且会让"Bridge 退出后 /dev/shm 里没有 kitchenos 对象"这条检查失真。
 * 所以收尾一律走这里 —— 提前 return 的失败路径也必须走到（用 goto done）。
 * 幂等：没建过、已经删过都不算错。 */
static void drop_section(const char *shm_name, const char *sem_name)
{
    kos_shm_opts opts;
    kos_error err;

    fill_opts(&opts, shm_name, sem_name);
    (void)kos_shm_unlink(&opts, &err);
}

/* ============================================================ 不变式 */

/*
 * 一帧世界状态的内容全部由帧号推出。读者只要能从 game_time 反推出每一个字段，
 * 就说明这份快照没有把两帧拼在一起 —— 这正是撕裂读的signature。
 * 浮点用 *16 的小系数：值域远小于 2^24，float 能精确表示，比较才不含糊。
 */
static void fill_frame(kos_world_state *w, unsigned long long frame)
{
    unsigned i, j;

    kos_world_state_reset(w);
    w->score = (int64_t)frame * 7 + 1;
    w->game_time = (int64_t)frame;
    w->chef_count = KOS_SHM_MAX_CHEFS;
    for (i = 0; i < KOS_SHM_MAX_CHEFS; i++) {
        w->chefs[i].id = (int32_t)i;
        w->chefs[i].x = (float)((double)frame * 16.0 + (double)i);
        w->chefs[i].y = (float)((double)frame * 16.0 - (double)i);
        snprintf(w->chefs[i].holding, sizeof w->chefs[i].holding, "h%llu", frame);
        snprintf(w->chefs[i].state, sizeof w->chefs[i].state, "s%llu", frame);
    }
    w->order_count = KOS_SHM_MAX_ORDERS;
    for (i = 0; i < KOS_SHM_MAX_ORDERS; i++) {
        snprintf(w->orders[i].id, sizeof w->orders[i].id, "order_%03u", (unsigned)(frame % 1000u));
        snprintf(w->orders[i].recipe, sizeof w->orders[i].recipe, "r%llu", frame);
        w->orders[i].time_remaining = (int32_t)(frame + i);
        w->orders[i].ingredient_count = KOS_SHM_MAX_ORDER_INGREDIENTS;
        for (j = 0; j < KOS_SHM_MAX_ORDER_INGREDIENTS; j++)
            snprintf(w->orders[i].ingredients[j], sizeof w->orders[i].ingredients[j], "i%03u_%u",
                     (unsigned)(frame % 1000u), j);
    }
    w->resource_count = KOS_SHM_MAX_RESOURCES;
    for (i = 0; i < KOS_SHM_MAX_RESOURCES; i++) {
        snprintf(w->resources[i].name, sizeof w->resources[i].name, "res_%u", i);
        snprintf(w->resources[i].state, sizeof w->resources[i].state, "st%llu", frame);
        snprintf(w->resources[i].content, sizeof w->resources[i].content, "c%llu", frame);
        w->resources[i].time_left = (float)((double)frame * 8.0 + (double)i);
        w->resources[i].chef_id = (int32_t)(i % KOS_SHM_MAX_CHEFS);
        w->resources[i].count = (int32_t)(frame + i);
    }
}

/* 自洽返回 1；把反推出的帧号写进 *frame_out。 */
static int frame_consistent(const kos_world_state *w, unsigned long long *frame_out)
{
    char buf[KOS_SHM_NAME_MAX];
    unsigned long long f;
    unsigned i, j;

    if (w->game_time < 0)
        return 0;
    f = (unsigned long long)w->game_time;
    if (w->score != (int64_t)f * 7 + 1)
        return 0;
    if (w->chef_count != KOS_SHM_MAX_CHEFS)
        return 0;
    if (w->order_count != KOS_SHM_MAX_ORDERS)
        return 0;
    if (w->resource_count != KOS_SHM_MAX_RESOURCES)
        return 0;

    for (i = 0; i < KOS_SHM_MAX_CHEFS; i++) {
        if (w->chefs[i].id != (int32_t)i)
            return 0;
        if (w->chefs[i].x != (float)((double)f * 16.0 + (double)i))
            return 0;
        if (w->chefs[i].y != (float)((double)f * 16.0 - (double)i))
            return 0;
        snprintf(buf, sizeof buf, "h%llu", f);
        if (strcmp(w->chefs[i].holding, buf) != 0)
            return 0;
        snprintf(buf, sizeof buf, "s%llu", f);
        if (strcmp(w->chefs[i].state, buf) != 0)
            return 0;
    }
    for (i = 0; i < KOS_SHM_MAX_ORDERS; i++) {
        snprintf(buf, sizeof buf, "order_%03u", (unsigned)(f % 1000u));
        if (strcmp(w->orders[i].id, buf) != 0)
            return 0;
        snprintf(buf, sizeof buf, "r%llu", f);
        if (strcmp(w->orders[i].recipe, buf) != 0)
            return 0;
        if (w->orders[i].time_remaining != (int32_t)(f + i))
            return 0;
        if (w->orders[i].ingredient_count != KOS_SHM_MAX_ORDER_INGREDIENTS)
            return 0;
        for (j = 0; j < KOS_SHM_MAX_ORDER_INGREDIENTS; j++) {
            snprintf(buf, sizeof buf, "i%03u_%u", (unsigned)(f % 1000u), j);
            if (strcmp(w->orders[i].ingredients[j], buf) != 0)
                return 0;
        }
    }
    for (i = 0; i < KOS_SHM_MAX_RESOURCES; i++) {
        snprintf(buf, sizeof buf, "res_%u", i);
        if (strcmp(w->resources[i].name, buf) != 0)
            return 0;
        snprintf(buf, sizeof buf, "st%llu", f);
        if (strcmp(w->resources[i].state, buf) != 0)
            return 0;
        snprintf(buf, sizeof buf, "c%llu", f);
        if (strcmp(w->resources[i].content, buf) != 0)
            return 0;
        if (w->resources[i].time_left != (float)((double)f * 8.0 + (double)i))
            return 0;
        if (w->resources[i].chef_id != (int32_t)(i % KOS_SHM_MAX_CHEFS))
            return 0;
        if (w->resources[i].count != (int32_t)(f + i))
            return 0;
    }
    if (frame_out)
        *frame_out = f;
    return 1;
}

/* ================================================== 并发读的统计块 */

enum { MODE_HONEST = 0, MODE_RAW = 1 };

struct reader_stats {
    unsigned long long reads;
    unsigned long long attempts;
    unsigned long long collided;     /* 读数里"拷过一次以上"的次数：证明与写者真的重叠 */
    unsigned long long tears;        /* 反证阶段（RAW）检出的人为不一致次数 */
    unsigned long long honest_tears; /* 正常读法下快照不自洽的次数：必须为 0 */
    unsigned long long busy;         /* 尝试次数用尽：必须为 0 */
    unsigned long long errors;       /* 别的失败码：必须为 0 */
    unsigned ack;                    /* 已确认的 epoch：父进程靠它确认模式切换生效 */
};

struct ctl {
    int stop;
    int mode;      /* MODE_HONEST / MODE_RAW，先于 epoch 写 */
    unsigned epoch; /* 每次切模式 +1：读者确认后才能开始下一阶段 */
    struct reader_stats stats[READERS];
};

/* 换模式并等所有读者确认。必须先写 mode 再写 epoch，读者看到新 epoch 时模式已经定了。 */
static int switch_mode(struct ctl *ctl, int mode, unsigned epoch, int readers)
{
    int64_t deadline;
    int i;

    __atomic_store_n(&ctl->mode, mode, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&ctl->epoch, epoch, __ATOMIC_RELAXED);

    deadline = now_ms() + 2000;
    for (;;) {
        int all = 1;

        for (i = 0; i < readers; i++) {
            if (__atomic_load_n(&ctl->stats[i].ack, __ATOMIC_RELAXED) != epoch)
                all = 0;
        }
        if (all)
            return 1;
        if (now_ms() > deadline)
            return 0;
        sleep_ms(2);
    }
}

static int reader_child(const char *shm_name, const char *sem_name, struct ctl *ctl, int index)
{
    kos_shm *shm = NULL;
    struct reader_stats *st = &ctl->stats[index];
    unsigned seen = 0;
    int rc;

    rc = open_segment(shm_name, sem_name, KOS_SHM_ATTACH, &shm, NULL);
    if (rc != KOS_OK)
        return 1;

    while (!__atomic_load_n(&ctl->stop, __ATOMIC_RELAXED)) {
        kos_world_state w;
        unsigned long long frame;
        unsigned ep = __atomic_load_n(&ctl->epoch, __ATOMIC_RELAXED);

        if (ep != seen) {
            /* 只在这里确认模式切换：确认之后本进程不会再回到上一阶段的读法，
             * 父进程随后统计到的数字才是干净的。 */
            seen = ep;
            __atomic_store_n(&st->ack, ep, __ATOMIC_RELAXED);
            continue;
        }

        if (__atomic_load_n(&ctl->mode, __ATOMIC_RELAXED) == MODE_RAW) {
            /* 反证阶段专用：故意不走 seqlock，直接拷载荷。 */
            memcpy(&w, &shm->seg->world, sizeof w);
            st->reads++;
            if (!frame_consistent(&w, &frame))
                st->tears++;
            continue;
        }

        {
            uint32_t attempts = 0;
            int r = kos_shm_world_read(shm, &w, 0, &attempts);

            st->reads++;
            st->attempts += attempts;
            if (attempts > 1)
                st->collided++; /* 这一读第一次拷到的是撕裂的，说明当时确实有写者在写 */
            if (r == KOS_OK) {
                if (!frame_consistent(&w, &frame))
                    st->honest_tears++; /* 库说自洽、不变式说不是：seqlock 没做到它承诺的事 */
            } else if (r == KOS_ERR_BUSY) {
                st->busy++;
            } else {
                st->errors++;
            }
        }
    }

    kos_shm_close(shm, NULL);
    return (st->reads > 0 && st->honest_tears == 0 && st->busy == 0 && st->errors == 0) ? 0 : 1;
}

static int collect_child(pid_t pid, const char *who)
{
    int status = 0;

    if (waitpid(pid, &status, 0) < 0) {
        printf("       waitpid(%s) 失败: %s\n", who, strerror(errno));
        return 0;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("       %s 退出异常: exited=%d code=%d signal=%d\n", who, WIFEXITED(status),
               WIFEXITED(status) ? WEXITSTATUS(status) : -1,
               WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        return 0;
    }
    return 1;
}

/* ================================================== 1. 名字解析与覆盖 */

static void test_names(void)
{
    kos_shm_opts opts;
    kos_shm *shm = NULL;
    kos_error err;
    int rc;

    printf("[test_shm] 1. 名字解析与配置覆盖\n");

    setenv(KOS_SHM_ENV_NAME, "/kitchenos_env_state", 1);
    setenv(KOS_SHM_ENV_SEM_NAME, "/kitchenos_env_sem", 1);
    setenv(KOS_SHM_ENV_DIR, "/dev/shm", 1);
    kos_shm_opts_init(&opts);
    expect(opts.object_name && strcmp(opts.object_name, "/kitchenos_env_state") == 0,
           "env KITCHENOS_SHM_NAME 覆盖默认对象名");
    expect(opts.sem_name && strcmp(opts.sem_name, "/kitchenos_env_sem") == 0,
           "env KITCHENOS_SEM_NAME 覆盖默认信号量名");
    expect(opts.dir && strcmp(opts.dir, "/dev/shm") == 0, "env KITCHENOS_SHM_DIR 覆盖默认目录");
    unsetenv(KOS_SHM_ENV_NAME);
    unsetenv(KOS_SHM_ENV_SEM_NAME);
    unsetenv(KOS_SHM_ENV_DIR);
    kos_shm_opts_init(&opts);
    expect(opts.object_name == NULL && opts.sem_name == NULL && opts.dir == NULL,
           "env 清空后 opts 回到『用默认值』状态");

    /* 名字格式：glibc 对含 '/' 的名字直接 EINVAL，提前拦住给一句人话。 */
    opts.object_name = "kitchenos_state";
    opts.sem_name = "/kitchenos_test_sem_names";
    rc = kos_shm_open(&opts, KOS_SHM_CREATE, &shm, &err);
    expect(rc == KOS_ERR_INVAL, "对象名不以 '/' 开头 → KOS_ERR_INVAL");
    opts.object_name = "/nested/state";
    rc = kos_shm_open(&opts, KOS_SHM_CREATE, &shm, &err);
    expect(rc == KOS_ERR_INVAL, "对象名含内嵌 '/' → KOS_ERR_INVAL（POSIX 命名对象只有单段名）");
    opts.object_name = KOS_SHM_DEFAULT_NAME;
    opts.dir = "dev/shm";
    rc = kos_shm_open(&opts, KOS_SHM_CREATE, &shm, &err);
    expect(rc == KOS_ERR_DIR, "shm 目录不是绝对路径 → KOS_ERR_DIR");
    expect(shm == NULL, "失败的 open 不返回句柄");
}

/* ============================================ 2. 创建、挂接、版本校验 */

static void test_open_and_version(void)
{
    char shm_name[KOS_SHM_PATH_MAX];
    char sem_name[KOS_SHM_PATH_MAX];
    char missing_name[KOS_SHM_PATH_MAX];
    kos_shm *shm = NULL;
    kos_shm *second = NULL;
    kos_error err;
    kos_world_state w;
    kos_shm_opts opts;
    uint32_t attempts = 0;
    pid_t pid;
    int rc;

    printf("[test_shm] 2. 创建、挂接、版本与布局校验\n");
    make_names(shm_name, sem_name, "open");
    snprintf(missing_name, sizeof missing_name, "/kitchenos_test_missing_%ld", (long)getpid());

    rc = open_segment(missing_name, sem_name, KOS_SHM_ATTACH, &shm, NULL);
    expect(rc == KOS_ERR_MISSING, "挂接不存在的段 → KOS_ERR_MISSING");
    expect(!object_exists(missing_name), "挂接失败不会顺手建出对象");

    rc = open_segment(shm_name, sem_name, KOS_SHM_CREATE, &shm, NULL);
    expect(rc == KOS_OK, "创建段成功");
    if (rc != KOS_OK)
        goto done;
    expect(object_exists(shm_name), "对象落在 /dev/shm 里（配置目录核对通过）");
    expect(shm->created == 1, "句柄标记自己是创建者");

    rc = kos_shm_world_read(shm, &w, 0, &attempts);
    expect(rc == KOS_OK, "新建段立刻可读（创建时就发布了一份空世界）");
    expect(attempts == 1, "首读不需要重试");
    expect(w.chef_count == 0 && w.order_count == 0 && w.resource_count == 0 && w.score == 0,
           "新建段是空厨房");

    rc = open_segment(shm_name, sem_name, KOS_SHM_CREATE, &second, NULL);
    expect(rc == KOS_ERR_EXISTS, "重复创建同名的段 → KOS_ERR_EXISTS（绝不复用旧段）");
    expect(second == NULL, "失败时不给句柄");

    /* 版本/magic/布局三道闸门：每改一项都必须被拒绝挂接，而不是被将就着用。 */
    shm->seg->version = KOS_SHM_VERSION + 41u;
    kos_error_clear(&err);
    rc = open_segment(shm_name, sem_name, KOS_SHM_ATTACH, &second, &err);
    expect(rc == KOS_ERR_VERSION, "版本不匹配 → KOS_ERR_VERSION");
    expect(strstr(err.context, "版本") != NULL, "版本不匹配的错误上下文说得清是版本问题");
    shm->seg->version = KOS_SHM_VERSION;
    /* 旧版本挂接方必须被拒（版本号从 1 升到 2） */
    shm->seg->version = 1u;
    kos_error_clear(&err);
    rc = open_segment(shm_name, sem_name, KOS_SHM_ATTACH, &second, &err);
    expect(rc == KOS_ERR_VERSION, "旧版（版本 1）挂接方被拒 → KOS_ERR_VERSION");
    shm->seg->version = KOS_SHM_VERSION;


    shm->seg->magic = 0xdeadbeefu;
    rc = open_segment(shm_name, sem_name, KOS_SHM_ATTACH, &second, NULL);
    expect(rc == KOS_ERR_MAGIC, "magic 不对 → KOS_ERR_MAGIC");
    shm->seg->magic = KOS_SHM_MAGIC;

    shm->seg->header_size = shm->seg->header_size + 8u;
    rc = open_segment(shm_name, sem_name, KOS_SHM_ATTACH, &second, NULL);
    expect(rc == KOS_ERR_LAYOUT, "结构尺寸漂移 → KOS_ERR_LAYOUT");
    shm->seg->header_size = (uint32_t)sizeof(kos_shm_segment);

    /* 子进程按名字挂接：这才是真实进程架构里的路径（不是继承 fd）。 */
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        kos_shm *child = NULL;
        kos_world_state cw;
        int crc = open_segment(shm_name, sem_name, KOS_SHM_ATTACH, &child, NULL);

        if (crc != KOS_OK)
            _exit(1);
        crc = kos_shm_world_read(child, &cw, 0, NULL);
        if (crc != KOS_OK || cw.chef_count != 0)
            _exit(2);
        kos_shm_close(child, NULL);
        _exit(0);
    }
    expect(pid > 0 && collect_child(pid, "attach 子进程"), "另一个进程按名字挂接并读到同一份状态");

    /* 目录配置错时必须报错，而且不能留下孤儿对象。 */
    {
        char bad_name[KOS_SHM_PATH_MAX];
        char bad_sem[KOS_SHM_PATH_MAX];
        kos_shm *bad = NULL;

        make_names(bad_name, bad_sem, "baddir");
        fill_opts(&opts, bad_name, bad_sem);
        opts.dir = "/tmp";
        rc = kos_shm_open(&opts, KOS_SHM_CREATE, &bad, &err);
        expect(rc == KOS_ERR_DIR, "shm 目录指向非 /dev/shm 时创建 → KOS_ERR_DIR");
        expect(bad == NULL, "报错时不返回句柄");
        expect(!object_exists(bad_name), "创建失败已把对象清掉，不留半成品");
    }

    kos_shm_close(shm, NULL);
done:
    drop_section(shm_name, sem_name);
}

/* ================================ 3. seqlock：并发读 + 能失败的反证 */

static void test_seqlock_concurrency(void)
{
    char shm_name[KOS_SHM_PATH_MAX];
    char sem_name[KOS_SHM_PATH_MAX];
    kos_shm *shm = NULL;
    struct ctl *ctl = NULL; /* 提前失败时 done: 会看它，必须先初始化 */
    pid_t pids[READERS];
    kos_world_state w;
    unsigned long long total_reads = 0;
    unsigned long long total_collided = 0;
    unsigned long long total_tears = 0;
    unsigned long long total_honest_tears = 0;
    unsigned long long total_busy = 0;
    unsigned long long total_errors = 0;
    unsigned long long frames = 0;
    int64_t deadline;
    int started = 0;
    int alive = 1;
    int i;
    int rc;

    printf("[test_shm] 3. seqlock：并发读一致性（含反证实验）\n");
    make_names(shm_name, sem_name, "seq");
    rc = open_segment(shm_name, sem_name, KOS_SHM_CREATE, &shm, NULL);
    expect(rc == KOS_OK, "创建并发测试用的段");
    if (rc != KOS_OK)
        goto done;

    ctl = mmap(NULL, sizeof *ctl, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    expect(ctl != MAP_FAILED, "分配跨进程共享的统计块");
    if (ctl == MAP_FAILED)
        goto done;
    memset(ctl, 0, sizeof *ctl);

    fill_frame(&w, 1);
    expect(kos_shm_world_write(shm, &w) == KOS_OK, "发布第 1 帧");

    fflush(stdout);
    for (i = 0; i < READERS; i++) {
        pids[i] = fork();
        if (pids[i] == 0)
            _exit(reader_child(shm_name, sem_name, ctl, i));
        if (pids[i] < 0) {
            printf("       fork 第 %d 个读者失败: %s\n", i, strerror(errno));
            break;
        }
        started++;
    }
    expect(started == READERS, "启动 4 个并发读者进程");
    if (started == 0) {
        munmap(ctl, sizeof *ctl);
        ctl = NULL;
        goto done;
    }

    /* --- 反证：故意不走 seqlock 地改一个字段，读者必须报出不一致 ---
     * 这一段的意义是证明后面那句"没有撕裂"不是空的：同一套自洽性检查，
     * 面对真正撕裂的载荷是会报警的。 */
    expect(switch_mode(ctl, MODE_RAW, 1, started), "读者进入反证模式（故意绕过 seqlock）");
    sleep_ms(20);
    shm->seg->world.score = (int64_t)(2u * 7u + 1u); /* 和 game_time=1 对不上的一帧值 */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    deadline = now_ms() + TEAR_WAIT_MS;
    for (;;) {
        int all_seen = 1;

        for (i = 0; i < started; i++) {
            if (__atomic_load_n(&ctl->stats[i].tears, __ATOMIC_RELAXED) == 0)
                all_seen = 0;
        }
        if (all_seen || now_ms() > deadline)
            break;
        sleep_ms(5);
    }
    {
        int all_seen = 1;
        unsigned long long got = 0;

        for (i = 0; i < started; i++) {
            unsigned long long t = __atomic_load_n(&ctl->stats[i].tears, __ATOMIC_RELAXED);

            if (t == 0)
                all_seen = 0;
            got += t;
        }
        expect(all_seen, "反证：每个读者都检出了人为制造的不一致（自检本身能失败）");
        printf("       反证阶段共检出 %llu 次不一致\n", got);
    }

    /* 恢复自洽载荷，再切回正常读法；等所有读者确认后才开始统计。 */
    fill_frame(&w, 2);
    kos_shm_world_write(shm, &w);
    expect(switch_mode(ctl, MODE_HONEST, 2, started), "读者切回正常读法（经过 epoch 确认）");
    for (i = 0; i < started; i++)
        total_tears += __atomic_load_n(&ctl->stats[i].tears, __ATOMIC_RELAXED);

    /* --- 正证：父进程不停改写，读者只应看到自洽快照 ---
     * 写者按固定节奏出帧，而不是"能写多快写多快"：不节奏化的话，读者报出多少
     * KOS_ERR_BUSY 取决于机器当时多忙，测的是机器不是代码，结论会随负载飘。
     * 1 ms 一帧比真实 Bridge 的 60 Hz 快 16 倍，重叠照样密集，但写者总会留出偶数窗口。 */
    {
        struct timespec next;

        clock_gettime(CLOCK_MONOTONIC, &next);
        while (frames < WRITE_FRAMES) {
            fill_frame(&w, 100u + frames);
            if (kos_shm_world_write(shm, &w) != KOS_OK) {
                printf("       第 %llu 帧写入失败\n", frames);
                break;
            }
            frames++;
            next.tv_nsec += (long)WRITE_PERIOD_MS * 1000000L;
            if (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec += 1;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
    }
    expect(frames == WRITE_FRAMES, "父进程按 1 ms 一帧写满了预定帧数");
    __atomic_store_n(&ctl->stop, 1, __ATOMIC_RELAXED);

    for (i = 0; i < started; i++) {
        if (!collect_child(pids[i], "读者进程"))
            alive = 0;
    }
    expect(alive, "所有读者进程正常退出（自证阶段没有撕裂/错误）");

    for (i = 0; i < started; i++) {
        total_reads += __atomic_load_n(&ctl->stats[i].reads, __ATOMIC_RELAXED);
        total_collided += __atomic_load_n(&ctl->stats[i].collided, __ATOMIC_RELAXED);
        total_honest_tears += __atomic_load_n(&ctl->stats[i].honest_tears, __ATOMIC_RELAXED);
        total_busy += __atomic_load_n(&ctl->stats[i].busy, __ATOMIC_RELAXED);
        total_errors += __atomic_load_n(&ctl->stats[i].errors, __ATOMIC_RELAXED);
    }

    printf("       写了 %llu 帧（每 %d ms 一帧）；%d 个读者共读 %llu 次，其中撞上写者 %llu 次"
           "（反证阶段另计 %llu 次不一致）\n",
           frames, WRITE_PERIOD_MS, started, total_reads, total_collided, total_tears);
    printf("       正证阶段：撕裂 %llu，重试耗尽 %llu，其它错误 %llu\n", total_honest_tears,
           total_busy, total_errors);
    expect(total_reads > 1000ull, "并发读者真的读了很多次");
    expect(total_collided > 0ull, "读者与写者确实重叠过（不是各跑各的）");
    expect(total_honest_tears == 0, "正证：并发读写期间没有任何一份快照出现撕裂");
    expect(total_busy == 0, "没有读者把重试次数用尽（写者不会长期卡在奇数序号）");
    expect(total_errors == 0, "读者没有遇到别的错误码");

    rc = kos_shm_world_read(shm, &w, 0, NULL);
    expect(rc == KOS_OK && frame_consistent(&w, NULL), "写循环结束后仍能读到自洽快照");

    /* 写者停在"正在写"的中间（序号为奇）时，读者必须等，绝不能把半份载荷当快照交出去。
     * 这一条不靠调度运气：序号是我们自己停在奇数的，所以它是可重复的事实，
     * 和上面"正常并发下 0 次 BUSY"正好互为对照。 */
    memset(&w, 0, sizeof w);
    {
        kos_world_state *half = kos_shm_world_write_begin(shm);
        int64_t t0 = now_ms();
        uint32_t got = 0;

        expect(half != NULL, "写者进入写窗口（序号转奇）");
        half->score = 987654321; /* 半写的载荷：读者绝不该看到 */
        rc = kos_shm_world_read(shm, &w, 4, &got);
        expect(rc == KOS_ERR_BUSY, "写者持奇序号期间读者报 KOS_ERR_BUSY，而不是交回半份数据");
        expect(got == 0, "读者一次都没往里拷：它等的是写者，不是拿旧载荷凑数");
        expect(w.score == 0, "半写的载荷没有漏给读者（输出缓冲一点没被动过）");
        expect(now_ms() - t0 >= 200, "读者是等到期限才放弃的，不是看一眼就放弃");
        printf("       写者卡在奇数序号时，读者等了 %lld ms 才报 BUSY（正常并发下是 0 次）\n",
               (long long)(now_ms() - t0));
        kos_shm_world_write_end(shm);

        fill_frame(&w, 7u);
        expect(kos_shm_world_write(shm, &w) == KOS_OK, "写者收尾后重新发布一帧");
        rc = kos_shm_world_read(shm, &w, 0, NULL);
        expect(rc == KOS_OK && frame_consistent(&w, NULL), "写者一放手，读者立刻又能读到自洽快照");
    }

    munmap(ctl, sizeof *ctl);
    ctl = NULL;
done:
    if (ctl && ctl != MAP_FAILED)
        munmap(ctl, sizeof *ctl);
    if (shm)
        kos_shm_close(shm, NULL);
    drop_section(shm_name, sem_name);
}

/* ================================== 4. 输入槽、信号量、帧屏障 */

struct publish_arg {
    const char *shm_name;
    const char *sem_name;
    unsigned slot;
    unsigned long long frame_id;
    int delay_ms;
};

static int publisher_child(const struct publish_arg *arg)
{
    kos_shm *shm = NULL;
    kos_input_action action;
    int rc;

    rc = open_segment(arg->shm_name, arg->sem_name, KOS_SHM_ATTACH, &shm, NULL);
    if (rc != KOS_OK)
        return 1;
    if (arg->delay_ms > 0)
        sleep_ms(arg->delay_ms);

    memset(&action, 0, sizeof action);
    action.frame_id = arg->frame_id;
    action.chef_id = (int32_t)arg->slot;
    action.kind = KOS_ACTION_MOVE;
    action.target_x = (float)arg->slot * 1.5f;
    action.target_y = (float)arg->frame_id;
    snprintf(action.target_object, sizeof action.target_object, "pot_%u", arg->slot);

    rc = kos_shm_slot_publish(shm, arg->slot, &action);
    kos_shm_close(shm, NULL);
    return rc == KOS_OK ? 0 : 1;
}

static pid_t spawn_publisher(const struct publish_arg *arg)
{
    pid_t pid = fork();

    if (pid == 0)
        _exit(publisher_child(arg));
    return pid;
}

static void test_slots_and_barrier(void)
{
    char shm_name[KOS_SHM_PATH_MAX];
    char sem_name[KOS_SHM_PATH_MAX];
    kos_shm *shm = NULL;
    kos_input_action action;
    kos_input_action got;
    struct publish_arg arg;
    pid_t pids[2];
    uint64_t missing = 0;
    int64_t t0;
    int64_t elapsed;
    int rc;

    printf("[test_shm] 4. 输入槽、就绪信号量与帧屏障\n");
    make_names(shm_name, sem_name, "slots");
    rc = open_segment(shm_name, sem_name, KOS_SHM_CREATE, &shm, NULL);
    expect(rc == KOS_OK, "创建输入槽测试用的段");
    if (rc != KOS_OK)
        goto done;

    /* 单槽语义：就绪判定必须是确定的是/否 */
    expect(kos_shm_slot_frame_ready(shm, 0, 7) == 0, "没写过的槽在第 7 帧不算就绪");
    rc = kos_shm_slot_read(shm, 0, &got);
    expect(rc == KOS_ERR_EMPTY, "读没写过的槽 → KOS_ERR_EMPTY");

    memset(&action, 0, sizeof action);
    action.frame_id = 7;
    action.chef_id = 0;
    action.kind = KOS_ACTION_PICKUP;
    action.target_x = 3.25f;
    action.target_y = -1.5f;
    snprintf(action.target_object, sizeof action.target_object, "tomato");
    rc = kos_shm_slot_publish(shm, 0, &action);
    expect(rc == KOS_OK, "Worker 发布第 7 帧到 0 号槽");

    expect(kos_shm_slot_frame_ready(shm, 0, 7) == 1, "第 7 帧的 0 号槽就绪 → 是");
    expect(kos_shm_slot_frame_ready(shm, 0, 8) == 0, "第 8 帧的 0 号槽就绪 → 否");
    expect(kos_shm_slot_frame_ready(shm, 1, 7) == 0, "第 7 帧的 1 号槽就绪 → 否（没写）");
    rc = kos_shm_slot_read_for_frame(shm, 0, 7, &got);
    expect(rc == KOS_OK, "读第 7 帧的 0 号槽 → OK");
    expect(got.kind == KOS_ACTION_PICKUP && got.chef_id == 0 && got.target_x == 3.25f &&
               got.target_y == -1.5f && strcmp(got.target_object, "tomato") == 0,
           "读到的是原样载荷（含目标物字符串）");
    rc = kos_shm_slot_read_for_frame(shm, 0, 9, &got);
    expect(rc == KOS_ERR_STALE, "读第 9 帧的 0 号槽 → KOS_ERR_STALE（有内容但不是这帧）");
    expect(kos_shm_slot_publish(shm, KOS_SHM_MAX_CHEFS, &action) == KOS_ERR_RANGE,
           "槽号越界 → KOS_ERR_RANGE");
    action.kind = 99;
    expect(kos_shm_slot_publish(shm, 0, &action) == KOS_ERR_INVAL, "未知动作种类 → KOS_ERR_INVAL");
    action.kind = KOS_ACTION_MOVE;

    /* 信号量：有 post 立刻返回，没有就超时 */
    (void)kos_shm_drain_ready(shm);
    expect(kos_shm_wait_ready(shm, 50) == KOS_ERR_TIMEOUT, "门铃没响时 wait(50ms) → 超时");
    expect(kos_shm_post_ready(shm) == KOS_OK, "post 门铃");
    expect(kos_shm_wait_ready(shm, 50) == KOS_OK, "门铃响过后 wait(50ms) → 立刻返回");
    expect(kos_shm_drain_ready(shm) == 0, "门铃已被取走，再 drain 得到 0");

    /* 帧屏障：两个 Worker 各写一格、同一帧，父进程等到齐 */
    (void)kos_shm_drain_ready(shm);
    arg.shm_name = shm_name;
    arg.sem_name = sem_name;
    arg.frame_id = 42;
    arg.delay_ms = 120;
    arg.slot = 0;
    pids[0] = spawn_publisher(&arg);
    arg.slot = 1;
    pids[1] = spawn_publisher(&arg);
    t0 = now_ms();
    missing = 0xffffu;
    rc = kos_shm_wait_frame(shm, 42, 2, 4000, &missing);
    elapsed = now_ms() - t0;
    expect(rc == KOS_OK && missing == 0, "等 2 格同帧：全部到齐");
    expect(elapsed >= 100, "确实是等到了子进程发布之后才返回（不是立刻蒙对）");
    printf("       等齐用了 %lld ms\n", (long long)elapsed);
    expect(collect_child(pids[0], "发布者 0") && collect_child(pids[1], "发布者 1"),
           "两个发布者进程正常退出");

    /* 少一格：必须超时，并点名缺的是哪一格 */
    (void)kos_shm_drain_ready(shm);
    arg.frame_id = 43;
    arg.delay_ms = 60;
    arg.slot = 0;
    pids[0] = spawn_publisher(&arg);
    t0 = now_ms();
    missing = 0;
    rc = kos_shm_wait_frame(shm, 43, 2, 300, &missing);
    elapsed = now_ms() - t0;
    expect(rc == KOS_ERR_TIMEOUT, "只来 1 格时等 2 格 → KOS_ERR_TIMEOUT");
    expect(missing == 0x2u, "缺失掩码点名第 1 格（第 0 格已到）");
    expect(elapsed >= 250, "超时时间大致等于设定值，没有提前返回");
    printf("       超时用了 %lld ms，missing_mask=0x%llx\n", (long long)elapsed,
           (unsigned long long)missing);
    expect(collect_child(pids[0], "发布者 0"), "单个发布者进程正常退出");

    /* 水位式屏障：对接"frame_id 由 Worker 自己递增"的情形。
     * 水位起点取"已经消费掉的那一帧"，这才是 Bridge 的真实用法：它只关心有没有更新的输入。 */
    (void)kos_shm_drain_ready(shm);
    {
        kos_input_action consumed;
        uint64_t watermark[1];

        rc = kos_shm_slot_read(shm, 0, &consumed);
        expect(rc == KOS_OK, "先读出 0 号槽已经消费到的那一帧，作为水位起点");
        watermark[0] = consumed.frame_id;
        arg.frame_id = 900;
        arg.delay_ms = 40;
        arg.slot = 0;
        pids[0] = spawn_publisher(&arg);
        missing = 0xffffu;
        rc = kos_shm_wait_frame_after(shm, watermark, 1, 3000, &missing);
        expect(rc == KOS_OK && missing == 0 && watermark[0] == 900,
               "水位式屏障：等到比水位更新的版本，并把水位推进到 900");
        missing = 0xffffu;
        rc = kos_shm_wait_frame_after(shm, watermark, 1, 200, &missing);
        expect(rc == KOS_ERR_TIMEOUT && missing == 0x1u,
               "水位已推进：同一帧不会满足下一次等待 → 超时");
        expect(collect_child(pids[0], "发布者 0"), "水位用例的发布者进程正常退出");
    }

    /* 水位只该为"真的拿到了更新版本"的格子推进：没到的那格必须原地不动，
     * 否则下一帧会误以为它已经消费过，把这一格的输入永久漏掉。 */
    (void)kos_shm_drain_ready(shm);
    {
        kos_input_action snapshot;
        uint64_t watermark[2];

        rc = kos_shm_slot_read(shm, 1, &snapshot);
        expect(rc == KOS_OK && snapshot.frame_id == 42, "1 号槽停在更旧的第 42 帧");
        watermark[0] = 900;  /* 0 号槽已经消费到 900，只认更新的 */
        watermark[1] = 1000; /* 故意把 1 号槽的水位抬到它一时够不着的地方 */
        arg.frame_id = 901;
        arg.delay_ms = 30;
        arg.slot = 0;
        pids[0] = spawn_publisher(&arg);
        missing = 0;
        rc = kos_shm_wait_frame_after(shm, watermark, 2, 300, &missing);
        expect(rc == KOS_ERR_TIMEOUT, "两个格子里有一格拿不到更新版本 → 超时");
        printf("       missing_mask=0x%llx 水位=[%llu, %llu]\n", (unsigned long long)missing,
               (unsigned long long)watermark[0], (unsigned long long)watermark[1]);
        expect(missing == 0x2u, "缺失掩码只点名没更新的 1 号槽");
        expect(watermark[0] == 901 && watermark[1] == 1000,
               "水位只为真正拿到新版本的格子推进，超时的那格原地不动");
        expect(collect_child(pids[0], "发布者 0"), "水位用例的发布者进程正常退出");
    }

    kos_shm_close(shm, NULL);
done:
    drop_section(shm_name, sem_name);
}

/* ================================================== 5. 清理与复用 */

static void test_cleanup(void)
{
    char shm_name[KOS_SHM_PATH_MAX];
    char sem_name[KOS_SHM_PATH_MAX];
    kos_shm *shm = NULL;
    kos_shm_opts opts;
    kos_error err;
    kos_world_state w;
    kos_input_action action;
    int rc;

    printf("[test_shm] 6. 清理：unlink 之后能重新建段\n");
    make_names(shm_name, sem_name, "clean");
    rc = open_segment(shm_name, sem_name, KOS_SHM_CREATE, &shm, NULL);
    expect(rc == KOS_OK, "创建段");
    if (rc != KOS_OK)
        goto done;

    memset(&action, 0, sizeof action);
    action.frame_id = 5;
    action.kind = KOS_ACTION_DASH;
    kos_shm_slot_publish(shm, 0, &action);
    expect(object_exists(shm_name), "清理前对象存在于 /dev/shm");

    fill_opts(&opts, shm_name, sem_name);
    rc = kos_shm_unlink(&opts, &err);
    expect(rc == KOS_OK, "unlink 成功");
    expect(!object_exists(shm_name), "unlink 之后 /dev/shm 里不再有这个名字");
    expect(kos_shm_unlink(&opts, &err) == KOS_OK, "重复 unlink 幂等成功");

    kos_shm_close(shm, NULL);

    rc = open_segment(shm_name, sem_name, KOS_SHM_CREATE, &shm, NULL);
    expect(rc == KOS_OK, "unlink 之后重新创建成功");
    if (rc != KOS_OK)
        goto done;
    rc = kos_shm_world_read(shm, &w, 0, NULL);
    expect(rc == KOS_OK && w.chef_count == 0 && w.score == 0, "新段是全新的空状态");
    expect(kos_shm_slot_frame_ready(shm, 0, 5) == 0, "新段的输入槽也是空的（旧内容没跟过来）");

    kos_shm_close(shm, NULL);
    kos_shm_unlink(&opts, &err);
    expect(!object_exists(shm_name), "收尾清理干净");
done:
    drop_section(shm_name, sem_name);
}

/* ================================================== 6. 动作名映射 */

static void test_action_names(void)
{
    printf("[test_shm] 7. 动作种类与协议名互转\n");
    expect(strcmp(kos_action_kind_name(KOS_ACTION_MOVE), "move") == 0, "MOVE → \"move\"");
    expect(strcmp(kos_action_kind_name(KOS_ACTION_DASH), "dash") == 0, "DASH → \"dash\"");
    expect(strcmp(kos_action_kind_name(KOS_ACTION_PATH), "path") == 0, "PATH → \"path\"");
    expect(strcmp(kos_action_kind_name(KOS_ACTION_FACE), "face") == 0, "FACE → \"face\"");
    expect(strcmp(kos_action_kind_name(KOS_ACTION_WORK), "work") == 0, "WORK → \"work\"");
    expect(kos_action_kind_from_name("path") == KOS_ACTION_PATH, "\"path\" → KOS_ACTION_PATH");
    expect(kos_action_kind_from_name("face") == KOS_ACTION_FACE, "\"face\" → KOS_ACTION_FACE");
    expect(kos_action_kind_from_name("work") == KOS_ACTION_WORK, "\"work\" → KOS_ACTION_WORK");
    expect(kos_action_kind_from_name("interact") == KOS_ACTION_INTERACT,
           "\"interact\" → KOS_ACTION_INTERACT");
    expect(kos_action_kind_from_name("nonsense") == -1, "未知名字 → -1");
    expect(kos_action_kind_from_name(NULL) == -1, "空名字 → -1");
    expect(strcmp(kos_action_kind_name(4242), "unknown") == 0, "越界种类 → \"unknown\"");
}

/* ========================================= 5. path/face/work 的槽载荷 */

static void test_new_action_slots(void)
{
    char shm_name[KOS_SHM_PATH_MAX];
    char sem_name[KOS_SHM_PATH_MAX];
    kos_shm *shm = NULL;
    kos_input_action action;
    kos_input_action got;
    int rc;
    size_t i;

    printf("[test_shm] 5. path/face/work 的槽载荷与序号\n");
    make_names(shm_name, sem_name, "newactions");
    rc = open_segment(shm_name, sem_name, KOS_SHM_CREATE, &shm, NULL);
    expect(rc == KOS_OK, "创建新动作测试用的段");
    if (rc != KOS_OK)
        goto done;

    memset(&action, 0, sizeof action);
    action.frame_id = 1;
    action.chef_id = 1;
    action.kind = KOS_ACTION_PATH;
    action.seq = 1234567890123LL;
    action.waypoint_count = KOS_MAX_WAYPOINTS;
    for (i = 0; i < KOS_MAX_WAYPOINTS; i++) {
        action.waypoints[i].x = (float)i + 0.5f;
        action.waypoints[i].y = -(float)i;
    }
    rc = kos_shm_slot_publish(shm, 0, &action);
    expect(rc == KOS_OK, "8 个路点的 path 能发布（种类上界不再停在 dash）");
    rc = kos_shm_slot_read(shm, 0, &got);
    expect(rc == KOS_OK, "读回 path 载荷");
    expect(got.kind == KOS_ACTION_PATH && got.seq == 1234567890123LL &&
               got.waypoint_count == KOS_MAX_WAYPOINTS,
           "path 的 seq 与路点数原样保留");
    expect(got.waypoints[0].x == 0.5f && got.waypoints[7].x == 7.5f &&
               got.waypoints[7].y == -7.0f,
           "path 的 8 个路点逐项原样保留");

    /* 同一个 seq 再发一版（插件负责“只追加”），槽本身只要求载荷仍是完整的。 */
    action.frame_id = 2;
    action.waypoint_count = 4;
    rc = kos_shm_slot_publish(shm, 0, &action);
    expect(rc == KOS_OK, "同 seq 的 path 可以再次发布");
    rc = kos_shm_slot_read(shm, 0, &got);
    expect(rc == KOS_OK && got.seq == 1234567890123LL && got.waypoint_count == 4,
           "同 seq 的新一版仍是独立快照（seq 不被改写）");

    memset(&action, 0, sizeof action);
    action.frame_id = 3;
    action.kind = KOS_ACTION_FACE;
    snprintf(action.target_object, sizeof action.target_object, "cutting_board_2");
    rc = kos_shm_slot_publish(shm, 1, &action);
    expect(rc == KOS_OK, "face（target_object）能发布");
    rc = kos_shm_slot_read(shm, 1, &got);
    expect(rc == KOS_OK && got.kind == KOS_ACTION_FACE &&
               strcmp(got.target_object, "cutting_board_2") == 0,
           "face 的目标物原样保留");

    memset(&action, 0, sizeof action);
    action.frame_id = 4;
    action.kind = KOS_ACTION_WORK;
    snprintf(action.target_object, sizeof action.target_object, "sink_0");
    rc = kos_shm_slot_publish(shm, 2, &action);
    expect(rc == KOS_OK, "work 能发布");
    rc = kos_shm_slot_read(shm, 2, &got);
    expect(rc == KOS_OK && got.kind == KOS_ACTION_WORK &&
               strcmp(got.target_object, "sink_0") == 0,
           "work 的目标物原样保留");

    action.kind = KOS_ACTION_WORK + 1;
    expect(kos_shm_slot_publish(shm, 2, &action) == KOS_ERR_INVAL,
           "work 之后的下一个编号仍被当作未知动作拒绝");

    memset(&action, 0, sizeof action);
    memset(action.target_object, 'A', sizeof action.target_object);
    action.kind = KOS_ACTION_WORK;
    action.frame_id = 5;
    expect(kos_shm_slot_publish(shm, 2, &action) == KOS_ERR_INVAL,
           "没有 NUL 结尾的 target_object 被拒绝");
    memset(&action, 0, sizeof action);
    action.kind = KOS_ACTION_PATH;
    action.frame_id = 6;
    action.waypoint_count = KOS_MAX_WAYPOINTS + 1;
    expect(kos_shm_slot_publish(shm, 2, &action) == KOS_ERR_INVAL,
           "超过 8 个路点的输入动作被拒绝");

    kos_shm_close(shm, NULL);
    shm = NULL;
done:
    if (shm != NULL)
        kos_shm_close(shm, NULL);
    drop_section(shm_name, sem_name);
}

int main(void)
{
    printf("[test_shm] 开始：世界状态 seqlock + 输入槽 + 就绪信号量\n");

    test_names();
    test_open_and_version();
    test_seqlock_concurrency();
    test_slots_and_barrier();
    test_new_action_slots();
    test_cleanup();
    test_action_names();

    printf("[test_shm] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
