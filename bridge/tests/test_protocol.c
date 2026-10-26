/*
 * test_protocol.c -- unit tests for the KitchenOS message layer.
 *
 * The state_update sample is copied verbatim from §5.2 of the project document,
 * because that sample plus the plugin's Messages.cs are the contract: if this
 * test still passes, the bridge understands what the game sends.
 */
#include <kitchenos/protocol.h>

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

/* §5.2 「状态上报」, byte for byte. */
static const char STATE_SAMPLE[] =
    "{\n"
    "  \"type\": \"state_update\",\n"
    "  \"timestamp\": 1234567890,\n"
    "  \"game_time\": 180,\n"
    "  \"game_time_ms\": 180000,\n"
    "  \"score\": 450,\n"
    "  \"chefs\": [\n"
    "    {\n"
    "      \"id\": 0,\n"
    "      \"position\": {\"x\": 10.5, \"y\": 5.2},\n"
    "      \"holding\": \"tomato\",\n"
    "      \"state\": \"moving\"\n"
    "    },\n"
    "    {\n"
    "      \"id\": 1,\n"
    "      \"position\": {\"x\": 3.0, \"y\": 8.0},\n"
    "      \"holding\": null,\n"
    "      \"state\": \"idle\"\n"
    "    }\n"
    "  ],\n"
    "  \"orders\": [\n"
    "    {\n"
    "      \"id\": \"order_001\",\n"
    "      \"recipe\": \"salad\",\n"
    "      \"time_remaining\": 45,\n"
    "      \"ingredients\": [\"lettuce\", \"tomato\"]\n"
    "    }\n"
    "  ],\n"
    "  \"resources\": {\n"
    "    \"pot_0\": {\"state\": \"occupied\", \"content\": \"soup\", \"time_left\": 10},\n"
    "    \"pot_1\": {\"state\": \"free\"},\n"
    "    \"cutting_board_0\": {\"state\": \"occupied\", \"chef_id\": 0},\n"
    "    \"plate_stack\": {\"count\": 5}\n"
    "  }\n"
    "}";

/* The same message from a future game build: a pot the sample never had, plus a
 * station kind nobody has invented yet. Dynamic keys must not break the parser. */
static const char STATE_FUTURE[] =
    "{\"type\":\"state_update\",\"timestamp\":2,\"game_time\":1,\"game_time_ms\":1000,\"score\":0,"
    "\"resources\":{\"pot_7\":{\"state\":\"occupied\",\"content\":\"soup\","
    "\"time_left\":3},\"sink_2\":{\"state\":\"dirty\"},\"plate_stack\":{\"count\":0}}}";

static const char *const HELLO_ACTIONS[] = { "move", "stop", "interact" };

static json_value *parse_ok(const char *text, size_t len)
{
    json_value *root = NULL;
    json_error err;

    if (json_parse_ex(text, len, &root, &err) != JSON_OK) {
        g_failures++;
        printf("  FAIL cannot parse fixture at offset %lu: %s\n",
               (unsigned long)err.offset, err.detail);
        return NULL;
    }
    return root;
}

/* Fixtures written inline are easier to read without a hand-counted length. */
#define PARSE_OK(text) parse_ok((text), strlen(text))

static const proto_resource *find_resource(const proto_state_update *state,
                                           const char *key)
{
    size_t i;

    for (i = 0; i < state->resource_count; i++) {
        if (strcmp(state->resources[i].key, key) == 0)
            return &state->resources[i];
    }
    return NULL;
}

static void app(char *buf, size_t cap, size_t *pos, const char *text)
{
    size_t n = strlen(text);

    if (*pos + n + 1 > cap) {
        g_failures++;
        printf("  FAIL test fixture buffer overflow\n");
        return;
    }
    memcpy(buf + *pos, text, n + 1);
    *pos += n;
}

static void app_int(char *buf, size_t cap, size_t *pos, int value)
{
    char tmp[32];

    snprintf(tmp, sizeof tmp, "%d", value);
    app(buf, cap, pos, tmp);
}

static void test_state_update_sample(void)
{
    json_value *root = parse_ok(STATE_SAMPLE, sizeof STATE_SAMPLE - 1);
    proto_state_update state;
    proto_diag diag;
    const proto_resource *pot0;
    const proto_resource *pot1;
    const proto_resource *board;
    const proto_resource *plates;

    CHECK(root != NULL);
    if (root == NULL)
        return;

    memset(&diag, 0, sizeof diag);
    CHECK_INT(protocol_message_type(root), PROTO_MSG_STATE_UPDATE);
    CHECK_INT(protocol_parse_state_update(root, &state, &diag), PROTO_OK);
    CHECK_INT(diag.dropped_items, 0);
    CHECK_INT(diag.truncated_strings, 0);

    CHECK_INT(state.timestamp, 1234567890LL);
    CHECK_DBL(state.game_time, 180.0);
    CHECK_INT(state.game_time_ms, 180000LL);
    CHECK_DBL(state.score, 450.0);

    CHECK_INT(state.chef_count, 2);
    CHECK_INT(state.chefs[0].id, 0);
    CHECK_DBL(state.chefs[0].x, 10.5);
    CHECK_DBL(state.chefs[0].y, 5.2);
    CHECK_STR(state.chefs[0].holding, "tomato");
    CHECK_STR(state.chefs[0].state, "moving");
    CHECK_INT(state.chefs[1].id, 1);
    CHECK_DBL(state.chefs[1].x, 3.0);
    CHECK_DBL(state.chefs[1].y, 8.0);
    CHECK_STR(state.chefs[1].holding, ""); /* JSON null becomes "carries nothing" */
    CHECK_STR(state.chefs[1].state, "idle");

    CHECK_INT(state.order_count, 1);
    CHECK_STR(state.orders[0].id, "order_001");
    CHECK_STR(state.orders[0].recipe, "salad");
    CHECK_DBL(state.orders[0].time_remaining, 45.0);
    CHECK_INT(state.orders[0].ingredient_count, 2);
    CHECK_STR(state.orders[0].ingredients[0], "lettuce");
    CHECK_STR(state.orders[0].ingredients[1], "tomato");

    CHECK_INT(state.resource_count, 4);
    pot0 = find_resource(&state, "pot_0");
    pot1 = find_resource(&state, "pot_1");
    board = find_resource(&state, "cutting_board_0");
    plates = find_resource(&state, "plate_stack");
    CHECK(pot0 != NULL && pot1 != NULL && board != NULL && plates != NULL);
    if (pot0 != NULL) {
        CHECK_INT(pot0->kind, PROTO_RES_POT);
        CHECK_INT(pot0->index, 0);
        CHECK_STR(pot0->state, "occupied");
        CHECK_STR(pot0->content, "soup");
        CHECK_INT(pot0->has_time_left, 1);
        CHECK_DBL(pot0->time_left, 10.0);
        CHECK_INT(pot0->has_count, 0);
    }
    if (pot1 != NULL) {
        CHECK_STR(pot1->state, "free");
        CHECK_INT(pot1->has_time_left, 0);
        CHECK_STR(pot1->content, "");
    }
    if (board != NULL) {
        CHECK_INT(board->kind, PROTO_RES_CUTTING_BOARD);
        CHECK_INT(board->index, 0);
        CHECK_INT(board->has_chef_id, 1);
        CHECK_INT(board->chef_id, 0);
    }
    if (plates != NULL) {
        CHECK_INT(plates->kind, PROTO_RES_PLATE_STACK);
        CHECK_INT(plates->index, -1);
        CHECK_INT(plates->has_count, 1);
        CHECK_INT(plates->count, 5);
    }

    json_free(root);
}

