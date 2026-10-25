#!/usr/bin/env python3
"""batch.py - 把实验清单排进空闲的多开游戏实例，跑完整局后汇总 results.csv。

清单是 CSV/TSV（带表头，`#` 开头是注释），列：
    scene, strategy, nav, dispatch, orders, seed
  scene     关卡场景名（如 s_wellcooked_demo_2）
  strategy  fcfs / edf（当前 Supervisor 支持的订单策略）
  nav       pibt（当前 navd 只实现 PIBT；其它值会记录并提示）
  dispatch  flexible（分区流水线在任务 10 才接入）
  orders    订单序列 JSON 的路径（相对清单所在目录或仓库根目录）
  seed      固定随机源的种子；`-1` 表示不固定
只有 `scene` 与 `orders` 必填，其余有默认值。

每个实例一次跑一局：先按清单写 orders/seed，重启该实例的 Supervisor 进程树
（策略/走位随参数变化），`load=`/`restart=` 进关，等回合计时归零，从插件落盘的
`wellcooked-state.json` 取最终得分，并记录该局 Scheduler 的 metrics.json。

用法:
    python3 tools/batch.py experiments/policies.tsv
    python3 tools/batch.py manifest.csv --instances 3 --out results.csv
    python3 tools/batch.py manifest.csv --dry-run

真实游戏操作前必须按 .cache/wellcooked/kc-lanes/common.md 持有
/tmp/wellcooked-game.lock；本脚本不自己加锁/放锁（批跑期间锁要一直拿着）。
"""

import argparse
import csv
import json
import os
import queue
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import instances as instance_mod  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
FIELDS = ["scene", "strategy", "nav", "dispatch", "orders", "seed"]
SYNONYMS = {
    "map": "scene", "level": "scene", "scene": "scene",
    "strategy": "strategy", "policy": "strategy", "order_strategy": "strategy",
    "nav": "nav", "nav_algo": "nav", "navigation": "nav",
    "dispatch": "dispatch", "mode": "dispatch", "dispatch_mode": "dispatch",
    "orders": "orders", "order_seq": "orders", "order_sequence": "orders",
    "seed": "seed",
}
DEFAULTS = {
    "strategy": "fcfs",
    "nav": "pibt",
    "dispatch": "flexible",
    "seed": -1,
}


class BatchError(RuntimeError):
    pass


def log(message):
    print(message, flush=True)


def resolve_orders(value, manifest_dir):
    path = Path(value)
    if path.is_absolute():
        return path
    candidate = (manifest_dir / path).resolve()
    if candidate.is_file():
        return candidate
    candidate = (REPO_ROOT / path).resolve()
    if candidate.is_file():
        return candidate
    raise BatchError("订单序列不存在: %s（相对清单目录与仓库根目录都找不到）" % value)


def read_manifest(path):
    raw = []
    for lineno, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        raw.append((lineno, line))
    if not raw:
        raise BatchError("清单是空的: %s" % path)

    delimiter = "\t" if "\t" in raw[0][1] else ","
    first = next(csv.reader([raw[0][1]], delimiter=delimiter))
    keys = [SYNONYMS.get(cell.strip().lower()) for cell in first]
    has_header = any(keys) and all(k in FIELDS for k in keys if k)
    if has_header:
        if "scene" not in keys or "orders" not in keys:
            raise BatchError("表头需要 scene 与 orders: %s" % first)
        rows = raw[1:]
    else:
        keys = FIELDS
        rows = raw

    jobs = []
    for lineno, line in rows:
        cells = [cell.strip() for cell in next(csv.reader([line], delimiter=delimiter))]
        if len(cells) < len([k for k in keys if k]):
            raise BatchError("第 %d 行列数不够: %s" % (lineno, line))
        job = dict(zip(keys, cells + [""] * (len(keys) - len(cells))))
        if not job.get("scene"):
            raise BatchError("第 %d 行没有 scene" % lineno)
        if not job.get("orders"):
            raise BatchError("第 %d 行没有 orders" % lineno)
        for key, default in DEFAULTS.items():
            if job.get(key) in (None, ""):
                job[key] = default
        try:
            job["seed"] = int(job["seed"])
        except (TypeError, ValueError):
            raise BatchError("第 %d 行 seed 不是整数: %s" % (lineno, job.get("seed")))
        job["orders_path"] = resolve_orders(job["orders"], path.parent)
        job["line"] = lineno
        jobs.append(job)
    return jobs


def latest_run_dir(instance):
    if not instance.runs.is_dir():
        return None
    runs = [path for path in instance.runs.iterdir() if path.is_dir()]
    if not runs:
        return None
    return max(runs, key=lambda path: path.stat().st_mtime)


