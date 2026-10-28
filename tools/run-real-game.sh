#!/usr/bin/env bash
#
# One-command real-game iteration for WellCooked.
#
#   SCENE=s_wellcooked_demo_3 CHEFS=4 bash tools/run-real-game.sh
#
# The command takes the shared game lock, deploys the plugin and level bundle,
# starts (or reuses) Overcooked! 2, makes exactly CHEFS virtual pads join, turns
# the frontend A presses off, loads the scene, waits for five stable seconds of
# fresh state, optionally re-exports the map, starts the Supervisor tree, runs
# until the round ends, writes a one-screen summary, restores configs and
# releases the lock.
#
# No step asks for a human key press.  Useful overrides:
#   GAME_DIR=...            game installation
#   SOURCE_ROOT=...         tree whose plugin/bridge/configs are used
#   BUNDLE_REF=origin/main  git ref for the PR42 level bundles (or BUNDLE_SRC=dir)
#   BUILD_PLUGIN=0          do not build; use SOURCE_ROOT's existing DLL
#   BUILD_BRIDGE=0          do not build; use SOURCE_ROOT's existing bridge/build
#   REUSE_GAME=1            reuse the game a KEEP_GAME=1 run left open when it still fits:
#                           same CHEFS, same game pids, same plugin DLL and level bundles,
#                           state still ticking; otherwise stop it and relaunch
#   EXPORT=1                menu=1 + scripts/export-kitchen.py --map-dir MAP_DIR
#   MAP_DIR=...             default <this repo>/config/maps
#   KEEP_GAME=1             do not stop the game after a finished (or console-stopped) round;
#                           the lock stays held and the next REUSE_GAME=1 run loads its
#                           scene in the same game, skipping launch and pad joining
#   DIAG=1                  plugin diag=true
#   GAME_SECONDS=240        hard cap after the Supervisor starts
#   WAIT_SECONDS=240        launch/load/pad timeout
#   PORT=14455 HOST=127.0.0.1
#   STRATEGY=fcfs           order policy: fcfs|sjf|llf|hrrn|edf
#   TASK_POLICY=cost        task assignment: cost|fifo|cp
#   DISPATCH=flexible       flexible|zoned
#
# Exit status is 0 only when the summary assertions pass.  A completed run with
# failed gameplay assertions still leaves all logs and a summary and exits 1.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}"
SCENE="${SCENE:-s_wellcooked_demo_3}"
CHEFS="${CHEFS:-4}"
TAG="${TAG:-${SCENE##*_}-${CHEFS}chefs}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-14455}"
WAIT_SECONDS="${WAIT_SECONDS:-240}"
GAME_SECONDS="${GAME_SECONDS:-240}"
LOCK="${LOCK:-/tmp/wellcooked-game.lock}"
LOCK_OWNER="${LOCK_OWNER:-A 09 runner}"
LOCK_WAIT="${LOCK_WAIT:-120}"
LOCK_HELD=0
REUSE_GAME="${REUSE_GAME:-0}"
KEEP_GAME="${KEEP_GAME:-0}"
BUILD_PLUGIN="${BUILD_PLUGIN:-1}"
BUILD_BRIDGE="${BUILD_BRIDGE:-1}"
EXPORT="${EXPORT:-0}"
DIAG="${DIAG:-false}"
EXPORT_SCRIPT="${EXPORT_SCRIPT:-}"
MAP_DIR="${MAP_DIR:-$ROOT/config/maps}"
STRATEGY="${STRATEGY:-fcfs}"
TASK_POLICY="${TASK_POLICY:-cost}"
DISPATCH="${DISPATCH:-flexible}"

SOURCE_ROOT="${SOURCE_ROOT:-$ROOT}"
PLUGIN_DLL="${PLUGIN_DLL:-$SOURCE_ROOT/plugin/bin/Release/net35/WellCooked.Smoke.dll}"
BRIDGE_BIN_DIR="${BRIDGE_BIN_DIR:-$SOURCE_ROOT/bridge/build}"
SUPERVISOR="$BRIDGE_BIN_DIR/wellcooked-supervisor"
MONITOR="$BRIDGE_BIN_DIR/wellcooked-monitor"
BUNDLE_SRC="${BUNDLE_SRC:-}"
BUNDLE_REF="${BUNDLE_REF:-origin/main}"

