#!/usr/bin/env bash
# 重启游戏 → 等前端自己走完（虚拟手柄按 A：标题 → 继续 → 世界地图 → 关卡）→
# 打一段位置/走位日志。
#
# 为什么不用截图：调试时人在终端、游戏在后台。插件里有三个补丁让后台能跑：
#   Application.runInBackground、InGamePauseMenu.OnApplicationFocus、Application.isFocused
# （游戏把所有本地输入都卡在 isFocused 上，不扳过来后台既不认手柄也不认"按 A"）。
# 进关卡不用手动点：前端由虚拟手柄按 A 走完。
#
# 用法：scripts/dev-cycle.sh [观察秒数]
set -euo pipefail

GAME_DIR="${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}"
WATCH="${1:-20}"
LOG="$GAME_DIR/BepInEx/LogOutput.log"
STATE="$GAME_DIR/BepInEx/wellcooked-state.json"

cd "$(dirname "$0")/.."

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

for _ in $(seq 1 60); do
    n=$(python3 -c "import json;print(len(json.load(open('$STATE'))['chefs']))" 2>/dev/null || echo 0)
    [ "$n" != "0" ] && break
    sleep 3
done

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
