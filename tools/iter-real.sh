#!/usr/bin/env bash
#
# Real-game iteration helper for WellCooked (kc-iter).
# Merges incoming branches, builds bridge & plugin, runs s_wellcooked_demo_3 (4 chefs),
# and prints summary and iteration digest.
#
# Usage:
#   bash tools/iter-real.sh [options] [branch1] [branch2] ...
# Options:
#   --dry-run       仅执行合入与编译自检，不启动游戏
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}"
LOCK="/tmp/wellcooked-game.lock"
DRY_RUN="${DRY_RUN:-0}"

# 解析参数
TARGETS=()
for arg in "$@"; do
    case "$arg" in
        --dry-run)
            DRY_RUN=1
            ;;
        *)
            TARGETS+=("$arg")
            ;;
    esac
done

# -------------------------------------------------- 1. 检查锁与人工占用
# 锁的获取与释放在运行期全权委托给 tools/run-real-game.sh（避免 double-lock 与重复释放）。
# 此处做前置审查：若锁被人类持有，严禁抢锁；若为死锁 (stale pid)，则清理；若被其他活动进程占用，则终止。
if [ -e "$LOCK" ]; then
    owner="$(cat "$LOCK/owner" 2>/dev/null || cat "$LOCK" 2>/dev/null || true)"
    owner="${owner%%$'\n'*}"
    if echo "$owner" | grep -iE 'user|manual|human' >/dev/null 2>&1; then
        echo "FAIL: 游戏锁当前由人工持有 ('$owner'): $LOCK，停止迭代" >&2
        exit 1
    fi
    # 检查持锁进程是否已消亡
    pid="$(echo "$owner" | grep -oE 'pid=[0-9]+' | cut -d= -f2 || true)"
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
        echo "WARN: 检测到僵死锁 (owner='$owner', pid $pid 已退出)，清理陈旧锁..." >&2
        rm -rf "$LOCK"
    else
        echo "FAIL: 游戏锁已被占用 ('$owner'): $LOCK，停止执行" >&2
        exit 1
    fi
