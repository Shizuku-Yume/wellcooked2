/*
 * live.json 编码：把一份自洽的世界状态快照写成控制台用的 JSON。
 * 进程入口在 monitor_main.c；这里不碰共享内存，测试可以直接喂结构体。
 */
#include <wellcooked/monitor.h>
#include <wellcooked/json.h>

static int put_string(json_writer *w, const char *key, const char *value)
{
    return json_writer_key(w, key) || json_writer_string(w, value ? value : "");
}

static int put_int(json_writer *w, const char *key, long long value)
{
    return json_writer_key(w, key) || json_writer_int(w, value);
}

static int put_double(json_writer *w, const char *key, double value)
{
    return json_writer_key(w, key) || json_writer_double(w, value);
}

static int put_flag(json_writer *w, const char *key, int has, int value)
{
    if (json_writer_key(w, key)) {
        return -1;
    }
    return has ? json_writer_bool(w, value) : json_writer_null(w);
}

static int put_names(json_writer *w, const char *key, const char names[][KOS_SHM_INGREDIENT_MAX],
                     int32_t count, int32_t max)
{
    int32_t i;

    if (count > max) {
        count = max;
    }
    if (json_writer_key(w, key) || json_writer_array_begin(w)) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (json_writer_string(w, names[i])) {
            return -1;
        }
    }
    return json_writer_array_end(w);
}

static int put_chef(json_writer *w, const kos_chef *c)
{
    return json_writer_object_begin(w) || put_int(w, "id", c->id) || put_double(w, "x", c->x) ||
           put_double(w, "z", c->y) || put_double(w, "facing", c->facing) ||
           put_string(w, "holding", c->holding) ||
           put_names(w, "holding_contents", c->holding_contents, c->holding_content_count,
                     KOS_SHM_MAX_CONTENTS) ||
           put_string(w, "state", c->state) || json_writer_object_end(w);
}

static int put_order(json_writer *w, const kos_order *o)
{
    return json_writer_object_begin(w) || put_string(w, "id", o->id) ||
           put_string(w, "dish", o->recipe) || put_int(w, "remaining_s", o->time_remaining) ||
           put_int(w, "score", o->score) ||
           put_names(w, "ingredients", o->ingredients, o->ingredient_count,
                     KOS_SHM_MAX_ORDER_INGREDIENTS) ||
           json_writer_object_end(w);
}

static int put_station(json_writer *w, const kos_resource *r)
{
    if (json_writer_object_begin(w) || put_string(w, "id", r->name) ||
        put_string(w, "kind", r->kind) || put_string(w, "state", r->state) ||
        put_string(w, "item", r->item) || put_string(w, "step", r->step) ||
        put_names(w, "contents", r->contents, r->content_count, KOS_SHM_MAX_CONTENTS) ||
        put_double(w, "progress", r->progress) || put_double(w, "time_left", r->time_left) ||
        put_int(w, "chef_id", r->chef_id) || put_int(w, "count", r->count) ||
        put_flag(w, "cooked", r->has_cooked, r->cooked) ||
        put_flag(w, "burning", r->has_burning, r->burning) ||
        put_flag(w, "fire", r->has_fire, r->fire) || json_writer_key(w, "burn_in") ||
        (r->has_burn_in ? json_writer_double(w, r->burn_in) : json_writer_null(w))) {
        return -1;
    }
    if (r->has_pos) {
        if (put_double(w, "x", r->pos_x) || put_double(w, "z", r->pos_y)) {
            return -1;
        }
    }
    return json_writer_object_end(w);
}

static int put_result(json_writer *w, const kos_order_result *r)
{
    if (json_writer_object_begin(w) || put_int(w, "seq", r->seq) || put_string(w, "id", r->id) ||
        put_string(w, "dish", r->recipe) || put_string(w, "outcome", r->outcome) ||
        put_int(w, "t_ms", r->game_time_ms) || json_writer_key(w, "score") ||
        (r->has_score ? json_writer_int(w, r->score) : json_writer_null(w))) {
        return -1;
    }
    return json_writer_object_end(w);
}

int kos_monitor_snapshot_json(const kos_world_state *world, char *buf, size_t cap,
                              size_t *len_out)
{
    json_writer w;
    uint32_t i, n;

    if (!world || !buf || json_writer_init(&w, buf, cap) < 0) {
        return -1;
    }
    if (json_writer_object_begin(&w) || json_writer_key(&w, "ok") || json_writer_bool(&w, 1) ||
        put_int(&w, "t_ms", world->game_time_ms) || put_int(&w, "score", world->score) ||
        json_writer_key(&w, "timer")) {
        return -1;
    }
    if (world->has_round_time) {
        if (json_writer_object_begin(&w) || put_double(&w, "left_s", world->round_time_left) ||
            put_double(&w, "limit_s", world->round_time_limit) || json_writer_object_end(&w)) {
            return -1;
        }
    } else if (json_writer_null(&w)) {
        return -1;
    }

    if (json_writer_key(&w, "chefs") || json_writer_array_begin(&w)) {
        return -1;
    }
    n = world->chef_count > KOS_SHM_MAX_CHEFS ? KOS_SHM_MAX_CHEFS : world->chef_count;
    for (i = 0; i < n; i++) {
        if (put_chef(&w, &world->chefs[i])) {
            return -1;
        }
    }
    if (json_writer_array_end(&w) || json_writer_key(&w, "orders") || json_writer_array_begin(&w)) {
        return -1;
    }
    n = world->order_count > KOS_SHM_MAX_ORDERS ? KOS_SHM_MAX_ORDERS : world->order_count;
    for (i = 0; i < n; i++) {
        if (put_order(&w, &world->orders[i])) {
            return -1;
        }
    }
    if (json_writer_array_end(&w) || json_writer_key(&w, "stations") ||
        json_writer_array_begin(&w)) {
        return -1;
    }
    n = world->resource_count > KOS_SHM_MAX_RESOURCES ? KOS_SHM_MAX_RESOURCES
                                                       : world->resource_count;
    for (i = 0; i < n; i++) {
        if (put_station(&w, &world->resources[i])) {
            return -1;
        }
    }
    if (json_writer_array_end(&w) || json_writer_key(&w, "results") ||
        json_writer_array_begin(&w)) {
        return -1;
    }
    n = world->order_result_count > KOS_SHM_MAX_ORDER_RESULTS ? KOS_SHM_MAX_ORDER_RESULTS
                                                               : world->order_result_count;
    for (i = 0; i < n; i++) {
        if (put_result(&w, &world->order_results[i])) {
            return -1;
        }
    }
    if (json_writer_array_end(&w) || json_writer_object_end(&w) || json_writer_error(&w)) {
        return -1;
    }
    if (len_out) {
        *len_out = json_writer_length(&w);
    }
    return 0;
}
