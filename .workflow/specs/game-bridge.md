# 游戏接入与 Bridge（阶段A）

**Status:** in progress
**Priority:** critical

## What this is for

调度系统要用游戏得分当唯一评价指标，就必须能读游戏状态、能控制游戏里的厨师。缺了这一层，Supervisor、调度算法、策略对比实验全都无处落地，第6周的验收演示和占 60% 的答辩都拿不到分。方案 §9 把游戏接入定为项目基础并要求第3周跑通，而本 effort 开始时已经是第4周，仓库里还只有文档和汇报材料：游戏目录没有 BepInEx，没有插件，也没有 Linux 侧能和游戏对话的进程。任务 01 已经把插件链路打通（BepInEx 加载、Harmony 补丁生效），剩下的部分见下面各任务。

## What it does

- 启动游戏后，游戏内的插件自动连上 Linux 侧的 Bridge；游戏重启后插件自己重连，不需要重启 Bridge。
- 游戏每帧上报世界状态：厨师位置与手持物、订单列表与剩余时间、厨具（锅、砧板、盘子）状态、当前得分。Bridge 解析后写日志。
- Bridge 能下发动作指令（移动到坐标、交互、抓取/放下、投掷），游戏里对应厨师按指令行动。
- 两名虚拟玩家能加入本地合作局，由指令驱动完成订单并正常得分。
- 用户自己导航进关卡：插件在关卡加载前把空着的玩家位用虚拟手柄占上（用户占着的那位不动），进关卡后自动接管我们那几名厨师。第一条策略是最小策略（来回走 + 周期按交互键），调度算法接在同一个驱动点上。
- 任一侧断开，另一侧靠心跳超时发现并记日志，不静默失效。

## How it is built

四个部分，边界固定：

- **游戏插件（C#，BepInEx 5.4.x + Harmony 2）** — 跑在游戏进程内，负责状态读取、输入注入、TCP 客户端。它不做任何调度决策，只执行"某厨师做某动作"。工程放 `plugin/`，构建产物拷进游戏目录的 `BepInEx/plugins/`。输入注入走**设备层**：进程内建一个 `InControl.InputDevice`（`AddControl` 注册控制项）用 `InputManager.AttachDevice` 挂上，每帧设置按键状态（spike game-bridge/06 实测，见 Notes）。
- **数据面（世界状态）** — 插件每 0.5 秒读一次并写 `BepInEx/wellcooked-state.json`：厨师坐标/朝向/手持物（`PlayerControls` + `PlayerIDProvider` + `ServerPlayerAttachmentCarrier.InspectCarriedItem()`）、锅的进度与烧焦（`ServerCookingHandler.GetCookingHandlers()`）、订单（`ServerKitchenFlowControllerBase.GetMonitorForTeam` → `ServerTeamMonitor.OrdersController` → `ActiveRecipes`）、得分（`GetPoints`）、回合计时（`RoundTimer.TimeElapsed` + `m_timeLimit`）。取不到的字段进 `missing`。这是调度算法的输入面；发给 Linux 侧（TCP 上报）归任务 03。
- **状态语义（踩过的坑，写下来省得再查）** — 回合计时 `RoundTimer` **在第一个订单交付前不走**（游戏设计，用户 2026-10-14 确认）：开局 `elapsed=0`、`left` 恒等于满额。所以 `timer` 不能当"回合已开始"的信号，开局阶段的截止期判断只能用订单自己的 `remaining`/`lifetime`。另外厨师坐标取水平面 `(x,z)`（`y` 是高度，恒为 0），障碍不在导航网格里（这个游戏没有可用导航网格），可走区域靠 `PlayerControls.GroundLayer` 向下打射线采样。
- **接管时机** — 插件每 0.5 秒检查一次：玩家位空着就用虚拟手柄占上（`PCPlayerManager.EngagePadToSlot`，protected，反射调；pad 号靠 `PCPadInputProvider.GetActionSet` 反查），所以关卡是按多人加载的；场景里一出现本地厨师控制器（`PlayerControls`）就开始驱动，离开关卡就停止并清掉按键状态。用户自己占着的那一位不动。
- **Bridge（C11，Linux）** — TCP 服务端，唯一连接游戏的进程。解析上报、写日志、回发控制指令；后续版本把世界状态写进共享内存供 Scheduler 和 Worker 读。源码放 `bridge/`。
- **协议（`protocol/`）** — JSON 行协议，四种消息 `state_update`、`action`、`heartbeat`、`config`，字段按方案 §5.2。schema、示例和字段含义落在 `protocol/protocol.md`，两侧各自实现序列化（方案 §5.3）。
- **部署脚本（`scripts/`）** — 安装 BepInEx 到游戏目录、构建插件、启动游戏和 Bridge 的可重复命令。游戏目录是外部依赖，仓库只放脚本和源码。

关键接口：Bridge 监听 `127.0.0.1:14455`（跨机时改绑定地址），插件作为客户端连接并在 2 秒后重试；一行一条 JSON，UTF-8，`\n` 结尾。

