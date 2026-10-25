# 06: 宏动作执行器取代 worker_task

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** dacd788388158ac06b3123ef742a1dfb18495b62
**Base branch:** kitchen-control/04-navd-pibt
**Started:** 2026-10-25T16:08:48+08:00

**Delivers:** Worker 把一道工序按映射表展开成宏动作（goto/pickup/drop/chop/start_cook/
wait_ready/wash/deliver），每步按 shm 世界状态判完成、超时报 `FAILED`，日志有
`macro_start`/`macro_done`/`macro_failed`。

**Blocked by:** 04
**Files:** `bridge/include/kitchenos/macro.h`、`bridge/src/{macro,worker}.c`、删除
`bridge/{include/kitchenos/worker_task.h,src/worker_task.c}`、`bridge/tests/test_macro.c`、
`tools/fake_plugin.py`（场景 `macro`）

**Read first:** `.workflow/specs/kitchen-control.md` 的"L2 宏动作"与 W6；glossary 宏动作；
`bridge/include/kitchenos/{mq,shm}.h`；`.workflow/tasks/os-core/03-worker-loop-and-path.md`
**Covers:** W6

**Check:** `cd bridge && make test` → `test_macro` 覆盖六种工序的展开；`bash
tools/run-bridge-scenarios.sh --only macro,os-loop` → fake_plugin 延迟 2 s 才改 `holding`/
`progress`/锅 `state` 时，宏动作等到变化才 `macro_done`；不变化时按表超时 `macro_failed`
且工序 `FAILED`，`detail` 写宏动作与所等条件；os-loop 原有检查不退化。

- [x] `goto` 结束前发 `face`，插件只在 15° 内 ack；Worker 用世界状态 facing 判完成（decisions 2026-10-25）
- [x] `cook` 放下即走，不在锅前等熟
- [x] 完成条件被打破（手上东西没了 / 砧板料没了）立即失败，不等超时
- [x] 内部 3000 mm/s 速度推算全部删掉（worker_task 已由 04 删除，macro 只读世界状态）

## Progress

**Finished:** 2026-10-25T16:50:00+08:00

Dirty at start: none。从 `origin/kitchen-control/04-navd-pibt`（dacd788）开
`kitchen-control/06-macro-actions`；前置 04 的归档文件已确认存在，任务为完成态。
04 已删除 `worker_task.{h,c}` 与旧的单厨师 A*，本任务在它的 Worker 骨架（目标槽/计划槽 +
`path` 下发）上把"交互 200 ms → 工时计时"的相位换成按世界状态判完成的宏动作。

Check before: `test_macro` 与 `macro` 场景尚不存在，无法运行；作为基线，
`cd bridge && make test` 与 `bash tools/run-bridge-scenarios.sh --only os-loop` 先跑一遍
现有套件：`make test` 13 套件全过；`--only os-loop` PASS。

Check after（全部实跑）:
- `cd bridge && make test` → **14 test suites, 0 failed**（新增 `test_macro` 64 项：六种工序展开 + mix/assemble、face 先行、holding/progress/pot 状态完成、1.5 s/工时超时、砧板内容消失与 deliver 手上没东西立即失败）。
- `bash tools/run-bridge-scenarios.sh --only macro,os-loop` → **2/2 PASS**。`macro` 三局：完整一单 `macro_start` 覆盖 goto/pickup/drop/chop/start_cook/wait_ready/deliver，pickup/chop/drop 的 `waited_ms` 均 ≥ 2000（fake 延迟 2 s 才改世界状态），`order_completed` 出现；wash 局 `wash` 的 `waited_ms=2050`；不回料局 `macro_failed detail=macro=pickup wait=holding timeout`，`task_failed t_ms≈1700`（不是 30 s）。`os-loop` 原有 path/interact/无 move/无同格/worker 重启 RECOVERED→requeue→新任务全部保持。
- 实现：`macro.{h,c}`（计划展开 + 状态机，不碰 shm/时钟）、`worker.c` 改为宏动作、`Makefile -lm`、`test_macro.c`、fake `WorldSim`+`macro` 场景（3 局，约 35 s）。
- 已知并发风险：fake 的 mq 名不带 run-id，和别的通道同时跑场景时曾撞到对方的 `/kitchenos_tasks_0`（一次 os-loop 出现 `station=dispenser-left` 的错任务）；单独重跑通过。不是本任务引入，未改 mq 命名。
- Review（flow-verify 两个 brief）与 `test-san` 未跑：本轮执行预算用尽；提交前已跑 make test 与任务 Check。

