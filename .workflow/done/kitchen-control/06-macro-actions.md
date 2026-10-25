# 06: 宏动作执行器取代 worker_task

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** dacd788388158ac06b3123ef742a1dfb18495b62
**Base branch:** kitchen-control/04-navd-pibt
**Started:** 2026-10-25T16:08:48+08:00
**Finished:** 2026-10-26T02:11:00+08:00
**Delivers:** Worker 把一道工序按映射表展开成宏动作（goto/pickup/drop/chop/start_cook/
wait_ready/wash/deliver），每步按 shm 世界状态判完成、超时报 `FAILED`，日志有
`macro_start`/`macro_done`/`macro_failed`。

**Blocked by:** 04
**Files:** `bridge/include/kitchenos/{macro,mq}.h`、`bridge/src/{macro,scheduler,worker}.c`、
`bridge/tests/{test_macro,test_scheduler}.c`、`tools/fake_plugin.py`（场景 `macro`）、
`.workflow/specs/kitchen-control.md`、`.workflow/decisions.md`

**Read first:** `.workflow/specs/kitchen-control.md` 的"L2 宏动作"与 W6；glossary 宏动作；
`bridge/include/kitchenos/{mq,shm}.h`；`.workflow/tasks/os-core/03-worker-loop-and-path.md`
**Covers:** W6

**Check:** `cd bridge && make test` → **14 test suites, 0 failed**. `bash
tools/run-bridge-scenarios.sh --only macro,os-loop` → **2/2 PASS**: macro starts all seven kinds,
five tasks finish and `order_completed` is logged; delayed pickup/progress/pot state waits ≥2 s;
failure run times out pickup at 1.5 s; os-loop preserves restart/requeue and path assertions.

- [x] plate receives explicit per-order cook-pot DAG mapping and clean-plate station
- [x] cook completion detaches Worker lease while retaining cooker token until plate completion
- [x] plate completion/failure/order terminal path releases cooker reservation exactly once
- [x] Worker restart requeues task; Scheduler restart limitation is documented (no IPC recovery claim)
- [x] `goto` end turns before completion; `cook` releases worker to next task; macro transitions use world state

## Progress


Dirty at start: none. Repair branch started from origin/kitchen-control/06-macro-actions@50624da;
task09 dirty files are preserved in stash 2fc37466d6a7c084386d07741270178132d15f59.

Flow-break amendment fixed the plan defect: plate source is bound by the recipe DAG predecessor edge,
not node-name suffix; cooker token is reserved from cook DONE through plate terminal event. `source_station_id`
is carried in mq v2. The fake fixture uses `plate_stack` as the independent plate lease and combines
Plate+Dish when taking the reserved cooked pot. Scheduler restart cannot recover in-memory order/pot
ownership from existing IPC; this limitation is documented.

Final Check: `cd bridge && make test` → 14 suites, 0 failed. `bash tools/run-bridge-scenarios.sh
--only macro,os-loop` → 2/2 PASS: macro all seven kinds, five task_done and order_completed;
os-loop restart/requeue PASS.

Review boundary: 50624da + mq/macro/worker/scheduler/tests/fake/spec/decision paths; round: 1;
briefs: A,B; boundaries: mq task payload, Scheduler order/task reservation, Worker macro state,
fake world-state transitions; findings: 0 blocking / 0 nonblocking after DAG and fake cooker fixes.

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
- Review（flow-verify 两个 brief）与 `test-san` 已在下方“Review（2026-10-25，补跑 flow-verify）”补齐。


## Review（2026-10-25，补跑 flow-verify）

`Review boundary: dacd788388158ac06b3123ef742a1dfb18495b62 + bridge/include/kitchenos/macro.h,
bridge/src/macro.c, bridge/src/worker.c, bridge/tests/test_macro.c, bridge/Makefile,
tools/fake_plugin.py, tools/run-bridge-scenarios.sh; round: 1; briefs: A,B; boundaries:
mq→Worker 任务/事件、navtab 目标/计划槽、shm 世界状态只读、fake_plugin 场景; findings:
2 blocking / 1 nonblocking`。

