# 03: 插件连上 Bridge 并上报世界状态

**Effort:** game-bridge
**Task type:** feature
**Base commit:** 9d72d38
**Base branch:** game-bridge/10-plugin-cleanup
**Started:** 2026-10-16T02:30:00+08:00
**Finished:** 2026-10-16T10:20:00+08:00

**Delivers:** 游戏跑起来后，插件自己连上 Bridge 并每帧上报世界状态；关掉 Bridge 游戏不崩、插件持续重连，Bridge 重启后连接自动恢复。Bridge 侧能看到与画面一致的得分和厨师数。

**Blocked by:** 01, 02
**Files:** `plugin/`（TCP 客户端、状态读取、Harmony patch）、`bridge/`（如需按实测修正解析）、`scripts/run-game.sh`

**Read first:** spec「What it does」「How it is built」「How we will know it works」W3、W4、W7；方案 §9 阶段A2、§5.2 `state_update` 字段；`protocol/protocol.md`
**Covers:** W3, W4, W7

**Note:** 链路层落到哪一份插件已定（`decisions.md: 2026-10-16 - 两个插件工程合并到 net35 那份`）：
本任务在 `plugin/*.cs`（net35，演示机那份）上做，`plugin/WellCooked.Plugin/Net/TcpLink.cs` 与
`Protocol/Messages.cs` 是搬运来源，那份之后只留作参考实现。`technical-debt.md` 的「两个插件工程并存」
拆成两半：状态读取与链路在**本任务**（已完成），输入注入在任务 04，**04 落地后删掉那条债务**。

**Check:** 启动 Bridge，再启动游戏进一局：`bridge.log` 5 秒内出现 `client connected`，随后持续收到 `state_update`，其中 `score` 与画面一致、`chefs` 数量与场上厨师数一致（人工比对一次）；`kill` 掉 Bridge，游戏继续运行且插件日志出现重连行，重启 Bridge 后 5 秒内重新连上。

- [x] 插件用后台线程跑 TCP 客户端，不阻塞游戏主线程
      （**TCP 收发**在后台线程；但状态读取、JSON 构造仍在游戏主线程 —— 首帧 `StateFrame.Build` 实测
      7.0 ms，常态耗时与帧率**没有单独测**。间接证据只有：桥连着、约 60 Hz 上报的同时本地工序跑完了一单）
- [x] 连接失败后重试，日志打印重试次数；对端断开后同样重连
      （**对端立刻拒绝**时实测约 2 秒一次、带「第 N 次」；**连接建立阶段超时**时还会叠加最多 3 秒
      connect 等待，这一条路径没有单独实测 —— 见"没验证到的"）
- [x] 状态读取覆盖（**代码路径**）：厨师位置、手持物、订单列表与剩余时间、锅与砧板状态、盘子数量、当前得分
      （真机日志只核到 `chefs=2`、`score=28`、`orders=4`、`game_time=19` 四项；位置/手持物/锅/砧板/盘子
      的字段值没有逐项从真机日志核过）
      （**朝向没上报**：协议 §3.1 与 C 侧 `proto_chef` 都没有这个字段，见 Review 第 5 条）
- [x] 上报频率可配置（`state_ms`，0 = 每帧）
- [x] 状态帧**预算** 4 KB：实测首帧 84 字节（`MaxStateBytes = 4096` 只是日志阈值，**不截断也不丢弃**；
      协议的分帧硬上限是单行 1 MiB。进关卡后含厨师/订单/资源的较大帧没有实测）
- [x] 游戏内读到的对象/字段记进 `plugin/README.md`（类型名、字段名、取值含义），供后面写调度用
- [x] 关掉 Bridge 期间游戏不卡顿、不崩溃（游戏继续跑、状态文件继续更新；帧率同样没有单独测）

**Progress:**

## Progress

Dirty at start: none（从 `9d72d38` 起，叠在 `game-bridge/10-plugin-cleanup` 上）。

**Check before**（改之前**演示用的 net35 插件**没有接入链路；net46 参考实现 `plugin/WellCooked.Plugin/`
早就有这套东西，只是那份插件没被用上）：`grep -rn 'TcpClient\|Socket' plugin/*.cs` 在 net35 插件根目录无命中；
`bridge.log` 里只有假客户端（`tools/fake_plugin.py`）留下的 `plugin_hello instance=windows-host`，
没有来自本机插件的行。所以这条 Check 在改之前**跑不起来**（没有可连的客户端）。

**改了什么**（链路层从 net46 那份参考实现搬进本工程，`plugin/Link/`）：

