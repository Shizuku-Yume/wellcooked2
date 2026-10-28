#!/usr/bin/env bash
# 任务 05 的第一步：两名厨师各自被外部指令驱动，互不串扰，位置连续不跳变。
#
# 用法: bash scripts/demo-multi-chef.sh [秒数] [场景]
#   默认 20 秒、场景 s_sushi_1_1（spec 开放问题"第一版固定地图"的默认值）。
#
# 判据（脚本自己断言，失败返回非零；以前只看输出、永远退 0，等于没有检查）：
#   1) 两个厨师都走到**各自**的目标附近（0.8 米以内），且离对方目标至少 1.5 米
#      —— 同一个 action 没有串到另一个 chef_id；
#   2) 采样里没有跳变：按状态文件自己的时间戳归一化后速度不超过 5 米/秒，
#      同时保留原来的"单步位移 > 1.5 米"提示（采样间隔被 IO 拉长时看速度）；
#   3) 桥侧每帧 actions_sent 在两个 stub 都活着的窗口里是 move=2（不是合成的 stop）。
#
# 目标点不是写死的：从状态文件读厨师当前位置，再在 `wellcooked-terrain.json` 的
# 可走格里选同一排、约 3 米远的一格。写死坐标会随出生点和上一轮残留位置漂移，
# 而且可能落在墙后面——第一次跑就是这样：一个厨师在 9.4 米外卡住，另一个在两排之间绕。
set -uo pipefail

