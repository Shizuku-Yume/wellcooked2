/*
 * test_macro.c -- 宏动作展开与按世界状态判完成。
 *
 * 用 config/kitchen.example.json 的真实工位 id 与一份合成世界状态，覆盖六种工序的
 * 展开、八种宏动作的完成/超时/完成条件被打破。整套检查不碰共享内存与进程，几微秒
 * 就能把"等 1.5 s"跑完。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kitchenos/child.h"
#include "kitchenos/kitchen.h"
#include "kitchenos/macro.h"
#include "kitchenos/stand.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

static void expect(int cond, const char *what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        g_failures++;
        printf("  FAIL %s\n", what);
    }
}

static int load_kitchen(kos_kitchen *kitchen)
{
    char path[KOS_DAEMON_PATH_MAX];
    kos_error err;

    if (kos_child_default_config_path("kitchen.example.json", path, sizeof path) != 0) {
        return -1;
    }
    memset(kitchen, 0, sizeof *kitchen);
    kos_error_clear(&err);
    return kos_kitchen_load(path, kitchen, &err) == KOS_OK ? 0 : -1;
}

static const char *station_of_kind(const kos_kitchen *kitchen, const char *kind)
{
    int i;

    for (i = 0; i < kitchen->station_count; i++) {
        if (strcmp(kitchen->stations[i].kind, kind) == 0) {
            return kitchen->stations[i].id;
        }
    }
    return NULL;
}

static kos_resource *add_resource(kos_world_state *world, const char *name)
{
    kos_resource *resource;

    if (world->resource_count >= KOS_SHM_MAX_RESOURCES) {
        return NULL;
    }
    resource = &world->resources[world->resource_count++];
    memset(resource, 0, sizeof *resource);
    snprintf(resource->name, sizeof resource->name, "%s", name);
    resource->chef_id = -1;
    return resource;
}

static kos_order *add_order(kos_world_state *world, const char *id)
{
    kos_order *order;

    if (world->order_count >= KOS_SHM_MAX_ORDERS) {
        return NULL;
    }
    order = &world->orders[world->order_count++];
    memset(order, 0, sizeof *order);
    snprintf(order->id, sizeof order->id, "%s", id);
    return order;
}

static kos_chef *chef_at_station(kos_world_state *world, const kos_kitchen *kitchen,
                                 const char *station_id)
{
    int station_index = kos_kitchen_station_index(kitchen, station_id);
    const kos_station_stands *stands;
    kos_chef *chef;

    if (station_index < 0) {
        return NULL;
    }
    stands = kos_stand_cells(kitchen, station_index);
    if (stands == NULL || stands->count <= 0) {
        return NULL;
    }
    world->chef_count = 1;
    chef = &world->chefs[0];
    memset(chef, 0, sizeof *chef);
    chef->id = 0;
    chef->x = kitchen->cells[stands->cells[0].cell].x;
    chef->y = kitchen->cells[stands->cells[0].cell].z;
    chef->facing = atan2f(kitchen->stations[station_index].x - chef->x,
                         kitchen->stations[station_index].z - chef->y);
    return chef;
}

/* 测试只按 chef_id=0 跑；find_chef 在 macro.c 里是 static，这里自己找一遍。 */
static kos_chef *chef_find(kos_world_state *world, int chef_id)
{
    int i;

    for (i = 0; i < (int)world->chef_count; i++) {
        if ((int)world->chefs[i].id == chef_id) {
            return &world->chefs[i];
        }
    }
    return NULL;
}

static kos_resource *resource_by_name(kos_world_state *world, const char *name)
{
    int i;

    for (i = 0; i < (int)world->resource_count; i++) {
        if (strcmp(world->resources[i].name, name) == 0) {
            return &world->resources[i];
        }
    }
    return NULL;
}

static void base_world(kos_world_state *world)
{
    kos_world_state_reset(world);
    world->game_time_ms = 0;
}

static int build(kos_macro *macro, const kos_kitchen *kitchen, const char *op,
                 const char *node_id, const char *station_id, const char *order_id,
                 int duration_ms)
{
    kos_macro_plan plan;
    kos_error err;

    kos_error_clear(&err);
    if (kos_macro_plan_build(kitchen, op, node_id, station_id, order_id, duration_ms,
                             &plan, &err) != KOS_OK) {
        return -1;
    }
    kos_macro_begin(macro, &plan);
    return 0;
}

/* 复制一个已有工位的坐标与站位格，给测试补一个 exporter 现在才会产出的工位类型
 * （config/kitchen.example.json 是旧导出，没有 board）。 */
static int add_station_clone(kos_kitchen *kitchen, const char *source_id,
                             const char *new_id, const char *new_kind)
{
    int source = kos_kitchen_station_index(kitchen, source_id);
    int index = kitchen->station_count;

    if (source < 0 || index >= KOS_KITCHEN_MAX_STATIONS) {
        return -1;
    }
    kitchen->stations[index] = kitchen->stations[source];
    snprintf(kitchen->stations[index].id, sizeof kitchen->stations[index].id, "%s", new_id);
    snprintf(kitchen->stations[index].kind, sizeof kitchen->stations[index].kind, "%s",
             new_kind);
    kitchen->stand.station[index] = kitchen->stand.station[source];
    kitchen->station_count++;
    return index;
}

/* 最小厨房：一格地、一个工位。只给"工位 → 世界状态资源键"的映射测试用。 */
static void one_station_kitchen(kos_kitchen *kitchen, const char *id, const char *kind)
{
    memset(kitchen, 0, sizeof *kitchen);
    snprintf(kitchen->scene, sizeof kitchen->scene, "%s", "synthetic");
    kitchen->step = 0.6f;
    kitchen->cell_count = 1;
    kitchen->cells[0].x = 0.0f;
    kitchen->cells[0].z = 0.0f;
    kitchen->cells[0].mask = 0;
    kitchen->station_count = 1;
    snprintf(kitchen->stations[0].id, sizeof kitchen->stations[0].id, "%s", id);
    snprintf(kitchen->stations[0].kind, sizeof kitchen->stations[0].kind, "%s", kind);
    kitchen->stations[0].x = 0.0f;
    kitchen->stations[0].z = 0.0f;
    kitchen->stand.station[0].count = 0;
}

/* 厨师站在工位上（朝向恰好对准）：goto 直接发 face，第二次 advance 就完成。 */
static kos_chef *place_chef_on_station(kos_world_state *world, const char *holding)
{
    kos_chef *chef;

    world->chef_count = 1;
    chef = &world->chefs[0];
    memset(chef, 0, sizeof *chef);
    chef->id = 0;
    chef->x = 0.0f;
    chef->y = 0.0f;
    chef->facing = 0.0f;
    if (holding != NULL) {
        snprintf(chef->holding, sizeof chef->holding, "%s", holding);
    }
    return chef;
}


