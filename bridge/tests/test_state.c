/*
 * test_state.c -- wire message -> shared-memory snapshot (state.c).
 *
 * bridge.c owns main(), so the mapping lives in its own module and is tested
 * here: parse a `state_update` exactly as the plugin sends it, run
 * kos_state_from_update(), then check the fixed shm layout. This is the layer
 * where protocol caps (PROTO_MAX_*) become shm caps (KOS_SHM_MAX_*); a silent
 * truncation here would look like a state loss to the scheduler.
 */
#include <kitchenos/json.h>
#include <kitchenos/protocol.h>
#include <kitchenos/shm.h>
#include <kitchenos/state.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        g_checks++;                                                     \
        if (!(cond)) {                                                  \
            g_failures++;                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                               \
    } while (0)

#define CHECK_INT(actual, expected)                                          \
    do {                                                                     \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        g_checks++;                                                          \
        if (a_ != e_) {                                                      \
            g_failures++;                                                    \
            printf("  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__,    \
                   __LINE__, #actual, a_, e_);                               \
        }                                                                    \
    } while (0)

#define CHECK_STR(actual, expected)                                          \
    do {                                                                     \
        const char *a_ = (actual);                                           \
        const char *e_ = (expected);                                         \
        g_checks++;                                                          \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                             \
            g_failures++;                                                    \
            printf("  FAIL %s:%d: %s == \"%s\", expected \"%s\"\n",          \
                   __FILE__, __LINE__, #actual, a_ == NULL ? "(null)" : a_,  \
                   e_);                                                      \
        }                                                                    \
    } while (0)

#define CHECK_DBL(actual, expected)                                          \
    do {                                                                     \
        double a_ = (actual);                                                \
        double e_ = (expected);                                              \
        g_checks++;                                                          \
        if (a_ != e_) {                                                      \
            g_failures++;                                                    \
            printf("  FAIL %s:%d: %s == %f, expected %f\n", __FILE__,        \
                   __LINE__, #actual, a_, e_);                               \
        }                                                                    \
    } while (0)

static int parse_and_map(const char *text, size_t len, proto_state_update *up,
                         kos_world_state *world)
{
    json_value *root = NULL;
    json_error err;
    int rc;

    if (json_parse_ex(text, len, &root, &err) != JSON_OK) {
        g_failures++;
        printf("  FAIL cannot parse fixture at offset %lu: %s\n",
               (unsigned long)err.offset, err.detail);
        return -1;
    }
    rc = protocol_parse_state_update(root, up, NULL);
    if (rc == PROTO_OK) {
        kos_state_from_update(up, world);
    }
    json_free(root);
    return rc;
}

static const kos_resource *shm_resource(const kos_world_state *world, const char *name)
{
    size_t i;

    for (i = 0; i < world->resource_count; i++) {
        if (strcmp(world->resources[i].name, name) == 0)
            return &world->resources[i];
    }
    return NULL;
}

static void test_mapping_basic(void)
{
    static const char sample[] =
        "{\"type\":\"state_update\",\"timestamp\":9,\"game_time\":12,\"game_time_ms\":12000,"
        "\"score\":80,\"round_time_left\":42,\"round_time_limit\":120,"
        "\"chefs\":[{\"id\":2,\"position\":{\"x\":1.5,\"y\":2.5},\"facing\":1.25,"
        "\"holding\":\"utensil_pot_01\","
        "\"holding_contents\":[\"Rice\",\"cooked:BurritoChoppedChicken\"]},"
        "{\"id\":3,\"position\":{\"x\":0,\"y\":0}}],"
        "\"orders\":[{\"id\":\"order_001\",\"recipe\":\"Chicken_Burrito\","
        "\"time_remaining\":88.4,\"score\":80},"
        "{\"id\":\"order_002\",\"recipe\":\"Sushi_PlainFish\",\"time_remaining\":90}],"
        "\"order_results\":["
        "{\"seq\":7,\"id\":\"order_001\",\"recipe\":\"Chicken_Burrito\","
        "\"outcome\":\"delivered\",\"score\":80,\"game_time_ms\":4900},"
        "{\"seq\":8,\"id\":\"order_002\",\"outcome\":\"expired\"}],"
        "\"resources\":{"
        "\"pot_4\":{\"kind\":\"cooker\",\"pos\":{\"x\":6.6,\"y\":-6.0},"
        "\"state\":\"cooking\",\"item\":\"utensil_pot_01\",\"step\":\"Pot\","
        "\"progress\":0.25,\"time_left\":9,\"contents\":[\"Rice\"],\"content\":\"Rice\"},"
        "\"counter_0\":{\"kind\":\"counter\",\"pos\":{\"x\":0.6,\"y\":-6.0},"
        "\"state\":\"occupied\",\"item\":\"Tortilla\","
        "\"contents\":[\"cooked:BurritoChoppedChicken\",\"cooked:Rice\"]},"
        "\"container_3\":{\"kind\":\"container\",\"state\":\"empty\","
        "\"item\":\"equipment_plate_01\",\"contents\":[]},"
        "\"mystery\":{\"state\":\"free\"}"
        "}}";
    proto_state_update up;
    kos_world_state world;
    const kos_resource *pot;
    const kos_resource *counter;
    const kos_resource *container;
    const kos_resource *mystery;

    printf("[test_state] 基本映射：厨师/订单/结果/资源新字段\n");
    CHECK_INT(parse_and_map(sample, sizeof sample - 1, &up, &world), PROTO_OK);

    CHECK_INT(world.score, 80);
    CHECK_INT(world.game_time, 12);
    CHECK_INT(world.game_time_ms, 12000);
    CHECK_INT(world.has_round_time, 1);
    CHECK_DBL(world.round_time_left, 42.0);
    CHECK_DBL(world.round_time_limit, 120.0);

    CHECK_INT(world.chef_count, 2);
    CHECK_INT(world.chefs[0].id, 2);
    CHECK_DBL(world.chefs[0].x, 1.5);
    CHECK_DBL(world.chefs[0].y, 2.5);
    CHECK_DBL(world.chefs[0].facing, 1.25);
    CHECK_STR(world.chefs[0].holding, "utensil_pot_01");
    CHECK_INT(world.chefs[0].holding_content_count, 2);
    CHECK_STR(world.chefs[0].holding_contents[0], "Rice");
    CHECK_STR(world.chefs[0].holding_contents[1], "cooked:BurritoChoppedChicken");
    CHECK_INT(world.chefs[1].holding_content_count, 0);
    CHECK_STR(world.chefs[1].holding, "");

    CHECK_INT(world.order_count, 2);
    CHECK_STR(world.orders[0].id, "order_001");
    CHECK_INT(world.orders[0].time_remaining, 88);
    CHECK_INT(world.orders[0].score, 80);
    CHECK_INT(world.orders[1].score, 0); /* 没上报与 0 同形，插件总是报 score */

    CHECK_INT(world.order_result_count, 2);
    CHECK_INT(world.order_results[0].seq, 7);
    CHECK_STR(world.order_results[0].id, "order_001");
    CHECK_STR(world.order_results[0].recipe, "Chicken_Burrito");
    CHECK_STR(world.order_results[0].outcome, "delivered");
    CHECK_INT(world.order_results[0].has_score, 1);
    CHECK_INT(world.order_results[0].score, 80);
    CHECK_INT(world.order_results[0].game_time_ms, 4900);
    CHECK_INT(world.order_results[1].seq, 8);
    CHECK_STR(world.order_results[1].outcome, "expired");
    CHECK_INT(world.order_results[1].has_score, 0);

    CHECK_INT(world.resource_count, 4);
    pot = shm_resource(&world, "pot_4");
    counter = shm_resource(&world, "counter_0");
    container = shm_resource(&world, "container_3");
    mystery = shm_resource(&world, "mystery");
    CHECK(pot != NULL && counter != NULL && container != NULL && mystery != NULL);

    if (pot != NULL) {
        CHECK_STR(pot->kind, "cooker");
        CHECK_STR(pot->state, "cooking");
        CHECK_STR(pot->item, "utensil_pot_01");
        CHECK_STR(pot->step, "Pot");
        CHECK_INT(pot->has_pos, 1);
        CHECK_DBL(pot->pos_x, 6.6f);
        CHECK_DBL(pot->pos_y, (double)-6.0f);
        CHECK_INT(pot->content_count, 1);
        CHECK_STR(pot->contents[0], "Rice");
        CHECK_STR(pot->content, "Rice");
        CHECK_DBL(pot->progress, 0.25);
        CHECK_DBL(pot->time_left, 9.0);
        CHECK_INT(pot->chef_id, -1); /* 没上报占用者 */
    }
    if (counter != NULL) {
        CHECK_STR(counter->kind, "counter");
        CHECK_INT(counter->content_count, 2);
        CHECK_STR(counter->contents[0], "cooked:BurritoChoppedChicken");
        CHECK_STR(counter->contents[1], "cooked:Rice");
        CHECK_INT(counter->has_pos, 1);
        CHECK_INT(counter->count, 0);
    }
    if (container != NULL) {
        CHECK_INT(container->content_count, 0);
        CHECK_INT(container->has_pos, 0); /* 没位置就别假装在 (0,0) */
        CHECK_DBL(container->pos_x, 0.0);
    }
    if (mystery != NULL) {
        CHECK_STR(mystery->kind, ""); /* 未知键原样保留 */
        CHECK_INT(mystery->has_pos, 0);
    }
}

/* Protocol caps are wider than the shm layout; the mapping truncates and keeps
 * the counts self-consistent. Tests the exact boundary of every new array. */
static void test_mapping_caps(void)
{
    char sample[16384];
    size_t pos = 0;
    int i;
    proto_state_update up;
    kos_world_state world;

    printf("[test_state] 上限截断：资源 48 / 结果 16 / 内容 4\n");

    pos += (size_t)snprintf(sample + pos, sizeof sample - pos,
                            "{\"type\":\"state_update\",\"game_time_ms\":1,\"chefs\":["
                            "{\"id\":0,\"holding\":\"plate\",\"holding_contents\":"
                            "[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\"]}],"
                            "\"order_results\":[");
    for (i = 0; i < 18; i++) {
        pos += (size_t)snprintf(sample + pos, sizeof sample - pos,
                                "%s{\"seq\":%d,\"id\":\"order_%03d\",\"outcome\":\"delivered\"}",
                                i == 0 ? "" : ",", i + 1, i);
    }
    pos += (size_t)snprintf(sample + pos, sizeof sample - pos, "],\"resources\":{");
    for (i = 0; i < 60; i++) {
        pos += (size_t)snprintf(sample + pos, sizeof sample - pos,
                                "%s\"pot_%d\":{\"kind\":\"cooker\",\"pos\":{\"x\":%d,\"y\":0},"
                                "\"contents\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\"]}",
                                i == 0 ? "" : ",", i, i);
    }
    pos += (size_t)snprintf(sample + pos, sizeof sample - pos, "}}");
    CHECK(pos < sizeof sample);

    CHECK_INT(parse_and_map(sample, pos, &up, &world), PROTO_OK);
    CHECK_INT(up.resource_count, 60);
    CHECK_INT(up.order_result_count, 18);
    CHECK_INT(up.chefs[0].holding_content_count, 8);

    CHECK_INT(world.resource_count, KOS_SHM_MAX_RESOURCES);
    CHECK_INT(world.order_result_count, KOS_SHM_MAX_ORDER_RESULTS);
    CHECK_INT(world.chefs[0].holding_content_count, KOS_SHM_MAX_CONTENTS);
    CHECK_STR(world.chefs[0].holding_contents[3], "d"); /* 前 4 条保留 */
    CHECK_STR(world.resources[0].contents[3], "4");
    CHECK_INT(world.resources[0].content_count, KOS_SHM_MAX_CONTENTS);
    CHECK_STR(world.resources[47].name, "pot_47");       /* 第 48 个资源还在 */
    CHECK_INT(world.order_results[15].seq, 16);
}

static void test_mapping_defaults(void)
{
    static const char sample[] = "{\"type\":\"state_update\",\"game_time_ms\":123}";
    proto_state_update up;
    kos_world_state world;

    printf("[test_state] 缺省值：空厨房\n");
    CHECK_INT(parse_and_map(sample, sizeof sample - 1, &up, &world), PROTO_OK);
    CHECK_INT(world.chef_count, 0);
    CHECK_INT(world.order_count, 0);
    CHECK_INT(world.order_result_count, 0);
    CHECK_INT(world.resource_count, 0);
    CHECK_INT(world.has_round_time, 0);
    CHECK_DBL(world.round_time_left, 0.0);
    CHECK_INT(world.game_time_ms, 123);
}

static void test_numeric_helpers(void)
{
    printf("[test_state] 线上 double → shm 数值：NaN/越界\n");
    CHECK_INT(kos_state_double_is_finite(0.0), 1);
    CHECK_INT(kos_state_double_is_finite(NAN), 0);
    CHECK_INT(kos_state_double_is_finite(INFINITY), 0);
    CHECK_INT(kos_state_double_to_ll(NAN), 0);
    CHECK_INT(kos_state_double_to_ll(-2.5), -3);
    CHECK_INT(kos_state_double_to_ll(2.5), 3);
    CHECK(kos_state_double_to_float(INFINITY) == 0.0f);
}

int main(void)
{
    printf("test_state: state_update -> kos_world_state\n");

    test_mapping_basic();
    test_mapping_caps();
    test_mapping_defaults();
    test_numeric_helpers();

    printf("test_state: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
