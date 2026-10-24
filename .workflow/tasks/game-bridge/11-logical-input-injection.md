# 11: 接入 Overcooked2-TAS 的控制方案（逻辑层注入 + 帧步进）

**Effort:** game-bridge
**Task type:** feature
**Base commit:** 0608de6
**Base branch:** main
**Started:** 2026-10-24T11:32:09+08:00
**Status:** paused

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

- [x] 逻辑层按键：`ILogicalButton` 包装（`IsDown/JustPressed/JustReleased`，按 `CanButtonBePressed()` 门控）
- [x] 逻辑层摇杆：`ILogicalValue` 包装（`GetValue()`）
- [x] 安装/卸载：在 `PlayerControls` 就绪后替换 control scheme 字段，退出关卡/换手柄时还原
- [x] 前端加入仍走设备层虚拟手柄（回归：`frontend_press=true` 仍能自己进关卡）
- [x] 帧步进：`step=<n>` 指令暂停并推进 n 帧（调试确定性用）
- [x] 驱动处改走逻辑层，`Takeover` 不再直接写 InControl 控制项

**Done when:** 上面每条打勾，Check 里两处证据都有（完成一单 + 单帧推进）。

## Progress

Dirty at start: none（本任务在独立 worktree `../overcooked-schedule-gb11`；主工作树里
`plugin/` 与 `levels/` 有用户未提交的改动，本任务不碰）。

**Check before（2026-10-24，基线 0608de6）**

```sh
bash scripts/dev-cycle.sh 20 s_wellcooked_demo_2
# === 指定进关: s_wellcooked_demo_2 厨师=4
# === 进关: s_wellcooked_demo_2 厨师=4 订单=0
# t=39.7 One(-3.20,-5.05) Two(7.20,-7.20)
# 计数: 已到达=0 卡住=2 不可达=0 路径走完=0 没登记目标=0
```

日志里 `工序: 接单 Sushi_PlainPrawn` → `1/6 去取料` → `GetIngredient 精确就位卡住了，放宽到
1.4 米直接试`；`score=0`，40 秒内没有交付。与任务 09/13 记录的输入/导航阻塞一致，Check 不通过。

**实现（2026-10-24）**

- `LogicalInput` 不再补 `PlayerInputLookup.GetButton/GetValue`，改成在厨师出现后替换
  `PlayerControls.ControlScheme` 的 5 个字段；按键继承游戏的 `LogicalButtonBase`
  （`JustPressed`/`JustReleased` 的领取语义与真按键一致），`IsDown/GetValue` 都按
  `CanButtonBePressed()` 门控。`Attach` 每次厨师扫描幂等重试，`Detach` 退出关卡时原样装回。
- 关卡内只写逻辑层：`Takeover.Drive`/`DriveWorkOrder` 遍历所有本地厨师，`ActionDriver.Feed`
  只调 `LogicalInput`。设备层只剩前端 `PressForJoin`（`Grep Pad.Want` 仅剩这一处）。
- **修正逻辑 Y 轴方向**：`PlayerControlsHelper.GetControlAxis` 把逻辑 Y 取反成世界 z
  （`z = -MoveY`），调用方给的是世界方向 `(x,z)`，包装里再取反。修复前目标在南、厨师往北
  （`GetIngredient` 卡在料箱反方向）；修复后同一局走到料箱 0.06 米、按 A 拿到 `SushiFish`、
  走到砧板 0.18 米、按 A 放下，日志都带 `逻辑层=已装`。
- 帧步进 `FrameStep`：`step=<n>` 改 `TimeManager.m_debugtimeScale`，不是只写 `Time.timeScale`
  （游戏的 `TimeManager.Update` 每帧覆盖后者，实测 step 后仍是 1.00）；推进后保持暂停，
  `step=0` 恢复。`Commands.Tick` 移到 `SmokePlugin.Update`，暂停期间也读指令。
- `dev-cycle.sh` 的进关状态读取加了重试：插件整份重写 JSON 时脚本会撞上空文件，曾把一次正常
  进关报成 Python traceback。

**Check after**

