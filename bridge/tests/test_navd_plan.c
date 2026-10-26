/*
 * test_navd_plan.c -- 验证 navd 路线延续判定与到站滞回。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../src/navd_plan.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

static void expect(int cond, const char *what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        g_failures++;
        printf("  FAIL %s\n", what);
    }
}

int main(void)
{
    printf("[test_navd_plan] 开始：路线延续与到站滞回\n");

    /* 1. 路线延续判定 */
    {
        kos_nav_plan old_plan;
        kos_nav_plan new_plan;

        memset(&old_plan, 0, sizeof old_plan);
        memset(&new_plan, 0, sizeof new_plan);

        old_plan.target_seq = 1;
        old_plan.status = KOS_NAV_MOVING;
        old_plan.waypoint_count = 4;
        old_plan.waypoints[0].x = 0.0f; old_plan.waypoints[0].y = 0.0f;
        old_plan.waypoints[1].x = 0.6f; old_plan.waypoints[1].y = 0.0f;
        old_plan.waypoints[2].x = 1.2f; old_plan.waypoints[2].y = 0.0f;
        old_plan.waypoints[3].x = 1.8f; old_plan.waypoints[3].y = 0.0f;

        /* A. 走过一格从队首弹出 (后缀) */
        new_plan.target_seq = 1;
        new_plan.status = KOS_NAV_MOVING;
        new_plan.waypoint_count = 3;
        new_plan.waypoints[0].x = 0.6f; new_plan.waypoints[0].y = 0.0f;
        new_plan.waypoints[1].x = 1.2f; new_plan.waypoints[1].y = 0.0f;
        new_plan.waypoints[2].x = 1.8f; new_plan.waypoints[2].y = 0.0f;

        expect(kos_nav_plan_is_continuation(&old_plan, &new_plan) == 1,
               "队首弹出后缀算作同一路线延续");

        /* B. PIBT 临时少放行 (前缀) */
        new_plan.waypoint_count = 2;
        new_plan.waypoints[0].x = 0.0f; new_plan.waypoints[0].y = 0.0f;
        new_plan.waypoints[1].x = 0.6f; new_plan.waypoints[1].y = 0.0f;

        expect(kos_nav_plan_is_continuation(&old_plan, &new_plan) == 1,
               "PIBT 临时少放行前缀算作同一路线延续");

        /* C. 队首弹出且 PIBT 临时少放行 (中间子段) */
        new_plan.waypoint_count = 2;
        new_plan.waypoints[0].x = 0.6f; new_plan.waypoints[0].y = 0.0f;
        new_plan.waypoints[1].x = 1.2f; new_plan.waypoints[1].y = 0.0f;

        expect(kos_nav_plan_is_continuation(&old_plan, &new_plan) == 1,
               "队首弹出且少放行中间子段算作同一路线延续");

        /* D. 尾部追加新格 (延伸) */
        new_plan.waypoint_count = 3;
        new_plan.waypoints[0].x = 1.2f; new_plan.waypoints[0].y = 0.0f;
        new_plan.waypoints[1].x = 1.8f; new_plan.waypoints[1].y = 0.0f;
        new_plan.waypoints[2].x = 2.4f; new_plan.waypoints[2].y = 0.0f;

        expect(kos_nav_plan_is_continuation(&old_plan, &new_plan) == 1,
               "尾部延长放行算作同一路线延续");

        /* E. 临时 0 路点 */
        new_plan.waypoint_count = 0;
        expect(kos_nav_plan_is_continuation(&old_plan, &new_plan) == 1,
               "临时 0 路点等待不换 seq");

        /* F. 路线分叉转向其它格 (新路线) */
        new_plan.waypoint_count = 3;
        new_plan.waypoints[0].x = 0.6f; new_plan.waypoints[0].y = 0.0f;
        new_plan.waypoints[1].x = 0.6f; new_plan.waypoints[1].y = 0.6f; /* 拐向 y=0.6 */
        new_plan.waypoints[2].x = 0.6f; new_plan.waypoints[2].y = 1.2f;

        expect(kos_nav_plan_is_continuation(&old_plan, &new_plan) == 0,
               "路线分叉判为新路线");

        /* G. 目标序号变化 (新目标) */
        new_plan.target_seq = 2;
        new_plan.waypoint_count = 3;
        new_plan.waypoints[0].x = 0.6f; new_plan.waypoints[0].y = 0.0f;
        new_plan.waypoints[1].x = 1.2f; new_plan.waypoints[1].y = 0.0f;
        new_plan.waypoints[2].x = 1.8f; new_plan.waypoints[2].y = 0.0f;

        expect(kos_nav_plan_is_continuation(&old_plan, &new_plan) == 0,
               "目标序号改变换 seq");
    }

    /* 2. 到站滞回判定 */
    {
        int arrived = 0;

        /* 初始靠近过程：0.4m 未进 0.3m */
        arrived = kos_nav_goal_hysteresis(arrived, 0.40f);
        expect(arrived == 0, "0.40m 初始状态未到站");

        /* 进 0.30m 算到 */
        arrived = kos_nav_goal_hysteresis(arrived, 0.29f);
        expect(arrived == 1, "0.29m 进 0.3m 算到站");

        /* 转身阶段漂移到 0.35m，因滞回 (0.5m) 保持到站状态 */
        arrived = kos_nav_goal_hysteresis(arrived, 0.35f);
        expect(arrived == 1, "0.35m 转身漂移滞回保持到站");

        /* 漂移到 0.49m 仍算到站 */
        arrived = kos_nav_goal_hysteresis(arrived, 0.49f);
        expect(arrived == 1, "0.49m 仍在 0.5m 内保持到站");

        /* 离开 0.51m 算离开 */
        arrived = kos_nav_goal_hysteresis(arrived, 0.51f);
        expect(arrived == 0, "0.51m 超出 0.5m 算离开");

        /* 离开后在 0.40m 处，未进 0.3m 不算到站 */
        arrived = kos_nav_goal_hysteresis(arrived, 0.40f);
        expect(arrived == 0, "离开后 0.40m 未重新进 0.3m 不算到站");
    }

    printf("[test_navd_plan] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
