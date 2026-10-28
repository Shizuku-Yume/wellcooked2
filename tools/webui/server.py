#!/usr/bin/env python3
"""KitchenOS 浏览器控制台：启动一局、实时看板、切策略、杀 Scheduler、结果对比。

    python3 tools/webui/server.py [--port 8765]

只用标准库，只监听 127.0.0.1，没有登录：本机任何用户都能开局/发信号，别转发端口。
实时数据走 SSE（/api/stream，每 500 ms 一帧）。数据全部来自 run-real-game.sh 的运行目录：
live.json（kitchenos-monitor）、status.json（Supervisor）、policy.json（Scheduler）、
runs/<run>/events.jsonl|decisions.jsonl|metrics.json、summary.txt。
"""
import argparse
import collections
import json
import os
import re
import signal
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATIC = Path(__file__).resolve().parent / "static"
ORDERS = ("fcfs", "sjf", "llf", "hrrn", "edf")
TASKS = ("cost", "fifo", "cp")
DISPATCH = ("flexible", "zoned")
RUN_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
SCENE_NAME = re.compile(r"^[A-Za-z0-9_]{1,64}$")
STATIC_TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
                ".js": "application/javascript; charset=utf-8", ".svg": "image/svg+xml"}
FEED_KEEP = 60
OUTPUT_KEEP = 400


def read_json(path, default=None):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def latest_subdir(path):
    try:
        dirs = [p for p in path.iterdir() if p.is_dir()]
    except OSError:
        return None
    return max(dirs, key=lambda p: p.stat().st_mtime, default=None)


class JsonlTail:
    """增量读一个 .jsonl：只读新增字节，保留最后 keep 条解析过的记录。"""

    def __init__(self, keep=FEED_KEEP):
        self.path = None
        self.offset = 0
        self.partial = b""
        self.items = collections.deque(maxlen=keep)

    def poll(self, path):
        if path != self.path:
            self.path, self.offset, self.partial = path, 0, b""
            self.items.clear()
        if path is None:
            return list(self.items)
        try:
            with open(path, "rb") as f:
                f.seek(self.offset)
                chunk = f.read(1 << 20)
        except OSError:
            return list(self.items)
        self.offset += len(chunk)
        lines = (self.partial + chunk).split(b"\n")
        self.partial = lines.pop()
        for line in lines:
            try:
                self.items.append(json.loads(line))
            except ValueError:
                pass
        return list(self.items)


def scheduler_pid(rt):
    """status.json 里活着的 Scheduler pid；再用 /proc 核对可执行名，防 pid 复用误杀。"""
    status = read_json(rt / "status.json", {}) or {}
    for child in status.get("children", []):
        if child.get("role") == "scheduler" and child.get("state") == "running":
            pid = int(child.get("pid") or 0)
            try:
                cmd = Path("/proc/%d/cmdline" % pid).read_bytes()
            except OSError:
                return 0
            return pid if pid > 0 and b"kitchenos-scheduler" in cmd else 0
    return 0