失败行为：心跳超时 5 秒判定对端断开；Bridge 记日志并继续监听，插件保留状态继续重连；JSON 解析失败只丢弃该行并记日志（方案 §5.3）。

平台差异只有地址和启动参数：Fedora 走 Proton 前缀内的同一个游戏目录，WSL2 侧插件跑在 Windows 原生游戏里，连 WSL2 的监听端口。代码同一份，不用条件编译（方案 §2.2.1）。

游戏是 **32 位**（`Overcooked2.exe` 和 `UnityPlayer.dll` 都是 PE32/x86），所以 BepInEx 必须装 `win_x86` 包；`scripts/install-bepinex.sh` 从 exe 的 PE 头读架构，不靠人工判断。插件按 `net35` 编译：游戏的 `Overcooked2_Data/Managed/` 里没有 `netstandard.dll`（Unity 2017.4.8f1），引用的是游戏自带的 UnityEngine 程序集和 `BepInEx/core` 里的 `BepInEx.dll`、`0Harmony.dll`。

Wine 默认优先加载自带的 `winhttp`，BepInEx 的 doorstop 就不会被注入（见 BepInEx 文档 `advanced/proton_wine.html`），所以前缀里需要 `winhttp=native,builtin` 的 DLL 覆盖，由 `scripts/set-winhttp-override.sh` 写进 Proton 前缀注册表。

## What it must not do

- 不做调度决策。订单排序、任务分配、资源预订属于 Scheduler；插件和 Bridge 只搬运数据。
- 不用条件编译区分平台（方案 §2.2.1）。
- 不把离散事件模拟器当演示主平台，它只在阶段D 做策略预筛。
- 不提交游戏 DLL 和 BepInEx 二进制到仓库（方案 §11.6）。
- 不追求 Carnival 3-2 那种物理级复现（方案 §7.1）。第一版只要"能读状态、能控角色、能完成订单"。

## What was considered and rejected

- **插件作为 TCP 服务端、Bridge 主动连接**（方案 §6.1、§6.2 的写法）。最强理由：游戏可以随时重启而 Bridge 不必重启，Bridge 只做主动连接。否决原因：方案 §2.2.1 架构图和 §9 阶段B 都要求 Bridge 监听；WSL2 默认 localhostForwarding 让 Windows 侧直连 WSL2 的监听端口，反向要取宿主 IP 并处理防火墙；游戏由人手动启动，插件重连比维护监听生命周期更简单。
- **BepInEx 6（bleeding edge）**。它对新版 Unity 的 doorstop 有更新。否决原因：游戏是 Unity 2017.4.8f1 的老 Mono 运行时，社区插件和文档集中在 5.4.x，第4周不该赌版本。
- **Linux 原生 BepInEx 包**。否决原因：安装的是 Windows 版 `Overcooked2.exe`（Proton 运行），doorstop 必须注入 `winhttp`，只能用 Windows 包；架构按游戏 exe 的 PE 头选，本机是 `win_x86`（`Overcooked2.exe` 和 `UnityPlayer.dll` 都是 32 位）。
- **不装插件、从外部进程读内存或模拟键盘**。否决原因：跨不过 WSL/Windows 边界，也拿不到结构化的订单和厨具状态，答辩时讲不出 IPC 设计。
- **只用 overcooked_ai 仿真环境做实验**。否决原因：评分表要求真实游戏画面，模拟器只留作策略预筛（方案 §8）。

## How we will know it works

- **W1.** 启动游戏后（标题画面即可，日志在这之前就已经写完），游戏目录 `BepInEx/LogOutput.log` 里有 BepInEx 启动行和 `WellCooked.Smoke` 插件加载行，没有 `Failed to load`。
- **W2.** 同一次启动的 `LogOutput.log` 里出现 smoke 插件 Harmony 补丁的日志行（证明补丁真的执行，不只是程序集加载成功）。
- **W3.** Bridge 没启动时启动游戏，插件日志显示重连；随后启动 Bridge，5 秒内建立连接（`bridge.log` 出现 `client connected`）。
- **W4.** 游戏里跑一局，`bridge.log` 收到 `state_update`，其中 `score` 与画面得分一致，`chefs` 数量与场上厨师数一致（人工比对）。
- **W5.** 通过 Bridge 发一条 `action` 让 0 号厨师移动到指定坐标，游戏里该厨师到达目标附近（一格以内）。
- **W6.** 两名虚拟玩家进入同一局，用固定动作序列完成 3 个订单，游戏得分为正。
- **W7.** `kill` 掉 Bridge，游戏不崩溃，插件继续重连；重启 Bridge 后连接恢复。

测试层次：W1、W2 只能靠日志证据（插件无法单测）；W3、W4 用脚本假客户端和真实游戏各跑一次；W5、W6 只能实测（游戏行为没有可断言的单元）；W7 用真实游戏加 `kill`。Bridge 侧的协议解析做单元测试（`tests/` 里放 `state_update` 样本，断言字段解析），因为协议错误最容易到联调才暴露。

