#!/usr/bin/env bash
#
# One-command real-game iteration for KitchenOS.
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
#   REUSE_GAME=1            keep an already-running game instead of relaunching
#   EXPORT=1                menu=1 + scripts/export-kitchen.py --map-dir MAP_DIR
#   MAP_DIR=...             default <this repo>/config/maps
#   KEEP_GAME=1             do not stop the game at the end (lock stays held)
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
SUPERVISOR="$BRIDGE_BIN_DIR/kitchenos-supervisor"
MONITOR="$BRIDGE_BIN_DIR/kitchenos-monitor"
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
        if [ -f "$BACKUP_DIR/$name" ]; then
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

cleanup() {
    [ "$CLEANED" = 1 ] && return 0
    CLEANED=1
    set +e
    stop_supervisor
    stop_monitor
    if [ "$GAME_STARTED" = 1 ] && [ "$KEEP_GAME" != 1 ]; then
        stop_game
    fi
    restore_configs
    release_lock
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
        if ! log_since | grep -aFq "指令: 已请求加载 $SCENE"; then
            sleep 1
            continue
        fi
        if [ "$((SECONDS - wait_start))" -ge 10 ] && [ "$(( (SECONDS - wait_start) % 15 ))" -eq 0 ]; then
            say ">>> ENTER LEVEL NOW: 若未自动跳转，请在游戏内手动进入 $SCENE 关卡 <<<"
        fi
        mtime="$(state_mtime_ns)"
        read -r scene chefs stations left elapsed round <<<"$(state_probe)"
        if [ "$mtime" -gt "$before" ] && [ "$scene" = "$SCENE" ] \
           && [ "$chefs" -eq "$CHEFS" ] && [ "$stations" -gt 0 ]; then
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

# Stop leftovers only after owning the lock.  The fixture/recording game is
# stopped as well unless REUSE_GAME=1.
if [ "$REUSE_GAME" != 1 ]; then
    say "game: stopping any previous Overcooked2 before a fresh launch"
    stop_game
fi
pkill -TERM -f "$BRIDGE_BIN_DIR/kitchenos-" 2>/dev/null || true
sleep 2
pkill -KILL -f "$BRIDGE_BIN_DIR/kitchenos-" 2>/dev/null || true
if ss -ltn 2>/dev/null | grep -q ":${PORT}\b"; then
    die "port $PORT is still in use"
fi

backup_configs
if [ -f "$LOG" ]; then
    mv "$LOG" "$RT/LogOutput.previous.log"
fi
LOG_OFFSET=0

# Deploy the plugin and the two files for this scene.
if [ "$BUILD_PLUGIN" = 1 ]; then
    say "build: plugin"
    ( cd "$SOURCE_ROOT" && GAME_DIR="$GAME_DIR" bash scripts/build-plugin.sh ) \
        >"$RT/build-plugin.log" 2>&1 || die "plugin build failed (see $RT/build-plugin.log)"
    PLUGIN_DLL="$SOURCE_ROOT/plugin/bin/Release/net35/WellCooked.Smoke.dll"
fi
[ -f "$PLUGIN_DLL" ] || die "plugin DLL not found: $PLUGIN_DLL"
if [ "$BUILD_PLUGIN" = 0 ] && [ "$SOURCE_ROOT" = "$ROOT" ] && [ "$PLUGIN_DLL" = "$ROOT/plugin/bin/Release/net35/WellCooked.Smoke.dll" ]; then
    if [ ! -f "$PLUGIN_DLL" ] || [ "$PLUGIN_DLL" -ot "$ROOT/plugin/WorldState.cs" ] || [ "$PLUGIN_DLL" -ot "$ROOT/plugin/Link/StateFrame.cs" ]; then
        die "BUILD_PLUGIN=0 使用了过期插件；先 BUILD_PLUGIN=1 重新编译"
    fi
fi
mkdir -p "$(dirname "$DEST_PLUGIN")"
cp -p "$PLUGIN_DLL" "$DEST_PLUGIN"
say "deploy: plugin $(basename "$PLUGIN_DLL")"

if [ "$BUILD_BRIDGE" = 1 ]; then
    say "build: bridge"
    make -C "$SOURCE_ROOT/bridge" -j"$(nproc)" >"$RT/build-bridge.log" 2>&1 \
        || die "bridge build failed (see $RT/build-bridge.log)"
fi
for bin in kitchenos-supervisor kitchenos-bridge kitchenos-scheduler kitchenos-worker kitchenos-navd kitchenos-monitor; do
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
mkdir -p "$DEST_LEVEL"
for bundle in "$SCENE" info_wellcooked_demo; do
    if [ -f "$DEST_LEVEL/$bundle" ]; then
        cp -p "$DEST_LEVEL/$bundle" "$RT/bundles.previous/$bundle"
    fi
    cp -p "$BUNDLE_TMP/$bundle" "$DEST_LEVEL/$bundle"
done
say "deploy: bundles $SCENE info_wellcooked_demo ($BUNDLE_REF)"

write_takeover true
rm -f "$COMMAND"

# Launch (or reuse) and wait for the plugin.
if [ "$REUSE_GAME" = 1 ] && pgrep -f 'Overcooked2\.exe' >/dev/null 2>&1; then
    say "game: reusing running Overcooked2"
    GAME_STARTED=1
    LOG_OFFSET="$(stat -c %s "$LOG" 2>/dev/null || printf '0')"
    wait_plugin || die "plugin not ready in the reused game"
else
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
fi

# Exactly CHEFS pads join.  Pad-to-slot mapping is read from the log, so reverse
# joining order is accepted as long as every configured slot appears once.
wait_pads || die "only joined slots: $(cat "$RT/pads-joined.txt" 2>/dev/null || echo none)"
write_takeover false
wait_frontend_off

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
[ -n "$end_reason" ] || end_reason="timeout"
say "round: $end_reason"
sleep 3

stop_supervisor
stop_monitor
collect_logs

if [ "$KEEP_GAME" != 1 ]; then
    stop_game
    cp -p "$LOG" "$RT/LogOutput.full.log" 2>/dev/null || true
fi
restore_configs
if [ "$KEEP_GAME" = 1 ]; then
    say "lock: retained because KEEP_GAME=1"
    LOCK_HELD=0
else
    release_lock
fi
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
