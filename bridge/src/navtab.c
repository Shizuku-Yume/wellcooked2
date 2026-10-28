/*
 * navtab.c -- /wellcooked_nav 的创建、挂接与两槽读写。
 *
 * 每槽单写者 + seqlock：写者把序号改奇数、写载荷、改偶数；读者只接受两次序号
 * 相同且载荷校验通过的快照。命名信号量只协调写者，读侧不拿锁——否则一个在
 * 临界区里被 SIGKILL 的 navd 会让所有 Worker 永远读不到计划。写者获取信号量
 * 超时后会重建它（POSIX 命名信号量没有 robust mutex），临界区只有几百字节。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <wellcooked/navtab.h>

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NAV_LOCK_WAIT_MS 100

static int64_t realtime_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

static int set_error(kos_error *err, int code, const char *fmt, ...)
{
    va_list ap;

    if (err != NULL) {
        err->code = code;
        err->sys_errno = errno;
        va_start(ap, fmt);
        vsnprintf(err->context, sizeof err->context, fmt, ap);
        va_end(ap);
    }
    return code;
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

static const char *object_name_of(const kos_navtab_opts *opts)
{
    const char *name = opts != NULL ? opts->object_name : NULL;

    if (name == NULL || name[0] == '\0') {
        return KOS_NAV_DEFAULT_NAME;
    }
    return name;
}

static const char *sem_name_of(const kos_navtab_opts *opts)
{
    const char *name = opts != NULL ? opts->sem_name : NULL;

    if (name == NULL || name[0] == '\0') {
        return KOS_NAV_DEFAULT_SEM_NAME;
    }
    return name;
}

void kos_navtab_opts_init(kos_navtab_opts *opts)
{
    if (opts == NULL) {
        return;
    }
    memset(opts, 0, sizeof *opts);
    opts->perm = 0600;
    {
        const char *env = getenv("WELLCOOKED_NAV_NAME");

        opts->object_name = (env != NULL && env[0] != '\0') ? env : KOS_NAV_DEFAULT_NAME;
    }
    {
        const char *env = getenv("WELLCOOKED_NAV_SEM_NAME");

        opts->sem_name =
            (env != NULL && env[0] != '\0') ? env : KOS_NAV_DEFAULT_SEM_NAME;
    }
}

int kos_navtab_default_names(const char *state_name, char *nav_name, size_t nav_len,
                             char *sem_name, size_t sem_len)
{
    if (nav_name == NULL || sem_name == NULL || nav_len == 0 || sem_len == 0) {
        return -1;
    }
    if (state_name == NULL || state_name[0] == '\0' ||
        strcmp(state_name, KOS_SHM_DEFAULT_NAME) == 0) {
        copy_str(nav_name, nav_len, KOS_NAV_DEFAULT_NAME);
        copy_str(sem_name, sem_len, KOS_NAV_DEFAULT_SEM_NAME);
        return 0;
    }
    if (strlen(state_name) + sizeof "_nav" > nav_len ||
        strlen(state_name) + sizeof "_nav_lock" > sem_len) {
        return -1;
    }
    snprintf(nav_name, nav_len, "%s_nav", state_name);
    snprintf(sem_name, sem_len, "%s_nav_lock", state_name);
    return 0;
}

static void nav_lock_init_timespec(struct timespec *ts, int64_t deadline_ms)
{
    ts->tv_sec = (time_t)(deadline_ms / 1000);
    ts->tv_nsec = (long)(deadline_ms % 1000) * 1000000L;
}

/*
 * 拿写锁。正常情况 sem_timedwait 立刻返回；超时几乎只可能是上一个持有者在临界区
 * 里被 SIGKILL。此时重建信号量而不是永远阻塞，临界区只有一次 memcpy。
 */
static void nav_write_lock(kos_navtab *tab)
{
    struct timespec deadline;

    if (tab == NULL || tab->sem == NULL) {
        return;
    }
    nav_lock_init_timespec(&deadline, realtime_ms() + NAV_LOCK_WAIT_MS);
    while (sem_timedwait(tab->sem, &deadline) != 0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno != ETIMEDOUT) {
            return; /* 信号量坏了也不要写者死等：seqlock 仍然保证数据自洽 */
        }
        /* 超时 ≈ 上一个持有者在临界区里死了；重建比永远写不进去好。 */
        (void)sem_unlink(tab->sem_name);
        (void)sem_close(tab->sem);
        tab->sem = sem_open(tab->sem_name, O_CREAT, 0600, 1);
        return;
    }
}

static void nav_write_unlock(kos_navtab *tab)
{
    if (tab != NULL && tab->sem != NULL) {
        (void)sem_post(tab->sem);
    }
}