fi
echo "==> 游戏锁 (/tmp/wellcooked-game.lock) 由 tools/run-real-game.sh 负责原子获取与安全释放，避免 double-lock"
# -------------------------------------------------- 2. 退出清理陷阱
# 仅针对本工作区构建的 Bridge 进程做兜底守护，不误伤其他 worktree 的测试进程。
# 游戏进程停止由 run-real-game.sh 的 GAME_STARTED 机制严格按需管理。
cleanup() {
    set +e
    pkill -f "$ROOT/bridge/build/" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

# -------------------------------------------------- 3. 合入指定分支/commit
if [ "${#TARGETS[@]}" -gt 0 ]; then
    for target in "${TARGETS[@]}"; do
        echo "==> 合入 $target..."
        if ! git -C "$ROOT" merge --no-edit "$target"; then
            echo "FAIL: 合入 $target 发生冲突，正在中止合并 (git merge --abort)..." >&2
            git -C "$ROOT" merge --abort 2>/dev/null || true
            echo "FAIL: 已中止合入 '$target'，请先在源分支解决冲突" >&2
            exit 1
        fi
    done
fi

# -------------------------------------------------- 4. 构建与单元测试
echo "==> 构建 Bridge (make -j)..."
make -C "$ROOT/bridge" -j

echo "==> 执行 Bridge 单元测试 (make test)..."
make -C "$ROOT/bridge" test

echo "==> 构建与部署插件 (Release)..."
GAME_DIR="$GAME_DIR" bash "$ROOT/scripts/build-plugin.sh"

if [ "$DRY_RUN" = 1 ]; then
    echo "==> Dry-run 完成：合入与构建测试均已通过，跳过启动游戏。"
    exit 0
fi

# -------------------------------------------------- 5. 执行实机场景
# 进关机制说明：
#   run-real-game.sh 通过向 BepInEx/wellcooked-command.txt 写入 load=s_wellcooked_demo_3 触发
#   ServerMessenger.LoadLevel 自动切关。若 10s 内未完成切关，会在终端打印 ENTER LEVEL NOW
#   提示人工在游戏内点击进入。
echo "==> 启动实机运行 (s_wellcooked_demo_3, 4 chefs)..."
set +e
SCENE=s_wellcooked_demo_3 CHEFS=4 SOURCE_ROOT="$ROOT" bash "$ROOT/tools/run-real-game.sh"
GAME_RC=$?
set -e

# -------------------------------------------------- 6. 输出报告与诊断 Digest
RUN_DIR="$(ls -td "$ROOT/logs/real-game/"* 2>/dev/null | head -n 1 || true)"
if [ -n "$RUN_DIR" ] && [ -d "$RUN_DIR" ]; then
    echo ""
    echo "============================================================"
    echo ">> REAL GAME SUMMARY"
    echo "============================================================"
    if [ -f "$RUN_DIR/summary.txt" ]; then
        cat "$RUN_DIR/summary.txt"
    else
        echo "（未找到 summary.txt）"
    fi

    echo ""
    echo "============================================================"
    echo ">> ITERATION DIGEST"
    echo "============================================================"
    python3 - "$RUN_DIR" <<'PY'
import json, glob, re, sys, os
from collections import Counter

run_dir = sys.argv[1]

# 1. macro_failed by count & first 3 failures
failure_lines = []
failures = Counter()
for f in sorted(glob.glob(f"{run_dir}/logs/worker-*.log")):
    try:
        for line in open(f, errors="replace"):
            if "macro_failed" in line:
                failure_lines.append(line.strip())
                m = re.search(r"detail=(.+)$", line)
                reason = m.group(1).strip() if m else line.strip()
                failures[reason] += 1
    except OSError:
        pass
print("1. macro_failed 原因与次数统计:")
if failures:
    for reason, count in failures.most_common():
        print(f"   * {count}x: {reason}")
    print("\n   前 3 条失败完整日志行 (first 3 failures):")
    for fl in failure_lines[:3]:
        print(f"     > {fl}")
else:
    print("   * 无宏失败 (none)")

# 2. chop events details
print("\n2. 切菜动作详情 (chop completion / rename / time):")
chop_starts = []
chop_dones = []
chop_fails = []
for f in sorted(glob.glob(f"{run_dir}/logs/worker-*.log")):
    w_name = os.path.basename(f)
    try:
        for line in open(f, errors="replace"):
            if "macro=chop" in line:
                if "macro_start" in line:
                    chop_starts.append((w_name, line.strip()))
                elif "macro_done" in line:
                    chop_dones.append((w_name, line.strip()))
                elif "macro_failed" in line:
                    chop_fails.append((w_name, line.strip()))
    except OSError:
        pass

if chop_starts:
    for w_name, start_line in chop_starts:
        m_task = re.search(r"task=(\d+)", start_line)
        tid = m_task.group(1) if m_task else ""
        m_st = re.search(r"station=(\S+)", start_line)
        st = m_st.group(1) if m_st else ""
        m_it = re.search(r"item=(\S+)", start_line)
        it = m_it.group(1) if m_it else ""
        done_match = [l for w, l in chop_dones if f"task={tid}" in l]
        fail_match = [l for w, l in chop_fails if f"task={tid}" in l]
        if done_match:
            print(f"   * [{w_name}] task={tid} item={it} @ {st}: 完成 (DETECTED) -> {done_match[0]}")
        elif fail_match:
            print(f"   * [{w_name}] task={tid} item={it} @ {st}: 失败 (FAILED) -> {fail_match[0]}")
        else:
            print(f"   * [{w_name}] task={tid} item={it} @ {st}: 未结束/中断 -> {start_line}")
else:
    print("   * 无切菜动作记录 (none)")

# 3. pour log lines
print("\n3. 倒料 (pour) 关键日志行:")
pours = []
for f in sorted(glob.glob(f"{run_dir}/logs/worker-*.log")):
    try:
        for line in open(f, errors="replace"):
            if any(k in line for k in ("macro_pour", "pour_source", "start_cook_content", "macro_pour_source")):
                pours.append(line.strip())
    except OSError:
        pass
if pours:
    for p in pours:
        print(f"   * {p}")
else:
    print("   * 未产生倒料日志 (none)")

# 4. final pot states
print("\n4. 最终锅/灶状态 (final pot states):")
try:
    final_state = json.load(open(f"{run_dir}/final-state.json"))
    pots = final_state.get("cooking", [])
    if pots:
        for pot in pots:
            burn_in = pot.get('burn_in')
            burn_in_str = f" burn_in={burn_in:.1f}s" if burn_in is not None else ""
            print(f"   * pos={pot.get('pos')} progress={pot.get('progress'):.3f}s cooked={pot.get('cooked')} burning={pot.get('burning')}{burn_in_str}")
    else:
        print("   * 无正在烹饪/烧焦的锅对象 (none)")
except Exception as e:
    print(f"   * 读取 final-state.json 异常: {e}")

# 5. resource waits
print("\n5. 资源等待指标 (cooker / board waits):")
metrics_files = glob.glob(f"{run_dir}/runs/*/metrics.json")
if metrics_files:
    try:
        metrics = json.load(open(metrics_files[0]))
        for r in metrics.get("resources", []):
            if r.get("kind") in ("cooker", "board"):
                print(f"   * {r.get('kind')}: waits={r.get('waits')} wait_ms={r.get('wait_ms')} acquires={r.get('acquires')} in_use={r.get('in_use')} capacity={r.get('capacity')}")
    except Exception as e:
        print(f"   * 读取 metrics.json 异常: {e}")
else:
    print("   * 未找到 metrics.json")
PY
    echo "============================================================"
fi

exit "$GAME_RC"
