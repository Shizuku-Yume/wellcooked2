# 08: 多开游戏实例与批跑

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** dacd788388158ac06b3123ef742a1dfb18495b62
**Base branch:** kitchen-control/04-navd-pibt
**Started:** 2026-10-25T16:02:00+08:00
**Status:** paused

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
- [ ] 连跑结果定下默认 N，写回 spec Open questions

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
  实测 IPC 名字为 `kitchenos_events_1/2`、`kitchenos_tasks_0_1/2`、`kitchenos_state_1/2`、
  `sem.kitchenos_input_ready_1/2`、`kitchenos_nav_1/2`、`sem.kitchenos_res_*_1/2`。
- 实机（2026-10-25 16:16–16:30，持 `/tmp/wellcooked-game.lock`）：`instances.py up 3`
  起了 3 个 640×360 实例（日志 `takeover: 窗口 = 640x360（全屏=False）`），各自端口
  14456/14457/14458；GPU 7.5/8.2 GB、可用内存约 2 GB，能同时跑但余量很小。
  `batch.py` 三行清单跑满整局（107 秒），`results.csv` 3 行都有 score 字段，但得分
  是 0/-150/-270/-390 一类的负分：两局诊断里厨师坐标全程不动，Scheduler 派出的
  `fetch` 任务 30 秒租约到期后重排，订单全部过期。插件日志有 `action=path` 与
  `ack ok`，但既没有 `path 到达` 也没有 `path 超时` 行；`work=off` 时 ActionDriver
  应被 `Takeover.Drive` 调用，实际却没推动厨师。这是暂停的根因，未解决。
- 未完成：查清 path/face 输入为何没有让厨师移动（怀疑 `Takeover.Drive` 的调用链或
  `VirtualPad/LogicalInput` 在实例里的输入层）；用修好后的动作链重跑 3 行清单、
  全尺寸 vs 小窗口得分对照、3 实例连跑 10 局，再定默认 N 并写回 spec。
- 环境已还原：3 个实例已全部关停，游戏目录的 `wellcooked-takeover.txt` /
  `wellcooked-orders.json` 已恢复为取锁前内容，`wellcooked-command.txt` 已删除，
  `/tmp/wellcooked-game.lock` 已移除。工作区改动已提交到本分支，未开 PR。

