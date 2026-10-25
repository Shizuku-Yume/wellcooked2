# 01: 四张地图的配置导出与每格移动时间实测

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** 0eee7c05d00d2ac189c0960cf9124f129eb2986b
**Base branch:** plan/kitchen-control
**Started:** 2026-10-25T13:35:33+08:00
**Finished:** 2026-10-25T14:31:08+08:00

**Delivers:** `config/maps/s_wellcooked_demo_{1..4}/` 各有从实机导出的 `kitchen.json`、
`recipes.json`，Scheduler 与 Worker 用现有的 kitchen/recipes 路径开关能加载任意一张；
`_2` 上实测出的每格移动时间写进 spec 的 Open questions。

**Blocked by:** None
**Files:** `scripts/export-kitchen.py`、`config/maps/`、`bridge/src/kitchen.c`（只在导出字段
超出现有解析时改）、`.workflow/specs/kitchen-control.md`

**Read first:** `.workflow/specs/kitchen-control.md` 的"每张地图的配置"、Open questions 第一条、
Notes；`.workflow/done/game-bridge/15-kitchen-config-export.md`；`config/kitchen.example.json`

**Check:** `cd bridge && make test` 全绿，且对四个目录各跑一次 Scheduler 与 Worker 的加载
（`--kitchen`/`--recipes` 指向该目录）不报错 → 四张地图都可用；实测记录里有 10 格直线
往返的游戏时钟耗时。

- [x] 四张地图在实机上各导出一次，提交导出结果，不手抄 README §6
- [x] 导出的工位数与 `layout_*.tsv` 的差异记在 spec Notes（以导出为准）
- [x] `_2` 上一个厨师直线走 10 格，取 3 次均值，写出每格时间
- [x] 加载检查覆盖四个目录（单元检查或 `make test` 里的一个用例）

## Progress

Dirty at start: none（从 `origin/plan/kitchen-control` 的 0eee7c0 起，
`kitchen-control/01-map-configs`）。

**Check before:** 新增的 `bridge/tests/test_map_configs.c` 在 `make test` 里失败 12 项：四个
`config/maps/<scene>/` 目录还不存在，`kos_kitchen_load` / `kos_recipes_load` 跑不起来；
其余 10 个套件当时全绿。

**实机导出（2026-10-25）**

- 用本分支 `scripts/build-plugin.sh` 部署 `WellCooked.Smoke.dll`，`GAME_DIR` 为默认 Steam 目录。
  每张图都从 `scripts/dev-cycle.sh 1 <scene>` 冷启动进关一次；`work=off`，`menu=1` 导出菜谱表。
  原始 `wellcooked-{state,terrain,menu}.json` 备份在 `/tmp/kc-A-exports/<scene>/`（不进仓库）。
- `python3 scripts/export-kitchen.py --map-dir config/maps` 写出四目录（脚本新增 `--map-dir`，
  写 `<dir>/<scene>/{kitchen,recipes}.json`；默认输出位置不变，同一输入跑两次 `cmp` 相同）。

| 地图 | 原始工位 → 合并后 | 可走格（连通区） | 菜单 | 导出 kinds |
|---|---|---|---|---|
| `_1` | 26 → 20 | 288（1） | 2 道 | serving 1 / dispenser 6 / cooker 6 / sink 1 / container 6 |
| `_2` | 44 → 17 | 350（**2**） | 3 道 | serving 1 / dispenser 5 / cooker 4 / sink 1 / clean_plate_stack 1 / plate_stack 1 / container 4 |
| `_3` | 52 → 23 | 98（1） | 3 道 | serving 1 / dispenser 7 / cooker 6 / sink 1 / clean_plate_stack 1 / plate_stack 1 / container 6 |
| `_4` | 52 → 23 | 223（1） | 3 道 | serving 1 / dispenser 7 / cooker 6 / sink 1 / clean_plate_stack 1 / plate_stack 1 / container 6 |

与 `layout_*.tsv` 的差异（砧板未导出、PlateReturn 未导出、`_1` 缺 plate_stack、锅/炸台合并成
cooker、CleanPlateStack 与 ClientPlate 拆成两个同坐标工位、`_3/_4` 实机菜单只有 3 道）已写进
spec 的 Notes；`_1` 的 `plate_stack` 缺口让 Scheduler 启动时为 `plate:*` 节点报
`recipe_no_station`，这是 spec 已记的已知缺口（wash 任务补），不是 kitchen/recipes 解析失败。

