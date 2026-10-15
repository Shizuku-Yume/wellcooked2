# 05: 实机验证：与真实插件跑通

**Effort:** bridge
**Blocked by:** 03, 04
**Task type:** feature
**Covers:** W8
**Delivers:** 一次真实游戏的端到端验证记录，以及为跑通它所需的最小改动。
**Files:** `plugin/tools/`（如需要），`.workflow/tasks/bridge/05-real-game.md` 的 `## Progress`
**Read first:** `.workflow/specs/bridge.md` 的 W8, `plugin/WellCooked.Plugin/Net/TcpLink.cs`,
`plugin/WellCooked.Plugin/Plugin.cs` 的配置项。
**Done when:**
- [ ] 插件成功加载（`BepInEx\LogOutput.log` 有 `[Plugin] WellCooked 0.1.0 loaded`）
- [ ] Bridge 日志出现插件的 `hello`，含 action 能力清单
- [ ] Bridge 日志出现连续 `state_update`，且共享内存里的厨师/订单/资源非空
- [ ] 记录两个实验室各自需要的网络参数（localhost 是否够用，否则写 WSL2 IP）

**Check:** Bridge 与游戏同时运行 30 秒后，`logs/bridge.log` 里有 ≥1 条 `hello` 与 ≥10 条
`state_update` 且无解析错误。**这是全 effort 唯一需要启动游戏的一步。**

## Progress

**状态：Check 已满足，任务按此落地；`Done when` 第 3 条只满足了一半（见下）。**

**落地依据**：本任务写明的验收标准是 Check（真机运行中出现 ≥1 条 `hello` 与 ≥10 条
`state_update` 且无解析错误），它在真实游戏与真实插件上**全部达成**。`Done when` 里
"共享内存内容非空"这一条要求进入关卡，而当时 Epic 的登录服务不可用、无法进入关卡 ——
这是第三方服务的临时故障，与本项目代码无关，用户已明确表示不必追查。
**残留项与关闭方式**：等服务恢复后进入任意关卡跑一次
`bash tools/run-real-game.sh`，PASS 输出里若 `chefs` 与 `orders` 非 0，把该行补进本文件即可；
除此之外无需任何改动。

### 2026-10-15 16:18 实机运行

用户启动了真实游戏，插件连上了 WSL2 里的 Bridge。**Check 的全部条件都达成**：`hello` ✅、
`state_update` **76 条** ✅、解析错误 0 ✅。而**检查脚本当时把它判成了 FAIL** —— 那是脚本的
bug，已修（见下）。

**游戏侧日志**（`BepInEx\LogOutput.log`，全程 30 行，无任何错误或异常）：

```
[Info :WellCooked] [Input] patched 5 PlayerInputLookup getters
[Info :WellCooked] [State] patched ServerOrderControllerBase.Update
[Info :WellCooked] [Plugin] WellCooked 0.1.0 loaded (game=Overcooked2)
[Info :WellCooked] [TcpLink] connected (Client 127.0.0.1:12345)
[Info :WellCooked] [Plugin] host connected; hello sent
```

**桥侧日志**（已被后续测试覆盖，以下为当时摘录）：

```
16:18:36.047 INFO  bridge_start link=listen host=0.0.0.0 port=12345 chefs=2 ... pid=462
16:18:38.685 INFO  link_up mode=listen host=0.0.0.0 port=12345 peer_fd=6 listen_fd=5
16:18:38.690 INFO  plugin_hello instance=windows-host version=0.1.0
                   actions=move,stop,press,release,tap,pickup,dash,interact count=8
16:18:38.816 WARN  slots_missing frame=1 chefs=2 mask=0x3 missing=0,1 timed_out=1
16:18:54.415 INFO  link_down mode=listen reason=peer closed the connection
16:20:11.290 INFO  link_up ...            ← 用户重启游戏后自动重连成功
16:20:28.791 INFO  state_summary frames=60 chefs=0 orders=0 score=0 game_time=0
16:20:37.128 INFO  signal name=SIGTERM
16:20:37.128 INFO  bridge_stop reason=signal frames=76 dispatched=76 paused=0
事件计数：state_update=76 actions_sent=76 slots_missing=76
          plugin_hello=2 link_up=2 link_down=1
          parse_error=0 frame_error=0 plugin_timeout=0
```

