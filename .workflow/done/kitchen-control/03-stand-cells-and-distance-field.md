# 03: 站位格表与距离场

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** 3818609ae731e0f9f5634a84df6ab35e406e0955
**Base branch:** kitchen-control/01-map-configs
**Started:** 2026-10-25T15:10:00+08:00
**Finished:** 2026-10-25T15:25:00+08:00

**Delivers:** 加载 `kitchen.json` 时为每个工位算出带朝向的站位格（≤ 4 个、按距离排序）和
到全图的 BFS 距离场；没有站位格的工位让加载失败并报出工位 id。

**Blocked by:** 01
**Files:** `bridge/include/wellcooked/stand.h`、`bridge/src/stand.c`、`bridge/src/kitchen.c`、
`bridge/tests/test_stand.c`、`bridge/Makefile`

**Read first:** `.workflow/specs/kitchen-control.md` 的"L1 走位 / 站位格表与距离场"与 W1；glossary
站位格、距离场；`bridge/include/wellcooked/{kitchen,nav}.h`
**Covers:** W1

**Check:** `cd bridge && make test` → `test_stand` 对 `config/maps/` 四张地图全过：每个工位
≥ 1 个站位格（≤4、按距离排序、朝向是工位实际所在的一侧）、站位格在网格里；朝工位方向的
相邻格可走时 `mask` 位为 1、不可走时为 0；距离场在站位格上为 0、每个非 0 格有 `dist-1`
的父格、相邻可达格差 ≤ 1、跨连通区为 `UINT16_MAX`；把某工位四周的可走格去掉后加载失败且
错误里有该工位 id。字面的"朝向边未被 `mask` 挡"在实机导出上不可满足（工位那一格不可走，
位必为 0），改成上面这条双向检查，理由见 Progress 与 decisions。

- [x] 站位格带朝向（+x/-x/+z/-z），两个工位共用一格时两边都列出
- [x] 距离场 `uint16`，不可达记最大值；内存按 64 工位 × 2048 格上限静态分配
- [x] `nav` 的"工位相邻可走格"改用站位格表，旧逻辑删掉；距离场接口给 `navd` 与 Scheduler 共用

## Progress

Dirty at start: none（从 `origin/kitchen-control/01-map-configs` 的 3818609 起，
`kitchen-control/03-stand-cells-and-distance-field`）。

**Check before:** `test_stand` 还不存在，`cd bridge && make test` 只有现有 11 个套件全绿，
测不到站位格与距离场。按任务要求先写检查再实现。

**任务 01 的 `_3`/`_4` 地图导出不完整（先修再用）**

- 现象：按任务文件字面算站位格（离工位一个 0.6 m step、朝向边未被 mask 挡），四张地图
  所有工位都是 0 个。查数据发现工位坐标在台面/墙中心，最近的 `grid.cells` 在 1.2 m 外，
  工位那一格根本不在网格里——不是站位算法的问题。
- 根因（实机日志）：插件 `DumpTerrainOnce` 在场景刚加载、状态还是 `工位=0` 时就采样并
  缓存；采样范围锚点只找 `ClientIngredientContainer`，出菜口/水槽不参与。`_3` 因此只有
  98 格（x -1.2..10.2），`_4` 只有 223 格（x -5.4..7.8），水槽和出菜口落在网格外。
- 修法（`plugin/WorldState.cs`）：工位类型表提成 `StationTypes`，导出与采样锚点共用；
  工位数为 0 时不采样；工位数或厨师数比上次采样多就重采。试过"保留含工位锚点的连通块"，
  会把 `_3` 的 380 格外围装饰地面和 45 格台面顶当可走格（厨师能走到台面上），已放弃，
  仍只保留含厨师的块；真正要修的是采样时机与范围。
- 重导出 `_3`（167 格、1 连通区）、`_4`（381 格、1 连通区），工位列表不变；`_1`/`_2`
  的原导出已验证每个工位 1.2 m 内有正对格，不动。决定记在 decisions.md:
  2026-10-25 - 地形采样等工位生成后再做，锚点用全部工位类型。
- 实机证据：`_3` 日志 `可走格 167/936`，`_4` `可走格 381/1260`；两张图 23 个工位最近的
  正对可走格都是 1.2 m（两格）。

**实现**

- `kitchen.h`：加 `kos_stand_cell` / `kos_station_stands` / `kos_stand_map` 与
  `KOS_STAND_UNREACHABLE`；`kos_kitchen` 内嵌 stand（64×4 站位格 + 64×2048 uint16 距离场，
  约 256 KB，静态定长）；新增 `kos_kitchen_station_index`。
- `stand.h` / `stand.c`：四方向各取最近可走格（沿轴 > 半格、垂直偏移 ≤ 半格），按距离
  排序、带朝向位；工位没有站位格返回 `KOS_ERR_MISSING` 且 `context` 带工位 id。按 mask
  建邻接表、标连通区，再逐工位多源 BFS；提供 `kos_stand_cells` / `kos_stand_distance` /
  `kos_stand_best_cell`（同连通区、按曼哈顿步数挑最近站位格）。
- `kitchen.c`：解析完 stations 后调用 `kos_stand_build`，失败即整次加载失败。
- `worker_task.c`：目标格从 `kos_kitchen_nearest_cell(工位坐标)` 改为站位格表
  （`kos_stand_best_cell`），`kos_worker_task_init` 去掉 target_x/y；`worker.c` 调用点同步。
  `kos_kitchen_nearest_cell` 只保留给厨师出生点取最近格。
- spec 的 L1 站位格段与 W1 按实测改写；glossary、plugin README、game-bridge spec 的
  连通块说法同步。

**Check after**

- `cd bridge && make test`：12 个套件全过；`test_stand` 1277 项 0 失败（四张地图 + 合成
  厨房：两工位共用一格且朝向相反、无站位格报错带 id、一维距离场 1/0/1）。
- `cd bridge && make test-san`：12 个套件全过，无 ASan/UBSan 报告。
- 反证：把 `_1` 的 `serving-0` 移到 (100,100) 后 `test_stand` 失败（`kos_kitchen_load`
  报没有站位格）；恢复后通过——检查能失败，不是只会绿。
- `.workflow/bin/workflow validate`：仍是 3 error + 1 warning（os-core/01、game-bridge/13、
  specs/os-core），与任务无关，没有新增。

**没验证到的**

- 站位格在游戏 1.2 m 交互格上是否真能按到工位，要等任务 09 实机两厨师；本任务只保证
  几何、朝向与加载契约。
- `_2` 的第二条条带没有在本次会话重导；沿用任务 01 的 350 格导出（两个条带各 175 格，
  所有工位 1.2 m 内有正对格，`test_stand` 覆盖）。

**Review**

- `Review boundary: 3818609 + bridge/{include/wellcooked/{kitchen,stand,worker_task}.h,src/{kitchen,stand,worker_task,worker}.c,tests/{test_stand,test_worker_nav}.c}、config/maps/s_wellcooked_demo_{3,4}、plugin/{WorldState.cs,README.md}、.workflow/{specs,glossary,decisions,tasks}；round: 1；briefs: reviewer；boundaries: 不可信 kitchen.json 解析、插件从游戏场景采样地形；findings: 0 blocking / 2 nonblocking`。
- 非阻塞 1：距离场接口还没有 navd/Scheduler 消费者。不是本任务范围（任务 04/07 接），spec 已改成"本任务只提供接口，任务 04/07 接入"。
- 非阻塞 2：`stand.h` 写了"站位格必须在厨师连通区"，实现没有厨师输入。已改成"两侧都可能列出，调用方用距离场判断可达"，与实现一致。