/* facing / progress / dirty_plates / sink progress are additive optional fields;
 * this locks their wire names to the C structs the rest of the system reads. */
static void test_state_new_fields(void)
{
    static const char sample[] =
        "{\"type\":\"state_update\",\"timestamp\":7,\"game_time\":2,\"game_time_ms\":2345,"
        "\"score\":0,"
        "\"chefs\":[{\"id\":0,\"position\":{\"x\":1,\"y\":2},\"facing\":-1.25}],"
        "\"resources\":{"
        "\"cutting_board_0\":{\"state\":\"occupied\",\"content\":\"fish\",\"progress\":0.5},"
        "\"dirty_plates\":{\"state\":\"occupied\",\"count\":3},"
        "\"sink_0\":{\"state\":\"washing\",\"count\":2,\"progress\":0.75}}}";
    json_value *root = parse_ok(sample, sizeof sample - 1);
    proto_state_update state;
    const proto_resource *board;
    const proto_resource *dirty;
    const proto_resource *sink;

    printf("[test_protocol] 新增状态字段：facing / progress / dirty_plates / sink\n");
    CHECK(root != NULL);
    if (root == NULL)
        return;

    CHECK_INT(protocol_parse_state_update(root, &state, NULL), PROTO_OK);
    CHECK_INT(state.game_time_ms, 2345LL);
    CHECK_INT(state.chef_count, 1);
    CHECK_DBL(state.chefs[0].facing, -1.25);

    board = find_resource(&state, "cutting_board_0");
    dirty = find_resource(&state, "dirty_plates");
    sink = find_resource(&state, "sink_0");
    CHECK(board != NULL && dirty != NULL && sink != NULL);
    if (board != NULL) {
        CHECK_INT(board->has_progress, 1);
        CHECK_DBL(board->progress, 0.5);
        CHECK_STR(board->content, "fish");
    }
    if (dirty != NULL) {
        CHECK_INT(dirty->kind, PROTO_RES_DIRTY_PLATES);
        CHECK_INT(dirty->index, -1);
        CHECK_INT(dirty->has_count, 1);
        CHECK_INT(dirty->count, 3);
        CHECK_INT(dirty->has_progress, 0);
    }
    if (sink != NULL) {
        CHECK_INT(sink->kind, PROTO_RES_SINK);
        CHECK_INT(sink->index, 0);
        CHECK_INT(sink->has_progress, 1);
        CHECK_DBL(sink->progress, 0.75);
        CHECK_INT(sink->count, 2);
    }
    json_free(root);
}

static void test_dynamic_resources(void)
{
    json_value *root = parse_ok(STATE_FUTURE, sizeof STATE_FUTURE - 1);
    proto_state_update state;
    const proto_resource *pot7;
    const proto_resource *sink;
    const proto_resource *plates;

    CHECK(root != NULL);
    if (root == NULL)
        return;

    CHECK_INT(protocol_parse_state_update(root, &state, NULL), PROTO_OK);
    CHECK_INT(state.chef_count, 0); /* an empty kitchen is a valid kitchen */
    CHECK_INT(state.resource_count, 3);

    pot7 = find_resource(&state, "pot_7");
    sink = find_resource(&state, "sink_2");
    plates = find_resource(&state, "plate_stack");
    CHECK(pot7 != NULL && sink != NULL && plates != NULL);
    if (pot7 != NULL) {
        CHECK_INT(pot7->kind, PROTO_RES_POT);
        CHECK_INT(pot7->index, 7); /* the key is parsed, not matched against a list */
        CHECK_STR(pot7->content, "soup");
        CHECK_DBL(pot7->time_left, 3.0);
    }
    if (sink != NULL) {
        CHECK_INT(sink->kind, PROTO_RES_SINK);
        CHECK_INT(sink->index, 2);
        CHECK_STR(sink->key, "sink_2");
        CHECK_STR(sink->state, "dirty");
    }
    if (plates != NULL) {
        CHECK_INT(plates->has_count, 1);
        CHECK_INT(plates->count, 0); /* an empty plate stack is not "missing" */
    }

    json_free(root);
}

