/*
 * assign.c -- 派工代价、匈牙利配对、锅链与洗盘判定。见 assign.h 的接口约束。
 *
 * 匈牙利算法是 O(n³) 的势函数版本（Kuhn-Munkres / Jonker-Volgenant 的经典写法）：
 * 厨师 × 工序 ≤ 8 × 32，最坏几万次浮点运算，一次派工就到微秒级。矩阵按"行少于
 * 列"摆；厨师比工序多时先转置，保证每道工序都有人接而不是留下空闲厨师。
 * 平局规则是扫描顺序本身（严格小于才更新），所以同一输入永远给同一配对。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <wellcooked/assign.h>

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wellcooked/json.h>

#define ASSIGN_FILE_MAX (1024 * 1024)
#define ASSIGN_PATH_MAX 512
/* 匈牙利的工作矩阵与势函数按工序上限（32）静态分配，避免每次派工 malloc。 */
#define ASSIGN_DIM (KOS_ASSIGN_MAX_TASKS + 1)

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

void kos_assign_weights_defaults(kos_assign_weights *out)
{
    if (out == NULL) {
        return;
    }
    out->w_d = 1.0;
    out->w_t = 0.2;
    out->w_c = 0.5;
    out->w_u = 2.0;
    out->w_z = 1.0;
}

void kos_assign_config_defaults(kos_assign_config *out)
{
    if (out == NULL) {
        return;
    }
    kos_assign_weights_defaults(&out->weights);
    out->cell_ms = KOS_ASSIGN_CELL_MS_DEFAULT;
    out->wash_ms = KOS_ASSIGN_WASH_MS_DEFAULT;
    out->from_file = 0;
}

double kos_assign_travel_s(double cells, double cell_ms)
{
    if (cells <= 0.0 || cell_ms <= 0.0) {
        return 0.0;
    }
    return cells * cell_ms / 1000.0;
}

static double boosted_wc(const kos_assign_weights *weights, const kos_assign_terms *terms)
{
    return terms->critical_x2 ? weights->w_c * KOS_ASSIGN_CRITICAL_BOOST : weights->w_c;
}

double kos_assign_cost(const kos_assign_weights *weights, const kos_assign_terms *terms)
{
    if (weights == NULL || terms == NULL) {
        return 0.0;
    }
    if (terms->unreachable) {
        return KOS_ASSIGN_FORBIDDEN;
    }
    return weights->w_d * terms->travel_s + weights->w_t * terms->work_s -
           boosted_wc(weights, terms) * terms->critical_s -
           weights->w_u * terms->urgency + weights->w_z * terms->zone_penalty;
}

void kos_assign_breakdown(const kos_assign_weights *weights,
                           const kos_assign_terms *terms, kos_assign_components *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (weights == NULL || terms == NULL) {
        return;
    }
    if (terms->unreachable) {
        out->cost = KOS_ASSIGN_FORBIDDEN;
        return;
    }
    out->w_d = weights->w_d * terms->travel_s;
    out->w_t = weights->w_t * terms->work_s;
    out->w_c = -boosted_wc(weights, terms) * terms->critical_s;
    out->w_u = -weights->w_u * terms->urgency;
    out->w_z = weights->w_z * terms->zone_penalty;
    out->cost = out->w_d + out->w_t + out->w_c + out->w_u + out->w_z;
}

int kos_assign_wash_needed(int clean_plates, int dirty_plates)
{
    return clean_plates <= 1 && dirty_plates > 0;
}

void kos_assign_pot_chain(const kos_recipes *recipes, unsigned char *out)
{
    int i;
    int pass;

    if (out == NULL) {
        return;
    }
    if (recipes == NULL || recipes->node_count <= 0) {
        return;
    }
    memset(out, 0, (size_t)recipes->node_count);
    for (i = 0; i < recipes->node_count; i++) {
        if (strcmp(recipes->nodes[i].op, "cook") == 0) {
            out[i] = 1;
        }
    }
    /* 边是子 → 父；从 cook 沿父→子反向可达的节点都是"锅前置"。节点数最多 64，
     * 固定点迭代比写拓扑排序简单，也不会被畸形图卡住。 */
    for (pass = 0; pass < recipes->node_count; pass++) {
        int changed = 0;

        for (i = 0; i < recipes->edge_count; i++) {
            int child = recipes->edges[i].from;
            int parent = recipes->edges[i].to;

            if (child >= 0 && child < recipes->node_count && parent >= 0 &&
                parent < recipes->node_count && out[parent] && !out[child]) {
                out[child] = 1;
                changed = 1;
            }
        }
        if (!changed) {
            break;
        }
    }
}

/* ============================================================ 配置读取 */

