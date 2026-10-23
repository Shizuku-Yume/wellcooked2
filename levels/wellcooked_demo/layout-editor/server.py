import json
import os
import shutil
import subprocess
import threading
import urllib.parse
from datetime import datetime
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
# 编辑器放在 levels/wellcooked_demo/layout-editor/，layout TSV 就在上一级。
ROOT = os.path.dirname(HERE)
PORT = int(os.environ.get("WELLCOOKED_EDITOR_PORT", "8770"))
# 构建脚本在仓库外（Windows 侧是 D:\oc2-level-build\build-only.ps1），用环境变量传入。
BUILD_PS1 = os.environ.get("WELLCOOKED_BUILD_PS1", "")
# Linux 侧没有 Unity 构建能力；需要真实构建时把命令放这里（例如 ssh 到 Windows 构建机），
# 命令成功后必须把 5 个 bundle 写回 levels/wellcooked_demo/，然后本服务再部署。
BUILD_CMD = os.environ.get("WELLCOOKED_BUILD_CMD", "")
DEPLOY_SH = os.path.join(ROOT, "deploy.sh")
STATUS_FILE = os.environ.get("WELLCOOKED_STATUS_FILE", os.path.join(HERE, "build-status.txt"))
# 美术件实测尺寸表 + 中文名表：Windows 构建机上默认在仓库外的 oc2-level-build 目录，
# 用环境变量覆盖；都读不到时 /api/art 返回空列表（前端会提示没有尺寸表）。
ART_SIZES = os.environ.get("WELLCOOKED_ART_SIZES",
                           r"D:\oc2-level-build\art-sizes.tsv")
ART_NAMES = os.environ.get("WELLCOOKED_ART_NAMES",
                           r"D:\oc2-level-build\art-names-zh.json")
_art_cache = {"mtime": None, "catalog": None}
_names_cache = {"mtime": None, "names": None}


