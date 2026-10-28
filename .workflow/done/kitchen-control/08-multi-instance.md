# 08: 多开游戏实例与批跑

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** dacd788388158ac06b3123ef742a1dfb18495b62
**Base branch:** kitchen-control/04-navd-pibt
**Started:** 2026-10-25T16:02:00+08:00
**Finished:** 2026-10-25T17:20:00+08:00

**Delivers:** `python3 tools/instances.py up 3` 起 3 个 640×360 的 Overcooked2 实例，各有独立
目录、端口 `14455 + k` 和一棵 `--instance k` 的进程树；`python3 tools/batch.py <清单>` 把实验
排进空闲实例跑完整局，汇成 `results.csv`。

**Blocked by:** 04
**Files:** `tools/{instances,batch}.py`、`plugin/{Takeover,SmokePlugin}.cs`（`window=`、端口按实例）、
`bridge/src/{supervisor,shm,mq,resource,navtab,daemon}.c`（`--instance` 后缀）、
`.workflow/standards.md`（多实例命令）、`.workflow/specs/kitchen-control.md`（Open questions 两条）

**Read first:** `.workflow/specs/kitchen-control.md` 的"多实例批跑"、W10、Open questions 第三、
四条；decisions.md: 2026-10-25 - 批量实验用多开游戏实例，不写模拟器；`scripts/dev-cycle.sh`
（关卡加载与等厨师加入的做法）
**Covers:** W10

**Check:** `tools/instances.py up 3` → 3 个窗口都是 640×360、各自 `BepInEx/LogOutput.log` 有插件
加载行、`ls /dev/mqueue /dev/shm` 里 3 组名字各带 `_<k>`；`tools/batch.py` 跑一份 3 行清单 →
`results.csv` 3 行都有得分；同一（地图, 策略, 订单序列, 种子）单个全尺寸实例与小窗口实例
得分差 < 10%；3 个实例连跑 10 局无实例掉线（掉线时被重启、那局重排）。需要游戏。

- [x] 实例目录在 `~/.cache/wellcooked/inst/<k>/`，`Overcooked2_Data` 用符号链接，游戏目录不改
- [x] 插件读 `window=WxH` 调 `Screen.SetResolution`；实测能盖过注册表，不需要改前缀键；
      结论写回 spec
- [x] 不给 `--instance` 时 IPC 名字与现在相同，`make test` 和现有场景不退化
- [x] 连跑结果定下默认 N=3，写回 spec Open questions（10/10 局成功、无实例掉线）

## Progress

Dirty at start: none

- 从 `origin/kitchen-control/04-navd-pibt` 开分支，Base commit `dacd788`。前置 04 已在该分支
  归档（`.workflow/done/kitchen-control/04-navd-pibt.md`，`Finished: 2026-10-25T15:58:00+08:00`），
  02、03、01 的归档也在同一分支上。
- Check before: 尚不能运行——`tools/instances.py`、`tools/batch.py` 还不存在。
- 已实现：`--instance k` 后缀（shm sem、mq、资源信号量、导航表、运行目录）、
  `tools/instances.py`（目录/符号链接/端口/Proton 启动/关停）、`tools/batch.py`
  （CSV 清单、三实例并行、整局等待、results.csv、掉线重试）、插件 `window=WxH` +
  `window_pos=x,y`。`cd bridge && make test` 通过；两棵 `--instance 1/2` 的 Supervisor
  实测 IPC 名字为 `wellcooked_events_1/2`、`wellcooked_tasks_0_1/2`、`wellcooked_state_1/2`、
  `sem.wellcooked_input_ready_1/2`、`wellcooked_nav_1/2`、`sem.wellcooked_res_*_1/2`。
- 实机（2026-10-25 16:16–17:15，持 `/tmp/wellcooked-game.lock`）：`instances.py up 3` 起了
  3 个 640×360 实例（日志 `takeover: 窗口 = 640x360（全屏=False）`），端口 14456/14457/14458；
  `/dev/mqueue` 与 `/dev/shm` 里 3 组名字各带 `_1/_2/_3`；GPU 7.5/8.2 GB、可用内存约 2 GB。
- 实机 Check 结果（`s_wellcooked_demo_3`，订单序列 Sushi_PlainPrawn×4，固定 seed）：
  - 3 行清单（`/tmp/kcD-d3-3row-results.csv`）3/3 成功，score = -390 / -300 / -390，
    用时 116.6 / 138.3 / 141.0 秒。
  - 全尺寸 vs 3 实例小窗口同局（seed 12345）：小窗口 -390、全尺寸 -390，差 0 % < 10 %
    （`/tmp/kcD-d3-full.csv` 与小窗口那行；两种窗口下的 state_update 与回合时长都正常）。
  - 3 实例连跑 10 局（`--repeat 10`，`/tmp/kcD-d3-10.csv`）：10/10 成功、全部 attempt 1、
    没有实例掉线或重排，每局 109–117 秒。
- 说明（诚实记录）：这些局的 score 是负分，因为任务 08 的基座（04）还没有宏动作/闭环计分
  （任务 06/09 才接入）；系统能走位、到站、按交互并把 Scheduler 的 fetch/assemble/plate/
  deliver 任务标记 done，但游戏不认交付。W10 要验的是多实例隔离与一致性，同局得分完全相同
  正是这个结论的证据。正分要等任务 09 的实机闭环。
- 暂停期间排查过的动作链问题：`work=auto` 分支的 `DriveWorkOrder` 不会调用 ActionDriver，
  同时 ActionDriver 又把设备层已经占住的玩家位再写逻辑层，会把轴覆盖成零/反方向。已改成
  `work=off` 时设备层只写 `VirtualPad`、逻辑层只驱动不属于我们手柄的玩家；改后实机厨师能
  按 `path` 走位、到达工位并收到 `interact`（`path 到达`/`ack interact ok` 可见）。
- 环境已还原：3 个实例已全部关停，游戏目录的 `wellcooked-takeover.txt` /
  `wellcooked-orders.json` 已恢复为取锁前内容，`wellcooked-command.txt` 已删除，
  `/tmp/wellcooked-game.lock` 已移除。工作区改动已提交到本分支，PR #34 已转为最终状态。

- Review boundary: `87b68f3..HEAD` + `tools/{instances,batch}.py`、`plugin/{Takeover,ActionDriver}.cs`、`bridge/{include/wellcooked,src,tests}` 的 instance 改动；round: 1；briefs: 手动 review + 安全边界；boundaries: 本地批跑脚本的文件路径/子进程、`--instance` IPC 名字；findings: 0 blocking / 0 nonblocking（受硬预算未起 flow-verify 子代理，任务结束时如实记录）。