- `plugin/Link/TcpLink.cs`：单后台线程的 TCP 链路，一行一条 UTF-8 JSON、`\n` 分帧、单行上限 1MB、
  心跳与断线重连。只改了三处游戏 Mono 上没有的类型（`ConcurrentQueue`→`Queue`+锁、
  `ManualResetEventSlim`→`ManualResetEvent`、`DateTimeOffset.ToUnixTimeSeconds`→自己算），
  分帧规则与上限逐字保留（那是协议的一部分）。
- `plugin/Link/Messages.cs`：`JsonBuilder`（`resources` 是动态键，JsonUtility 表达不了）、`MsgType`、
  `Msg`（hello/heartbeat/ack/echo_reply）、入站 `InboundMessage`。字段名与 `protocol/protocol.md` 对齐。
- `plugin/Link/StateFrame.cs`：一帧 `state_update`。游戏事实一律走 `WorldState.Get/Invoke/StaticMethod/FindAll`
  （与落盘那份 `wellcooked-state.json` 同一套反射），不另写取字段的办法。
- `plugin/Link/LinkRunner.cs`：配置（`bridge=`、`state_ms=`，写在 `wellcooked-takeover.txt` 里，
  5 秒内生效）、连上发 hello、按间隔发状态、入站分发（心跳/hello/echo/state_request/config/action）、
  动作交给执行器（任务 04 装上；没装时回 `ack ok=false`，不假装执行过）。
- `plugin/Takeover.cs` 的每帧钩子里加 `LinkRunner.Tick(now)`；`plugin/SmokePlugin.cs` 里 Install；
  `WorldState` 补 `Flow()`/`Team()` 两个访问器（摘要与上报共用同一份查找）。

**Check after（第二轮，修完复核意见后）**（2026-10-16，同一套命令）：

| 判据 | 实测 |
|---|---|
| 连上 + 握手 | 桥侧 `link_up … port=14455`、`plugin_hello instance=YILONG15PRO-plugin version=0.1.0 actions=… count=8`（`instance` 从写死的 `fedora-proton` 改成"机器名-plugin"，桥侧因此认得出是哪台机器） |
| 单帧大小与开销 | 插件侧 `link: 首帧 state_update 84 字节，构建 7.0 ms`（4 KB 是状态帧预算、只警告不截断；协议分帧硬上限单行 1 MiB；订单按协议容量 32 截断） |
| 持续上报 | 桥侧 `state_summary frames=60 → 5580`，进关卡后 `chefs=2`；**`score=28`、`orders=4`、`game_time=19`** —— 得分与订单经链路解析正确（这一轮本地工序同时跑完了一单，说明桥连着、每帧下发 idle/stop 也不影响本地驱动） |
| 配置热生效 | 把 `bridge=` 改成 `127.0.0.1:14456`：插件 `link: 地址改成 127.0.0.1:14456，重连` + `connect to 127.0.0.1:14456 failed（第 2/3/4/5 次，2000 ms 后重试）`（约 2 秒一次，重试次数进日志）；改回 14455 后桥侧又出现一次 `link_up` |
| kill Bridge 游戏不崩 | 游戏侧 `link: 与 Bridge 断开，后台每 2 秒重试`，`wellcooked-state.json` 仍在更新（`scene s_sushi_1_1 chefs 2`） |
| 重启 Bridge 自动恢复 | 重启后 1 秒内桥侧又出现 `link_up` + `plugin_hello`（同一次运行内实测） |

**Check after（第一轮）**（2026-10-16，`bash scripts/build-plugin.sh` 0 警告 0 错误；Bridge 用
`bridge/build/kitchenos-bridge --link listen --host 127.0.0.1 --port 14455 --log logs/bridge.log`，
游戏用 `bash scripts/dev-cycle.sh 20 s_sushi_1_1` 进关卡）：

| 判据 | 实测 |
|---|---|
| 连上 | 桥侧 `link_up mode=listen host=127.0.0.1 port=14455 peer_fd=7` |
| 握手 | 桥侧 `plugin_hello instance=fedora-proton version=0.1.0 actions=move,stop,press,release,tap,pickup,dash,interact count=8` |
| 持续上报 | 桥侧 `state_summary frames=60/120/180…`，进关卡后一路到 `frames=3685`（≈60/s）；进关卡那一帧 `state_summary frames=60 chefs=2 orders=2 score=0 game_time=0` |
| 厨师数与场上一致 | 桥侧解析出 `chefs=2`，与 `wellcooked-state.json` 的 `chefs` 数一致（同一帧两处都读 2 名） |
| kill Bridge 游戏不崩 | 游戏侧 `link: 与 Bridge 断开，后台每 2 秒重试`，`wellcooked-state.json` 仍在更新（`scene s_sushi_1_1 chefs 2`） |
| 重启 Bridge 自动恢复 | 重启后 1 秒内桥侧又出现 `link_up` + `plugin_hello`（同一次运行内实测） |

