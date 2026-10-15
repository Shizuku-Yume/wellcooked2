# 17: 修 `wellcooked-terrain.json` 的 cells 与最大连通块不一致

**Effort:** game-bridge
**Task type:** bugfix
**Base commit:** <commit-sha>

**Delivers:** 导出的 `wellcooked-terrain.json` 里 `cells` 只含保留下来的可走格——被
`KeepLargestComponent` 丢掉的格子不再出现在文件里，Scheduler/模拟器读到的地图与插件内部用的网格一致。

**根因**：`plugin/WorldState.cs:720-729` 先建边、再调 `KeepLargestComponent(out blocks)` 过滤，但紧接着的
`foreach (var p in free)` 遍历的是**过滤之前**建的 `free` 列表，所以 JSON 里仍写进了被丢掉的格；只有日志里的
`可走格 count-dropped` 减过了。导航本身用的是过滤后的网格，所以这只影响导出文件（也就是 OS 侧的输入面）。

**Blocked by:** None
**Files:** `plugin/WorldState.cs`

**Read first:** `plugin/WorldState.cs:700-745`（建边 → `KeepLargestComponent` → 写 JSON 的顺序与注释）

**Check:** 进关卡导出地形后跑一段脚本（可放在 `scripts/export-kitchen.py` 或临时脚本里）：从
`wellcooked-terrain.json` 的 `cells` 重建邻接并数连通区 → 结果是 1 个连通区；同时打印三个数——
过滤前格数、文件里的格数、日志里报的 `丢掉 N 格`，前两者之差应等于 N。

- [ ] `cells` 改成遍历过滤后的网格（或让 `KeepLargestComponent` 返回留下的格），日志里的数字与文件一致
- [ ] 不改变导航行为：`path=`/`probe=` 的结果与修之前相同
- [ ] 修完在任务 Progress 里记下三个数字（过滤前 / 文件内 / 丢掉）

**Done when:** 上面每条打勾，Check 的三个数对得上。

## Progress
