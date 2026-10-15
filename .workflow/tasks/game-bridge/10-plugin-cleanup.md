# 10: 插件整理：把这一轮堆出来的诊断与兜底收敛

**Effort:** game-bridge
**Task type:** refactor
**Base commit:** <commit-sha>

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

- [ ] 诊断输出统一：`probe`/`dump`/`mech`/`levels` 与工序日志的开关集中到一处，默认不刷屏
- [ ] 反射辅助合并：`WorldState` 的 `Get/Invoke/FindAll` 收敛成一套并补注释（哪些字段名与游戏版本绑定）
- [ ] 兜底集中并标注：`_looseApproach`（1.4 米 + 不校验朝向）、按住 X 等超时、侧向错身 3 米限制，都移到一处并写清"为什么是兜底、什么时候该去掉"
- [ ] 删死代码与过期注释（含已确认不用的 `FreeCounter`/`LogicalInput` 路径，按实际再判）
- [ ] `standards.md` 的 `Running it` 补上挑关卡工具与 `unlock/levels/mech` 指令的用法

**Done when:** 上面每条打勾，且 Check 的单子能跑完、`git diff` 里没有行为变化。

## Progress
