#ifndef KOS_WORKER_TIMING_H
#define KOS_WORKER_TIMING_H

/* Keep aligned with plugin/ActionDriver.cs: PulseSeconds=.25, PulseInterval=.6,
 * KeyPulses=2. A worker intent must survive through the second pulse release. */
#define KOS_ACTION_PULSE_HOLD_MS 250
#define KOS_ACTION_PULSE_INTERVAL_MS 600
#define KOS_ACTION_PULSE_COUNT 2
#define KOS_WORKER_INTERACT_WINDOW_MS 1000

static inline int kos_worker_interact_window_covers_action_pulses(int window_ms)
{
    int required_ms = (KOS_ACTION_PULSE_COUNT - 1) * KOS_ACTION_PULSE_INTERVAL_MS +
                      KOS_ACTION_PULSE_HOLD_MS;
    return window_ms >= required_ms;
}

#endif
