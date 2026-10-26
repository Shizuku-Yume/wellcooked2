/*
 * macro.c -- 工序 → 宏动作展开，以及按世界状态推进的状态机。
 *
 * 这里没有时钟、没有共享内存、没有动作发布：所有外部事实都由调用方传进来，
 * 所以 test_macro 可以用合成世界状态把"等 1.5 s 超时"压缩成几微秒。Worker 只负责
 * 导航目标槽/计划槽与把 out 翻译成 kos_input_action。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <kitchenos/macro.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <kitchenos/nav.h>
#include <kitchenos/stand.h>

#define MACRO_PI 3.14159265358979323846f
#define MACRO_PROGRESS_DONE 0.999f

/* 超时表：与 spec L2 的表格一一对应。 */
#define MACRO_PICKUP_TIMEOUT_MS 1500
#define MACRO_DROP_TIMEOUT_MS 1500
#define MACRO_START_TIMEOUT_MS 1500
#define MACRO_DELIVER_TIMEOUT_MS 1500
#define MACRO_CHOP_MARGIN_MS 3000
#define MACRO_WASH_MARGIN_MS 3000
#define MACRO_POUR_MARGIN_MS 3000
/* 倒料时看不到源锅（插件没上报那只锅）时的兜底交互窗口：一次 interact 就够把料倒过去，
 * 800 ms 只用来覆盖按键与状态上报的往返。 */
#define MACRO_POUR_FALLBACK_MS 800
/* 四人同场时 PIBT 让路、反复重规划，实机 `_3` 里 goto 在快到站时（剩 0~1 个路点）
 * 被 3 s 余量判超时；6 s 覆盖一次让路往返。 */
#define MACRO_GOTO_MARGIN_MS 6000
#define MACRO_GOTO_NO_DISTANCE_MS 30000

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

const char *kos_macro_kind_name(int kind)
{
    switch (kind) {
    case KOS_MACRO_GOTO:
        return "goto";
    case KOS_MACRO_PICKUP:
        return "pickup";
    case KOS_MACRO_DROP:
        return "drop";
    case KOS_MACRO_CHOP:
        return "chop";
    case KOS_MACRO_START_COOK:
        return "start_cook";
    case KOS_MACRO_POUR:
        return "pour";
    case KOS_MACRO_WASH:
        return "wash";
    case KOS_MACRO_DELIVER:
        return "deliver";
    default:
        return "unknown";
    }
}

const char *kos_macro_action_name(int action)
{
    switch (action) {
    case KOS_MACRO_ACT_STOP:
        return "stop";
    case KOS_MACRO_ACT_NAVIGATE:
        return "navigate";
    case KOS_MACRO_ACT_FACE:
        return "face";
    case KOS_MACRO_ACT_INTERACT:
        return "interact";
    case KOS_MACRO_ACT_WORK:
        return "work";
    default:
        return "unknown";
    }
}

/* node_id 形如 "fetch:Carrot"；冒号后是游戏里的物品/菜名。 */
static const char *node_item(const char *node_id)
{
    const char *colon;

    if (node_id == NULL) {
        return "";
    }
    colon = strchr(node_id, ':');
    return colon != NULL ? colon + 1 : "";
}

static void step_set(kos_macro_step *step, int kind, const char *station_id,
                     const char *item, int duration_ms)
{
    memset(step, 0, sizeof *step);
    step->kind = kind;
    copy_str(step->station_id, sizeof step->station_id, station_id);
    copy_str(step->item, sizeof step->item, item);
    step->duration_ms = duration_ms > 0 ? duration_ms : 0;
}

static int add_step(kos_macro_plan *plan, int kind, const char *station_id,
                    const char *item, int duration_ms)
{
    if (plan->step_count >= KOS_MACRO_MAX_STEPS) {
        return -1;
    }
    step_set(&plan->steps[plan->step_count++], kind, station_id, item, duration_ms);
    return 0;
}

