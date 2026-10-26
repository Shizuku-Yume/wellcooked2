# 09: 实机 _2 两个厨师系统控制完成一单

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** b0129232ece42cb481a5821c2688a86a0a631d7e
**Base branch:** kitchen-control/05-ring-and-pa-baseline
**Started:** 2026-10-25T16:50:00+08:00
**Status:** paused

**Delivers:** 插件 `work=auto` 关掉 `WorkOrder.cs` 的替身逻辑后，`s_wellcooked_demo_2` 上两个
厨师完全由 Linux 侧控制，无手动输入完成至少一单；`_1`（没有干净盘堆）上洗盘让盘子循环起来。

**Blocked by:** 05, 06, 07
**Files:** `plugin/{WorkOrder,SmokePlugin,ActionDriver}.cs`、`tools/run-real-game.sh`、
`scripts/dev-cycle.sh`、`.workflow/tasks/os-core/`（只更新 W10 的进度记录）

**Read first:** `.workflow/specs/kitchen-control.md` 的"L2 宏动作"末段、"资源规则"、W9 与 W11；
`.workflow/tasks/game-bridge/05-two-chefs-complete-orders.md`；
`.workflow/tasks/game-bridge/13-diy-demo-level.md`
**Covers:** W9, W11

**Check:** 实机跑 `tools/run-real-game.sh`（`_2`、2 厨师、`work=auto`）→ 局内得分 > 0，
Worker 日志能对出一单完整的 `macro_*` 链，`navd` 日志有对应的 `path` 放行，期间没有手柄输入；
再在 `_1` 上跑一局 → 出现 `wash` 工序、没有订单因缺盘超时，`decisions.jsonl` 里 `cook` 前置先于
等长的非烹饪工序派出。需要游戏。

- [ ] 系统控制下 `WorkOrder.cs` 的选厨师/走工序/1.4 m 兜底不运行
- [ ] 实机发现的协议或完成条件偏差，回写 spec 对应段落
- [ ] 跑通后在 os-core W10 与 game-bridge/05 的进度里记一笔实测结果

## Progress

Dirty at start: none。

**前置核验（2026-10-25）**：`git fetch -q origin` 后逐个查 `git ls-tree -r --name-only
origin/<分支> .workflow/done/kitchen-control/`：
- `kitchen-control/05-ring-and-pa-baseline` → `05-ring-and-pa-baseline.md` 存在。
- `kitchen-control/06-macro-actions` → `06-macro-actions.md` 存在。
- `kitchen-control/07-assign-cost-and-policies` → `07-assign-cost-and-policies.md` 存在。
三个前置都已有 done 归档，无需 600 s 轮询。

从 `origin/kitchen-control/05-ring-and-pa-baseline`（b012923）开
`kitchen-control/09-real-game-two-chefs`；随后按通道规则 `--no-ff` 并入 06、07。

**Check before:** 本分支（仅 05）上 `cd bridge && make test` = 13 个套件、0 失败；
`tools/run-real-game.sh` 仍是 game-bridge 的 W8 检查（只数 hello/state_update，不驱动作），
插件 `work=auto` 走的是 `WorkOrder` 替身，Linux 侧没有真动作链路，任务 Check 尚不能执行。

**2026-10-25T17:21 暂停交接（实机联调中，未完成）**

已完成并提交：
- `5eca602` 并入 06 宏动作执行器（冲突：fake_plugin 的场景表/imports/WorldSim 字段取并集；run-bridge 场景表取并集）。
- `7bdafaa` 并入 07 派工代价与锅盘规则（冲突：child.h/child.c/supervisor 取并集；scheduler 以 07 的
  assign 派发为基底，补回 04 的 `avoid_chef`/`blocked_hold`/BLOCKED 回队与按 (厨师,工序) 的避让）。
  另修 07 原有 bug：`dispatch_ready` 的 `idle[i]=i` 未压缩空闲表，第二个厨师忙时 `idle[0]` 读到未初始化值；
  改为 `idle[idle_count++]=i`。修后 `nav-stuck` fake 场景通过（此前 `task_blocked none`）。
- `cd bridge && make test`：15 套件、0 失败（05 基线为 13 套件）。
- `bash tools/run-bridge-scenarios.sh --only nav-stuck,os-loop`：2/2 PASS；`nav-ring`、`macro` 也各 PASS。

