/*
 * protocol.h -- the WellCooked message layer (bridge <-> game plugin).
 *
 * Field names are the cross-end contract. Its home is protocol/protocol.md plus
 * protocol/schema/; §5.2 of the project document is where they came from. Both
 * implementations follow it: bridge/src/protocol.c here and
 * plugin/WellCooked.Plugin/Protocol/Messages.cs. Note that the plugin was written
 * by this same effort, so "both ends agree" is agreement by construction; the
 * independent checks are plugin/tools/conformance/ (a peer compiled from the
 * plugin's own sources) and the real-game run. This module only converts between
 * JSON documents and C structs -- it makes no decisions about what to send.
 *
 * Collections are bounded: one line is at most 1 MiB (framing.h), and a fixed
 * layout keeps the bridge allocation-free on the hot path. Overflow never fails a
 * parse, it is counted in proto_diag so the caller can log "dropped N orders" and
 * carry on with the part it understood.
 */
#ifndef WELLCOOKED_PROTOCOL_H
#define WELLCOOKED_PROTOCOL_H

#include <stddef.h>

#include <wellcooked/json.h>

/* Message type names, byte-for-byte with MsgType in the plugin. */
#define PROTO_TYPE_STATE_UPDATE "state_update"
#define PROTO_TYPE_ACTION "action"
#define PROTO_TYPE_CONFIG "config"
#define PROTO_TYPE_HEARTBEAT "heartbeat"
#define PROTO_TYPE_STATE_REQUEST "state_request"
#define PROTO_TYPE_ECHO "echo"
#define PROTO_TYPE_ECHO_REPLY "echo_reply"
#define PROTO_TYPE_HELLO "hello"
#define PROTO_TYPE_ACK "ack"

/* Action names understood by the plugin's InputInjector. The plugin implements
 * eight (move, stop, press, release, tap, pickup, dash, interact); the wire
 * carries these five, which is everything `kos_action_kind` can express.
 * press/release/tap stay plugin-only until something needs to schedule a raw
 * button at a chosen frame (see decisions.md). */
#define PROTO_ACTION_MOVE "move"
#define PROTO_ACTION_STOP "stop" /* the idle action sent for missing input slots */
#define PROTO_ACTION_INTERACT "interact"
#define PROTO_ACTION_PICKUP "pickup" /* take/drop what the chef stands on; no payload */
#define PROTO_ACTION_DASH "dash"     /* sprint in the facing direction; no payload */
#define PROTO_ACTION_PATH "path"
#define PROTO_ACTION_FACE "face"
#define PROTO_ACTION_WORK "work"

/* Identity this side announces in `hello`; the version string is what the plugin
 * sees in its log, so bump it when the wire format changes. */
#define PROTO_BRIDGE_INSTANCE "linux-bridge"
#define PROTO_VERSION "0.3.0"

#define PROTO_MAX_CHEFS 16
#define PROTO_MAX_ORDERS 32
#define PROTO_MAX_INGREDIENTS 8
/* One entry per reportable world object (cookers, boards, stacks, sinks, counters
 * with an item, loose containers, carried containers). _3 tops out around 24;
 * the cap is the receive-side fixed layout, so it stays a little above. */
#define PROTO_MAX_RESOURCES 96
/* Recursive content names per resource/chef; the protocol accepts more than the
 * shm layout (KOS_SHM_MAX_CONTENTS) and the bridge truncates. */
#define PROTO_MAX_CONTENTS 8
#define PROTO_MAX_ORDER_RESULTS 32
#define PROTO_MAX_ACTIONS 32
#define PROTO_MAX_WAYPOINTS 8

#define PROTO_STR_MAX 64 /* recipes, states, details, tokens */
#define PROTO_KEY_MAX 32 /* resource keys and ingredient names */

enum {
    PROTO_OK = 0,
    PROTO_ERR_TYPE = -1,    /* not an object, or `type` is another message */
    PROTO_ERR_MISSING = -2, /* a required member is absent */
    PROTO_ERR_RANGE = -3    /* present but unusable (wrong JSON type, out of range) */
};

typedef enum {
    PROTO_MSG_UNKNOWN = 0,
    PROTO_MSG_STATE_UPDATE,
    PROTO_MSG_ACTION,
    PROTO_MSG_CONFIG,
    PROTO_MSG_HEARTBEAT,
    PROTO_MSG_STATE_REQUEST,
    PROTO_MSG_ECHO,
    PROTO_MSG_ECHO_REPLY,
    PROTO_MSG_HELLO,
    PROTO_MSG_ACK
} proto_msg_type;

const char *protocol_msg_type_name(proto_msg_type type);
proto_msg_type protocol_msg_type_of(const char *name);

/* What the parser had to leave out of an otherwise usable message. */
typedef struct {
    size_t dropped_items;     /* array entries beyond the PROTO_MAX_* cap */
    size_t truncated_strings; /* strings longer than the destination field */
} proto_diag;
typedef struct {
    double x;
    double y;
} proto_waypoint;


