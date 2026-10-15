#!/usr/bin/env bash
#
# W8：与真实游戏跑通 —— 这是整个 effort 里唯一需要启动游戏的一步。
#
# 用法（在 WSL 里）：
#     bash tools/run-real-game.sh
# 然后按提示启动 Overcooked! 2。
#
# 脚本自己起 Bridge、自己等插件、自己判定 task 05 的 Check，不要求你读日志下结论：
#     logs/bridge.log 里有 >=1 条 hello 与 >=10 条 state_update，且没有解析错误。
#
# 之所以先起 Bridge 再让你启动游戏：插件是 client（Network.Mode=client，
# Network.Port=12345），收不到连接就每 ReconnectDelayMs 重试一次。所以顺序反过来
# 也照样能连上，你不用掐时间。

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_ROOT}/core/build/kitchenos-bridge"
LOG="${REPO_ROOT}/logs/bridge.log"
PORT="${PORT:-12345}"
HOST="${HOST:-0.0.0.0}"
WAIT_SECONDS="${WAIT_SECONDS:-120}"
NEED_HELLO="${NEED_HELLO:-1}"
NEED_STATE="${NEED_STATE:-10}"
# 游戏侧日志。插件在 Awake 里会写一行 "[Plugin] WellCooked <版本> loaded (game=...)"，
# 这一行是"插件真的初始化了"与"BepInEx 只是找到了这个类型"之间的分界。
GAME_DIR="${GAME_DIR:-/mnt/d/EPIC/Games/Overcooked2}"
BEPINEX_LOG="${BEPINEX_LOG:-${GAME_DIR}/BepInEx/LogOutput.log}"

BRIDGE_PID=""

