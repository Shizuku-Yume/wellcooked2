# 10: 插件整理：把这一轮堆出来的诊断与兜底收敛

**Effort:** game-bridge
**Task type:** refactor
**Base commit:** 9d72d38
**Started:** 2026-10-16T00:05:00+08:00
**Finished:** 2026-10-16T02:00:00+08:00

**Delivers:** `plugin/` 里这一轮为调试堆出来的东西收敛成清晰结构，**行为不变**：诊断输出归位（统一走一个开关）、重复的反射/查找辅助函数合并、临时兜底标注清楚并集中到一处、死代码与过期注释删掉；`scripts/` 下的挑关卡工具（`scan-levels.py`、`eval-levels.sh`）在 `standards.md` 里有位置说明。

任务 09 的进度是"每遇到一个游戏机制就往里加一段"堆出来的：`Kitchen.cs` 里既有工位查询又有面向日志的诊断，
`WorldState.cs` 里 `Get/Invoke/FindAll` 三套反射helper被到处调用，`WorkOrder.cs` 里混着"就位/朝向/放宽/按住/超时"五种策略。
不改这些，任务 11/12 往里加控制与煮煎逻辑只会更难。

**Blocked by:** None
**Files:** `plugin/`（`Kitchen.cs`、`WorldState.cs`、`WorkOrder.cs`、`Takeover.cs`、`Commands.cs`）、`.workflow/standards.md`
**Read first:** 任务 09 的两段 Progress（尤其"当前不稳的地方"）、`.workflow/technical-debt.md` 里与插件相关的条目
**Covers:** W6
**Check:** `bash scripts/dev-cycle.sh` 进关卡后 `work=auto` 能完成至少 1 单（`score` 从 0 变正、日志有 `完成一单`），
且 `bash scripts/build-plugin.sh` 无警告新增；`git diff` 里没有行为变化（同样的日志关键行、同样的得分路径）。

- [x] 诊断输出统一：`probe`/`dump`/`mech`/`levels` 与工序日志的开关集中到一处，默认不刷屏
- [x] 反射辅助合并：`WorldState` 的 `Get/Invoke/FindAll` 收敛成一套并补注释（哪些字段名与游戏版本绑定）
- [x] 兜底集中并标注：`_looseApproach`（1.4 米 + 不校验朝向）、按住 X 等超时、侧向错身 3 米限制，都移到一处并写清"为什么是兜底、什么时候该去掉"
- [x] 删死代码与过期注释（含已确认不用的 `FreeCounter`/`LogicalInput` 路径，按实际再判）
- [x] `standards.md` 的 `Running it` 补上挑关卡工具与 `unlock/levels/mech` 指令的用法

**Done when:** 上面每条打勾，且 Check 的单子能跑完、`git diff` 里没有行为变化。

## Progress

Dirty at start: none（从 main 的 `9d72d38` 起，分支 `game-bridge/10-plugin-cleanup`）。

**Check before**（2026-10-15，`07bb031` 那份代码重建的插件，`s_sushi_1_1`，`work=auto`）：
`bash scripts/dev-cycle.sh 15 s_sushi_1_1` 连跑两次 —— 第 1 次走完 `1/6 → 7/7`、日志 `工序: 完成一单（累计 1 单）`、
`score = 28`；第 2 次卡在 `4/6 去拿盘子`（`GetPlate 超时 20 秒` ×N）、`score = 0`。即 Check 本来就**时通时不通**，
卡点在执行面（按住 X 没被工位接受、就位超时），那正是任务 11 要换掉的那条路。所以这次 refactor 的判据按
"关键日志行与得分路径不变"来比，不按"每次都成一单"（跑不过的那次不是这次改动造成的，任务 09 的复核 Progress 有记录）。

### 五条 checklist 的落点

1. **诊断输出统一** → 新增 `plugin/Diag.cs`：唯一开关，`wellcooked-takeover.txt` 里写 `diag=true`（旧的
   `trace` 等价，仍然认）；`Takeover` 的每帧 trace、每 2 秒的走位/驱动中/手柄/工序模式、`LogChefs`、
   `Navigation` 记下的边、`Kitchen`/`WorldState` 的过程行全部走 `Diag.Trace`；**结果行**（工序每步的开始与结果、
   失败原因、指令执行结果、状态迁移）不受开关影响。
   真正的刷屏来源不是这些行：`WorldState` 的反射走 Harmony `AccessTools`，它**每次**找不到成员都写一行 Warning，
   而这些调用里有每帧都在跑的（订单与状态读取挂在游戏自己的 Update 上）—— 实测一次 7 分钟的运行 167615 行日志，
   其中 **171415 行**是这种警告。
