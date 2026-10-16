#!/usr/bin/env bash
# 依次跑完 tools/fake_plugin.py 的全部场景，每个场景独占 shm 名/日志/端口，
# 逐场景打印 PASS/FAIL，最后清理残留的 Bridge 进程与 /dev/shm 对象。
#
# 隔离：shm/sem/日志名都带本次运行的 run-id（默认本脚本 pid，--run-id 可覆盖），
# 所以两个 runner 同时跑也不会抢 IPC 对象、误杀对方的进程；端口默认交内核分配。
# 任一场景失败 -> 退出码非 0；场景被跳过（前置产物缺失）默认也算未通过，
# 想让"跳过"不挡路就用 --allow-skip。场景自己的结论只在 fake_plugin.py 里下，
# 这个脚本本身不做断言，只负责编排、隔离和清理。
#
# 用法:
#   bash tools/run-bridge-scenarios.sh                 # 全部场景
#   bash tools/run-bridge-scenarios.sh --only happy    # 只跑一个
#   bash tools/run-bridge-scenarios.sh --strict        # soft 检查也当硬断言
#   bash tools/run-bridge-scenarios.sh -- --duration 8 # '--' 之后的参数透传给 fake_plugin

set -u -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PY=${PYTHON:-python3}
BRIDGE=${KITCHENOS_BRIDGE:-$ROOT/bridge/build/kitchenos-bridge}
STUB=${KITCHENOS_WORKER_STUB:-$ROOT/bridge/build/kitchenos-worker-stub}
LOGDIR=${KITCHENOS_LOG_DIR:-$ROOT/logs}
PREFIX=kitchenos_c04
RUN_ID=${KITCHENOS_RUN_ID:-$$}
SCENARIOS=(happy one-slot no-heartbeat bad-frames connect-mode reconnect-storm
           silent-connect data-shapes second-peer pickup-dash input-grace)
SCEN_TIMEOUT=90
STRICT=0
ALLOW_SKIP=0
RETRY_KILLED=1
ONLY=()
LISTEN_PORT=""
CONNECT_PORT=""
PASSTHRU=()

usage() {
    sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

选项:
  --only NAME        只跑指定场景（可重复，也可用逗号分隔）
  --timeout SECS     单个场景的超时（默认 90，用 timeout 兜底，缺 Bridge 也不会挂住）
  --bridge PATH      kitchenos-bridge 路径
  --worker-stub PATH kitchenos-worker-stub 路径
  --log-dir DIR      场景日志目录（默认 <repo>/logs）
  --strict           透传 --strict 给 fake_plugin：soft 检查升级为硬断言
  --allow-skip       有场景被跳过时仍然退出 0（默认跳过算未通过）
  --listen-port P    固定 listen 模式端口（默认自动挑空闲端口，避免和别的 Bridge 抢 14455）
  --connect-port P   固定 connect 模式端口（默认优先 14455，被占则换空闲端口）
  --run-id ID        shm/sem/日志名的运行标识（默认本脚本 pid；同机并发跑要各不相同）
  --retry-killed N   场景被外部信号杀掉（不是超时、不是断言失败）时自动重跑几次（默认 1）
  --list             只列出场景名
  -h, --help         显示本帮助
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --only) ONLY+=("${2:-}"); shift 2 ;;
        --only=*) ONLY+=("${1#*=}"); shift ;;
        --timeout) SCEN_TIMEOUT=${2:-90}; shift 2 ;;
        --timeout=*) SCEN_TIMEOUT=${1#*=}; shift ;;
        --bridge) BRIDGE=${2:-}; shift 2 ;;
        --bridge=*) BRIDGE=${1#*=}; shift ;;
        --worker-stub) STUB=${2:-}; shift 2 ;;
        --worker-stub=*) STUB=${1#*=}; shift ;;
        --log-dir) LOGDIR=${2:-}; shift 2 ;;
        --log-dir=*) LOGDIR=${1#*=}; shift ;;
        --listen-port) LISTEN_PORT=${2:-}; shift 2 ;;
        --listen-port=*) LISTEN_PORT=${1#*=}; shift ;;
        --connect-port) CONNECT_PORT=${2:-}; shift 2 ;;
        --connect-port=*) CONNECT_PORT=${1#*=}; shift ;;
        --run-id) RUN_ID=${2:-}; shift 2 ;;
        --run-id=*) RUN_ID=${1#*=}; shift ;;
        --retry-killed) RETRY_KILLED=${2:-1}; shift 2 ;;
        --retry-killed=*) RETRY_KILLED=${1#*=}; shift ;;
        --strict) STRICT=1; shift ;;
        --allow-skip) ALLOW_SKIP=1; shift ;;
        --list) printf '%s\n' "${SCENARIOS[@]}"; exit 0 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; PASSTHRU=("$@"); break ;;
        *) echo "未知参数: $1（--help 看用法）" >&2; exit 2 ;;
    esac
