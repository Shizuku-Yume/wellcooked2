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

Review（flow-verify brief A，2026-10-14）：blocked，1 blocking / 2 nonblocking。

- blocking：Findings 的结论写宽了——问题问的是"控制厨师"，本次只证实了设备层能驱动前端。已把结论收窄成"设备层能驱动前端/加入流程"，并加了一段范围说明，写明关卡内厨师动作留给任务 04；spec 里"逻辑层只适合关卡内已有玩家"这个没测过的说法也一并改掉。
- nonblocking：证据没写来源 → 补上截图路径与"截图在 `/tmp`、游戏日志每次运行前被清空"的说明；构建产物 `plugin/bin`、`plugin/obj` 是本任务构建产生的，已被 `.gitignore` 忽略。
- Review boundary: `fd300b8e0903ab31ba59564511940f495d5564f7` → main（`69100c3`、`09bb809`）+ `spike/game-bridge-06`（`f7a0554`）; round: 1; briefs: A; boundaries: 游戏进程内输入注入、游戏目录构建产物; findings: 1 blocking / 2 nonblocking

**Finished:** 2026-10-14T14:52:00+08:00

## Findings

**Result:** 设备层能控制游戏。**路线 B（在游戏进程里建一个 `InControl.InputDevice`，用 `InputManager.AttachDevice` 挂上）能驱动游戏的前端/加入流程**：挂上虚拟手柄后，A 键（每 3 秒按 0.3 秒，共 30 秒）把游戏从标题画面推进到开场剧情过场。路线 A（替换 `PlayerInputLookup` 返回的逻辑按钮）在本次测的按钮和场景里不驱动前端。

**关于 Check 的措辞：** Check 写的是"标题画面的卷帘门打开"。实测到的可见表现是**标题画面被消费、进入开场过场**（两次运行的截图都是过场画面），没有逐帧抓到卷帘门动画本身。按行为判定算通过——继续提示被消费了——但"卷帘门打开"这个具体画面没有被单独证明，这里如实记下。

**范围（重要，别读宽了）：** 本次只证实"设备层能驱动前端/加入流程"。**关卡内厨师的动作（移动、交互）没有验证**——进关卡要先过前端和加入流程，那是任务 04 的验收内容。所以这次回答的是"能控制游戏、注入通道可用"，不是"能控制厨师"；"能控制厨师"由任务 04 证实。

**路线 A 的证据**

- 标题画面阶段游戏查询的 8 个逻辑输入（日志 `control-spike query:`）：`GetFixedButton(UISelect, PlayerGameInput)`、`GetEngagementButton(PlayerGameInput)`、`GetFixedButton(UICancel, PlayerGameInput)`、`GetFixedButton(Curse, PlayerGameInput)`、`GetFixedButton(DebugMenu, PlayerGameInput)`、`GetButton(DebugMenu, One)`、`GetButton(UISelect, One)`、`GetUIButton(UISelect, One)`。
- 强制开关是一个文本文件，内容为要按的按钮名，插件每 0.5 秒重读一次；被强制的按钮每 0.4 秒"按一次"（按下 → 领取 → 释放），也就是在标题画面反复按。
- 把 `GetEngagementButton` 的返回值换成这个包装对象后，日志出现 `control-spike: 强制按钮 GetEngagementButton 被轮询 2400 次（最近一次 JustPressed）`——游戏确实在轮询我们塞进去的对象，但画面一直停在标题画面（截图 `/tmp/spike2.png`，抓于该次运行末尾）。
- 结论要限定：**本次测的这个按钮、这个场景**（标题画面的继续/加入流程）没有被逻辑层驱动。这不等于"前端普遍不走逻辑按钮层"——前端其它流程、关卡内已有的玩家输入这次都没测。注入点本身是成立的（对象被轮询），所以逻辑层没被彻底排除，只是不能用来过前端。

**路线 B 的证据**

- 两次独立运行，日志都有 `control-spike: 虚拟手柄已挂上，接下来 30 秒每 3 秒按一次 A（跳过不存在的控制: LeftStickX, LeftStickY, DPadX, DPadY）`，随后画面从标题画面进入开场过场：截图 `/tmp/spike5.png`（第一次）、`/tmp/spike6.png`（第二次，同一份代码另加控制名清单日志）。两次都是"标题画面消失、进入洋葱国王王座厅过场"，由看图确认。
- 挂载时机：InControl 初始化之后才有效，借游戏主循环里已有的补丁延时挂载。
- 注意：截图在 `/tmp`（重启会丢），日志在游戏目录 `BepInEx/LogOutput.log`（每次运行前被清空）。仓库里没有留这两样，只有本节抄录的日志行；要复现就照 `spike/game-bridge-06` 分支的代码重跑。

**给任务 04 的代价与坑（都在这次实测里撞到过）**

- `InControl.InputDevice.UpdateWithState` 在这个 build 里不是 protected：子类直接调用编译不过（CS0103），要反射调用。
- `InputControlType` 的 83 个名字里没有 `LeftStickX/Y`、`DPadX/Y`：摇杆和方向键按方向拆开，叫 `LeftStickUp/Down/Left/Right`、`DPadUp/Down/Left/Right`，按键是 `Action1..4`、`LeftTrigger/RightTrigger`、`LeftBumper/RightBumper` 等。
- 这条路绑死 InControl 的私有 API（所以要走反射）和游戏对设备的枚举顺序；supercharged 的 `TasLocalPad` + `bridge/SessionSetup.cs` 是现成参考（MIT 之外的仓库，只借做法不抄代码）。
- 按键时序要自己造边沿（按下 → 被领取 → 释放），并注意游戏的帧同步：一个 `Update` 里设置的状态只对这一帧有效。

**Recorded in:** spec「How it is built」「游戏内部接口的已知起点」；`decisions.md: 2026-10-14 - 输入注入走进程内 InControl 设备`。

**代码去向:** 保留在分支 `spike/game-bridge-06`（`plugin/ControlSpike.cs`、`plugin/VirtualPad.cs`、csproj 里的 `Assembly-CSharp` 引用），不进 main；任务 04 按正式要求重写，可以拿它当起点。
