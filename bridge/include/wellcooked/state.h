/*
 * state.h -- wire message -> shared-memory snapshot.
 *
 * 这一层把 `proto_state_update`（协议形状）落成 `kos_world_state`（共享内存形状）。
 * 它单独成模块是为了可测：bridge.c 带 main()，测试不能链接它；把纯映射放在这里，
 * `bridge/tests/test_state.c` 就能直接拿一份解析结果核对每个字段。
 */
#ifndef WELLCOOKED_STATE_H
#define WELLCOOKED_STATE_H

#include <wellcooked/log.h>
#include <wellcooked/protocol.h>
#include <wellcooked/shm.h>

/*
 * 覆盖写 *world：先把整份快照清成"空厨房"，再按字段上限截断后逐项拷贝。
 * 纯函数，不分配内存，不碰 IPC。数组超限时只丢超出部分（与协议声明的行为一致）。
 */
void kos_state_from_update(const proto_state_update *up, kos_world_state *world);

/*
 * 同上，但资源数超过共享内存布局上限时打一行 `resource_truncated`（数量变化时
 * 才打）。截断是真实的丢数据：插件按 cookers → … → carried → containers 的顺序
 * 上报，超限时先丢 carried_<M>。log 传 NULL 时静默（测试用）。
 */
void kos_state_from_update_logged(const proto_state_update *up, kos_world_state *world,
                                  kos_log *log);

/*
 * 这一帧有多少个资源键因为共享内存布局的上限被丢掉。0 表示整帧都落下了。
 * 插件按固定顺序上报、`carried_<M>` 靠后，所以被丢的正是"锅在谁手上"这类信息；
 * bridge 用它在丢的时候打一行 `resource_truncated`，别让截断静默发生。
 */
size_t kos_state_resources_dropped(const proto_state_update *up);

/*
 * 线上 double → 共享内存数值的唯一一套规则（NaN/越界取 0 或饱和、四舍五入）。
 * 导出是因为 bridge.c 校验输入槽与打日志时也要用同一套：两处各写一份迟早会在
 * NaN 上给出不同答案，而共享内存里出现 NaN 会让调度侧的比较全部失真。
 */
int kos_state_double_is_finite(double v);
int64_t kos_state_double_to_ll(double v);
float kos_state_double_to_float(double v);

#endif /* WELLCOOKED_STATE_H */
