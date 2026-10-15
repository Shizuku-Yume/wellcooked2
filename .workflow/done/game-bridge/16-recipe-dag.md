# 16: 菜谱转工序 DAG（`config/recipes.json`）

**Effort:** game-bridge
**Task type:** feature
**Base commit:** 0766952
**Started:** 2026-10-15T23:20:00+08:00

**Delivers:** 转换器把插件导出的菜谱节点（`WorldState.cs:1078-1148` 展开的 `m_composition`）写成
`config/recipes.json`：每道菜一张工序 DAG——节点是取料 / 切 / 煮 / 煎 / 装盘 / 交付，边是前置依赖，
每个节点带资源类型（砧板 / 锅 / 煎锅 / 盘子 / 出菜口）与工时；至少覆盖 3 道菜，其中两道共享中间产物
（例如都用到煮好的饭）。工时优先写实测值，没实测的写 `null` 并在文件里写明来源。

**为什么**：方案 §4.2 把"调度重心下沉到工序级"定为核心问题——多厨师并行执行一张带前置约束的工序图；
没有这张图，Scheduler 的列表调度、关键路径优先都无从下手。现在 `WorldState.cs` 只把菜谱展成嵌套树
（深度上限 4、缺 `m_cookingStep`/`m_progress`、缺工时），还不是调度能用的依赖边。

**Blocked by:** 15
**Files:** `plugin/Orders.cs`（加 `menu=` 指令导出关卡菜谱表）、`plugin/WorldState.cs`（料箱导出它那个 prefab 的组件名，用来判要不要切）、
`scripts/export-kitchen.py`（复用 15 建的转换器，所以排在它后面）、`config/recipes.json`、`config/recipes.example.json`、`.workflow/standards.md`

**Read first:** 方案 §4.2、§6.5 的工序时长示例（取料 1s / 切 3s / 煮 5s / 装盘 2s / 交付 1s，可调）；
`.workflow/maps/prior-art.md` 的"现在就能落地的事"第 4 条；`plugin/WorldState.cs:1078-1148`；
`plugin/WorkOrder.cs` 的步骤枚举（取料 → 放砧板 → 切 → 拿盘子 → 装盘 → 拿走 → 交付）

**Check:** `menu=1` + `python3 scripts/export-kitchen.py` 之后，断言：每道菜的 `fetch`/`mix`/`cook`/`assemble`
都在 `plate` 之前、`deliver` 是最后一步、`plate → deliver` 这条边在、每条边的方向与 `nodes` 顺序一致；
两道菜共用同一节点时 `nodes` 里只有一条；同一份（冻结的）输入跑两次，两个输出文件字节相同。

- [x] 叶子食材 → 取料节点；`Cooked*` → 煮/煎、`Mixed*` → 搅拌（`m_cookingStep` 在节点上不存在，见下面的更正）
- [x] 前置边按菜谱树推导，共享中间产物只出现一个节点（`fetch:Flour` 被 3 道菜共用，`nodes` 里只有一条）
- [x] 每个节点带资源类型与工时；工时来源写在文件里（`durations.source`）
- [x] 覆盖 3 道菜以上（4 道），其中两道共享同一节点（`Flour` 被 3 道共用）
- [x] 格式写进 `config/recipes.example.json`，`standards.md` 里给命令

## Progress

Dirty at start: none（从 main 的 0766952 起，`game-bridge/16-recipe-dag`）。

**插件侧补了两样**：

- `Orders.cs` 加 `menu=1` 指令：把这一关的整张菜谱表写成 `BepInEx/wellcooked-menu.json`
  （`config/recipes.json` 的输入）。菜谱表在 `InitialiseRound` 时就记下来，所以**没有玩家的局也能导出**
  （实测：那一局 `订单: 接管 GetNextRecipe` 计数 0，但菜谱表照常 4 道菜）。
- `WorldState.cs`：料箱的导出里加 `prefab_components`（这个料的 prefab 上挂了哪些组件）。
  这是判"要不要切"的唯一数据来源（菜谱树里没有这道工序）。

**菜谱 → 工序的映射**（`scripts/export-kitchen.py`，与 15 同一个脚本，默认一起写两个文件）：

| 游戏节点类型 | 工序 | 资源 |
|---|---|---|
| `IngredientOrderNode` / `ItemOrderNode` / `WildcardOrderNode` | `fetch` | `dispenser` |
| `CookedCompositeOrderNode` | `cook` | `cooker` |
| `MixedCompositeOrderNode` | `mix` | `mixer` |
| 其它 Composite | `assemble` | `counter` |
| （每道菜末尾自动补） | `plate` → `deliver` | `plate_stack` → `serving` |

节点 id = `工序:游戏里的节点名`，所以两道菜共用同一节点时只有一个（实测 `fetch:Flour` 被 3 道菜共用）。
工时用方案 §6.5 的默认值，文件里 `durations.source` 写明"未实测"。

**Check 结果**（`s_city_h5`，冻结输入下跑两次）：

```
菜谱 4 道、工序节点 20 个、边 18 条
  SteamedSpecial_Fish    fetch:UncookedFish → cook:SteamedSpecial_Fish → plate:… → deliver:…
  SteamedSpecial_Prawns  fetch:Flour → fetch:Prawn → mix:MixedFlourPrawns → cook:… → plate:… → deliver:…
共用的中间产物：fetch:Flour
```

- 断言（临时脚本）：每道菜的 `fetch/mix/cook/assemble` 都排在 `plate` 之前、`deliver` 是最后一步、
  `plate → deliver` 这条边在、每条边的顺序与 `nodes` 一致 → 全部通过。
- 同一份输入跑两次，`kitchen.json` 与 `recipes.json` 都字节相同（`cmp` 通过）。
  注意：输入必须冻结——游戏在跑时插件每 0.5 秒重写 state，`generated.at`/`state_t` 会变。

**两处更正（原任务里的写法实测不成立）**：

1. **"`m_cookingStep`/`m_progress` → 切/煮/煎节点"不成立**：菜谱节点上没有这两个字段（`m_progress`
   在 `ClientWorkableItem` 上，是**当前加工进度**，不是菜谱要求的工序）。菜谱树只写"由什么组成"：
   `Cooked*` 要加热、`Mixed*` 要搅拌。所以 DAG 里没有"切"这道工序。
2. **不再按 `WorkableItem` 自动插 `chop` 节点**（先写出来过，又删了）：`WorkableItem` 只说明"这个料
   能被加工"（实测 `Fish`/`Prawn`/`Carrot`/`Meat` 有、`Flour` 没有），并不说明**这道菜**要不要切——
   `SteamedSpecial_Prawns` 的树是 `Cooked(Mixed(Flour, Prawn))`，虾是直接进搅拌台的。按 prefab 插
   `chop` 会给调度派多余的工序，比不给更糟。现在 `fetch` 节点带 `workable` 这个事实，要不要切留给
   游戏/执行层（任务 09 里"鱼要切"是执行层自己定的流程，不是菜谱要求）。

**没验证到的**：`plate`/`deliver` 的工时是方案默认值；`assemble` 这一类没在 `s_city_h5` 出现过；
"两道菜共用同一**加工**产物"（例如共用煮好的饭）这关没有例子——那是任务 13 自制关卡里要设计的；
本文件的 DAG 还没被 Scheduler 真跑过。

**Done when:** 上面每条打勾，Check 的断言脚本通过。

## Progress