class App:
    def __init__(self, script, logs_dir):
        self.script = Path(script)
        self.logs = Path(logs_dir)
        self.lock = threading.Lock()
        self.proc = None
        self.rt = None
        self.params = None
        self.started_at = None
        self.output = collections.deque(maxlen=OUTPUT_KEEP)
        self.events = JsonlTail()
        self.decisions = JsonlTail()
        self.notes = collections.deque(maxlen=20)
        self.cond = threading.Condition()
        self.frame = "{}"
        self.frame_seq = 0

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def note(self, text):
        self.notes.append({"t": time.time(), "text": text})

    def start(self, params):
        with self.lock:
            if self.running():
                return False
            env = os.environ.copy()
            env.update({"SCENE": params["scene"], "CHEFS": str(params["chefs"]),
                        "STRATEGY": params["strategy"], "TASK_POLICY": params["task_policy"],
                        "DISPATCH": params["dispatch"],
                        "GAME_SECONDS": str(params["game_seconds"])})
            self.output.clear()
            self.notes.clear()
            self.rt = None
            self.params = params
            self.started_at = time.time()
            self.proc = subprocess.Popen(["bash", str(self.script)], cwd=str(ROOT), env=env,
                                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT)
            threading.Thread(target=self._watch, args=(self.proc,), daemon=True).start()
            self.note("开局：%s %d 人 order=%s task=%s" % (
                params["scene"], params["chefs"], params["strategy"], params["task_policy"]))
            return True

    def _watch(self, proc):
        for raw in proc.stdout:
            line = raw.decode("utf-8", "replace").rstrip()
            self.output.append(line)
            m = re.search(r"runtime: (\S.*)$", line)
            if m:
                self.rt = Path(m.group(1).strip())
        proc.wait()
        self.note("脚本结束，退出码 %d" % proc.returncode)

    def stop(self):
        with self.lock:
            if not self.running():
                return False
            # run-real-game.sh 自己 trap TERM：停 Supervisor/monitor、还原配置、放锁。
            self.proc.send_signal(signal.SIGTERM)
            self.note("已请求停止本局")
            return True

    def current_run_dir(self):
        return latest_subdir(self.rt / "runs") if self.rt else None

    def snapshot(self):
        rt = self.rt
        run = self.current_run_dir()
        data = {
            "running": self.running(),
            "exit_code": None if self.proc is None or self.running() else self.proc.returncode,
            "params": self.params,
            "started_at": self.started_at,
            "runtime": rt.name if rt else None,
            "output": list(self.output)[-40:],
            "notes": list(self.notes),
            "events": self.events.poll(run / "events.jsonl" if run else None),
            "decisions": self.decisions.poll(run / "decisions.jsonl" if run else None),
        }
        if rt:
            data["live"] = read_json(rt / "live.json")
            data["status"] = read_json(rt / "status.json")
            data["policy"] = read_json(rt / "policy.json")
            data["metrics"] = read_json(run / "metrics.json") if run else None
        return data

    def refresh_metrics(self):
        """Scheduler 只在关机和 SIGUSR1 时写 metrics.json；局中定期发 SIGUSR1，实时面板才有数。"""
        rt = self.rt
        if rt is None or not self.running():
            return
        pid = scheduler_pid(rt)
        if pid > 0:
            try:
                os.kill(pid, signal.SIGUSR1)
            except OSError:
                pass

    def poll_forever(self, interval=0.5, metrics_every=4):
        """唯一的读盘线程：每 interval 秒刷新一次快照，SSE 连接只读缓存，几个标签页都不多读。"""
        tick = 0
        while True:
            if tick % metrics_every == 0:
                self.refresh_metrics()
            tick += 1
            try:
                text = json.dumps(self.snapshot(), ensure_ascii=False)
            except Exception as exc:  # 读盘出错不能让推送线程死掉
                text = json.dumps({"running": self.running(), "error": str(exc)})
            with self.cond:
                self.frame = text
                self.frame_seq += 1
                self.cond.notify_all()
            time.sleep(interval)

    def wait_frame(self, last_seq, timeout=5.0):
        with self.cond:
            self.cond.wait_for(lambda: self.frame_seq != last_seq, timeout=timeout)
            return self.frame_seq, self.frame


SUMMARY_PATTERNS = {
    "score": re.compile(r"^score=([-\d.]+)", re.M),
    "orders": re.compile(r"^orders: (\d+) arrived, (\d+) completed", re.M),
    "result": re.compile(r"^RESULT=(\w+)", re.M),
    "policy": re.compile(r"^policy: order=(\S+) task=(\S+) dispatch=(\S+)", re.M),
    "header": re.compile(r"scene=(\S+) chefs=(\d+)"),
}


