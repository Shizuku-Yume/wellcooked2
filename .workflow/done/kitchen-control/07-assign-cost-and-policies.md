# 07: 派工代价、匈牙利配对、LLF/HRRN 与锅/盘子规则

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** d98cf7a3b5d2bdc8a307df5412f1f221befbcc53
**Base branch:** kitchen-control/03-stand-cells-and-distance-field
**Started:** 2026-10-25T15:20:00+08:00
**Finished:** 2026-10-25T16:30:11+08:00

**Delivers:** Scheduler 有厨师空闲时按 spec 的代价公式给 (厨师, 就绪工序) 打分，多个空闲厨师
用匈牙利算法配对；订单策略多出 LLF 与 HRRN；锅前置 `w_c`×2，干净盘 ≤ 1 且有脏盘时生成
`wash` 工序；每次决策的各分量写进 `decisions.jsonl`。

**Blocked by:** 03
**Files:** `bridge/include/kitchenos/{assign,policy}.h`、`bridge/src/{assign,policy,scheduler}.c`、
`bridge/tests/test_assign.c`、`bridge/tests/test_policy.c`

**Read first:** `.workflow/specs/kitchen-control.md` 的"L3 调度"、W7、W8；
`.workflow/specs/os-core.md` 的 Scheduler 段；`bridge/include/kitchenos/{policy,recipes,resource}.h`
**Covers:** W7, W8

**Check:** `cd bridge && make test` → `test_assign`：2 个空闲厨师 × 3 道工序的配对与手算匈牙利
一致，`decisions.jsonl` 每条含 `w_d/w_t/w_c/w_u/w_z`；`test_policy`：LLF/HRRN 选择顺序与手算
一致，并列时退化为 FCFS；`kill -HUP` 能切到 `llf`/`hrrn`。

- [x] 路程用任务 03 的距离场，不读导航表
- [x] 权重从 `config/maps/<scene>/` 的调度配置读，缺省值写在 spec 里
- [x] 已开工订单按开工顺序交付，策略只决定下一单
- [x] `wash` 工序走现有就绪队列，不另起队列

## Progress

Dirty at start: none（从 `origin/kitchen-control/03-stand-cells-and-distance-field` 的
d98cf7a 起，`kitchen-control/07-assign-cost-and-policies`；工作区切换前是 detached HEAD）。

**Check before:** `cd bridge && make test` 现有 12 个套件全绿；`test_assign` 还不存在，
LLF/HRRN 不在 `policy.h`，Scheduler 仍是"每个空闲厨师挑策略选中的订单里关键路径最长的
一道"，`decisions.jsonl` 的 dispatch 行没有 `w_d/w_t/w_c/w_u/w_z`。按任务要求先写检查。

**实现（2026-10-25）**

- `bridge/include/kitchenos/assign.h`、`bridge/src/assign.c`：派工代价（五项带符号分量）、
  矩形匈牙利配对（厨师多于工序时转置；不可达配对用 `unreachable` 标记并在求解后丢弃）、
  锅链标记（cook 及其全部前置）、洗盘判定、`<kitchen.json 同目录>/schedule.json` 读取。
- `policy.h/policy.c`：新增 LLF 与 HRRN，四种策略主键并列时都退化为 FCFS；名字/轮换/CLI
  四个名字统一由 `kos_policy_kind_from_name`/`kos_policy_other` 给出；比较用 `__int128`
  中间量避免极端值回绕。
- `scheduler.c`：世界状态缓存；订单 `opened/open_seq`，策略只开下一单，已开工订单按开工
  顺序交付（更早订单未完成时后续 `deliver` 不就绪）；按空闲厨师数补开订单，且只数"资源
  令牌够用"的就绪工序；候选含至多一道合成的 `wash`，与订单工序共用候选列表、匈牙利配对和
  租约；资源 kind 在候选阶段预扣；`decisions.jsonl` 每条带 `w_d/w_t/w_c/w_u/w_z`，派工行
  另写原始输入；派工计数加 LLF/HRRN。
- `config/maps/s_wellcooked_demo_{1..4}/schedule.json`：初始缺省权重（spec 表格同值）。
- `child.c`、`supervisor.c`：`--strategy` 与帮助文本接受并转发四个策略名。
- `metrics.h/metrics.c`：`metrics.json` 的 dispatch 计数加 `llf`/`hrrn`。
- 测试：`test_assign`（代价/分量、2×3 手算匈牙利+暴力核对、3×2、不可达、并列、锅链、
  洗盘判定、配置、真实 Scheduler 的 decisions 字段/SIGHUP、世界状态距离与 wash 生成）；
  `test_policy`（LLF/HRRN 手算顺序、并列退化、四策略轮换、`--strategy llf` + SIGHUP→HRRN）；
  `scheduler_fixture.h` 用带 pid 后缀的工位 kind 起真实 Scheduler，资源信号量与临时目录
  自清理。
- 文档：spec 的 L3/每张地图配置/Open questions、glossary、os-core spec 的 Scheduler 段与
  decisions.md 三条设计决定 + 一条游戏时钟延后接线的决定。

