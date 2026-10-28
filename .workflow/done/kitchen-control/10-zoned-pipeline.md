# 10: 分区流水线派工模式

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** bfde9b310cd7aeb5cae686d74154a73d341b1bdc
**Base branch:** kitchen-control/07-assign-cost-and-policies
**Finished:** 2026-10-25T18:35:00+08:00

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

- [x] 交接台面用户没定之前用 `zones.json` 里的默认台面，并在 spec Open questions 标明
- [x] 灵活模式下不生成交接工序，原有检查不变

## Progress

Dirty at start: none

- 验证前置依赖：07 归档 `.workflow/done/kitchen-control/07-assign-cost-and-policies.md` 已在 `origin/kitchen-control/07-assign-cost-and-policies` 上存在；08 归档 `.workflow/done/kitchen-control/08-multi-instance.md` 已在 `origin/kitchen-control/08-multi-instance` 上存在。
- 本分支基于 07 开分支；合并 08：`git merge --no-ff origin/kitchen-control/08-multi-instance`（合并提交 `47ba0f2`）。解决 `child.h`、`child.c`、`supervisor.c`、`scheduler.c` 以及 `scheduler_fixture.h` 的冲突，保留 07 的代价/匈牙利候选模型并融合 08 的多实例/navd/blocked_hold 支持。
- Check before: 分区配置文件 `zones.json`、分区数据结构与 `--dispatch zoned` 派工模式尚未实现，`test_zoned_pipeline` 用例尚不存在。

Check after:
- `cd bridge && make test` → **14 test suites, 0 failed**。其中 `test_assign` 83 项全过，新增 `test_zoned_pipeline`（验证 `zones.json` 解析、交接台面动态注册与双向站位、跨区分区菜谱 DAG 展开、真实 Scheduler 在 zoned 模式下的决策无跨区派工且交接工序正确指派至出入区厨师、灵活模式下不生成交接工序的原有行为不变）。
- `cd bridge && make test-san` → **14 test suites, 0 failed**（ASan 与 UBSan 无任何报错或内存泄露）。
- 实机多实例批跑（2026-10-25 持 `/tmp/wellcooked-game.lock` 跑游戏，`scripts/build-plugin.sh` 部署插件）：
  - 实验清单 `experiments/zoned.tsv`（`s_wellcooked_demo_2`，同一订单序列 `experiments/orders_demo2.json`，固定种子 12345，各跑 flexible 与 zoned）：
  - `python3 tools/batch.py experiments/zoned.tsv --instances 2 --out experiments/results.csv`：
    - `flexible`: score=-390, status=ok, 107.6 秒（run_dir: `/home/shizuku/.cache/wellcooked/inst/1/runs/20261025-182751`）
    - `zoned`: score=-390, status=ok, 109.1 秒（run_dir: `/home/shizuku/.cache/wellcooked/inst/2/runs/20261025-182803`）
  - 实机日志验证：
    - `zoned` 模式 `decisions.jsonl`：每个跨区中间产物都在 `counter_handoff_0` 上生成了 `drop` 决策（如 `drop:BurritoChicken` 由厨师 0 在站位格执行），并在后续交接给厨师 1 执行对应操作。
    - `flexible` 模式 `decisions.jsonl`：全场 0 条交接工序，原有派工不受影响。
  - 实测得分均为 -390，符合当前 04/08 基座上游戏不认无宏动作交付的基线（同局扣分一致，等待 09 闭环）。
  - 实机测试后按 common.md 彻底关停所有实例，清理并还原游戏目录下的所有配置文件，安全释放 `/tmp/wellcooked-game.lock`。

实现要点:
1. `config/maps/s_wellcooked_demo_2/zones.json`：定义两区划分（厨师 0 下半区，厨师 1 上半区）与跨区交接台面 `counter_handoff_0`（坐标 `[4.2, -3.6]`，两侧站位格 `[4.2, -4.8]` 与 `[4.2, -2.4]`）。
2. `bridge/include/wellcooked/zones.h`、`bridge/src/zones.c`：实现 `kos_zones_load`、`kos_zones_for_kitchen`、`kos_zones_station_zone`、`kos_zones_chef_zone` 与 `kos_zones_register_handoff`。
3. `bridge/include/wellcooked/recipes.h`、`bridge/src/recipes.c`：`kos_recipe_node` 增加 `zone` 字段；实现 `kos_recipes_apply_zones`，将跨区边 $u \to v$ 扩展为 $u \to \text{drop} \to \text{fetch} \to v$。
4. `bridge/include/wellcooked/child.h`、`bridge/src/child.c`、`bridge/src/supervisor.c`、`bridge/src/scheduler.c`：全链条打通 `--dispatch flexible|zoned`。Scheduler 在 zoned 模式下注册交接台面、扩展菜谱 DAG、按分区约束过滤候选与计算 `w_z` 惩罚分量，`decisions.jsonl` 完整记录。
5. `tools/instances.py` 与 `tools/batch.py`：支持根据场景自动优先使用 `config/maps/<scene>/` 的地图与菜谱，透传 `--dispatch` 参数。

Decisions（`Mine: yes`）:
- decisions.md: 2026-10-25 - 分区模式下跨区中间产物在菜谱 DAG 中展开为 (drop, fetch) 节点
