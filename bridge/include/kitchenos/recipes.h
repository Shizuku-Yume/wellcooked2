/*
 * recipes.h -- config/recipes.json 的工序图（模块契约）。
 *
 * 边是"子 → 父"（`edges[]` 与 `dishes[].nodes` 的拓扑序一致）：父工序只有在所有
 * 子工序完成后才就绪。关键路径长度按"从该节点走到本菜完成还要多久"算，列表调度
 * 用它给就绪任务排序。
 */
#ifndef KITCHENOS_RECIPES_H
#define KITCHENOS_RECIPES_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>

#include <kitchenos/shm.h>
#include <kitchenos/kitchen.h>
#include <kitchenos/zones.h>

#define KOS_RECIPE_MAX_NODES 64
#define KOS_RECIPE_MAX_EDGES 128
#define KOS_RECIPE_MAX_DISHES 16
#define KOS_RECIPE_MAX_DISH_NODES 24
#define KOS_RECIPE_ID_MAX 64
#define KOS_RECIPE_OP_MAX 16
#define KOS_RECIPE_RESOURCE_MAX 32
#define KOS_RECIPE_LABEL_MAX 64
#define KOS_RECIPE_NAME_MAX 32
#define KOS_RECIPE_FILE_MAX (4 * 1024 * 1024)

typedef struct {
    char id[KOS_RECIPE_ID_MAX];
    char op[KOS_RECIPE_OP_MAX];
    char resource[KOS_RECIPE_RESOURCE_MAX];
    char label[KOS_RECIPE_LABEL_MAX];
    char ingredient[KOS_RECIPE_NAME_MAX]; /* fetch 节点才有，其它为空 */
    int duration_ms;
    int zone; /* 所属分区下标；-1 = 未指定/不限 */
} kos_recipe_node;

typedef struct {
    int from; /* 子工序 */
    int to;   /* 父工序 */
} kos_recipe_edge;

typedef struct {
    char dish[KOS_RECIPE_NAME_MAX];
    int score;
    int root;
    int node_count;
    int nodes[KOS_RECIPE_MAX_DISH_NODES]; /* 拓扑序：子在前，父在后 */
} kos_dish;

typedef struct {
    char scene[KOS_RECIPE_NAME_MAX];
    int node_count;
    int edge_count;
    int dish_count;
    kos_recipe_node nodes[KOS_RECIPE_MAX_NODES];
    kos_recipe_edge edges[KOS_RECIPE_MAX_EDGES];
    kos_dish dishes[KOS_RECIPE_MAX_DISHES];
} kos_recipes;

int kos_recipes_load(const char *path, kos_recipes *out, kos_error *err);

int kos_recipes_node_index(const kos_recipes *recipes, const char *node_id);
int kos_recipes_dish_index(const kos_recipes *recipes, const char *dish);

/*
 * 就绪判定：`done` 按节点下标标记本订单里已完成的工序；节点所有子工序都完成才算
 * 就绪。变量名用 done 而不是全局完成表——不同订单的同名工序是不同实例。
 */
int kos_recipe_node_ready(const kos_recipes *recipes, int dish_index,
                          const unsigned char *done, int node_index);

/* 该工序到本菜完成的最长剩余工时（含自身）。 */
int64_t kos_recipe_critical_ms(const kos_recipes *recipes, int dish_index, int node_index);

/* 本菜所有工序工时之和，用作订单 est_cost。 */
int64_t kos_recipe_est_cost_ms(const kos_recipes *recipes, int dish_index);

/*
 * config/orders.json 的 menu：把其中出现的菜名按顺序摊平。重复菜名保留（固定菜单
 * 可以重复同一道菜）；超过 max 的部分不读，返回实际数量。
 */
int kos_recipe_menu_load(const char *path, char dishes[][KOS_RECIPE_NAME_MAX], int max,
                         int *count, kos_error *err);

/*
 * 在分区模式下，根据工位位置与分区归属扩展菜谱：为跨区中间产物插入 (drop, fetch) 节点。
 */
int kos_recipes_apply_zones(kos_recipes *recipes, const kos_kitchen *kitchen,
                            const kos_zones *zones, kos_error *err);

#endif /* KITCHENOS_RECIPES_H */
