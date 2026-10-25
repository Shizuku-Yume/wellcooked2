# 09: 实机 _2 两个厨师系统控制完成一单

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** <commit-sha>
**Base branch:** small/os-core-acceptance-fixes

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
