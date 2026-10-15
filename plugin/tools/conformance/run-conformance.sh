#!/usr/bin/env bash
#
# run-conformance.sh —— 跨实现一致性测试的 WSL 半场：
# 起 C 桥，用「由插件自己的协议代码编译出来的对端」驱动它，并判定双方。
#
# 在 WSL 里、仓库根目录下运行：
#
#     bash plugin/tools/conformance/run-conformance.sh [port] [seconds] [require-kinds] [stub-kind]
#
# 默认：端口 12401，60 Hz 跑 3 秒，要求出现 move+stop，stub 往 slot 0 写 move。端口特意不用
# 14455，这样真插件或 W8 运行可以同时在跑。
#
# `stub-kind` 可以是逗号分隔的一到两个动作名（move/stop/interact/pickup/dash），逐个写进
# slot 0、slot 1；用它来让"扩过的 action 枚举"也被插件自己的解码器验一遍，例如：
#
#     powershell -NoProfile -File plugin/tools/conformance/run-conformance.ps1 \
#         -RequireKinds pickup,dash -StubKind pickup,dash
#
# 先编译对端（它是 Windows 程序；见 run-conformance.ps1——它用 csc 编译完再来调本脚本）：
#
#     powershell -NoProfile -File plugin/tools/conformance/run-conformance.ps1
#
# 本脚本为什么不自己编译对端：工作目录是本仓库时，csc.exe 无法经 WSL interop 驱动，
# 因为检出路径不是 ASCII——它打不开传给它的任何一个源文件（报 "Source file '<仓库>\PluginPeer.cs'
# could not be opened"）。这是 interop 的事实，不是源码的问题；从 Windows 侧编译正常，
# 所以 run-conformance.ps1 在那里编译，跨边界只传 ASCII 参数。
#
# 这半场为什么用 bash 而不是 PowerShell：桥是 Linux 进程，对端经 WSL interop 运行，
# 只有一个 shell 能同时管住两个进程的生命周期、两个退出码和日志。用 PowerShell 把 shell 脚本
# 交给 `wsl.exe -c` 是行不通的：wsl.exe 用空格拼接参数，会静默吃掉引号，跑起来的是别的东西。
#
# 共享内存名是本次测试专用的，所以这一轮既不会撞上、也不会杀掉别人按默认名留着的桥。
#
# 它检查什么，什么情况算失败：
#   * 桥到达 net_ready 并接受两次连接（阶段 1 的探针、阶段 2 的 TcpLink 会话）；
#   * 桥解析了每一个探针帧——拆分帧、粘包帧、容量探针帧，各自靠 game_time 标记在桥
#     自己的 verbose 日志里被认出来；
#   * 容量探针帧的 dropped=/truncated= 计数等于预测值，这样「计数器根本没接上」会被抓住；
#   * 日志里一条 parse_error、一条 frame_error 都没有；
#   * 对端解出了每一条 action，厨师号都是它上报过的，并且退出码为 0。

set -u

# 对端故意会打印一个非 ASCII 字符（echo 的 token），所以别让 locale 把它的输出变成
# grep 眼里的二进制文件。
export LC_ALL="${LC_ALL:-C.UTF-8}"

PORT="${1:-12401}"
STREAM_SECONDS="${2:-3}"
REQUIRE_KINDS="${3:-move,stop}"
STUB_KIND="${4:-move}"

IFS=',' read -r -a STUB_KINDS <<< "$STUB_KIND"
if [ "${#STUB_KINDS[@]}" -eq 0 ] || [ "${#STUB_KINDS[@]}" -gt 2 ]; then
    printf 'stub-kind 要给 1 到 2 个动作名（桥只开 2 个厨师），收到：%s\n' "$STUB_KIND"
    exit 3
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT" || exit 3

# ---- 被测产物（由 run-conformance.ps1 编译）--------------------------------
PEER_SRC="plugin/tools/conformance/PluginPeer.cs"
PEER_EXE="plugin/tools/conformance/bin/PluginPeer.exe"
MESSAGES_SRC="plugin/WellCooked.Plugin/Protocol/Messages.cs"
TCPLINK_SRC="plugin/WellCooked.Plugin/Net/TcpLink.cs"

BRIDGE_BIN="bridge/build/kitchenos-bridge"
STUB_BIN="bridge/build/kitchenos-worker-stub"

# ---- 证据文件（logs/ 已 gitignore：这些都是运行产物）------------------------
LOG="logs/conformance-bridge.log"
BRIDGE_OUT="logs/conformance-bridge.out"
STUB_OUT="logs/conformance-stub.out"
PEER_OUT="logs/conformance-peer.out"
CSC_OUT="logs/conformance-csc.out"   # written by run-conformance.ps1

