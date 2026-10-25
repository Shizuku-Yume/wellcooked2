#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""协议级模拟插件：不启动游戏就能把 Bridge 的全部场景跑完。

本文件扮演 Windows 侧插件在 PROTOCOL 层的对端，说的是同一套话：
一行一条 UTF-8 JSON，'\n' 结尾（protocol/protocol.md §1），字段名以
plugin/WellCooked.Plugin/ 与 overcooked_os_course_project.md §5.2 为基准
（分帧基准是 Net/TcpLink.cs 的 MaxFrameBytes/ExtractFrames，消息基准是
Protocol/Messages.cs）。

每个场景自己判定成败并用退出码表达结论，失败点写 stderr，不需要人翻日志：

    0   全部断言通过
    1   有断言失败（stderr 点名是哪一条、期望什么、实际什么）
    2   环境错误（Bridge 二进制缺失/起不来、端口占用等），不是断言结论
    77  跳过：前置产物不在（例如 kitchenos-worker-stub 还没构建）

用法见 --help；批量跑用 tools/run-bridge-scenarios.sh。
"""

import argparse
import ctypes
import json
import math
import os
import re
import select
import signal
import socket
import struct
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from collections import Counter
from pathlib import Path

# --------------------------------------------------------------------------
# 协议常量：这些数字与名字来自插件实现，改了就不再是同一个协议
# --------------------------------------------------------------------------

MAX_FRAME_BYTES = 1024 * 1024   # TcpLink.cs 的 MaxFrameBytes：一行的上限，不含结尾的 '\n'
DEFAULT_LISTEN_PORT = 14455     # Bridge 作服务端时的默认端口（两端同一个号，谁在听由模式决定）
DEFAULT_CONNECT_PORT = 14455    # 插件作服务端时的默认端口，Bridge --link connect 去连它
HELLO_INSTANCE = "windows-host"
HELLO_VERSION = "0.2.0"
HELLO_ACTIONS = ("move", "stop", "press", "release", "tap", "pickup", "dash", "interact",
                 "path", "face", "work")
IDLE_ACTION = "stop"            # 输入槽缺失时 Bridge 下发的空闲动作
# 线上 action 枚举（protocol/protocol.md §3.6 + schema/action.schema.json）：插件会做八个，
# 桥能下发的也是这八个（path/face/work 在 kitchen-control 任务 02 上线）。
# 收到别的名字说明 Bridge 与契约不一致，要当硬错误。
WIRE_ACTIONS = ("move", "stop", "interact", "pickup", "dash", "path", "face", "work")

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_ERROR = 2
EXIT_SKIP = 77

SCENARIOS = ("happy", "one-slot", "no-heartbeat", "bad-frames", "connect-mode",
             "reconnect-storm", "silent-connect", "data-shapes", "second-peer",
             "pickup-dash", "path-face-work", "input-grace", "os-loop", "os-resources",
             "nav-corridor", "nav-stuck", "nav-crash", "macro")

# 日志行 = "ISO8601(毫秒) 级别 事件名 key=value ..."（bridge/src/log.c）。
# 只认行首这个位置上的事件名：key=value 里出现同名词不算一次事件。
LOG_LINE = re.compile(r"^\d{4}-\d\d-\d\dT[\d:.]+ +\w+ +(\S+)")

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
# 分帧：一行一条 UTF-8 JSON，'\n' 结尾，单行上限 1 MiB
# --------------------------------------------------------------------------


def encode_frame(payload):
    """把 str/dict/bytes 打成一帧：载荷 + 一个 '\n'。

    载荷里带裸换行符就没法分帧（对端只会把它读成两行），所以这里与越界一样直接报错，
    而不是悄悄发出去。上限与 C# 侧 MaxFrameBytes 一样，不含结尾的 '\n'。
    """
    if isinstance(payload, dict):
        payload = json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    if len(payload) > MAX_FRAME_BYTES:
        raise FramingError("payload %d > %d" % (len(payload), MAX_FRAME_BYTES))
    if b"\n" in payload:
        raise FramingError("payload contains a raw newline")
    return payload + b"\n"


class FrameDecoder:
    """增量解帧器：喂多少字节都行，吐出一条条完整的行。"""

    def __init__(self, max_frame=MAX_FRAME_BYTES):
        self._buf = bytearray()
        self._max = max_frame

    def feed(self, data):
        self._buf += data

    @property
    def pending(self):
        return len(self._buf)

    def next_payload(self):
        """返回下一行的 payload（str）；不完整返回 None；超长行抛 FramingError。

        注意零长行（一个孤立的换行符）返回空串而不是 None，调用方必须用 `is None` 判断。
        超长行是不可恢复的：没有换行符就没有重新同步的位置，所以这里报错让调用方断链，
        而不是像坏 JSON 那样丢掉这一条继续读。
        """
        nl = self._buf.find(b"\n")
        if nl < 0:
            # 还在收；只有连上限都过了才说明这一行永远不会结束
            if len(self._buf) > self._max:
                raise FramingError("unterminated line of %d bytes exceeds limit %d"
                                   % (len(self._buf), self._max))
            return None
        if nl > self._max:
            raise FramingError("line length %d exceeds limit %d" % (nl, self._max))
        payload = bytes(self._buf[:nl])
        del self._buf[:nl + 1]
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


def msg_ack(action, chef_id, ok=True, detail="", seq=None):
    out = {"type": "ack", "action": action or "", "chef_id": int(chef_id),
           "ok": bool(ok), "detail": detail or ""}
    if seq is not None:
        out["seq"] = int(seq)
    return jdump(out)


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
        chef = {"id": i, "position": {"x": x, "y": y}, "facing": 0.0,
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
        "game_time_ms": int((frame_index / 60.0) * 1000),
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


def msg_state_update_os(chefs, frame_index, cells, positions=None, game_ms=None,
                        resources=None, holdings=None, facings=None, orders=None):
    """os-loop / macro 用：厨师站在 kitchen 网格里，订单用真实菜名。

    positions 给出时用它（NavSim 模拟的实时坐标）；否则按 picks 放进网格。
    holdings/facings/orders 由 WorldSim 提供：宏动作的完成条件读的就是这些字段，
    场景不模拟它们的话 pickup/chop 只能等超时。resources 直接覆盖默认的盘子堆。
    game_ms 由调用方按状态帧率给，确保 Worker/navd 与插件看到同一个游戏时钟。
    """
    picks = [cells[0], cells[-1], cells[len(cells) // 2], cells[len(cells) // 4]]
    out = []
    for i in range(chefs):
        if positions is not None and i < len(positions):
            cell = positions[i]
        else:
            cell = picks[i % len(picks)]
        holding = holdings[i] if holdings is not None and i < len(holdings) else None
        facing = facings[i] if facings is not None and i < len(facings) else 0.0
        out.append({"id": i, "position": {"x": cell[0], "y": cell[1]},
                    "facing": facing, "holding": holding, "state": "idle"})
    if orders is None:
        orders = [
            {"id": "world-001", "recipe": "SteamedSpecial_Fish",
             "time_remaining": 90, "ingredients": ["fish"]},
            {"id": "world-002", "recipe": "SteamedSpecial_Meat",
             "time_remaining": 90, "ingredients": ["meat"]},
        ]
    msg = {
        "type": "state_update",
        "timestamp": int(time.time()),
        "game_time": int((game_ms or 0) // 1000),
        "game_time_ms": int(game_ms or 0),
        "score": 0,
        "chefs": out,
        "orders": orders,
        "resources": resources if resources is not None else
                     {"plate_stack": {"count": 5}},
    }
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
    elif action == "interact":
        target_object = msg.get("target_object")
        if not isinstance(target_object, str) or not target_object:
            errs.append("interact without a target_object")
    elif action == "path":
        seq = msg.get("seq")
        if not isinstance(seq, int) or isinstance(seq, bool) or seq < 0:
            errs.append("path without non-negative seq")
        waypoints = msg.get("waypoints")
        if not isinstance(waypoints, list) or len(waypoints) < 1 or len(waypoints) > 8:
            errs.append("path waypoints must be list of 1..8 items")
        else:
            for idx, wp in enumerate(waypoints):
                if not isinstance(wp, dict) or not isinstance(wp.get("x"), (int, float)) or not isinstance(wp.get("y"), (int, float)):
                    errs.append("path waypoint %d invalid" % idx)
    elif action == "face":
        target = msg.get("target")
        target_obj = msg.get("target_object")
        has_target = isinstance(target, dict) and isinstance(target.get("x"), (int, float)) and isinstance(target.get("y"), (int, float))
        has_obj = isinstance(target_obj, str) and bool(target_obj)
        if not has_target and not has_obj:
            errs.append("face requires target or target_object")
    elif action == "work":
        target_obj = msg.get("target_object")
        if not isinstance(target_obj, str) or not target_obj:
            errs.append("work without a target_object")
    elif action not in WIRE_ACTIONS:
        errs.append("action %r is not on the wire (allowed: %s)"
                    % (action, "/".join(WIRE_ACTIONS)))
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
                    # 零长行（空行）：按 W1 属于"要能承受"的输入，记下来但不当作消息
                    out.append({"_frame_error": "zero-length frame"})
                    continue
                try:
                    out.append(json.loads(payload))
                except ValueError as exc:
                    out.append({"_frame_error": "invalid JSON: %s" % exc,
                                "_raw": payload[:160]})
        except FramingError as exc:
            # 行超长 = 流已失步（没有换行符就没有重新同步的位置），直接判连接不可用
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

    def close_rst(self):
        """强制 RST 关闭：SO_LINGER=0 让 close() 直接发 RST，而不是 FIN。

        游戏崩溃、进程被杀时对端看到的就是这个；RST 会把还没读走的字节一起丢掉，
        所以"Bridge 重连时把解码器复位"这件事只有这条路径能验：残留的半行若没被
        清掉，下一条流的第一行就会被读成上一行的尾巴。
        """
        if self.closed:
            return
        self.closed = True
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                 struct.pack("ii", 1, 0))
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


def proc_fds(pid):
    """{fd: 目标}：/proc/<pid>/fd 的真实内容，用来做泄漏对比。

    只数个数不够：一个泄漏的 socket 和一条新开的日志 fd 在计数上长得一样。
    所以连目标一起带回来，断言比的是"socket: 的个数"和整张表，泄漏时还能说出
    多出来的是哪一个 fd。
    """
    fds = {}
    try:
        names = os.listdir("/proc/%d/fd" % pid)
    except OSError as exc:
        raise InfraError("cannot list /proc/%d/fd: %s" % (pid, exc))
    for name in names:
        try:
            fds[int(name)] = os.readlink("/proc/%d/fd/%s" % (pid, name))
        except (OSError, ValueError):
            continue
    return fds


def socket_fds(fds):
    return {fd: target for fd, target in fds.items() if target.startswith("socket:")}


def proc_rss_kb(pid):
    """VmRSS：fd 之外的另一类泄漏（解码器缓冲区、日志行缓冲）也值得看一眼。"""
    try:
        for line in Path("/proc/%d/status" % pid).read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        pass
    return None


def describe_fds(fds):
    return "%d fds (%d sockets)" % (len(fds), len(socket_fds(fds)))


def fd_delta(before, after):
    """多出来的 fd 描述；没有新 fd 时返回空串。"""
    extra = [fd for fd in sorted(after) if fd not in before]
    if not extra:
        return ""
    return "new fds: " + ", ".join("%d -> %s" % (fd, after[fd]) for fd in extra)


def log_event_counts(ctx):
    """日志里各事件名出现次数（事件名只认行首那一列）。"""
    counts = Counter()
    for line in ctx.log_text().splitlines():
        match = LOG_LINE.match(line)
        if match:
            counts[match.group(1)] += 1
    return counts


def wait_log_events(ctx, event, minimum, timeout):
    """等到某个事件至少出现 minimum 次；返回是否等到。"""
    deadline = time.monotonic() + timeout
    while True:
        if log_event_counts(ctx)[event] >= minimum:
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.02)


def live_link(host, port, timeout=5.0):
    """连上 Bridge 并发一句 hello：所有多会话场景的开场都一样。"""
    link = Link(connect(host, port, timeout))
    link.send_json(msg_hello())
    return link


def collect(link, stats, duration):
    """只收不发，把窗口内收到的帧记进 stats。"""
    end = time.monotonic() + duration
    while time.monotonic() < end:
        for msg in link.pump(min(0.05, max(0.0, end - time.monotonic()))):
            _absorb(msg, stats, None, False, None)


def run_exchange(link, stats, duration, *, chefs=1, hz=60.0, heartbeat_ms=500,
                 send_state=True, send_heartbeat=True, ack_actions=True,
                 echo_at=None, edge_at=None, flat_xy=False, state_builder=None):
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
            if state_builder is not None:
                payload = state_builder(chefs, stats.frame_index)
            else:
                payload = msg_state_update(chefs, stats.frame_index, flat_xy=flat_xy,
                                           edge_fields=False)
            if _safe_send(link, stats, payload, "state_update"):
                stats.tx_states += 1
                stats.frame_index += 1
            next_state += period
            if next_state < now:      # 落后太多就直接对齐，不补发风暴
                next_state = now + period

        if edge_at is not None and not edge_sent and now - start >= edge_at and now < end:
            if state_builder is not None:
                payload = state_builder(chefs, stats.frame_index)
            else:
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
            action = msg.get("action", "")
            seq = msg.get("seq") if action == "path" else None
            if _safe_send(link, stats, msg_ack(action,
                                               chef_id if isinstance(chef_id, int) else -1,
                                               ok, "fake_plugin executed" if ok else "rejected",
                                               seq),
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

    多 agent 并行跑测试时 14455 上可能蹲着别人的 Bridge，光看"端口在监听"会把
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


class SupervisorProc:
    """被测试的整棵进程树：Supervisor 自己拉起 Bridge/Scheduler/Workers。"""

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

    def terminate(self, timeout=5.0):
        if self.proc is None:
            return True, 0.0, None
        if not self.alive():
            self.term_ok = True
            self.term_seconds = 0.0
            return True, 0.0, self.exit_code()
        start = time.monotonic()
        try:
            self.proc.send_signal(signal.SIGTERM)
        except OSError:
            pass
        while time.monotonic() - start < timeout:
            if not self.alive():
                self.term_ok = True
                self.term_seconds = time.monotonic() - start
                return True, self.term_seconds, self.exit_code()
            time.sleep(0.05)
        try:
            os.killpg(os.getpgid(self.pid), signal.SIGKILL)
        except OSError:
            pass
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass
        self.term_ok = False
        self.term_seconds = time.monotonic() - start
        return False, self.term_seconds, self.exit_code()

    def console_text(self):
        return _tail(self.console_path, 40)


# --------------------------------------------------------------------------
# 共享内存世界状态：按 shm.h 的布局直接读，证明 Bridge 究竟写进去了什么
# --------------------------------------------------------------------------
#
# 为什么不用日志代替：state_update 那行只报"解析出来几个"，报不出"映射进段里
# 之后还剩几个"（两处上限不同：PROTO_MAX_ORDERS=32、KOS_SHM_MAX_ORDERS=16）。
# 数据形状这类边界要判的就是映射后的结果，所以按段布局读原始内存。
#
# 这里的结构体是 shm.h 的**镜像**，不是同一份定义：字段一旦漂移，段头里那些
# 尺寸字段就会对不上，read_world() 会直接报 InfraError 而不是给出错位的结论。


class _Chef(ctypes.Structure):
    _fields_ = [("id", ctypes.c_int32), ("x", ctypes.c_float), ("y", ctypes.c_float),
                ("facing", ctypes.c_float),
                ("holding", ctypes.c_char * 32), ("state", ctypes.c_char * 24)]


class _Order(ctypes.Structure):
    _fields_ = [("id", ctypes.c_char * 32), ("recipe", ctypes.c_char * 32),
                ("time_remaining", ctypes.c_int32), ("ingredient_count", ctypes.c_int32),
                ("ingredients", (ctypes.c_char * 24) * 6)]


class _Resource(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * 32), ("state", ctypes.c_char * 24),
                ("content", ctypes.c_char * 32), ("time_left", ctypes.c_float),
                ("progress", ctypes.c_float),
                ("chef_id", ctypes.c_int32), ("count", ctypes.c_int32)]


class _World(ctypes.Structure):
    _fields_ = [("score", ctypes.c_int64), ("game_time", ctypes.c_int64),
                ("game_time_ms", ctypes.c_int64),
                ("chef_count", ctypes.c_uint32), ("reserved0", ctypes.c_uint32),
                ("chefs", _Chef * 8),
                ("order_count", ctypes.c_uint32), ("reserved1", ctypes.c_uint32),
                ("orders", _Order * 16),
                ("resource_count", ctypes.c_uint32), ("reserved2", ctypes.c_uint32),
                ("resources", _Resource * 32),
                ("write_seq", ctypes.c_uint64), ("checksum", ctypes.c_uint32),
                ("reserved3", ctypes.c_uint32)]


class _SegmentHeader(ctypes.Structure):
    _fields_ = [("magic", ctypes.c_uint32), ("version", ctypes.c_uint32),
                ("header_size", ctypes.c_uint32), ("world_size", ctypes.c_uint32),
                ("slot_size", ctypes.c_uint32), ("slot_count", ctypes.c_uint32),
                ("max_chefs", ctypes.c_uint32), ("max_orders", ctypes.c_uint32),
                ("max_resources", ctypes.c_uint32),
                ("max_order_ingredients", ctypes.c_uint32),
                ("creator_pid", ctypes.c_int64), ("created_at_ms", ctypes.c_int64),
                ("world_seq", ctypes.c_uint64), ("reserved", ctypes.c_uint32 * 4)]


KOS_SHM_MAGIC = 0x4B4F5331
KOS_SHM_WORLD_CHECKSUM_OFFSET = _World.checksum.offset


def fnv1a_words(data):
    """shm.c 的 checksum_bytes()：按 64 位小端字做 FNV-1a，最后折成 32 位。"""
    h = 14695981039346656037
    i = 0
    while i + 8 <= len(data):
        h = ((h ^ int.from_bytes(data[i:i + 8], "little")) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        i += 8
    while i < len(data):
        h = ((h ^ data[i]) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        i += 1
    return (h ^ (h >> 32)) & 0xFFFFFFFF


def _cstr(raw):
    return bytes(raw).split(b"\0", 1)[0].decode("utf-8", "replace")


class WorldSnapshot:
    """一份读出来的世界状态，附上判定它自洽的依据。"""

    def __init__(self, header, raw_world):
        self.header = header
        self.world = _World.from_buffer_copy(raw_world)
        self.raw = raw_world
        self.checksum_ok = fnv1a_words(
            raw_world[:KOS_SHM_WORLD_CHECKSUM_OFFSET]) == self.world.checksum
        self.seq_ok = (header.world_seq % 2 == 0
                       and self.world.write_seq == header.world_seq)

    def chef_ids(self):
        return [self.world.chefs[i].id for i in range(self.world.chef_count)]

    def order_ids(self):
        return [_cstr(self.world.orders[i].id) for i in range(self.world.order_count)]

    def order_ingredient_counts(self):
        return [self.world.orders[i].ingredient_count
                for i in range(self.world.order_count)]

    def resource_names(self):
        return [_cstr(self.world.resources[i].name)
                for i in range(self.world.resource_count)]

    def resource(self, name):
        for i in range(self.world.resource_count):
            item = self.world.resources[i]
            if _cstr(item.name) == name:
                return item
        return None

    def describe(self):
        return ("chefs=%d orders=%d resources=%d score=%d game_time=%d "
                "checksum_ok=%s seqlock_ok=%s"
                % (self.world.chef_count, self.world.order_count,
                   self.world.resource_count, self.world.score, self.world.game_time,
                   self.checksum_ok, self.seq_ok))


def read_world(path):
    """读一份自洽的世界状态；布局对不上或快照被撕裂就抛 InfraError。

    两种失败都要分开：结构对不上是本工具的错（shm.h 改了），撕裂是写者正在写的
    正常瞬间，重试几次即可 —— 混在一起会把工具的错误说成 Bridge 的错。
    """
    size = ctypes.sizeof(_SegmentHeader)
    try:
        raw = Path(path).read_bytes()
    except OSError as exc:
        raise InfraError("cannot read %s: %s" % (path, exc))
    if len(raw) < size + ctypes.sizeof(_World):
        raise InfraError("%s is %d bytes, too small for a segment" % (path, len(raw)))
    header = _SegmentHeader.from_buffer_copy(raw[:size])
    if header.magic != KOS_SHM_MAGIC:
        raise InfraError("%s: magic 0x%08x is not KOS_SHM_MAGIC" % (path, header.magic))
    # 世界状态不是紧跟在段头字段后面：kos_shm_segment 把 world 和 slots 都算在
    # header_size 里（shm.h），所以前缀长度要从三个尺寸字段倒推，再和本镜像对账。
    prefix = header.header_size - header.world_size - header.slot_size * header.slot_count
    if (prefix != size or header.world_size != ctypes.sizeof(_World)
            or header.max_chefs != 8 or header.max_orders != 16
            or header.max_resources != 32 or header.max_order_ingredients != 6):
        raise InfraError(
            "%s: segment layout header_size=%d world_size=%d slot_size=%d slot_count=%d "
            "chefs=%d orders=%d resources=%d ingredients=%d does not match this reader "
            "(prefix=%d vs %d, world=%d vs %d, 8/16/32/6) — update the ctypes mirror"
            % (path, header.header_size, header.world_size, header.slot_size,
               header.slot_count, header.max_chefs, header.max_orders,
               header.max_resources, header.max_order_ingredients, prefix, size,
               header.world_size, ctypes.sizeof(_World)))
    return header, raw[prefix:prefix + header.world_size]


def read_world_stable(path, timeout=3.0):
    """等一份 seqlock+checksum 都自洽的快照（写者持奇序号时重读）。"""
    deadline = time.monotonic() + timeout
    last = None
    while True:
        header, raw = read_world(path)
        snap = WorldSnapshot(header, raw)
        if snap.checksum_ok and snap.seq_ok:
            return snap
        last = snap
        if time.monotonic() >= deadline:
            raise InfraError("no self-consistent snapshot of %s within %.1fs "
                             "(last: %s)" % (path, timeout, last.describe()))
        time.sleep(0.02)


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
        self.supervisor = None
        self.runtime_dir = None
        self.keep_runtime = False
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
                     heartbeat_timeout_ms, input_grace_ms=None, verbose=False):
        binary = Path(self.args.bridge)
        if not binary.exists():
            raise InfraError("bridge binary not found: %s (build it with: cd bridge && make)"
                             % binary)
        argv = ["--link", link, "--host", host, "--port", str(port),
                "--chefs", str(chefs),
                "--state-name", self.state_name, "--sem-name", self.sem_name,
                "--log", str(self.log_path),
                "--frame-timeout-ms", str(frame_timeout_ms),
                "--heartbeat-timeout-ms", str(heartbeat_timeout_ms)]
        if input_grace_ms is not None:
            argv += ["--input-grace-ms", str(input_grace_ms)]
        # state_update 是 DEBUG 级：要按帧核对"解析出几个、丢了几个"的场景必须开它，
        # 否则默认 INFO 下那行根本不落盘（state_summary 每 60 帧才有，且字段不全）。
        if verbose:
            argv.append("--verbose")
        self.bridge = BridgeProc(binary, argv, self.console_path)
        self.bridge.start()
        _say("    info       bridge pid=%d: %s" % (self.bridge.pid, " ".join(argv)))
        return self.bridge

    def start_bridge_and_wait(self, *, link, host, chefs, frame_timeout_ms,
                              heartbeat_timeout_ms, input_grace_ms=None, port=None,
                              retries=3, verbose=False):
        """起 Bridge 并等它就绪，返回实际用的端口。

        端口默认由内核分配：多 agent 并行开发时 14455 很可能被别的 Bridge 或
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
                              heartbeat_timeout_ms=heartbeat_timeout_ms,
                              input_grace_ms=input_grace_ms,
                              verbose=verbose)
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

    def start_supervisor(self, *, port, chefs, lease_ttl_ms=None, kitchen=None,
                         recipes=None, orders=None, backoff_ms=None):
        kitchen = kitchen or self.args.kitchen
        recipes = recipes or self.args.recipes
        orders = orders or self.args.orders
        binary = Path(self.args.supervisor or
                      Path(self.args.bridge).with_name("kitchenos-supervisor"))
        if not binary.exists():
            raise InfraError("supervisor binary not found: %s (build it with: cd bridge && make)"
                             % binary)
        self.runtime_dir = Path(tempfile.mkdtemp(prefix="kitchenos-osloop-"))
        argv = ["--foreground",
                "--runtime-dir", str(self.runtime_dir),
                "--state-name", self.state_name, "--sem-name", self.sem_name,
                "--bridge-port", str(port), "--workers", str(chefs),
                "--strategy", "fcfs",
                "--health-interval-ms", "100",
                "--supervisor-heartbeat-ms", "100",
                "--heartbeat-timeout-ms", "2000",
                "--stop-timeout-ms", "2000",
                "--kitchen", str(kitchen),
                "--recipes", str(recipes),
                "--orders", str(orders)]
        if lease_ttl_ms is not None:
            argv += ["--lease-ttl-ms", str(lease_ttl_ms)]
        if backoff_ms is not None:
            argv += ["--backoff-ms", str(backoff_ms)]
        argv.append("--verbose")
        console = self.runtime_dir / "supervisor.console"
        self.supervisor = SupervisorProc(binary, argv, console)
        self.supervisor.start()
        _say("    info       supervisor pid=%d runtime=%s" %
             (self.supervisor.pid, self.runtime_dir))
        return self.supervisor

    def wait_supervisor_ready(self, port, chefs, timeout):
        expected = {"bridge", "scheduler", "navd"} | {
            "worker-%d" % i for i in range(chefs)}
        deadline = time.monotonic() + timeout
        status_path = self.runtime_dir / "status.json"
        while time.monotonic() < deadline:
            if self.supervisor is not None and not self.supervisor.alive():
                return False
            try:
                status = json.loads(status_path.read_text())
                running = {c.get("role") for c in status.get("children", [])
                           if c.get("state") == "running" and (c.get("pid") or 0) > 0}
            except (OSError, ValueError):
                running = set()
            if expected <= running:
                try:
                    sock = socket.create_connection((self.args.host, port), timeout=0.2)
                    sock.close()
                    return True
                except OSError:
                    pass
            time.sleep(0.05)
        return False

    def supervisor_status(self):
        if self.runtime_dir is None:
            return {}
        try:
            return json.loads((self.runtime_dir / "status.json").read_text())
        except (OSError, ValueError):
            return {}

    def stop_supervisor(self):
        if self.supervisor is None:
            return None, None
        ok, seconds, code = self.supervisor.terminate(5.0)
        _say("    info       supervisor SIGTERM: ok=%s %.2fs exit=%s" %
             (ok, seconds or 0.0, code))
        self.supervisor = None
        return code, seconds

    def run_dir(self):
        if self.runtime_dir is None:
            return None
        runs = sorted((self.runtime_dir / "runs").glob("*"))
        return runs[-1] if runs else None

    def start_stub(self, *, slot, chefs, hz, duration, kind="move", waypoints=None):
        binary = Path(self.args.worker_stub)
        argv = ["--state-name", self.state_name, "--sem-name", self.sem_name,
                "--chefs", str(chefs), "--slot", str(slot),
                "--hz", str(hz), "--duration", str(duration), "--kind", kind]
        if waypoints is not None:
            argv.extend(["--waypoints", str(waypoints)])
        console = Path(str(self.log_path) + ".slot%d.console" % slot)
        stub = WorkerStub(binary, argv, console)
        stub.start()
        self.stubs.append(stub)
        _say("    info       worker-stub slot=%d pid=%d kind=%s"
             % (slot, stub.proc.pid, kind))
        return stub

    def stop_all(self):
        if self.supervisor is not None:
            self.stop_supervisor()
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
        if self.runtime_dir is not None and not self.keep_runtime:
            shutil.rmtree(self.runtime_dir, ignore_errors=True)

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
        if self.supervisor is not None:
            parts.append("supervisor pid=%s alive=%s exit=%s" %
                         (self.supervisor.pid, self.supervisor.alive(),
                          self.supervisor.exit_code()))
            text = self.supervisor.console_text()
            if text:
                parts.append("--- supervisor console (tail) ---\n" + text)
        if self.runtime_dir is not None:
            for name in ("supervisor", "scheduler", "worker-0", "worker-1", "bridge"):
                path = self.runtime_dir / "logs" / (name + ".log")
                text = _tail(path, 25)
                if text:
                    parts.append("--- %s.log (tail) ---\n%s" % (name, text))
            run_dir = self.run_dir()
            if run_dir is not None:
                text = _tail(run_dir / "decisions.jsonl", 10)
                if text:
                    parts.append("--- decisions.jsonl (tail) ---\n" + text)
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
                      "— build with: cd bridge && make" % stub_binary)

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


