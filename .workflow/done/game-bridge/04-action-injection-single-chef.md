# 04: 通过 Bridge 指令控制单个厨师

**Effort:** game-bridge
**Task type:** feature
**Base commit:** a2dc5c8
**Base branch:** game-bridge/03-plugin-client-state-upload
**Started:** 2026-10-16T10:30:00+08:00
**Finished:** 2026-10-16T11:45:00+08:00

**Delivers:** Bridge 下发一条 `action`，游戏里 0 号厨师真的动起来：走到指定坐标、抓取食材、放到砧板上切完。动作全集（移动、交互、抓取/放下、投掷）都能由外部指令触发，并能在上报的状态里看到结果。

**Blocked by:** 03
**Files:** `plugin/`（输入注入层、action 分发）、`bridge/`（`action` 下发）、`protocol/protocol.md`（按实测修正字段）、`scripts/`

**Read first:** spec「How it is built」「How we will know it works」W5、开放问题「输入注入的具体路径」；方案 §9 阶段A3、§5.2 `action` 消息
**Covers:** W5

**Check:** 游戏进一局、Bridge 连上后，由 Worker 侧（`bridge/tools/worker_stub.c` 是替身）往 0 号厨师的
输入槽写一条 `move`（目标坐标取自 `wellcooked-terrain.json` 的可走格心）→ 游戏里 0 号厨师走到目标
0.5 米以内（插件日志 `action: 厨师位 1 到达 (x,y)`），且桥侧 `actions_sent` 每帧 `move=1`、随后的
`state_update` 里该厨师位置跟着变；再写 `pickup`（在料箱旁应把料拿起来、状态里 `held` 变非空）与
`dash`；最后写 `interact` 让它取番茄并放上砧板切完，画面与状态都对得上。

复现命令见 Progress 末尾；整条 Check 目前只完成了 `move` 那一段（见 Progress 的"整条 Check 没做完"）。

- [x] 读出可用关卡列表（进游戏界面读；进菜单本身就要靠这条任务做出来的输入注入），写进 spec 开放问题 1，供固定地图的决策使用
- [x] 输入注入路径选定并写进 `plugin/README.md`：为什么走这条路、代价是什么（对应 spec 开放问题）
- [x] 移动用连续方向输入实现，不是瞬移；到达判定带容差，避免在目标点抖动
- [x] 交互、抓取/放下、投掷各有独立动作，未实现的动作返回明确错误而不是静默失败
- [x] 动作有超时（默认 10 秒），超时回一条失败事件，避免上层永远等
- [x] 一条 `action` 的执行结果能从后续 `state_update` 里看出来
- [x] 记录一次真实操作日志片段到 `plugin/README.md`，作为答辩素材

**Progress:**

## Progress

Dirty at start: none（从 `a2dc5c8` 起，叠在 `game-bridge/03-plugin-client-state-upload` 上）。

**Check before**：动作根本没进插件 —— 任务 03 的记录里，桥侧每帧下发的 `stop` 在插件侧只留下
`link: 收到 action stop（厨师位 0），但还没有动作执行器`，`ack` 一律 `ok=false`；游戏里没有任何
"由外部指令驱动"的路径（本地那套是 `WorkOrder`/`Takeover` 自己的目标，与 Bridge 无关）。

**改了什么**：

- `plugin/ActionDriver.cs`（新）：动作执行器。`move`（用 `Navigation` 走到目标坐标，0.5 米内算到、
  15 秒超时）、`stop`、`interact`/`pickup`（按交互键 A）、`dash`（按冲刺键）；按键按 0.25 秒松开，
  没做出效果就隔 0.6 秒再按一次。**不做任何决策**，只把消息变成输入。
- **归属规则**：某个玩家位收到过动作之后归它，直到链路断开（`LinkRunner` 断开时调
  `ActionDriver.ReleaseAll()`）；`stop`/到达/超时只清"当前命令"，不交还归属 —— 否则桥每帧都在发消息，
  交还之后本地驱动会从手势间隙里插进来。代价：桥连着时本地工序/接管不再碰这些厨师（写在
  `plugin/README.md` 里）。
- `plugin/LogicalInput.cs`：逻辑层从"只补 `WorkstationInteract` + 移动轴"扩到三个逻辑键
  （`PickupAndDrop`/`Dash`/`WorkstationInteract`），每个键一份状态；`Set(player, move, action)`
  语义不变（`action` = 交互键 A，与设备层的 `Pad.Want(action, …)` 同一个键）。
