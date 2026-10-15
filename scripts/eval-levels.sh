#!/usr/bin/env bash
# 逐关评估：每关**重开一局**（`load` 只换场景，菜谱/地形要新进程才刷新；手柄也要重新占位才有厨师，
# 没厨师时地形采样会用兜底半径，通道被判死、连通性数字不可信）。
#
# 用法: bash scripts/eval-levels.sh s_city_h4 s_city_h7 ...
#
# 输出每关：菜谱、工位数、连通性（主块占可走格的比例——"一块厨房"就看它接近 1）、机制组件。
set -uo pipefail

cd "$(dirname "$0")/.."
GAME_DIR="${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}"
LOG="$GAME_DIR/BepInEx/LogOutput.log"
STATE="$GAME_DIR/BepInEx/wellcooked-state.json"
CMD="$GAME_DIR/BepInEx/wellcooked-command.txt"

scene() { python3 -c "import json;print(json.load(open('$STATE')).get('scene'))" 2>/dev/null; }
chefs() { python3 -c "import json;print(len(json.load(open('$STATE'))['chefs']))" 2>/dev/null || echo 0; }

for level in "$@"; do
    pkill -f 'Overcooked2.exe' 2>/dev/null
    sleep 4
    rm -f "$LOG"
    : > "$CMD"
    nohup bash scripts/run-game.sh >/tmp/eval-run.out 2>&1 &

    # 等插件起来
    for _ in $(seq 1 70); do
        [ -f "$LOG" ] && grep -q 'Chainloader startup complete' "$LOG" && break
        sleep 3
    done

    # 等游戏自己把手柄占位（前端流程跑起来），否则 load 完没有厨师
    for _ in $(seq 1 40); do
        joined=$(grep -c '游戏已自己加入 pad' "$LOG" 2>/dev/null || echo 0)
        [ "$joined" -ge 2 ] && break
        sleep 3
    done
    sleep 12

    printf 'load=%s\n' "$level" > "$CMD"
    for _ in $(seq 1 30); do
        [ "$(scene)" = "$level" ] && break
        sleep 3
    done
    for _ in $(seq 1 25); do
        [ "$(chefs)" != "0" ] && break
        sleep 3
    done
    sleep 5
    printf 'mech=1\n' > "$CMD"
    sleep 6

    echo "=== $level"
    python3 - "$STATE" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
recipes = [x.get("dish") for x in (d.get("recipes") or []) if x.get("dish")]
print("   菜谱:", " | ".join(sorted(set(recipes))) or "（没读到）")
print("   厨师 %d，工位 %d" % (len(d["chefs"]), len(d.get("stations") or [])))
PY
    grep -E 'state: 连通块' "$LOG" | tail -1 | sed 's/.*state: /   地形: /' | cut -c1-130
    grep 'mech: 机制' "$LOG" | tail -1 | sed 's/.*机制 = /   机制: /' | cut -c1-130
done

pkill -f 'Overcooked2.exe' 2>/dev/null
