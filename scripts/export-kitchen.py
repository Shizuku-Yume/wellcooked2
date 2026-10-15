#!/usr/bin/env python3
"""把插件导出的世界状态与地形合成 OS 侧要读的 `config/kitchen.json`。

输入（游戏目录 `BepInEx/` 下，插件每局写一次）：
  - `wellcooked-state.json`：工位（`stations[]`，带 `type`/`name`/`pos`/`height`/`gives`/`contents`）
  - `wellcooked-terrain.json`：0.6 米物理采样的可走格（`cells`）与每格的可通行邻居掩码（`mask`）

输出：一个文件说清"这个厨房有什么资源、地面怎么走"——Scheduler 和模拟器只读它，不再解析插件的原始 JSON。
方案 §6.6 把 `config/kitchen.json` 定为 OS 侧的输入；为什么不用外部数据集见 `.workflow/maps/prior-art.md`。

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
                           "types": [], "gives": None, "contents": []}
            order.append(key)
        group = groups[key]
        station_type = station.get("type")
        if station_type and station_type not in group["types"]:
            group["types"].append(station_type)
        if station.get("gives") and not group["gives"]:
            group["gives"] = station["gives"]
        if station.get("contents") and not group["contents"]:
            group["contents"] = station["contents"]
    return [groups[key] for key in order]


def kind_of(types):
    for kind in KIND_ORDER:
        if any(t in TYPE_TO_KIND and TYPE_TO_KIND[t] == kind for t in types):
            return kind
    return "unknown"


def build_stations(raw):
    merged = []
    unknown = []
    for group in group_stations(raw):
        kind = kind_of(group["types"])
        for station_type in group["types"]:
            if station_type not in TYPE_TO_KIND and station_type not in unknown:
                unknown.append(station_type)
        merged.append({
            "kind": kind,
            "name": group["name"],
            "types": group["types"],
            "pos": [round(group["pos"][0], 2), round(group["pos"][1], 2)],
            "height": round(float(group["height"]), 2),
            "gives": group["gives"],
            "contents": group["contents"],
        })
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--state", default=DEFAULT_STATE)
    parser.add_argument("--terrain", default=DEFAULT_TERRAIN)
    parser.add_argument("--out", default=DEFAULT_OUT)
    args = parser.parse_args()

    for path in (args.state, args.terrain):
        if not os.path.exists(path):
            print("读不到 %s：先起游戏进一次关卡（插件会导出）" % path, file=sys.stderr)
            return 1

    state = load(args.state)
    terrain = load(args.terrain)

    stations, unknown = build_stations(state.get("stations") or [])
    step, indices, lookup, cells = grid_index(terrain)
    mask = terrain.get("mask") or []
    sizes = areas_of(indices, mask, lookup)

    kitchen = {
        "scene": terrain.get("scene") or state.get("scene"),
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
