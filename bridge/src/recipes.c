/*
 * recipes.c -- 解析 recipes.json 与 orders.json 的 menu。
 *
 * 工序节点 id 在整个文件里唯一（不同菜共用中间产物时只有一个节点）；每道菜的
 * `nodes[]` 是它自己的拓扑序。这里只做"读 + 索引 + 就绪/关键路径计算"，不决定
 * 谁先做、不碰任何 IPC。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/recipes.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kitchenos/json.h>
#include <kitchenos/kitchen.h>
#include <kitchenos/zones.h>

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

static int set_error(kos_error *err, int code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int set_error(kos_error *err, int code, const char *fmt, ...)
{
    va_list ap;

    if (err != NULL) {
        err->code = code;
        err->sys_errno = 0;
        va_start(ap, fmt);
        vsnprintf(err->context, sizeof err->context, fmt, ap);
        va_end(ap);
    }
    return code;
}

static char *read_file(const char *path, size_t *len_out, kos_error *err)
{
    FILE *file;
    long size;
    char *text;
    size_t got;

    file = fopen(path, "rb");
    if (file == NULL) {
        if (err != NULL) {
            err->code = KOS_ERR_IO;
            err->sys_errno = errno;
            snprintf(err->context, sizeof err->context, "fopen('%s')", path);
        }
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        size > KOS_RECIPE_FILE_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        set_error(err, KOS_ERR_IO, "读取 '%s' 大小失败", path);
        return NULL;
    }
    text = malloc((size_t)size + 1);
    if (text == NULL) {
        fclose(file);
        set_error(err, KOS_ERR_NOMEM, "malloc(%ld)", size + 1);
        return NULL;
    }
    got = fread(text, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(text);
        set_error(err, KOS_ERR_IO, "读取 '%s' 只得到 %lu/%ld 字节", path,
                  (unsigned long)got, size);
        return NULL;
    }
    text[size] = '\0';
    *len_out = (size_t)size;
    return text;
}

static int read_json(const char *path, json_value **out, kos_error *err)
{
    char *text;
    size_t len = 0;
    json_error jerr;
    int rc;

    text = read_file(path, &len, err);
    if (text == NULL) {
        return KOS_ERR_IO;
    }
    json_error_init(&jerr);
    rc = json_parse_ex(text, len, out, &jerr);
    free(text);
    if (rc != JSON_OK) {
        return set_error(err, KOS_ERR_INVAL, "'%s' JSON 解析失败: %s @%lu", path,
                         json_strerror(rc), (unsigned long)jerr.offset);
    }
    return KOS_OK;
}

int kos_recipes_load(const char *path, kos_recipes *out, kos_error *err)
{
    json_value *root = NULL;
    const json_value *nodes;
    const json_value *edges;
    const json_value *dishes;
    size_t i;
    int rc;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (path == NULL || out == NULL) {
        return set_error(err, KOS_ERR_INVAL, "kos_recipes_load 参数为空");
    }
    memset(out, 0, sizeof *out);
    rc = read_json(path, &root, err);
    if (rc != KOS_OK) {
        return rc;
    }
    if (json_typeof(root) != JSON_OBJECT ||
        json_object_get_array(root, "nodes", &nodes) != JSON_OK ||
        json_object_get_array(root, "edges", &edges) != JSON_OK ||
        json_object_get_array(root, "dishes", &dishes) != JSON_OK) {
        rc = set_error(err, KOS_ERR_INVAL, "'%s' 缺 nodes/edges/dishes", path);
        goto done;
    }
    if (json_array_size(nodes) > KOS_RECIPE_MAX_NODES ||
        json_array_size(edges) > KOS_RECIPE_MAX_EDGES ||
        json_array_size(dishes) > KOS_RECIPE_MAX_DISHES) {
        rc = set_error(err, KOS_ERR_RANGE, "'%s' 超过编译期上限（nodes %d / edges %d / dishes %d）",
                       path, KOS_RECIPE_MAX_NODES, KOS_RECIPE_MAX_EDGES,
                       KOS_RECIPE_MAX_DISHES);
        goto done;
    }
    {
        const char *scene = NULL;
        size_t scene_len = 0;

        if (json_object_get_string(root, "scene", &scene, &scene_len) == JSON_OK) {
            copy_str(out->scene, sizeof out->scene, scene);
        }
    }

    for (i = 0; i < json_array_size(nodes); i++) {
        const json_value *entry = json_array_get(nodes, i);
        kos_recipe_node *node = &out->nodes[out->node_count];
        const char *value = NULL;
        size_t value_len = 0;
        double duration = 0.0;

        if (json_typeof(entry) != JSON_OBJECT ||
            json_object_get_string(entry, "id", &value, &value_len) != JSON_OK ||
            json_object_get_string(entry, "op", &value, &value_len) != JSON_OK) {
            rc = set_error(err, KOS_ERR_INVAL, "'%s' nodes[%lu] 缺 id/op", path,
                           (unsigned long)i);
            goto done;
        }
        if (json_object_get_string(entry, "id", &value, &value_len) == JSON_OK) {
            copy_str(node->id, sizeof node->id, value);
        }
        if (json_object_get_string(entry, "op", &value, &value_len) == JSON_OK) {
            copy_str(node->op, sizeof node->op, value);
        }
        if (json_object_get_string(entry, "resource", &value, &value_len) == JSON_OK) {
            copy_str(node->resource, sizeof node->resource, value);
        }
        if (json_object_get_string(entry, "label", &value, &value_len) == JSON_OK) {
            copy_str(node->label, sizeof node->label, value);
        }
        if (json_object_get_string(entry, "ingredient", &value, &value_len) == JSON_OK) {
            copy_str(node->ingredient, sizeof node->ingredient, value);
        }
        if (json_object_get_double(entry, "duration", &duration) == JSON_OK &&
            duration >= 0.0) {
            node->duration_ms = (int)(duration * 1000.0 + 0.5);
        } else {
            node->duration_ms = 1000; /* 缺失工时按 1 s，日志里能从决策看出来 */
        }
        node->zone = -1;
        out->node_count++;
    }

    for (i = 0; i < json_array_size(edges); i++) {
        const json_value *entry = json_array_get(edges, i);
        const json_value *from_value;
        const json_value *to_value;
        const char *from_text;
        const char *to_text;
        int from;
        int to;

        if (json_typeof(entry) != JSON_ARRAY || json_array_size(entry) != 2) {
            rc = set_error(err, KOS_ERR_INVAL, "'%s' edges[%lu] 不是 [子,父]", path,
                           (unsigned long)i);
            goto done;
        }
        from_value = json_array_get(entry, 0);
        to_value = json_array_get(entry, 1);
        from_text = json_string_value(from_value);
        to_text = json_string_value(to_value);
        from = kos_recipes_node_index(out, from_text);
        to = kos_recipes_node_index(out, to_text);
        if (from < 0 || to < 0) {
            rc = set_error(err, KOS_ERR_INVAL, "'%s' edge[%lu] 引用了未知节点", path,
                           (unsigned long)i);
            goto done;
        }
        out->edges[out->edge_count].from = from;
        out->edges[out->edge_count].to = to;
        out->edge_count++;
    }

    for (i = 0; i < json_array_size(dishes); i++) {
        const json_value *entry = json_array_get(dishes, i);
        kos_dish *dish = &out->dishes[out->dish_count];
        const json_value *dish_nodes;
        const char *value = NULL;
        size_t value_len = 0;
        long long score = 0;
        size_t j;

        if (json_typeof(entry) != JSON_OBJECT ||
            json_object_get_string(entry, "dish", &value, &value_len) != JSON_OK ||
            json_object_get_array(entry, "nodes", &dish_nodes) != JSON_OK) {
            rc = set_error(err, KOS_ERR_INVAL, "'%s' dishes[%lu] 缺 dish/nodes", path,
                           (unsigned long)i);
            goto done;
        }
        copy_str(dish->dish, sizeof dish->dish, value);
        if (json_object_get_int(entry, "score", &score) == JSON_OK) {
            dish->score = (int)score;
        }
        if (json_object_get_string(entry, "root", &value, &value_len) == JSON_OK) {
            dish->root = kos_recipes_node_index(out, value);
        } else {
            dish->root = -1;
        }
        if (json_array_size(dish_nodes) > KOS_RECIPE_MAX_DISH_NODES) {
            rc = set_error(err, KOS_ERR_RANGE, "菜 %s 的工序数超过 %d", dish->dish,
                           KOS_RECIPE_MAX_DISH_NODES);
            goto done;
        }
        for (j = 0; j < json_array_size(dish_nodes); j++) {
            int node = kos_recipes_node_index(out, json_string_value(json_array_get(dish_nodes, j)));

            if (node < 0) {
                rc = set_error(err, KOS_ERR_INVAL, "菜 %s 引用了未知工序", dish->dish);
                goto done;
            }
            dish->nodes[dish->node_count++] = node;
        }
        if (dish->node_count == 0) {
            rc = set_error(err, KOS_ERR_INVAL, "菜 %s 没有工序", dish->dish);
            goto done;
        }
        out->dish_count++;
    }
    rc = KOS_OK;