def art_names():
    """读取中文名表，返回 {"主题/素材名": {zh, zhName, themeZh}}"""
    if not os.path.isfile(ART_NAMES):
        return {}
    mtime = os.path.getmtime(ART_NAMES)
    if _names_cache["mtime"] == mtime and _names_cache["names"] is not None:
        return _names_cache["names"]
    try:
        with open(ART_NAMES, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except Exception:
        data = {}
    _names_cache["mtime"] = mtime
    _names_cache["names"] = data
    return data


def art_catalog():
    """art-sizes.tsv -> [{theme,count,items:[{name,w,h,d,minY,colliders,renderers,zh,zhName,themeZh}]}]"""
    if not os.path.isfile(ART_SIZES):
        return []
    mtime = os.path.getmtime(ART_SIZES)
    if _art_cache["mtime"] == mtime and _art_cache["catalog"] is not None:
        return _art_cache["catalog"]

    themes = {}
    order = []
    with open(ART_SIZES, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh):
            line = line.rstrip("\n")
            if not line:
                continue
            cols = line.split("\t")
            if lineno == 0 or cols[0] == "theme":
                continue
            if len(cols) < 9:
                continue
            theme, name = cols[0], cols[1]
            try:
                w, h, d, miny = (float(cols[3]), float(cols[4]),
                                 float(cols[5]), float(cols[6]))
                colliders = int(cols[7])
                renderers = int(cols[8])
            except ValueError:
                continue
            if theme not in themes:
                themes[theme] = []
                order.append(theme)
            themes[theme].append({
                "name": name,
                "w": w, "h": h, "d": d, "minY": miny,
                "colliders": colliders, "renderers": renderers,
            })

    names = art_names()
    for theme, items in themes.items():
        for item in items:
            zh = names.get(theme + "/" + item["name"])
            item["zh"] = (zh or {}).get("zh", item["name"])
            item["zhName"] = (zh or {}).get("zhName", item["name"])
            item["themeZh"] = (zh or {}).get("themeZh", theme)

    catalog = []
    for theme in order:
        items = sorted(themes[theme], key=lambda item: item["name"].lower())
        catalog.append({"theme": theme, "count": len(items), "items": items})
    _art_cache["mtime"] = mtime
    _art_cache["catalog"] = catalog
    return catalog

_building = [False]


def default_game_dir():
    """按常见 Steam 路径找游戏目录；找不到就返回 Windows 时代的旧默认值。"""
    candidates = [
        os.path.expanduser("~/.local/share/Steam/steamapps/common/Overcooked! 2"),
        os.path.expanduser("~/.steam/steam/steamapps/common/Overcooked! 2"),
        os.path.join("/run/media", os.environ.get("USER", ""),
                     "File/Steam/steamapps/common/Overcooked! 2"),
        os.path.join(os.path.dirname(os.path.dirname(ROOT)), "Overcooked 2!"),
    ]
    for candidate in candidates:
        if os.path.isdir(os.path.join(candidate, "BepInEx")):
            return candidate
    return candidates[0]


# 游戏侧“处理时间”覆盖：本编辑器写，BepInEx 插件 WellCooked 读同一份文件。
# 换游戏目录时设置环境变量 WELLCOOKED_GAME_DIR。
GAME_DIR = os.environ.get("WELLCOOKED_GAME_DIR") or default_game_dir()
PROCESSING_FILE = os.path.join(GAME_DIR, "BepInEx", "wellcooked-processing.json")
PROCESSING_DEFAULTS = {"cook_time": 10.0, "chop_stages": 8, "chop_multiplier": 5}
# 必须和 plugin/WellCooked.Plugin/Game/ProcessingTuner.cs 的常量一致。
PROCESSING_RANGES = {
    "cook_time": (1.0, 30.0, 0.5),
    "chop_stages": (2, 8, 1),
    "chop_multiplier": (1, 10, 1),
}

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


# ---------------- 构建 / 部署 ----------------

def _write_status(text, append=True):
    folder = os.path.dirname(STATUS_FILE)
    if folder:
        os.makedirs(folder, exist_ok=True)
    with open(STATUS_FILE, "a" if append else "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
        if not text.endswith("\n"):
            fh.write("\n")


def _run_logged(cmd, shell=False, env=None):
    """跑命令，把 stdout/stderr 逐行写进状态文件（前端每 3 秒轮询）。"""
    printable = cmd if isinstance(cmd, str) else " ".join(cmd)
    _write_status("$ " + printable)
    try:
        proc = subprocess.Popen(
            cmd, shell=shell, env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        for line in proc.stdout:
            _write_status(line.rstrip("\n"))
        rc = proc.wait()
    except Exception as exc:  # noqa: BLE001
        _write_status("运行失败: " + str(exc))
        return 127
    _write_status("退出码 %d" % rc)
    return rc


def _run_build():
    try:
        _write_status("[%s] 构建/部署开始" % datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                      append=False)
        built = False
        if BUILD_CMD:
            _write_status("构建方式: WELLCOOKED_BUILD_CMD")
            rc = _run_logged(BUILD_CMD, shell=True)
            if rc != 0:
                _write_status("构建命令失败，不部署。")
                return
            built = True
        elif BUILD_PS1 and os.path.isfile(BUILD_PS1):
            powershell = shutil.which("powershell") or shutil.which("pwsh")
            if powershell:
                _write_status("构建方式: " + powershell + " " + BUILD_PS1)
                rc = _run_logged([
                    powershell, "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-File", BUILD_PS1])
                if rc != 0:
                    _write_status("PowerShell 构建失败，不部署。")
                    return
                built = True
            else:
                _write_status("找到 WELLCOOKED_BUILD_PS1，但本机没有 powershell/pwsh；"
                              "跳过构建，只部署现有 bundle。")
        else:
            _write_status("未配置 WELLCOOKED_BUILD_CMD / WELLCOOKED_BUILD_PS1；"
                          "Linux 端只部署现有 bundle（不会重建 AssetBundle）。")

        if not os.path.isfile(DEPLOY_SH):
            _write_status("找不到部署脚本: " + DEPLOY_SH)
            return
        env = dict(os.environ)
        env["WELLCOOKED_GAME_DIR"] = GAME_DIR
        _write_status("部署目标: " + GAME_DIR + ("（已构建）" if built else "（现有 bundle）"))
        rc = _run_logged(["bash", DEPLOY_SH], env=env)
        _write_status("[%s] %s" % (
            datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "完成" if rc == 0 else "部署失败（退出码 %d）" % rc))
    finally:
        _building[0] = False


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
        if parsed.path == "/api/art":
            self._json({"ok": True, "themes": art_catalog()})
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
            if _building[0]:
                self._json({"ok": False, "error": "busy"}, 409); return
            _building[0] = True
            threading.Thread(target=_run_build, daemon=True).start()
            powershell = shutil.which("powershell") or shutil.which("pwsh")
            mode = "build+deploy" if (BUILD_CMD or (BUILD_PS1 and powershell)) else "deploy-only"
            self._json({"ok": True, "mode": mode}); return
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