static int finish_goto(kos_macro *macro, const kos_kitchen *kitchen,
                       const kos_world_state *world, int64_t *game_ms,
                       kos_macro_output *out)
{
    int rc;

    rc = kos_macro_advance(macro, kitchen, world, 0, 1, *game_ms, out);
    expect(rc == 0 && out->action == KOS_MACRO_ACT_FACE && macro->step_index == 0,
           "goto 到站后先发 face 且还不算完成");
    *game_ms += 16;
    rc = kos_macro_advance(macro, kitchen, world, 0, 1, *game_ms, out);
    return rc;
}

static void test_expansions(const kos_kitchen *kitchen)
{
    const char *dispenser = station_of_kind(kitchen, "dispenser");
    const char *board = station_of_kind(kitchen, "board");
    const char *container = station_of_kind(kitchen, "container");
    const char *cooker = station_of_kind(kitchen, "cooker");
    const char *plate = station_of_kind(kitchen, "plate_stack");
    const char *serving = station_of_kind(kitchen, "serving");
    const char *sink = station_of_kind(kitchen, "sink");
    kos_macro_plan plan;
    kos_error err;

    printf("test_expansions\n");
    expect(dispenser != NULL && board != NULL && container != NULL && cooker != NULL &&
               plate != NULL && serving != NULL && sink != NULL,
           "测试厨房七类工位齐全（board 由测试补进旧导出）");

    kos_error_clear(&err);
    expect(kos_macro_plan_build(kitchen, "fetch", "fetch:Carrot", dispenser, "o1", 1000,
                                &plan, &err) == KOS_OK &&
               plan.step_count == 2 && plan.steps[0].kind == KOS_MACRO_GOTO &&
               plan.steps[1].kind == KOS_MACRO_PICKUP &&
               strcmp(plan.steps[1].item, "Carrot") == 0,
           "fetch = goto + pickup(Carrot)");
    expect(kos_macro_plan_build(kitchen, "cut", "cut:ChoppedCarrot", board, "o1",
                                1000, &plan, &err) == KOS_OK &&
               plan.step_count == 4 && plan.steps[1].kind == KOS_MACRO_DROP &&
               plan.steps[2].kind == KOS_MACRO_CHOP && plan.steps[3].kind == KOS_MACRO_PICKUP,
           "cut = goto + drop + chop + pickup");
    expect(kos_macro_plan_build(kitchen, "cook", "cook:CookedCarrot", cooker, "o1", 1000,
                                &plan, &err) == KOS_OK &&
               plan.step_count == 3 && plan.steps[1].kind == KOS_MACRO_DROP &&
               plan.steps[2].kind == KOS_MACRO_START_COOK,
           "cook = goto + drop + start_cook");
    expect(kos_macro_plan_build_with_source(kitchen, "plate", "plate:Dish", plate, cooker,
                                            "o1", 1000, &plan, &err) == KOS_OK &&
               plan.step_count == 3 && plan.steps[0].kind == KOS_MACRO_GOTO &&
               plan.steps[1].kind == KOS_MACRO_DROP &&
               plan.steps[2].kind == KOS_MACRO_PICKUP &&
               strcmp(plan.steps[2].item, "Plate") == 0,
           "plate = drop held dish on the stack + take the plate");
    expect(kos_macro_plan_build(kitchen, "pickup", "pickup:Chicken_Burrito", container, "o2",
                                1000, &plan, &err) == KOS_OK &&
               plan.step_count == 2 && plan.steps[1].kind == KOS_MACRO_PICKUP &&
               plan.steps[1].item[0] == '\0',
           "pickup = goto + pickup(任意物品)");
    expect(kos_macro_plan_build(kitchen, "place", "place:Tortilla", container, "o2", 1000,
                                &plan, &err) == KOS_OK &&
               plan.step_count == 2 && plan.steps[1].kind == KOS_MACRO_DROP,
           "place = goto + drop（玉米饼上台面）");
    expect(kos_macro_plan_build_with_source(kitchen, "pour", "pour:BurritoChicken", container,
                                            cooker, "o1", 100, &plan, &err) == KOS_OK &&
               plan.step_count == 6 && plan.steps[0].kind == KOS_MACRO_GOTO &&
               strcmp(plan.steps[0].station_id, cooker) == 0 &&
               plan.steps[1].kind == KOS_MACRO_PICKUP && plan.steps[1].item[0] == '\0' &&
               plan.steps[2].kind == KOS_MACRO_GOTO &&
               strcmp(plan.steps[2].station_id, container) == 0 &&
               plan.steps[3].kind == KOS_MACRO_POUR &&
               strcmp(plan.steps[3].item, "BurritoChicken") == 0 &&
               plan.steps[4].kind == KOS_MACRO_GOTO &&
               plan.steps[5].kind == KOS_MACRO_DROP,
           "pour = 端锅 + 到容器倒 + 锅放回灶");
    expect(kos_macro_plan_build_with_source(kitchen, "pour", "pour:Rice", container, "",
                                            "o1", 100, &plan, &err) == KOS_ERR_INVAL,
           "pour 缺源灶被拒绝");
    expect(kos_macro_plan_build(kitchen, "deliver", "deliver:Dish", serving, "o1", 1000,
                                &plan, &err) == KOS_OK &&
               plan.step_count == 2 && plan.steps[1].kind == KOS_MACRO_DELIVER,
           "deliver = goto + deliver");
    expect(kos_macro_plan_build(kitchen, "wash", "wash:CleanPlate", sink, "o1", 1000,
                                &plan, &err) == KOS_OK &&
               plan.step_count == 3 && plan.steps[1].kind == KOS_MACRO_DROP &&
               plan.steps[2].kind == KOS_MACRO_WASH,
           "wash = goto + drop + wash");
    expect(kos_macro_plan_build(kitchen, "mix", "mix:Mixed", container, "o1", 1000, &plan,
                                &err) == KOS_OK &&
               plan.step_count == 4 && plan.steps[2].kind == KOS_MACRO_CHOP,
           "mix = goto + drop + chop + pickup");
    expect(kos_macro_plan_build(kitchen, "assemble", "assemble:Dish", container, "o1",
                                1000, &plan, &err) == KOS_OK &&
               plan.step_count == 2 && plan.steps[1].kind == KOS_MACRO_DROP,
           "assemble = goto + drop");
    expect(kos_macro_plan_build(kitchen, "bogus", "bogus:X", container, "o1", 1000, &plan,
                                &err) == KOS_ERR_INVAL,
           "未知工序被拒绝");
    expect(kos_macro_plan_build(kitchen, "fetch", "fetch", container, "o1", 1000, &plan,
                                &err) == KOS_ERR_INVAL,
           "fetch 缺目标物品名被拒绝");
}

