#!/usr/bin/env python3
"""compare-runs.py - 比较两局 runs/<ts> 的完成、过期与平均等待。

用法:
    python3 tools/compare-runs.py RUN_DIR_A RUN_DIR_B
    python3 tools/compare-runs.py --json RUN_DIR_A RUN_DIR_B

每个 RUN_DIR 里必须有 metrics.json；events.jsonl 可选。completed/failed 来自
metrics.json，expired 由 events.jsonl 的 lease_expired 计数，avg_wait_ms 用各资源
wait_ms / waits 的加总（避免按资源平均时被小资源稀释），dispatch 取 metrics.json
里的策略计数。脚本只读，不改运行目录。
"""

import argparse
import json
import sys
from pathlib import Path


def load_run(path):
    run_dir = Path(path)
    if run_dir.is_file():
        metrics_path = run_dir
        run_dir = metrics_path.parent
    else:
        metrics_path = run_dir / "metrics.json"
    metrics = json.loads(metrics_path.read_text())
    events = []
    events_path = run_dir / "events.jsonl"
    if events_path.exists():
        for line in events_path.read_text(errors="replace").splitlines():
            if line.strip():
                try:
                    events.append(json.loads(line))
                except ValueError:
                    pass
    decisions = []
    decisions_path = run_dir / "decisions.jsonl"
    if decisions_path.exists():
        for line in decisions_path.read_text(errors="replace").splitlines():
            if line.strip():
                try:
                    decisions.append(json.loads(line))
                except ValueError:
                    pass
    policies = sorted({d.get("policy") for d in decisions
                       if d.get("kind") == "dispatch" and d.get("policy")}
                      or {k for k, v in metrics.get("dispatch", {}).items() if v})
    expired = sum(1 for e in events if e.get("event") == "lease_expired")
    waits = sum((r.get("waits") or 0) for r in metrics.get("resources", []))
    wait_ms = sum((r.get("wait_ms") or 0) for r in metrics.get("resources", []))
    orders = metrics.get("orders", {})
    return {
        "run_dir": str(run_dir),
        "policies": policies,
        "completed": orders.get("completed", 0),
        "failed": orders.get("failed", 0),
        "expired": expired,
        "avg_wait_ms": round(wait_ms / waits, 1) if waits else 0.0,
        "waits": waits,
        "dispatch": metrics.get("dispatch", {}),
        "worker_restarts": metrics.get("worker_restarts", 0),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("runs", nargs="+", help="两个 runs/<ts> 目录（或 metrics.json）")
    parser.add_argument("--json", action="store_true", help="输出 JSON 数组")
    args = parser.parse_args(argv)
    if len(args.runs) != 2:
        parser.error("需要正好两个运行目录")
    rows = [load_run(path) for path in args.runs]
    if args.json:
        print(json.dumps(rows, ensure_ascii=False, indent=2))
        return 0
    print("%-12s %-10s %9s %7s %8s %12s %10s" %
          ("policies", "completed", "failed", "expired", "avg_wait_ms", "dispatch", "restarts"))
    for row in rows:
        print("%-12s %-10s %7s %8s %12s %10s %9s" %
              (",".join(row["policies"]) or "?",
               row["completed"], row["failed"], row["expired"], row["avg_wait_ms"],
               json.dumps(row["dispatch"], separators=(",", ":")),
               row["worker_restarts"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