/* ---- parsed messages ---- */

typedef struct {
    long long id;
    double x;
    double y;
    char holding[PROTO_STR_MAX]; /* "" when the chef carries nothing (JSON null) */
    double facing;
    char state[PROTO_STR_MAX];
    /* Recursive leaves inside the carried item (a plate's dish, a pot's contents).
     * Same encoding as proto_resource.contents. */
    size_t holding_content_count;
    char holding_contents[PROTO_MAX_CONTENTS][PROTO_KEY_MAX];
    /* The game's own selected interactable (InteractWithItemHelper facing cell):
     * its world position is the authority for "pressing A acts on this station",
     * the name is for logs only. */
    int facing_reported; /* the plugin does the lookup at all (even when nothing is faced) */
    int has_facing_target;
    double facing_x;
    double facing_y;
} proto_chef;

typedef struct {
    char id[PROTO_STR_MAX];
    char recipe[PROTO_STR_MAX];
    double time_remaining;
    long long score; /* RecipeListEntry.m_scoreForMeal; has_score distinguishes 0 */
    int has_score;
    size_t ingredient_count;
    char ingredients[PROTO_MAX_INGREDIENTS][PROTO_KEY_MAX];
} proto_order;

/* One finished order. The plugin keeps a bounded ring of the latest results and
 * resends the whole snapshot every frame; consumers dedupe by `seq`. */
typedef struct {
    long long seq;
    char id[PROTO_STR_MAX];
    char recipe[PROTO_STR_MAX];
    char outcome[PROTO_STR_MAX]; /* "delivered" | "expired" | "failed" */
    long long score;
    int has_score;
    long long game_time_ms;
} proto_order_result;

/* `resources` keys are dynamic (pot_0, pot_1, cutting_board_0, plate_stack, and
 * whatever the game grows later), so they are keyed by the raw string and only
 * classified by prefix. Unknown keys are kept, never rejected. */
typedef enum {
    PROTO_RES_UNKNOWN = 0,
    PROTO_RES_POT,           /* pot_<N>: stove slot N, N matches cooker-<N> */
    PROTO_RES_CUTTING_BOARD, /* cutting_board_<N> */
    PROTO_RES_PLATE_STACK,   /* plate_stack */
    PROTO_RES_DIRTY_PLATES,  /* dirty_plates */
    PROTO_RES_SINK,          /* sink_<N> */
    PROTO_RES_COUNTER,       /* counter_<N>: a plain counter that holds an item */
    PROTO_RES_CONTAINER,     /* container_<N>: loose container (floor/attached nowhere) */
    PROTO_RES_CARRIED        /* carried_<chef_id>: a container a chef carries */
} proto_resource_kind;

typedef struct {
    char key[PROTO_KEY_MAX]; /* verbatim, e.g. "pot_7" */
    proto_resource_kind kind;
    int index; /* pot_N / cutting_board_N / sink_N / counter_N / container_N -> N,
                * carried_N -> chef id, otherwise -1 */
    char state[PROTO_STR_MAX];
    char content[PROTO_STR_MAX];
    double time_left;
    int has_time_left;
    long long chef_id;
    int has_chef_id;
    long long count;
    int has_count;
    double progress;
    int has_progress;
    /* ---- state audit G1–G6 ---- */
    char kind_name[PROTO_KEY_MAX]; /* "cooker"/"board"/"plate_stack"/... ; "" = only the key tells */
    int has_pos;
    double x;
    double y;
    char item[PROTO_KEY_MAX]; /* what sits on the station / the container object itself */
    char step[PROTO_KEY_MAX]; /* cooker only: "Pot" / "FryingPan" / "DeepFatFryer" */
    size_t content_count;
    char contents[PROTO_MAX_CONTENTS][PROTO_KEY_MAX];
    /* Cooking state the scheduler needs but `state` cannot carry alone: a burnt pot
     * is still `cooked`, and the pour gate has to know both. Optional; has_* tells
     * "reported false" from "not reported". */
    int has_cooked;
    int cooked;
    int has_burning;
    int burning;
    double burn_in; /* seconds until it catches fire; only while cooked && !burning */
    int has_burn_in;
    int has_fire;
    int fire; /* ClientFlammable.OnFire() on the item or on its station */
} proto_resource;

typedef struct {
    long long timestamp;
    double game_time;
    long long game_time_ms;
    double score;
    double round_time_left; /* G8 */
    double round_time_limit;
    int has_round_time;
    size_t chef_count;
    proto_chef chefs[PROTO_MAX_CHEFS];
    size_t order_count;
    proto_order orders[PROTO_MAX_ORDERS];
    size_t order_result_count;
    proto_order_result order_results[PROTO_MAX_ORDER_RESULTS];
    size_t resource_count;
    proto_resource resources[PROTO_MAX_RESOURCES];
} proto_state_update;

