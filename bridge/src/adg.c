/*
 * adg.c -- 按计划占用顺序放行路点。
 *
 * 判定只用两个事实：计划里谁先经过这个格、以及那人现在实际在哪个格。不做时间
 * 推算，所以慢的厨师不会因为"计划里该走了"就被提前放行。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/adg.h>

#include <string.h>

int kos_adg_build(kos_adg *graph, const int *const *sequences, const int *lengths,
                  int chef_count)
{
    int i;
    int j;

    if (graph == NULL || sequences == NULL || lengths == NULL || chef_count <= 0 ||
        chef_count > KOS_ADG_MAX_CHEFS) {
        return KOS_ERR_INVAL;
    }
    memset(graph, 0, sizeof *graph);
    graph->chef_count = chef_count;
    for (i = 0; i < chef_count; i++) {
        if (sequences[i] == NULL || lengths[i] < 1 ||
            lengths[i] > KOS_ADG_MAX_STEPS) {
            return KOS_ERR_RANGE;
        }
        graph->length[i] = lengths[i];
        for (j = 0; j < lengths[i]; j++) {
            graph->cells[i][j] = sequences[i][j];
        }
    }
    return KOS_OK;
}

/*
 * 下一格能不能放，两个条件都要成立：
 *   1. 现在没有任何人实际站在这一格；
 *   2. 计划里排在它前面、也经过这一格的人已经执行过那一步（否则那个动作将来
 *      还会把这人带进同一格）。
 * 同一位厨师自己过去经过同一格不算冲突——那几步已经在 executed 前缀里。
 */
static int cell_released(const kos_adg *graph, int chef, int step, int target,
                         const int *executed, const int *actual_cells)
{
    int other;

    for (other = 0; other < graph->chef_count; other++) {
        int m;

        if (other != chef && actual_cells[other] == target) {
            return 0; /* 上一位使用者还站在格子里 */
        }
        for (m = 0; m < step; m++) {
            if (other == chef) {
                continue;
            }
            if (graph->cells[other][m] != target) {
                continue;
            }
            if (m >= executed[other]) {
                return 0; /* 计划里更早的那一步还没执行 */
            }
        }
    }
    return 1;
}

int kos_adg_releasable(const kos_adg *graph, int chef, const int *executed,
                       const int *actual_cells, int *count_out)
{
    int released = 0;
    int step;

    if (graph == NULL || executed == NULL || actual_cells == NULL ||
        count_out == NULL || chef < 0 || chef >= graph->chef_count) {
        return KOS_ERR_INVAL;
    }
    *count_out = 0;
    if (executed[chef] < 1 || executed[chef] > graph->length[chef]) {
        return KOS_ERR_RANGE;
    }
    for (step = executed[chef]; step < graph->length[chef]; step++) {
        int target = graph->cells[chef][step];

        if (!cell_released(graph, chef, step, target, executed, actual_cells)) {
            break;
        }
        released++;
    }
    *count_out = released;
    return KOS_OK;
}
