/*
 * mq.h -- 调度消息队列的名字与消息布局（模块契约）。
 *
 * Scheduler 创建 `/wellcooked_events` 与每个厨师一条 `/wellcooked_tasks_<chef>`；
 * Worker 打开自己的一条与 events。消息是定长结构体（不是 JSON）：跨进程只通过这个
 * 头文件解释字节，字段上限写死，mq_msgsize 取 sizeof(kos_mq_msg)。
 *
 * 优先级用 mq 的 priority（1..32，越大越先被取走），对应策略算出的紧急度。
 */
#ifndef WELLCOOKED_MQ_H
#define WELLCOOKED_MQ_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <mqueue.h>

#include <wellcooked/shm.h>

#define KOS_MQ_EVENTS_NAME "/wellcooked_events"
#define KOS_MQ_TASK_PREFIX "/wellcooked_tasks_"
#define KOS_MQ_NAME_MAX 64
/* Task payload includes an explicit plate-source station. */
#define KOS_MQ_VERSION 2u
#define KOS_MQ_NODE_ID_MAX 64
#define KOS_MQ_OP_MAX 16
#define KOS_MQ_DETAIL_MAX 64
#define KOS_MQ_MAX_MESSAGES 10 /* Linux default /proc/sys/fs/mqueue/msg_max */

typedef enum {
    KOS_MQ_TASK = 1,
    KOS_MQ_EVENT = 2
} kos_mq_kind;

typedef enum {
    KOS_EVT_ACCEPTED = 1,
    KOS_EVT_DONE = 2,
    KOS_EVT_FAILED = 3,
    KOS_EVT_BLOCKED = 4,
    KOS_EVT_RECOVERED = 5 /* Worker restarted; Scheduler requeues its in-flight task */
} kos_mq_event;

typedef struct {
    uint32_t version;
    uint32_t kind;
    uint64_t task_id;
    int32_t chef_id;
    int32_t priority;
    int32_t duration_ms;
    int32_t event; /* KOS_EVT_*，仅 kind == KOS_MQ_EVENT 时有效 */
    int32_t generation; /* Worker 事件：用于区分首次启动与重启 */
    float target_x;
    float target_y;
    char order_id[KOS_SHM_NAME_MAX];
    char dish[KOS_SHM_NAME_MAX];
    char node_id[KOS_MQ_NODE_ID_MAX];
    char op[KOS_MQ_OP_MAX];
    char resource[KOS_SHM_NAME_MAX];
    char station_id[KOS_SHM_NAME_MAX];
    /* plate keeps the plate-stack lease but names the exact cooker holding this order. */
    char source_station_id[KOS_SHM_NAME_MAX];
    char detail[KOS_MQ_DETAIL_MAX];
} kos_mq_msg;

/* instance >= 0 时名字加 `_<instance>`，与 shm 的后缀规则一致。 */
int kos_mq_events_name(int instance, char *buf, size_t len);
int kos_mq_task_name(int chef, int instance, char *buf, size_t len);

/* 创建队列；同名已存在返回 KOS_ERR_EXISTS。max_messages 限制队列深度。 */
int kos_mq_create(const char *name, int max_messages, kos_error *err);

/* flags 取 O_RDONLY / O_WRONLY / O_RDWR。 */
int kos_mq_open(const char *name, int flags, mqd_t *out, kos_error *err);
void kos_mq_close(mqd_t queue);

/* timeout_ms < 0 表示一直等；0 表示只试一次；>0 是毫秒上限。 */
int kos_mq_send(mqd_t queue, const kos_mq_msg *msg, unsigned priority, int timeout_ms,
                kos_error *err);
int kos_mq_receive(mqd_t queue, kos_mq_msg *msg, int timeout_ms, kos_error *err);

/*
 * 删除 events 与 workers 条任务队列；已经不存在的名字也算成功（清理必须幂等）。
 * 返回第一个真实错误；调用方负责记录 workers 数量与结果。
 */
int kos_mq_unlink_all(int workers, int instance, kos_error *err);

#endif /* WELLCOOKED_MQ_H */
