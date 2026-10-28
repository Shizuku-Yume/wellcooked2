# 05: 环形不堵死、pa 对照基线与 nav_metrics

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** dacd788388158ac06b3123ef742a1dfb18495b62
**Base branch:** kitchen-control/04-navd-pibt
**Started:** 2026-10-25T16:09:39+08:00
**Finished:** 2026-10-25T16:44:56+08:00

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

- [x] `pa` 只在 `navd` 内存里建预约表，不新增共享内存；放行同样走依赖图
- [x] 算法由启动参数选，一局内不切换；`nav_metrics.json` 每局一份，写在该进程树的运行目录

## Progress

- Dirty at start: none.
- 分支基底：`origin/kitchen-control/04-navd-pibt` = dacd788（04 的 done 归档存在，前置完成）。
- Check before: 未实现 `--nav`、`pa` 与 `nav-ring`，`bash tools/run-bridge-scenarios.sh --only nav-ring`
  会报未知场景；`make test` 里没有 `pa` 用例。二者都不能通过。
- 任务文件原 Base branch 写的是 `small/os-core-acceptance-fixes`（模板残留），按通道规则改为
  `kitchen-control/04-navd-pibt`。
- 实现：新增 `pa.h/pa.c`（时间展开图上的优先级时空 A*，预约表只在 navd 内存，终点要求
  后续时刻空闲）；`navd` 按 `--nav pibt|pa` 选算法，按已执行计划步累计 steps/waits、
  `nav_stuck` latch 次数与每次规划推进耗时，原子写运行目录的 `nav_metrics.json`。
  `Supervisor`/`child` 透传 `--nav`；`fake_plugin.py` 加 `nav-ring`（真实 `_3` 地图、
  四个 world order 一次派满 4 个厨师、每厨师 0.5×–1.5× 速度、从导航表目标槽读实际工位），
  `run-bridge-scenarios.sh` 加 `--nav pibt|pa`。
- Check after（2026-10-25）：`bash tools/run-bridge-scenarios.sh --only nav-ring` PASS：4 个
  厨师在 1283/1467/2867/4233 ms（游戏时钟）到达各自工位，`nav_stuck=0`，没有同格帧，
  `nav_metrics.json` 写出；`--nav pa` 同场景 PASS（多次运行均到达），两份指标字段逐层相同
  （top/chef/totals/planning_us）。`cd bridge && make test`：13 个套件全过；`test_pibt` 的
  `pa` 在四张图各 100 组随机用例无冲突、同输入一致，`_3` 上 94/100 组 200 步内到达
  （W2 只要求 pa 无冲突，到达是 PIBT 的保证）。最终复跑：`--only nav-ring` 与
  `--only nav-ring --nav pa` 都是 PASS，4 人到达 1333/1533/2867/4233 ms，PIBT `nav_stuck=0`，
  pa 同场景 `nav_stuck=0`；`make test` 与 `make test-san` 都是 13 套件 0 失败。
- 全套场景（2026-10-25）：18 个场景 16 过、2 失败。`os-loop` 单独复跑 PASS，全套那次失败是
  另一个通道的 supervisor 把全局 mq 名 `/wellcooked_tasks_*` unlink 掉造成的（串行跑无问题）。
  `os-resources` 在 `origin/kitchen-control/04-navd-pibt` 的原始代码上（`git archive` 到
  `/tmp/kc04-base` 重建后）同样失败同样的 5 条断言（`resource_released_events`、
  `contention_observed`、`other_chef_acquires_after_reclaim`、`metrics_resource_wait`、
  `no_semaphore_left_after_stop`），失败原因是该场景的 fake 状态不按 `path` 移动厨师、
  也不清理别的运行的 `/dev/shm` 信号量；不是本任务引入。
- Review boundary: dacd788388158ac06b3123ef742a1dfb18495b62 + task 05 的 11 个路径；
  round: 1; briefs: A,B; boundaries: supervisor/child 参数转发、导航表读取、nav_metrics 文件；
  findings: 0 blocking / 2 nonblocking。两条 nonblocking（导航表段头/槽尺寸校验、非对象 JSON
  的指标检查）都在本轮修掉；修后复跑 nav-ring 两种算法与 `make test` 全绿。安全 pass 未触发：
  改动不涉及认证、凭据、shell/query/URL/进程注入或权限状态；shm 与运行目录都属于本项目
  自己的进程树。