typedef struct {
    long long timestamp;
} proto_heartbeat;

typedef struct {
    char instance[PROTO_STR_MAX];
    char version[PROTO_STR_MAX];
    size_t action_count;
    char actions[PROTO_MAX_ACTIONS][PROTO_KEY_MAX];
} proto_hello;

typedef struct {
    char action[PROTO_STR_MAX];
    long long chef_id;
    int ok;
    long long seq; /* path 的规划序号；插件的 ack 带，其它动作缺省 */
    int has_seq;
    char detail[PROTO_STR_MAX]; /* "" when the plugin omits it */
} proto_ack;

typedef struct {
    char token[PROTO_STR_MAX];
    size_t token_len;
} proto_echo;

typedef struct {
    char id[PROTO_STR_MAX];
    char recipe[PROTO_STR_MAX];
    double arrival_time;
    double deadline;
} proto_order_plan;

typedef struct {
    char strategy[PROTO_STR_MAX];
    size_t order_count;
    proto_order_plan orders[PROTO_MAX_ORDERS];
} proto_config;

typedef struct {
    long long chef_id;
    char action[PROTO_STR_MAX];
    int has_target;
    double target_x;
    double target_y;
    char target_object[PROTO_STR_MAX];
    char button[PROTO_STR_MAX];
    double hold_ms;
    long long seq;
    size_t waypoint_count;
    proto_waypoint waypoints[PROTO_MAX_WAYPOINTS];
} proto_action;

/* ---- parsing ----
 *
 * `root` must come from json_parse(). Each function demands `"type"` to be
 * present and to match its own message, so a mis-dispatched frame is caught here
 * instead of turning into silently zeroed fields. Anything else that is merely
 * absent -- or explicitly JSON null, which is how the game says "no value" --
 * falls back to a default; a member that is present with the wrong JSON type is
 * always PROTO_ERR_RANGE, because that is a bug on the other end and the bridge
 * wants it in the log. Entries of chefs/orders/resources that are not objects
 * are dropped individually and counted in proto_diag, so one malformed chef does
 * not cost the bridge the rest of the kitchen. `diag` is optional. */
proto_msg_type protocol_message_type(const json_value *root);

int protocol_parse_state_update(const json_value *root, proto_state_update *out,
                                proto_diag *diag);
int protocol_parse_heartbeat(const json_value *root, proto_heartbeat *out);
int protocol_parse_hello(const json_value *root, proto_hello *out, proto_diag *diag);
int protocol_parse_ack(const json_value *root, proto_ack *out, proto_diag *diag);
int protocol_parse_echo(const json_value *root, proto_echo *out);
int protocol_parse_config(const json_value *root, proto_config *out, proto_diag *diag);
int protocol_parse_action(const json_value *root, proto_action *out, proto_diag *diag);

/* ---- building ----
 *
 * Each builder writes one complete, NUL-terminated JSON message into `out` and
 * reports its length through `out_len` (optional). No allocation. On failure the
 * buffer holds a partial message and must be thrown away: the code is PROTO_OK or
 * one of the json.h writer codes, JSON_ERR_NOSPACE when `out` cannot hold the
 * message. */
int protocol_build_heartbeat(long long timestamp, char *out, size_t cap,
                             size_t *out_len);
int protocol_build_hello(const char *instance, const char *version,
                         const char *const *actions, size_t action_count,
                         char *out, size_t cap, size_t *out_len);
int protocol_build_ack(const char *action, long long chef_id, int ok,
                       const char *detail, char *out, size_t cap, size_t *out_len);
int protocol_build_echo_reply(const char *token, char *out, size_t cap,
                              size_t *out_len);
int protocol_build_action_move(long long chef_id, double x, double y,
                               char *out, size_t cap, size_t *out_len);
int protocol_build_action_stop(long long chef_id, char *out, size_t cap,
                               size_t *out_len);
int protocol_build_action_interact(long long chef_id, const char *target_object,
                                   int has_target, double x, double y,
                                   char *out, size_t cap, size_t *out_len);
int protocol_build_action_pickup(long long chef_id, char *out, size_t cap,
                                 size_t *out_len);
int protocol_build_action_dash(long long chef_id, char *out, size_t cap,
                               size_t *out_len);
int protocol_build_action_path(long long chef_id, long long seq,
                               const proto_waypoint *waypoints, size_t waypoint_count,
                               char *out, size_t cap, size_t *out_len);
int protocol_build_action_face(long long chef_id, int has_target, double x, double y,
                               const char *target_object, char *out, size_t cap, size_t *out_len);
int protocol_build_action_work(long long chef_id, const char *target_object,
                               char *out, size_t cap, size_t *out_len);

const char *protocol_strerror(int code);

#endif /* WELLCOOKED_PROTOCOL_H */
