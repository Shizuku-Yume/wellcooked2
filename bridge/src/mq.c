/*
 * mq.c -- POSIX 消息队列的薄封装。
 *
 * 定时收发用绝对 CLOCK_REALTIME 时间（mq_timed* 的接口如此），调用方只给相对
 * 毫秒；这里负责换算。消息长度必须正好是 sizeof(kos_mq_msg)：短消息说明两端对
 * 布局的理解不一致，宁可报错也不要按错位读字段。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <wellcooked/mq.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int set_error(kos_error *err, int code, int sys_errno, const char *what,
                     const char *name)
{
    if (err != NULL) {
        err->code = code;
        err->sys_errno = sys_errno;
        if (name != NULL) {
            snprintf(err->context, sizeof err->context, "%s('%s')", what, name);
        } else {
            snprintf(err->context, sizeof err->context, "%s", what);
        }
    }
    return code;
}

static void deadline_after(struct timespec *ts, int timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

int kos_mq_events_name(int instance, char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return KOS_ERR_INVAL;
    }
    return kos_name_instance(KOS_MQ_EVENTS_NAME, instance, buf, len);
}

int kos_mq_task_name(int chef, int instance, char *buf, size_t len)
{
    char base[KOS_MQ_NAME_MAX];

    if (chef < 0 || buf == NULL || len == 0) {
        return KOS_ERR_INVAL;
    }
    if (snprintf(base, sizeof base, "%s%d", KOS_MQ_TASK_PREFIX, chef) >=
        (int)sizeof base) {
        return KOS_ERR_INVAL;
    }
    return kos_name_instance(base, instance, buf, len);
}

int kos_mq_create(const char *name, int max_messages, kos_error *err)
{
    struct mq_attr attr;
    mqd_t queue;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (name == NULL || name[0] != '/' || max_messages <= 0 ||
        max_messages > KOS_MQ_MAX_MESSAGES) {
        return set_error(err, KOS_ERR_INVAL, 0, "队列参数不合法", name);
    }
    memset(&attr, 0, sizeof attr);
    attr.mq_flags = 0;
    attr.mq_maxmsg = max_messages;
    attr.mq_msgsize = (long)sizeof(kos_mq_msg);
    queue = mq_open(name, O_RDWR | O_CREAT | O_EXCL, 0644, &attr);
    if (queue == (mqd_t)-1) {
        int saved = errno;

        if (saved == EEXIST) {
            return set_error(err, KOS_ERR_EXISTS, saved, "队列已存在", name);
        }
        return set_error(err, KOS_ERR_IO, saved, "mq_open", name);
    }
    mq_close(queue);
    return KOS_OK;
}

int kos_mq_open(const char *name, int flags, mqd_t *out, kos_error *err)
{
    mqd_t queue;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (name == NULL || out == NULL) {
        return KOS_ERR_INVAL;
    }
    queue = mq_open(name, flags);
    if (queue == (mqd_t)-1) {
        int saved = errno;
        int code = saved == ENOENT ? KOS_ERR_MISSING : KOS_ERR_IO;

        return set_error(err, code, saved, "mq_open", name);
    }
    *out = queue;
    return KOS_OK;
}

void kos_mq_close(mqd_t queue)
{
    if (queue != (mqd_t)-1) {
        mq_close(queue);
    }
}

int kos_mq_send(mqd_t queue, const kos_mq_msg *msg, unsigned priority, int timeout_ms,
                kos_error *err)
{
    int rc;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (queue == (mqd_t)-1 || msg == NULL || msg->version != KOS_MQ_VERSION) {
        return KOS_ERR_INVAL;
    }
    if (priority < 1) {
        priority = 1;
    } else if (priority > 32) {
        priority = 32;
    }
    if (timeout_ms < 0) {
        rc = mq_send(queue, (const char *)msg, sizeof *msg, priority);
    } else {
        struct timespec deadline;

        deadline_after(&deadline, timeout_ms);
        rc = mq_timedsend(queue, (const char *)msg, sizeof *msg, priority, &deadline);
    }
    if (rc != 0) {
        int saved = errno;

        if (saved == ETIMEDOUT || saved == EAGAIN) {
            return set_error(err, KOS_ERR_TIMEOUT, saved, "mq_timedsend", NULL);
        }
        return set_error(err, KOS_ERR_IO, saved, "mq_send", NULL);
    }
    return KOS_OK;
}

int kos_mq_receive(mqd_t queue, kos_mq_msg *msg, int timeout_ms, kos_error *err)
{
    ssize_t got;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (queue == (mqd_t)-1 || msg == NULL) {
        return KOS_ERR_INVAL;
    }
    if (timeout_ms < 0) {
        got = mq_receive(queue, (char *)msg, sizeof *msg, NULL);
    } else {
        struct timespec deadline;

        deadline_after(&deadline, timeout_ms);
        got = mq_timedreceive(queue, (char *)msg, sizeof *msg, NULL, &deadline);
    }
    if (got < 0) {
        int saved = errno;

        if (saved == ETIMEDOUT || saved == EAGAIN) {
            return set_error(err, KOS_ERR_TIMEOUT, saved, "mq_timedreceive", NULL);
        }
        return set_error(err, KOS_ERR_IO, saved, "mq_receive", NULL);
    }
    if ((size_t)got != sizeof *msg || msg->version != KOS_MQ_VERSION) {
        return set_error(err, KOS_ERR_LAYOUT, 0, "消息长度/版本不匹配", NULL);
    }
    return KOS_OK;
}

int kos_mq_unlink_all(int workers, int instance, kos_error *err)
{
    char events_name[KOS_MQ_NAME_MAX];
    int i;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (workers < 0 || workers > KOS_SHM_MAX_CHEFS ||
        kos_mq_events_name(instance, events_name, sizeof events_name) != KOS_OK) {
        return KOS_ERR_INVAL;
    }

    if (mq_unlink(events_name) != 0 && errno != ENOENT) {
        if (err != NULL) {
            err->code = KOS_ERR_IO;
            err->sys_errno = errno;
            snprintf(err->context, sizeof err->context, "mq_unlink('%s')", events_name);
        }
        return KOS_ERR_IO;
    }

    for (i = 0; i < workers; i++) {
        char name[KOS_MQ_NAME_MAX];

        if (kos_mq_task_name(i, instance, name, sizeof name) != KOS_OK) {
            return KOS_ERR_INVAL;
        }
        if (mq_unlink(name) != 0 && errno != ENOENT) {
            if (err != NULL) {
                err->code = KOS_ERR_IO;
                err->sys_errno = errno;
                snprintf(err->context, sizeof err->context, "mq_unlink('%s')", name);
            }
            return KOS_ERR_IO;
        }
    }
    return KOS_OK;
}
