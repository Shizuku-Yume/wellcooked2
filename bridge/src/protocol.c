/*
 * protocol.c -- KitchenOS message encode/decode. See protocol.h for the contract
 * and protocol/protocol.md for the field-by-field agreement with the game plugin.
 */
#include <kitchenos/protocol.h>

#include <string.h>

const char *protocol_msg_type_name(proto_msg_type type)
{
    switch (type) {
    case PROTO_MSG_STATE_UPDATE: return PROTO_TYPE_STATE_UPDATE;
    case PROTO_MSG_ACTION: return PROTO_TYPE_ACTION;
    case PROTO_MSG_CONFIG: return PROTO_TYPE_CONFIG;
    case PROTO_MSG_HEARTBEAT: return PROTO_TYPE_HEARTBEAT;
    case PROTO_MSG_STATE_REQUEST: return PROTO_TYPE_STATE_REQUEST;
    case PROTO_MSG_ECHO: return PROTO_TYPE_ECHO;
    case PROTO_MSG_ECHO_REPLY: return PROTO_TYPE_ECHO_REPLY;
    case PROTO_MSG_HELLO: return PROTO_TYPE_HELLO;
    case PROTO_MSG_ACK: return PROTO_TYPE_ACK;
    default: return "unknown";
    }
}

proto_msg_type protocol_msg_type_of(const char *name)
{
    static const struct {
        const char *name;
        proto_msg_type type;
    } table[] = {
        { PROTO_TYPE_STATE_UPDATE, PROTO_MSG_STATE_UPDATE },
        { PROTO_TYPE_ACTION, PROTO_MSG_ACTION },
        { PROTO_TYPE_CONFIG, PROTO_MSG_CONFIG },
        { PROTO_TYPE_HEARTBEAT, PROTO_MSG_HEARTBEAT },
        { PROTO_TYPE_STATE_REQUEST, PROTO_MSG_STATE_REQUEST },
        { PROTO_TYPE_ECHO, PROTO_MSG_ECHO },
        { PROTO_TYPE_ECHO_REPLY, PROTO_MSG_ECHO_REPLY },
        { PROTO_TYPE_HELLO, PROTO_MSG_HELLO },
        { PROTO_TYPE_ACK, PROTO_MSG_ACK }
    };
    size_t i;

    if (name == NULL)
        return PROTO_MSG_UNKNOWN;
    for (i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(name, table[i].name) == 0)
            return table[i].type;
    }
    return PROTO_MSG_UNKNOWN;
}

const char *protocol_strerror(int code)
{
    switch (code) {
    case PROTO_OK: return "ok";
    case PROTO_ERR_TYPE: return "unexpected message or field type";
    case PROTO_ERR_MISSING: return "required field is missing";
    case PROTO_ERR_RANGE: return "field value is unusable";
    default: return json_strerror(code); /* builders return the json.h writer codes */
    }
}

/* ------------------------------------------------------------------ */
/* parsing helpers                                                     */
/* ------------------------------------------------------------------ */

/* JSON_ERR_MISSING is the only json.h failure that is not the caller's fault, so
 * every other one collapses into "unusable value". */
static int map_status(int rc)
{
    if (rc == JSON_OK)
        return PROTO_OK;
    if (rc == JSON_ERR_MISSING)
        return PROTO_ERR_MISSING;
    return PROTO_ERR_RANGE;
}

static int expect_type(const json_value *root, const char *want)
{
    const json_value *type;

    if (json_typeof(root) != JSON_OBJECT)
        return PROTO_ERR_TYPE;
    type = json_object_get(root, "type");
    if (type == NULL)
        return PROTO_ERR_MISSING;
    if (json_typeof(type) != JSON_STRING)
        return PROTO_ERR_TYPE;
    if (strcmp(json_string_value(type), want) != 0)
        return PROTO_ERR_TYPE;
    return PROTO_OK;
}