done

# --only 支持重复与逗号分隔
SELECTED=()
if [ ${#ONLY[@]} -gt 0 ]; then
    for chunk in "${ONLY[@]}"; do
        IFS=',' read -r -a parts <<<"$chunk"
        for part in "${parts[@]}"; do
            [ -n "$part" ] && SELECTED+=("$part")
        done
    done
else
    SELECTED=("${SCENARIOS[@]}")
fi

for name in "${SELECTED[@]}"; do
    found=0
    for known in "${SCENARIOS[@]}"; do
        [ "$name" = "$known" ] && found=1
    done
    if [ "$found" -eq 0 ]; then
        echo "未知场景: $name（可选: ${SCENARIOS[*]}）" >&2
        exit 2
    fi
done

mkdir -p "$LOGDIR"

if [ ! -f "$BRIDGE" ]; then
    echo "FAIL: Bridge 二进制不存在: $BRIDGE" >&2
    echo "      先构建: cd bridge && make   （或 --bridge PATH / KITCHENOS_BRIDGE 指定）" >&2
    exit 2
fi
if [ ! -x "$BRIDGE" ]; then
    echo "FAIL: Bridge 不可执行: $BRIDGE" >&2
    exit 2
fi
if ! command -v "$PY" >/dev/null 2>&1; then
    echo "FAIL: 找不到 python3（用 -- 环境变量 PYTHON 指定）" >&2
    exit 2
fi
if ! command -v timeout >/dev/null 2>&1; then
    echo "FAIL: 找不到 timeout，无法给场景设上限" >&2
    exit 2
fi

if [ -f "$STUB" ] && [ -x "$STUB" ]; then
    echo "worker-stub: $STUB"
else
    echo "worker-stub: 缺失 ($STUB) — one-slot 会被跳过，happy 只走 idle 路径"
fi
echo "bridge:      $BRIDGE"
echo "logs:        $LOGDIR"
echo "run-id:      $RUN_ID (shm/sem/日志名都带它，多实例并行互不干扰)"
echo "scenarios:   ${SELECTED[*]}"
echo

declare -a RESULTS=()
PASSED=0
FAILED=0
SKIPPED=0
FAILED_NAMES=()

# 只清掉本场景自己的进程和 IPC 对象。
# 模式必须以二进制路径开头（^）：否则 "kitchenos-bridge .*--state-name X" 会连
# 正在跑这个场景的 python3 一起匹配上（它的命令行里既有 --bridge 路径又有
# --state-name），自己把自己的场景 SIGKILL 掉，症状是"场景 0 秒超时"。
cleanup_scenario() {
    local state=$1 sem=$2
    local bp=${BRIDGE//./\\.} sp=${STUB//./\\.}
    pkill -f -- "^${bp} .*--state-name ${state}( |$)" 2>/dev/null
    pkill -f -- "^${sp} .*--state-name ${state}( |$)" 2>/dev/null
    sleep 0.2
    pkill -9 -f -- "^${bp} .*--state-name ${state}( |$)" 2>/dev/null
    pkill -9 -f -- "^${sp} .*--state-name ${state}( |$)" 2>/dev/null
    rm -f "/dev/shm/${state}" "/dev/shm/${sem}" "/dev/shm/sem.${sem}" 2>/dev/null
}

# 只扫自己这次运行留下的对象；别人（另一个 run-id、别的 agent）的对象只报告不删。
final_cleanup() {
    local obj others=()
    for obj in /dev/shm/${PREFIX}_${RUN_ID}_* /dev/shm/sem.${PREFIX}_${RUN_ID}_*; do
        [ -e "$obj" ] || continue
        echo "清理残留 IPC 对象: $obj"
        rm -f "$obj"
    done
    for obj in /dev/shm/${PREFIX}_*; do
        [ -e "$obj" ] || continue
        case "$obj" in
            /dev/shm/${PREFIX}_${RUN_ID}_*) ;;
            *) others+=("$obj") ;;
        esac
    done
    if [ ${#others[@]} -gt 0 ]; then
        echo "注意: /dev/shm 里还有不是本次运行的 ${PREFIX}_* 对象（没动它们）: ${others[*]}"
    fi
}
trap final_cleanup EXIT

run_one() {
    local name=$1
    # 前导 '/' 与 shm.h 的默认名 "/kitchenos_state" 一致；glibc 只接受单个前导斜杠
    local state="/${PREFIX}_${RUN_ID}_${name}"
    local sem="${state}_ready"
    local log="$LOGDIR/bridge-${RUN_ID}-${name}.log"
    local out="$LOGDIR/scenario-${RUN_ID}-${name}.out"
    local extra=()
    [ "$STRICT" -eq 1 ] && extra+=(--strict)
    [ -n "$LISTEN_PORT" ] && extra+=(--listen-port "$LISTEN_PORT")
    [ -n "$CONNECT_PORT" ] && extra+=(--connect-port "$CONNECT_PORT")

    echo "----------------------------------------------------------------"
    echo ">> $name  (state=$state, log=$log)"

    local attempt=0 rc=0 elapsed=0 start=0 signal=0 interrupted=0
    while :; do
        attempt=$((attempt + 1))
        cleanup_scenario "$state" "$sem"
        : >"$log" 2>/dev/null || true
        start=$SECONDS
        ## fake_plugin.py 自己判定成败；timeout 只是不让缺 Bridge 的运行挂死在这里
        timeout --signal=TERM --kill-after=5 "${SCEN_TIMEOUT}s" \
            "$PY" "$ROOT/tools/fake_plugin.py" \
            --scenario "$name" \
            --bridge "$BRIDGE" \
            --worker-stub "$STUB" \
            --state-name "$state" \
            --sem-name "$sem" \
            --log "$log" \
            "${extra[@]}" "${PASSTHRU[@]}" >"$out" 2>&1
        rc=$?
        elapsed=$((SECONDS - start))
        signal=0
        [ "$rc" -ge 128 ] && [ "$rc" -le 192 ] && signal=$((rc - 128))
        # timeout 自己收到 SIGTERM/SIGKILL 时也退出 124/137（本机实测），所以
        # "rc=124 却远没跑满上限"同样不是场景挂死，而是外部信号打到了 timeout
        # 或场景进程上（另一个 runner 的无锚点 pkill、OOM）—— 和上一段区别开，
        # 只影响结论怎么写，不影响判定：真正的超时一定跑满 SCEN_TIMEOUT。
        interrupted=0
        if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
            [ "$elapsed" -lt "$((SCEN_TIMEOUT - 5))" ] && interrupted=1
        fi
        # 被外部信号打死（别的 agent 的 pkill 横扫、手动 kill）既不是超时也不是
        # 断言结论，直接当 FAIL 会冤枉 Bridge；重跑一次，并把这件事说出来。
        if { [ "$signal" -gt 0 ] || [ "$interrupted" -eq 1 ]; } \
           && [ "$elapsed" -lt "$SCEN_TIMEOUT" ] \
           && [ "$attempt" -le "$RETRY_KILLED" ]; then
            echo "    注意: 第 ${attempt} 次跑到 ${elapsed}s 时被外部打断" \
                 "（rc=$rc signal=$signal，不是超时、也不是断言失败），重跑"
            continue
        fi
        break
    done

    # 场景自己的输出原样给人看，缩进以区分层级
    sed 's/^/    /' "$out"
    if [ "$attempt" -gt 1 ]; then
        echo "    （本场景共跑了 ${attempt} 次：前 $((attempt - 1)) 次被外部打断）"
    fi

    case "$rc" in
        0)
            echo "PASS $name (${elapsed}s)"
            RESULTS+=("PASS $name (${elapsed}s)")
            PASSED=$((PASSED + 1))
            ;;
        77)
            echo "SKIP $name (${elapsed}s) — 前置产物缺失，未验证"
            RESULTS+=("SKIP $name (${elapsed}s)")
            SKIPPED=$((SKIPPED + 1))
            ;;
        124|137)
            if [ "$interrupted" -eq 1 ]; then
                echo "FAIL $name (${elapsed}s) — rc=$rc 且远未跑满 ${SCEN_TIMEOUT}s：" \
                     "外部信号打到了 timeout/场景进程上（不是场景超时）" >&2
                RESULTS+=("FAIL $name (killed)")
                FAILED=$((FAILED + 1))
                FAILED_NAMES+=("$name(killed)")
            else
                echo "FAIL $name (${elapsed}s) — 超时 ${SCEN_TIMEOUT}s 未结束（timeout 杀掉）" >&2
                RESULTS+=("FAIL $name (timeout)")
                FAILED=$((FAILED + 1))
                FAILED_NAMES+=("$name(timeout)")
            fi
            ;;
        *)
            echo "FAIL $name (${elapsed}s) — fake_plugin 退出码 $rc" >&2
            RESULTS+=("FAIL $name (rc=$rc)")
            FAILED=$((FAILED + 1))
            FAILED_NAMES+=("$name")
            ;;
    esac
    cleanup_scenario "$state" "$sem"
}