STATE="$GAME_DIR/BepInEx/wellcooked-state.json"
MENU="$GAME_DIR/BepInEx/wellcooked-menu.json"
TERRAIN="$GAME_DIR/BepInEx/wellcooked-terrain.json"
LOG="$GAME_DIR/BepInEx/LogOutput.log"
TAKEOVER="$GAME_DIR/BepInEx/wellcooked-takeover.txt"
ORDERS="$GAME_DIR/BepInEx/wellcooked-orders.json"
COMMAND="$GAME_DIR/BepInEx/wellcooked-command.txt"
DEST_PLUGIN="$GAME_DIR/BepInEx/plugins/WellCooked.Smoke.dll"
DEST_LEVEL="$GAME_DIR/BepInEx/plugins/OC2DIYLevel/levels/wellcooked_demo"

RT=""
SUPERVISOR_PID=""
MONITOR_PID=""
BACKUP_DIR=""
LOG_OFFSET=0
GAME_STARTED=0
CLEANED=0
FAIL_REASON=""
SUMMARY_RC=0
REUSED=0
GAME_READY=0
REUSE_WHY=""
LAUNCH_LOG=""
# KEEP_GAME=1 留下的游戏的记录：厨师数、游戏 pid、插件与关卡包的 sha256、启动段日志。
KEPT="${KEPT:-$LOCK.kept}"
STARTED_AT="$(date +%Y%m%d-%H%M%S)"
SLOTS="$(seq -s, 1 "$CHEFS")"

say() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
die() { FAIL_REASON="$*"; printf 'FAIL: %s\n' "$*" >&2; exit 1; }

[ -d "$GAME_DIR/BepInEx" ] || die "game BepInEx directory missing: $GAME_DIR"
case "$CHEFS" in 1|2|3|4) ;; *) die "CHEFS must be 1..4, got $CHEFS" ;; esac
[ -f "$GAME_DIR/Overcooked2.exe" ] || die "Overcooked2.exe not found under $GAME_DIR"

# --------------------------------------------------------------- lock

lock_owner() {
    if [ -d "$LOCK" ]; then
        cat "$LOCK/owner" 2>/dev/null || true
    else
        cat "$LOCK" 2>/dev/null || true
    fi
}

acquire_lock() {
    local owner deadline
    if [ -e "$LOCK" ] || [ -d "$LOCK" ]; then
        owner="$(lock_owner)"
        owner="${owner%%$'\n'*}"
        if [ "$owner" = "$LOCK_OWNER" ]; then
            LOCK_HELD=1
            say "lock: already held by $LOCK_OWNER"
            return
        fi
        say "lock: held by '${owner:-unknown}', waiting up to ${LOCK_WAIT}s"
        deadline=$((SECONDS + LOCK_WAIT))
        while [ -e "$LOCK" ] || [ -d "$LOCK" ]; do
            [ "$(date +%s)" -lt "$deadline" ] || die "game lock is held by '${owner:-unknown}': $LOCK"
            sleep 2
        done
    fi
    if ( set -o noclobber; : > "$LOCK" ) 2>/dev/null; then
        printf '%s\n' "$LOCK_OWNER" > "$LOCK"
        LOCK_HELD=1
        say "lock: acquired ($LOCK_OWNER)"
    else
        owner="$(lock_owner)"
        [ "$owner" = "$LOCK_OWNER" ] || die "could not acquire game lock: '${owner:-unknown}'"
        LOCK_HELD=1
    fi
}

release_lock() {
    local owner
    [ "$LOCK_HELD" = 1 ] || return 0
    owner="$(lock_owner)"
    owner="${owner%%$'\n'*}"
    if [ "$owner" != "$LOCK_OWNER" ]; then
        warn "lock owner changed to '${owner:-unknown}'; not removing $LOCK"
        LOCK_HELD=0
        return 0
    fi
    if [ -d "$LOCK" ]; then
        rm -f "$LOCK/owner" 2>/dev/null || true
        rmdir "$LOCK" 2>/dev/null || true
    else
        rm -f "$LOCK"
    fi
    LOCK_HELD=0
    say "lock: released"
}

# --------------------------------------------------------------- log offset

log_since() {
    local size
    [ -f "$LOG" ] || return 0
    size="$(stat -c %s "$LOG" 2>/dev/null || printf '0')"
    if [ "$size" -lt "$LOG_OFFSET" ]; then
        LOG_OFFSET=0
        say "log: truncated, offset reset to 0"
    fi
    tail -c +"$((LOG_OFFSET + 1))" "$LOG" 2>/dev/null || true
}

log_slice_to() {
    local out size
    out="$1"
    [ -f "$LOG" ] || { : > "$out"; return 0; }
    size="$(stat -c %s "$LOG" 2>/dev/null || printf '0')"
    if [ "$size" -lt "$LOG_OFFSET" ]; then LOG_OFFSET=0; fi
    tail -c +"$((LOG_OFFSET + 1))" "$LOG" > "$out" 2>/dev/null || true
}

