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

# 只改指定的几行，文件里其它配置（slots、work）保持不动；退出时整份还原（本来没有这个文件就删掉，
# 否则脚本留下的 `diag=true` 会一直生效，"默认不刷屏"就落空了）。
TAKEOVER_EXISTED=0
[ -f "$TAKEOVER" ] && TAKEOVER_EXISTED=1
ORIGINAL_TAKEOVER="$(cat "$TAKEOVER" 2>/dev/null || true)"
set_key() { # $1 = 键，$2 = 值
    local tmp
    tmp="$(mktemp)"
    grep -vE "^[[:space:]]*$1[[:space:]]*=" "$TAKEOVER" > "$tmp" 2>/dev/null || true
    printf '%s=%s\n' "$1" "$2" >> "$tmp"
    mv "$tmp" "$TAKEOVER"
}
restore_takeover() {
    if [ "$TAKEOVER_EXISTED" = "1" ]; then
        printf '%s\n' "$ORIGINAL_TAKEOVER" > "$TAKEOVER"
    else
        rm -f "$TAKEOVER"
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

# 前端按键：不开的话游戏停在标题画面（插件只挂手柄，没人按 A）。
# 诊断输出：这个脚本的产出就是走位/卡住那几行，它们默认不打了（任务 10 把过程行收进 Diag）。
set_key frontend_press true
set_key diag true
pkill -f 'Overcooked2.exe' >/dev/null 2>&1 || true
# 等进程真的没了再启动：Steam 还认为游戏在跑时，`steam -applaunch` 是个空操作
# （实测：上一局没退干净，这一次就只打印一行"启动 appid 728880"，游戏根本没起来）。
for _ in $(seq 1 20); do
    pgrep -f 'Overcooked2.exe' >/dev/null 2>&1 || break
    sleep 1
done
sleep 5
rm -f "$LOG" "$GAME_DIR/BepInEx/wellcooked-command.txt"

# 游戏的启动时间随机器负载变（实测同一台机器 40 秒到 3 分钟都出现过），等宽一点。
# 还没起来就再点一次火：Steam 偶尔会把第一次 applaunch 吃掉。
attempt=0
while [ $attempt -lt 2 ]; do
    attempt=$((attempt + 1))
    nohup bash scripts/run-game.sh >/tmp/wellcooked-run.out 2>&1 &
    for _ in $(seq 1 40); do
        grep -q 'takeover: 每帧检查' "$LOG" 2>/dev/null && break
        sleep 3
    done
    grep -q 'takeover: 每帧检查' "$LOG" && break
    echo "（第 $attempt 次启动没等到插件，再点一次火）"
    pkill -f 'Overcooked2.exe' >/dev/null 2>&1 || true
    for _ in $(seq 1 20); do
        pgrep -f 'Overcooked2.exe' >/dev/null 2>&1 || break
        sleep 1
    done
    sleep 5
    rm -f "$LOG"
done
grep -q 'takeover: 每帧检查' "$LOG" || { echo "插件没起来，看 $LOG"; exit 1; }
grep -E '前台伪装|失焦暂停' "$LOG" || true

if [ -n "$SCENE" ]; then
    # 等游戏自己把两副虚拟手柄都加入：这一步只在前端发生，加入之后才能 load 进关卡。
    for _ in $(seq 1 240); do
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
    set_key frontend_press false
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

# 状态文件由插件整份重写，读到一半会撞上空文件/半截 JSON；读失败不算进关失败，重试到下一份。
state_read=0
for _ in $(seq 1 10); do
    if python3 -c "
import json
d=json.load(open('$STATE'))
print('=== 进关: %s 厨师=%d 订单=%d' % (d['scene'], len(d['chefs']), len(d.get('orders',[]))))
" 2>/dev/null; then
        state_read=1
        break
    fi
    sleep 1
done
[ "$state_read" = 1 ] || echo "=== 进关: $SCENE（状态文件暂时读不到，继续观察）"
end=$((SECONDS + WATCH))
while [ $SECONDS -lt $end ]; do
    python3 -c "
import json
d=json.load(open('$STATE'))
print('t=%6.1f'%d['t'], ' '.join('%s(%.2f,%.2f)'%(c['player'],c['pos'][0],c['pos'][1]) for c in d['chefs']))
" 2>/dev/null || true
    sleep 2
done

echo "=== 走位（最后 8）:"
grep -E 'takeover: 走位' "$LOG" | tail -8 || true
echo -n "=== 计数: "
for k in 已到达 卡住 不可达 路径走完 没登记目标; do
    printf '%s=%s ' "$k" "$(grep -c "$k" "$LOG" || true)"
done
echo