done:
    json_free(root);
    if (rc != KOS_OK) {
        memset(out, 0, sizeof *out);
    }
    return rc;
}

int kos_recipes_node_index(const kos_recipes *recipes, const char *node_id)
{
    int i;

    if (recipes == NULL || node_id == NULL) {
        return -1;
    }
    for (i = 0; i < recipes->node_count; i++) {
        if (strcmp(recipes->nodes[i].id, node_id) == 0) {
            return i;
        }
    }
    return -1;
}

int kos_recipes_dish_index(const kos_recipes *recipes, const char *dish)
{
    int i;

    if (recipes == NULL || dish == NULL) {
        return -1;
    }
    for (i = 0; i < recipes->dish_count; i++) {
        if (strcmp(recipes->dishes[i].dish, dish) == 0) {
            return i;
        }
    }
    return -1;
}

int kos_recipe_node_ready(const kos_recipes *recipes, int dish_index,
                          const unsigned char *done, int node_index)
{
    int e;

    if (recipes == NULL || done == NULL || dish_index < 0 ||
        dish_index >= recipes->dish_count) {
        return 0;
    }
    for (e = 0; e < recipes->edge_count; e++) {
        if (recipes->edges[e].to == node_index &&
            !done[recipes->edges[e].from]) {
            return 0;
        }
    }
    return 1;
}

