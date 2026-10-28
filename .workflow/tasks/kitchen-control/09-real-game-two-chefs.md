# 09: 实机 _3 四个厨师系统控制完成一单

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** b0129232ece42cb481a5821c2688a86a0a631d7e
**Base branch:** kitchen-control/05-ring-and-pa-baseline
**Started:** 2026-10-25T16:50:00+08:00
**Status:** paused

**Delivers:** 插件 `work=auto` 关掉 `WorkOrder.cs` 的替身逻辑后，`s_wellcooked_demo_3` 上四个
厨师（4 个玩家位全占）完全由 Linux 侧控制，按灵活派工无手动输入完成至少一单；
`_1`（没有干净盘堆）上洗盘让盘子循环起来。

**Blocked by:** 05, 06, 07
**Files:** `plugin/{WorkOrder,SmokePlugin,ActionDriver}.cs`、`tools/run-real-game.sh`、
`scripts/dev-cycle.sh`、`.workflow/tasks/os-core/`（只更新 W10 的进度记录）

**Read first:** `.workflow/specs/kitchen-control.md` 的"L2 宏动作"末段、"资源规则"、W9 与 W11；
`.workflow/tasks/game-bridge/05-two-chefs-complete-orders.md`；
`.workflow/tasks/game-bridge/13-diy-demo-level.md`
**Covers:** W9, W11

**Check:** 实机跑 `tools/run-real-game.sh`（`_3`、`CHEFS=4`、`work=auto`、灵活派工）→ 局内得分 > 0，
四个厨师都由 Worker 驱动，Worker 日志能对出一单完整的 `macro_*` 链，`navd` 日志有对应的 `path`
放行，期间没有手柄输入；再在 `_1` 上用 4 个厨师跑一局 → 出现 `wash` 工序、没有订单因缺盘超时，
`decisions.jsonl` 里 `cook` 前置先于等长的非烹饪工序派出。需要游戏。

- [x] `tools/run-real-game.sh` 的 `slots=` 按 `CHEFS` 生成（4 → `1,2,3,4`），默认 `SCENE=_3`、`CHEFS=4`；
      Supervisor 以 `--workers 4` 起 4 个 Worker；`tools/instances.py`/`tools/batch.py` 默认 4 人
- [ ] `_3` 四人灵活派工实机完成一单
- [ ] `_3` 严格验收通过后，`_1` 用 4 个厨师跑 wash/cook 优先
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

仍未完成：真实 `_2` 必须有 score>0、`order_completed`、两个 worker 完整宏链、navd path、无手动输入；随后真实 `_1` wash/cook priority 且无订单超时。未移动 done archive、未创建完成 PR；任务因地图/关卡契约矛盾 blocked，非验收通过。

**2026-10-27 离线实现 P0 第 3/4 步（卷饼流程，未进游戏）：** 按 spec P0 第 1 步的动作表把宏、
调度、导出三处改齐，进游戏只差重导。改动：

- `bridge/src/macro.c`：新增 `pour`（goto 源灶 → 拿锅 → goto 容器 → interact → 回灶放锅）。
  完成判据按世界状态的"内容"、不按资源编号：先看到手持锅/容器 `carried_<chefId>` 的内容非空、
  随后变空 = 倒完；旧状态没有 `carried_` 时退回"`pot_*` 里那味料消失"，并顺带看 proto 的
  `find_station_resource`（按位置）找到的台面容器有没有内容；整段都没有可用信号时 800 ms 兜底。
  `carried_` 由 `find_carried_resource`（macro.c 内的名字查找）取；台面的位置匹配仍归 proto 的
  `find_station_resource`。插件侧资源块按 `StateFrame.ResourceTtl` 最多 0.25 s 重建一次，锅在缓存
  窗口内被端起/倒空会一步跳成"空 carried_"；"先见过非空"的门与 800 ms 兜底正好覆盖这种跳变（锅里有
  料时同一帧就带内容，不会分帧补）。`plate` 改成
  "放下手里整份菜 → 取盘"（删掉 cooker source、`wait_ready` 与取料）；`cut` 的收尾 pickup 与新增的
  `pickup`（台面容器取成品）都按"拿到任意物品"判；新增 `place`（goto + drop）。`KOS_MACRO_WAIT_READY`
  删除，kind 表里换成 `pour`。
- `bridge/src/scheduler.c`：删 `plate_cook_task`/`release_plate_source`/`cook_has_plate_successor`/
  `plate_ambiguous_cooker`；cook DONE 只在直接后继真有 `pour` 时留锅，pour DONE 释放；派工时 pour 的
  `source_station_id` 取源 cook 任务的灶；`place/pour/pickup` 的容器工位沿 `pour` 往上一层取
  （pickup 的父工序是 pour，不是 place——只认直接父工序会挑到随机台面），可行性检查同样按祖先找；
  启动与 SIGHUP 重读时 `recipe_feasibility_check`，不可行的菜在 `add_order` 直接置 FAILED 并写
  `order_rejected`（events.jsonl 打开之前的单延后补写）。
