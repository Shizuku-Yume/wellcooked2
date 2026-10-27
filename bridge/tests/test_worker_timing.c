#include "../src/worker_timing.h"

#include <stdio.h>

int main(void)
{
    if (kos_worker_interact_window_covers_action_pulses(250)) {
        fprintf(stderr, "old 250 ms window unexpectedly covers retry pulse\n");
        return 1;
    }
    if (!kos_worker_interact_window_covers_action_pulses(KOS_WORKER_INTERACT_WINDOW_MS)) {
        fprintf(stderr, "worker interact window truncates plugin retry pulses\n");
        return 1;
    }
    /*
     * S6：宏在等游戏状态变化时会连续几秒返回 INTERACT（实机 chop 4.7 s、
     * deliver 等订单结果）。交互窗口用完必须续，否则这一步剩下的时间全发 stop，
     * 插件只按过一次。这里用 5 ms 的 worker 节奏模拟 4709 ms 的等待。
     */
    {
        int64_t game_ms = 0;
        int64_t until = KOS_WORKER_INTERACT_WINDOW_MS;
        int armed_step = 3;
        int rearm_count = 0;
        int t;
        int stopped_frames = 0;

        for (t = 0; t < 941; t++) { /* 941 × 5 ms = 4705 ms */
            int rearm = kos_worker_interact_should_rearm(armed_step, 3, game_ms, until);

            if (rearm) {
                rearm_count++;
                until = game_ms + KOS_WORKER_INTERACT_WINDOW_MS;
            }
            if (!rearm && game_ms > until) {
                stopped_frames++;
            }
            game_ms += 5;
        }
        if (rearm_count < 4) {
            fprintf(stderr, "interact window not re-armed during a 4.7 s wait (%d)\n",
                    rearm_count);
            return 1;
        }
        if (stopped_frames != 0) {
            fprintf(stderr, "interact intent expired mid-wait %d times\n", stopped_frames);
            return 1;
        }
        if (!kos_worker_interact_should_rearm(-1, 0, 0, 0)) {
            fprintf(stderr, "new step must arm the window\n");
            return 1;
        }
        if (kos_worker_interact_should_rearm(0, 0, 500, KOS_WORKER_INTERACT_WINDOW_MS)) {
            fprintf(stderr, "armed window must not re-arm early\n");
            return 1;
        }
        printf("4.7 s wait re-armed interact %d times, never expired\n", rearm_count);
    }
    printf("250 ms window rejects; 1000 ms window covers both plugin pulse edges\n");
    return 0;
}