**Check after**

- `cd bridge && make test`：13 个套件、0 失败；`test_assign` 54 项、`test_policy` 53 项全过。
- `cd bridge && make test-san`：13 个套件、0 失败（ASan/UBSan）。
- 真实路径：`test_assign` 第 9 节启动 `build/kitchenos-scheduler`（临时地图、真实资源信号量、
  真实 mq），`decisions.jsonl` 每条都有五个分量、派工行 `cost = 五项之和`，三次 SIGHUP 依次
  出现 EDF/LLF/HRRN；第 10 节写入带 2 个厨师坐标与 `dirty_plates=2` 的 shm 世界状态，Scheduler
  生成并派出一道 `wash`，`travel_cells > 0`，事件流有 `wash_generated`。`test_policy` 第 5 节
  以 `--strategy llf` 启动、SIGHUP 后策略与决策变为 HRRN。
- `.workflow/bin/workflow validate`：仍是 3 个错误 + 1 个警告，全部来自 os-core/01、
  game-bridge/13、specs/os-core 的既有问题，本任务没有新增。

**没验证到的**

- 没有跑真游戏：本任务的 Check 不要求；洗盘的实机验收在任务 09，`dirty_plates` 字段由任务 02
  提供、`wash` 的宏动作由任务 06 提供，07 的基分支里两者都不存在（基础地图不会生成 wash）。
- 真实游戏厨师坐标下的路程只在这个 shm 夹具里验证过，避让等待不在代价模型内（spec 明说）。

**Review**

`Review boundary: d98cf7a + bridge/{include/kitchenos/{assign,child,metrics,policy}.h,src/{assign,child,metrics,policy,scheduler,supervisor}.c,tests/{test_assign,test_policy,scheduler_fixture.h}}, config/maps/*/schedule.json, .workflow/{specs,glossary,decisions,tasks}; round: 1–3; briefs: A,B; boundaries: schedule JSON、shm 世界状态、Scheduler↔Worker mq；findings: 8 blocking / 3 nonblocking（round 3 后无 blocking 遗留）`。

- **blocking（已修）匈牙利矩阵行距。** `solve_rows` 按实际 `cols` 读紧凑矩阵，调用方却传了
  `[33][33]` 二维数组。改成 `matrix[r * cols + c]` 的一维缓冲；`scheduler.c` 的 terms/
  travel_cells 也改成按 `pool_count` 紧凑排再传给 `kos_assign_solve`。
- **blocking（已修）不可达配对仍被派出。** 距离场 `KOS_STAND_UNREACHABLE` 原来换成 600 格
  惩罚，仍可能被派给走不到的厨师。加 `kos_assign_terms.unreachable` 与
  `KOS_ASSIGN_FORBIDDEN`，求解后丢弃禁区配对（`task_of_chef = -1`），并加 2×2 用例
  （一个厨师只到得了一道 / 两道都到不了）。
- **blocking（已修）只数依赖不看资源的开工判断。** `opened_ready_count` 现在按 kind 预扣
  令牌；随后又改成"任务侧可行厨师掩码 + Kuhn 最大匹配"，把同一厨师够得着的两道工序只算
  一份工作，避免 `_2` 两条不通条带上下一个厨师空转、也不开下一单。
- **blocking（已修）Supervisor 只认 fcfs/edf。** `supervisor.c` 的 `--strategy` 改用
  `kos_policy_kind_from_name`，帮助文本同步四个名字。
- **blocking（已修）os-core spec 与实现不一致。** Scheduler 段、模块表、W6 说明补上
  LLF/HRRN 与四策略轮换。
- **blocking（跨任务，已记录）wash 的 Worker 消费端。** 宏动作 `wash` 属于任务 06；
  07 的基分支没有 `dirty_plates` 字段（任务 02 提供），实机不会生成可执行的 wash。spec 的
  洗盘段与"没验证到的"都写明；任务 09 合并 02/06 后验收。
- **blocking（跨任务，已记录）游戏时钟。** 07 的基分支（03）没有 `game_time_ms`，订单到达/
  截止期也还是单调时钟坐标。策略接口只收一个 `now_ms`，任务 02 合并后只改 Scheduler 取时的
  一处；决定记在 decisions.md: 2026-10-25 - 游戏时钟落地前，Scheduler 的时间参数沿用单调
  时钟，spec L3 也写了一句。
- **nonblocking（已修）** 策略比较的极端值回绕（改用 `__int128` 中间量）；`schedule.json`
  的 `weights` 不是对象时原来静默用缺省值（现在报错）；任务文件缺少 Check after/归档记录。
- round 2 的阻塞项（不可达工序占住令牌/阻止开单）与 round 3 的"缺工位订单永远不开"都已修；
  后者经实现者说明与新增 `test_missing_station_fails_order`（真实 Scheduler、`no_such_resource`
  → `task_failed` + `order_id`）后由 reviewer 撤回。最终两份 brief 都报无 blocking。