/* 单槽 seqlock。seq 是 volatile：写者发布顺序不能被编译器重排到 memcpy 之前。 */
static int slot_read(volatile uint64_t *seq, void *dst, const void *src, size_t len)
{
    int attempt;

    for (attempt = 0; attempt < KOS_NAV_READ_ATTEMPTS; attempt++) {
        uint64_t before = *seq;

        if ((before & 1u) != 0) {
            continue;
        }
        __sync_synchronize();
        memcpy(dst, src, len);
        __sync_synchronize();
        if (*seq == before) {
            return KOS_OK;
        }
    }
    return KOS_ERR_BUSY;
}

static void slot_write(volatile uint64_t *seq, void *dst, const void *src, size_t len)
{
    uint64_t before = *seq;

    *seq = before + 1; /* 奇：正在写 */
    __sync_synchronize();
    memcpy(dst, src, len);
    __sync_synchronize();
    *seq = before + 2; /* 偶：发布 */
}

static int validate_segment(kos_navtab *tab, kos_error *err)
{
    const kos_nav_segment *seg = tab->seg;

    if (seg->magic != KOS_NAV_MAGIC) {
        return set_error(err, KOS_ERR_MAGIC, "导航表魔数不匹配: 0x%08x", seg->magic);
    }
    if (seg->version != KOS_NAV_VERSION) {
        return set_error(err, KOS_ERR_VERSION, "导航表版本不匹配: 段 %u / 本程序 %u",
                         seg->version, KOS_NAV_VERSION);
    }
    if (seg->header_size != sizeof(kos_nav_segment) ||
        seg->target_slot_size != sizeof(kos_nav_target_slot) ||
        seg->plan_slot_size != sizeof(kos_nav_plan_slot) ||
        seg->max_chefs != KOS_SHM_MAX_CHEFS) {
        return set_error(err, KOS_ERR_LAYOUT,
                         "导航表布局不匹配: header %u/%zu target %u/%zu plan %u/%zu chefs %u/%d",
                         seg->header_size, sizeof(kos_nav_segment), seg->target_slot_size,
                         sizeof(kos_nav_target_slot), seg->plan_slot_size,
                         sizeof(kos_nav_plan_slot), seg->max_chefs, KOS_SHM_MAX_CHEFS);
    }
    return KOS_OK;
}

