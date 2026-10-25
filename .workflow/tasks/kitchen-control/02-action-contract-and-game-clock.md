# 02: 插件动作 path/face/work、新状态字段与游戏时钟

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** <commit-sha>
**Base branch:** small/os-core-acceptance-fixes

**Delivers:** 线上动作多出 `path`/`face`/`work` 三种，插件按 spec 的动作契约执行并回 ack；
`state_update` 带 `game_time_ms`、砧板/水槽 `progress`、`dirty_plates`、厨师 `facing`，
Bridge 写进 shm（版本号加一），fake_plugin 两端都认。

**Blocked by:** None
**Files:** `protocol/protocol.md`、`protocol/schema/{action,state_update}.schema.json`、
`bridge/include/kitchenos/{protocol,shm}.h`、`bridge/src/{protocol,shm,bridge}.c`、
`plugin/{ActionDriver,WorldState,Kitchen}.cs`、`tools/fake_plugin.py`、`bridge/tests/`

**Read first:** `.workflow/specs/kitchen-control.md` 的"插件动作契约"与"时钟"；
decisions.md: 2026-10-15 - action 枚举扩到 5 个；2026-10-25 - Linux 侧计时统一用游戏时钟

**Check:** `cd bridge && make test` 与 `bash tools/run-bridge-scenarios.sh` 全绿，新增的
协议用例对 `path`（1 个与 8 个路点、同 `seq` 追加）/`face`/`work` 编解码往返一致、对缺
`game_time_ms` 的旧消息给出明确错误；实机上用指令文件对一个厨师发一条 4 路点带转角的
`path`、一条 `face`、一条 `work`，插件日志有对应 ack，`path` 中途没有停顿。

- [ ] `kos_action_kind` 加三种，`kos_action` 加 8 项路点数组与 `seq`；schema、`protocol.c`、
      `ActionDriver`、fake_plugin 同一次改
- [ ] `path` 沿路点连续走，转角偏离 ≤ 0.15 m，到最后一个路点停下回 ack；同 `seq` 只追加
- [ ] `face` 误差 < 15° 回 ack；`work` 按住加工键直到下一条动作
- [ ] `game_time_ms` 暂停时不走；shm 结构版本号加一，旧挂接方被拒
- [ ] 砧板 `progress`、`dirty_plates`、水槽 `progress`、`facing` 读不到时，把字段与原因写进
      spec Open questions 第二条并改写 L2 对应完成条件
- [ ] 原 `move` 保留可用
