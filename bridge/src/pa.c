/*
 * pa.c -- 优先级时空 A*（对照基线）。
 *
 * 时间展开图上的状态是 (格, 时间)，边是四邻域或原地等待；每个厨师按优先级
 * 依次搜索，后来的搜索避开前面已预约的 (时间, 格) 与对换。启发式是忽略其他
 * 厨师与固定障碍的 BFS 距离，因此可采纳；平局按 (f, h, 格, 时间) 固定顺序，
 * 同一输入两次规划得到同一条路径。
 *
 * 规划顺序是"高优先级先走"，与 PIBT 的优先级排序一致。还没轮到的人不预约
 * 自己的格：先规划的人可以规划经过它当前所在的格，后来者再按已预约的时空格
 * 与对换找路。终止状态要求这一格在剩余时间里没人用（人都停在计划终点），
 * 所以路径在时间上可安全执行。PIBT 靠推人让出格子，这里靠后来者改道；它是
 * 对照基线，不承担 PIBT 的可达性保证。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/pa.h>

#include <string.h>

#include <kitchenos/nav.h>

#define PA_UNREACHABLE UINT16_MAX
#define PA_STATIC 0xFFu

static int state_index(const kos_pa *pa, int time, int cell)
{
    return time * pa->cell_count + cell;
}

static int heuristic_cost(const kos_pa *pa, int cell)
{
    uint16_t value = pa->heuristic[cell];

    /* 不可达用比任何真实距离都大的常数，免得它被排到前面。 */
    return value == PA_UNREACHABLE ? pa->cell_count : (int)value;
}

static int heap_less(const kos_pa *pa, int state_a, int f_a, int state_b, int f_b)
{
    int cell_a = state_a % pa->cell_count;
    int cell_b = state_b % pa->cell_count;
    int h_a;
    int h_b;

    if (f_a != f_b) {
        return f_a < f_b;
    }
    h_a = heuristic_cost(pa, cell_a);
    h_b = heuristic_cost(pa, cell_b);
    if (h_a != h_b) {
        return h_a < h_b;
    }
    if (cell_a != cell_b) {
        return cell_a < cell_b;
    }
    return state_a < state_b;
}

static void heap_push(kos_pa *pa, int *count, int state, int f)
{
    int i = *count;

    pa->heap_state[i] = state;
    pa->heap_f[i] = f;
    (*count)++;
    while (i > 0) {
        int parent = (i - 1) / 2;

        if (heap_less(pa, pa->heap_state[parent], pa->heap_f[parent],
                      pa->heap_state[i], pa->heap_f[i])) {
            break;
        }
        {
            int state_swap = pa->heap_state[parent];
            int f_swap = pa->heap_f[parent];

            pa->heap_state[parent] = pa->heap_state[i];
            pa->heap_f[parent] = pa->heap_f[i];
            pa->heap_state[i] = state_swap;
            pa->heap_f[i] = f_swap;
        }
        i = parent;
    }
}

static int heap_pop(kos_pa *pa, int *count, int *f_out)
{
    int top = pa->heap_state[0];
    int last_state;
    int last_f;
    int i = 0;

    *f_out = pa->heap_f[0];
    (*count)--;
    if (*count == 0) {
        return top;
    }
    last_state = pa->heap_state[*count];
    last_f = pa->heap_f[*count];
    pa->heap_state[0] = last_state;
    pa->heap_f[0] = last_f;
    for (;;) {
        int left = i * 2 + 1;
        int right = left + 1;
        int best = i;

        if (left < *count &&
            heap_less(pa, pa->heap_state[left], pa->heap_f[left],
                      pa->heap_state[best], pa->heap_f[best])) {
            best = left;
        }
        if (right < *count &&
            heap_less(pa, pa->heap_state[right], pa->heap_f[right],
                      pa->heap_state[best], pa->heap_f[best])) {
            best = right;
        }
        if (best == i) {
            break;
        }
        {
            int state_swap = pa->heap_state[i];
            int f_swap = pa->heap_f[i];

            pa->heap_state[i] = pa->heap_state[best];
            pa->heap_f[i] = pa->heap_f[best];
            pa->heap_state[best] = state_swap;
            pa->heap_f[best] = f_swap;
        }
        i = best;
    }
    return top;
}