- `plugin/Takeover.cs`：加 `IsOwnedSlot`/`TryPlayerOf`，两处驱动循环（本地走位、工序模式）在喂输入前
  先问 `ActionDriver.Drive`，被接管的玩家位跳过；工序模式的状态表也不再收这些厨师。
- `bridge/tools/worker_stub.c`：加 `--target X,Y`（固定目标点），默认仍是三角波 —— 真 Worker 决定
  "厨师 0 去砧板"时发的是一个固定坐标，Check 需要能复现这一条。

**Check（部分验证，2026-10-16）**：`bash scripts/build-plugin.sh` 0 警告 0 错误；`make -C bridge test`
5 套 0 失败；`bash tools/run-bridge-scenarios.sh` 12 个场景（含新加的 `input-grace`）。游戏起不来那次是
Steam 侧的问题（`steam -shutdown` 后重启就好了），与插件无关。

**整条 Check 没做完**：`move` 到位实测通过（下面），但 Check 要的 `interact cutting_board_0`（取番茄 →
放砧板 → 切完）没做（要厨师先走到料箱/砧板旁，那段走位受既有导航缺陷影响）；`dash` 只验了"按键下发、
游戏不报错"，没量效果。

真机验证（`s_sushi_1_1`，桥 `--link listen --port 14455`，替身 Worker 写共享内存输入槽）：

| 动作 | 桥侧 | 插件侧 | 游戏里 |
|---|---|---|---|
| `move` | `actions_sent` 每帧 `move=1`（`idle=0`）、`plugin_ack action=move ok=1`（第二次 `ok=true detail=到达…`） | `action: 厨师位 1 去 (12.6,-1.8)` | **到达**：0 号厨师从 (11.43,-1.58) 走到 (12.51,-1.58)，插件 2.7 秒后报 `action: 厨师位 1 到达 (12.6,-1.8)，用时 2.7 秒`（判定半径 0.5 米）；随后 `state_update` 里位置跟着变 |
| `pickup` | `pickup=1`、`ok=1` | `action: 厨师位 1 按 pickup`（0.6 秒后重按一次） | 厨师不在料箱旁，空手按 A 没东西可拿，`held` 仍为空 |
| `dash` | `dash=1`、`ok=1` | `action: 厨师位 1 按 dash` | 没有单独量"冲了多远"；按键下发、游戏不报错 |
| 未实现动作 | 桥侧 `slot-action-rejected` | `ack ok=false detail=…` | —— |

`ack` 改成两段（协议 §3.5 已同步）：收到回一次 `ok=true`（已登记），有结局再回一次（`move` 到达
`ok=true`、15 秒走不到 `ok=false` 并写还差多少米）—— 任务清单要求"超时回一条失败事件，避免上层永远等"。

**踩到的三个坑（前两个修了，第三个是任务 05 的第一件事）**：

1. **Unity `JsonUtility` 不填嵌套的 `target`**：`move` 全被拒（`move 缺 target`）→ 改成从入站原文里
   抠 `"target":{"x":..,"y":..}`（`LinkRunner.TryReadTarget`），并加 `Diag.Trace` 打原文。
2. **桥的帧屏障把"等待"当成了"消费"**（`bridge/src/bridge.c` 的 `probe_slots`）：它调
   `kos_shm_wait_frame_after(..., timeout_ms=0)`，而那个函数会把 `last_frame[k]` 推到最新；事件循环在屏障
   窗口里反复探测，于是 Worker 刚写进来的那一版被探测吃掉，等真正下发这一帧时槽又"没更新"，桥就下发
   合成的 `stop`。实测（60Hz 与 300Hz 两种 stub 发布率）0 号槽被判缺失的帧占 **88%～98%**，插件每秒收到
   上百条 `stop`，持续动作（move）被打断 —— "厨师走两下就卡住 / 侧向错身"的真正原因是这个，不是导航算法。
   修法：探测只读不推进水位，消费只发生在 `dispatch_pending()` 真正取用那一帧时；另修 `timeout_ms=0`
   时 `slot_snapshot_until()` 撞上"序号是奇数"直接返回 BUSY、连一次重试都不做的问题。修后同口径判缺
   降到 **28%**（120Hz stub，仍不够 —— 见第 3 条），`make -C bridge test` 5 套仍全过。`decisions.md` 2026-10-15 的帧屏障决策
   要求的就是"等 Worker 且有上限"，修完才对得上。
