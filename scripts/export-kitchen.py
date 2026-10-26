#!/usr/bin/env python3
"""把插件导出的世界状态与地形合成 OS 侧要读的 `config/kitchen.json`。

输入（游戏目录 `BepInEx/` 下，插件每局写一次）：
  - `wellcooked-state.json`：工位（`stations[]`，带 `type`/`name`/`pos`/`height`/`gives`/`contents`）
  - `wellcooked-terrain.json`：0.6 米物理采样的可走格（`cells`）与每格的可通行邻居掩码（`mask`）

输出：一个文件说清"这个厨房有什么资源、地面怎么走"——Scheduler 和模拟器只读它，不再解析插件的原始 JSON。
方案 §6.6 把 `config/kitchen.json` 定为 OS 侧的输入；为什么不用外部数据集见 `.workflow/maps/prior-art.md`。

`--map-dir DIR` 按场景名写到 `DIR/<scene>/kitchen.json` 与 `recipes.json`（四张自制地图的
固定位置，Scheduler / Worker 的 `--kitchen` / `--recipes` 直接指过去）；不给时仍写
`config/kitchen.json` + `config/recipes.json`。

工位去重与分类：
  - 同一个物体上常挂多个组件（`equipment_plate_01` 同时是 `ClientIngredientContainer` 与 `ClientPlate`），
    按**物体名**归一个工位，`types` 保留全部组件类型；
  - `kind` 取该物体上优先级最高的那个组件的语义（出菜口 > 料箱 > 锅 > 洗盘 > 盘子堆 > 容器能力）；
  - `id` 是 `kind-序号`，排序后生成，所以同一场景两次导出得到的 id 一样。

连通区按 `mask` 算（物理可通行）。插件运行期学到的"这条边过不去"不在导出里，属于一局之内的临时状态。
"""

import argparse
import json
import os
import sys
import time

GAME_DIR = os.environ.get(
    "GAME_DIR", "/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2"
)
DEFAULT_STATE = os.path.join(GAME_DIR, "BepInEx", "wellcooked-state.json")
DEFAULT_TERRAIN = os.path.join(GAME_DIR, "BepInEx", "wellcooked-terrain.json")
DEFAULT_OUT = "config/kitchen.json"
DEFAULT_MENU = os.path.join(GAME_DIR, "BepInEx", "wellcooked-menu.json")
DEFAULT_OUT_RECIPES = "config/recipes.json"

# 组件类型 → 资源语义。优先级从上到下：一个物体上挂多个组件时取最上面那个当 kind。
KINDS = [
    ("serving", ["ClientPlateStation"]),          # 出菜口：游戏的交付就发生在 ClientPlateStation 上
    ("dispenser", ["ClientPickupItemSpawner"]),   # 料箱（DispenserCrate）
    ("bin", ["ClientRubbishBin"]),                # 垃圾桶：挑工位必须排除
    ("cooker", ["ClientCookingStation"]),         # 锅/灶
    ("mixer", ["ClientMixingStation"]),           # 搅拌台
    ("sink", ["ClientWashingStation"]),           # 洗盘台
    ("heater", ["ClientHeatedStation"]),          # 加热台
    ("clean_plate_stack", ["ClientCleanPlateStack"]),
    ("dirty_plate_stack", ["ClientDirtyPlateStack"]),
    ("plate_stack", ["ClientPlate"]),             # 盘子本体（通常在盘子堆/台面上）
    ("conveyor", ["ClientConveyorStation"]),      # 传送带（挑关卡时排除）
    ("switch", ["ClientSwitchStation"]),          # 开关（挑关卡时排除）
    # 以下只是"这个物体能装东西"，单独出现时归 container
    ("container", [
        "ClientIngredientContainer",
        "ClientCookableContainer",
        "ClientCookablePreparationContainer",
        "ClientTrayIngredientContainer",
    ]),
]
KIND_ORDER = [kind for kind, _ in KINDS]
TYPE_TO_KIND = {t: kind for kind, types in KINDS for t in types}
CONTAINER_TYPES = set(dict(KINDS)["container"])

