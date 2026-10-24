#!/usr/bin/env bash
#
# W8：与真实游戏跑通 —— 这是整个 effort 里唯一需要启动游戏的一步。
#
# 用法（在 WSL 里）：
#     bash tools/run-real-game.sh
# 然后按提示启动 Overcooked! 2。
#
# 只验证游戏侧判据（不启动 Bridge、不需要游戏）：
#     BEPINEX_LOG=/path/to/LogOutput.log bash tools/run-real-game.sh --check-game
# 退出码 0 = 日志里有交付插件的启动行；1 = 没有。
#
# 脚本自己起 Bridge、自己等插件、自己判定 task 05 的 Check，不要求你读日志下结论：
#     logs/bridge.log 里有 >=1 条 hello 与 >=10 条 state_update，且没有解析错误。
#
# 之所以先起 Bridge 再让你启动游戏：插件是 client（LinkRunner 默认连 127.0.0.1:14455，
# 断线后等 2 秒再连），收不到连接就自己重试。所以顺序反过来也照样能连上，你不用掐时间。

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_ROOT}/bridge/build/kitchenos-bridge"
LOG="${REPO_ROOT}/logs/bridge.log"
PORT="${PORT:-14455}"
# 绑 loopback 就够：WSL2 的 localhost 转发能到只绑 VM 回环的监听器（实测过），而链路
# 没有认证。跨机跑时用 HOST=0.0.0.0 覆盖。
HOST="${HOST:-127.0.0.1}"
WAIT_SECONDS="${WAIT_SECONDS:-120}"
NEED_HELLO="${NEED_HELLO:-1}"
NEED_STATE="${NEED_STATE:-10}"
# 游戏侧日志。交付插件（SmokePlugin.cs）在 Awake 里写
# "[Info   :WellCooked.Smoke] WellCooked.Smoke loaded; Unity ..."；这一行是"插件真的
# 初始化了"与"BepInEx 只是找到了这个类型（Loading [WellCooked.Smoke 0.1.0]）"之间的分界。
GAME_DIR="${GAME_DIR:-/mnt/d/EPIC/Games/Overcooked2}"
BEPINEX_LOG="${BEPINEX_LOG:-${GAME_DIR}/BepInEx/LogOutput.log}"
PLUGIN_LOG_MARK="WellCooked.Smoke loaded"
# 插件每 0.5 秒写一份状态快照（WorldState.Tick）。它是"插件 tick 循环跑过"的第二个独立证据，
# 与日志行互不依赖 —— 只看日志会把"没启动"和"日志没写出来"混为一谈。
PLUGIN_STATE="${GAME_DIR}/BepInEx/wellcooked-state.json"
# 开关文件是插件的输入配置，不是它生成的；写 bridge=off 时插件在跑也不会连。
PLUGIN_SWITCH="${GAME_DIR}/BepInEx/wellcooked-takeover.txt"

BRIDGE_PID=""

cleanup() {
    if [ -n "$BRIDGE_PID" ] && kill -0 "$BRIDGE_PID" 2>/dev/null; then
        kill -TERM "$BRIDGE_PID" 2>/dev/null
        wait "$BRIDGE_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

say() { printf '%s\n' "$*"; }

# 游戏侧判据：BepInEx 日志里必须有交付插件自己的启动行。旧参考工程写的是
# "[Plugin] WellCooked <版本> loaded"，不算数 —— 它只能证明参考工程在跑。
game_plugin_initialized() {
    [ -f "$BEPINEX_LOG" ] && grep -qF -- "$PLUGIN_LOG_MARK" "$BEPINEX_LOG"
}

# 给判据做双向证明用：只判定游戏侧证据，不启动 Bridge、不需要游戏。
#   BEPINEX_LOG=/tmp/fake.log bash tools/run-real-game.sh --check-game
# 退出码 0 = 找到交付插件的启动行；1 = 没找到（含文件不存在）。
if [ "${1:-}" = "--check-game" ]; then
    if game_plugin_initialized; then
        say "PASS 游戏侧：$BEPINEX_LOG 有 '$PLUGIN_LOG_MARK'"
        exit 0
    fi
    say "FAIL 游戏侧：$BEPINEX_LOG 没有 '$PLUGIN_LOG_MARK'"
    exit 1
fi

# ---------------------------------------------------------------- 前置检查
if [ ! -x "$BIN" ]; then
    say "找不到 Bridge 可执行文件：$BIN"
    say "先构建：cd bridge && make"
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
"$BIN" --link listen --host "$HOST" --port "$PORT" --log "$LOG" --verbose &
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
        # state_update 只证明"游戏在往这边发状态"，不证明"发的是关卡里的状态"。
        # 判据本身来自文档，不该被语义内容绑架；但内容为空时必须明说，
        # 否则一次停在菜单里的运行会被当成关卡联调成功。
        summary=$(grep "state_summary" "$LOG" | tail -n 1)
        if [ -n "$summary" ]; then
            say ""
            say " 最近一次状态摘要："
            say "   $summary"
            if echo "$summary" | grep -q "chefs=0"; then
                say ""
                say " 注意：状态帧确实在正常到达（帧号在涨就是证据），但游戏报告的内容是空的"
                say "       （chefs=0）—— 通常是还停在菜单/大厅，没有进入关卡。"
                say "       判定标准本身已满足；若验收要求的是关卡内的状态，进入关卡后再跑一次即可。"
            fi
        fi
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
            elif game_plugin_initialized; then
                say "   v 插件已初始化（它自己写了启动行）："
                grep -nF -- "$PLUGIN_LOG_MARK" "$BEPINEX_LOG" | tail -n 1 | sed 's/^/       /'
                say ""
                say "     插件输出的其余日志："
                grep -nF "WellCooked.Smoke]" "$BEPINEX_LOG" | tail -n 6 | sed 's/^/       /'
                say "     → 插件在跑却连不上，属于网络问题："
                say "       在 $PLUGIN_SWITCH 写 bridge=${WSL_IP}:${PORT}，或查 Windows 防火墙入站规则。"
            else
                say "   x 插件没有写出启动行 '$PLUGIN_LOG_MARK'"
                if [ -f "$PLUGIN_STATE" ]; then
                    say "     但状态快照已生成（$(basename "$PLUGIN_STATE")，修改时间 $(date -r "$PLUGIN_STATE" '+%F %T')）——"
                    say "     WorldState 每 0.5 秒写一次，说明插件 tick 循环跑过；"
                    say "     更可能是这一行日志没写出来或被截断。"
                else
                    say "     状态快照也没有生成（$(basename "$PLUGIN_STATE")）。"
                    say "     WorldState 每 0.5 秒写一次，与启动行互相独立，"
                    say "     所以两个信号都指向同一结论：插件没有被初始化。"
                fi
                if [ -f "$PLUGIN_SWITCH" ] &&
                   grep -Eiq '^[[:space:]]*bridge[[:space:]]*=[[:space:]]*(off|false|0)[[:space:]]*$' "$PLUGIN_SWITCH"; then
                    say "     ! $PLUGIN_SWITCH 里写着 bridge=off：插件在跑也不会连。"
                fi
                say "     BepInEx 打印 'Loading [WellCooked.Smoke ...]' 只说明它扫描到了这个类型，"
                say "     不等于 Awake 跑过了；两者必须分开看。"
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
            say " WorldState 只有在关卡内才读得到厨师/订单，菜单阶段状态帧里没有这些内容。"
        fi
        say ""
        say " 日志末尾："
        tail -n 10 "$LOG" | sed 's/^/   /'
        exit 1
    fi

    sleep 1
done