static void copy_str(char *dst, size_t cap, const char *src, size_t len,
                     proto_diag *diag)
{
    size_t n;

    if (cap == 0)
        return;
    if (len >= cap) {
        n = cap - 1;
        if (diag != NULL)
            diag->truncated_strings++;
    } else {
        n = len;
    }
    if (n > 0)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Optional members: absent *and* JSON null both mean "not applicable" and leave
 * the destination at its default, because Unity's serializer writes null for
 * fields the game had no value for. A present value of the wrong JSON type is
 * still an error -- that is a bug on the other end, not a missing datum. */
static int get_str(const json_value *object, const char *key, char *dst, size_t cap,
                   proto_diag *diag)
{
    const char *text = NULL;
    size_t len = 0;
    int rc = json_object_get_string(object, key, &text, &len);

    if (rc == JSON_ERR_MISSING || json_is_null(json_object_get(object, key)))
        return PROTO_OK;
    if (rc != JSON_OK)
        return PROTO_ERR_RANGE;
    copy_str(dst, cap, text, len, diag);
    return PROTO_OK;
}

static int get_req_str(const json_value *object, const char *key, char *dst, size_t cap,
                       proto_diag *diag)
{
    const char *text = NULL;
    size_t len = 0;
    int rc = json_object_get_string(object, key, &text, &len);

    if (rc != JSON_OK)
        return map_status(rc);
    copy_str(dst, cap, text, len, diag);
    return PROTO_OK;
}

static int get_num(const json_value *object, const char *key, double *dst)
{
    int rc = json_object_get_double(object, key, dst);
    if (rc == JSON_ERR_MISSING || json_is_null(json_object_get(object, key)))
        return PROTO_OK;
    return map_status(rc);
}

static int get_int(const json_value *object, const char *key, long long *dst)
{
    int rc = json_object_get_int(object, key, dst);
    if (rc == JSON_ERR_MISSING || json_is_null(json_object_get(object, key)))
        return PROTO_OK;
    return map_status(rc);
}

static int get_req_int(const json_value *object, const char *key, long long *dst)
{
    return map_status(json_object_get_int(object, key, dst));
}

static int get_req_bool(const json_value *object, const char *key, int *dst)
{
    return map_status(json_object_get_bool(object, key, dst));
}

/* Optional boolean: absent or null leaves *dst untouched and returns PROTO_OK;
 * a present member of the wrong type is an error, never a silent false. */
static int get_opt_bool(const json_value *object, const char *key, int *dst)
{
    int rc = json_object_get_bool(object, key, dst);

    if (rc == JSON_ERR_MISSING || json_is_null(json_object_get(object, key)))
        return PROTO_OK;
    return map_status(rc);
}

/* Optional string-array member (`contents`, `holding_contents`): absent or JSON
 * null means "nothing to say" and leaves the count at 0; a present member of the
 * wrong type is still an error. Entries that are not strings are dropped and
 * counted, and anything past `max` is dropped and counted once. */
static int get_str_array(const json_value *object, const char *key,
                         char dst[][PROTO_KEY_MAX], size_t max, size_t *count,
                         proto_diag *diag)
{
    const json_value *array = NULL;
    size_t total;
    size_t i;
    int rc = json_object_get_array(object, key, &array);

    *count = 0;
    if (rc == JSON_ERR_MISSING || json_is_null(json_object_get(object, key)))
        return PROTO_OK;
    if (rc != JSON_OK)
        return PROTO_ERR_RANGE;

    total = json_array_size(array);
    for (i = 0; i < total; i++) {
        const json_value *item = json_array_get(array, i);
        const char *text = json_string_value(item);

        if (text == NULL) {
            if (diag != NULL)
                diag->dropped_items++;
            continue;
        }
        if (*count >= max) {
            if (diag != NULL)
                diag->dropped_items += total - i;
            break;
        }
        copy_str(dst[*count], PROTO_KEY_MAX, text, json_string_length(item), diag);
        (*count)++;
    }
    return PROTO_OK;
}

/* "pot_7" -> POT/7, "cutting_board_0" -> CUTTING_BOARD/0, "plate_stack" ->
 * PLATE_STACK, "carried_2" -> CARRIED/2. Anything else is kept as an unknown
 * resource rather than dropped: the game may grow new station kinds and the
 * bridge must survive that. */
static proto_resource_kind classify_key(const char *key, int *index)
{
    static const struct {
        const char *prefix;
        proto_resource_kind kind;
    } table[] = {
        { "pot_", PROTO_RES_POT },
        { "cutting_board_", PROTO_RES_CUTTING_BOARD },
        { "sink_", PROTO_RES_SINK },
        { "counter_", PROTO_RES_COUNTER },
        { "container_", PROTO_RES_CONTAINER },
        { "carried_", PROTO_RES_CARRIED }
    };
    const char *digits = NULL;
    proto_resource_kind kind = PROTO_RES_UNKNOWN;
    const char *p;
    size_t i;
    int value = 0;

    *index = -1;
    if (strcmp(key, "plate_stack") == 0)
        return PROTO_RES_PLATE_STACK;
    if (strcmp(key, "dirty_plates") == 0)
        return PROTO_RES_DIRTY_PLATES;

    for (i = 0; i < sizeof table / sizeof table[0]; i++) {
        size_t len = strlen(table[i].prefix);
        if (strncmp(key, table[i].prefix, len) == 0) {
            kind = table[i].kind;
            digits = key + len;
            break;
        }
    }
    if (digits == NULL || *digits == '\0')
        return PROTO_RES_UNKNOWN;

    for (p = digits; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return PROTO_RES_UNKNOWN;
        if (value < 100000)
            value = value * 10 + (*p - '0');
    }
    *index = value;
    return kind;
}

proto_msg_type protocol_message_type(const json_value *root)
{
    const json_value *type;

    if (json_typeof(root) != JSON_OBJECT)
        return PROTO_MSG_UNKNOWN;
    type = json_object_get(root, "type");
    if (json_typeof(type) != JSON_STRING)
        return PROTO_MSG_UNKNOWN;
    return protocol_msg_type_of(json_string_value(type));
}

static int parse_chefs(const json_value *root, proto_state_update *out, proto_diag *diag)
{
    const json_value *chefs = NULL;
    size_t total;
    size_t i;
    int rc = json_object_get_array(root, "chefs", &chefs);

    if (rc == JSON_ERR_MISSING)
        return PROTO_OK;
    if (rc != JSON_OK)
        return PROTO_ERR_RANGE;

    total = json_array_size(chefs);
    for (i = 0; i < total; i++) {
        const json_value *entry = json_array_get(chefs, i);
        const json_value *position = NULL;
        const json_value *holding;
        proto_chef *chef;
        long long id = 0;

        /* An entry a chef cannot be read from carries nothing, and one bad chef
         * must not cost the bridge the whole kitchen: drop it and say so. */
        if (json_typeof(entry) != JSON_OBJECT ||
            json_object_get_int(entry, "id", &id) != JSON_OK) {
            if (diag != NULL)
                diag->dropped_items++;
            continue;
        }
        if (out->chef_count >= PROTO_MAX_CHEFS) {
            if (diag != NULL)
                diag->dropped_items += total - i;
            break;
        }

        chef = &out->chefs[out->chef_count];
        memset(chef, 0, sizeof *chef);
        chef->id = id;

        position = json_object_get(entry, "position");
        if (position != NULL && json_typeof(position) != JSON_NULL) {
            if (json_typeof(position) != JSON_OBJECT)
                return PROTO_ERR_RANGE;
            rc = get_num(position, "x", &chef->x);
            if (rc != PROTO_OK)
                return rc;
            rc = get_num(position, "y", &chef->y);
            if (rc != PROTO_OK)
                return rc;
        }
        rc = get_num(entry, "facing", &chef->facing);
        if (rc != PROTO_OK)
            return rc;

        /* `holding` is a string when the chef carries something and JSON null
         * otherwise; both describe a usable chef, so null stays as "". */
        holding = json_object_get(entry, "holding");
        if (holding != NULL && json_typeof(holding) != JSON_NULL) {
            rc = get_str(entry, "holding", chef->holding, sizeof chef->holding, diag);
            if (rc != PROTO_OK)
                return rc;
        }
        rc = get_str(entry, "state", chef->state, sizeof chef->state, diag);
        if (rc != PROTO_OK)
            return rc;
        rc = get_str_array(entry, "holding_contents", chef->holding_contents,
                           PROTO_MAX_CONTENTS, &chef->holding_content_count, diag);
        if (rc != PROTO_OK)
            return rc;

        out->chef_count++;
    }
    return PROTO_OK;
}

static int parse_orders(const json_value *root, proto_state_update *out, proto_diag *diag)
{
    const json_value *orders = NULL;
    size_t total;
    size_t i;
    int rc = json_object_get_array(root, "orders", &orders);

    if (rc == JSON_ERR_MISSING)
        return PROTO_OK;
    if (rc != JSON_OK)
        return PROTO_ERR_RANGE;

    total = json_array_size(orders);
    for (i = 0; i < total; i++) {
        const json_value *entry = json_array_get(orders, i);
        const json_value *ingredients = NULL;
        proto_order *order;
        size_t j;
        size_t ingredient_total;

        if (json_typeof(entry) != JSON_OBJECT) {
            if (diag != NULL)
                diag->dropped_items++;
            continue;
        }
        if (out->order_count >= PROTO_MAX_ORDERS) {
            if (diag != NULL)
                diag->dropped_items += total - i;
            break;
        }

        order = &out->orders[out->order_count];
        memset(order, 0, sizeof *order);

        rc = get_str(entry, "id", order->id, sizeof order->id, diag);
        if (rc != PROTO_OK)
            return rc;
        rc = get_str(entry, "recipe", order->recipe, sizeof order->recipe, diag);
        if (rc != PROTO_OK)
            return rc;
        rc = get_num(entry, "time_remaining", &order->time_remaining);
        if (rc != PROTO_OK)
            return rc;
        if (json_object_get(entry, "score") != NULL) {
            rc = get_int(entry, "score", &order->score);
            if (rc != PROTO_OK)
                return rc;
            order->has_score = 1;
        }

        rc = json_object_get_array(entry, "ingredients", &ingredients);
        if (rc == JSON_OK) {
            ingredient_total = json_array_size(ingredients);
            for (j = 0; j < ingredient_total; j++) {
                const json_value *item = json_array_get(ingredients, j);
                const char *text = json_string_value(item);
                if (text == NULL) {
                    if (diag != NULL)
                        diag->dropped_items++;
                    continue;
                }
                if (order->ingredient_count >= PROTO_MAX_INGREDIENTS) {
                    if (diag != NULL)
                        diag->dropped_items += ingredient_total - j;
                    break;
                }
                copy_str(order->ingredients[order->ingredient_count], PROTO_KEY_MAX, text,
                         json_string_length(item), diag);
                order->ingredient_count++;
            }
        } else if (rc != JSON_ERR_MISSING &&
                   json_typeof(json_object_get(entry, "ingredients")) != JSON_NULL) {
            return PROTO_ERR_RANGE;
        }

        out->order_count++;
    }
    return PROTO_OK;
}

static int parse_resources(const json_value *root, proto_state_update *out,
                           proto_diag *diag)
{
    const json_value *resources = NULL;
    size_t total;
    size_t i;
    int rc = json_object_get_object(root, "resources", &resources);

    if (rc == JSON_ERR_MISSING)
        return PROTO_OK;
    if (rc != JSON_OK)
        return PROTO_ERR_RANGE;

    total = json_object_size(resources);
    for (i = 0; i < total; i++) {
        const char *key = json_object_key_at(resources, i);
        const json_value *entry = json_object_value_at(resources, i);
        const json_value *pos;
        proto_resource *res;

        if (json_typeof(entry) != JSON_OBJECT) {
            if (diag != NULL)
                diag->dropped_items++;
            continue;
        }
        if (out->resource_count >= PROTO_MAX_RESOURCES) {
            if (diag != NULL)
                diag->dropped_items += total - i;
            break;
        }

        res = &out->resources[out->resource_count];
        memset(res, 0, sizeof *res);
        copy_str(res->key, sizeof res->key, key, json_object_key_length_at(resources, i),
                 diag);
        res->kind = classify_key(res->key, &res->index);

        rc = get_str(entry, "state", res->state, sizeof res->state, diag);
        if (rc != PROTO_OK)
            return rc;
        rc = get_str(entry, "content", res->content, sizeof res->content, diag);
        if (rc != PROTO_OK)
            return rc;
        if (json_object_get(entry, "time_left") != NULL) {
            rc = get_num(entry, "time_left", &res->time_left);
            if (rc != PROTO_OK)
                return rc;
            res->has_time_left = 1;
        }
        if (json_object_get(entry, "chef_id") != NULL) {
            rc = get_int(entry, "chef_id", &res->chef_id);
            if (rc != PROTO_OK)
                return rc;
            res->has_chef_id = 1;
        }
        if (json_object_get(entry, "count") != NULL) {
            long long count = 0;
            rc = get_int(entry, "count", &count);
            if (rc != PROTO_OK)
                return rc;
            res->count = count;
            res->has_count = 1;
        }
        if (json_object_get(entry, "progress") != NULL) {
            rc = get_num(entry, "progress", &res->progress);
            if (rc != PROTO_OK)
                return rc;
            /* 0..1 是契约（schema minimum/maximum）；越界值来自对端 bug，
             * 不能写进 shm 让调度侧当成“已完成”或负进度。 */
            if (res->progress < 0.0 || res->progress > 1.0)
                return PROTO_ERR_RANGE;
            res->has_progress = 1;
        }

        /* ---- state audit G1–G6 ---- */
        rc = get_str(entry, "kind", res->kind_name, sizeof res->kind_name, diag);
        if (rc != PROTO_OK)
            return rc;
        rc = get_str(entry, "item", res->item, sizeof res->item, diag);
        if (rc != PROTO_OK)
            return rc;
        rc = get_str(entry, "step", res->step, sizeof res->step, diag);
        if (rc != PROTO_OK)
            return rc;

        /* `pos` is how the worker finds a station without trusting enumeration
         * order (G1). A half-written position is a sender bug: refuse it rather
         * than pretend the station sits at (0,0). */
        pos = json_object_get(entry, "pos");
        if (pos != NULL && json_typeof(pos) != JSON_NULL) {
            if (json_typeof(pos) != JSON_OBJECT)
                return PROTO_ERR_RANGE;
            if (json_object_get(pos, "x") == NULL || json_object_get(pos, "y") == NULL)
                return PROTO_ERR_RANGE;
            rc = get_num(pos, "x", &res->x);
            if (rc != PROTO_OK)
                return rc;
            rc = get_num(pos, "y", &res->y);
            if (rc != PROTO_OK)
                return rc;
            res->has_pos = 1;
        }

        rc = get_str_array(entry, "contents", res->contents, PROTO_MAX_CONTENTS,
                           &res->content_count, diag);
        if (rc != PROTO_OK)
            return rc;

        /* Cooking flags (KOS_SHM_VERSION 3): `state` collapses cooked+burning into
         * one string, so the pour gate reads these instead. */
        if (json_object_get(entry, "cooked") != NULL) {
            rc = get_opt_bool(entry, "cooked", &res->cooked);
            if (rc != PROTO_OK)
                return rc;
            res->has_cooked = 1;
        }
        if (json_object_get(entry, "burning") != NULL) {
            rc = get_opt_bool(entry, "burning", &res->burning);
            if (rc != PROTO_OK)
                return rc;
            res->has_burning = 1;
        }
        if (json_object_get(entry, "burn_in") != NULL) {
            rc = get_num(entry, "burn_in", &res->burn_in);
            if (rc != PROTO_OK)
                return rc;
            if (res->burn_in < 0.0)
                return PROTO_ERR_RANGE;
            res->has_burn_in = 1;
        }
        if (json_object_get(entry, "fire") != NULL) {
            rc = get_opt_bool(entry, "fire", &res->fire);
            if (rc != PROTO_OK)
                return rc;
            res->has_fire = 1;
        }

        out->resource_count++;
    }
    return PROTO_OK;
}

/* Finished orders (G9). The plugin resends its whole bounded ring every frame,
 * so an entry is dropped, not the message, when it lacks the three fields that
 * make it usable: seq, id, outcome. */
static int parse_order_results(const json_value *root, proto_state_update *out,
                               proto_diag *diag)
{
    const json_value *results = NULL;
    size_t total;
    size_t i;
    int rc = json_object_get_array(root, "order_results", &results);

    if (rc == JSON_ERR_MISSING)
        return PROTO_OK;
    if (rc != JSON_OK)
        return PROTO_ERR_RANGE;

    total = json_array_size(results);
    for (i = 0; i < total; i++) {
        const json_value *entry = json_array_get(results, i);
        const char *id;
        const char *outcome;
        size_t id_len;
        size_t outcome_len;
        long long seq = 0;
        proto_order_result *result;

        if (json_typeof(entry) != JSON_OBJECT ||
            json_object_get_int(entry, "seq", &seq) != JSON_OK ||
            json_object_get_string(entry, "id", &id, &id_len) != JSON_OK ||
            json_object_get_string(entry, "outcome", &outcome, &outcome_len) != JSON_OK) {
            if (diag != NULL)
                diag->dropped_items++;
            continue;
        }
        if (out->order_result_count >= PROTO_MAX_ORDER_RESULTS) {
            if (diag != NULL)
                diag->dropped_items += total - i;
            break;
        }

        result = &out->order_results[out->order_result_count];
        memset(result, 0, sizeof *result);
        result->seq = seq;
        copy_str(result->id, sizeof result->id, id, id_len, diag);
        copy_str(result->outcome, sizeof result->outcome, outcome, outcome_len, diag);
        rc = get_str(entry, "recipe", result->recipe, sizeof result->recipe, diag);
        if (rc != PROTO_OK)
            return rc;
        if (json_object_get(entry, "score") != NULL) {
            rc = get_int(entry, "score", &result->score);
            if (rc != PROTO_OK)
                return rc;
            result->has_score = 1;
        }
        rc = get_int(entry, "game_time_ms", &result->game_time_ms);
        if (rc != PROTO_OK)
            return rc;

        out->order_result_count++;
    }
    return PROTO_OK;
}

int protocol_parse_state_update(const json_value *root, proto_state_update *out,
                                proto_diag *diag)
{
    int rc;

    if (out == NULL)
        return PROTO_ERR_TYPE;
    memset(out, 0, sizeof *out);

    rc = expect_type(root, PROTO_TYPE_STATE_UPDATE);
    if (rc != PROTO_OK)
        return rc;

    rc = get_int(root, "timestamp", &out->timestamp);
    if (rc != PROTO_OK)
        return rc;
    rc = get_num(root, "game_time", &out->game_time);
    if (rc != PROTO_OK)
        return rc;
    rc = get_req_int(root, "game_time_ms", &out->game_time_ms);
    if (rc != PROTO_OK)
        return rc;
    if (out->game_time_ms < 0)
        return PROTO_ERR_RANGE;
    rc = get_num(root, "score", &out->score);
    if (rc != PROTO_OK)
        return rc;

    /* The round clock is one datum with two numbers: report neither rather than
     * a left value without its limit. */
    if (json_object_get(root, "round_time_left") != NULL ||
        json_object_get(root, "round_time_limit") != NULL) {
        rc = get_num(root, "round_time_left", &out->round_time_left);
        if (rc != PROTO_OK)
            return rc;
        rc = get_num(root, "round_time_limit", &out->round_time_limit);
        if (rc != PROTO_OK)
            return rc;
        out->has_round_time = 1;
    }

    rc = parse_chefs(root, out, diag);
    if (rc != PROTO_OK)
        return rc;
    rc = parse_orders(root, out, diag);
    if (rc != PROTO_OK)
        return rc;
    rc = parse_order_results(root, out, diag);
    if (rc != PROTO_OK)
        return rc;
    return parse_resources(root, out, diag);
}

int protocol_parse_heartbeat(const json_value *root, proto_heartbeat *out)
{
    int rc;

    if (out == NULL)
        return PROTO_ERR_TYPE;
    memset(out, 0, sizeof *out);

    rc = expect_type(root, PROTO_TYPE_HEARTBEAT);
    if (rc != PROTO_OK)
        return rc;
    return map_status(json_object_get_int(root, "timestamp", &out->timestamp));
}

int protocol_parse_hello(const json_value *root, proto_hello *out, proto_diag *diag)
{
    const json_value *actions = NULL;
    size_t total;
    size_t i;
    int rc;

    if (out == NULL)
        return PROTO_ERR_TYPE;
    memset(out, 0, sizeof *out);

    rc = expect_type(root, PROTO_TYPE_HELLO);
    if (rc != PROTO_OK)
        return rc;

    rc = get_req_str(root, "instance", out->instance, sizeof out->instance, diag);
    if (rc != PROTO_OK)
        return rc;
    rc = get_req_str(root, "version", out->version, sizeof out->version, diag);
    if (rc != PROTO_OK)
        return rc;

    rc = json_object_get_array(root, "actions", &actions);
    if (rc == JSON_ERR_MISSING)
        return PROTO_OK;
    if (rc != JSON_OK)
        return PROTO_ERR_RANGE;

    total = json_array_size(actions);
    for (i = 0; i < total; i++) {
        const json_value *item = json_array_get(actions, i);
        const char *text = json_string_value(item);

        if (text == NULL) {
            if (diag != NULL)
                diag->dropped_items++;
            continue;
        }
        if (out->action_count >= PROTO_MAX_ACTIONS) {
            if (diag != NULL)
                diag->dropped_items += total - i;
            break;
        }
        copy_str(out->actions[out->action_count], PROTO_KEY_MAX, text,
                 json_string_length(item), diag);
        out->action_count++;
    }
    return PROTO_OK;
}

int protocol_parse_ack(const json_value *root, proto_ack *out, proto_diag *diag)
{
    int rc;

    if (out == NULL)
        return PROTO_ERR_TYPE;
    memset(out, 0, sizeof *out);

    rc = expect_type(root, PROTO_TYPE_ACK);
    if (rc != PROTO_OK)
        return rc;

    rc = get_req_str(root, "action", out->action, sizeof out->action, diag);
    if (rc != PROTO_OK)
        return rc;
    rc = get_req_int(root, "chef_id", &out->chef_id);
    if (rc != PROTO_OK)
        return rc;
    rc = get_req_bool(root, "ok", &out->ok);
    if (rc != PROTO_OK)
        return rc;
    if (json_object_get(root, "seq") != NULL) {
        rc = get_int(root, "seq", &out->seq);
        if (rc != PROTO_OK)
            return rc;
        out->has_seq = 1;
    }
    return get_str(root, "detail", out->detail, sizeof out->detail, diag);
}

int protocol_parse_echo(const json_value *root, proto_echo *out)
{
    const char *token = NULL;
    size_t len = 0;
    int rc;

    if (out == NULL)
        return PROTO_ERR_TYPE;
    memset(out, 0, sizeof *out);

    rc = expect_type(root, PROTO_TYPE_ECHO);
    if (rc != PROTO_OK)
        return rc;
    rc = json_object_get_string(root, "token", &token, &len);
    if (rc != JSON_OK)
        return map_status(rc);
    copy_str(out->token, sizeof out->token, token, len, NULL);
    out->token_len = len < sizeof out->token ? len : sizeof out->token - 1;
    return PROTO_OK;
}

int protocol_parse_config(const json_value *root, proto_config *out, proto_diag *diag)
{
    const json_value *order_list = NULL;
    size_t total;
    size_t i;
    int rc;

    if (out == NULL)
        return PROTO_ERR_TYPE;
    memset(out, 0, sizeof *out);

    rc = expect_type(root, PROTO_TYPE_CONFIG);
    if (rc != PROTO_OK)
        return rc;

    rc = get_req_str(root, "strategy", out->strategy, sizeof out->strategy, diag);
    if (rc != PROTO_OK)
        return rc;

    rc = json_object_get_array(root, "order_list", &order_list);
    if (rc == JSON_ERR_MISSING)
        return PROTO_OK;
    if (rc != JSON_OK)
        return PROTO_ERR_RANGE;

    total = json_array_size(order_list);
    for (i = 0; i < total; i++) {
        const json_value *entry = json_array_get(order_list, i);
        proto_order_plan *plan;

        if (json_typeof(entry) != JSON_OBJECT) {
            if (diag != NULL)
                diag->dropped_items++;
            continue;
        }
        if (out->order_count >= PROTO_MAX_ORDERS) {
            if (diag != NULL)
                diag->dropped_items += total - i;
            break;
        }

        plan = &out->orders[out->order_count];
        memset(plan, 0, sizeof *plan);
        rc = get_str(entry, "id", plan->id, sizeof plan->id, diag);
        if (rc != PROTO_OK)
            return rc;
        rc = get_str(entry, "recipe", plan->recipe, sizeof plan->recipe, diag);
        if (rc != PROTO_OK)
            return rc;
        rc = get_num(entry, "arrival_time", &plan->arrival_time);
        if (rc != PROTO_OK)
            return rc;
        rc = get_num(entry, "deadline", &plan->deadline);
        if (rc != PROTO_OK)
            return rc;
        out->order_count++;
    }
    return PROTO_OK;
}

int protocol_parse_action(const json_value *root, proto_action *out, proto_diag *diag)
{
    const json_value *target = NULL;
    int rc;

    if (out == NULL)
        return PROTO_ERR_TYPE;
    memset(out, 0, sizeof *out);

    rc = expect_type(root, PROTO_TYPE_ACTION);
    if (rc != PROTO_OK)
        return rc;

    rc = get_req_int(root, "chef_id", &out->chef_id);
    if (rc != PROTO_OK)
        return rc;
    rc = get_req_str(root, "action", out->action, sizeof out->action, diag);
    if (rc != PROTO_OK)
        return rc;

    rc = json_object_get_object(root, "target", &target);
    if (rc == JSON_OK) {
        out->has_target = 1;
        rc = get_num(target, "x", &out->target_x);
        if (rc != PROTO_OK)
            return rc;
        rc = get_num(target, "y", &out->target_y);
        if (rc != PROTO_OK)
            return rc;
    } else if (rc != JSON_ERR_MISSING) {
        return PROTO_ERR_RANGE;
    }

    rc = get_str(root, "target_object", out->target_object, sizeof out->target_object, diag);
    if (rc != PROTO_OK)
        return rc;
    rc = get_str(root, "button", out->button, sizeof out->button, diag);
    if (rc != PROTO_OK)
        return rc;
    rc = get_num(root, "hold_ms", &out->hold_ms);
    if (rc != PROTO_OK)
        return rc;

    if (strcmp(out->action, PROTO_ACTION_PATH) == 0) {
        const json_value *waypoints = NULL;
        size_t total, i;
        rc = get_req_int(root, "seq", &out->seq);
        if (rc != PROTO_OK)
            return rc;
        if (out->seq < 0)
            return PROTO_ERR_RANGE;
        rc = json_object_get_array(root, "waypoints", &waypoints);
        if (rc != JSON_OK)
            return PROTO_ERR_MISSING;
        total = json_array_size(waypoints);
        if (total < 1 || total > PROTO_MAX_WAYPOINTS)
            return PROTO_ERR_RANGE;
        for (i = 0; i < total; i++) {
            const json_value *wp = json_array_get(waypoints, i);
            if (json_typeof(wp) != JSON_OBJECT)
                return PROTO_ERR_RANGE;
            rc = get_num(wp, "x", &out->waypoints[i].x);
            if (rc != PROTO_OK)
                return rc;
            rc = get_num(wp, "y", &out->waypoints[i].y);
            if (rc != PROTO_OK)
                return rc;
        }
        out->waypoint_count = total;
    } else if (strcmp(out->action, PROTO_ACTION_MOVE) == 0) {
        if (!out->has_target)
            return PROTO_ERR_MISSING;
    } else if (strcmp(out->action, PROTO_ACTION_INTERACT) == 0 ||
               strcmp(out->action, PROTO_ACTION_WORK) == 0) {
        if (out->target_object[0] == '\0')
            return PROTO_ERR_MISSING;
    } else if (strcmp(out->action, PROTO_ACTION_FACE) == 0) {
        if (!out->has_target && out->target_object[0] == '\0')
            return PROTO_ERR_MISSING;
    }
    return PROTO_OK;
}

/* ------------------------------------------------------------------ */
/* builders                                                            */
/* ------------------------------------------------------------------ */

/* The writer's error is sticky, so a builder writes the whole message and looks
 * once at the end instead of testing every call. */
static int w_done(json_writer *w, size_t *out_len)
{
    int rc = json_writer_error(w);

    if (rc != JSON_OK)
        return rc;
    if (out_len != NULL)
        *out_len = json_writer_length(w);
    return PROTO_OK;
}

int protocol_build_heartbeat(long long timestamp, char *out, size_t cap,
                             size_t *out_len)
{
    json_writer w;

    json_writer_init(&w, out, cap);
    json_writer_object_begin(&w);
    json_writer_key(&w, "type");
    json_writer_string(&w, PROTO_TYPE_HEARTBEAT);
    json_writer_key(&w, "timestamp");
    json_writer_int(&w, timestamp);
    json_writer_object_end(&w);
    return w_done(&w, out_len);
}

int protocol_build_hello(const char *instance, const char *version,
                         const char *const *actions, size_t action_count,
                         char *out, size_t cap, size_t *out_len)
{
    json_writer w;
    size_t i;

    json_writer_init(&w, out, cap);
    json_writer_object_begin(&w);
    json_writer_key(&w, "type");
    json_writer_string(&w, PROTO_TYPE_HELLO);
    json_writer_key(&w, "instance");
    json_writer_string(&w, instance == NULL ? "" : instance);
    json_writer_key(&w, "version");
    json_writer_string(&w, version == NULL ? "" : version);
    json_writer_key(&w, "actions");
    json_writer_array_begin(&w);
    for (i = 0; i < action_count; i++)
        json_writer_string(&w, actions[i] == NULL ? "" : actions[i]);
    json_writer_array_end(&w);
    json_writer_object_end(&w);
    return w_done(&w, out_len);
}

int protocol_build_ack(const char *action, long long chef_id, int ok,
                       const char *detail, char *out, size_t cap, size_t *out_len)
{
    json_writer w;

    json_writer_init(&w, out, cap);
    json_writer_object_begin(&w);
    json_writer_key(&w, "type");
    json_writer_string(&w, PROTO_TYPE_ACK);
    json_writer_key(&w, "action");
    json_writer_string(&w, action == NULL ? "" : action);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef_id);
    json_writer_key(&w, "ok");
    json_writer_bool(&w, ok != 0);
    json_writer_key(&w, "detail");
    json_writer_string(&w, detail == NULL ? "" : detail);
    json_writer_object_end(&w);
    return w_done(&w, out_len);
}