def scenario_pickup_dash(ctx, checks):
    """pickup/dash：槽里写的这两个动作必须原样到对端，不能被降级成 idle。

    这两个 action 是 2026-10-15 按用户的选择补上线的（"补 builder，扩到 5 个动作"）。
    在那之前 build_action_from_slot 落到 default 分支，槽里写 pickup/dash 的 Worker
    拿到的是 stop —— 没有任何断言能发现，因为对端只看得到 stop。
    """
    args = ctx.args
    chefs = 2
    stub_binary = Path(args.worker_stub)
    if not stub_binary.exists():
        raise Skipped("worker-stub not built (%s); this scenario needs a live slot writer "
                      "— build with: cd bridge && make" % stub_binary)

    ctx.clear_stale_shm()
    port = ctx.start_bridge_and_wait(link="listen", host=args.host, chefs=chefs,
                                     frame_timeout_ms=args.frame_timeout_ms,
                                     heartbeat_timeout_ms=args.heartbeat_timeout_ms,
                                     port=args.listen_port)

    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    priming = Stats("priming")
    run_exchange(link, priming, 1.0, chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=not args.no_ack,
                 flat_xy=args.flat_xy)
    checks.hard("bridge_writes_state_shm", ctx.wait_shm(3.0), "%s" % ctx.shm_paths()[0])

    checks.note("worker_stubs", "slot 0 -> pickup, slot 1 -> dash (both live)")
    ctx.start_stub(slot=0, chefs=chefs, hz=args.stub_hz, duration=args.duration + 6,
                   kind="pickup")
    ctx.start_stub(slot=1, chefs=chefs, hz=args.stub_hz, duration=args.duration + 6,
                   kind="dash")

    warm = Stats("warmup")
    run_exchange(link, warm, 1.5, chefs=chefs, hz=args.hz, heartbeat_ms=args.heartbeat_ms,
                 ack_actions=not args.no_ack, flat_xy=args.flat_xy)
    checks.note("warmup", warm.describe())

    window = max(1.5, args.duration)
    stats = Stats("measure")
    run_exchange(link, stats, window, chefs=chefs, hz=args.hz, heartbeat_ms=args.heartbeat_ms,
                 ack_actions=not args.no_ack, flat_xy=args.flat_xy)
    link.close()

    # stub 以 stub_hz 往槽里写、Bridge 以 hz 开帧，所以有些帧落在两次发布之间。那种帧
    # 现在沿用槽里最后一版意图（--input-grace-ms 之内），不再降级成 idle；本场景的判据
    # 仍然是"这两个动作真的到了"，idle 的时机由 input-grace 场景单独验。
    seen0 = [a.get("action") for a in stats.actions if a.get("chef_id") == 0]
    seen1 = [a.get("action") for a in stats.actions if a.get("chef_id") == 1]
    got0 = seen0.count("pickup")
    got1 = seen1.count("dash")
    checks.note("measured_window", "%s; chef0=%s chef1=%s"
                % (stats.describe(), Counter(seen0) or "none", Counter(seen1) or "none"))
    checks.hard("pickup_reaches_the_peer", got0 >= 3,
                "chef 0 (slot writes pickup): %d pickup of %d actions (%s)"
                % (got0, len(seen0), Counter(seen0) or "none"))
    checks.hard("dash_reaches_the_peer", got1 >= 3,
                "chef 1 (slot writes dash): %d dash of %d actions (%s)"
                % (got1, len(seen1), Counter(seen1) or "none"))
    checks.hard("no_unknown_action_names", not stats.bad_actions,
                "%d actions the peer rejected: %s"
                % (len(stats.bad_actions), stats.bad_actions[:2]))
    checks.hard("bridge_alive_after_new_actions", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    return ("%s in %.1fs; chef 0 -> %s; chef 1 -> %s"
            % (stats.describe(), window, Counter(seen0) or "none", Counter(seen1) or "none"))


def scenario_path_face_work(ctx, checks):
    """path/face/work：槽里的新动作必须按契约到插件，而不是被降级成 idle。

    2026-10-25（kitchen-control 任务 02）加。判据同时看三个槽：
    path 的 seq 与路点数（1 与 8）、face/work 的 target_object；再加桥自己的
    actions_sent 计数与"没有 slot-action-rejected"两条日志断言，免得只看线格式
    时把"其实每个动作都被降级、只是凑巧发过 stop"当成通过。
    """
    args = ctx.args
    chefs = 4
    stub_binary = Path(args.worker_stub)
    if not stub_binary.exists():
        raise Skipped("worker-stub not built (%s); this scenario needs a live slot writer "
                      "— build with: cd bridge && make" % stub_binary)

    ctx.clear_stale_shm()
    port = ctx.start_bridge_and_wait(link="listen", host=args.host, chefs=chefs,
                                     frame_timeout_ms=args.frame_timeout_ms,
                                     heartbeat_timeout_ms=args.heartbeat_timeout_ms,
                                     port=args.listen_port)
    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    priming = Stats("priming")
    run_exchange(link, priming, 1.0, chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=not args.no_ack,
                 flat_xy=args.flat_xy)
    checks.hard("bridge_writes_state_shm", ctx.wait_shm(3.0), "%s" % ctx.shm_paths()[0])

    checks.note("worker_stubs",
                "slot 0 -> path(1), slot 1 -> path(8), slot 2 -> face, slot 3 -> work")
    ctx.start_stub(slot=0, chefs=chefs, hz=args.stub_hz, duration=args.duration + 6,
                   kind="path", waypoints=1)
    ctx.start_stub(slot=1, chefs=chefs, hz=args.stub_hz, duration=args.duration + 6,
                   kind="path", waypoints=8)
    ctx.start_stub(slot=2, chefs=chefs, hz=args.stub_hz, duration=args.duration + 6,
                   kind="face")
    ctx.start_stub(slot=3, chefs=chefs, hz=args.stub_hz, duration=args.duration + 6,
                   kind="work")

    warm = Stats("warmup")
    run_exchange(link, warm, 1.5, chefs=chefs, hz=args.hz, heartbeat_ms=args.heartbeat_ms,
                 ack_actions=not args.no_ack, flat_xy=args.flat_xy)
    checks.note("warmup", warm.describe())

    window = max(1.5, args.duration)
    stats = Stats("measure")
    run_exchange(link, stats, window, chefs=chefs, hz=args.hz, heartbeat_ms=args.heartbeat_ms,
                 ack_actions=not args.no_ack, flat_xy=args.flat_xy)
    link.close()

    by_chef = {slot: [a for a in stats.actions if a.get("chef_id") == slot]
               for slot in range(chefs)}
    checks.note("measured_window", "%s; %s" % (
        stats.describe(),
        "; ".join("chef%d=%s" % (slot, Counter(a.get("action") for a in by_chef[slot]) or "none")
                  for slot in range(chefs))))

    paths1 = [a for a in by_chef[0] if a.get("action") == "path"]
    paths8 = [a for a in by_chef[1] if a.get("action") == "path"]
    faces = [a for a in by_chef[2] if a.get("action") == "face"]
    works = [a for a in by_chef[3] if a.get("action") == "work"]

    checks.hard("one_waypoint_path_reaches_the_peer", len(paths1) >= 3,
                "chef 0: %d path of %d actions (%s)"
                % (len(paths1), len(by_chef[0]),
                   Counter(a.get("action") for a in by_chef[0]) or "none"))
    checks.hard("eight_waypoint_path_reaches_the_peer", len(paths8) >= 3,
                "chef 1: %d path of %d actions (%s)"
                % (len(paths8), len(by_chef[1]),
                   Counter(a.get("action") for a in by_chef[1]) or "none"))
    if paths1:
        checks.hard("path_carries_seq", all(a.get("seq") == 1000 for a in paths1),
                    "seqs=%s" % sorted({a.get("seq") for a in paths1}))
        checks.hard("one_waypoint_count", all(len(a.get("waypoints") or []) == 1
                                              for a in paths1),
                    "counts=%s" % sorted({len(a.get("waypoints") or []) for a in paths1}))
    if paths8:
        checks.hard("eight_waypoints_count", all(len(a.get("waypoints") or []) == 8
                                                 for a in paths8),
                    "counts=%s" % sorted({len(a.get("waypoints") or []) for a in paths8}))
        checks.hard("path_waypoints_are_numbers",
                    all(isinstance(w.get("x"), (int, float)) and
                        isinstance(w.get("y"), (int, float))
                        for a in paths8 for w in (a.get("waypoints") or [])),
                    "first=%s" % ((paths8[0].get("waypoints") or [None])[0],))
    checks.hard("face_reaches_the_peer", len(faces) >= 3,
                "chef 2: %d face of %d actions (%s)"
                % (len(faces), len(by_chef[2]),
                   Counter(a.get("action") for a in by_chef[2]) or "none"))
    if faces:
        checks.hard("face_carries_target_object",
                    all(a.get("target_object") == "cutting_board_2" for a in faces)
                    and all(a.get("target") is None for a in faces),
                    "objects=%s targets=%s"
                    % (sorted({a.get("target_object") for a in faces}),
                       [a.get("target") for a in faces[:2]]))
    checks.hard("work_reaches_the_peer", len(works) >= 3,
                "chef 3: %d work of %d actions (%s)"
                % (len(works), len(by_chef[3]),
                   Counter(a.get("action") for a in by_chef[3]) or "none"))
    if works:
        checks.hard("work_carries_target_object",
                    all(a.get("target_object") == "cutting_board_3" for a in works),
                    "objects=%s" % sorted({a.get("target_object") for a in works}))

    log = ctx.log_text()
    sent_lines = [line for line in log.splitlines() if "actions_sent" in line]
    checks.hard("bridge_counted_new_actions",
                bool(re.search(r"actions_sent .*path=[1-9]\d*", log))
                and bool(re.search(r"actions_sent .*face=[1-9]\d*", log))
                and bool(re.search(r"actions_sent .*work=[1-9]\d*", log)),
                "actions_sent lines: %s" % (sent_lines[-1:] or "none"))
    checks.hard("no_new_action_rejected",
                "slot-action-rejected" not in log,
                "rejected: %s" % ([line for line in log.splitlines()
                                   if "slot-action-rejected" in line][:3] or "none"))
    checks.hard("no_unknown_action_names", not stats.bad_actions,
                "%d actions the peer rejected: %s"
                % (len(stats.bad_actions), stats.bad_actions[:2]))
    checks.hard("bridge_alive_after_new_actions", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    return ("%s in %.1fs; path1/path8/face/work reached %d/%d/%d/%d times"
            % (stats.describe(), window, len(paths1), len(paths8), len(faces), len(works)))


def scenario_input_grace(ctx, checks):
    """输入宽限窗口：Worker 停写后，桥先在窗口内沿用最后一版意图，窗口过了才发 idle。

    2026-10-16 加（任务 04 真机调试时发现）：桥原来把"这一格这帧没交新版本"直接当成
    "这个 Worker 缺席"，于是任何比游戏帧率慢的 Worker 都会有一批帧被冻住 —— 实测
    120Hz 的替身对 117fps 的游戏，对端收到的 action 里 85% 是合成的 stop，
    持续动作（move）走两步停一步。现在的语义是：槽超过 --input-grace-ms（默认 250ms）
    没有新版本才当缺席，Worker 崩了之后照样会降级成 idle。
    """
    args = ctx.args
    chefs = 2
    stub_binary = Path(args.worker_stub)
    if not stub_binary.exists():
        raise Skipped("worker-stub not built (%s); this scenario needs a live slot writer "
                      "— build with: cd bridge && make" % stub_binary)

    grace_ms = 400
    ctx.clear_stale_shm()
    port = ctx.start_bridge_and_wait(link="listen", host=args.host, chefs=chefs,
                                     frame_timeout_ms=args.frame_timeout_ms,
                                     input_grace_ms=grace_ms,
                                     heartbeat_timeout_ms=args.heartbeat_timeout_ms,
                                     port=args.listen_port)

    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    priming = Stats("priming")
    run_exchange(link, priming, 1.0, chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=not args.no_ack,
                 flat_xy=args.flat_xy)
    checks.hard("bridge_writes_state_shm", ctx.wait_shm(3.0), "%s" % ctx.shm_paths()[0])

    checks.note("grace_ms", "%d (Bridge --input-grace-ms)" % grace_ms)
    stub = ctx.start_stub(slot=0, chefs=chefs, hz=args.stub_hz, duration=3.0, kind="move")

    live = Stats("live")
    run_exchange(link, live, 2.0, chefs=chefs, hz=args.hz, heartbeat_ms=args.heartbeat_ms,
                 ack_actions=not args.no_ack, flat_xy=args.flat_xy)

    # 停写：窗口内应该还在沿用最后一版 move，窗口过了才是 stop。
    stub.terminate()
    inside = Stats("inside_grace")
    run_exchange(link, inside, grace_ms / 1000.0 * 0.6, chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=not args.no_ack,
                 flat_xy=args.flat_xy)
    after = Stats("after_grace")
    run_exchange(link, after, grace_ms / 1000.0 * 2.0 + 0.5, chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=not args.no_ack,
                 flat_xy=args.flat_xy)
    link.close()

    live0 = [a.get("action") for a in live.actions if a.get("chef_id") == 0]
    inside0 = [a.get("action") for a in inside.actions if a.get("chef_id") == 0]
    after0 = [a.get("action") for a in after.actions if a.get("chef_id") == 0]
    checks.note("windows", "live=%s inside=%s after=%s"
                % (Counter(live0) or "none", Counter(inside0) or "none",
                   Counter(after0) or "none"))
    checks.hard("live_worker_input_forwarded", live0.count("move") >= 3,
                "chef 0 got %d move of %d actions while the stub was writing (%s)"
                % (live0.count("move"), len(live0), Counter(live0) or "none"))
    checks.hard("last_intent_kept_inside_grace", inside0.count("move") >= 1,
                "chef 0 got %s in the %.0fms after the stub stopped (want the last move kept)"
                % (Counter(inside0) or "none", grace_ms * 0.6))
    tail = after0[-5:]
    checks.hard("idle_after_grace", "stop" in after0 and "move" not in tail,
                "chef 0 got %s after the grace window (want stop; last five: %s)"
                % (Counter(after0) or "none", tail))
    checks.hard("bridge_alive_after_grace", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    return ("grace=%dms; live=%s; inside=%s; after=%s"
            % (grace_ms, Counter(live0) or "none", Counter(inside0) or "none",
               Counter(after0) or "none"))


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

    # B) 连换行符都没有的 >1 MiB 流：这一行永远不会结束，Bridge 必须断开重连并记录
    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    oversize = MAX_FRAME_BYTES + 65536
    link.send_raw(b"x" * oversize)
    time.sleep(0.6)
    checks.hard("bridge_alive_after_oversize_line", ctx.bridge.alive(),
                "sent %d bytes (>%d) with no '\\n'; exit=%s"
                % (oversize, MAX_FRAME_BYTES, ctx.bridge.exit_code()))
    # specs/bridge.md:56 —— 行超长 = 流已失步（没有换行符就没有重新同步的位置），
    # 必须断开重连并记录。
    # 探活是必须的：对端发来的 FIN 不 recv 就看不见，光看标志位会误判成"还连着"。
    dropped = link.probe_closed(0.6)
    oversize_dropped = dropped
    checks.hard("oversize_line_drops_link", dropped,
                "%d bytes with no terminator exceed the %d cap, so the stream is desynced "
                "and the bridge must drop the link (specs/bridge.md:56); link=%s"
                % (oversize, MAX_FRAME_BYTES, link.error or "still open"))
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

    # C2) 空行：只有一个换行符，分帧本身没有失步（换行符就是边界），
    #     参考实现 TcpLink.ExtractFrames 会照常切出"正文为空的一行"，正文解析失败
    #     按 specs/bridge.md:55 属于"丢弃该消息并记录，不断连接"。所以：进程存活是
    #     硬要求，断不断连按 soft 记录（这条是当前实现对不上 spec 的地方之一）。
    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    link.send_raw(encode_frame(b""))
    time.sleep(0.4)
    checks.hard("bridge_alive_after_zero_length_line", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    dropped = link.probe_closed(0.6)
    zero_dropped = dropped
    checks.soft("zero_length_line_keeps_connection", not dropped,
                "bridge %s；空行的载荷是空的，分帧没有失步，"
                "参考实现按『正文为空的一行』走完并丢弃（specs/bridge.md:55 不断连接）；"
                "link=%s"
                % ("kept the connection" if not dropped else "dropped the connection",
                   link.error or "open"))
    link.close()

    # D) 截断的行：64 字节之后没有换行符就断开，Bridge 只能当成半条消息
    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    link.send_raw(b"{" * 64)
    time.sleep(0.4)
    link.close()
    time.sleep(0.4)
    checks.hard("bridge_alive_after_truncated_line", ctx.bridge.alive(),
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


# --------------------------------------------------------------------------
# 边界场景：specs/bridge.md:104-105 点名、原套件没覆盖的那几个
# --------------------------------------------------------------------------


def _shape_state_update(chefs=None, orders=None, resources=None, frame_index=0):
    """按 §5.2 拼一帧，但三个集合都由调用方给：数据形状边界要能单独摆出来。"""
    if chefs is None:
        chefs = [{"id": 0, "position": {"x": 1.0, "y": 2.0}, "facing": 0.0,
                  "holding": None, "state": "idle"}]
    if orders is None:
        orders = [{"id": "order_001", "recipe": "salad", "time_remaining": 30,
                   "ingredients": ["lettuce"]}]
    if resources is None:
        resources = {"pot_0": {"state": "free"}, "plate_stack": {"count": 3}}
    return jdump({"type": "state_update", "timestamp": int(time.time()),
                  "game_time": frame_index // 60, "game_time_ms": int((frame_index / 60.0) * 1000), "score": 100,
                  "chefs": chefs, "orders": orders, "resources": resources})


def _served_window(ctx, link, chefs, seconds):
    """在会话里正常跑一小段，返回收到的动作；用来证明"还照常服务"。"""
    stats = Stats("served")
    run_exchange(link, stats, seconds, chefs=chefs, hz=ctx.args.hz,
                 heartbeat_ms=ctx.args.heartbeat_ms, ack_actions=not ctx.args.no_ack)
    return stats


def pump_with_heartbeats(link, stats, seconds, *, chefs, ack_actions=True, hb_period=0.3):
    """边收边发心跳地等一段时间。

    必须喂心跳：Bridge 的 `--heartbeat-timeout-ms` 一到就会（正确地）判插件失联、
    暂停下发并断链（spec W4）。等待窗口里不喂心跳，测到的就不是想问的那件事。
    """
    end = time.monotonic() + seconds
    next_hb = 0.0
    while True:
        now = time.monotonic()
        if now >= end:
            return
        for msg in link.pump(min(0.05, max(0.0, end - now))):
            _absorb(msg, stats, link, ack_actions, chefs)
        now = time.monotonic()
        if now >= next_hb:
            _safe_send(link, stats, msg_heartbeat(), "heartbeat")
            next_hb = now + hb_period


def scenario_silent_connect(ctx, checks):
    """边界：连上、一个字节都不发就断开，反复多次（specs/bridge.md:104）。

    两条要求：每次会话都要作为一次链路事件被记下来（"全程写事件"里含 IPC 连接
    与断开），并且这些空会话不能留下 fd、不能挡住下一个对端。
    """
    args = ctx.args
    chefs = 1
    ctx.clear_stale_shm()
    port = ctx.start_bridge_and_wait(link="listen", host=args.host, chefs=chefs,
                                     frame_timeout_ms=args.frame_timeout_ms,
                                     heartbeat_timeout_ms=args.heartbeat_timeout_ms,
                                     port=args.listen_port)
    pid = ctx.bridge.pid

    # 基线相位与收尾相位必须可比：两次都是"对端连着、正在跑帧"。
    link = live_link(args.host, port, args.ready_timeout)
    base = _served_window(ctx, link, chefs, 1.0)
    checks.hard("baseline_dispatch", len(base.actions) >= 1, base.describe())
    fds_before = proc_fds(pid)
    rss_before = proc_rss_kb(pid)
    checks.note("fds_before", "%s — %s" % (describe_fds(fds_before),
                                           ", ".join("%d=%s" % (fd, tgt) for fd, tgt
                                                     in sorted(fds_before.items()))))
    # 计数器必须在断开之前取：close() 之后 link_down 可能立刻 +1，
    # 拿它当基线就永远等不到"再加一次"
    events = log_event_counts(ctx)
    up0, down0 = events["link_up"], events["link_down"]
    link.close()
    checks.hard("baseline_session_was_logged", wait_log_events(ctx, "link_down", down0 + 1, 3.0),
                "the baseline session's disconnect must show up as link_down (baseline: "
                "link_up=%d link_down=%d)" % (up0, down0))

    cycles = max(3, args.silent_cycles)
    seen_up = seen_down = 0
    for i in range(1, cycles + 1):
        # 一个字节都不发：只有 connect 和 close
        sock = connect(args.host, port, args.ready_timeout)
        up_ok = wait_log_events(ctx, "link_up", up0 + i, 2.0)
        sock.close()
        down_ok = wait_log_events(ctx, "link_down", down0 + i, 2.0)
        seen_up += 1 if up_ok else 0
        seen_down += 1 if down_ok else 0
        if not (up_ok and down_ok):
            checks.note("cycle_%d_not_fully_logged" % i,
                        "link_up=%s link_down=%s (up so far %d/%d)"
                        % (up_ok, down_ok, seen_up, i))
            break
    checks.hard("every_silent_session_logged_as_up_and_down",
                seen_up == cycles and seen_down == cycles,
                "%d/%d connect-close cycles each produced exactly one link_up and one "
                "link_down" % (min(seen_up, seen_down), cycles))

    # 收尾：正常会话必须还是不折不扣地工作
    link = live_link(args.host, port, args.ready_timeout)
    after = _served_window(ctx, link, chefs, 1.5)
    checks.hard("dispatch_after_silent_connects", len(after.actions) >= 5, after.describe())
    checks.hard("actions_wellformed_after_silent_connects",
                not after.bad_actions and not base.bad_actions,
                "malformed: %s" % (after.bad_actions[:2] or base.bad_actions[:2] or "none"))
    # "还连着"必须真的探：对端的 FIN 不 recv 是看不见的，心跳还可能排在它前面
    still_open = not link.probe_closed(0.4)
    checks.hard("final_connection_really_open", still_open,
                "0.4s 内反复 recv 都没撞到 EOF 才算开着；link=%s" % (link.error or "open"))

    fds_after = proc_fds(pid)
    checks.note("fds_after", "%s — %s" % (describe_fds(fds_after),
                                          ", ".join("%d=%s" % (fd, tgt) for fd, tgt
                                                    in sorted(fds_after.items()))))
    checks.hard("no_fd_leak_after_silent_connects", len(fds_after) == len(fds_before),
                "%d cycles: before=%d fds, after=%d fds%s"
                % (cycles, len(fds_before), len(fds_after), " — " + fd_delta(fds_before, fds_after)
                   if fd_delta(fds_before, fds_after) else ""))
    checks.hard("no_socket_leak_after_silent_connects",
                len(socket_fds(fds_after)) == len(socket_fds(fds_before)),
                "sockets before=%d after=%d (%s)"
                % (len(socket_fds(fds_before)), len(socket_fds(fds_after)),
                   fd_delta(fds_before, fds_after) or "no new fd"))
    rss_after = proc_rss_kb(pid)
    checks.note("rss_kb", "before=%s after=%s (VmRSS；fd 之外的泄漏也看一眼)"
                % (rss_before, rss_after))
    checks.soft("rss_not_grown_after_silent_connects",
                rss_before is None or rss_after is None or rss_after - rss_before <= 512,
                "VmRSS %s -> %s kB over %d cycles" % (rss_before, rss_after, cycles))
    checks.hard("bridge_alive_after_silent_connects", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    checks.hard("bridge_pid_unchanged", ctx.bridge.pid == pid,
                "pid %s -> %s (no restart expected)" % (pid, ctx.bridge.pid))
    link.close()
    return ("%d silent connect-close cycles, every one logged link_up+link_down; "
            "fds %d -> %d (%d sockets -> %d), VmRSS %s -> %s kB; then %s"
            % (cycles, len(fds_before), len(fds_after), len(socket_fds(fds_before)),
               len(socket_fds(fds_after)), rss_before, rss_after, after.describe()))


def scenario_reconnect_storm(ctx, checks):
    """边界：连续快速断开重连（关卡重载 / alt-tab 的真实形态，bridge.md:105）。

    四种会话形态轮着来：什么都不发就 FIN、发几帧再 FIN、发半行后 RST、发一行后
    RST。RST 是解码器复位唯一验得到的路径：半行的字节若留在解码器里，下一条流的
    第一行就会被读成上一行的尾巴，症状是 parse_error / framing desync 而不是干净报错。

    硬结论只有两条：进程还活着、还在下发、最后的连接真的还开着；以及
    /proc/<pid>/fd 里 socket 数与整表都不增长。
    """
    args = ctx.args
    chefs = 1
    ctx.clear_stale_shm()
    port = ctx.start_bridge_and_wait(link="listen", host=args.host, chefs=chefs,
                                     frame_timeout_ms=args.frame_timeout_ms,
                                     heartbeat_timeout_ms=args.heartbeat_timeout_ms,
                                     port=args.listen_port)
    pid = ctx.bridge.pid

    link = live_link(args.host, port, args.ready_timeout)
    base = _served_window(ctx, link, chefs, 1.0)
    checks.hard("baseline_dispatch", len(base.actions) >= 1, base.describe())
    fds_before = proc_fds(pid)
    rss_before = proc_rss_kb(pid)
    checks.note("fds_before", "%s — %s" % (describe_fds(fds_before),
                                           ", ".join("%d=%s" % (fd, tgt) for fd, tgt
                                                     in sorted(fds_before.items()))))
    link.close()
    checks.hard("baseline_link_down_logged", wait_log_events(ctx, "link_down", 1, 3.0),
                "the baseline session's disconnect must show up as link_down")
    # 基线会话自己那次 EOF 断开也会落在下面的统计里，先记下来，风暴后做差
    base_fin_downs = len(re.findall(r"link_down .*reason=peer closed the connection",
                                    ctx.log_text()))

    cycles = max(20, args.storm_cycles)
    gap = max(0.0, args.storm_gap_ms / 1000.0)
    forms = Counter()
    for i in range(cycles):
        form = i % 4
        link = Link(connect(args.host, port, args.ready_timeout))
        if form == 0:
            forms["silent+FIN"] += 1
        elif form == 1:
            forms["hello+2frames+FIN"] += 1
            link.send_json(msg_hello())
            link.send_json(msg_state_update(chefs, i))
            link.send_json(msg_state_update(chefs, i + 1))
        elif form == 2:
            forms["hello+half-line+RST"] += 1
            link.send_json(msg_hello())
            link.send_raw(b"{" * 64)
        else:
            forms["hello+frame+RST"] += 1
            link.send_json(msg_hello())
            link.send_json(msg_state_update(chefs, i))
        # 干净关闭的那两种先让 Bridge 把帧读走；RST 的那两种立刻断，
        # 让"字节还在路上就被 RST 丢掉"这件事真的发生
        if form in (0, 1):
            if gap:
                time.sleep(gap)
            link.close()
        else:
            link.close_rst()
        if gap:
            time.sleep(gap)

    # 每一轮都是"上去又下来"：全部断完之后 Bridge 的链路事件必须配平（没有半截状态）
    drained = False
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        events = log_event_counts(ctx)
        if events["link_up"] > 0 and events["link_down"] >= events["link_up"]:
            drained = True
            break
        time.sleep(0.02)
    events = log_event_counts(ctx)
    checks.hard("storm_edges_balanced", drained,
                "after %d cycles the bridge should be back DOWN: link_up=%d link_down=%d"
                % (cycles, events["link_up"], events["link_down"]))
    checks.hard("storm_logged_link_up_and_down",
                events["link_up"] >= 1 and events["link_down"] >= 1,
                "link_up=%d link_down=%d for %d cycles (%s); link_up < cycles 是正常的，"
                "见 rst_cycles_may_vanish" % (events["link_up"], events["link_down"],
                                              cycles, dict(forms)))
    checks.soft("storm_observed_more_than_one_session", events["link_up"] >= 2,
                "link_up=%d: one continuous UP through all %d cycles would mean the "
                "bridge never noticed a single peer leaving"
                % (events["link_up"], cycles))

    # 按断开原因给每类周期对账（link_down 行带 reason）。FIN 收尾的连接断开后仍留在
    # accept 队列里，内核不会把它摘掉，accept 一定能拿到它（第一次 read 就是 EOF），
    # 所以这两类周期必须每次都留下痕迹：要么 link_down(EOF)，要么被下一条连接顶掉
    # 而记 link_peer_replaced。RST 收尾的可以整个消失——还没被 accept 就被 reset 的
    # 连接会被内核从队列里摘掉，进程从头到尾没见过它，那不是漏记。
    graceful_total = sum(forms[f] for f in ("silent+FIN", "hello+2frames+FIN"))
    fin_down = replaced = 0
    deadline = time.monotonic() + 2.0
    while True:
        text = ctx.log_text()
        fin_down = len(re.findall(r"link_down .*reason=peer closed the connection", text))
        replaced = len(re.findall(r"link_peer_replaced", text))
        if (fin_down - base_fin_downs) + replaced >= graceful_total \
           or time.monotonic() >= deadline:
            break
        time.sleep(0.05)
    rst_down = len(re.findall(r"link_down .*reason=.*Connection reset", text))
    storm_fin = fin_down - base_fin_downs
    checks.hard("every_graceful_cycle_was_seen", storm_fin + replaced >= graceful_total,
                "FIN 收尾的 %d 轮：新增 link_down(reason=EOF)=%d（基线那次不算），"
                "link_peer_replaced=%d" % (graceful_total, storm_fin, replaced))
    checks.note("rst_cycles_may_vanish",
                "RST 收尾的 %d 轮里 %d 轮留下 link_down(reason=reset)；没留下的那些是"
                "还没 accept 就被 reset、被内核从 accept 队列摘掉的，进程没见过，"
                "不是漏记" % (cycles - graceful_total, rst_down))

    # 收尾：正常会话
    link = live_link(args.host, port, args.ready_timeout)
    after = _served_window(ctx, link, chefs, max(2.0, args.duration))
    checks.hard("dispatch_after_storm", len(after.actions) >= 10,
                "%s after %d reconnect cycles" % (after.describe(), cycles))
    checks.hard("actions_wellformed_after_storm",
                not after.bad_actions and not base.bad_actions,
                "malformed: %s" % (after.bad_actions[:2] or base.bad_actions[:2] or "none"))
    still_open = not link.probe_closed(0.4)
    checks.hard("final_connection_really_open", still_open,
                "0.4s 内反复 recv 都没撞到 EOF 才算开着；link=%s" % (link.error or "open"))

    # 解码器若没在重连时复位，残留半行会让后面的字节被读成上一行的尾巴：症状是
    # parse_error（解码器把垃圾当消息交上来）或 framing desync（一行超过上限仍没有换行符）。
    text = ctx.log_text()
    desync = re.findall(r"framing desync|stage=framing", text)
    checks.hard("no_framing_desync_during_storm", not desync,
                "残留半行没被清掉的信号；命中 %d 次: %s" % (len(desync), desync[:2] or "none"))
    checks.hard("no_parse_error_during_storm", log_event_counts(ctx)["parse_error"] == 0,
                "parse_error 次数=%d（半行在复位后不该被当成消息；命中说明解码器串了流）"
                % log_event_counts(ctx)["parse_error"])

    fds_after = proc_fds(pid)
    checks.note("fds_after", "%s — %s" % (describe_fds(fds_after),
                                          ", ".join("%d=%s" % (fd, tgt) for fd, tgt
                                                    in sorted(fds_after.items()))))
    checks.hard("no_fd_leak_after_storm", len(fds_after) == len(fds_before),
                "%d cycles: before=%d fds, after=%d fds%s"
                % (cycles, len(fds_before), len(fds_after),
                   (" — " + fd_delta(fds_before, fds_after))
                   if fd_delta(fds_before, fds_after) else ""))
    checks.hard("no_socket_leak_after_storm",
                len(socket_fds(fds_after)) == len(socket_fds(fds_before)),
                "sockets before=%d after=%d (%s)"
                % (len(socket_fds(fds_before)), len(socket_fds(fds_after)),
                   fd_delta(fds_before, fds_after) or "no new fd"))
    rss_after = proc_rss_kb(pid)
    checks.note("rss_kb", "before=%s after=%s (VmRSS；fd 之外的泄漏也看一眼)"
                % (rss_before, rss_after))
    checks.soft("rss_not_grown_after_storm",
                rss_before is None or rss_after is None or rss_after - rss_before <= 512,
                "VmRSS %s -> %s kB over %d cycles" % (rss_before, rss_after, cycles))
    checks.hard("bridge_alive_after_storm", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    checks.hard("bridge_pid_unchanged", ctx.bridge.pid == pid,
                "pid %s -> %s (no restart expected)" % (pid, ctx.bridge.pid))
    link.close()
    return ("%d reconnect cycles (%s); link_up=%d link_down=%d (FIN 类 link_down(EOF)=%d，"
            "RST 类 link_down(reset)=%d，替换=%d); fds %d -> %d (%d sockets -> %d), "
            "VmRSS %s -> %s kB; then %s; framing desync=%d parse_error=%d"
            % (cycles, dict(forms), events["link_up"], events["link_down"], storm_fin, rst_down,
               replaced, len(fds_before), len(fds_after), len(socket_fds(fds_before)),
               len(socket_fds(fds_after)), rss_before, rss_after, after.describe(), len(desync),
               log_event_counts(ctx)["parse_error"]))


def scenario_data_shapes(ctx, checks):
    """边界：空 chefs、resources 未知键、超大 orders、plate_stack.count==0（bridge.md:104-105）。

    判据分两层：解析层不能因为这四种形状拒收整条 state_update（§5.3 只把"畸形消息"
    和"流失步"分开处理），映射层按 shm.h 的段布局读回来核对 —— 协议上限
    （PROTO_MAX_ORDERS=32）与段上限（KOS_SHM_MAX_ORDERS=16）不同，日志只报前者。
    """
    args = ctx.args
    chefs = 2
    ctx.clear_stale_shm()
    port = ctx.start_bridge_and_wait(link="listen", host=args.host, chefs=chefs,
                                     frame_timeout_ms=args.frame_timeout_ms,
                                     heartbeat_timeout_ms=args.heartbeat_timeout_ms,
                                     port=args.listen_port, verbose=True)
    pid = ctx.bridge.pid
    shm_path = ctx.shm_paths()[0]

    link = live_link(args.host, port, args.ready_timeout)
    base = _served_window(ctx, link, chefs, 1.0)
    checks.hard("baseline_dispatch", len(base.actions) >= 1, base.describe())
    checks.hard("bridge_writes_state_shm", ctx.wait_shm(3.0), "%s" % shm_path)
    # 段布局自检：本工具的 ctypes 镜像必须和 Bridge 写出的段头一致，否则"读回来是什么"
    # 这类结论都不成立。对不上时 read_world_stable() 直接报环境错误（退出码 2），
    # 所以这里只把对上的尺寸记下来当证据。
    snap = read_world_stable(shm_path)
    checks.note("segment_layout", "header=%d world=%d slot=%d x%d max_chefs=%d max_orders=%d "
                                  "max_resources=%d max_order_ingredients=%d"
                % (snap.header.header_size, snap.header.world_size, snap.header.slot_size,
                   snap.header.slot_count, snap.header.max_chefs, snap.header.max_orders,
                   snap.header.max_resources, snap.header.max_order_ingredients))
    max_orders = snap.header.max_orders
    max_resources = snap.header.max_resources
    max_ingredients = snap.header.max_order_ingredients

    served = Stats("served-after-shapes")
    seen = Stats("shape-frames")

    def send_shape(label, payload, settle=0.45):
        """发一帧形状帧、等它落进段里，返回那一刻的快照。

        写 shm 发生在收包的同一步（早于帧屏障），所以 settle 只是给调度抖动留的余量；
        这段时间照常喂心跳，免得 Bridge 因为"插件不出声"而按 W4 断链。
        """
        sent = _safe_send(link, seen, payload, "state_update(%s)" % label)
        checks.hard("shape_frame_sent_%s" % label, sent, "link=%s" % (link.error or "open"))
        pump_with_heartbeats(link, seen, settle, chefs=chefs, ack_actions=not args.no_ack)
        snap = read_world_stable(shm_path)
        # 形状帧之后 Bridge 必须照常下发：这里顺手把"还活着"的证据收下来
        run_exchange(link, served, 0.4, chefs=chefs, hz=args.hz,
                     heartbeat_ms=min(200, args.heartbeat_ms),
                     ack_actions=not args.no_ack)
        return snap

    # 1) chefs: []
    snap = send_shape("empty-chefs", _shape_state_update(chefs=[], frame_index=1))
    checks.hard("empty_chefs_maps_to_zero_chefs", snap.world.chef_count == 0, snap.describe())
    checks.note("empty_chefs_note",
                "N 个输入槽来自 --chefs（本场景 %d），不来自帧里的 chefs 数组，"
                "所以空数组不影响下发" % chefs)

    # 2) resources 未知键：键是动态的，未知键必须原样带过去而不是被拒
    res = {"pot_0": {"state": "occupied", "content": "soup", "time_left": 3},
           "cutting_board_0": {"state": "occupied", "chef_id": 0},
           "plate_stack": {"count": 4}, "pot_7": {"state": "free"},
           "sink_left": {"state": "free"}}
    snap = send_shape("unknown-resource-keys",
                      _shape_state_update(resources=res, frame_index=2))
    names = snap.resource_names()
    unknown_names = names
    checks.hard("unknown_resource_keys_kept_verbatim",
                "pot_7" in names and "sink_left" in names,
                "shm resource names=%s" % names)
    checks.hard("resource_count_matches_frame", snap.world.resource_count == len(res),
                "frame has %d keys, shm has %d" % (len(res), snap.world.resource_count))
    pot7 = snap.resource("pot_7")
    checks.hard("unknown_resource_defaults_are_sane",
                pot7 is not None and _cstr(pot7.state) == "free" and pot7.chef_id == -1
                and pot7.count == 0,
                "pot_7: state=%s chef_id=%d count=%d time_left=%s"
                % (_cstr(pot7.state) if pot7 is not None else "?",
                   pot7.chef_id if pot7 is not None else -999,
                   pot7.count if pot7 is not None else -999,
                   pot7.time_left if pot7 is not None else -999))
    stack4 = snap.resource("plate_stack")
    checks.hard("known_resource_fields_mapped",
                stack4 is not None and stack4.count == 4,
                "plate_stack count=%d (frame says 4)"
                % (stack4.count if stack4 is not None else -999))

    # 3) 超大 orders：解析层丢超出的项并计数，映射层再截到段上限，两层都要自洽
    big_orders = [{"id": "order_%03d-%s" % (i, "x" * 40), "recipe": "recipe_%d" % i,
                   "time_remaining": 100 - i,
                   "ingredients": ["ing_%d_%d" % (i, j) for j in range(10)]}
                  for i in range(100)]
    snap = send_shape("oversized-orders",
                      _shape_state_update(orders=big_orders, frame_index=3))
    counts = snap.order_ingredient_counts()
    ids = snap.order_ids()
    orders_in_shm = snap.world.order_count
    checks.hard("oversized_orders_capped_at_segment_limit",
                snap.world.order_count == max_orders,
                "frame sends %d orders, segment cap=%d, shm order_count=%d"
                % (len(big_orders), max_orders, snap.world.order_count))
    checks.hard("oversized_orders_ingredients_capped",
                bool(counts) and max(counts) <= max_ingredients,
                "per-order ingredient_count=%s (segment cap=%d, frame sends 10)"
                % (sorted(set(counts)), max_ingredients))
    checks.hard("oversized_orders_kept_the_prefix",
                bool(ids) and ids[0].startswith("order_000") and len(ids[0]) <= 31,
                "first shm id=%r (frame sends %d chars; KOS_SHM_NAME_MAX=32 with NUL)"
                % (ids[0] if ids else None, len(big_orders[0]["id"])))

    # 4) plate_stack.count == 0：0 是合法计数，不是"缺失"
    snap = send_shape("plate-stack-zero",
                      _shape_state_update(resources={"plate_stack": {"count": 0}},
                                          frame_index=4))
    stack0 = snap.resource("plate_stack")
    checks.hard("plate_stack_zero_stays_zero",
                stack0 is not None and _cstr(stack0.name) == "plate_stack" and stack0.count == 0,
                "plate_stack: name=%s count=%d"
                % (_cstr(stack0.name) if stack0 is not None else "missing",
                   stack0.count if stack0 is not None else -999))

    # 5) resources 也有上限：40 个键只该留下前 32 个
    many = {"pot_%d" % i: {"state": "free"} for i in range(40)}
    snap = send_shape("oversized-resources",
                      _shape_state_update(resources=many, frame_index=5))
    names = snap.resource_names()
    resources_in_shm = snap.world.resource_count
    checks.hard("oversized_resources_capped_at_segment_limit",
                snap.world.resource_count == max_resources,
                "frame sends 40 keys, segment cap=%d, shm resource_count=%d"
                % (max_resources, snap.world.resource_count))
    checks.hard("oversized_resources_kept_the_prefix",
                "pot_0" in names and "pot_39" not in names,
                "shm first=%s last=%s" % (names[:1], names[-1:]))

    checks.hard("dispatch_continued_after_every_shape", len(served.actions) >= 4,
                "%d actions in the 0.4s windows after each of the 5 shape frames"
                % len(served.actions))
    checks.hard("actions_wellformed_for_shapes",
                not served.bad_actions and not seen.bad_actions,
                "malformed: %s" % (served.bad_actions[:2] or seen.bad_actions[:2] or "none"))
    checks.hard("no_parse_error_for_these_shapes",
                log_event_counts(ctx)["parse_error"] == 0,
                "parse_error 次数=%d（这四种形状都该被接受，只有畸形/失步才拒）"
                % log_event_counts(ctx)["parse_error"])
    checks.hard("log_shows_the_empty_chefs_frame",
                bool(re.search(r"state_update .*chefs=0 ", ctx.log_text())),
                "state_update 行里应能看到 chefs=0：解析层确实收下了空数组")
    dropped = [int(n) for n in re.findall(r"state_update .*?dropped=(\d+)", ctx.log_text())]
    checks.note("parse_diag_dropped_items",
                "state_update 行里最大的 dropped=%s（%d 行；协议上限 32 单 / 8 食材，"
                "与段上限 %d / %d 不同）"
                % (max(dropped) if dropped else None, len(dropped), max_orders, max_ingredients))
    checks.soft("log_reports_orders_above_protocol_cap",
                max(dropped, default=0) > 0,
                "100 单超出 PROTO_MAX_ORDERS=32，dropped 应当非 0；实际 %s"
                % (max(dropped) if dropped else "无 state_update 行"))
    checks.hard("bridge_alive_after_shapes", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    checks.hard("bridge_pid_unchanged", ctx.bridge.pid == pid,
                "pid %s -> %s (no restart expected)" % (pid, ctx.bridge.pid))
    link.close()
    return ("empty chefs -> chef_count=0; unknown keys kept verbatim (%s); 100 orders -> "
            "%d in shm (cap %d), ingredients<=%d, dropped=%s; plate_stack.count=0 -> 0; "
            "40 resources -> %d in shm"
            % (unknown_names, orders_in_shm, max_orders, max_ingredients,
               max(dropped) if dropped else None, resources_in_shm))


def scenario_second_peer(ctx, checks):
    """第二个对端在第一个还活着时连进来（spec 没写，本场景只记录+钉不变量）。

    net.c:547-558 的实现是"最新连接获胜"：accept 到新连接就关掉旧 peer 的 fd。
    spec 对此沉默，所以这里不改语义，只断言与语义无关的三条不变量：
      - 一个厨房只能有一个执行者：两个对端不能同时收到动作；
      - 替换不能留下多余的 socket fd；
      - 无论谁被留下，进程都要活着，且被留下的那个必须真的还连着。
    """
    args = ctx.args
    chefs = 1
    ctx.clear_stale_shm()
    port = ctx.start_bridge_and_wait(link="listen", host=args.host, chefs=chefs,
                                     frame_timeout_ms=args.frame_timeout_ms,
                                     heartbeat_timeout_ms=args.heartbeat_timeout_ms,
                                     port=args.listen_port)
    pid = ctx.bridge.pid

    link_a = live_link(args.host, port, args.ready_timeout)
    first = Stats("peer-a")
    run_exchange(link_a, first, 1.0, chefs=chefs, hz=args.hz, heartbeat_ms=args.heartbeat_ms,
                 ack_actions=not args.no_ack)
    checks.hard("first_peer_gets_actions", len(first.actions) >= 1, first.describe())
    sockets_one_peer = len(socket_fds(proc_fds(pid)))
    checks.note("peer_a_alone", "%s; bridge %s" % (first.describe(), describe_fds(proc_fds(pid))))
    events_before = log_event_counts(ctx)

    link_b = live_link(args.host, port, args.ready_timeout)
    checks.note("peer_b", "second connection opened while peer A is still open")

    # A 的下场：Bridge 若选择替换，A 应当立刻看到自己的连接被关（FIN/RST）
    a_closed = link_a.probe_closed(1.5)
    checks.note("peer_a_after_b_connected",
                "closed=%s error=%s" % (a_closed, link_a.error or "none"))

    # 之后一个窗口里，两边都发帧、都发心跳，各收各的：动作收到谁手里就是谁在被服务
    window = max(1.5, args.duration / 2)
    a_during = Stats("peer-a-during")
    b_during = Stats("peer-b-during")
    period = 1.0 / args.hz if args.hz > 0 else 0.0
    hb_period = min(300, args.heartbeat_ms) / 1000.0
    start = time.monotonic()
    next_state = start
    next_hb = start
    while time.monotonic() - start < window:
        for msg in link_a.pump(0.01):
            _absorb(msg, a_during, None, False, chefs)
        for msg in link_b.pump(0.01):
            _absorb(msg, b_during, None, False, chefs)
        now = time.monotonic()
        if period and now >= next_state:
            # 已经断掉的那条不再发：给一个死连接发帧只会刷一堆 tx_error
            for lk, st in ((link_a, a_during), (link_b, b_during)):
                if lk.closed:
                    continue
                _safe_send(lk, st, msg_state_update(chefs, st.frame_index), "state_update")
                st.frame_index += 1
            next_state += period
            if next_state < now:
                next_state = now + period
        if now >= next_hb:
            for lk, st in ((link_a, a_during), (link_b, b_during)):
                if not lk.closed:
                    _safe_send(lk, st, msg_heartbeat(), "heartbeat")
            next_hb += hb_period

    a_actions, b_actions = len(a_during.actions), len(b_during.actions)
    winner = "peer B (the newer connection)" if b_actions else (
        "peer A (the older connection)" if a_actions else "neither")
    checks.note("who_gets_served", "peer A %d actions, peer B %d actions in %.1fs -> %s"
                % (a_actions, b_actions, window, winner))
    checks.hard("exactly_one_peer_is_served", (a_actions > 0) != (b_actions > 0),
                "one kitchen has one executor: A=%d actions, B=%d actions" % (a_actions, b_actions))
    checks.hard("served_peer_keeps_getting_actions", max(a_actions, b_actions) >= 3,
                "winner=%s got %d actions in %.1fs" % (winner, max(a_actions, b_actions), window))
    checks.hard("actions_wellformed_with_two_peers",
                not a_during.bad_actions and not b_during.bad_actions,
                "malformed: %s" % (a_during.bad_actions[:2] or b_during.bad_actions[:2] or "none"))
    checks.hard("displaced_peer_sees_a_closed_socket",
                (b_actions == 0) or a_closed,
                "peer B is being served, so peer A must have been told (closed=%s, "
                "error=%s) rather than left hanging" % (a_closed, link_a.error or "none"))
    sockets_after = len(socket_fds(proc_fds(pid)))
    checks.hard("second_peer_leaves_no_extra_socket", sockets_after == sockets_one_peer,
                "sockets with peer A alone=%d, after B connected=%d (listen + one peer)"
                % (sockets_one_peer, sockets_after))
    checks.hard("bridge_alive_after_second_peer", ctx.bridge.alive(),
                "exit=%s" % ctx.bridge.exit_code())
    checks.hard("bridge_pid_unchanged", ctx.bridge.pid == pid,
                "pid %s -> %s (no restart expected)" % (pid, ctx.bridge.pid))
    events_after = log_event_counts(ctx)
    swaps = events_after["link_peer_replaced"] - events_before["link_peer_replaced"]
    checks.note("log_events_with_two_peers",
                "link_up=%d->%d link_down=%d->%d link_peer_replaced=%d->%d"
                % (events_before["link_up"], events_after["link_up"],
                   events_before["link_down"], events_after["link_down"],
                   events_before["link_peer_replaced"], events_after["link_peer_replaced"]))
    # spec 要求日志含 IPC 连接与断开（bridge.md:27-28）：第二个对端顶掉第一个时，
    # 链路始终是 UP，所以状态机自己看不见这次替换 —— 不记就是一条连接事件丢失。
    swap_line = re.search(r"link_peer_replaced .*old_peer_fd=\d+ new_peer_fd=\d+",
                          ctx.log_text())
    checks.hard("log_records_the_peer_swap", swaps >= 1 and bool(swap_line),
                "替换必须留一行带新旧 fd 的记录；link_peer_replaced 新增 %d 行，"
                "样本=%s" % (swaps, swap_line.group(0) if swap_line else "none"))
    link_a.close()
    link_b.close()
    return ("peer A alone got %s; then B connected -> %s; a_closed=%s; sockets %d -> %d"
            % (first.describe(), winner, a_closed, sockets_one_peer, sockets_after))


def _read_jsonl(path):
    rows = []
    bad = []
    try:
        text = Path(path).read_text(errors="replace")
    except OSError:
        return rows, bad
    for line in text.splitlines():
        if not line.strip():
            continue
        try:
            rows.append(json.loads(line))
        except ValueError as exc:
            bad.append("%s: %s" % (line[:80], exc))
    return rows, bad



# --------------------------------------------------------------------------
# 走位场景：走廊地图、按 path 移动的模拟厨师
# --------------------------------------------------------------------------

NAV_SPEED_MPS = 5.6          # 任务 01 实测 0.107 s/格（0.6 m）≈ 5.6 m/s
NAV_STEP = 0.6


def nav_corridor_configs(directory, *, stations, cells, masks):
    """写一套最小的 kitchen/recipes/orders，用于走位场景（不依赖真游戏导出的图）。"""
    directory = Path(directory)
    kitchen = {
        "scene": "nav_corridor",
        "grid": {
            "step": NAV_STEP,
            "map": [1, 1],
            "edge_bits": "1:+x 2:-x 4:+z 8:-z",
            "cells": cells,
            "mask": masks,
            "areas": {"count": 1, "largest": len(cells), "sizes": [len(cells)]},
        },
        "stations": stations,
    }
    nodes = []
    dishes = []
    for station in stations:
        ing = station["gives"]
        node_id = "fetch:" + ing
        nodes.append({"id": node_id, "op": "fetch", "label": ing,
                      "resource": "dispenser", "ingredient": ing,
                      "duration": 0.2})
        dishes.append({"dish": ing + "_Dish", "score": 1, "root": node_id,
                       "nodes": [node_id]})
    recipes = {"scene": "nav_corridor", "nodes": nodes, "edges": [],
               "dishes": dishes, "durations": {"fetch": 0.2}}
    orders = {"seed": 7, "level": "nav_corridor",
              "menu": [{"recipes": [d["dish"] for d in dishes]}]}
    paths = {}
    for name, payload in (("kitchen.json", kitchen), ("recipes.json", recipes),
                          ("orders.json", orders)):
        path = directory / name
        path.write_text(json.dumps(payload, ensure_ascii=False))
        paths[name] = str(path)
    return paths


def nav_line_map(*, length, bay, station_sides):
    """一格宽走廊；bay 为 True 时在中间加一个错身凹槽。"""
    cells = [[round(i * NAV_STEP, 3), 0.0] for i in range(length)]
    masks = [0] * length
    for i in range(length):
        if i > 0:
            masks[i] |= 2  # -x
        if i + 1 < length:
            masks[i] |= 1  # +x
    if bay:
        middle = length // 2
        cells.append([round(middle * NAV_STEP, 3), round(NAV_STEP, 3)])
        masks.append(8)  # 朝 -z 回主道
        masks[middle] |= 4  # +z 进凹槽
    stations = []
    for side in station_sides:
        if side == "left":
            stations.append({"id": "dispenser-left", "kind": "dispenser",
                             "name": "dispenser_left", "types": ["ClientPickupItemSpawner"],
                             "pos": [-NAV_STEP, 0.0], "height": 0.0,
                             "gives": "Left", "contents": []})
        else:
            stations.append({"id": "dispenser-right", "kind": "dispenser",
                             "name": "dispenser_right", "types": ["ClientPickupItemSpawner"],
                             "pos": [round(length * NAV_STEP, 3), 0.0], "height": 0.0,
                             "gives": "Right", "contents": []})
    return cells, masks, stations


def nav_ring_map(*, length=11):
    """一格宽的矩形环走廊；左右两端是料箱站位，用于两人对向错身。"""
    cells = []
    masks = []
    for i in range(length):
        cells.append([round(i * NAV_STEP, 3), 0.0])
        masks.append((2 if i > 0 else 0) | (1 if i + 1 < length else 0))
    for i in range(length):
        cells.append([round(i * NAV_STEP, 3), round(2 * NAV_STEP, 3)])
        masks.append((2 if i > 0 else 0) | (1 if i + 1 < length else 0))
    left_mid = len(cells)
    cells.append([0.0, round(NAV_STEP, 3)])
    masks.append(8 | 4)  # -z 接上排 0，+z 接下排 length
    right_mid = len(cells)
    cells.append([round((length - 1) * NAV_STEP, 3), round(NAV_STEP, 3)])
    masks.append(8 | 4)
    masks[0] |= 4
    masks[length] |= 8
    masks[length - 1] |= 4
    masks[2 * length - 1] |= 8
    stations = [
        {"id": "dispenser-right", "kind": "dispenser", "name": "dispenser_right",
         "types": ["ClientPickupItemSpawner"],
         "pos": [round(length * NAV_STEP, 3), round(NAV_STEP, 3)],
         "height": 0.0, "gives": "Right", "contents": []},
        {"id": "dispenser-left", "kind": "dispenser", "name": "dispenser_left",
         "types": ["ClientPickupItemSpawner"],
         "pos": [-NAV_STEP, round(NAV_STEP, 3)],
         "height": 0.0, "gives": "Left", "contents": []},
    ]
    return cells, masks, stations, left_mid, right_mid


def nav_cell_index(cells, x, y):
    """离 (x,y) 最近的可走格下标；坐标在两个格之间时取欧氏最近。"""
    best = -1
    best_d = None
    for i, (cx, cy) in enumerate(cells):
        dx = cx - x
        dy = cy - y
        d = dx * dx + dy * dy
        if best_d is None or d < best_d:
            best_d = d
            best = i
    return best


class NavSim:
    """按 path 动作移动模拟厨师，并记录每帧位置/同格/到达时刻。

    seq 语义与插件一致：同一 seq 只在路点变长时追加，新 seq 替换整条命令；
    stop 清掉当前命令（navd 崩溃时 Worker 发的 stop 必须真的让厨师停下）。
    """

    def __init__(self, positions, cells, *, speed=NAV_SPEED_MPS, frozen=()):
        self.positions = [[float(x), float(y)] for x, y in positions]
        self.cells = cells
        self.speed = speed
        self.frozen = {int(c) for c in frozen}
        self.commands = [None] * len(positions)
        self.seen_actions = 0
        self.last_game_ms = 0
        self.collisions = []
        self.arrivals = {}
        self.stops = {}
        self.path_actions = Counter()
        self.move_actions = Counter()
        self.frames = 0

    def _chef(self, action):
        chef = action.get("chef_id")
        if not isinstance(chef, int) or isinstance(chef, bool):
            return None
        if chef < 0 or chef >= len(self.positions):
            return None
        return chef

    def process_actions(self, actions, game_ms):
        for action in actions[self.seen_actions:]:
            chef = self._chef(action)
            if chef is None:
                continue
            kind = action.get("action")
            if kind == "path":
                self.path_actions[chef] += 1
                waypoints = action.get("waypoints") or []
                seq = action.get("seq", -1)
                command = self.commands[chef]
                if command is not None and command["seq"] == seq:
                    if len(waypoints) > len(command["waypoints"]):
                        command["waypoints"].extend(waypoints[len(command["waypoints"]):])
                else:
                    self.commands[chef] = {"seq": seq,
                                           "waypoints": list(waypoints),
                                           "index": 0}
            elif kind == "move":
                self.move_actions[chef] += 1
            elif kind == "stop":
                self.commands[chef] = None
                self.stops.setdefault(chef, []).append(game_ms)
        self.seen_actions = len(actions)

    def advance(self, game_ms):
        dt = max(0, game_ms - self.last_game_ms)
        self.last_game_ms = game_ms
        if dt <= 0:
            return
        for chef, command in enumerate(self.commands):
            if command is None or chef in self.frozen:
                continue
            budget = self.speed * dt / 1000.0
            while budget > 0 and command["index"] < len(command["waypoints"]):
                waypoint = command["waypoints"][command["index"]]
                tx = float(waypoint["x"])
                ty = float(waypoint["y"])
                x, y = self.positions[chef]
                dx = tx - x
                dy = ty - y
                distance = (dx * dx + dy * dy) ** 0.5
                if distance <= 1e-6:
                    command["index"] += 1
                    continue
                if distance <= budget:
                    self.positions[chef] = [tx, ty]
                    command["index"] += 1
                    budget -= distance
                else:
                    self.positions[chef] = [x + dx * budget / distance,
                                            y + dy * budget / distance]
                    budget = 0

    def frame(self, game_ms):
        """记录这一帧的格子占用；返回是否发生同格。"""
        occupied = [nav_cell_index(self.cells, x, y) for x, y in self.positions]
        counts = Counter(occupied)
        same = any(n > 1 for n in counts.values())
        if same:
            self.collisions.append((game_ms, occupied))
        self.frames += 1
        return same

    def note_arrival(self, chef, x, y, game_ms, tolerance=0.15):
        px, py = self.positions[chef]
        dx = px - x
        dy = py - y
        if dx * dx + dy * dy <= tolerance * tolerance and chef not in self.arrivals:
            self.arrivals[chef] = game_ms


class WorldSim(NavSim):
    """NavSim + 宏动作完成条件要读的那部分世界状态。

    只模拟 fake_plugin 自己能看到的事实：interact 改 holding/工位内容，work 推进
    progress，serving 上的 interact 移除一单。效果在 delay_ms 之后才生效，`macro`
    场景用它验证"世界状态变了才 macro_done"；效果按厨师去重（Worker 在 250 ms 窗口
    内会重复发同一条 interact，不能变成连点）。
    """

    def __init__(self, positions, cells, stations, *, delay_ms=0, products=None,
                 resources=None, orders=None, frozen=(), wall_delay=False):
        super().__init__(positions, cells, frozen=frozen)
        self.stations = {s.get("id"): s for s in stations if s.get("id")}
        self.delay_ms = int(delay_ms)
        self.wall_delay = bool(wall_delay)
        self.frozen_clock = 0
        self.products = dict(products or {})
        self.resources = {name: dict(payload)
                          for name, payload in (resources or {}).items()}
        self.resources.setdefault("plate_stack", {"count": 0})
        for station in self.stations.values():
            kind = station.get("kind", "")
            sid = station["id"]
            if kind in ("cooker", "container", "mixer"):
                self.resources.setdefault(sid, {"state": "free"})
            elif kind == "sink":
                self.resources.setdefault(sid, {"state": "free", "count": 0,
                                                "progress": 0.0})
        self.orders = [dict(order) for order in (orders or [])]
        self.holdings = [None] * len(positions)
        self.facings = [0.0] * len(positions)
        self.seen_world_actions = 0
        self.pending = []
        self.busy = set()
        self.cooldown = {}
        self.delivered = []
        self.stalled = set()

    # -- 世界状态访问 --
    def resource(self, name, default=None):
        if name not in self.resources:
            self.resources[name] = dict(default) if default is not None else {"state": "free"}
        return self.resources[name]

    def resources_message(self):
        return {name: dict(payload) for name, payload in self.resources.items()}

    def message(self, n_chefs, frame_index, cells, game_ms):
        return msg_state_update_os(
            n_chefs, frame_index, cells,
            positions=[tuple(p) for p in self.positions], game_ms=game_ms,
            resources=self.resources_message(), holdings=self.holdings,
            facings=self.facings, orders=self.orders)

    def _schedule(self, game_ms, chef, effect, station_id=None, cooldown=False):
        self.busy.add(chef)
        if self.wall_delay:
            due = time.monotonic() * 1000.0 + self.delay_ms
        else:
            due = game_ms + self.delay_ms

        def run():
            effect()
            if cooldown and station_id is not None:
                # Worker sees world state one or two frames later; coalesce repeated interact.
                self.cooldown[(chef, station_id)] = time.monotonic() * 1000.0 + 750.0

        self.pending.append((due, chef, run))

    def _finish(self, chef):
        self.busy.discard(chef)

    def process_actions(self, actions, game_ms):
        super().process_actions(actions, game_ms)
        for action in actions[self.seen_world_actions:]:
            chef = self._chef(action)
            if chef is None:
                continue
            kind = action.get("action")
            if kind == "face":
                target = action.get("target") or {}
                station = self.stations.get(action.get("target_object") or "")
                if isinstance(target.get("x"), (int, float)) and isinstance(target.get("y"), (int, float)):
                    tx, ty = float(target["x"]), float(target["y"])
                elif station is not None:
                    pos = station.get("pos") or [0.0, 0.0]
                    tx, ty = float(pos[0]), float(pos[1])
                else:
                    continue
                x, y = self.positions[chef]
                self.facings[chef] = math.atan2(tx - x, ty - y)
                continue
            station = self.stations.get(action.get("target_object") or "")
            if station is None:
                continue
            if kind == "interact":
                self._interact(chef, station, game_ms)
            elif kind == "work":
                self._work(chef, station, game_ms)
        self.seen_world_actions = len(actions)

    def _interact(self, chef, station, game_ms):
        if station["id"] in self.stalled or chef in self.busy:
            return
        if self.cooldown.get((chef, station["id"]), 0.0) > time.monotonic() * 1000.0:
            return
        kind = station.get("kind", "")
        holding = self.holdings[chef]

        if kind == "dispenser":
            gives = station.get("gives") or ""
            if holding is not None or not gives:
                return

            def take():
                self.holdings[chef] = gives
                self._finish(chef)

            self._schedule(game_ms, chef, take, station["id"], cooldown=True)
            return

        resource = self.resource(station["id"])
        if kind in ("container", "mixer"):
            if holding is not None:
                def put():
                    resource["content"] = holding
                    resource["state"] = "occupied"
                    resource["progress"] = 0.0
                    self.holdings[chef] = None
                    self._finish(chef)

                self._schedule(game_ms, chef, put)
            elif float(resource.get("progress") or 0.0) >= 0.999 and resource.get("content"):
                product = self.products.get(station["id"], resource["content"])

                def take_product():
                    self.holdings[chef] = product
                    resource["content"] = ""
                    resource["state"] = "free"
                    resource["progress"] = 0.0
                    self._finish(chef)

                self._schedule(game_ms, chef, take_product, station["id"], cooldown=True)
            return

        if kind == "cooker":
            if holding is not None and not (holding == "Plate" and resource.get("state") == "cooked"):
                def put_in_pot():
                    self.holdings[chef] = None
                    resource["content"] = holding
                    resource["state"] = "cooking"
                    resource["progress"] = 0.1
                    self._finish(chef)
                    self.pending.append((time.monotonic() * 1000.0 + self.delay_ms,
                                         chef, lambda: resource.__setitem__("state", "cooked")))

                self._schedule(game_ms, chef, put_in_pot)
            elif resource.get("state") == "cooked" and resource.get("content"):
                def take_cooked():
                    product = self.products.get(station["id"], resource["content"])
                    self.holdings[chef] = (holding + "+" + product) if holding else product
                    resource["content"] = ""
                    resource["state"] = "free"
                    resource["progress"] = 0.0
                    self._finish(chef)

                self._schedule(game_ms, chef, take_cooked, station["id"], cooldown=True)
            return

        if kind == "sink":
            if holding is not None:
                def drop_dirty():
                    self.holdings[chef] = None
                    self._finish(chef)

                self._schedule(game_ms, chef, drop_dirty)
            return

        if kind == "serving":
            if holding is not None:
                def deliver():
                    self.holdings[chef] = None
                    if self.orders:
                        self.delivered.append(self.orders.pop(0).get("id"))
                    self._finish(chef)

                self._schedule(game_ms, chef, deliver)
            return

        if kind in ("plate_stack", "clean_plate_stack"):
            if holding is not None:
                return
            product = self.products.get(station["id"], "Plate")

            def take_plate():
                self.holdings[chef] = product
                self._finish(chef)

            self._schedule(game_ms, chef, take_plate, station["id"], cooldown=True)

    def _work(self, chef, station, game_ms):
        if chef in self.busy:
            return
        resource = self.resource(station["id"])
        if float(resource.get("progress") or 0.0) >= 0.999:
            return
        kind = station.get("kind", "")

        def finish_work():
            resource["progress"] = 1.0
            if kind == "sink":
                resource["count"] = int(resource.get("count") or 0) + 1
                plates = self.resource("plate_stack", {"count": 0})
                plates["count"] = int(plates.get("count") or 0) + 1
            self._finish(chef)

        self._schedule(game_ms, chef, finish_work)

    def _apply_due(self, now_ms):
        due = [entry for entry in self.pending if entry[0] <= now_ms]
        self.pending = [entry for entry in self.pending if entry[0] > now_ms]
        for _, _, effect in sorted(due, key=lambda entry: entry[0]):
            effect()

    def _clock(self, real_game_ms):
        if self.wall_delay and self.busy:
            return self.frozen_clock
        self.frozen_clock = real_game_ms
        return real_game_ms

    def tick(self, real_game_ms):
        if self.wall_delay:
            self._apply_due(time.monotonic() * 1000.0)
        game_ms = self._clock(real_game_ms)
        if not self.wall_delay:
            self._apply_due(game_ms)
        super().advance(game_ms)
        return game_ms


def read_log_events(path):
    counts = Counter()
    try:
        text = Path(path).read_text(errors="replace")
    except OSError:
        return counts
    for line in text.splitlines():
        match = LOG_LINE.match(line)
        if match:
            counts[match.group(1)] += 1
    return counts


def _nav_scenario(ctx, checks, *, name, length, bay, station_sides, starts,
                  goal_xy, duration, freeze=(), backoff_ms=None, kill_after=None,
                  require_blocked=False, ring=False):
    """nav-corridor / nav-stuck / nav-crash 共用的编排。"""
    args = ctx.args
    chefs = len(starts)
    workdir = Path(tempfile.mkdtemp(prefix="kitchenos-%s-" % name))
    if ring:
        cells, masks, stations, _left, _right = nav_ring_map(length=11)
    else:
        cells, masks, stations = nav_line_map(length=length, bay=bay,
                                              station_sides=station_sides)
    paths = nav_corridor_configs(workdir, stations=stations, cells=cells, masks=masks)
    period_ms = 1000.0 / args.hz
    sim = NavSim(starts, cells, frozen=freeze)

    ctx.clear_stale_shm()
    port = args.listen_port or pick_free_port(args.host)
    ctx.start_supervisor(port=port, chefs=chefs, kitchen=paths["kitchen.json"],
                         recipes=paths["recipes.json"], orders=paths["orders.json"],
                         backoff_ms=backoff_ms)
    if not ctx.wait_supervisor_ready(port, chefs, args.ready_timeout):
        raise InfraError("Supervisor tree never reached running state on %s:%d\n%s" %
                         (args.host, port, ctx.diagnostics()))
    checks.note("supervisor_tree", "bridge+scheduler+navd+%d workers running" % chefs)

    clock = {"paused": False, "pause_game_ms": 0, "frozen_frames": 0}

    def current_game_ms(frame_index):
        if clock["paused"]:
            clock["frozen_frames"] += 1
            return clock["pause_game_ms"]
        return int(round((frame_index - clock["frozen_frames"]) * period_ms))

    def builder(n, frame_index):
        game_ms = current_game_ms(frame_index)
        sim.process_actions(stats.actions, game_ms)
        sim.advance(game_ms)
        sim.frame(game_ms)
        for chef, (gx, gy) in goal_xy.items():
            sim.note_arrival(chef, gx, gy, game_ms)
        return msg_state_update_os(n, frame_index, cells,
                                   positions=[tuple(p) for p in sim.positions],
                                   game_ms=game_ms, resources={})

    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    stats = Stats(name)
    killed_at = None
    pause_replans_before = None
    pause_replans_after = None
    try:
        if require_blocked:
            # 先正常跑 1 s，再把游戏时钟冻结 2.5 s：暂停期间坐标不动，navd 不得
            # 判卡住；恢复后仍应凑满 3 次重规划。
            pre, paused = 1.0, 2.5
            run_exchange(link, stats, pre, chefs=chefs, hz=args.hz,
                         heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                         state_builder=builder)
            nav_log_path = (ctx.runtime_dir / "logs" / "navd.log") if ctx.runtime_dir else None
            pause_replans_before = (read_log_events(nav_log_path).get("nav_replan", 0)
                                    if nav_log_path else 0)
            clock["pause_game_ms"] = current_game_ms(stats.frame_index)
            clock["paused"] = True
            run_exchange(link, stats, paused, chefs=chefs, hz=args.hz,
                         heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                         state_builder=builder)
            clock["paused"] = False
            pause_replans_after = (read_log_events(nav_log_path).get("nav_replan", 0)
                                   if nav_log_path else 0)
            run_exchange(link, stats, duration - pre - paused, chefs=chefs, hz=args.hz,
                         heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                         state_builder=builder)
        elif kill_after is None:
            run_exchange(link, stats, duration, chefs=chefs, hz=args.hz,
                         heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                         state_builder=builder)
        else:
            run_exchange(link, stats, kill_after, chefs=chefs, hz=args.hz,
                         heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                         state_builder=builder)
            killed_at = int(round(stats.frame_index * period_ms))
            status = ctx.supervisor_status()
            navd_pid = next((c.get("pid") for c in status.get("children", [])
                             if c.get("role") == "navd"), None)
            if not navd_pid:
                raise InfraError("no running navd in status.json: %s" % status)
            os.kill(navd_pid, signal.SIGKILL)
            checks.note("navd_killed", "pid=%s at game_ms=%d" % (navd_pid, killed_at))
            run_exchange(link, stats, duration - kill_after, chefs=chefs, hz=args.hz,
                         heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                         state_builder=builder)
            deadline = time.monotonic() + 8.0
            restarted = False
            while time.monotonic() < deadline:
                status = ctx.supervisor_status()
                navd = next((c for c in status.get("children", [])
                             if c.get("role") == "navd"), {})
                if (navd.get("spawns") or 0) >= 2:
                    restarted = True
                    break
                time.sleep(0.05)
            checks.hard("navd_restarted", restarted,
                        "navd spawns=%s" % (navd or {}))
    finally:
        link.close()
        if ctx.supervisor is not None:
            ctx.stop_supervisor()

    checks.note("sim_final_positions", sim.positions)
    checks.note("sim_arrivals", sim.arrivals)
    first_paths = {}
    for chef in range(chefs):
        seen = [a for a in stats.actions
                if a.get("chef_id") == chef and a.get("action") == "path"]
        first_paths[chef] = [
            (a.get("seq"), len(a.get("waypoints") or []),
             (a.get("waypoints") or [{}])[0], (a.get("waypoints") or [{}])[-1])
            for a in seen[:1]
        ]
    checks.note("first_paths", first_paths)
    checks.hard("path_actions_seen", sum(sim.path_actions.values()) > 0,
                "paths per chef=%s" % dict(sim.path_actions))
    checks.hard("no_move_action",
                sum(sim.move_actions.values()) == 0,
                "move per chef=%s (system path must only send path)" %
                dict(sim.move_actions))
    checks.hard("no_same_cell_frames", not sim.collisions,
                "collisions=%s" % (sim.collisions[:3] or "none"))

    nav_log = (ctx.runtime_dir / "logs" / "navd.log") if ctx.runtime_dir else None
    nav_events = read_log_events(nav_log) if nav_log is not None else Counter()
    checks.note("navd_events", dict(nav_events))

    run_dir = ctx.run_dir()
    events, bad = _read_jsonl(run_dir / "events.jsonl") if run_dir else ([], [])
    checks.hard("events_parse_as_json", not bad, "%d bad line(s)" % len(bad))
    dispatch = [e for e in events if e.get("event") == "task_dispatched"]
    checks.note("task_dispatched", dispatch[:4])

    if require_blocked:
        blocked = [e for e in events if e.get("event") == "task_blocked" and
                   e.get("chef_id") == 0]
        replans = nav_events.get("nav_replan", 0)
        checks.hard("paused_clock_does_not_count_stuck",
                    pause_replans_before is not None and
                    pause_replans_after == pause_replans_before,
                    "replans before pause=%s, after pause=%s" %
                    (pause_replans_before, pause_replans_after))
        checks.hard("stuck_replans_reported", replans >= 3,
                    "nav_replan=%d (need >=3)" % replans)
        checks.hard("worker_reports_blocked", bool(blocked),
                    "task_blocked chef0: %s" % (blocked[:1] or "none"))
        blocked_at = blocked[0].get("t_ms") if blocked else None
        reassign = [e for e in dispatch if e.get("chef_id") != 0 and
                    e.get("node") == (blocked[0].get("node") if blocked else None) and
                    (blocked_at is None or (e.get("t_ms") or 0) >= blocked_at)]
        checks.hard("blocked_task_reassigned", bool(reassign),
                    "same node dispatched to another chef after blocked: %s" %
                    (reassign[:2] or "none"))
    else:
        for chef, (gx, gy) in goal_xy.items():
            at = sim.arrivals.get(chef)
            checks.hard("chef%d_arrived_within_10s" % chef,
                        at is not None and at <= 10000,
                        "arrival game_ms=%s" % at)

    if kill_after is not None:
        first_stop = None
        for when in sim.stops.get(0, []):
            if killed_at is not None and when > killed_at:
                first_stop = when
                break
        latency = (first_stop - killed_at) if first_stop is not None else None
        checks.hard("stop_after_navd_crash",
                    latency is not None and latency <= 700,
                    "stop latency game_ms=%s (stale threshold 500)" % latency)
        at = sim.arrivals.get(0)
        checks.hard("chef_resumed_after_restart", at is not None and
                    killed_at is not None and at > killed_at,
                    "arrival game_ms=%s killed_at=%s" % (at, killed_at))

    shutil.rmtree(workdir, ignore_errors=True)
    return stats.describe()


def scenario_nav_corridor(ctx, checks):
    """1 格宽环走廊两端对向走：10 s 游戏时钟内两人到达，任何一帧不同格。

    环走廊没有死胡同，PIBT 的可达性前提成立（每对相邻格都在环上）；两人从
    下排两端出发，目标分别是右、左中点的料箱站位，路径自然对向并在环上错身。
    """
    return _nav_scenario(ctx, checks, name="nav-corridor", length=11, bay=False,
                         station_sides=("right", "left"), ring=True,
                         starts=[(0.0, 1.2), (6.0, 1.2)],
                         goal_xy={0: (6.0, 0.6), 1: (0.0, 0.6)},
                         duration=4.0)


def scenario_nav_stuck(ctx, checks):
    """一个厨师被冻住 → navd 重规划 3 次 → BLOCKED → Scheduler 改派另一个厨师。"""
    return _nav_scenario(ctx, checks, name="nav-stuck", length=12, bay=False,
                         station_sides=("right",),
                         starts=[(0.0, 0.0), (6.6, 0.0)],
                         goal_xy={1: (6.6, 0.0)},
                         duration=10.0, freeze=(0,), require_blocked=True)


def scenario_nav_crash(ctx, checks):
    """SIGKILL navd：Worker 在 500 ms 级延迟内 stop；Supervisor 重启后继续到达。"""
    return _nav_scenario(ctx, checks, name="nav-crash", length=12, bay=False,
                         station_sides=("right",), starts=[(0.0, 0.0)],
                         goal_xy={0: (6.6, 0.0)}, duration=5.0,
                         kill_after=0.6, backoff_ms="2000")


def macro_configs(directory):
    """宏动作场景的最小厨房：一条 12 格走廊，六类工位各一个，一单用五道工序走完。

    配置订单的 menu 故意留空，让 fake 的世界订单成为唯一来源，这样一局里只有一条
    工序链，断言不会被 Scheduler 同时开的五单搅乱。
    """
    directory = Path(directory)
    cells, masks, _ = nav_line_map(length=12, bay=False, station_sides=())
    stations = []
    for sid, kind, gives, x in (
            ("dispenser-0", "dispenser", "Tomato", 0.0),
            ("container-0", "container", "", 1.2),
            ("cooker-0", "cooker", "", 2.4),
            ("plate_stack-0", "plate_stack", "", 3.6),
            ("serving-0", "serving", "", 4.8),
            ("sink-0", "sink", "", 6.0)):
        stations.append({"id": sid, "kind": kind, "name": sid.replace("-", "_"),
                         "types": [], "pos": [x, 1.2], "height": 0.0,
                         "gives": gives, "contents": []})
    kitchen = {
        "scene": "macro",
        "grid": {
            "step": NAV_STEP,
            "map": [1, 1],
            "edge_bits": "1:+x 2:-x 4:+z 8:-z",
            "cells": cells,
            "mask": masks,
            "areas": {"count": 1, "largest": len(cells), "sizes": [len(cells)]},
        },
        "stations": stations,
    }
    nodes = [
        {"id": "fetch:Tomato", "op": "fetch", "label": "Tomato",
         "resource": "dispenser", "ingredient": "Tomato", "duration": 5.0},
        {"id": "cut:ChoppedTomato", "op": "cut", "label": "ChoppedTomato",
         "resource": "container", "duration": 5.0},
        {"id": "cook:CookedTomato", "op": "cook", "label": "CookedTomato",
         "resource": "cooker", "duration": 5.0},
        {"id": "plate:TomatoDish", "op": "plate", "label": "TomatoDish",
         "resource": "plate_stack", "duration": 5.0},
        {"id": "deliver:TomatoDish", "op": "deliver", "label": "TomatoDish",
         "resource": "serving", "duration": 5.0},
        {"id": "wash:CleanPlate", "op": "wash", "label": "CleanPlate",
         "resource": "sink", "duration": 5.0},
    ]
    recipes = {
        "scene": "macro",
        "nodes": nodes,
        "edges": [["fetch:Tomato", "cut:ChoppedTomato"],
                  ["cut:ChoppedTomato", "cook:CookedTomato"],
                  ["cook:CookedTomato", "plate:TomatoDish"],
                  ["plate:TomatoDish", "deliver:TomatoDish"]],
        "dishes": [
            {"dish": "TomatoSoup", "score": 80, "root": "deliver:TomatoDish",
             "nodes": ["fetch:Tomato", "cut:ChoppedTomato", "cook:CookedTomato",
                       "plate:TomatoDish", "deliver:TomatoDish"]},
            {"dish": "CleanPlate", "score": 1, "root": "wash:CleanPlate",
             "nodes": ["wash:CleanPlate"]},
        ],
        "durations": {"fetch": 5.0, "cut": 5.0, "cook": 5.0, "plate": 5.0,
                      "deliver": 5.0, "wash": 5.0},
    }
    orders = {"seed": 7, "level": "macro", "menu": []}
    paths = {}
    for name, payload in (("kitchen.json", kitchen), ("recipes.json", recipes),
                          ("orders.json", orders)):
        path = directory / name
        path.write_text(json.dumps(payload, ensure_ascii=False))
        paths[name] = str(path)
    products = {"container-0": "ChoppedTomato", "cooker-0": "TomatoDish"}
    return paths, cells, stations, products


def _macro_log_rows(path):
    rows = []
    try:
        text = Path(path).read_text(errors="replace")
    except OSError:
        return rows
    for line in text.splitlines():
        match = LOG_LINE.match(line)
        if not match or not match.group(1).startswith("macro_"):
            continue
        event = match.group(1)[len("macro_"):]
        kind = re.search(r"\bmacro=(\w+)", line)
        waited = re.search(r"\bwaited_ms=(\d+)", line)
        detail = re.search(r"\bdetail=(.+)$", line)
        rows.append({"event": event,
                     "macro": kind.group(1) if kind else None,
                     "waited_ms": int(waited.group(1)) if waited else None,
                     "detail": detail.group(1).strip() if detail else ""})
    return rows


def _macro_run(ctx, checks, name, paths, cells, stations, products, *, recipe,
               duration, delay_ms=2000, stalled=()):
    """起一棵 1 厨师的 Supervisor，让 WorldSim 按动作改世界，跑一局宏动作。"""
    args = ctx.args
    ctx.clear_stale_shm()
    port = args.listen_port or pick_free_port(args.host)
    ctx.start_supervisor(port=port, chefs=1, kitchen=paths["kitchen.json"],
                         recipes=paths["recipes.json"], orders=paths["orders.json"])
    if not ctx.wait_supervisor_ready(port, 1, args.ready_timeout):
        raise InfraError("Supervisor tree never reached running state on %s:%d\n%s" %
                         (args.host, port, ctx.diagnostics()))
    checks.note("supervisor_tree_%s" % name,
                "bridge+scheduler+navd+1 worker running")

    sim = WorldSim([(0.0, 0.0)], cells, stations, delay_ms=delay_ms,
                   products=products, resources={"plate_stack": {"count": 0}},
                   orders=[{"id": "macro-1", "recipe": recipe, "time_remaining": 300,
                            "ingredients": []}], wall_delay=True)
    sim.stalled = set(stalled)
    period_ms = 1000.0 / args.hz
    stats = Stats(name)

    def builder(n, frame_index):
        game_ms = int(round(frame_index * period_ms))
        sim.process_actions(stats.actions, game_ms)
        game_ms = sim.tick(game_ms)
        sim.frame(game_ms)
        return sim.message(n, frame_index, cells, game_ms)

    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    try:
        run_exchange(link, stats, duration, chefs=1, hz=args.hz,
                     heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                     state_builder=builder)
    finally:
        link.close()
        time.sleep(0.4)

    worker_log = (ctx.runtime_dir / "logs" / "worker-0.log") if ctx.runtime_dir else None
    rows = _macro_log_rows(worker_log) if worker_log is not None else []
    run_dir = ctx.run_dir()
    events, bad = _read_jsonl(run_dir / "events.jsonl") if run_dir else ([], [])
    code, seconds = ctx.stop_supervisor()
    checks.hard("%s_supervisor_clean" % name, code == 0,
                "exit=%s in %.2fs" % (code, seconds or 0.0))
    checks.hard("%s_events_parse" % name, not bad, "%d bad line(s)" % len(bad))
    return {"stats": stats, "rows": rows, "events": events, "sim": sim,
            "actions": stats.actions}


def scenario_macro(ctx, checks):
    """宏动作按世界状态完成：fake 延迟 2 s 才改 holding/progress/锅 state。

    三局：
      1. 完整一单（fetch→cut→cook→plate→deliver）：每步都要等世界状态变化才
         macro_done，cook 放下即走；
      2. wash：水槽 progress 与干净盘数在 2 s 后才变化；
      3. 料箱不回料：pickup 按 1.5 s 超时 macro_failed，工序 FAILED，detail 带
         宏动作与所等条件。
    """
    workdir = Path(tempfile.mkdtemp(prefix="kitchenos-macro-"))
    checks.note("workdir", str(workdir))
    try:
        paths, cells, stations, products = macro_configs(workdir)

        print("  -- macro: 完整一单")
        soup = _macro_run(ctx, checks, "macro-soup", paths, cells, stations, products,
                          recipe="TomatoSoup", duration=20.0)
        soup_rows = soup["rows"]
        started = {row["macro"] for row in soup_rows
                   if row["event"] == "start" and row["macro"]}
        checks.hard("macro_all_kinds_started",
                    {"goto", "pickup", "drop", "chop", "start_cook", "wait_ready",
                     "deliver"} <= started,
                    "started=%s" % sorted(started))
        done = [row for row in soup_rows if row["event"] == "done"]
        by_kind = {}
        for row in done:
            by_kind.setdefault(row["macro"], []).append(row["waited_ms"] or 0)
        checks.hard("macro_waits_for_holding",
                    max(by_kind.get("pickup", [0])) >= 1800,
                    "pickup waited_ms=%s (fake 延迟 2000 ms)" % by_kind.get("pickup"))
        checks.hard("macro_waits_for_progress",
                    max(by_kind.get("chop", [0])) >= 1800,
                    "chop waited_ms=%s" % by_kind.get("chop"))
        checks.hard("macro_waits_for_pot_state",
                    max(by_kind.get("drop", [0])) >= 1800,
                    "drop waited_ms=%s（锅 state 与 holding 同一次上报才变）"
                    % by_kind.get("drop"))
        checks.hard("macro_cook_leaves_pot",
                    "start_cook" in started and "wait_ready" in started,
                    "cook 只到 start_cook；wait_ready 属于盘工序")
        failed = [row for row in soup_rows if row["event"] == "failed"]
        checks.hard("macro_no_failures_in_success_run", not failed,
                    "macro_failed=%s" % failed[:2])
        soup_events = soup["events"]
        task_done = [e for e in soup_events if e.get("event") == "task_done"]
        completed = [e for e in soup_events if e.get("event") == "order_completed"]
        checks.hard("macro_full_order_done", len(task_done) >= 5 and bool(completed),
                    "task_done=%d order_completed=%s" %
                    (len(task_done), completed[:1] or "none"))
        checks.hard("macro_interacts_sent",
                    sum(1 for a in soup["actions"] if a.get("action") == "interact") >= 3,
                    "interacts=%d" % sum(1 for a in soup["actions"]
                                        if a.get("action") == "interact"))
        checks.hard("macro_no_move_action",
                    not any(a.get("action") == "move" for a in soup["actions"]),
                    "system path must only send path/face/work/interact")

        print("  -- macro: wash 等水槽进度")
        wash = _macro_run(ctx, checks, "macro-wash", paths, cells, stations, products,
                          recipe="CleanPlate", duration=8.0)
        wash_rows = wash["rows"]
        wash_started = {row["macro"] for row in wash_rows if row["event"] == "start"}
        wash_done = [row for row in wash_rows
                     if row["event"] == "done" and row["macro"] == "wash"]
        checks.hard("macro_wash_started", "wash" in wash_started,
                    "started=%s" % sorted(wash_started))
        checks.hard("macro_wash_waits_for_progress",
                    bool(wash_done) and wash_done[0]["waited_ms"] >= 1800,
                    "wash done=%s" % wash_done[:1])
        checks.hard("macro_wash_no_failures",
                    not any(row["event"] == "failed" for row in wash_rows),
                    "macro_failed=%s" % [r for r in wash_rows if r["event"] == "failed"][:2])

        print("  -- macro: 料箱不回料 → pickup 超时")
        stalled = _macro_run(ctx, checks, "macro-fail", paths, cells, stations,
                             products, recipe="TomatoSoup", duration=5.0,
                             stalled=("dispenser-0",))
        fail_rows = stalled["rows"]
        failures = [row for row in fail_rows if row["event"] == "failed"]
        task_failed = [e for e in stalled["events"] if e.get("event") == "task_failed"]
        detail = task_failed[0].get("detail", "") if task_failed else ""
        checks.hard("macro_pickup_times_out", bool(failures),
                    "macro_failed=%s" % failures[:2])
        checks.hard("macro_failed_detail_names_macro_and_condition",
                    "macro=pickup" in detail and "wait=holding" in detail,
                    "task_failed detail=%r" % detail)
        failed_t = task_failed[0].get("t_ms") if task_failed else None
        checks.hard("macro_failure_before_scheduler_timeout",
                    failed_t is not None and failed_t < 10000,
                    "task_failed t_ms=%s (1.5 s 超时，不是 30 s)" % failed_t)
        checks.hard("macro_fail_run_no_done_pickup",
                    not any(row["event"] == "done" and row["macro"] == "pickup"
                            for row in fail_rows),
                    "done=%s" % [r for r in fail_rows if r["event"] == "done"][:3])
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return "soup/wash 两局世界状态延迟 2 s 后宏动作完成；不回料一局 pickup 超时 FAILED"


def scenario_os_loop(ctx, checks):
    """整棵 Supervisor 树的闭环：plugin → Bridge → shm → Scheduler → mq → Worker → 槽 → plugin。

    这个场景不启动游戏：fake_plugin 提供状态与订单，Supervisor 拉起 Bridge、
    Scheduler 与两个 Worker。断言分三层：
      * 调度器把任务发到了每个厨师（events.jsonl 的 task_dispatched）；
      * Worker 的输入槽经 Bridge 变成对端的 path/interact，系统路径不再发 move；
      * SIGHUP 之后 decisions.jsonl 的政策名从 FCFS 变成 EDF。
    """
    args = ctx.args
    chefs = args.chefs or 2
    ctx.clear_stale_shm()
    port = args.listen_port or pick_free_port(args.host)
    ctx.start_supervisor(port=port, chefs=chefs)
    if not ctx.wait_supervisor_ready(port, chefs, args.ready_timeout):
        raise InfraError("Supervisor tree never reached running state on %s:%d\n%s" %
                         (args.host, port, ctx.diagnostics()))
    checks.note("supervisor_tree", "bridge+scheduler+%d workers running" % chefs)

    try:
        kitchen = json.loads(Path(args.kitchen).read_text())
        cells = kitchen["grid"]["cells"]
    except (OSError, ValueError, KeyError) as exc:
        raise InfraError("cannot read kitchen grid from %s: %s" % (args.kitchen, exc))
    picks = [cells[0], cells[-1], cells[len(cells) // 2], cells[len(cells) // 4]]
    starts = [tuple(picks[i % len(picks)]) for i in range(chefs)]
    sim = WorldSim(starts, cells, kitchen.get("stations", []),
                   resources={"plate_stack": {"count": 5}},
                   orders=[{"id": "world-001", "recipe": "SteamedSpecial_Fish",
                            "time_remaining": 90, "ingredients": ["fish"]},
                           {"id": "world-002", "recipe": "SteamedSpecial_Meat",
                            "time_remaining": 90, "ingredients": ["meat"]}])
    period_ms = 1000.0 / args.hz

    def builder(n, frame_index):
        game_ms = int(round(frame_index * period_ms))
        sim.process_actions(stats.actions, game_ms)
        game_ms = sim.tick(game_ms)
        sim.frame(game_ms)
        return sim.message(n, frame_index, cells, game_ms)

    link = Link(connect(args.host, port, args.ready_timeout))
    link.send_json(msg_hello())
    stats = Stats("os-loop")

    pre = min(2.0, max(1.0, args.os_duration * 0.3))
    run_exchange(link, stats, pre, chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                 state_builder=builder)
    if ctx.supervisor is None:
        raise InfraError("supervisor died during first half")
    os.kill(ctx.supervisor.pid, signal.SIGHUP)
    checks.note("sighup_sent_at_s", "%.1f" % pre)

    # Kill a Worker while a task is in flight; the Supervisor restarts it and the
    # Scheduler must requeue the lost task on the Worker's RECOVERED event.
    # Kill after the first station interaction (chef0's fetch takes ~4.8 s of travel)
    # but while the follow-up task is in flight; the recovery requeue then has a
    # RUNNING task to move back to PENDING.
    mid = 4.0 if args.os_duration > pre + 5.5 else 1.0
    run_exchange(link, stats, mid, chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                 state_builder=builder)
    status = ctx.supervisor_status()
    killed_chef = 0
    killed_pid = next((c.get("pid") for c in status.get("children", [])
                       if c.get("role") == "worker-%d" % killed_chef), None)
    if not killed_pid:
        raise InfraError("no running worker-%d in status.json: %s" % (killed_chef, status))
    os.kill(killed_pid, signal.SIGKILL)
    checks.note("worker_killed", "chef=%d pid=%s at ~%.1fs" %
                (killed_chef, killed_pid, pre + mid))
    tail = max(0.5, args.os_duration - pre - mid)
    run_exchange(link, stats, tail, chefs=chefs, hz=args.hz,
                 heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                 state_builder=builder)
    link.close()
    time.sleep(0.5)

    run_dir = ctx.run_dir()
    decisions = []
    events = []
    bad_lines = []
    if run_dir is None:
        checks.hard("runs_dir_created", False, "no <runtime>/runs/* directory")
    else:
        checks.hard("runs_dir_created", True, str(run_dir))
        decisions, bad_d = _read_jsonl(run_dir / "decisions.jsonl")
        events, bad_e = _read_jsonl(run_dir / "events.jsonl")
        bad_lines = bad_d + bad_e
        checks.hard("runs_files_exist",
                    (run_dir / "decisions.jsonl").exists() and
                    (run_dir / "events.jsonl").exists(),
                    "%s" % run_dir)
    checks.hard("decisions_parse_as_json", not bad_lines,
                "%d bad line(s): %s" % (len(bad_lines), bad_lines[:2] or "none"))
    checks.hard("decisions_nonempty", len(decisions) > 0, "%d lines" % len(decisions))

    dispatch = [d for d in decisions if d.get("kind") == "dispatch"]
    dispatchers = {d.get("chef_id") for d in dispatch}
    checks.hard("scheduler_dispatched_each_chef",
                set(range(chefs)) <= dispatchers,
                "dispatch chefs=%s expected 0..%d (%d dispatch lines)" %
                (sorted(dispatchers), chefs - 1, len(dispatch)))

    policies = Counter(d.get("policy") for d in dispatch)
    checks.hard("decisions_name_fcfs", policies.get("FCFS", 0) > 0,
                "dispatch policies=%s" % dict(policies))
    switch_positions = [i for i, d in enumerate(decisions)
                        if d.get("kind") == "strategy" and d.get("to") == "EDF"]
    edf_after = [i for i, d in enumerate(decisions)
                 if d.get("policy") == "EDF" and switch_positions and i > switch_positions[0]]
    checks.hard("sighup_flips_policy",
                bool(switch_positions) and bool(edf_after),
                "strategy lines=%s; EDF decisions after switch=%s" %
                (switch_positions, edf_after[:3]))
    checks.hard("sighup_flip_logged_in_events",
                any(e.get("event") == "strategy_switch" and e.get("to") == "EDF"
                    for e in events),
                "events strategy_switch=%s" %
                [e for e in events if e.get("event") == "strategy_switch"][:2])

    task_events = [e for e in events if e.get("event") == "task_dispatched"]
    checks.hard("scheduler_events_parse_as_json", not bad_lines,
                "%d bad line(s)" % len(bad_lines))
    checks.hard("task_events_each_chef",
                {e.get("chef_id") for e in task_events} >= set(range(chefs)),
                "task chefs=%s (%d dispatched)" %
                (sorted({e.get("chef_id") for e in task_events}), len(task_events)))

    checks.hard("actions_wellformed", not stats.bad_actions,
                "%d malformed: %s" % (len(stats.bad_actions),
                                      stats.bad_actions[:2] or "none"))
    moves = Counter(a.get("chef_id") for a in stats.actions if a.get("action") == "move")
    paths = Counter(a.get("chef_id") for a in stats.actions if a.get("action") == "path")
    interacts = Counter(a.get("chef_id") for a in stats.actions
                        if a.get("action") == "interact")
    checks.hard("worker_never_sends_move", sum(moves.values()) == 0,
                "move per chef=%s" % dict(moves))
    checks.hard("worker_path_forwarded_each_chef",
                all(paths.get(c, 0) >= 3 for c in range(chefs)),
                "paths per chef=%s" % dict(paths))
    checks.hard("worker_interact_forwarded_each_chef",
                all(interacts.get(c, 0) >= 1 for c in range(chefs)),
                "interacts per chef=%s" % dict(interacts))
    checks.hard("no_same_cell_frames", not sim.collisions,
                "collisions=%s" % (sim.collisions[:3] or "none"))

    recovered = [e for e in events if e.get("event") == "worker_recovered" and
                 e.get("chef_id") == killed_chef]
    requeued = [e for e in events if e.get("event") == "task_requeued" and
                e.get("chef_id") == killed_chef]
    first_recovery_t = recovered[0].get("t_ms") if recovered else None
    post_recovery_dispatch = [e for e in events
                              if e.get("event") == "task_dispatched" and
                              e.get("chef_id") == killed_chef and
                              first_recovery_t is not None and
                              (e.get("t_ms") or 0) >= first_recovery_t]
    checks.hard("worker_restart_detected", bool(recovered),
                "worker_recovered chef=%d: %s" % (killed_chef, recovered[:1] or "none"))
    checks.hard("worker_restart_requeues_task", bool(requeued),
                "task_requeued: %s" % (requeued[:2] or "none"))
    checks.hard("worker_restart_gets_new_task", bool(post_recovery_dispatch),
                "dispatch after recovery: %s" % (post_recovery_dispatch[:2] or "none"))
    status = ctx.supervisor_status()
    worker_status = next((c for c in status.get("children", [])
                          if c.get("role") == "worker-%d" % killed_chef), {})
    checks.hard("worker_process_restarted", (worker_status.get("spawns") or 0) >= 2,
                "worker-%d status=%s" % (killed_chef, worker_status))
    checks.hard("supervisor_alive_during_run",
                ctx.supervisor is not None and ctx.supervisor.alive(),
                "exit=%s" % (ctx.supervisor.exit_code() if ctx.supervisor else "gone"))
    checks.hard("bridge_actions_per_chef",
                stats.per_chef() and set(stats.per_chef()) >= set(range(chefs)),
                "%s" % stats.describe())

    code, seconds = ctx.stop_supervisor()
    checks.hard("supervisor_clean_shutdown", code == 0,
                "exit=%s in %.2fs" % (code, seconds or 0.0))
    pidfile = (ctx.runtime_dir / "kitchend.pid") if ctx.runtime_dir else None
    checks.hard("supervisor_removes_pidfile",
                pidfile is not None and not pidfile.exists(), "%s" % pidfile)
    leftovers = [p.name for p in Path("/dev/shm").glob(ctx.state_name.lstrip("/") + "*")]
    checks.hard("supervisor_unlinks_shm", not leftovers, ", ".join(leftovers) or "none")

    return ("%s; dispatch=%d chefs=%s; policies=%s; moves=%s; interacts=%s; run_dir=%s" %
            (stats.describe(), len(dispatch), sorted(dispatchers), dict(policies),
             dict(moves), dict(interacts), run_dir))


def _resource_holder(events, chef):
    """最近一次 acquire 之后没有被 released/reclaimed 的 task_id。"""
    held = {}
    for event in events:
        if event.get("event") == "resource_acquired" and event.get("chef_id") == chef:
            held[event.get("task_id")] = event
        elif event.get("event") in ("resource_released", "resource_reclaimed"):
            held.pop(event.get("task_id"), None)
    return list(held.values())[-1] if held else None


def scenario_os_resources(ctx, checks):
    """一个 cooker、两个厨师：争用、租约到期、持租约 Worker 被 kill 后回收。

    先造一个只有单个 cooker 工位的 kitchen 和只含 SteamedSpecial_Fish 的订单，
    让两个厨师必然抢同一台锅。租约 TTL 压到 5 秒，使 5 秒的 cook 节点跨过到期点；
    在确认 worker-0 仍持租约时 SIGKILL 它，Supervisor 重启后用 RECOVERED 触发
    reclaim；另一个厨师的租约则由 Scheduler 的到期扫描回收。最后停整棵树，核对
    Supervisor 在 unlink 前记录的 sem value、metrics.json 与 /dev/shm 残留。
    """
    args = ctx.args
    tmp = Path(tempfile.mkdtemp(prefix="kitchenos-resources-"))
    try:
        kitchen = json.loads(Path(args.kitchen).read_text())
        cookers = [st for st in kitchen.get("stations", []) if st.get("kind") == "cooker"]
        if not cookers:
            raise InfraError("kitchen %s has no cooker station" % args.kitchen)
        kitchen["stations"] = [st for st in kitchen["stations"]
                               if st.get("kind") != "cooker" or st is cookers[0]]
        kitchen_path = tmp / "kitchen.json"
        kitchen_path.write_text(json.dumps(kitchen))
        orders_path = tmp / "orders.json"
        orders_path.write_text(json.dumps({
            "seed": 7,
            "level": kitchen.get("scene", "s_city_h5"),
            "menu": [{"recipes": ["SteamedSpecial_Fish"]}],
        }))
        args.kitchen = str(kitchen_path)
        args.orders = str(orders_path)

        ctx.clear_stale_shm()
        port = args.listen_port or pick_free_port(args.host)
        ctx.start_supervisor(port=port, chefs=2, lease_ttl_ms=8000)
        if not ctx.wait_supervisor_ready(port, 2, args.ready_timeout):
            raise InfraError("Supervisor tree never ready\n%s" % ctx.diagnostics())
        checks.note("single_cooker", "capacity=1, chefs=2, lease_ttl_ms=8000")

        cells = kitchen["grid"]["cells"]
        builder = lambda n, idx: msg_state_update_os(n, idx, cells)
        link = Link(connect(args.host, port, args.ready_timeout))
        link.send_json(msg_hello())
        stats = Stats("os-resources")

        run_exchange(link, stats, 5.0, chefs=2, hz=args.hz,
                     heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                     state_builder=builder)
        run_dir = ctx.run_dir()
        events = []
        if run_dir is not None:
            events, _ = _read_jsonl(run_dir / "events.jsonl")

        # SIGUSR1 的 metrics 快照必须在不关机时就落盘，且带 reason=sigusr1。
        status = ctx.supervisor_status()
        scheduler_pid = next((c.get("pid") for c in status.get("children", [])
                              if c.get("role") == "scheduler"), None)
        # 通过 Supervisor 转发，验证操作者只需要知道整棵树的 pid。
        os.kill(ctx.supervisor.pid, signal.SIGUSR1)
        sig_metrics = {}
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if run_dir is not None:
                try:
                    sig_metrics = json.loads((run_dir / "metrics.json").read_text())
                except (OSError, ValueError):
                    sig_metrics = {}
                if sig_metrics.get("reason") == "sigusr1":
                    break
            run_exchange(link, stats, 0.2, chefs=2, hz=args.hz,
                         heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                         state_builder=builder)
        checks.hard("metrics_sigusr1_snapshot", sig_metrics.get("reason") == "sigusr1",
                    "scheduler_pid=%s reason=%s" %
                    (scheduler_pid, sig_metrics.get("reason")))

        holder = _resource_holder(events, 0)
        waited = 0.0
        # 等到 cooker 租约再杀，证明确实是"持锅时被 kill"；超时则退化为任意租约。
        while (holder is None or holder.get("kind") != "cooker") and waited < 6.0:
            run_exchange(link, stats, 0.5, chefs=2, hz=args.hz,
                         heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                         state_builder=builder)
            waited += 0.5
            if run_dir is not None:
                events, _ = _read_jsonl(run_dir / "events.jsonl")
                holder = _resource_holder(events, 0)
        checks.hard("worker_killed_while_holding_lease", holder is not None,
                    "held=%s after %.1fs extra" % (holder, waited))
        checks.note("killed_lease_kind", "%s" % (holder.get("kind") if holder else "none"))
        status = ctx.supervisor_status()
        killed_pid = next((c.get("pid") for c in status.get("children", [])
                           if c.get("role") == "worker-0"), None)
        if not killed_pid:
            raise InfraError("no running worker-0 in status.json: %s" % status)
        os.kill(killed_pid, signal.SIGKILL)
        killed_task = holder.get("task_id") if holder else None
        killed_kind = holder.get("kind") if holder else None
        checks.note("worker_killed", "chef=0 pid=%s task=%s kind=%s" %
                    (killed_pid, killed_task, killed_kind))

        run_exchange(link, stats, 10.0, chefs=2, hz=args.hz,
                     heartbeat_ms=args.heartbeat_ms, ack_actions=True,
                     state_builder=builder)
        link.close()
        time.sleep(0.5)

        code, seconds = ctx.stop_supervisor()
        checks.hard("supervisor_clean_shutdown", code == 0,
                    "exit=%s in %.2fs" % (code, seconds or 0.0))

        run_dir = ctx.run_dir()
        if run_dir is None:
            checks.hard("run_dir_layout", False, "no run dir")
            return "no run dir"
        events, bad_e = _read_jsonl(run_dir / "events.jsonl")
        decisions, bad_d = _read_jsonl(run_dir / "decisions.jsonl")
        checks.hard("run_dir_layout",
                    (run_dir / "events.jsonl").exists() and
                    (run_dir / "decisions.jsonl").exists() and
                    (run_dir / "metrics.json").exists(),
                    "%s" % run_dir)
        checks.hard("run_files_parse_as_json", not (bad_e + bad_d),
                    "%d bad lines" % len(bad_e + bad_d))

        acquired = [e for e in events if e.get("event") == "resource_acquired"]
        released = [e for e in events if e.get("event") == "resource_released"]
        reclaimed = [e for e in events if e.get("event") == "resource_reclaimed"]
        expired = [e for e in events if e.get("event") == "lease_expired"]
        waits = [e for e in events if e.get("event") == "resource_wait"]
        checks.hard("resource_acquired_events", bool(acquired), "%d acquired" % len(acquired))
        checks.hard("resource_released_events", bool(released), "%d released" % len(released))
        checks.hard("resource_reclaimed_events", bool(reclaimed),
                    "%d reclaimed: %s" % (len(reclaimed), reclaimed[:2]))
        checks.hard("lease_expired_event", bool(expired), "%d expired" % len(expired))
        checks.hard("contention_observed", bool(waits),
                    "wait events=%s" % waits[:2])
        checks.hard("killed_lease_reclaimed_by_worker_death",
                    killed_task is not None and
                    any(e.get("task_id") == killed_task and
                        e.get("reason") == "worker-recovered" for e in reclaimed),
                    "task=%s reclaimed=%s" % (killed_task, reclaimed[:3]))
        checks.hard("other_chef_acquires_after_reclaim",
                    any(e.get("kind") == "cooker" and e.get("chef_id") != 0
                        for e in acquired),
                    "cooker acquisitions=%s" %
                    [(e.get("chef_id"), e.get("task_id")) for e in acquired
                     if e.get("kind") == "cooker"])
        recovered = [e for e in events if e.get("event") == "worker_recovered" and
                     e.get("chef_id") == 0]
        checks.hard("worker_restart_detected", bool(recovered),
                    "worker_recovered=%s" % recovered[:1])

        metrics_path = run_dir / "metrics.json"
        metrics = {}
        try:
            metrics = json.loads(metrics_path.read_text())
        except (OSError, ValueError) as exc:
            checks.hard("metrics_fields_complete", False, "read %s: %s" % (metrics_path, exc))
        else:
            resources = {r.get("kind"): r for r in metrics.get("resources", [])}
            cooker = resources.get("cooker", {})
            per_order = metrics.get("orders", {}).get("per_order", [])
            complete = (
                metrics.get("schema") == "kitchenos.metrics.v1" and
                "orders" in metrics and "resources" in metrics and
                "dispatch" in metrics and
                "worker_restarts" in metrics and
                cooker.get("capacity") == 1 and
                cooker.get("in_use") == 0 and
                per_order and all("lateness_ms" in o and "finished_ms" in o
                                  for o in per_order))
            checks.hard("metrics_fields_complete", complete,
                        "cooker=%s orders=%d dispatch=%s restarts=%s" %
                        (cooker, len(per_order), metrics.get("dispatch"),
                         metrics.get("worker_restarts")))
            checks.hard("metrics_worker_restarts", (metrics.get("worker_restarts") or 0) >= 1,
                        "worker_restarts=%s" % metrics.get("worker_restarts"))
            checks.hard("metrics_resource_wait",
                        (cooker.get("waits") or 0) > 0 and (cooker.get("wait_ms") or 0) > 0,
                        "cooker waits=%s wait_ms=%s" %
                        (cooker.get("waits"), cooker.get("wait_ms")))

        supervisor_log = ctx.runtime_dir / "logs" / "supervisor.log"
        values = []
        try:
            for line in supervisor_log.read_text(errors="replace").splitlines():
                if "resource_values" in line:
                    values.append(line)
        except OSError:
            pass
        cooker_values = [line for line in values if "kind=cooker" in line]
        checks.hard("resource_values_logged_before_unlink", bool(cooker_values),
                    "%s" % (cooker_values[-1:] or "none"))
        checks.hard("sem_value_back_to_capacity",
                    bool(cooker_values) and "value=1 capacity=1" in cooker_values[-1],
                    "%s" % (cooker_values[-1:] or "none"))
        leftovers = sorted(p.name for p in Path("/dev/shm").glob("sem.kitchenos*"))
        checks.hard("no_semaphore_left_after_stop", not leftovers,
                    ", ".join(leftovers) or "none")

        actions = len(stats.actions)
        checks.hard("bridge_actions_flowing", actions > 0, "%d actions" % actions)
        return ("single cooker; acquired=%d released=%d reclaimed=%d expired=%d "
                "waits=%d killed_task=%s; metrics=%s; actions=%d; run_dir=%s" %
                (len(acquired), len(released), len(reclaimed), len(expired),
                 len(waits), killed_task, metrics.get("dispatch"), actions, run_dir))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


SCENARIO_IMPL = {
    "happy": scenario_happy,
    "one-slot": scenario_one_slot,
    "no-heartbeat": scenario_no_heartbeat,
    "bad-frames": scenario_bad_frames,
    "connect-mode": scenario_connect_mode,
    "reconnect-storm": scenario_reconnect_storm,
    "silent-connect": scenario_silent_connect,
    "data-shapes": scenario_data_shapes,
    "second-peer": scenario_second_peer,
    "pickup-dash": scenario_pickup_dash,
    "path-face-work": scenario_path_face_work,
    "input-grace": scenario_input_grace,
    "os-loop": scenario_os_loop,
    "os-resources": scenario_os_resources,
    "nav-corridor": scenario_nav_corridor,
    "nav-stuck": scenario_nav_stuck,
    "nav-crash": scenario_nav_crash,
    "macro": scenario_macro,
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

    # 1) 编码布局必须与 TcpLink.cs 一致：UTF-8 + 一个 '\n'
    payload = '{"type":"heartbeat","timestamp":1}'
    raw = encode_frame(payload)
    bits = payload.encode("utf-8")
    checks.hard("encode_layout",
                raw == bits + b"\n" and raw[-1:] == b"\n",
                "%d bytes, tail=%s" % (len(raw), raw[-4:].hex()))

    utf8 = "订单-001 汤 🍲"
    frame = encode_frame(utf8)
    checks.hard("line_length_counts_bytes_not_chars",
                frame == utf8.encode("utf-8") + b"\n"
                and len(utf8.encode("utf-8")) != len(utf8),
                "chars=%d bytes=%d" % (len(utf8), len(utf8.encode("utf-8"))))

    try:
        encode_frame('{"a":"b\nc"}')
        newline_ok = False
    except FramingError:
        newline_ok = True
    checks.hard("encode_refuses_raw_newline_in_payload", newline_ok,
                "a payload with a bare '\\n' cannot be framed and must not be sent")

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

    # 5) 半行：只留在缓冲区，不吐半条消息
    def truncate(conn):
        conn.sendall(encode_frame(small) + b"{" * 40)
        time.sleep(0.6)

    port, thread, errors = _start_peer(truncate)
    link = Link(connect("127.0.0.1", port, 5.0))
    got = []
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        got.extend(link.pump(0.1))
    pending = link.dec.pending
    checks.hard("truncated_line_stays_pending",
                len(got) == 1 and pending == 40,
                "frames=%d pending_bytes=%d (expected 40)" % (len(got), pending))
    link.close()
    thread.join(timeout=5)

    # 6) 上限的边界：没有换行符时只有"超过"才算失步，正好等于上限必须继续等
    #    （否则一条刚好 1 MiB 的合法消息会被判成垃圾）；有换行符时超过上限同样是失步。
    dec = FrameDecoder()
    dec.feed(b"x" * MAX_FRAME_BYTES)
    checks.hard("unterminated_line_at_cap_waits", dec.next_payload() is None,
                "%d bytes with no '\\n' may still be arriving" % MAX_FRAME_BYTES)

    dec = FrameDecoder()
    dec.feed(b"x" * (MAX_FRAME_BYTES + 1))
    try:
        dec.next_payload()
        oversize_ok = False
    except FramingError:
        oversize_ok = True
    checks.hard("unterminated_oversize_line_rejected", oversize_ok,
                "%d bytes with no '\\n'" % (MAX_FRAME_BYTES + 1))

    dec = FrameDecoder()
    dec.feed(b"x" * (MAX_FRAME_BYTES + 1) + b"\n")
    try:
        dec.next_payload()
        long_line_ok = False
    except FramingError:
        long_line_ok = True
    checks.hard("terminated_oversize_line_rejected", long_line_ok,
                "%d bytes + '\\n'" % (MAX_FRAME_BYTES + 1))

    # 7) §5.2 的 state_update 字段集必须逐字对得上
    state = json.loads(msg_state_update(2, 7))
    chefs_keys = set(state["chefs"][0]) if state.get("chefs") else set()
    checks.hard("state_update_top_level_keys",
                set(state) == {"type", "timestamp", "game_time", "game_time_ms", "score", "chefs",
                               "orders", "resources"},
                "keys=%s" % sorted(state))
    checks.hard("state_update_chef_shape",
                chefs_keys == {"id", "position", "facing", "holding", "state"}
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
                hello == {"type": "hello", "instance": "windows-host", "version": "0.2.0",
                          "actions": ["move", "stop", "press", "release", "tap",
                                      "pickup", "dash", "interact", "path", "face", "work"]},
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
                            str(REPO_ROOT / "bridge" / "build" / "kitchenos-bridge")),
                        help="kitchenos-bridge 路径（或设 $KITCHENOS_BRIDGE）")
    parser.add_argument("--worker-stub",
                        default=os.environ.get(
                            "KITCHENOS_WORKER_STUB",
                            str(REPO_ROOT / "bridge" / "build" / "kitchenos-worker-stub")),
                        help="kitchenos-worker-stub 路径（或设 $KITCHENOS_WORKER_STUB）")
    parser.add_argument("--supervisor", default=os.environ.get("KITCHENOS_SUPERVISOR"),
                        help="kitchenos-supervisor 路径（默认取 --bridge 同目录）")
    parser.add_argument("--kitchen", default=str(REPO_ROOT / "config" / "kitchen.example.json"),
                        help="os-loop 的 kitchen.json")
    parser.add_argument("--recipes", default=str(REPO_ROOT / "config" / "recipes.example.json"),
                        help="os-loop 的 recipes.json")
    parser.add_argument("--orders", default=str(REPO_ROOT / "config" / "orders.example.json"),
                        help="os-loop 的 orders.json")
    parser.add_argument("--os-duration", type=float, default=10.0,
                        help="os-loop 的运行时长（秒）")
    parser.add_argument("--host", default="127.0.0.1", help="listen 模式为绑定地址，connect 模式为拨号目标")
    parser.add_argument("--listen-port", type=int, default=None,
                        help="listen 模式端口；默认由内核挑一个空闲端口，"
                             "要复现协议默认值就传 14455")
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
    parser.add_argument("--storm-cycles", type=int, default=24,
                        help="reconnect-storm 的断开重连轮数（至少 20 才有意义）")
    parser.add_argument("--storm-gap-ms", type=int, default=40,
                        help="reconnect-storm 每轮之间的间隔；0 = 尽量快，全挤在一个 poll 窗口里")
    parser.add_argument("--silent-cycles", type=int, default=8,
                        help="silent-connect 的\"连上就断\"轮数")
    parser.add_argument("--no-ack", action="store_true", help="不回 ack（模拟插件不回执）")
    parser.add_argument("--flat-xy", action="store_true",
                        help="state_update 额外带顶层 x/y（兼容扁平解析）")
    parser.add_argument("--strict", action="store_true", help="把 soft 检查升级为硬断言")
    parser.add_argument("--require-echo-reply", action="store_true",
                        help="把 echo_reply 往返当硬断言（spec 未要求 Bridge 回，默认只记录）")
    parser.add_argument("--require-bridge-ack", action="store_true",
                        help="把 Bridge 主动发 ack 当硬断言（spec 未要求，默认只记录）")
    args = parser.parse_args(argv)
    if not args.supervisor:
        args.supervisor = str(Path(args.bridge).with_name("kitchenos-supervisor"))
    return args


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
            ctx.keep_runtime = bool(checks.failures) or status != EXIT_OK
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