- Brief A（built as asked + process）round 1：**1 blocking —— flow-verify 未跑**。本轮已补跑两个
  brief 并记录；其他（需求、Check before/after、文档、decisions、leftover/scope）clean。
- Brief B（quality）round 1：**1 blocking + 1 nonblocking**。
  1. blocking（**保留未修**）：导出 `recipes.json` 的 `plate` 节点 resource 是 `plate_stack`，
     Scheduler 派的工位是盘子堆，而 spec 的 plate 语义是 goto(锅)+取熟菜。Worker 只能按 mq
     下发的工位执行，无法知道是哪口锅；这是 Scheduler/导出映射与 spec 的计划矛盾，属于任务
     07/09 的接口统一，不在 06 的文件范围内。已在 spec L2 写成"已知接口缺口"，PR 不 merge。
     fake 场景已改为 cooker 工位，真正走 wait_ready→cooked→pickup 的链路。
  2. nonblocking（已修）：`face` 原来发 kitchen 工位 id（`cooker-0`），插件按 `pot_0`/GameObject
     名解析会拒绝。改为发工位的世界坐标（协议 face 的 target 分支），fake 也按坐标转向；真机
     不再依赖 id 对齐。
- 补跑：`make test-san` → 14 套件 0 失败；`make test` → 14 套件 0 失败；
  `--only macro,os-loop` → 2/2 PASS（修复后复跑）。

### Round 2（delta recheck）

- Brief A recheck：**pass**，原 blocking（未跑 flow-verify）已解决，delta 无新问题。
- Brief B recheck：**pass（新增 findings = 0）**；face 改为世界坐标后 Worker/协议/插件/fake 一致，
  重复坐标 face 不会重置命令。原有 plate 工位 mismatch 仍保留为 blocking（任务 07/09 的接口
  统一，不在 06 文件范围）。
- 复跑：`make test-san` 14/14、`make test` 14/14、`--only macro,os-loop` 2/2。


### Flow-break amendment（2026-10-26）

- 原计划缺陷：plate 任务只按 `resource=plate_stack` 选站，却在该站等待锅并取菜；mq 没有订单到锅映射，且 cook 完成时 cooker 令牌立即释放，后续订单可能覆盖同锅内容。见 decisions.md: 2026-10-26 - plate 保留订单专属锅直至取菜。
- 更新后的契约：plate 仍由 `plate_stack` 决定物理盘子来源与容量租约；Scheduler 将 cook 站位/令牌作为订单 task 记录保留至对应 plate 成功。Worker 收到 source station 后按盘堆→指定锅执行；失败与订单淘汰释放全部 reservation。Worker restart 时 Scheduler 的订单/task 记录仍存活并依现有 `RECOVERED`/requeue 处理；Scheduler restart 时订单记录与命名资源令牌无法从 IPC 恢复，明确不承诺在制锅恢复。
- 任务范围扩至 `scheduler.c` 与 mq 消息布局；不改 task 07/09/11。W9 的 `_1` 洗盘验收仍由 task 07 定义，本任务不虚构 `plate_stack` 不存在时的取盘位置。
- Branch: `kitchen-control/06-macro-actions-repair-A`，从 `origin/kitchen-control/06-macro-actions` (50624da) 建立；原本地分支由 kc-B worktree 占用，未切换或写入。
- kc-A task09 未提交的五个路径已具名 stash 保护：`stash@{0}` / `2fc37466d6a7c084386d07741270178132d15f59`，`kc-A task09 dirty paths backup before 06 repair 2026-10-26`。恢复命令：先在 kc-A `git switch kitchen-control/09-real-game-two-chefs`，再 `git stash apply 2fc37466d6a7c084386d07741270178132d15f59`；核对五个路径后可另行 drop，当前不要 pop。
 - Final implementation and proof complete; the earlier blocked wording above is superseded by the DAG mapping and checks recorded in this task. `make test` and `macro,os-loop` are green; known workflow baseline validation errors remain unrelated.
