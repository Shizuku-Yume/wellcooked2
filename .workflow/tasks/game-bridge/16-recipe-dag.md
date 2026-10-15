# 16: 菜谱转工序 DAG（`config/recipes.json`）

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>

**Delivers:** 转换器把插件导出的菜谱节点（`WorldState.cs:1078-1148` 展开的 `m_composition`）写成
`config/recipes.json`：每道菜一张工序 DAG——节点是取料 / 切 / 煮 / 煎 / 装盘 / 交付，边是前置依赖，
每个节点带资源类型（砧板 / 锅 / 煎锅 / 盘子 / 出菜口）与工时；至少覆盖 3 道菜，其中两道共享中间产物
（例如都用到煮好的饭）。工时优先写实测值，没实测的写 `null` 并在文件里写明来源。

**为什么**：方案 §4.2 把"调度重心下沉到工序级"定为核心问题——多厨师并行执行一张带前置约束的工序图；
没有这张图，Scheduler 的列表调度、关键路径优先都无从下手。现在 `WorldState.cs` 只把菜谱展成嵌套树
（深度上限 4、缺 `m_cookingStep`/`m_progress`、缺工时），还不是调度能用的依赖边。

**Blocked by:** 15
**Files:** `scripts/export-kitchen.py`（复用 15 建的转换器，所以排在它后面）、`config/recipes.json`、`config/recipes.example.json`、`.workflow/standards.md`

**Read first:** 方案 §4.2、§6.5 的工序时长示例（取料 1s / 切 3s / 煮 5s / 装盘 2s / 交付 1s，可调）；
`.workflow/maps/prior-art.md` 的"现在就能落地的事"第 4 条；`plugin/WorldState.cs:1078-1148`；
`plugin/WorkOrder.cs` 的步骤枚举（取料 → 放砧板 → 切 → 拿盘子 → 装盘 → 拿走 → 交付）

**Check:** `python3 scripts/export-kitchen.py` 之后，`config/recipes.json` 里每道菜至少有一个需要加热的节点
（若该菜谱确实需要），且用脚本断言：`切`/`煮` 节点都在 `装盘` 之前、`交付` 在所有节点之后；同一份输入两次输出一致。

- [ ] 叶子食材 → 取料节点；`m_cookingStep`/`m_progress` → 切/煮/煎节点（不再只认 `m_composition`）
- [ ] 前置边按菜谱树推导，共享中间产物只出现一个节点（两道菜指向它，而不是各写一份）
- [ ] 每个节点带资源类型与工时；工时来源（实测 / 方案默认值 / 未知）写在节点或文件头
- [ ] 覆盖 3 道菜以上，其中两道共享中间产物；把"哪些菜被覆盖"记在任务 Progress 里
- [ ] 格式写进 `config/recipes.example.json`，`standards.md` 里给命令

**Done when:** 上面每条打勾，Check 的断言脚本通过。

## Progress