# --------------------------------------------------------------- configs

backup_configs() {
    local name
    BACKUP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/kc-runner-configs.XXXXXX")"
    for name in wellcooked-takeover.txt wellcooked-orders.json wellcooked-command.txt; do
        if [ -e "$GAME_DIR/BepInEx/$name" ]; then
            cp -p "$GAME_DIR/BepInEx/$name" "$BACKUP_DIR/$name"
        fi
    done
}

restore_configs() {
    local name
    [ -n "$BACKUP_DIR" ] && [ -d "$BACKUP_DIR" ] || return 0
    for name in wellcooked-takeover.txt wellcooked-orders.json wellcooked-command.txt; do
        if [ "$name" = wellcooked-takeover.txt ] && keeping_game; then
            # 留着的游戏下一局还要用：保持占位与 auto，前端别按 A。
            write_takeover false
        elif [ -f "$BACKUP_DIR/$name" ]; then
            cp -p "$BACKUP_DIR/$name" "$GAME_DIR/BepInEx/$name"
        else
            rm -f "$GAME_DIR/BepInEx/$name"
        fi
    done
    rm -f "$COMMAND"
}

write_takeover() { # $1 frontend_press true|false
    cat > "$TAKEOVER" <<EOF
work=auto
slots=$SLOTS
frontend_press=$1
diag=$DIAG
manual_log=false
bridge=$HOST:$PORT
EOF
}

# --------------------------------------------------------------- process control

stop_supervisor() {
    local i
    [ -n "$SUPERVISOR_PID" ] || return 0
    if kill -0 "$SUPERVISOR_PID" 2>/dev/null; then
        kill -TERM "$SUPERVISOR_PID" 2>/dev/null || true
        for i in $(seq 1 12); do
            kill -0 "$SUPERVISOR_PID" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "$SUPERVISOR_PID" 2>/dev/null || true
        wait "$SUPERVISOR_PID" 2>/dev/null || true
    fi
    SUPERVISOR_PID=""
}

stop_monitor() {
    [ -n "$MONITOR_PID" ] || return 0
    kill -TERM "$MONITOR_PID" 2>/dev/null || true
    wait "$MONITOR_PID" 2>/dev/null || true
    MONITOR_PID=""
}

stop_game() {
    local i
    if pgrep -f 'Overcooked2\.exe' >/dev/null 2>&1; then
        pkill -TERM -f 'Overcooked2\.exe' 2>/dev/null || true
        for i in $(seq 1 15); do
            pgrep -f 'Overcooked2\.exe' >/dev/null 2>&1 || break
            sleep 1
        done
        pkill -KILL -f 'Overcooked2\.exe' 2>/dev/null || true
    fi
    say "game: stopped"
}

game_pids() { pgrep -f 'Overcooked2\.exe' 2>/dev/null | sort -n | tr '\n' ' ' || true; }

sha_of() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

deploy_bundles() {
    local bundle
    mkdir -p "$DEST_LEVEL"
    for bundle in "$SCENE" info_wellcooked_demo; do
        if [ -f "$DEST_LEVEL/$bundle" ]; then
            cp -p "$DEST_LEVEL/$bundle" "$RT/bundles.previous/$bundle"
        fi
        cp -p "$BUNDLE_TMP/$bundle" "$DEST_LEVEL/$bundle"
    done
    say "deploy: bundles $SCENE info_wellcooked_demo ($BUNDLE_REF)"
}

kept_get() { sed -n "s/^$1=//p" "$KEPT" 2>/dev/null | head -n 1; }

# 留着的游戏还能不能直接开下一局：同一个游戏进程、同样多的厨师已加入、插件和关卡包没变、
# 插件还在写 state。任一不满足就重开游戏（换插件/关卡包不能在游戏运行中做）。
reuse_check() {
    local pids age bundle
    pids="$(game_pids)"
    [ -n "$pids" ] || { REUSE_WHY="game not running"; return 1; }
    [ -f "$KEPT" ] || { REUSE_WHY="no kept-game record"; return 1; }
    [ "$(kept_get pids)" = "$pids" ] || { REUSE_WHY="game pids changed"; return 1; }
    [ "$(kept_get chefs)" = "$CHEFS" ] || { REUSE_WHY="kept game has $(kept_get chefs) chefs, want $CHEFS"; return 1; }
    [ "$(kept_get plugin)" = "$(sha_of "$PLUGIN_DLL")" ] || { REUSE_WHY="plugin DLL changed"; return 1; }
    for bundle in "$SCENE" info_wellcooked_demo; do
        [ "$(sha_of "$DEST_LEVEL/$bundle")" = "$(sha_of "$BUNDLE_TMP/$bundle")" ] \
            || { REUSE_WHY="level bundle $bundle differs from the deployed one"; return 1; }
    done
    [ -f "$(kept_get launch_log)" ] || { REUSE_WHY="launch log missing"; return 1; }
    age=$(( $(date +%s) - $(stat -c %Y "$STATE" 2>/dev/null || echo 0) ))
    [ "$age" -le 5 ] || { REUSE_WHY="state.json not updated for ${age}s"; return 1; }
    return 0
}