static void test_fetch(const kos_kitchen *kitchen, const char *dispenser)
{
    kos_world_state world;
    kos_macro macro;
    kos_macro_output out;
    kos_chef *chef;
    int64_t game_ms = 1000;
    int rc;

    printf("test_fetch\n");
    base_world(&world);
    chef = chef_at_station(&world, kitchen, dispenser);
    expect(chef != NULL, "fetch 世界状态有厨师");
    expect(build(&macro, kitchen, "fetch", "fetch:Carrot", dispenser, "o1", 1000) == 0,
           "fetch 计划建成");

    rc = finish_goto(&macro, kitchen, &world, &game_ms, &out);
    expect(rc == 0 && macro.step_index == 1, "fetch goto 完成进入 pickup");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    expect(rc == 0 && out.action == KOS_MACRO_ACT_INTERACT &&
               strcmp(out.target_object, dispenser) == 0,
           "fetch pickup 发 interact 目标工位");
    snprintf(chef->holding, sizeof chef->holding, "Carrot");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    expect(rc == 1, "holding 变成 Carrot 后整道 fetch 完成");
}

static void test_cut(const kos_kitchen *kitchen, const char *board_id)
{
    kos_world_state world;
    kos_macro macro;
    kos_macro_output out;
    kos_chef *chef;
    kos_resource *board;
    int64_t game_ms = 1000;
    int rc;

    printf("test_cut\n");
    base_world(&world);
    chef = chef_at_station(&world, kitchen, board_id);
    snprintf(chef->holding, sizeof chef->holding, "Carrot");
    board = add_resource(&world, "cutting_board_0");
    board->content[0] = '\0';
    expect(build(&macro, kitchen, "cut", "cut:ChoppedCarrot", board_id, "o1", 1000) == 0,
           "cut 计划建成");
    rc = finish_goto(&macro, kitchen, &world, &game_ms, &out);
    expect(rc == 0 && macro.step_index == 1, "cut goto 完成进入 drop");

    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(out.action == KOS_MACRO_ACT_INTERACT, "cut drop 发 interact");
    chef->holding[0] = '\0';
    snprintf(board->content, sizeof board->content, "Carrot");
    board->progress = 0.25f;
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2 && out.action == KOS_MACRO_ACT_STOP,
           "手上变空后 drop 完成进入 chop");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2 && out.action == KOS_MACRO_ACT_WORK,
           "chop 按住 work");
    board->progress = 0.6f;
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2 && out.action == KOS_MACRO_ACT_WORK,
           "progress 未到 1 时继续按住 work");
    board->progress = 1.0f;
    /* 实机切完会把板上物体换成 Chopped* prefab；第一次核对过后不能再按原料名判错。 */
    snprintf(board->content, sizeof board->content, "ChoppedCarrot");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3, "progress=1 且物体改名后进入 pickup");
    snprintf(chef->holding, sizeof chef->holding, "ChoppedCarrot");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 1, "拿到切好的料后 cut 完成");

    /* 完成条件被打破：砧板上的料被别人拿走，应在超时前立即失败。 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, board_id);
    chef->holding[0] = '\0';
    board = add_resource(&world, "cutting_board_0");
    snprintf(board->content, sizeof board->content, "Carrot");
    board->progress = 0.2f;
    expect(build(&macro, kitchen, "cut", "cut:ChoppedCarrot", board_id, "o1", 1000) == 0,
           "broken 场景计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* drop 完成 */
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* chop 开始 */
    board->content[0] = '\0';
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "content-gone") != NULL &&
               strstr(kos_macro_fail_detail(&macro), "macro=chop") != NULL,
           "砧板内容消失立即失败（不等超时）");

    /* G13：板上放的是别的料（放错板/别人占着）→ 立即判工序不可行，不等 progress 超时。 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, board_id);
    chef->holding[0] = '\0';
    board = add_resource(&world, "cutting_board_0");
    snprintf(board->content, sizeof board->content, "Onion");
    board->progress = 0.2f;
    expect(build(&macro, kitchen, "cut", "cut:ChoppedCarrot", board_id, "o1", 1000) == 0,
           "content-mismatch 场景计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* drop 完成 */
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* chop 开始 */
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "content-mismatch") != NULL &&
               strstr(kos_macro_fail_detail(&macro), "macro=chop") != NULL,
           "砧板放了别的料立即判 content-mismatch");
}

static void test_cook(const kos_kitchen *kitchen, const char *cooker)
{
    kos_world_state world;
    kos_macro macro;
    kos_macro_output out;
    kos_chef *chef;
    kos_resource *pot;
    int64_t game_ms = 1000;
    int rc;

    printf("test_cook\n");
    base_world(&world);
    chef = chef_at_station(&world, kitchen, cooker);
    snprintf(chef->holding, sizeof chef->holding, "ChoppedCarrot");
    pot = add_resource(&world, "pot_0");
    snprintf(pot->state, sizeof pot->state, "free");
    expect(build(&macro, kitchen, "cook", "cook:CookedCarrot", cooker, "o1", 1000) == 0,
           "cook 计划建成");
    rc = finish_goto(&macro, kitchen, &world, &game_ms, &out);
    expect(rc == 0 && macro.step_index == 1, "cook goto 完成进入 drop");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(out.action == KOS_MACRO_ACT_INTERACT, "cook drop 发 interact");
    chef->holding[0] = '\0';
    snprintf(pot->content, sizeof pot->content, "ChoppedCarrot");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2 && out.action == KOS_MACRO_ACT_STOP,
           "放下后 drop 完成进入 start_cook");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2 && out.action == KOS_MACRO_ACT_STOP,
           "start_cook 不发 interact（空手等 cooking）");
    snprintf(pot->state, sizeof pot->state, "cooking");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 1 && strcmp(pot->state, "cooking") == 0,
           "锅进入 cooking 后 cook 任务立即完成（不等 cooked）");

    /* start_cook 等待 cooking 超时测试：必须绝不发 interact，超时报错明确 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, cooker);
    snprintf(chef->holding, sizeof chef->holding, "ChoppedCarrot");
    pot = add_resource(&world, "pot_0");
    snprintf(pot->state, sizeof pot->state, "free");
    expect(build(&macro, kitchen, "cook", "cook:CookedCarrot", cooker, "o1", 1000) == 0,
           "cook timeout 计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* drop */
    chef->holding[0] = '\0';
    snprintf(pot->content, sizeof pot->content, "ChoppedCarrot");
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* drop 完成 */
    /* 进入 start_cook，直到超时都不应输出 interact */
    int saw_interact = 0;
    for (int t = 0; t < 100; t++) {
        game_ms += 16;
        rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
        if (out.action == KOS_MACRO_ACT_INTERACT) {
            saw_interact = 1;
        }
        if (rc != 0) {
            break;
        }
    }
    expect(saw_interact == 0, "start_cook 等待期间绝不发 interact");
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "wait=pot=cooking") != NULL &&
               strstr(kos_macro_fail_detail(&macro), "timeout") != NULL,
           "start_cook 超时以 pot=cooking timeout 明确报错");
    /* G13：锅里是别的料——开火会被游戏接受，但做的不是本单的菜。 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, cooker);
    snprintf(chef->holding, sizeof chef->holding, "ChoppedCarrot");
    pot = add_resource(&world, "pot_0");
    snprintf(pot->state, sizeof pot->state, "free");
    snprintf(pot->content, sizeof pot->content, "Onion");
    expect(build(&macro, kitchen, "cook", "cook:CookedCarrot", cooker, "o1", 1000) == 0,
           "cook content-mismatch 计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* drop */
    chef->holding[0] = '\0';
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* drop 完成 */
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* start_cook */
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "content-mismatch") != NULL &&
               strstr(kos_macro_fail_detail(&macro), "macro=start_cook") != NULL,
           "锅里是别的料立即判 content-mismatch");

    /* 实机/模拟器切配与菜谱名差异测试：厨师拿 DicedMeat，锅里上报 BurritoMeat */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, cooker);
    snprintf(chef->holding, sizeof chef->holding, "DicedMeat");
    pot = add_resource(&world, "pot_0");
    snprintf(pot->state, sizeof pot->state, "free");
    expect(build(&macro, kitchen, "cook", "cook:FriedBurritoMeat", cooker, "o1", 1000) == 0,
           "cook FriedBurritoMeat 计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* drop */
    chef->holding[0] = '\0';
    snprintf(pot->content, sizeof pot->content, "BurritoMeat");
    pot->content_count = 1;
    snprintf(pot->contents[0], sizeof pot->contents[0], "BurritoMeat");
    snprintf(pot->state, sizeof pot->state, "cooking");
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* drop 完成 */
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* start_cook */
    expect(rc == 1, "start_cook 识别 BurritoMeat 成功完成（不报 content-mismatch）");
}

