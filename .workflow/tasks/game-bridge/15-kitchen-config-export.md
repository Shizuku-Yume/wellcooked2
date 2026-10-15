# 15: 数据面转 `config/kitchen.json`（工位 + 地形 + 连通区）

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>

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

**Check:** `bash scripts/dev-cycle.sh` 进关卡后跑 `python3 scripts/export-kitchen.py`：
`config/kitchen.json` 的工位数与 `wellcooked-state.json` 的 `stations` 数量一致，可走格数与 terrain JSON 一致，
且同一份输入跑两次输出字节相同（稳定 ID 与排序）。

- [ ] 工位去重 + 稳定 ID：同一场景两次导出 ID 不变，ID 里不含随机量
- [ ] 分类归一化：砧板 / 锅 / 煎锅 / 盘子堆 / 料箱 / 出菜口 / 盘子回收口 / 垃圾桶，去掉名称里的实例后缀
- [ ] 料箱写出它给的料；交付口与普通台面分开（Scheduler 的交付资源要单独建模）
- [ ] 地形：可走格集合、连通区、每格到最近工位的距离（调度算路径成本用）
- [ ] 格式写进 `config/kitchen.example.json`，`standards.md` 里给命令

**Done when:** 上面每条打勾，Check 的数字对得上。

## Progress