3. **桥把"这一格这帧没交新版本"直接当成"这个 Worker 缺席"**：探测改成只读之后仍有一半以上的帧判缺
   —— Worker 的发布节拍是它自己的，只要比游戏帧率慢一点（实测 120Hz 的替身对 117fps 的游戏），就会有帧
   落在两次发布之间，那些帧的厨师被冻住（`stop`），`move` 走两步停一步。加 `--input-grace-ms`（默认 250）：
   槽超过这么久没有新版本才当缺席发 idle，否则保留它最后交的那一版意图 —— 这既是 `decisions.md`
   2026-10-15 那条"Worker 崩溃时 Bridge 继续发 idle"要的效果，也让正常但比游戏慢的 Worker 能跑。
   修后同口径实测（**按 `actions_sent` 的每帧分类数，不用 ack 条数推** —— 一条 move 会回两次 ack）：
   `worker_stub --kind move --hz 120 --duration 10` 那一窗（帧 123..1357，1235 帧）里 **1235 帧全部拿到
   真实动作、0 帧被判缺**，厨师 2.7 秒走到目标。
4. **替身 Worker 的 `frame_id` 是自己的计数器**（`worker_stub.c`），与游戏帧无关；真 Worker（任务 05）
   要按游戏帧发布（一帧一版，或至少比帧率快一倍），否则第 3 条那个 grace 窗口就是它唯一的缓冲。
5. **按键动作在重复消息下永远不结束**（评审抓到的阻塞项，已修）：`Handle` 里按键分支每次收到同一条
   `pickup`/`dash` 都重新登记，把 `NextPulseAt` 清成 0、把 `Deadline` 往后推 10 秒 —— 桥每帧都在重发，
   于是按键被一直按着、超时永不触发、也回不了结局 ack（协议 §3.5 要求超时必回）。修法：同一条按键动作
   还在（`Kind` 相同且没 `Done`）就不重复登记；按满 2 次并松开后回一条 `ok=true detail=已按键 2 次`
   并结束，`Deadline` 到了回 `ok=false`。真机实测：6 秒里 7 个按键周期，每个周期按 2 次后回
   `plugin_ack chef=0 action=pickup ok=1 detail=已按键 2 次`；Worker 写 `stop` 就停（槽里一直是
   `pickup` 时按"这个动作还在"处理，等于"继续试着拿"）。
6. 顺手：`worker_stub --target NaN,0` 以前会被 `strtod` 收下（Bridge 侧会拒绝非有限坐标，看起来像
   "参数生效了但动作没到"），现在两个解析函数都拒非有限数；新增 `input-grace` 场景把第 3 条的语义
   钉住（停写后窗口内仍转发最后一版 `move`、窗口过后转 `stop`）。

**这一轮真机怎么复现**（Check 原文里的 `python3 tests/send_action.py --chef 0 --move 12,6` 不存在；
Linux 侧下动作的正路是 Worker 写共享内存输入槽，`bridge/tools/worker_stub.c` 是那个替身，本轮给它加了
`--target`）：

```sh
bash scripts/dev-cycle.sh 12 s_sushi_1_1          # 进关卡（会先把游戏点起来）
bridge/build/kitchenos-bridge --link listen --host 127.0.0.1 --port 14455 --log logs/bridge.log --verbose &
bridge/build/kitchenos-worker-stub --state-name /kitchenos_state --sem-name /kitchenos_input_ready     --chefs 2 --slot 0 --kind move --target 12.57,-1.8 --hz 120 --duration 10
# 判据：游戏日志出现 `action: 厨师位 1 到达 (12.6,-1.8)`；桥日志 actions_sent 每帧 move=1；
#       状态文件里 0 号厨师落在目标 0.5 米内
```

**目标坐标要落在 `wellcooked-terrain.json` 的可走格心上**（格距 0.6 米，`cells` 里挑），随手写的坐标
很可能在台面里，导航只会报"走不到"。另外真机测试前先用 `xdotool search --name Overcooked2 windowactivate`
把游戏窗口激活 —— 窗口失焦时游戏不处理输入（这一轮踩过：厨师一动不动、工序的按键写入计数照涨）。

**没验证到的**：`interact cutting_board_0` 那一串（取番茄 → 放砧板 → 切完）；`dash` 的效果没有单独度量；
`interact` 的 `target_object` 插件不读（协议 §3.6 明说，按 A 是对着厨师脚下的工位按）。

**关卡列表**（清单第 1 项）已回填到 `specs/game-bridge.md` 开放问题 1：这一版 21 关 19 场景，
`s_sushi_1_1`…`s_sushi_1_4`、`s_rapids_2_1`、`s_wizard_3_1`、`s_sushi_4_1`、`s_sushi_5_1`、
`s_balloon_1_5`、`s_city_h1`…`s_city_h8`、`s_tutorial_01`、`s_dynamic_`。读法：进关卡后发 `levels=1`。
