/*
 * state.c -- wire message -> shared-memory snapshot (see state.h).
 */
#include <kitchenos/state.h>

#include <stdint.h>
#include <string.h>

/* Bounded string copy that always NUL-terminates. The shm layout uses fixed
 * char arrays, and a frame from the game may carry arbitrarily long strings, so
 * truncation here is normal and must not be a buffer overrun. */
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

int kos_state_double_is_finite(double v)
{
    /* Deliberately not isfinite(): that is a macro from math.h and pulls the
     * -lm question into a program that otherwise needs no math library. */
    return v == v && v <= 1.0e308 && v >= -1.0e308;
}

int64_t kos_state_double_to_ll(double v)
{
    if (!kos_state_double_is_finite(v)) {
        return 0;
    }
    if (v >= 9.0e18) {
        return INT64_MAX;
    }
    if (v <= -9.0e18) {
        return INT64_MIN;
    }
    return (int64_t)(v < 0 ? v - 0.5 : v + 0.5);
}

float kos_state_double_to_float(double v)
{
    if (!kos_state_double_is_finite(v)) {
        return 0.0f;
    }
    if (v >= 3.0e38) {
        return 3.0e38f;
    }
    if (v <= -3.0e38) {
        return -3.0e38f;
    }
    return (float)v;
}

static size_t capped(size_t value, size_t max)
{
    return value > max ? max : value;
}

static void copy_contents(char dst[][KOS_SHM_INGREDIENT_MAX], size_t cap,
                          const char src[][PROTO_KEY_MAX], size_t count)
{
    size_t n = capped(count, KOS_SHM_MAX_CONTENTS);
    size_t j;

    if (n > cap) {
        n = cap;
    }
    for (j = 0; j < n; j++) {
        copy_str(dst[j], KOS_SHM_INGREDIENT_MAX, src[j]);
    }
}

/* 截断只在数量变化时打一行：每帧一条会把日志冲掉，而 0.25 s 重建的资源块
 * 本来就可能忽多忽少——变化时打一次足够看出"哪一帧开始丢 carried_*"。 */
static size_t g_resources_dropped_logged;

void kos_state_from_update(const proto_state_update *up, kos_world_state *world)
{
    kos_state_from_update_logged(up, world, NULL);
}

