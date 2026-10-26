/*
 * shm.c —— /kitchenos_state 的布局、seqlock、输入槽与就绪信号量。
 *
 * 这个文件里只有两件值得记住的事：
 *   1. 世界状态与每格输入槽各有一个 seqlock，写者与读者只通过它交换一致性；
 *   2. 段头带 magic + version + 结构尺寸，任何一个对不上都拒绝挂接，
 *      因为"按错位的结构读一段别人的内存"是最难查的一类故障。
 *
 * 谁写谁读（冻结）：Bridge 独占写世界状态；厨师 k 的 Worker 独占写 slots[k]；
 * 其余进程一律只读。库不校验调用者身份 —— 那是进程架构的事，写在这里只会变成
 * 一句永远为真的空话，但每个写入口都做成"整份发布"，调用者没有机会写出半份状态。
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/shm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ============================================================ 小工具 */

/* 错误上下文里的名字最长只看这么多字符：context 是给人看的一行日志，
 * 不是审计记录，没必要被一个 255 字符的名字撑爆。 */
#define KOS_NAME_SHOWN 48

/*
 * 等写者放手与重试预算。
 *
 * 为什么等待和重试要分开算：写者持奇序号的时间是"写完一份载荷"（微秒级），而读者
 * "看一眼发现是奇数"只要几纳秒。如果每次看都算一次尝试，读者会在写者还没写完的时候
 * 就把 64 次预算烧光 —— 本模块的并发测试就是这么抓到 260 万次假 KOS_ERR_BUSY 的。
 * 所以：真正拷过载荷才算一次尝试；等写者放手另有上限。
 *
 * 等待为什么要自旋 + 让出 CPU：写者通常几微秒就写完，自旋最快；但写者也可能被调度器
 * 切走，此时死烧 CPU 只会让它更晚回来（测试在 20 万次读里抓到过 8 次这种情况），所以
 * 每轮自旋之后 sched_yield() 一次。
 *
 * 为什么上限给到 250 ms：这个上限不是性能参数，而是"写者是不是死了"的判定时间。写者被
 * 切走几十毫秒在负载高的机器上很常见，上限给小了就会把调度抖动报成 KOS_ERR_BUSY
 * （本模块的测试因此一度时好时坏）。真死掉的写者必须能在有限时间内被发现，否则读者
 * 永远卡住。帧屏障那种自带期限的调用会把更紧的期限传进来，不会因此超时。
 */
#define KOS_SHM_WRITER_SPINS 4096u /* 让出 CPU 之前先自旋多久 */
#define KOS_SHM_WRITER_WAIT_MS 250 /* 等写者放手的总时长上限（毫秒） */

static void set_error(kos_error *err, int code, int sys_errno, const char *fmt, ...)
{
    va_list ap;

    if (!err)
        return;
    err->code = code;
    err->sys_errno = sys_errno;
    va_start(ap, fmt);
    vsnprintf(err->context, sizeof err->context, fmt, ap);
    va_end(ap);
}

void kos_error_clear(kos_error *err)
{
    if (!err)
        return;
    err->code = KOS_OK;
    err->sys_errno = 0;
    err->context[0] = '\0';
}

const char *kos_strerror(int code)
{
    switch (code) {
    case KOS_OK: return "KOS_OK";
    case KOS_ERR_INVAL: return "KOS_ERR_INVAL";
    case KOS_ERR_NOMEM: return "KOS_ERR_NOMEM";
    case KOS_ERR_IO: return "KOS_ERR_IO";
    case KOS_ERR_EXISTS: return "KOS_ERR_EXISTS";
    case KOS_ERR_MISSING: return "KOS_ERR_MISSING";
    case KOS_ERR_MAGIC: return "KOS_ERR_MAGIC";
    case KOS_ERR_VERSION: return "KOS_ERR_VERSION";
    case KOS_ERR_LAYOUT: return "KOS_ERR_LAYOUT";
    case KOS_ERR_DIR: return "KOS_ERR_DIR";
    case KOS_ERR_RANGE: return "KOS_ERR_RANGE";
    case KOS_ERR_EMPTY: return "KOS_ERR_EMPTY";
    case KOS_ERR_STALE: return "KOS_ERR_STALE";
    case KOS_ERR_BUSY: return "KOS_ERR_BUSY";
    case KOS_ERR_TIMEOUT: return "KOS_ERR_TIMEOUT";
    default: return "KOS_ERR_UNKNOWN";
    }
}

int kos_error_format(const kos_error *err, char *buf, size_t len)
{
    int n;

    if (!buf || len == 0)
        return 0;
    if (!err) {
        buf[0] = '\0';
        return 0;
    }
    if (err->code == KOS_OK)
        n = snprintf(buf, len, "%s", kos_strerror(KOS_OK));
    else if (err->sys_errno != 0)
        n = snprintf(buf, len, "%s: %s (errno=%d) [%.*s]", kos_strerror(err->code),
                     strerror(err->sys_errno), err->sys_errno, (int)sizeof err->context,
                     err->context);
    else
        n = snprintf(buf, len, "%s: %.*s", kos_strerror(err->code), (int)sizeof err->context,
                     err->context);
    return n < 0 ? 0 : n;
}

/* 定长字段赋值：保证留下 NUL。名字类字段是短标识符，超长说明对端给了意料之外的串，
 * 截断比越界安全，也比把整段内存写坏安全。 */
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int64_t mono_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

static int64_t real_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

int kos_instance_suffix(int instance, char *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return KOS_ERR_INVAL;
    if (instance < 0) {
        buf[0] = '\0';
        return KOS_OK;
    }
    if (snprintf(buf, len, "_%d", instance) >= (int)len)
        return KOS_ERR_RANGE;
    return KOS_OK;
}