/* 任务 09 G1–G9：灶台槽位、递归内容、手持内容、每单分值、回合时钟、订单结果。 */
static void test_state_audit_fields(void)
{
    static const char sample[] =
        "{\"type\":\"state_update\",\"game_time_ms\":5000,\"score\":80,"
        "\"round_time_left\":42,\"round_time_limit\":120,"
        "\"chefs\":[{\"id\":2,\"position\":{\"x\":1,\"y\":2},\"holding\":\"utensil_pot_01\","
        "\"holding_contents\":[\"Rice\",\"cooked:BurritoChoppedChicken\"]}],"
        "\"orders\":[{\"id\":\"order_001\",\"recipe\":\"Chicken_Burrito\","
        "\"time_remaining\":88,\"score\":80}],"
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
        "\"carried_2\":{\"kind\":\"carried\",\"chef_id\":2,\"pos\":{\"x\":1.0,\"y\":-5.5},"
        "\"state\":\"carried\",\"item\":\"utensil_frying_pan_01\","
        "\"contents\":[\"BurritoChicken\"]},"
        "\"container_3\":{\"kind\":\"container\",\"pos\":{\"x\":-1,\"y\":-1},"
        "\"state\":\"empty\",\"item\":\"equipment_plate_01\",\"contents\":[]},"
        "\"plate_stack\":{\"kind\":\"plate_stack\",\"count\":4,\"pos\":{\"x\":3,\"y\":1.2}},"
        "\"mystery\":{\"state\":\"free\"}"
        "}}";
    json_value *root = parse_ok(sample, sizeof sample - 1);
    proto_state_update state;
    const proto_resource *pot;
    const proto_resource *counter;
    const proto_resource *carried;
    const proto_resource *container;
    const proto_resource *mystery;

    printf("[test_protocol] 任务 09 状态字段：锅槽位 / 内容递归 / 订单结果\n");
    CHECK(root != NULL);
    if (root == NULL)
        return;

    CHECK_INT(protocol_parse_state_update(root, &state, NULL), PROTO_OK);
    CHECK_INT(state.has_round_time, 1);
    CHECK_DBL(state.round_time_left, 42.0);
    CHECK_DBL(state.round_time_limit, 120.0);

    CHECK_INT(state.chef_count, 1);
    CHECK_INT(state.chefs[0].holding_content_count, 2);
    CHECK_STR(state.chefs[0].holding_contents[0], "Rice");
    CHECK_STR(state.chefs[0].holding_contents[1], "cooked:BurritoChoppedChicken");

    CHECK_INT(state.order_count, 1);
    CHECK_INT(state.orders[0].has_score, 1);
    CHECK_INT(state.orders[0].score, 80);

    CHECK_INT(state.order_result_count, 2);
    CHECK_INT(state.order_results[0].seq, 7);
    CHECK_STR(state.order_results[0].outcome, "delivered");
    CHECK_INT(state.order_results[0].has_score, 1);
    CHECK_INT(state.order_results[0].score, 80);
    CHECK_INT(state.order_results[0].game_time_ms, 4900);
    CHECK_INT(state.order_results[1].seq, 8);
    CHECK_STR(state.order_results[1].outcome, "expired");
    CHECK_INT(state.order_results[1].has_score, 0);

    pot = find_resource(&state, "pot_4");
    counter = find_resource(&state, "counter_0");
    carried = find_resource(&state, "carried_2");
    container = find_resource(&state, "container_3");
    mystery = find_resource(&state, "mystery");
    CHECK(pot != NULL && counter != NULL && carried != NULL && container != NULL
          && mystery != NULL);
    if (pot != NULL) {
        CHECK_INT(pot->kind, PROTO_RES_POT);
        CHECK_INT(pot->index, 4);
        CHECK_STR(pot->kind_name, "cooker");
        CHECK_INT(pot->has_pos, 1);
        CHECK_DBL(pot->x, 6.6);
        CHECK_DBL(pot->y, -6.0);
        CHECK_STR(pot->item, "utensil_pot_01");
        CHECK_STR(pot->step, "Pot");
        CHECK_INT(pot->content_count, 1);
        CHECK_STR(pot->contents[0], "Rice");
        CHECK_STR(pot->content, "Rice");
        CHECK_INT(pot->has_progress, 1);
        CHECK_INT(pot->has_time_left, 1);
    }
    if (counter != NULL) {
        CHECK_INT(counter->kind, PROTO_RES_COUNTER);
        CHECK_INT(counter->index, 0);
        CHECK_INT(counter->content_count, 2);
        CHECK_STR(counter->contents[0], "cooked:BurritoChoppedChicken");
        CHECK_STR(counter->item, "Tortilla");
    }
    if (carried != NULL) {
        CHECK_INT(carried->kind, PROTO_RES_CARRIED);
        CHECK_INT(carried->index, 2); /* carried_<chef_id> */
        CHECK_INT(carried->has_chef_id, 1);
        CHECK_INT(carried->chef_id, 2);
        CHECK_STR(carried->state, "carried");
    }
    if (container != NULL) {
        CHECK_INT(container->kind, PROTO_RES_CONTAINER);
        CHECK_INT(container->index, 3);
        CHECK_INT(container->content_count, 0); /* 明确的空数组，不是"少了字段" */
    }
    if (mystery != NULL) {
        CHECK_INT(mystery->kind, PROTO_RES_UNKNOWN); /* 认不出也要原样保留 */
        CHECK_INT(mystery->index, -1);
        CHECK_INT(mystery->has_pos, 0);
    }

    json_free(root);
}

