#!/usr/bin/env python3
"""instances.py - 管理多个隔离的 Overcooked2 实例。

每个实例 k（1 基）有：
  ~/.cache/wellcooked/inst/<k>/           exe、DLL、BepInEx/ 与符号链接的 Overcooked2_Data
  BepInEx/wellcooked-takeover.txt         window=640x360、bridge=127.0.0.1:14455+k
  run/                                    该实例自己的 Supervisor 进程树、pid、日志
  runs/                                   Scheduler 的每次 metrics/decisions

游戏目录本身只读：exe/DLL/BepInEx 从游戏目录复制，`Overcooked2_Data` 是符号链接。
不经过 Steam 的 applaunch，直接用 Proton 的 `run ./Overcooked2.exe` 从实例目录启动，
所以每个进程的 BepInEx/LogOutput.log 各写各的，不会互相轮转。

用法:
    python3 tools/instances.py up 3
    python3 tools/instances.py status
    python3 tools/instances.py down

真实游戏操作前必须按 .cache/wellcooked/kc-lanes/common.md 持有
/tmp/wellcooked-game.lock；up 会检查它，down 允许在没有锁时清理残留。
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_GAME_DIR = Path("/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2")
GAME_DIR = Path(os.environ.get("GAME_DIR", str(DEFAULT_GAME_DIR)))
INSTANCE_ROOT = Path(os.environ.get("WELLCOOKED_INSTANCE_ROOT",
                                    str(Path.home() / ".cache" / "wellcooked" / "inst")))
LOCK_DIR = Path("/tmp/wellcooked-game.lock")
APPID = "728880"

# 根目录里这几样是游戏启动与 Doorstop 注入的最小集合。`version.dll`/`winmm.dll`
# 在部分安装里是别的 mod 的代理，存在就一起复制，不存在不算错。
ROOT_FILES = [
    "Overcooked2.exe",
    "UnityPlayer.dll",
    "steam_api.dll",
    "winhttp.dll",
    "version.dll",
    "winmm.dll",
    "doorstop_config.ini",
    ".doorstop_version",
]
OPTIONAL_ROOT_FILES = {"version.dll", "winmm.dll"}

DEFAULT_WINDOW = "640x360"
DEFAULT_KITCHEN = REPO_ROOT / "config" / "kitchen.example.json"
DEFAULT_RECIPES = REPO_ROOT / "config" / "recipes.example.json"
DEFAULT_ORDERS = REPO_ROOT / "config" / "orders.example.json"


class InstanceError(RuntimeError):
    pass


def log(message):
    print(message, flush=True)


def require_lock():
    if not LOCK_DIR.is_dir():
        raise InstanceError(
            "没有 /tmp/wellcooked-game.lock。先按 common.md 取游戏锁再操作实例：\n"
            "  mkdir /tmp/wellcooked-game.lock && "
            "echo \"D 08 $(date -Is)\" > /tmp/wellcooked-game.lock/owner")


def _steam_root():
    return Path(os.environ.get("STEAM_COMPAT_CLIENT_INSTALL_PATH",
                               str(Path.home() / ".local" / "share" / "Steam")))


def find_proton():
    explicit = os.environ.get("PROTON")
    if explicit:
        return Path(explicit)
    candidates = [
        GAME_DIR.parent / "Proton - Experimental" / "proton",
        _steam_root() / "steamapps" / "common" / "Proton - Experimental" / "proton",
        Path.home() / ".steam" / "steam" / "steamapps" / "common" /
        "Proton - Experimental" / "proton",
        GAME_DIR.parent / "Proton Hotfix" / "proton",
    ]
    for path in candidates:
        if path.is_file():
            return path
    raise InstanceError("找不到 Proton；用 PROTON=<proton 路径> 指定")


def find_compat_data():
    explicit = os.environ.get("STEAM_COMPAT_DATA_PATH")
    if explicit:
        return Path(explicit)
    for root in (_steam_root(), GAME_DIR.parents[2] if len(GAME_DIR.parents) >= 3 else None):
        if root is None:
            continue
        candidate = Path(root) / "steamapps" / "compatdata" / APPID
        if candidate.is_dir():
            return candidate
    return Path.home() / ".local" / "share" / "Steam" / "steamapps" / "compatdata" / APPID


def bridge_binary(name):
    path = REPO_ROOT / "bridge" / "build" / name
    if not path.is_file():
        raise InstanceError("找不到 %s；先在 bridge/ 里 make" % path)
    return path


def pid_alive(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    # 被父进程丢下的僵尸仍然接受 kill(pid, 0)，但它已经不再跑游戏了。
    try:
        stat = Path("/proc/%d/stat" % pid).read_text()
        if stat.rsplit(")", 1)[1].split()[0] == "Z":
            return False
    except (OSError, IndexError):
        pass
    return True


def read_pid(path):
    try:
        return int(Path(path).read_text().strip())
    except (OSError, ValueError):
        return None


def write_pid(path, pid):
    Path(path).write_text("%d\n" % pid)


def pids_with_cwd(path):
    """找出 cwd 正好在实例目录里的进程（Wine 的游戏进程会继承启动目录）。"""
    target = os.path.realpath(str(path))
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            cwd = os.readlink(str(entry / "cwd"))
        except OSError:
            continue
        if os.path.realpath(cwd) == target:
            found.append(int(entry.name))
    return found


class Instance:
    def __init__(self, index, window=DEFAULT_WINDOW, kitchen=DEFAULT_KITCHEN,
                 recipes=DEFAULT_RECIPES, orders=DEFAULT_ORDERS, strategy="fcfs",
                 workers=4, nav="pibt", dispatch="flexible"):
        self.index = index
        self.window = window or DEFAULT_WINDOW
        self.kitchen = Path(kitchen)
        self.recipes = Path(recipes)
        self.orders = Path(orders)
        self.strategy = strategy
        self.workers = workers
        self.nav = nav
        self.dispatch = dispatch
        self.dir = INSTANCE_ROOT / str(index)
        self.bepinex = self.dir / "BepInEx"
        self.runtime = self.dir / "run"
        self.runs = self.dir / "runs"
        self.log = self.bepinex / "LogOutput.log"
        self.state_path = self.bepinex / "wellcooked-state.json"
        self.takeover = self.bepinex / "wellcooked-takeover.txt"
        self.command_path = self.bepinex / "wellcooked-command.txt"
        self.orders_path = self.bepinex / "wellcooked-orders.json"
        self.game_log = self.dir / "game.log"
        self.game_pid_path = self.dir / "game.pid"
        self.supervisor_pid_path = self.dir / "supervisor.pid"
        self.port = 14455 + index
        self._game = None
        self._supervisor = None
        self._bridge_hello_base = 0
        self._bridge_link_base = 0
        self.current_scene = None

    # ---------------------------------------------------------------- 目录

    def prepare(self):
        if not GAME_DIR.is_dir():
            raise InstanceError("游戏目录不存在: %s（用 GAME_DIR=<路径> 指定）" % GAME_DIR)
        self.bepinex.mkdir(parents=True, exist_ok=True)
        self.runtime.mkdir(parents=True, exist_ok=True)
        self.runs.mkdir(parents=True, exist_ok=True)

        for name in ROOT_FILES:
            source = GAME_DIR / name
            if not source.is_file():
                if name in OPTIONAL_ROOT_FILES:
                    continue
                raise InstanceError("游戏目录缺少 %s" % source)
            target = self.dir / name
            if not target.is_file() or target.stat().st_size != source.stat().st_size:
                shutil.copy2(source, target)

        # 共享大资源：符号链接而不是 8 GB 拷贝。已经指对就不动。
        data_link = self.dir / "Overcooked2_Data"
        wanted = GAME_DIR / "Overcooked2_Data"
        if data_link.is_symlink():
            if os.path.realpath(data_link) != os.path.realpath(wanted):
                data_link.unlink()
        elif data_link.exists():
            raise InstanceError("%s 已存在且不是符号链接，拒绝覆盖" % data_link)
        if not data_link.exists():
            os.symlink(wanted, data_link)

        # BepInEx 的核心与插件每个实例各一份；配置和日志也写在这份里。
        for sub in ("core", "patchers", "plugins"):
            source = GAME_DIR / "BepInEx" / sub
            target = self.bepinex / sub
            if source.is_dir():
                shutil.copytree(source, target, dirs_exist_ok=True)
        (self.bepinex / "config").mkdir(parents=True, exist_ok=True)

        # 新部署的插件在每次 up 时同步进实例，避免实例拿着旧 DLL。
        built = REPO_ROOT / "plugin" / "bin" / "Release" / "net35" / "WellCooked.Smoke.dll"
        source = built if built.is_file() else GAME_DIR / "BepInEx" / "plugins" / "WellCooked.Smoke.dll"
        if source.is_file():
            (self.bepinex / "plugins").mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, self.bepinex / "plugins" / "WellCooked.Smoke.dll")

        self.write_config({
            "window": self.window,
            "window_pos": "%d,0" % ((self.index - 1) * self.window_width()),
            # 插件玩家位 1 基：N 个厨师占 1..N 号位。
            "slots": ",".join(str(slot) for slot in range(1, self.workers + 1)),
            "bridge": "127.0.0.1:%d" % self.port,
            "frontend_press": "true",
        })

    def window_width(self):
        try:
            return int(self.window.split("x", 1)[0])
        except (ValueError, IndexError):
            return 0

    # ---------------------------------------------------------------- 配置

    def read_config(self):
        values = {}
        if not self.takeover.is_file():
            return values
        for line in self.takeover.read_text(errors="replace").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip().lower()] = value.strip()
        return values

    def write_config(self, updates):
        values = self.read_config()
        values.update({key: str(value) for key, value in updates.items()})
        text = "".join("%s=%s\n" % (key, values[key]) for key in sorted(values))
        tmp = self.takeover.with_suffix(".tmp")
        tmp.write_text(text)
        tmp.replace(self.takeover)

    def write_orders(self, orders_path=None, seed=None):
        path = Path(orders_path) if orders_path else self.orders
        if not path.is_absolute():
            path = (REPO_ROOT / path).resolve()
        data = json.loads(path.read_text())
        if seed is not None:
            data["seed"] = int(seed)
        if not data.get("menu"):
            raise InstanceError("%s 没有 menu，无法固定订单" % path)
        self.orders_path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")

    def command(self, text):
        self.command_path.write_text(text if text.endswith("\n") else text + "\n")

    # ---------------------------------------------------------------- 日志/状态

    def log_text(self):
        try:
            return self.log.read_text(errors="replace")
        except OSError:
            return ""

    def bridge_log_text(self):
        try:
            return (self.runtime / "logs" / "bridge.log").read_text(errors="replace")
        except OSError:
            return ""

    def log_count(self, needle):
        return self.log_text().count(needle)

    def state(self):
        try:
            return json.loads(self.state_path.read_text())
        except (OSError, ValueError):
            return None

    def scene(self):
        state = self.state()
        return state.get("scene") if state else None

    def chefs(self):
        state = self.state()
        return len(state.get("chefs") or []) if state else 0

    def wait_until(self, predicate, timeout, what, interval=1.0):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            try:
                value = predicate()
            except (OSError, ValueError):
                value = None
            if value:
                return value
            last = value
            time.sleep(interval)
        raise InstanceError("实例 %d 等待超时（%s 秒）：%s" % (self.index, timeout, what))

    def wait_plugin(self, timeout=240):
        def ready():
            if self._game is not None and self._game.poll() is not None:
                raise InstanceError("实例 %d 的游戏进程退出了（exit=%s），看 %s"
                                    % (self.index, self._game.returncode, self.game_log))
            text = self.log_text()
            return ("takeover: 每帧检查" in text
                    and ("WellCooked.Smoke loaded" in text or "smoke patch alive" in text))
        self.wait_until(ready, timeout, "插件加载", interval=2)
        text = self.log_text()
        for line in text.splitlines():
            if "takeover: 窗口" in line:
                log("  实例 %d %s" % (self.index, line.strip()))
                break

    def wait_pads(self, count=2, timeout=180):
        def joined():
            return self.log_count("游戏已自己加入 pad") >= count
        self.wait_until(joined, timeout, "%d 个虚拟手柄被游戏加入" % count, interval=2)

    def wait_bridge_link(self, timeout=30):
        def linked():
            log_text = self.bridge_log_text()
            return (log_text.count("plugin_hello") > self._bridge_hello_base
                    and log_text.count("link_up") > self._bridge_link_base)
        self.wait_until(linked, timeout, "插件连上新 Supervisor 的 Bridge", interval=0.5)

    # ---------------------------------------------------------------- 进程

    def game_running(self):
        pid = read_pid(self.game_pid_path)
        return pid_alive(pid)

    def supervisor_running(self):
        pid = read_pid(self.supervisor_pid_path)
        return pid_alive(pid)

    def start_game(self, timeout=240):
        if self.game_running():
            return
        self.prepare_runtime_files()
        proton = find_proton()
        env = os.environ.copy()
        compat = find_compat_data()
        env["STEAM_COMPAT_DATA_PATH"] = str(compat)
        env["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = str(_steam_root())
        env["SteamAppId"] = APPID
        env["SteamGameId"] = APPID
        # 不覆盖用户已设置的 WINEDLLOVERRIDES；winhttp 的覆盖在 Proton 前缀注册表里。
        with open(self.game_log, "wb") as out:
            self._game = subprocess.Popen(
                [str(proton), "run", "./Overcooked2.exe"],
                cwd=str(self.dir),
                stdin=subprocess.DEVNULL,
                stdout=out,
                stderr=subprocess.STDOUT,
                start_new_session=True,
                env=env,
            )
        write_pid(self.game_pid_path, self._game.pid)
        log("  实例 %d 启动游戏 pid=%d port=%d" % (self.index, self._game.pid, self.port))
        self.wait_plugin(timeout)

    def prepare_runtime_files(self):
        for path in (self.log, self.state_path, self.command_path):
            try:
                path.unlink()
            except OSError:
                pass
        for old in self.bepinex.glob("LogOutput.log.*"):
            try:
                old.unlink()
            except OSError:
                pass

    def stop_game(self):
        pid = read_pid(self.game_pid_path)
        if pid_alive(pid):
            try:
                os.killpg(os.getpgid(pid), signal.SIGTERM)
            except OSError:
                pass
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline and pid_alive(pid):
                time.sleep(0.3)
            if pid_alive(pid):
                try:
                    os.killpg(os.getpgid(pid), signal.SIGKILL)
                except OSError:
                    pass
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and pid_alive(pid):
                    time.sleep(0.2)
        # Wine 有时把游戏进程放到别的进程组；按 cwd 再收一遍，只碰本实例目录。
        for extra in pids_with_cwd(self.dir):
            if extra == os.getpid():
                continue
            try:
                os.kill(extra, signal.SIGTERM)
            except OSError:
                pass
        for path in (self.game_pid_path,):
            try:
                path.unlink()
            except OSError:
                pass
        self._game = None

    def restart_game(self, timeout=240):
        self.stop_game()
        self.current_scene = None
        self.start_game(timeout)

    def start_supervisor(self, strategy=None, nav=None, dispatch=None, timeout=40):
        self.stop_supervisor()
        before = self.bridge_log_text()
        self._bridge_hello_base = before.count("plugin_hello")
        self._bridge_link_base = before.count("link_up")
        strategy = strategy or self.strategy
        nav = nav or self.nav
        dispatch = dispatch or self.dispatch
        if nav != "pibt":
            log("  实例 %d 注意：当前 navd 只实现 pibt，忽略 nav=%s" % (self.index, nav))
        dispatch_opt = "zoned" if dispatch in ("zoned", "zone") else "flexible"
        argv = [
            str(bridge_binary("kitchenos-supervisor")),
            "--foreground",
            "--runtime-dir", str(self.runtime),
            "--instance", str(self.index),
            "--bridge-port", str(self.port),
            "--workers", str(self.workers),
            "--kitchen", str(self.kitchen),
            "--recipes", str(self.recipes),
            "--orders", str(self.orders),
            "--runs-dir", str(self.runs),
            "--strategy", strategy,
            "--dispatch", dispatch_opt,
            "--health-interval-ms", "500",
            "--supervisor-heartbeat-ms", "500",
            "--heartbeat-timeout-ms", "3000",
            "--stop-timeout-ms", "2000",
        ]
        self.runtime.mkdir(parents=True, exist_ok=True)
        console = self.runtime / "supervisor.console"
        with open(console, "ab") as out:
            self._supervisor = subprocess.Popen(
                argv, cwd=str(REPO_ROOT), stdin=subprocess.DEVNULL, stdout=out,
                stderr=subprocess.STDOUT, start_new_session=True)
        write_pid(self.supervisor_pid_path, self._supervisor.pid)
        log("  实例 %d 启动 Supervisor pid=%d（%s）"
            % (self.index, self._supervisor.pid, strategy))
        self.wait_supervisor_ready(timeout)

    def wait_supervisor_ready(self, timeout=40):
        status_path = self.runtime / "status.json"
        expected = {"bridge", "scheduler", "navd"} | {
            "worker-%d" % i for i in range(self.workers)}

        def ready():
            if not self.supervisor_running():
                raise InstanceError("Supervisor 启动后退出了，看 %s"
                                    % (self.runtime / "supervisor.console"))
            try:
                status = json.loads(status_path.read_text())
            except (OSError, ValueError):
                return None
            running = {child.get("role") for child in status.get("children", [])
                       if child.get("state") == "running" and child.get("pid")}
            if not expected <= running:
                return None
            try:
                sock = socket.create_connection(("127.0.0.1", self.port), timeout=0.2)
                sock.close()
            except OSError:
                return None
            return True
        self.wait_until(ready, timeout, "进程树就绪（bridge+scheduler+navd+workers）", interval=0.3)

    def stop_supervisor(self):
        pid = read_pid(self.supervisor_pid_path)
        if pid_alive(pid):
            try:
                os.killpg(os.getpgid(pid), signal.SIGTERM)
            except OSError:
                pass
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline and pid_alive(pid):
                time.sleep(0.2)
            if pid_alive(pid):
                try:
                    os.killpg(os.getpgid(pid), signal.SIGKILL)
                except OSError:
                    pass
        try:
            self.supervisor_pid_path.unlink()
        except OSError:
            pass
        self._supervisor = None

    # ---------------------------------------------------------------- 一局

    def ensure_up(self, start_supervisor=True):
        self.prepare()
        if not self.game_running():
            self.start_game()
        if start_supervisor:
            status = None
            try:
                status = json.loads((self.runtime / "status.json").read_text())
            except (OSError, ValueError):
                pass
            if not self.supervisor_running() or status is None:
                self.start_supervisor()

    def set_scene(self, scene, timeout=120):
        """进入指定关卡；同一进程里换场景不刷新菜谱，所以换关由 restart_game 负责。"""
        self.write_config({"frontend_press": "false"})
        time.sleep(6)  # 插件每 5 秒重读配置
        self.command("orders=reload")
        time.sleep(0.5)
        self.command("load=%s" % scene)

        def loaded():
            state = self.state()
            if state is None or state.get("scene") != scene or not state.get("chefs"):
                return None
            timer = state.get("timer") or {}
            left = timer.get("left")
            # 关卡刚载入时状态文件里可能还留着上一局的 left=0；等新回合计时出现。
            return left is None or left > 1
        self.wait_until(loaded, timeout, "进入 %s 且有厨师/回合计时" % scene, interval=1)
        self.current_scene = scene

    def restart_round(self, scene, timeout=120):
        self.command("orders=reload")
        time.sleep(0.5)
        self.command("restart=1")

        def loaded():
            state = self.state()
            if state is None or state.get("scene") != scene or not state.get("chefs"):
                return None
            timer = state.get("timer") or {}
            left = timer.get("left")
            return left is None or left > 1
        self.wait_until(loaded, timeout, "重开 %s 且回合计时已重置" % scene, interval=1)
        self.current_scene = scene

    def stop(self):
        self.stop_supervisor()
        self.stop_game()


def parse_window(value):
    if value in (None, "", "full", "off", "0"):
        return "full"
    if "x" in value:
        return value
    raise argparse.ArgumentTypeError("window 必须是 WxH 或 full")


def instance_from_args(index, args):
    return Instance(index, window=args.window, kitchen=args.kitchen,
                    recipes=args.recipes, orders=args.orders, strategy=args.strategy,
                    workers=args.workers, nav=args.nav, dispatch=args.dispatch)


def cmd_up(args):
    require_lock()
    instances = [instance_from_args(k, args) for k in range(1, args.count + 1)]
    started = []
    try:
        for inst in instances:
            log("实例 %d：准备 %s" % (inst.index, inst.dir))
            inst.ensure_up(start_supervisor=True)
            started.append(inst)
    except Exception:
        for inst in started:
            inst.stop()
        raise
    log("")
    log("已启动 %d 个实例。窗口 %s，端口 %s"
        % (len(instances), args.window,
           ", ".join(str(inst.port) for inst in instances)))
    for inst in instances:
        log("  #%d  %s  BepInEx/LogOutput.log=%s  run=%s"
            % (inst.index, inst.dir, inst.log, inst.runtime))
    return 0


def cmd_down(args):
    if args.count:
        indexes = range(1, args.count + 1)
    else:
        indexes = sorted({int(p.name) for p in INSTANCE_ROOT.glob("*")
                          if p.is_dir() and p.name.isdigit()})
    for index in indexes:
        inst = instance_from_args(index, args)
        log("实例 %d：停止" % index)
        inst.stop()
    return 0


def cmd_status(args):
    indexes = range(1, args.count + 1) if args.count else [
        int(p.name) for p in sorted(INSTANCE_ROOT.glob("*")) if p.name.isdigit()]
    for index in indexes:
        inst = instance_from_args(index, args)
        if not inst.dir.is_dir():
            continue
        state = inst.state() or {}
        log("#%d dir=%s game=%s supervisor=%s port=%d scene=%s chefs=%s score=%s"
            % (index, inst.dir, "running" if inst.game_running() else "stopped",
               "running" if inst.supervisor_running() else "stopped", inst.port,
               state.get("scene"), len(state.get("chefs") or []), state.get("score")))
    return 0


def build_parser():
    parser = argparse.ArgumentParser(
        description="管理隔离的 Overcooked2 多实例（真实游戏；先取 /tmp/wellcooked-game.lock）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["up", "down", "status"])
    parser.add_argument("count", nargs="?", type=int, default=None,
                        help="实例个数（up 默认 3；down/status 省略表示全部）")
    parser.add_argument("--window", type=parse_window, default=DEFAULT_WINDOW,
                        help="窗口大小 WxH 或 full（默认 640x360）")
    parser.add_argument("--kitchen", default=str(DEFAULT_KITCHEN))
    parser.add_argument("--recipes", default=str(DEFAULT_RECIPES))
    parser.add_argument("--orders", default=str(DEFAULT_ORDERS))
    parser.add_argument("--strategy", default="fcfs", choices=["fcfs", "edf"])
    parser.add_argument("--nav", default="pibt", help="走位算法（当前仅 pibt）")
    parser.add_argument("--dispatch", default="flexible",
                        help="派工模式（当前仅 flexible）")
    parser.add_argument("--workers", type=int, default=4, choices=[1, 2, 3, 4],
                        help="每实例厨师数（默认 4，本地合作最多 4 个玩家位）")
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.count is None:
        args.count = 3 if args.command == "up" else 0
    if args.count < 0 or args.count > 9:
        parser.error("count 必须在 0..9")
    try:
        if args.command == "up":
            return cmd_up(args)
        if args.command == "down":
            return cmd_down(args)
        return cmd_status(args)
    except (InstanceError, OSError, ValueError) as exc:
        print("instance: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