**发现并修掉：`_2` 的地形选错连通块**

- 修前 `_2` 的 `wellcooked-terrain.json` 只有 281 格，全部厨师离最近格 6.0–7.8 m；日志里的
  连通块是 `1=281, 2=57, 3=175, 4=175, 5=57`。根因是 `KeepLargestComponent` 选了外围装饰
  地面（281 格），而厨房是两条各 175 格、互不相通的条带；`_2` 按设计就是两条线。
- 改成保留所有含厨师（没有厨师时含工位锚点）的连通块，其余口袋丢掉。重新部署后同一进关
  采样得到 350 格 / 2 个连通区，所有厨师离最近格 ≤0.31 m。决定记在 decisions.md:
  2026-10-25 - 可走格保留所有含厨师的连通块，不取最大块（Mine: yes）。
- 这个插件改动不在任务文件原 Files 列表里，但不变更就只能提交一张把厨师留在网格外的
  `_2` 地图，后续 stand/距离场（任务 03）必然失败；改动只动连通块筛选，不动采样与导航。

**每格移动时间（`_2`）**

方法：在插件里临时加 `walk=slot,dx,dz,秒` 调试指令，直接按单轴喂 `VirtualPad` 与
`LogicalInput`（不经过 Navigation），日志同时记 `Time.realtimeSinceStartup` 与厨师坐标；
测完已把该临时指令删除，提交的插件里没有它。1 号厨师沿 z=-7.2 一行走，取三段约 10 格：

| 段 | 距离 | 游戏时钟 | 每格 |
|---|---|---|---|
| 1 | 5.88 m（9.80 格） | 1.071 s | 0.1093 s |
| 2 | 6.00 m（10.00 格） | 1.080 s | 0.1080 s |
| 3 | 6.12 m（10.20 格） | 1.052 s | 0.1031 s |
| 均值 |  |  | **0.1068 s/格 ≈ 0.107 s** |

同一轮另有 9.4 / 9.4 / 10.2 格的三段，每格 0.1069 / 0.1156 / 0.1031 s，速度 5.19–5.82 m/s；
连续 10 格往返（20 格）两对实测 2.092 / 2.151 s。结论写进 spec 的 Open questions：0.15 m
转角容差与 2 s 卡住阈值不用按地图调。

**Check after**

- `cd bridge && make test`：11 个套件、0 失败；新用例 `test_map_configs` 60 项全过。
- 四个目录各跑一次 Scheduler 与 Worker（`--kitchen` / `--recipes` 指向该目录）：都打出
  `config_loaded`（格数/工位数/菜谱节点数见上表）。`_1` 另有前述 `recipe_no_station`。
- `python3 scripts/export-kitchen.py` 用同一份 `_2` 原始导出跑两次，`kitchen.json` 与
  `recipes.json` 都字节相同。
- `.workflow/bin/workflow validate`：3 个错误 + 1 个警告，全部来自 os-core/01、game-bridge/13、
  specs/os-core，与本任务无关（与 brief 里说的既有失败一致），没有新增。

**没验证到的**

- 四张图的导出里都没有 `chopping` 工位（插件 `AppendMap` 的组件表不含 `ClientWorkstation`），
  L2 的 `cut` 宏动作还需要后续任务补；本任务只做加载与导出。
- Scheduler / Worker 只验证了加载，没有在这些地图上实际派工跑单（任务 03 之后）。

**Review**

- Reviewer 子代理启动后报告无法访问工作目录，没有产出 findings；改为自查（diff、生成的 JSON、
  test 覆盖与 spec 数字逐项核对）。`Review boundary: 0eee7c0 + .workflow/{specs,decisions,
  standards,tasks}、plugin/{WorldState.cs,README.md}、scripts/export-kitchen.py、
  bridge/tests/test_map_configs.c、config/maps/；round: 1；briefs: self；boundaries: none
  （没有新的信任边界：`--map-dir` 只拼接本机游戏导出的场景名，config 是本地输入）；
  findings: 0 blocking / 0 nonblocking`。
- 自查确认：`--map-dir` 与 `--out`/`--out-recipes` 互斥，默认输出路径未变；test 用与
  Scheduler/Worker 相同的 `kos_*_load` 入口，四个目录都真读；`config/maps` 的 scene、格数、
  mask 长度、菜谱边与菜单数都逐文件核对过（见上表）。
