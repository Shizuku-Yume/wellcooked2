# 04: 通过 Bridge 指令控制单个厨师

**Effort:** game-bridge
**Task type:** feature
**Base commit:** a2dc5c8
**Base branch:** game-bridge/03-plugin-client-state-upload
**Started:** 2026-10-16T10:30:00+08:00
**Status:** paused

**Delivers:** Bridge 下发一条 `action`，游戏里 0 号厨师真的动起来：走到指定坐标、抓取食材、放到砧板上切完。动作全集（移动、交互、抓取/放下、投掷）都能由外部指令触发，并能在上报的状态里看到结果。

**Blocked by:** 03
**Files:** `plugin/`（输入注入层、action 分发）、`bridge/`（`action` 下发）、`protocol/protocol.md`（按实测修正字段）、`scripts/`

**Read first:** spec「How it is built」「How we will know it works」W5、开放问题「输入注入的具体路径」；方案 §9 阶段A3、§5.2 `action` 消息
**Covers:** W5

**Check:** 游戏进一局、Bridge 连上后，用 `python3 tests/send_action.py --chef 0 --move 12,6` → 游戏里 0 号厨师走到 (12,6) 一格以内，且 `bridge.log` 随后一条 `state_update` 里该厨师位置随之变化；再发 `--interact cutting_board_0` 让它取番茄并放上砧板切完，画面与状态都对得上。

- [ ] 读出可用关卡列表（进游戏界面读；进菜单本身就要靠这条任务做出来的输入注入），写进 spec 开放问题 1，供固定地图的决策使用
- [ ] 输入注入路径选定并写进 `plugin/README.md`：为什么走这条路、代价是什么（对应 spec 开放问题）
- [ ] 移动用连续方向输入实现，不是瞬移；到达判定带容差，避免在目标点抖动
- [ ] 交互、抓取/放下、投掷各有独立动作，未实现的动作返回明确错误而不是静默失败
- [ ] 动作有超时（默认 10 秒），超时回一条失败事件，避免上层永远等
- [ ] 一条 `action` 的执行结果能从后续 `state_update` 里看出来
- [ ] 记录一次真实操作日志片段到 `plugin/README.md`，作为答辩素材

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

**Check（未验证完）**：`bash scripts/build-plugin.sh` 0 警告 0 错误；桥侧那一半（`worker_stub --target`
→ 输入槽带目标 → 桥构造 `move`）由 `bridge/tests/test_protocol.c` 的 action 用例与 `tools/run-bridge-scenarios.sh`
的场景覆盖。**真机那一半没跑成：游戏起不来了**（见下）。

**卡在哪**：`steam -applaunch 728880` 与 `xdg-open steam://run/728880` 都只打印一行"启动 appid 728880"，
之后**没有游戏进程、Steam 控制台日志也没有新条目**（`~/.steam/steam/logs/console-linux.txt` 最后一条
还是 10:09:11 那次"Game Recording - game stopped"）。已排除的：两个 appmanifest 都是 `StateFlags 4`
（已装全）；10:09–10:11 Steam 在给 appid 1364780（Street Fighter 6 的 shader cache）跑一次更新，10:12 起
下载速率 0；Steam 主进程与 `steam.sh` 都在跑；不是我们的插件崩（游戏进程根本没起来）。
**下一步**：把 Steam 重启一下（或手动点 Play 并告诉我它报什么），然后跑

```sh
# 桥 + 进关卡（dev-cycle 会自己把游戏点起来）
bridge/build/kitchenos-bridge --link listen --host 127.0.0.1 --port 14455 --log logs/bridge.log &
bash scripts/dev-cycle.sh 15 s_sushi_1_1
# 让"某个 Worker"命令厨师 0 去一个坐标（s_sushi_1_1 的厨房在 x∈[8,22]、z∈[-6,-2] 一带）
bridge/build/kitchenos-worker-stub --state-name /kitchenos_state --sem-name /kitchenos_input_ready \
    --chefs 2 --slot 0 --kind move --target 12,-3 --hz 30 --duration 12
python3 -c "import json;print(json.load(open('$GAME_DIR/BepInEx/wellcooked-state.json'))['chefs'])"
```

判据：插件日志出现 `action: 厨师位 1 去 (12.0,-3.0)` 与 `到达`，且状态文件里 0 号厨师落在 (12,-3)
一格（0.6 米）以内；再依次跑 `--kind pickup`（料箱旁应把料拿起来）与 `--kind dash`。

**Check 原文里的 `python3 tests/send_action.py --chef 0 --move 12,6` 不存在**：Linux 侧下动作的正路是
Worker 写共享内存输入槽（`bridge/tools/worker_stub.c` 就是那个替身），Bridge 按帧转发；这一轮给它加了
`--target`。

**没验证到的**：动作在真游戏里的效果（move 到位、pickup 拿起料、dash 生效）—— 等游戏能起来；
`interact` 的 `target_object` 插件不读（协议 §3.6 明说当前不读，按 A 是对着厨师脚下的工位按）。