write_kept() {
    {
        printf 'chefs=%s\n' "$CHEFS"
        printf 'pids=%s\n' "$(game_pids)"
        printf 'plugin=%s\n' "$(sha_of "$DEST_PLUGIN")"
        printf 'launch_log=%s\n' "$LAUNCH_LOG"
    } > "$KEPT"
    say "game: kept running for the next REUSE_GAME=1 run ($KEPT)"
}

# KEEP_GAME=1 时留不留游戏：本局把它带到了就绪（占满位），或本局还没碰游戏、上一局留下的记录还在。
keeping_game() {
    [ "$KEEP_GAME" = 1 ] && [ -n "$(game_pids)" ] || return 1
    [ "$GAME_READY" = 1 ] || { [ "$GAME_STARTED" = 0 ] && [ -f "$KEPT" ]; }
}

# 收尾：留游戏就连锁一起留（下一局同 LOCK_OWNER 可重入）；否则照旧关游戏、放锁。
finish_game() {
    if keeping_game; then
        [ "$GAME_READY" = 1 ] && write_kept
        say "lock: retained because KEEP_GAME=1"
        LOCK_HELD=0
        return 0
    fi
    if [ "$GAME_STARTED" = 1 ]; then
        stop_game
    fi
    rm -f "$KEPT"
    release_lock
}

cleanup() {
    [ "$CLEANED" = 1 ] && return 0
    CLEANED=1
    set +e
    stop_supervisor
    stop_monitor
    restore_configs
    finish_game
}
trap cleanup EXIT
trap 'FAIL_REASON="interrupted"; exit 130' INT TERM

# --------------------------------------------------------------- launch helpers

press_game_key() {
    local key id
    key="${1:-Return}"
    [ -n "${DISPLAY:-}" ] || return 0
    command -v xdotool >/dev/null 2>&1 || return 0
    id="$(xdotool search --name '^Overcooked2$' 2>/dev/null | head -n 1 || true)"
    if [ -n "$id" ]; then
        xdotool windowactivate --sync "$id" key --clearmodifiers "$key" \
            >/dev/null 2>&1 || true
        say "frontend: sent $key to Overcooked2 (window $id)"
    fi
}

launch_game() {
    local attempt="$1"
    if ! pgrep -x steam >/dev/null 2>&1; then
        die "Steam is not running"
    fi
    say "game: launching (attempt $attempt)"
    nohup bash "$ROOT/scripts/run-game.sh" >"$RT/launch-$attempt.log" 2>&1 &
    sleep 2
}

wait_plugin() {
    local i
    for i in $(seq 1 "$WAIT_SECONDS"); do
        if log_since | grep -aFq 'takeover: 每帧检查已挂上'; then
            say "game: plugin Takeover ready after ${i}s"
            return 0
        fi
        if [ "$i" -eq 45 ] || [ "$i" -eq 90 ]; then
            press_game_key Return
        fi
        sleep 1
    done
    return 1
}

wait_pads() {
    local i text joined all slot
    for i in $(seq 1 "$WAIT_SECONDS"); do
        text="$(log_since)"
        joined="$(printf '%s\n' "$text" | sed -nE \
            's/.*游戏已自己加入 pad [0-9]+ → 配置玩家位 [0-9]+（实际玩家位 ([0-9]+)）.*/\1/p' \
            | sort -nu)"
        all=1
        for slot in $(seq 1 "$CHEFS"); do
            if ! printf '%s\n' "$joined" | grep -qx "$slot"; then all=0; break; fi
        done
        if [ "$all" = 1 ]; then
            say "pads: joined actual slots $(printf '%s' "$joined" | tr '\n' ' ')"
            printf '%s\n' "$joined" > "$RT/pads-joined.txt"
            return 0
        fi
        if [ "$i" -eq 35 ] || [ "$i" -eq 70 ]; then
            press_game_key Return
        fi
        sleep 1
    done
    return 1
}