static void test_plate(const kos_kitchen *kitchen, const char *cooker, const char *plate_stack)
{
    kos_world_state world;
    kos_macro_plan plan;
    kos_macro macro;
    kos_macro_output out;
    kos_error err;
    kos_chef *chef;
    int64_t game_ms = 1000;
    int rc;

    printf("test_plate\n");
    base_world(&world);
    chef = chef_at_station(&world, kitchen, plate_stack);
    snprintf(chef->holding, sizeof chef->holding, "Tortilla+Fillings");
    (void)add_resource(&world, plate_stack);
    kos_error_clear(&err);
    /* source 指向灶台只是旧调用方还在传；plate 不再用它取料。 */
    expect(kos_macro_plan_build_with_source(kitchen, "plate", "plate:Dish", plate_stack,
                                            cooker, "o1", 100, &plan, &err) == KOS_OK,
           "plate 计划 = 放菜 + 取盘");
    kos_macro_begin(&macro, &plan);
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 1 && out.action == KOS_MACRO_ACT_INTERACT &&
               strcmp(out.target_object, plate_stack) == 0,
           "plate 先把手里整份菜放到盘堆上");
    chef->holding[0] = '\0';
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2, "放下后进入取盘");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2 && out.action == KOS_MACRO_ACT_INTERACT,
           "取盘发 interact");
    snprintf(chef->holding, sizeof chef->holding, "equipment_plate_01");
    game_ms += 16;
    expect(kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out) == 1,
           "拿到 equipment_plate_01 后 plate 完成（Plate 按子串匹配）");
}

static void move_chef_to(kos_world_state *world, const kos_kitchen *kitchen,
                         const char *station_id)
{
    int station_index = kos_kitchen_station_index(kitchen, station_id);
    const kos_station_stands *stands;
    kos_chef *chef = &world->chefs[0];

    if (station_index < 0) {
        return;
    }
    stands = kos_stand_cells(kitchen, station_index);
    if (stands == NULL || stands->count <= 0) {
        return;
    }
    chef->x = kitchen->cells[stands->cells[0].cell].x;
    chef->y = kitchen->cells[stands->cells[0].cell].z;
    chef->facing = atan2f(kitchen->stations[station_index].x - chef->x,
                         kitchen->stations[station_index].z - chef->y);
}

/*
 * 倒料完成按"目的台面多出这味料"判（G1：锅端在手上时源槽位是空的，pot_* 里没有
 * 锅里的内容）。这里不挂 carried_，模拟资源帧跨过"端起→倒完"的那一帧。
 */
static void test_pour(const kos_kitchen *kitchen, const char *cooker, const char *container)
{
    kos_world_state world;
    kos_macro_plan plan;
    kos_macro macro;
    kos_macro_output out;
    kos_error err;
    kos_chef *chef;
    kos_resource *pot;
    kos_resource *counter;
    const kos_station *target;
    int64_t game_ms = 1000;
    int rc;

    printf("test_pour\n");
    base_world(&world);
    chef = chef_at_station(&world, kitchen, cooker);
    pot = add_resource(&world, "pot_3");
    snprintf(pot->state, sizeof pot->state, "cooked");
    snprintf(pot->content, sizeof pot->content, "BurritoChicken");
    target = kos_kitchen_station_by_id(kitchen, container);
    expect(target != NULL, "pour 目标工位在 kitchen 里");
    kos_error_clear(&err);
    expect(kos_macro_plan_build_with_source(kitchen, "pour", "pour:BurritoChicken", container,
                                            cooker, "o1", 100, &plan, &err) == KOS_OK,
           "pour 计划建成");
    kos_macro_begin(&macro, &plan);
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 1 && out.action == KOS_MACRO_ACT_INTERACT &&
               strcmp(out.target_object, cooker) == 0,
           "pour 先从灶上端锅（pickup 不认物品名）");
    snprintf(chef->holding, sizeof chef->holding, "utensil_frying_pan_01");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2, "端到锅后去容器");
    move_chef_to(&world, kitchen, container);
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3, "面向容器后进入倒料");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3 && out.action == KOS_MACRO_ACT_INTERACT &&
               strcmp(out.target_object, container) == 0,
           "倒料按容器上的 interact 推进");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3, "台面还没这味料时继续按");

    /* 台面容器里出现这一味料（contents[] 是 cooked:<name>，compat content 是料名） */
    counter = add_resource(&world, "container_0");
    snprintf(counter->kind, sizeof counter->kind, "container");
    snprintf(counter->item, sizeof counter->item, "Tortilla");
    counter->has_pos = 1;
    counter->pos_x = target->x;
    counter->pos_y = target->z;
    counter->content_count = 1;
    snprintf(counter->contents[0], sizeof counter->contents[0], "cooked:BurritoChicken");
    snprintf(counter->content, sizeof counter->content, "BurritoChicken");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 4, "台面出现这味料后回灶");

    move_chef_to(&world, kitchen, cooker);
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    expect(rc == 0 && macro.step_index == 5, "面向灶台后进入放锅");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 5 && out.action == KOS_MACRO_ACT_INTERACT,
           "放锅发 interact");
    chef->holding[0] = '\0';
    game_ms += 16;
    expect(kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out) == 1,
           "锅放回灶上后 pour 完成");
}

