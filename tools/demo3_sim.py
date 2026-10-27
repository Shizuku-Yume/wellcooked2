#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""s_wellcooked_demo_3 的离线世界模型：按实机实测规则判定放置、加工、装盘与交付。

和 tools/fake_plugin.py 里的 WorldSim 的区别：WorldSim 只模拟"宏动作等世界状态"的
最小事实（holding/工位内容/一个 bool），这个模块按 `_3` 单人手动局的动作表
（.workflow/specs/kitchen-control.md「P0 第 1 步：_3 实测动作表」）判定每一步：

  * 交互按"按下沿"生效，Worker 的 interact 意图按插件节奏变成两次按键
    （0 ms 一次、600 ms 一次；每次按住 250 ms），同一次意图不会变成连点。
  * 拿/放要面对面，且在这个工位的交互半径内；半个格子的网格偏移会按格选错工位。
  * 砧板：按住加工键才推进 8 格进度（合计 1.7 s），X 松开就停。
  * 锅：只收 `AllowedCookingSteps` 含锅类型的料；12.1 s 熟，继续加热到 20 s 烧焦
    （烧焦时间 [推断]——手动局只测到"已熟后 16.1 s 仍未烧焦"）。
  * 倒料：端着锅对着台面上的玉米饼按 A，锅里的料进玉米饼，锅仍在手上。
  * 装盘：手里的整份菜放到干净盘堆上，是"转交给最上面那只盘子"；空手再按一次取盘。
  * 交付：盘到传菜口 0.5 s 后消失并加分；订单已过期时盘子照收，但只扣分不加分。
  * 订单：`_3` 的 5 道菜，3 个同时挂着的槽位，每单 60 s [推断]，一局 120 s。

上报格式以 protocol/schema/state_update.schema.json（commit c3cd157）为准：默认发修好后的
字段（pot_<N>=灶台槽位、空灶台 state=empty、plate_stack.count、counter_N、carried_N、
contents[] 叶子带 cooked: 前缀、chefs[].holding_contents、orders[].score、order_results[]、
round_time_left/limit、资源 pos）。"实机上报缺陷"用开关回到旧行为，用来证明修好前后：

  legacy_pot_keys=True  空锅不进帧；有料的锅按"枚举下标"编 pot_N（G1/P3）
  plate_stack_reported=False  帧里没有 plate_stack.count（ClientCleanPlateStack.GetSize
                              不存在，G2/P0 第 6 步）
  retry_guard=True     第二次按键前比较手持物；True = 已修好的 ActionDriver
  interact_pulses=True 按 250 ms 按住 + 600 ms 重试的节奏展开交互意图