static char *read_text(const char *path, size_t *len_out, int *missing, kos_error *err)
{
    FILE *file;
    long size;
    char *text;
    size_t got;

    *missing = 0;
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            *missing = 1;
            return NULL;
        }
        set_error(err, KOS_ERR_IO, "fopen('%s'): %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        size > ASSIGN_FILE_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        set_error(err, KOS_ERR_IO, "读取 '%s' 大小失败", path);
        return NULL;
    }
    text = calloc((size_t)size + 1, 1);
    if (text == NULL) {
        fclose(file);
        set_error(err, KOS_ERR_NOMEM, "calloc('%s')", path);
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
    *len_out = (size_t)size;
    return text;
}

static int read_weight(const json_value *object, const char *key, double *out,
                       kos_error *err)
{
    double value;

    switch (json_object_get_double(object, key, &value)) {
    case JSON_OK:
        if (!isfinite(value) || value < 0.0) {
            return set_error(err, KOS_ERR_RANGE, "%s 必须是非负数", key);
        }
        *out = value;
        return KOS_OK;
    case JSON_ERR_MISSING:
        return KOS_OK;
    default:
        return set_error(err, KOS_ERR_INVAL, "%s 不是数字", key);
    }
}

int kos_assign_config_load(const char *path, kos_assign_config *out, kos_error *err)
{
    json_value *root = NULL;
    const json_value *weights;
    json_error jerr;
    char *text;
    size_t len = 0;
    int missing = 0;
    int rc;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (path == NULL || out == NULL) {
        return set_error(err, KOS_ERR_INVAL, "kos_assign_config_load 参数为空");
    }
    kos_assign_config_defaults(out);
    text = read_text(path, &len, &missing, err);
    if (text == NULL) {
        if (missing) {
            return KOS_OK;
        }
        return err != NULL ? err->code : KOS_ERR_IO;
    }
    json_error_init(&jerr);
    rc = json_parse_ex(text, len, &root, &jerr);
    free(text);
    if (rc != JSON_OK) {
        return set_error(err, KOS_ERR_INVAL, "'%s' JSON 解析失败: %s @%lu", path,
                         json_strerror(rc), (unsigned long)jerr.offset);
    }
    if (json_typeof(root) != JSON_OBJECT) {
        json_free(root);
        return set_error(err, KOS_ERR_INVAL, "'%s' 顶层不是对象", path);
    }
    switch (json_object_get_object(root, "weights", &weights)) {
    case JSON_OK:
        break;
    case JSON_ERR_MISSING:
        weights = NULL;
        break;
    default:
        json_free(root);
        return set_error(err, KOS_ERR_INVAL, "'%s' 的 weights 不是对象", path);
    }
    if (weights != NULL) {
        rc = read_weight(weights, "w_d", &out->weights.w_d, err);
        if (rc == KOS_OK) {
            rc = read_weight(weights, "w_t", &out->weights.w_t, err);
        }
        if (rc == KOS_OK) {
            rc = read_weight(weights, "w_c", &out->weights.w_c, err);
        }
        if (rc == KOS_OK) {
            rc = read_weight(weights, "w_u", &out->weights.w_u, err);
        }
        if (rc == KOS_OK) {
            rc = read_weight(weights, "w_z", &out->weights.w_z, err);
        }
        if (rc != KOS_OK) {
            json_free(root);
            return rc;
        }
    }
    {
        double cell_ms;
        double wash_ms;

        switch (json_object_get_double(root, "cell_ms", &cell_ms)) {
        case JSON_OK:
            if (!isfinite(cell_ms) || cell_ms <= 0.0 || cell_ms > 60000.0) {
                json_free(root);
                return set_error(err, KOS_ERR_RANGE,
                                 "cell_ms 必须在 (0, 60000] 内");
            }
            out->cell_ms = cell_ms;
            break;
        case JSON_ERR_MISSING:
            break;
        default:
            json_free(root);
            return set_error(err, KOS_ERR_INVAL, "cell_ms 不是数字");
        }
        switch (json_object_get_double(root, "wash_ms", &wash_ms)) {
        case JSON_OK:
            if (!isfinite(wash_ms) || wash_ms <= 0.0 || wash_ms > 600000.0) {
                json_free(root);
                return set_error(err, KOS_ERR_RANGE, "wash_ms 必须在 (0, 600000] 内");
            }
            out->wash_ms = wash_ms;
            break;
        case JSON_ERR_MISSING:
            break;
        default:
            json_free(root);
            return set_error(err, KOS_ERR_INVAL, "wash_ms 不是数字");
        }
    }
    json_free(root);
    out->from_file = 1;
    return KOS_OK;
}

int kos_assign_config_for_kitchen(const char *kitchen_path, kos_assign_config *out,
                                  kos_error *err)
{
    char path[ASSIGN_PATH_MAX];
    const char *slash;

    if (kitchen_path == NULL || out == NULL) {
        return set_error(err, KOS_ERR_INVAL, "kos_assign_config_for_kitchen 参数为空");
    }
    slash = strrchr(kitchen_path, '/');
    if (slash == NULL) {
        snprintf(path, sizeof path, "schedule.json");
    } else {
        size_t dir_len = (size_t)(slash - kitchen_path);

        if (dir_len + sizeof("/schedule.json") > sizeof path) {
            return set_error(err, KOS_ERR_RANGE, "kitchen 路径过长: %s", kitchen_path);
        }
        memcpy(path, kitchen_path, dir_len);
        path[dir_len] = '\0';
        snprintf(path + dir_len, sizeof path - dir_len, "/schedule.json");
    }
    return kos_assign_config_load(path, out, err);
}