| 项 | 证据 |
|---|---|
| 逻辑层接管 | `logical: 玩家 One 的 control scheme 已接管` / `玩家 Two...`；trace `逻辑层=已装` |
| 移动方向 | 修复 Y 后 `GetIngredient` 从 `(11.43,-1.58)` 走到料箱 `(8.4,-3.6)` 附近 0.06 米 |
| A 键 | `GetIngredient 按交互（手上「」，面前是「DispenserCrate 3 (4)」，台面上是「（空台面）」）` → 下一步 `手上「SushiFish」` |
| 放料 | `DropOnCounter 按交互（手上「SushiFish」...）` → 下一步 `3/6 切料`、`手上「」` |
| X 键 | trace `用台面=True`（逻辑层喂给 `m_worksurfaceUseButton`） |
| 单帧推进 | `step=1`：`Time.time=29.944 frame=4523` → `Time.time=29.952 frame=4524`；`pause=1` 显示 `Time.timeScale=0.00`；`step=0` 恢复 `1.00` |
| 完成一单 | **运行时通过、证据未保留**（`s_sushi_1_1`）：日志出现 `工序: 完成一单（累计 1 单）`，完整顺序 1/6 取料 → 2/6 放砧板 → 3/6 切 → 4/6 拿盘子 → 5/7 装盘 → 6/7 拿走 → 7/7 交付；同一 run 的 state 读数 `score=28`。随后一次 run 覆盖了 state 文件，`score=28` 的 artifact 没留下（Brief A blocking 1）。 |

### 解除（2026-10-24）

原计划以为“完成一单只差输入层”，实测后暴露两处执行面问题；都在本任务里做了最小修复，
复查后整单通过（上表）：

1. **切菜时料换了位置。** 厨师开始用工位后，料从 `ClientAttachStation.m_item` 转到
   `ClientWorkstation.m_item`；`Kitchen.WorkState` 只看前者，下一帧就误判“板上没料”，
   工序于是跳过切菜。现在两处都看（优先 attach station）。
   日志里的 `反射: 没找到属性/字段 ClientWorkableItem.HasFinished` 是查找过程噪声：
   `WorldState.Get` 在属性/字段未命中后还会找同名方法，`HasFinished()` 本来就能调到。
2. **单人贴墙也走侧向错身。** 错身本意是两名厨师互推；单人时它朝固定方向推墙
   （实测停在 `20.6,-5.82`、方向 `(1,0)`）。`Navigation` 现在只在 1.2 米内有另一名本地厨师
   时才错身（`Takeover` 每次扫描位置更新 `Crowded`）。

修完后 `4/6 GetPlate` 能走到盘子堆，`s_sushi_1_1` 完成 1 单（同一 run 的 state 读数 `score=28`）。
任务 12 仍要做煮/煎与切步的 `HasFinished` 收手；这里只修了“切菜期间料的位置”这条前置。

## Review（round 1）

Review boundary: `0608de6...HEAD` + working tree；round 1；paths `plugin/`、`scripts/dev-cycle.sh`、
`.workflow/standards.md`、任务 11；briefs A,B；trust boundaries: 前端虚拟手柄、关卡内逻辑输入、
指令文件帧步进；findings: 2 blocking / 1 nonblocking。

- **Brief A（built as asked / process）：blocked**，2 条 blocking：
  1. `score=28` 没有同 run 的 state/log artifact（后续 run 覆盖了 state）；日志能证明完成一单，
     但没有同 run 的分数记录。
  2. `step=1` 的 `Time.time/frame` 前进有日志，但没有记录 `wellcooked-state.json` 的前后变化；
     Check 原文要求“状态文件随之更新”。
- **Brief B（quality）：pass**，1 条 nonblocking：`FrameStep` 在“游戏本来已暂停”或连续 `step` 时
  会把 0 当成原始倍率，`step=0` 恢复成 1。已在 `FrameStep` 加 `_ownsPause`：只记录第一次接管前的
  倍率，`step=0` 只恢复自己按下的暂停；该修复尚未重跑单帧 Check。

## Paused（2026-10-24）

代码已完成（分支 `game-bridge/11-logical-input-injection`，worktree
`/home/shizuku/Project/overcooked-schedule-gb11`）。未归档，因为 Brief A 的两条证据缺口还没消。
下一步（不需要改代码）：

1. 跑一次 `bash scripts/dev-cycle.sh 180 s_sushi_1_1`，在日志出现 `工序: 完成一单` 后立刻把
   `wellcooked-state.json` 和 `LogOutput.log` 复制到 `/tmp/`（下一次 run 会覆盖/删除它们）。
2. 在暂停状态读一次 state 的 `t`/mtime，`step=1` 后再读一次，记录文件确实更新；同时保留
   `step: 已暂停 → 已推进 1 帧` 日志。
3. 证据到手后重跑 Brief A；Brief B 已 pass，`FrameStep` 的暂停所有权修复只需补一次 step 日志。

环境注意：连续 `dev-cycle.sh` 会在游戏仍持有日志时 `rm BepInEx/LogOutput.log`，之后该 run 的
日志不可见（本轮最后两次就没抓到日志）；抓证据前先停掉旧的 dev-cycle/游戏进程，
或启动后直接把日志复制走。若同 run 证据始终抓不到，另一条路是把 Check 收窄成
“逻辑层驱动移动/A/X + `step` 单帧”，整单验收转给 09/13——这需要用户点头。

