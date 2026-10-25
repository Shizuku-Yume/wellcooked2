# 10: 分区流水线派工模式

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** <commit-sha>
**Base branch:** small/os-core-acceptance-fixes

**Delivers:** Scheduler 读 `config/maps/<scene>/zones.json` 后可切到分区模式：厨师只接本区工序，
跨区中间产物在交接台面上生成一对 `drop`/`fetch`；`_2` 上灵活与分区两种模式用多实例批跑的
得分可以并列比较。

**Blocked by:** 07, 08
**Files:** `config/maps/s_wellcooked_demo_2/zones.json`、`bridge/src/{assign,scheduler,recipes}.c`、
`bridge/tests/test_assign.c`、实验清单 `experiments/zoned.tsv`

**Read first:** `.workflow/specs/kitchen-control.md` 的"L3 调度"（代价里的 `w_z`、台面交接）、
W14、Open questions 第三条；glossary 分区流水线
**Covers:** W14

**Check:** `cd bridge && make test` → 分区用例里除交接工序外没有跨区派工；`tools/batch.py
experiments/zoned.tsv` 在 `_2` 上用同一订单序列各跑两种模式 → `decisions.jsonl` 里每个跨区
中间产物都有一对交接 `drop`/`fetch`，两个得分写进报告表格。需要游戏。

- [ ] 交接台面用户没定之前用 `zones.json` 里的默认台面，并在 spec Open questions 标明
- [ ] 灵活模式下不生成交接工序，原有检查不变