wait_frontend_off() {
    local i
    for i in $(seq 1 15); do
        if log_since | grep -aFq 'takeover: 前端按键 关'; then
            say "frontend: A presses off"
            return 0
        fi
        sleep 1
    done
    warn "did not see 'takeover: 前端按键 关'; continuing after 15s"
}

state_probe() { # echoes: scene chefs stations left elapsed round
    python3 - "$STATE" <<'PY' 2>/dev/null || printf '? 0 0 0 0 0\n'
import json, sys
try:
    d = json.load(open(sys.argv[1], encoding="utf-8"))
except Exception:
    print("? 0 0 0 0 0")
    raise SystemExit(0)
t = d.get("timer") or {}
print("%s %d %d %s %s %s" % (
    d.get("scene", ""), len(d.get("chefs") or []), len(d.get("stations") or []),
    t.get("left", 0), t.get("elapsed", 0), t.get("round", 0)))
PY
}

state_mtime_ns() {
    python3 -c 'import os,sys; print(os.stat(sys.argv[1]).st_mtime_ns)' "${1:-$STATE}" 2>/dev/null || printf '0'
}

wait_load_stable() {
    local before deadline streak i scene chefs stations left elapsed round mtime wait_start
    before="$1"
    deadline=$((SECONDS + WAIT_SECONDS))
    wait_start=$SECONDS
    streak=0
    while [ "$SECONDS" -lt "$deadline" ]; do
        # 复用的游戏里上一局的 state 场景名一样；要等插件在这次 load 之后重新接管厨师，
        # 且计时还有剩余（上一局结束时 left=0），才算新的一局。
        if ! log_since | awk -v s="指令: 已请求加载 $SCENE" \
                'index($0, s) { l = 1 } l && /开始接管驱动/ { f = 1 } END { exit !f }'; then
            sleep 1
            continue
        fi
        if [ "$((SECONDS - wait_start))" -ge 10 ] && [ "$(( (SECONDS - wait_start) % 15 ))" -eq 0 ]; then
            say ">>> ENTER LEVEL NOW: 若未自动跳转，请在游戏内手动进入 $SCENE 关卡 <<<"
        fi
        mtime="$(state_mtime_ns)"
        read -r scene chefs stations left elapsed round <<<"$(state_probe)"
        if [ "$mtime" -gt "$before" ] && [ "$scene" = "$SCENE" ] \
           && [ "$chefs" -eq "$CHEFS" ] && [ "$stations" -gt 0 ] \
           && awk -v x="$left" 'BEGIN { exit !(x + 0 > 1) }'; then
            streak=$((streak + 1))
            if [ "$streak" -ge 5 ]; then
                say "state: stable 5s scene=$SCENE chefs=$chefs stations=$stations"
                return 0
            fi
        else
            streak=0
        fi
        sleep 1
    done
    return 1
}

export_map() {
    local before deadline dishes
    [ "$EXPORT" = 1 ] || return 0
    [ -n "$EXPORT_SCRIPT" ] || EXPORT_SCRIPT="$SOURCE_ROOT/scripts/export-kitchen.py"
    [ -f "$EXPORT_SCRIPT" ] || die "exporter not found: $EXPORT_SCRIPT"
    before="$(state_mtime_ns)"
    printf 'menu=1\n' > "$COMMAND"
    deadline=$((SECONDS + 30))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -f "$MENU" ]; then
            mtime="$(state_mtime_ns "$MENU")"
            dishes="$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1])).get("dishes") or []))' "$MENU" 2>/dev/null || echo 0)"
            [ "$mtime" -gt "$before" ] && [ "$dishes" -gt 0 ] && break
        fi
        sleep 1
    done
    [ -f "$MENU" ] || die "menu=1 did not produce $MENU"
    say "export: $(basename "$EXPORT_SCRIPT") --map-dir $MAP_DIR"
    GAME_DIR="$GAME_DIR" python3 "$EXPORT_SCRIPT" \
        --state "$STATE" --terrain "$TERRAIN" --menu "$MENU" --map-dir "$MAP_DIR" \
        >"$RT/export.log" 2>&1 || die "map export failed (see $RT/export.log)"
    say "export: wrote $MAP_DIR/$SCENE"
}

# --------------------------------------------------------------- summary