- `plugin/WorldState.cs` + `plugin/Kitchen.cs`：`StationTypes` 加 `ClientAttachStation`，`AppendMap` 用
  `Kitchen.IsPlainCounter`（名字含 `countertop` 且不是砧板）筛普通台面。**组件选择是 [INFERENCE]**：
  与砧板同款（砧板的 `m_item` 也从 AttachStation 读），还没在关卡里导出确认过对象名。
- `scripts/export-kitchen.py`：`KINDS` 加 `counter`；`build_recipes` 按动作表生成寿司
  （`fetch → cut → plate → deliver`）与卷饼（玉米饼 `fetch → place`；每份熟料 `fetch → [cut] → cook →
  pour`；`pickup → plate → deliver`），共享中间产物只建一个节点（`cook:BoiledRice`/`pour:Rice`/
  `place:Tortilla`），`pickup` 按菜区分；工时 100/1700/12100/100/100/100/500 ms；`notes` 写来源与
  "已熟后 16.1 s 未烧焦"；不再写消费者不解析的 `workable`（G10 的另一条路：删字段而不是加解析）。
- 夹具与测试：`scripts/fixtures/s_wellcooked_demo_3/{menu,state}.json` 是**离线推导**的（坐标/料名/
  菜单来自 `levels/wellcooked_demo/layout_3.tsv` 与动作表，Burrito 结构照导出的 Chicken 树，
  `FriedMeat`/`FriedMushroom` 两个熟料名在游戏 resources.assets 里核对过）；`scripts/
  test_export_kitchen.py` 28 项断言锁住 5 道菜的节点序列、共享节点、工时、station 归并与旧形状
  fallback；`bridge/tests/test_map_configs.c` 加 op 白名单与 `cut → board`、`place/pour/pickup →
  counter`；`macro.h` 工位上限 64→128（`kos_kitchen` 不进共享内存，不改段布局；`_3` 重导后约 65 个
  工位）。
- `tools/fake_plugin.py`：盘堆在手持物品时先"放菜"，空手再"取盘"（宏的 plate 已是 DROP+PICKUP），
  `scenario_macro` 的 kinds 集合去掉 `wait_ready`，cook 只断言到 `start_cook`。这一处是 sim 的
  文件，只改到宏场景能过的最小范围。

离线预览：`/tmp/manual-flow/recipes.demo3.preview.json`（34 节点/34 边/5 菜，未写入 `config/maps`）。
用 C 侧的 `kos_recipes_load` + `kos_recipes_dish_index` 验过 5 个菜名都能解析（无 unknown dish）、
`cook` 的 `step`/`resource` 与工时解析正确；临时拼的 kitchen 因为格点与最新 layout 不一致没跑起
Scheduler（夹具问题，不是配置问题）。

验证：`cd bridge && make test` 16 套件 0 失败；`python3 scripts/test_export_kitchen.py` 28/28 OK；
`bash tools/run-bridge-scenarios.sh` 19 个场景 17 PASS，2 FAIL——`os-loop` 首次因重启窗口抖动失败
（单跑重跑 PASS），`os-resources` 的 5 项（无 release/wait/cooker 争用 + 别的 agent 留下的 shm 信号量）
在改动前的 16:37 日志里就是同样 5 项；插件 Release 编译 0 警告 0 错误（本机 `plugin/Libs2017` 是指向
主仓 `plugin/Libs` 的软链，`plugin/Libs*/` 已 ignore）。

剩余（要游戏）：进关卡（不是 StartScreen）后 `printf 'menu=1\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"`，
再 `python3 scripts/export-kitchen.py --map-dir config/maps`，核对：① 普通台面的对象名确实含
`countertop`（否则 counter 一张也不会导出，卷饼全判不可行）；② 5 个菜名与订单的 `recipe` 一致；
③ 42 张台面都在可走格旁（`kos_kitchen_load` 遇到没有站位的工位会拒绝整张图）；④ 台面由 hash 挑，
可能选中离灶远的台面（走位变长，不影响正确性）。

## Replanned 2026-10-26

原 `## Blocked` 记的是“两个厨师都出生在下带、上带工位不可达，需要 level owner 改图”。根因是计划
只占了 2 个玩家位：`layout_2.tsv` 每条带各有 2 个出生点，用户一直要求 4 人控制。地图不改。
用户定 W11 先在 `_3` 上做：`_3` 是一整块（167 格环形过道，3 道菜，有盘堆），4 人走灵活派工，
不需要交接；`_2` 四人分区流水线（每带 2 人、一区多厨师的 `zones.json`、位→出生带的实测对应）
归 W14（见 spec W11/W14 与 decisions.md: 2026-10-26 - 实机控制按 4 个厨师验收）。
本分支已并入 `kitchen-control/10-zoned-pipeline`（分区派工与交接工序），合并后 `make test`
16 套件 0 失败、插件 `WellCooked.Smoke.csproj` 编译 0 错误。

