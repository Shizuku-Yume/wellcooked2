# 08: 多开游戏实例与批跑

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** dacd788388158ac06b3123ef742a1dfb18495b62
**Base branch:** kitchen-control/04-navd-pibt
**Started:** 2026-10-25T16:02:00+08:00
**Status:** active

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

- [ ] 实例目录在 `~/.cache/wellcooked/inst/<k>/`，`Overcooked2_Data` 用符号链接，游戏目录不改
- [ ] 插件读 `window=WxH` 调 `Screen.SetResolution`；盖不过注册表时改前缀 `Screenmanager` 键，
      结论写回 spec
- [ ] 不给 `--instance` 时 IPC 名字与现在相同，`make test` 和现有场景不退化
- [ ] 连跑结果定下默认 N，写回 spec Open questions

## Progress

Dirty at start: none

- 从 `origin/kitchen-control/04-navd-pibt` 开分支，Base commit `dacd788`。前置 04 已在该分支
  归档（`.workflow/done/kitchen-control/04-navd-pibt.md`，`Finished: 2026-10-25T15:58:00+08:00`），
  02、03、01 的归档也在同一分支上。
- Check before: 尚不能运行——`tools/instances.py`、`tools/batch.py` 还不存在。