def read_json(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return None


def round_result(instance, scene, timeout_extra):
    """等这一局的计时归零；返回 (score, seconds)。状态文件停更 30 秒算实例掉线。"""
    started = time.monotonic()
    last_change = started
    last_mtime = 0
    limit = 0
    score = None
    saw_scene = False
    saw_running = False

    while True:
        now = time.monotonic()
        if not instance.game_running():
            raise BatchError("游戏进程已退出")
        try:
            mtime = instance.state_path.stat().st_mtime
        except OSError:
            mtime = 0
        if mtime and mtime != last_mtime:
            last_mtime = mtime
            last_change = now
        elif now - last_change > 30:
            raise BatchError("30 秒没有新的 state_update（实例掉线）")

        state = instance.state()
        if state:
            if state.get("scene") == scene:
                saw_scene = True
                if state.get("score") is not None:
                    score = state["score"]
                timer = state.get("timer") or {}
                if not limit:
                    limit = timer.get("limit") or timer.get("round") or 0
                left = timer.get("left")
                if left is not None and left > 1:
                    saw_running = True
                if saw_running and left is not None and left <= 0.2:
                    time.sleep(2.0)
                    final = instance.state() or {}
                    if final.get("scene") == scene and final.get("score") is not None:
                        score = final["score"]
                    return score, now - started
            elif saw_scene and now - started > 5:
                # 计时结束后游戏可能切回结算/菜单；保留最后看到的分数。
                return score, now - started

        elapsed = now - started
        if elapsed > max(180, (limit or 240) + timeout_extra):
            raise BatchError("等回合结束超时（%.0f 秒，limit=%s）" % (elapsed, limit))
        time.sleep(0.5)


def run_job(instance, job, args, attempt):
    scene = job["scene"]
    map_kitchen = REPO_ROOT / "config" / "maps" / scene / "kitchen.json"
    map_recipes = REPO_ROOT / "config" / "maps" / scene / "recipes.json"
    if map_kitchen.is_file():
        instance.kitchen = map_kitchen
    elif args.kitchen:
        instance.kitchen = Path(args.kitchen)
    if map_recipes.is_file():
        instance.recipes = map_recipes
    elif args.recipes:
        instance.recipes = Path(args.recipes)
    instance.orders = job["orders_path"]
    instance.write_orders(job["orders_path"], job["seed"])
    instance.write_config({
        "window": args.window,
        "frontend_press": "true",
        # 08 的批跑用 Linux 侧动作驱动；WorkOrder 替身会被 ActionDriver 跳过，
        # 而 work=auto 分支又不会调用 ActionDriver，所以这里必须保持 off。
        "work": "off",
    })

    if not instance.game_running():
        instance.start_game(timeout=args.start_timeout)
    # 菜谱只在新的游戏进程里刷新。刚启动还停在标题画面时直接 load；已经停在
    # 某个关卡里（上一批留下的进程）就先重启，避免拿旧菜谱跑新关卡。
    if instance.current_scene != scene:
        current = instance.scene()
        if current not in (None, "", "StartScreen", "Loading"):
            instance.restart_game(timeout=args.start_timeout)
        instance.wait_pads(2, timeout=args.start_timeout)

    instance.start_supervisor(strategy=job["strategy"], nav=job["nav"],
                              dispatch=job["dispatch"])
    instance.wait_bridge_link()
    instance.write_config({"frontend_press": "false"})
    time.sleep(6)  # 插件每 5 秒重读配置

    if instance.current_scene == scene:
        instance.restart_round(scene, timeout=args.load_timeout)
    else:
        instance.set_scene(scene, timeout=args.load_timeout)

    log("  实例 %d 第 %d 行 %s：开跑（attempt %d）"
        % (instance.index, job["line"], scene, attempt + 1))
    score, seconds = round_result(instance, scene, args.timeout)
    run_dir = latest_run_dir(instance)
    metrics = run_dir / "metrics.json" if run_dir else None
    nav_metrics = run_dir / "nav_metrics.json" if run_dir else None
    if metrics is not None and not metrics.is_file():
        metrics = None
    if nav_metrics is not None and not nav_metrics.is_file():
        nav_metrics = None
    return {
        "scene": scene,
        "strategy": job["strategy"],
        "nav": job["nav"],
        "dispatch": job["dispatch"],
        "orders": str(job["orders_path"]),
        "seed": job["seed"],
        "instance": instance.index,
        "score": "" if score is None else score,
        "status": "ok" if score is not None else "failed",
        "seconds": "%.1f" % seconds,
        "run_dir": str(run_dir or ""),
        "metrics": str(metrics or ""),
        "nav_metrics": str(nav_metrics or ""),
        "note": "" if attempt == 0 else "retry %d" % attempt,
    }


FIELDNAMES = ["scene", "strategy", "nav", "dispatch", "orders", "seed", "instance",
              "score", "status", "seconds", "run_dir", "metrics", "nav_metrics", "note"]


def write_row(writer, handle, row, lock):
    with lock:
        writer.writerow(row)
        handle.flush()


def run_batch(args):
    jobs = read_manifest(Path(args.manifest))
    if args.only:
        jobs = jobs[:args.only]
    if args.repeat > 1:
        jobs = jobs * args.repeat

    if args.dry_run:
        log("清单 %s：%d 局，%d 个实例，窗口 %s"
            % (args.manifest, len(jobs), args.instances, args.window))
        for job in jobs:
            log("  第 %d 行 scene=%s strategy=%s nav=%s dispatch=%s seed=%d orders=%s"
                % (job["line"], job["scene"], job["strategy"], job["nav"],
                   job["dispatch"], job["seed"], job["orders_path"]))
        return 0

    instance_mod.require_lock()
    instances = [instance_mod.Instance(
        k, window=args.window, kitchen=args.kitchen, recipes=args.recipes,
        orders=jobs[0]["orders_path"], workers=args.workers)
        for k in range(1, args.instances + 1)]

    try:
        for inst in instances:
            log("实例 %d：准备（窗口 %s，端口 %d）" % (inst.index, args.window, inst.port))
            inst.prepare()
            if not inst.game_running():
                inst.start_game(timeout=args.start_timeout)
    except Exception:
        for inst in instances:
            inst.stop()
        raise

    output = Path(args.out)
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = threading.Lock()
    work = queue.Queue()
    for job in jobs:
        work.put((job, 0))
    rows = []

    with open(output, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDNAMES)
        writer.writeheader()
        handle.flush()

        def worker(index):
            inst = instances[index]
            while True:
                try:
                    job, attempt = work.get_nowait()
                except queue.Empty:
                    return
                try:
                    row = run_job(inst, job, args, attempt)
                    rows.append(row)
                    write_row(writer, handle, row, lock)
                    log("  实例 %d 第 %d 行：score=%s status=%s %s 秒"
                        % (inst.index, job["line"], row["score"], row["status"],
                           row["seconds"]))
                except Exception as exc:  # 实例掉线/加载失败都走重排
                    log("  实例 %d 第 %d 行失败（attempt %d）：%s"
                        % (inst.index, job["line"], attempt + 1, exc))
                    inst.stop()
                    if attempt + 1 < args.retries:
                        work.put((job, attempt + 1))
                    else:
                        row = {
                            "scene": job["scene"], "strategy": job["strategy"],
                            "nav": job["nav"], "dispatch": job["dispatch"],
                            "orders": str(job["orders_path"]), "seed": job["seed"],
                            "instance": inst.index, "score": "", "status": "failed",
                            "seconds": "", "run_dir": "", "metrics": "",
                            "nav_metrics": "", "note": str(exc)[:200],
                        }
                        rows.append(row)
                        write_row(writer, handle, row, lock)
                finally:
                    work.task_done()

        threads = [threading.Thread(target=worker, args=(i,), daemon=True)
                   for i in range(len(instances))]
        for thread in threads:
            thread.start()
        work.join()
        for thread in threads:
            thread.join()

    if not args.keep:
        for inst in instances:
            inst.stop()

    ok = sum(1 for row in rows if row["status"] == "ok")
    log("完成：%d/%d 局成功，结果 %s" % (ok, len(jobs), output))
    return 0 if ok == len(jobs) else 1


def build_parser():
    parser = argparse.ArgumentParser(
        description="多开游戏实例批跑（真实游戏；先取 /tmp/wellcooked-game.lock）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("manifest", help="CSV/TSV 实验清单")
    parser.add_argument("--instances", type=int, default=3, help="并行实例数（默认 3）")
    parser.add_argument("--window", default="640x360",
                        help="窗口 WxH 或 full（对照全尺寸实例时用 full）")
    parser.add_argument("--out", default="results.csv", help="结果 CSV（默认 results.csv）")
    parser.add_argument("--kitchen", default=str(instance_mod.DEFAULT_KITCHEN))
    parser.add_argument("--recipes", default=str(instance_mod.DEFAULT_RECIPES))
    parser.add_argument("--workers", type=int, default=2, help="每实例厨师数（默认 2）")
    parser.add_argument("--timeout", type=int, default=90, help="回合结束后额外等待秒数")
    parser.add_argument("--start-timeout", type=int, default=240, help="游戏启动等待秒数")
    parser.add_argument("--load-timeout", type=int, default=120, help="进关等待秒数")
    parser.add_argument("--retries", type=int, default=3, help="单局最多尝试次数")
    parser.add_argument("--only", type=int, default=0, help="只跑清单前 N 行")
    parser.add_argument("--repeat", type=int, default=1, help="清单重复几遍（连跑用）")
    parser.add_argument("--keep", action="store_true", help="跑完不关实例")
    parser.add_argument("--dry-run", action="store_true", help="只解析清单")
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.instances < 1 or args.instances > 9:
        parser.error("--instances 必须在 1..9")
    if args.repeat < 1:
        parser.error("--repeat 必须 >= 1")
    args.window = instance_mod.parse_window(args.window)
    try:
        return run_batch(args)
    except (BatchError, instance_mod.InstanceError, OSError, ValueError) as exc:
        print("batch: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