/* 坏输入：半个位置是发送端 bug，必须拒绝整条消息；超量数组只丢超出部分。 */
static void test_state_field_errors(void)
{
    static const char bad_pos[] =
        "{\"type\":\"state_update\",\"game_time_ms\":1,"
        "\"resources\":{\"pot_0\":{\"pos\":{\"x\":1}}}}";
    static const char many[] =
        "{\"type\":\"state_update\",\"game_time_ms\":1,"
        "\"order_results\":[{\"id\":\"order_009\",\"outcome\":\"expired\"}],"
        "\"resources\":{\"counter_0\":{\"contents\":"
        "[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}}}";
    json_value *root;
    proto_state_update state;
    proto_diag diag;

    printf("[test_protocol] 任务 09 状态字段：坏位置 / 超量内容\n");

    root = parse_ok(bad_pos, sizeof bad_pos - 1);
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_state_update(root, &state, NULL), PROTO_ERR_RANGE);
        json_free(root);
    }

    root = parse_ok(many, sizeof many - 1);
    CHECK(root != NULL);
    if (root != NULL) {
        memset(&diag, 0, sizeof diag);
        CHECK_INT(protocol_parse_state_update(root, &state, &diag), PROTO_OK);
        CHECK_INT(state.resource_count, 1);
        CHECK_INT(state.resources[0].content_count, PROTO_MAX_CONTENTS);
        CHECK(diag.dropped_items >= 1);
        /* 缺 seq 的结果条目单独丢弃，不连累整条消息 */
        CHECK_INT(state.order_result_count, 0);
        CHECK(diag.dropped_items >= 2);
        json_free(root);
    }
}

static void test_heartbeat(void)
{
    char buf[128];
    size_t len = 0;
    json_value *root;
    proto_heartbeat beat;

    CHECK_INT(protocol_build_heartbeat(1234567890LL, buf, sizeof buf, &len), PROTO_OK);
    CHECK_STR(buf, "{\"type\":\"heartbeat\",\"timestamp\":1234567890}");
    CHECK_INT(len, strlen(buf));

    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_message_type(root), PROTO_MSG_HEARTBEAT);
        CHECK_INT(protocol_parse_heartbeat(root, &beat), PROTO_OK);
        CHECK_INT(beat.timestamp, 1234567890LL);
        json_free(root);
    }

    /* The builder must report a buffer it cannot fill instead of truncating. */
    CHECK_INT(protocol_build_heartbeat(1, buf, 8, &len), JSON_ERR_NOSPACE);
}

static void test_hello(void)
{
    char buf[256];
    size_t len = 0;
    json_value *root;

    CHECK_INT(protocol_build_hello(PROTO_BRIDGE_INSTANCE, PROTO_VERSION, HELLO_ACTIONS,
                                   3, buf, sizeof buf, &len),
              PROTO_OK);
    CHECK_STR(buf,
              "{\"type\":\"hello\",\"instance\":\"linux-bridge\",\"version\":\""
              PROTO_VERSION "\",\"actions\":[\"move\",\"stop\",\"interact\"]}");

    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_hello hello;
        proto_diag diag;

        memset(&diag, 0, sizeof diag);
        CHECK_INT(protocol_message_type(root), PROTO_MSG_HELLO);
        CHECK_INT(protocol_parse_hello(root, &hello, &diag), PROTO_OK);
        CHECK_STR(hello.instance, "linux-bridge");
        CHECK_STR(hello.version, PROTO_VERSION);
        CHECK_INT(hello.action_count, 3);
        CHECK_STR(hello.actions[0], "move");
        CHECK_STR(hello.actions[2], "interact");
        CHECK_INT(diag.dropped_items, 0);
        json_free(root);
    }
}

static void test_ack(void)
{
    char buf[256];
    size_t len = 0;
    json_value *root;

    CHECK_INT(protocol_build_ack("move", 1, 1, "ok", buf, sizeof buf, &len), PROTO_OK);
    CHECK_STR(buf,
              "{\"type\":\"ack\",\"action\":\"move\",\"chef_id\":1,\"ok\":true,"
              "\"detail\":\"ok\"}");

    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_ack ack;

        CHECK_INT(protocol_message_type(root), PROTO_MSG_ACK);
        CHECK_INT(protocol_parse_ack(root, &ack, NULL), PROTO_OK);
        CHECK_STR(ack.action, "move");
        CHECK_INT(ack.chef_id, 1);
        CHECK_INT(ack.ok, 1);
        CHECK_STR(ack.detail, "ok");
        json_free(root);
    }

    /* Rejection keeps the same shape, just ok=false. */
    CHECK_INT(protocol_build_ack("interact", 2, 0, "no such object", buf, sizeof buf,
                                 &len),
              PROTO_OK);
    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_ack ack;

        CHECK_INT(protocol_parse_ack(root, &ack, NULL), PROTO_OK);
        CHECK_INT(ack.ok, 0);
        CHECK_INT(ack.chef_id, 2);
        CHECK_INT(ack.has_seq, 0);
        CHECK_STR(ack.detail, "no such object");
        json_free(root);
    }

    /* path 的结局 ack 带规划序号；ack 的其它字段形状不变。 */
    root = PARSE_OK("{\"type\":\"ack\",\"action\":\"path\",\"chef_id\":3,\"ok\":true,"
                    "\"seq\":42,\"detail\":\"path 到达终点 seq=42\"}");
    CHECK(root != NULL);
    if (root != NULL) {
        proto_ack ack;

        CHECK_INT(protocol_parse_ack(root, &ack, NULL), PROTO_OK);
        CHECK_INT(ack.has_seq, 1);
        CHECK_INT(ack.seq, 42);
        CHECK_INT(ack.chef_id, 3);
        CHECK_STR(ack.action, "path");
        json_free(root);
    }
}

static void test_echo(void)
{
    static const char incoming[] = "{\"type\":\"echo\",\"token\":\"probe-42\"}";
    char buf[128];
    size_t len = 0;
    json_value *root = parse_ok(incoming, sizeof incoming - 1);
    proto_echo echo;

    CHECK(root != NULL);
    if (root == NULL)
        return;

    CHECK_INT(protocol_message_type(root), PROTO_MSG_ECHO);
    CHECK_INT(protocol_parse_echo(root, &echo), PROTO_OK);
    CHECK_STR(echo.token, "probe-42");
    CHECK_INT(echo.token_len, 8);

    CHECK_INT(protocol_build_echo_reply(echo.token, buf, sizeof buf, &len), PROTO_OK);
    CHECK_STR(buf, "{\"type\":\"echo_reply\",\"token\":\"probe-42\"}");
    json_free(root);

    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_echo reply;

        CHECK_INT(protocol_message_type(root), PROTO_MSG_ECHO_REPLY);
        CHECK_INT(protocol_parse_echo(root, &reply), PROTO_ERR_TYPE); /* type must match */
        json_free(root);
    }
}

