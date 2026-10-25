# 11: 离线上界、权重网格搜索与走位算法对比

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** <commit-sha>
**Base branch:** small/os-core-acceptance-fixes

**Delivers:** `tools/opt.py` 不开游戏，对每张地图给出束搜索上界；多实例批跑做代价权重网格搜索，
定出默认值写回各地图调度配置；报告表格列出各订单策略占上界的百分比，以及 `_3` 上 PIBT 与 `pa`
的等待步数、卡住次数、规划耗时与得分。

**Blocked by:** 05, 08, 09
**Files:** `tools/opt.py`、`experiments/{sweep,policies,nav}.tsv`、`config/maps/*/`（调度配置）、
`tools/compare-runs.py`、`.workflow/specs/kitchen-control.md`（权重默认值、Open questions 最后一条）

**Read first:** `.workflow/specs/kitchen-control.md` 的"多实例批跑"末段、W12、W15、Open questions；
`tools/compare-runs.py`
**Covers:** W12, W15

**Check:** `python3 tools/opt.py --map <scene> --orders <序列>` 四张地图各一次 → 上界 ≥
`tools/batch.py experiments/policies.tsv` 里 FCFS/EDF/LLF/HRRN 的得分；`experiments/nav.tsv`
（`_3`、4 厨师、两种走位各 3 局）跑完，表格里 PIBT 卡住次数为 0；`compare-runs.py` 生成报告表格。
需要游戏（批跑部分）。

- [ ] 束宽默认 64，路程取距离场、不计避让等待
- [ ] 网格搜索结果与选定默认值写进 spec，权重写回四张地图配置
- [ ] 上界低于某局批跑得分时，在 spec 里写出原因并改 `opt.py` 的模型，不改断言
