# 04: navd 进程、导航表、PIBT 与依赖图放行

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** <commit-sha>
**Base branch:** small/os-core-acceptance-fixes

**Delivers:** Supervisor 多一个 `navd` 子进程，按每版世界状态用 PIBT 给所有厨师规划、按执行
依赖图放行路点；Worker 写目标槽、读计划槽、发 `path`；两个厨师在 1 格宽过道对向能错身；
卡住 3 次报 `BLOCKED`，`navd` 崩溃时厨师停下、重启后继续。

**Blocked by:** 02, 03
**Files:** `bridge/include/kitchenos/{nav,navtab}.h`、`bridge/src/{navd,navtab,pibt,adg,worker,
supervisor,scheduler}.c`、`bridge/tests/{test_pibt,test_adg}.c`、`bridge/Makefile`、
`tools/fake_plugin.py`（场景 `nav-corridor`、`nav-stuck`、`nav-crash`）、`tools/run-bridge-scenarios.sh`

**Read first:** `.workflow/specs/kitchen-control.md` 的"L1 走位"全段、W2、W3、W5 与"值得测的
边界"；glossary navd、PIBT、执行依赖图；decisions.md: 2026-10-25 - 走位改由中央 navd 进程用
PIBT 规划；Okumura 等 2022（PIBT，AIJ）与 Hönig 等 2019（ADG，RA-L）的算法描述；
`bridge/include/kitchenos/{nav,shm,mq}.h`
**Covers:** W2, W3, W5

**Check:** `cd bridge && make test` → `test_pibt` 在四张地图各 100 组随机起终点、2–4 个厨师下
无顶点/交换冲突、每步满足 `mask`、`_3` 上 200 步内全部到达、同输入两次结果相同；`test_adg`
随机打乱执行快慢时放行序列下没有两人同格；`bash tools/run-bridge-scenarios.sh --only
nav-corridor,nav-stuck,nav-crash` → 两人 10 s（游戏时钟）内到达且无同格帧；卡住链
`nav_replan`×3 → `BLOCKED` → 另一厨师收到该工序；`SIGKILL navd` 后 500 ms 内 `stop`，重启后到达。

- [ ] `/kitchenos_nav` + `/kitchenos_nav_lock` 由 Supervisor 创建与 unlink；目标槽只有 Worker 写、
      计划槽只有 `navd` 写
- [ ] PIBT 优先级 = 换目标后经过的步数 + 紧迫度，并列 `chef_id` 小者先；`pinned` 当障碍
- [ ] 计划窗口 H = 8；路点到达按实际坐标 ≤ 0.3 m 判定，不推算
- [ ] 游戏暂停不判卡住；Worker 重启在 RECOVERED 前清空自己的目标槽
- [ ] Worker 里旧的单厨师 A* 与 `move` 下发删掉，系统控制路径只发 `path`
