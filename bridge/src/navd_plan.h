#ifndef KOS_NAVD_PLAN_H
#define KOS_NAVD_PLAN_H

#include <kitchenos/navtab.h>
#include <math.h>

#define NAVD_ARRIVE_EPS 0.3f
#define NAVD_LEAVE_EPS  0.5f

/*
 * 判断 new_plan 是否为 old_plan 在同一目标下的路线延续：
 * 1. 走过的格从队首弹出 (new 是 old 的后缀)；
 * 2. PIBT 临时少放行 (new 是 old 的前缀或内部子段)；
 * 3. PIBT 向后延长放行 (new 在尾部追加新路点)；
 * 4. 临时 0 路点（等待格释放）。
 * 只要路线没有分叉转向其它格，且目标序号一致，均不算新路线，不换 plan_seq。
 */
static inline int kos_nav_plan_is_continuation(const kos_nav_plan *old_plan,
                                               const kos_nav_plan *new_plan)
{
    int old_count;
    int new_count;
    int k;
    int j;

    if (old_plan == NULL || new_plan == NULL) {
        return 0;
    }
    if (new_plan->target_seq != old_plan->target_seq) {
        return 0;
    }
    if (new_plan->status != old_plan->status) {
        return 0;
    }

    old_count = old_plan->waypoint_count;
    new_count = new_plan->waypoint_count;

    if (new_count == 0 || old_count == 0) {
        return 1;
    }

    for (k = 0; k < old_count; k++) {
        float dx = new_plan->waypoints[0].x - old_plan->waypoints[k].x;
        float dy = new_plan->waypoints[0].y - old_plan->waypoints[k].y;
        if (dx * dx + dy * dy < 0.0001f) {
            int match = 1;
            for (j = 0; j < new_count && (k + j) < old_count; j++) {
                float djx = new_plan->waypoints[j].x - old_plan->waypoints[k + j].x;
                float djy = new_plan->waypoints[j].y - old_plan->waypoints[k + j].y;
                if (djx * djx + djy * djy >= 0.0001f) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                return 1;
            }
        }
    }
    return 0;
}

/*
 * 到站判定滞回：进 0.3 m 算到，出 0.5 m 才算离开。
 */
static inline int kos_nav_goal_hysteresis(int was_arrived, float min_stand_dist)
{
    float eps = was_arrived ? NAVD_LEAVE_EPS : NAVD_ARRIVE_EPS;
    return min_stand_dist <= eps;
}

/*
 * 双厨师同时无进度时，只让优先级较低的一方重新规划。调用方已通过实际
 * 位置/计划筛选候选；这里只负责稳定、无状态的仲裁，不改变目标和 pinned。
 */
static inline int kos_nav_should_yield_pair(int self_stuck, int other_stuck,
                                            int self_pinned, int other_pinned,
                                            int self_arrived, int other_arrived,
                                            int self_priority, int other_priority,
                                            int self_chef_id, int other_chef_id)
{
    if (!self_stuck || !other_stuck || self_pinned || other_pinned ||
        self_arrived || other_arrived) {
        return 0;
    }
    if (self_priority != other_priority) {
        return self_priority < other_priority;
    }
    return self_chef_id > other_chef_id;
}

#endif /* KOS_NAVD_PLAN_H */
