/*
 * macro.h -- 一道工序的宏动作序列与按世界状态判完成的执行器（模块契约）。
 *
 * Worker 不再是"走到、按一下、等工时"：Scheduler 派的 op 先展开成 goto/pickup/
 * drop/chop/start_cook/wait_ready/wash/deliver 的序列，每一步读 shm 世界状态判完成，
 * 超时或完成条件被打破才失败。只有导航目标槽/计划槽仍由 Worker 接线（见 worker.c），
 * 本模块不碰共享内存、不读时钟——调用方把一份 kos_world_state 与游戏时钟传进来，
 * 所以单元测试能用合成世界状态在微秒里跑完一整道工序。
 *
 * 工位 id 来自 kitchen.json（`cooker-0`），世界状态里的资源键来自插件（`pot_0`）。
 * 两者不一致时按"精确名 → 归一化 `-`/`_` → 同类工位序号"找资源；这是任务 09 的
 * 真实接入前唯一的匹配层，编号规则与插件 StateFrame 的排序一致。
 *
 * 完成条件（与 spec L2 一致）：
 *   goto      计划槽 arrived + 插件转到工位（世界状态 facing 误差 < 15°；插件只在
 *             这个误差内回 ack，ack 本身不下发到 Worker）
 *   pickup    holding 变成目标物品；`item` 为空串表示"拿到任意物品"（切完的 prefab
 *             名与原料名不一定同源，切菜与端起锅都用这一条，别按名字猜）
 *   drop      holding 变空
 *   chop      工位 progress 到 1
 *   start_cook 锅 state 变成 cooking/cooked
 *   pour      手上锅/容器（`carried_<chefId>`）的内容从非空变空；旧状态没有 carried_
 *             时退回"源锅里那味料消失"（按内容找 pot_*，不按槽位号）；整段没有可用
 *             信号时按 MACRO_POUR_FALLBACK_MS 兜底
 *   wash      水槽 progress 到 1 且干净盘数加一（没有 plate_stack 可读时只认 progress）
 *   deliver   订单从世界状态订单表消失
 */
#ifndef KITCHENOS_MACRO_H
#define KITCHENOS_MACRO_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>

#include <kitchenos/kitchen.h>
#include <kitchenos/shm.h>

#define KOS_MACRO_MAX_STEPS 8
#define KOS_MACRO_DETAIL_MAX 96
#define KOS_MACRO_FACE_TOLERANCE_RAD 0.26179939f /* 15° */

/*
 * 任务 01 实测的每格移动时间（0.107 s/格，5.6 m/s）。只用来给 goto 定超时：
 * Worker 不再用它推算厨师位置，位置只认世界状态与规划结果。
 */
#define KOS_MACRO_CELL_MS 107

typedef enum {
    KOS_MACRO_GOTO = 0,
    KOS_MACRO_PICKUP = 1,
    KOS_MACRO_DROP = 2,
    KOS_MACRO_CHOP = 3,
    KOS_MACRO_START_COOK = 4,
    KOS_MACRO_POUR = 5,
    KOS_MACRO_WASH = 6,
    KOS_MACRO_DELIVER = 7
} kos_macro_kind;

/* 每一步对 Worker 说"这一帧该发什么"；导航仍由 Worker 读计划槽后发 path。 */
typedef enum {
    KOS_MACRO_ACT_STOP = 0,
    KOS_MACRO_ACT_NAVIGATE = 1,
    KOS_MACRO_ACT_FACE = 2,
    KOS_MACRO_ACT_INTERACT = 3,
    KOS_MACRO_ACT_WORK = 4
} kos_macro_action;

typedef struct {
    int kind;                          /* kos_macro_kind */
    char station_id[KOS_SHM_NAME_MAX];
    char item[KOS_SHM_NAME_MAX];       /* pickup 要拿到的东西 / chop 的目标产物 */
    int64_t duration_ms;               /* 工时类超时的基准（Scheduler 下发的值） */
} kos_macro_step;

typedef struct {
    int step_count;
    kos_macro_step steps[KOS_MACRO_MAX_STEPS];
    char order_id[KOS_SHM_NAME_MAX];   /* deliver 判"订单消失"用 */
} kos_macro_plan;

typedef struct {
    int action;                        /* kos_macro_action */
    char target_object[KOS_SHM_NAME_MAX]; /* face/work/interact 的目标工位 */
} kos_macro_output;

typedef struct {
    kos_macro_plan plan;
    int step_index;
    int step_started;
    int64_t step_start_ms;             /* 游戏时钟 */
    int64_t deadline_ms;               /* 游戏时钟；第一次 advance 时按步算 */
    int face_sent;
    int64_t face_sent_ms;
    int saw_content;                   /* chop/start_cook 曾看到工位上有料 */
    /* chop：第一次看到板上内容时核对过"是不是这一份料"。切完游戏会把物体换成
     * Chopped…/Diced… 名字，之后不能再按原始料名核对（G13 只抓放错料）。 */
    int content_checked;
    char entry_holding[KOS_SHM_NAME_MAX];
    int clean_baseline_valid;
    int32_t clean_baseline;
    /* DROP 步开始时厨师手里的物品；start_cook/chop 用它核对工位 content，
     * 抓"料放错锅/板"这种游戏会静默拒绝的工序（G13）。 */
    char dropped_item[KOS_SHM_NAME_MAX];
    /* DELIVER 步开始时的 world.score。订单消失时用它区分"交付"与"过期"（G9）：
     * 分数上涨是交付，下降是过期；不变说明整局分数没有上报（离线模拟器）。 */
    int64_t score_base;
    int score_base_valid;
    /* DELIVER 步手变空的时刻：盘子交付时先消失、订单结果晚几百毫秒才到，
     * 先等结果/订单消失，超过 grace 才判 holding-lost。 */
    int64_t empty_hand_since_ms;
    char fail_detail[KOS_MACRO_DETAIL_MAX];
} kos_macro;

const char *kos_macro_kind_name(int kind);
const char *kos_macro_action_name(int action);

/* 把 Scheduler 的 op 展开成宏动作序列。node_id 形如 "fetch:Carrot"。
 *
 * `source_station_id` 只给 `pour` 用：源锅所在的灶台（本订单 cook 已占用），
 * plate 不再从灶上取料，传进来的值会被忽略。 */
int kos_macro_plan_build_with_source(const kos_kitchen *kitchen, const char *op,
                                     const char *node_id, const char *station_id,
                                     const char *source_station_id, const char *order_id,
                                     int duration_ms, kos_macro_plan *out, kos_error *err);

int kos_macro_plan_build(const kos_kitchen *kitchen, const char *op, const char *node_id,
                         const char *station_id, const char *order_id, int duration_ms,
                         kos_macro_plan *out, kos_error *err);

/* 开始执行一份已建好的计划。 */
void kos_macro_begin(kos_macro *macro, const kos_macro_plan *plan);

/*
 * 推进到 game_ms。nav_arrived 由 Worker 从计划槽读（目标序号匹配且状态 arrived）。
 * 返回 0 = 还在执行，1 = 全部完成，-1 = 失败（fail_detail 写宏动作与所等条件）。
 * 每次调用都填 out；调用方按 out 发动作。世界状态只读，可传任意合成快照。
 */
int kos_macro_advance(kos_macro *macro, const kos_kitchen *kitchen,
                      const kos_world_state *world, int chef_id, int nav_arrived,
                      int64_t game_ms, kos_macro_output *out);

/* 失败原因（一行，含宏动作与所等条件）；没有失败时为空串。 */
const char *kos_macro_fail_detail(const kos_macro *macro);

#endif /* KITCHENOS_MACRO_H */