DURATION=${1:-20}
SCENE=${2:-s_sushi_1_1}
GAME_DIR=${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$ROOT/bridge/build/kitchenos-bridge
STUB=$ROOT/bridge/build/kitchenos-worker-stub
BLOG=$ROOT/logs/bridge-demo-multi.log
STATE=$GAME_DIR/BepInEx/wellcooked-state.json
TERRAIN=$GAME_DIR/BepInEx/wellcooked-terrain.json
TAKEOVER=$GAME_DIR/BepInEx/wellcooked-takeover.txt

BRIDGE_PID=""
STUB_PIDS=()
TAKEOVER_EXISTED=0
ORIGINAL_TAKEOVER=""
[ -f "$TAKEOVER" ] && { TAKEOVER_EXISTED=1; ORIGINAL_TAKEOVER=$(cat "$TAKEOVER"); }

set_key() { # $1 = 键，$2 = 值；只改这一行，其余保持不动
    local tmp
    tmp=$(mktemp)
    grep -vE "^[[:space:]]*$1[[:space:]]*=" "$TAKEOVER" > "$tmp" 2>/dev/null || true
    printf '%s=%s\n' "$1" "$2" >> "$tmp"
    mv "$tmp" "$TAKEOVER"
}

cleanup() {
    for pid in "${STUB_PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
    [ -n "$BRIDGE_PID" ] && kill "$BRIDGE_PID" 2>/dev/null || true
    if [ "$TAKEOVER_EXISTED" = "1" ]; then
        printf '%s\n' "$ORIGINAL_TAKEOVER" > "$TAKEOVER"
    else
        rm -f "$TAKEOVER"
    fi
}
trap cleanup EXIT

# 动作到达后 `ActionDriver` 会接管玩家位，本地驱动（工序/来回走）不再碰它；
# 但动作到达前最小策略会自己走、自己按交互。这里关掉工序模式，少一层干扰。
set_key work off

if ! pgrep -f 'Overcooked2.exe' >/dev/null 2>&1; then
    echo "游戏没在跑，先进关卡（$SCENE）..."
    bash "$ROOT/scripts/dev-cycle.sh" 12 "$SCENE" >/dev/null 2>&1 || true
fi
xdotool search --name Overcooked2 windowactivate --sync 2>/dev/null || true

pkill -f kitchenos-bridge >/dev/null 2>&1
sleep 1
: > "$BLOG"
"$BIN" --link listen --host 127.0.0.1 --port 14455 --log "$BLOG" >/dev/null 2>&1 &
BRIDGE_PID=$!
for _ in $(seq 1 15); do
    sleep 1
    grep -aq 'plugin_hello' "$BLOG" && break
done
grep -aq 'plugin_hello' "$BLOG" || { echo "插件没连上，看 $BLOG"; exit 1; }

# 等状态文件和地形都对应到这一关，且至少两名厨师。
python3 - "$STATE" "$TERRAIN" "$SCENE" <<'PY'
import json, sys, time
state, terrain, scene = sys.argv[1], sys.argv[2], sys.argv[3]
deadline = time.time() + 30
while time.time() < deadline:
    try:
        d = json.load(open(state))
        t = json.load(open(terrain))
        if d.get('scene') == scene and len(d.get('chefs') or []) >= 2 and t.get('scene') == scene:
            sys.exit(0)
    except Exception:
        pass
    time.sleep(0.5)
print('30 秒内没等到 %s 的两名厨师/地形（看 %s）' % (scene, state), file=sys.stderr)
sys.exit(1)
PY
[ $? -eq 0 ] || exit 1

# 先用 stop 把两个玩家位从本地驱动手里收回来：`ActionDriver` 收到 stop 后保留归属，
# 后面的探测和实测期间没人再插手（work=off 只是不再跑工序，最小策略仍会驱动无人接管的位）。
# 然后重开当前关：上一轮结束时厨师可能停在墙角（所有边都被记为过不去），
# 从那种位置连 1.2 米的目标都探不到路；出生点是确定的，重开一次测试才可重复。
for slot in 0 1; do
    "$STUB" --state-name /kitchenos_state --sem-name /kitchenos_input_ready --chefs 2 \
        --slot "$slot" --kind stop --duration 30 >/dev/null 2>&1 &
    STUB_PIDS+=($!)
done
sleep 1
printf 'restart=1\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"
for _ in $(seq 1 20); do
    [ -f "$GAME_DIR/BepInEx/wellcooked-command.txt" ] || break
    sleep 0.5
done
sleep 6

echo "起点:"
python3 - "$STATE" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for c in d['chefs']:
    print("  %-4s %s" % (c['player'], c['pos']))
PY

# 选目标：在可走格里挑一个 1~3 米远、**插件自己的 A\* 认为可达**的格子。
# 不能只看 wellcooked-terrain.json 的 mask：任务 08 的运行时网格会记住过不去的边，
# 实测同一排的两个方向，一边能走、另一边第一路点被挡；那次目标只能靠 `path=` 探。
TARGETS=$(python3 - "$STATE" "$TERRAIN" "$GAME_DIR" "$GAME_DIR/BepInEx/LogOutput.log" <<'PY'
import json, math, os, re, sys, time

state_path, terrain_path, game_dir, log_path = sys.argv[1:5]
state = json.load(open(state_path))
terrain = json.load(open(terrain_path))
cells = [(round(c[0], 2), round(c[1], 2)) for c in terrain['cells']]
chefs = state['chefs'][:2]
pos = [(c['pos'][0], c['pos'][1]) for c in chefs]
command = game_dir + '/BepInEx/wellcooked-command.txt'

def nearest(p):
    return min(cells, key=lambda c: (c[0] - p[0]) ** 2 + (c[1] - p[1]) ** 2)

def candidates(p, other):
    cx, cz = nearest(p)
    rows = {}
    for x, z in cells:
        rows.setdefault(z, set()).add(x)
    out = []
    for axis_index, axis in enumerate(('x', 'z')):
        if axis == 'x':
            line = rows.get(cz, set())
            fixed = cz
        else:
            line = set(z for x, z in cells if abs(x - cx) < 0.01)
            fixed = cx
        for step in (2, 3, 4, 5):
            for direction in (-1, 1):
                if axis == 'x':
                    target = (round(cx + direction * 0.6 * step, 2), fixed)
                    path = [round(cx + direction * 0.6 * k, 2) for k in range(1, step + 1)]
                    ok = all(q in line for q in path)
                else:
                    target = (fixed, round(cz + direction * 0.6 * step, 2))
                    path = [round(cz + direction * 0.6 * k, 2) for k in range(1, step + 1)]
                    ok = all(q in line for q in path)
                if ok:
                    # 先横排（出生点所在的那条长走道），再竖排；同一步取离对方远的方向。
                    out.append((axis_index, step, -math.hypot(target[0] - other[0], target[1] - other[1]),
                                target))
    out.sort()
    return [t for _, _, _, t in out]

def probe(start, target):
    try:
        os.remove(command)
    except OSError:
        pass
    before = os.path.getsize(log_path) if os.path.exists(log_path) else 0
    with open(command, 'w') as f:
        f.write('path=%.2f,%.2f,%.2f,%.2f\n' % (start[0], start[1], target[0], target[1]))
    time.sleep(1.6)
    try:
        with open(log_path, 'rb') as f:
            f.seek(before)
            text = f.read().decode('utf-8', 'replace')
    except OSError:
        return False, 'no log'
    if 'A* 返回 null' in text:
        return False, 'A* 不可达'
    m = re.search(r'A\* 返回 (\d+) 个路点: ([^\n]*)', text)
    if not m:
        return False, '没有探测结果'
    count = int(m.group(1))
    points = [(float(a), float(b)) for a, b in re.findall(r'\(([-\d.]+),([-\d.]+)\)', m.group(2))]
    if count == 0 or not points:
        return False, '空路径'
    # 直线路径经过 Simplify 后只剩目标点；拐弯路径会多几个拐点。这里只要求 A* 可达，
    # 具体怎么走是插件导航的事（任务 08/11）。
    return True, '%d 个路点' % count

picked = []
for i in range(2):
    other = pos[1 - i] if not picked else picked[0]
    chosen = None
    for target in candidates(pos[i], other):
        ok, why = probe(pos[i], target)
        print('  %s 候选 (%.1f,%.1f): %s%s' % (chefs[i]['player'], target[0], target[1],
                                                '可达 ' if ok else '跳过 ', '' if ok else why),
              file=sys.stderr)
        if ok:
            chosen = target
            break
    if chosen is None:
        print('选不出可达目标（看 %s）' % log_path, file=sys.stderr)
        sys.exit(1)
    picked.append(chosen)

if math.hypot(picked[0][0] - picked[1][0], picked[0][1] - picked[1][1]) < 2.0:
    print('两个目标太近，判不出串扰', file=sys.stderr)
    sys.exit(1)
print("%.2f,%.2f %.2f,%.2f" % (picked[0][0], picked[0][1], picked[1][0], picked[1][1]))
PY
)
[ $? -eq 0 ] || { echo "选不出目标点"; exit 1; }
read -r T0 T1 <<< "$TARGETS"
[ -n "$T0" ] && [ -n "$T1" ] || { echo "选不出目标点"; exit 1; }
echo "目标: One=$T0 Two=$T1"

# stop 替身必须退出，否则它和 move 替身会抢同一个输入槽。
for pid in "${STUB_PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
STUB_PIDS=()
sleep 0.5

for slot in 0 1; do
    target=${T0}
    [ "$slot" = "1" ] && target=${T1}
    "$STUB" --state-name /kitchenos_state --sem-name /kitchenos_input_ready --chefs 2 \
        --slot "$slot" --kind move --target "$target" --hz 120 --duration "$DURATION" \
        >/dev/null 2>&1 &
    STUB_PIDS+=($!)
done

python3 - "$STATE" "$DURATION" "$T0" "$T1" <<'PY'
import json, math, sys, time

state, duration, t0, t1 = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4]
targets = [tuple(float(v) for v in s.split(',')) for s in (t0, t1)]
prev = [None, None]
prev_t = [None, None]
worst_jump = 0.0
worst_speed = 0.0
samples = 0
last_print = 0.0
end = time.time() + duration
while time.time() < end:
    time.sleep(0.25)
    try:
        d = json.load(open(state))
    except Exception:
        continue
    if len(d.get('chefs') or []) < 2:
        continue
    now = d.get('t')
    row = []
    for i, chef in enumerate(d['chefs'][:2]):
        pos = chef['pos']
        dist = math.hypot(pos[0] - targets[i][0], pos[1] - targets[i][1])
        if prev[i] is not None and now is not None and prev_t[i] is not None and now > prev_t[i]:
            step = math.hypot(pos[0] - prev[i][0], pos[1] - prev[i][1])
            dt = now - prev_t[i]
            speed = step / dt
            worst_jump = max(worst_jump, step)
            worst_speed = max(worst_speed, speed)
        prev[i] = pos
        prev_t[i] = now
        row.append('%s (%.2f,%.2f) 距目标 %.2f 米' % (chef['player'], pos[0], pos[1], dist))
    samples += 1
    if time.time() - last_print >= 2:
        print('  ' + ' | '.join(row))
        last_print = time.time()

