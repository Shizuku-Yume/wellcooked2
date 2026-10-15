# 17: 修 `wellcooked-terrain.json` 的 cells 与最大连通块不一致

**Effort:** game-bridge
**Task type:** bugfix
**Base commit:** e51d23b
**Started:** 2026-10-15T20:10:00+08:00

**Delivers:** 导出的 `wellcooked-terrain.json` 里 `cells` 只含保留下来的可走格——被
`KeepLargestComponent` 丢掉的格子不再出现在文件里，Scheduler/模拟器读到的地图与插件内部用的网格一致。

**根因**：`plugin/WorldState.cs:720-729` 先建边、再调 `KeepLargestComponent(out blocks)` 过滤，但紧接着的
`foreach (var p in free)` 遍历的是**过滤之前**建的 `free` 列表，所以 JSON 里仍写进了被丢掉的格；只有日志里的
`可走格 count-dropped` 减过了。导航本身用的是过滤后的网格，所以这只影响导出文件（也就是 OS 侧的输入面）。

**Blocked by:** None
**Files:** `plugin/WorldState.cs`

**Read first:** `plugin/WorldState.cs:700-745`（建边 → `KeepLargestComponent` → 写 JSON 的顺序与注释）

**Check:** 进关卡导出地形后，`wellcooked-terrain.json` 里 `cells` 的个数**等于**日志里报的"可走格"数
（`state: 连通块 N 个（丢掉 M 格），可走格 X/Y`），而不是 `X+M`。修之前实测：文件 730 格、日志 546/988、
丢掉 184 格（730 = 546 + 184）；修之后应读到 546，且日志里的 连通块/可走格/可走边 三个数不变。

- [x] `cells` 改成遍历过滤后的网格，日志里的数字与文件一致（546 == 546）
- [x] 不改变导航行为：日志里的 连通块/可走格/可走边（8 / 546 / 1288）与修前完全一致
- [x] Progress 里记下三个数字（过滤前 730 / 文件内 546 / 丢掉 184）

**为什么不用"数连通区"当判据**：插件的连通块是按**边可通行**（`CanStep`）算的，而 JSON 里只有格点、
没有边；被丢掉的格常常与主块 4 邻接（只是中间那条边过不去），所以从 cells 重建 4 邻域会得到 1 块，
看不出这个 bug。可观测的判据是格数对不上。

**Done when:** 上面每条打勾，Check 的三个数对得上。

## Progress

Dirty at start: none（从 main 的 e51d23b 起，`game-bridge/17-terrain-cells-consistency`）。

**根因**：`WorldState.cs` 里 `free` 列表在建边**之前**就填好了，`KeepLargestComponent` 之后写 JSON 的循环
遍历的还是它，于是被丢掉的格子照样进文件；只有日志里的 `可走格 count - dropped` 减过了。

**修法**：写 JSON 时直接遍历过滤后的 `_walkable`（顺序与原来一致），`free` 列表随之删掉（没有别的用处）。

**证据（s_city_h5，同一次进关导出）**：

| | 修之前 | 修之后 |
|---|---|---|
| 文件里的 `cells` | 730 | **546** |
| 日志"可走格" | 546/988 | 546/988 |
| 日志"丢掉" | 184 | 184 |
| 日志"连通块" | 8 | 8 |
| 日志"可走边" | 1288 | 1288 |

修前 730 = 546 + 184（正好把丢掉的格子写进去了）；修后 `cells` 与日志的可走格数相等。
后三行与修前完全一致，说明导航用的网格没动——这次只改了"往文件里写什么"。

**判据的更正**：原 Check 写"从 cells 重建 4 邻域数连通区"，实测不可行——插件的连通块按**边可通行**
（`CanStep`）算，被丢掉的格常常与主块 4 邻接（只是中间那条边过不去），重建出来是 1 块、看不出 bug。
改成"文件 cells 数 == 日志可走格数"。

**没验证到的**：`path=`/`probe=` 没有重跑对比（它们用的是同一张过滤后的网格，而这次没动网格代码；
上面三个数与修前一致是间接证据）。