int protocol_build_echo_reply(const char *token, char *out, size_t cap, size_t *out_len)
{
    json_writer w;

    json_writer_init(&w, out, cap);
    json_writer_object_begin(&w);
    json_writer_key(&w, "type");
    json_writer_string(&w, PROTO_TYPE_ECHO_REPLY);
    json_writer_key(&w, "token");
    json_writer_string(&w, token == NULL ? "" : token);
    json_writer_object_end(&w);
    return w_done(&w, out_len);
}

int protocol_build_action_move(long long chef_id, double x, double y,
                               char *out, size_t cap, size_t *out_len)
{
    json_writer w;

    json_writer_init(&w, out, cap);
    json_writer_object_begin(&w);
    json_writer_key(&w, "type");
    json_writer_string(&w, PROTO_TYPE_ACTION);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef_id);
    json_writer_key(&w, "action");
    json_writer_string(&w, PROTO_ACTION_MOVE);
    json_writer_key(&w, "target");
    json_writer_object_begin(&w);
    json_writer_key(&w, "x");
    json_writer_double(&w, x);
    json_writer_key(&w, "y");
    json_writer_double(&w, y);
    json_writer_object_end(&w);
    json_writer_object_end(&w);
    return w_done(&w, out_len);
}

/*
 * stop/pickup/dash are all "just the name" actions: no target, no object, no
 * duration. The plugin's InputInjector switches on the name, so one body keeps
 * the three byte-identical apart from it.
 */
