# `plugin/`：游戏侧插件

跑在《胡闹厨房2》进程里的 BepInEx 5 插件（net35，`WellCooked.Smoke.csproj`）。
它只做三件事，**不做任何调度决策**（`specs/game-bridge.md`）：

1. **读世界状态**：写 `BepInEx/wellcooked-state.json`（给人看、给 `scripts/export-kitchen.py` 读），
   并把同一批数据按协议编码成 `state_update` 发给 Linux 侧的 Bridge（任务 03）。
2. **注入输入**：前端"按 A 加入"走设备层虚拟手柄；关卡内的动作走设备层手柄 + 逻辑层
   （`LogicalInput`），由本地的工序状态机（`WorkOrder`）或 Bridge 下发的动作驱动。
3. **调试通道**：`BepInEx/wellcooked-command.txt` 一行一条指令（`load`/`restart`/`mech`/`levels`/…），
   配置在 `BepInEx/wellcooked-takeover.txt`（见 `.workflow/standards.md`）。

## 读的是游戏的哪些对象与字段

**类型名与字段名都是这一版游戏（Unity 2017.4.8f1 / OC2）的私有接口**，游戏更新后要重新核对。
核对办法：日志里 `反射: 没找到…（同一处只记一次）` 那些行就是失效的名字（任务 10 收敛出来的，
未命中按 (种类, 类型, 名字, 参数) 只报一次）。

| 要什么 | 从哪读 | 取值含义 |
|---|---|---|
| 厨师 | `PlayerControls`（场景里枚举）；编号 `PlayerIDProvider.GetID()`（**强转 int 就是输入槽号**，`Takeover` 与上报都用它）；手持物 `ServerPlayerAttachmentCarrier.InspectCarriedItem()`（`null` = 空手）；位置取 `transform.position` 的 **x/z**（`y` 是高度，恒为 0） | 编号是"哪个厨师"，也是动作指令里的 `chef_id` |
| 得分/订单/计时 | `ServerKitchenFlowControllerBase` → `GetPoints(TeamID)`、`GetMonitorForTeam(TeamID)` → `ServerTeamMonitor.OrdersController` → `m_activeOrders`、`ActiveRecipes`；计时 `RoundTimer.TimeElapsed`、`m_timeLeft`、`m_roundTimer`、`m_timeLimit` | `RoundTimer` **在第一个订单交付前不走**（游戏设计），开局别拿它判"回合已开始"，用订单自己的 `remaining`/`lifetime` |
| 订单 | 每张 `ServerOrderData`：`ID.m_id`、`Remaining`、`Lifetime`；菜名在 `RecipeListEntry.m_order`（ScriptableObject）的 `name` | `Remaining` 是剩余秒；菜名与关卡菜谱表里的名字一致 |
| 锅 | 静态 `ServerCookingHandler.GetCookingHandlers()`；`GetCookingProgress()`（0..1）、`IsCooked()`、`IsBurning()`、`AccessCookingTime`（总时长）；位置从它交出的 `GetCookingHandler()` 组件上取（handler 本身不是 `Component`） | 上报里 `time_left = AccessCookingTime × (1 − progress)` |
| 砧板/工位 | 客户端组件 `ClientWorkstation`、`ClientAttachStation`；**板上有没有料看 `ClientAttachStation.m_item`**（`workstation.m_item` 只在有厨师用时才非空）；切菜进度在 `ClientWorkableItem.m_progress`（整数阶段）/`m_subProgress`/`HasFinished()` | 名字不可靠（盘子堆挂在台面上时返回台面本身），判"面前是什么"要比**格子** |
| 盘子 | `ClientCleanPlateStack`（`GetSize()`）、`ClientPlateStation`（交付口） | 上报里 `plate_stack.count` |
| 料箱给什么 | `ClientPickupItemSpawner.GetItemPrefab()` 的名字（如 `SushiFish`）；prefab 上挂的 `Choppable*`/`Cookable*` 组件决定它要不要切/煮（菜谱树里看不出来） | 导出到 `wellcooked-state.json` 的 `gives`/`prefab_components` |
| 网格 | `GridManager.GetActive(0)` → `GetUnclampedGridLocationFromPos`/`GetPosFromGridLocation`/`GetGridOccupant`，格坐标类型 `GridIndex` | 游戏自己的网格是 **1.2 米**，台面摆在 0.6 米格上，粗一半表示不出中间的障碍 —— 所以可走格自己按 0.6 米采样（`WorldState`） |
| 输入 | 逻辑层 `PlayerInputLookup.GetButton/GetValue`（`ILogicalButton`/`ILogicalValue`，按 `LogicalButtonID`：`PickupAndDrop`/`Dash`/`WorkstationInteract`）；设备层 InControl `InputDevice`（`AddControl` 注册控制项，`InputManager.AttachDevice` 挂上） | 前端"按 A 加入"只能走设备层；关卡内动作两层都能用 |