int kos_macro_plan_build_with_source(const kos_kitchen *kitchen, const char *op, const char *node_id,
                                     const char *station_id, const char *source_station_id,
                                     const char *order_id, int duration_ms,
                                     kos_macro_plan *out, kos_error *err)
{
    const char *item = node_item(node_id);
    int rc = KOS_OK;

    if (err != NULL) {
        kos_error_clear(err);
    }
    if (kitchen == NULL || op == NULL || station_id == NULL || out == NULL) {
        return KOS_ERR_INVAL;
    }
    if (kos_kitchen_station_index(kitchen, station_id) < 0) {
        if (err != NULL) {
            snprintf(err->context, sizeof err->context, "工位不在 kitchen 里: %s",
                     station_id);
        }
        return KOS_ERR_MISSING;
    }
    memset(out, 0, sizeof *out);
    copy_str(out->order_id, sizeof out->order_id, order_id);

    if (strcmp(op, "fetch") == 0) {
        if (item[0] == '\0') {
            rc = KOS_ERR_INVAL;
        } else {
            (void)add_step(out, KOS_MACRO_GOTO, station_id, "", duration_ms);
            (void)add_step(out, KOS_MACRO_PICKUP, station_id, item, duration_ms);
        }
    } else if (strcmp(op, "cut") == 0) {
        if (item[0] == '\0') {
            rc = KOS_ERR_INVAL;
        } else {
            (void)add_step(out, KOS_MACRO_GOTO, station_id, "", duration_ms);
            (void)add_step(out, KOS_MACRO_DROP, station_id, "", duration_ms);
            (void)add_step(out, KOS_MACRO_CHOP, station_id, item, duration_ms);
            /* 切完的 prefab 名与原料名不一定同源（BurritoMeat → DicedMeat），按"拿到任意物品"判。 */
            (void)add_step(out, KOS_MACRO_PICKUP, station_id, "", duration_ms);
        }
    } else if (strcmp(op, "cook") == 0) {
        (void)add_step(out, KOS_MACRO_GOTO, station_id, "", duration_ms);
        (void)add_step(out, KOS_MACRO_DROP, station_id, "", duration_ms);
        (void)add_step(out, KOS_MACRO_START_COOK, station_id, "", duration_ms);
    } else if (strcmp(op, "pour") == 0) {
        /* 端起源锅 → 到容器前按下 → 源锅里那味料消失 → 锅放回灶上。
         * 倒的时候锅一直在手上（实测），所以不能用 drop 的"手空"判完成。 */
        if (item[0] == '\0' || source_station_id == NULL || source_station_id[0] == '\0') {
            rc = KOS_ERR_INVAL;
        } else if (kos_kitchen_station_index(kitchen, source_station_id) < 0) {
            rc = KOS_ERR_MISSING;
        } else {
            (void)add_step(out, KOS_MACRO_GOTO, source_station_id, "", duration_ms);
            (void)add_step(out, KOS_MACRO_PICKUP, source_station_id, "", duration_ms);
            (void)add_step(out, KOS_MACRO_GOTO, station_id, "", duration_ms);
            (void)add_step(out, KOS_MACRO_POUR, station_id, item, duration_ms);
            (void)add_step(out, KOS_MACRO_GOTO, source_station_id, "", duration_ms);
            (void)add_step(out, KOS_MACRO_DROP, source_station_id, "", duration_ms);
        }
    } else if (strcmp(op, "plate") == 0) {
        /* 实测：把手里整份菜放到干净盘堆上，游戏在下面垫一只盘子；再按一次取走那只盘。
         * 盘上原本放着什么由前一道工序决定（寿司=切好的料，卷饼=玉米饼），这里不再去灶上取料。 */
        if (item[0] == '\0') {
            rc = KOS_ERR_INVAL;
        } else {
            (void)add_step(out, KOS_MACRO_GOTO, station_id, "", duration_ms);
            (void)add_step(out, KOS_MACRO_DROP, station_id, "", duration_ms);
            (void)add_step(out, KOS_MACRO_PICKUP, station_id, "Plate", duration_ms);
        }
    } else if (strcmp(op, "pickup") == 0) {
        /* 从台面上的容器里拿成品（卷饼）。节点名是成品（菜名），而台面上那个物体叫
         * Tortilla，所以按"拿到任意物品"判完成，别拿名字去猜。 */
        (void)add_step(out, KOS_MACRO_GOTO, station_id, "", duration_ms);
        (void)add_step(out, KOS_MACRO_PICKUP, station_id, "", duration_ms);
    } else if (strcmp(op, "place") == 0) {
        /* 把手里容器放到台面（玉米饼当容器）；组合由后续 pour 完成。 */
        (void)add_step(out, KOS_MACRO_GOTO, station_id, "", duration_ms);
        (void)add_step(out, KOS_MACRO_DROP, station_id, "", duration_ms);
    } else if (strcmp(op, "deliver") == 0) {
        (void)add_step(out, KOS_MACRO_GOTO, station_id, "", duration_ms);
        (void)add_step(out, KOS_MACRO_DELIVER, station_id, "", duration_ms);
    } else if (strcmp(op, "wash") == 0) {
        (void)add_step(out, KOS_MACRO_GOTO, station_id, "", duration_ms);
        (void)add_step(out, KOS_MACRO_DROP, station_id, "", duration_ms);
        (void)add_step(out, KOS_MACRO_WASH, station_id, "", duration_ms);
    } else if (strcmp(op, "mix") == 0) {
        if (item[0] == '\0') {
            rc = KOS_ERR_INVAL;
        } else {
            /* 搅拌与切菜共用"按住加工键直到 progress=1"的机制。 */
            (void)add_step(out, KOS_MACRO_GOTO, station_id, "", duration_ms);
            (void)add_step(out, KOS_MACRO_DROP, station_id, "", duration_ms);
            (void)add_step(out, KOS_MACRO_CHOP, station_id, item, duration_ms);
            (void)add_step(out, KOS_MACRO_PICKUP, station_id, item, duration_ms);
        }
    } else if (strcmp(op, "assemble") == 0) {
        /* 游戏在容器上完成组合；Worker 只负责把手里最后一份料放下。 */
        (void)add_step(out, KOS_MACRO_GOTO, station_id, "", duration_ms);
        (void)add_step(out, KOS_MACRO_DROP, station_id, "", duration_ms);
    } else {
        rc = KOS_ERR_INVAL;
    }

    if (rc != KOS_OK) {
        if (err != NULL) {
            snprintf(err->context, sizeof err->context, "未知工序或缺少目标物品: op=%s node=%s",
                     op, node_id != NULL ? node_id : "");
        }
        memset(out, 0, sizeof *out);
        return rc;
    }
    return KOS_OK;
}