for name in "${SELECTED[@]}"; do
    run_one "$name"
done

echo "================================================================"
for line in "${RESULTS[@]}"; do
    echo "$line"
done
echo "----------------------------------------------------------------"
echo "合计: ${#SELECTED[@]} 个场景, 通过 $PASSED, 失败 $FAILED, 跳过 $SKIPPED"
echo "日志与场景输出: $LOGDIR"

leftover=$(ls /dev/shm 2>/dev/null | grep -E "^${PREFIX}_${RUN_ID}_" || true)
if [ -n "$leftover" ]; then
    echo "注意: /dev/shm 仍有本次运行的 IPC 对象: $(echo "$leftover" | tr '\n' ' ')"
else
    echo "/dev/shm: 本次运行的 IPC 对象已清空"
fi
stray=$(pgrep -af "kitchenos-(bridge|worker-stub)" || true)
if [ -n "$stray" ]; then
    echo "注意: 仍有 kitchenos 进程在跑（不一定是本工具的，可能属于其他 agent 的测试）:"
    echo "$stray"
else
    echo "进程: 没有残留的 kitchenos-bridge / kitchenos-worker-stub"
fi

if [ "$FAILED" -gt 0 ]; then
    echo "结论: 失败 ${FAILED} 个场景: ${FAILED_NAMES[*]}" >&2
    exit 1
fi
if [ "$SKIPPED" -gt 0 ] && [ "$ALLOW_SKIP" -eq 0 ]; then
    echo "结论: 有 ${SKIPPED} 个场景被跳过，不能算通过（--allow-skip 可放行）" >&2
    exit 1
fi
echo "结论: 全部通过"
exit 0
