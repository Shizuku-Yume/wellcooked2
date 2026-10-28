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

#include <wellcooked/macro.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <wellcooked/nav.h>
#include <wellcooked/stand.h>
#include <wellcooked/navtab.h>

#define MACRO_PI 3.14159265358979323846f
#define MACRO_PROGRESS_DONE 0.999f
/* 游戏选中的交互对象与目标工位的允许偏差：工位按 0.6 m 摆放，0.35 m 不会串到
 * 隔壁工位；实机发现"面前是别的台面"时差值是半格以上。 */
#define MACRO_FOCUS_TOL 0.35f
/* 焦点不对时先原地转身；这么久还不对说明站位格本身错了，重新走位。 */
#define MACRO_FOCUS_RETRY_MS 500
/* goto 完成还必须真的站在该工位的站位格附近（格心 ±0.6 m）。navd 的到达是按
 * 0.3 m 判的，但 target_seq/pin 的一帧时序可能把一个旧到达落到新目标上；实机
 * run 3 的 board-0 就在 2.7 m 外被判到达（worker-2.log:20）。 */
#define MACRO_STAND_ARRIVE_TOL 0.6f
/* 倒料前等锅熟的兜底上限（拿不到 time_left 时）。 */
#define MACRO_POUR_WAIT_MS 15000
/* 倒料失败后把锅送回灶上的上限。 */
#define MACRO_POUR_RECOVER_MS 3000

/* 超时表：与 spec L2 的表格一一对应。 */
#define MACRO_PICKUP_TIMEOUT_MS 1500
/* 盘堆上取盘：实测放料后 1.0–1.3 s 盘子才到手，1.5 s 经常差一点超时（run 011629 worker-0:30）。 */
#define MACRO_PLATE_PICKUP_TIMEOUT_MS 3000
#define MACRO_DROP_TIMEOUT_MS 1500
#define MACRO_START_TIMEOUT_MS 1500
/* Cutting can lose focus while another chef crosses the board; allow one re-focus cycle. */
#define MACRO_CHOP_MARGIN_MS 6000
/* 盘堆/出菜口几何兜底：厨师与工位在非朝向轴上的偏差上限（半格 0.6）。 */
#define MACRO_GEOMETRIC_OFFAXIS_MAX 0.6f
/* goto：navd 没报到站但人已在站位格附近，这么久后按到站处理。 */
#define MACRO_GOTO_NEAR_FALLBACK_MS 1500
#define MACRO_WASH_MARGIN_MS 3000
#define MACRO_POUR_MARGIN_MS 3000
/* 交付时盘子先消失、游戏的订单结果（order_results）与 m_activeOrders 移除都要晚
 * 几百毫秒；手空后等这么久还没有结果才判 holding-lost。 */
#define MACRO_DELIVER_LOST_GRACE_MS 1000
/* 四人同场时 PIBT 让路、反复重规划，实机 `_3` 里 goto 在快到站时（剩 0~1 个路点）
 * 被 3 s 余量判超时；覆盖一次让路往返与完整卡死判定窗口。 */
#define MACRO_GOTO_MARGIN_MS KOS_NAV_BLOCKED_WINDOW_MS
/* goto 的默认超时必须覆盖 navd 判定 BLOCKED 所需的完整窗口（3 次卡住 = 6 s），
 * 再加 2 s 余量（总计 8 s，即 6~8 s 有界超时）。决不能等于或短于 6 s，
 * 否则 Worker 会在 navd 报告第 3 次重规划与 BLOCKED 之前超时失败。 */
#define MACRO_GOTO_BLOCKED_MARGIN_MS 2000
#define MACRO_GOTO_DEFAULT_TIMEOUT_MS (KOS_NAV_BLOCKED_WINDOW_MS + MACRO_GOTO_BLOCKED_MARGIN_MS)

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
        (void)add_step(out, KOS_MACRO_START_COOK, station_id, item, duration_ms);
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
            /* Return to the exact source cooker, even if another cooker has the same kind. */
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