下一步：`SCENE=s_wellcooked_demo_3 CHEFS=4 REAL_GAME=1 bash tools/run-real-game.sh` 实机验收。
2 人时的 `_2` fresh 证据保留在 `logs/real-game/20261026-110357-_2-finalfresh`，作为“只占 2 位时
上带无人”的对照。

**2026-10-26T12:14 `_3` 四人首跑（未通过）：** `logs/real-game/20261026-121357-_3-4chefs`。第一次启动卡在
Steam 云存档提示（0/4 pad、停在 StartScreen，不是插件问题）；重跑后 4 个虚拟 pad 都由游戏自己加入
（pad 0..3 ↔ 配置位 4..1），`_3` 以 4 名厨师加载，Supervisor 起 4 个 Worker。结果 `score=-300`、
没有 `order_completed`：navd 第一轮给 4 人都下了路径，插件对每条 `path` 都 ack，但约 2 秒后 4 人同时
`nav_replan reason=stuck`，随后 `status=blocked`；worker-0/1 的 `fetch` 全是 `goto wait=arrived+face`
约 5 s 超时，worker-2/3 是 `navd blocked: dispenser-0/1`；插件报 `path 超时，距第 1 个路点还差 0.6 米`。
局末 `wellcooked-state.json` 里 4 人位置 (1.8,-3.6)/(4.2,-3.6)/(6.0,-3.6)/(7.8,-3.6) 正好是
`layout_3.tsv` 的 4 个出生格 ×0.6 m，也就是整局没离开出生点：输入没有变成移动，不是 PIBT 互堵。
下一步：`diag=true` 重跑，看 `trace 玩家位=` 的方向与手柄状态，确认 4 个虚拟 pad 的摇杆有没有喂进游戏
（2 人时同一链路能走动）。游戏已关、配置已还原、锁已释放。