## 两种状态形状（同一批数据，别混淆）

- `BepInEx/wellcooked-state.json`：给人看与给 OS 侧导出用（`scripts/export-kitchen.py` 读它生成
  `config/kitchen.json` + `config/recipes.json`）。取不到的字段进 `missing` 数组，**不假装拿到了**。
  同目录还有 `wellcooked-terrain.json`（可走格）与 `wellcooked-menu.json`（菜谱表，`menu=1` 导出）。
- `state_update`（TCP，**默认每帧**；`wellcooked-takeover.txt` 里写 `state_ms=<毫秒>` 就按间隔限流）：
  字段名以 `protocol/protocol.md` 为准，构造在 `plugin/Link/StateFrame.cs`。协议的分帧硬上限是单行 1 MiB；
  4 KB 是状态帧的**预算**（超过只在日志里警告一次，不截断也不丢弃 —— 见 `LinkRunner.MaxStateBytes`）。
  协议里可选的字段取不到时**省略**（不是填 0）：`chefs[].state`、`orders[].ingredients`、
  `resources[].chef_id` 这一轮没映射；锅/砧板读不到状态时只报编号。

## 链路配置与重连（`wellcooked-takeover.txt`）

| 键 | 行为 | 生效时机 |
|---|---|---|
| `bridge=<host>:<port>` | 停掉旧链路，建新链路并重新握手 | 最多一个配置轮询周期（5 秒） |
| `bridge=off` | 停掉链路（不再连） | 同上 |
| `state_ms=<毫秒>` | 只改上报间隔（`0` = 每帧）；**不重建 TCP 连接** | 同上 |

心跳与重连参数这一版写死在 `LinkRunner` 里（心跳 2000 ms、断线后等 2000 ms 再连），
没有做成可热改的配置键 —— 协议文档里 `Network.*` 那些键是 net46 参考实现的，net35 演示插件不读它们。

重连的两种耗时别混：**对端立刻拒绝**（端口没人听）时约 2 秒一次；**连接建立阶段超时**时还要叠加
最多 3 秒的 connect 等待，所以两次尝试之间可能看到约 5 秒。日志里都有重试次数。

## 输入注入走哪条路，代价是什么

对应 spec 开放问题"输入注入的具体路径"（`specs/game-bridge.md`，2026-10-16 定稿）：

- **前端（"按 A 加入"、继续、过场）只能走设备层。** 进程内建一个 InControl `InputDevice`
  （`AddControl` 注册控制项）→ `InputManager.AttachDevice` 挂上 → 每帧 `UpdateWithState` 设置按键。
  逻辑层在前端没有对应对象（`PlayerInputLookup` 要玩家位先存在），spike game-bridge/06 实测过这条能
  从标题画面推进到过场。
- **关卡内动作走逻辑层 + 设备层两手都写。** 逻辑层替换 `PlayerControls.ControlScheme` 的
  `m_pickupButton`/`m_worksurfaceUseButton`/`m_dashButton`/`m_moveX`/`m_moveY`（自实现
  `ILogicalButton`/`ILogicalValue`），这是游戏内动作真正读的那一层；设备层再写一遍，是因为厨师是
  挂在我们占的虚拟手柄上的（`EngagePadToSlot`），两条路径都可能是它这次取值的来源，写一致的意图
  比猜哪一层生效便宜。
- **槽号与玩家号是同一个数。** 协议里的 `chef_id` = 输入槽（0 基）；实测
  `(int)PlayerIDProvider.GetID()` 也是 0 基（名字 `Player 1` 的厨师，`GetID()` 转 int 得 0），
  所以 `chef_id` 可以直接喂给逻辑层。**别按名字里的数字猜** —— 这一轮就是先把它当成 1 基，
  结果动作全喂给了另一个厨师（日志里 `玩家号=0 名字=Player 1` 才看出来）。