static int build_bare_action(long long chef_id, const char *name,
                             char *out, size_t cap, size_t *out_len)
{
    json_writer w;

    json_writer_init(&w, out, cap);
    json_writer_object_begin(&w);
    json_writer_key(&w, "type");
    json_writer_string(&w, PROTO_TYPE_ACTION);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef_id);
    json_writer_key(&w, "action");
    json_writer_string(&w, name);
    json_writer_object_end(&w);
    return w_done(&w, out_len);
}

int protocol_build_action_stop(long long chef_id, char *out, size_t cap, size_t *out_len)
{
    return build_bare_action(chef_id, PROTO_ACTION_STOP, out, cap, out_len);
}

int protocol_build_action_pickup(long long chef_id, char *out, size_t cap, size_t *out_len)
{
    return build_bare_action(chef_id, PROTO_ACTION_PICKUP, out, cap, out_len);
}

int protocol_build_action_dash(long long chef_id, char *out, size_t cap, size_t *out_len)
{
    return build_bare_action(chef_id, PROTO_ACTION_DASH, out, cap, out_len);
}

int protocol_build_action_interact(long long chef_id, const char *target_object,
                                   char *out, size_t cap, size_t *out_len)
{
    json_writer w;

    json_writer_init(&w, out, cap);
    json_writer_object_begin(&w);
    json_writer_key(&w, "type");
    json_writer_string(&w, PROTO_TYPE_ACTION);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef_id);
    json_writer_key(&w, "action");
    json_writer_string(&w, PROTO_ACTION_INTERACT);
    json_writer_key(&w, "target_object");
    json_writer_string(&w, target_object == NULL ? "" : target_object);
    json_writer_object_end(&w);
    return w_done(&w, out_len);
}

