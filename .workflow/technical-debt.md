# Technical Debt

Code that makes the next change expensive, one entry per finding, so reviews don't
rediscover it and the reason it wasn't fixed yet is kept. `flow-architect` adds
entries; any skill that finds debt may add one.

## Format

Each entry is a level-2 heading naming the concrete symptom, under `## Entries`.
Refer to an entry by its heading.

```markdown
## Order total computed in three places

- **Where:** <files or modules>
- **Added:** <YYYY-MM-DD>
- **Priority:** fix-now | worth-doing | only-if-grows
- **Problem:** <what is wrong, with evidence>
- **Impact:** <what gets harder while it stays>
- **Solution:** <the change, in plain terms>
- **Cost:** <what the fix risks, how big it is>
```

An entry leaves this file in one of two ways:

- Fixed: the task that fixes it deletes the entry in its own commit.
- Declined by the user: delete the entry and append a decision entry to
  `decisions.md` (Decided: not fixing <symptom>; Instead of: fixing it; Because;
  Mine: no; Revisit when), so reviews don't re-propose it.

## Entries

## 工序"就位"卡住后退化成 1.4 米内直接按、不校验朝向

- **Where:** `plugin/WorkOrder.cs`（`_looseApproach`、`_arriveAt`、`_looseApproach ? 1.4f : ReachRadius`）
- **Added:** 2026-10-15
- **Priority:** worth-doing
- **Problem:** 精确就位（0.25 米 + 面前必须是工位）在工位相邻格站不进去时会一直打转，于是加了"卡住 6 秒就放宽到 1.4 米、不再校验朝向"的兜底。出菜口嵌在墙里、盘子堆旁边被台面挡，都会走到这条路上；此时按 A 是按"大概方向"按的，站错格就可能按到别的东西（垃圾桶那次就是这么把鱼扔掉的）。
- **Impact:** 交付/拿盘子这类步骤偶发失败但日志看起来"正常在按"，排查时容易误判成按键问题。
- **Solution:** 把"站哪一格"交给游戏自己的网格判定（`GridNavSpace`/`GetFacingGridOccupant` 反推可用格），或按工位类型分别定义"哪个方向能站"，去掉半径放宽。
- **Cost:** 中，要动工序与导航的到达判定（任务 08 的边界），需要逐关验证。

## 切菜靠"按住 X + 超时重试"，没有按 HasFinished() 收手

- **Where:** `plugin/WorkOrder.cs`（`HandleChop`）
- **Added:** 2026-10-15
- **Priority:** fix-now（并入任务 12）
- **Problem:** 切菜进度在 `ClientWorkableItem` 上（`m_progress`/`HasFinished()`），但当前实现是"按住 X 一直等"，超时（60 秒）才重试；进度读数只用于日志。
- **Impact:** 切完不及时松手，多切/切糊都有可能；超时值成了隐式行为参数。
- **Solution:** 任务 12 里改成按 `HasFinished()` 收手，并把熟度/进度超时改为"报出当前进度"。

## 关卡评估数字依赖"有厨师在场"，且探针比游戏更严

- **Where:** `plugin/WorldState.cs`（`SampleGameGrid`/`KeepLargestComponent`）、`scripts/eval-levels.sh`
- **Added:** 2026-10-15
- **Priority:** fix-now
- **Problem:** 没厨师时用兜底半径 0.35，通道会被判死、连通块虚高；有厨师时用实测半径 0.40，又比游戏自己的通行判定严（游戏网格 1.2 米、厨师连续移动能过 0.9 米的缝），于是"主连通块占比"偏悲观。采样还带 3 米边距，墙外/台面后的可走口袋也算进总格数：任务 13 的四张自制图因此被报成 3/8/12 块，但 `path=` 探测显示厨师位置与关键工位都在主块内、A* 1-6 路点可达。
- **Impact:** 挑关卡和任务 13 的"主块 ≥90%"会把可用地图判成"碎/不可用"，需要人肉复核；这次差点把 `_1`/`_3`/`_4` 当成坏图。
- **Solution:** 评估判据改成"所有 spawn + 关键工位在主块内且两两 A* 可达"，或把采样边距/墙外口袋从分母去掉；`eval-levels.sh` 同时打印主块占比与关键工位探测结果。

## 两个插件工程并存，状态读取与输入注入各有一份

- **Where:** `plugin/*.cs` + `plugin/WellCooked.Smoke.csproj`（世界状态、导航、工序、关卡接管、调试指令）
  与 `plugin/WellCooked.Plugin/`（协议编解码、TCP 链路、每帧状态上报、动作注入）
- **Added:** 2026-10-15
- **Priority:** worth-doing
- **Problem:** 两条线并行做插件，合并后仓库里有两个 BepInEx 工程，职责有实质重叠：状态读取有
  `WorldState.cs`/`Kitchen.cs` 与 `StateReader.cs` 两份，输入注入有 `LogicalInput.cs`+`VirtualPad.cs`
  与 `InputInjector.cs` 两份。类型名实测不冲突（两边 10 : 4 个类型无交集），所以各自都能编过、
  互不报错 —— 也正因如此，编译器不会提醒任何人这里重了。
- **Impact:** 同一条游戏事实（厨师坐标怎么取、交互键是哪个）改一处不改另一处就会漂移；两个工程都往
  `BepInEx/plugins/` 拷，装哪个、要不要一起装，只能靠人记。
- **Solution:** 先定一个存活方向再合并：按链路层（协议/上报/注入接口）与游戏控制层（导航/工序/接管）
  分层，让组员那份当控制层、本分支那份当链路层，共用一套状态读取。`game-bridge.md` 把插件定义成
  "状态读取、输入注入、TCP 客户端"三件事，正好是两边各做了一半 —— 合并方向在那份 spec 里已经有依据。
  **方向已定，由任务 03 + 04 一起做完**：`decisions.md: 2026-10-16 - 两个插件工程合并到 net35 那份`
  选了 net35 那份（演示机）存活，net46 退成参考实现。任务 03 已经把**状态读取与链路**这一半并进来
  （`plugin/Link/`，上报走 `WorldState` 的同一套读取），**输入注入**那一半在任务 04（动作分发与
  按键注入并进 `Takeover`/`LogicalInput`）；04 落地后删掉本条目。
- **Cost:** 大，要动两边共 15 个文件，而且输入注入那条路只有进游戏才能验（本机测不了）。
