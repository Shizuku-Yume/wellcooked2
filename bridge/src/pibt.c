/*
 * pibt.c -- 一步 PIBT。
 *
 * 候选顺序与递归顺序都是确定的：优先级（大者先）、chef_id（小者先）、候选格按
 * (距离, 下标)。这样同一输入两次规划得到同一串动作，测试能拿它当复现基准。
 *
 * "被推"的处理：高优先级厨师要进低优先级所在格时，递归让对方先走。对方如果
 * 已经到目标（或根本没任务），当前格在候选里距离为 0，会一直选原地——那样谁都
 * 过不去。因此递归时带 pushed 标记，跳过"原地"候选，逼它找相邻空格；找不到才
 * 回退，让对方换候选。pinned 的厨师不走这条路径。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <wellcooked/pibt.h>

#include <string.h>

#include <wellcooked/nav.h>

typedef struct {
    kos_pibt *pibt;
    const kos_pibt_agent *agents;
    int count;
    int decided[KOS_PIBT_MAX_AGENTS];
    int next[KOS_PIBT_MAX_AGENTS];
    int visiting[KOS_PIBT_MAX_AGENTS];
    int occupied[KOS_KITCHEN_MAX_CELLS]; /* 当前格 → 厨师下标，-1 = 空 */
    int owner[KOS_KITCHEN_MAX_CELLS];    /* 本步已决定的目的格 → 厨师下标 */
} pibt_run;