int64_t kos_recipe_critical_ms(const kos_recipes *recipes, int dish_index, int node_index)
{
    const kos_dish *dish;
    int64_t cost[KOS_RECIPE_MAX_NODES];
    int i;

    if (recipes == NULL || dish_index < 0 || dish_index >= recipes->dish_count ||
        node_index < 0 || node_index >= recipes->node_count) {
        return 0;
    }
    dish = &recipes->dishes[dish_index];
    memset(cost, 0, sizeof cost);
    /* dish.nodes 是子→父的拓扑序：反向遍历时父节点的最长剩余路径已经算好。 */
    for (i = dish->node_count - 1; i >= 0; i--) {
        int node = dish->nodes[i];
        int64_t best_parent = 0;
        int e;

        for (e = 0; e < recipes->edge_count; e++) {
            if (recipes->edges[e].from == node && cost[recipes->edges[e].to] > best_parent) {
                best_parent = cost[recipes->edges[e].to];
            }
        }
        cost[node] = (int64_t)recipes->nodes[node].duration_ms + best_parent;
    }
    return cost[node_index];
}

int64_t kos_recipe_est_cost_ms(const kos_recipes *recipes, int dish_index)
{
    int64_t total = 0;
    int i;

    if (recipes == NULL || dish_index < 0 || dish_index >= recipes->dish_count) {
        return 0;
    }
    for (i = 0; i < recipes->dishes[dish_index].node_count; i++) {
        total += recipes->nodes[recipes->dishes[dish_index].nodes[i]].duration_ms;
    }
    return total;
}

