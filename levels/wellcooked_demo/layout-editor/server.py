import json
import subprocess
import os
import urllib.parse
from datetime import datetime
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
# 编辑器放在 levels/wellcooked_demo/layout-editor/，layout TSV 就在上一级。
ROOT = os.path.dirname(HERE)
PORT = int(os.environ.get("WELLCOOKED_EDITOR_PORT", "8770"))
# 构建脚本在仓库外（本机是 D:\oc2-level-build\build-only.ps1），用环境变量传入。
BUILD_PS1 = os.environ.get("WELLCOOKED_BUILD_PS1", "")
STATUS_FILE = os.environ.get("WELLCOOKED_STATUS_FILE", os.path.join(HERE, "build-status.txt"))
_building = [False]

# 游戏侧“处理时间”覆盖：本编辑器写，BepInEx 插件 WellCooked 读同一份文件。
# 换游戏目录时设置环境变量 WELLCOOKED_GAME_DIR（例如 D:\EPIC\Games\Overcooked2）。
GAME_DIR = os.environ.get("WELLCOOKED_GAME_DIR") or os.path.join(
    os.path.dirname(os.path.dirname(ROOT)), "Overcooked 2!")
PROCESSING_FILE = os.path.join(GAME_DIR, "BepInEx", "wellcooked-processing.json")
PROCESSING_DEFAULTS = {"cook_time": 10.0, "chop_stages": 8, "chop_multiplier": 5}
# 必须和 plugin/WellCooked.Plugin/Game/ProcessingTuner.cs 的常量一致。
PROCESSING_RANGES = {
    "cook_time": (1.0, 30.0, 0.5),
    "chop_stages": (2, 8, 1),
    "chop_multiplier": (1, 10, 1),
}

HEADER = (
    "# wellcooked_demo 关卡布局（单位：1 米格；x 向右、z 向上）\n"
    "# 列（制表符分隔，固定 7 列）：kind\tprefab\tso\tingredient\tx\tz\tnote\n"
    "# kind：dispenser | chopping | cooker | frying | plate_stack | serving | plate_return | utensil | spawn | counter\n"
)


def layout_files():
    if not os.path.isdir(ROOT):
        return []
    return sorted(f for f in os.listdir(ROOT)
                  if f.startswith("layout_") and f.endswith(".tsv"))


def _processing_ranges_json():
    return {k: [lo, hi, step] for k, (lo, hi, step) in PROCESSING_RANGES.items()}


def _clamp_processing(payload):
    if not isinstance(payload, dict):
        raise ValueError("请求体必须是 JSON 对象")
    values = {}
    for key, (lo, hi, step) in PROCESSING_RANGES.items():
        raw = payload.get(key, PROCESSING_DEFAULTS[key])
        if key == "cook_time":
            try:
                v = float(raw)
            except (TypeError, ValueError):
                raise ValueError("cook_time 必须是数字")
            if v != v or v in (float("inf"), float("-inf")):
                raise ValueError("cook_time 必须是有限数字")
            v = min(max(v, lo), hi)
            values[key] = round(round(v / step) * step, 2)
        else:
            try:
                v = int(float(raw))
            except (TypeError, ValueError):
                raise ValueError("%s 必须是整数" % key)
            values[key] = min(max(v, lo), hi)
    return values


def _read_processing():
    values = dict(PROCESSING_DEFAULTS)
    updated = None
    exists = os.path.isfile(PROCESSING_FILE)
    if exists:
        try:
            with open(PROCESSING_FILE, "r", encoding="utf-8") as fh:
                data = json.load(fh)
            values.update(_clamp_processing(data))
            updated = data.get("updated_at")
        except Exception:
            values = dict(PROCESSING_DEFAULTS)
    return values, updated, exists


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=HERE, **kwargs)

    def _json(self, payload, code=200):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/api/build/status"):
            txt = ""
            try:
                txt = open(STATUS_FILE, encoding="utf-8", errors="replace").read()[-4000:]
            except Exception:
                pass
            self._json({"ok": True, "building": _building[0], "log": txt}); return
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/processing":
            values, updated, exists = _read_processing()
            self._json({"ok": True, "path": PROCESSING_FILE, "values": values,
                        "defaults": PROCESSING_DEFAULTS,
                        "ranges": _processing_ranges_json(),
                        "updated_at": updated, "exists": exists})
            return
        if parsed.path == "/api/list":
            self._json({"ok": True, "files": layout_files(), "root": ROOT})
            return
        if parsed.path == "/api/load":
            name = os.path.basename(urllib.parse.parse_qs(parsed.query).get("name", [""])[0])
            if not name:
                self._json({"ok": False, "error": "缺少 name"}, 400)
                return
            path = os.path.join(ROOT, name)
            if not os.path.isfile(path):
                self._json({"ok": False, "error": "文件不存在: " + name}, 404)
                return
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                self._json({"ok": True, "name": name, "content": fh.read()})
            return
        super().do_GET()

    def do_POST(self):
        if self.path.startswith("/api/build"):
            if not BUILD_PS1 or not os.path.isfile(BUILD_PS1):
                self._json({"ok": False, "error": "未配置 WELLCOOKED_BUILD_PS1（指向 build-only.ps1）"}, 400); return
            if _building[0]:
                self._json({"ok": False, "error": "busy"}, 409); return
            _building[0] = True
            import threading, subprocess as sp
            def run():
                try:
                    sp.run(["powershell","-NoProfile","-ExecutionPolicy","Bypass","-File",BUILD_PS1], stdout=sp.DEVNULL, stderr=sp.DEVNULL)
                finally:
                    _building[0] = False
            threading.Thread(target=run, daemon=True).start()
            self._json({"ok": True}); return
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/processing":
            try:
                length = int(self.headers.get("Content-Length", "0"))
                payload = json.loads(self.rfile.read(length).decode("utf-8"))
                values = _clamp_processing(payload)
                folder = os.path.dirname(PROCESSING_FILE)
                if not os.path.isdir(folder):
                    raise ValueError("BepInEx 目录不存在: " + folder)
                data = {"version": 1}
                data.update(values)
                data["updated_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
                data["source"] = "layout-editor"
                tmp = PROCESSING_FILE + ".tmp"
                with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
                    json.dump(data, fh, ensure_ascii=False, indent=2)
                    fh.write("\n")
                os.replace(tmp, PROCESSING_FILE)
                self._json({"ok": True, "path": PROCESSING_FILE, "values": values,
                            "ranges": _processing_ranges_json(),
                            "updated_at": data["updated_at"], "exists": True})
            except Exception as exc:  # noqa: BLE001
                self._json({"ok": False, "error": str(exc)}, 400)
            return
        if parsed.path != "/api/save":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            name = os.path.basename(payload.get("name") or "")
            content = payload.get("content") or ""
            if not name.endswith(".tsv"):
                raise ValueError("文件名必须是 .tsv")
            if ".." in name or name != os.path.basename(name):
                raise ValueError("非法文件名")
            path = os.path.join(ROOT, name)
            backup = path + ".bak"
            if os.path.isfile(path):
                with open(path, "rb") as src:
                    data = src.read()
                with open(backup, "wb") as dst:
                    dst.write(data)
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(content)
            self._json({"ok": True, "name": name, "bytes": len(content.encode("utf-8"))})
        except Exception as exc:  # noqa: BLE001
            self._json({"ok": False, "error": str(exc)}, 400)

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    print("ROOT =", ROOT)
    print("processing file =", PROCESSING_FILE)
    print("layout files:", layout_files())
    print("serving http://127.0.0.1:%d/" % PORT)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
