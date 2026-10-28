/*
 * adg.h -- 执行依赖图放行（action dependency graph）。
 *
 * 规划是同步的（第 k 步所有厨师同时走），执行不是：各厨师速度不同，所以 navd
 * 不能按时间把整条计划一次发给插件。对每个格记下计划里先后经过它的人：厨师 A
 * 的第 k 个路点要等计划里同一个格的上一位使用者实际离开（实际所在格变了）才
 * 放行。这样无论谁快谁慢，已放行的路点序列都不会让两个人同时进同一格。
 *
 * 这里只做纯函数计算：输入各厨师按计划步排列的格子序列（sequence[0] 是起点格）
 * 与每人的实际格，输出"从已执行位置往后还能连续放行几个路点"。
 */
#ifndef WELLCOOKED_ADG_H
#define WELLCOOKED_ADG_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <wellcooked/shm.h>

#define KOS_ADG_MAX_CHEFS KOS_SHM_MAX_CHEFS
#define KOS_ADG_MAX_STEPS 512

typedef struct {
    int chef_count;
    int length[KOS_ADG_MAX_CHEFS];
    int cells[KOS_ADG_MAX_CHEFS][KOS_ADG_MAX_STEPS];
} kos_adg;

/*
 * 建图。sequences[i] 有 lengths[i] 项，第一项是该厨师的起始格；lengths[i] 至少 1。
 * 同一厨师同一格可以重复出现（等待/折返），依赖按计划顺序检查。
 */
int kos_adg_build(kos_adg *graph, const int *const *sequences, const int *lengths,
                  int chef_count);

/*
 * executed[i] 是各厨师已经实际走到的序列长度（>= 1，含起点；下一格下标 =
 * executed[i]，sequence[executed[i]-1] 是当前所在格）。从厨师 chef 的下一格
 * 开始，返回能连续放行的路点数（0 = 下一格还被别人占着，或计划里更早的
 * 占用者还没执行到那一步）。actual_cells[i] 是各厨师当前实际格。
 */
int kos_adg_releasable(const kos_adg *graph, int chef, const int *executed,
                       const int *actual_cells, int *count_out);

#endif /* WELLCOOKED_ADG_H */