**2026-10-26 `_3` 四人联调修复：**
- 不动的根因：`Takeover.cs` 的 DriveSystem 对自有玩家位除了 `owned.Pad.Want(zero)` 归零设备层，还调 `LogicalInput.SetButtons(player, zero, …)` 写逻辑层；逻辑层因此一直是 Active，盖住设备层的轴，`ActionDriver.Feed` 只写设备层的 path 输入全被归零。删掉逻辑层写入后实测 4 人都能走。
- 抓错箱子：`_3` 的 `QuadGridManager` 原点与 StaticGridLocation 错半格，按 crate 所在格交互拿到邻格。新增 `plugin/GridAlign.cs`：`s_wellcooked*` 场景在快照前从原点 / +半格 x / +半格 z / 两者四选一，取重叠最少者并重新占格、刷新缓存的 `m_gridIndex`。run `20261026-143817` 日志 `_3 网格原点 (0,0)→(0.60,0)，重叠 17→0`，第一下按键拿到 Rice。
- 双拿/重复按：`ActionDriver.cs` 记录第一下时的手持物，手持已变化就跳过第二下；Done 之后 1 s 内同种 interact 重注册直接忽略。press 日志带位姿与面前格。
- `macro=goto wait=arrived+face timeout`（run 143817，score -300，0 单）：navd 用 0.3 m 判到站，厨师转身时漂出 0.3 m，navd 报 moving 且无路点，宏回到 navigate、worker 发 stop 顶掉 face。`bridge/src/macro.c` `run_goto` 改为 face 发出后即使 navd 回报 moving 也留在转身阶段；`test_macro.c` 新增该场景，`make -C bridge test` 16 套件 0 失败。
- run `20261026-145754-_3-fix3`（score -300，0 单）：5 次 `goto wait=arrived+face timeout`（等了 3.4–6.1 s，navd 最后几帧只剩 0–1 个路点，4 人让路、反复重规划），2 次 `pickup wait=holding timeout`。14:59:07 之后不再派活：`scheduler.c` 把任何宏失败都当成订单失败，5 单全被判死，游戏里订单还在，直到超时扣分。另见一次抓错箱子：厨师位 2 离站位格约 0.6 m、斜着朝 dispenser-3，按到 Rice 箱。
- 宏失败回队：`handle_worker_event` 对 detail 以 `macro=` 开头的失败释放租约、`task_requeue(reason=macro-failed)`，同一工序累计失败 3 次（`SCHED_TASK_MAX_RETRIES`）才让订单失败；不设 `avoid_chef`，因为料多半还在原厨师手里。缺工位、宏映射失败仍直接失败。`fake_plugin.py --scenario macro` 新增 `macro_failure_requeues_before_order_fails`（requeued=2、macro_failed=3）。
- goto 余量：`MACRO_GOTO_MARGIN_MS` 3 s → 6 s，`test_macro.c` 断言 5 s 时仍在走、7 s 时超时。`make -C bridge test` 16 套件 0 失败。
- run `20261026-154815-_3-fix4`（score -300，0 单）：回队生效，7 个 `task_done`（4 次 fetch、1 次 assemble、1 次 plate）、12 次回队。注意运行目录里的 `LogOutput.previous.log` 是上一局的插件日志（脚本启动时把旧日志挪进来）；本局插件日志在局后从游戏目录复制为 `LogOutput.run.log`。主要失败是 `assemble` 的 `drop wait=empty-hand timeout`：厨师位 3 拿着生虾对着 `cooker-3` 上的平底锅（`container-1`）按了 3 轮 interact，游戏不收。其余：cook 的 `goto arrived+face` 约 9 s 超时、fetch 的 `pickup wait=holding timeout`、唯一装好盘的那单 deliver 时订单已过期（`order-gone holding-lost`）。
- 组合工位：菜谱里子节点带装盘步骤（菜单 `plate`）的 `CompositeOrderNode` 是在盘子上组合的；干净盘堆把放置转交给最上面那只盘子（`ServerCleanPlateStack.HandlePlacement`）。`scripts/export-kitchen.py` 的 `build_recipes` 增加 `station_kinds` 参数，地图有 `plate_stack` 时这类 `assemble` 的 `resource` 记为 `plate_stack`，没有盘堆时仍是 `container`。`_2/_3/_4` 的 `recipes.json` 只改了这 3 个 assemble 节点的 `resource`。`scripts/test_export_kitchen.py` 新增 2 个用例，5/5 OK；`make -C bridge test` 16 套件 0 失败；`fake_plugin.py --scenario macro` PASS。
- run `20261026-163526-_3-fix6`（score -300，0 单）：组合落到盘堆后有几次放进去了；主要失败是 `deliver` 派给了空手的厨师，菜还在另一个厨师手里。
- 手持亲和：`bridge/src/scheduler.c` 增加 `op_leaves_item_in_hand`（fetch/cut/mix/plate 做完东西还在手上）、`task_holder_chef`、`holding_chefs`。下游工序只派给拿着料的那个厨师，这个厨师手上有料时不接别的活；`opened_ready_count` 与 `dispatch_ready` 按此计数；被堵的厨师就是持有者时，BLOCKED 处理不设 avoid/hold。`make -C bridge test` 16 套件 0 失败，`fake_plugin.py` macro/os-loop/nav-stuck/nav-ring PASS（os-resources 的 5 个断言在干净 HEAD 上同样失败，与本改动无关）。
- run `20261026-165847-_3-fix7`（score -300，0 单）：亲和生效，cook/assemble 都派给了取料的厨师。新的根因：`cook:BoiledRice` 派到 `cooker-0`（FryingStation，炸锅），厨师拿着米按 3 次 interact 游戏都不收。游戏规则见 `CookableContainer.AllowItemPlacement`：料 prefab 的 `CookableProperties.AllowedCookingSteps` 必须含锅的 `CookingHandler.m_cookingType`；`pick_cooker_station` 只按 kind 挑灶台，不看锅的类型。其余：`assemble:Sushi_PlainPrawn` 在盘堆 `drop wait=empty-hand timeout`，`fetch:SushiFish` `pickup wait=holding timeout`，`goto arrived+face` 6–10 s 超时。
- 锅类型匹配：插件 `WorldState.cs` 给 `ClientCookableContainer` 工位导出 `step`（`GetCookingHandler().AccessCookingType` 的资源名），料箱导出 `cook_steps`（`AllowedCookingSteps`），菜单的 `CookedCompositeOrderNode` 导出 `cook`（`m_cookingStep`）。`export-kitchen.py` 让 cooker 继承同一格上那口锅的 `step`，cook 节点带 `step`。`kos_station.step` / `kos_recipe_node.step` 由 `kitchen.c` / `recipes.c` 读入，`pick_cooker_station` 只挑 step 相同的灶台（任一方为空则不限）。重导 `_3`：cooker-0/1 = DeepFatFryer、2/3 = FryingPan、4/5 = Pot；`cook:BoiledRice` = Pot、`cook:FriedChicken` = FryingPan。`test_export_kitchen.py` 新增 2 个用例，7/7 OK；`make -C bridge test` 16 套件 0 失败。
- run `20261026-172424-_3-fix8`（score -300，0 单）：锅类型匹配生效，`cook:BoiledRice` → `cooker-4`（Pot）、`cook:FriedChicken` → `cooker-2`（FryingPan）。新的根因：`start_cook` 报 `wait=pot=cooking no-station-resource`，`state_update` 里根本没有 `pot_<N>`（每帧 `resources=4`：砧板/盘堆/脏盘/水槽）。原因见反编译 `ServerCookableContainer.StartSynchronising`：它把自己的 `ServerCookingHandler.enabled` 设成 false，`OnContentsChanged` 只在锅里有料时才打开；`ServerCookingHandler` 只在 `OnEnable` 时进静态表 `s_cookingHandlers`，所以 `GetCookingHandlers()` 只列出有料的锅，空锅一口都不报（局末 `wellcooked-state.json` 的 `cooking[]` 只有一口烧糊的锅）。其余：`cook:FriedChicken` 首次放料 `drop wait=empty-hand timeout` 2 次（之后一次放进去了）；`assemble:Sushi_PlainPrawn` 盘堆 `drop` 超时、`fetch:SushiFish` `pickup` 超时仍在。剩余工作与修法写进 spec `kitchen-control.md` 的"W11 实机遗留（2026-10-26 交接）"。

