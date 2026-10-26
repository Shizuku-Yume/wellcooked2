#!/usr/bin/env bash
#
# W8 默认模式只做 Bridge 连接检查；REAL_GAME=1 用完整 Supervisor + 真游戏验收。
#
# 用法：
#     GAME_DIR=<游戏目录> REAL_GAME=1 SCENE=s_wellcooked_demo_3 CHEFS=4 \
#       bash tools/run-real-game.sh
# 完整模式会启动 Supervisor、通过 Steam 启动游戏、自动继续存档并加载场景，再检查新鲜运行产物。
# `_1` 验收同样设置 SCENE=s_wellcooked_demo_1。CHEFS 决定占几个玩家位（slots=1..CHEFS）与起几个 Worker。
#
# 两种模式都要求先取得共享游戏锁；完整模式还会备份并恢复三个 wellcooked 配置文件。

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
# 游戏侧日志。插件在 Awake 里会写一行 "[Plugin] WellCooked <版本> loaded (game=...)"，
# 这一行是"插件真的初始化了"与"BepInEx 只是找到了这个类型"之间的分界。
GAME_DIR="${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}"
BEPINEX_LOG="${BEPINEX_LOG:-${GAME_DIR}/BepInEx/LogOutput.log}"
# Awake 的第一件事是 BindConfig()，BepInEx 在 Bind 时会写这个文件。它是"Awake 跑过"
# 的第二个独立证据，与日志行互不依赖 —— 只看日志会把"没启动"和"日志没写出来"混为一谈。
PLUGIN_CFG="${GAME_DIR}/BepInEx/config/com.wellcooked.plugin.cfg"

# 完整实机验收由调用方提供 supervisor 运行目录；默认仍保留 W8 的 Bridge 轻量检查。
# REAL_GAME=1 时对整棵进程树的产物做严格断言，不把 fake_plugin 当作证据。
REAL_GAME="${REAL_GAME:-0}"
RUNTIME_DIR="${RUNTIME_DIR:-}"
SCENE="${SCENE:-s_wellcooked_demo_3}"
CHEFS="${CHEFS:-4}"
case "$CHEFS" in
    1|2|3|4) ;;
    *) printf 'CHEFS 必须是 1..4（本地合作最多 4 个玩家位），收到：%s\n' "$CHEFS" >&2; exit 2 ;;
esac
# 插件的玩家位是 1 基，0 号厨师对应 1 号位。
SLOTS="$(seq -s, 1 "$CHEFS")"
GAME_SECONDS="${GAME_SECONDS:-240}"

say() { printf '%s\n' "$*"; }
# 启动前把旧的 LogOutput.log 挪进本次运行目录，所以这里读到的只会是这次启动写的日志。
# 不能按旧文件长度/最后一个 loaded 标记切：BepInEx 重写日志之前，旧文件里上一局的
# “加入 pad”“已请求加载”会让各道闸门立刻放行（2026-10-26 `_3` 实测：pad 还没加入就发了 load）。
plugin_run_log() {
    [ -f "$BEPINEX_LOG" ] || return 1
    cat "$BEPINEX_LOG"
}

assert_real_game() {
    [ "$REAL_GAME" = "1" ] || return 0
    [ -d "$RUNTIME_DIR/runs" ] || { say "FAIL real-game: runs 目录不存在"; return 1; }
    python3 - "$RUNTIME_DIR" "$GAME_DIR/BepInEx/wellcooked-state.json" \
        "$GAME_DIR/BepInEx/LogOutput.log" "$SCENE" "$CHEFS" <<'PY'
import glob, json, os, sys
runtime, state_path, plugin_path, scene, chefs = sys.argv[1:]
chefs = int(chefs)
runs = sorted(glob.glob(os.path.join(runtime, "runs", "*")))
if not runs:
    raise SystemExit("FAIL real-game: 没有本次运行的 Scheduler run artifacts")
run = runs[-1]
def rows(path):
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]
events = rows(os.path.join(run, "events.jsonl"))
decisions = rows(os.path.join(run, "decisions.jsonl"))
with open(state_path, encoding="utf-8") as f:
    state = json.load(f)
if state.get("scene") != scene or len(state.get("chefs", [])) != chefs:
    raise SystemExit("FAIL real-game: state scene/chefs=%r/%d expected %s/%d" %
                     (state.get("scene"), len(state.get("chefs", [])), scene, chefs))