int kos_recipe_menu_load(const char *path, char dishes[][KOS_RECIPE_NAME_MAX], int max,
                         int *count, kos_error *err)
{
    json_value *root = NULL;
    const json_value *menu;
    int rc;
    size_t i;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (path == NULL || dishes == NULL || count == NULL || max <= 0) {
        return set_error(err, KOS_ERR_INVAL, "kos_recipe_menu_load 参数为空");
    }
    *count = 0;
    rc = read_json(path, &root, err);
    if (rc != KOS_OK) {
        return rc;
    }
    if (json_typeof(root) != JSON_OBJECT ||
        json_object_get_array(root, "menu", &menu) != JSON_OK) {
        rc = set_error(err, KOS_ERR_INVAL, "'%s' 缺 menu", path);
        goto done;
    }
    for (i = 0; i < json_array_size(menu) && *count < max; i++) {
        const json_value *stage = json_array_get(menu, i);
        const json_value *recipes;
        size_t j;

        if (json_typeof(stage) != JSON_OBJECT ||
            json_object_get_array(stage, "recipes", &recipes) != JSON_OK) {
            continue;
        }
        for (j = 0; j < json_array_size(recipes) && *count < max; j++) {
            const char *name = json_string_value(json_array_get(recipes, j));

            if (name != NULL && name[0] != '\0') {
                copy_str(dishes[*count], KOS_RECIPE_NAME_MAX, name);
                (*count)++;
            }
        }
    }
    rc = *count > 0 ? KOS_OK : set_error(err, KOS_ERR_EMPTY, "'%s' menu 没有菜", path);

done:
    json_free(root);
    return rc;
}

