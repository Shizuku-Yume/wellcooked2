#!/usr/bin/env bash
# 重启游戏 → 等前端自己走完（虚拟手柄按 A：标题 → 继续 → 世界地图 → 关卡）→
# 打一段位置/走位日志。
#
# 为什么不用截图：调试时人在终端、游戏在后台。插件里有三个补丁让后台能跑：
#   Application.runInBackground、InGamePauseMenu.OnApplicationFocus、Application.isFocused
# （游戏把所有本地输入都卡在 isFocused 上，不扳过来后台既不认手柄也不认"按 A"）。
# 进关卡不用手动点：前端由虚拟手柄按 A 走完。
#
# 用法：scripts/dev-cycle.sh [观察秒数] [场景名]
#
# 给了场景名就走**确定性进关**：虚拟手柄在前端按 A，等游戏自己把两副手柄都加入（`游戏已自己加入 pad`），
# 然后关掉前端按键、用 `load=<场景名>` 进指定关卡。两个理由：
#   - 不给场景名时进哪一关取决于存档里世界地图光标停在哪（复核任务 09 时自动进了 s_mine_4_6，
#     那关带矿车轨道、挑关卡时已被排除），量出来的结论不可用；
#   - 顺序不能反：`load` 之前必须等游戏自己加入虚拟手柄，否则关卡里没有厨师（实测 chefs=0，
#     工序无从跑起）。
# 另外前端按键必须开：`wellcooked-takeover.txt` 里没有 `frontend_press=true` 时游戏会停在
# 标题画面（插件只挂手柄，没人按 A）。脚本会改这一行并在退出时还原。
set -euo pipefail

GAME_DIR="${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}"
WATCH="${1:-20}"
SCENE="${2:-}"
LOG="$GAME_DIR/BepInEx/LogOutput.log"
STATE="$GAME_DIR/BepInEx/wellcooked-state.json"
TAKEOVER="$GAME_DIR/BepInEx/wellcooked-takeover.txt"

cd "$(dirname "$0")/.."

# 只改 frontend_press 这一行，文件里其它配置（slots、work、trace）保持不动。
ORIGINAL_PRESS="$(grep -E '^[[:space:]]*frontend_press' "$TAKEOVER" 2>/dev/null || true)"
set_frontend_press() {
    local tmp
    tmp="$(mktemp)"
    grep -vE '^[[:space:]]*frontend_press' "$TAKEOVER" > "$tmp" 2>/dev/null || true
    printf 'frontend_press=%s\n' "$1" >> "$tmp"
    mv "$tmp" "$TAKEOVER"
}
restore_takeover() {
    if [ -n "$ORIGINAL_PRESS" ]; then
        set_frontend_press "$(printf '%s' "$ORIGINAL_PRESS" | sed -E 's/^[^=]*=[[:space:]]*//')"
    fi
}
trap restore_takeover EXIT

state_field() { # $1 = 字段名；读不到就打印空
    python3 -c "
import json,sys
try:
    d=json.load(open('$STATE'))
except Exception:
    print(''); sys.exit()
v=d.get('$1')
print(len(v) if isinstance(v,list) else v if v is not None else '')
" 2>/dev/null || true
}

set_frontend_press true
pkill -f 'Overcooked2.exe' >/dev/null 2>&1 || true
sleep 3
rm -f "$LOG" "$GAME_DIR/BepInEx/wellcooked-command.txt"
nohup bash scripts/run-game.sh >/tmp/wellcooked-run.out 2>&1 &

for _ in $(seq 1 40); do
    grep -q 'takeover: 每帧检查' "$LOG" 2>/dev/null && break
    sleep 3
done
grep -q 'takeover: 每帧检查' "$LOG" || { echo "插件没起来，看 $LOG"; exit 1; }
grep -E '前台伪装|失焦暂停' "$LOG" || true

if [ -n "$SCENE" ]; then
    # 等游戏自己把两副虚拟手柄都加入：这一步只在前端发生，加入之后才能 load 进关卡。
    for _ in $(seq 1 90); do
        joined="$(grep -ac '游戏已自己加入 pad' "$LOG" 2>/dev/null || true)"
        [ "${joined:-0}" -ge 2 ] && break
        sleep 2
    done
    joined="$(grep -ac '游戏已自己加入 pad' "$LOG" 2>/dev/null || true)"
    joined="${joined:-0}"
    if [ "$joined" -lt 2 ]; then
        echo "虚拟手柄没被游戏加入（$joined/2），停在 $(state_field scene)，看 $LOG"
        exit 1
    fi
    grep -a '游戏已自己加入 pad' "$LOG" | tail -2
    # 关掉前端按键：否则虚拟手柄会自己一路按进存档里的那一关。
    set_frontend_press false
    sleep 2
    printf 'load=%s\n' "$SCENE" > "$GAME_DIR/BepInEx/wellcooked-command.txt"
    for _ in $(seq 1 30); do
        [ "$(state_field scene)" = "$SCENE" ] && [ "$(state_field chefs)" != "0" ] && break
        sleep 2
    done
    echo "=== 指定进关: $(state_field scene) 厨师=$(state_field chefs)"
else
    for _ in $(seq 1 60); do
        n="$(state_field chefs)"
        [ -n "$n" ] && [ "$n" != "0" ] && break
        sleep 3
    done
fi

python3 -c "
import json
d=json.load(open('$STATE'))
print('=== 进关: %s 厨师=%d 订单=%d' % (d['scene'], len(d['chefs']), len(d.get('orders',[]))))
"
end=$((SECONDS + WATCH))
while [ $SECONDS -lt $end ]; do
    python3 -c "
import json
d=json.load(open('$STATE'))
print('t=%6.1f'%d['t'], ' '.join('%s(%.2f,%.2f)'%(c['player'],c['pos'][0],c['pos'][1]) for c in d['chefs']))
"
    sleep 2
done

echo "=== 走位（最后 8）:"
grep -E 'takeover: 走位' "$LOG" | tail -8 || true
echo -n "=== 计数: "
for k in 已到达 卡住 不可达 路径走完 没登记目标; do
    printf '%s=%s ' "$k" "$(grep -c "$k" "$LOG" || true)"
done
echo
