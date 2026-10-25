/*
 * navd.c -- 中央走位进程：每版世界状态推进一次 PIBT，按执行依赖图放行路点。
 *
 * 它只做四件事：读世界状态里的实际坐标、读 Worker 写的目标槽、规划/重规划、
 * 写计划槽。不选工序、不下发动作——Worker 读计划槽后自己发 path。
 *
 * 计划窗口 H = 8。计划从"下一格"开始存，执行到哪一格按实际坐标 ≤0.3 m 对；
 * 卡住判定用游戏时钟（暂停不走），连续 3 次重规划仍不动就把状态写 blocked。
 * 进程崩溃重启只从世界状态与目标槽重建；计划序号从导航表里的旧序号继续加。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/adg.h>
#include <kitchenos/child.h>
#include <kitchenos/kitchen.h>
#include <kitchenos/log.h>
#include <kitchenos/navtab.h>
#include <kitchenos/pibt.h>
#include <kitchenos/shm.h>
#include <kitchenos/stand.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NAVD_LOOP_MS 10
#define NAVD_STUCK_MS 2000
#define NAVD_STUCK_LIMIT 3
#define NAVD_ARRIVE_EPS 0.3f
#define NAVD_MOVE_EPS 0.02f
#define NAVD_PLAN_H KOS_NAV_MAX_PLAN
#define NAVD_ATTACH_LOG_MS 2000

typedef struct {
    int chef_id;
    kos_nav_target target;
    int have_target_state;
    int station_index;
    int has_target;
    int pinned;
    int goal_ok;
    int actual_cell;
    float x;
    float z;
    int64_t game_ms;
    uint16_t dist[KOS_KITCHEN_MAX_CELLS];
    int goal_cell;

    int origin_cell;
    int plan[NAVD_PLAN_H];
    int plan_len;
    int executed;
    int released;

    int stuck_count;
    uint32_t replans;
    int64_t last_progress_ms;
    int64_t target_step;
    float last_x;
    float last_z;
    int blocked_latched;
    uint32_t blocked_seq;

    uint32_t plan_seq; /* 上一次写进计划槽的序号 */
    int have_written;
    kos_nav_plan last_plan;
} nav_agent;

typedef struct {
    kos_child_runtime rt;
    kos_kitchen kitchen;
    char kitchen_path[KOS_DAEMON_PATH_MAX];
    kos_pibt pibt;
    kos_navtab *tab;
    uint64_t last_world_seq;
    int have_world;
    int64_t next_attach_log_ms;
    int64_t step_counter;
    nav_agent agents[KOS_SHM_MAX_CHEFS];
    int chef_count;
} navd;

static int player_at_goal(navd *n, const nav_agent *agent);