if float(state.get("score", 0)) <= 0:
    raise SystemExit("FAIL real-game: score=%s (must be > 0)" % state.get("score"))
completed = [e for e in events if e.get("event") == "order_completed"]
if not completed:
    raise SystemExit("FAIL real-game: no order_completed in %s" % run)
dispatched = [d for d in decisions if d.get("kind") == "dispatch"]
if not dispatched:
    raise SystemExit("FAIL real-game: no scheduler dispatch decisions")
navlog = os.path.join(runtime, "logs", "navd.log")
navtext = open(navlog, encoding="utf-8").read() if os.path.isfile(navlog) else ""
if not __import__("re").search(r"nav_plan .*released=1(?: |$)", navtext):
    raise SystemExit("FAIL real-game: navd has no released path plan")
workers = []
macro_done = set()
task_done = {int(e.get("task_id")) for e in events
             if e.get("event") == "task_done" and e.get("task_id") is not None}
for chef in range(chefs):
    path = os.path.join(runtime, "logs", "worker-%d.log" % chef)
    text = open(path, encoding="utf-8").read() if os.path.isfile(path) else ""
    starts = set(map(int, __import__("re").findall(r"macro_start task=(\d+)", text)))
    dones = set(map(int, __import__("re").findall(r"macro_done task=(\d+)", text)))
    if not starts or not dones or "macro=goto" not in text:
        raise SystemExit("FAIL real-game: worker-%d has no completed navigation macro" % chef)
    workers.append((chef, len(starts), len(dones)))
    macro_done.update(dones)
if not macro_done.intersection(task_done):
    raise SystemExit("FAIL real-game: no completed scheduler task matched worker macro_done")
if scene.endswith("_1"):
    if not any(d.get("node") == "wash" for d in dispatched):
        raise SystemExit("FAIL real-game: _1 had no wash dispatch")
    if any(e.get("event") in ("order_expired", "order_timeout") for e in events):
        raise SystemExit("FAIL real-game: _1 order expired/timed out")
    cooks = [i for i, d in enumerate(dispatched) if str(d.get("node", "")).startswith("cook:")]
    if not cooks:
        raise SystemExit("FAIL real-game: _1 has no cook dispatch to verify priority")
    boosted = [d for d in dispatched if str(d.get("node", "")).startswith("cook:") and
               float(d.get("weight_c", 0)) < 0]
    if not boosted:
        raise SystemExit("FAIL real-game: cook dispatch has no critical-path priority weight")
if not os.path.isfile(plugin_path):
    raise SystemExit("FAIL real-game: missing BepInEx LogOutput.log")
# 启动前旧日志已挪走，这个文件只有本次启动写的内容。
with open(plugin_path, encoding="utf-8", errors="replace") as f:
    plugin = f.read()
if "工作模式 auto（系统控制：只执行 Bridge 动作）" not in plugin:
    raise SystemExit("FAIL real-game: plugin did not confirm Linux-controlled auto mode")
if "工序:" in plugin:
    raise SystemExit("FAIL real-game: WorkOrder local substitute ran under auto")
print("PASS real-game: scene=%s chefs=%d score=%s orders=%d workers=%s run=%s" %
      (scene, chefs, state.get("score"), len(completed), workers, run))
PY
}

SUPERVISOR_PID=""
BACKUP_DIR=""
restore_game_configs() {
    [ -n "$BACKUP_DIR" ] || return 0
    local name
    for name in wellcooked-takeover.txt wellcooked-orders.json; do
        if [ -f "$BACKUP_DIR/$name" ]; then
            cp -p "$BACKUP_DIR/$name" "$GAME_DIR/BepInEx/$name"
        else
            rm -f "$GAME_DIR/BepInEx/$name"
        fi
    done
    rm -f "$GAME_DIR/BepInEx/wellcooked-command.txt"
}
cleanup() {
    if [ -n "$SUPERVISOR_PID" ] && kill -0 "$SUPERVISOR_PID" 2>/dev/null; then
        kill -TERM "$SUPERVISOR_PID" 2>/dev/null || true
        wait "$SUPERVISOR_PID" 2>/dev/null || true
    fi
    restore_game_configs
}
trap cleanup EXIT
trap 'exit 130' INT TERM

