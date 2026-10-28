# 02: 插件动作 path/face/work、新状态字段与游戏时钟

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** 0eee7c05d00d2ac189c0960cf9124f129eb2986b
**Base branch:** plan/kitchen-control
**Started:** 2026-10-25T14:35:00+08:00
**Finished:** 2026-10-25T15:12:00+08:00

**Delivers:** 线上动作多出 `path`/`face`/`work` 三种，插件按 spec 的动作契约执行并回 ack；
`state_update` 带 `game_time_ms`、砧板/水槽 `progress`、`dirty_plates`、厨师 `facing`，
Bridge 写进 shm（版本号加一），fake_plugin 两端都认。

**Blocked by:** None
**Files:** `protocol/protocol.md`、`protocol/schema/{action,state_update}.schema.json`、
`bridge/include/wellcooked/{protocol,shm}.h`、`bridge/src/{protocol,shm,bridge}.c`、
`plugin/{ActionDriver,WorldState,Kitchen}.cs`、`tools/fake_plugin.py`、`bridge/tests/`

**Read first:** `.workflow/specs/kitchen-control.md` 的"插件动作契约"与"时钟"；
decisions.md: 2026-10-15 - action 枚举扩到 5 个；2026-10-25 - Linux 侧计时统一用游戏时钟

**Check:** `cd bridge && make test` 与 `bash tools/run-bridge-scenarios.sh` 全绿，新增的
协议用例对 `path`（1 个与 8 个路点、同 `seq` 追加）/`face`/`work` 编解码往返一致、对缺
`game_time_ms` 的旧消息给出明确错误；实机上用指令文件对一个厨师发一条 4 路点带转角的
`path`、一条 `face`、一条 `work`，插件日志有对应 ack，`path` 中途没有停顿。

- [x] `kos_action_kind` 加三种，`kos_action` 加 8 项路点数组与 `seq`；schema、`protocol.c`、
      `ActionDriver`、fake_plugin 同一次改
- [x] `path` 沿路点连续走，转角偏离 ≤ 0.15 m，到最后一个路点停下回 ack；同 `seq` 只追加
- [x] `face` 误差 < 15° 回 ack；`work` 按住加工键直到下一条动作
- [x] `game_time_ms` 暂停时不走；shm 结构版本号加一，旧挂接方被拒
- [x] 砧板 `progress`、`dirty_plates`、水槽 `progress`、`facing` 读不到时，把字段与原因写进
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

## 实机验收（2026-10-25，锁定后）

锁：14:25:10 拿到 `/tmp/wellcooked-game.lock`，owner=`B 02`；锁内第一件事用本分支
`bash scripts/build-plugin.sh` 重新部署，随后进 `s_city_h5`（4 个厨师，我们的两个位是 One/Two）。
结束已恢复 `wellcooked-takeover.txt`（work=auto/frontend_press=true/diag=true）、
`wellcooked-orders.json`，删除临时 command 文件并释放锁。

- `action_test=1` → `action_selftest PASS 玩家位=1 同seq追加=ok work按住=ok 拒绝非法path=ok`。
- path：先用 A* 指令 `path=-7.08,2.44,-4.8,0.0` 取得 4 个路点，再发
  `{"type":"action","chef_id":0,"action":"path","seq":7501,"waypoints":[{"x":-7.2,"y":1.2},{"x":-6.6,"y":1.2},{"x":-6.6,"y":0.0},{"x":-4.8,"y":0.0}]}`。
  日志：路点切换 `1→2 t=75.24`、`2→3 t=78.70`、`3→4 t=79.33`，`path 到达终点 (-4.8,0.0)
  seq=7501，用时 4.7 秒`；两次 ack 都带 `seq=7501`（收到时 ok=true，到达时 ok=true）。
  没有收到 stop/零向量注入。另有一条自选路点在台面边 2→3 卡了 6.36 s（错误路点），
  不作为通过证据；验收用的是 A* 给出的可走路线。
- face：`face target={-6.6,0.0}` → `face 对准 误差=0.0°` + 结局 ack；另一条
  `face_act=0:cutting_board_0` 解析到 (4.8,-4.8)，`误差=0.4°`，证明 target_object 键也是实坐标。
- work：`work_act=0:cutting_board_0` → `work 按住 cutting_board_0（直到下一条动作）` +
  收到时 ack；随后 `stop` → `work 松开（收到 stop）`。本轮没有可切食材放上砧板，
  切菜 `progress` 的数值增长仍未实机验证（字段/方法已用 ilspy 对照 `Assembly-CSharp.dll`）。
