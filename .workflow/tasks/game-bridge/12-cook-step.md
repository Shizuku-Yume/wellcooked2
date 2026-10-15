# 12: 工序补齐"煮 / 煎"（读 server 侧进度，别靠超时硬等）

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>

**Delivers:** 工序状态机能做**需要加热的菜**：走到锅/煎锅前 → 下料 → **按 `HasFinished()` 等到熟**
→ 取出 → 继续装盘/交付；"切"那一步也从"按住 X 硬等超时"改成按 `HasFinished()` 收手。
验收：在带煮饭/煎锅的关卡上完成至少 1 单（`score` 从 0 变正、日志有 `完成一单`），且日志能打出熟度。

现在只做得了寿司 1-1 那种单料菜：`WorkOrder` 的步骤是 `取料 → 放砧板 → 切 → 拿盘子 → 装盘 → 拿走 → 交付`，
没有加热步；而 OC2 稍复杂的菜（`Sushi_Fish` = 海苔 + 煮饭 + 鱼、卷饼 = 饼 + 煮饭 + 煎肉）都要它。
判熟的数据源按 `gua248/Overcooked2-TAS` 的 `DebugItemOverlay`：`ServerCookingHandler.m_cookingProgress`
（`CookingStateMessage`）、`ServerMixingHandler.m_mixingProgress`、`ServerHeatedStation.m_heatValue`、
`ServerWashingStation.m_cleaningTimer`。

**Blocked by:** 11
**Files:** `plugin/WorkOrder.cs`（新增 `Cook`/`WaitCooked` 步）、`plugin/Kitchen.cs`（锅/煎锅查询与熟度读取）、`plugin/WorldState.cs`（server 侧 handler 查找）
**Read first:** 任务 09 Progress 的「切菜进度」行；`/tmp/gua/Overcooked2-TAS/plugin/DebugItemOverlay.cs` 的 Progress provider 段
**Covers:** W6
**Check:** 在 `s_sushi_1_2`（菜谱含煮饭）上 `work=auto` 完成至少 1 单，日志出现
`下料 → 熟度 x.xx → 取出` 且 `score` 从 0 变正；同一次运行里"切"那步不再等到超时才收手。

- [ ] 工位查询：锅/煎锅/热板（`ClientCookingStation`/`ServerCookingHandler` 一类）的定位与占用判断
- [ ] 下料与取出：各一次交互，带结果确认（手上内容物变化）
- [ ] 等熟：按熟度/`HasFinished()` 判定 + 超时；超时要报出当前熟度而不是只说"超时"
- [ ] 切那步改成按 `HasFinished()` 收手，去掉"按住 X 等超时"
- [ ] 步骤顺序按菜谱推导：需要加热的叶子节点自动插入 `Cook` 步
- [ ] 失败可复现：烧糊/取错锅这类情况有明确日志与重试

**Done when:** 上面每条打勾，Check 的证据齐全（完成一单 + 熟度日志 + 切步不再超时）。

## Progress
