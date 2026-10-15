#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""协议级模拟插件：不启动游戏就能把 Bridge 的全部场景跑完。

本文件扮演 Windows 侧插件在 PROTOCOL 层的对端，说的是同一套话：
4 字节大端长度前缀 + UTF-8 JSON，字段名以 plugin/WellCooked.Plugin/ 与
overcooked_os_course_project.md §5.2 为基准（分帧基准是 Net/TcpLink.cs 的
MaxFrameBytes/ExtractFrames，消息基准是 Protocol/Messages.cs）。

每个场景自己判定成败并用退出码表达结论，失败点写 stderr，不需要人翻日志：

    0   全部断言通过
    1   有断言失败（stderr 点名是哪一条、期望什么、实际什么）
    2   环境错误（Bridge 二进制缺失/起不来、端口占用等），不是断言结论
    77  跳过：前置产物不在（例如 kitchenos-worker-stub 还没构建）

用法见 --help；批量跑用 tools/run-bridge-scenarios.sh。
"""

import argparse
import json
import os
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback
from collections import Counter
from pathlib import Path

# --------------------------------------------------------------------------
# 协议常量：这些数字与名字来自插件实现，改了就不再是同一个协议
# --------------------------------------------------------------------------

MAX_FRAME_BYTES = 1024 * 1024   # TcpLink.cs 的 MaxFrameBytes，超出即越界
DEFAULT_LISTEN_PORT = 12345     # Bridge 作服务端时的默认端口
DEFAULT_CONNECT_PORT = 14455    # 插件作服务端时的默认端口，Bridge --link connect 去连它
HELLO_INSTANCE = "windows-host"
HELLO_VERSION = "0.1.0"
HELLO_ACTIONS = ("move", "stop", "press", "release", "tap", "pickup", "dash", "interact")
IDLE_ACTION = "stop"            # 输入槽缺失时 Bridge 下发的空闲动作

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_ERROR = 2
EXIT_SKIP = 77

SCENARIOS = ("happy", "one-slot", "no-heartbeat", "bad-frames", "connect-mode")

REPO_ROOT = Path(__file__).resolve().parent.parent

# 场景自检失败时用来找证据的关键词：日志文案没冻结，所以只在 log 里找"提到过"。
# 除中文说法外还覆盖 Bridge 现有实现的事件名（state_update 是 debug 级，
# 默认 INFO 下看不到，所以连它的每 60 帧摘要 state_summary 一起认）。
LOG_PATTERNS = {
    "state_update": re.compile(r"state_update|state_summary|状态", re.I),
    "action_dispatch": re.compile(r"actions?_sent|action|动作|下发", re.I),
    "heartbeat_timeout": re.compile(r"plugin_timeout|plugin_resumed|heartbeat|silent_ms|"
                                    r"心跳|link_down|disconnect", re.I),
    "json_error": re.compile(r"parse_error|json|parse|解析", re.I),
    "frame_error": re.compile(r"frame_error|frame|length|帧长|长度|oversize|too\s+(long|large)",
                              re.I),
    "idle_or_missing_slot": re.compile(r"slots_missing|idle|missing|slot|槽|缺失|空闲", re.I),
}


class FramingError(Exception):
    """分帧层失步：长度越界，或对端发来的字节流已经不是帧。"""


class InfraError(Exception):
    """环境问题（不是被测对象的断言结论）。"""


class Skipped(Exception):
    """前置产物缺失，本场景无法给出结论。"""


def _say(msg=""):
    print(msg, flush=True)


def _tail(path, lines=25):
    try:
        text = Path(path).read_text(errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(text[-lines:])


# --------------------------------------------------------------------------
# 分帧：4 字节大端长度前缀 + UTF-8 JSON，单帧上限 1 MiB
# --------------------------------------------------------------------------


def encode_frame(payload):
    """把 str/dict/bytes 打成一个帧；越界直接报错，免得发出不该发的字节。"""
    if isinstance(payload, dict):
        payload = json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    if len(payload) > MAX_FRAME_BYTES:
        raise FramingError("payload %d > %d" % (len(payload), MAX_FRAME_BYTES))
    return struct.pack(">I", len(payload)) + payload


class FrameDecoder:
    """增量解帧器：喂多少字节都行，吐出一条条完整 payload。"""

    def __init__(self, max_frame=MAX_FRAME_BYTES):
        self._buf = bytearray()
        self._max = max_frame

    def feed(self, data):
        self._buf += data

    @property
    def pending(self):
        return len(self._buf)

    def next_payload(self):
        """返回下一个 payload（str）；不完整返回 None；长度越界抛 FramingError。

        注意零长帧返回空串而不是 None，调用方必须用 `is None` 判断。
        """
        if len(self._buf) < 4:
            return None
        (length,) = struct.unpack(">I", self._buf[:4])
        # C# 侧把长度当有符号 int 读，负数在无符号视角下就是 >2^31，同样落在越界里
        if length > self._max:
            raise FramingError("frame length %d exceeds limit %d" % (length, self._max))
        if len(self._buf) - 4 < length:
            return None
        payload = bytes(self._buf[4:4 + length])
        del self._buf[:4 + length]
        return payload.decode("utf-8", errors="replace")

    def reset(self):
        self._buf.clear()


# --------------------------------------------------------------------------
# 消息构造：字段名与 Messages.cs / §5.2 对齐
# --------------------------------------------------------------------------


def jdump(obj):
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def msg_hello(instance=HELLO_INSTANCE, version=HELLO_VERSION, actions=HELLO_ACTIONS):
    return jdump({"type": "hello", "instance": instance, "version": version,
                  "actions": list(actions)})


def msg_heartbeat(now=None):
    return jdump({"type": "heartbeat",
                  "timestamp": int(time.time() if now is None else now)})


def msg_ack(action, chef_id, ok=True, detail=""):
    return jdump({"type": "ack", "action": action or "", "chef_id": int(chef_id),
                  "ok": bool(ok), "detail": detail or ""})


def msg_echo(token):
    return jdump({"type": "echo", "token": token})


def msg_state_update(n_chefs, frame_index, flat_xy=False, edge_fields=False):
    """§5.2 的状态上报，逐字段照抄（含 resources 的动态键对象）。

    flat_xy：额外给每个 chef 加顶层 x/y。文档 §5.2 用 position 对象，
    bridge.md 的数据形状一行写成了 chefs[]{id,x,y,...}；默认按 §5.2，
    需要迁就扁平解析时用 --flat-xy。
    edge_fields：加入文档里点名的边界（resources 出现未知键 pot_7、
    顶层未知字段、plate_stack.count 为 0），用来验证解析器忽略而不是报错。
    """
    chefs = []
    for i in range(n_chefs):
        x = round(10.5 + 1.25 * i, 3)
        y = round(5.2 + 0.75 * i, 3)
        chef = {"id": i, "position": {"x": x, "y": y},
                "holding": "tomato" if i % 2 == 0 else None,
                "state": "moving" if i % 2 == 0 else "idle"}
        if flat_xy:
            chef["x"] = x
            chef["y"] = y
        chefs.append(chef)

    resources = {
        "pot_0": {"state": "occupied", "content": "soup",
                  "time_left": max(0, 10 - frame_index % 20)},
        "pot_1": {"state": "free"},
        "cutting_board_0": {"state": "occupied", "chef_id": 0},
        "plate_stack": {"count": 5},
    }
    msg = {
        "type": "state_update",
        "timestamp": int(time.time()),
        "game_time": frame_index // 60,
        "score": 450,
        "chefs": chefs,
        "orders": [{"id": "order_001", "recipe": "salad", "time_remaining": 45,
                    "ingredients": ["lettuce", "tomato"]}],
        "resources": resources,
    }
    if edge_fields:
        resources["pot_7"] = {"state": "free"}
        resources["plate_stack"] = {"count": 0}
        msg["unknown_field"] = "parsers must ignore this"
    return jdump(msg)


def validate_action(msg):
    """返回不符合 §5.2 action 形状的字段名列表，空列表表示合法。"""
    errs = []
    if msg.get("type") != "action":
        errs.append("type != action")
    chef_id = msg.get("chef_id")
    if not isinstance(chef_id, int) or isinstance(chef_id, bool):
        errs.append("chef_id is not an int")
    action = msg.get("action")
    if not isinstance(action, str) or not action:
        errs.append("action is missing/empty")
    elif action == "move":
        target = msg.get("target")
        if not isinstance(target, dict):
            errs.append("move without a target object")
        else:
            for key in ("x", "y"):
                value = target.get(key)
                if not isinstance(value, (int, float)) or isinstance(value, bool):
                    errs.append("move target.%s is not a number" % key)
    return errs


# --------------------------------------------------------------------------
# 插件侧链路：非阻塞收发 + 自分帧
# --------------------------------------------------------------------------


class Stats:
    """一次交换的观察结果。"""

    def __init__(self, label="run"):
        self.label = label
        self.actions = []
        self.bad_actions = []
        self.by_type = Counter()
        self.echo_replies = []
        self.bridge_acks = []
        self.bad_payloads = []
        self.tx_states = 0
        self.tx_heartbeats = 0
        self.tx_acks = 0
        self.tx_echoes = 0
        self.tx_frames = 0
        self.tx_errors = []
        self.frame_index = 0

    def chef_ids(self):
        return sorted({a.get("chef_id") for a in self.actions})

    def count_action(self, name):
        return sum(1 for a in self.actions if a.get("action") == name)

    def per_chef(self):
        c = Counter()
        for a in self.actions:
            c[a.get("chef_id")] += 1
        return c

    def describe(self):
        per_chef = self.per_chef()
        chefs = ", ".join("chef%s=%d" % (k, v) for k, v in sorted(
            per_chef.items(), key=lambda kv: (kv[0] is None, kv[0])))
        return ("%d actions (%s; move=%d stop=%d)" %
                (len(self.actions), chefs or "none",
                 self.count_action("move"), self.count_action(IDLE_ACTION)))


class Link:
    """模拟插件的这一端连接：非阻塞收、按帧解、发不出去就记错。"""

    def __init__(self, sock, label="plugin-link"):
        self.sock = sock
        self.sock.setblocking(False)
        self.label = label
        self.dec = FrameDecoder()
        self.closed = False
        self.error = None
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.rx_frames = 0
        self.stash = []          # probe_closed 期间收到的消息，下次 pump 一起交出去

    # -- 收 --
    def pump(self, timeout):
        """最多等 timeout 秒，返回这一轮解出来的消息（dict）。"""
        out = self.take_stash()
        if self.closed:
            time.sleep(min(max(timeout, 0.0), 0.05))
            return out
        try:
            ready, _, _ = select.select([self.sock], [], [], max(0.0, timeout))
        except (OSError, ValueError):
            self._die("select failed")
            return out
        if not ready:
            return out
        try:
            data = self.sock.recv(65536)
        except (BlockingIOError, InterruptedError):
            return out
        except OSError as exc:
            self._die("recv: %s" % exc)
            return out
        if not data:
            self._die("peer closed the connection (EOF)")
            return out
        self.rx_bytes += len(data)
        self.dec.feed(data)
        try:
            while True:
                payload = self.dec.next_payload()
                if payload is None:
                    break
                self.rx_frames += 1
                if not payload:
                    # 零长帧：按 W1 属于"要能承受"的输入，记下来但不当作消息
                    out.append({"_frame_error": "zero-length frame"})
                    continue
                try:
                    out.append(json.loads(payload))
                except ValueError as exc:
                    out.append({"_frame_error": "invalid JSON: %s" % exc,
                                "_raw": payload[:160]})
        except FramingError as exc:
            # 长度越界 = 流已失步，后面再读都是垃圾，直接判连接不可用
            self._die("framing: %s" % exc)
        return out

    def take_stash(self):
        items = self.stash
        self.stash = []
        return items

    def probe_closed(self, timeout=0.4):
        """主动探活，返回 True 表示连接已断。

        对端发的 FIN 只有 recv 才看得见：刚被断开时 send() 往往还能成功、self.closed
        也还是 False，只看标志位会把"已经被断开的连接"当成"还连着"。而且 recv 一次未必
        就撞上 EOF —— Bridge 的心跳可能排在前头，所以要在 timeout 内反复收到没数据为止。
        探活期间收到的消息先存起来，下次 pump() 原样交出去，不丢。
        """
        deadline = time.monotonic() + max(0.0, timeout)
        while not self.closed:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            self.stash.extend(self.pump(left))
        return self.closed

    # -- 发 --
    def send_raw(self, data, timeout=5.0):
        if self.closed:
            return False
        view = memoryview(data)
        sent = 0
        start = time.monotonic()
        while sent < len(view):
            try:
                n = self.sock.send(view[sent:])
                sent += n
                self.tx_bytes += n
            except (BlockingIOError, InterruptedError):
                left = timeout - (time.monotonic() - start)
                if left <= 0:
                    self._die("send timed out")
                    return False
                select.select([], [self.sock], [], left)
            except OSError as exc:
                self._die("send: %s" % exc)
                return False
        return True

    def send_frame(self, payload):
        return self.send_raw(encode_frame(payload))

    def send_json(self, text):
        return self.send_frame(text)

    def close(self):
        """干净关闭：先 half-close 让对端看到 EOF，再放手。"""
        if self.closed:
            return
        self.closed = True
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass

    def _die(self, reason):
        if self.error is None:
            self.error = reason
        if not self.closed:
            self.closed = True
            try:
                self.sock.close()
            except OSError:
                pass


def connect(host, port, timeout=5.0):
    start = time.monotonic()
    last = None
    while time.monotonic() - start < timeout:
        try:
            sock = socket.create_connection((host, port), timeout=2.0)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return sock
        except OSError as exc:
            last = exc
            time.sleep(0.05)
    raise InfraError("cannot connect to %s:%d within %.1fs: %s" % (host, port, timeout, last))


def collect(link, stats, duration):
    """只收不发，把窗口内收到的帧记进 stats。"""
    end = time.monotonic() + duration
    while time.monotonic() < end:
        for msg in link.pump(min(0.05, max(0.0, end - time.monotonic()))):
            _absorb(msg, stats, None, False, None)


def run_exchange(link, stats, duration, *, chefs=1, hz=60.0, heartbeat_ms=500,
                 send_state=True, send_heartbeat=True, ack_actions=True,
                 echo_at=None, edge_at=None, flat_xy=False):
    """主循环：按 hz 发 state_update、按 heartbeat_ms 发心跳，同时收动作并回 ack。

    收和发在同一个循环里，select 的等待时间取"下一个发送时刻"，所以 60 Hz 不会
    靠忙等实现，收帧也不会被 16 ms 的节奏卡住。
    """
    period = 1.0 / hz if hz and hz > 0 else 0.0
    start = time.monotonic()
    end = start + duration
    next_state = start
    next_heartbeat = start + heartbeat_ms / 1000.0
    echo_sent = echo_at is None
    echo_token = None
    edge_sent = edge_at is None

    while True:
        now = time.monotonic()
        if now >= end:
            break

        deadline = end
        if send_state and period:
            deadline = min(deadline, next_state)
        if send_heartbeat:
            deadline = min(deadline, next_heartbeat)
        for msg in link.pump(max(0.0, min(deadline - now, 0.05))):
            _absorb(msg, stats, link, ack_actions, chefs)

        now = time.monotonic()

        if echo_at is not None and not echo_sent and now - start >= echo_at:
            echo_token = "probe-%d" % (int(start * 1000) % 100000)
            _safe_send(link, stats, msg_echo(echo_token), "echo")
            stats.tx_echoes += 1
            echo_sent = True

        if send_state and period and now >= next_state and now < end:
            payload = msg_state_update(chefs, stats.frame_index, flat_xy=flat_xy,
                                       edge_fields=False)
            if _safe_send(link, stats, payload, "state_update"):
                stats.tx_states += 1
                stats.frame_index += 1
            next_state += period
            if next_state < now:      # 落后太多就直接对齐，不补发风暴
                next_state = now + period

        if edge_at is not None and not edge_sent and now - start >= edge_at and now < end:
            payload = msg_state_update(chefs, stats.frame_index, flat_xy=flat_xy,
                                       edge_fields=True)
            if _safe_send(link, stats, payload, "state_update(edge)"):
                stats.tx_states += 1
                stats.frame_index += 1
            edge_sent = True

        if send_heartbeat and now >= next_heartbeat and now < end:
            if _safe_send(link, stats, msg_heartbeat(), "heartbeat"):
                stats.tx_heartbeats += 1
            next_heartbeat += heartbeat_ms / 1000.0

    stats.echo_token = echo_token


def _safe_send(link, stats, payload, what):
    if link.closed:
        stats.tx_errors.append("%s: link closed (%s)" % (what, link.error))
        return False
    if link.send_frame(payload):
        stats.tx_frames += 1
        return True
    stats.tx_errors.append("%s: %s" % (what, link.error or "send failed"))
    return False


def _absorb(msg, stats, link, ack_actions, chefs):
    if "_frame_error" in msg:
        stats.bad_payloads.append(msg["_frame_error"])
        return
    if not isinstance(msg, dict):
        stats.bad_payloads.append("not a JSON object: %r" % (msg,))
        return
    mtype = msg.get("type")
    stats.by_type[mtype] += 1
    if mtype == "action":
        errs = validate_action(msg)
        if errs:
            stats.bad_actions.append((msg, errs))
        stats.actions.append(msg)
        if ack_actions and link is not None:
            chef_id = msg.get("chef_id")
            ok = not errs
            if _safe_send(link, stats, msg_ack(msg.get("action", ""),
                                               chef_id if isinstance(chef_id, int) else -1,
                                               ok, "fake_plugin executed" if ok else "rejected"),
                          "ack"):
                stats.tx_acks += 1
    elif mtype == "echo_reply":
        stats.echo_replies.append(msg.get("token"))
    elif mtype == "ack":
        stats.bridge_acks.append(msg)


# --------------------------------------------------------------------------
# 被驱动的进程：Bridge 与 worker stub
# --------------------------------------------------------------------------


def listening_ports():
    """从 /proc/net/tcp 读 LISTEN 端口，避免用 connect 探测打扰 Bridge 的 accept 循环。"""
    return set(listen_inodes().keys())


def listen_inodes():
    """{port: socket inode}：只包含处于 LISTEN 的 TCP socket。"""
    found = {}
    for path in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            lines = Path(path).read_text().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            fields = line.split()
            if len(fields) < 10 or fields[3] != "0A":
                continue
            try:
                found[int(fields[1].split(":")[1], 16)] = int(fields[9])
            except (IndexError, ValueError):
                continue
    return found


def pid_owns_socket(pid, inode):
    """这个 pid 是否持有该 socket：靠 /proc/<pid>/fd 反查 inode。

    多 agent 并行跑测试时 12345 上可能蹲着别人的 Bridge，光看"端口在监听"会把
    对端认成自己的进程，断言就全是错的。
    """
    if pid is None:
        return False
    fd_dir = "/proc/%d/fd" % pid
    want = "socket:[%d]" % inode
    try:
        fds = os.listdir(fd_dir)
    except OSError:
        return False
    for fd in fds:
        try:
            if os.readlink(os.path.join(fd_dir, fd)) == want:
                return True
        except OSError:
            continue
    return False


def pick_free_port(host, preferred=None):
    """挑一个当前能绑的端口：先试 preferred，被占就交给内核分配。"""
    for candidate in (preferred, 0):
        if candidate is None:
            continue
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.bind((host, candidate))
            return sock.getsockname()[1]
        except OSError:
            continue
        finally:
            sock.close()
    raise InfraError("no free TCP port available on %s" % host)


class BridgeProc:
    def __init__(self, binary, argv, console_path):
        self.binary = str(binary)
        self.argv = list(argv)
        self.console_path = Path(console_path)
        self.proc = None
        self.console = None
        self.pid = None
        self.term_seconds = None
        self.term_ok = None

    def start(self):
        self.console_path.parent.mkdir(parents=True, exist_ok=True)
        self.console = open(self.console_path, "wb")
        self.proc = subprocess.Popen(
            [self.binary] + self.argv,
            cwd=str(REPO_ROOT),
            stdin=subprocess.DEVNULL,
            stdout=self.console,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self.pid = self.proc.pid

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def exit_code(self):
        return None if self.proc is None else self.proc.poll()

    def wait_listen(self, port, timeout):
        """等 Bridge 真的在监听：进程活着 + 该端口的 LISTEN socket 属于这个 pid。"""
        start = time.monotonic()
        while time.monotonic() - start < timeout:
            if self.listen_owner_is_self(port):
                return True
            if not self.alive():
                return False
            time.sleep(0.05)
        return self.listen_owner_is_self(port)

    def listen_owner_is_self(self, port):
        inode = listen_inodes().get(port)
        return inode is not None and pid_owns_socket(self.pid, inode)

    def port_is_taken_by_other(self, port):
        """端口在监听，但不是这个 Bridge 在听 —— 对端是别人。"""
        return port in listening_ports() and not self.listen_owner_is_self(port)

    def terminate(self, timeout=3.0):
        if self.proc is None:
            return True
        if not self.alive():
            self.term_ok = True
            self.term_seconds = 0.0
            return True
        start = time.monotonic()
        self._signal(signal.SIGTERM)
        while time.monotonic() - start < timeout:
            if not self.alive():
                self.term_ok = True
                self.term_seconds = time.monotonic() - start
                return True
            time.sleep(0.05)
        self._signal(signal.SIGKILL)
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass
        self.term_ok = False
        self.term_seconds = time.monotonic() - start
        return False

    def _signal(self, sig):
        try:
            os.killpg(os.getpgid(self.pid), sig)
        except (ProcessLookupError, PermissionError, OSError):
            try:
                self.proc.send_signal(sig)
            except OSError:
                pass

    def console_text(self):
        return _tail(self.console_path, 30)


class WorkerStub:
    """另一个 agent 产出的输入槽写者；我们只按冻结的 CLI 调用它。"""

    def __init__(self, binary, argv, console_path):
        self.binary = str(binary)
        self.argv = list(argv)
        self.console_path = Path(console_path)
        self.proc = None
        self.console = None

    def start(self):
        self.console_path.parent.mkdir(parents=True, exist_ok=True)
        self.console = open(self.console_path, "wb")
        self.proc = subprocess.Popen(
            [self.binary] + self.argv,
            cwd=str(REPO_ROOT),
            stdin=subprocess.DEVNULL,
            stdout=self.console,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def terminate(self, timeout=2.0):
        if self.proc is None or not self.alive():
            return
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
        except (ProcessLookupError, PermissionError, OSError):
            try:
                self.proc.terminate()
            except OSError:
                pass
        start = time.monotonic()
        while time.monotonic() - start < timeout and self.alive():
            time.sleep(0.05)
        if self.alive():
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except OSError:
                pass

    def console_text(self):
        return _tail(self.console_path, 15)


# --------------------------------------------------------------------------
# 断言收集与场景上下文
# --------------------------------------------------------------------------


class Checks:
    def __init__(self, scenario, strict=False):
        self.scenario = scenario
        self.strict = strict
        self.failures = []
        self.warnings = []
        self.observations = []

    def hard(self, name, ok, detail=""):
        if ok:
            _say("    HARD ok    %s%s" % (name, (" — " + detail) if detail else ""))
        else:
            _say("    HARD FAIL  %s%s" % (name, (" — " + detail) if detail else ""))
            self.failures.append((name, detail))

    def soft(self, name, ok, detail=""):
        if ok:
            _say("    soft ok    %s%s" % (name, (" — " + detail) if detail else ""))
        elif self.strict:
            _say("    HARD FAIL  %s%s (--strict)" % (name, (" — " + detail) if detail else ""))
            self.failures.append((name, detail + " (promoted by --strict)"))
        else:
            _say("    soft WARN  %s%s" % (name, (" — " + detail) if detail else ""))
            self.warnings.append((name, detail))

    def warn(self, name, detail=""):
        _say("    soft WARN  %s%s" % (name, (" — " + detail) if detail else ""))
        self.warnings.append((name, detail))

    def note(self, name, value=""):
        _say("    info       %s%s" % (name, (": " + str(value)) if value != "" else ""))
        self.observations.append((name, value))


class Ctx:
    """一个场景独占的运行环境：自己的 shm 名、日志、端口、子进程。"""

    def __init__(self, args, scenario):
        self.args = args
        self.scenario = scenario
        # 名字带前导 '/'：shm.h 的默认值就是 "/kitchenos_state"，而 glibc 拒绝名字里
        # 出现第二个 '/'。"//name" 在这里同样能被接受，所以两端怎么拼都不会错开。
        self.state_name = "/" + (args.state_name or
                                 "%s_%s" % (args.state_prefix, scenario)).lstrip("/")
        self.sem_name = "/" + (args.sem_name or
                               (self.state_name.lstrip("/") + "_ready")).lstrip("/")
        log = args.log or (REPO_ROOT / "logs" / ("bridge-%s.log" % scenario))
        self.log_path = Path(log)
        self.console_path = Path(str(self.log_path) + ".console")
        self.bridge = None
        self.stubs = []
        self.started_shm = []

    # -- /dev/shm --
    def shm_paths(self):
        # Path("/dev/shm") / "/name" 会被 pathlib 当成绝对路径直接替换掉基路径，
        # 所以拼之前必须先去掉前导斜杠
        base = Path("/dev/shm")
        return [base / self.state_name.lstrip("/"),
                base / self.sem_name.lstrip("/"),
                base / ("sem." + self.sem_name.lstrip("/"))]

    def clear_stale_shm(self):
        removed = []
        for path in self.shm_paths():
            if path.exists():
                try:
                    path.unlink()
                    removed.append(str(path))
                except OSError:
                    pass
        if removed:
            _say("    info       removed stale shm objects: %s" % ", ".join(removed))

    def wait_shm(self, timeout):
        target = self.shm_paths()[0]
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if target.exists():
                return True
            if self.bridge is not None and not self.bridge.alive():
                return False
            time.sleep(0.05)
        return target.exists()

    def leftover_shm(self):
        # 只看自己这一轮的名字，别把别的 agent 的测试残留也算进来
        return sorted(p.name for p in Path("/dev/shm").glob(self.state_name.lstrip("/") + "*"))

    # -- 进程 --
    def start_bridge(self, *, link, port, host, chefs, frame_timeout_ms,
                     heartbeat_timeout_ms):
        binary = Path(self.args.bridge)
        if not binary.exists():
            raise InfraError("bridge binary not found: %s (build it with: cd core && make)"
                             % binary)
        argv = ["--link", link, "--host", host, "--port", str(port),
                "--chefs", str(chefs),
                "--state-name", self.state_name, "--sem-name", self.sem_name,
                "--log", str(self.log_path),
                "--frame-timeout-ms", str(frame_timeout_ms),
                "--heartbeat-timeout-ms", str(heartbeat_timeout_ms)]
        self.bridge = BridgeProc(binary, argv, self.console_path)
        self.bridge.start()
        _say("    info       bridge pid=%d: %s" % (self.bridge.pid, " ".join(argv)))
        return self.bridge

    def start_bridge_and_wait(self, *, link, host, chefs, frame_timeout_ms,
                              heartbeat_timeout_ms, port=None, retries=3):
        """起 Bridge 并等它就绪，返回实际用的端口。

        端口默认由内核分配：多 agent 并行开发时 12345 很可能被别的 Bridge 或
        net 单元测试占着，抢不到就换一个；而且"端口在监听"不等于"对端是我的
        Bridge"，wait_listen 会核对 socket inode 属于这个 pid，否则连上去聊的
        是别人的进程，断言全无意义。
        """
        requested = port
        last = ""
        for attempt in range(1, retries + 1):
            use_port = requested if requested is not None else pick_free_port(host)
            self.start_bridge(link=link, port=use_port, host=host, chefs=chefs,
                              frame_timeout_ms=frame_timeout_ms,
                              heartbeat_timeout_ms=heartbeat_timeout_ms)
            if self.bridge.wait_listen(use_port, self.args.ready_timeout):
                return use_port
            foreign = self.bridge.port_is_taken_by_other(use_port)
            blob = (self.bridge.console_text() + "\n" + self.log_text()).lower()
            busy = foreign or "address already in use" in blob or "eaddrinuse" in blob
            last = ("port %d is %s" % (use_port, "held by another process" if foreign
                                       else "unusable"))
            if busy and requested is None and attempt < retries:
                _say("    info       %s; picking another port and retrying" % last)
                self.bridge.terminate()
                self.bridge = None
                continue
            raise InfraError("bridge never listened on %s:%d (exit=%s)%s\n%s"
                             % (host, use_port, self.bridge.exit_code(),
                                " — " + last if busy else "", self.diagnostics()))
        raise InfraError("bridge could not get a usable port after %d attempts (%s)"
                         % (retries, last))

    def start_stub(self, *, slot, chefs, hz, duration):
        binary = Path(self.args.worker_stub)
        argv = ["--state-name", self.state_name, "--sem-name", self.sem_name,
                "--chefs", str(chefs), "--slot", str(slot),
                "--hz", str(hz), "--duration", str(duration)]
        console = Path(str(self.log_path) + ".slot%d.console" % slot)
        stub = WorkerStub(binary, argv, console)
        stub.start()
        self.stubs.append(stub)
        _say("    info       worker-stub slot=%d pid=%d" % (slot, stub.proc.pid))
        return stub

    def stop_all(self):
        for stub in self.stubs:
            stub.terminate()
        if self.bridge is not None:
            ok = self.bridge.terminate()
            if self.bridge.term_seconds is None:
                return
            if ok:
                _say("    info       bridge exited on SIGTERM after %.2fs (code %s)"
                     % (self.bridge.term_seconds, self.bridge.exit_code()))
            else:
                _say("    soft WARN  bridge ignored SIGTERM for %.1fs; killed (W6 belongs to task 03)"
                     % self.bridge.term_seconds)
        leftovers = [p.name for p in self.shm_paths() if p.exists()]
        if leftovers:
            _say("    soft WARN  shm objects left after exit: %s (W6 cleanup, task 03)"
                 % ", ".join(leftovers))
            for path in self.shm_paths():
                if path.exists():
                    try:
                        path.unlink()
                    except OSError:
                        pass
        else:
            _say("    info       no shm objects left after exit")

    # -- 证据 --
    def log_text(self):
        try:
            return self.log_path.read_text(errors="replace")
        except OSError:
            return ""

    def log_grep(self, key, limit=3):
        pattern = LOG_PATTERNS[key]
        hits = [ln.strip() for ln in self.log_text().splitlines() if pattern.search(ln)]
        return hits[:limit]

    def diagnostics(self):
        parts = []
        if self.bridge is not None:
            parts.append("bridge pid=%s alive=%s exit=%s"
                         % (self.bridge.pid, self.bridge.alive(), self.bridge.exit_code()))
            console = self.bridge.console_text()
            if console:
                parts.append("--- bridge stdout/stderr (tail) ---\n" + console)
        log = _tail(self.log_path, 30)
        if log:
            parts.append("--- bridge log %s (tail) ---\n%s" % (self.log_path, log))
        for i, stub in enumerate(self.stubs):
            text = stub.console_text()
            if text:
                parts.append("--- worker-stub slot #%d (tail) ---\n%s" % (i, text))
        leftovers = self.leftover_shm()
        if leftovers:
            parts.append("/dev/shm leftovers: " + ", ".join(leftovers))
        return "\n".join(parts)


# --------------------------------------------------------------------------
# 场景
# --------------------------------------------------------------------------


def scenario_happy(ctx, checks):
    """W2：正常一帧一帧地跑，Bridge 必须持续下发动作。"""
    args = ctx.args
    chefs = args.chefs or 2
    ctx.clear_stale_shm()
    port = ctx.start_bridge_and_wait(link="listen", host=args.host, chefs=chefs,
                                     frame_timeout_ms=args.frame_timeout_ms,
                                     heartbeat_timeout_ms=args.heartbeat_timeout_ms,
                                     port=args.listen_port)
    # 段可能在启动时就建好，也可能第一帧才建；先探一下只为决定 stub 什么时候起，
    # 硬断言放在跑完之后（那时它必须已经存在）
    shm_early = ctx.wait_shm(2.0)
    checks.note("state_shm_present_before_first_frame", "yes" if shm_early else "no")

    stub_binary = Path(args.worker_stub)
    stubs_ran = stub_binary.exists() and not args.no_stubs
    if stubs_ran:
        for slot in range(chefs):
            ctx.start_stub(slot=slot, chefs=chefs, hz=args.stub_hz,
                           duration=args.duration + 6)
        checks.note("worker_stubs", "%d running (slots 0..%d)" % (chefs, chefs - 1))
    else:
        checks.warn("worker_stubs_absent",
                    "no %s — only the idle (stop) path is exercised here" % stub_binary)

    link = Link(connect(args.host, port, args.ready_timeout))
    checks.hard("bridge_accepts_plugin_connection", True, "%s:%d" % (args.host, port))
    link.send_json(msg_hello())
    checks.note("hello_sent", msg_hello())

    stats = Stats("happy")
    run_exchange(link, stats, args.duration, chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=not args.no_ack,
                 echo_at=0.5, edge_at=max(0.0, args.duration - 0.5), flat_xy=args.flat_xy)

    # W2 的核心就是"下发动作"：spec 没有要求 Bridge 回 ack，所以硬断言落在 action 上。
    checks.hard("actions_dispatched", len(stats.actions) >= args.min_actions,
                "got %d, need >= %d in %.1fs (link=%s, bridge alive=%s)"
                % (len(stats.actions), args.min_actions, args.duration,
                   link.error or "open", ctx.bridge.alive()))
    checks.hard("actions_wellformed", not stats.bad_actions,
                "%d malformed: %s" % (len(stats.bad_actions),
                                      stats.bad_actions[:2] or "none"))
    checks.hard("bridge_writes_state_shm", ctx.wait_shm(1.0), "%s" % ctx.shm_paths()[0])
    checks.hard("bridge_alive_after_stream", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    checks.soft("plugin_acks_accepted",
                not any("ack" in e for e in stats.tx_errors),
                "sent %d acks; first error: %s" % (stats.tx_acks,
                                                   (stats.tx_errors or ["none"])[0]))
    checks.soft("all_chefs_get_actions", set(stats.chef_ids()) >= set(range(chefs)),
                "chef ids seen: %s (expected 0..%d)" % (stats.chef_ids(), chefs - 1))
    checks.soft("connection_kept_open", link.error is None,
                "link error: %s" % (link.error or "none"))
    if stubs_ran:
        checks.soft("worker_slot_moves_forwarded", stats.count_action("move") > 0,
                    "move=%d stop=%d" % (stats.count_action("move"),
                                         stats.count_action(IDLE_ACTION)))
    checks.soft("echo_reply_roundtrip",
                bool(stats.echo_replies) and stats.echo_replies[0] == stats.echo_token,
                "echo token=%s, replies=%s (spec 未要求 Bridge 回 echo_reply，仅记录)"
                % (stats.echo_token, stats.echo_replies or "none"))
    checks.note("bridge_ack_messages", len(stats.bridge_acks))
    checks.note("bridge_message_types", dict(stats.by_type))
    checks.soft("log_shows_state_update", bool(ctx.log_grep("state_update")),
                "log evidence: %s" % (ctx.log_grep("state_update") or "none"))
    checks.soft("log_shows_action_dispatch", bool(ctx.log_grep("action_dispatch")),
                "log evidence: %s" % (ctx.log_grep("action_dispatch") or "none"))
    if args.require_echo_reply:
        checks.hard("require_echo_reply", bool(stats.echo_replies),
                    "replies=%s" % (stats.echo_replies or "none"))
    if args.require_bridge_ack:
        checks.hard("require_bridge_ack", bool(stats.bridge_acks),
                    "bridge never sent an ack (spec does not require it either)")
    link.close()

    return "%s in %.1fs; tx state=%d hb=%d acks=%d; bridge types=%s; echo_reply=%s" % (
        stats.describe(), args.duration, stats.tx_states, stats.tx_heartbeats,
        stats.tx_acks, dict(stats.by_type), "yes" if stats.echo_replies else "no")


def scenario_one_slot(ctx, checks):
    """W3：只写 1/N 个输入槽，缺失的那个厨师必须收到 idle，Bridge 不退。"""
    args = ctx.args
    chefs = 2
    stub_binary = Path(args.worker_stub)
    if not stub_binary.exists():
        raise Skipped("worker-stub not built (%s); this scenario needs a live slot writer "
                      "— build with: cd core && make" % stub_binary)

    ctx.clear_stale_shm()
    port = ctx.start_bridge_and_wait(link="listen", host=args.host, chefs=chefs,
                                     frame_timeout_ms=args.frame_timeout_ms,
                                     heartbeat_timeout_ms=args.heartbeat_timeout_ms,
                                     port=args.listen_port)

    # stub 必须挂到 Bridge 建的段上，所以先连上并发几帧再等段出现（段可能是启动时
    # 建的，也可能是第一帧才建的）
    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    priming = Stats("priming")
    run_exchange(link, priming, 1.0, chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=not args.no_ack,
                 flat_xy=args.flat_xy)
    checks.hard("bridge_writes_state_shm", ctx.wait_shm(3.0), "%s" % ctx.shm_paths()[0])

    checks.note("worker_stubs", "1 of %d slots (slot 0 only)" % chefs)
    ctx.start_stub(slot=0, chefs=chefs, hz=args.stub_hz, duration=args.duration + 6)

    warm = Stats("warmup")
    run_exchange(link, warm, 1.5, chefs=chefs, hz=args.hz, heartbeat_ms=args.heartbeat_ms,
                 ack_actions=not args.no_ack, flat_xy=args.flat_xy)
    checks.note("warmup", warm.describe())

    window = max(1.5, args.duration)
    stats = Stats("measure")
    run_exchange(link, stats, window, chefs=chefs, hz=args.hz, heartbeat_ms=args.heartbeat_ms,
                 ack_actions=not args.no_ack, flat_xy=args.flat_xy)
    link.close()

    chef0 = [a for a in stats.actions if a.get("chef_id") == 0]
    chef1 = [a for a in stats.actions if a.get("chef_id") == 1]
    seen1 = [a.get("action") for a in chef1]
    seen0 = [a.get("action") for a in chef0]

    checks.note("measured_window", "%s; chef0=%s chef1=%s"
                % (stats.describe(), Counter(seen0) or "none", Counter(seen1) or "none"))
    checks.hard("bridge_keeps_dispatching", len(stats.actions) >= 10,
                "got %d actions in %.1fs (link=%s, bridge alive=%s)"
                % (len(stats.actions), window, link.error or "open", ctx.bridge.alive()))
    checks.hard("live_slot_chef_gets_move", "move" in seen0,
                "chef 0 (slot written) actions=%s" % (Counter(seen0) or "none"))
    checks.hard("missing_slot_chef_gets_idle_stop", IDLE_ACTION in seen1,
                "chef 1 (no slot) actions=%s, expected at least one '%s'"
                % (Counter(seen1) or "none", IDLE_ACTION))
    checks.hard("bridge_alive_after_degradation", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())

    idle_hits = [ln for ln in ctx.log_text().splitlines()
                 if LOG_PATTERNS["idle_or_missing_slot"].search(ln)]
    named = [ln.strip() for ln in idle_hits if re.search(r"\b1\b", ln)]
    checks.soft("log_names_the_missing_chef", bool(named),
                "log lines mentioning slot/idle and chef 1: %s"
                % (named[:2] or (idle_hits[:2] or "none")))
    return ("%s in %.1fs; live chef 0 -> %s; missing chef 1 -> %s"
            % (stats.describe(), window, Counter(seen0) or "none", Counter(seen1) or "none"))


def scenario_no_heartbeat(ctx, checks):
    """W4：停心跳 -> 判离线并暂停下发；重连后必须恢复下发。

    心跳超时有两种合理语义：只看 heartbeat 消息，或看"任何入站消息"（插件自己的
    TcpLink 就是后者）。本场景把两种都跑一遍：只停心跳但仍发 state_update 时是否
    暂停属于 soft 证据；完全静默时必须暂停（两种语义都成立），这条是硬断言。
    """
    args = ctx.args
    chefs = 1
    timeout_ms = args.nb_timeout_ms
    hb_s = timeout_ms / 1000.0

    ctx.clear_stale_shm()
    port = ctx.start_bridge_and_wait(link="listen", host=args.host, chefs=chefs,
                                     frame_timeout_ms=args.frame_timeout_ms,
                                     heartbeat_timeout_ms=timeout_ms,
                                     port=args.listen_port)

    current = [None]

    def fresh_link():
        if current[0] is not None:
            current[0].close()
        link = Link(connect(args.host, port, args.ready_timeout))
        link.send_json(msg_hello())
        current[0] = link
        return link

    # 1) 基线：带心跳地跑一段，确认"正常时确实在下发"
    link = fresh_link()
    base = Stats("baseline")
    run_exchange(link, base, 1.5, chefs=chefs, hz=args.hz,
                 heartbeat_ms=min(300, args.heartbeat_ms), ack_actions=not args.no_ack,
                 flat_xy=args.flat_xy)
    checks.hard("baseline_dispatch", len(base.actions) >= 3,
                "%s in 1.5s" % base.describe())

    # 2) 只停心跳，state_update 继续发：看 Bridge 是否因此暂停
    ramp = Stats("hb-ramp")
    run_exchange(link, ramp, hb_s + 0.3, chefs=chefs, hz=args.hz, send_heartbeat=False,
                 ack_actions=not args.no_ack, flat_xy=args.flat_xy)
    tail = Stats("hb-tail")
    run_exchange(link, tail, 1.0, chefs=chefs, hz=args.hz, send_heartbeat=False,
                 ack_actions=not args.no_ack, flat_xy=args.flat_xy)
    paused_on_hb = not tail.actions
    checks.soft("heartbeat_loss_alone_pauses_dispatch", paused_on_hb,
                "state_update kept flowing for %.1fs after heartbeats stopped at %dms; "
                "%d actions in the tail second; link=%s"
                % (hb_s + 1.3, timeout_ms, len(tail.actions), link.error or "open"))
    if link.error:
        checks.note("bridge_closed_connection", link.error)

    # 3) 恢复一次，再完全静默：这样"暂停"不是因为没有帧，而是因为对端判死
    link = fresh_link()
    recovery = Stats("recovery")
    run_exchange(link, recovery, 1.2, chefs=chefs, hz=args.hz,
                 heartbeat_ms=min(300, args.heartbeat_ms), ack_actions=not args.no_ack,
                 flat_xy=args.flat_xy)
    checks.hard("dispatch_resumes_after_reconnect", len(recovery.actions) >= 1,
                "%s in 1.2s after reconnect; link=%s"
                % (recovery.describe(), link.error or "open"))

    drain = Stats("silence-drain")
    run_exchange(link, drain, hb_s + 0.5, chefs=chefs, hz=args.hz, send_state=False,
                 send_heartbeat=False, ack_actions=False)
    silent = Stats("silence-tail")
    collect(link, silent, 1.0)
    checks.hard("total_silence_pauses_dispatch", not silent.actions,
                "%d actions arrived in a 1.0s window %.1fs after the last byte"
                % (len(silent.actions), hb_s + 1.5))
    checks.hard("bridge_survives_heartbeat_timeout", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())

    # 4) 再重连一次，必须恢复下发（W4 的结论）
    link = fresh_link()
    resumed = Stats("resumed")
    run_exchange(link, resumed, max(1.5, args.duration / 2), chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=not args.no_ack,
                 flat_xy=args.flat_xy)
    checks.hard("dispatch_resumes_after_silence", len(resumed.actions) >= 1,
                "%s after reconnect+heartbeat; link=%s"
                % (resumed.describe(), link.error or "open"))
    link.close()

    hits = ctx.log_grep("heartbeat_timeout")
    checks.soft("log_mentions_heartbeat_or_disconnect", bool(hits),
                "log evidence: %s" % (hits or "none"))
    return ("baseline=%d actions, paused-on-heartbeat-loss=%s, silence-pause=%s, "
            "resumed=%d actions" % (len(base.actions), "yes" if paused_on_hb else "no",
                                    "yes" if not silent.actions else "no",
                                    len(resumed.actions)))


def scenario_bad_frames(ctx, checks):
    """W5：超长帧 / 非法 JSON / 截断帧各来一次，Bridge 必须活着并继续干活。"""
    args = ctx.args
    chefs = 1
    ctx.clear_stale_shm()
    port = ctx.start_bridge_and_wait(link="listen", host=args.host, chefs=chefs,
                                     frame_timeout_ms=args.frame_timeout_ms,
                                     heartbeat_timeout_ms=max(2000, args.heartbeat_timeout_ms),
                                     port=args.listen_port)
    pid0 = ctx.bridge.pid

    def exchange_ok(label, seconds):
        link = Link(connect(args.host, port, args.ready_timeout))
        link.send_json(msg_hello())
        stats = Stats(label)
        run_exchange(link, stats, seconds, chefs=chefs, hz=args.hz,
                     heartbeat_ms=min(300, args.heartbeat_ms),
                     ack_actions=not args.no_ack, flat_xy=args.flat_xy)
        return link, stats

    # A) 先证明这条链路本来是通的
    link, base = exchange_ok("baseline", 1.0)
    checks.hard("baseline_dispatch", len(base.actions) >= 1, base.describe())
    link.close()

    # B) >1 MiB 的帧：长度前缀就是越界的，Bridge 必须记下来而不是吃下去
    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    oversize = MAX_FRAME_BYTES + 1
    link.send_raw(struct.pack(">I", oversize) + b"x" * 65536)
    time.sleep(0.6)
    checks.hard("bridge_alive_after_oversize_frame", ctx.bridge.alive(),
                "sent declared length %d (>%d) + 64 KiB; exit=%s"
                % (oversize, MAX_FRAME_BYTES, ctx.bridge.exit_code()))
    # specs/bridge.md:56 —— 帧长越界 = 流已失步，必须断开重连并记录。
    # 探活是必须的：对端发来的 FIN 不 recv 就看不见，光看标志位会误判成"还连着"。
    dropped = link.probe_closed(0.6)
    oversize_dropped = dropped
    checks.hard("oversize_frame_drops_link", dropped,
                "declared length %d is out of range, so the stream is desynced and the "
                "bridge must drop the link (specs/bridge.md:56); link=%s"
                % (oversize, link.error or "still open"))
    link.close()

    # C1) 非法 JSON（分帧本身正确）：只丢这条消息，连接必须保留。
    #     这条必须独占一条连接：曾经把零长帧排在它前面，结果 Bridge 因为零长帧
    #     断了流，非法 JSON 那条根本没被读到，断言失败得不明不白。
    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    link.send_raw(encode_frame('{"type":"state_update","chefs":['))
    time.sleep(0.3)
    checks.hard("bridge_alive_after_bad_json", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    stats = Stats("after-bad-json")
    run_exchange(link, stats, 1.0, chefs=chefs, hz=args.hz,
                 heartbeat_ms=min(300, args.heartbeat_ms), ack_actions=not args.no_ack,
                 flat_xy=args.flat_xy)
    checks.hard("bad_json_keeps_connection", len(stats.actions) >= 1,
                "same connection: %s; link=%s" % (stats.describe(), link.error or "open"))
    link.close()

    # C2) 零长帧：长度 0 在上限之内，分帧本身没有失步（下一个长度前缀就在原处），
    #     参考实现 TcpLink.ExtractFrames 会照常切出"正文为空的一帧"，正文解析失败
    #     按 specs/bridge.md:55 属于"丢弃该消息并记录，不断连接"。所以：进程存活是
    #     硬要求，断不断连按 soft 记录（这条是当前实现对不上 spec 的地方之一）。
    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    link.send_raw(encode_frame(b""))
    time.sleep(0.4)
    checks.hard("bridge_alive_after_zero_length_frame", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    dropped = link.probe_closed(0.6)
    zero_dropped = dropped
    checks.soft("zero_length_frame_keeps_connection", not dropped,
                "bridge %s；零长帧的长度 0 未越界，分帧没有失步，"
                "参考实现按『正文为空的一帧』走完并丢弃（specs/bridge.md:55 不断连接）；"
                "link=%s"
                % ("kept the connection" if not dropped else "dropped the connection",
                   link.error or "open"))
    link.close()

    # D) 截断帧：声明 8192 字节只给 64 字节，然后断开半帧
    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    link.send_raw(struct.pack(">I", 8192) + b"{" * 64)
    time.sleep(0.4)
    link.close()
    time.sleep(0.4)
    checks.hard("bridge_alive_after_truncated_frame", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())

    # E) 之后仍要能正常处理一帧（W5 的收尾条件）
    link, after = exchange_ok("after-all-bad-frames", 1.2)
    checks.hard("recovers_after_bad_frames", len(after.actions) >= 1, after.describe())
    link.close()

    checks.hard("bridge_pid_unchanged", ctx.bridge.pid == pid0 and ctx.bridge.alive(),
                "pid %s -> %s (no restart expected)" % (pid0, ctx.bridge.pid))
    hits = ctx.log_grep("json_error") + ctx.log_grep("frame_error")
    checks.soft("log_records_bad_input", bool(hits),
                "log evidence: %s" % (hits[:3] or "none"))
    return ("baseline=%d, oversize->alive+link dropped=%s, bad-json->%d actions same "
            "connection, zero-length->link %s, truncated->alive, after-all=%d actions"
            % (len(base.actions), "yes" if oversize_dropped else "no",
               len(stats.actions), "dropped" if zero_dropped else "kept",
               len(after.actions)))


def scenario_connect_mode(ctx, checks):
    """W7：反转拓扑——我们自己监听 14455，Bridge 用 --link connect 连进来。"""
    args = ctx.args
    chefs = args.chefs or 1

    # 14455 是插件侧服务端的默认端口，优先用它；被别的进程占了就换一个空闲端口，
    # 反正 Bridge 的端口是我们用 --port 显式告诉它的
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    port = None
    for attempt in range(3):
        candidate = args.connect_port or pick_free_port(args.host, DEFAULT_CONNECT_PORT)
        try:
            server.bind((args.host, candidate))
            port = candidate
            break
        except OSError as exc:
            if args.connect_port is not None:
                server.close()
                raise InfraError("cannot listen on %s:%d: %s"
                                 % (args.host, candidate, exc))
            _say("    info       port %d unusable (%s); picking another" % (candidate, exc))
    if port is None:
        server.close()
        raise InfraError("no free port to listen on for connect-mode")
    server.listen(1)
    server.settimeout(args.ready_timeout)

    ctx.clear_stale_shm()
    try:
        ctx.start_bridge(link="connect", port=port, host=args.host, chefs=chefs,
                         frame_timeout_ms=args.frame_timeout_ms,
                         heartbeat_timeout_ms=args.heartbeat_timeout_ms)
        _say("    info       plugin listening on %s:%d, waiting for the bridge to dial in"
             % (args.host, port))
        try:
            conn, peer = server.accept()
        except (socket.timeout, OSError) as exc:
            raise InfraError("bridge never dialed %s:%d within %.1fs (%s; exit=%s)\n%s"
                             % (args.host, port, args.ready_timeout, exc,
                                ctx.bridge.exit_code(), ctx.diagnostics()))
    finally:
        server.close()

    checks.hard("bridge_dials_plugin_server", True, "accepted from %s:%d" % peer)
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    link = Link(conn, "plugin-server")
    link.send_json(msg_hello())
    stats = Stats("connect-mode")
    run_exchange(link, stats, max(1.5, args.duration / 2), chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=not args.no_ack,
                 echo_at=0.3, flat_xy=args.flat_xy)
    checks.hard("actions_dispatched_in_connect_mode", len(stats.actions) >= 1,
                stats.describe())
    checks.hard("actions_wellformed", not stats.bad_actions,
                "%d malformed" % len(stats.bad_actions))
    checks.hard("bridge_alive_in_connect_mode", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    checks.soft("listen_port_not_used",
                DEFAULT_LISTEN_PORT not in listening_ports(),
                "port %d %s (should stay unused in --link connect)"
                % (DEFAULT_LISTEN_PORT,
                   "is listening" if DEFAULT_LISTEN_PORT in listening_ports() else "free"))
    checks.soft("echo_reply_roundtrip",
                bool(stats.echo_replies) and stats.echo_replies[0] == stats.echo_token,
                "echo token=%s replies=%s (spec 未要求，仅记录)"
                % (stats.echo_token, stats.echo_replies or "none"))
    if args.require_echo_reply:
        checks.hard("require_echo_reply", bool(stats.echo_replies),
                    "replies=%s" % (stats.echo_replies or "none"))
    link.close()
    return ("dial-in ok from %s:%d; %s" % (peer[0], peer[1], stats.describe()))


SCENARIO_IMPL = {
    "happy": scenario_happy,
    "one-slot": scenario_one_slot,
    "no-heartbeat": scenario_no_heartbeat,
    "bad-frames": scenario_bad_frames,
    "connect-mode": scenario_connect_mode,
}


# --------------------------------------------------------------------------
# 自检：不起 Bridge，只验证本工具自己的编解码与消息形状
# --------------------------------------------------------------------------


def _start_peer(handler):
    """起一个本地 TCP 对端，handler(conn) 在后台线程里跑。"""
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]
    errors = []

    def run():
        conn = None
        try:
            conn, _ = server.accept()
            conn.settimeout(10)
            handler(conn)
        except Exception as exc:                      # noqa: BLE001 - 线程里的错误要带回主线程
            errors.append(exc)
        finally:
            try:
                if conn is not None:
                    conn.close()
            except OSError:
                pass
            server.close()

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return port, thread, errors


def _read_until(conn, predicate, timeout=5.0, limit=20):
    """读到第一个满足 predicate 的消息就返回（跳过 hello 之类的开场帧）。"""
    conn.settimeout(timeout)
    dec = FrameDecoder()
    for _ in range(limit):
        try:
            data = conn.recv(65536)
        except (socket.timeout, OSError):
            break
        if not data:
            break
        dec.feed(data)
        while True:
            payload = dec.next_payload()
            if payload is None:
                break
            try:
                msg = json.loads(payload)
            except ValueError:
                continue
            if isinstance(msg, dict) and predicate(msg):
                return [payload]
    return []


def selftest():
    checks = Checks("selftest", strict=True)
    _say("selftest: 验证 fake_plugin 自己的分帧编解码、消息形状与 ack 回路")

    # 1) 编码布局必须与 TcpLink.cs 一致：4 字节大端长度 + UTF-8
    payload = '{"type":"heartbeat","timestamp":1}'
    raw = encode_frame(payload)
    bits = payload.encode("utf-8")
    checks.hard("encode_layout",
                raw == struct.pack(">I", len(bits)) + bits
                and raw[:4] == bytes([0, 0, 0, len(bits)]),
                "%d bytes, prefix=%s" % (len(raw), raw[:4].hex()))

    utf8 = "订单-001 汤 🍲"
    frame = encode_frame(utf8)
    checks.hard("length_prefix_counts_bytes_not_chars",
                struct.unpack(">I", frame[:4])[0] == len(utf8.encode("utf-8"))
                and len(utf8.encode("utf-8")) != len(utf8),
                "chars=%d bytes=%d" % (len(utf8), len(utf8.encode("utf-8"))))

    # 2) 通过一条真的 TCP 连接跑一次往返（自己当对端）
    small = '{"type":"action","chef_id":0,"action":"stop"}'
    # 正好撑满上限的合法 JSON：越界一字节就要被拒，正好等于上限必须放过
    pad = "a" * (MAX_FRAME_BYTES - len('{"type":"action","chef_id":0,"action":"stop","pad":""}'))
    big = '{"type":"action","chef_id":0,"action":"stop","pad":"%s"}' % pad
    sent = [small, "", big]

    def peer(conn):
        for item in sent:
            conn.sendall(encode_frame(item))

    port, thread, errors = _start_peer(peer)
    sock = connect("127.0.0.1", port, 5.0)
    link = Link(sock)
    got = []
    deadline = time.monotonic() + 5.0
    while len(got) < 3 and time.monotonic() < deadline:
        for msg in link.pump(0.2):
            got.append(msg)
    link.close()
    thread.join(timeout=5)
    decoded = [m.get("_frame_error", m) for m in got]
    checks.hard("tcp_roundtrip_three_frames", len(got) == 3 and not errors,
                "got %d frames, peer errors=%s" % (len(got), errors or "none"))
    checks.hard("tcp_roundtrip_zero_length_frame",
                len(got) == 3 and decoded[1] == "zero-length frame",
                "frame[1]=%r" % (decoded[1] if len(got) > 1 else None))
    checks.hard("tcp_roundtrip_max_size_frame",
                len(got) == 3 and isinstance(decoded[2], dict)
                and decoded[2].get("type") == "action"
                and len(decoded[2].get("pad", "")) == len(pad),
                "frame[2] %d bytes payload (%d = MAX_FRAME_BYTES)"
                % (len(big.encode("utf-8")), MAX_FRAME_BYTES))

    # 3) 粘包：三个帧一次 write
    def cohesive(conn):
        conn.sendall(encode_frame(small) + encode_frame(small) + encode_frame(small))

    port, thread, errors = _start_peer(cohesive)
    link = Link(connect("127.0.0.1", port, 5.0))
    got = []
    deadline = time.monotonic() + 5.0
    while len(got) < 3 and time.monotonic() < deadline:
        got.extend(link.pump(0.2))
    link.close()
    thread.join(timeout=5)
    checks.hard("coalesced_frames_split_correctly",
                len(got) == 3 and all(m.get("type") == "action" for m in got),
                "%d frames from one write" % len(got))

    # 4) 拆包：一个字节一个字节发，最后才应解出消息
    def dribble(conn):
        for byte in encode_frame(small):
            conn.sendall(bytes([byte]))
            time.sleep(0.002)

    port, thread, errors = _start_peer(dribble)
    link = Link(connect("127.0.0.1", port, 5.0))
    got = []
    deadline = time.monotonic() + 5.0
    while not got and time.monotonic() < deadline:
        got.extend(link.pump(0.2))
    checks.hard("split_frame_decoded_once_complete",
                len(got) == 1 and got[0].get("type") == "action",
                "%d frames from byte-by-byte delivery" % len(got))
    link.close()
    thread.join(timeout=5)

    # 5) 截断帧：只留在缓冲区，不吐半条消息
    def truncate(conn):
        conn.sendall(encode_frame(small) + struct.pack(">I", 100) + b"{" * 40)
        time.sleep(0.6)

    port, thread, errors = _start_peer(truncate)
    link = Link(connect("127.0.0.1", port, 5.0))
    got = []
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        got.extend(link.pump(0.1))
    pending = link.dec.pending
    checks.hard("truncated_frame_stays_pending",
                len(got) == 1 and pending == 44,
                "frames=%d pending_bytes=%d (expected 4+40)" % (len(got), pending))
    link.close()
    thread.join(timeout=5)

    # 6) 越界与"负数"长度必须抛 FramingError（C# 侧读成负数，无符号视角就是 >2^31）
    dec = FrameDecoder()
    dec.feed(struct.pack(">I", MAX_FRAME_BYTES + 1))
    try:
        dec.next_payload()
        oversize_ok = False
    except FramingError:
        oversize_ok = True
    checks.hard("oversize_length_rejected", oversize_ok,
                "declared %d" % (MAX_FRAME_BYTES + 1))

    dec = FrameDecoder()
    dec.feed(b"\xff\xff\xff\xff")
    try:
        dec.next_payload()
        negative_ok = False
    except FramingError:
        negative_ok = True
    checks.hard("negative_length_rejected", negative_ok, "declared 0xFFFFFFFF")

    # 7) §5.2 的 state_update 字段集必须逐字对得上
    state = json.loads(msg_state_update(2, 7))
    chefs_keys = set(state["chefs"][0]) if state.get("chefs") else set()
    checks.hard("state_update_top_level_keys",
                set(state) == {"type", "timestamp", "game_time", "score", "chefs",
                               "orders", "resources"},
                "keys=%s" % sorted(state))
    checks.hard("state_update_chef_shape",
                chefs_keys == {"id", "position", "holding", "state"}
                and set(state["chefs"][0]["position"]) == {"x", "y"},
                "chef keys=%s" % sorted(chefs_keys))
    checks.hard("state_update_order_shape",
                set(state["orders"][0]) == {"id", "recipe", "time_remaining",
                                            "ingredients"}
                and state["orders"][0]["id"] == "order_001",
                "order=%s" % state["orders"][0])
    checks.hard("state_update_resources_shape",
                set(state["resources"]) == {"pot_0", "pot_1", "cutting_board_0",
                                            "plate_stack"}
                and set(state["resources"]["pot_0"]) == {"state", "content", "time_left"}
                and set(state["resources"]["pot_1"]) == {"state"}
                and set(state["resources"]["cutting_board_0"]) == {"state", "chef_id"}
                and set(state["resources"]["plate_stack"]) == {"count"},
                "resources=%s" % sorted(state["resources"]))
    edge = json.loads(msg_state_update(1, 0, edge_fields=True))
    checks.hard("edge_fields_are_extra_not_replacing",
                "pot_7" in edge["resources"] and edge["resources"]["plate_stack"]["count"] == 0
                and "unknown_field" in edge,
                "resources=%s" % sorted(edge["resources"]))

    # 8) hello 必须与冻结的那条一致
    hello = json.loads(msg_hello())
    checks.hard("hello_matches_frozen_protocol",
                hello == {"type": "hello", "instance": "windows-host", "version": "0.1.0",
                          "actions": ["move", "stop", "press", "release", "tap",
                                      "pickup", "dash", "interact"]},
                msg_hello())
    ack = json.loads(msg_ack("move", 3, True, "ok"))
    checks.hard("ack_matches_messages_cs",
                set(ack) == {"type", "action", "chef_id", "ok", "detail"}
                and ack["chef_id"] == 3 and ack["ok"] is True,
                msg_ack("move", 3, True, "ok"))
    echo = json.loads(msg_echo("tok-1"))
    checks.hard("echo_shape", set(echo) == {"type", "token"} and echo["token"] == "tok-1",
                msg_echo("tok-1"))

    # 9) action 校验器：合法的过、缺字段的必须被抓住
    checks.hard("validate_action_accepts_move",
                validate_action({"type": "action", "chef_id": 0, "action": "move",
                                 "target": {"x": 1, "y": 2}}) == [],
                "move with target")
    checks.hard("validate_action_accepts_idle_stop",
                validate_action({"type": "action", "chef_id": 1, "action": "stop"}) == [],
                "idle form")
    checks.hard("validate_action_rejects_broken",
                len(validate_action({"type": "action", "chef_id": "0", "action": "move"})) >= 2,
                "%s" % validate_action({"type": "action", "chef_id": "0", "action": "move"}))

    # 10) 整条链路：对端（假 Bridge）发 5 条消息，我们回 ack，对端收到同 chef_id 的 ack
    received = []

    def fake_bridge(conn):
        conn.sendall(encode_frame(msg_heartbeat(1)))
        conn.sendall(encode_frame(jdump({"type": "echo_reply", "token": "tok-9"})))
        conn.sendall(encode_frame(msg_ack("move", 0, True, "plugin-side ack")))
        conn.sendall(encode_frame(jdump({"type": "action", "chef_id": 0, "action": "move",
                                         "target": {"x": 1, "y": 2}})))
        conn.sendall(encode_frame("{not json"))
        received.extend(_read_until(conn, lambda m: m.get("type") == "ack", timeout=5.0))

    port, thread, errors = _start_peer(fake_bridge)
    link = Link(connect("127.0.0.1", port, 5.0))
    link.send_json(msg_hello())
    stats = Stats("selftest")
    run_exchange(link, stats, 0.6, chefs=1, hz=30, heartbeat_ms=200, ack_actions=True)
    link.close()
    thread.join(timeout=5)
    acked = json.loads(received[0]) if received else {}
    checks.hard("link_loop_receives_all_message_types",
                len(stats.actions) == 1 and stats.echo_replies == ["tok-9"]
                and len(stats.bridge_acks) == 1 and len(stats.bad_payloads) == 1,
                "actions=%d echo_replies=%s acks=%d bad_payloads=%d"
                % (len(stats.actions), stats.echo_replies, len(stats.bridge_acks),
                   len(stats.bad_payloads)))
    checks.hard("link_loop_acks_actions_back",
                acked.get("type") == "ack" and acked.get("chef_id") == 0
                and acked.get("action") == "move",
                "peer received: %s" % (received[0] if received else "nothing"))

    _say("")
    if checks.failures:
        for name, detail in checks.failures:
            _say("SELFTEST FAIL %s — %s" % (name, detail))
        print("SELFTEST FAIL: %d 项未通过" % len(checks.failures), file=sys.stderr)
        return EXIT_FAIL
    _say("SELFTEST PASS: 分帧往返、粘包/拆包/截断/越界、§5.2 消息形状、ack 回路全部通过")
    return EXIT_OK


# --------------------------------------------------------------------------
# 入口
# --------------------------------------------------------------------------


def parse_args(argv):
    parser = argparse.ArgumentParser(
        prog="fake_plugin.py",
        description="协议级模拟插件：不启动游戏就把 Bridge 的场景跑完，用退出码给结论。",
        epilog="退出码: 0 通过 / 1 断言失败 / 2 环境错误 / 77 跳过（前置产物缺失）")
    parser.add_argument("--scenario", choices=SCENARIOS, help="要跑的场景")
    parser.add_argument("--selftest", action="store_true",
                        help="只验证本工具自身的编解码与消息形状，不需要 Bridge")
    parser.add_argument("--list-scenarios", action="store_true", help="列出场景名后退出")
    parser.add_argument("--bridge",
                        default=os.environ.get(
                            "KITCHENOS_BRIDGE",
                            str(REPO_ROOT / "core" / "build" / "kitchenos-bridge")),
                        help="kitchenos-bridge 路径（或设 $KITCHENOS_BRIDGE）")
    parser.add_argument("--worker-stub",
                        default=os.environ.get(
                            "KITCHENOS_WORKER_STUB",
                            str(REPO_ROOT / "core" / "build" / "kitchenos-worker-stub")),
                        help="kitchenos-worker-stub 路径（或设 $KITCHENOS_WORKER_STUB）")
    parser.add_argument("--host", default="127.0.0.1", help="listen 模式为绑定地址，connect 模式为拨号目标")
    parser.add_argument("--listen-port", type=int, default=None,
                        help="listen 模式端口；默认由内核挑一个空闲端口，"
                             "要复现协议默认值就传 12345")
    parser.add_argument("--connect-port", type=int, default=None,
                        help="connect 模式端口；默认优先 14455，被占则换空闲端口")
    parser.add_argument("--chefs", type=int, default=None, help="厨师数（默认 happy=2, connect-mode=1）")
    parser.add_argument("--state-name", default=None, help="共享内存状态对象名")
    parser.add_argument("--sem-name", default=None, help="就绪信号量名")
    parser.add_argument("--state-prefix", default="kitchenos_c04",
                        help="未显式给名字时用的前缀，保证场景之间互不干扰")
    parser.add_argument("--log", default=None, help="Bridge 日志路径")
    parser.add_argument("--duration", type=float, default=4.0, help="happy 场景的时长（秒）")
    parser.add_argument("--hz", type=float, default=60.0, help="state_update 频率")
    parser.add_argument("--heartbeat-ms", type=int, default=500, help="心跳间隔")
    parser.add_argument("--heartbeat-timeout-ms", type=int, default=1500,
                        help="传给 Bridge 的心跳超时")
    parser.add_argument("--nb-timeout-ms", type=int, default=800,
                        help="no-heartbeat 场景使用的心跳超时（要短，否则测试很慢）")
    parser.add_argument("--frame-timeout-ms", type=int, default=200, help="传给 Bridge 的帧屏障超时")
    parser.add_argument("--min-actions", type=int, default=20,
                        help="happy 场景要求的最少 action 数（idle 降级路径下每帧只有 N 条）")
    parser.add_argument("--ready-timeout", type=float, default=10.0, help="等 Bridge 就绪的上限")
    parser.add_argument("--stub-hz", type=int, default=30, help="worker stub 的写槽频率")
    parser.add_argument("--no-stubs", action="store_true", help="不启动 worker stub，只测 idle 路径")
    parser.add_argument("--no-ack", action="store_true", help="不回 ack（模拟插件不回执）")
    parser.add_argument("--flat-xy", action="store_true",
                        help="state_update 额外带顶层 x/y（兼容扁平解析）")
    parser.add_argument("--strict", action="store_true", help="把 soft 检查升级为硬断言")
    parser.add_argument("--require-echo-reply", action="store_true",
                        help="把 echo_reply 往返当硬断言（spec 未要求 Bridge 回，默认只记录）")
    parser.add_argument("--require-bridge-ack", action="store_true",
                        help="把 Bridge 主动发 ack 当硬断言（spec 未要求，默认只记录）")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    if args.list_scenarios:
        for name in SCENARIOS:
            _say(name)
        return EXIT_OK
    if args.selftest:
        return selftest()
    if not args.scenario:
        print("ERROR: 需要 --scenario NAME 或 --selftest（见 --help）", file=sys.stderr)
        return EXIT_ERROR

    checks = Checks(args.scenario, strict=args.strict)
    ctx = Ctx(args, args.scenario)
    summary = None
    _say("== scenario %s ==" % args.scenario)
    status = EXIT_OK
    try:
        summary = SCENARIO_IMPL[args.scenario](ctx, checks)
    except Skipped as exc:
        print("SKIP %s: %s" % (args.scenario, exc), file=sys.stderr)
        status = EXIT_SKIP
    except InfraError as exc:
        print("ERROR %s: %s" % (args.scenario, exc), file=sys.stderr)
        status = EXIT_ERROR
    except Exception:                                  # noqa: BLE001 - 兜底并保留证据
        print("ERROR %s: harness exception" % args.scenario, file=sys.stderr)
        traceback.print_exc()
        print(ctx.diagnostics(), file=sys.stderr)
        status = EXIT_ERROR
    finally:
        try:
            ctx.stop_all()
        except Exception:                              # noqa: BLE001 - 清理失败不该掩盖结论
            traceback.print_exc()

    if status == EXIT_OK and checks.failures:
        status = EXIT_FAIL

    if status == EXIT_OK:
        _say("PASS %s: %s" % (args.scenario, summary))
    elif status == EXIT_FAIL:
        detail = "; ".join("%s (%s)" % (n, d) for n, d in checks.failures)
        print("FAIL %s: %d 条断言未通过: %s" % (args.scenario, len(checks.failures), detail),
              file=sys.stderr)
        print(ctx.diagnostics(), file=sys.stderr)
    elif status == EXIT_ERROR:
        print(ctx.diagnostics(), file=sys.stderr)
    return status


if __name__ == "__main__":
    # timeout/runner 发 SIGTERM 时也要走 finally 里的子进程清理
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(EXIT_ERROR))
    sys.exit(main())