static int64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if (cap == 0) {
        return;
    }
    if (src != NULL) {
        while (i + 1 < cap && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void error_text(const kos_error *err, char *buf, size_t len)
{
    kos_error_format(err, buf, len);
}

static int resolve_kitchen_path(navd *n)
{
    if (n->rt.opts.kitchen_path[0] != '\0') {
        copy_str(n->kitchen_path, sizeof n->kitchen_path, n->rt.opts.kitchen_path);
        return 0;
    }
    if (kos_child_default_config_path("kitchen.example.json", n->kitchen_path,
                                      sizeof n->kitchen_path) != 0) {
        kos_log_error(n->rt.log, "kitchen_missing",
                      "no --kitchen and no config/kitchen.example.json");
        return -1;
    }
    return 0;
}

static void reset_plans(navd *n)
{
    int i;

    for (i = 0; i < n->chef_count; i++) {
        nav_agent *agent = &n->agents[i];

        agent->origin_cell = agent->actual_cell;
        agent->plan_len = 0;
        agent->executed = 0;
        agent->released = 0;
    }
}


static int in_stands(const kos_kitchen *kitchen, int station_index, int cell)
{
    const kos_station_stands *stands = kos_stand_cells(kitchen, station_index);
    int i;

    if (stands == NULL) {
        return 0;
    }
    for (i = 0; i < stands->count; i++) {
        if (stands->cells[i].cell == cell) {
            return 1;
        }
    }
    return 0;
}

static int recompute_goal(navd *n, nav_agent *agent, const uint8_t *blocked)
{
    const kos_station_stands *stands;
    int best = -1;
    uint16_t best_dist = KOS_STAND_UNREACHABLE;
    int i;

    if (agent->station_index < 0 || agent->station_index >= n->kitchen.station_count) {
        return -1;
    }
    if (in_stands(&n->kitchen, agent->station_index, agent->actual_cell)) {
        agent->goal_cell = agent->actual_cell;
        return 0;
    }
    stands = kos_stand_cells(&n->kitchen, agent->station_index);
    if (stands != NULL) {
        for (i = 0; i < stands->count; i++) {
            int cell = stands->cells[i].cell;
            uint16_t dist;

            if (blocked != NULL && blocked[cell]) {
                continue; /* 站位格被 pinned 的人占着：换下一个 */
            }
            dist = kos_stand_distance(&n->kitchen, agent->station_index, cell);
            if (dist == KOS_STAND_UNREACHABLE) {
                continue;
            }
            if (best < 0 || dist < best_dist ||
                (dist == best_dist && cell < best)) {
                best = cell;
                best_dist = dist;
            }
        }
    }
    if (best < 0) {
        best = kos_stand_best_cell(&n->kitchen, agent->station_index,
                                   agent->actual_cell);
    }
    if (best < 0) {
        return -1;
    }
    agent->goal_cell = best;
    return 0;
}

/*
 * 每个厨师的距离场按"固定障碍"重算：pinned 和已到站的厨师占着的格不可通过。
 * 只用 stand 的静态距离场时，最近邻被挡会在两个格之间来回走（随机用例踩到过）。
 */
static void prepare_distance(navd *n, nav_agent *agent, const uint8_t *blocked)
{
    (void)kos_pibt_distance_blocked(&n->pibt, agent->goal_cell, blocked, agent->dist,
                                    KOS_KITCHEN_MAX_CELLS);
}

static const uint16_t *agent_distance(nav_agent *agent)
{
    return agent->dist;
}

static void update_executed(navd *n, nav_agent *agent)
{
    while (agent->executed < agent->plan_len) {
        int cell = agent->plan[agent->executed];
        float dx = n->kitchen.cells[cell].x - agent->x;
        float dz = n->kitchen.cells[cell].z - agent->z;

        if (dx * dx + dz * dz > NAVD_ARRIVE_EPS * NAVD_ARRIVE_EPS) {
            break;
        }
        agent->executed++;
    }
}

/* 把已经走过的前缀丢掉，让"计划从下一格开始"的不变式继续保持。 */
static void compact_plan(navd *n, nav_agent *agent)
{
    int shift;

    if (agent->executed <= 0) {
        return;
    }
    shift = agent->executed;
    agent->origin_cell = agent->plan[shift - 1];
    if (shift < agent->plan_len) {
        memmove(agent->plan, agent->plan + shift,
                sizeof agent->plan[0] * (size_t)(agent->plan_len - shift));
        agent->plan_len -= shift;
    } else {
        agent->plan_len = 0;
    }
    agent->released = agent->released > shift ? agent->released - shift : 0;
    agent->executed = 0;
    (void)n;
}

static int plan_extension_round(navd *n)
{
    kos_pibt_agent pibt_agents[KOS_PIBT_MAX_AGENTS];
    int tails[KOS_PIBT_MAX_AGENTS];
    int next_cells[KOS_PIBT_MAX_AGENTS];
    int active[KOS_PIBT_MAX_AGENTS];
    int count = 0;
    int appended = 0;
    int i;

    for (i = 0; i < n->chef_count; i++) {
        nav_agent *agent = &n->agents[i];

        if (agent->plan_len >= NAVD_PLAN_H) {
            continue;
        }
        active[count] = i;
        tails[count] = agent->plan_len > 0 ? agent->plan[agent->plan_len - 1]
                                          : agent->actual_cell;
        count++;
    }
    if (count == 0) {
        return 0;
    }
    memset(pibt_agents, 0, sizeof pibt_agents);
    for (i = 0; i < count; i++) {
        nav_agent *agent = &n->agents[active[i]];

        pibt_agents[i].chef_id = agent->chef_id;
        pibt_agents[i].cell = tails[i];
        pibt_agents[i].goal = agent->goal_cell;
        pibt_agents[i].priority =
            agent->has_target ? (int)(n->step_counter - agent->target_step) +
                                    (int)agent->target.urgency
                              : 0;
        pibt_agents[i].pinned = agent->pinned;
        pibt_agents[i].goal_static = agent->has_target;
        pibt_agents[i].distance = agent_distance(agent);
    }
    if (kos_pibt_step(&n->pibt, pibt_agents, count, next_cells) != KOS_OK) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        nav_agent *agent = &n->agents[active[i]];

        if (next_cells[i] == tails[i]) {
            continue;
        }
        agent->plan[agent->plan_len++] = next_cells[i];
        appended = 1;
    }
    if (appended) {
        n->step_counter++;
    }
    return appended;
}

static void build_plans(navd *n)
{
    uint8_t blocked[KOS_KITCHEN_MAX_CELLS];
    int i;
    int round;

    memset(blocked, 0, sizeof blocked);
    for (i = 0; i < n->chef_count; i++) {
        nav_agent *agent = &n->agents[i];

        if (agent->pinned ||
            (agent->has_target && agent->station_index >= 0 &&
             player_at_goal(n, agent))) {
            blocked[agent->actual_cell] = 1;
        }
    }
    for (i = 0; i < n->chef_count; i++) {
        nav_agent *agent = &n->agents[i];

        agent->origin_cell = agent->actual_cell;
        agent->plan_len = 0;
        agent->executed = 0;
        agent->released = 0;
        if (agent->blocked_latched || (agent->has_target && agent->pinned)) {
            continue;
        }
        agent->goal_ok = 1;
        if (agent->has_target && recompute_goal(n, agent, blocked) != 0) {
            agent->goal_ok = 0;
            continue; /* 计划槽在 plan_and_publish 里写 blocked */
        }
        if (player_at_goal(n, agent)) {
            /* 到站优先清零；它同时是上面的固定障碍。 */
            agent->target_step = n->step_counter;
        }
        prepare_distance(n, agent, blocked);
    }
    for (round = 0; round < NAVD_PLAN_H; round++) {
        if (!plan_extension_round(n)) {
            break;
        }
    }
}

static void release_plans(navd *n)
{
    const int *sequences[KOS_ADG_MAX_CHEFS];
    int lengths[KOS_ADG_MAX_CHEFS];
    int actual_cells[KOS_ADG_MAX_CHEFS];
    int executed[KOS_ADG_MAX_CHEFS];
    int seq_cells[KOS_ADG_MAX_CHEFS][KOS_ADG_MAX_STEPS];
    kos_adg graph;
    int i;

    memset(&graph, 0, sizeof graph);
    for (i = 0; i < n->chef_count; i++) {
        nav_agent *agent = &n->agents[i];
        int j;

        actual_cells[i] = agent->actual_cell;
        executed[i] = 1; /* 序列第 0 项就是出发格 */
        seq_cells[i][0] = agent->origin_cell;
        for (j = 0; j < agent->plan_len && j + 1 < KOS_ADG_MAX_STEPS; j++) {
            seq_cells[i][j + 1] = agent->plan[j];
        }
        lengths[i] = agent->plan_len + 1;
        sequences[i] = seq_cells[i];
    }
    if (kos_adg_build(&graph, sequences, lengths, n->chef_count) != KOS_OK) {
        return;
    }
    for (i = 0; i < n->chef_count; i++) {
        nav_agent *agent = &n->agents[i];
        int count = 0;

        if (agent->blocked_latched || agent->pinned) {
            agent->released = 0;
            continue;
        }
        if (kos_adg_releasable(&graph, i, executed, actual_cells, &count) == KOS_OK) {
            if (count > agent->plan_len) {
                count = agent->plan_len;
            }
            agent->released = count;
        }
    }
}

static int player_at_goal(navd *n, const nav_agent *agent)
{
    int cell;
    float dx;
    float dz;

    if (!agent->has_target || agent->station_index < 0) {
        return 0;
    }
    cell = agent->actual_cell;
    if (!in_stands(&n->kitchen, agent->station_index, cell)) {
        return 0;
    }
    dx = n->kitchen.cells[cell].x - agent->x;
    dz = n->kitchen.cells[cell].z - agent->z;
    return dx * dx + dz * dz <= NAVD_ARRIVE_EPS * NAVD_ARRIVE_EPS;
}

static int plan_content_equal(const kos_nav_plan *a, const kos_nav_plan *b)
{
    int i;

    if (a->status != b->status || a->target_seq != b->target_seq ||
        a->waypoint_count != b->waypoint_count) {
        return 0;
    }
    for (i = 0; i < a->waypoint_count; i++) {
        if (a->waypoints[i].x != b->waypoints[i].x ||
            a->waypoints[i].y != b->waypoints[i].y) {
            return 0;
        }
    }
    return 1;
}

static void write_plan(navd *n, nav_agent *agent, int status)
{
    kos_nav_plan plan;
    int i;
    int count = 0;

    memset(&plan, 0, sizeof plan);
    plan.status = status;
    plan.target_seq = agent->target.target_seq;
    plan.replans = agent->replans;
    plan.stuck_count = (uint32_t)agent->stuck_count;
    plan.game_time_ms = (uint64_t)(agent->game_ms > 0 ? agent->game_ms : 0);
    if (status == KOS_NAV_MOVING || status == KOS_NAV_IDLE) {
        count = agent->released;
        if (count > NAVD_PLAN_H) {
            count = NAVD_PLAN_H;
        }
        if (count > agent->plan_len) {
            count = agent->plan_len;
        }
        plan.waypoint_count = count;
        for (i = 0; i < count; i++) {
            plan.waypoints[i].x = n->kitchen.cells[agent->plan[i]].x;
            plan.waypoints[i].y = n->kitchen.cells[agent->plan[i]].z;
        }
    }
    plan.plan_seq = agent->plan_seq;
    if (!agent->have_written || !plan_content_equal(&plan, &agent->last_plan)) {
        plan.plan_seq = agent->plan_seq + 1;
        agent->plan_seq = plan.plan_seq;
    }
    agent->last_plan = plan;
    agent->have_written = 1;
    (void)kos_navtab_write_plan(n->tab, agent->chef_id, &plan);
}

static void apply_target(navd *n, int index, const kos_nav_target *target)
{
    nav_agent *agent = &n->agents[index];
    int identity_changed;
    int pinned_changed;

    identity_changed =
        !agent->have_target_state ||
        target->target_seq != agent->target.target_seq ||
        target->has_target != agent->target.has_target ||
        strcmp(target->station_id, agent->target.station_id) != 0;
    pinned_changed = target->pinned != agent->target.pinned;
    agent->target = *target;
    agent->target.station_id[KOS_SHM_NAME_MAX - 1] = '\0';
    agent->have_target_state = 1;
    agent->has_target = target->has_target != 0;
    agent->pinned = target->pinned != 0;
    if (!identity_changed && !pinned_changed) {
        return;
    }
    agent->station_index =
        agent->has_target ? kos_kitchen_station_index(&n->kitchen, target->station_id) : -1;
    agent->goal_cell = agent->actual_cell;
    agent->goal_ok = 1;
    if (!identity_changed && pinned_changed) {
        /* pinned 只影响"要不要让路"：不重置卡住计数，也不解开 blocked。 */
        reset_plans(n);
        return;
    }
    if (agent->blocked_latched && target->target_seq == agent->blocked_seq &&
        !agent->has_target) {
        /* Worker 读到 blocked 后清空目标槽不会改变 target_seq；保持 blocked，
         * 等下一道工序把序号加一，否则 Worker 可能来不及看到 blocked。 */
        reset_plans(n);
        return;
    }
    agent->target_step = n->step_counter;
    agent->stuck_count = 0;
    agent->replans = 0;
    agent->blocked_latched = 0;
    agent->blocked_seq = 0;
    agent->last_progress_ms = agent->game_ms;
    agent->last_x = agent->x;
    agent->last_z = agent->z;
    reset_plans(n);
    if (agent->has_target && agent->station_index < 0) {
        kos_log_warn(n->rt.log, "nav_target_invalid", "chef=%d station=%s",
                     agent->chef_id, agent->target.station_id);
    } else if (agent->has_target) {
        kos_log_info(n->rt.log, "nav_target",
                     "chef=%d station=%s seq=%u urgency=%u",
                     agent->chef_id, agent->target.station_id, target->target_seq,
                     target->urgency);
    }
}

static int update_agents(navd *n, const kos_world_state *world)
{
    uint32_t i;
    int chef;
    int missing = 0;

    for (chef = 0; chef < n->chef_count; chef++) {
        nav_agent *agent = &n->agents[chef];
        int found = -1;

        for (i = 0; i < world->chef_count; i++) {
            if (world->chefs[i].id == chef) {
                found = (int)i;
                break;
            }
        }
        if (found >= 0) {
            agent->x = world->chefs[found].x;
            agent->z = world->chefs[found].y;
            agent->actual_cell =
                kos_kitchen_nearest_cell(&n->kitchen, agent->x, agent->z);
        } else {
            agent->actual_cell = -1; /* 还没上报：这一版不规划，免得两个"0"撞格 */
            missing = 1;
        }
        agent->game_ms = world->game_time_ms;
    }
    return missing;
}

static void detect_stuck(navd *n)
{
    int i;

    for (i = 0; i < n->chef_count; i++) {
        nav_agent *agent = &n->agents[i];
        int moved = 0;
        float dx;
        float dz;
        int had_released;

        if (!agent->have_target_state) {
            continue;
        }
        dx = agent->x - agent->last_x;
        dz = agent->z - agent->last_z;
        if (dx * dx + dz * dz > NAVD_MOVE_EPS * NAVD_MOVE_EPS) {
            moved = 1;
        }
        update_executed(n, agent);
        compact_plan(n, agent);
        had_released = agent->released > 0;
        if (moved) {
            agent->last_progress_ms = agent->game_ms;
            agent->stuck_count = 0;
            agent->last_x = agent->x;
            agent->last_z = agent->z;
        }
        if (!agent->has_target || agent->blocked_latched || agent->pinned) {
            continue;
        }
        if (!had_released || player_at_goal(n, agent)) {
            continue;
        }
        if (agent->game_ms - agent->last_progress_ms < NAVD_STUCK_MS) {
            continue;
        }
        agent->last_progress_ms = agent->game_ms;
        agent->last_x = agent->x;
        agent->last_z = agent->z;
        agent->stuck_count++;
        agent->replans++;
        reset_plans(n);
        kos_log_warn(n->rt.log, "nav_replan",
                     "chef=%d reason=stuck replans=%u stuck=%d target=%s",
                     agent->chef_id, agent->replans, agent->stuck_count,
                     agent->target.station_id);
        if (agent->stuck_count >= NAVD_STUCK_LIMIT) {
            agent->blocked_latched = 1;
            agent->blocked_seq = agent->target.target_seq;
            kos_log_warn(n->rt.log, "nav_stuck",
                         "chef=%d replans=%u stuck=%d target=%s",
                         agent->chef_id, agent->replans, agent->stuck_count,
                         agent->target.station_id);
        }
    }
}

static void plan_and_publish(navd *n)
{
    int i;

    for (i = 0; i < n->chef_count; i++) {
        nav_agent *agent = &n->agents[i];
        int status = KOS_NAV_IDLE;
        int unreachable = 0;

        if (!agent->have_target_state) {
            continue;
        }
        if (agent->has_target) {
            if (agent->station_index < 0 || !agent->goal_ok) {
                unreachable = 1;
            }
        }
        if (agent->blocked_latched) {
            status = KOS_NAV_BLOCKED;
        } else if (unreachable) {
            status = KOS_NAV_BLOCKED;
            kos_log_warn(n->rt.log, "nav_unreachable", "chef=%d station=%s",
                         agent->chef_id, agent->target.station_id);
        } else if (player_at_goal(n, agent)) {
            status = KOS_NAV_ARRIVED;
            agent->plan_len = 0;
            agent->released = 0;
        } else if (agent->stuck_count > 0 && agent->released == 0) {
            status = KOS_NAV_STUCK;
        } else if (agent->released > 0) {
            status = KOS_NAV_MOVING;
        } else if (agent->plan_len > 0) {
            status = KOS_NAV_MOVING;
        } else {
            status = agent->has_target ? KOS_NAV_MOVING : KOS_NAV_IDLE;
        }
        write_plan(n, agent, status);
    }
}

static void tick_planning(navd *n, const kos_world_state *world)
{
    int i;
    int duplicate = 0;

    if (n->chef_count <= 0) {
        return;
    }
    for (i = 0; i < n->chef_count; i++) {
        kos_nav_target target;

        if (kos_navtab_read_target(n->tab, n->agents[i].chef_id, &target) != KOS_OK) {
            memset(&target, 0, sizeof target);
        }
        apply_target(n, i, &target);
    }
    if (update_agents(n, world)) {
        return;
    }
    for (i = 0; i < n->chef_count; i++) {
        int j;

        for (j = 0; j < i; j++) {
            if (n->agents[i].actual_cell == n->agents[j].actual_cell) {
                duplicate = 1;
            }
        }
    }
    if (duplicate) {
        kos_log_warn(n->rt.log, "nav_state_conflict", "reason=two-chefs-same-cell");
    } else {
        detect_stuck(n);
        build_plans(n);
        release_plans(n);
        plan_and_publish(n);
    }
}

static int navd_main(navd *n, const kos_child_opts *opts)
{
    int i;

    memset(n, 0, sizeof *n);
    n->rt.opts = *opts;
    n->chef_count = opts->chefs > 0 ? opts->chefs : 1;
    if (n->chef_count > KOS_SHM_MAX_CHEFS) {
        n->chef_count = KOS_SHM_MAX_CHEFS;
    }
    if (kos_child_setup(&n->rt, opts) != 0) {
        return 1;
    }
    if (resolve_kitchen_path(n) != 0) {
        kos_child_teardown(&n->rt);
        return 1;
    }
    {
        kos_error err;
        char text[256];

        kos_error_clear(&err);
        if (kos_kitchen_load(n->kitchen_path, &n->kitchen, &err) != KOS_OK) {
            error_text(&err, text, sizeof text);
            kos_log_error(n->rt.log, "kitchen_load_failed", "path=%s error=%s",
                          n->kitchen_path, text);
            kos_child_teardown(&n->rt);
            return 1;
        }
        if (kos_pibt_init(&n->pibt, &n->kitchen) != KOS_OK) {
            kos_log_error(n->rt.log, "pibt_init_failed", "path=%s", n->kitchen_path);
            kos_child_teardown(&n->rt);
            return 1;
        }
    }
    for (i = 0; i < n->chef_count; i++) {
        nav_agent *agent = &n->agents[i];

        agent->chef_id = i;
        agent->station_index = -1;
        agent->goal_cell = 0;
        agent->actual_cell = 0;
        agent->plan_seq = 0;
    }
    kos_log_info(n->rt.log, "nav_start", "kitchen=%s cells=%d stations=%d chefs=%d H=%d",
                 n->kitchen_path, n->kitchen.cell_count, n->kitchen.station_count,
                 n->chef_count, NAVD_PLAN_H);

    while (!kos_child_poll(&n->rt)) {
        kos_navtab_opts tab_opts;
        kos_error err;
        kos_world_state world;
        int64_t now = now_ms();

        if (n->tab == NULL) {
            char text[256];

            kos_navtab_opts_init(&tab_opts);
            if (n->rt.opts.nav_name[0] != '\0') {
                tab_opts.object_name = n->rt.opts.nav_name;
            }
            if (n->rt.opts.nav_sem_name[0] != '\0') {
                tab_opts.sem_name = n->rt.opts.nav_sem_name;
            }
            kos_error_clear(&err);
            if (kos_navtab_open(&tab_opts, &n->tab, &err) != KOS_OK) {
                if (now >= n->next_attach_log_ms) {
                    error_text(&err, text, sizeof text);
                    kos_log_info(n->rt.log, "nav_wait", "error=%s", text);
                    n->next_attach_log_ms = now + NAVD_ATTACH_LOG_MS;
                }
                kos_child_sleep(&n->rt, NAVD_LOOP_MS);
                continue;
            }
            for (i = 0; i < n->chef_count; i++) {
                kos_nav_plan previous;

                if (kos_navtab_read_plan(n->tab, i, &previous) == KOS_OK) {
                    n->agents[i].plan_seq = previous.plan_seq;
                }
            }
            kos_log_info(n->rt.log, "nav_attached", "chefs=%d", n->chef_count);
        }
        if (n->rt.shm != NULL &&
            kos_shm_world_read(n->rt.shm, &world, KOS_SHM_READ_ATTEMPTS, NULL) == KOS_OK) {
            if (!n->have_world || world.write_seq != n->last_world_seq) {
                n->have_world = 1;
                n->last_world_seq = world.write_seq;
                tick_planning(n, &world);
            }
        }
        kos_child_sleep(&n->rt, NAVD_LOOP_MS);
    }
    if (n->tab != NULL) {
        kos_navtab_close(n->tab);
        n->tab = NULL;
    }
    kos_child_teardown(&n->rt);
    return 0;
}

int main(int argc, char **argv)
{
    kos_child_opts opts;
    navd *n;
    int parsed;
    int rc;

    kos_child_opts_init(&opts, KOS_CHILD_NAVD);
    parsed = kos_child_parse_args(&opts, argc, argv);
    if (parsed == KOS_CHILD_ARG_HELP) {
        kos_child_print_usage(stdout, KOS_CHILD_NAVD);
        return 0;
    }
    if (parsed == KOS_CHILD_ARG_VERSION) {
        printf("kitchenos-navd 0.1.0\n");
        return 0;
    }
    if (parsed != KOS_CHILD_ARG_OK) {
        kos_child_print_usage(stderr, KOS_CHILD_NAVD);
        return 2;
    }
    n = calloc(1, sizeof *n);
    if (n == NULL) {
        return 1;
    }
    rc = navd_main(n, &opts);
    free(n);
    return rc;
}