def parse_summary(text):
    out = {"score": None, "arrived": None, "completed": None, "result": None,
           "order": None, "task": None, "dispatch": None, "scene": None, "chefs": None}
    m = SUMMARY_PATTERNS["score"].search(text)
    if m:
        out["score"] = float(m.group(1))
    m = SUMMARY_PATTERNS["orders"].search(text)
    if m:
        out["arrived"], out["completed"] = int(m.group(1)), int(m.group(2))
    m = SUMMARY_PATTERNS["result"].search(text)
    if m:
        out["result"] = m.group(1)
    m = SUMMARY_PATTERNS["policy"].search(text)
    if m:
        out["order"], out["task"], out["dispatch"] = m.group(1).upper(), m.group(2), m.group(3)
    m = SUMMARY_PATTERNS["header"].search(text)
    if m:
        out["scene"], out["chefs"] = m.group(1), int(m.group(2))
    return out


def run_metrics(rt):
    run = latest_subdir(rt / "runs")
    return read_json(run / "metrics.json") if run else None


def list_runs(logs):
    out = []
    for summary in sorted((logs / "real-game").glob("*/summary.txt"), reverse=True)[:100]:
        try:
            text = summary.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        row = parse_summary(text)
        row["name"] = summary.parent.name
        metrics = run_metrics(summary.parent) or {}
        per_order = (metrics.get("orders") or {}).get("per_order") or []
        done = [o for o in per_order if o.get("completed")]
        if done:
            row["avg_turnaround_s"] = round(
                sum(o["finished_ms"] - o["arrival_ms"] for o in done) / len(done) / 1000.0, 1)
        row["worker_restarts"] = metrics.get("worker_restarts")
        out.append(row)
    return out