POS_TOLERANCE = 0.15  # 同一物体上的组件坐标应当一致；差得比这多就当成两个物体，别硬合并


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def group_stations(raw):
    """按物体名归组；名字相同但坐标差得远的（不该出现）单独成组。"""
    groups = {}
    order = []
    for station in raw:
        name = station.get("name") or "?"
        pos = tuple(round(v, 2) for v in station.get("pos", [0.0, 0.0]))
        key = None
        for candidate in groups:
            if candidate[0] != name:
                continue
            other = groups[candidate]["pos"]
            if abs(other[0] - pos[0]) <= POS_TOLERANCE and abs(other[1] - pos[1]) <= POS_TOLERANCE:
                key = candidate
                break
        if key is None:
            key = (name, pos)
            groups[key] = {"name": name, "pos": pos, "height": station.get("height", 0.0),
                           "types": [], "gives": None, "contents": [], "prefab_components": [],
                           "step": None, "cook_steps": []}
            order.append(key)
        group = groups[key]
        station_type = station.get("type")
        if station_type and station_type not in group["types"]:
            group["types"].append(station_type)
        if station.get("gives") and not group["gives"]:
            group["gives"] = station["gives"]
        if station.get("contents") and not group["contents"]:
            group["contents"] = station["contents"]
        for component in station.get("prefab_components") or []:
            if component not in group["prefab_components"]:
                group["prefab_components"].append(component)
        if station.get("step") and not group["step"]:
            group["step"] = station["step"]
        for step in station.get("cook_steps") or []:
            if step not in group["cook_steps"]:
                group["cook_steps"].append(step)
    return [groups[key] for key in order]


def kind_of(types):
    for kind in KIND_ORDER:
        if any(t in TYPE_TO_KIND and TYPE_TO_KIND[t] == kind for t in types):
            return kind
    return "unknown"


def snap_near_grid(value, origin, step):
    """Remove tiny transform noise when a station lies on a sampled grid coordinate."""
    if step <= 0 or origin is None:
        return round(value, 2)
    aligned = origin + round((value - origin) / step) * step
    return round(aligned if abs(value - aligned) <= 0.02 else value, 2)


def build_stations(raw, grid_step, grid_origin):
    merged = []
    unknown = []
    for group in group_stations(raw):
        kind = kind_of(group["types"])
        for station_type in group["types"]:
            if station_type not in TYPE_TO_KIND and station_type not in unknown:
                unknown.append(station_type)
        station = {
            "kind": kind,
            "name": group["name"],
            "types": group["types"],
            "pos": [snap_near_grid(group["pos"][0], grid_origin[0], grid_step),
                    snap_near_grid(group["pos"][1], grid_origin[1], grid_step)],
            "height": round(float(group["height"]), 2),
            "gives": group["gives"],
            "contents": group["contents"],
        }
        if group["prefab_components"]:
            # 料箱才有：这个料的 prefab 上挂了哪些组件（`WorkableItem` = 能加工）
            station["prefab_components"] = group["prefab_components"]
        if group["cook_steps"]:
            # 料箱才有：这个料能进哪种烹饪（CookableProperties.AllowedCookingSteps）
            station["cook_steps"] = group["cook_steps"]
        if group["step"]:
            # 锅类容器才有：它做哪种烹饪（CookingHandler.m_cookingType）
            station["step"] = group["step"]
        merged.append(station)
    # 灶台本身不带烹饪类型，类型在放在它上面的锅/炸篮上（同一格、不同物体）。
    # 游戏只让 AllowedCookingSteps 含这一步的料放进锅（CookableContainer.AllowItemPlacement），
    # 所以 cooker 要带上它那口锅的 step，Scheduler 按 step 挑灶台（实机 `_3`：米被派到炸锅放不进去）。
    for station in merged:
        if station["kind"] != "cooker" or station.get("step"):
            continue
        for other in merged:
            if other.get("step") and other is not station and \
                    abs(other["pos"][0] - station["pos"][0]) <= POS_TOLERANCE and \
                    abs(other["pos"][1] - station["pos"][1]) <= POS_TOLERANCE:
                station["step"] = other["step"]
                break
    merged.sort(key=lambda s: (KIND_ORDER.index(s["kind"]) if s["kind"] in KIND_ORDER else 99,
                               s["pos"][0], s["pos"][1], s["name"]))
    seen = {}
    for station in merged:
        index = seen.get(station["kind"], 0)
        seen[station["kind"]] = index + 1
        station["id"] = "%s-%d" % (station["kind"], index)
    return merged, unknown


