# 07: 进入关卡后自动接管并驱动厨师

**Effort:** game-bridge
**Task type:** feature
**Base commit:** a897c08adf298fc3b0a8f27690e258e00ea18635
**Started:** 2026-10-14T15:26:44+08:00

**Delivers:** 用户在菜单里自己导航并进入关卡，插件自动接管：进关卡前把空着的玩家位用虚拟手柄占上（所以关卡里有两名厨师），进关卡后立刻驱动我们那几名厨师移动、周期按交互键；同时每 0.5 秒把厨房世界状态（厨师坐标/朝向/手持物、锅的进度与烧焦、订单与剩余时间、得分、回合计时）写成 `BepInEx/wellcooked-state.json`，这是调度算法的输入面。

**Blocked by:** None
**Finished:** 2026-10-14T20:42:54+08:00
**Files:** `plugin/`（`Takeover.cs`、`VirtualPad.cs`、csproj 引用）、`scripts/`
**Read first:** spec「What it does」「How it is built」「游戏内部接口的已知起点」；spike `game-bridge/06` 的 Findings（设备层的坑）；方案 §9 阶段A3/A4
**Covers:** W5

**Check:** `bash scripts/run-game.sh` 起游戏，手动导航进入任意一张地图的关卡 → `BepInEx/LogOutput.log` 出现接管行与每名厨师的驱动行，画面里被接管的厨师自己来回移动并周期按交互键（两帧截图位置不同）。

- [x] 进关卡前把空玩家位用虚拟手柄占上，人数可配置（`wellcooked-takeover.txt` 的 `slots=1,2,3,4`）
- [x] 关卡开始的判定不误触发：前端/菜单里不驱动，只在出现本地厨师控制器后开始
- [x] 最小策略写在代码里并注明"调度算法由后续任务接"：现在是任务 08 的导航（给个工位就走过去），比来回走更接近真实用法
- [x] 日志有接管时刻、每个厨师的玩家号与手柄号、开始/停止驱动
- [x] 与用户的物理手柄共存：只驱动我们自己的虚拟手柄对应的位（`OwnedPads`），不碰用户占着的那位
- [x] 离开关卡回到前端时停止驱动并清按键（`StopDriving` 把按键与摇杆归零、`LogicalInput.Clear()`；日志实测出现"离开关卡，停止驱动"）
- [x] 世界状态落盘：每 0.5 秒一份 JSON（实测 `score/timer/orders/recipes/cooking/stations/chefs` 都在，`chefs` 含坐标/朝向/手持物）；取不到的字段进 `missing`

**Progress:**
Dirty at start: none

Check before：没有接管代码，插件只会打冒烟补丁和导出类型清单，Check 跑不起来。

已完成、已提交在分支 `game-bridge/07-level-takeover`（提交 `game-bridge/07: 进关卡前占位、进关卡后接管驱动`）：

- `plugin/Takeover.cs`：每 0.5 秒检查一次，空玩家位用虚拟手柄占上；出现本地厨师控制器就开始驱动，离开关卡停止并清按键。
- `plugin/VirtualPad.cs`：正式版虚拟手柄（按枚举注册全部控制项，按键与摇杆状态反射写入）。
- 参数文件 `BepInEx/wellcooked-takeover.txt`：`players=2`、`frontend_press=false`（改文件 5 秒内生效，不用重新编译）。
- 实测到的一半：游戏起到标题画面时日志已出现 `takeover: 已占玩家位 1（pad 1）`、`已占玩家位 2（pad 2）`，说明占位链路通了。

**下一步（等用户操作）**：游戏已经起着，需要用户自己导航进一张地图的关卡（键盘空格继续，或用他自己的手柄按 A）。
进去之后要看的是：日志有没有 `takeover: 检测到 N 名厨师，开始接管驱动` 和每 2 秒一行的 `驱动中`，画面里我们占的位对应的厨师有没有自己来回走、周期按交互键。
如果关卡里没出现我们那两位（可能前端要按 A 才算加入），把 `wellcooked-takeover.txt` 的 `frontend_press` 改成 `true` 再看一次。

**范围追加（2026-10-14，用户要求）**：用户明确要求"先拿到游戏里的所有数据（人物坐标等等），算法再根据这些数据实时寻路调度"。所以本任务除了接管驱动，还包含**数据面**：读厨房世界状态并落盘。真正把数据发给 Linux 侧（TCP 上报）仍归任务 03，等组员的 `protocol/protocol.md`。

**数据面进度**：`plugin/WorldState.cs` 已写。取值路径：`ServerKitchenFlowControllerBase.GetMonitorForTeam/GetPoints/RoundTimer` → `ServerTeamMonitor.Score/OrdersController` → `ServerOrderControllerBase.ActiveRecipes`；锅走静态 `ServerCookingHandler.GetCookingHandlers()`；厨师走 `PlayerControls` + `PlayerIDProvider` + `ServerPlayerAttachmentCarrier.InspectCarriedItem()`。前端实测输出：
`{"t":18.80,"scene":"StartScreen","chefs":[],"score":null,"orders":[],"timer":null,"cooking":[],"missing":["ServerKitchenFlowControllerBase"]}` —— 前端没有厨房流程控制器，如实写进 `missing`。进关卡后的真实数据待验证。

Review boundary：还没跑审查（任务未完成）。

## Check 结果（2026-10-14，任务 08 收尾时一并验完）

`bash scripts/dev-cycle.sh`（重启 → 虚拟手柄自己走完前端进关卡 → 打日志）：

- 日志：`已占玩家位 1..4`、`检测到 4 名厨师，开始接管驱动`、`离开关卡，停止驱动` 都出现；
- 状态文件：`score=0`、`timer={elapsed,round,left,limit}`、`orders=2`、`stations=9`、
  `chefs=[{player,local,pos,height,facing,held}]`、`missing=[]`；
- 厨师被驱动移动（见任务 08 的 169 条目标统计）。

补的一处：`score` / `timer` 取不到时原来只写 `null`，现在会同时进 `missing`（任务要求"取不到的字段进 missing，不假装拿到"）。
