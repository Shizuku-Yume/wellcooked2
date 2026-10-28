# 04: navd 进程、导航表、PIBT 与依赖图放行

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** 765682a62090406e942125cb73a6082f59f55e01
**Base branch:** kitchen-control/03-stand-cells-and-distance-field
**Started:** 2026-10-25T15:25:00+08:00
**Finished:** 2026-10-25T15:58:00+08:00

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

- [x] `/kitchenos_nav` + `/kitchenos_nav_lock` 由 Supervisor 创建与 unlink；目标槽只有 Worker 写、
      计划槽只有 `navd` 写
- [x] PIBT 优先级 = 换目标后经过的步数 + 紧迫度，并列 `chef_id` 小者先；`pinned` 当障碍
- [x] 计划窗口 H = 8；路点到达按实际坐标 ≤ 0.3 m 判定，不推算
- [x] 游戏暂停不判卡住；Worker 重启在 RECOVERED 前清空自己的目标槽
- [x] Worker 里旧的单厨师 A* 与 `move` 下发删掉，系统控制路径只发 `path`

## Progress

Dirty at start: none

- 从 `origin/kitchen-control/03-stand-cells-and-distance-field` 开分支；`git merge --no-ff
  origin/kitchen-control/02-action-contract-and-game-clock`（765682a）。唯一冲突在
  `.workflow/specs/kitchen-control.md` 的 Open questions：保留 03 的每格移动时间实测，
  同时保留 02 的砧板/脏盘/朝向实机结论。本分支的 Base commit 即该合并提交。
- 前置 02 的 done 归档
  `.workflow/done/kitchen-control/02-action-contract-and-game-clock.md` 已在
  `origin/kitchen-control/02-action-contract-and-game-clock` 上确认存在，任务为完成态。

Check before: `test_pibt`、`test_adg` 与 `nav-corridor/nav-stuck/nav-crash` 场景尚不存在，
无法运行；前置 03 的 `make test` 10 个套件通过。本任务新增的 Check 是 feature 型，改动前
必然不可用。

Check after（全部实跑）:
- `cd bridge && make test` → **13 test suites, 0 failed**。新增 `test_pibt` 14 项、`test_adg`
  18 项；`test_supervisor` 77 项（多出 navd 子进程后仍全过）。
- `cd bridge && make test-san` → **13 suites, 0 failed**（ASan/UBSan）。
- `bash tools/run-bridge-scenarios.sh --only nav-corridor,nav-stuck,nav-crash` →
  **3/3 PASS**，逐项证据：
  - `nav-corridor`：1 格宽环走廊，厨师 0/1 到达目标游戏时钟 **1217 ms / 2250 ms**（<10 s），
    `no_same_cell_frames` 为 none；path 动作 141/212 条、`move` 0 条。
  - `nav-stuck`：冻住厨师 0 后 navd 日志 `nav_replan=3`、`nav_stuck=1`；先把游戏时钟冻结
    2.5 s，暂停前后 `nav_replan` 都是 0（暂停不判卡住）；恢复后第 3 次重规划触发
    Worker 在 8606 ms 发 `task_blocked`（detail 带工位），Scheduler
    `task_requeued reason=worker-blocked` 后把同一节点 `fetch:Right` 改派给厨师 1
    （`task_dispatched chef_id=1`）。
  - `nav-crash`：游戏时钟 600 ms `SIGKILL navd`，Worker 在 **533 ms** 后发 `stop`（计划槽
    写时钟落后 500 ms 的阈值 + 60 Hz 采样误差），调度器日志 `navd spawns=2`，厨师于
    2733 ms 到达右侧站位格。
- `bash tools/run-bridge-scenarios.sh --only os-loop` → PASS（path 而非 move 驱动，worker 重启
  的 RECOVERED/requeue/new-task 链仍成立）。

实现要点:
- 新增 `navtab.{h,c}`：独立 `/kitchenos_nav` 段，每厨师目标槽（Worker 写）/计划槽（navd 写），
  seqlock + `/kitchenos_nav_lock`；计划序号在 navd 重启后从旧槽继续加一。
- 新增 `pibt.{h,c}`、`adg.{h,c}`：PIBT 一步规划 + 执行依赖图放行；PIBT 把 pinned 与"有目标
  且已到站"的厨师当固定障碍，距离场按障碍格重算；没任务的目标=当前格仍可被推。ADG 同时
  检查"计划里更早的占用者已执行到那一步"和"实际已离开该格"。
- `navd.c`：每版世界状态推进；H=8 计划窗口、实际坐标 ≤0.3 m 对进度；游戏时钟 2 s 没动判
  卡住，3 次写 blocked；崩溃重启从世界状态+目标槽重建。
- Worker 删除单厨师 A* 与 `move` 下发：写目标槽、按计划槽发 `path`，到站置 pinned，navd
  写时钟落后 500 ms 先发 stop；重启在 RECOVERED 前清空目标槽。
- Supervisor 创建/unlink 导航表并多拉起 `navd`；`--state-name` 非默认时派生
  `<state-name>_nav`/`_nav_lock`。Scheduler 收到 BLOCKED 时回队列、记录 `avoid_chef`、
  暂时扣住原厨师，改派成功后解除。
- 更新 `test_worker_nav.c` 为 `test_pibt`/`test_adg`（旧文件随 `worker_task` 删除）；os-loop
  场景改用同一套按 path 移动的模拟器，断言改为 path/no-move/无同格。

边界说明:
- `nav-corridor` 用 1 格宽**环**走廊（每对相邻格都在环上，符合 PIBT 可达性前提）；简单
  死胡同一格宽走廊不可能错身，由 spec 的"死胡同靠卡住重规划"兜底，不在本场景断言。
- `nav-crash` 的 stop 实测 533 ms：spec 阈值是"写入时钟落后 500 ms"，60 Hz 状态采样使观测
  值多 1–2 帧；Supervisor 以 `--backoff-ms 2000` 留给停止可见的窗口，重启后仍到达。

Decisions（均 `Mine: yes`）:
- decisions.md: 2026-10-25 - 已到站的目标厨师在 PIBT 里当固定障碍
- decisions.md: 2026-10-25 - BLOCKED 后先扣住原厨师，再把工序改派