int kos_macro_plan_build(const kos_kitchen *kitchen, const char *op, const char *node_id,
                         const char *station_id, const char *order_id, int duration_ms,
                         kos_macro_plan *out, kos_error *err)
{
    return kos_macro_plan_build_with_source(kitchen, op, node_id, station_id, "", order_id,
                                            duration_ms, out, err);
}
void kos_macro_begin(kos_macro *macro, const kos_macro_plan *plan)
{
    if (macro == NULL) {
        return;
    }
    memset(macro, 0, sizeof *macro);
    if (plan != NULL) {
        macro->plan = *plan;
    }
}

const char *kos_macro_fail_detail(const kos_macro *macro)
{
    return macro == NULL ? "" : macro->fail_detail;
}

/* 比较时忽略大小写，并把 '-' 与 '_' 当同一个字符：cooker-0 ↔ cooker_0。 */
static int name_eq(const char *a, const char *b)
{
    size_t i = 0;

    if (a == NULL || b == NULL) {
        return 0;
    }
    for (;;) {
        char ca = a[i];
        char cb = b[i];

        if (ca == '-' || ca == '_') {
            ca = '_';
        }
        if (cb == '-' || cb == '_') {
            cb = '_';
        }
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
        i++;
    }
}

/* 工位 id 末尾的序号（cooker-0 → 0）；没有数字返回 -1。 */
static int station_ordinal(const char *id)
{
    const char *dash;
    int value = 0;
    int digits = 0;

    if (id == NULL) {
        return -1;
    }
    dash = strrchr(id, '-');
    if (dash == NULL) {
        return -1;
    }
    dash++;
    while (*dash >= '0' && *dash <= '9') {
        value = value * 10 + (*dash - '0');
        digits++;
        dash++;
    }
    return digits > 0 ? value : -1;
}

/*
 * 工位 → 世界状态资源键。插件用 pot_0/cutting_board_0/sink_0，kitchen.json 用
 * cooker-0/board-0/sink-0；先认精确/归一化名，再按同类工位序号找。
 * cooker-N 只认 pot_N、board-N 只认 cutting_board_N：编号规则与插件 StateFrame /
 * Kitchen.ChoppingBoards 的排序一致（任务 09 P0）。
 */
