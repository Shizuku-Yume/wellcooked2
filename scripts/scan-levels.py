#!/usr/bin/env python3
"""扫一遍游戏里的关卡资源，给"挑一关做调度演示/调试"用。

为什么要离线扫：游戏一共 144 个关卡场景（`Overcooked2_Data/StreamingAssets/Windows/` 下
的 bundle 文件名就是场景名），一关一关进游戏看太慢。这个脚本直接读 bundle，
按**物体名**统计每关的料箱/砧板/盘子/锅和可疑机制物体，先筛掉明显不能用的。

注意：按名字筛会误判——布景也叫 `city_bridge`、`raft_flower_01`。所以筛出来的
"无机制"只是候选，最后要在游戏里用 `mech` 指令数**真实组件**确认（`load=<场景名>` 进关，
往 `BepInEx/wellcooked-command.txt` 写 `mech=1`）。

依赖：UnityPy（`uv venv /tmp/.ocenv --python 3.12 && uv pip install --python /tmp/.ocenv/bin/python UnityPy`）

用法：
    python3 scripts/scan-levels.py                 # 全部关卡，打印候选
    python3 scripts/scan-levels.py s_sushi_1_2     # 只看指定关卡的物体清单
"""

import collections
import os
import re
import subprocess
import sys

LIB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib.sh")
_BUNDLES = None


def game_dir():
    """GAME_DIR 的默认值、存在性检查与绝对化只在 scripts/lib.sh。"""
    proc = subprocess.run(["bash", LIB], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise SystemExit(proc.returncode)
    return proc.stdout.strip()


def bundles_dir():
    global _BUNDLES
    if _BUNDLES is None:
        _BUNDLES = os.path.join(game_dir(), "Overcooked2_Data", "StreamingAssets", "Windows")
    return _BUNDLES


# 真正的关卡机制（出现这些名字基本就不能用：传送带、传送门、桥、开关、冰面、黏液…）
MECH = re.compile(
    r"(conveyorbelt|conveyorstation|conveyor_props|teleport|portal|warp|slippery"
    r"|ice_floor|puddle|slime|flood|\bwind\b|windzone|cannon|elevator|geyser"
    r"|trolley|raft|\bboat\b|bridge|switch_pad|toggle_switch|firepit|hotplate"
    r"|firestick|trapdoor|chute|lava)",
    re.I,
)
# 每关都有的通用系统/布景，不算机制
NOISE = re.compile(
    r"(bootstrap|playerswitching|pfx_|combinedmesh|window|glass|wall|voicechat"
    r"|minecart|backpack|manager|light|fx_)",
    re.I,
)


def load_bundle(level):
    import UnityPy

    return UnityPy.load(os.path.join(bundles_dir(), level))


def game_objects(env):
    """返回 {归一化名字: 个数}；名字里的编号后缀（" 3 (4)"）去掉。"""
    counts = collections.Counter()
    for obj in env.objects:
        if obj.type.name != "GameObject":
            continue
        try:
            name = re.sub(r"[\s_]*\(?\d+\)?$", "", obj.read().m_Name or "").strip()
        except Exception:
            continue
        if name:
            counts[name] += 1
    return counts


def container_path(env):
    for file in env.files.values():
        for path in (getattr(file, "container", {}) or {}):
            return path
    return "?"


def summarize(level):
    env = load_bundle(level)
    go = game_objects(env)
    return {
        "level": level,
        "world": (re.search(r"Scenes/([^/]+)/", container_path(env)) or [None, "?"])[1],
        "dlc": "DownloadableContent" in container_path(env),
        "crates": sum(v for k, v in go.items() if "DispenserCrate" in k or k == "NewCrate"),
        "boards": sum(v for k, v in go.items() if k.startswith("ChoppingBoard")),
        "plates": sum(v for k, v in go.items() if k.startswith("Plate") and "Return" not in k),
        "pots": sum(v for k, v in go.items() if "Cooking Station" in k or "Fryer" in k),
        "bad": sorted({k for k in go if MECH.search(k) and not NOISE.search(k)}),
    }


def main():
    levels = sorted(f for f in os.listdir(bundles_dir()) if f.startswith("s_"))
    if len(sys.argv) > 1:
        for level in sys.argv[1:]:
            go = game_objects(load_bundle(level))
            print("=== %s" % level)
            for name, count in sorted(go.items()):
                if MECH.search(name) or re.search(
                    r"crate|board|plate|counter|pot|pan|dispenser|workstation|sink|bin",
                    name,
                    re.I,
                ):
                    print("    %-46s %d" % (name, count))
        return

    rows = []
    for level in levels:
        try:
            rows.append(summarize(level))
        except Exception as error:  # 个别 bundle 读不了就跳过
            print("!! %s 读取失败: %s" % (level, error), file=sys.stderr)

    base = [r for r in rows if not r["dlc"]]
    clean = [r for r in base if not r["bad"]]
    print("关卡 %d（本体 %d，DLC %d）；本体里没扫到机制物体的 %d 个："
          % (len(rows), len(base), len(rows) - len(base), len(clean)))
    clean.sort(key=lambda r: -(r["crates"] + r["boards"] * 2 + r["pots"] * 3))
    print("  %-20s %-12s 料箱 砧板 盘子 锅" % ("场景", "世界"))
    for r in clean:
        print("  %-20s %-12s %4d %4d %4d %2d"
              % (r["level"], r["world"], r["crates"], r["boards"], r["plates"], r["pots"]))
    print("\n被机制物体排除的本体关卡（前 15）：")
    for r in [x for x in base if x["bad"]][:15]:
        print("  %-20s %s" % (r["level"], ", ".join(r["bad"][:4])))


if __name__ == "__main__":
    main()
