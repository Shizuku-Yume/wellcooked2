# 09: 插件侧工序执行面：按订单自动完成一单

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>

**Delivers:** 插件把"工序"跑通：给定当前订单，自动走完 取料 → 切 → 装盘 → 交付，游戏得分从 0 变正、订单数减一，日志逐步打出"做了什么、目标是谁、结果如何"。

这是任务 04/05 里**游戏侧的那一半**：原子动作执行面（移动/交互/等待，每个动作有明确结果与超时）+ 工序状态机（按菜谱决定下一步、按工位内容物与进度判断能不能做）。它不依赖 Bridge 就能验证；等 02/03（组员的协议与客户端）落地，把"谁来下动作"从本地脚本换成 Bridge 下发即可，执行器不动。

**Blocked by:** None
**Files:** `plugin/`（新增 `Actions.cs`、`WorkOrder.cs`、`Kitchen.cs`；复用 `Navigation.cs`、`WorldState.cs`、`Takeover.cs`）
**Read first:** spec「What it does」「状态语义」；任务 07/08 的 Findings（世界状态字段、导航与到达判定）；`wellcooked-state.json` 现有字段
**Covers:** W6（仅游戏侧；传输侧仍由 04/05 覆盖）

**Check:** `bash scripts/dev-cycle.sh` 进关卡后，日志出现 取料 / 切 / 装盘 / 交付 四步都成功，`wellcooked-state.json` 的 `score` 从 0 变正且订单数减一；同一序列连跑两次都能完成至少 1 单。

- [ ] 原子动作：`MoveTo`（复用任务 08 的导航）、`Interact`（按下→松开一次）、`WaitFor`（按状态判定 + 超时）；每个动作返回明确结果（成功 / 超时 / 做不到），不静默失败
- [ ] 工序状态机：按当前订单的菜谱决定步骤顺序，能读工位内容物与进度决定"这一步现在能不能做"
- [ ] 决策来源隔离：谁下动作由一个接口决定；本地脚本是替身并在代码里注明"由 Scheduler 接管"
- [ ] 日志：每一步的动作、目标工位、耗时、结果；失败有超时与原因
- [ ] 不依赖 Bridge：任务 02/03 未落地时也能跑通本 Check

**Done when:** 上面每条都打勾，且 Check 的两次连跑都有日志与 `score` 证据。
