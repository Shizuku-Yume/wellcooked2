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

/* 第一次 advance 发 face，第二次面对准后 goto 完成；返回 advance 的返回值。 */
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
    const char *container = station_of_kind(kitchen, "container");
    const char *cooker = station_of_kind(kitchen, "cooker");
    const char *plate = station_of_kind(kitchen, "plate_stack");
    const char *serving = station_of_kind(kitchen, "serving");
    const char *sink = station_of_kind(kitchen, "sink");
    kos_macro_plan plan;
    kos_error err;

    printf("test_expansions\n");
    expect(dispenser != NULL && container != NULL && cooker != NULL && plate != NULL &&
               serving != NULL && sink != NULL,
           "kitchen.example.json 六类工位齐全");

    kos_error_clear(&err);
    expect(kos_macro_plan_build(kitchen, "fetch", "fetch:Carrot", dispenser, "o1", 1000,
                                &plan, &err) == KOS_OK &&
               plan.step_count == 2 && plan.steps[0].kind == KOS_MACRO_GOTO &&
               plan.steps[1].kind == KOS_MACRO_PICKUP &&
               strcmp(plan.steps[1].item, "Carrot") == 0,
           "fetch = goto + pickup(Carrot)");
    expect(kos_macro_plan_build(kitchen, "cut", "cut:ChoppedCarrot", container, "o1",
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
               plan.step_count == 5 && plan.steps[0].kind == KOS_MACRO_GOTO &&
               plan.steps[1].kind == KOS_MACRO_PICKUP &&
               strcmp(plan.steps[1].item, "Plate") == 0 &&
               strcmp(plan.steps[2].station_id, cooker) == 0 &&
               plan.steps[3].kind == KOS_MACRO_WAIT_READY &&
               plan.steps[4].kind == KOS_MACRO_PICKUP,
           "plate = clean plate pickup + reserved pot wait/pickup");
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

static void test_cut(const kos_kitchen *kitchen, const char *container)
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
    chef = chef_at_station(&world, kitchen, container);
    snprintf(chef->holding, sizeof chef->holding, "Carrot");
    board = add_resource(&world, container);
    board->content[0] = '\0';
    expect(build(&macro, kitchen, "cut", "cut:ChoppedCarrot", container, "o1", 1000) == 0,
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
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3, "progress=1 后进入 pickup");
    snprintf(chef->holding, sizeof chef->holding, "ChoppedCarrot");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 1, "拿到切好的料后 cut 完成");

    /* 完成条件被打破：砧板上的料被别人拿走，应在超时前立即失败。 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, container);
    chef->holding[0] = '\0';
    board = add_resource(&world, container);
    snprintf(board->content, sizeof board->content, "Carrot");
    board->progress = 0.2f;
    expect(build(&macro, kitchen, "cut", "cut:ChoppedCarrot", container, "o1", 1000) == 0,
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
    pot = add_resource(&world, cooker);
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
    expect(rc == 0 && macro.step_index == 2 && out.action == KOS_MACRO_ACT_INTERACT,
           "start_cook 发 interact");
    snprintf(pot->state, sizeof pot->state, "cooking");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 1 && strcmp(pot->state, "cooking") == 0,
           "锅进入 cooking 后 cook 任务立即完成（不等 cooked）");
}

static void test_plate(const kos_kitchen *kitchen, const char *cooker, const char *plate_stack)
{
    kos_world_state world;
    kos_macro_plan plan;
    kos_macro macro;
    kos_macro_output out;
    kos_error err;
    kos_chef *chef;
    kos_resource *pot;
    int64_t game_ms = 1000;
    int rc;

    printf("test_plate\n");
    base_world(&world);
    chef = chef_at_station(&world, kitchen, plate_stack);
    chef->holding[0] = '\0';
    (void)add_resource(&world, plate_stack);
    pot = add_resource(&world, cooker);
    snprintf(pot->state, sizeof pot->state, "cooking");
    snprintf(pot->content, sizeof pot->content, "Dish");
    kos_error_clear(&err);
    expect(kos_macro_plan_build_with_source(kitchen, "plate", "plate:Dish", plate_stack,
                                            cooker, "o1", 1000, &plan, &err) == KOS_OK,
           "plate plans clean plate and exact pot");
    kos_macro_begin(&macro, &plan);
    (void)finish_goto(&macro, kitchen, &world, &game_ms, &out);
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 1 && out.action == KOS_MACRO_ACT_INTERACT,
           "plate fetches clean plate first");
    snprintf(chef->holding, sizeof chef->holding, "Plate");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 2, "clean plate pickup advances to reserved pot");
    chef->x = kitchen->cells[kos_stand_cells(kitchen,
                                              kos_kitchen_station_index(kitchen, cooker))->cells[0].cell].x;
    chef->y = kitchen->cells[kos_stand_cells(kitchen,
                                              kos_kitchen_station_index(kitchen, cooker))->cells[0].cell].z;
    chef->facing = atan2f(kitchen->stations[kos_kitchen_station_index(kitchen, cooker)].x - chef->x,
                          kitchen->stations[kos_kitchen_station_index(kitchen, cooker)].z - chef->y);
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    game_ms += 16;
    (void)kos_macro_advance(&macro, kitchen, &world, 0, 1, game_ms, &out);
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 3 && out.action == KOS_MACRO_ACT_STOP,
           "waits on exact reserved pot");
    snprintf(pot->state, sizeof pot->state, "cooked");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && macro.step_index == 4, "cooked pot advances to dish pickup");
    game_ms += 16;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && out.action == KOS_MACRO_ACT_INTERACT, "dish pickup emits interact");
    snprintf(chef->holding, sizeof chef->holding, "Plate+Dish");
    game_ms += 16;
    expect(kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out) == 1,
           "plated dish completes");
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
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "holding-lost") != NULL,
           "deliver 手上东西没了立即失败");
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
}

static void test_timeouts(const kos_kitchen *kitchen, const char *dispenser,
                          const char *container)
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

    /* goto 到不了：按距离×2+3 s 超时，不是永远等。 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, dispenser);
    expect(build(&macro, kitchen, "fetch", "fetch:Carrot", dispenser, "o1", 1000) == 0,
           "goto 超时场景计划建成");
    game_ms = 2000;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == 0 && out.action == KOS_MACRO_ACT_NAVIGATE, "goto 未到达时发 navigate");
    game_ms += 4000;
    rc = kos_macro_advance(&macro, kitchen, &world, 0, 0, game_ms, &out);
    expect(rc == -1 && strstr(kos_macro_fail_detail(&macro), "macro=goto") != NULL,
           "goto 超时失败");

    /* chop 的工时超时：工时 1 s + 3 s。 */
    base_world(&world);
    chef = chef_at_station(&world, kitchen, container);
    chef->holding[0] = '\0';
    board = add_resource(&world, container);
    snprintf(board->content, sizeof board->content, "Carrot");
    expect(build(&macro, kitchen, "cut", "cut:ChoppedCarrot", container, "o1", 1000) == 0,
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

int main(void)
{
    kos_kitchen kitchen;
    const char *dispenser;
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
    cooker = station_of_kind(&kitchen, "cooker");
    serving = station_of_kind(&kitchen, "serving");
    plate_stack = station_of_kind(&kitchen, "plate_stack");
    sink = station_of_kind(&kitchen, "sink");

    test_expansions(&kitchen);
    test_fetch(&kitchen, dispenser);
    test_cut(&kitchen, container);
    test_cook(&kitchen, cooker);
    test_plate(&kitchen, cooker, plate_stack);
    test_deliver(&kitchen, serving);
    test_wash(&kitchen, sink);
    test_timeouts(&kitchen, dispenser, container);

    printf("[test_macro] 检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
