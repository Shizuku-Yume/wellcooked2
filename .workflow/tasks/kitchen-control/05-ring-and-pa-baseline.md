# 05: 环形不堵死、pa 对照基线与 nav_metrics

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** <commit-sha>
**Base branch:** small/os-core-acceptance-fixes

**Delivers:** `_3` 环形过道上 4 个厨师不同速度同时走全部到达；`navd --nav pa` 提供优先级时空
A* 对照基线；两种算法都输出 `nav_metrics.json`（总步数、等待步数、重规划、卡住、规划耗时
p50/p99），供 W15 的报告表格使用。

**Blocked by:** 04
**Files:** `bridge/src/{navd,pa}.c`、`bridge/tests/test_pibt.c`（加 `pa` 用例）、`tools/fake_plugin.py`
（场景 `nav-ring`、按厨师设速度）、`tools/run-bridge-scenarios.sh`

**Read first:** `.workflow/specs/kitchen-control.md` 的"PIBT 规划""对照基线"与 W2、W4、W15
**Covers:** W4

**Check:** `bash tools/run-bridge-scenarios.sh --only nav-ring` → PIBT 下 4 个厨师（速度 0.5×–1.5×）
全部到达对面工位、`nav_stuck` 0 次；同场景 `--nav pa` 也跑完并写出 `nav_metrics.json`，两份
文件字段一致；`cd bridge && make test` 里 `pa` 的无冲突用例全过。

- [ ] `pa` 只在 `navd` 内存里建预约表，不新增共享内存；放行同样走依赖图
- [ ] 算法由启动参数选，一局内不切换；`nav_metrics.json` 每局一份，写在该进程树的运行目录
