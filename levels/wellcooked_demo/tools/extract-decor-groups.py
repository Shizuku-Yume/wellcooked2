"""从示例关卡里提取"装饰组"：把 Decoration / Left / Right 下的物件按 1.8 米邻近聚类，
打印每组的构成（相对偏移），用于移植到我们自己的关卡。"""
import json
import math
import os
from collections import defaultdict
import UnityPy

SA = r"D:\2026-2027秋季学期\操作系统课设\wellcooked2\Overcooked 2!"
BUNDLE = os.path.join(SA, "BepInEx", "plugins", "OC2DIYLevel", "levels", "test_level", "s_test_level")
OUT = r"D:\oc2-level-build\testlevel-decor-groups.json"

env = UnityPy.load(BUNDLE)
name, father, children, lpos = {}, {}, defaultdict(list), {}
for o in env.objects:
    if o.type.name != "Transform":
        continue
    try:
        d = o.read()
        go = d.m_GameObject.read()
    except Exception:
        continue
    name[o.path_id] = go.m_Name
    father[o.path_id] = d.m_Father.path_id if d.m_Father else 0
    lp = d.m_LocalPosition
    lpos[o.path_id] = (lp.x, lp.y, lp.z)
    children[father[o.path_id]].append(o.path_id)


def find_child(pid, target):
    for k in children.get(pid, []):
        if name[k].strip() == target or name[k].strip().split(" (")[0].strip() == target:
            return k
    return None


def clean(n):
    n = n.strip()
    return n.split(" (")[0].strip()


roots = [p for p in father if father[p] == 0]
by_name = {clean(name[p]): p for p in roots}
art = by_name.get("Art")
decor = find_child(art, "Decoration") if art else None
ground = find_child(art, "Ground") if art else None
print("Art=", art, "Decoration=", decor, "Ground=", ground)

# 收集：Decoration 的所有孩子 + Ground/Left + Ground/Right 的孩子
pools = {}
if decor:
    pools["Decoration"] = [k for k in children.get(decor, [])]
if ground:
    for nm in ("Left", "Right"):
        k = find_child(ground, nm)
        if k:
            pools[nm] = [c for c in children.get(k, [])]

SKIP_PREFIX = ("collidor", "collider")
groups_out = []
for pool, ids in pools.items():
    items = []
    for pid in ids:
        nm = clean(name[pid])
        if nm.lower().startswith(SKIP_PREFIX):
            continue
        x, y, z = lpos[pid]
        items.append({"name": nm, "x": x, "y": y, "z": z})
    # 邻近聚类
    parent = list(range(len(items)))
    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            a, b = items[i], items[j]
            if math.hypot(a["x"] - b["x"], a["z"] - b["z"]) <= 1.8 and abs(a["y"] - b["y"]) < 2.0:
                union(i, j)
    buckets = defaultdict(list)
    for i, it in enumerate(items):
        buckets[find(i)].append(it)
    for members in buckets.values():
        if len(members) < 2:
            continue
        cx = sum(m["x"] for m in members) / len(members)
        cz = sum(m["z"] for m in members) / len(members)
        y0 = min(m["y"] for m in members)
        groups_out.append({
            "pool": pool,
            "size": len(members),
            "members": [{"name": m["name"],
                         "dx": round(m["x"] - cx, 2),
                         "dy": round(m["y"] - y0, 2),
                         "dz": round(m["z"] - cz, 2)} for m in members],
        })

groups_out.sort(key=lambda g: -g["size"])
with open(OUT, "w", encoding="utf-8") as fh:
    json.dump(groups_out, fh, ensure_ascii=False, indent=1)
print("groups:", len(groups_out), "->", OUT)
for g in groups_out[:8]:
    print("  [%s] %d 件: %s" % (g["pool"], g["size"],
          ", ".join("%s(%.1f,%.1f)" % (m["name"], m["dx"], m["dz"]) for m in g["members"])))
