# 07: 派工代价、匈牙利配对、LLF/HRRN 与锅/盘子规则

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** d98cf7a3b5d2bdc8a307df5412f1f221befbcc53
**Base branch:** kitchen-control/03-stand-cells-and-distance-field
**Started:** 2026-10-25T15:20:00+08:00
**Status:** active

**Delivers:** Scheduler 有厨师空闲时按 spec 的代价公式给 (厨师, 就绪工序) 打分，多个空闲厨师
用匈牙利算法配对；订单策略多出 LLF 与 HRRN；锅前置 `w_c`×2，干净盘 ≤ 1 且有脏盘时生成
`wash` 工序；每次决策的各分量写进 `decisions.jsonl`。

**Blocked by:** 03
**Files:** `bridge/include/kitchenos/{assign,policy}.h`、`bridge/src/{assign,policy,scheduler}.c`、
`bridge/tests/test_assign.c`、`bridge/tests/test_policy.c`

**Read first:** `.workflow/specs/kitchen-control.md` 的"L3 调度"、W7、W8；
`.workflow/specs/os-core.md` 的 Scheduler 段；`bridge/include/kitchenos/{policy,recipes,resource}.h`
**Covers:** W7, W8

**Check:** `cd bridge && make test` → `test_assign`：2 个空闲厨师 × 3 道工序的配对与手算匈牙利
一致，`decisions.jsonl` 每条含 `w_d/w_t/w_c/w_u/w_z`；`test_policy`：LLF/HRRN 选择顺序与手算
一致，并列时退化为 FCFS；`kill -HUP` 能切到 `llf`/`hrrn`。

- [ ] 路程用任务 03 的距离场，不读导航表
- [ ] 权重从 `config/maps/<scene>/` 的调度配置读，缺省值写在 spec 里
- [ ] 已开工订单按开工顺序交付，策略只决定下一单
- [ ] `wash` 工序走现有就绪队列，不另起队列

## Progress

Dirty at start: none（从 `origin/kitchen-control/03-stand-cells-and-distance-field` 的
d98cf7a 起，`kitchen-control/07-assign-cost-and-policies`；工作区切换前是 detached HEAD）。

**Check before:** `cd bridge && make test` 现有 12 个套件全绿；`test_assign` 还不存在，
LLF/HRRN 不在 `policy.h`，Scheduler 仍是"每个空闲厨师挑策略选中的订单里关键路径最长的
一道"，`decisions.jsonl` 的 dispatch 行没有 `w_d/w_t/w_c/w_u/w_z`。按任务要求先写检查。