/* 把 pour 宏推进到"面向容器、准备倒料"那一步。carried_content 非 NULL 时挂一只
 * `carried_0`（G1/G5 的手持锅），内容按 proto 的编码（"cooked:<name>"）。 */
static void drive_to_pour_item(const kos_kitchen *kitchen, kos_world_state *world, kos_macro *macro,
                               const char *cooker, const char *container, const char *node_id,
                               const char *carried_content, int64_t *game_ms, kos_macro_output *out)
{
    kos_macro_plan plan;
    kos_error err;
    kos_chef *chef;

    kos_error_clear(&err);
    expect(kos_macro_plan_build_with_source(kitchen, "pour", node_id, container,
                                            cooker, "o1", 100, &plan, &err) == KOS_OK,
           "carried 夹具：pour 计划建成");
    kos_macro_begin(macro, &plan);
    (void)finish_goto(macro, kitchen, world, game_ms, out);
    *game_ms += 16;
    (void)kos_macro_advance(macro, kitchen, world, 0, 0, *game_ms, out); /* 端锅 interact */
    chef = chef_find(world, 0);
    if (chef != NULL) {
        snprintf(chef->holding, sizeof chef->holding, "utensil_frying_pan_01");
    }
    if (carried_content != NULL) {
        kos_resource *carried = add_resource(world, "carried_0");

        if (carried != NULL) {
            snprintf(carried->content, sizeof carried->content, "%s", carried_content);
        }
    }
    *game_ms += 16;
    (void)kos_macro_advance(macro, kitchen, world, 0, 0, *game_ms, out); /* 去容器 */
    move_chef_to(world, kitchen, container);
    (void)kos_macro_advance(macro, kitchen, world, 0, 1, *game_ms, out);
    *game_ms += 16;
    (void)kos_macro_advance(macro, kitchen, world, 0, 1, *game_ms, out);
}

static void drive_to_pour(const kos_kitchen *kitchen, kos_world_state *world, kos_macro *macro,
                          const char *cooker, const char *container,
                          const char *carried_content, int64_t *game_ms, kos_macro_output *out)
{
    drive_to_pour_item(kitchen, world, macro, cooker, container, "pour:BurritoChicken",
                       carried_content, game_ms, out);
}

/*
 * G1/G5 的状态模型：锅端在手上，灶台槽位（pot_N）里已经没有内容；倒料完成要按
 * 手上 `carried_<chefId>` 的内容判——既不能因为槽位空就提前收工，也不能卡到超时。
 */
static void test_pour_by_carried(const kos_kitchen *kitchen, const char *cooker,
                                 const char *container)
{
    kos_world_state world;
    kos_macro macro;
    kos_macro_output out;
    kos_resource *carried;
    int64_t game_ms = 1000;
    int rc;

    printf("test_pour_by_carried\n");
    base_world(&world);
    (void)chef_at_station(&world, kitchen, cooker);
    drive_to_pour(kitchen, &world, &macro, cooker, container, "cooked:BurritoChicken", &game_ms,
                  &out);
    carried = resource_by_name(&world, "carried_0");
    expect(macro.step_index == 3 && carried != NULL &&
               strcmp(carried->content, "cooked:BurritoChicken") == 0,
           "carried：端到锅后进入倒料");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3 && out.action == KOS_MACRO_ACT_INTERACT &&
               strcmp(out.target_object, container) == 0,
           "carried：锅里有料时对着容器按");
    game_ms += 2000; /* 时间过去但锅里有料：不能提前推进 */
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3,
           "carried：见过锅里的料后不因时间过去提前推进");
    carried->content[0] = '\0';
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 4, "carried：锅内容清空后回灶");
}

/* 手里的锅装错了料（如鸡肉工序装了米饭）：不能按"内容变空"判成功，要在倒之前就失败（G13 的内容身份）。 */
static void test_pour_wrong_content(const kos_kitchen *kitchen, const char *cooker,
                                    const char *container)
{
    kos_world_state world;
    kos_macro macro;
    kos_macro_output out;
    int64_t game_ms = 1000;
    int rc;

    printf("test_pour_wrong_content\n");
    base_world(&world);
    (void)chef_at_station(&world, kitchen, cooker);
    drive_to_pour(kitchen, &world, &macro, cooker, container, "cooked:BoiledRice", &game_ms,
                  &out);
    expect(macro.step_index == 3, "wrong-content：进入倒料");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc < 0 && macro.step_index == 3,
           "wrong-content：料对不上时停在倒料这一步并失败");
    expect(strstr(macro.fail_detail, "macro=pour") != NULL &&
               strstr(macro.fail_detail, "wrong-item") != NULL,
           "wrong-content：失败原因写明 pour 与 wrong-item");
}

/*
 * 容差匹配：pour 节点按生料名命名（pour:BurritoChicken），锅内上报熟料名
 * （cooked:FriedChicken）或切块 prefab 名（cooked:BurritoChoppedChicken）均能
 * 正常推进倒料，而不是报 wrong-item 失败。
 */
static void test_pour_tolerant_matching(const kos_kitchen *kitchen, const char *cooker,
                                        const char *container)
{
    kos_world_state world;
    kos_macro macro;
    kos_macro_output out;
    kos_resource *carried;
    int64_t game_ms = 1000;
    int rc;

    printf("test_pour_tolerant_matching\n");

    /* 1. 熟料名 cooked:FriedChicken 匹配 pour:BurritoChicken */
    base_world(&world);
    (void)chef_at_station(&world, kitchen, cooker);
    drive_to_pour(kitchen, &world, &macro, cooker, container, "cooked:FriedChicken", &game_ms,
                  &out);
    carried = resource_by_name(&world, "carried_0");
    expect(macro.step_index == 3 && carried != NULL,
           "tolerant：熟料名 FriedChicken 进入倒料");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3 && out.action == KOS_MACRO_ACT_INTERACT,
           "tolerant：熟料名 FriedChicken 通过原料校验并发送 interact");
    carried->content[0] = '\0';
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 4, "tolerant：倒料完成后回灶");

    /* 2. 切块 prefab 名 cooked:BurritoChoppedChicken 匹配 pour:BurritoChicken */
    base_world(&world);
    (void)chef_at_station(&world, kitchen, cooker);
    game_ms = 1000;
    drive_to_pour(kitchen, &world, &macro, cooker, container, "cooked:BurritoChoppedChicken",
                  &game_ms, &out);
    carried = resource_by_name(&world, "carried_0");
    expect(macro.step_index == 3 && carried != NULL,
           "tolerant：切块 prefab 名 BurritoChoppedChicken 进入倒料");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3 && out.action == KOS_MACRO_ACT_INTERACT,
           "tolerant：切块 prefab 名通过原料校验并发送 interact");

    /* 3. 动态词根推导：测试不在任何旧根词列表中的生料/熟料组合（TacoBeef ↔ CookedBeef） */
    base_world(&world);
    (void)chef_at_station(&world, kitchen, cooker);
    game_ms = 1000;
    drive_to_pour_item(kitchen, &world, &macro, cooker, container, "pour:TacoBeef",
                       "cooked:CookedBeef", &game_ms, &out);
    carried = resource_by_name(&world, "carried_0");
    expect(macro.step_index == 3 && carried != NULL,
           "tolerant：未在预设列表的 Beef 词根进入倒料");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3 && out.action == KOS_MACRO_ACT_INTERACT,
           "tolerant：未在预设列表的 Beef 动态推导词根通过并发送 interact");

    /* 4. 无关原料（TacoBeef 装入 BoiledRice）拦截测试 */
    base_world(&world);
    (void)chef_at_station(&world, kitchen, cooker);
    game_ms = 1000;
    drive_to_pour_item(kitchen, &world, &macro, cooker, container, "pour:TacoBeef",
                       "cooked:BoiledRice", &game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc < 0 && macro.step_index == 3 && strstr(macro.fail_detail, "wrong-item") != NULL,
           "tolerant：未在预设列表的 Beef 装入 Rice 判定 wrong-item 失败");
}