时间单位统一为 game_ms，与 state_update.game_time_ms 同一个钟。
"""

import json
import math
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
KITCHEN_3 = REPO_ROOT / "config" / "maps" / "s_wellcooked_demo_3" / "kitchen.json"

CELL_M = 0.6
CHOP_TIME_MS = 1700          # 实测 8 格 1.6–1.75 s（L117–L531 等五段）
CHOP_STEPS = 8
COOK_TIME_MS = 12100         # 实测 12.07–12.15 s 到"已熟"
BURN_TIME_MS = 20000         # [推断] 实测已熟后 16.10 s 仍未烧焦，取 20 s 作默认
WASH_TIME_MS = 3000          # 任务 07 的 wash_ms 默认值；未在 `_3` 上单独实测
PASS_CLEAR_MS = 500          # 实测盘落传菜口到消失约 0.5 s
PULSE_MS = 250               # ActionDriver 一次按键按住 250 ms
RETRY_MS = 600               # 第二次按键从 600 ms 起
INTENT_GAP_MS = 300          # 交互意图断 300 ms 才算新一次按键序列
PRESS_REACH_M = 1.55         # 站位格到工位中心 1.2 m，留转身漂移余量
PRESS_CONE_DEG = 35.0        # 面前 ±35° 内才算对着这个工位
WORK_CONE_DEG = 45.0
TURN_DRIFT_M = 0.30          # 实测转身漂出 navd 的 0.3 m 到站圈

# `_3` 的菜单顺序（levels/wellcooked_demo/layout_3.tsv 的 `# menu:` 行；游戏日志 0–4 一致）
MENU = ["Chicken_Burrito", "Meat_Burrito", "Mushroom_Burrito",
        "Sushi_PlainFish", "Sushi_PlainPrawn"]
REWARDS = {"Chicken_Burrito": 80, "Meat_Burrito": 80, "Mushroom_Burrito": 80,
           "Sushi_PlainFish": 20, "Sushi_PlainPrawn": 20}

# 料箱 prefab 的组件（spec P0「已核对的事实」表）+ 实测切完的名字（手动局动作表）。
#   workable: 切完的 prefab 名（None = 不能切）
#   cook_steps: CookableProperties.AllowedCookingSteps（空 = 不能下锅）
INGREDIENTS = {
    "SushiFish":       {"workable": "ChoppedSushiFish",      "cook_steps": ()},
    "SushiPrawn":      {"workable": "ChoppedSushiPrawn",     "cook_steps": ("Pot", "FryingPan")},
    "BurritoChicken":  {"workable": "BurritoChoppedChicken", "cook_steps": ("FryingPan",)},
    "BurritoMeat":     {"workable": "DicedMeat",             "cook_steps": ("FryingPan",)},
    "Mushroom":        {"workable": "ChoppedMushroom",       "cook_steps": ("FryingPan",)},
    "Rice":            {"workable": None,                    "cook_steps": ("Pot",)},
    "Tortilla":        {"workable": None, "cook_steps": (), "container": True},
}

# 盘上内容（(prefab, 是否熟) 的多重集）→ 菜名
# 切完的名字也要能下锅（名字变了，CookableProperties 跟着 prefab 走）
COOK_RULES = {}
for _raw, _rules in INGREDIENTS.items():
    COOK_RULES[_raw] = _rules
    if _rules.get("workable"):
        COOK_RULES[_rules["workable"]] = {"cook_steps": _rules.get("cook_steps", ()),
                                          "workable": None}

CHOPPED_NAMES = {"ChoppedSushiFish", "ChoppedSushiPrawn", "BurritoChoppedChicken",
                 "DicedMeat", "ChoppedMushroom"}

# 实测（logs/manual/20261027-022317 manual.log）：切好的料进锅后，锅/容器内容报的是
# 订单里的生料名——t=64.63 锅=[BurritoChicken]（手上是 BurritoChoppedChicken）、
# t=105.47 锅=[BurritoMeat] 已熟、t=124.67 锅=[Mushroom]。既不是切完的 prefab
# （DicedMeat/BurritoChoppedChicken），也不是菜单上的熟料名（FriedChicken）。
# 所以“进锅”这一刻把 prefab 名换回生料名；手里拿的仍是切完的 prefab。
ORDER_NAMES = {"BurritoChoppedChicken": "BurritoChicken", "DicedMeat": "BurritoMeat",
               "ChoppedMushroom": "Mushroom", "ChoppedSushiFish": "SushiFish",
               "ChoppedSushiPrawn": "SushiPrawn"}

DISH_CONTENTS = {
    "Sushi_PlainFish":   (("ChoppedSushiFish", False),),
    "Sushi_PlainPrawn":  (("ChoppedSushiPrawn", False),),
    # 锅里的叶子按生料名上报，装盘/玉米饼里也就是生料名（熟叶子带 cooked: 前缀）。
    "Chicken_Burrito":   (("BurritoChicken", True), ("Rice", True)),
    "Meat_Burrito":      (("BurritoMeat", True), ("Rice", True)),
    "Mushroom_Burrito":  (("Mushroom", True), ("Rice", True)),
}

MACRO_STATION_POS_TOL = 0.30

def _load_kitchen_positions(source=KITCHEN_3):
    boards = {}
    counters = {}
    if Path(source).exists():
        try:
            data = json.loads(Path(source).read_text())
            for s in data.get("stations", []):
                sid = s.get("id")
                pos = s.get("pos")
                if not sid or not pos:
                    continue
                if s.get("kind") == "board":
                    boards[sid] = (float(pos[0]), float(pos[1]))
                elif s.get("kind") == "counter":
                    counters[sid] = (float(pos[0]), float(pos[1]))
        except Exception:
            pass
    if not boards:
        boards = {"board-0": (3.0, -1.2), "board-1": (6.6, -1.2)}
    if not counters:
        counters = {"counter-0": (-3.0, -7.2)}
    return boards, counters

BOARD_POS, COUNTER_POS = _load_kitchen_positions()
SPAWNS = [(1.8, -3.6), (4.2, -3.6), (6.0, -3.6), (7.8, -3.6)]

def compute_stands_from_kitchen(source=KITCHEN_3):
    if not Path(source).exists():
        return {}
    try:
        kitchen = json.loads(Path(source).read_text())
    except Exception:
        return {}
    cells = kitchen.get("grid", {}).get("cells", [])
    stands = {}
    for s in kitchen.get("stations", []):
        sid = s.get("id")
        pos = s.get("pos")
        if not sid or not pos:
            continue
        px, py = float(pos[0]), float(pos[1])
        best = None
        best_d = float('inf')
        for cx, cy in cells:
            cx, cy = float(cx), float(cy)
            dx = abs(cx - px)
            dy = abs(cy - py)
            if dx < 0.1:
                d = dy
            elif dy < 0.1:
                d = dx
            else:
                continue
            if 0.5 <= d <= 1.5 and d < best_d:
                best_d = d
                best = (round(cx, 3), round(cy, 3))
        if best:
            stands[sid] = best
    return stands

# 站位格（STANDS）：每个工位旁边供厨师站立交互的可走格坐标（非工位自身坐标）。
# 例如：cooker-5 位于 (7.8, -6.0)，对应站位格为 (7.8, -7.2)；
# 所有工位实际坐标均从 kitchen.json（或实机 fixture）的 stations[].pos 加载。
STANDS = {
    "dispenser-0": (0.6, -7.2), "dispenser-1": (1.8, -7.2),
    "dispenser-2": (3.0, -7.2), "dispenser-3": (4.2, -7.2),
    "dispenser-4": (5.4, -7.2), "dispenser-5": (6.6, -7.2),
    "dispenser-6": (7.8, -7.2),
    "board-0": (3.0, -2.4), "board-1": (6.6, -2.4),
    "counter-0": (-1.8, -7.2), "counter-13": (0.6, -7.2),
    "cooker-0": (-1.8, -3.6), "cooker-1": (-1.8, -2.4),
    "cooker-2": (1.8, -7.2), "cooker-3": (3.0, -7.2),
    "cooker-4": (6.6, -7.2), "cooker-5": (7.8, -7.2),
    "plate_stack-0": (3.0, 0.0), "serving-0": (4.8, 0.0),
    "sink-0": (11.4, -2.4),
}
_k_stands = compute_stands_from_kitchen(KITCHEN_3)
_k_stands["board-1"] = (6.6, -2.4)
_k_stands["cooker-4"] = (6.6, -7.2)
_k_stands["counter-0"] = (-1.8, -7.2)
STANDS.update(_k_stands)


class Demo3Rules:
    """模拟器的规则开关。默认 = 修好后的上报 + 已修好的 ActionDriver。"""

    def __init__(self, **overrides):
        self.legacy_pot_keys = False
        self.plate_stack_reported = True
        self.interact_pulses = True
        self.retry_guard = True
        self.grid_offset = (0.0, 0.0)
        self.turn_drift = TURN_DRIFT_M
        self.burn_ms = BURN_TIME_MS
        self.cook_ms = COOK_TIME_MS
        self.wash_ms = WASH_TIME_MS
        self.arrival_gap_ms = 5000
        self.slots = 3
        self.dispenser_swap = True
        for key, value in overrides.items():
            if not hasattr(self, key):
                raise AttributeError("unknown demo3 rule %r" % (key,))
            setattr(self, key, value)


def linear_recipes(dish, steps, *, score=10, root=None):
    """一条链的 recipes.json：steps 是 _node() 的参数（按顺序连边）。

    回归场景用它把某个游戏规则单独隔离出来（不带 pour/plate 这些依赖别的 worker 的工序）。
    """
    nodes = []
    edges = []
    previous = None
    for spec in steps:
        node = _node(**spec)
        nodes.append(node)
        if previous is not None:
            edges.append([previous, node["id"]])
        previous = node["id"]
    durations = {}
    for node in nodes:
        durations[node["op"]] = max(durations.get(node["op"], 0.0), node["duration"])
    return {"scene": "s_wellcooked_demo_3-sim", "nodes": nodes, "edges": edges,
            "dishes": [{"dish": dish, "score": int(score),
                        "root": root or previous,
                        "nodes": [node["id"] for node in nodes]}],
            "durations": durations,
            "notes": ["linear fixture for a single game rule"]}


# --------------------------------------------------------------------------
# 工位与菜谱 fixture
# --------------------------------------------------------------------------


def demo3_kitchen(*, boards=True, counter=True, cookers=None, clean_plates=5,
                  sink=True, source=KITCHEN_3):
    """以实机导出的 kitchen.json 为底：网格与料箱/灶台/砧板/台面/盘堆/传菜口都是真数据。

    导出的 `container-N` 是锅具 utensil，并进 cooker 的 step，这里不再作为工位。
    `clean_plate_stack` 与 `plate_stack` 同格，模拟器只保留后者（洗好的盘子直接进 plate_stack.count）。
    若 kitchen.json 缺少对应工位（旧 fixture），按默认坐标补齐。
    """
    kitchen = json.loads(Path(source).read_text())
    stations = []
    for station in kitchen["stations"]:
        kind = station.get("kind")
        if kind in ("container", "clean_plate_stack"):
            continue
        if not sink and kind == "sink":
            continue
        if not boards and kind == "board":
            continue
        if not counter and kind == "counter":
            continue
        stations.append(dict(station))
    if cookers is not None:
        stations = [s for s in stations if s.get("kind") != "cooker"]
        stations.extend(dict(s) for s in cookers)
    if boards and not any(s.get("kind") == "board" for s in stations):
        for sid, pos in BOARD_POS.items():
            stations.append({"id": sid, "kind": "board", "name": "chopping_board",
                             "types": ["ClientWorkstation"], "pos": list(pos),
                             "height": 0.0, "contents": []})
    if counter and not any(s.get("kind") == "counter" for s in stations):
        for sid, pos in COUNTER_POS.items():
            stations.append({"id": sid, "kind": "counter", "name": "countertop",
                             "types": ["ClientAttachStation"], "pos": list(pos),
                             "height": 0.0, "contents": []})
    kitchen["stations"] = stations
    kitchen["scene"] = "s_wellcooked_demo_3-sim"
    kitchen.setdefault("sim", {})
    kitchen["sim"]["clean_plates"] = int(clean_plates)
    return kitchen


def _node(node_id, op, label, resource, *, ingredient="", duration=1.0, step="",
          workable=None):
    node = {"id": node_id, "op": op, "label": label, "resource": resource,
            "duration": float(duration)}
    if ingredient:
        node["ingredient"] = ingredient
    if step:
        node["step"] = step
    if workable is not None:
        node["workable"] = bool(workable)
    return node


def build_recipes(*, cook_step=True, dishes=None, plate_layout="spec"):
    """按 spec「对 build_recipes 的约束」生成 `_3` 的工序图（手动 flow 的目标形态）。

    cook_step=False 造老图（cook 节点没有 step），用来复现"米被派到炸锅"。
    plate_layout="source" 造老图（plate 只有一个节点，靠 source 去灶上取料），
    用来复现装盘规则不对时的失败；默认是"整份菜放盘堆 → 取盘"。
    """
    dishes = list(MENU if dishes is None else dishes)
    nodes = []
    edges = []
    seen = set()

    def add(node, child_of=()):
        if node["id"] in seen:
            return node["id"]
        seen.add(node["id"])
        nodes.append(node)
        for parent in child_of:
            # edges 的约定是"前置 → 后继"（子 → 父）：第一个元素先做。
            edges.append([parent, node["id"]])
        return node["id"]

    dish_entries = []
    for dish in dishes:
        if dish in ("Sushi_PlainFish", "Sushi_PlainPrawn"):
            raw = "SushiFish" if dish.endswith("Fish") else "SushiPrawn"
            cut = INGREDIENTS[raw]["workable"]
            fetch = add(_node("fetch:%s" % raw, "fetch", raw, "dispenser",
                              ingredient=raw.lower(), workable=True))
            cut_id = add(_node("cut:%s" % cut, "cut", cut, "board", duration=1.7),
                         child_of=(fetch,))
            if plate_layout == "source":
                plate = add(_node("plate:%s" % dish, "plate", dish, "plate_stack",
                                  duration=0.2), child_of=(cut_id,))
            else:
                plate = add(_node("plate:%s" % dish, "plate", dish, "plate_stack",
                                  duration=0.2), child_of=(cut_id,))
            deliver = add(_node("deliver:%s" % dish, "deliver", dish, "serving"),
                          child_of=(plate,))
            dish_entries.append({"dish": dish, "score": REWARDS[dish],
                                 "root": deliver,
                                 "nodes": [fetch, cut_id, plate, deliver]})
            continue

        # 卷饼：玉米饼放台面当容器；料切完下锅；米下汤锅；两口锅先后倒进玉米饼。
        tortilla_fetch = add(_node("fetch:Tortilla", "fetch", "Tortilla", "dispenser",
                                   ingredient="tortilla"))
        tortilla_place = add(_node("place:Tortilla", "place", "Tortilla", "counter"),
                             child_of=(tortilla_fetch,))
        if dish == "Chicken_Burrito":
            raw, chopped, cooked, step = "BurritoChicken", "BurritoChoppedChicken", \
                "FriedChicken", "FryingPan"
        elif dish == "Meat_Burrito":
            raw, chopped, cooked, step = "BurritoMeat", "DicedMeat", \
                "FriedBurritoMeat", "FryingPan"
        else:
            raw, chopped, cooked, step = "Mushroom", "ChoppedMushroom", \
                "FriedMushrooms", "FryingPan"
        fetch = add(_node("fetch:%s" % raw, "fetch", raw, "dispenser",
                          ingredient=raw.lower(), workable=True))
        cut = add(_node("cut:%s" % chopped, "cut", chopped, "board", duration=1.7),
                  child_of=(fetch,))
        cook = add(_node("cook:%s" % cooked, "cook", cooked, "cooker", duration=12.1,
                         step=step if cook_step else ""), child_of=(cut,))
        rice_fetch = add(_node("fetch:Rice", "fetch", "Rice", "dispenser",
                               ingredient="rice"))
        rice_cook = add(_node("cook:BoiledRice", "cook", "BoiledRice", "cooker",
                              duration=12.1, step="Pot" if cook_step else ""),
                        child_of=(rice_fetch,))
        # pour 的 item 取锅里 contents[] 的叶子名：实机锅里报的是订单里的生料名
        # （logs/manual 20261027-022317：手上 BurritoChoppedChicken，锅=[BurritoChicken]；
        # 已熟的 BurritoMeat/Rice 也是这个名字），既不是切完的 prefab 也不是熟料名。
        # macro run_pour 按 content 子串对源锅，节点 label 必须写这个名字。
        pour_meat = add(_node("pour:%s" % raw, "pour", raw, "counter",
                              ingredient=raw.lower(), duration=1.0),
                        child_of=(cook, tortilla_place))
        pour_rice = add(_node("pour:Rice", "pour", "Rice", "counter",
                              ingredient="rice", duration=1.0),
                        child_of=(rice_cook, tortilla_place))
        pickup = add(_node("pickup:%s_Tortilla" % dish, "pickup", dish, "counter",
                           duration=1.0), child_of=(pour_meat, pour_rice))
        plate = add(_node("plate:%s" % dish, "plate", dish, "plate_stack",
                          duration=0.2), child_of=(pickup,))
        deliver = add(_node("deliver:%s" % dish, "deliver", dish, "serving"),
                      child_of=(plate,))
        dish_entries.append({"dish": dish, "score": REWARDS[dish], "root": deliver,
                             "nodes": [tortilla_fetch, tortilla_place, fetch, cut, cook,
                                       rice_fetch, rice_cook, pour_meat, pour_rice,
                                       pickup, plate, deliver]})
    durations = {"fetch": 1.0, "cut": 1.7, "cook": 12.1, "pour": 1.0, "place": 1.0,
                 "pickup": 1.0, "plate": 1.0, "deliver": 1.0, "wash": 3.0}
    return {"scene": "s_wellcooked_demo_3-sim", "nodes": nodes, "edges": edges,
            "dishes": dish_entries, "durations": durations,
            "notes": ["工时来自 logs/manual/20261027-022317 的实测动作表",
                      "烧焦时间未实测，模拟器用 BURN_TIME_MS=20000 [推断]"]}


def demo3_files(directory, *, kitchen=None, recipes=None, orders_menu=(), seed=7):
    """把 kitchen/recipes/orders 写到目录里，返回 Supervisor 用的三个路径。"""
    directory = Path(directory)
    kitchen = kitchen if kitchen is not None else demo3_kitchen()
    recipes = recipes if recipes is not None else build_recipes()
    orders = {"seed": seed, "level": "s_wellcooked_demo_3", "menu": list(orders_menu)}
    paths = {}
    for name, payload in (("kitchen.json", kitchen), ("recipes.json", recipes),
                          ("orders.json", orders)):
        path = directory / name
        path.write_text(json.dumps(payload, ensure_ascii=False))
        paths[name] = str(path)
    return paths


def order_schedule(dishes=None, *, first_at_ms=0, timeout_s=60, slots=3, arrival_gap_ms=5000):
    """`_3` 的订单到达表：3 个槽位，空出一个 5 s 后补下一道 [推断]。"""
    dishes = list(MENU if dishes is None else dishes)
    schedule = []
    for index, dish in enumerate(dishes):
        schedule.append({"id": "order_%03d" % (index + 1), "dish": dish,
                         "reward": REWARDS.get(dish, 10), "timeout_s": timeout_s,
                         "arrive_ms": first_at_ms if index < slots else None})
    return {"schedule": schedule, "slots": slots, "arrival_gap_ms": arrival_gap_ms}


# --------------------------------------------------------------------------
# 世界状态
# --------------------------------------------------------------------------


class Item:
    """手上/工位上的一个物体。盘子和玉米饼是容器，contents 里装料。"""

    __slots__ = ("name", "contents", "cooked", "container", "utensil", "step")

    def __init__(self, name, *, contents=None, cooked=False, container=False,
                 utensil=False, step=""):
        self.name = name
        self.contents = list(contents or [])
        self.cooked = bool(cooked)
        self.container = bool(container)
        self.utensil = bool(utensil)
        self.step = step

    def copy(self, *, deep=True):
        return Item(self.name,
                    contents=[item.copy() for item in self.contents] if deep else self.contents,
                    cooked=self.cooked, container=self.container, utensil=self.utensil,
                    step=self.step)

    def describe(self):
        if self.contents:
            return "%s[%s]" % (self.name, "+".join(item.name for item in self.contents))
        return self.name

    def __repr__(self):
        return "Item(%s)" % self.describe()


def _make_item(name):
    rules = INGREDIENTS.get(name)
    if rules is None:
        if name == "Plate":
            return Item("equipment_plate_01", container=True)
        return Item(name)
    if rules.get("container"):
        return Item(name, container=True)
    return Item(name)


def _utensil_name(step):
    return {"Pot": "utensil_pot_01", "FryingPan": "utensil_frying_pan_01",
            "DeepFatFryer": "utensil_frier_basket_01"}.get(step, "utensil_" + step)


def _item_ingredients(item):
    if item.container:
        return [entry for child in item.contents for entry in _item_ingredients(child)]
    return [(item.name, item.cooked)]


def match_dish(contents):
    """盘上内容的多重集 → 菜名；对不上任何菜谱时返回 None。"""
    have = sorted(_item_ingredients(item) for item in contents)
    have = sorted(entry for part in have for entry in part)
    for dish, want in DISH_CONTENTS.items():
        if sorted(want) == have:
            return dish
    return None


def _game_facing(dx, dy):
    """游戏朝向：atan2(forward.x, forward.z)，我们的平面 y 就是 z（与插件/macro.c 一致）。"""
    return math.atan2(dx, dy)


def _plane_angle(facing):
    """游戏朝向 → 平面上的移动方向角（atan2(dy, dx)）。"""
    return math.pi / 2.0 - facing


def _angle_diff(a, b):
    diff = math.fmod(a - b, 2.0 * math.pi)
    if diff > math.pi:
        diff -= 2.0 * math.pi
    elif diff < -math.pi:
        diff += 2.0 * math.pi
    return abs(diff)


class Demo3World:
    """`_3` 的规则世界。移动交给 NavSim（positions 是同一个列表对象）。

    场景每帧调用 process_actions(累计动作, game_ms) 和 tick(game_ms)，然后用
    message() 发 state_update。动作效果与动作到达同一帧生效，和实机差一帧。
    """

    def __init__(self, motion, stations, *, rules=None, orders=None, score=0,
                 round_ms=120000, expire_penalty=100, clean_plates=5,
                 plate_stack_reported=True):
        self.motion = motion
        self.positions = motion.positions
        self.stations = {station["id"]: dict(station) for station in stations
                         if station.get("id")}
        self.chef_count = len(self.positions)
        self.score = int(score)
        self.round_ms = int(round_ms)
        self.expire_penalty = int(expire_penalty)
        self.game_ms = 0
        self.last_tick_ms = 0

        # 规则开关：默认真机（缺陷在）
        self.legacy_pot_keys = bool(getattr(rules, "legacy_pot_keys", False))
        self.interact_pulses = bool(getattr(rules, "interact_pulses", True))
        self.retry_guard = bool(getattr(rules, "retry_guard", True))
        self.grid_offset = tuple(getattr(rules, "grid_offset", (0.0, 0.0)))
        self.turn_drift = float(getattr(rules, "turn_drift", TURN_DRIFT_M))
        self.burn_ms = int(getattr(rules, "burn_ms", BURN_TIME_MS))
        self.cook_ms = int(getattr(rules, "cook_ms", COOK_TIME_MS))
        self.wash_ms = int(getattr(rules, "wash_ms", WASH_TIME_MS))
        self.arrival_gap_ms = int(getattr(rules, "arrival_gap_ms", 5000))
        self.slots = int(getattr(rules, "slots", 3))
        self.dispenser_swap = bool(getattr(rules, "dispenser_swap", True))
        self.plate_stack_reported = bool(
            getattr(rules, "plate_stack_reported", plate_stack_reported))

        self.facings = [0.0] * self.chef_count
        self.holdings = [None] * self.chef_count
        self.working = [False] * self.chef_count
        self.press_state = [self._new_press() for _ in range(self.chef_count)]
        self.work_state = [{"station": None} for _ in range(self.chef_count)]
        self.last_action_ms = [-10 ** 9] * self.chef_count
        self.seen_actions = 0
        self.face_targets = [None] * self.chef_count

        # 工位内容
        self.board_state = {}
        self.cooker_state = {}
        self.counter_state = {}
        for station in self.stations.values():
            kind = station.get("kind")
            if kind == "board":
                self.board_state[station["id"]] = {"item": None, "progress": 0.0}
            elif kind == "cooker":
                self.cooker_state[station["id"]] = {
                    "step": station.get("step") or "",
                    "utensil": _utensil_name(station.get("step") or ""),
                    "item": None, "state": "free", "started_ms": 0}
            elif kind == "counter":
                self.counter_state[station["id"]] = {"item": None}
        self.sink = {"dirty": 0, "progress": 0.0, "done_ms": -10 ** 9}
        self.plates = [Item("equipment_plate_01", container=True)
                       for _ in range(int(clean_plates))]
        self.pass_pending = []
        self.delivered = []
        self.delivered_expired = []
        self.expired = []
        self.rejected = []
        self.presses = []
        self.drift_events = []
        self.cook_events = []
        self.pour_events = []
        self.order_queue = []
        self.active_orders = []
        self.next_arrival_ms = 0
        self.order_source = order_schedule()  # 被 set_orders 覆盖
        self.order_results = []
        self.order_result_seq = 0
        self._reject_reason = ""
        self._assert_station_positions()

    def _assert_station_positions(self):
        if not KITCHEN_3.exists() or len(self.stations) < 50:
            return
        try:
            ref = json.loads(KITCHEN_3.read_text())
        except Exception:
            return
        ref_stations = {s["id"]: s["pos"] for s in ref.get("stations", []) if s.get("id") and "pos" in s}
        for sid, station in self.stations.items():
            if sid in ref_stations and "pos" in station:
                p1 = station["pos"]
                p2 = ref_stations[sid]
                dist = math.hypot(p1[0] - p2[0], p1[1] - p2[1])
                assert dist <= MACRO_STATION_POS_TOL, (
                    f"Station {sid} position {p1} drifts from kitchen.json {p2} (dist={dist:.3f} > {MACRO_STATION_POS_TOL})"
                )
    # -- 初始化订单 --
    def set_orders(self, schedule):
        self.order_queue = [dict(entry) for entry in schedule.get("schedule", [])]
        self.slots = int(schedule.get("slots", self.slots))
        self.arrival_gap_ms = int(schedule.get("arrival_gap_ms", self.arrival_gap_ms))
        self.active_orders = []
        for entry in self.order_queue:
            if entry.get("arrive_ms") is None:
                break
        self._fill_orders(0)

    def _new_press(self):
        return {"active": False, "start_ms": 0, "target": "", "pressed": 0,
                "entry_holding": None, "skipped_second": False}

    def _fill_orders(self, game_ms):
        while len(self.active_orders) < self.slots and self.order_queue:
            entry = self.order_queue[0]
            arrive = entry.get("arrive_ms")
            if arrive is not None and game_ms < arrive:
                break
            self.order_queue.pop(0)
            order = {"id": entry["id"], "recipe": entry["dish"],
                     "reward": int(entry.get("reward", 10)),
                     "deadline_ms": game_ms + int(entry.get("timeout_s", 60)) * 1000}
            self.active_orders.append(order)

    def _release_next(self, game_ms):
        if self.order_queue and self.order_queue[0].get("arrive_ms") is None:
            self.order_queue[0]["arrive_ms"] = game_ms + self.arrival_gap_ms

    # -- 每帧 --
    def process_actions(self, actions, game_ms):
        self.game_ms = max(self.game_ms, game_ms)
        self.motion.process_actions(actions, game_ms)
        for action in actions[self.seen_actions:]:
            chef = action.get("chef_id")
            if not isinstance(chef, int) or isinstance(chef, bool):
                continue
            if chef < 0 or chef >= self.chef_count:
                continue
            kind = action.get("action")
            target = action.get("target_object") or ""
            if kind == "face":
                self._face(chef, action, game_ms)
            elif kind == "interact":
                self._interact_intent(chef, target, game_ms)
            elif kind == "work":
                self._work_intent(chef, target, game_ms)
            elif kind == "stop":
                # ActionDriver 收到 stop 会删掉当前 Command：动作里的按键序列到此为止，
                # 下一步的 interact 会重新登记、重新按两下（不留 1 s 重发黑窗）。
                self.face_targets[chef] = None
                self.press_state[chef] = self._new_press()
                self.last_action_ms[chef] = -10 ** 9
        self.seen_actions = len(actions)

    def tick(self, game_ms):
        dt = max(0, int(game_ms) - int(self.last_tick_ms))
        self.last_tick_ms = int(game_ms)
        self.game_ms = int(game_ms)
        self._advance_jobs(dt)
        self.working = [False] * self.chef_count
        self._advance_cookers(game_ms)
        self._advance_orders(game_ms)
        self._advance_pass(game_ms)
        self.motion.advance(game_ms)
        return game_ms

    def frame(self, game_ms):
        return self.motion.frame(game_ms)

    def _advance_jobs(self, dt):
        if dt <= 0:
            return
        for chef in range(self.chef_count):
            if not self.working[chef]:
                continue
            station = self.work_state[chef]["station"]
            if station is None:
                continue
            kind = station.get("kind")
            if kind == "board":
                state = self.board_state[station["id"]]
                item = state["item"]
                if item is None or state["progress"] >= 1.0:
                    continue
                rules = INGREDIENTS.get(item.name, {})
                if not rules.get("workable"):
                    continue
                state["progress"] = min(1.0, state["progress"] + dt / float(CHOP_TIME_MS))
                if state["progress"] >= 1.0:
                    item.name = rules["workable"]
                    self.cook_events.append(("chop", station["id"], item.name))
            elif kind == "sink":
                # progress 停在 1.0 直到下一次"重新开始洗"（macro run_wash 要看到
                # progress=1 且干净盘数上升才判完成；实机也是洗完那一下才归零）。
                if self.sink["progress"] >= 1.0 or self.sink["dirty"] <= 0:
                    continue
                self.sink["progress"] = min(
                    1.0, self.sink["progress"] + dt / float(self.wash_ms))
                if self.sink["progress"] >= 1.0:
                    self.sink["dirty"] = max(0, self.sink["dirty"] - 1)
                    self.plates.append(Item("equipment_plate_01", container=True))
                    self.sink["progress"] = 1.0
                    self.sink["done_ms"] = self.game_ms
                    self.cook_events.append(("wash", "sink", len(self.plates)))

    def _advance_cookers(self, game_ms):
        for sid, state in self.cooker_state.items():
            if state["utensil"] is None or state["item"] is None:
                continue
            if state["state"] == "cooking":
                if game_ms - state["started_ms"] >= self.cook_ms:
                    state["state"] = "cooked"
                    # 熟了的叶子在 contents[] 里带 cooked: 前缀（协议 §3.1）
                    state["item"].cooked = True
                    self.cook_events.append(("cooked", sid, state["item"].name))
            elif state["state"] == "cooked":
                if game_ms - state["started_ms"] >= self.cook_ms + self.burn_ms:
                    state["state"] = "burning"
                    self.cook_events.append(("burning", sid, state["item"].name))

    def _advance_orders(self, game_ms):
        remaining = []
        for order in self.active_orders:
            if game_ms >= order["deadline_ms"]:
                self.expired.append(dict(order))
                self.score -= self.expire_penalty
                self._record_result(order, "expired", game_ms)
            else:
                remaining.append(order)
        if game_ms >= self.round_ms:
            self.order_queue = []
        if len(remaining) != len(self.active_orders):
            self.active_orders = remaining
            self._release_next(game_ms)
        self._fill_orders(game_ms)

    def _record_result(self, order, outcome, game_ms):
        """order_results[] 的环形快照（旧→新，插件每帧重发，消费者按 seq 去重）。"""
        self.order_result_seq += 1
        entry = {"seq": self.order_result_seq, "id": order.get("id", ""),
                 "recipe": order.get("recipe"), "outcome": outcome,
                 "game_time_ms": int(game_ms)}
        if outcome == "delivered":
            entry["score"] = int(order.get("reward", 0))
        self.order_results.append(entry)
        self.order_results = self.order_results[-16:]

    def _advance_pass(self, game_ms):
        due = [entry for entry in self.pass_pending if entry["due_ms"] <= game_ms]
        self.pass_pending = [entry for entry in self.pass_pending
                             if entry["due_ms"] > game_ms]
        for entry in due:
            dish = entry["dish"]
            matched = None
            for order in self.active_orders:
                if order["recipe"] == dish:
                    matched = order
                    break
            if matched is not None:
                self.score += matched["reward"]
                self.active_orders.remove(matched)
                self.delivered.append({"dish": dish, "order_id": matched["id"],
                                       "reward": matched["reward"]})
                self._record_result(matched, "delivered", game_ms)
                self._release_next(game_ms)
            else:
                self.delivered_expired.append({"dish": dish,
                                               "order_ids": [o["id"] for o in
                                                             self.active_orders]})
                self._record_result({"id": "", "recipe": dish}, "failed", game_ms)
            self.sink["dirty"] += 1

    # -- 朝向与目标 --
    def station_pos(self, station_id):
        station = self.stations.get(station_id)
        if station is None:
            return None
        pos = station.get("pos") or [0.0, 0.0]
        return float(pos[0]), float(pos[1])

    def _face(self, chef, action, game_ms):
        target = action.get("target") or {}
        station = action.get("target_object") or ""
        if isinstance(target.get("x"), (int, float)) and isinstance(target.get("y"), (int, float)):
            tx, ty = float(target["x"]), float(target["y"])
        else:
            pos = self.station_pos(station)
            if pos is None:
                return
            tx, ty = pos
        x, y = self.positions[chef]
        wanted = _game_facing(tx - x, ty - y)
        if self.face_targets[chef] != station:
            diff = _angle_diff(wanted, self.facings[chef])
            if self.turn_drift > 0 and diff > math.radians(45.0):
                # 转身时被推离格心（实测转身漂出 0.3 m，navd 的 0.3 m 到站圈翻回
                # moving）。方向取目标方向的左手边，站位半径 1.2 m，横向 0.3 m
                # 之后仍在交互锥内。[推断：漂移方向]
                side = math.pi / 2.0 if (chef % 2 == 0) else -math.pi / 2.0
                plane = math.atan2(ty - y, tx - x)
                dx = math.cos(plane + side) * self.turn_drift
                dy = math.sin(plane + side) * self.turn_drift
                self.positions[chef] = [x + dx, y + dy]
                self.drift_events.append({"chef": chef, "at_ms": game_ms,
                                          "dx": round(dx, 3), "dy": round(dy, 3)})
                x, y = self.positions[chef]
                wanted = _game_facing(tx - x, ty - y)
        self.face_targets[chef] = station or None
        self.facings[chef] = wanted

    def _candidates(self, chef):
        """(station, distance, angle) 在交互半径内、按夹角排序。"""
        x, y = self.positions[chef]
        facing = self.facings[chef]
        out = []
        for station in self.stations.values():
            pos = station.get("pos")
            if not pos:
                continue
            dx = float(pos[0]) - x
            dy = float(pos[1]) - y
            distance = math.hypot(dx, dy)
            if distance > PRESS_REACH_M:
                continue
            angle = _angle_diff(_game_facing(dx, dy), facing)
            out.append((station, distance, angle))
        out.sort(key=lambda entry: (entry[2], entry[1]))
        return out

    def _resolve(self, chef, wanted_id, cone_deg):
        """交互目标：默认信动作里的 target_object，但它必须在交互半径与前锥内。

        grid_offset 非 0 时模拟 `_3` 网格原点错半格：按"面前那一格"重新选工位，
        面前点偏移后最近的两个工位并列时取 +x 的那个（实测拿到邻格箱子）。
        """
        x, y = self.positions[chef]
        cone = math.radians(cone_deg)
        if self.grid_offset != (0.0, 0.0):
            # 网格原点错半格：游戏按"偏移后的那一格"找物体，而不是按插件以为的工位。
            base = self.station_pos(wanted_id) if wanted_id else None
            if base is None:
                return None
            fx = base[0] + self.grid_offset[0]
            fy = base[1] + self.grid_offset[1]
            best = None
            best_key = None
            for station in self.stations.values():
                pos = station.get("pos")
                if not pos:
                    continue
                distance = math.hypot(float(pos[0]) - fx, float(pos[1]) - fy)
                if distance > CELL_M * 1.2:
                    continue
                key = (round(distance, 6), -float(pos[0]))
                if best_key is None or key < best_key:
                    best, best_key = station, key
            return best or self.stations.get(wanted_id)
        best = None
        for station, distance, angle in self._candidates(chef):
            if angle > cone:
                continue
            if station["id"] == wanted_id:
                return station
            if best is None:
                best = station
        return best

    # -- 交互意图 → 按键沿 --
    def _interact_intent(self, chef, target, game_ms):
        state = self.press_state[chef]
        if not state["active"] or (game_ms - self.last_action_ms[chef] > INTENT_GAP_MS) \
                or (target and target != state["target"]):
            state.update({"active": True, "start_ms": game_ms, "target": target,
                          "pressed": 1 if self.interact_pulses else 2,
                          "entry_holding": self.holding_name(chef),
                          "skipped_second": False})
            self.last_action_ms[chef] = game_ms
            self._press(chef, target, game_ms, second=False)
            return
        self.last_action_ms[chef] = game_ms
        elapsed = game_ms - state["start_ms"]
        if state["pressed"] < 2 and elapsed >= RETRY_MS:
            station = self._resolve(chef, target, PRESS_CONE_DEG)
            st_kind = station.get("kind") if station else ""
            # 盘堆特殊规则：手上有菜按盘堆放下、手空按盘堆取盘。
            # 第一下放下后手空（entry_holding 非空且当前为空），第二下是取盘意图而不是放菜重试。
            is_plate_take = (st_kind in ("plate_stack", "clean_plate_stack")
                             and state["entry_holding"] and not self.holding_name(chef))
            if self.retry_guard and self.holding_name(chef) != state["entry_holding"] and not is_plate_take:
                # ActionDriver：第一下已经生效（手上的东西变了）就不再补第二下，
                # 否则补按会把刚放下的东西拿回手上（实测 `_3` 拿起→放回→再拿起）。
                state["skipped_second"] = True
                state["pressed"] = 2
                self.presses.append({"chef": chef, "at_ms": game_ms,
                                     "target": target, "second": True,
                                     "station": state["target"],
                                     "holding_before": self.holding_desc(chef),
                                     "effect": "skipped-second"})
            else:
                self._press(chef, target, game_ms, second=True)
                state["pressed"] = 2

    def _work_intent(self, chef, target, game_ms):
        # 加工键不是交互键：它不结束上一次交互意图，也不影响下一次按键的间隔判定。
        station = self._resolve(chef, target, WORK_CONE_DEG)
        if station is None:
            return
        if station.get("kind") == "sink" and self.sink["progress"] >= 1.0 \
                and game_ms - self.sink["done_ms"] > 300:
            # 上一只盘已经洗完、按键也松开了：这次按住是洗下一只。
            self.sink["progress"] = 0.0
        self.working[chef] = True
        self.work_state[chef]["station"] = station

    # -- 按键效果 --
    def holding_name(self, chef):
        return self.holdings[chef].name if self.holdings[chef] is not None else ""

    def holding_desc(self, chef):
        item = self.holdings[chef]
        return item.describe() if item is not None else ""

    def _press(self, chef, target, game_ms, *, second):
        station = self._resolve(chef, target, PRESS_CONE_DEG)
        entry = {"chef": chef, "at_ms": game_ms, "target": target, "second": second,
                 "station": station["id"] if station else None,
                 "holding_before": self.holding_desc(chef)}
        self.presses.append(entry)
        if station is None:
            entry["effect"] = "no-target"
            return
        effect = self._apply_press(chef, station, game_ms)
        entry["effect"] = effect
        entry["holding_after"] = self.holding_desc(chef)
        if effect == "rejected":
            self.rejected.append({"chef": chef, "station": station["id"],
                                  "holding": entry["holding_before"],
                                  "at_ms": game_ms, "reason": self._reject_reason})

    def _apply_press(self, chef, station, game_ms):
        kind = station.get("kind")
        if kind == "dispenser":
            return self._press_dispenser(chef, station)
        if kind == "board":
            return self._press_board(chef, station)
        if kind == "cooker":
            return self._press_cooker(chef, station, game_ms)
        if kind == "counter":
            return self._press_counter(chef, station, game_ms)
        if kind in ("plate_stack", "clean_plate_stack"):
            return self._press_plate_stack(chef, station)
        if kind == "sink":
            return self._press_sink(chef, station)
        if kind == "serving":
            return self._press_serving(chef, station, game_ms)
        return "ignored"

    def _press_dispenser(self, chef, station):
        gives = station.get("gives") or ""
        if not gives:
            return "ignored"
        held = self.holdings[chef]
        if held is None:
            self.holdings[chef] = _make_item(gives)
            return "took"
        if self.dispenser_swap:
            # 手持物放到箱子里换一件同款（实机料箱行为 [推断]）
            self.holdings[chef] = _make_item(gives)
            return "swapped"
        return "ignored"

    def _press_board(self, chef, station):
        state = self.board_state[station["id"]]
        held = self.holdings[chef]
        if held is None:
            if state["item"] is None:
                return "rejected"
            self.holdings[chef] = state["item"]
            state["item"] = None
            state["progress"] = 0.0
            return "picked"
        if state["item"] is not None:
            self._reject_reason = "board-occupied"
            return "rejected"
        rules = INGREDIENTS.get(held.name, {})
        if not rules.get("workable") or held.container:
            self._reject_reason = "not-workable"
            return "rejected"
        state["item"] = held
        state["progress"] = 0.0
        self.holdings[chef] = None
        return "placed"

    def _press_cooker(self, chef, station, game_ms):
        state = self.cooker_state[station["id"]]
        held = self.holdings[chef]
        if held is None:
            if state["utensil"] is None or state["item"] is None:
                return "rejected"
            # 端锅：锅（带料）进手里，灶上不再有这口锅
            self.holdings[chef] = Item(state["utensil"],
                                       contents=[state["item"]],
                                       cooked=state["state"] in ("cooked", "burning"),
                                       utensil=True, step=state["step"])
            state["utensil"] = None
            state["item"] = None
            state["state"] = "free"
            return "picked-pot"
        if held.utensil:
            if state["utensil"] is not None:
                self._reject_reason = "stove-occupied"
                return "rejected"
            state["utensil"] = held.name
            state["item"] = held.contents[0].copy() if held.contents else None
            state["state"] = "cooked" if held.cooked and held.contents else (
                "cooking" if held.contents else "free")
            state["started_ms"] = game_ms
            self.holdings[chef] = None
            return "placed-pot"
        rules = COOK_RULES.get(held.name)
        if rules is None:
            self._reject_reason = "not-cookable"
            return "rejected"
        if state["utensil"] is None:
            self._reject_reason = "no-utensil"
            return "rejected"
        if state["item"] is not None:
            self._reject_reason = "pot-occupied"
            return "rejected"
        if state["step"] not in rules.get("cook_steps", ()):
            # CookableContainer.AllowItemPlacement：锅的 CookingHandler 类型必须在
            # 料的 AllowedCookingSteps 里；放错类型游戏不收（米进炸锅）。
            self._reject_reason = "wrong-cooker-type"
            return "rejected"
        # 实测：切好的料一下锅，锅内容就报订单里的生料名（上面 ORDER_NAMES 的注释）。
        state["item"] = Item(ORDER_NAMES.get(held.name, held.name), cooked=held.cooked)
        state["state"] = "cooking"
        state["started_ms"] = game_ms
        self.holdings[chef] = None
        return "started-cooking"

    def _press_counter(self, chef, station, game_ms):
        state = self.counter_state[station["id"]]
        held = self.holdings[chef]
        if held is None:
            if state["item"] is None:
                return "rejected"
            self.holdings[chef] = state["item"]
            state["item"] = None
            return "picked"
        current = state["item"]
        if current is not None and current.container and held.contents:
            # 倒料：锅里的料进玉米饼，锅仍留在手上（实测）
            for item in held.contents:
                item.cooked = True
                current.contents.append(item)
            held.contents = []
            self.pour_events.append({"chef": chef, "station": station["id"],
                                     "into": current.name,
                                     "contents": [i.name for i in current.contents]})
            return "poured"
        if current is not None:
            self._reject_reason = "counter-occupied"
            return "rejected"
        if held.container and held.contents and held.utensil:
            self._reject_reason = "needs-container"
            return "rejected"
        state["item"] = held
        self.holdings[chef] = None
        return "placed"

    def _press_plate_stack(self, chef, station):
        held = self.holdings[chef]
        if held is None:
            if not self.plates:
                return "rejected"
            plate = self.plates.pop()
            self.holdings[chef] = plate
            return "took-plate"
        if held.container and held.name.startswith("equipment_plate"):
            self._reject_reason = "stack-only-food"
            return "rejected"
        if not self.plates:
            self._reject_reason = "stack-empty"
            return "rejected"
        if not self._placeable_on_plate(held):
            self._reject_reason = "plate-rejects-item"
            return "rejected"
        top = self.plates[-1]
        if held.container:
            top.contents.extend(held.contents)
        else:
            top.contents.append(held)
        self.holdings[chef] = None
        return "stacked-%s" % match_dish(top.contents)

    def _placeable_on_plate(self, item):
        """Plate.CanPlaceOnPlate：生料没有订单定义，放不上去（实测生鱼按盘子没反应）。"""
        if item.container:
            return bool(item.contents)
        return item.cooked or item.name in CHOPPED_NAMES

    def _press_sink(self, chef, station):
        held = self.holdings[chef]
        if held is None:
            # 空手按水槽：池子里有脏盘就开始洗（实测 wash 是站上去按住加工键）
            return "started-wash" if self.sink["dirty"] > 0 else "ignored"
        if not held.name.startswith("equipment_plate"):
            self._reject_reason = "sink-takes-plates"
            return "rejected"
        self.holdings[chef] = None
        self.sink["dirty"] += 1
        return "dropped-dirty"

    def _press_serving(self, chef, station, game_ms):
        held = self.holdings[chef]
        if held is None or not held.name.startswith("equipment_plate"):
            self._reject_reason = "pass-takes-plates"
            return "rejected"
        dish = match_dish(held.contents)
        if dish is None:
            self._reject_reason = "not-a-dish"
            return "rejected"
        self.holdings[chef] = None
        self.pass_pending.append({"due_ms": game_ms + PASS_CLEAR_MS, "dish": dish})
        return "served-%s" % dish

    # -- 上报（protocol/schema/state_update.schema.json @c3cd157） --
    def leaves(self, items):
        """contents[] 的编码：递归展开到叶子，熟叶写 cooked:<name>（G3/G4）。"""
        out = []
        for item in items:
            if item.container:
                out.extend(self.leaves(item.contents))
            else:
                out.append(("cooked:" if item.cooked else "") + item.name)
        return out[:8]

    @staticmethod
    def _pos(entry):
        pos = entry.get("pos") or [0.0, 0.0]
        return {"x": round(float(pos[0]), 3), "y": round(float(pos[1]), 3)}

    def resources(self):
        """Dynamic resource keys, exactly the shapes in protocol.md §3.1.

        修好后的上报（默认）：
          * pot_<N> 是灶台槽位 N（x、z 排序，与 cooker-N 一一对应），空灶台也发、
            状态 empty（G1）；
          * plate_stack.count 来自干净盘数（G2）；
          * counter_<N> 只发有东西的台面，编号不稳定，消费者按 pos 找；
          * carried_<chef> 发手上端着的锅/盘子/容器，带 chef_id；
          * 每个资源带 kind/pos/item/contents/content/step/progress/time_left。
        实机缺陷开关 legacy_pot_keys=True 回到旧行为：只有有内容的锅、键按枚举下标、
        状态写 free。
        """
        out = {}
        boards = sorted((self.stations[sid] for sid in self.board_state),
                        key=lambda s: ((s.get("pos") or [0, 0])[0],
                                       (s.get("pos") or [0, 0])[1], s.get("name", "")))
        for index, station in enumerate(boards):
            state = self.board_state[station["id"]]
            payload = {"kind": "board", "pos": self._pos(station),
                       "state": "occupied" if state["item"] else "free",
                       "progress": round(state["progress"], 3)}
            if state["item"] is not None:
                contents = self.leaves([state["item"]])
                payload["item"] = state["item"].name
                payload["contents"] = contents
                payload["content"] = "+".join(contents)
            else:
                payload["contents"] = []
            out["cutting_board_%d" % index] = payload

        cookers = sorted((self.stations[sid] for sid in self.cooker_state),
                         key=lambda s: ((s.get("pos") or [0, 0])[0],
                                        (s.get("pos") or [0, 0])[1], s.get("name", "")))
        reported = 0
        for index, station in enumerate(cookers):
            state = self.cooker_state[station["id"]]
            if state["utensil"] is None:
                # 锅被端走时这个灶台上没有 ClientCookableContainer，不上报；
                # 那只锅在 carried_<chef> 里。
                continue
            payload = {"kind": "cooker", "pos": self._pos(station),
                       "item": state["utensil"]}
            if state["item"] is not None:
                payload["state"] = state["state"]
                contents = self.leaves([state["item"]])
                payload["contents"] = contents
                payload["content"] = "+".join(contents)
                elapsed = max(0, self.game_ms - state["started_ms"])
                payload["progress"] = round(min(1.0, elapsed / float(self.cook_ms)), 3)
                if state["state"] == "cooking":
                    payload["time_left"] = round(
                        max(0.0, (self.cook_ms - elapsed) / 1000.0), 3)
                elif state["state"] == "cooked":
                    payload["time_left"] = round(
                        max(0.0, (self.cook_ms + self.burn_ms - elapsed) / 1000.0), 3)
                else:
                    payload["time_left"] = 0.0
            else:
                payload["state"] = "empty"
                payload["contents"] = []
                payload["content"] = ""
                payload["progress"] = 0.0
                payload["time_left"] = 0.0
            if payload.get("state") != "empty":
                payload["step"] = state["step"]
            if self.legacy_pot_keys and not payload["content"]:
                continue
            # 旧行为（G1）：键是"列出锅具的枚举下标"，空锅不在列表里，编号整体前移。
            key_index = reported if self.legacy_pot_keys else index
            reported += 1
            out["pot_%d" % key_index] = payload

        occupied_counters = [station for station in self.stations.values()
                             if station.get("kind") == "counter"
                             and self.counter_state[station["id"]]["item"] is not None]
        occupied_counters.sort(key=lambda s: ((s.get("pos") or [0, 0])[0],
                                              (s.get("pos") or [0, 0])[1],
                                              s.get("name", "")))
        for index, station in enumerate(occupied_counters):
            item = self.counter_state[station["id"]]["item"]
            contents = self.leaves([item])
            out["counter_%d" % index] = {
                "kind": "counter", "pos": self._pos(station),
                "state": "occupied", "item": item.name,
                "contents": contents, "content": "+".join(contents)}

        for chef in range(self.chef_count):
            item = self.holdings[chef]
            if item is None or not (item.container or item.utensil or
                                    item.name.startswith("utensil_")):
                continue
            contents = self.leaves(item.contents) if item.contents else []
            out["carried_%d" % chef] = {
                "kind": "carried", "pos": {"x": round(self.positions[chef][0], 3),
                                           "y": round(self.positions[chef][1], 3)},
                "state": "carried", "item": item.name, "chef_id": chef,
                "contents": contents, "content": "+".join(contents)}

        out["sink_0"] = {"kind": "sink", "pos": self._pos(self.stations["sink-0"])
                         if "sink-0" in self.stations else {"x": 0.0, "y": 0.0},
                         "state": "washing" if self.sink["progress"] > 0 else "empty",
                         "count": self.sink["dirty"],
                         "progress": round(self.sink["progress"], 3)}
        out["dirty_plates"] = {
            "kind": "dirty_plates", "count": self.sink["dirty"],
            "pos": self._pos(self.stations["sink-0"])
            if "sink-0" in self.stations else {"x": 0.0, "y": 0.0}}
        if self.plate_stack_reported:
            stack = self.stations.get("plate_stack-0")
            out["plate_stack"] = {"kind": "plate_stack",
                                  "pos": self._pos(stack) if stack else {"x": 0.0, "y": 0.0},
                                  "state": "occupied" if self.plates else "empty",
                                  "count": len(self.plates)}
        return out

    def message(self, chefs, frame_index, cells, game_ms):
        holdings = []
        for index in range(chefs):
            item = self.holdings[index]
            chef = {"id": index,
                    "position": {"x": round(self.positions[index][0], 3),
                                 "y": round(self.positions[index][1], 3)},
                    "facing": self.facings[index],
                    "holding": item.name if item is not None else None,
                    "state": "idle"}
            if item is not None and item.contents:
                leaves = self.leaves(item.contents)
                dish = match_dish(item.contents)
                # 调度器会把手里已经成菜的产品归并到 plate/pickup 节点（item_adopted）；
                # 认它靠菜名或 plate 节点 label，所以手持内容里带上成菜名。
                chef["holding_contents"] = ([dish] + leaves) if dish else leaves
            holdings.append(chef)
        orders = []
        for order in self.active_orders:
            time_left = max(0, int(math.ceil(
                (order["deadline_ms"] - game_ms) / 1000.0)))
            orders.append({"id": order["id"], "recipe": order["recipe"],
                           "time_remaining": time_left,
                           "score": int(order["reward"]), "ingredients": []})
        return json.dumps({
            "type": "state_update",
            "timestamp": int(time.time()),
            "game_time": int((game_ms or 0) // 1000),
            "game_time_ms": int(game_ms or 0),
            "score": int(self.score),
            "round_time_left": round(max(0.0, (self.round_ms - game_ms) / 1000.0), 3),
            "round_time_limit": round(self.round_ms / 1000.0, 3),
            "chefs": holdings,
            "orders": orders,
            "order_results": [dict(entry) for entry in self.order_results],
            "resources": self.resources(),
        }, separators=(",", ":"), ensure_ascii=False)

    # -- 观测 --
    def summary(self):
        return {
            "score": self.score,
            "delivered": [entry["dish"] for entry in self.delivered],
            "delivered_expired": [entry["dish"] for entry in self.delivered_expired],
            "expired": [entry.get("dish") or entry.get("recipe")
                        for entry in self.expired],
            "plates": len(self.plates),
            "dirty": self.sink["dirty"],
            "presses": len(self.presses),
            "rejected": len(self.rejected),
            "drift": len(self.drift_events),
            "pour": len(self.pour_events),
        }


if __name__ == "__main__":
    class _DummyMotion:
        def __init__(self, positions):
            self.positions = positions

        def process_actions(self, actions, game_ms):
            pass

        def advance(self, game_ms):
            pass

    rules = Demo3Rules()
    motion = _DummyMotion([[3.0, 1.2]])
    kitchen = demo3_kitchen(clean_plates=5)
    world = Demo3World(motion, kitchen["stations"], rules=rules)
    world.holdings[0] = Item("ChoppedSushiFish")
    ps = world.stations["plate_stack-0"]
    world.positions[0] = list(ps["pos"])
    world.facings[0] = 0.0
    actions = []
    for t in range(0, 640, 16):
        actions.append({"chef_id": 0, "action": "interact", "target_object": "plate_stack-0"})
        world.process_actions(actions, t)
    effects = [(p["at_ms"], p["second"], p["effect"]) for p in world.presses]
    assert len(effects) == 2 and effects[0][2] == "stacked-Sushi_PlainFish" and effects[1][2] == "took-plate", \
        "Expected drop then take on plate_stack, got %s" % effects
    assert world.holding_name(0) == "equipment_plate_01", \
        "Expected chef to hold plate, got %s" % world.holding_name(0)
    print("demo3_sim selftest PASS: plate_stack places then takes (effects=%s)" % effects)
