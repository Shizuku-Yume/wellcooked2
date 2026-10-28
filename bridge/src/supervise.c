/*
 * supervise.c -- 重启策略的纯状态机。没有系统调用，所以测试能直接构造时间。
 *
 * 崩溃窗口用环形历史而不是"窗口起点 + 计数"：后者在窗口到期时必须一次性清零，
 * 会让"刚刚还在崩溃、清零点后立刻又崩"这种模式被当成首次失败、立即重启，退避形同
 * 虚设。按时间戳逐条判断，窗口是滑动的，语义与 §6.1 的"10 秒内 / 1 分钟内"一致。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <wellcooked/supervise.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

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

void kos_supervise_policy_init(kos_supervise_policy *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof *p);
    p->crash_limit = 5;
    p->crash_window_ms = 60000;
    p->backoff_ms[0] = 0;
    p->backoff_ms[1] = 2000;
    p->backoff_ms[2] = 10000;
    p->backoff_count = 3;
}

int kos_supervise_policy_parse_backoff(const char *list, kos_supervise_policy *p,
                                       kos_error *err)
{
    const char *cursor;
    int count = 0;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (list == NULL || p == NULL || list[0] == '\0') {
        return KOS_ERR_INVAL;
    }

    cursor = list;
    while (*cursor != '\0') {
        char *end = NULL;
        long value;

        if (count >= KOS_SUPERVISE_MAX_BACKOFF) {
            if (err != NULL) {
                err->code = KOS_ERR_RANGE;
                snprintf(err->context, sizeof err->context,
                         "backoff 列表超过 %d 项", KOS_SUPERVISE_MAX_BACKOFF);
            }
            return KOS_ERR_RANGE;
        }
        errno = 0;
        value = strtol(cursor, &end, 10);
        if (errno != 0 || end == cursor || value < 0 || value > 600000) {
            if (err != NULL) {
                err->code = KOS_ERR_INVAL;
                snprintf(err->context, sizeof err->context, "backoff 项不合法: %s", cursor);
            }
            return KOS_ERR_INVAL;
        }
        p->backoff_ms[count++] = (int)value;
        if (*end == ',') {
            if (end[1] == '\0' || end[1] == ',') {
                if (err != NULL) {
                    err->code = KOS_ERR_INVAL;
                    snprintf(err->context, sizeof err->context,
                             "backoff 列表有空的项: %s", list);
                }
                return KOS_ERR_INVAL;
            }
            cursor = end + 1;
        } else if (*end == '\0') {
            break;
        } else {
            if (err != NULL) {
                err->code = KOS_ERR_INVAL;
                snprintf(err->context, sizeof err->context, "backoff 分隔符不合法: %s", end);
            }
            return KOS_ERR_INVAL;
        }
    }
    if (count == 0) {
        return KOS_ERR_INVAL;
    }
    p->backoff_count = count;
    return KOS_OK;
}

int kos_supervise_delay_for(const kos_supervise_policy *p, int failures_in_window)
{
    int index;

    if (p == NULL || p->backoff_count <= 0 || failures_in_window <= 0) {
        return 0;
    }
    /* 第 1 次失败用第 1 项（默认 0 = 立即），第 2 次用第 2 项……列表最后一项重复。 */
    index = failures_in_window - 1;
    if (index >= p->backoff_count) {
        index = p->backoff_count - 1;
    }
    return p->backoff_ms[index];
}

void kos_child_init(kos_child *c, const char *role, pid_t pid)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof *c);
    copy_str(c->role, sizeof c->role, role);
    c->pid = pid;
    c->state = (pid > 0) ? KOS_CHILD_RUNNING : KOS_CHILD_STOPPED;
    c->last_exit_code = -1;
}

void kos_child_started(kos_child *c, pid_t pid)
{
    if (c == NULL) {
        return;
    }
    c->pid = pid;
    c->state = KOS_CHILD_RUNNING;
    c->spawns++;
    /* last_exit_* stays as the previous exit's result: after an immediate restart the
     * status file must still say what killed the old process. */
}

int kos_child_failures_in_window(const kos_child *c, int64_t now_ms, int window_ms)
{
    int stored;
    int count = 0;
    int i;

    if (c == NULL || c->exit_count <= 0) {
        return 0;
    }
    stored = (c->exit_count < KOS_SUPERVISE_HISTORY) ? c->exit_count : KOS_SUPERVISE_HISTORY;
    for (i = 0; i < stored; i++) {
        int index = (c->exit_count - 1 - i) % KOS_SUPERVISE_HISTORY;

        if (index < 0) {
            index += KOS_SUPERVISE_HISTORY;
        }
        if (c->exit_times[index] > 0 && now_ms - c->exit_times[index] <= window_ms) {
            count++;
        }
    }
    return count;
}

kos_supervise_decision kos_child_on_exit(kos_child *c, int status, int64_t now_ms,
                                         const kos_supervise_policy *p)
{
    kos_supervise_decision decision;
    int failures;

    memset(&decision, 0, sizeof decision);
    if (c == NULL || p == NULL) {
        decision.action = KOS_SUPERVISE_NONE;
        return decision;
    }

    c->pid = 0;
    c->exits++;
    c->last_exit_ms = now_ms;
    c->last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    c->last_exit_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;

    c->exit_times[c->exit_count % KOS_SUPERVISE_HISTORY] = now_ms;
    c->exit_count++;

    failures = kos_child_failures_in_window(c, now_ms, p->crash_window_ms);
    decision.failures_in_window = failures;

    if (failures >= p->crash_limit) {
        c->state = KOS_CHILD_GIVEN_UP;
        c->next_restart_ms = 0;
        decision.action = KOS_SUPERVISE_GIVE_UP;
        return decision;
    }

    decision.action = KOS_SUPERVISE_RESTART;
    decision.delay_ms = kos_supervise_delay_for(p, failures);
    c->state = KOS_CHILD_PENDING;
    c->next_restart_ms = now_ms + decision.delay_ms;
    return decision;
}

int kos_child_alive(const kos_child *c)
{
    return c != NULL && c->state == KOS_CHILD_RUNNING && c->pid > 0;
}

const char *kos_child_state_name(int state)
{
    switch (state) {
    case KOS_CHILD_RUNNING:
        return "running";
    case KOS_CHILD_PENDING:
        return "pending_restart";
    case KOS_CHILD_GIVEN_UP:
        return "given_up";
    case KOS_CHILD_STOPPED:
        return "stopped";
    case KOS_CHILD_DISABLED:
        return "disabled";
    default:
        return "unknown";
    }
}