final = json.load(open(state))
chefs = final['chefs'][:2]
own = [math.hypot(c['pos'][0] - targets[i][0], c['pos'][1] - targets[i][1]) for i, c in enumerate(chefs)]
cross = [math.hypot(c['pos'][0] - targets[1 - i][0], c['pos'][1] - targets[1 - i][1]) for i, c in enumerate(chefs)]

print('采样 %d 次，单步最大位移 %.2f 米，归一化最大速度 %.2f 米/秒' % (samples, worst_jump, worst_speed))
ok = True
for i, c in enumerate(chefs):
    print('  %s: 距自己目标 %.2f 米，距对方目标 %.2f 米' % (c['player'], own[i], cross[i]))
    if own[i] > 0.8:
        print('  FAIL: %s 没走到自己的目标' % c['player'])
        ok = False
    if cross[i] < 1.5:
        print('  FAIL: %s 停在对方目标附近（疑似串扰）' % c['player'])
        ok = False
if worst_jump > 1.5 and worst_speed > 5.0:
    print('  FAIL: 单步位移 %.2f 米（%.2f 米/秒），超过跳变阈值' % (worst_jump, worst_speed))
    ok = False
if worst_speed > 5.0:
    print('  FAIL: 归一化速度 %.2f 米/秒，超过 5 米/秒（丢输入/瞬移）' % worst_speed)
    ok = False
sys.exit(0 if ok else 1)
PY
SAMPLE_RC=$?

echo -n "桥侧每帧分类（stub 窗口内）: "
grep -a 'actions_sent' "$BLOG" | tail -40 | grep -oE 'idle=[0-9]+ move=[0-9]+' | sort | uniq -c | tr '\n' ' '
echo
if ! grep -aqE 'idle=[0-9]+ move=2' "$BLOG"; then
    echo "FAIL: 桥侧没有出现 move=2（两个厨师都收到动作）"
    exit 1
fi
exit "$SAMPLE_RC"