**2026-10-26 P0 第 1、2 步离线准备（未验收；等人工打一局）：**

- 手动局观察：新增 `plugin/ManualLog.cs`（开关 `manual_log=true`）。每帧读 `PlayerControls.ControlScheme`
  的 A（`PickupAndDrop`）与 X（`WorkstationInteract`），按下/松开各一行，带手持物（盘子内容全列）、
  面前的占位物体与坐标、厨师的游戏格四邻；每 0.25 秒给砧板（料 + 进度）、锅（料 + 烹饪秒数）、
  有内容的容器/盘子、干净盘堆拍快照，变化才打。前缀固定 `manual:`，不影响游戏输入
  （只调 `IsDown()`，不碰 `JustPressed()` 的按键事件）。
- 手动局不接管：新增 `slots=none`。原来 `work=off` 仍会用"来回走 + 周期按交互"驱动逻辑层玩家，
  而 `work=manual` 根本不存在；`slots=none` + `work=auto` 才真正一个位都不占。运行中改这一项不会
  解绑已挂的虚拟手柄，必须启动前写好。
- 砧板导出：插件 `WorldState.StationTypes` 加 `ClientWorkstation`，只认名字含 Chop/Cut
  （与 `Kitchen.IsChoppingBoard` 同一规则）；`Kitchen.ChoppingBoards` 改成按 x、z、name 排序，
  使 `cutting_board_N` 与 `kitchen.json` 的 `board-N` 同序。`export-kitchen.py` 新增 kind `board`；
  `ClientCookableContainer` 锅具组不再进 `stations`，只把 `step` 交给同格 cooker；`container`
  保留给真正的容器（`ClientIngredientContainer` 等）。
- `macro.c`：`find_station_resource` 改为 `board-N → cutting_board_N`、`cooker-N → pot_N`，
  删除 `container-N → cutting_board_N` 以及 `cooker_N`/`container_N` 旧别名。
- `tools/fake_plugin.py`：WorldSim 的资源键按协议生成（`pot_N`/`cutting_board_N`/`sink_N`），
  macro 场景的切菜工位改成 `board-0` + `cutting_board_0`。
- 文档：`.workflow/standards.md` 开关表、spec W11 P0 第 1/2 步、`plugin/README.md` 文件表。

测试（kc-A 本树；改动前基线 `make test` 也是 16 套件 0 失败）：
- `cd bridge && make test` → `=== 16 test suites, 0 failed ===`
- `cd bridge && make test-san` → `=== 16 test suites, 0 failed ===`（78 s）
- `python3 scripts/test_export_kitchen.py` → `Ran 11 tests ... OK`
- `bash tools/run-bridge-scenarios.sh --only macro,os-loop,nav-stuck,nav-ring` →
  `合计: 4 个场景, 通过 4, 失败 0, 跳过 0`（macro 35 s、os-loop 10 s、nav-stuck 10 s、nav-ring 11 s）
- 双向证明：用 `HEAD` 的旧 `scripts/export-kitchen.py` 跑新测试 → `FAILED (failures=4)`；
  用 `HEAD` 的旧 `bridge/src/macro.c` 编译新 `test_macro` → `检查 85 项，失败 9 项`
  （含 `board-0 映射到 cutting_board_0 并按 work`、`container-0 不再映射到 cutting_board_0`）。
- 插件 `WellCooked.Smoke.csproj` Release 编译 0 警告 0 错误；已部署到游戏目录
  （无游戏/Proton 进程且 `/tmp/wellcooked-game.lock` 不存在时取锁→拷贝→释放），
  sha256 `be280c2d010cd5da4f3cdf821eab89ed273d4fb30a5cd5348d8c3687a10895b6` 与构建产物一致；
  未启动游戏。

未做：
- P0 第 3/4 步（按 prefab 与动作表重建 recipes、离线校验工序图）要先有人的动作表。
- `config/maps/*/kitchen.json` 没有重导：导出文件不能手改，等手动局里 `menu=1` + 导出脚本。
- P1–P3 未开始。`config/kitchen.example.json` 是旧导出（没有 board）；`test_macro` 用测试内克隆
  补 `board-0` 来跑新映射，不是改导出文件。

**2026-10-26 手动局注入审计与现场准备（游戏未启动）：**

审计结论（takeover 文件同时满足 `slots=none`、`work=auto`、`bridge=off`、`frontend_press=false`、
`diag=false`、`manual_log=true`）：
- `Takeover.EnsurePads`（`plugin/Takeover.cs:488`）只遍历 `Slots`；`slots=none` 在
  `Takeover.Install` 的首次读配置里清空它，`InputManager.AttachDevice`（:498）不会执行，
  `ResolvePendingPads`（:539）也因 `PendingPads` 为空直接返回。启动前写好配置这一点已做到。