write_summary() {
    local summary="$RT/summary.txt"
    set +e
    python3 "$ROOT/tools/real-game-summary.py" \
        --runtime "$RT" --state "$RT/final-state.json" \
        --plugin-log "$RT/LogOutput.run.log" \
        --takeover "$RT/wellcooked-takeover.final.txt" \
        --chefs "$CHEFS" --scene "$SCENE" | tee "$summary"
    SUMMARY_RC=${PIPESTATUS[0]}
    # 最后一次生效的策略（启动参数或中途切换后），供结果对比页读取。
    python3 - "$RT/policy.json" "$DISPATCH" >>"$summary" <<'PY' || true
import json, sys
try:
    with open(sys.argv[1]) as f:
        p = json.load(f)
except (OSError, ValueError):
    raise SystemExit(0)
print("policy: order=%s task=%s dispatch=%s" % (p.get("order", "?"), p.get("task", "?"), sys.argv[2]))
PY
    tail -n 1 "$summary"
    set -e
}

collect_logs() {
    cp -p "$LOG" "$RT/LogOutput.full.log" 2>/dev/null || true
    log_slice_to "$RT/LogOutput.run.log"
    cp -p "$STATE" "$RT/final-state.json" 2>/dev/null || true
    cp -p "$MENU" "$RT/wellcooked-menu.json" 2>/dev/null || true
    cp -p "$TERRAIN" "$RT/wellcooked-terrain.json" 2>/dev/null || true
    cp -p "$TAKEOVER" "$RT/wellcooked-takeover.final.txt" 2>/dev/null || true
    cp -p "$ORDERS" "$RT/wellcooked-orders.final.json" 2>/dev/null || true
    cp -p "$COMMAND" "$RT/wellcooked-command.final.txt" 2>/dev/null || true
    cp -p "$RT/status.json" "$RT/status.final.json" 2>/dev/null || true
    cp -p "$RT/supervisor.console.log" "$RT/supervisor.console.final.log" 2>/dev/null || true
}

# --------------------------------------------------------------- main

acquire_lock
mkdir -p "$ROOT/logs/real-game"
RT="$ROOT/logs/real-game/${STARTED_AT}-${TAG}"
[ ! -e "$RT" ] || die "runtime directory already exists: $RT"
mkdir -p "$RT/logs" "$RT/runs" "$RT/bundles.previous"
say "run: $RT"
say "runtime: $RT"

# Stop leftovers only after owning the lock.  Whether the game itself is stopped
# is decided after the build: a kept game is reused only when it still fits.
pkill -TERM -f "$BRIDGE_BIN_DIR/wellcooked-" 2>/dev/null || true
sleep 2
pkill -KILL -f "$BRIDGE_BIN_DIR/wellcooked-" 2>/dev/null || true
if ss -ltn 2>/dev/null | grep -q ":${PORT}\b"; then
    die "port $PORT is still in use"
fi

backup_configs

# Build only; deploying waits until we know whether a kept game is reused (a
# loaded plugin DLL must not be overwritten under the running game).
if [ "$BUILD_PLUGIN" = 1 ]; then
    say "build: plugin"
    ( cd "$SOURCE_ROOT" && GAME_DIR="$GAME_DIR" DEPLOY=0 bash scripts/build-plugin.sh ) \
        >"$RT/build-plugin.log" 2>&1 || die "plugin build failed (see $RT/build-plugin.log)"
    PLUGIN_DLL="$SOURCE_ROOT/plugin/bin/Release/net35/WellCooked.Smoke.dll"
fi
[ -f "$PLUGIN_DLL" ] || die "plugin DLL not found: $PLUGIN_DLL"
if [ "$BUILD_PLUGIN" = 0 ] && [ "$SOURCE_ROOT" = "$ROOT" ] && [ "$PLUGIN_DLL" = "$ROOT/plugin/bin/Release/net35/WellCooked.Smoke.dll" ]; then
    if [ ! -f "$PLUGIN_DLL" ] || [ "$PLUGIN_DLL" -ot "$ROOT/plugin/WorldState.cs" ] || [ "$PLUGIN_DLL" -ot "$ROOT/plugin/Link/StateFrame.cs" ]; then
        die "BUILD_PLUGIN=0 使用了过期插件；先 BUILD_PLUGIN=1 重新编译"
    fi
fi

if [ "$BUILD_BRIDGE" = 1 ]; then
    say "build: bridge"
    make -C "$SOURCE_ROOT/bridge" -j"$(nproc)" >"$RT/build-bridge.log" 2>&1 \
        || die "bridge build failed (see $RT/build-bridge.log)"
fi
for bin in wellcooked-supervisor wellcooked-bridge wellcooked-scheduler wellcooked-worker wellcooked-navd wellcooked-monitor; do
    [ -x "$BRIDGE_BIN_DIR/$bin" ] || die "missing bridge binary: $BRIDGE_BIN_DIR/$bin"
