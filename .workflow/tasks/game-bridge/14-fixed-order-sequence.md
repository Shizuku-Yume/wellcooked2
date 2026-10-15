# 14: 订单可复现：分阶段固定菜单 + 固定随机源

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>

**Delivers:** 进关卡后订单按配置的菜谱序列生成，随机源也固定：同一关卡连跑两次，日志里的菜名序列与
`seed=` 完全相同。序列写在游戏目录的 `BepInEx/wellcooked-orders.json`（`{"seed": 12345, "menu": [["菜名", ...]]}`），
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

- [ ] 补丁只接管订单生成，订单数量、时限与计分不变（分数仍与画面一致）
- [ ] 序列用尽后回到原逻辑并记一行日志，不能返回 null 把订单卡住
- [ ] 菜名解析失败时报错并打印该关卡 `m_recipes` 里的可用菜名
- [ ] 固定 seed 与固定序列分开记日志（两者不是一回事，别混成一条）
- [ ] `config/orders.example.json` 有注释字段说明；`standards.md` 的 `Running it` 里写上怎么改、怎么读日志

**Done when:** 上面每条打勾，Check 的两次运行序列一致。

## Progress