static void test_config(void)
{
    static const char incoming[] =
        "{\"type\":\"config\",\"strategy\":\"EDF\",\"order_list\":["
        "{\"id\":\"order_001\",\"recipe\":\"salad\",\"arrival_time\":10,"
        "\"deadline\":60}]}";
    json_value *root = parse_ok(incoming, sizeof incoming - 1);
    proto_config config;
    proto_diag diag;

    CHECK(root != NULL);
    if (root == NULL)
        return;

    memset(&diag, 0, sizeof diag);
    CHECK_INT(protocol_message_type(root), PROTO_MSG_CONFIG);
    CHECK_INT(protocol_parse_config(root, &config, &diag), PROTO_OK);
    CHECK_STR(config.strategy, "EDF");
    CHECK_INT(config.order_count, 1);
    CHECK_STR(config.orders[0].id, "order_001");
    CHECK_STR(config.orders[0].recipe, "salad");
    CHECK_DBL(config.orders[0].arrival_time, 10.0);
    CHECK_DBL(config.orders[0].deadline, 60.0);
    CHECK_INT(diag.dropped_items, 0);

    json_free(root);
}

static void test_actions(void)
{
    char buf[256];
    size_t len = 0;
    json_value *root;

    /* §5.2 「控制指令」sample 1. */
    CHECK_INT(protocol_build_action_move(0, 12, 6, buf, sizeof buf, &len), PROTO_OK);
    CHECK_STR(buf, "{\"type\":\"action\",\"chef_id\":0,\"action\":\"move\","
                   "\"target\":{\"x\":12,\"y\":6}}");

    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_action action;

        CHECK_INT(protocol_message_type(root), PROTO_MSG_ACTION);
        CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_OK);
        CHECK_INT(action.chef_id, 0);
        CHECK_STR(action.action, PROTO_ACTION_MOVE);
        CHECK_INT(action.has_target, 1);
        CHECK_DBL(action.target_x, 12.0);
        CHECK_DBL(action.target_y, 6.0);
        json_free(root);
    }

    /* Fractional targets keep their decimals. */
    CHECK_INT(protocol_build_action_move(3, 10.5, -2.25, buf, sizeof buf, &len), PROTO_OK);
    CHECK_STR(buf, "{\"type\":\"action\",\"chef_id\":3,\"action\":\"move\","
                   "\"target\":{\"x\":10.5,\"y\":-2.25}}");

    /* The idle action sent for a missing input slot. */
    CHECK_INT(protocol_build_action_stop(1, buf, sizeof buf, &len), PROTO_OK);
    CHECK_STR(buf, "{\"type\":\"action\",\"chef_id\":1,\"action\":\"stop\"}");
    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_action action;

        CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_OK);
        CHECK_INT(action.chef_id, 1);
        CHECK_STR(action.action, PROTO_ACTION_STOP);
        CHECK_INT(action.has_target, 0);
        json_free(root);
    }

    /* §5.2 「控制指令」sample 2. */
    CHECK_INT(protocol_build_action_interact(1, "cutting_board_0", buf, sizeof buf, &len),
              PROTO_OK);
    CHECK_STR(buf, "{\"type\":\"action\",\"chef_id\":1,\"action\":\"interact\","
                   "\"target_object\":\"cutting_board_0\"}");
    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_action action;

        CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_OK);
        CHECK_STR(action.action, PROTO_ACTION_INTERACT);
        CHECK_STR(action.target_object, "cutting_board_0");
        json_free(root);
    }

    /* pickup/dash: same bare shape as stop, different name. `kos_action_kind` has
     * always had these two and the plugin's InputInjector implements them; these
     * builders are what makes them reachable on the wire at all. */
    CHECK_INT(protocol_build_action_pickup(0, buf, sizeof buf, &len), PROTO_OK);
    CHECK_STR(buf, "{\"type\":\"action\",\"chef_id\":0,\"action\":\"pickup\"}");
    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_action action;

        CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_OK);
        CHECK_INT(action.chef_id, 0);
        CHECK_STR(action.action, PROTO_ACTION_PICKUP);
        CHECK_INT(action.has_target, 0);
        CHECK_STR(action.target_object, "");
        json_free(root);
    }

    CHECK_INT(protocol_build_action_dash(2, buf, sizeof buf, &len), PROTO_OK);
    CHECK_STR(buf, "{\"type\":\"action\",\"chef_id\":2,\"action\":\"dash\"}");
    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_action action;

        CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_OK);
        CHECK_INT(action.chef_id, 2);
        CHECK_STR(action.action, PROTO_ACTION_DASH);
        CHECK_INT(action.has_target, 0);
        json_free(root);
    }


    /* path with 1 waypoint and 8 waypoints roundtrip */
    {
        proto_waypoint wps[1];
        wps[0].x = 10.5;
        wps[0].y = 5.25;
        CHECK_INT(protocol_build_action_path(0, 101, wps, 1, buf, sizeof buf, &len), PROTO_OK);
        root = parse_ok(buf, len);
        CHECK(root != NULL);
        if (root != NULL) {
            proto_action action;
            CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_OK);
            CHECK_STR(action.action, PROTO_ACTION_PATH);
            CHECK_INT(action.chef_id, 0);
            CHECK_INT(action.seq, 101);
            CHECK_INT(action.waypoint_count, 1);
            CHECK_DBL(action.waypoints[0].x, 10.5);
            CHECK_DBL(action.waypoints[0].y, 5.25);
            json_free(root);
        }
    }
    {
        proto_waypoint wps[8];
        for (size_t i = 0; i < 8; i++) {
            wps[i].x = (double)i * 1.5;
            wps[i].y = (double)i * 2.0;
        }
        CHECK_INT(protocol_build_action_path(1, 202, wps, 8, buf, sizeof buf, &len), PROTO_OK);
        root = parse_ok(buf, len);
        CHECK(root != NULL);
        if (root != NULL) {
            proto_action action;
            CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_OK);
            CHECK_STR(action.action, PROTO_ACTION_PATH);
            CHECK_INT(action.chef_id, 1);
            CHECK_INT(action.seq, 202);
            CHECK_INT(action.waypoint_count, 8);
            for (size_t i = 0; i < 8; i++) {
                CHECK_DBL(action.waypoints[i].x, (double)i * 1.5);
                CHECK_DBL(action.waypoints[i].y, (double)i * 2.0);
            }
            json_free(root);
        }
    }
    /* Invalid waypoint count or negative seq fails before emitting a schema-invalid message. */
    CHECK_INT(protocol_build_action_path(0, 1, NULL, 0, buf, sizeof buf, &len), PROTO_ERR_RANGE);
    {
        proto_waypoint one = { 1.0, 2.0 };
        CHECK_INT(protocol_build_action_path(0, -1, &one, 1, buf, sizeof buf, &len),
                  PROTO_ERR_RANGE);
    }

    /* Path semantic errors are rejected, not silently defaulted. */
    root = PARSE_OK("{\"type\":\"action\",\"chef_id\":0,\"action\":\"path\",\"waypoints\":[{\"x\":1,\"y\":2}],\"seq\":-1}");
    CHECK(root != NULL);
    if (root != NULL) {
        proto_action action;
        CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_ERR_RANGE);
        json_free(root);
    }
    root = PARSE_OK("{\"type\":\"action\",\"chef_id\":0,\"action\":\"path\",\"waypoints\":[{\"x\":1,\"y\":2}]}");
    CHECK(root != NULL);
    if (root != NULL) {
        proto_action action;
        CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_ERR_MISSING);
        json_free(root);
    }
    root = PARSE_OK("{\"type\":\"action\",\"chef_id\":0,\"action\":\"path\",\"seq\":1,"
                    "\"waypoints\":[{\"x\":1,\"y\":2},{\"x\":1,\"y\":3},{\"x\":1,\"y\":4},"
                    "{\"x\":1,\"y\":5},{\"x\":1,\"y\":6},{\"x\":1,\"y\":7},{\"x\":1,\"y\":8},"
                    "{\"x\":1,\"y\":9},{\"x\":1,\"y\":10}]}");
    CHECK(root != NULL);
    if (root != NULL) {
        proto_action action;
        CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_ERR_RANGE);
        json_free(root);
    }

    /* Two messages with the same seq stay independent on the wire; the plugin is
     * the side that appends (runtime selftest covers that). */
    {
        const char *same_seq =
            "{\"type\":\"action\",\"chef_id\":0,\"action\":\"path\",\"seq\":7,"
            "\"waypoints\":[{\"x\":1,\"y\":2},{\"x\":1,\"y\":3}]}";
        proto_action first;
        proto_action second;

        memset(&first, 0, sizeof first);
        memset(&second, 0, sizeof second);
        root = PARSE_OK(same_seq);
        CHECK(root != NULL);
        if (root != NULL) {
            CHECK_INT(protocol_parse_action(root, &first, NULL), PROTO_OK);
            json_free(root);
        }
        root = PARSE_OK(same_seq);
        CHECK(root != NULL);
        if (root != NULL) {
            CHECK_INT(protocol_parse_action(root, &second, NULL), PROTO_OK);
            CHECK_INT(second.seq, 7);
            CHECK_INT(second.waypoint_count, 2);
            CHECK_INT(first.seq, 7);
            CHECK_INT(first.waypoint_count, 2);
            json_free(root);
        }
    }

    /* face action roundtrip: with target, target_object, and invalid without both */
    CHECK_INT(protocol_build_action_face(0, 1, 12.0, 6.0, NULL, buf, sizeof buf, &len), PROTO_OK);
    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_action action;
        CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_OK);
        CHECK_STR(action.action, PROTO_ACTION_FACE);
        CHECK_INT(action.chef_id, 0);
        CHECK_INT(action.has_target, 1);
        CHECK_DBL(action.target_x, 12.0);
        CHECK_DBL(action.target_y, 6.0);
        json_free(root);
    }
    CHECK_INT(protocol_build_action_face(1, 0, 0.0, 0.0, "cutting_board_0", buf, sizeof buf, &len), PROTO_OK);
    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_action action;
        CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_OK);
        CHECK_STR(action.action, PROTO_ACTION_FACE);
        CHECK_INT(action.chef_id, 1);
        CHECK_STR(action.target_object, "cutting_board_0");
        json_free(root);
    }
    CHECK_INT(protocol_build_action_face(0, 0, 0.0, 0.0, NULL, buf, sizeof buf, &len), PROTO_ERR_MISSING);

    /* work action roundtrip */
    CHECK_INT(protocol_build_action_work(0, "cutting_board_0", buf, sizeof buf, &len), PROTO_OK);
    root = parse_ok(buf, len);
    CHECK(root != NULL);
    if (root != NULL) {
        proto_action action;
        CHECK_INT(protocol_parse_action(root, &action, NULL), PROTO_OK);
        CHECK_STR(action.action, PROTO_ACTION_WORK);
        CHECK_INT(action.chef_id, 0);
        CHECK_STR(action.target_object, "cutting_board_0");
        json_free(root);
    }
    CHECK_INT(protocol_build_action_work(0, "", buf, sizeof buf, &len), PROTO_ERR_MISSING);
    /* A buffer that cannot hold the message fails loudly instead of truncating. */
    CHECK(protocol_build_action_pickup(0, buf, 8, &len) != PROTO_OK);
}