/* 没有任何信号（carried_ 空、台面也没多料）时不再有 800 ms 兜底：一直按到步超时，
 * 让失败原因停在 source-empty 而不是把没倒成的料当成倒完了。 */
static void test_pour_carried_without_contents(const kos_kitchen *kitchen, const char *cooker,
                                               const char *container)
{
    kos_world_state world;
    kos_macro macro;
    kos_macro_output out;
    int64_t game_ms = 1000;
    int rc;

    printf("test_pour_carried_without_contents\n");
    base_world(&world);
    (void)chef_at_station(&world, kitchen, cooker);
    drive_to_pour(kitchen, &world, &macro, cooker, container, "", &game_ms, &out);
    expect(macro.step_index == 3, "carried 空：仍然进入倒料");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3 && out.action == KOS_MACRO_ACT_INTERACT,
           "carried 空：第一帧先按一次");
    game_ms += 400;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3, "carried 空：没有兜底窗口，继续按");
    game_ms = macro.deadline_ms + 1;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == -1, "carried 空且台面无料：超时失败");
}

/*
 * 旧状态（无 carried_ 资源）：同一张台面上连续两次倒料（如卷饼先倒肉再倒米）。
 * 第 2 次倒料开始时台面内容已非空（已有肉），不能把台面非空当成"源锅仍有料 (found=1)"，
 * 否则会导致倒完后由于 found 恒为 1 而无法推进，并禁用兜底最终超时。
 */
static void test_pour_legacy_two_in_a_row(const kos_kitchen *kitchen, const char *cooker1,
                                          const char *cooker2, const char *container)
{
    kos_world_state world;
    kos_macro macro;
    kos_macro_plan plan;
    kos_macro_output out;
    kos_error err;
    kos_chef *chef;
    kos_resource *pot1;
    kos_resource *pot2;
    kos_resource *counter;
    int station_idx;
    int64_t game_ms = 1000;
    int rc;

    printf("test_pour_legacy_two_in_a_row\n");
    base_world(&world);
    chef = chef_at_station(&world, kitchen, cooker1);

    pot1 = add_resource(&world, "pot_0");
    snprintf(pot1->state, sizeof pot1->state, "cooked");
    snprintf(pot1->content, sizeof pot1->content, "BurritoChicken");

    pot2 = add_resource(&world, "pot_1");
    snprintf(pot2->state, sizeof pot2->state, "cooked");
    snprintf(pot2->content, sizeof pot2->content, "Rice");

    counter = add_resource(&world, "counter_0");
    station_idx = kos_kitchen_station_index(kitchen, container);
    if (station_idx >= 0) {
        counter->has_pos = 1;
        counter->pos_x = kitchen->stations[station_idx].x;
        counter->pos_y = kitchen->stations[station_idx].z;
    }
    counter->content[0] = '\0';

    /* --- 第 1 单倒料：从 cooker1 倒 BurritoChicken 到 container --- */
    kos_error_clear(&err);
    expect(kos_macro_plan_build_with_source(kitchen, "pour", "pour:BurritoChicken", container,
                                            cooker1, "o1", 100, &plan, &err) == KOS_OK,
           "legacy 连续倒料：第 1 单 pour 计划建成");
    kos_macro_begin(&macro, &plan);
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    snprintf(chef->holding, sizeof chef->holding, "utensil_frying_pan_01");
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    move_chef_to(&world, kitchen, container);
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    expect(macro.step_index == 3, "第 1 单：面向容器进入倒料");

    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    pot1->content[0] = '\0';
    snprintf(counter->content, sizeof counter->content, "BurritoChicken");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 4, "第 1 单：源锅清空且台面有肉后回灶");

    move_chef_to(&world, kitchen, cooker1);
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    chef->holding[0] = '\0';
    game_ms += 16;
    expect(kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out) == 1,
           "第 1 单：放回锅完成");

    /* --- 第 2 单倒料：从 cooker2 倒 Rice 到同一张台面（此时台面已有肉） --- */
    expect(kos_macro_plan_build_with_source(kitchen, "pour", "pour:Rice", container,
                                            cooker2, "o1", 100, &plan, &err) == KOS_OK,
           "legacy 连续倒料：第 2 单 pour 计划建成");
    kos_macro_begin(&macro, &plan);
    move_chef_to(&world, kitchen, cooker2);
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    snprintf(chef->holding, sizeof chef->holding, "utensil_pot_01");
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    move_chef_to(&world, kitchen, container);
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    expect(macro.step_index == 3, "第 2 单：台面已有料时进入倒料");

    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    pot2->content[0] = '\0';
    snprintf(counter->content, sizeof counter->content, "BurritoChicken+Rice");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 4,
           "第 2 单：同一台面第 2 次倒料正确完成并回灶");

    move_chef_to(&world, kitchen, cooker2);
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    chef->holding[0] = '\0';
    game_ms += 16;
    expect(kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out) == 1,
           "第 2 单：放回锅完成");
}