done

BUNDLE_TMP="$RT/bundles"
mkdir -p "$BUNDLE_TMP"
if [ -n "$BUNDLE_SRC" ]; then
    for bundle in "$SCENE" info_wellcooked_demo; do
        [ -f "$BUNDLE_SRC/$bundle" ] || die "missing level bundle: $BUNDLE_SRC/$bundle"
        cp -p "$BUNDLE_SRC/$bundle" "$BUNDLE_TMP/$bundle"
    done
else
    for bundle in "$SCENE" info_wellcooked_demo; do
        git -C "$SOURCE_ROOT" show "$BUNDLE_REF:levels/wellcooked_demo/$bundle"             > "$BUNDLE_TMP/$bundle" 2>/dev/null             || die "cannot read $BUNDLE_REF:levels/wellcooked_demo/$bundle"
    done
fi

rm -f "$COMMAND"
if [ "$REUSE_GAME" = 1 ] && reuse_check; then
    say "game: reusing kept Overcooked2 (pids $(game_pids)); skipping launch and pad joining"
    REUSED=1
    LAUNCH_LOG="$(kept_get launch_log)"
    GAME_STARTED=1
    GAME_READY=1
    LOG_OFFSET="$(stat -c %s "$LOG" 2>/dev/null || printf '0')"
    printf '%s\n' "$(seq 1 "$CHEFS")" > "$RT/pads-joined.txt"
    write_takeover false
else
    [ "$REUSE_GAME" = 1 ] && say "game: kept game not reusable ($REUSE_WHY); relaunching"
    say "game: stopping any previous Overcooked2 before a fresh launch"
    stop_game
    rm -f "$KEPT"
    mkdir -p "$(dirname "$DEST_PLUGIN")"
    cp -p "$PLUGIN_DLL" "$DEST_PLUGIN"
    say "deploy: plugin $(basename "$PLUGIN_DLL")"
    deploy_bundles
    if [ -f "$LOG" ]; then
        mv "$LOG" "$RT/LogOutput.previous.log"
    fi
    LOG_OFFSET=0
    write_takeover true

    GAME_STARTED=1
    launch_game 1
    if ! wait_plugin; then
        warn "plugin not ready after ${WAIT_SECONDS}s; retrying launch once"
        stop_game
        rm -f "$LOG"
        LOG_OFFSET=0
        launch_game 2
        wait_plugin || die "plugin not ready after two launches"
    fi

    # Exactly CHEFS pads join.  Pad-to-slot mapping is read from the log, so reverse
    # joining order is accepted as long as every configured slot appears once.
    wait_pads || die "only joined slots: $(cat "$RT/pads-joined.txt" 2>/dev/null || echo none)"
    write_takeover false
    wait_frontend_off
    GAME_READY=1
    # 复用的局插件日志里没有占位/工作模式行；把启动段存下来给后面每一局的汇总用。
    LAUNCH_LOG="$RT/LogOutput.launch.log"
    log_slice_to "$LAUNCH_LOG"
fi

start_supervisor() {
    say "supervisor: starting"
    "$SUPERVISOR" --foreground --verbose --runtime-dir "$RT" \
        --workers "$CHEFS" --bridge-host "$HOST" --bridge-port "$PORT" \
        --kitchen "$KITCHEN" --recipes "$RECIPES" \
        --orders "$ROOT/config/orders.world.json" --runs-dir "$RT/runs" \
        --strategy "$STRATEGY" --task-policy "$TASK_POLICY" --dispatch "$DISPATCH" \
        >"$RT/supervisor.console.log" 2>&1 &
    SUPERVISOR_PID=$!
    say "supervisor: pid $SUPERVISOR_PID (order=$STRATEGY task=$TASK_POLICY dispatch=$DISPATCH)"
    "$MONITOR" --runtime-dir "$RT" >"$RT/monitor.console.log" 2>&1 &
    MONITOR_PID=$!
    say "monitor: pid $MONITOR_PID"
}

if [ "$EXPORT" = 1 ]; then
    KITCHEN="$MAP_DIR/$SCENE/kitchen.json"
    RECIPES="$MAP_DIR/$SCENE/recipes.json"
else
    KITCHEN="$ROOT/config/maps/$SCENE/kitchen.json"
    RECIPES="$ROOT/config/maps/$SCENE/recipes.json"
    [ -f "$KITCHEN" ] || die "kitchen config missing: $KITCHEN"
    [ -f "$RECIPES" ] || die "recipes config missing: $RECIPES"