STATE="/kitchenos_conformance"
SEM="/kitchenos_conformance_sem"
STUB_HZ=120            # above the peer's 60 Hz, so slot 0 is ready for most frames
STUB_SECONDS=$((STREAM_SECONDS + 60))

# 帧标记，与 PluginPeer.cs 里是同一组常量。它们负责把桥日志里的某一行钉到
# 产生它的那一个探针上。
SPLIT_MARKER=4242      # state_update written as two socket writes
COALESCED_MARKER=4243  # state_update written in the same write as hello
CAP_MARKER=4244        # state_update with 50 resource keys (one longer than 32)
CAP_RESOURCES=48       # PROTO_MAX_RESOURCES keeps 48 of the 50
CAP_DROPPED=2
CAP_TRUNCATED=1

BRIDGE_PID=""
STUB_PID=""
STUB_PID2=""
FAILURES=0

say() { printf '%s\n' "$*"; }
rule() { say "------------------------------------------------------------------"; }
fail() { FAILURES=$((FAILURES + 1)); say "  FAIL  $*"; }
ok()   { say "  ok    $*"; }

cleanup() {
    for pid in "$STUB_PID" "$STUB_PID2"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
        fi
    done
    if [ -n "$BRIDGE_PID" ] && kill -0 "$BRIDGE_PID" 2>/dev/null; then
        kill -TERM "$BRIDGE_PID" 2>/dev/null
        wait "$BRIDGE_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

count() { grep -c -- "$1" "$LOG" 2>/dev/null || true; }
field() { grep -m 1 -- "$1" "$LOG" 2>/dev/null || true; }

mkdir -p logs plugin/tools/conformance/bin

# ============================================================ 1. 对端程序
say "== 1. 对端：由插件自己的源码编译（未改动）=="
if [ ! -x "$PEER_EXE" ]; then
    say "  $PEER_EXE 不存在或不可执行。"
    say "  请先在 Windows 侧编译："
    say "      powershell -NoProfile -File plugin/tools/conformance/run-conformance.ps1"
    say "  它执行的就是下面这条命令，用的是 Windows 自带的编译器："
    say "      C:\\Windows\\Microsoft.NET\\Framework64\\v4.0.30319\\csc.exe /nologo /target:exe /warn:4 \\"
    say "          /out:$PEER_EXE \\"
    say "          $PEER_SRC $MESSAGES_SRC $TCPLINK_SRC"
    exit 3
fi

# 那两个源文件才是被测对象：对端必须由「此刻磁盘上的字节」编译，而不是旧版本的字节。
# 拿可执行文件的时间戳和源文件比，能抓住「我改了 Messages.cs 却没重编译就跑」这种情况。
for src in "$PEER_SRC" "$MESSAGES_SRC" "$TCPLINK_SRC"; do
    if [ "$src" -nt "$PEER_EXE" ]; then
        say "  $src 比 $PEER_EXE 新：先重新编译，这一轮才可信"
        exit 3
    fi
done
ok "$PEER_EXE （$(stat -c %s "$PEER_EXE") 字节）比它所依据的源文件新"

# ============================================================ 2. 编译 C 核心
say ""
say "== 2. 编译 C 核心 =="
if ! make -C bridge > /dev/null 2>&1; then
    say "make -C bridge 失败："
    make -C bridge 2>&1 | sed 's/^/  /'
    exit 3
fi
for bin in "$BRIDGE_BIN" "$STUB_BIN"; do
    [ -x "$bin" ] || { say "missing $bin (run: cd bridge && make)"; exit 3; }
done
ok "$BRIDGE_BIN 与 $STUB_BIN 都在"

# ============================================================ 3. 起桥
say ""
say "== 3. 起桥（listen 模式，verbose 日志）=="
: > "$LOG"
: > "$BRIDGE_OUT"

"$BRIDGE_BIN" --link listen --host 0.0.0.0 --port "$PORT" \
    --chefs 2 --state-name "$STATE" --sem-name "$SEM" \
    --log "$LOG" --verbose > "$BRIDGE_OUT" 2>&1 &
BRIDGE_PID=$!
say "  桥 pid $BRIDGE_PID，日志 $LOG"

ready=0
for _ in $(seq 1 60); do
    if grep -q 'net_ready' "$LOG" 2>/dev/null; then ready=1; break; fi
    if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then break; fi
    sleep 0.25
done
if [ "$ready" -ne 1 ]; then
    say "  桥没有到达 net_ready。它的日志："
    sed 's/^/    /' "$LOG" 2>/dev/null
    say "  它的 stdout/stderr："
    sed 's/^/    /' "$BRIDGE_OUT" 2>/dev/null
    say "  （常见原因：端口 $PORT 已经被另一个桥占用）"
    exit 3
fi
ok "$(field 'bridge_start')"

# worker stub 必须在桥建好共享段之后再起，而且桥活着的时候不能重启它：slot 的 frame id 是
# 单调的，从 1 重新开始的 stub 对一个 last_frame 已经越过它的桥等于隐形（这样的 worker
# 在桥眼里永远在空转）。
"$STUB_BIN" --state-name "$STATE" --sem-name "$SEM" \
    --chefs 2 --slot 0 --hz "$STUB_HZ" --duration "$STUB_SECONDS" \
    --kind "${STUB_KINDS[0]}" > "$STUB_OUT" 2>&1 &
STUB_PID=$!
say "  worker stub pid $STUB_PID（slot 0，${STUB_HZ} Hz，kind=${STUB_KINDS[0]}）"
STUB_EVIDENCE="$STUB_OUT"
if [ "${#STUB_KINDS[@]}" -ge 2 ]; then
    STUB_OUT2="logs/conformance-stub-slot1.out"
    "$STUB_BIN" --state-name "$STATE" --sem-name "$SEM" \
        --chefs 2 --slot 1 --hz "$STUB_HZ" --duration "$STUB_SECONDS" \
        --kind "${STUB_KINDS[1]}" > "$STUB_OUT2" 2>&1 &
    STUB_PID2=$!
    say "  worker stub pid $STUB_PID2（slot 1，${STUB_HZ} Hz，kind=${STUB_KINDS[1]}）"
    STUB_EVIDENCE="$STUB_OUT 与 $STUB_OUT2"
else
    say "  （slot 1 故意留空：没人写入的 slot 要收到空转的 stop）"
fi

# ============================================================ 4. 跑对端
say ""
say "== 4. 跑对端（插件自己的协议代码）=="
say "  $PEER_EXE --host 127.0.0.1 --port $PORT --seconds $STREAM_SECONDS --require-kinds $REQUIRE_KINDS"
say ""

"$PEER_EXE" --host 127.0.0.1 --port "$PORT" --seconds "$STREAM_SECONDS" \
    --hz 60 --require-kinds "$REQUIRE_KINDS" \
    2>&1 | tee "$PEER_OUT"
PEER_RC=${PIPESTATUS[0]}
say ""
say "对端退出码：$PEER_RC"

# ============================================================ 5. 停双方
say ""
say "== 5. 停双方 =="
if [ -n "$STUB_PID" ] && kill -TERM "$STUB_PID" 2>/dev/null; then
    wait "$STUB_PID" 2>/dev/null
    STUB_RC=$?
    STUB_PID=""
    say "  worker stub（slot 0）退出码：$STUB_RC"
fi
if [ -n "$STUB_PID2" ] && kill -TERM "$STUB_PID2" 2>/dev/null; then
    wait "$STUB_PID2" 2>/dev/null
    STUB_RC2=$?
    STUB_PID2=""
    say "  worker stub（slot 1）退出码：$STUB_RC2"
fi
kill -TERM "$BRIDGE_PID" 2>/dev/null
wait "$BRIDGE_PID" 2>/dev/null
BRIDGE_RC=$?
BRIDGE_PID=""
say "  桥退出码：$BRIDGE_RC（0 = 收到 SIGTERM 后干净退出）"

# ============================================================ 6. 判定日志
say ""
say "== 6. 桥自己的日志 =="
LINK_UP=$(count 'link_up')
LINK_DOWN=$(count 'link_down')
HELLO=$(count 'plugin_hello')
STATE_UPDATES=$(count 'state_update')
ACTIONS_SENT=$(count 'actions_sent')
SLOTS_MISSING=$(count 'slots_missing')
ACKS=$(count 'plugin_ack')
SUMMARY=$(count 'state_summary')
PARSE_ERR=$(count 'parse_error')
FRAME_ERR=$(count 'frame_error')
TIMEOUTS=$(count 'plugin_timeout')
SHM_RECREATE=$(count 'shm_recreate')

say "  bridge_start=$([ -n "$(field bridge_start)" ] && echo 1 || echo 0) link_up=$LINK_UP link_down=$LINK_DOWN shm_recreate=$SHM_RECREATE"
say "  plugin_hello=$HELLO state_update=$STATE_UPDATES actions_sent=$ACTIONS_SENT"
say "  slots_missing=$SLOTS_MISSING plugin_ack=$ACKS state_summary=$SUMMARY plugin_timeout=$TIMEOUTS"
say "  parse_error=$PARSE_ERR frame_error=$FRAME_ERR"
say "  派发出的第一帧：$(field 'actions_sent' | sed 's/^.*INFO  //')"
say "  第一帧带真实输入（slot 0 就绪，kind=${STUB_KINDS[0]}）：$(grep -m 1 "actions_sent.*${STUB_KINDS[0]}=1 " "$LOG" 2>/dev/null | sed 's/^.*INFO  //')"
say "  没人写入的那个 slot 第一次空转兜底：$(grep -m 1 'slots_missing.*missing=1 ' "$LOG" 2>/dev/null | sed 's/^.*WARN  //' || true)"
say ""

[ "$LINK_UP" -ge 2 ] && ok "两次连接都被接受（阶段 1 的探针、阶段 2 的 TcpLink 会话）" \
                     || fail "link_up 应 >= 2（每次连接一条），实际 $LINK_UP"
[ "$HELLO" -ge 2 ] && ok "hello 已被解析（$HELLO 次：粘包探针 + TcpLink 会话）" \
                   || fail "plugin_hello 应 >= 2，实际 $HELLO"
[ "$STATE_UPDATES" -ge 10 ] && ok "已解析 $STATE_UPDATES 个 state_update 帧" \
                           || fail "state_update 帧应 >= 10，实际 $STATE_UPDATES"
[ "$ACTIONS_SENT" -ge 10 ] && ok "已派发回对端 $ACTIONS_SENT 帧" \
                          || fail "actions_sent 应 >= 10，实际 $ACTIONS_SENT"
[ "$ACKS" -ge 1 ] && ok "plugin_ack：对端的 Msg.Ack 帧已被 C 侧解析" \
                 || fail "没有 plugin_ack 行：对端的 ack 没到达或没被解析"

# 探针靠标记认出来，而不是靠「有东西来了」。
SPLIT_LINE=$(grep -m 1 -- "game_time=$SPLIT_MARKER " "$LOG" 2>/dev/null || true)
if [ -n "$SPLIT_LINE" ]; then
    ok "拆分帧已重组：$(printf '%s' "$SPLIT_LINE" | sed 's/^.*DEBUG //')"
else
    fail "日志里没有 game_time=$SPLIT_MARKER 的 state_update 行：拆成两次写的那一帧没被解析"
fi

COAL_LINE=$(grep -m 1 -- "game_time=$COALESCED_MARKER " "$LOG" 2>/dev/null || true)
if [ -n "$COAL_LINE" ] && [ "$HELLO" -ge 1 ]; then
    ok "粘包写入已切成两帧：$(printf '%s' "$COAL_LINE" | sed 's/^.*DEBUG //')"
else
    fail "日志里没有 game_time=$COALESCED_MARKER 的 state_update 行：一次写两帧没被解析"
fi

CAP_LINE=$(grep -m 1 -- "game_time=$CAP_MARKER " "$LOG" 2>/dev/null || true)
# 三个计数分开检查：日志行的字段顺序由桥决定，写死顺序的检查会被一次无害的调序弄挂。
if printf '%s' "$CAP_LINE" | grep -q "resources=$CAP_RESOURCES " &&
   printf '%s' "$CAP_LINE" | grep -q "dropped=$CAP_DROPPED " &&
   printf '%s' "$CAP_LINE" | grep -q "truncated=$CAP_TRUNCATED "; then
    ok "容量计数完全一致：$(printf '%s' "$CAP_LINE" | sed 's/^.*DEBUG //')"
else
    fail "容量探针帧应记录 resources=$CAP_RESOURCES、dropped=$CAP_DROPPED、truncated=$CAP_TRUNCATED；实际：${CAP_LINE:-<no line>}"
fi

[ "$PARSE_ERR" -eq 0 ] && ok "parse_error=0" || fail "parse_error=$PARSE_ERR（见日志）"
[ "$FRAME_ERR" -eq 0 ] && ok "frame_error=0" || fail "frame_error=$FRAME_ERR（见日志）"
[ "$TIMEOUTS" -eq 0 ] && ok "plugin_timeout=0" || fail "plugin_timeout=$TIMEOUTS"

# ============================================================ 7. 结论
say ""
rule
say "对端退出码=$PEER_RC   桥退出码=$BRIDGE_RC"
say "证据：$PEER_OUT（对端 stdout）、$LOG（桥）、$STUB_OUT（stub）、$CSC_OUT（编译器）"
rule

if [ "$PEER_RC" -ne 0 ]; then
    fail "对端报出了跨实现分歧（退出码 $PEER_RC）"
fi
if [ "$BRIDGE_RC" -ne 0 ]; then
    fail "桥退出码是 $BRIDGE_RC，不是 0（见 $LOG）"
fi

say ""
if [ "$FAILURES" -eq 0 ]; then
    say "PASS  C 桥与插件自己的协议代码互相看得懂，"
    say "      而且双方退出码都是 0。"
    exit 0
fi
say "FAIL  有 $FAILURES 项检查没过。"
exit 1