def run_detail(logs, name):
    if not RUN_NAME.match(name):
        return None
    base = (logs / "real-game").resolve()
    rt = (base / name).resolve()
    if rt.parent != base or not rt.is_dir():
        return None
    try:
        text = (rt / "summary.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        text = ""
    detail = parse_summary(text)
    detail.update({"name": name, "summary": text, "metrics": run_metrics(rt),
                   "policy_file": read_json(rt / "policy.json")})
    return detail


def list_scenes():
    maps = ROOT / "config" / "maps"
    try:
        return sorted(p.name for p in maps.iterdir()
                      if (p / "kitchen.json").is_file() and SCENE_NAME.match(p.name))
    except OSError:
        return []


APP = None
MAX_BODY = 4096


class Handler(BaseHTTPRequestHandler):
    server_version = "KitchenOSConsole/1"

    def log_message(self, fmt, *args):  # 不刷屏；SSE 每帧一条太吵
        pass

    def send_json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length < 0 or length > MAX_BODY:
            raise ValueError("body too large")
        raw = self.rfile.read(length) if length else b"{}"
        data = json.loads(raw or b"{}")
        if not isinstance(data, dict):
            raise ValueError("body must be an object")
        return data

    def send_static(self, name):
        path = (STATIC / name).resolve()
        if path.parent != STATIC or path.suffix not in STATIC_TYPES or not path.is_file():
            return self.send_json(404, {"error": "not found"})
        body = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", STATIC_TYPES[path.suffix])
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        seq = -1
        while True:
            seq, frame = APP.wait_frame(seq)
            try:
                self.wfile.write(("data: %s\n\n" % frame).encode("utf-8"))
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                return

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            return self.send_static("index.html")
        if path.startswith("/static/"):
            return self.send_static(path[len("/static/"):])
        if path == "/api/stream":
            return self.stream()
        if path == "/api/config":
            return self.send_json(200, {"scenes": list_scenes(), "orders": ORDERS,
                                        "tasks": TASKS, "dispatch": DISPATCH})
        if path == "/api/runs":
            return self.send_json(200, list_runs(APP.logs))
        if path.startswith("/api/runs/"):
            detail = run_detail(APP.logs, path[len("/api/runs/"):])
            return self.send_json(200, detail) if detail else self.send_json(
                404, {"error": "no such run"})
        return self.send_json(404, {"error": "not found"})

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        try:
            data = self.read_body()
        except (ValueError, UnicodeDecodeError):
            return self.send_json(400, {"error": "请求体不是合法 JSON 对象"})
        if path == "/api/run":
            return self.post_run(data)
        if path == "/api/stop":
            return self.send_json(200, {"stopping": True}) if APP.stop() else self.send_json(
                409, {"error": "当前没有在跑的局"})
        if path in ("/api/strategy", "/api/kill-scheduler"):
            return self.post_signal(path, data)
        return self.send_json(404, {"error": "not found"})

    def post_run(self, data):
        try:
            params = {
                "scene": str(data.get("scene", "")),
                "chefs": int(data.get("chefs", 4)),
                "strategy": str(data.get("strategy", "fcfs")).lower(),
                "task_policy": str(data.get("task_policy", "cost")).lower(),
                "dispatch": str(data.get("dispatch", "flexible")).lower(),
                "game_seconds": int(data.get("game_seconds", 240)),
            }
        except (TypeError, ValueError):
            return self.send_json(400, {"error": "参数类型不对"})
        if (not SCENE_NAME.match(params["scene"]) or not 1 <= params["chefs"] <= 4
                or params["strategy"] not in ORDERS or params["task_policy"] not in TASKS
                or params["dispatch"] not in DISPATCH or not 30 <= params["game_seconds"] <= 600):
            return self.send_json(400, {"error": "参数不在允许范围内"})
        if params["dispatch"] == "zoned" and not (ROOT / "config" / "maps" / params["scene"] / "zones.json").is_file():
            return self.send_json(400, {"error": "所选关卡没有分区配置，不能使用分区派工"})
        if not APP.start(params):
            return self.send_json(409, {"error": "已有一局在跑，先停掉它"})
        return self.send_json(202, {"started": True})

    def post_signal(self, path, data):
        rt = APP.rt
        if rt is None or not APP.running():
            return self.send_json(409, {"error": "当前没有在跑的局"})
        pid = scheduler_pid(rt)
        if pid <= 0:
            return self.send_json(409, {"error": "Scheduler 现在没在运行（可能正在被重启）"})
        if path == "/api/kill-scheduler":
            os.kill(pid, signal.SIGKILL)
            APP.note("已 kill -9 Scheduler pid %d，等 Supervisor 拉起" % pid)
            return self.send_json(200, {"killed": pid})
        order = str(data.get("order", "")).lower()
        task = str(data.get("task", "")).lower()
        words = []
        if order:
            if order not in ORDERS:
                return self.send_json(400, {"error": "未知订单策略"})
            words.append("order=" + order)
        if task:
            if task not in TASKS:
                return self.send_json(400, {"error": "未知工序策略"})
            words.append("task=" + task)
        if not words:
            return self.send_json(400, {"error": "order 和 task 至少给一个"})
        tmp = rt / ".strategy.request.tmp"
        tmp.write_text(" ".join(words) + "\n", encoding="utf-8")
        os.replace(tmp, rt / "strategy.request")
        os.kill(pid, signal.SIGHUP)
        APP.note("切换策略：%s（SIGHUP → pid %d）" % (" ".join(words), pid))
        return self.send_json(200, {"pid": pid, "request": " ".join(words)})


def main():
    global APP
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--script", default=str(ROOT / "tools" / "run-real-game.sh"))
    ap.add_argument("--logs-dir", default=str(ROOT / "logs"))
    args = ap.parse_args()
    APP = App(args.script, args.logs_dir)
    threading.Thread(target=APP.poll_forever, daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    print("KitchenOS 控制台: http://127.0.0.1:%d/" % args.port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        APP.stop()


if __name__ == "__main__":
    main()