/* 忽略其他厨师与固定障碍的 BFS 距离：真实路径只会更长，所以它是可采纳的下界。 */
static void build_heuristic(kos_pa *pa, int goal)
{
    int queue[KOS_KITCHEN_MAX_CELLS];
    int head = 0;
    int tail = 0;
    int i;

    for (i = 0; i < pa->cell_count; i++) {
        pa->heuristic[i] = PA_UNREACHABLE;
    }
    pa->heuristic[goal] = 0;
    queue[tail++] = goal;
    while (head < tail) {
        int current = queue[head++];
        int n;

        for (n = 0; n < pa->degree[current]; n++) {
            int other = pa->neighbor[current][n];

            if (pa->heuristic[other] != PA_UNREACHABLE) {
                continue;
            }
            pa->heuristic[other] = (uint16_t)(pa->heuristic[current] + 1u);
            queue[tail++] = other;
        }
    }
}

/*
 * 路径在 time 走完后就停在这一格，直到重新规划。只有后续所有时刻都没人预约
 * 这一格，这个终点才安全；否则应继续搜索（等对方过去或绕开）。
 */
static int stay_free(const kos_pa *pa, int cell, int time, int horizon)
{
    int t;

    for (t = time + 1; t <= horizon; t++) {
        if (pa->reserved[t][cell] != 0) {
            return 0;
        }
    }
    return 1;
}

/*
 * 为一个人找最多 horizon 步的路径。到了 goal 就返回；否则搜索耗尽，返回离
 * goal 最近且能安全停下的已展开状态。
 */
static int search_agent(kos_pa *pa, const kos_pa_agent *agent, int horizon,
                        int *path, int *length)
{
    int state_count = (horizon + 1) * pa->cell_count;
    int start = state_index(pa, 0, agent->cell);
    int best_state = -1; /* 只接受"能安全停下"的状态，哪怕它离目标更远 */
    int best_cost = 0;
    int best_time = 0;
    int heap_count = 0;
    int states = state_count;

    memset(pa->seen, 0, (size_t)states);
    pa->parent[start] = -1;
    pa->seen[start] = 1;
    heap_push(pa, &heap_count, start, best_cost);

    while (heap_count > 0) {
        int f;
        int state = heap_pop(pa, &heap_count, &f);
        int time = state / pa->cell_count;
        int cell = state % pa->cell_count;
        int cost = heuristic_cost(pa, cell);
        int candidates[5];
        int candidate_count = 0;
        int n;

        (void)f;
        if (cell == agent->goal && stay_free(pa, cell, time, horizon)) {
            int current = state;
            int t = time;

            while (current >= 0 && current != start) {
                int at_time = current / pa->cell_count;

                if (at_time >= 1 && at_time <= horizon) {
                    path[at_time - 1] = current % pa->cell_count;
                }
                current = pa->parent[current];
            }
            *length = t;
            return 1;
        }
        if (stay_free(pa, cell, time, horizon) &&
            (best_state < 0 || cost < best_cost ||
             (cost == best_cost &&
              (time < best_time || (time == best_time && state < best_state))))) {
            best_cost = cost;
            best_time = time;
            best_state = state;
        }
        if (time >= horizon) {
            continue;
        }
        candidates[candidate_count++] = cell;
        for (n = 0; n < pa->degree[cell]; n++) {
            candidates[candidate_count++] = pa->neighbor[cell][n];
        }
        for (n = 0; n < candidate_count; n++) {
            int next_cell = candidates[n];
            int next_time = time + 1;
            int next_state = state_index(pa, next_time, next_cell);
            int occupant;
            int f_next;

            if (pa->seen[next_state]) {
                continue;
            }
            if (pa->reserved[next_time][next_cell] != 0) {
                continue;
            }
            /* 对换：上一刻别人在 next_cell，下一刻它要回到我现在这格。 */
            occupant = pa->reserved[time][next_cell];
            if (occupant != 0 && occupant == pa->reserved[next_time][cell]) {
                continue;
            }
            f_next = next_time + heuristic_cost(pa, next_cell);
            pa->parent[next_state] = state;
            pa->seen[next_state] = 1;
            heap_push(pa, &heap_count, next_state, f_next);
        }
    }

    if (best_state < 0) {
        /* 连"往后所有时刻都空"的格都找不到：保留原地，让调用方报 BUSY，
         * 不要在这里悄悄产出一条时间上冲突的路径。 */
        *length = 0;
        return 0;
    }
    {
        int current = best_state;
        int t = best_time;

        while (current >= 0 && current != start) {
            int at_time = current / pa->cell_count;

            if (at_time >= 1 && at_time <= horizon) {
                path[at_time - 1] = current % pa->cell_count;
            }
            current = pa->parent[current];
        }
        *length = t;
    }
    return 0;
}

static int effective_priority(const kos_pa_agent *agent)
{
    if (agent->cell == agent->goal) {
        return 0;
    }
    return agent->priority;
}