void kos_state_from_update_logged(const proto_state_update *up, kos_world_state *world,
                                  kos_log *log)
{
    size_t chefs;
    size_t orders;
    size_t resources;
    size_t results;
    size_t i;
    size_t k;

    if (up == NULL || world == NULL) {
        return;
    }

    kos_world_state_reset(world);
    world->score = kos_state_double_to_ll(up->score);
    world->game_time = kos_state_double_to_ll(up->game_time);
    world->game_time_ms = up->game_time_ms;
    if (up->has_round_time) {
        world->round_time_left = kos_state_double_to_float(up->round_time_left);
        world->round_time_limit = kos_state_double_to_float(up->round_time_limit);
        world->has_round_time = 1;
    }

    chefs = capped(up->chef_count, KOS_SHM_MAX_CHEFS);
    world->chef_count = (uint32_t)chefs;
    for (i = 0; i < chefs; i++) {
        const proto_chef *src = &up->chefs[i];
        kos_chef *dst = &world->chefs[i];

        dst->id = (int32_t)src->id;
        dst->x = kos_state_double_to_float(src->x);
        dst->y = kos_state_double_to_float(src->y);
        dst->facing = kos_state_double_to_float(src->facing);
        copy_str(dst->holding, sizeof dst->holding, src->holding);
        copy_str(dst->state, sizeof dst->state, src->state);
        copy_contents(dst->holding_contents, KOS_SHM_MAX_CONTENTS,
                      src->holding_contents, src->holding_content_count);
        dst->holding_content_count =
            (int32_t)capped(src->holding_content_count, KOS_SHM_MAX_CONTENTS);
        dst->facing_reported = src->facing_reported;
        dst->has_facing_target = src->has_facing_target;
        dst->facing_x = kos_state_double_to_float(src->facing_x);
        dst->facing_y = kos_state_double_to_float(src->facing_y);
    }

    orders = capped(up->order_count, KOS_SHM_MAX_ORDERS);
    world->order_count = (uint32_t)orders;
    for (i = 0; i < orders; i++) {
        const proto_order *src = &up->orders[i];
        kos_order *dst = &world->orders[i];

        copy_str(dst->id, sizeof dst->id, src->id);
        copy_str(dst->recipe, sizeof dst->recipe, src->recipe);
        dst->time_remaining = (int32_t)kos_state_double_to_ll(src->time_remaining);
        dst->score = src->has_score ? (int32_t)src->score : 0;
        k = capped(src->ingredient_count, KOS_SHM_MAX_ORDER_INGREDIENTS);
        dst->ingredient_count = (int32_t)k;
        for (size_t j = 0; j < k; j++) {
            copy_str(dst->ingredients[j], sizeof dst->ingredients[j], src->ingredients[j]);
        }
    }

    results = capped(up->order_result_count, KOS_SHM_MAX_ORDER_RESULTS);
    world->order_result_count = (uint32_t)results;
    for (i = 0; i < results; i++) {
        const proto_order_result *src = &up->order_results[i];
        kos_order_result *dst = &world->order_results[i];

        dst->seq = src->seq;
        copy_str(dst->id, sizeof dst->id, src->id);
        copy_str(dst->recipe, sizeof dst->recipe, src->recipe);
        copy_str(dst->outcome, sizeof dst->outcome, src->outcome);
        dst->score = src->has_score ? (int32_t)src->score : 0;
        dst->has_score = src->has_score;
        dst->game_time_ms = src->game_time_ms;
    }

    /* resources are keyed by name on the wire (pot_0, cutting_board_1,
     * plate_stack, and whatever the game grows later); the layout keeps the key
     * verbatim in `name` and classifies by prefix only, so an unknown key is
     * carried through instead of rejected.
     *
     * The plugin reports in a fixed order and `carried_<M>` is near the end, so a
     * layout cap below the protocol cap silently drops exactly the thing the pour
     * step needs. 96 == PROTO_MAX_RESOURCES now; if the two ever drift apart again
     * the caller must still see it, hence kos_state_resources_dropped(). */
    resources = capped(up->resource_count, KOS_SHM_MAX_RESOURCES);
    world->resource_count = (uint32_t)resources;
    if (up->resource_count > KOS_SHM_MAX_RESOURCES) {
        size_t dropped = up->resource_count - KOS_SHM_MAX_RESOURCES;

        if (dropped != g_resources_dropped_logged) {
            g_resources_dropped_logged = dropped;
            kos_log_warn(log, "resource_truncated",
                         "have=%zu kept=%d dropped=%zu carried_may_be_missing=1",
                         up->resource_count, (int)KOS_SHM_MAX_RESOURCES, dropped);
        }
    } else {
        g_resources_dropped_logged = 0;
    }
    for (i = 0; i < resources; i++) {
        const proto_resource *src = &up->resources[i];
        kos_resource *dst = &world->resources[i];

        copy_str(dst->name, sizeof dst->name, src->key);
        copy_str(dst->state, sizeof dst->state, src->state);
        copy_str(dst->content, sizeof dst->content, src->content);
        dst->time_left = src->has_time_left ? kos_state_double_to_float(src->time_left) : 0.0f;
        dst->chef_id = src->has_chef_id ? (int32_t)src->chef_id : -1;
        dst->count = src->has_count ? (int32_t)src->count : 0;
        dst->progress = src->has_progress ? kos_state_double_to_float(src->progress) : 0.0f;
        copy_str(dst->kind, sizeof dst->kind, src->kind_name);
        copy_str(dst->item, sizeof dst->item, src->item);
        copy_str(dst->step, sizeof dst->step, src->step);
        if (src->has_pos) {
            dst->pos_x = kos_state_double_to_float(src->x);
            dst->pos_y = kos_state_double_to_float(src->y);
            dst->has_pos = 1;
        }
        copy_contents(dst->contents, KOS_SHM_MAX_CONTENTS,
                      src->contents, src->content_count);
        dst->content_count = (int32_t)capped(src->content_count, KOS_SHM_MAX_CONTENTS);
        /* Cooking flags: keep "reported false" and "not reported" apart. */
        dst->has_cooked = src->has_cooked;
        dst->cooked = src->has_cooked ? (src->cooked != 0) : 0;
        dst->has_burning = src->has_burning;
        dst->burning = src->has_burning ? (src->burning != 0) : 0;
        dst->has_burn_in = src->has_burn_in;
        dst->burn_in = src->has_burn_in ? kos_state_double_to_float(src->burn_in) : 0.0f;
        dst->has_fire = src->has_fire;
        dst->fire = src->has_fire ? (src->fire != 0) : 0;
    }
}

size_t kos_state_resources_dropped(const proto_state_update *up)
{
    if (up == NULL || up->resource_count <= KOS_SHM_MAX_RESOURCES) {
        return 0;
    }
    return up->resource_count - KOS_SHM_MAX_RESOURCES;
}
