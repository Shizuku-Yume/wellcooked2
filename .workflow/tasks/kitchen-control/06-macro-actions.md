# 06: 宏动作执行器取代 worker_task

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** <commit-sha>
**Base branch:** small/os-core-acceptance-fixes

**Delivers:** Worker 把一道工序按映射表展开成宏动作（goto/pickup/drop/chop/start_cook/
wait_ready/wash/deliver），每步按 shm 世界状态判完成、超时报 `FAILED`，日志有
`macro_start`/`macro_done`/`macro_failed`。

**Blocked by:** 04
**Files:** `bridge/include/kitchenos/macro.h`、`bridge/src/{macro,worker}.c`、删除
`bridge/{include/kitchenos/worker_task.h,src/worker_task.c}`、`bridge/tests/test_macro.c`、
`tools/fake_plugin.py`（场景 `macro`）

**Read first:** `.workflow/specs/kitchen-control.md` 的"L2 宏动作"与 W6；glossary 宏动作；
`bridge/include/kitchenos/{mq,shm}.h`；`.workflow/tasks/os-core/03-worker-loop-and-path.md`
**Covers:** W6

**Check:** `cd bridge && make test` → `test_macro` 覆盖六种工序的展开；`bash
tools/run-bridge-scenarios.sh --only macro,os-loop` → fake_plugin 延迟 2 s 才改 `holding`/
`progress`/锅 `state` 时，宏动作等到变化才 `macro_done`；不变化时按表超时 `macro_failed`
且工序 `FAILED`，`detail` 写宏动作与所等条件；os-loop 原有检查不退化。

- [ ] `goto` 结束前发 `face`，ack 后才算到达
- [ ] `cook` 放下即走，不在锅前等熟
- [ ] 完成条件被打破（手上东西没了）立即失败，不等超时
- [ ] 内部 3000 mm/s 速度推算全部删掉