fi
before="$(state_mtime_ns)"
printf 'load=%s\n' "$SCENE" > "$COMMAND"
if [ "$EXPORT" = 1 ]; then
    wait_load_stable "$before" || die "scene $SCENE did not stay fresh/stable with $CHEFS chefs"
    export_map
    KITCHEN="$MAP_DIR/$SCENE/kitchen.json"
    RECIPES="$MAP_DIR/$SCENE/recipes.json"
    say "supervisor: early start skipped because exported configs require load completion"
else
    start_supervisor
    wait_load_stable "$before" || die "scene $SCENE did not stay fresh/stable with $CHEFS chefs"
fi
[ -f "$KITCHEN" ] || die "kitchen config missing: $KITCHEN"
[ -f "$RECIPES" ] || die "recipes config missing: $RECIPES"
if [ "$EXPORT" = 1 ]; then
    start_supervisor
fi

clock_ok=0
for i in $(seq 1 30); do
    read -r scene chefs stations left elapsed round <<<"$(state_probe)"
    if python3 -c 'import sys; raise SystemExit(0 if float(sys.argv[1]) > 0 else 1)' \
        "$round" 2>/dev/null; then
        clock_ok=1
        break
    fi
    sleep 1
done
if [ "$clock_ok" != 1 ]; then
    stop_supervisor
    die "round clock stayed at 0 after load (scene is stuck before RunKitchen)"
fi


say "round: waiting up to ${GAME_SECONDS}s for round end"
deadline=$((SECONDS + GAME_SECONDS))
round_started=0
scene_change_streak=0
end_reason=""
while [ "$SECONDS" -lt "$deadline" ]; do
    if ! kill -0 "$SUPERVISOR_PID" 2>/dev/null; then
        end_reason="supervisor-exited"
        break
    fi
    read -r scene chefs stations left elapsed round <<<"$(state_probe)"
    if [ "$scene" = "$SCENE" ] && [ "$elapsed" != "0" ] && [ "$elapsed" != "0.0" ]; then
        round_started=1
    fi
    # 插件用 File.WriteAllText 非原子写 state；读到半截文件时 state_probe 给出 "?"，
    # 不能当成换场景（实机 20261028-070912 在 elapsed=40s 被误判 scene-changed 提前收工）。
    # 连续 3 次读到别的场景才算真换场景。
    if [ "$scene" = "?" ] || [ -z "$scene" ]; then
        :
    elif [ "$round_started" = 1 ] && [ "$scene" != "$SCENE" ]; then
        scene_change_streak=$((scene_change_streak + 1))
        if [ "$scene_change_streak" -ge 3 ]; then
            end_reason="scene-changed"
            break
        fi
    else
        scene_change_streak=0
    fi
    if python3 -c 'import sys; raise SystemExit(0 if float(sys.argv[1]) > 5 and float(sys.argv[2]) <= 0.05 else 1)' \
        "$elapsed" "$left" 2>/dev/null; then
        end_reason="round-end"
        break
    fi
    sleep 2
done
sleep 3
# 上限与回合等长时（控制台 game_seconds=120 跑 120 s 关），墙钟 deadline 常比插件写出
# left=0 早一两秒；收工前再读一次，回合确实结束就按 round-end 算，不报 timeout。
if [ -z "$end_reason" ]; then
    read -r scene chefs stations left elapsed round <<<"$(state_probe)"
    if python3 -c 'import sys; raise SystemExit(0 if float(sys.argv[1]) > 5 and float(sys.argv[2]) <= 0.05 else 1)' \
        "$elapsed" "$left" 2>/dev/null; then
        end_reason="round-end"
    else
        end_reason="timeout"
    fi
fi
say "round: $end_reason"

stop_supervisor
stop_monitor
collect_logs
if [ "$REUSED" = 1 ]; then
    # 汇总要的占位/工作模式行在开游戏那一局的启动段里。
    cat "$LAUNCH_LOG" "$RT/LogOutput.run.log" > "$RT/LogOutput.run.log.tmp" 2>/dev/null \
        && mv "$RT/LogOutput.run.log.tmp" "$RT/LogOutput.run.log"
fi

restore_configs
finish_game
cp -p "$LOG" "$RT/LogOutput.full.log" 2>/dev/null || true
CLEANED=1
trap - EXIT INT TERM

write_summary
say "logs: $RT"
if [ "$end_reason" = "timeout" ]; then
    say "run: incomplete (timeout)"
    exit 1
fi
if [ "$SUMMARY_RC" -ne 0 ]; then
    say "run: summary assertions failed"
    exit 1
fi
say "run: PASS"