int kos_recipes_apply_zones(kos_recipes *recipes, const kos_kitchen *kitchen,
                            const kos_zones *zones, kos_error *err)
{
    int i;
    int d;

    if (recipes == NULL || kitchen == NULL || zones == NULL) {
        return KOS_ERR_INVAL;
    }
    if (!zones->from_file || zones->zone_count <= 1) {
        return KOS_OK;
    }

    /* 1. 为每个现有节点根据工位确定所属分区 */
    for (i = 0; i < recipes->node_count; i++) {
        kos_recipe_node *node = &recipes->nodes[i];
        const char *hint = node->ingredient[0] != '\0' ? node->ingredient : node->label;
        int st_idx = -1;

        if (kos_kitchen_pick_station(kitchen, node->resource, hint, 0, &st_idx) == KOS_OK) {
            node->zone = kos_zones_station_zone(zones, kitchen->stations[st_idx].id);
        } else {
            node->zone = -1;
        }
    }

    /* 2. 扫描原始边，对跨区边插入 drop/fetch 对 */
    {
        int orig_edge_count = recipes->edge_count;
        int e;

        for (e = 0; e < orig_edge_count; e++) {
            int u = recipes->edges[e].from;
            int v = recipes->edges[e].to;
            int drop_idx = -1;
            int fetch_idx = -1;
            char drop_id[KOS_RECIPE_ID_MAX];
            char fetch_id[KOS_RECIPE_ID_MAX];

            if (recipes->nodes[u].zone < 0 || recipes->nodes[v].zone < 0 ||
                recipes->nodes[u].zone == recipes->nodes[v].zone) {
                continue;
            }

            if (strncmp(recipes->nodes[u].id, "fetch:", 6) == 0) {
                snprintf(drop_id, sizeof drop_id, "drop:%s", recipes->nodes[u].id + 6);
                snprintf(fetch_id, sizeof fetch_id, "fetch:handoff:%s", recipes->nodes[u].id + 6);
            } else if (strncmp(recipes->nodes[u].id, "plate:", 6) == 0) {
                snprintf(drop_id, sizeof drop_id, "drop:plated:%s", recipes->nodes[u].id + 6);
                snprintf(fetch_id, sizeof fetch_id, "fetch:plated:%s", recipes->nodes[u].id + 6);
            } else {
                snprintf(drop_id, sizeof drop_id, "drop:%s", recipes->nodes[u].label);
                snprintf(fetch_id, sizeof fetch_id, "fetch:%s", recipes->nodes[u].label);
            }

            drop_idx = kos_recipes_node_index(recipes, drop_id);
            if (drop_idx < 0) {
                if (recipes->node_count >= KOS_RECIPE_MAX_NODES) {
                    return set_error(err, KOS_ERR_RANGE, "工序节点数超限");
                }
                drop_idx = recipes->node_count++;
                kos_recipe_node *dn = &recipes->nodes[drop_idx];
                memset(dn, 0, sizeof *dn);
                copy_str(dn->id, sizeof dn->id, drop_id);
                copy_str(dn->op, sizeof dn->op, "drop");
                copy_str(dn->resource, sizeof dn->resource,
                         zones->handoff.kind[0] ? zones->handoff.kind : "counter");
                copy_str(dn->label, sizeof dn->label, recipes->nodes[u].label);
                dn->duration_ms = 1000;
                dn->zone = recipes->nodes[u].zone;
            }

            fetch_idx = kos_recipes_node_index(recipes, fetch_id);
            if (fetch_idx < 0) {
                if (recipes->node_count >= KOS_RECIPE_MAX_NODES) {
                    return set_error(err, KOS_ERR_RANGE, "工序节点数超限");
                }
                fetch_idx = recipes->node_count++;
                kos_recipe_node *fn = &recipes->nodes[fetch_idx];
                memset(fn, 0, sizeof *fn);
                copy_str(fn->id, sizeof fn->id, fetch_id);
                copy_str(fn->op, sizeof fn->op, "fetch");
                copy_str(fn->resource, sizeof fn->resource,
                         zones->handoff.kind[0] ? zones->handoff.kind : "counter");
                copy_str(fn->label, sizeof fn->label, recipes->nodes[u].label);
                fn->duration_ms = 1000;
                fn->zone = recipes->nodes[v].zone;
            }

            recipes->edges[e].to = drop_idx;

            if (recipes->edge_count >= KOS_RECIPE_MAX_EDGES) {
                return set_error(err, KOS_ERR_RANGE, "工序边数超限");
            }
            recipes->edges[recipes->edge_count].from = drop_idx;
            recipes->edges[recipes->edge_count].to = fetch_idx;
            recipes->edge_count++;

            if (recipes->edge_count >= KOS_RECIPE_MAX_EDGES) {
                return set_error(err, KOS_ERR_RANGE, "工序边数超限");
            }
            recipes->edges[recipes->edge_count].from = fetch_idx;
            recipes->edges[recipes->edge_count].to = v;
            recipes->edge_count++;

            for (d = 0; d < recipes->dish_count; d++) {
                kos_dish *dish = &recipes->dishes[d];
                int pos_u = -1;
                int pos_v = -1;
                int k;

                for (k = 0; k < dish->node_count; k++) {
                    if (dish->nodes[k] == u) pos_u = k;
                    if (dish->nodes[k] == v) pos_v = k;
                }
                if (pos_u >= 0 && pos_v >= 0 && dish->node_count + 2 <= KOS_RECIPE_MAX_DISH_NODES) {
                    for (k = dish->node_count - 1; k > pos_u; k--) {
                        dish->nodes[k + 2] = dish->nodes[k];
                    }
                    dish->nodes[pos_u + 1] = drop_idx;
                    dish->nodes[pos_u + 2] = fetch_idx;
                    dish->node_count += 2;
                }
            }
        }
    }
    return KOS_OK;
}
