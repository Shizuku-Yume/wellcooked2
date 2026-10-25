/*
 * assign.h -- 派工代价与多厨师配对（模块契约）。
 *
 * L3 的"谁接哪道"分两步：先给每个 (空闲厨师, 就绪工序) 算一个标量代价，再用匈牙利
 * 算法求总代价最小的配对。代价公式（spec "L3 调度"）：
 *
 *   cost = +w_d * 路程秒 + w_t * 工时秒 − w_c * 剩余关键路径秒
 *          − w_u * 订单紧迫度 + w_z * 跨区惩罚
 *
 * 前两项越大越不想接，后两项越大越想接（所以带负号）。锅及其前置的 w_c 乘 2（锅先
 * 启动）；分区模式的跨区惩罚为 1，灵活模式为 0。
 *
 * 这个模块不认识订单、shm 和游戏：四个工位参数都由调用方先量好（路程用 stand 的
 * 距离场），决策日志的各分量也由 kos_assign_breakdown() 给出，保证写进
 * decisions.jsonl 的数字和实际用来比较的是同一组。
 */
#ifndef KITCHENOS_ASSIGN_H
#define KITCHENOS_ASSIGN_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>

#include <kitchenos/recipes.h>
#include <kitchenos/shm.h>

/* 厨师上限取 shm 的槽位数；就绪工序上限与 Scheduler 的订单 × 菜品节点规模一致。 */
#define KOS_ASSIGN_MAX_CHEFS KOS_SHM_MAX_CHEFS
#define KOS_ASSIGN_MAX_TASKS 32

/* 锅链（cook 及其前置）的 w_c 倍数。 */
#define KOS_ASSIGN_CRITICAL_BOOST 2.0

/*
 * 不可达配对的代价。距离场在两个互不相通的厨房区之间返回 UINT16_MAX，这种配对
 * 不该被派出去：代价取一个远大于任何真实组合的有限值，匈牙利算法会先最大化可行
 * 配对数量，求解后再把代价落在禁区里的配对丢弃（task_of_chef = -1）。
 */
#define KOS_ASSIGN_FORBIDDEN 1e15

/* 任务 01 实测的每格移动时间：0.107 s/格。地图片区的调度配置可以覆盖。 */
#define KOS_ASSIGN_CELL_MS_DEFAULT 107.0
#define KOS_ASSIGN_WASH_MS_DEFAULT 3000.0

typedef struct {
    double w_d; /* 每秒路程 */
    double w_t; /* 每秒工时 */
    double w_c; /* 每秒剩余关键路径 */
    double w_u; /* 每单位紧迫度 */
    double w_z; /* 跨区惩罚（0/1） */
} kos_assign_weights;

typedef struct {
    double travel_s;     /* 距离场步数 × cell_ms */
    double work_s;       /* 工序工时 */
    double critical_s;   /* 该工序到本单完成的剩余关键路径 */
    double urgency;      /* 策略给的订单排名，0..1（1 最紧急） */
    double zone_penalty; /* 分区模式跨区为 1，否则 0 */
    int critical_x2;     /* 锅链：w_c 乘 2 */
    int unreachable;     /* 1 = 厨师到不了该工位，禁止配对 */
} kos_assign_terms;

typedef struct {
    double w_d; /* 带符号的加权分量，cost = 五项之和 */
    double w_t;
    double w_c;
    double w_u;
    double w_z;
    double cost;
} kos_assign_components;

typedef struct {
    kos_assign_weights weights;
    double cell_ms; /* 每格移动毫秒，来自实测或地图配置 */
    double wash_ms; /* 生成的 wash 工序工时 */
    int from_file;  /* 1 = 读到了 schedule.json，0 = 用缺省值 */
} kos_assign_config;

/* spec "L3 调度"里写下的缺省值；任务 11 的网格搜索会写回各地图配置。 */
void kos_assign_weights_defaults(kos_assign_weights *out);
void kos_assign_config_defaults(kos_assign_config *out);

/*
 * 读一份调度配置（weights / cell_ms / wash_ms）。文件不存在时写缺省值并返回
 * KOS_OK、from_file = 0：没有 schedule.json 的老地图仍能跑。文件存在但 JSON 坏、
 * 数值为负或越界时返回错误，调用方决定退回缺省值还是拒绝启动。
 */
int kos_assign_config_load(const char *path, kos_assign_config *out, kos_error *err);

/* 在 kitchen.json 同目录找 schedule.json；找不到就用缺省值。 */
int kos_assign_config_for_kitchen(const char *kitchen_path, kos_assign_config *out,
                                  kos_error *err);

double kos_assign_cost(const kos_assign_weights *weights, const kos_assign_terms *terms);

/* 把 cost 拆成写进 decisions.jsonl 的五个带符号分量。 */
void kos_assign_breakdown(const kos_assign_weights *weights,
                          const kos_assign_terms *terms, kos_assign_components *out);

/* 距离场步数 → 秒；step 为负时按 0 算。 */
double kos_assign_travel_s(double cells, double cell_ms);

/*
 * 匈牙利算法：terms 按 chef-major 排（terms[chef * task_count + task]），求
 * min(chef_count, task_count) 对配对的总代价最小解。task_of_chef[chef] 写任务下标，
 * 没配上（任务少于厨师时）写 -1。并列时取下标小的配对，同一输入结果永远相同。
 * 厨师数 > 工序数时先把矩阵转置，保证每道工序都有人接。
 */
int kos_assign_solve(const kos_assign_weights *weights, int chef_count, int task_count,
                     const kos_assign_terms *terms, int *task_of_chef, kos_error *err);

/* 干净盘 ≤ 1 且有脏盘时应该生成 wash 工序。 */
int kos_assign_wash_needed(int clean_plates, int dirty_plates);

/*
 * 标记锅链：op 为 cook 的节点及其全部前置（沿 edges 反向可达）置 1，其余 0。
 * out 长度至少 recipes->node_count。cook 是长工时无人值守设备，调度要尽早开火。
 */
void kos_assign_pot_chain(const kos_recipes *recipes, unsigned char *out);

#endif /* KITCHENOS_ASSIGN_H */