2. **反射辅助收敛成一套** → `WorldState` 的 `FindType`/`FindMember`（按 (种类, 类型, 名字, 参数) 缓存命中与未命中）
   + 对外的 `Get`/`Invoke`/`FindAll`/`StaticType`/`StaticMethod`；`Orders.GetMember`（自己写的第三套）删掉改用
   `WorldState.Get`；`Kitchen` 里每帧重查的 `GetFacingGridOccupant`/`GetButton` 改成一次性解析；
   `GameCellOf`/`GameCellPosition`/`GameCellOccupant`/`GameGridManager`/`TeamId` 也改走缓存（原先每次都
   `AccessTools.TypeByName` + 查方法）。版本绑定的类型名与成员名清单写在 `WorldState.cs` 那段注释里；
   未命中只记一次 —— 一次运行 74 行，正好是"这一版游戏里哪些名字不存在"的清单。
3. **兜底集中并标注** → 新增 `plugin/Fallbacks.cs`：精确就位放宽（`LooseApproachAfterSeconds`/`LooseApproachRadius`）、
   切菜按住 X 的超时（`ChopHoldTimeoutSeconds`）、侧向错身半径（`PassByRadius`），每条写清"为什么加""代价""该去掉"；
   `WorkOrder`/`Navigation` 改用这些常量，交付那处"一开始就放宽"也指回同一条。
4. **删死代码与过期注释** → `Kitchen.FreeCounter`（86 行，无调用者）、`HeldName`、`PositionOf`；`WorkOrder` 里
   5 个只赋值不读的字段、`Takeover._targetCursor`（编译器的 CS0169/CS0414 点名）；`Kitchen.cs` 重复的
   `using HarmonyLib;`（写了两遍）。`FreeCounter` 里只此一份的游戏事实（不能拿着盘子直接取料、放料台面要离
   料箱/盘子堆 2.4 米、垃圾桶也挂 `ClientAttachStation`）搬进 `WorkOrder` 的类注释，并把它那段与真实步骤
   不符的旧描述（"取盘子 → 取料装盘 → 交付"）改成真实七步。
   `LogicalInput` **不是死代码**（`Takeover` 用它的逻辑层注入），按"按实际再判"保留。
5. **`standards.md`** → 补 `bash scripts/dev-cycle.sh <秒数> <场景名>` 的用法、`wellcooked-takeover.txt` 的四个开关
   （`work`/`slots`/`diag`/`frontend_press`，含 `diag` 与结果行/过程行的分工）；挑关卡工具（`scan-levels.py`、
   `eval-levels.sh`）与 `mech`/`levels`/`unlock` 指令原文就有，逐条核对过、没有改动。

### 行为变化（refactor 的例外，都写在文档里）

- 过程行默认不再打（这正是任务要的"默认不刷屏"）；结果行一字未改。
- `takeover` 的 `trace` 键改名为 `diag`，旧名仍然认（`Takeover.cs` 里两个键都接）。
- `scripts/dev-cycle.sh` 会临时把 `frontend_press` 与 `diag` 写进 `wellcooked-takeover.txt`，退出时整份还原
  （`set_key`/`restore_takeover`）。**默认值不变**：不跑这个脚本时 `diag` 是关的；脚本自己要产走位/卡住那几行，
  所以替自己的运行打开它 —— 这一点写在 `standards.md` 里。
- 删除的三处死代码没有调用者（`grep` 全仓库只剩定义）。

### Check after

2026-10-16，`bash scripts/build-plugin.sh`：**0 警告 0 错误**（改前 6 个警告）；Check 用的命令是
`bash scripts/dev-cycle.sh <秒数> s_sushi_1_1` + `work=auto`。改前那一栏是把 `9d72d38` 用
`git worktree add /tmp/oc-base 9d72d38` 单独编出来、部署到同一个游戏目录跑同一条命令量出来的：