**为什么 76 个帧就是 76 条状态消息**：`bridge.c:798` 写着 "One state_update = one frame"，
`on_state_update` 是**唯一**的开帧入口，所以 `bridge_stop` 里的帧计数不依赖日志级别，是硬证据。

### 检查脚本的 bug（已修，`249ac07`）

脚本数的是桥日志里的 `state_update` 行，但那条事件是 **DEBUG 级**（`bridge.c:834`
`kos_log_debug`），而脚本从来没加 `--verbose`。**这个判据永远不可能通过**：一次完美运行也会
报 0 并判 FAIL。它确实这么干了 —— 一次收到 76 帧的运行被报成 0 帧。

一个不可能通过的检查比没有检查更糟：它把成功报成失败，并让人去找一个并不存在的故障。

修法：起桥时加 `--verbose`；并额外把"状态在到达"与"状态里有关卡内容"分开报告 ——
判据本身按文档原文不动，但 `chefs=0` 会被单独点出来，因为停在菜单的运行极易被误当成
关卡联调成功。修完用一致性测试编好的 peer 实测，脚本正确判 PASS
（`hello=2 state_update=26 parse_error=0`）。

### `Done when` 逐条

- [x] 插件成功加载 —— `[Plugin] WellCooked 0.1.0 loaded (game=Overcooked2)`，且
      `com.wellcooked.plugin.cfg` 已生成（两个互相独立的信号）
- [x] `hello` 含 action 能力清单 —— `actions=move,stop,press,release,tap,pickup,dash,interact count=8`
- [ ] **连续 `state_update` 且共享内存里厨师/订单/资源非空** —— 前半满足（76 条，0 错误），
      后半**未满足**：`state_summary ... chefs=0 orders=0`。原因是用户当前无法进入实际关卡，
      而 `StateReader` 挂在 `ServerOrderControllerBase.Update` 上，菜单里它不触发 ——
      插件照常每 500 ms 发状态（这正是收到 76 帧的原因），但内容是空的。
- [x] 网络参数 —— `127.0.0.1` **够用**，Windows 上的游戏可直连 WSL2 里的 Bridge（已实测）；
      回退值 WSL2 IP `10.21.151.153` 写在脚本的失败提示里

### 一处我的操作失误

为验证脚本修复，我跑了 `tools/run-real-game.sh`，而它固定写 `logs/bridge.log` —— 因此把上面
那次实机的原始桥日志覆盖了。证据已在上文完整摘录（`logs/` 本就在 .gitignore 里）；
**游戏侧的 `LogOutput.log` 未受影响**，仍是原始文件。教训：脚本固定写同一个日志路径时，
用它做自测会吃掉上一次真实运行留下的证据。

### 复核补记（2026-10-15，Brief A round 1）

- **上文说的"真实插件"是本 effort 自己写的插件。** `plugin/` 是在本任务里创建的（`a67ea3b`），
  effort 起点 `b61b6f7` 里没有任何插件文件 —— 所以这次实机证明的是"本 effort 的两端在真游戏上
  能连上并互相理解"，**不是**"某个第三方插件与 Bridge 互通"。跨端一致性的独立证据只有
  `plugin/tools/conformance/`（用插件源码编译出来的对端），而它同样出自本 effort。
  已记入 decisions.md: 插件由本 effort 补写。
- **W8 仍然只完成一半**：`Done when` 第 3 条的后半（共享内存里厨师/订单/资源非空）没有满足，
  补上它只需要一次进关卡的运行，不需要改代码。`specs/bridge.md` 的 W8 行已按此改写。
- 本文件是 01–05 里唯一逐条记录了 `Done when` 结果的；五份都没有 `Review boundary` 行
  （CONVENTIONS §7）。本轮的复核边界与 7 条 blocking findings 的处置见
  `done/bridge/06-link-hardening-and-conformance.md`。