/* ========================================================== 匈牙利配对 */

/*
 * 行数 ≤ 列数的方阵最小代价配对（每行必须配到一个不同的列）。结果写进
 * row_to_col[rows]；cost 是 rows × cols 的行主序矩阵。
 */
static int solve_rows(const double *cost, int rows, int cols, int *row_to_col)
{
    double u[ASSIGN_DIM];
    double v[ASSIGN_DIM];
    double minv[ASSIGN_DIM];
    int p[ASSIGN_DIM];
    int way[ASSIGN_DIM];
    unsigned char used[ASSIGN_DIM];
    int i;
    int j;

    memset(u, 0, sizeof u);
    memset(v, 0, sizeof v);
    memset(p, 0, sizeof p);
    for (i = 0; i < rows; i++) {
        row_to_col[i] = -1;
    }
    for (i = 1; i <= rows; i++) {
        int j0 = 0;

        p[0] = i;
        memset(way, 0, sizeof way);
        for (j = 0; j <= cols; j++) {
            minv[j] = DBL_MAX;
            used[j] = 0;
        }
        do {
            int i0;
            double delta = DBL_MAX;
            int j1 = 0;

            used[j0] = 1;
            i0 = p[j0];
            for (j = 1; j <= cols; j++) {
                double cur;

                if (used[j]) {
                    continue;
                }
                cur = cost[(size_t)(i0 - 1) * (size_t)cols + (size_t)(j - 1)] - u[i0] - v[j];
                /* 严格小于才更新：并列时保留先扫到的列，配对可复现。 */
                if (cur < minv[j]) {
                    minv[j] = cur;
                    way[j] = j0;
                }
                if (minv[j] < delta) {
                    delta = minv[j];
                    j1 = j;
                }
            }
            if (j1 == 0) {
                return KOS_ERR_EMPTY;
            }
            for (j = 0; j <= cols; j++) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            int j1 = way[j0];

            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }
    for (j = 1; j <= cols; j++) {
        if (p[j] > 0 && p[j] <= rows) {
            row_to_col[p[j] - 1] = j - 1;
        }
    }
    return KOS_OK;
}

int kos_assign_solve(const kos_assign_weights *weights, int chef_count, int task_count,
                     const kos_assign_terms *terms, int *task_of_chef, kos_error *err)
{
    /* 按 rows × cols 紧凑排（solve_rows 以实际 cols 为行距），不能拿二维数组
     * 直接传指针：那样第二行会从第一行的 padding 读起。 */
    double matrix[ASSIGN_DIM * ASSIGN_DIM];
    int row_to_col[ASSIGN_DIM];
    int transposed;
    int rows;
    int cols;
    int r;
    int c;
    int rc;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (weights == NULL || terms == NULL || task_of_chef == NULL) {
        return set_error(err, KOS_ERR_INVAL, "kos_assign_solve 参数为空");
    }
    if (chef_count < 0 || chef_count > KOS_ASSIGN_MAX_CHEFS || task_count < 0 ||
        task_count > KOS_ASSIGN_MAX_TASKS) {
        return set_error(err, KOS_ERR_RANGE, "chef_count=%d task_count=%d 超过上限",
                         chef_count, task_count);
    }
    for (r = 0; r < chef_count; r++) {
        task_of_chef[r] = -1;
    }
    if (chef_count == 0 || task_count == 0) {
        return KOS_OK;
    }
    transposed = chef_count > task_count;
    rows = transposed ? task_count : chef_count;
    cols = transposed ? chef_count : task_count;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            const kos_assign_terms *pair = transposed ? &terms[c * task_count + r]
                                                      : &terms[r * task_count + c];

            matrix[r * cols + c] = kos_assign_cost(weights, pair);
        }
    }
    rc = solve_rows(matrix, rows, cols, row_to_col);
    if (rc != KOS_OK) {
        return set_error(err, rc, "匈牙利算法没有可行配对");
    }
    if (transposed) {
        for (c = 0; c < rows; c++) {
            /* 转置矩阵的每行是工序 c，列是厨师；反写回 chef → task。 */
            if (row_to_col[c] >= 0 &&
                matrix[c * cols + row_to_col[c]] < KOS_ASSIGN_FORBIDDEN / 2.0) {
                task_of_chef[row_to_col[c]] = c;
            }
        }
    } else {
        for (r = 0; r < rows; r++) {
            if (row_to_col[r] >= 0 &&
                matrix[r * cols + row_to_col[r]] < KOS_ASSIGN_FORBIDDEN / 2.0) {
                task_of_chef[r] = row_to_col[r];
            }
        }
    }
    return KOS_OK;
}