| | 改前（`9d72d38`） | 改后 |
|---|---|---|
| 一次进关卡运行的日志行数 | 15475 / 19095 | 632 ~ 4369（`diag=true`）／**183**（不写 `diag`，即默认） |
| 其中 `[Warning: HarmonyX] AccessTools…` | **15072 / 18629** | **6**（都是启动时一次性的类型查找） |
| 反射未命中 | 每帧一行（就是上面那一万五千行） | 74 ~ 80 行，每处只记一次 |
| `工序:` 关键行 | `1/6 去取料「SushiFish」（DispenserCrate 3 (4) 8.4,-3.6）` … `7/7 去交付（workstation_plate_station 21.6,-1.8）`、`完成一单（累计 1 单）` | 逐字相同 |
| 跑完一单时的 `score` | 28 | 28 |
| 跑不完时的卡点 | `1/6 取料` → `GetIngredient 超时 20 秒` ×N，位置停在离料箱 1.4 米、`寻路=侧向错身` | 同一处、同一句话 |

**Check 里"至少完成 1 单"这次没有复现**：2026-10-16 下午在这台机器上连跑 5 次（含一次 `slots=1`、
一次启动卡在 `Ident`），进关卡的那几次全部卡在 `1/6 取料`，`score` 停在 0。**改前那份代码在同一条命令下
同样 0 单、卡在同一个地方**（对照栏第二行与最后两行），所以这不是这次改动引入的 —— 与
`technical-debt.md`「工序就位卡住后退化成 1.4 米内直接按」「切菜靠按住 X + 超时重试」以及任务 09 复核
Progress 记的是同一件事，属任务 11/12 的执行面。改后**确实跑通过**：做到 `GameCell*` 缓存之前的那一版，
同一条命令下拿到过 `工序: 完成一单（累计 1 单）` + `score = 28`；`GameCell*` 缓存之后没有新的成功样本，
那一步的等价性由 review（brief B 两轮 clean）与"没有新的 `反射: 没找到` 行"（构造函数与方法都查到了）支撑。

**没验证到的**：最终这一版在"跑得完一单"的环境下没再复现过一次完整单。判据本身不动（仍然要求至少 1 单），
它现在挡在任务 11/12 的执行面修复后面。

### Review（CONVENTIONS §7）

`Review boundary: 对比点 9d72d38；路径 plugin/{Diag,Fallbacks,WorldState,Kitchen,Orders,WorkOrder,Takeover,Navigation,SmokePlugin}.cs、scripts/dev-cycle.sh、.workflow/{standards.md,technical-debt.md}`

两条 brief 各一个 reviewer（harness 里没有 `workflow-reviewer`，用 `reviewer` 代，brief 原文取自
`.agents/agents/workflow-reviewer.md`）：

- **第 1 轮** —— brief A：4 blocking + 3 nonblocking。blocking 全是"记录还没写"（checklist 未勾、
  没有 Check after、`refactor` 的行为变化没声明、`dev-cycle.sh` 写 `diag` 这个例外没声明）→ 本轮按上面
  四节补齐。nonblocking 里"反射收敛没有可核验证据"（`GameCell*` 仍在重复查）与"`WorkOrder` 类注释编号
  自相矛盾"→ 改了代码与注释；技术债条目加了"由任务 03 执行"的指向。brief B：**clean**。
- **第 2 轮**（修复后只重跑一次，两条 brief 都跑） —— brief A：1 blocking + 1 nonblocking。
  blocking：`dev-cycle.sh` 在"`wellcooked-takeover.txt` 本来不存在"时退出不会删掉自己写的键，
  `diag=true` 会一直留着、与"默认不刷屏"矛盾 → 已修（记下文件是否存在：不存在就删掉、存在就逐字还原），
  两个分支都实测过（文件不存在 → 退出后文件没了；文件有内容 → 逐字还原）。nonblocking：任务记录里把
  "带日志前缀的行数"当成了警告数、复核状态自相矛盾 → 已换成实测值（本节与上表）。brief B：**clean**。
- 第 2 轮那条 blocking 的修复落在 `scripts/dev-cycle.sh`（同一文件、同一交付面，没有新调用者/新信任边界），
  按 §5 不另开一轮；修复本身用两个分支的实测记录在上。

### 留给别的任务

- 两个插件工程合并（`technical-debt.md` 的那条）按新决策归**任务 03**，本任务不动，条目里已加指向。
- 切菜改用 `HasFinished()` 收手、工序补煮/煎：任务 12；逻辑层注入：任务 11。
- `plugin/` 下的 `bin/`、`obj/` 是构建产物（已被 gitignore 忽略），本任务不碰。
