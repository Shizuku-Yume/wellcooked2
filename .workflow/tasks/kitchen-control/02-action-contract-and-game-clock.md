# 02: 插件动作 path/face/work、新状态字段与游戏时钟

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** 0eee7c05d00d2ac189c0960cf9124f129eb2986b
**Base branch:** plan/kitchen-control
**Started:** 2026-10-25T14:35:00+08:00
**Status:** paused

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

- [x] `kos_action_kind` 加三种，`kos_action` 加 8 项路点数组与 `seq`；schema、`protocol.c`、
      `ActionDriver`、fake_plugin 同一次改
- [ ] `path` 沿路点连续走，转角偏离 ≤ 0.15 m，到最后一个路点停下回 ack；同 `seq` 只追加
- [ ] `face` 误差 < 15° 回 ack；`work` 按住加工键直到下一条动作
- [ ] `game_time_ms` 暂停时不走；shm 结构版本号加一，旧挂接方被拒
- [ ] 砧板 `progress`、`dirty_plates`、水槽 `progress`、`facing` 读不到时，把字段与原因写进
      spec Open questions 第二条并改写 L2 对应完成条件
- [x] 原 `move` 保留可用

## Progress

Dirty at start: none
Check before: `cd bridge && make test` 通过（10 个套件）；`bash tools/run-bridge-scenarios.sh` 通过（13 个场景）；
协议测试与 fake_plugin 尚不支持 path/face/work 与 game_time_ms/progress/facing。

Check after（代码层）:
- `cd bridge && make test` → **10 test suites, 0 failed**；`test_protocol` 321 checks、
  `test_shm` 114 checks。新增用例覆盖：path 1/8 路点往返、非法路点/负数或缺失 seq、
  face 的 target/target_object、work、缺 `game_time_ms` 返回 `PROTO_ERR_MISSING`、
  旧 shm 版本挂接被拒、path/face/work 输入槽发布与回读、facing/progress/dirty_plates/sink 解析。
- `bash tools/run-bridge-scenarios.sh` → **14/14 PASS**（新增 `path-face-work`：四个 Worker
  分别写 path(1)/path(8)/face/work，断言 seq=1000、路点数、target_object 原样到对端，
  `actions_sent` 的 path/face/work 计数非零，且无 `slot-action-rejected`）。
- `~/.dotnet/dotnet build plugin/WellCooked.Smoke.csproj -c Release` → 0 warning / 0 error。
- `python3 tools/fake_plugin.py --selftest` → PASS；`--scenario path-face-work` → PASS。

实现要点（已写入代码）:
- 修 fast worker 的实质缺陷：`kos_shm_slot_publish` 仍把种类上界卡在 `KOS_ACTION_DASH`，
  path/face/work 一律被拒；已改成 `KOS_ACTION_WORK` 并加测试。
- 桥的 `face` 之前把 target_object 场景下的 (0,0) 当坐标下发；现在 target_object 优先，
  坐标只在没有目标物时使用；插件把资源键按 `state_update` 的同一编号顺序解析成工位坐标。
- path 的结局 ack 带 `seq`（协议 `proto_ack` 可选字段、C 解析、插件两次 ack、schema/doc、fake 端）；
  shm 输入动作上界、路点有限性、负数 seq 都有校验。
- `face` 改为短促脉冲转身（每 0.35 s 喂 0.1 s 方向），避免像旧实现那样朝目标走过去；
  `< 15°` 才回成功 ack；目标就在脚下按 0° 处理。
- `work` 缺 target_object 直接拒绝；收到下一条动作时日志明确写"work 松开"。
- 砧板 progress 改用游戏自己的 `ClientWorkableItem.GetProgress()`；`dirty_plates` 用
  `ClientPlateStackBase.GetCount()`；水槽字段 `m_plateCount`/`m_isWashing`/`m_cleaningTimer`/
  `m_washingStation.m_cleanPlateTime` 已用 ilspy 对照 Assembly-CSharp 确认存在与类型。
- 新增 `wellcooked-command.txt` 指令：`action=<JSON>`、`path_act/face_act/work_act`、
  `state=1`（把完整 state_update JSON 写日志）、`action_test=1`（在已占玩家位上自测
  "同 seq 只追加"与"work 按到下一条动作"，写 `action_selftest PASS/FAIL`）。

未完成（阻塞在共享游戏锁，不是代码问题）:
- 实机验收没有跑：`/tmp/wellcooked-game.lock` 从 2026-10-25T13:37:55 起由 `A 01` 持有，
  且 A 的游戏进程一直在跑；按 common 规则不能抢锁。需要拿到锁后：
  1. 保存并备份 `$GAME_DIR/BepInEx/{wellcooked-takeover.txt,wellcooked-command.txt,wellcooked-orders.json}`。
  2. 锁内第一件事：`bash scripts/build-plugin.sh`（本分支代码重新部署）。
  3. 建议 `s_city_h5`（有 sink / 3 个 plate_stack / cooker / container）；takeover 写
     `work=off`、`frontend_press=true`、`diag=true`，用 `bash scripts/dev-cycle.sh 20 s_city_h5` 进关。
  4. 进关后向 `wellcooked-command.txt` 依次写：`state=1`、`action_test=1`、
     一条 4 路点带转角的 `action=<JSON path>`（走相邻可走格）、`face_act=<slot>:<x,y>`、
     `work_act=<slot>:cutting_board_0`，再写 `{"type":"action","chef_id":N,"action":"stop"}`。
     要检查 `action_selftest PASS`、path 的路点切换行之间无 >1 s 空档、`path 到达终点 ... seq=`
     的两次 ack 都带 seq、`face 对准 误差=<15°`、work 期间 `state=1` 的 `progress` 增长且
     stop 后停止；暂停时钟用 Escape 打开暂停菜单后连续两次 `state=1` 比较 `game_time_ms`。
  5. 结束无论成败恢复三个配置文件并 `rm -rf /tmp/wellcooked-game.lock`。
- 实机确认字段可读后，按任务第 5 项更新 spec Open questions 第二条与 L2 行（若读不到）。
- 还没跑 flow-verify 两个 brief、workflow validate、任务归档与最终 commit/PR（当前只提交暂停交接）。

锁定状态：工作树改动全部保留、未提交；分支 `kitchen-control/02-action-contract-and-game-clock`。
