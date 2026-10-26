/*
 * state.h -- wire message -> shared-memory snapshot.
 *
 * 这一层把 `proto_state_update`（协议形状）落成 `kos_world_state`（共享内存形状）。
 * 它单独成模块是为了可测：bridge.c 带 main()，测试不能链接它；把纯映射放在这里，
 * `bridge/tests/test_state.c` 就能直接拿一份解析结果核对每个字段。
 */
#ifndef KITCHENOS_STATE_H
#define KITCHENOS_STATE_H

#include <kitchenos/protocol.h>
#include <kitchenos/shm.h>

/*
 * 覆盖写 *world：先把整份快照清成"空厨房"，再按字段上限截断后逐项拷贝。
 * 纯函数，不分配内存，不碰 IPC。数组超限时只丢超出部分（与协议声明的行为一致）。
 */
void kos_state_from_update(const proto_state_update *up, kos_world_state *world);

/*
 * 线上 double → 共享内存数值的唯一一套规则（NaN/越界取 0 或饱和、四舍五入）。
 * 导出是因为 bridge.c 校验输入槽与打日志时也要用同一套：两处各写一份迟早会在
 * NaN 上给出不同答案，而共享内存里出现 NaN 会让调度侧的比较全部失真。
 */
int kos_state_double_is_finite(double v);
int64_t kos_state_double_to_ll(double v);
float kos_state_double_to_float(double v);

#endif /* KITCHENOS_STATE_H */
