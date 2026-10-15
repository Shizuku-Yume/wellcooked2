# 14: 订单可复现：分阶段固定菜单 + 固定随机源

**Effort:** game-bridge
**Task type:** feature
**Base commit:** 262c1a0
**Started:** 2026-10-15T18:42:08+08:00

**Delivers:** 进关卡后订单按配置的菜谱序列生成，随机源也固定：同一关卡连跑两次，日志里的菜名序列与
`seed=` 完全相同。序列写在游戏目录的 `BepInEx/wellcooked-orders.json`
（`{"seed": 12345, "menu": [{"recipes": ["菜名", ...]}]}`，一个元素是一个阶段，静态关卡只用第一个），
改这个文件就能换一组实验订单；菜名写错时报错并列出该关卡可用的菜名。

**为什么**：方案 §4.3 的策略对比要求"所有策略使用相同地图、订单、时间限制"，而现在订单由游戏随机生成，
两次跑的结果不可比。参考项目里已有两处现成做法（都是 MIT，保留版权声明与来源注释，只借这两处）：

- `gua248/Overcooked2-TAS` `plugin/Patch.cs:189-241`：Harmony Prefix 接管 `RoundData.GetNextRecipe` 与
  `DynamicRoundData.GetNextRecipe`，用 `instance.CumulativeFrequencies.Collapse((f, total) => total + f)` 算出
  这是第几单，返回 `roundData.m_recipes.m_recipes[menuIndex]`（动态关卡走 `Phases[CurrentPhase].Recipes`），
  并更新 `instance.RecipeCount` 与 `instance.CumulativeFrequencies[menuIndex]`。
- `toasterparty/oc2-modding` `OC2Modding/FixedRNG.cs:59-140`：`GetNextRecipe` 前后 `Random.InitState(seed)` 与
  恢复，`InitialiseRound` 复位偏移。

它只决定"第几阶段出哪道菜"，不决定订单何时生成；到达时刻与时限见
`decisions.md: 2026-10-15 - 固定菜单复用 TAS 的 GetNextRecipe Prefix`，不在本任务里。

**Blocked by:** None
**Files:** `plugin/`（新增 `Orders.cs`；`SmokePlugin.cs` 注册补丁；`Commands.cs` 加 `orders=` 打印当前配置与已出单）、
`config/orders.example.json`、`.workflow/standards.md`

**Read first:** `.workflow/maps/prior-art.md` 的 §A 与"现在就能落地的事"第 1、2 条；
`decisions.md: 2026-10-15 - 固定菜单复用 TAS 的 GetNextRecipe Prefix`；任务 09 的 Progress 里订单字段那几行
（`ServerOrderControllerBase.m_activeOrders`、`RecipeList.Entry.m_scoreForMeal`）

**Check:** 连续两次进同一关（`printf 'load=<关卡>\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"`），
日志里两次的 `订单序列` 行与 `seed=` 行完全一致，且序列与 `wellcooked-orders.json` 写的菜名一致。

- [x] 补丁只接管订单生成，订单数量、时限与计分不变（只换返回哪条 `RecipeList.Entry`，不碰订单控制器；
      实测订单照常出现且 `remaining`/`lifetime` 正常；这几局没有交付，计分未复验）
- [x] 序列用尽后回到原逻辑并记一行日志，不能返回 null 把订单卡住（实测 `序列用完（2 单），之后回到游戏自己的随机订单`）
- [x] 菜名解析失败时报错并打印该关卡 `m_recipes` 里的可用菜名（实测列出 4 个 `SteamedSpecial_*`）
- [x] 固定 seed 与固定序列分开记日志（`本局随机源固定 seed=12345` 与 `阶段 0 第 N/M 单 = 菜名` 是两行）
- [x] `config/orders.example.json` 有说明字段（`level`，插件不读）；`standards.md` 的 `Running it` 里写上怎么改、怎么读日志

**Done when:** 上面每条打勾，Check 的两次运行序列一致。

## Progress

Dirty at start: none（从 main 的 262c1a0 起，`game-bridge/14-fixed-order-sequence`）。

**实现**：`plugin/Orders.cs`（新增）+ `SmokePlugin.Awake` 注册 + `Commands` 的 `orders=` / `orders=reload`
+ `config/orders.example.json` + `standards.md` 的"固定订单"一节。