未提交（本交接一并提交）：
- `plugin/Takeover.cs`：`work=auto` 改为系统控制（只执行 Bridge 动作，`WorkOrder` 替身在
  auto 下完全不运行），`work=local` 才跑旧工序状态机；`ActionDriver` 加 `OwnedCount`。
- `bridge/src/navd.c`：`write_plan` 内容变化时打 INFO `nav_plan chef seq status waypoints released target_seq`，
  供任务 Check 对 `path` 放行。
- `bridge/src/scheduler.c`：空 menu 的 orders 文件记 `menu_empty` 且不造配置订单；配套
  `config/orders.world.json`（真游戏只用插件上报的世界订单）。

**真游戏实测证据（2026-10-25，_2，2 厨师，`work=auto`，supervisor 全树）**：
运行目录 `logs/real-game/manual2-171656`（保留）。Bridge `hello` 成功，`state_update chefs=2
orders=3 resources=3`；插件日志出现 `takeover: 工作模式 auto（系统控制：只执行 Bridge 动作）`，
没有 `工序:` 日志。Scheduler 收世界单 `order_001/002`，开 `order_002` 后只派了
`fetch:BurritoChicken`（decision 里 `ready_tasks=1`），worker-0 `macro_start goto`
→ `macro_done waited_ms=1253` → `macro_start pickup` → `macro_done 87 ms`，navd 有
`nav_plan ... status=arrived`。第二厨师始终空闲，`score=0`，没有 `order_completed`。

**卡点 / 下一步**：同一订单的另外两个就绪 `fetch:Rice`、`fetch:Tortilla` 没进候选
（`build_candidates` 只返回 1 个，且无 `task_no_station`/`task_failed`）；先加候选构建的
DEBUG/探针查 `task_dispatch_ready`、`task_station_index`、`station_reachable` 哪一步把它们刷掉，
修后重跑 _2。随后才做：
1. 把 `tools/run-real-game.sh` 扩成 supervisor 全树 + 场景参数 + 自动断言
   （score>0、`order_completed`、macro 链、navd `nav_plan`、无手动手柄、`_1` 的 wash/cook 优先）。
2. `_1` 洗盘实机验收；`_1` 的 `plate:*` 无 `plate_stack` 工位（`recipe_no_station`）要按实机结果决定
   是否补资源/宏映射，并把偏差回写 spec。
3. 更新 os-core W10 与 game-bridge/05 的进度、spec、decisions；归档、PR（base `kitchen-control/05-ring-and-pa-baseline`）。

实机结束已还原 `BepInEx/wellcooked-takeover.txt` 与 `wellcooked-orders.json`、删除 command 文件，
并删除 `/tmp/wellcooked-game.lock`；游戏与 supervisor 已停止。

**2026-10-25T18:00 继续联调：** `build_candidates()` 现在对因当前空闲厨师所在连通区不可达而跳过的工序写 `task_station_unreachable`（带 station 与 compact idle chef cell），用于区分工序依赖未就绪和 `_2` 两条厨房带的站位不可达；`run-real-game.sh` 增加 `REAL_GAME=1 RUNTIME_DIR=... SCENE=... CHEFS=...` 的 supervisor 全树断言入口（order_completed、双 worker 宏链、navd 放行、无手动输入）。实机重跑前仍需锁定共享游戏资源；未将轻量 W8 证据冒充 W9/W11。

**2026-10-25T21:10 pause handoff:** FACE stable station mapping and a `dispenser-4` coordinate regression test are implemented. `cd bridge && make test` last passed 15 suites with 0 failures; the suite is being rerun after these edits. Shell syntax and diff checks passed earlier. One setup run reached `_2` with exactly two chefs; its observed score was -390 and that is not acceptance evidence.

The supported branch script `scripts/build-plugin.sh` builds/deploys only `WellCooked.Smoke.dll`, not the Bridge integration plugin required for OS-driven work. A direct integration-project build also failed because this checkout lacks `plugin/Libs2017/*.dll`. No alternate plugin build path was used. A later dev-cycle restart failed after two attempts because `BepInEx/LogOutput.log` never appeared; `scripts/run-game.sh` explicitly returned `Steam 没在运行，先启动 Steam。` The remaining requirement is paused on external Steam/game availability and missing plugin references—not marked plan-blocked.

No Supervisor acceptance run occurred. There is no evidence for positive score, `order_completed`, both workers' completed macro chains, navd path, or absence of manual input. `_1` wash/cook-priority and the plate/wash gap are untested. Do not count demo2/two-chef setup or W8 Bridge traffic as acceptance.