static void sort_candidates(const kos_pibt_agent *agent, const int *cells, int count,
                            int *order)
{
    int i;
    int j;

    for (i = 0; i < count; i++) {
        order[i] = i;
    }
    for (i = 1; i < count; i++) {
        int value = order[i];
        uint16_t value_dist = agent->distance[cells[value]];
        int value_cell = cells[value];

        j = i - 1;
        while (j >= 0) {
            uint16_t other_dist = agent->distance[cells[order[j]]];
            int other_cell = cells[order[j]];

            if (other_dist < value_dist ||
                (other_dist == value_dist && other_cell < value_cell)) {
                break;
            }
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = value;
    }
}

static int effective_priority(const kos_pibt_agent *agent)
{
    /* 已经在目标格的人优先级清零；被推开后按原公式恢复，否则两个都到过
     * 目标的人会在单宽过道里互不相让。 */
    if (agent->cell == agent->goal) {
        return 0;
    }
    return agent->priority;
}

static int agent_order_before(const kos_pibt_agent *a, const kos_pibt_agent *b)
{
    int pa = effective_priority(a);
    int pb = effective_priority(b);

    if (pa != pb) {
        return pa > pb;
    }
    return a->chef_id < b->chef_id;
}

static int decide(pibt_run *run, int i, int priority, int pushed, int depth)
{
    const kos_pibt_agent *agent = &run->agents[i];

    if (run->decided[i]) {
        return 1;
    }
    if (run->visiting[i]) {
        return 0; /* 递归成环：这一支不成立，让上层换候选 */
    }
    if (depth > run->count + 2) {
        return 0;
    }
    if (agent->pinned || (agent->goal_static && agent->cell == agent->goal)) {
        run->decided[i] = 1;
        run->next[i] = agent->cell;
        run->owner[agent->cell] = i;
        return 1;
    }
    run->visiting[i] = 1;
    {
        int candidates[5];
        int order[5];
        int cand_count = 0;
        int n;
        int ci;

        /* 候选包含原地，按距离排序：动态距离场已经把 pinned/到站的人占的格
         * 当成障碍，所以"绕路"的邻格距离会小于原地，先被尝试；反过来，前方
         * 只是暂时推不动的人时，原地会排在前面的邻格之前，不会无故后退。 */
        candidates[cand_count++] = agent->cell;
        for (n = 0; n < run->pibt->degree[agent->cell]; n++) {
            candidates[cand_count++] = run->pibt->neighbor[agent->cell][n];
        }
        sort_candidates(agent, candidates, cand_count, order);

        for (ci = 0; ci < cand_count; ci++) {
            int candidate = candidates[order[ci]];
            int occupant;

            if (candidate == agent->cell && pushed) {
                continue; /* 被推的人不能靠"原地"把高优先级挡回去 */
            }
            if (run->owner[candidate] >= 0) {
                continue; /* 本步已经有人要去 */
            }
            occupant = run->occupied[candidate];
            if (occupant >= 0 && occupant != i) {
                const kos_pibt_agent *other = &run->agents[occupant];

                if (other->pinned ||
                    (other->goal_static && other->cell == other->goal)) {
                    continue; /* 已到站的目标厨师当固定障碍 */
                }
                if (run->decided[occupant]) {
                    if (run->next[occupant] == candidate) {
                        continue; /* 对方原地不动 */
                    }
                    /* 对方已决定离开；但不能是和我对换。 */
                    if (run->next[occupant] == agent->cell) {
                        continue;
                    }
                } else {
                    int inherits = priority;
                    int ok;

                    if (run->visiting[occupant]) {
                        continue;
                    }
                    ok = decide(run, occupant, inherits, 1, depth + 1);
                    if (!ok) {
                        continue;
                    }
                    if (run->next[occupant] == candidate ||
                        run->next[occupant] == agent->cell) {
                        continue; /* 对方占着/对换，换下一个候选 */
                    }
                }
            }
            run->decided[i] = 1;
            run->next[i] = candidate;
            run->owner[candidate] = i;
            run->visiting[i] = 0;
            return 1;
        }
    }
    run->visiting[i] = 0;
    return 0;
}

int kos_pibt_init(kos_pibt *pibt, const kos_kitchen *kitchen)
{
    int i;
    int j;

    if (pibt == NULL || kitchen == NULL || kitchen->cell_count <= 0 ||
        kitchen->cell_count > KOS_KITCHEN_MAX_CELLS) {
        return KOS_ERR_INVAL;
    }
    memset(pibt, 0, sizeof *pibt);
    pibt->kitchen = kitchen;
    pibt->cell_count = kitchen->cell_count;
    for (i = 0; i < kitchen->cell_count; i++) {
        for (j = 0; j < kitchen->cell_count; j++) {
            if (i == j || !kos_nav_adjacent(kitchen, i, j)) {
                continue;
            }
            if (pibt->degree[i] < 4) {
                pibt->neighbor[i][pibt->degree[i]++] = j;
            }
        }
    }
    return KOS_OK;
}

int kos_pibt_step(kos_pibt *pibt, const kos_pibt_agent *agents, int count,
                  int *next_cells)
{
    pibt_run run;
    int order[KOS_PIBT_MAX_AGENTS];
    int i;
    int j;

    if (pibt == NULL || agents == NULL || next_cells == NULL || count <= 0 ||
        count > KOS_PIBT_MAX_AGENTS) {
        return KOS_ERR_INVAL;
    }
    memset(&run, 0, sizeof run);
    run.pibt = pibt;
    run.agents = agents;
    run.count = count;
    for (i = 0; i < KOS_KITCHEN_MAX_CELLS; i++) {
        run.occupied[i] = -1;
        run.owner[i] = -1;
    }
    for (i = 0; i < count; i++) {
        if (agents[i].cell < 0 || agents[i].cell >= pibt->cell_count ||
            agents[i].goal < 0 || agents[i].goal >= pibt->cell_count ||
            agents[i].distance == NULL) {
            return KOS_ERR_INVAL;
        }
        if (run.occupied[agents[i].cell] >= 0) {
            return KOS_ERR_INVAL; /* 两个厨师同格：调用方没同步好 */
        }
        run.occupied[agents[i].cell] = i;
        order[i] = i;
        next_cells[i] = agents[i].cell;
    }
    for (i = 1; i < count; i++) {
        int value = order[i];

        j = i - 1;
        while (j >= 0 && agent_order_before(&agents[value], &agents[order[j]])) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = value;
    }
    for (i = 0; i < count; i++) {
        int index = order[i];

        if (!run.decided[index]) {
            if (!decide(&run, index, effective_priority(&agents[index]), 0, 0)) {
                /* 理论上不发生；保底原地，测试会抓出任何冲突。 */
                if (run.owner[agents[index].cell] >= 0 &&
                    run.owner[agents[index].cell] != index) {
                    return KOS_ERR_BUSY;
                }
                run.decided[index] = 1;
                run.next[index] = agents[index].cell;
                run.owner[agents[index].cell] = index;
            }
        }
    }
    for (i = 0; i < count; i++) {
        next_cells[i] = run.next[i];
    }
    return KOS_OK;
}

static int distance_field(const kos_pibt *pibt, int goal, const uint8_t *blocked,
                          uint16_t *distance, int cap)
{
    int queue[KOS_KITCHEN_MAX_CELLS];
    int head = 0;
    int tail = 0;
    int i;

    if (pibt == NULL || distance == NULL || cap < pibt->cell_count || goal < 0 ||
        goal >= pibt->cell_count) {
        return KOS_ERR_INVAL;
    }
    for (i = 0; i < pibt->cell_count; i++) {
        distance[i] = KOS_STAND_UNREACHABLE;
    }
    distance[goal] = 0;
    queue[tail++] = goal;
    while (head < tail) {
        int current = queue[head++];
        int n;

        for (n = 0; n < pibt->degree[current]; n++) {
            int other = pibt->neighbor[current][n];

            if (blocked != NULL && other != goal && blocked[other]) {
                continue;
            }
            if (distance[other] == KOS_STAND_UNREACHABLE) {
                distance[other] = (uint16_t)(distance[current] + 1);
                queue[tail++] = other;
            }
        }
    }
    return KOS_OK;
}

int kos_pibt_distance(const kos_pibt *pibt, int goal, uint16_t *distance, int cap)
{
    return distance_field(pibt, goal, NULL, distance, cap);
}

int kos_pibt_distance_blocked(const kos_pibt *pibt, int goal, const uint8_t *blocked,
                              uint16_t *distance, int cap)
{
    return distance_field(pibt, goal, blocked, distance, cap);
}
