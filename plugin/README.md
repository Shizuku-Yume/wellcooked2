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
- `state_update`（TCP，每帧）：字段名以 `protocol/protocol.md` 为准，构造在 `plugin/Link/StateFrame.cs`。
  协议里可选的字段取不到时**省略**（不是填 0）：`chefs[].state`、`orders[].ingredients`、
  `resources[].chef_id` 这一轮没映射；锅/砧板读不到状态时只报编号。

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