- 接管点照 `gua248/Overcooked2-TAS` 的 `plugin/Patch.cs:189-241`：Prefix 打在 `RoundData.GetNextRecipe` 与
  `DynamicRoundData.GetNextRecipe` 上，用 `CumulativeFrequencies` 的和算"本阶段第几单"，返回
  `m_recipes.m_recipes[i]`，并同步 `RecipeCount` 与 `CumulativeFrequencies`；剧本关卡（`ScriptedRoundData`
  的 `m_manualOrder` 还没走完）不插手。
- `seed` 照 `toasterparty/oc2-modding` 的 `FixedRNG.cs:59-140`：`InitialiseRound` 时 `Random.InitState(seed)`，
  与"固定序列"分开记日志。
- 两个坑（写在这里省得下次再查）：
  - `RoundData.RoundInstanceData` / `DynamicRoundData.DynamicRoundInstanceData` 是 **protected** 嵌套类型，
    在 `Orders` 里直接 `as` 会 CS0122；照 TAS 的 `FixedMenuRoundData` 做法放进两个空壳派生类
    （`RoundDataAccess` / `DynamicRoundDataAccess`，不实例化）才拿到访问权。
  - `JsonUtility.FromJson<Config>` 在 Unity 2017.4 下把 `menu` 解成 null（日志 `订单: ... 里没有 menu`），
    所以配置自己解析（只认 `seed` 与 `menu[].recipes`，其它键跳过）。

**Check 结果**（`s_city_h5`，`bash scripts/build-plugin.sh` 0 错误）：

- 两次 `load=s_city_h5` 各跑一局，日志里的订单行与 `seed=` 行**完全一致**（`diff` 空）：
  `订单: 阶段 0 第 1/4 单 = SteamedSpecial_Fish（菜谱下标 0，累计生成 1 单，原 RecipeCount=0）`。
- 接管证据：`订单: 接管 GetNextRecipe（订单实例 RoundInstanceData）`（每局一行，说明补丁真的进了游戏的方法，
  而不是"日志里看不出差别"）。
- 菜名写错时（先用 `NOPE_NOT_A_DISH` 试）：`订单: 阶段 0 第 1 单的菜名 ... 不在本关菜谱里 ... 本关可用菜名：
  0:SteamedSpecial_Fish, 1:SteamedSpecial_Prawns, 2:SteamedSpecial_Meat, 3:SteamedSpecial_Carrot`。
- 序列用尽：`订单: 阶段 0 的序列用完（2 单），之后回到游戏自己的随机订单`。
- 本关每次交付/超时后游戏才要下一单，所以"有厨师"的一局只看到第 1 单；没有厨师的一局会连续出单，
  用那一局验证逐单推进（见下）。

**Check 的最终结果**（`s_city_h5`，两次独立进程各一局，`diff` 订单行与 `seed` 行为空 = 完全一致）：

```
订单: 已读 ...\wellcooked-orders.json，seed=12345，阶段 0 = SteamedSpecial_Fish, SteamedSpecial_Prawns, ...
订单: 本局随机源固定 seed=12345
订单: 接管 GetNextRecipe（订单实例 RoundInstanceData）
订单: 阶段 0 第 1/4 单 = SteamedSpecial_Fish（菜谱下标 0，累计生成 1 单，原 RecipeCount=0）
```

**没验证到的（写清楚，别当成已通过）**：

- **有效菜名在"第 2 单及以后"没在游戏里跑出来过**。两局里游戏各只要了一单（`s_city_h5` 在有人上场时是
  "上一单交付/超时后才要下一单"），索引推进路径只在菜名写错的那一局被走到过（那一局游戏连续要了两单，
  日志出现 `第 1 单的菜名 NOPE_NOT_A_DISH` 与 `第 2 单的菜名 NOPE2`，随后 `序列用完（2 单）`）。
  要让第 2..4 单真正发出来，得有交付（`work=auto`）——本任务试了一次 10 分钟的自动干活，没有交付成功
  （那是任务 09/10 的可靠性范围，不是这里的问题）。
- **剧本关卡/没有玩家的局不走这个接口**：有一局进关后场上已有 4 张订单、`GetNextRecipe` 一次都没被调用
  （`订单: 接管` 计数 0），说明那种情况下订单不是经 `RoundData.GetNextRecipe` 来的。固定菜单只对
  `RoundData`/`DynamicRoundData` 这条路生效；以后换关卡时先看日志有没有那行"接管"。
- **计分未复验**：这几局没有交付，`score` 一直是 0；补丁只替换返回的菜谱条目，不碰订单控制器与计分代码。
