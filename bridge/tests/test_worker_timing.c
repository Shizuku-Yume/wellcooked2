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
    printf("250 ms window rejects; 1000 ms window covers both plugin pulse edges\n");
    return 0;
}