static void test_type_names(void)
{
    static const struct {
        const char *name;
        proto_msg_type type;
    } table[] = {
        { "state_update", PROTO_MSG_STATE_UPDATE },
        { "action", PROTO_MSG_ACTION },
        { "config", PROTO_MSG_CONFIG },
        { "heartbeat", PROTO_MSG_HEARTBEAT },
        { "state_request", PROTO_MSG_STATE_REQUEST },
        { "echo", PROTO_MSG_ECHO },
        { "echo_reply", PROTO_MSG_ECHO_REPLY },
        { "hello", PROTO_MSG_HELLO },
        { "ack", PROTO_MSG_ACK }
    };
    size_t i;

    for (i = 0; i < sizeof table / sizeof table[0]; i++) {
        CHECK_INT(protocol_msg_type_of(table[i].name), table[i].type);
        CHECK_STR(protocol_msg_type_name(table[i].type), table[i].name);
    }
    CHECK_INT(protocol_msg_type_of("nonsense"), PROTO_MSG_UNKNOWN);
    CHECK_INT(protocol_msg_type_of(""), PROTO_MSG_UNKNOWN);
    CHECK_INT(protocol_msg_type_of(NULL), PROTO_MSG_UNKNOWN);
    CHECK_STR(protocol_msg_type_name(PROTO_MSG_UNKNOWN), "unknown");
    CHECK_STR(protocol_strerror(PROTO_ERR_MISSING), "required field is missing");
    CHECK_STR(protocol_strerror(JSON_ERR_NOSPACE), "buffer too small");
}