- 暂停时钟：`pause_game=1`（走 `TimeManager.SetPaused(Main)`）后连续两次 `state=1` 的
  `game_time_ms` 都是 `47892`；`pause_game=0` 后恢复。证明 `game_time_ms` 暂停不走。
- 新状态字段实机值：`chefs[].facing`（1.571/2.946）、`dirty_plates{state:free,count:0}`、
  `sink_0{state:free,count:0,progress:0}` 都在 `state_update` 里；砧板 `progress` 因板上无料未出现。
  该关卡没有出现 `plate_stack` 键（`ClientCleanPlateStack` 未找到），与本任务无关。
- 实机输入轴：实机喂 +y 厨师向 -z 走；`ActionDriver.Feed` 已统一翻转 y，否则
  `path` 会朝路点反方向走（首轮实测在反方向 3.35 m 超时）。

## Review 与结论

- Brief A round 1（boundary 0eee7c05 + 26 文件）：6 findings。处理：
  - 2（face 双字段优先级）：已改为 target_object 优先，插件与桥一致。
  - 4（必填字段未升协议版本）：`PROTO_VERSION`、插件 hello/版本、fake、conformance、schema/doc
    统一升到 `0.2.0`。
  - 5（progress 0..1 未校验）：C 解析加范围检查 + 测试，schema 已有 min/max。
  - 3（实机证据缺失）：本轮补齐，见上。
  - 1/6（Linux 计时消费者未接 game_time_ms）：属任务 03/04/06/07 的计时路径；本任务只交付
    字段与契约，已在 decisions.md 记录范围决定，任务文件不再把消费者算进 02。
- Brief B round 1（同一 boundary）：7 findings。处理：
  - 实机证据：本任务开始时记录缺失，现已补 path/face/work/pause；砧板 progress 见下一条。
  - Scheduler 截止期仍用墙钟：与 Brief A 的 1/6 同一范围问题，按 decisions 的范围决定不计入 02。
  - 插件 path/face 超时用墙钟：已改为 `StateFrame.GameTime()`（与 game_time_ms 同源），
    暂停时 deadline 不走；pulse 间隔仍用实时。
  - work 收到不同 target_object 不替换：已改为同目标去重、不同目标记录“换目标”并替换。
  - `dirty_plates` 读不到时仍写 free/0：已改为读不到就省略字段并 WarnOnce。
  - `seq` 宽度：插件 InboundMessage/ack/Command 全部改为 `long`，`TryReadSeq` 直接解析整数，
    不再经 float；C 侧仍是 long long。
  - `protocol_build_action_path()` 负数 seq：已拒绝并加测试（326 checks）。
  - Brief B 的其余项已按上面修完；它的“实机证据缺失”在本轮已补齐。
- Brief A round 2（recheck）：原 2/4/5 fixed；1/6 out of boundary（决策记录）；
  实机证据已补；唯一保留是“砧板 `progress` 数值样本未取得”。任务 Check 的实机步骤只要求
  `path`/`face`/`work` 有对应 ack（已取得），checklist 第 5 条是“读不到时”才改 L2；
  `ClientWorkableItem.GetProgress()` 已对照 `Assembly-CSharp.dll` 确认，spec Open question
  如实写了“待放料后一次实机确认”。因此本任务不把它算作未交付，但保留在 spec 待测项。
- 安全 pass（CONVENTIONS §7，boundary 同 review）：0 blocking / 6 nonblocking。
  已修：target_object 无 NUL 的越界读风险（shm publish + bridge 消费端）、插件路径/目标
  非有限值、`Commands` JSON 分支异常、game_time_ms 负值、插件 seq 宽度；手写扫描已加 8 项上限。
  未修的两条 nonblocking（`target`/waypoints 必填坐标的解析细节、手写扫描的语法作用域收紧）
  不改变当前正常 Bridge 报文的结果，留给后续加固。
- `Review boundary: 0eee7c05d00d2ac189c0960cf9124f129eb2986b + 全部改动路径; round: 2;
  briefs: A,B,sec; boundaries: bridge TCP JSON ingest, plugin action ingest, shm 输入槽,
  shm 世界状态挂接, 本地调试指令文件; findings: 1 blocking（砧板 progress 实机数值样本，
  按任务 Check 保留为 spec 待测项）/ 12 nonblocking（均已处理或记录）`。