def grid_index(terrain):
    """cells → 网格下标；掩码的位含义由导出的 edge_bits 说明。"""
    step = terrain["step"]
    cells = terrain["cells"]
    if not cells:
        return step, [], {}, []
    min_x = min(x for x, _ in cells)
    min_z = min(z for _, z in cells)
    indices = []
    lookup = {}
    for order, (x, z) in enumerate(cells):
        i = int(round((x - min_x) / step))
        j = int(round((z - min_z) / step))
        indices.append((i, j))
        lookup[(i, j)] = order
    return step, indices, lookup, cells


def areas_of(cells, mask, lookup):
    """按 mask 求连通区（并查集）。"""
    parent = list(range(len(cells)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    for order, (i, j) in enumerate(cells):
        bits = mask[order] if order < len(mask) else 0
        for bit, (di, dj) in ((1, (1, 0)), (2, (-1, 0)), (4, (0, 1)), (8, (0, -1))):
            if not bits & bit:
                continue
            other = lookup.get((i + di, j + dj))
            if other is not None:
                union(order, other)

    sizes = {}
    for order in range(len(cells)):
        root = find(order)
        sizes[root] = sizes.get(root, 0) + 1
    return sorted(sizes.values(), reverse=True)


# 菜谱节点类型 → 工序。类型名来自游戏自己的菜谱节点（`Cooked*` 要加热、`Mixed*` 要搅拌）。
OP_BY_NODE_TYPE = {
    "IngredientOrderNode": "fetch",
    "ItemOrderNode": "fetch",
    "WildcardOrderNode": "fetch",
    "CookedCompositeOrderNode": "cook",
    "MixedCompositeOrderNode": "mix",
    "CompositeOrderNode": "assemble",
    "CacheAssembledOrderNode": "assemble",
}

# 工序 → 用哪种工位（与 kitchen.json 的 `kind` 对齐）
RESOURCE_BY_OP = {
    "fetch": "dispenser",
    "cook": "cooker",
    "mix": "mixer",
    "assemble": "container",
    "plate": "plate_stack",
    "deliver": "serving",
}

# 工时：没有实测值，先给方案 §6.5 的默认值，并在文件里写清来源。
DURATION_BY_OP = {"fetch": 1.0, "cook": 5.0, "mix": 5.0, "assemble": 1.0, "plate": 2.0, "deliver": 1.0}
DURATION_SOURCE = "plan-§6.5-default（未实测；任务 12 拿到真实进度后再改）"

# 工序的先后顺序：只在拓扑排序并列时用来定序（写进文件时便于人读）
OP_ORDER = ["fetch", "cook", "mix", "assemble", "plate", "deliver"]


def chop_ingredients(state):
    """料 → 要不要切。

    菜谱树里看不出"要不要切"（树只写"由什么组成"），判据取料箱导出的 prefab 组件名：
    这个料能加工（`WorkableItem`）就要切。实测 `Fish`/`Prawn`/`Carrot`/`Meat` 有 `WorkableItem`、
    `Flour` 没有；任务 09 在游戏里实测过"鱼要切"，与之一致。
    """
    result = {}
    for station in state.get("stations") or []:
        gives = station.get("gives")
        if not gives:
            continue
        components = station.get("prefab_components") or []
        result[gives.lower()] = any(name == "WorkableItem" for name in components)
    return result


def match_ingredient(node_name, chop_flags):
    """菜谱叶子名（UncookedFish）对上料名（Fish）：按后缀/包含匹配，取最长命中。"""
    lower = node_name.lower()
    best = None
    for ingredient in chop_flags:
        if lower == ingredient or lower.endswith(ingredient) or ingredient in lower:
            if best is None or len(ingredient) > len(best):
                best = ingredient
    return best


def topo_order(keys, edges, nodes):
    """给定节点集合与"子 → 父"的边，排出可执行顺序；并列时按工序顺序、再按 id。"""
    pending = {key: 0 for key in keys}
    parents = {key: [] for key in keys}
    for child, parent in edges:
        if child in pending and parent in pending:
            parents[child].append(parent)
    # 统计每个节点还剩几个前置没排（用"父的入度"表示：父要等子）
    remaining = {key: 0 for key in keys}
    children_of = {key: [] for key in keys}
    for child, parent in edges:
        if child in pending and parent in pending:
            children_of[parent].append(child)
            remaining[parent] += 1
    ready = sorted([key for key in keys if remaining[key] == 0],
                   key=lambda k: (OP_ORDER.index(nodes[k]["op"]), k))
    out = []
    while ready:
        key = ready.pop(0)
        out.append(key)
        for parent in parents[key]:
            remaining[parent] -= 1
            if remaining[parent] == 0:
                ready.append(parent)
                ready.sort(key=lambda k: (OP_ORDER.index(nodes[k]["op"]), k))
    # 有环或漏排时兜底：把剩下的按工序顺序接上（不该发生）
    for key in sorted(keys - set(out), key=lambda k: (OP_ORDER.index(nodes[k]["op"]), k)):
        out.append(key)
    return out


def build_recipes(menu, state, station_kinds=None):
    """菜谱表 → 工序 DAG：节点按"工序 + 游戏里的节点名"去重，所以两道菜共用中间产物时只有一个节点。

    `station_kinds` 是 kitchen.json 里有的工位类型；有 `plate_stack` 时，子节点带装盘步骤
    （菜单里的 `plate`）的组合工序落在盘堆上：干净盘堆把放置转交给最上面那只盘子
    （`ServerCleanPlateStack.HandlePlacement`），料直接放进盘里。落到锅类容器上游戏会拒收
    （实机 `_3`：生虾对着平底锅按 interact 不放手）。"""
    station_kinds = set(station_kinds or ())
    chop_flags = chop_ingredients(state)
    nodes = {}
    edges = set()
    dishes = []

    def node_id(op, name):
        return "%s:%s" % (op, name)

    def add_node(op, name, **extra):
        key = node_id(op, name)
        if key not in nodes:
            node = {"id": key, "op": op, "label": name, "resource": RESOURCE_BY_OP[op],
                    "duration": DURATION_BY_OP[op]}
            node.update(extra)
            nodes[key] = node
        return key

    def walk(node):
        """返回这个菜谱节点对应的工序 id；子节点先建（前置依赖）。"""
        name = node.get("node") or "?"
        node_type = node.get("type") or ""
        op = OP_BY_NODE_TYPE.get(node_type, "assemble")
        raw_children = node.get("children") or []
        children = [walk(child) for child in raw_children]

        ingredient = match_ingredient(name, chop_flags) if op == "fetch" else None
        extra = {}
        if ingredient:
            extra["ingredient"] = ingredient
            # 能不能加工（`WorkableItem`）是事实；"这道菜要不要切"不是——菜谱树里没有这道工序，
            # 所以不替游戏做决定（早先按 WorkableItem 自动插 chop 节点会给出多余的工序）。
            extra["workable"] = bool(chop_flags.get(ingredient))
        if op == "assemble" and "plate_stack" in station_kinds and \
                any(child.get("plate") for child in raw_children):
            extra["resource"] = "plate_stack"
        if op == "cook" and node.get("cook"):
            # 这一步要哪种锅（CookedCompositeOrderNode.m_cookingStep）：与 cooker 的 `step` 对上才放得进
            extra["step"] = node["cook"]
        key = add_node(op, name, **extra)
        for child in children:
            edges.add((child, key))
        return key

    for dish in menu.get("dishes") or []:
        dish_name = dish.get("dish") or "?"
        roots = [walk(node) for node in (dish.get("steps") or [])]
        # 装盘 → 交付：游戏里必须先装盘再送到出菜口
        plate = add_node("plate", dish_name)
        deliver = add_node("deliver", dish_name)
        for root in roots:
            edges.add((root, plate))
        edges.add((plate, deliver))

        # 这道菜要做的工序：从根沿"子 → 父"边往回走，再拓扑排序（并列时按 OP_ORDER）
        seen = set()
        stack = list(roots)
        while stack:
            key = stack.pop()
            if key in seen or key not in nodes:
                continue
            seen.add(key)
            stack.extend([child for child, parent in edges if parent == key])
        seen.add(plate)      # 装盘、交付是根的后续（在边的另一头），别漏
        seen.add(deliver)
        dishes.append({
            "dish": dish_name,
            "score": dish.get("score"),
            "root": roots[0] if roots else None,
            "nodes": topo_order(seen, edges, nodes),
        })

    return {
        "nodes": [nodes[key] for key in sorted(nodes)],
        "edges": sorted([list(pair) for pair in edges]),
        "dishes": dishes,
        "durations": {"source": DURATION_SOURCE, "seconds": DURATION_BY_OP},
        "notes": [
            "节点 id = 工序:游戏里的节点名，两道菜共用同一中间产物时只有一个节点",
            "工时是方案 §6.5 的默认值，不是实测值",
            "菜谱树里没有\"切\"这道工序：`fetch` 节点带 `workable`（料箱 prefab 上有没有 WorkableItem）作为事实，",
            "要不要切由游戏决定，本文件不替它下结论（早先按 WorkableItem 自动插 chop 节点会给出多余的工序）",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--state", default=DEFAULT_STATE)
    parser.add_argument("--terrain", default=DEFAULT_TERRAIN)
    parser.add_argument("--out", default=None,
                        help="kitchen.json 输出路径（默认 %s）" % DEFAULT_OUT)
    parser.add_argument("--menu", default=DEFAULT_MENU)
    parser.add_argument("--out-recipes", default=None,
                        help="recipes.json 输出路径（默认 %s）" % DEFAULT_OUT_RECIPES)
    parser.add_argument("--map-dir", default=None,
                        help="按场景名写到 <DIR>/<scene>/{kitchen,recipes}.json；"
                             "与 --out/--out-recipes 互斥")
    args = parser.parse_args()
    if args.map_dir and (args.out or args.out_recipes):
        parser.error("--map-dir 不能和 --out/--out-recipes 一起用")

    for path in (args.state, args.terrain):
        if not os.path.exists(path):
            print("读不到 %s：先起游戏进一次关卡（插件会导出）" % path, file=sys.stderr)
            return 1

    state = load(args.state)
    terrain = load(args.terrain)
    scene = terrain.get("scene") or state.get("scene")
    if args.map_dir:
        if not scene:
            print("导出文件里没有场景名，没法按地图目录写出", file=sys.stderr)
            return 1
        args.out = os.path.join(args.map_dir, scene, "kitchen.json")
        args.out_recipes = os.path.join(args.map_dir, scene, "recipes.json")
    else:
        args.out = args.out or DEFAULT_OUT
        args.out_recipes = args.out_recipes or DEFAULT_OUT_RECIPES

    step, indices, lookup, cells = grid_index(terrain)
    grid_origin = (min((x for x, _ in cells), default=None),
                   min((z for _, z in cells), default=None))
    stations, unknown = build_stations(state.get("stations") or [], step, grid_origin)
    mask = terrain.get("mask") or []
    sizes = areas_of(indices, mask, lookup)

    kitchen = {
        "scene": scene,
        "generated": {
            "at": time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(os.path.getmtime(args.state))),
            "state_t": state.get("t"),
            "sources": [os.path.basename(args.state), os.path.basename(args.terrain)],
        },
        "grid": {
            "step": step,
            "map": terrain.get("map"),
            "edge_bits": terrain.get("edge_bits", "1:+x 2:-x 4:+z 8:-z"),
            "cells": [[round(x, 2), round(z, 2)] for x, z in cells],
            "mask": mask,
            "areas": {"count": len(sizes), "largest": sizes[0] if sizes else 0, "sizes": sizes[:10]},
            "note": "连通区按 mask 算（物理可通行）；运行期学到的封边不在导出里",
        },
        "stations": stations,
        "kinds": {kind: sum(1 for s in stations if s["kind"] == kind) for kind in KIND_ORDER
                  if any(s["kind"] == kind for s in stations)},
    }
    if unknown:
        kitchen["unknown_types"] = unknown

    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    # 顶层缩进便于人读，但 546 个格点各自占三行没有意义：先把两个大数组换成占位符，
    # 序列化之后再塞回紧凑形式。
    compact_cells = [[round(x, 2), round(z, 2)] for x, z in cells]
    kitchen["grid"]["cells"] = "__CELLS__"
    kitchen["grid"]["mask"] = "__MASK__"
    text = json.dumps(kitchen, ensure_ascii=False, indent=2)
    text = text.replace('"__CELLS__"', json.dumps(compact_cells, separators=(",", ":")))
    text = text.replace('"__MASK__"', json.dumps(mask, separators=(",", ":")))
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(text + "\n")

    print("场景 %s：原始工位 %d → 合并后 %d，可走格 %d，连通区 %d（最大 %d）"
          % (kitchen["scene"], len(state.get("stations") or []), len(stations),
             len(cells), len(sizes), sizes[0] if sizes else 0))
    for kind, count in kitchen["kinds"].items():
        print("  %-20s %d" % (kind, count))
    if unknown:
        print("  !! 没归类的组件类型：" + ", ".join(unknown))
    print("写到 %s" % args.out)

    # 菜谱 → 工序 DAG。菜单是插件用 `menu=1` 导出的（游戏自己的 RecipeList）。
    if not os.path.exists(args.menu):
        print("没有 %s：进关卡后发一次 menu=1 再跑（这次只写了 kitchen.json）" % args.menu,
              file=sys.stderr)
        return 0

    menu = load(args.menu)
    recipes = build_recipes(menu, state, kitchen["kinds"].keys())
    recipes["scene"] = menu.get("scene") or kitchen["scene"]
    recipes["generated"] = kitchen["generated"]
    station_kinds = set(kitchen["kinds"].keys())
    missing_kinds = {}
    for node in recipes["nodes"]:
        res = node.get("resource")
        if res and res not in station_kinds:
            missing_kinds.setdefault(res, []).append(node["id"])
    if missing_kinds:
        for res, node_ids in sorted(missing_kinds.items()):
            print("  !! 警告：工序节点缺少对应工位类型 %s，涉及节点：%s"
                  % (res, ", ".join(node_ids)), file=sys.stderr)
    with open(args.out_recipes, "w", encoding="utf-8") as handle:
        json.dump(recipes, handle, ensure_ascii=False, indent=2)
        handle.write("\n")

    shared = [n["id"] for n in recipes["nodes"]
              if sum(1 for d in recipes["dishes"] if n["id"] in d["nodes"]) > 1]
    print("菜谱 %d 道、工序节点 %d 个、边 %d 条，写到 %s"
          % (len(recipes["dishes"]), len(recipes["nodes"]), len(recipes["edges"]), args.out_recipes))
    for dish in recipes["dishes"]:
        print("  %-24s %s" % (dish["dish"], " → ".join(dish["nodes"])))
    if shared:
        print("  共用的中间产物：" + ", ".join(sorted(shared)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