static const kos_resource *find_station_resource(const kos_kitchen *kitchen,
                                                 const kos_macro_step *step,
                                                 const kos_world_state *world)
{
    int station_index;
    const kos_station *station;
    int ordinal;
    int i;

    station_index = kos_kitchen_station_index(kitchen, step->station_id);
    if (station_index < 0) {
        return NULL;
    }
    station = &kitchen->stations[station_index];

    for (i = 0; i < (int)world->resource_count; i++) {
        if (strcmp(world->resources[i].name, step->station_id) == 0) {
            return &world->resources[i];
        }
    }
    for (i = 0; i < (int)world->resource_count; i++) {
        if (name_eq(world->resources[i].name, step->station_id)) {
            return &world->resources[i];
        }
    }

    ordinal = station_ordinal(step->station_id);
    if (ordinal < 0) {
        return NULL;
    }
    if (strcmp(station->kind, "cooker") == 0) {
        char name[KOS_SHM_NAME_MAX];

        snprintf(name, sizeof name, "pot_%d", ordinal);
        for (i = 0; i < (int)world->resource_count; i++) {
            if (strcmp(world->resources[i].name, name) == 0) {
                return &world->resources[i];
            }
        }
    } else if (strcmp(station->kind, "board") == 0) {
        char name[KOS_SHM_NAME_MAX];

        snprintf(name, sizeof name, "cutting_board_%d", ordinal);
        for (i = 0; i < (int)world->resource_count; i++) {
            if (strcmp(world->resources[i].name, name) == 0) {
                return &world->resources[i];
            }
        }
    } else if (strcmp(station->kind, "sink") == 0) {
        char name[KOS_SHM_NAME_MAX];

        snprintf(name, sizeof name, "sink_%d", ordinal);
        for (i = 0; i < (int)world->resource_count; i++) {
            if (strcmp(world->resources[i].name, name) == 0) {
                return &world->resources[i];
            }
        }
    } else if (strcmp(station->kind, "mixer") == 0) {
        char name[KOS_SHM_NAME_MAX];

        snprintf(name, sizeof name, "mixer_%d", ordinal);
        for (i = 0; i < (int)world->resource_count; i++) {
            if (strcmp(world->resources[i].name, name) == 0) {
                return &world->resources[i];
            }
        }
        snprintf(name, sizeof name, "workstation_%d", ordinal);
        for (i = 0; i < (int)world->resource_count; i++) {
            if (strcmp(world->resources[i].name, name) == 0) {
                return &world->resources[i];
            }
        }
    } else if (strcmp(station->kind, "plate_stack") == 0) {
        for (i = 0; i < (int)world->resource_count; i++) {
            if (strcmp(world->resources[i].name, "plate_stack") == 0) {
                return &world->resources[i];
            }
        }
    }
    return NULL;
}

static const kos_chef *find_chef(const kos_world_state *world, int chef_id)
{
    int i;

    for (i = 0; i < (int)world->chef_count; i++) {
        if (world->chefs[i].id == chef_id) {
            return &world->chefs[i];
        }
    }
    return NULL;
}

static int holding_is(const kos_chef *chef, const char *item)
{
    return chef != NULL && item != NULL && item[0] != '\0' &&
           kos_kitchen_name_matches(item, chef->holding);
}

static int facing_towards(const kos_kitchen *kitchen, const kos_macro_step *step,
                          const kos_chef *chef)
{
    int index = kos_kitchen_station_index(kitchen, step->station_id);
    float dx;
    float dz;
    float desired;
    float diff;

    if (index < 0 || chef == NULL) {
        return 0;
    }
    dx = kitchen->stations[index].x - chef->x;
    dz = kitchen->stations[index].z - chef->y;
    if (dx * dx + dz * dz < 0.0001f) {
        return 1; /* 站在工位上：没有"朝哪"的问题，插件也按 0° 处理 */
    }
    desired = atan2f(dx, dz);
    diff = chef->facing - desired;
    while (diff > MACRO_PI) {
        diff -= 2.0f * MACRO_PI;
    }
    while (diff < -MACRO_PI) {
        diff += 2.0f * MACRO_PI;
    }
    return fabsf(diff) < KOS_MACRO_FACE_TOLERANCE_RAD;
}

