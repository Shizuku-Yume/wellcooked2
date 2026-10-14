# 06: 插件能不能控制厨师（输入注入可行性）

**Effort:** game-bridge
**Task type:** spike
**Base commit:** fd300b8e0903ab31ba59564511940f495d5564f7
**Started:** 2026-10-14T14:19:01+08:00

**Question:** 插件能不能控制游戏里的厨师（至少做出一次可见的输入效果）？走哪条路线，代价是什么？
**Time box:** 两条路线各试一次，先试补 `PlayerInputLookup`（游戏逻辑层），不成功再试进程内 `InControl.InputDevice`；不做完整动作集，不接 Bridge。

**Delivers:** Answer to: 插件能不能控制游戏，走哪条路线。

**Blocked by:** None
**Files:** `plugin/`（试验代码，可能丢弃）、游戏目录日志
**Read first:** spec「How it is built」「游戏内部接口的已知起点」；`.workflow/spike-tasks.md`

**Check:** 启动游戏，插件在脚本窗口内强制按下逻辑按钮 → 标题画面的卷帘门打开（截图）且日志列出被查询的 `LogicalButtonID`；答案与证据写进 `## Findings`。

- [x] 列出标题画面阶段游戏查询了哪些逻辑按钮/轴 ID
- [x] 至少一次可见的输入效果（画面截图 + 对应日志行）
- [x] 记录这条路线的代价：绑了哪些私有类型、版本变化会不会断、帧同步要求
- [x] 结论回写 spec 与 decisions；试验代码保留还是丢弃写清楚

**Progress:**
Dirty at start: none
Check before（spike 不要求）：无。
游戏窗口的截图用 `import -window <xdotool search --name Overcooked2 的 id>` 抓（`-window root` 在这台 Wayland 上取不到画面）。

**Finished:** 2026-10-14T14:52:00+08:00

## Findings

**Result:** 能。**路线 B（在游戏进程里建一个 `InControl.InputDevice`，用 `InputManager.AttachDevice` 挂上）可以控制游戏**；路线 A（替换 `PlayerInputLookup` 返回的逻辑按钮）被游戏轮询，但驱动不了前端。

**路线 A 的证据**

- 标题画面阶段游戏查询的 8 个逻辑输入（日志 `control-spike query:`）：`GetFixedButton(UISelect, PlayerGameInput)`、`GetEngagementButton(PlayerGameInput)`、`GetFixedButton(UICancel, PlayerGameInput)`、`GetFixedButton(Curse, PlayerGameInput)`、`GetFixedButton(DebugMenu, PlayerGameInput)`、`GetButton(DebugMenu, One)`、`GetButton(UISelect, One)`、`GetUIButton(UISelect, One)`。
- 把 `GetEngagementButton` 的返回值换成"强制按下 0.4 秒"的包装对象后，日志显示 `强制按钮 GetEngagementButton 被轮询 2400 次（最近一次 JustPressed）`——游戏确实在轮询我们塞进去的对象，但画面一直停在标题画面（截图确认）。
- 结论：逻辑层可以替换（注入点成立），但前端的"按 A 继续"走的是设备/加入流程，不是逻辑按钮层。要驱动前端得在设备层做。

**路线 B 的证据**

- 挂上虚拟手柄后日志 `虚拟手柄已挂上，接下来 30 秒每 3 秒按一次 A`，游戏从标题画面进入开场剧情过场（截图两次、两次独立运行都成立）。
- 挂载时机：InControl 初始化之后才有效，借游戏主循环里已有的补丁延时挂载。

**给任务 04 的代价与坑（都在这次实测里撞到过）**

- `InControl.InputDevice.UpdateWithState` 在这个 build 里不是 protected：子类直接调用编译不过（CS0103），要反射调用。
- `InputControlType` 的 83 个名字里没有 `LeftStickX/Y`、`DPadX/Y`：摇杆和方向键按方向拆开，叫 `LeftStickUp/Down/Left/Right`、`DPadUp/Down/Left/Right`，按键是 `Action1..4`、`LeftTrigger/RightTrigger`、`LeftBumper/RightBumper` 等。
- 这条路绑死 InControl 的私有 API（所以要走反射）和游戏对设备的枚举顺序；supercharged 的 `TasLocalPad` + `bridge/SessionSetup.cs` 是现成参考（MIT 之外的仓库，只借做法不抄代码）。
- 按键时序要自己造边沿（按下 → 被领取 → 释放），并注意游戏的帧同步：一个 `Update` 里设置的状态只对这一帧有效。

**Recorded in:** spec「How it is built」「游戏内部接口的已知起点」；`decisions.md: 2026-10-14 - 输入注入走进程内 InControl 设备`。

**代码去向:** 保留在分支 `spike/game-bridge-06`（`plugin/ControlSpike.cs`、`plugin/VirtualPad.cs`、csproj 里的 `Assembly-CSharp` 引用），不进 main；任务 04 按正式要求重写，可以拿它当起点。