**Check 原文里 `client connected` 这个字样**现在对应桥侧 `link_up`（任务 02 归档时记过同一件事）；
`python3 tests/send_action.py` 不存在，动作那半属于任务 04。

**没映射的字段**（协议里都是可选项，缺省即"没有这个信息"，写在这里免得下游以为是 bug）：
`chefs[].state`（游戏没有现成的 moving/idle 字段）、`orders[].ingredients`（原料表在菜谱 DAG
`config/recipes.json` 里，调度侧从那儿读）、`resources[].chef_id`（客户端工作台没有 `m_interacters`，
那是服务端字段）、锅的 `content`（取得到就填，取不到省略）。`resources` 按 0.25 秒缓存，
别每帧把全场景的锅/砧板/盘子找一遍。

### Review（CONVENTIONS §7）

`Review boundary: 对比点 game-bridge/10-plugin-cleanup（本任务叠在任务 10 的 PR #15 上；Base commit 9d72d38 是历史点）；路径 plugin/Link/*、plugin/{WorldState,Takeover,SmokePlugin,LogicalInput}.cs、plugin/README.md、.workflow/{standards.md,technical-debt.md}`

两条 brief 各一个 reviewer（harness 里没有 `workflow-reviewer`，用 `reviewer` 代）：

**brief B（3 blocking + 2 nonblocking）** —— 全部已修：

1. **重试间隔翻倍**：`AcquireClient` 失败后自己睡 2 秒、`Worker` 的 `finally` 又睡 2 秒，实际约 4 秒；且日志没有重试次数 → 改成只有 `finally` 等一次（server 模式没有那一处，所以 server 的两条失败路径自己等），失败日志带"第 N 次，2000 ms 后重试"，连上时补一行"第 N 次重试连上了"并清零。实测见上表。
2. **配置热改地址不生效**：`ApplyConfig` 见到 `_link != null` 就返回 → 记下当前连接的 host/port，变了就 `Stop()` 重建；实测见上表。
3. **订单 id 读不到时合成 `order_000`**：两张读不到 id 的单会撞号 → 改成**省略 `id` 字段**（协议里 id 可选）+ 只报一次。
4. **锅/砧板状态读不到时推断 `free`**（nonblocking）：会让调度侧以为锅空着 → 改成不写 `state`、只报编号 + 只报一次。
5. **没有单帧耗时/GC 证据**（nonblocking）：加了首帧的字节数与构建耗时日志（实测 84 字节 / 7.0 ms，首帧含预热；60 fps 下每帧一次 `FindObjectsOfType<PlayerControls>` 与 0.25 秒一次的资源扫描）。

**brief A（5 blocking + 2 nonblocking）** —— 4 条已处理，1 条记成已知缺口：

1. **重试间隔与次数**（同 B1）→ 已修。
2. **4 KB 上限没有落实与验证** → 订单按协议容量 32 截断、资源按锅/砧板/盘子实际数量（当前地图远小于 48，
      但发送端没有 48 的上限），实测首帧 84 字节；超 4 KB 只报一次警告（丢帧会让帧屏障少一帧，所以不丢）。
      评审指出"单条消息小于 4 KB"不是当前代码保证的行为 —— 判据改成"预算是 4 KB、硬上限是 1 MiB"。
3. **`plugin/README.md` 缺失** → 已补：游戏对象/字段/含义表、两种状态形状的区别、文件分工。
4. **技术债「两个插件工程并存」该由本任务删** → 拆成两半：状态读取与链路在本任务完成，输入注入在任务 04；条目已改成"04 落地后删掉本条目"，本任务不删（现在删等于宣称输入注入也合并完了）。
5. **厨师朝向没有上报** → **不改协议**，记为已知缺口：`protocol/protocol.md` §3.1 与 C 侧 `proto_chef` 都没有朝向字段，调度侧要的是坐标（动作是"走到某个坐标"），加字段是一次跨端改动（`protocol.h`/`protocol.c`/`schema`/`shm`/C 测试）。方案 §9 A2 写了"朝向"，要补就开一个跨端任务。
6. **Check 的"与画面得分一致"没有人工比对**（blocking，证据类）→ 桥侧解析出的 `score=28`/`orders=4` 与
      插件自己那份 `wellcooked-state.json` **同源**（同一套反射读同一批对象），只能证明两条输出路径一致，
      **不能代替"画面就是 28"**；与屏幕比对只能人做，留在这里交给验收的人。
7. **断开期间"不卡顿"没有帧率证据**（nonblocking）→ 未测，见"没验证到的"。

**没验证到的**：断开 Bridge 期间与高频上报时的游戏帧率（只有"状态文件仍在更新、游戏没崩"这一层）；
厨师朝向（见上）；`orders[].ingredients`（原料表在 `config/recipes.json` 里，调度侧从那儿读，不每帧重算）。