static void test_deliver(const kos_kitchen *kitchen, const char *serving)
{
    kos_world_state world;
    kos_macro macro;
    kos_macro_output out;
    kos_chef *chef;
    int64_t game_ms = 1000;
    int rc;

    printf("test_deliver\n");
    base_world(&world);
    chef = chef_at_station(&world, kitchen, serving);
    snprintf(chef->holding, sizeof chef->holding, "Dish");
    (void)add_order(&world, "world-1");
    expect(build(&macro, kitchen, "deliver", "deliver:Dish", serving, "world-1", 1000) == 0,
           "deliver 计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 1 && out.action == KOS_MACRO_ACT_INTERACT,
           "deliver 发 interact");
    world.order_count = 0;
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 1, "订单从世界状态消失后 deliver 完成");

    /* 手上东西没了：完成条件被打破，应在 1.5 s 之前失败。 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, serving);
    snprintf(chef->holding, sizeof chef->holding, "Dish");
    (void)add_order(&world, "world-2");
    expect(build(&macro, kitchen, "deliver", "deliver:Dish", serving, "world-2", 1000) == 0,
           "deliver holding-lost 计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    chef->holding[0] = '\0';
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 1,
           "deliver 手空：先等订单结果（交付时盘子先消失）");
    game_ms += 1000;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "holding-lost") != NULL,
           "deliver 手空且没有订单结果：grace 后失败");

    /* G9：订单消失 + 分数下降 = 过期，不能记成交付。 */
    base_world(&world);
    world.score = 100;
    chef = chef_at_station(&world, kitchen, serving);
    snprintf(chef->holding, sizeof chef->holding, "Dish");
    (void)add_order(&world, "world-3");
    expect(build(&macro, kitchen, "deliver", "deliver:Dish", serving, "world-3", 1000) == 0,
           "deliver 过期场景计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    world.order_count = 0;
    world.score = 80;
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "macro=deliver") != NULL &&
               strstr(kos_macro_fail_detail(&macro), "expired") != NULL,
           "订单消失且分数下降 → deliver 判过期失败");

    /* 分数上涨 = 交付成功。 */
    base_world(&world);
    world.score = 100;
    chef = chef_at_station(&world, kitchen, serving);
    snprintf(chef->holding, sizeof chef->holding, "Dish");
    (void)add_order(&world, "world-4");
    expect(build(&macro, kitchen, "deliver", "deliver:Dish", serving, "world-4", 1000) == 0,
           "deliver 交付场景计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    world.order_count = 0;
    world.score = 180;
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 1, "订单消失且分数上涨 → deliver 成功");
}

static void test_wash(const kos_kitchen *kitchen, const char *sink)
{
    kos_world_state world;
    kos_macro macro;
    kos_macro_output out;
    kos_chef *chef;
    kos_resource *basin;
    kos_resource *plates;
    int64_t game_ms = 1000;
    int rc;

    printf("test_wash\n");
    base_world(&world);
    chef = chef_at_station(&world, kitchen, sink);
    chef->holding[0] = '\0';
    basin = add_resource(&world, sink);
    plates = add_resource(&world, "plate_stack");
    plates->count = 2;
    expect(build(&macro, kitchen, "wash", "wash:CleanPlate", sink, "o1", 1000) == 0,
           "wash 计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2 && out.action == KOS_MACRO_ACT_STOP,
           "手上没盘子时 drop 立即完成进入 wash");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2 && out.action == KOS_MACRO_ACT_WORK,
           "wash 按住 work");
    basin->progress = 0.5f;
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2, "progress 未满继续洗");
    basin->progress = 1.0f;
    plates->count = 3;
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 1, "progress=1 且干净盘数 +1 后 wash 完成");

    /* 盘堆被其他厨师取走（3 -> 2），但水槽进度从 < 1 到达 1.0 时洗盘仍能正确完成 */
    plates->count = 2;
    expect(build(&macro, kitchen, "wash", "wash:CleanPlate", sink, "o1", 1000) == 0,
           "第 2 盘 wash 计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    basin->progress = 0.5f;
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    /* 别人取走一只盘子，盘堆从 2 减少到 1 */
    plates->count = 1;
    basin->progress = 1.0f;
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 1, "盘堆被并发取走时，progress=1 仍判定 wash 完成");
}

static void test_timeouts(const kos_kitchen *kitchen, const char *dispenser,
                          const char *board_id)
{
    kos_world_state world;
    kos_macro macro;
    kos_macro_output out;
    kos_chef *chef;
    kos_resource *board;
    int64_t game_ms = 1000;
    int rc;

    printf("test_timeouts\n");
    base_world(&world);
    chef = chef_at_station(&world, kitchen, dispenser);
    expect(build(&macro, kitchen, "fetch", "fetch:Carrot", dispenser, "o1", 1000) == 0,
           "超时场景 fetch 计划建成");
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    game_ms += 1501;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "macro=pickup") != NULL &&
               strstr(kos_macro_fail_detail(&macro), "wait=holding") != NULL &&
               strstr(kos_macro_fail_detail(&macro), "timeout") != NULL,
           "pickup 1.5 s 未拿到东西按表超时，detail 写宏动作与条件");

    /* goto 到不了：按距离×2+6 s 超时，不是永远等。 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, dispenser);
    expect(build(&macro, kitchen, "fetch", "fetch:Carrot", dispenser, "o1", 1000) == 0,
           "goto 超时场景计划建成");
    game_ms = 2000;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && out.action == KOS_MACRO_ACT_NAVIGATE, "goto 未到达时发 navigate");
    game_ms += 5000;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && out.action == KOS_MACRO_ACT_NAVIGATE, "6 s 余量内仍在走，不判超时");
    game_ms += 2000;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "macro=goto") != NULL,
           "goto 超时失败");

    /* 距离未知/无计划场景：旧逻辑给 30 s 导致宏永远不报失败；新逻辑必须在短默认时间 (<= 8 s) 内有界超时 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, dispenser);
    {
        int disp_idx = kos_kitchen_station_index(kitchen, dispenser);
        int chef_cell = kos_kitchen_nearest_cell(kitchen, chef->x, chef->y);
        uint16_t orig_dist = ((kos_kitchen *)kitchen)->stand.distance[disp_idx][chef_cell];
        ((kos_kitchen *)kitchen)->stand.distance[disp_idx][chef_cell] = KOS_STAND_UNREACHABLE;

        expect(build(&macro, kitchen, "fetch", "fetch:Carrot", dispenser, "o1", 1000) == 0,
               "距离未知场景计划建成");
        game_ms = 1000;
        rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
        expect(rc == 0 && out.action == KOS_MACRO_ACT_NAVIGATE, "未知距离时先发 navigate");
        game_ms += 9000; /* 9 s 后（超过短默认 6~8 s，但远小于旧逻辑的 30 s） */
        rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
        expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "macro=goto") != NULL,
               "距离未知时 goto 在有界时间内失败，不拖到 30 s");
        ((kos_kitchen *)kitchen)->stand.distance[disp_idx][chef_cell] = orig_dist;
    }
    /* 到站后转身把厨师推离格心，navd 报回 moving：goto 仍在转身阶段发 face，不退回 navigate。 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, dispenser);
    chef->facing += 3.14159265f;
    expect(build(&macro, kitchen, "fetch", "fetch:Carrot", dispenser, "o1", 1000) == 0,
           "到站后漂移场景计划建成");
    game_ms = 9000;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    expect(rc == 0 && out.action == KOS_MACRO_ACT_FACE, "到站先发 face");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && out.action == KOS_MACRO_ACT_FACE && macro.step_index == 0,
           "转身时 navd 报回 moving，goto 继续发 face");
    chef->facing -= 3.14159265f;
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 1, "转过去后 goto 完成，不要求重新到站");

    /* chop 的工时超时：工时 1 s + 3 s。板上放的是该节点的产物名（G13 内容核对
     * 通过），进度一直不涨，等的就是超时。 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, board_id);
    chef->holding[0] = '\0';
    board = add_resource(&world, "cutting_board_0");
    snprintf(board->content, sizeof board->content, "ChoppedCarrot");
    expect(build(&macro, kitchen, "cut", "cut:ChoppedCarrot", board_id, "o1", 1000) == 0,
           "chop 超时场景计划建成");
    game_ms = 3000;
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* drop */
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out); /* chop 开始 */
    game_ms += 4001;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "macro=chop") != NULL &&
               strstr(kos_macro_fail_detail(&macro), "timeout") != NULL,
           "chop 工时 + 3 s 后超时失败");
}