- `Takeover.PressForJoin`（:461）只在 `_frontendPress` 为真且没在驱动时走；本次为 false，且
  没有虚拟手柄可写。前端不会由插件按任何键。
- `Takeover.Drive`（:879）在 `work=auto` 下只进 `DriveSystem`（:1000），后者只遍历
  `OwnedPads`；为空 → 不写设备层、不写逻辑层。`WorkOrder.Tick`（:1064）只在
  `DriveWorkOrder`（`work=local`）里，`ActionDriver.Drive` 只对收到过 Bridge 动作的槽生效。
- `ActionDriver`/`LinkRunner`：`bridge=off` 时不建链路（`_link == null`），没有动作来源；
  `ActionDriver.Feed` 不会执行。`Commands` 只在 `wellcooked-command.txt` 存在时执行显式命令，
  本次已删旧文件，后续只会写 `load=`/`menu=1`。`engage=`/`continue=` 不会自动运行。
- `LogicalInput`：原来无条件给四个玩家套 `ILogicalValue/ILogicalButton` 包装；现在
  `Takeover.OccupiesNoSlots` 时在 `LogicalInput.Install` 直接跳过补丁（启动日志
  `logical: slots=none，不安装逻辑层注入`），`GetValue/GetButton` 返回游戏原生对象。
  另外修了包装器 `ClaimPressEvent/ClaimReleaseEvent` 在未驱动时没有转发给原生按钮的问题。
- `VirtualPad` 只在 `EnsurePads` 里 new；本次无实例，故没有虚拟设备注册到 `InputManager`。
- 保留但不动输入的：`Orders` 的 `GetNextRecipe/InitialiseRound` 补丁（固定三单需要）、
  `GridAlign.Apply`（修 `_3` 网格原点，不改输入）、`SmokePlugin.ApplySmokePatches`（只计数日志）、
  `WorldState`/`ManualLog`（只读/写状态文件与日志）。
- 仍会改游戏全局行为、但不写输入/不占位的：`Application.runInBackground = true`、
  失焦不暂停、`Application.isFocused` 恒真（`plugin/SmokePlugin.cs:31-34,111-161`）；
  窗口配置未写（`window`/`window_pos` 都没有），`ApplyWindow` 直接返回，不改分辨率。

代码改动：`plugin/Takeover.cs` 增加 `OccupiesNoSlots` 并把安装日志写成 `占位 none`；
`plugin/SmokePlugin.cs` 把 `Takeover.Install` 挪到 `LogicalInput.Install` 之前（先读配置）；
`plugin/LogicalInput.cs` 在 `slots=none` 时完全不装补丁，并补齐未驱动时的 claim 转发。
`cd bridge && make test` 仍为 16 套件 0 失败；插件 Release 编译 0 警告 0 错误。
当前部署 sha256 = `eb69fc6a437316c2c4ed601011241c4e9970e4fef5b8f11353b22e1c5474f2fb`
（游戏目录与 `plugin/bin/Release/net35` 一致）。现场已写好：

takeover（备份在 `/tmp/kc09-takeover.before-manual`）：
```ini
slots=none
work=auto
bridge=off
frontend_press=false
diag=false
manual_log=true
```
orders（旧文件备份在 `/tmp/kc09-orders.before-manual`，如存在）：
```json
{"seed":12345,"level":"s_wellcooked_demo_3","menu":[{"recipes":["Sushi_PlainFish","Sushi_PlainPrawn","Chicken_Burrito"]}]}
```

**用户现在只需要（配置已写好，不要再改 takeover/orders）：**
```sh
cd /home/shizuku/Project/overcooked-schedule-wt/kc-A
export GAME_DIR="${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}"

# 1) 起游戏（Steam 要先开着）
bash scripts/run-game.sh

# 2) 在前端按手柄 A / 键盘回车加入 1 号玩家

# 3) 看到 1 个玩家位后进 _3，等关卡完全加载、厨师出现
printf 'load=s_wellcooked_demo_3\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"

# 4) 用这一个人把三道菜各做一次：拿料 → 切/煮 → 组合/装盘 → 上菜

# 5) 仍在关卡里：导出菜单与工位（写 config/maps/s_wellcooked_demo_3/）
printf 'menu=1\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"
sleep 2
python3 scripts/export-kitchen.py --map-dir config/maps

# 6) 保存日志
mkdir -p logs
cp "$GAME_DIR/BepInEx/LogOutput.log" "logs/manual-$(date +%Y%m%d-%H%M%S)-demo3.log"
grep -c 'manual:' "$GAME_DIR/BepInEx/LogOutput.log"

# 7) 恢复现场并关游戏
rm -f "$GAME_DIR/BepInEx/wellcooked-orders.json" "$GAME_DIR/BepInEx/wellcooked-command.txt"
cp /tmp/kc09-takeover.before-manual "$GAME_DIR/BepInEx/wellcooked-takeover.txt"
# 关闭游戏窗口；确认没有 Overcooked2/Proton 进程
```