cleanup() {
    if [ -n "$BRIDGE_PID" ] && kill -0 "$BRIDGE_PID" 2>/dev/null; then
        kill -TERM "$BRIDGE_PID" 2>/dev/null
        wait "$BRIDGE_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

say() { printf '%s\n' "$*"; }

# ---------------------------------------------------------------- 前置检查
if [ ! -x "$BIN" ]; then
    say "找不到 Bridge 可执行文件：$BIN"
    say "先构建：cd core && make"
    exit 2
fi

mkdir -p "$(dirname "$LOG")"
# 每次从空日志开始，否则上一次的证据会冒充这一次的。
: > "$LOG"

if ss -ltn 2>/dev/null | grep -q ":${PORT}\b"; then
    say "端口 ${PORT} 已被占用；可能是上一次的 Bridge 还活着。"
    say "先清理：pkill -f kitchenos-bridge"
    exit 2
fi

WSL_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"

say "=================================================================="
say " 正在启动 Bridge：$HOST:$PORT"
say " 日志：$LOG"
"$BIN" --link listen --host "$HOST" --port "$PORT" --log "$LOG" &
BRIDGE_PID=$!
sleep 1

if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
    say "Bridge 启动后立刻退出了。日志末尾："
    tail -n 15 "$LOG"
    exit 1
fi

say " Bridge 已就绪 (pid $BRIDGE_PID)。"
say "=================================================================="
say ""
say " 现在请启动 Overcooked! 2，进入一个关卡（故事模式或街机都行）。"
say " 插件默认就是 client 连 127.0.0.1:${PORT}，游戏侧不需要改配置。"
say " 如果 127.0.0.1 连不上，把插件配置里的 Network.Host 改成：${WSL_IP}"
say "   配置文件：D:\\EPIC\\Games\\Overcooked2\\BepInEx\\config\\"
say ""
say " 本脚本最多等 ${WAIT_SECONDS} 秒，判定标准：hello >= ${NEED_HELLO}，state_update >= ${NEED_STATE}，解析错误 = 0"
say "------------------------------------------------------------------"

# ---------------------------------------------------------------- 等待与判定
deadline=$(( $(date +%s) + WAIT_SECONDS ))
reported_game_seen=0

count() { grep -c -- "$1" "$LOG" 2>/dev/null || true; }

while :; do
    hello=$(count "plugin_hello")
    state=$(count "state_update")
    parse_err=$(count "parse_error")
    frame_err=$(count "frame_error")

    if [ "$hello" -ge 1 ] && [ "$reported_game_seen" -eq 0 ]; then
        reported_game_seen=1
        say " 插件已连上，hello 收到。开始收状态……"
    fi

    if [ "$hello" -ge "$NEED_HELLO" ] && [ "$state" -ge "$NEED_STATE" ] && [ "$parse_err" -eq 0 ]; then
        say "------------------------------------------------------------------"
        say " PASS  W8：hello=${hello} state_update=${state} parse_error=${parse_err}"
        say ""
        say " 证据（bridge.log 摘录）："
        grep -m 2 "plugin_hello" "$LOG" | sed 's/^/   /'
        grep -m 3 "state_update" "$LOG" | sed 's/^/   /'
        say "------------------------------------------------------------------"
        exit 0
    fi

    if [ "$(date +%s)" -ge "$deadline" ]; then
        say "------------------------------------------------------------------"
        say " FAIL  W8：等了 ${WAIT_SECONDS} 秒仍未达标"
        say "   hello=${hello}（需要 >= ${NEED_HELLO}）"
        say "   state_update=${state}（需要 >= ${NEED_STATE}）"
        say "   parse_error=${parse_err}（需要 0）"
        say ""
        if [ "$hello" -eq 0 ]; then
            say " 插件一次都没连上。游戏侧走到哪一步了："
            say ""
            if [ ! -f "$BEPINEX_LOG" ]; then
                say "   ? 找不到 $BEPINEX_LOG"
                say "     → 游戏没有以 BepInEx 方式启动过。若装在别处：GAME_DIR=<游戏目录> 重跑。"
            elif grep -qF "could not be instantiated" "$BEPINEX_LOG"; then
                say "   x 插件加载失败：日志里有 'could not be instantiated'"
                grep -nF "could not be instantiated" "$BEPINEX_LOG" | tail -n 2 | sed 's/^/       /'
                say "     → 这是插件自身的问题，与网络无关。"
            elif grep -qF "[Plugin] WellCooked" "$BEPINEX_LOG"; then
                say "   v 插件已初始化（它自己写了启动行）："
                grep -nF "[Plugin] WellCooked" "$BEPINEX_LOG" | tail -n 1 | sed 's/^/       /'
                say ""
                say "     插件输出的其余日志："
                grep -nF "[Plugin]" "$BEPINEX_LOG" | tail -n 6 | sed 's/^/       /'
                say "     → 插件在跑却连不上，属于网络问题："
                say "       把插件配置 Network.Host 改成 ${WSL_IP}，或查 Windows 防火墙入站规则。"
            else
                say "   x 日志里没有插件自己的启动行 '[Plugin] WellCooked ... loaded'"
                say "     → 插件没有被初始化。BepInEx 打印 'Loading [WellCooked ...]' 只说明"
                say "       它找到了这个类型，不等于 Awake 跑过了；两者必须分开看。"
                say ""
                say "     游戏侧日志末尾："
                tail -n 8 "$BEPINEX_LOG" | sed 's/^/       /'
            fi
            say ""
            say " 另外还要确认："
            say "  - 游戏是否真的进了关卡（菜单阶段没有状态可发）"
            say "  - Windows 防火墙是否拦了 WSL 的入站连接"
        else
            say " 插件连上了但状态不够。可能是停在菜单里没有进入关卡 ——"
            say " StateReader 只有在关卡内才拿得到厨师/订单，菜单阶段没有状态可发。"
        fi
        say ""
        say " 日志末尾："
        tail -n 10 "$LOG" | sed 's/^/   /'
        exit 1
    fi

    sleep 1
done