Game configs were verified restored byte-for-byte against `/tmp/kc09-backup`; `wellcooked-command.txt` is absent. No game, Bridge, or Supervisor process remains; the lock was already absent at handoff (previous owner record was this task's `A 09` session).

**2026-10-26 恢复：** 前置06修复分支 `origin/kitchen-control/06-macro-actions-repair-A` 已推送且含 `.workflow/done/kitchen-control/06-macro-actions.md`；07 的归档也已在 `origin/kitchen-control/07-assign-cost-and-policies` 核验。06 修复明确采用 recipe DAG 前驱绑定 `source_station_id`，cook 完成后保留锅令牌到 plate 终态，并覆盖冷菜及间接前驱。合并前置06并在当前分支保留 07 的匈牙利派工、订单策略、`avoid_chef`/`blocked_hold` 与 sink/wash 逻辑。`bridge && make` 已通过；`make test` 与 `macro,os-loop,nav-stuck` 场景均通过。继续恢复原 stash 后再验证全链路与实机验收。

**2026-10-26 实机 _2 复验：** runtime `logs/real-game/20261026-033629-_2-retry` 的 `events.jsonl` 与 `decisions.jsonl` 均为 0 字节；插件新状态仍为 `scene=Ident`、`chefs=[]`，worker 无 `macro_start`，navd 无 `nav_plan`，故此轮没有进入关卡或派工，不是验收通过。脚本退出码 1：等待两个虚拟玩家位的日志满 180 s 后报“未能在本次运行中挂接两个虚拟玩家位”。插件日志实际已写入玩家位 1、2 的挂接行及自动前端 A 按下/松开；根因是游戏启动时重建/截断 `LogOutput.log`，脚本仍按旧文件长度 seek 到新文件末尾，漏读本次行。已修复日志截断后偏移归零。此轮结束时仅停止本任务进程，三个配置文件与 `/tmp/kc09-backup` 逐个 `cmp` 一致，A 09 锁已释放。继续实机验收；`_2` 与 `_1` Check 仍未完成。

**2026-10-26 _2 启动脚本复验：** `logs/real-game/20261026-034353-_2-final` 的 Supervisor 已启动，Steam 启动命令返回后脚本因 `set -u` 报 `tools/run-real-game.sh: line 177: BEPINEX_LOG: 未绑定的变量` 退出；没有进入场景、没有调度证据。已在脚本声明该日志路径。游戏进程由本任务启动，已停止；三个配置逐个与 `/tmp/kc09-backup` 比对一致，A 09 锁已释放。

**2026-10-26 _2 run4：** Supervisor/Scheduler/Bridge/navd/两个 Worker 均由此任务启动且由其停止；锁 owner 为 `A 09`。`logs/real-game/20261026-035157-_2-run4/runs/20261026-035157/{events,decisions}.jsonl` 均 0 字节；`worker-0.log` 只有启动、初始 `chefs=0` 与 SIGTERM，无宏链；`navd.log` 只有启动与初始 `chefs=0`，无 `nav_plan`。新鲜游戏状态为 `scene=Ident, chefs=[]`。插件显示 `work=auto`、两个 pad 的 A 按键切换，并在 180 s 后退出“未能在本次运行中挂接两个虚拟玩家位”；本次无 `WorkOrder` 工序日志，也无手柄来源的键盘/用户输入记录。三个配置均恢复并与备份逐一相同，锁已释放。实机阻塞证据是启动后仍停在 Ident，前端按键自动注入未促成两个加入玩家位；不构成 `_2` 或 `_1` Check 通过。

**2026-10-26 _2 coordinate diagnosis:** In live runtime `logs/real-game/20261026-042210-_2-live`, chef0 was sent to `dispenser-4` at (-3.0,-3.6); its world position reached about (-3.08,-5.07), navd reported arrived and face acked, but pickup timed out waiting for `holding=BurritoChicken`. This is a failed run, not acceptance. Navd waypoint endpoint and station-interaction geometry are now documented separately; plugin path endpoint uses an actual-coordinate 0.3 m bound (turn-corner skip remains 0.15 m), with runnable .300/.301 boundary tests. `test_stand` now asserts the nearest stand cell for each station in all four maps is within 1.2 m. This exposed two sink coordinates with 0.01 m transform noise (`_2` -7.19 and `_3` -2.39), whose nearest grid stand measured 1.20004 m; exporter now snaps station positions only when within 0.02 m of the sampled grid, and those map positions are normalized to -7.2/-2.4. Exporter tests pass; `cd bridge && make test` passes 15 suites, 0 failures; path-arrival tests pass at 0.3 m and reject 0.301 m.

The failed `_2` Supervisor tree owned by this task was stopped via PID 1416010; runtime status now records all five children stopped. No Overcooked/Steam process, game config, or lock was touched. `/tmp/wellcooked-game.lock` reports `31B 24m ago`; the owner is not this task, so it was not acquired, removed, or modified. Remaining: review the patch, obtain this task's lock by the shared 120-second retry rule, build/deploy this branch first, repeat `_2` through the two-stage join/load/startup flow until strict order/score evidence passes, then run `_1` wash/cook-priority acceptance.

**2026-10-26 offline interaction diagnosis/fix:** Fresh `_2` logs confirm `face` ack at 2.5° and repeated `interact` acks are receipt/registration only; `worker-0` failed `pickup` on `holding` timeout and plugin state held remained empty. Worker had kept interact intent for only 250 ms, while ActionDriver's documented cadence is an immediate 250 ms pulse plus a retry beginning 600 ms later. The worker therefore stopped before the retry edge. `WORKER_INTERACT_WINDOW_MS` now derives from a shared timing contract and is 1000 ms; `test_worker_timing` proves the old 250 ms window fails and 1000 ms covers both pulses. `cd bridge && make test` now passes 16 suites, 0 failures; PathArrival and exporter tests pass. This is not yet real-game acceptance: lock owner remains another channel and the existing game process must not be touched until that channel releases the lock.

**2026-10-26 fresh-state false positive diagnosis:** `logs/real-game/20261026-085345-_2-interactfix` never entered the target scene: `LogOutput.log` shows the `LoadLevel(s_wellcooked_demo_2)` request but immediately reports `chefs=0`, and no subsequent kitchen-state initialization, joined-pad, order, or world snapshot records occurred. Worker/scheduler logs contain only startup with `chefs=0/orders=0`. The earlier runner gate could accept a stale target state in the same one-second mtime bucket and only sampled readiness once. `tools/run-real-game.sh` now compares nanosecond mtimes and requires five consecutive seconds of target-scene/exact-chef-count state before starting Supervisor. Fixture validation rejects old-mtime and a single transient sample, accepts five stable samples. After the user's UI-blocked notice, the freshgate run `20261026-095500-_2-freshgate` was explicitly interrupted; owned launcher PID 251554 and game tree 251852→251628 were stopped, configs matched backup, command file absent, and A09 lock released. This is interruption, not test failure. The existing real-game startup still consistently reaches pad join but game load itself is currently not observed; continue diagnosis before claiming acceptance.

**2026-10-26T10:15 pause handoff:** 外部/其它启动链仍持有游戏资源，不触碰。`/tmp/wellcooked-game.lock` 是普通文件（0644，内容 `A 09`），mtime 09:23:08；游戏树 PID 319826/319981/319983/320059 分别于 09:23:26–09:23:29 启动，PPID 回溯到 Steam UI PID 12058，cwd 为实际 Steam 游戏目录，cmdline 是 SteamLaunch/Proton/Overcooked2.exe；没有 `run-real-game.sh`、`scripts/run-game.sh`、Supervisor 或本 worktree 启动链。当前分支最新 task runtime `logs/real-game/20261026-095500-_2-freshgate` 在 09:06，`/tmp/kc09-diagnostic-backup` 在 09:23:22，故该树不是本轮 task09 runner。绝不删除锁、停止游戏或修改游戏配置；等待外部树释放后再按共享锁规则取得。

离线收尾已验证：`bridge && make test` 16 套件、0 失败；`bridge && make test-san` 16 套件、0 失败；`bash tools/run-bridge-scenarios.sh --only nav-stuck,os-loop,nav-ring,macro` 4/4 PASS；`python3 scripts/test_export_kitchen.py` 3/3 OK；`bash -n tools/run-real-game.sh` OK。PathArrival 边界测试产物存在但本机无 dotnet/runtime，直接运行报 `You must install .NET`，未伪称通过。`git diff --check` clean。当前 dirty paths 保持任务本身既有改动，未触碰外部游戏树。

仍未完成：真实 `_2` 必须有 score>0、`order_completed`、两个 worker 完整宏链、navd path、无手动输入；随后真实 `_1` wash/cook priority 且无订单超时。未移动 done archive、未创建完成 PR；本任务暂停而非完成。