int kos_name_instance(const char *base, int instance, char *buf, size_t len)
{
    if (base == NULL || buf == NULL || len == 0)
        return KOS_ERR_INVAL;
    if (instance < 0) {
        copy_str(buf, len, base);
        return KOS_OK;
    }
    if (snprintf(buf, len, "%s_%d", base, instance) >= (int)len)
        return KOS_ERR_RANGE;
    return KOS_OK;
}

/* sem_timedwait 只认 CLOCK_REALTIME 的绝对时刻，所以每次等待都要从"现在"换算。 */
static void realtime_after(int timeout_ms, struct timespec *abs)
{
    clock_gettime(CLOCK_REALTIME, abs);
    abs->tv_sec += timeout_ms / 1000;
    abs->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (abs->tv_nsec >= 1000000000L) {
        abs->tv_nsec -= 1000000000L;
        abs->tv_sec += 1;
    }
}

/* 按 64 位字一步算校验和，最后折成 32 位。
 *
 * 为什么不是逐字节的 FNV-1a：这段载荷 7 KB，而校验是在写者持奇序号的窗口里算的。
 * 逐字节乘（每字节约 5 个周期）会让写者持奇十几微秒，读者全堵在这段时间上 ——
 * 这个版本的并发测试就是这么做出来的（4 个读者 85% 的读被写者挡住）。
 * 按字算把窗口压回亚微秒，同时保留"载荷被改过就变值"这个唯一要的性质。 */
static uint32_t checksum_bytes(const void *data, size_t len)
{
    const unsigned char *p = data;
    uint64_t h = 14695981039346656037ULL; /* FNV-1a 64 位偏移基数 */
    size_t i = 0;

    for (; i + 8 <= len; i += 8) {
        uint64_t word;

        memcpy(&word, p + i, 8); /* memcpy 而不是指针转换：不踩严格别名规则 */
        h = (h ^ word) * 1099511628211ULL;
    }
    for (; i < len; i++)
        h = (h ^ (uint64_t)p[i]) * 1099511628211ULL;
    return (uint32_t)(h ^ (h >> 32));
}

uint32_t kos_world_checksum(const kos_world_state *world)
{
    if (!world)
        return 0;
    return checksum_bytes(world, offsetof(kos_world_state, checksum));
}

int kos_world_state_check(const kos_world_state *world)
{
    if (!world)
        return 0;
    return kos_world_checksum(world) == world->checksum ? 1 : 0;
}

static uint32_t action_checksum(const kos_input_action *action)
{
    return checksum_bytes(action, offsetof(kos_input_action, checksum));
}

void kos_world_state_reset(kos_world_state *world)
{
    if (!world)
        return;
    memset(world, 0, sizeof *world);
}

/* ============================================================ seqlock */

/*
 * 认领一次写窗口，返回本轮写完要发布的偶数序号。
 *
 * 为什么不是一句简单的 fetch_add：写者中途死掉会把序号永远卡在奇数，读者就一直等。
 * 这里把"看到奇数"当成"上一轮写者没了"来处理，把它补成偶数再重新抢。补偶数那一瞬间
 * 暴露出去的可能是撕裂载荷，所以载荷自带 checksum 与 write_seq：读者对不上就继续重试，
 * 不会认下这份数据。两处写路径（世界状态、输入槽）共用这一份逻辑。
 */