static void test_parse_errors(void)
{
    json_value *root;
    proto_state_update state;
    proto_ack ack;
    proto_heartbeat beat;

    /* A message of another type must not be read as this one. */
    root = PARSE_OK("{\"type\":\"ack\",\"action\":\"move\",\"chef_id\":0,\"ok\":true}");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_state_update(root, &state, NULL), PROTO_ERR_TYPE);
        json_free(root);
    }

    /* `type` is mandatory: a message that does not say what it is cannot be routed. */
    root = PARSE_OK("{\"timestamp\":1}");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_heartbeat(root, &beat), PROTO_ERR_MISSING);
        CHECK_INT(protocol_message_type(root), PROTO_MSG_UNKNOWN);
        json_free(root);
    }

    /* Required members of an ack. */
    root = PARSE_OK("{\"type\":\"ack\",\"chef_id\":0,\"ok\":true}");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_ack(root, &ack, NULL), PROTO_ERR_MISSING);
        json_free(root);
    }
    root = PARSE_OK("{\"type\":\"ack\",\"action\":\"move\",\"ok\":true}");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_ack(root, &ack, NULL), PROTO_ERR_MISSING);
        json_free(root);
    }
    /* ok must be a boolean, not the string "true". */
    root = PARSE_OK("{\"type\":\"ack\",\"action\":\"m\",\"chef_id\":0,\"ok\":\"true\"}");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_ack(root, &ack, NULL), PROTO_ERR_RANGE);
        json_free(root);
    }
    /* A non-object root is never a message. */
    root = PARSE_OK("[1,2,3]");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_ack(root, &ack, NULL), PROTO_ERR_TYPE);
        CHECK_INT(protocol_message_type(root), PROTO_MSG_UNKNOWN);
        json_free(root);
    }

    /* A field of the wrong JSON type inside a well-formed message. */
    root = PARSE_OK("{\"type\":\"state_update\",\"game_time\":\"soon\"}");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_state_update(root, &state, NULL), PROTO_ERR_RANGE);
        json_free(root);
    }
    root = PARSE_OK("{\"type\":\"heartbeat\",\"timestamp\":\"now\"}");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_heartbeat(root, &beat), PROTO_ERR_RANGE);
        json_free(root);
    }

    /* game_time_ms is game-internal elapsed time; negative is not usable. */
    root = PARSE_OK("{\"type\":\"state_update\",\"game_time_ms\":-1}");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_state_update(root, &state, NULL), PROTO_ERR_RANGE);
        json_free(root);
    }

    /* progress outside 0..1 is a sender bug, rejected at the trust boundary. */
    root = PARSE_OK("{\"type\":\"state_update\",\"game_time_ms\":1000,"
                    "\"resources\":{\"cutting_board_0\":{\"state\":\"occupied\","
                    "\"progress\":1.5}}}");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_state_update(root, &state, NULL), PROTO_ERR_RANGE);
        json_free(root);
    }
    root = PARSE_OK("{\"type\":\"state_update\",\"game_time_ms\":1000,"
                    "\"resources\":{\"sink_0\":{\"state\":\"washing\","
                    "\"progress\":-0.1}}}");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_state_update(root, &state, NULL), PROTO_ERR_RANGE);
        json_free(root);
    }

    /* Missing game_time_ms yields PROTO_ERR_MISSING (explicit error for old message format) */
    root = PARSE_OK("{\"type\":\"state_update\",\"timestamp\":1,\"game_time\":1,\"score\":0}");
    CHECK(root != NULL);
    if (root != NULL) {
        CHECK_INT(protocol_parse_state_update(root, &state, NULL), PROTO_ERR_MISSING);
        json_free(root);
    }
}

