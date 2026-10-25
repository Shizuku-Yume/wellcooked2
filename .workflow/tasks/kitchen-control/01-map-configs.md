# 01: 四张地图的配置导出与每格移动时间实测

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** <commit-sha>
**Base branch:** small/os-core-acceptance-fixes

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

- [ ] 四张地图在实机上各导出一次，提交导出结果，不手抄 README §6
- [ ] 导出的工位数与 `layout_*.tsv` 的差异记在 spec Notes（以导出为准）
- [ ] `_2` 上一个厨师直线走 10 格，取 3 次均值，写出每格时间
- [ ] 加载检查覆盖四个目录（单元检查或 `make test` 里的一个用例）
