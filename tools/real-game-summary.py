#!/usr/bin/env python3
"""Summarize and assert one WellCooked real-game run.

Reads only artifacts owned by the runner: the newest Scheduler run under
<runtime>/runs, Worker logs, navd.log, the final game state, and the plugin
log slice captured for this run.  Exit 0 only when the real-game acceptance
invariants hold.
"""
import argparse
import json
import pathlib
import re
import sys


def rows(path):
    out = []
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if line:
                    try:
                        out.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass
    except OSError:
        pass
    return out


def read(path):
    try:
        return pathlib.Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def all_runs(runtime):
    """一个 runtime 就是一局；Scheduler 每次被 Supervisor 拉起都会新建一个 run 目录。"""
    return sorted((runtime / "runs").glob("*"), key=lambda p: p.stat().st_mtime)


def norm_id(value):
    """Scheduler uses order_001; the game state uses 1.  Both mean one order."""
    text = str(value or "")
    match = re.search(r"(\d+)$", text)
    return str(int(match.group(1))) if match else text


def order_rows(events, final_state):
    arrived = {}
    completed = {}
    rejected = {}
    for event in events:
        kind = event.get("event")
        order_id = str(event.get("order_id", ""))
        if not order_id:
            continue
        key = norm_id(order_id)
        if kind == "order_arrived":
            arrived[key] = event
        elif kind == "order_completed":
            completed[key] = event
        elif kind == "order_rejected":
            rejected[key] = event

    live = {}
    for item in final_state.get("orders") or []:
        if isinstance(item, dict):
            live[norm_id(item.get("id", ""))] = item

    result = []
    for key in sorted(arrived, key=lambda s: (len(s), s)):
        info = arrived[key]
        if key in completed:
            outcome = "completed"
            when = completed[key].get("t_ms")
        elif key in rejected:
            reason = rejected[key].get("reason", "rejected")
            outcome = "rejected: %s" % reason
            when = rejected[key].get("t_ms")
        elif key in live:
            outcome = "unfinished"
            when = None
        else:
            outcome = "expired/left"
            when = None
        result.append({
            "id": info.get("order_id", key),
            "dish": info.get("dish", "?"),
            "outcome": outcome,
            "t_ms": when,
            "remaining": (live.get(key) or {}).get("remaining"),
        })
    return result


def worker_rows(runtime, chefs):
    result = []
    for chef in range(chefs):
        text = read(runtime / "logs" / f"worker-{chef}.log")
        starts = len(re.findall(r"\bmacro_start\b", text))
        dones = len(re.findall(r"\bmacro_done\b", text))
        done_tasks = len(re.findall(r"\btask_done\b", text))
        failures = re.findall(r"\bmacro_failed\b.*?\bop=(\S+)\s+detail=(.*)$",
                              text, re.MULTILINE)
        reasons = {}
        for op, detail in failures:
            key = f"{op}: {detail.strip()}"
            reasons[key] = reasons.get(key, 0) + 1
        result.append({
            "chef": chef,
            "starts": starts,
            "dones": dones,
            "tasks_done": done_tasks,
            "failures": len(failures),
            "reasons": reasons,
        })
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--plugin-log", required=True)
    parser.add_argument("--takeover", required=True)
    parser.add_argument("--chefs", type=int, required=True)
    parser.add_argument("--scene", required=True)
    args = parser.parse_args()

    runtime = pathlib.Path(args.runtime)
    try:
        state = json.loads(read(args.state))
    except json.JSONDecodeError:
        state = {}
    plugin = read(args.plugin_log)
    takeover = read(args.takeover)
    runs = all_runs(runtime)
    run = runs[-1] if runs else None
    events = [e for r in runs for e in rows(r / "events.jsonl")]
    decisions = [d for r in runs for d in rows(r / "decisions.jsonl")]

    orders = order_rows(events, state)
    completed = [o for o in orders if o["outcome"] == "completed"]
    completed_dishes = sorted({o["dish"] for o in completed})
    arrived_dishes = sorted({o["dish"] for o in orders})
    workers = worker_rows(runtime, args.chefs)
    nav_plans = len(re.findall(r"\bnav_plan\b", read(runtime / "logs" / "navd.log")))
    released = len(re.findall(r"\bnav_plan\b.*\breleased=1(?:\s|$)",
                              read(runtime / "logs" / "navd.log")))
    dispatch = [d for d in decisions if d.get("kind") == "dispatch"]
    joined = sorted({int(x) for x in re.findall(
        r"游戏已自己加入 pad \d+ → 配置玩家位 \d+（实际玩家位 (\d+)）", plugin)})
    invalid_cast = plugin.count("InvalidCastException")
    local_workorder = plugin.count("工序:")
    auto_mode = "工作模式 auto（系统控制：只执行 Bridge 动作）" in plugin
    frontend_off = re.search(r"^frontend_press\s*=\s*false\s*$", takeover,
                             re.MULTILINE) is not None
    score = float(state.get("score") or 0)

    checks = {
        "score>0": score > 0,
        "order_completed": len(completed) > 0,
        "completed_orders_have_dish": all(o["dish"] and o["dish"] != "?"
                                          for o in completed),
        "macro_chain_each_worker": all(w["starts"] > 0 and w["dones"] > 0
                                       for w in workers),
        "navd_released_plan": released > 0,
        "auto_mode": auto_mode,
        "no_local_workorder": local_workorder == 0,
        "all_pads_joined": joined == list(range(1, args.chefs + 1)),
        "frontend_press_off": frontend_off,
        "no_manual_input": auto_mode and local_workorder == 0 and frontend_off
                           and joined == list(range(1, args.chefs + 1)),
        "invalid_cast_zero": invalid_cast == 0,
    }
    failed = [name for name, ok in checks.items() if not ok]

    print("=== real-game summary: scene=%s chefs=%d ===" % (args.scene, args.chefs))
    print("score=%g  run=%s" % (score, ",".join(r.name for r in runs) or "(none)"))
    print("orders: %d arrived, %d completed; dishes completed=%s"
          % (len(orders), len(completed), ",".join(completed_dishes) or "-"))
    for order in orders:
        when = "%.1fs" % (order["t_ms"] / 1000.0) if order["t_ms"] is not None else "-"
        extra = ""
        if order["remaining"] is not None:
            extra = " left=%.1fs" % float(order["remaining"])
        print("  #%s %-22s %-15s t=%-8s%s"
              % (order["id"], order["dish"], order["outcome"], when, extra))
    if not orders:
        print("  (no Scheduler order_arrived events)")

    print("arrived dishes: %s" % (", ".join(arrived_dishes) or "-"))
    print("workers:")
    for worker in workers:
        print("  worker-%d macro_start=%d macro_done=%d task_done=%d macro_failed=%d"
              % (worker["chef"], worker["starts"], worker["dones"],
                 worker["tasks_done"], worker["failures"]))
        for reason, count in sorted(worker["reasons"].items()):
            print("    %dx %s" % (count, reason))
    print("navd: plans=%d released=%d  dispatch_decisions=%d" %
          (nav_plans, released, len(dispatch)))
    print("pads_joined=%s  auto=%s  local_workorder=%d  invalid_cast=%d  frontend_off=%s"
          % (joined, auto_mode, local_workorder, invalid_cast, frontend_off))
    print("checks: " + " ".join("%s=%s" % (k, "ok" if v else "FAIL")
                                for k, v in checks.items()))
    print("RESULT=%s" % ("PASS" if not failed else "FAIL"))
    if failed:
        print("failed: " + ", ".join(failed))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