int protocol_build_action_path(long long chef_id, long long seq,
                               const proto_waypoint *waypoints, size_t waypoint_count,
                               char *out, size_t cap, size_t *out_len)
{
    json_writer w;
    size_t i;

    if (waypoints == NULL || waypoint_count == 0 || waypoint_count > PROTO_MAX_WAYPOINTS
        || seq < 0)
        return PROTO_ERR_RANGE;

    json_writer_init(&w, out, cap);
    json_writer_object_begin(&w);
    json_writer_key(&w, "type");
    json_writer_string(&w, PROTO_TYPE_ACTION);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef_id);
    json_writer_key(&w, "action");
    json_writer_string(&w, PROTO_ACTION_PATH);
    json_writer_key(&w, "seq");
    json_writer_int(&w, seq);
    json_writer_key(&w, "waypoints");
    json_writer_array_begin(&w);
    for (i = 0; i < waypoint_count; i++) {
        json_writer_object_begin(&w);
        json_writer_key(&w, "x");
        json_writer_double(&w, waypoints[i].x);
        json_writer_key(&w, "y");
        json_writer_double(&w, waypoints[i].y);
        json_writer_object_end(&w);
    }
    json_writer_array_end(&w);
    json_writer_object_end(&w);
    return w_done(&w, out_len);
}