下一步（新会话从这里继续）：
1. 读上一步保存的 `logs/manual-*-demo3.log` 里的 `manual:` 行，整理"每道菜的动作表"写进 spec
   W11 P0 第 1 步；重点回答鱼/鸡切完的 prefab 名、虾要切还是要煮、卷饼组合顺序、装盘后是否还有一步。
2. 用该动作表做 P0 第 3 步（`build_recipes` 按 prefab 重建，去掉不可能成功的 `assemble`/盘堆动作），
   再做第 4 步（`test_export_kitchen.py` 锁住三道菜节点序列、Scheduler 的 `recipe_infeasible`）。
3. 重导后的 `config/maps/s_wellcooked_demo_3/` 与任务 Check 一起复核；P0 验收仍是
   `_3` 一个厨师、`Sushi_PlainFish`、系统控制完成一单且得分 > 0。

Review boundary: 无（本轮未开 flow-verify；离线改动待实机动作表后随 P0 一起复核）。
分支/工作树：`kitchen-control/09-real-game-two-chefs` / `~/Project/overcooked-schedule-wt/kc-A`；
本条目随本轮提交，工作树无未提交改动（`plugin/bin` 与 `logs/` 不入库）。

**2026-10-27 手动局动作表 + P0 第 3 步可行性审计（按用户要求停在决定点）**

Dirty at start: `plugin/README.md`、`plugin/SmokePlugin.cs`、`plugin/Takeover.cs`（已改）、
`plugin/L3Tuning.cs`（未跟踪）。本轮没碰它们，也没有提交任何东西。

已完成：
- 读 `logs/manual/20261027-022317/manual.log`（单局，切点 L4 / t=30.04），把 5 道菜的实测动作表写进
  spec W11 P0 第 1 步（含四个待核实问题的答案、逐项工时、对 `build_recipes` 的约束）。
  关键实测：切 = 8 格 / 1.6–1.75 s；煮与煎 = 12.07–12.15 s 到"已熟"；装盘 ≤0.1 s；交付约 0.5 s；
  烧焦未知（16.10 s 仍未烧焦）。切完的名字：`ChoppedSushiFish`、`ChoppedSushiPrawn`、
  `BurritoChoppedChicken`、`DicedMeat`、`ChoppedMushroom`。虾只切不煮。
- Check before：`cd bridge && make test` = 16 个套件、0 失败、26.44 s。

停下的原因（用户指示：schema 表达不了就先停下报告）。卷饼的正则流程有两步现有消费者表达不了：
- **`pour` 不存在。** `macro.c:kos_macro_plan_build_with_source`（fetch 151 / cut 158 / cook 167 /
  plate 171 / deliver 187 / wash 190 / mix 194 / assemble 204）没有"端着锅倒进容器"；
  `run_drop`（macro.c:625）只在**手空**时判完成，而实测倒完料锅还在手上；`cook`（167）把锅留在灶上、
  厨师空手走开，所以 `assemble` 的 goto+drop 要么没东西可倒、要么真倒时超时。
- **`counter` 工位类型不存在。** `plugin/WorldState.cs:StationTypes`（302–321）只导组件表里的物体，
  普通台面（`countertop_01_standard_wood`）不在其中，所以 state/kitchen.json 里没有它；
  `scripts/export-kitchen.py` 的 `KINDS` 也没有 counter。现有 kind =
  serving/dispenser/cooker/sink/clean_plate_stack/plate_stack/container。
  `bridge/tests/test_map_configs.c` 断言"每个工序的 resource 都有工位"，所以直接写 `counter`
  或 `board` 会让 `make test` 变红（board 只差重导，counter 还要插件导）。
- **`plate` 语义不对。** macro.c:171–186 是"先取盘 →（有 source 时）去灶上取料"；
  `scheduler.c` 的 `plate_cook_task`/`release_plate_source`/`cook_has_plate_successor` 把 source
  定为祖先 `cook` 的灶。实测是"把手里整份菜放到盘堆上 → 取盘"，卷饼的菜在台面的玉米饼里、不在灶上。
  寿司这条碰巧能跑通（`run_pickup` 的 `holding_is` 走 `kos_kitchen_name_matches` 双向子串，
  "Plate" 能匹配 `equipment_plate_01`；第一次 interact 会先把鱼放下），但卷饼的 source 指向灶，
  必然卡在 pickup。
- **数据缺一截。** `$GAME_DIR/BepInEx/wellcooked-menu.json` 还是 2026-10-26 17:21 的旧关卡（3 道菜），
  现在的 `_3` 是 5 道（`levels/wellcooked_demo/layout_3.tsv` 的 `# menu:` 行；游戏日志菜谱下标 0–4
  与之一致）。重导需要进关卡后发一次：
  `printf 'menu=1\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"`；脚本读
  `$GAME_DIR/BepInEx/{wellcooked-state.json,wellcooked-terrain.json,wellcooked-menu.json}`，
  跑 `python3 scripts/export-kitchen.py --map-dir config/maps`。不用重开游戏，只要这三个文件是新导出的
  （state 里还要有砧板；当前 state 是 StartScreen，0 个工位）。