坏掉的信号：`LogOutput.log` 不再出现插件行（插件没加载）；`bridge.log` 长时间没有 `state_update`（插件崩了或断连）；游戏里角色不动（输入注入失效）。

## Open questions

- **第一版固定地图与订单集。** 用户 2026-10-14 决定暂缓。可用关卡列表要进到游戏界面才能读，而进菜单需要输入注入（任务 04），所以列表由任务 04 读出并回填到这里，再由用户和组员确认；默认 Story 1-1（方案 §12 建议：无传送带、无移动平台）。影响任务 05 的验收地图和阶段D 的实验输入。
- **WSL2 ↔ Windows 的连通方案。** 由组员在 Windows 侧验证：先试 localhost，再试 WSL2 IP 加防火墙规则（方案 §2.2.1）。只影响实验室乙的启动参数，不影响代码。

## Notes

- 方案 §9 阶段A（A1–A4）是本 effort 的主体；§6.1 Bridge 职责、§5.2 协议、§5.3 联调顺序是接口依据。
- 任务文件：`.workflow/tasks/game-bridge/`。
- 阶段B（Supervisor、Scheduler、Worker 骨架）是另一个 effort，等本 effort 跑通状态读取后开始。
- 相关决策：`decisions.md: 2026-10-14 - TCP 连接由 Bridge 监听，游戏插件作为客户端`、`decisions.md: 2026-10-14 - OS 核心用 C11`、`decisions.md: 2026-10-14 - 不复制 overcooked-supercharged 的代码，只借用游戏类型与字段名`。

### 游戏内部接口的已知起点（2026-10-14 调研，任务 03/04 从这里开始）

类型名和字段名来自参考项目对游戏程序集的读取，属于游戏自身的接口事实：

- 实体遍历：`EntitySerialisationRegistry.m_EntitiesList`。
- 厨师：`ClientPlayerControlsImpl_Default`（`transform` 的位置与朝向、`m_lastMoveInputDirection`、`m_dashTimer`）；手持物在 `ServerPlayerAttachmentCarrier.m_carriedObjects`。
- 厨具：砧板与工作台上的物品 `ServerWorkstation.m_item`、`ServerWorkableItem.m_progress`；锅 `ServerCookingHandler` 的 `GetServerUpdate()` → `CookingStateMessage.m_cookingProgress`；搅拌 `ServerMixingHandler` → `MixingStateMessage.m_mixingProgress`；清洗 `ServerWashingStation.m_cleaningTimer`；加热 `ServerHeatedStation.m_heatValue`；盘子 `PlateReturnController.m_platesToReturn`。
- 订单与得分：`ServerTeamMonitor.Score.GetTotalScore()`（另有 `TotalBaseScore`、`TotalTipsScore`、`TotalSuccessfulDeliveries`）；订单在 `ServerOrderControllerBase.m_activeOrders`，每条 `ServerOrderData` 有 `ID.m_id`、`Remaining`、`Lifetime`、`RecipeListEntry.m_order.m_uID`/`name`/`m_scoreForMeal`；剩余时间用 `flow.RoundTimer.TimeElapsed` 加私有 `m_timeLimit` 算。
- 输入：游戏用 InControl 加自己的 `ILogicalButton`/`ILogicalValue`，入口是 `PlayerInputLookup.GetButton/GetValue`（`$Scripts/Game/Input/PlayerInputLookup`），另有 `XInputDotNetPure` 直接读手柄。**注入走设备层**（spike game-bridge/06 实测）：在游戏进程里建一个 `InControl.InputDevice`（`AddControl` 注册控制项），用 `InputManager.AttachDevice` 挂上，再每帧用 `UpdateWithState` 设置按键状态——这条能驱动前端（挂上后每 3 秒按 0.3 秒 A，游戏从标题画面推进到开场过场）。逻辑层替换（补 `PlayerInputLookup` 返回自己的包装对象，supercharged 的 `patch/AlteredComponents/InputPatching.cs` 和 TAS 的 `TASControl.cs` 的做法）在本次测的按钮和场景里不驱动前端；它在关卡内能不能用这次没测，暂不采用。
- InControl 在这个 build 里的两个坑（实测）：`InputDevice.UpdateWithState` 不是 protected，子类要反射调用；`InputControlType` 没有 `LeftStickX/Y`、`DPadX/Y`，摇杆和方向键按方向拆成 `LeftStickUp/Down/Left/Right`、`DPadUp/Down/Left/Right`，按键是 `Action1..4`、`LeftTrigger/RightTrigger`、`LeftBumper/RightBumper`。
- 菜单自动加入（来自 supercharged `bridge/SessionSetup.cs`）：建 `TasLocalPad(slot)` → `InputManager.AttachDevice(pad)` → 反射调 `PCPlayerManager.EngagePadToSlot(ControlPadInput.PadNum, EngagementSlot)` → 没有用户时 `ServerUserSystem.AddUser` 并等 `UsersChanged`/4 人同步。这条比"人工按 A"可靠，是任务 05 的首选做法。