run_supervisor_acceptance() {
    local state deadline run_dir score scene chefs joined state_mtime_before
    local state_ready_streak=0
    local supervisor_bin="$REPO_ROOT/bridge/build/kitchenos-supervisor"
    [ -x "$supervisor_bin" ] || { say "先构建：cd bridge && make"; return 2; }
    [ -d "$GAME_DIR/BepInEx" ] || { say "找不到游戏 BepInEx 目录：$GAME_DIR"; return 2; }
    if [ -z "$RUNTIME_DIR" ]; then
        RUNTIME_DIR="$REPO_ROOT/logs/real-game/$(date +%Y%m%d-%H%M%S)-$$"
    fi
    [ ! -e "$RUNTIME_DIR" ] || { say "本次运行目录已存在，拒绝复用旧证据：$RUNTIME_DIR"; return 2; }
    mkdir -p "$RUNTIME_DIR/logs" "$RUNTIME_DIR/runs" "$GAME_DIR/BepInEx"
    BACKUP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/kc09-configs.XXXXXX")"
    for name in wellcooked-takeover.txt wellcooked-command.txt wellcooked-orders.json; do
        [ ! -e "$GAME_DIR/BepInEx/$name" ] || cp -p "$GAME_DIR/BepInEx/$name" "$BACKUP_DIR/$name"
    done
    if [ -f "$BEPINEX_LOG" ]; then
        mv "$BEPINEX_LOG" "$RUNTIME_DIR/LogOutput.previous.log"
    fi
    printf 'work=auto\nslots=%s\nfrontend_press=true\ndiag=false\nbridge=127.0.0.1:%s\n' "$SLOTS" "$PORT" \
        > "$GAME_DIR/BepInEx/wellcooked-takeover.txt"
    rm -f "$GAME_DIR/BepInEx/wellcooked-command.txt"
    state="$GAME_DIR/BepInEx/wellcooked-state.json"
    state_mtime_before="$(stat -c %Y "$state" 2>/dev/null || printf '0')"
    say "Launching the game through Steam; waiting for $CHEFS virtual pads to join before loading the level."
    if ! bash "$REPO_ROOT/scripts/run-game.sh"; then
        say "Game launch failed; see scripts/run-game.sh output."
        return 1
    fi

    deadline=$(( $(date +%s) + WAIT_SECONDS ))
    plugin_ready=0
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if plugin_run_log | grep -Fq "takeover: 每帧检查已挂上"; then
            plugin_ready=1
            break
        fi
        sleep 1
    done
    if [ "$plugin_ready" -ne 1 ]; then
        say "本次启动未检测到插件 Takeover 初始化。"
        return 1
    fi

    say "Waiting for the game itself to join $CHEFS virtual pads (no continue/engage command)."
    deadline=$(( $(date +%s) + WAIT_SECONDS ))
    joined=0
    while [ "$(date +%s)" -lt "$deadline" ]; do
        joined="$(plugin_run_log | grep -Fc 'takeover: 游戏已自己加入 pad ' || true)"
        [ "$joined" -ge "$CHEFS" ] && break
        sleep 2
    done
    if [ "$joined" -lt "$CHEFS" ]; then
        say "游戏未在 ${WAIT_SECONDS}s 内自动加入 ${CHEFS} 个虚拟 pad（${joined}/${CHEFS}）；停在 $(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("scene","?"))' "$state" 2>/dev/null || echo '?')。"
        return 1
    fi

    printf 'work=auto\nslots=%s\nfrontend_press=false\ndiag=false\nbridge=127.0.0.1:%s\n' "$SLOTS" "$PORT" \
        > "$GAME_DIR/BepInEx/wellcooked-takeover.txt"
    sleep 2
    printf 'load=%s\n' "$SCENE" > "$GAME_DIR/BepInEx/wellcooked-command.txt"
    deadline=$(( $(date +%s) + WAIT_SECONDS ))
    load_requested=0
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if plugin_run_log | grep -Fq "指令: 已请求加载 $SCENE"; then
            load_requested=1
            break
        fi
        sleep 1
    done
    if [ "$load_requested" -ne 1 ]; then
        say "游戏未确认处理 load=$SCENE 命令；不启动 Supervisor。"
        return 1
    fi
    state_mtime_before="$(python3 -c 'import os,sys; print(os.stat(sys.argv[1]).st_mtime_ns)' "$state" 2>/dev/null || printf '0')"
    deadline=$(( $(date +%s) + WAIT_SECONDS ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if python3 - "$state" "$SCENE" "$CHEFS" "$state_mtime_before" <<'PY'
import json, os, sys
try:
    state = json.load(open(sys.argv[1]))
    fresh = os.stat(sys.argv[1]).st_mtime_ns > int(sys.argv[4])
    raise SystemExit(0 if fresh and state.get("scene") == sys.argv[2] and
                     len(state.get("chefs", [])) == int(sys.argv[3]) else 1)
except Exception:
    raise SystemExit(1)
PY
        then
            state_ready_streak=$((state_ready_streak + 1))
            [ "$state_ready_streak" -ge 5 ] && break
        else
            state_ready_streak=0
        fi
        sleep 1
    done
    if [ "$state_ready_streak" -lt 5 ]; then
        say "指定关卡没有连续 5 秒报告预期厨师数：$SCENE/$CHEFS"
        return 1
    fi

    say "Game state confirmed: scene=$SCENE chefs=$CHEFS. Starting a fresh Supervisor."
    "$supervisor_bin" --foreground --verbose --runtime-dir "$RUNTIME_DIR" \
        --workers "$CHEFS" --bridge-host "$HOST" --bridge-port "$PORT" \
        --kitchen "$REPO_ROOT/config/maps/$SCENE/kitchen.json" \
        --recipes "$REPO_ROOT/config/maps/$SCENE/recipes.json" \
        --orders "$REPO_ROOT/config/orders.world.json" --runs-dir "$RUNTIME_DIR/runs" \
        > "$RUNTIME_DIR/supervisor.console.log" 2>&1 &
    SUPERVISOR_PID=$!

    say "Waiting up to ${GAME_SECONDS}s for fresh real-game acceptance artifacts."
    deadline=$(( $(date +%s) + GAME_SECONDS ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        run_dir="$(find "$RUNTIME_DIR/runs" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort | tail -n 1)"
        if [ -n "$run_dir" ] && grep -q '"kind":"order_completed"' "$RUNTIME_DIR/runs/$run_dir/events.jsonl" 2>/dev/null; then
            score="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("score",0))' "$state" 2>/dev/null || echo 0)"
            navlog="$RUNTIME_DIR/logs/navd.log"
            if python3 -c 'import sys; raise SystemExit(0 if float(sys.argv[1]) > 0 else 1)' "$score" &&
                grep -Eq 'nav_plan .*released=1([[:space:]]|$)' "$navlog" 2>/dev/null; then break; fi
        fi
        sleep 2
    done
    assert_real_game
}

if [ "$REAL_GAME" = "1" ]; then
    run_supervisor_acceptance
    exit $?
fi

BRIDGE_PID=""

cleanup_w8() {
    if [ -n "$BRIDGE_PID" ] && kill -0 "$BRIDGE_PID" 2>/dev/null; then
        kill -TERM "$BRIDGE_PID" 2>/dev/null || true
        wait "$BRIDGE_PID" 2>/dev/null || true
    fi
}
trap cleanup_w8 EXIT
trap 'exit 130' INT TERM

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
        assert_real_game
        exit $?
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
                say "   x 插件没有写出启动行 '[Plugin] WellCooked ... loaded'"
                if [ -f "$PLUGIN_CFG" ]; then
                    say "     但它的配置文件已生成（$(basename "$PLUGIN_CFG")）——"
                    say "     Awake 至少跑到了 BindConfig，所以插件其实启动了，"
                    say "     更可能是这一行日志没写出来。"
                else
                    say "     配置文件也没有生成（$(basename "$PLUGIN_CFG")）。"
                    say "     Awake 干的第一件事就是 BindConfig，BepInEx 会在那时写这个文件，"
                    say "     所以两个互相独立的信号都指向同一结论：插件没有被初始化。"
                fi
                say "     BepInEx 打印 'Loading [WellCooked ...]' 只说明它扫描到了这个类型，"
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
            say " StateReader 只有在关卡内才拿得到厨师/订单，菜单阶段没有状态可发。"
        fi
        say ""
        say " 日志末尾："
        tail -n 10 "$LOG" | sed 's/^/   /'
        exit 1
    fi

    sleep 1
done