static void test_partial_and_oversized_arrays(void)
{
    char buf[16384];
    size_t pos = 0;
    json_value *root;
    proto_state_update state;
    proto_diag diag;
    int i;

    /* More chefs than the fixed layout holds: keep the first PROTO_MAX_CHEFS and
     * report the rest instead of failing the whole frame. */
    pos = 0;
    app(buf, sizeof buf, &pos, "{\"type\":\"state_update\",\"game_time_ms\":1000,\"chefs\":[");
    for (i = 0; i < PROTO_MAX_CHEFS + 4; i++) {
        if (i > 0)
            app(buf, sizeof buf, &pos, ",");
        app(buf, sizeof buf, &pos, "{\"id\":");
        app_int(buf, sizeof buf, &pos, i);
        app(buf, sizeof buf, &pos, ",\"position\":{\"x\":1,\"y\":2},\"holding\":null,"
                                   "\"state\":\"idle\"}");
    }
    app(buf, sizeof buf, &pos, "]}");
    root = parse_ok(buf, pos);
    CHECK(root != NULL);
    if (root != NULL) {
        memset(&diag, 0, sizeof diag);
        CHECK_INT(protocol_parse_state_update(root, &state, &diag), PROTO_OK);
        CHECK_INT(state.chef_count, PROTO_MAX_CHEFS);
        CHECK_INT(diag.dropped_items, 4);
        json_free(root);
    }

    /* A chef entry that is not an object is dropped on its own. */
    root = PARSE_OK("{\"type\":\"state_update\",\"game_time_ms\":1000,\"chefs\":[42,{\"id\":5,\"state\":\"idle\"}]}");
    CHECK(root != NULL);
    if (root != NULL) {
        memset(&diag, 0, sizeof diag);
        CHECK_INT(protocol_parse_state_update(root, &state, &diag), PROTO_OK);
        CHECK_INT(state.chef_count, 1);
        CHECK_INT(state.chefs[0].id, 5);
        CHECK_INT(diag.dropped_items, 1);
        json_free(root);
    }

    /* A chef without an id cannot be attributed to an input slot. */
    root = PARSE_OK("{\"type\":\"state_update\",\"game_time_ms\":1000,\"chefs\":[{\"state\":\"idle\"},"
                    "{\"id\":2,\"state\":\"moving\"}]}");
    CHECK(root != NULL);
    if (root != NULL) {
        memset(&diag, 0, sizeof diag);
        CHECK_INT(protocol_parse_state_update(root, &state, &diag), PROTO_OK);
        CHECK_INT(state.chef_count, 1);
        CHECK_INT(state.chefs[0].id, 2);
        CHECK_INT(diag.dropped_items, 1);
        json_free(root);
    }

    /* A big order list is the documented stress case. */
    pos = 0;
    app(buf, sizeof buf, &pos, "{\"type\":\"state_update\",\"game_time_ms\":1000,\"orders\":[");
    for (i = 0; i < 100; i++) {
        if (i > 0)
            app(buf, sizeof buf, &pos, ",");
        app(buf, sizeof buf, &pos, "{\"id\":\"o");
        app_int(buf, sizeof buf, &pos, i);
        app(buf, sizeof buf, &pos, "\",\"recipe\":\"salad\",\"time_remaining\":1,"
                                   "\"ingredients\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\","
                                   "\"g\",\"h\",\"i\",\"j\"]}");
    }
    app(buf, sizeof buf, &pos, "]}");
    root = parse_ok(buf, pos);
    CHECK(root != NULL);
    if (root != NULL) {
        memset(&diag, 0, sizeof diag);
        CHECK_INT(protocol_parse_state_update(root, &state, &diag), PROTO_OK);
        CHECK_INT(state.order_count, PROTO_MAX_ORDERS);
        CHECK_STR(state.orders[0].id, "o0");
        /* 100 - 32 orders plus 2 ingredients per order beyond the cap of 8. */
        CHECK(diag.dropped_items >= 68);
        CHECK_INT(state.orders[0].ingredient_count, PROTO_MAX_INGREDIENTS);
        json_free(root);
    }

    /* More resource keys than the layout holds. */
    pos = 0;
    app(buf, sizeof buf, &pos, "{\"type\":\"state_update\",\"game_time_ms\":1000,\"resources\":{");
    for (i = 0; i < PROTO_MAX_RESOURCES + 12; i++) {
        if (i > 0)
            app(buf, sizeof buf, &pos, ",");
        app(buf, sizeof buf, &pos, "\"pot_");
        app_int(buf, sizeof buf, &pos, i);
        app(buf, sizeof buf, &pos, "\":{\"state\":\"free\"}");
    }
    app(buf, sizeof buf, &pos, "}}");
    root = parse_ok(buf, pos);
    CHECK(root != NULL);
    if (root != NULL) {
        memset(&diag, 0, sizeof diag);
        CHECK_INT(protocol_parse_state_update(root, &state, &diag), PROTO_OK);
        CHECK_INT(state.resource_count, PROTO_MAX_RESOURCES);
        CHECK_INT(diag.dropped_items, 12);
        CHECK_INT(state.resources[PROTO_MAX_RESOURCES - 1].index, PROTO_MAX_RESOURCES - 1);
        json_free(root);
    }
}

static void test_long_strings_are_truncated_not_fatal(void)
{
    char buf[1024];
    size_t pos = 0;
    json_value *root;
    proto_state_update state;
    proto_diag diag;
    int i;

    pos = 0;
    app(buf, sizeof buf, &pos, "{\"type\":\"state_update\",\"game_time_ms\":1000,\"orders\":[{\"id\":\"");
    for (i = 0; i < 200; i++)
        app(buf, sizeof buf, &pos, "x");
    app(buf, sizeof buf, &pos, "\",\"recipe\":\"salad\"}]}");
    root = parse_ok(buf, pos);
    CHECK(root != NULL);
    if (root != NULL) {
        memset(&diag, 0, sizeof diag);
        CHECK_INT(protocol_parse_state_update(root, &state, &diag), PROTO_OK);
        CHECK_INT(strlen(state.orders[0].id), PROTO_STR_MAX - 1);
        CHECK_INT(diag.truncated_strings, 1);
        json_free(root);
    }
}

int main(void)
{
    printf("test_protocol: KitchenOS message encode/decode\n");

    test_state_update_sample();
    test_state_new_fields();
    test_dynamic_resources();
    test_state_audit_fields();
    test_state_field_errors();
    test_heartbeat();
    test_hello();
    test_ack();
    test_echo();
    test_config();
    test_actions();
    test_type_names();
    test_parse_errors();
    test_partial_and_oversized_arrays();
    test_long_strings_are_truncated_not_fatal();

    printf("test_protocol: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