void kos_macro_plan_set_cook_input(kos_macro_plan *plan, const char *label)
{
    if (plan == NULL) {
        return;
    }
    copy_str(plan->cook_input, sizeof plan->cook_input, label);
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

/*
 * 工位 content 与期望物品比对。插件把多个内容物用 '+' 拼成一个串（shm.h 的
 * KOS_SHM_CONTENT_MAX 注释），所以逐段比；忽略大小写与 '-'/'_' 的差异。
 */
static int content_matches(const char *content, const char *item)
{
    const char *start;

    if (content == NULL || item == NULL || item[0] == '\0') {
        return 0;
    }
    start = content;
    for (;;) {
        const char *plus = strchr(start, '+');
        size_t len = plus != NULL ? (size_t)(plus - start) : strlen(start);
        char token[KOS_SHM_CONTENT_MAX];

        if (len >= sizeof token) {
            len = sizeof token - 1;
        }
        memcpy(token, start, len);
        token[len] = '\0';
        if (name_eq(token, item)) {
            return 1;
        }
        if (plus == NULL) {
            return 0;
        }
        start = plus + 1;
    }
}

/*
 * 工位 content 是否还是这一步该有的东西：DROP 时记下的原料或节点产物名之一。
 * 空 content 不算匹配——"什么都没有"由调用方按 content-gone 处理。
 */
static int content_is_expected(const kos_resource *resource, const kos_macro *macro,
                               const kos_macro_step *step)
{
    return resource->content[0] != '\0' &&
           (content_matches(resource->content, macro->dropped_item) ||
            content_matches(resource->content, step->item));
}

/*
 * 板上的内容是否已经从进入这一步时的原料变成了另一个名字。
 * 实机切完游戏把物体换成 Chopped…/Diced…，换过的物体上没有 ClientWorkableItem，
 * 进度读不到；内容改名是唯一还能看到的完成证据。内容可能是 "a+b" 拼串，按 token
 * 比对：旧原料名还在就不算改名。
 */
static int chop_content_renamed(const kos_macro *macro, const kos_resource *resource)
{
    if (macro == NULL || resource == NULL) {
        return 0;
    }
    if (macro->board_item_before[0] == '\0' || resource->content[0] == '\0') {
        return 0;
    }
    return !content_matches(resource->content, macro->board_item_before);
}
/* 定义在后面（倒料与烹饪共用同一套原料名规则）。 */
static int ingredient_word_matches(const char *a, const char *b);

/*
 * 锅里的内容里有没有这一味料。
 *
 * 按"原料词"比（最后一个 CamelCase 词、忽略大小写与复数 s），与倒料同一套规则：
 * 游戏里同一味料在不同工序后的 prefab 名不同源，而锅里上报的是生料名。
 * 实机 manual.log:1833 起：锅里是 [BurritoChicken]，菜谱节点叫 FriedChicken，
 * 剥掉 Fried 得到 Chicken，按整名比永远对不上；[Mushroom] 与 FriedMushrooms 也
 * 只是复数差异。整名相等仍然是成立的（cook_input 就是 BurritoChicken/Rice）。
 */
static int pot_contains_item(const kos_resource *resource, const char *item)
{
    int i;

    if (resource == NULL || item == NULL || item[0] == '\0') {
        return 0;
    }
    if (resource->content[0] != '\0' && ingredient_word_matches(resource->content, item)) {
        return 1;
    }
    for (i = 0; i < (int)resource->content_count && i < KOS_SHM_MAX_CONTENTS; i++) {
        if (resource->contents[i][0] != '\0' &&
            ingredient_word_matches(resource->contents[i], item)) {
            return 1;
        }
    }
    return 0;
}

static const char *cook_input_ingredient(const char *item)
{
    if (item == NULL) {
        return "";
    }
    if (strncmp(item, "Fried", 5) == 0 && item[5] != '\0') {
        return item + 5;
    }
    if (strncmp(item, "Boiled", 6) == 0 && item[6] != '\0') {
        return item + 6;
    }
    if (strncmp(item, "Cooked", 6) == 0 && item[6] != '\0') {
        return item + 6;
    }
    return item;
}

static int resource_content_count(const kos_resource *res)
{
    const char *p;
    int count;

    if (res == NULL) {
        return 0;
    }
    if (res->content_count > 0) {
        return (int)res->content_count;
    }
    if (res->content[0] == '\0') {
        return 0;
    }
    count = 1;
    for (p = res->content; *p != '\0'; p++) {
        if (*p == '+' || *p == ',') {
            count++;
        }
    }
    return count;
}

/*
 * start_cook 的内容身份（S2）：锅里放的必须就是这道菜要的原料。
 *
 * 权威来源是菜谱里 cook 节点的子节点 label（`plan->cook_input`，Worker 从
 * recipes.json 的边推出）：cook:FriedChicken ← cut:BurritoChicken（BurritoChicken）、
 * cook:FriedBurritoMeat ← cut:BurritoMeat、cook:BoiledRice ← fetch:Rice。
 * 实机锅里上报的就是这些生料名（logs/manual/20261027-022317/manual.log:1833 起
 * `锅=[BurritoChicken]`、`[BurritoMeat]`），而厨师手里是切块 prefab
 * （DicedMeat / BurritoChoppedChicken）——拿手持物或"数量变多"当匹配都会认错料。
 *
 * 没有菜谱时退回两个保守来源：DROP 时记下的手持物（游戏确实接受了这次投料）和
 * step->item 按烹饪修饰语（Fried/Boiled/Cooked）剥前缀。数量变化只写进日志当旁证，
 * 绝不单独构成"匹配"：把别人锅里的一份料算成自己放对的，就会开火做错菜。
 */
static int start_cook_content_matches(const kos_resource *resource,
                                      const kos_macro *macro,
                                      const kos_macro_step *step)
{
    const char *ing;

    if (resource == NULL || (resource->content[0] == '\0' && resource->content_count == 0)) {
        return 0;
    }

    if (macro->plan.cook_input[0] != '\0' &&
        pot_contains_item(resource, macro->plan.cook_input)) {
        return 1;
    }
    if (macro->dropped_item[0] != '\0' && pot_contains_item(resource, macro->dropped_item)) {
        return 1;
    }
    if (step->item[0] != '\0' && pot_contains_item(resource, step->item)) {
        return 1;
    }
    ing = cook_input_ingredient(step->item);
    if (ing[0] != '\0' && pot_contains_item(resource, ing)) {
        return 1;
    }
    return 0;
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
 * cooker-0/board-0/sink-0。任务 09 G1 起**先按位置认**（`resources[].pos`），
 * 再退回精确/归一化名与序号：序号来自插件枚举顺序，锅空/满时会变，位置不会。
 * 位置容差 0.3 m —— 台面按 0.6 m 摆放，取半个格距，不会串到隔壁工位。
 */
#define MACRO_STATION_POS_TOL 0.30f

/* 资源 kind 与工位 kind 对齐；老插件不发 kind 时只认键名前缀。 */
static int resource_kind_matches(const kos_resource *res, const kos_station *station)
{
    if (res->kind[0] != '\0')
        return strcmp(res->kind, station->kind) == 0;
    if (strcmp(station->kind, "cooker") == 0)
        return strncmp(res->name, "pot_", 4) == 0;
    if (strcmp(station->kind, "board") == 0)
        return strncmp(res->name, "cutting_board_", 14) == 0;
    if (strcmp(station->kind, "sink") == 0)
        return strncmp(res->name, "sink_", 5) == 0;
    if (strcmp(station->kind, "counter") == 0)
        return strncmp(res->name, "counter_", 8) == 0;
    return 1;
}

static int facing_towards(const kos_kitchen *kitchen, const kos_macro_step *step,
                          const kos_chef *chef);

static const kos_resource *find_station_resource(const kos_kitchen *kitchen,
                                                 const kos_macro_step *step,
                                                 const kos_world_state *world)
{
    int station_index;
    const kos_station *station;
    const kos_resource *nearest = NULL;
    float nearest_d2 = MACRO_STATION_POS_TOL * MACRO_STATION_POS_TOL;
    int ordinal;
    int i;

    station_index = kos_kitchen_station_index(kitchen, step->station_id);
    if (station_index < 0) {
        return NULL;
    }
    station = &kitchen->stations[station_index];

    for (i = 0; i < (int)world->resource_count; i++) {
        float dx;
        float dz;
        float d2;

        if (!world->resources[i].has_pos || !resource_kind_matches(&world->resources[i], station))
            continue;
        dx = world->resources[i].pos_x - station->x;
        dz = world->resources[i].pos_y - station->z;
        d2 = dx * dx + dz * dz;
        if (d2 <= nearest_d2) {
            nearest_d2 = d2;
            nearest = &world->resources[i];
        }
    }
    if (nearest != NULL) {
        return nearest;
    }

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

/*
 * 游戏自己选中的交互对象是不是目标工位（宏按键的唯一授权）。
 *
 * 插件报 `facing_pos`（InteractWithItemHelper.GetFacingGridOccupant 那个物体的
 * 世界坐标）：它直接回答"现在按 A 会作用到谁"，比"厨师朝向哪边"可靠。实机
 * 20261027-131838 的四个失败都是朝向/站位看着对、游戏选中的却是隔壁台面或料箱
 * （LogOutput.run.log:731/1630/2297/…）：这时按 interact/work 只会把料放到错的
 * 地方，或者对着空气切 4.7 s。旧插件没有这个字段时退回几何朝向判断。
 */
static int station_in_focus(const kos_kitchen *kitchen, const kos_macro_step *step,
                            const kos_chef *chef)
{
    int index;

    if (kitchen == NULL || step == NULL) {
        return 0;
    }
    index = kos_kitchen_station_index(kitchen, step->station_id);
    if (index < 0) {
        return 0;
    }
    if (chef != NULL && chef->facing_reported) {
        /* 游戏对出菜口与盘堆的面前目标不可靠：要么不报（实机 20261027-190659
         * worker-0.log:142），要么报成旁边的台面（实机 20261027-200237 worker-0.log:166
         * 转身 1.5 s 仍失焦）。这两类目标对不上时退回几何朝向。 */
        const char *kind = kitchen->stations[index].kind;
        int geometric_ok =
            strcmp(kind, "serving") == 0 || strcmp(kind, "plate_stack") == 0 ||
            (kind[0] == '\0' && (strncmp(step->station_id, "serving", 7) == 0 ||
                                 strncmp(step->station_id, "plate_stack", 11) == 0));

        if (chef->has_facing_target) {
            float dx = chef->facing_x - kitchen->stations[index].x;
            float dz = chef->facing_y - kitchen->stations[index].z;

            if (dx * dx + dz * dz <= MACRO_FOCUS_TOL * MACRO_FOCUS_TOL) {
                /* 选中了也要正对：斜着朝向砧板时按切不进度（实机 20261027-234924 鸡肉
                 * 朝 (0.71,0.71) 切 146 次无进度）。 */
                return facing_towards(kitchen, step, chef);
            }
        }
        /* 其余工位：没选中或选中别的 = 按键会作用到错的对象，不能按。
         * 几何兜底还要求站在相邻格：斜对角时只看朝向轴会对着空气按（实机
         * 20261028-014743 盘堆 (3.0,1.2)、厨师 (4.06,0.20) 朝 -x 连按落空）。 */
        if (geometric_ok && chef != NULL) {
            float ax = fabsf(kitchen->stations[index].x - chef->x);
            float az = fabsf(kitchen->stations[index].z - chef->y);

            if ((ax < az ? ax : az) > MACRO_GEOMETRIC_OFFAXIS_MAX) {
                return 0;
            }
        }
        return geometric_ok ? facing_towards(kitchen, step, chef) : 0;
    }
    return facing_towards(kitchen, step, chef);
}

/*
 * 焦点不对就不按键：先发 FACE（原地转身），超过 MACRO_FOCUS_RETRY_MS 仍不对
 * 就发 NAVIGATE 重新走位。返回 1 = 可以按键，0 = 本帧已发出转向/重走动作。
 */
static int ensure_focus(kos_macro *macro, const kos_kitchen *kitchen,
                        const kos_world_state *world, int chef_id,
                        const kos_macro_step *step, int64_t game_ms,
                        kos_macro_output *out)
{
    const kos_chef *chef = find_chef(world, chef_id);

    if (station_in_focus(kitchen, step, chef)) {
        macro->focus_lost_since_ms = 0;
        return 1;
    }
    if (macro->focus_lost_since_ms == 0) {
        macro->focus_lost_since_ms = game_ms;
        /* 只在每次失焦开始记一条：实机 20261027-200237 出菜口 deliver 转身 1.5 s
         * 仍判失焦，日志里看不到游戏到底选中了什么。 */
        if (chef != NULL) {
            int index = kos_kitchen_station_index(kitchen, step->station_id);

            kos_log_info(macro->log, "macro_focus_lost",
                         "chef=%d station=%s station_pos=(%.2f,%.2f) pos=(%.2f,%.2f) "
                         "facing=%.2f reported=%d has_target=%d target=(%.2f,%.2f)",
                         chef_id, step->station_id,
                         index >= 0 ? kitchen->stations[index].x : 0.0f,
                         index >= 0 ? kitchen->stations[index].z : 0.0f, chef->x, chef->y,
                         chef->facing, chef->facing_reported, chef->has_facing_target,
                         chef->facing_x, chef->facing_y);
        }
    }
    out->action = game_ms - macro->focus_lost_since_ms >= MACRO_FOCUS_RETRY_MS
                      ? KOS_MACRO_ACT_NAVIGATE
                      : KOS_MACRO_ACT_FACE;
    copy_str(out->target_object, sizeof out->target_object, step->station_id);
    return 0;
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
    /* 游戏网格只有四个邻格：期望朝向取离工位最近的坐标轴方向，与插件 face 一致。 */
    if (fabsf(dx) >= fabsf(dz)) {
        dz = 0.0f;
    } else {
        dx = 0.0f;
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

/* 订单最近的游戏结果（插件按 seq 递增重发整个环形快照）。同一 id 只留最新一条。 */
static const kos_order_result *order_result_for(const kos_world_state *world,
                                                const char *order_id)
{
    const kos_order_result *best = NULL;
    int i;

    if (order_id == NULL || order_id[0] == '\0') {
        return NULL;
    }
    for (i = 0; i < (int)world->order_result_count; i++) {
        if (strcmp(world->order_results[i].id, order_id) == 0) {
            best = &world->order_results[i];
        }
    }
    return best;
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

    if (station_index < 0 || chef == NULL) {
        return MACRO_GOTO_DEFAULT_TIMEOUT_MS;
    }
    cell = kos_kitchen_nearest_cell(kitchen, chef->x, chef->y);
    if (cell >= 0) {
        distance = kos_stand_distance(kitchen, station_index, cell);
        if (distance != KOS_STAND_UNREACHABLE) {
            return (int64_t)distance * KOS_MACRO_CELL_MS * 2 + MACRO_GOTO_MARGIN_MS;
        }
    }
    /* 站位格距离未知或不可达：从世界状态估算有效欧氏距离，短兜底，决不放大到 30 s */
    if (station_index < kitchen->station_count) {
        float dx = kitchen->stations[station_index].x - chef->x;
        float dz = kitchen->stations[station_index].z - chef->y;
        float dist_m = sqrtf(dx * dx + dz * dz);
        if (dist_m >= 0.0f && dist_m <= 15.0f) {
            int est_cells = (int)ceilf(dist_m / 0.6f);
            int64_t est_ms = (int64_t)est_cells * KOS_MACRO_CELL_MS * 2 + MACRO_GOTO_MARGIN_MS;
            if (est_ms < MACRO_GOTO_DEFAULT_TIMEOUT_MS) {
                est_ms = MACRO_GOTO_DEFAULT_TIMEOUT_MS;
            }
            if (est_ms > MACRO_GOTO_MARGIN_MS + 2000) {
                est_ms = MACRO_GOTO_MARGIN_MS + 2000;
            }
            return est_ms;
        }
    }
    return MACRO_GOTO_DEFAULT_TIMEOUT_MS;
}

static void fail_step(kos_macro *macro, int64_t game_ms, const char *condition,
                      const char *reason)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    int64_t waited = game_ms - macro->step_start_ms;

    if (waited < 0) {
        waited = 0;
    }
    /* 分两段写，保证不会因为编译器算不出上界而报警；两段都远小于 96 字节。 */
    {
        size_t used = (size_t)snprintf(macro->fail_detail, sizeof macro->fail_detail,
                                       "macro=%s wait=%.20s %.20s waited_ms=",
                                       kos_macro_kind_name(step->kind), condition,
                                       reason != NULL ? reason : "");

        if (used < sizeof macro->fail_detail) {
            snprintf(macro->fail_detail + used, sizeof macro->fail_detail - used, "%lld",
                     (long long)waited);
        }
    }
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
    macro->content_checked = 0;
    macro->focus_lost_since_ms = 0;
    macro->saw_holding = chef != NULL && chef->holding[0] != '\0';
    macro->pour_wait_since_ms = 0;
    macro->pour_recovery = 0;
    macro->pour_recovery_since_ms = 0;
    macro->board_item_before[0] = '\0';
    macro->source_item[0] = '\0';
    macro->entry_holding[0] = '\0';
    macro->clean_baseline_valid = 0;
    macro->clean_baseline = 0;
    macro->empty_hand_since_ms = 0;
    if (step->kind == KOS_MACRO_DROP && chef != NULL) {
        /* 后面 start_cook/chop 要核对锅/板上放的是不是这一份料。DROP 之后厨师
         * 手就空了，所以只能在这里记。同时记录投料前锅内已有物料数量。 */
        copy_str(macro->dropped_item, sizeof macro->dropped_item, chef->holding);
        const kos_resource *res = find_station_resource(kitchen, step, world);
        if (res != NULL) {
            macro->pot_content_count_before = resource_content_count(res);
            macro->pot_content_count_valid = 1;
        } else {
            macro->pot_content_count_before = 0;
            macro->pot_content_count_valid = 1;
        }
    }
    if (step->kind == KOS_MACRO_CHOP) {
        /* 实机切完游戏把板上物体换成 Chopped…/Diced…，换过之后那只物体上没有
         * ClientWorkableItem，进度字段读不到。记下开机时的原料名，内容改名即完成。 */
        const kos_resource *res = find_station_resource(kitchen, step, world);

        if (res != NULL && res->content[0] != '\0') {
            copy_str(macro->board_item_before, sizeof macro->board_item_before,
                     res->content);
        }
    }
    if (step->kind == KOS_MACRO_PICKUP && macro->step_index > 0 &&
        macro->plan.steps[macro->step_index - 1].kind == KOS_MACRO_CHOP) {
        /* 紧跟在切/搅之后的 pickup：板上那个物体已经改名（Chopped…/Diced…），
         * 步骤 item 是空的。记下源工位的物体名，拿到同名物品才算完成（手动局
         * manual.log:619 手=「ChoppedSushiPrawn」）。旧插件没有 item 字段时退回
         * content 的单个 token，再没有就退回"拿到任意物品"。
         * 端锅的 pickup 不走这条：源灶位上报的是锅名而不是里面的料。 */
        const kos_resource *res = find_station_resource(kitchen, step, world);

        if (res != NULL) {
            const char *label = res->item[0] != '\0' ? res->item : res->content;

            if (label[0] != '\0' && strchr(label, '+') == NULL) {
                copy_str(macro->source_item, sizeof macro->source_item, label);
            }
        }
    }
    macro->counter_baseline[0] = '\0';

    if (step->kind == KOS_MACRO_GOTO) {
        macro->deadline_ms = game_ms + goto_timeout_ms(kitchen, step, chef);
    } else if (step->kind == KOS_MACRO_PICKUP || step->kind == KOS_MACRO_DROP ||
               step->kind == KOS_MACRO_START_COOK ||
               step->kind == KOS_MACRO_DELIVER) {
        macro->deadline_ms = game_ms + MACRO_PICKUP_TIMEOUT_MS;
        if (step->kind == KOS_MACRO_PICKUP &&
            (strncmp(step->station_id, "plate_stack", 11) == 0 ||
             kos_kitchen_name_matches("Plate", step->item))) {
            macro->deadline_ms = game_ms + MACRO_PLATE_PICKUP_TIMEOUT_MS;
        }
        /* 倒完料把锅放回源灶：料已经进了玉米饼，这一步失败会让整道 pour 回队重做、
         * 空锅留在手上。失焦后要能重新走一次位（实机 20261028-071415 1.5 s 就超时）。 */
        if (step->kind == KOS_MACRO_DROP && macro->step_index > 0 &&
            macro->step_index + 1 == macro->plan.step_count && macro->plan.step_count > 3 &&
            macro->plan.steps[3].kind == KOS_MACRO_POUR) {
            macro->deadline_ms = game_ms + MACRO_GOTO_DEFAULT_TIMEOUT_MS;
        }
    } else if (step->kind == KOS_MACRO_CHOP) {
        macro->deadline_ms = game_ms + step->duration_ms + MACRO_CHOP_MARGIN_MS;
    } else if (step->kind == KOS_MACRO_POUR) {
        macro->deadline_ms = game_ms + step->duration_ms + MACRO_POUR_MARGIN_MS;
    } else if (step->kind == KOS_MACRO_WASH) {
        macro->deadline_ms = game_ms + step->duration_ms + MACRO_WASH_MARGIN_MS;
    } else {
        macro->deadline_ms = game_ms + MACRO_PICKUP_TIMEOUT_MS;
    }

    if (step->kind == KOS_MACRO_DELIVER || step->kind == KOS_MACRO_PICKUP) {
        if (chef != NULL) {
            copy_str(macro->entry_holding, sizeof macro->entry_holding, chef->holding);
        }
    }
    if (step->kind == KOS_MACRO_DELIVER) {
        /* G9：订单消失既可能是交付也可能是过期，只有游戏分数能分开。 */
        macro->score_base = world->score;
        macro->score_base_valid = 1;
    }
    if (step->kind == KOS_MACRO_WASH) {
        macro->clean_baseline_valid = clean_plate_count(world, &macro->clean_baseline);
    }
    if (step->kind == KOS_MACRO_POUR) {
        const kos_resource *counter = find_station_resource(kitchen, step, world);
        if (counter != NULL && counter->content[0] != '\0') {
            copy_str(macro->counter_baseline, sizeof macro->counter_baseline, counter->content);
        }
    }
    return 0;
}

/* 厨师到该工位最近站位格的平面距离是否在到达容差内。 */
static int near_station_stand(const kos_kitchen *kitchen, const kos_macro_step *step,
                              const kos_chef *chef)
{
    int index = kos_kitchen_station_index(kitchen, step->station_id);
    const kos_station_stands *stands;
    float best = 1e9f;
    int i;

    if (index < 0 || chef == NULL) {
        return 0;
    }
    stands = kos_stand_cells(kitchen, index);
    if (stands == NULL || stands->count <= 0) {
        /* 没有站位格数据（合成测试厨房）：无从判断，不拦。真实地图加载时
         * kos_stand_build 保证每个工位至少一格，否则加载就失败了。 */
        return 1;
    }
    for (i = 0; i < stands->count; i++) {
        int cell = stands->cells[i].cell;
        float dx = kitchen->cells[cell].x - chef->x;
        float dz = kitchen->cells[cell].z - chef->y;
        float d2 = dx * dx + dz * dz;

        if (d2 < best) {
            best = d2;
        }
    }
    return best <= MACRO_STAND_ARRIVE_TOL * MACRO_STAND_ARRIVE_TOL;
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
    /* navd 迟迟不报到站、人却已经站在目标工位的站位格上（实机 20261028-023839：
     * 还锅回 cooker-2，人在 (1.82,-7.05)，navd 一直把目标给成 (2.4,-7.2) 来回拉，
     * 6.9 s 后 arrived+face 超时）：站够一会儿就按到站处理，直接转身。 */
    if (!nav_arrived && !macro->face_sent &&
        game_ms - macro->step_start_ms >= MACRO_GOTO_NEAR_FALLBACK_MS) {
        /* 只在有站位格数据的真实地图上兜底：合成测试厨房没有站位格，
         * near_station_stand 恒为真，会把"还在走"误判成到站。 */
        const kos_station_stands *stands =
            kos_stand_cells(kitchen, kos_kitchen_station_index(kitchen, step->station_id));

        if (stands != NULL && stands->count > 0 && near_station_stand(kitchen, step, chef)) {
            nav_arrived = 1;
        }
    }
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

    {
        int near = near_station_stand(kitchen, step, chef);

        if (near && game_ms > macro->face_sent_ms &&
            station_in_focus(kitchen, step, chef)) {
            return next_step(macro, out) == 1 ? 1 : 0;
        }
        copy_str(out->target_object, sizeof out->target_object, step->station_id);
        if ((!near || !station_in_focus(kitchen, step, chef)) &&
            game_ms > macro->face_sent_ms + MACRO_FOCUS_RETRY_MS) {
            /* 离站位格太远，或转身这么久仍选不中目标：清掉 face_sent 让 worker
             * 重新出 path（navd 可能还停在 ARRIVED）。 */
            macro->face_sent = 0;
            out->action = KOS_MACRO_ACT_NAVIGATE;
        } else {
            out->action = KOS_MACRO_ACT_FACE;
        }
    }
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
    /* 空的 item = "拿到源工位上那件东西"：切完 prefab 改了名、端起锅时手上是锅。
     * 进这一步时记下源工位的物体名（macro->source_item），拿到同名物品才算完成；
     * 旧插件没有 item 字段时 source_item 为空，退回"拿到任意物品"。 */
    int holding = chef != NULL && chef->holding[0] != '\0';
    int got = 0;

    if (step->item[0] != '\0') {
        got = holding_is(chef, step->item);
    } else if (macro->source_item[0] != '\0') {
        got = holding_is(chef, macro->source_item);
    } else if (macro->entry_holding[0] == '\0') {
        got = holding;
    }
    /* 进这一步时手上已经有东西（实机 _3：放下的玉米饼被补按拿回手里），"手上有东西"
     * 不能算拿到了锅：会拿着饼去"倒米饭"，锅一直留在灶上烧糊。 */
    if (got) {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    if (holding) {
        /* 手上已经有东西：游戏不会从料箱/台面再给一件。以前照按 1.5 s 才超时，
         * 还会把手里那件放到料箱上（run 3 LogOutput:2533 的平底锅）。 */
        fail_step(macro, game_ms, "hands-not-empty", "hand-occupied");
        return -1;
    }
    /* 倒料的第 2 步（端锅）必须先等锅熟：端走生料/烧焦的锅会把它从灶上拿走，
     * 后面这口灶就没锅可用了（run 3 worker-1.log:32-38 端走了生米锅）。 */
    if (macro->step_index == 1 && macro->plan.step_count > 3 &&
        macro->plan.steps[3].kind == KOS_MACRO_POUR) {
        const kos_resource *pot = find_station_resource(kitchen, step, world);

        if (pot != NULL && (pot->has_cooked || pot->has_burning || pot->has_fire)) {
            int ready = pot->has_cooked && pot->cooked &&
                        !(pot->has_burning && pot->burning) &&
                        !(pot->has_fire && pot->fire);
            int64_t limit = pot->time_left > 0.0f
                                ? (int64_t)((double)pot->time_left * 1000.0) + 3000
                                : MACRO_POUR_WAIT_MS;

            if (!ready) {
                if (macro->pour_wait_since_ms == 0) {
                    macro->pour_wait_since_ms = game_ms;
                    kos_log_info(macro->log, "macro_pour_wait",
                                 "chef=%d station=%s cooked=%d/%d burning=%d/%d fire=%d/%d "
                                 "time_left=%.2f limit_ms=%lld",
                                 chef_id, step->station_id, pot->has_cooked, pot->cooked,
                                 pot->has_burning, pot->burning, pot->has_fire, pot->fire,
                                 (double)pot->time_left, (long long)limit);
                }
                /* 等锅熟的时间不算进 pickup 的 1.5 s 超时：上限就是 time_left + 3 s。 */
                if (macro->deadline_ms < macro->pour_wait_since_ms + limit) {
                    macro->deadline_ms = macro->pour_wait_since_ms + limit;
                }
                if (game_ms - macro->pour_wait_since_ms >= limit) {
                    fail_step(macro, game_ms, "pot-cooked", "timeout");
                    return -1;
                }
                if (!ensure_focus(macro, kitchen, world, chef_id, step, game_ms, out)) {
                    return 0;
                }
                out->action = KOS_MACRO_ACT_STOP; /* 在灶边等，不端锅 */
                return 0;
            }
        }
    }
    if (!ensure_focus(macro, kitchen, world, chef_id, step, game_ms, out)) {
        if (game_ms >= macro->deadline_ms) {
            fail_step(macro, game_ms, "holding", "focus");
            return -1;
        }
        return 0;
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

static int run_drop(kos_macro *macro, const kos_kitchen *kitchen,
                    const kos_world_state *world, int chef_id, const kos_chef *chef,
                    const kos_macro_step *step, int64_t game_ms, kos_macro_output *out)
{
    if (chef != NULL && chef->holding[0] == '\0') {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    /* 装盘重试：上一轮盘子晚到手，已经端着盘了。再按一次会把盘放回去或卡住，
     * 直接进下一步（取盘），由它确认手上就是要的东西。 */
    if (chef != NULL && macro->step_index + 1 < macro->plan.step_count) {
        const kos_macro_step *next = &macro->plan.steps[macro->step_index + 1];

        if (next->kind == KOS_MACRO_PICKUP && holding_is(chef, next->item)) {
            return next_step(macro, out) == 1 ? 1 : 0;
        }
    }
    if (!ensure_focus(macro, kitchen, world, chef_id, step, game_ms, out)) {
        if (game_ms >= macro->deadline_ms) {
            fail_step(macro, game_ms, "empty-hand", "focus");
            return -1;
        }
        return 0;
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
                    const kos_world_state *world, int chef_id, int64_t game_ms,
                    kos_macro_output *out)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    const kos_resource *resource = find_station_resource(kitchen, step, world);

    if (resource == NULL) {
        fail_step(macro, game_ms, "progress=1", "no-station-resource");
        return -1;
    }
    if (resource->content[0] != '\0') {
        macro->saw_content = 1;
        /* 放下后状态帧可能还没带上板上的料，开机时 board_item_before 就是空的，
         * 改名判据永远不成立（鸡肉切完改名 BurritoChoppedChicken、进度读不到）。
         * 第一次看到的原料内容补记为改名前的名字。 */
        if (macro->board_item_before[0] == '\0' && content_is_expected(resource, macro, step)) {
            copy_str(macro->board_item_before, sizeof macro->board_item_before,
                     resource->content);
        }
        /* G13：板上不是这一步该放的东西（放错料/别人占着砧板）就立刻判工序不可行，
         * 不要等 progress 超时后把坏菜送到装盘才暴露。只核对第一次看到的内容：
         * 切完游戏会把板上物体换成 Chopped…/Diced…，那正是本步的产物。 */
        if (!macro->content_checked) {
            if (content_is_expected(resource, macro, step)) {
                macro->content_checked = 1;
            } else if (!chop_content_renamed(macro, resource)) {
                fail_step(macro, game_ms, "progress=1", "content-mismatch");
                return -1;
            }
        }
    } else if (macro->saw_content) {
        fail_step(macro, game_ms, "progress=1", "content-gone");
        return -1;
    }
    /* 实机（20261027-123220/124810 的 _3 与手动局）切完是"物体改名"而不是
     * progress=1：换过 prefab 的物体上没有 ClientWorkableItem，progress 一直读到 0。
     * 两个判据任一成立即完成。 */
    if (resource->progress >= MACRO_PROGRESS_DONE ||
        (resource->content[0] != '\0' && chop_content_renamed(macro, resource))) {
        if (resource->content[0] == '\0') {
            /* 切完了但板上是空的：要么被拿走，要么本来就放错了，后面 pickup 只会空等。 */
            fail_step(macro, game_ms, "progress=1", "content-gone");
            return -1;
        }
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    if (!ensure_focus(macro, kitchen, world, chef_id, step, game_ms, out)) {
        if (game_ms >= macro->deadline_ms) {
            fail_step(macro, game_ms, "progress=1", "focus");
            return -1;
        }
        return 0;
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
                          const kos_world_state *world, int chef_id, int64_t game_ms,
                          kos_macro_output *out)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    const kos_resource *resource = find_station_resource(kitchen, step, world);

    if (resource == NULL) {
        fail_step(macro, game_ms, "pot=cooking", "no-station-resource");
        return -1;
    }
    if (strcmp(resource->state, "cooking") == 0 || strcmp(resource->state, "cooked") == 0) {
        /* G13：锅里已经是别的料时判 content-mismatch */
        if (resource->content[0] != '\0' || resource->content_count > 0) {
            if (!macro->content_checked) {
                macro->content_checked = 1;
                kos_log_info(macro->log, "start_cook_content",
                             "chef=%d station=%s held_before=%s pot_contents_after=%s "
                             "expect=%s count_before=%d count_now=%d",
                             chef_id, step->station_id, macro->dropped_item, resource->content,
                             macro->plan.cook_input[0] != '\0' ? macro->plan.cook_input : "-",
                             macro->pot_content_count_before,
                             resource_content_count(resource));
            }
            if (!start_cook_content_matches(resource, macro, step)) {
                fail_step(macro, game_ms, "pot=cooking", "content-mismatch");
                return -1;
            }
        }
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    if (strcmp(resource->state, "burning") == 0) {
        fail_step(macro, game_ms, "pot=cooking", "burning");
        return -1;
    }
    if (resource->content[0] != '\0' || resource->content_count > 0) {
        macro->saw_content = 1;
        if (!macro->content_checked) {
            macro->content_checked = 1;
            kos_log_info(macro->log, "start_cook_content",
                         "chef=%d station=%s held_before=%s pot_contents_after=%s "
                         "expect=%s count_before=%d count_now=%d",
                         chef_id, step->station_id, macro->dropped_item, resource->content,
                         macro->plan.cook_input[0] != '\0' ? macro->plan.cook_input : "-",
                         macro->pot_content_count_before,
                         resource_content_count(resource));
        }
        if (!start_cook_content_matches(resource, macro, step)) {
            fail_step(macro, game_ms, "pot=cooking", "content-mismatch");
            return -1;
        }
    } else if (macro->saw_content) {
        fail_step(macro, game_ms, "pot=cooking", "content-gone");
        return -1;
    }
    if (game_ms >= macro->deadline_ms) {
        fail_step(macro, game_ms, "pot=cooking", "timeout");
        return -1;
    }
    out->action = KOS_MACRO_ACT_STOP;
    out->target_object[0] = '\0';
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

/* "cooked:BurritoChicken" / "pour:BurritoChicken" → 冒号后的名字。
 * 状态里的内容带 proto 的类型前缀，工序 id 带 op 前缀，比名字前先剥掉。 */
static const char *content_name(const char *text)
{
    const char *colon;

    if (text == NULL) {
        return "";
    }
    colon = strrchr(text, ':');
    return colon == NULL ? text : colon + 1;
}

/*
 * 名字的"原料词"：剥掉 proto/工序前缀（cooked: / pour:）后取最后一个 CamelCase 词，
 * 再忽略大小写的复数 's'。实机已知的改名链：
 *   BurritoMeat    → DicedMeat              (Meat)
 *   BurritoChicken → BurritoChoppedChicken  (Chicken)
 *   Mushroom       → ChoppedMushroom        (Mushroom)
 *   SushiFish      → ChoppedSushiFish       (Fish)
 *   Rice           → BoiledRice             (Rice)
 * 五个菜名去掉修饰语后词尾互不相同（Fish/Prawn/Chicken/Meat/Rice/Mushroom/Tortilla），
 * 所以整词相等就足够；不要再退回"公共后缀"，Tomato 与 Potato 会因共享 "ato" 被判成
 * 同一味料，把别的料倒进锅里。
 */
static const char *ingredient_word(const char *name)
{
    const char *text = content_name(name);
    const char *start = text;
    const char *p;

    for (p = text; *p != '\0'; p++) {
        if (p != text && *p >= 'A' && *p <= 'Z') {
            start = p;
        }
    }
    return start;
}

static size_t ingredient_word_len(const char *word)
{
    size_t len = strlen(word);

    if (len > 0 && (word[len - 1] == 's' || word[len - 1] == 'S')) {
        len--;
    }
    return len;
}

/* 两个名字的原料词是否逐字符相等（忽略大小写，'-' 与 '_' 等价）。 */
static int ingredient_word_matches(const char *a, const char *b)
{
    const char *wa = ingredient_word(a);
    const char *wb = ingredient_word(b);
    size_t la = ingredient_word_len(wa);
    size_t lb = ingredient_word_len(wb);
    size_t i;

    if (la == 0 || la != lb) {
        return 0;
    }
    for (i = 0; i < la; i++) {
        char ca = wa[i];
        char cb = wb[i];

        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca == '_' || ca == '-') ca = '_';
        if (cb == '_' || cb == '-') cb = '_';
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

/*
 * 比较倒料目标原料与容器里的一项内容是否匹配。
 *
 * `content` 是 contents[] 的 "+" 拼串兼容形式，所以先按 '+'/',' 拆成 token 再比：
 * 绝不拿 "a+b" 整体去比。每一项比的是"原料词"（见 ingredient_word），
 * 生料 / 熟料 / 切块 prefab 名都能对上同一味料。
 */
static int poured_content_matches(const char *node_id, const char *content)
{
    char buf[KOS_SHM_CONTENT_MAX];
    char *token;
    char *saveptr = NULL;

    if (node_id == NULL || content == NULL || content[0] == '\0') {
        return 0;
    }
    if (content_name(node_id)[0] == '\0') {
        return 0;
    }
    copy_str(buf, sizeof buf, content);
    token = strtok_r(buf, "+,", &saveptr);
    while (token != NULL) {
        if (ingredient_word_matches(node_id, token)) {
            return 1;
        }
        token = strtok_r(NULL, "+,", &saveptr);
    }
    return 0;
}

static int counter_has_content(const char *counter_content, const char *item)
{
    if (counter_content == NULL || counter_content[0] == '\0' ||
        item == NULL || item[0] == '\0') {
        return 0;
    }
    return poured_content_matches(item, counter_content);
}

/*
 * 台面上/容器里是否已经有这一味料。`contents[]` 是递归叶子（烹饪节点写成
 * cooked:<name>），所以用双向子串匹配：pour:BurritoChicken 命中
 * cooked:BurritoChoppedChicken；`content` 是同一份内容的兼容拼串。
 */
static int resource_has_item(const kos_resource *resource, const char *item)
{
    int i;

    if (resource == NULL || item == NULL || item[0] == '\0') {
        return 0;
    }
    if (poured_content_matches(item, resource->content)) {
        return 1;
    }
    for (i = 0; i < (int)resource->content_count && i < KOS_SHM_MAX_CONTENTS; i++) {
        if (poured_content_matches(item, resource->contents[i])) {
            return 1;
        }
    }
    return 0;
}

/*
 * 倒料：手上端着锅/容器，对着台面容器按 interact，直到锅里的料进到台面上。
 *
 * 完成判据只看内容，不看编号（锅跟着人走；G1 起 pot_N 是灶台槽位，锅在手上时
 * 那个槽位是 empty，锅本身以 carried_<chefId> 出现）：
 *   1. `carried_<chefId>` 的内容从非空变空 = 倒完了。这是权威信号：端起来的时候
 *      锅里的料就在 carried_ 上，倒完同一项变空。
 *   2. 资源块是 0.25 s 重建的，端起→倒完整段可能落在两个资源帧之间，carried_ 一次
 *      都没出现过。这时看**目的台面**：玉米饼里多出这一味料（contents[] 命中
 *      step->item）就是完成——它比"锅空了"更直接，也不会被同一张台面上别的锅骗到。
 * 源槽位在锅被端走后没有内容可看；旧状态没有 carried_ 时退回扫描 pot_* 的内容与
 * 目的台面的新增内容。两条来源都没有证据时**不判完成**：800 ms 的兜底交互只覆盖
 * 按键往返，倒没倒过去它并不知道，早年那条"时间到了就算倒完"会把没倒成的料当成功，
 * 后面装盘/交付全部连锁失败。没有证据就等到步超时，失败原因是 source-empty。
 */
/*
 * 倒料失败但手里还端着锅：先回源灶台放下再收尾。端着锅直接失败会把锅留在手上/
 * 台面上，那口灶后面就没有锅可用了（run 3 的 cooker-4 就是这样废掉的）。
 * 返回 1 = 已进入回收（调用方本帧返回 0），0 = 已失败（调用方返回 -1）。
 */
static int pour_fail(kos_macro *macro, const kos_chef *chef, int64_t game_ms,
                     const char *condition, const char *reason)
{
    if (chef != NULL && chef->holding[0] != '\0' && !macro->pour_recovery) {
        macro->pour_recovery = 1;
        macro->pour_recovery_since_ms = game_ms;
        macro->focus_lost_since_ms = 0;
        copy_str(macro->pour_fail_wait, sizeof macro->pour_fail_wait, condition);
        copy_str(macro->pour_fail_reason, sizeof macro->pour_fail_reason, reason);
        return 1;
    }
    fail_step(macro, game_ms, condition, reason);
    return 0;
}

/* 回收：回到源灶台（plan 的第 1 步）把锅放下，再按原失败原因收尾。 */
static int run_pour_recovery(kos_macro *macro, const kos_kitchen *kitchen,
                             const kos_world_state *world, int chef_id, int64_t game_ms,
                             kos_macro_output *out)
{
    const kos_chef *chef = find_chef(world, chef_id);
    const kos_macro_step *home = &macro->plan.steps[0];

    if (chef == NULL || chef->holding[0] == '\0' ||
        game_ms - macro->pour_recovery_since_ms > MACRO_POUR_RECOVER_MS) {
        fail_step(macro, game_ms, macro->pour_fail_wait, macro->pour_fail_reason);
        return -1;
    }
    if (!ensure_focus(macro, kitchen, world, chef_id, home, game_ms, out)) {
        return 0;
    }
    out->action = KOS_MACRO_ACT_INTERACT;
    copy_str(out->target_object, sizeof out->target_object, home->station_id);
    return 0;
}

static int run_pour(kos_macro *macro, const kos_kitchen *kitchen, const kos_world_state *world,
                    int chef_id, int64_t game_ms, kos_macro_output *out)
{
    const kos_macro_step *step = &macro->plan.steps[macro->step_index];
    const kos_chef *chef = find_chef(world, chef_id);
    const kos_resource *carried;
    const kos_resource *counter;
    int done = 0;

    if (macro->pour_recovery) {
        return run_pour_recovery(macro, kitchen, world, chef_id, game_ms, out);
    }
    carried = find_carried_resource(world, chef_id);
    counter = find_station_resource(kitchen, step, world);

    if (carried != NULL) {
        if (carried->content[0] != '\0' || carried->content_count > 0) {
            if (!macro->saw_content) {
                kos_log_info(macro->log, "macro_pour_source",
                             "chef=%d item=%s source=%s carried_content=%s",
                             chef_id, step->item, macro->plan.steps[0].station_id, carried->content);
            }
            /* 手里这口锅装的必须是这道工序要的料：派错锅/没熟就倒会把别的东西
             * 倒进玉米饼，而且倒完内容变空，只按"变空"判完成会当成成功。
             * 用 contents[] 的 token 认（content 只有 32 字节，两项内容会被截断），
             * content 只是兼容拼串。 */
            if (!resource_has_item(carried, step->item)) {
                if (pour_fail(macro, chef, game_ms, "content", "wrong-item")) {
                    out->action = KOS_MACRO_ACT_STOP;
                    return 0;
                }
                return -1;
            }
            macro->saw_content = 1;
        } else if (macro->saw_content) {
            done = 1;
        } else if (resource_has_item(counter, step->item)) {
            done = 1;
        }
    } else if (resource_has_item(counter, step->item)) {
        done = 1;
    } else {
        int found = 0;
        int i;

        for (i = 0; i < (int)world->resource_count; i++) {
            const kos_resource *resource = &world->resources[i];

            if (strncmp(resource->name, "pot_", 4) != 0 || resource->content[0] == '\0') {
                continue;
            }
            if (poured_content_matches(step->item, resource->content)) {
                if (!macro->saw_content) {
                    kos_log_info(macro->log, "macro_pour_source",
                                 "chef=%d item=%s source=%s pot=%s pot_content=%s",
                                 chef_id, step->item, macro->plan.steps[0].station_id,
                                 resource->name, resource->content);
                }
                found = 1;
                break;
            }
        }

        if (found) {
            macro->saw_content = 1;
        } else if (macro->saw_content) {
            done = 1;
        }
        /* 台面容器信号：在没有 carried_ 的旧状态里，台面内容新增了当前工序原料可判完成。
         * 注意：台面已有内容绝不能置 found=1（found 专指源锅仍有料）；
         * 否则同一张台面连续两次倒料（如卷饼先倒肉再倒米）时，台面在第二步开始就非空，
         * 会导致 found 恒为 1，永远无法判完成，并禁用兜底超时。 */
        if (!done && counter != NULL && counter->content[0] != '\0') {
            int baseline_had = counter_has_content(macro->counter_baseline, step->item);
            int now_has = counter_has_content(counter->content, step->item);
            if (now_has && !baseline_had) {
                done = 1;
            }
        }
    }
    if (done) {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    if (!ensure_focus(macro, kitchen, world, chef_id, step, game_ms, out)) {
        if (game_ms >= macro->deadline_ms) {
            if (pour_fail(macro, chef, game_ms, "source-empty", "focus")) {
                out->action = KOS_MACRO_ACT_STOP;
                return 0;
            }
            return -1;
        }
        return 0;
    }
    if (game_ms >= macro->deadline_ms) {
        if (pour_fail(macro, chef, game_ms, "source-empty", "timeout")) {
            out->action = KOS_MACRO_ACT_STOP;
            return 0;
        }
        return -1;
    }
    out->action = KOS_MACRO_ACT_INTERACT;
    copy_str(out->target_object, sizeof out->target_object, step->station_id);
    return 0;
}

static int run_wash(kos_macro *macro, const kos_kitchen *kitchen,
                    const kos_world_state *world, int chef_id, int64_t game_ms,
                    kos_macro_output *out)
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
    if (resource->progress < MACRO_PROGRESS_DONE) {
        macro->saw_content = 1;
    }
    if (resource->progress >= MACRO_PROGRESS_DONE &&
        (macro->saw_content ||
         (!macro->clean_baseline_valid || (have_clean && clean > macro->clean_baseline)))) {
        return next_step(macro, out) == 1 ? 1 : 0;
    }
    if (!ensure_focus(macro, kitchen, world, chef_id, step, game_ms, out)) {
        if (game_ms >= macro->deadline_ms) {
            fail_step(macro, game_ms, "progress=1+clean", "focus");
            return -1;
        }
        return 0;
    }
    if (game_ms >= macro->deadline_ms) {
        fail_step(macro, game_ms, "progress=1+clean", "timeout");
        return -1;
    }
    out->action = KOS_MACRO_ACT_WORK;
    copy_str(out->target_object, sizeof out->target_object, step->station_id);
    return 0;
}

static int run_deliver(kos_macro *macro, const kos_kitchen *kitchen,
                       const kos_world_state *world, int chef_id, const kos_chef *chef,
                       const kos_macro_step *step, int64_t game_ms, kos_macro_output *out)
{
    /* G9：游戏自己的订单结果比"从 m_activeOrders 消失"更早也更准。交付瞬间盘子
     * 就消失、手先空，订单却可能晚一两帧才移除；只按手空判失败会把成功交付记成
     * holding-lost。delivered 直接算完成，expired/failed 直接失败。 */
    const kos_order_result *result = order_result_for(world, macro->plan.order_id);

    if (result != NULL) {
        if (strcmp(result->outcome, "delivered") == 0) {
            return next_step(macro, out) == 1 ? 1 : 0;
        }
        if (strcmp(result->outcome, "expired") == 0) {
            fail_step(macro, game_ms, "order-gone", "expired");
            return -1;
        }
        if (strcmp(result->outcome, "failed") == 0) {
            fail_step(macro, game_ms, "order-gone", "delivery-failed");
            return -1;
        }
    }
    if (!order_present(world, macro->plan.order_id)) {
        /* 订单消失有两种：交付成功（分数上涨）或到期（分数下降/不变）。以前只看
         * "订单不在了"，过期也被当成交付，W11 实机把扣分记成过完成（G9）。
         * 现在成功必须有证据：游戏结果 delivered 或分数上涨。分数没动（可能是
         * 结果帧还没到）就继续等，等到步超时仍无证据才失败——"订单不见了"本身
         * 不是交付证据。 */
        if (macro->score_base_valid) {
            int64_t delta = world->score - macro->score_base;

            if (delta > 0) {
                return next_step(macro, out) == 1 ? 1 : 0;
            }
            if (delta < 0) {
                fail_step(macro, game_ms, "order-gone", "expired");
                return -1;
            }
        } else {
            /* 老插件/模拟器没报分数：没有可比对的基准，保留原来的完成路径。 */
            return next_step(macro, out) == 1 ? 1 : 0;
        }
        if (game_ms >= macro->deadline_ms) {
            fail_step(macro, game_ms, "order-gone", "no-evidence");
            return -1;
        }
        out->action = KOS_MACRO_ACT_STOP;
        out->target_object[0] = '\0';
        return 0;
    }
    /* 手上本来拿着菜，等订单消失时菜先没了：可能是交付成功（盘子先消失、结果晚到），
     * 也可能是菜掉了。先等 grace，结果/订单消失会先返回；一直没有才判 holding-lost。 */
    if (chef != NULL && macro->entry_holding[0] != '\0' && chef->holding[0] == '\0') {
        if (macro->empty_hand_since_ms == 0) {
            macro->empty_hand_since_ms = game_ms;
        }
        if (game_ms - macro->empty_hand_since_ms >= MACRO_DELIVER_LOST_GRACE_MS) {
            fail_step(macro, game_ms, "order-gone", "holding-lost");
            return -1;
        }
    } else {
        macro->empty_hand_since_ms = 0;
    }
    if (!ensure_focus(macro, kitchen, world, chef_id, step, game_ms, out)) {
        if (game_ms >= macro->deadline_ms) {
            fail_step(macro, game_ms, "order-gone", "focus");
            return -1;
        }
        return 0;
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
    /* 倒料续做：手上已经端着装有这味料的锅（上次走到一半被 navd 判卡住），
     * 跳过"去源灶 + 端锅"，直接去容器那一步。 */
    if (macro->step_index < 2 && macro->plan.step_count > 3 &&
        macro->plan.steps[1].kind == KOS_MACRO_PICKUP &&
        macro->plan.steps[3].kind == KOS_MACRO_POUR) {
        const kos_resource *carried = find_carried_resource(world, chef_id);
        const kos_chef *holder = find_chef(world, chef_id);
        /* 只认锅/平底锅：手上的玉米饼里也有 cooked:Rice（实机 20261028-022924）。 */
        int holding_pot = holder != NULL && (kos_kitchen_name_matches("pot", holder->holding) ||
                                             kos_kitchen_name_matches("pan", holder->holding));

        if (holding_pot && carried != NULL &&
            resource_has_item(carried, macro->plan.steps[3].item)) {
            macro->step_index = 2;
            macro->step_started = 0;
        }
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
        return run_drop(macro, kitchen, world, chef_id, chef, step, game_ms, out);
    case KOS_MACRO_CHOP:
        return run_chop(macro, kitchen, world, chef_id, game_ms, out);
    case KOS_MACRO_START_COOK:
        return run_start_cook(macro, kitchen, world, chef_id, game_ms, out);
    case KOS_MACRO_POUR:
        return run_pour(macro, kitchen, world, chef_id, game_ms, out);
    case KOS_MACRO_WASH:
        return run_wash(macro, kitchen, world, chef_id, game_ms, out);
    case KOS_MACRO_DELIVER:
        return run_deliver(macro, kitchen, world, chef_id, chef, step, game_ms, out);
    default:
        fail_step(macro, game_ms, "known-macro", "unknown-kind");
        return -1;
    }
}