static int open_locked(const kos_navtab_opts *opts, kos_navtab **out, int create,
                       kos_error *err)
{
    kos_navtab *tab;
    int open_flags;
    int fd;
    int rc;

    if (out == NULL) {
        return KOS_ERR_INVAL;
    }
    *out = NULL;
    tab = calloc(1, sizeof *tab);
    if (tab == NULL) {
        return set_error(err, KOS_ERR_NOMEM, "calloc(kos_navtab)");
    }
    tab->fd = -1;
    copy_str(tab->object_name, sizeof tab->object_name, object_name_of(opts));
    copy_str(tab->sem_name, sizeof tab->sem_name, sem_name_of(opts));
    open_flags = create ? (O_CREAT | O_EXCL | O_RDWR) : O_RDWR;
    fd = shm_open(tab->object_name, open_flags, 0600);
    if (fd < 0) {
        rc = set_error(err, create ? (errno == EEXIST ? KOS_ERR_EXISTS : KOS_ERR_IO)
                                   : (errno == ENOENT ? KOS_ERR_MISSING : KOS_ERR_IO),
                       "shm_open('%s')", tab->object_name);
        free(tab);
        return rc;
    }
    tab->fd = fd;
    if (create) {
        if (ftruncate(fd, (off_t)sizeof(kos_nav_segment)) != 0) {
            rc = set_error(err, KOS_ERR_IO, "ftruncate('%s')", tab->object_name);
            goto fail_object;
        }
        tab->sem = sem_open(tab->sem_name, O_CREAT | O_EXCL, 0600, 1);
        if (tab->sem == SEM_FAILED) {
            tab->sem = NULL;
            rc = set_error(err, errno == EEXIST ? KOS_ERR_EXISTS : KOS_ERR_IO,
                           "sem_open('%s')", tab->sem_name);
            goto fail_object;
        }
    } else {
        struct stat st;

        memset(&st, 0, sizeof st);
        if (fstat(fd, &st) != 0 ||
            (size_t)st.st_size != sizeof(kos_nav_segment)) {
            rc = set_error(err, KOS_ERR_LAYOUT, "导航表大小 %lld != %zu",
                           (long long)st.st_size, sizeof(kos_nav_segment));
            goto fail_object;
        }
        tab->sem = sem_open(tab->sem_name, 0);
        if (tab->sem == SEM_FAILED) {
            tab->sem = NULL;
            rc = set_error(err, errno == ENOENT ? KOS_ERR_MISSING : KOS_ERR_IO,
                           "sem_open('%s')", tab->sem_name);
            goto fail_object;
        }
    }
    tab->map_size = sizeof(kos_nav_segment);
    tab->seg = mmap(NULL, tab->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (tab->seg == MAP_FAILED) {
        tab->seg = NULL;
        rc = set_error(err, KOS_ERR_IO, "mmap('%s')", tab->object_name);
        goto fail_object;
    }
    tab->created = create;
    if (create) {
        memset(tab->seg, 0, tab->map_size);
        tab->seg->magic = KOS_NAV_MAGIC;
        tab->seg->version = KOS_NAV_VERSION;
        tab->seg->header_size = sizeof(kos_nav_segment);
        tab->seg->target_slot_size = sizeof(kos_nav_target_slot);
        tab->seg->plan_slot_size = sizeof(kos_nav_plan_slot);
        tab->seg->max_chefs = KOS_SHM_MAX_CHEFS;
        tab->seg->creator_pid = (int64_t)getpid();
        tab->seg->created_at_ms = realtime_ms();
    } else {
        rc = validate_segment(tab, err);
        if (rc != KOS_OK) {
            goto fail_object;
        }
    }
    *out = tab;
    return KOS_OK;

fail_object:
    if (tab->seg != NULL) {
        (void)munmap(tab->seg, tab->map_size);
    }
    if (tab->sem != NULL) {
        (void)sem_close(tab->sem);
        if (create) {
            (void)sem_unlink(tab->sem_name);
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    if (create) {
        (void)shm_unlink(tab->object_name);
    }
    free(tab);
    return rc;
}

int kos_navtab_create(const kos_navtab_opts *opts, kos_navtab **out, kos_error *err)
{
    return open_locked(opts, out, 1, err);
}

int kos_navtab_open(const kos_navtab_opts *opts, kos_navtab **out, kos_error *err)
{
    return open_locked(opts, out, 0, err);
}

int kos_navtab_close(kos_navtab *tab)
{
    if (tab == NULL) {
        return KOS_ERR_INVAL;
    }
    if (tab->seg != NULL) {
        (void)munmap(tab->seg, tab->map_size);
    }
    if (tab->sem != NULL) {
        (void)sem_close(tab->sem);
    }
    if (tab->fd >= 0) {
        close(tab->fd);
    }
    free(tab);
    return KOS_OK;
}

int kos_navtab_unlink(const kos_navtab_opts *opts, kos_error *err)
{
    const char *object_name = object_name_of(opts);
    const char *sem_name = sem_name_of(opts);
    int failed = 0;

    if (shm_unlink(object_name) != 0 && errno != ENOENT) {
        failed = errno;
    }
    if (sem_unlink(sem_name) != 0 && errno != ENOENT) {
        failed = failed != 0 ? failed : errno;
    }
    if (failed != 0) {
        return set_error(err, KOS_ERR_IO, "unlink('%s' / '%s')", object_name, sem_name);
    }
    return KOS_OK;
}

int kos_navtab_read_target(kos_navtab *tab, int chef, kos_nav_target *out)
{
    kos_nav_target_slot *slot;

    if (tab == NULL || out == NULL || chef < 0 || chef >= KOS_SHM_MAX_CHEFS) {
        return KOS_ERR_INVAL;
    }
    memset(out, 0, sizeof *out);
    slot = &tab->seg->targets[chef];
    return slot_read(&slot->seq, out, &slot->target, sizeof *out);
}

int kos_navtab_write_target(kos_navtab *tab, int chef, const kos_nav_target *target)
{
    kos_nav_target_slot *slot;

    if (tab == NULL || target == NULL || chef < 0 || chef >= KOS_SHM_MAX_CHEFS) {
        return KOS_ERR_INVAL;
    }
    slot = &tab->seg->targets[chef];
    nav_write_lock(tab);
    slot_write(&slot->seq, &slot->target, target, sizeof *target);
    nav_write_unlock(tab);
    return KOS_OK;
}

int kos_navtab_read_plan(kos_navtab *tab, int chef, kos_nav_plan *out)
{
    kos_nav_plan_slot *slot;

    if (tab == NULL || out == NULL || chef < 0 || chef >= KOS_SHM_MAX_CHEFS) {
        return KOS_ERR_INVAL;
    }
    memset(out, 0, sizeof *out);
    slot = &tab->seg->plans[chef];
    return slot_read(&slot->seq, out, &slot->plan, sizeof *out);
}

int kos_navtab_write_plan(kos_navtab *tab, int chef, const kos_nav_plan *plan)
{
    kos_nav_plan_slot *slot;

    if (tab == NULL || plan == NULL || chef < 0 || chef >= KOS_SHM_MAX_CHEFS) {
        return KOS_ERR_INVAL;
    }
    slot = &tab->seg->plans[chef];
    nav_write_lock(tab);
    slot_write(&slot->seq, &slot->plan, plan, sizeof *plan);
    nav_write_unlock(tab);
    return KOS_OK;
}

const char *kos_nav_status_name(int status)
{
    switch (status) {
    case KOS_NAV_IDLE:
        return "idle";
    case KOS_NAV_MOVING:
        return "moving";
    case KOS_NAV_ARRIVED:
        return "arrived";
    case KOS_NAV_STUCK:
        return "stuck";
    case KOS_NAV_BLOCKED:
        return "blocked";
    default:
        return "unknown";
    }
}
