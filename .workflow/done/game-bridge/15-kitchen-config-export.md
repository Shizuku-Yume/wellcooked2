# 15: 数据面转 `config/kitchen.json`（工位 + 地形 + 连通区）

**Effort:** game-bridge
**Task type:** feature
**Base commit:** 06b39a8
**Started:** 2026-10-15T20:30:00+08:00

**Delivers:** 一个转换器把插件导出的 `BepInEx/wellcooked-state.json`（`stations[]` 已有
`type/name/pos/height/gives`）与 `BepInEx/wellcooked-terrain.json`（0.6 米物理采样 + 最大连通块）合成
`config/kitchen.json`：场景名、工位列表（稳定 ID、归一化分类、坐标、料箱给什么料）、可走格与连通区、
导出元数据（时间、来源文件）。Scheduler 与模拟器读这一个文件就能建资源模型，不用再解析插件的原始 JSON。

**为什么**：方案 §6.6 把 `config/kitchen.json` 定为 OS 侧的输入；`.workflow/maps/prior-art.md` 复检过
所有候选数据源（supercharged 只有 4 关手工标注、LevelEditor 的 35 个场景是 OC1/测试场景、Archipelago 只有
关卡 ID 表），结论是**用我们自己的导出转换，不再找外部数据集**。

**Blocked by:** None
**Files:** `scripts/export-kitchen.py`（新）、`config/kitchen.example.json`、`.workflow/standards.md`

**Read first:** 方案 §6.6；`.workflow/maps/prior-art.md` 的"现在就能落地的事"第 3 条与"关卡与菜谱数据"小节；
`plugin/WorldState.cs` 的 `stations` 与地形导出段（`283-334`、`594-738`）

**Check:** 进关卡导出后跑 `python3 scripts/export-kitchen.py`：
`config/kitchen.json` 里每个原始工位都能对上（按物体名 + 坐标覆盖，脚本打印"原始 27 → 合并后 20"，
合并数 = 物体名的去重数），可走格数与 terrain JSON 一致，连通区为 1（导出只保留最大块），
同一份输入跑两次输出字节相同。

- [x] 工位去重 + 稳定 ID：同一场景两次导出 ID 不变，ID 里不含随机量
- [x] 分类归一化：出菜口 / 料箱 / 垃圾桶 / 锅 / 搅拌台 / 洗盘台 / 盘子堆 / 容器能力（名称里的实例后缀不进 `id`）
- [x] 料箱写出它给的料（`gives`）；出菜口单独一类（`ClientPlateStation` 就是交付口：游戏的
      `OnSuccessfulDelivery(..., ClientPlateStation)` 与 `Kitchen.DeliveryStation()` 都这么认）
- [x] 地形：可走格 + 每格掩码（可通行邻居）+ 连通区（按掩码算，实测 1 个）
- [x] 格式写进 `config/kitchen.example.json`（`s_city_h5` 实测导出），`standards.md` 里给命令

## Progress

Dirty at start: none（从 main 的 06b39a8 起，`game-bridge/15-kitchen-config-export`）。

**插件侧补了两样导出**（转换器需要，都是小改动）：

- `WorldState.cs` 的工位类型表加 `ClientRubbishBin`——原来没导出垃圾桶，而挑工位时必须排除它
  （实测把鱼扔进去过）。加了之后 `s_city_h5` 的原始工位从 26 变 27（`bin` 1 个）。
- 地形 JSON 每格多一个 `mask`（可通行邻居的位掩码，`edge_bits` 字段说明含义，要求邻居也可走）。
  没有它，OS 侧只有格点、没有边，重建不出真实连通性——实测按 4 邻域重建得 1 块，而插件按边算是 8 块
  （8 是**过滤前**的块数；导出只保留最大块，所以导出文件里应当是 1 块）。

**Check 结果**（`s_city_h5`，一次进关导出 + 转换）：

```
场景 s_city_h5：原始工位 27 → 合并后 20，可走格 546，连通区 1（最大 546）
  serving 2  dispenser 5  bin 1  cooker 2  mixer 2  sink 1  plate_stack 3  container 4
```

- 覆盖：27 个原始工位（按物体名 + 坐标）全部能对上合并后的工位；合并数 20 = 物体名去重数。
- 可走格 546 == terrain JSON 的 cells 数 == 插件日志里的"可走格 546/988"。
- 连通区 1（按 `mask` 算），与"导出只保留最大块"一致。
- 掩码对称性：546 格全查过，没有指向不存在格子的位、也没有单边（临时脚本 `/tmp/mask-check.py`）。
- 同一份输入跑两次，输出字节相同（`cmp` 通过）；`at` 字段取的是 state 文件的 mtime，不是当前时间。
- 掩码位数统计：导出网格里 999 条可通行边（过滤前日志是 1288 条，差的 289 条随 184 个格子一起丢掉）。

**没验证到的**：`ClientRubbishBin` 只在 `s_city_h5` 上见过一次（别的关卡可能没有垃圾桶）；
盘子回收口（dirty/clean plate stack）在这关没有实例，分类表里有但没跑过；
`config/kitchen.json` 还没被 Scheduler 真读过（OS 侧还没开始）。

**判据的两处更正**（原 Check 想当然，实测不成立）：

- "工位数与 state 的 `stations` 数量一致"——同一物体上挂多个组件（`equipment_plate_01` 同时是
  `ClientIngredientContainer` 与 `ClientPlate`），去重后数量必然少于原始条目（27 → 20）。
  可验证的是"每个原始工位都被覆盖" + "合并数 = 物体名去重数"。
- "每格到最近工位的距离"没有逐格写进文件：调度要的是网格路径成本，而路径要在 `mask` 上算；
  逐格写欧氏距离既是近似又是冗余，消费者拿 `stations[].pos` 自己算就行（记在 `grid.note` 里）。

**Done when:** 上面每条打勾，Check 的数字对得上。

## Progress
