# 11: 接入 Overcooked2-TAS 的控制方案（逻辑层注入 + 帧步进）

**Effort:** game-bridge
**Task type:** feature
**Base commit:** 0608de6
**Base branch:** main
**Started:** 2026-10-24T11:32:09+08:00
**Status:** active

**Delivers:** 插件的输入注入从"设备层 InControl 反射"换成**逻辑层**：把 `PlayerControls.ControlScheme` 的
`m_pickupButton`/`m_worksurfaceUseButton`/`m_dashButton`/`m_moveX`/`m_moveY` 换成自己实现的
`ILogicalButton`/`ILogicalValue`（按 `playerControls.CanButtonBePressed()` 门控），前端"按 A 加入"仍走设备层虚拟手柄；
另加**帧步进**（暂停 + 单帧）作为调试手段。行为验收：`work=auto` 仍能完成一单，且不再依赖
"X 键到底映射到 InControl 哪个控制项"这种猜测。

来源：`gua248/Overcooked2-TAS` 的 `plugin/TASControl.cs`（`TASLogicalButton`/`TASLogicalValue` + 在
`TASControl.cs:730-741` 处替换 control scheme 的字段）。对照我们现在的 `plugin/VirtualPad.cs`
（`UpdateWithState` 反射 + `Action1/Action3`）。

为什么值得换：
- 现在 pickup/interact 靠猜控制项（`Action1` vs `Action3`），任务 09 就在这上面丢过一次交互；
- 逻辑层天然 per-player、可给精确的 `JustPressed/JustReleased` 边沿，不用管 InControl 的控制项枚举差异；
- 前端加入必须走设备层（游戏自己枚举手柄），两条路各管一段，边界清楚。

**Blocked by:** 10
**Files:** `plugin/`（新增 `LogicalDrive.cs` 或改造 `VirtualPad.cs`/`LogicalInput.cs`、`Takeover.cs` 的驱动处、`Commands.cs` 加 `step`）
**Read first:** 任务 09 的 Progress「切菜键是 X 不是 A」那段；`/tmp/gua/Overcooked2-TAS/plugin/TASControl.cs`（参考实现，GPL-3.0 只读思路不拷码）
**Covers:** W6
**Check:** `bash scripts/dev-cycle.sh` 进关卡后 `work=auto` 完成至少 1 单；日志里交互按键与朝向由逻辑层给出
（不再出现 InControl 控制项名）；`step=1` 能让游戏单帧推进且状态文件随之更新。

- [ ] 逻辑层按键：`ILogicalButton` 包装（`IsDown/JustPressed/JustReleased`，按 `CanButtonBePressed()` 门控）
- [ ] 逻辑层摇杆：`ILogicalValue` 包装（`GetValue()`）
- [ ] 安装/卸载：在 `PlayerControls` 就绪后替换 control scheme 字段，退出关卡/换手柄时还原
- [ ] 前端加入仍走设备层虚拟手柄（回归：`frontend_press=true` 仍能自己进关卡）
- [ ] 帧步进：`step=<n>` 指令暂停并推进 n 帧（调试确定性用）
- [ ] 驱动处改走逻辑层，`Takeover` 不再直接写 InControl 控制项

**Done when:** 上面每条打勾，Check 里两处证据都有（完成一单 + 单帧推进）。

## Progress

Dirty at start: none（本任务在独立 worktree `../overcooked-schedule-gb11`；主工作树里
`plugin/` 与 `levels/` 有用户未提交的改动，本任务不碰）。

