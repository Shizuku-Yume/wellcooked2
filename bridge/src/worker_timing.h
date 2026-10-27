#ifndef KOS_WORKER_TIMING_H
#define KOS_WORKER_TIMING_H

#include <stdint.h>

/* Keep aligned with plugin/ActionDriver.cs: PulseSeconds=.25, PulseInterval=.6,
 * KeyPulses=2. A worker intent must survive through the second pulse release. */
#define KOS_ACTION_PULSE_HOLD_MS 250
#define KOS_ACTION_PULSE_INTERVAL_MS 600
#define KOS_ACTION_PULSE_COUNT 2
#define KOS_WORKER_INTERACT_WINDOW_MS 1000

/*
 * 一次 interact 意图的交互窗口到点后要不要重新起窗（S6）。
 *
 * 窗口只覆盖插件两次脉冲（250 ms 按住 + 600 ms 间隔）。宏在等待游戏状态变化时
 * 会连续几秒返回 INTERACT（切菜超时 4.7 s、交付等订单结果），窗口用完就再发一次
 * 意图，而不是让这一步剩下的时间都发 stop：实机 _3 的 pickup/deliver 只按了一次。
 */
static inline int kos_worker_interact_should_rearm(int armed_step, int step_index,
                                                   int64_t now_ms, int64_t until_ms)
{
    if (armed_step != step_index) {
        return 1; /* 这一步还没发过 interact */
    }
    return now_ms > until_ms; /* 窗口用完，宏仍然在要 interact */
}

static inline int kos_worker_interact_window_covers_action_pulses(int window_ms)
{
    int required_ms = (KOS_ACTION_PULSE_COUNT - 1) * KOS_ACTION_PULSE_INTERVAL_MS +
                      KOS_ACTION_PULSE_HOLD_MS;
    return window_ms >= required_ms;
}

#endif