最小改动清单（等用户拍板，本轮未动手）：
1. `bridge/src/macro.c`：新增 `pour`（goto 灶 → 拿锅 → goto 容器 → interact 到源锅内容为空 →
   goto 灶 → 放回锅）；`plate` 改成"放下手里的整份菜 → 取盘"，去掉 source/取料。
2. `bridge/src/scheduler.c`：撤掉 plate 的 cooker source 与锅租约转移（`plate_cook_task`、
   `release_plate_source`、`cook_has_plate_successor`、`plate_ambiguous_cooker`）。
3. `plugin/WorldState.cs` `StationTypes` 导普通台面（`countertop_*`，用 `ClientAttachStation`），
   排除已属于别的工位的格子；`scripts/export-kitchen.py` `KINDS` 加 `counter`。
4. `scripts/export-kitchen.py` `build_recipes`：按 spec 第 1 步的表生成 5 道菜（cut/board、pour/counter、
   合并装盘），工时用实测值，`notes` 写来源（`logs/manual/20261027-022317`、调参 chop 2 阶段 ×8、
   容量 1）与"烧焦未知，≥16.1 s"。
5. `tests/test_export_kitchen.py`（新）+ `bridge/tests/test_map_configs.c`（resource 断言认 counter）
   + `test_macro.c` 的 plate 用例。
6. 重导 `config/maps/s_wellcooked_demo_3/{kitchen,recipes}.json`（要上面那份新 state/menu）。

未做：`recipes.json` 与 `export-kitchen.py` 都没改——现在改只会生成消费者解析不了、或必然超时的图，
或者让 `make test` 变红。

下一步：用户决定第 1–3 项这类 schema 扩展是否要做；批了我就按第 4–6 项一次做完再验。

**2026-10-27 倒料工序命名决策与容差匹配（[推断] 待实机日志确认）**

- 倒料节点命名与实测对齐：`/tmp/kc-fixtures/demo3`（实机新关卡导出）中 `recipes.json` 的 pour 节点以生料名标注（`pour:BurritoChicken` / `pour:BurritoMeat` / `pour:Mushroom` / `pour:Rice`），烹饪节点为熟料名（`cook:FriedChicken` / `cook:FriedBurritoMeat` / `cook:FriedMushrooms` / `cook:BoiledRice`）。
- 开局状态 `wellcooked-state.json` 中锅具 `contents=[]`，实机运行中熟料在锅内上报的名字究竟是熟料名（`FriedChicken`）、切块 prefab 名（`BurritoChoppedChicken`）还是生料名（`BurritoChicken`）属于 [推断]。
- 决策：C 侧 `macro.c` 的 `run_pour` 采用动态公共后缀词根推导（common suffix stem matching，无硬编码词根列表），避免全字严格匹配将同源熟料 `FriedChicken` 误拦截为 `wrong-item`；同时保留对完全不同原料（如鸡肉倒米饭）的校验。倒料执行时通过 `kos_log_info(macro->log, "macro_pour_source", ...)` 输出项目日志到 `worker-*.log`，待下次真机运行直接确认实机真实命名（检索模式：`grep -E '\bmacro_pour_source\b' logs/worker-*.log`）。
- 检查锁住：`bridge/tests/test_macro.c` 补充 `test_pour_tolerant_matching` 单测（含不在任何列表中的 `TacoBeef` ↔ `CookedBeef` 测试）；`python3 scripts/test_export_kitchen.py` 与 `cd bridge && make && make test && make test-san` 全绿。

**2026-10-27 模拟器工位与 kitchen.json 匹配及切菜动作路径核对**

- 工位位置完全由 `kitchen.json` 驱动：`tools/demo3_sim.py` 移除了对 `counter-0` 等工位坐标的硬编码篡改（原代码曾将 `counter-0` 覆写为 `(0.6, -6.0)`，实际 `kitchen.json` 中 `counter-0` 位于 `(-3.0, -7.2)`，`counter-13` 位于 `(0.6, -6.0)`）。所有工位坐标均直接从 `kitchen.json` 加载，同时补充 `compute_stands_from_kitchen` 动态推导站位格（`STANDS` 为厨师站在过道交互时的可走格坐标，如 `cooker-5` 灶台在 `(7.8, -6.0)`，对应站位格为 `(7.8, -7.2)`）。在 `Demo3World` 初始化时断言所有工位位置与 `kitchen.json` 的偏移不超过 `MACRO_STATION_POS_TOL = 0.30`，无例外跳过。
- 砧板切菜动作路径核对：切菜推进必须使用 `action="work"`（Ctrl/X 加工键，由 `macro.c:run_chop` 对应下发 `KOS_MACRO_ACT_WORK`），并在面前角度小于 `WORK_CONE_DEG`（45°）时按住推进（合计 1.7 s 完成并更名 prefab）；`action="interact"`（A/空格 交互键）在砧板上仅判定放下手持物或端起成品，绝不会推进切菜进度。背对砧板时发送 `work` 亦不推进。