static int order_before(const kos_pa_agent *a, const kos_pa_agent *b)
{
    int pa = effective_priority(a);
    int pb = effective_priority(b);

    if (pa != pb) {
        return pa > pb;
    }
    return a->chef_id < b->chef_id;
}

int kos_pa_init(kos_pa *pa, const kos_kitchen *kitchen)
{
    int i;
    int j;

    if (pa == NULL || kitchen == NULL || kitchen->cell_count <= 0 ||
        kitchen->cell_count > KOS_KITCHEN_MAX_CELLS) {
        return KOS_ERR_INVAL;
    }
    memset(pa, 0, sizeof *pa);
    pa->kitchen = kitchen;
    pa->cell_count = kitchen->cell_count;
    for (i = 0; i < kitchen->cell_count; i++) {
        for (j = 0; j < kitchen->cell_count; j++) {
            if (i == j || !kos_nav_adjacent(kitchen, i, j)) {
                continue;
            }
            if (pa->degree[i] < 4) {
                pa->neighbor[i][pa->degree[i]++] = j;
            }
        }
    }
    return KOS_OK;
}

int kos_pa_plan(kos_pa *pa, const kos_pa_agent *agents, int count, int horizon,
                const uint8_t *blocked, int *lengths,
                int paths[KOS_PA_MAX_AGENTS][KOS_PA_MAX_HORIZON])
{
    int order[KOS_PA_MAX_AGENTS];
    int fixed[KOS_PA_MAX_AGENTS];
    int i;
    int j;
    int position;

    if (pa == NULL || agents == NULL || lengths == NULL || paths == NULL ||
        count <= 0 || count > KOS_PA_MAX_AGENTS || horizon <= 0 ||
        horizon > KOS_PA_MAX_HORIZON) {
        return KOS_ERR_INVAL;
    }
    for (i = 0; i < count; i++) {
        if (agents[i].cell < 0 || agents[i].cell >= pa->cell_count ||
            agents[i].goal < 0 || agents[i].goal >= pa->cell_count) {
            return KOS_ERR_INVAL;
        }
        if (i > 0) {
            for (j = 0; j < i; j++) {
                if (agents[i].cell == agents[j].cell) {
                    return KOS_ERR_INVAL;
                }
            }
        }
        memset(paths[i], 0, sizeof paths[i]);
        lengths[i] = 0;
    }
    memset(pa->reserved, 0, sizeof pa->reserved);
    if (blocked != NULL) {
        int cell;

        for (cell = 0; cell < pa->cell_count; cell++) {
            if (!blocked[cell]) {
                continue;
            }
            for (j = 0; j <= horizon; j++) {
                pa->reserved[j][cell] = PA_STATIC;
            }
        }
    }
    for (i = 0; i < count; i++) {
        fixed[i] = agents[i].pinned ||
                   (agents[i].goal_static && agents[i].cell == agents[i].goal);
        if (!fixed[i]) {
            continue;
        }
        for (j = 0; j <= horizon; j++) {
            if (pa->reserved[j][agents[i].cell] == 0) {
                pa->reserved[j][agents[i].cell] = PA_STATIC;
            }
        }
    }

    position = 0;
    for (i = 0; i < count; i++) {
        int at = position;

        if (fixed[i]) {
            continue;
        }
        while (at > 0 && order_before(&agents[i], &agents[order[at - 1]])) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
        position++;
    }

    for (i = 0; i < position; i++) {
        int index = order[i];
        const kos_pa_agent *agent = &agents[index];
        int *path = paths[index];
        int length = 0;
        int start = agent->cell;
        int final_cell;
        int t;

        build_heuristic(pa, agent->goal);
        (void)search_agent(pa, agent, horizon, path, &length);
        if (length < 0 || length > horizon) {
            return KOS_ERR_BUSY;
        }
        lengths[index] = length;
        final_cell = length > 0 ? path[length - 1] : start;
        if (pa->reserved[0][start] != 0) {
            return KOS_ERR_BUSY;
        }
        pa->reserved[0][start] = (uint8_t)(index + 1);
        for (t = 1; t <= length; t++) {
            int cell = path[t - 1];

            if (pa->reserved[t][cell] != 0) {
                return KOS_ERR_BUSY;
            }
            pa->reserved[t][cell] = (uint8_t)(index + 1);
        }
        /* 计划走完就停在那里，后面的人不能把它挤开。 */
        for (t = length + 1; t <= horizon; t++) {
            if (pa->reserved[t][final_cell] != 0) {
                return KOS_ERR_BUSY;
            }
            pa->reserved[t][final_cell] = (uint8_t)(index + 1);
        }
    }
    return KOS_OK;
}