static void test_station_resource_mapping(void)
{
    kos_kitchen kitchen;
    kos_world_state world;
    kos_macro macro;
    kos_macro_output out;
    kos_resource *resource;
    kos_chef *chef;
    int64_t game_ms;
    int rc;

    printf("test_station_resource_mapping\n");

    /* 正例：kitchen.json 的 board-0 ↔ state_update 的 cutting_board_0。 */
    one_station_kitchen(&kitchen, "board-0", "board");
    base_world(&world);
    chef = place_chef_on_station(&world, "Carrot");
    resource = add_resource(&world, "cutting_board_0");
    snprintf(resource->content, sizeof resource->content, "Carrot");
    resource->progress = 0.2f;
    game_ms = 1000;
    expect(build(&macro, &kitchen, "cut", "cut:ChoppedCarrot", "board-0", "o1", 1000) == 0,
           "board-0 的 cut 计划建成");
    rc = kos_macro_advance(&macro, &kitchen, &world, 0, 1, game_ms, &out);
    expect(rc == 0 && out.action == KOS_MACRO_ACT_FACE, "board 到站先发 face");
    game_ms += 16;
    rc = kos_macro_advance(&macro, &kitchen, &world, 0, 1, game_ms, &out);
    expect(rc == 0 && macro.step_index == 1, "board face 完成后进入 drop");
    game_ms += 16;
    rc = kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && out.action == KOS_MACRO_ACT_INTERACT, "board drop 发 interact");
    chef->holding[0] = '\0';
    game_ms += 16;
    rc = kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2, "放手后进入 chop");
    game_ms += 16;
    rc = kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && out.action == KOS_MACRO_ACT_WORK,
           "board-0 映射到 cutting_board_0 并按 work");
    resource->progress = 1.0f;
    game_ms += 16;
    rc = kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3, "切好后进入 pickup");

    /* 反例：container 不再是砧板，不能用 cutting_board_0 的资源。 */
    one_station_kitchen(&kitchen, "container-0", "container");
    base_world(&world);
    chef = place_chef_on_station(&world, "Carrot");
    resource = add_resource(&world, "cutting_board_0");
    snprintf(resource->content, sizeof resource->content, "Carrot");
    game_ms = 1000;
    expect(build(&macro, &kitchen, "cut", "cut:ChoppedCarrot", "container-0", "o1", 1000) == 0,
           "旧 container-0 的 cut 计划仍能建成（映射由执行器判定）");
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    chef->holding[0] = '\0';
    game_ms += 16;
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "no-station-resource") != NULL,
           "container-0 不再映射到 cutting_board_0");

    /* 正例：kitchen.json 的 cooker-0 ↔ state_update 的 pot_0。 */
    one_station_kitchen(&kitchen, "cooker-0", "cooker");
    base_world(&world);
    chef = place_chef_on_station(&world, "Rice");
    resource = add_resource(&world, "pot_0");
    snprintf(resource->state, sizeof resource->state, "free");
    game_ms = 1000;
    expect(build(&macro, &kitchen, "cook", "cook:BoiledRice", "cooker-0", "o1", 1000) == 0,
           "cooker-0 的 cook 计划建成");
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    chef->holding[0] = '\0';
    game_ms += 16;
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && out.action == KOS_MACRO_ACT_STOP,
           "cooker-0 映射到 pot_0 并等待开火（不发 interact）");

    /* 反例：另一口锅的 pot_1 不能被 cooker-0 借用（序号必须对上）。 */
    one_station_kitchen(&kitchen, "cooker-0", "cooker");
    base_world(&world);
    chef = place_chef_on_station(&world, "Rice");
    resource = add_resource(&world, "pot_1");
    snprintf(resource->state, sizeof resource->state, "free");
    game_ms = 1000;
    expect(build(&macro, &kitchen, "cook", "cook:BoiledRice", "cooker-0", "o1", 1000) == 0,
           "只报 pot_1 时 cook 计划仍能建成");
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    chef->holding[0] = '\0';
    game_ms += 16;
    (void)kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, &kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "no-station-resource") != NULL,
           "cooker-0 不借用 pot_1，序号必须一致");
}

int main(void)
{
    kos_kitchen kitchen;
    const char *dispenser;
    const char *board;
    const char *container;
    const char *cooker;
    const char *plate_stack;
    const char *serving;
    const char *sink;
    if (load_kitchen(&kitchen) != 0) {
        printf("FAIL cannot load config/kitchen.example.json\n");
        return 1;
    }
    dispenser = station_of_kind(&kitchen, "dispenser");
    container = station_of_kind(&kitchen, "container");
    /* kitchen.example.json 是砧板还没导出时的旧快照；测试按 exporter 现在的输出补一块
     * board-0（坐标与站位格沿用 container），让 cut 走真实的 board-N ↔ cutting_board_N。 */
    if (container == NULL ||
        add_station_clone(&kitchen, container, "board-0", "board") < 0) {
        printf("FAIL cannot synthesize board-0 in config/kitchen.example.json\n");
        return 1;
    }
    board = station_of_kind(&kitchen, "board");
    cooker = station_of_kind(&kitchen, "cooker");
    serving = station_of_kind(&kitchen, "serving");
    plate_stack = station_of_kind(&kitchen, "plate_stack");
    sink = station_of_kind(&kitchen, "sink");

    test_expansions(&kitchen);
    test_fetch(&kitchen, dispenser);
    test_cut(&kitchen, board);
    test_cook(&kitchen, cooker);
    test_plate(&kitchen, cooker, plate_stack);
    test_pour(&kitchen, cooker, container);
    test_pour_by_carried(&kitchen, cooker, container);
    test_pour_wrong_content(&kitchen, cooker, container);
    test_pour_tolerant_matching(&kitchen, cooker, container);
    test_pour_carried_without_contents(&kitchen, cooker, container);
    test_pour_legacy_two_in_a_row(&kitchen, cooker, "cooker-1", container);
    test_deliver(&kitchen, serving);
    test_wash(&kitchen, sink);
    test_timeouts(&kitchen, dispenser, board);
    test_station_resource_mapping();

    printf("[test_macro] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