static uint64_t seqlock_begin(uint64_t *seq)
{
    uint64_t s;

    for (;;) {
        s = __atomic_load_n(seq, __ATOMIC_RELAXED);
        if ((s & 1u) != 0u) {
            __atomic_store_n(seq, s + 1u, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            continue;
        }
        if (__atomic_compare_exchange_n(seq, &s, s + 1u, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            break;
        /* CAS 失败时 s 已被写成当前值，回到循环顶部重新判断奇偶 */
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return s + 2u;
}

static void seqlock_end(uint64_t *seq, uint64_t publish)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(seq, publish, __ATOMIC_RELAXED);
}

/* 等写者把序号放回偶数。先自旋（正常路径：写者几微秒就写完），自旋不成就让出 CPU，
 * 总时长到 deadline 仍不放回就当写者死了，报 KOS_ERR_BUSY。 */
static int wait_writer_release(uint64_t *seq, int64_t deadline_ms)
{
    for (;;) {
        uint32_t i;

        for (i = 0; i < KOS_SHM_WRITER_SPINS; i++) {
            if ((__atomic_load_n(seq, __ATOMIC_ACQUIRE) & 1u) == 0u)
                return KOS_OK;
        }
        if (mono_ms() >= deadline_ms)
            return KOS_ERR_BUSY;
        sched_yield();
    }
}

static int64_t reader_deadline(void)
{
    return mono_ms() + (int64_t)KOS_SHM_WRITER_WAIT_MS;
}

/* ======================================================== 世界状态读写 */

kos_world_state *kos_shm_world_write_begin(kos_shm *shm)
{
    if (!shm || !shm->seg)
        return NULL;
    shm->write_base = seqlock_begin(&shm->seg->world_seq);
    return &shm->seg->world;
}

void kos_shm_world_write_end(kos_shm *shm)
{
    kos_world_state *w;

    if (!shm || !shm->seg)
        return;
    w = &shm->seg->world;
    /* write_seq 与 checksum 必须在最后一次屏障之前落盘：读者靠这两个字段
     * 判断"这份载荷是不是这一版写的"。 */
    w->write_seq = shm->write_base;
    w->checksum = kos_world_checksum(w);
    seqlock_end(&shm->seg->world_seq, shm->write_base);
}

int kos_shm_world_write(kos_shm *shm, const kos_world_state *world)
{
    kos_world_state *w;

    if (!shm || !shm->seg || !world)
        return KOS_ERR_INVAL;
    w = kos_shm_world_write_begin(shm);
    if (!w)
        return KOS_ERR_INVAL;
    memcpy(w, world, sizeof *w);
    kos_shm_world_write_end(shm);
    return KOS_OK;
}

int kos_shm_world_read(kos_shm *shm, kos_world_state *out, uint32_t max_attempts,
                       uint32_t *attempts_out)
{
    int64_t deadline;
    uint32_t attempts = 0;
    uint32_t limit;

    if (!shm || !shm->seg || !out)
        return KOS_ERR_INVAL;
    limit = max_attempts ? max_attempts : (uint32_t)KOS_SHM_READ_ATTEMPTS;
    deadline = reader_deadline();

    while (attempts < limit) {
        uint64_t s1 = __atomic_load_n(&shm->seg->world_seq, __ATOMIC_ACQUIRE);
        uint64_t s2;

        if (s1 == 0u) {
            /* 序号还是初值：这个段建好之后一次都没发布过，没有快照可给。 */
            if (attempts_out)
                *attempts_out = attempts;
            return KOS_ERR_EMPTY;
        }
        if ((s1 & 1u) != 0u) {
            /* 写者正在写：先等它写完。这一次不算尝试，理由见 KOS_SHM_WRITER_SPINS 处。 */
            if (mono_ms() >= deadline ||
                wait_writer_release(&shm->seg->world_seq, deadline) != KOS_OK)
                break;
            continue;
        }
        attempts++;
        memcpy(out, &shm->seg->world, sizeof *out);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        s2 = __atomic_load_n(&shm->seg->world_seq, __ATOMIC_RELAXED);
        if (s1 != s2)
            continue; /* 拷贝期间序号动过，这份快照作废 */
        if (out->write_seq != s2)
            continue; /* 载荷不是这一版写的（写者死在半路后刚被补成偶数） */
        if (!kos_world_state_check(out))
            continue; /* 载荷自身不自洽：撕裂 */
        if (attempts_out)
            *attempts_out = attempts;
        return KOS_OK;
    }
    if (attempts_out)
        *attempts_out = attempts;
    return KOS_ERR_BUSY;
}

/* ============================================================ 输入槽 */

static kos_input_slot *slot_of(kos_shm *shm, uint32_t slot)
{
    if (!shm || !shm->seg)
        return NULL;
    if (slot >= shm->seg->slot_count)
        return NULL;
    return &shm->seg->slots[slot];
}

/* 读一格的自洽快照。序号 + 载荷校验都对上才算数，否则重试；
 * 用尽次数说明这格一直有人在写或写者卡住了，返回 KOS_ERR_BUSY。
 * 序号还是初值说明这格从来没被写过 → KOS_ERR_EMPTY，这是"确定没有"，
 * 和"读不到自洽快照"是两件事，必须分开报。
 *
 * deadline 由调用者给：单次读用默认期限；帧屏障用它自己的超时期限，
 * 否则一格卡住的槽会把屏障的 300 ms 超时拖成好几秒。 */
static int slot_snapshot_until(kos_input_slot *slot, kos_input_action *out, uint64_t *seq_out,
                               int64_t deadline)
{
    uint32_t attempts = 0;

    while (attempts < KOS_SHM_READ_ATTEMPTS) {
        uint64_t s1 = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        uint64_t s2;

        if (s1 == 0u)
            return KOS_ERR_EMPTY;
        if ((s1 & 1u) != 0u) {
            if (mono_ms() >= deadline || wait_writer_release(&slot->seq, deadline) != KOS_OK)
                return KOS_ERR_BUSY;
            continue;
        }
        attempts++;
        memcpy(out, &slot->action, sizeof *out);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        s2 = __atomic_load_n(&slot->seq, __ATOMIC_RELAXED);
        if (s1 != s2)
            continue;
        if (action_checksum(out) != out->checksum)
            continue;
        if (seq_out)
            *seq_out = s1;
        return KOS_OK;
    }
    return KOS_ERR_BUSY;
}

static int slot_snapshot(kos_input_slot *slot, kos_input_action *out, uint64_t *seq_out)
{
    return slot_snapshot_until(slot, out, seq_out, reader_deadline());
}

int kos_shm_slot_publish(kos_shm *shm, uint32_t slot, const kos_input_action *action)
{
    kos_input_slot *s = slot_of(shm, slot);
    uint64_t publish;

    if (!s || !action)
        return (action == NULL) ? KOS_ERR_INVAL : KOS_ERR_RANGE;
    if (action->kind < (int32_t)KOS_ACTION_NONE || action->kind > (int32_t)KOS_ACTION_WORK)
        return KOS_ERR_INVAL;
    /* The payload crosses a process boundary. A target name without a NUL can
     * make the Bridge's strlen read past kos_input_action, so reject it here
     * (and check it once more on the read side). */
    if (memchr(action->target_object, '\0', sizeof action->target_object) == NULL)
        return KOS_ERR_INVAL;
    if (action->waypoint_count > KOS_MAX_WAYPOINTS)
        return KOS_ERR_INVAL;

    publish = seqlock_begin(&s->seq);
    memcpy(&s->action, action, sizeof s->action);
    s->action.checksum = action_checksum(&s->action);
    seqlock_end(&s->seq, publish);

    /* 门铃在载荷之后响：读者被唤醒时槽里一定已经有东西。响铃失败不影响
     * 槽内容的有效性，但调用者必须知道，否则 Bridge 会白等到超时。 */
    return kos_shm_post_ready(shm);
}

int kos_shm_slot_read(kos_shm *shm, uint32_t slot, kos_input_action *out)
{
    kos_input_slot *s = slot_of(shm, slot);
    int rc;

    if (!s || !out)
        return KOS_ERR_INVAL;
    rc = slot_snapshot(s, out, NULL);
    if (rc != KOS_OK)
        return rc;
    return out->frame_id == 0u ? KOS_ERR_EMPTY : KOS_OK;
}

int kos_shm_slot_read_for_frame(kos_shm *shm, uint32_t slot, uint64_t frame_id,
                                kos_input_action *out)
{
    kos_input_slot *s = slot_of(shm, slot);
    int rc;

    if (!s || !out)
        return KOS_ERR_INVAL;
    if (frame_id == 0u)
        return KOS_ERR_INVAL;
    rc = slot_snapshot(s, out, NULL);
    if (rc != KOS_OK)
        return rc;
    if (out->frame_id == 0u)
        return KOS_ERR_EMPTY;
    if (out->frame_id != frame_id)
        return KOS_ERR_STALE;
    return KOS_OK;
}

int kos_shm_slot_frame_ready(kos_shm *shm, uint32_t slot, uint64_t frame_id)
{
    kos_input_slot *s = slot_of(shm, slot);
    kos_input_action a;

    if (!s)
        return KOS_ERR_RANGE;
    if (frame_id == 0u)
        return KOS_ERR_INVAL;
    if (slot_snapshot(s, &a, NULL) != KOS_OK)
        return 0; /* 一直在被写、读不到自洽快照 = 此刻不算就绪 */
    return a.frame_id == frame_id ? 1 : 0;
}

/* ========================================================== 就绪信号量 */

int kos_shm_post_ready(kos_shm *shm)
{
    if (!shm || !shm->sem)
        return KOS_ERR_INVAL;
    if (sem_post(shm->sem) != 0)
        return KOS_ERR_IO;
    return KOS_OK;
}

int kos_shm_wait_ready(kos_shm *shm, int timeout_ms)
{
    if (!shm || !shm->sem)
        return KOS_ERR_INVAL;
    if (timeout_ms < 0) {
        while (sem_wait(shm->sem) != 0) {
            if (errno != EINTR)
                return KOS_ERR_IO;
        }
        return KOS_OK;
    }
    if (timeout_ms == 0) {
        if (sem_trywait(shm->sem) == 0)
            return KOS_OK;
        return errno == EAGAIN ? KOS_ERR_TIMEOUT : KOS_ERR_IO;
    }
    for (;;) {
        struct timespec abs;

        realtime_after(timeout_ms, &abs);
        if (sem_timedwait(shm->sem, &abs) == 0)
            return KOS_OK;
        if (errno == ETIMEDOUT)
            return KOS_ERR_TIMEOUT;
        if (errno != EINTR)
            return KOS_ERR_IO;
        /* EINTR 后重算期限会稍微拉长总等待时间，但信号打断不是正常路径，
         * 这点偏差比"少等一点就误判超时"划算。 */
    }
}

int kos_shm_drain_ready(kos_shm *shm)
{
    int drained = 0;

    if (!shm || !shm->sem)
        return KOS_ERR_INVAL;
    while (sem_trywait(shm->sem) == 0)
        drained++;
    return drained;
}

/* ============================================================ 帧屏障 */

/*
 * 等门铃，deadline_ms < 0 表示无限等。
 * 门铃只用来省 CPU：醒来之后一定要回槽里核对，信号量分不清是哪一格、哪一帧。
 */
static int wait_bell(kos_shm *shm, int64_t deadline_ms)
{
    for (;;) {
        struct timespec abs;
        int64_t left;

        if (deadline_ms < 0) {
            if (sem_wait(shm->sem) == 0)
                return KOS_OK;
            if (errno == EINTR)
                continue;
            return KOS_ERR_IO;
        }
        left = deadline_ms - mono_ms();
        if (left <= 0)
            return KOS_ERR_TIMEOUT;
        realtime_after(left > (int64_t)INT_MAX ? INT_MAX : (int)left, &abs);
        if (sem_timedwait(shm->sem, &abs) == 0)
            return KOS_OK;
        if (errno == ETIMEDOUT)
            return KOS_ERR_TIMEOUT;
        if (errno != EINTR)
            return KOS_ERR_IO;
    }
}

static int64_t deadline_of(int timeout_ms)
{
    return timeout_ms < 0 ? -1 : mono_ms() + (int64_t)timeout_ms;
}

/* 哪些格还不是 frame_id 这一版。返回位掩码，第 k 位为 1 = 第 k 格没到。 */
static uint64_t missing_for_frame(kos_shm *shm, uint64_t frame_id, uint32_t chef_count,
                                  int64_t deadline)
{
    uint64_t missing = 0;
    uint32_t k;

    for (k = 0; k < chef_count; k++) {
        kos_input_action a;

        if (slot_snapshot_until(&shm->seg->slots[k], &a, NULL, deadline) != KOS_OK ||
            a.frame_id != frame_id)
            missing |= (uint64_t)1u << k;
    }
    return missing;
}

static uint64_t full_mask(uint32_t chef_count)
{
    return chef_count >= 64u ? ~(uint64_t)0 : (((uint64_t)1u << chef_count) - 1u);
}

/* 推进水位：把比 last_frame[k] 更新的那格消费掉，返回还没到的格。 */
static uint64_t advance_watermarks(kos_shm *shm, uint64_t *last_frame, uint32_t chef_count,
                                   int64_t deadline)
{
    uint64_t missing = 0;
    uint32_t k;

    for (k = 0; k < chef_count; k++) {
        kos_input_action a;

        if (slot_snapshot_until(&shm->seg->slots[k], &a, NULL, deadline) == KOS_OK &&
            a.frame_id > last_frame[k])
            last_frame[k] = a.frame_id;
        else
            missing |= (uint64_t)1u << k;
    }
    return missing;
}

static int check_frame_args(kos_shm *shm, uint32_t chef_count)
{
    if (!shm || !shm->seg || !shm->sem)
        return KOS_ERR_INVAL;
    if (chef_count == 0 || chef_count > shm->seg->slot_count)
        return KOS_ERR_INVAL;
    return KOS_OK;
}

int kos_shm_wait_frame(kos_shm *shm, uint64_t frame_id, uint32_t chef_count, int timeout_ms,
                       uint64_t *missing_mask)
{
    uint64_t missing;
    int64_t deadline;
    int rc;

    if (check_frame_args(shm, chef_count) != KOS_OK)
        return KOS_ERR_INVAL;
    if (frame_id == 0u)
        return KOS_ERR_INVAL;

    deadline = deadline_of(timeout_ms);
    missing = missing_for_frame(shm, frame_id, chef_count, deadline);
    while (missing != 0u) {
        rc = wait_bell(shm, deadline);
        missing = missing_for_frame(shm, frame_id, chef_count, deadline);
        if (missing == 0u)
            break;
        if (rc != KOS_OK) {
            if (missing_mask)
                *missing_mask = missing;
            return rc;
        }
    }
    if (missing_mask)
        *missing_mask = missing;
    return KOS_OK;
}

int kos_shm_wait_frame_after(kos_shm *shm, uint64_t *last_frame, uint32_t chef_count,
                             int timeout_ms, uint64_t *missing_mask)
{
    uint64_t pending;
    int64_t deadline;
    int rc;

    if (check_frame_args(shm, chef_count) != KOS_OK || !last_frame)
        return KOS_ERR_INVAL;

    deadline = deadline_of(timeout_ms);
    /* missing_mask 是**累积**语义：只有"这一轮里始终没拿到更新版本"的格子才算缺失。
     * 不能每次重算就覆盖 —— 水位一旦被推进，同一格立刻又"不比水位更新"了，
     * 用瞬时值会把已经按时交过输入的厨师报成缺席，Bridge 就会给它发 idle。
     * 这个 bug 是测试里那个 "missing_mask=0x2 却打印出 0x3" 抓出来的。 */
    pending = full_mask(chef_count);
    for (;;) {
        pending &= advance_watermarks(shm, last_frame, chef_count, deadline);
        if (pending == 0u) {
            if (missing_mask)
                *missing_mask = 0;
            return KOS_OK;
        }
        rc = wait_bell(shm, deadline);
        if (rc != KOS_OK) {
            /* 临走前再推进一次：刚好卡在期限上的输入也算到了。 */
            pending &= advance_watermarks(shm, last_frame, chef_count, deadline);
            if (missing_mask)
                *missing_mask = pending;
            return pending == 0u ? KOS_OK : rc;
        }
    }
}

/* ====================================================== 名字与目录 */

/* 对象名与信号量名都必须是 '/' 开头的单段名字。glibc 对含内嵌 '/' 的 shm 名字
 * 直接返回 EINVAL，对信号量同样如此（本机 Ubuntu 26.04 实测），所以这里提前拦下来
 * 给一句能看懂的话，而不是让调用者对着 EINVAL 猜。 */
static int check_object_name(const char *name, const char *what, kos_error *err)
{
    size_t len;

    if (!name || name[0] == '\0') {
        set_error(err, KOS_ERR_INVAL, 0, "%s为空", what);
        return KOS_ERR_INVAL;
    }
    if (name[0] != '/') {
        set_error(err, KOS_ERR_INVAL, 0, "%s '%.*s' 必须以 '/' 开头（如 /kitchenos_state）", what,
                  KOS_NAME_SHOWN, name);
        return KOS_ERR_INVAL;
    }
    len = strlen(name);
    if (len < 2) {
        set_error(err, KOS_ERR_INVAL, 0, "%s '%s' 只有一个 '/'，不是合法名字", what, name);
        return KOS_ERR_INVAL;
    }
    if (len >= KOS_SHM_PATH_MAX) {
        set_error(err, KOS_ERR_INVAL, 0, "%s 太长（%lu 字节，上限 %d）", what,
                  (unsigned long)len, KOS_SHM_PATH_MAX - 1);
        return KOS_ERR_INVAL;
    }
    if (strchr(name + 1, '/') != NULL) {
        set_error(err, KOS_ERR_INVAL, 0,
                  "%s '%.*s' 含内嵌 '/'：POSIX 命名对象只能是单段名字，换个名字（例如带上 pid）"
                  "而不是加目录",
                  what, KOS_NAME_SHOWN, name);
        return KOS_ERR_INVAL;
    }
    return KOS_OK;
}

/*
 * opts 字段 → 环境变量 → 常量。允许空串当"没设置"，因为 `KITCHENOS_SHM_NAME= cmd`
 * 这种写法在脚本里很常见，把它当成合法名字会得到一句莫名其妙的错误。
 */
static const char *pick(const char *from_opts, const char *env_name, const char *fallback)
{
    const char *v;

    if (from_opts && from_opts[0] != '\0')
        return from_opts;
    v = getenv(env_name);
    if (v && v[0] != '\0')
        return v;
    return fallback;
}

static int resolve_names(const kos_shm_opts *opts, char *object, char *sem, char *dir,
                         kos_error *err)
{
    const char *o = pick(opts ? opts->object_name : NULL, KOS_SHM_ENV_NAME, KOS_SHM_DEFAULT_NAME);
    const char *s = pick(opts ? opts->sem_name : NULL, KOS_SHM_ENV_SEM_NAME,
                         KOS_SHM_DEFAULT_SEM_NAME);
    const char *d = pick(opts ? opts->dir : NULL, KOS_SHM_ENV_DIR, KOS_SHM_DEFAULT_DIR);
    int rc;

    rc = check_object_name(o, "共享内存对象名", err);
    if (rc != KOS_OK)
        return rc;
    rc = check_object_name(s, "信号量名", err);
    if (rc != KOS_OK)
        return rc;
    if (d[0] != '/') {
        set_error(err, KOS_ERR_DIR, 0, "shm 目录 '%.*s' 必须是绝对路径", KOS_NAME_SHOWN, d);
        return KOS_ERR_DIR;
    }
    if (strlen(d) >= KOS_SHM_PATH_MAX) {
        set_error(err, KOS_ERR_DIR, 0, "shm 目录太长（上限 %d 字节）", KOS_SHM_PATH_MAX - 1);
        return KOS_ERR_DIR;
    }
    copy_str(object, KOS_SHM_PATH_MAX, o);
    copy_str(sem, KOS_SHM_PATH_MAX, s);
    copy_str(dir, KOS_SHM_PATH_MAX, d);
    return KOS_OK;
}

/* dir + name，去掉 dir 结尾多余的 '/'。手写拼接而不是 snprintf：
 * 长度上限自己算得清，也就不必依赖格式化函数的截断行为。 */
static int join_path(char *dst, size_t cap, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    size_t nl = strlen(name);

    while (dl > 1 && dir[dl - 1] == '/')
        dl--;
    if (dl + nl + 1 > cap)
        return -1;
    memcpy(dst, dir, dl);
    memcpy(dst + dl, name, nl);
    dst[dl + nl] = '\0';
    return 0;
}

/*
 * 确认对象真的落在配置的目录里。
 *
 * POSIX 命名共享内存只有**一个**落点（Linux 是 /dev/shm），名字选不了目录；所以
 * KITCHENOS_SHM_DIR 的用处不是"把段放到别处"，而是断言"它应该在这个 tmpfs 里"。
 * 目录配错时 shm_open 不会报错，只会把段放到 /dev/shm —— 于是写的人以为在别处、
 * 清的人去别处删，这类问题查起来最费劲，所以在打开路径上就核对 stat 的 inode。
 */
static int verify_object_dir(int fd, const char *dir, const char *object_name, kos_error *err)
{
    char path[KOS_SHM_PATH_MAX * 2];
    struct stat st_fd;
    struct stat st_path;

    if (join_path(path, sizeof path, dir, object_name) != 0) {
        set_error(err, KOS_ERR_DIR, 0, "目录与对象名拼起来超过 %lu 字节", (unsigned long)sizeof path);
        return KOS_ERR_DIR;
    }
    if (fstat(fd, &st_fd) != 0) {
        int e = errno;

        set_error(err, KOS_ERR_IO, e, "fstat(%s)", object_name);
        return KOS_ERR_IO;
    }
    if (stat(path, &st_path) != 0) {
        int e = errno;

        set_error(err, KOS_ERR_DIR, e,
                  "对象 '%.*s' 没出现在配置目录 '%.*s' 里：POSIX shm_open 只把命名对象放在"
                  "/dev/shm，请把 KITCHENOS_SHM_DIR 指向真正承载它的 tmpfs",
                  KOS_NAME_SHOWN, object_name, KOS_NAME_SHOWN, dir);
        return KOS_ERR_DIR;
    }
    if (st_fd.st_dev != st_path.st_dev || st_fd.st_ino != st_path.st_ino) {
        set_error(err, KOS_ERR_DIR, 0, "'%.*s' 里有个同名对象但不是我们打开的那个",
                  KOS_NAME_SHOWN, path);
        return KOS_ERR_DIR;
    }
    return KOS_OK;
}

/* ======================================================== 段头校验 */

static void init_header(kos_shm *shm)
{
    kos_shm_segment *g = shm->seg;

    memset(g, 0, sizeof *g);
    g->magic = KOS_SHM_MAGIC;
    g->version = KOS_SHM_VERSION;
    g->header_size = (uint32_t)sizeof *g;
    g->world_size = (uint32_t)sizeof(kos_world_state);
    g->slot_size = (uint32_t)sizeof(kos_input_slot);
    g->slot_count = (uint32_t)KOS_SHM_MAX_CHEFS;
    g->max_chefs = (uint32_t)KOS_SHM_MAX_CHEFS;
    g->max_orders = (uint32_t)KOS_SHM_MAX_ORDERS;
    g->max_resources = (uint32_t)KOS_SHM_MAX_RESOURCES;
    g->max_order_ingredients = (uint32_t)KOS_SHM_MAX_ORDER_INGREDIENTS;
    g->creator_pid = (int64_t)getpid();
    g->created_at_ms = real_ms();
}

static int validate_header(kos_shm *shm, kos_error *err)
{
    const kos_shm_segment *g = shm->seg;

    if (g->magic != KOS_SHM_MAGIC) {
        set_error(err, KOS_ERR_MAGIC, 0,
                  "'%.*s' 段头魔数 0x%08lx 不是 KitchenOS 段（应为 0x%08lx）：名字撞了别的对象",
                  KOS_NAME_SHOWN, shm->object_name, (unsigned long)g->magic,
                  (unsigned long)KOS_SHM_MAGIC);
        return KOS_ERR_MAGIC;
    }
    if (g->version != KOS_SHM_VERSION) {
        set_error(err, KOS_ERR_VERSION, 0,
                  "'%.*s' 段版本 %lu 与本程序 %lu 不匹配：拒绝挂接，删掉旧段或让 Bridge 用"
                  "重建模式启动",
                  KOS_NAME_SHOWN, shm->object_name, (unsigned long)g->version,
                  (unsigned long)KOS_SHM_VERSION);
        return KOS_ERR_VERSION;
    }
    if (g->header_size != (uint32_t)sizeof(kos_shm_segment) ||
        g->world_size != (uint32_t)sizeof(kos_world_state) ||
        g->slot_size != (uint32_t)sizeof(kos_input_slot) ||
        g->slot_count != (uint32_t)KOS_SHM_MAX_CHEFS) {
        set_error(err, KOS_ERR_LAYOUT, 0,
                  "'%.*s' 段布局与本程序不一致（段 header=%lu world=%lu slot=%lu，本程序 "
                  "%lu/%lu/%lu）：同一版本号下结构漂移，必须重建段",
                  KOS_NAME_SHOWN, shm->object_name, (unsigned long)g->header_size,
                  (unsigned long)g->world_size, (unsigned long)g->slot_size,
                  (unsigned long)sizeof(kos_shm_segment),
                  (unsigned long)sizeof(kos_world_state),
                  (unsigned long)sizeof(kos_input_slot));
        return KOS_ERR_LAYOUT;
    }
    return KOS_OK;
}

/* 创建失败时把半成品清干净：留下一个段头齐全但没有信号量的段，下次启动会以
 * KOS_ERR_EXISTS 拒绝，反而比直接坏掉更难查。 */
static void abort_create(kos_shm *shm)
{
    if (!shm)
        return;
    if (shm->sem) {
        sem_close(shm->sem);
        sem_unlink(shm->sem_name);
        shm->sem = NULL;
    }
    if (shm->seg) {
        munmap(shm->seg, shm->map_size);
        shm->seg = NULL;
    }
    if (shm->fd >= 0) {
        close(shm->fd);
        shm->fd = -1;
    }
    if (shm->created)
        shm_unlink(shm->object_name);
}

/* ============================================================ 生命周期 */

void kos_shm_opts_init(kos_shm_opts *opts)
{
    if (!opts)
        return;
    memset(opts, 0, sizeof *opts);
    /* 这里只是把 env 抓一份快照；kos_shm_open 里还会再走一遍同样的优先级，
     * 所以手搓 opts（比如 worker_stub 从命令行填）也不会漏掉 env。 */
    opts->object_name = getenv(KOS_SHM_ENV_NAME);
    opts->sem_name = getenv(KOS_SHM_ENV_SEM_NAME);
    opts->dir = getenv(KOS_SHM_ENV_DIR);
    opts->perm = 0666;
}

int kos_shm_open(const kos_shm_opts *opts, kos_shm_mode mode, kos_shm **out, kos_error *err)
{
    char object[KOS_SHM_PATH_MAX];
    char sem[KOS_SHM_PATH_MAX];
    char dir[KOS_SHM_PATH_MAX];
    mode_t perm;
    kos_shm *shm;
    struct stat st;
    int flags;
    int rc;

    if (err)
        kos_error_clear(err);
    if (!out) {
        set_error(err, KOS_ERR_INVAL, 0, "kos_shm_open: out 为空");
        return KOS_ERR_INVAL;
    }
    *out = NULL;
    if (mode != KOS_SHM_ATTACH && mode != KOS_SHM_CREATE && mode != KOS_SHM_RECREATE) {
        set_error(err, KOS_ERR_INVAL, 0, "kos_shm_open: 未知打开模式 %d", (int)mode);
        return KOS_ERR_INVAL;
    }
    rc = resolve_names(opts, object, sem, dir, err);
    if (rc != KOS_OK)
        return rc;
    perm = (opts && opts->perm != 0) ? opts->perm : (mode_t)0666;

    shm = calloc(1, sizeof *shm);
    if (!shm) {
        set_error(err, KOS_ERR_NOMEM, 0, "calloc(kos_shm)");
        return KOS_ERR_NOMEM;
    }
    shm->fd = -1;
    copy_str(shm->object_name, sizeof shm->object_name, object);
    copy_str(shm->sem_name, sizeof shm->sem_name, sem);
    copy_str(shm->dir, sizeof shm->dir, dir);

    if (mode == KOS_SHM_RECREATE) {
        /* 只给"启动时清掉崩溃残留"用：读者没有权力删别人的段。 */
        rc = kos_shm_unlink(opts, err);
        if (rc != KOS_OK) {
            free(shm);
            return rc;
        }
        mode = KOS_SHM_CREATE;
    }

    flags = O_RDWR;
    if (mode == KOS_SHM_CREATE)
        flags |= O_CREAT | O_EXCL;
    shm->fd = shm_open(shm->object_name, flags, perm);
    if (shm->fd < 0) {
        int e = errno;
        int code = KOS_ERR_IO;

        if (e == ENOENT)
            code = KOS_ERR_MISSING;
        else if (e == EEXIST)
            code = KOS_ERR_EXISTS;
        set_error(err, code, e, "shm_open('%.*s', %s)", KOS_NAME_SHOWN, shm->object_name,
                  mode == KOS_SHM_CREATE ? "O_CREAT|O_EXCL" : "O_RDWR");
        free(shm);
        return code;
    }

    if (mode == KOS_SHM_CREATE) {
        shm->created = 1;
        if (ftruncate(shm->fd, (off_t)sizeof(kos_shm_segment)) != 0) {
            int e = errno;

            set_error(err, KOS_ERR_IO, e, "ftruncate('%.*s', %lu)", KOS_NAME_SHOWN,
                      shm->object_name, (unsigned long)sizeof(kos_shm_segment));
            abort_create(shm);
            free(shm);
            return KOS_ERR_IO;
        }
    }

    if (fstat(shm->fd, &st) != 0) {
        int e = errno;

        set_error(err, KOS_ERR_IO, e, "fstat('%.*s')", KOS_NAME_SHOWN, shm->object_name);
        abort_create(shm);
        free(shm);
        return KOS_ERR_IO;
    }
    /* 先量尺寸再映射：段比结构体小的话，映射完按结构体读会越过对象末尾收 SIGBUS，
     * 那是一句没有上下文的崩溃，查不出所以然。 */
    if ((uint64_t)st.st_size < (uint64_t)sizeof(kos_shm_segment)) {
        set_error(err, KOS_ERR_LAYOUT, 0, "'%.*s' 只有 %lld 字节，本程序需要 %lu 字节",
                  KOS_NAME_SHOWN, shm->object_name, (long long)st.st_size,
                  (unsigned long)sizeof(kos_shm_segment));
        abort_create(shm);
        free(shm);
        return KOS_ERR_LAYOUT;
    }
    shm->map_size = sizeof(kos_shm_segment);
    shm->seg = mmap(NULL, shm->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0);
    if (shm->seg == MAP_FAILED) {
        int e = errno;

        shm->seg = NULL;
        set_error(err, KOS_ERR_IO, e, "mmap('%.*s', %lu)", KOS_NAME_SHOWN, shm->object_name,
                  (unsigned long)shm->map_size);
        abort_create(shm);
        free(shm);
        return KOS_ERR_IO;
    }

    rc = verify_object_dir(shm->fd, shm->dir, shm->object_name, err);
    if (rc != KOS_OK) {
        abort_create(shm);
        free(shm);
        return rc;
    }

    if (shm->created)
        init_header(shm);
    else {
        rc = validate_header(shm, err);
        if (rc != KOS_OK) {
            abort_create(shm);
            free(shm);
            return rc;
        }
    }

    if (shm->created)
        shm->sem = sem_open(shm->sem_name, O_CREAT | O_EXCL, perm, 0u);
    else
        shm->sem = sem_open(shm->sem_name, 0);
    if (shm->sem == SEM_FAILED) {
        int e = errno;
        int code = (e == ENOENT) ? KOS_ERR_MISSING : KOS_ERR_IO;

        shm->sem = NULL;
        set_error(err, code, e, "sem_open('%.*s', %s)", KOS_NAME_SHOWN, shm->sem_name,
                  shm->created ? "O_CREAT|O_EXCL" : "0");
        abort_create(shm);
        free(shm);
        return code;
    }

    if (shm->created) {
        /* 建好就先发布一份"空厨房"：否则读者面对序号 0 只能报 KOS_ERR_EMPTY，
         * 而"段已建好但一帧都没来"是很正常的状态。 */
        kos_world_state empty;

        kos_world_state_reset(&empty);
        kos_shm_world_write(shm, &empty);
    }

    *out = shm;
    return KOS_OK;
}

int kos_shm_close(kos_shm *shm, kos_error *err)
{
    int rc = KOS_OK;

    if (err)
        kos_error_clear(err);
    if (!shm) {
        set_error(err, KOS_ERR_INVAL, 0, "kos_shm_close: shm 为空");
        return KOS_ERR_INVAL;
    }
    if (shm->seg) {
        if (munmap(shm->seg, shm->map_size) != 0) {
            set_error(err, KOS_ERR_IO, errno, "munmap('%.*s')", KOS_NAME_SHOWN,
                      shm->object_name);
            rc = KOS_ERR_IO;
        }
        shm->seg = NULL;
    }
    if (shm->sem) {
        if (sem_close(shm->sem) != 0 && rc == KOS_OK) {
            set_error(err, KOS_ERR_IO, errno, "sem_close('%.*s')", KOS_NAME_SHOWN, shm->sem_name);
            rc = KOS_ERR_IO;
        }
        shm->sem = NULL;
    }
    if (shm->fd >= 0) {
        if (close(shm->fd) != 0 && rc == KOS_OK) {
            set_error(err, KOS_ERR_IO, errno, "close('%.*s')", KOS_NAME_SHOWN, shm->object_name);
            rc = KOS_ERR_IO;
        }
        shm->fd = -1;
    }
    free(shm);
    return rc;
}

int kos_shm_unlink(const kos_shm_opts *opts, kos_error *err)
{
    char object[KOS_SHM_PATH_MAX];
    char sem[KOS_SHM_PATH_MAX];
    char dir[KOS_SHM_PATH_MAX];
    int rc;

    if (err)
        kos_error_clear(err);
    rc = resolve_names(opts, object, sem, dir, err);
    if (rc != KOS_OK)
        return rc;

    /* 对象/信号量本来就不在也算成功：清理路径必须幂等，否则"清理时对象不见了"
     * 会被当成失败，把真正的错误盖掉。 */
    if (shm_unlink(object) != 0 && errno != ENOENT) {
        int e = errno;

        set_error(err, KOS_ERR_IO, e, "shm_unlink('%.*s')", KOS_NAME_SHOWN, object);
        return KOS_ERR_IO;
    }
    if (sem_unlink(sem) != 0 && errno != ENOENT) {
        int e = errno;

        set_error(err, KOS_ERR_IO, e, "sem_unlink('%.*s')", KOS_NAME_SHOWN, sem);
        return KOS_ERR_IO;
    }
    return KOS_OK;
}

/* ================================================================ 杂项 */

static const char *const action_names[] = {"none", "move", "stop", "interact", "pickup", "dash", "path", "face", "work"};

#define KOS_ACTION_NAME_COUNT ((int)(sizeof action_names / sizeof action_names[0]))

const char *kos_action_kind_name(int kind)
{
    if (kind < 0 || kind >= KOS_ACTION_NAME_COUNT)
        return "unknown";
    return action_names[kind];
}

int kos_action_kind_from_name(const char *name)
{
    int i;

    if (!name)
        return -1;
    for (i = 0; i < KOS_ACTION_NAME_COUNT; i++) {
        if (strcmp(name, action_names[i]) == 0)
            return i;
    }
    return -1;
}