代价：

| 代价 | 说明 |
|---|---|
| 依赖游戏私有接口 | 逻辑层的字段名、`ILogicalButton` 的签名、`EngagePadToSlot` 都是反射调的私有成员，游戏一更新就可能失效；失效表现为 `反射: 没找到…` 日志 + 动作没效果（`Fallbacks.cs` 集中兜底） |
| 占用 pad 位 | 设备层每接管一个玩家位要挂一个虚拟手柄，一台机器最多 4 个，所以能同时接管的厨师数受空闲 pad 限制 |
| 两条路都要维护 | 前端只能设备层、关卡内两层都写，注入相关的 bug 要分别到两层看（`LogicalInput` 与 `VirtualPad`） |
| 逻辑层要按"这一局谁是谁"写 | 逻辑层的键是 per-player 的，写错玩家号不会报错，只会让另一个厨师动 |

## 一次真实操作的日志片段（任务 04，2026-10-16，`s_sushi_1_1`）

Bridge（`--link listen`）+ 替身 Worker（`kitchenos-worker-stub`，写共享内存输入槽）→ 游戏插件：

```
# 桥侧：每帧把输入槽转成一条 action 下发，插件回 ack
INFO  actions_sent frame=1542 chefs=2 sent=2 idle=1 move=1 interact=0 pickup=0 dash=0 timed_out=0
DEBUG plugin_ack chef=0 action=move ok=1 detail=ok
INFO  actions_sent frame=444  chefs=2 sent=2 idle=1 move=0 interact=0 pickup=1 dash=0 timed_out=0
DEBUG plugin_ack chef=0 action=pickup ok=1 detail=ok
INFO  actions_sent frame=351  chefs=2 sent=2 idle=1 move=0 interact=0 pickup=0 dash=1 timed_out=0
DEBUG plugin_ack chef=0 action=dash ok=1 detail=ok

# 插件侧（游戏日志 BepInEx/LogOutput.log）
link: 收到 action 原文 {"type":"action","chef_id":0,"action":"move","target":{"x":11.37,"y":-4.63}}
action: 厨师位 1 去 (11.4,-4.6)
action: 移动中 厨师位 1 位置=(11.4,-1.7) 距目标=3.0 寻路=路点 1/3 剩余 3.0 路点坐标=(11.4,-2.3)
action: 厨师位 1 按 pickup
action: 厨师位 1 按 dash
```

`action: 厨师位 1` 里的"1"是**日志给人看的**编号（槽号 + 1）；协议与状态文件里是 0 基的 `chef_id`。

## 文件分工

| 文件 | 管什么 |
|---|---|
| `SmokePlugin.cs` | BepInEx 入口：安装各模块、打补丁（失焦暂停、`isFocused`、每帧钩子） |
| `Link/` | 与 Bridge 的链路：`TcpLink`（分帧/心跳/重连）、`Messages`（协议编解码）、`StateFrame`（一帧状态）、`LinkRunner`（配置、握手、上报节奏、动作分发） |
| `WorldState.cs` | 世界状态读取与落盘、可走格采样、反射缓存（`Get/Invoke/StaticMethod/FindAll`） |
| `Kitchen.cs` | 工序要用的查询：料箱/盘子/砧板/出菜台、面前是什么、切菜进度 |
| `Orders.cs` | 固定订单与固定随机源（策略对比要用同一组订单） |
| `WorkOrder.cs` | 工序状态机：取料 → 放砧板 → 切 → 拿盘子 → 装盘 → 拿走 → 交付 |
| `Takeover.cs` | 占位、进关卡后接管、每帧把"想去哪、按不按"喂给手柄 |
| `Navigation.cs` | 0.6 米格上的 A*、路点跟随、卡住处理 |
| `VirtualPad.cs` / `LogicalInput.cs` | 设备层虚拟手柄 / 逻辑层注入 |
| `Commands.cs` / `Diag.cs` / `Fallbacks.cs` | 调试指令通道 / 诊断开关 / 兜底集中处 |