static int clean_plate_count(const kos_world_state *world, int32_t *out)
{
    int i;

    for (i = 0; i < (int)world->resource_count; i++) {
        if (strcmp(world->resources[i].name, "plate_stack") == 0) {
            *out = world->resources[i].count;
            return 1;
        }
    }
    return 0;
}

static int order_present(const kos_world_state *world, const char *order_id)
{
    int i;

    if (order_id == NULL || order_id[0] == '\0') {
        return 0;
    }
    for (i = 0; i < (int)world->order_count; i++) {
        if (strcmp(world->orders[i].id, order_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int64_t goto_timeout_ms(const kos_kitchen *kitchen, const kos_macro_step *step,
                               const kos_chef *chef)
{
    int station_index = kos_kitchen_station_index(kitchen, step->station_id);
    int cell;
    uint16_t distance;

    if (station_index < 0) {
        return MACRO_GOTO_NO_DISTANCE_MS;
    }
    if (chef == NULL) {
        return MACRO_GOTO_NO_DISTANCE_MS;
    }
    cell = kos_kitchen_nearest_cell(kitchen, chef->x, chef->y);
    if (cell < 0) {
        return MACRO_GOTO_NO_DISTANCE_MS;
    }
    distance = kos_stand_distance(kitchen, station_index, cell);
    if (distance == KOS_STAND_UNREACHABLE) {
        return MACRO_GOTO_NO_DISTANCE_MS;
    }
    return (int64_t)distance * KOS_MACRO_CELL_MS * 2 + MACRO_GOTO_MARGIN_MS;
}

static void fail_step(kos_macro *macro, int64_t game_ms, const char *condition,
                      const char *reason)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    int64_t waited = game_ms - macro->step_start_ms;

    if (waited < 0) {
        waited = 0;
    }
    snprintf(macro->fail_detail, sizeof macro->fail_detail,
             "macro=%s wait=%s %s waited_ms=%lld", kos_macro_kind_name(step->kind),
             condition, reason != NULL ? reason : "", (long long)waited);
}

/* 进入下一步；全部完成时返回 1。 */
static int next_step(kos_macro *macro, kos_macro_output *out)
{
    macro->step_index++;
    macro->step_started = 0;
    memset(out, 0, sizeof *out);
    out->action = KOS_MACRO_ACT_STOP;
    return macro->step_index >= macro->plan.step_count ? 1 : 0;
}

static int step_init(kos_macro *macro, const kos_kitchen *kitchen,
                     const kos_world_state *world, int chef_id, int64_t game_ms)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    const kos_chef *chef = find_chef(world, chef_id);

    macro->step_started = 1;
    macro->step_start_ms = game_ms;
    macro->face_sent = 0;
    macro->face_sent_ms = 0;
    macro->saw_content = 0;
    macro->entry_holding[0] = '\0';
    macro->clean_baseline_valid = 0;
    macro->clean_baseline = 0;

    if (step->kind == KOS_MACRO_GOTO) {
        macro->deadline_ms = game_ms + goto_timeout_ms(kitchen, step, chef);
    } else if (step->kind == KOS_MACRO_PICKUP || step->kind == KOS_MACRO_DROP ||
               step->kind == KOS_MACRO_START_COOK ||
               step->kind == KOS_MACRO_DELIVER) {
        macro->deadline_ms = game_ms + MACRO_PICKUP_TIMEOUT_MS;
    } else if (step->kind == KOS_MACRO_CHOP) {
        macro->deadline_ms = game_ms + step->duration_ms + MACRO_CHOP_MARGIN_MS;
    } else if (step->kind == KOS_MACRO_POUR) {
        macro->deadline_ms = game_ms + step->duration_ms + MACRO_POUR_MARGIN_MS;
    } else if (step->kind == KOS_MACRO_WASH) {
        macro->deadline_ms = game_ms + step->duration_ms + MACRO_WASH_MARGIN_MS;
    } else {
        macro->deadline_ms = game_ms + MACRO_PICKUP_TIMEOUT_MS;
    }

    if (step->kind == KOS_MACRO_DELIVER) {
        if (chef != NULL) {
            copy_str(macro->entry_holding, sizeof macro->entry_holding, chef->holding);
        }
    }
    if (step->kind == KOS_MACRO_WASH) {
        macro->clean_baseline_valid = clean_plate_count(world, &macro->clean_baseline);
    }
    return 0;
}

static int run_goto(kos_macro *macro, const kos_kitchen *kitchen,
                    const kos_world_state *world, int chef_id, int nav_arrived,
                    int64_t game_ms, kos_macro_output *out)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    const kos_chef *chef = find_chef(world, chef_id);

    /* 到站一次就算到站：转身时厨师会被推离格心超过 navd 的 0.3 m，navd 随即把
     * arrived 改回 moving（没有路点）。这时再回到 navigate，Worker 发的是 stop，
     * 插件里的 face 被冲掉，永远转不过去，最后 arrived+face 超时（实机 `_3`）。 */
    if (!nav_arrived && !macro->face_sent) {
        out->action = KOS_MACRO_ACT_NAVIGATE;
        copy_str(out->target_object, sizeof out->target_object, step->station_id);
        if (game_ms >= macro->deadline_ms) {
            fail_step(macro, game_ms, "arrived+face", "timeout");
            return -1;
        }
        return 0;
    }

    if (!macro->face_sent) {
        /* 到站后先发 face；插件只在误差 < 15° 时回 ack，Worker 看不到 ack，
         * 所以完成判据取同一个事实（世界状态的 facing）。 */
        macro->face_sent = 1;
        macro->face_sent_ms = game_ms;
        out->action = KOS_MACRO_ACT_FACE;
        copy_str(out->target_object, sizeof out->target_object, step->station_id);
        return 0;
    }

    if (game_ms > macro->face_sent_ms && facing_towards(kitchen, step, chef)) {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    out->action = KOS_MACRO_ACT_FACE;
    copy_str(out->target_object, sizeof out->target_object, step->station_id);
    if (game_ms >= macro->deadline_ms) {
        fail_step(macro, game_ms, "arrived+face", "timeout");
        return -1;
    }
    return 0;
}

static int run_pickup(kos_macro *macro, const kos_kitchen *kitchen,
                      const kos_world_state *world, int chef_id, int64_t game_ms,
                      kos_macro_output *out)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    const kos_chef *chef = find_chef(world, chef_id);
    const kos_resource *resource = find_station_resource(kitchen, step, world);
    /* 空的 item = 拿到任意物品（切完的 prefab 改了名、端起锅时手上是锅）。 */
    int holding = chef != NULL && chef->holding[0] != '\0';

    if (step->item[0] == '\0' ? holding : holding_is(chef, step->item)) {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    if (resource != NULL) {
        if (resource->content[0] != '\0') {
            macro->saw_content = 1;
        } else if (macro->saw_content) {
            fail_step(macro, game_ms, "holding", "source-gone");
            return -1;
        }
    }
    if (game_ms >= macro->deadline_ms) {
        fail_step(macro, game_ms, "holding", "timeout");
        return -1;
    }
    out->action = KOS_MACRO_ACT_INTERACT;
    copy_str(out->target_object, sizeof out->target_object, step->station_id);
    return 0;
}

static int run_drop(kos_macro *macro, const kos_chef *chef,
                    const kos_macro_step *step, int64_t game_ms, kos_macro_output *out)
{
    if (chef != NULL && chef->holding[0] == '\0') {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    if (game_ms >= macro->deadline_ms) {
        fail_step(macro, game_ms, "empty-hand", "timeout");
        return -1;
    }
    out->action = KOS_MACRO_ACT_INTERACT;
    copy_str(out->target_object, sizeof out->target_object, step->station_id);
    return 0;
}

static int run_chop(kos_macro *macro, const kos_kitchen *kitchen,
                    const kos_world_state *world, int64_t game_ms, kos_macro_output *out)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    const kos_resource *resource = find_station_resource(kitchen, step, world);

    if (resource == NULL) {
        fail_step(macro, game_ms, "progress=1", "no-station-resource");
        return -1;
    }
    if (resource->progress >= MACRO_PROGRESS_DONE) {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    if (resource->content[0] != '\0') {
        macro->saw_content = 1;
    } else if (macro->saw_content) {
        fail_step(macro, game_ms, "progress=1", "content-gone");
        return -1;
    }
    if (game_ms >= macro->deadline_ms) {
        fail_step(macro, game_ms, "progress=1", "timeout");
        return -1;
    }
    out->action = KOS_MACRO_ACT_WORK;
    copy_str(out->target_object, sizeof out->target_object, step->station_id);
    return 0;
}

static int run_start_cook(kos_macro *macro, const kos_kitchen *kitchen,
                          const kos_world_state *world, int64_t game_ms,
                          kos_macro_output *out)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    const kos_resource *resource = find_station_resource(kitchen, step, world);

    if (resource == NULL) {
        fail_step(macro, game_ms, "pot=cooking", "no-station-resource");
        return -1;
    }
    if (strcmp(resource->state, "cooking") == 0 || strcmp(resource->state, "cooked") == 0) {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    if (strcmp(resource->state, "burning") == 0) {
        fail_step(macro, game_ms, "pot=cooking", "burning");
        return -1;
    }
    if (resource->content[0] != '\0') {
        macro->saw_content = 1;
    } else if (macro->saw_content) {
        fail_step(macro, game_ms, "pot=cooking", "content-gone");
        return -1;
    }
    if (game_ms >= macro->deadline_ms) {
        fail_step(macro, game_ms, "pot=cooking", "timeout");
        return -1;
    }
    out->action = KOS_MACRO_ACT_INTERACT;
    copy_str(out->target_object, sizeof out->target_object, step->station_id);
    return 0;
}

/*
 * 手上的锅/容器（G1/G5）：插件按 `carried_<chefId>` 上报手持的锅/盘，容器内容进
 * contents[]（content 是拼串兼容形式）。旧插件没有这一项，返回 NULL。
 * 这个名字由 macro 侧直接认；proto 的 find_station_resource 只负责"按位置找工位上的
 * 资源"，不覆盖"跟着人走"的锅。
 */
static const kos_resource *find_carried_resource(const kos_world_state *world, int chef_id)
{
    char name[KOS_SHM_NAME_MAX];
    int i;

    if (chef_id < 0 || world == NULL) {
        return NULL;
    }
    snprintf(name, sizeof name, "carried_%d", chef_id);
    for (i = 0; i < (int)world->resource_count; i++) {
        if (strcmp(world->resources[i].name, name) == 0) {
            return &world->resources[i];
        }
    }
    return NULL;
}

/*
 * 倒料：手上端着锅/容器，对着台面容器按 interact，直到锅里的料进到台面上。
 *
 * 完成判据取世界状态的"内容"，不按资源编号（锅跟着人走，pot_N 是灶台槽位、不是锅）：
 *   1. 插件报了 `carried_<chefId>`：先看到它的 content 非空（锅里确实有料），随后变空
 *      = 倒完了。一帧都没见过内容（插件的 contents 还没接上）不算完成，走兜底；
 *   2. 没有 carried_（旧插件）：退回"源锅里那味料从 pot_* 里消失"；
 *   3. 整段都没有可用信号时按 MACRO_POUR_FALLBACK_MS 兜底，别把一次成功的倒料拖成超时。
 * 目的台面（容器）的位置由 kitchen.json 的 station_id 给出，状态侧由 proto 的
 * find_station_resource 按位置找；只有"没有 carried_ 的旧状态"才拿它的内容当第二信号
 * （新状态里 carried_ 是权威：同一张台面可能被两单共用，内容分不清是谁的）。
 */
static int run_pour(kos_macro *macro, const kos_kitchen *kitchen, const kos_world_state *world,
                    int chef_id, int64_t game_ms, kos_macro_output *out)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    const kos_resource *carried = find_carried_resource(world, chef_id);
    const kos_resource *counter = find_station_resource(kitchen, step, world);
    int done = 0;

    if (carried != NULL) {
        if (carried->content[0] != '\0') {
            macro->saw_content = 1;
        } else if (macro->saw_content) {
            done = 1;
        }
    } else {
        int found = 0;
        int i;

        for (i = 0; i < (int)world->resource_count; i++) {
            const kos_resource *resource = &world->resources[i];

            if (strncmp(resource->name, "pot_", 4) != 0 || resource->content[0] == '\0') {
                continue;
            }
            if (kos_kitchen_name_matches(step->item, resource->content)) {
                found = 1;
                break;
            }
        }
        /* 台面容器上有内容（contents[] 里有料）说明这一步的料已经倒进去了；
         * 台面资源由 proto 的 find_station_resource 按位置找，旧世界状态里没有这一项。 */
        if (counter != NULL && counter->content[0] != '\0') {
            found = 1;
        }
        if (found) {
            macro->saw_content = 1;
        } else if (macro->saw_content) {
            done = 1;
        }
    }
    if (!done && !macro->saw_content &&
        game_ms - macro->step_start_ms >= MACRO_POUR_FALLBACK_MS) {
        done = 1;
    }
    if (done) {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    if (game_ms >= macro->deadline_ms) {
        fail_step(macro, game_ms, "source-empty", "timeout");
        return -1;
    }
    out->action = KOS_MACRO_ACT_INTERACT;
    copy_str(out->target_object, sizeof out->target_object, step->station_id);
    return 0;
}

static int run_wash(kos_macro *macro, const kos_kitchen *kitchen,
                    const kos_world_state *world, int64_t game_ms, kos_macro_output *out)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    const kos_resource *resource = find_station_resource(kitchen, step, world);
    int32_t clean = 0;
    int have_clean;

    if (resource == NULL) {
        fail_step(macro, game_ms, "progress=1+clean", "no-station-resource");
        return -1;
    }
    have_clean = clean_plate_count(world, &clean);
    if (resource->progress >= MACRO_PROGRESS_DONE &&
        (!macro->clean_baseline_valid || (have_clean && clean > macro->clean_baseline))) {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    if (game_ms >= macro->deadline_ms) {
        fail_step(macro, game_ms, "progress=1+clean", "timeout");
        return -1;
    }
    out->action = KOS_MACRO_ACT_WORK;
    copy_str(out->target_object, sizeof out->target_object, step->station_id);
    return 0;
}

static int run_deliver(kos_macro *macro, const kos_world_state *world,
                       const kos_chef *chef, const kos_macro_step *step,
                       int64_t game_ms, kos_macro_output *out)
{
    if (!order_present(world, macro->plan.order_id)) {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    /* 手上本来拿着菜，等订单消失时菜先没了：这是完成条件被打破，不必等到超时。 */
    if (chef != NULL && macro->entry_holding[0] != '\0' && chef->holding[0] == '\0') {
        fail_step(macro, game_ms, "order-gone", "holding-lost");
        return -1;
    }
    if (game_ms >= macro->deadline_ms) {
        fail_step(macro, game_ms, "order-gone", "timeout");
        return -1;
    }
    out->action = KOS_MACRO_ACT_INTERACT;
    copy_str(out->target_object, sizeof out->target_object, step->station_id);
    return 0;
}

int kos_macro_advance(kos_macro *macro, const kos_kitchen *kitchen,
                      const kos_world_state *world, int chef_id, int nav_arrived,
                      int64_t game_ms, kos_macro_output *out)
{
    const kos_macro_step *step;
    const kos_chef *chef;

    if (macro == NULL || kitchen == NULL || world == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    out->action = KOS_MACRO_ACT_STOP;
    if (macro->step_index >= macro->plan.step_count) {
        return 1;
    }
    if (!macro->step_started) {
        (void)step_init(macro, kitchen, world, chef_id, game_ms);
    }
    if (game_ms < macro->step_start_ms) {
        game_ms = macro->step_start_ms; /* 关卡重开时时钟可能回退，不把回退当超时 */
    }
    step = &macro->plan.steps[macro->step_index];
    chef = find_chef(world, chef_id);

    switch (step->kind) {
    case KOS_MACRO_GOTO:
        return run_goto(macro, kitchen, world, chef_id, nav_arrived, game_ms, out);
    case KOS_MACRO_PICKUP:
        return run_pickup(macro, kitchen, world, chef_id, game_ms, out);
    case KOS_MACRO_DROP:
        return run_drop(macro, chef, step, game_ms, out);
    case KOS_MACRO_CHOP:
        return run_chop(macro, kitchen, world, game_ms, out);
    case KOS_MACRO_START_COOK:
        return run_start_cook(macro, kitchen, world, game_ms, out);
    case KOS_MACRO_POUR:
        return run_pour(macro, kitchen, world, chef_id, game_ms, out);
    case KOS_MACRO_WASH:
        return run_wash(macro, kitchen, world, game_ms, out);
    case KOS_MACRO_DELIVER:
        return run_deliver(macro, world, chef, step, game_ms, out);
    default:
        fail_step(macro, game_ms, "known-macro", "unknown-kind");
        return -1;
    }
}