int protocol_build_action_face(long long chef_id, int has_target, double x, double y,
                               const char *target_object, char *out, size_t cap, size_t *out_len)
{
    json_writer w;

    if (!has_target && (target_object == NULL || target_object[0] == '\0'))
        return PROTO_ERR_MISSING;

    json_writer_init(&w, out, cap);
    json_writer_object_begin(&w);
    json_writer_key(&w, "type");
    json_writer_string(&w, PROTO_TYPE_ACTION);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef_id);
    json_writer_key(&w, "action");
    json_writer_string(&w, PROTO_ACTION_FACE);
    if (has_target) {
        json_writer_key(&w, "target");
        json_writer_object_begin(&w);
        json_writer_key(&w, "x");
        json_writer_double(&w, x);
        json_writer_key(&w, "y");
        json_writer_double(&w, y);
        json_writer_object_end(&w);
    }
    if (target_object != NULL && target_object[0] != '\0') {
        json_writer_key(&w, "target_object");
        json_writer_string(&w, target_object);
    }
    json_writer_object_end(&w);
    return w_done(&w, out_len);
}

int protocol_build_action_work(long long chef_id, const char *target_object,
                               char *out, size_t cap, size_t *out_len)
{
    json_writer w;

    if (target_object == NULL || target_object[0] == '\0')
        return PROTO_ERR_MISSING;

    json_writer_init(&w, out, cap);
    json_writer_object_begin(&w);
    json_writer_key(&w, "type");
    json_writer_string(&w, PROTO_TYPE_ACTION);
    json_writer_key(&w, "chef_id");
    json_writer_int(&w, chef_id);
    json_writer_key(&w, "action");
    json_writer_string(&w, PROTO_ACTION_WORK);
    json_writer_key(&w, "target_object");
    json_writer_string(&w, target_object);
    json_writer_object_end(&w);
    return w_done(&w, out_len);
}
