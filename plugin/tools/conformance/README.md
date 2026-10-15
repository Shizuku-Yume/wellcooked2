# 跨实现一致性测试：C 桥 ↔ 插件自己的协议代码

`tools/fake_plugin.py` 是对线格式的**重新实现**：只要它和 C 桥错得一样，两边就会互相点头，
而 `tools/run-real-game.sh` 里最怕的正是"连上了、但两端对线上的理解不一致"。能证伪这一点的
只有插件自己的代码。本目录的对端程序就是拿插件的 `Messages.cs` + `TcpLink.cs`
**原样**编译出来的，一个字节都不改。

## 1. 三个文件

| 文件 | 作用 |
|---|---|
| `PluginPeer.cs` | 对端程序（C# 5）。与上面两个插件源文件一起编译；阶段 1 用裸 socket 造分帧探针，阶段 2 用插件自己的 `TcpLink` 跑真实会话 |
| `run-conformance.ps1` | Windows 半场：用 Windows 自带的 `csc.exe` 编译对端，再把运行交给 WSL |
| `run-conformance.sh` | WSL 半场：起桥与 worker stub、跑对端、读桥日志并与对端退出码一起判定 |

编译产物 `bin/PluginPeer.exe` 与 `logs/` 都不入库（`plugin/.gitignore` 的 `bin/`、根 `.gitignore` 的 `logs/`）。

## 2. 怎么跑

```powershell
# 默认：端口 12401，60 Hz 跑 3 秒，要求观察到 move 与 stop
powershell -NoProfile -File plugin/tools/conformance/run-conformance.ps1

# 换端口/时长/必须出现的 action 种类
powershell -NoProfile -File plugin/tools/conformance/run-conformance.ps1 -Port 12401 -Seconds 3 -RequireKinds move,stop

# 让 stub 去写别的动作（最多两个，依次进 slot 0 / slot 1），用来把扩过的 action 枚举
# 也交给插件自己的解码器验一遍：
powershell -NoProfile -File plugin/tools/conformance/run-conformance.ps1 -RequireKinds pickup,dash -StubKind pickup,dash
```

对端已编译过时，WSL 半场可以单跑：

```bash
bash plugin/tools/conformance/run-conformance.sh 12401 3 move,stop
bash plugin/tools/conformance/run-conformance.sh 12402 3 pickup,dash pickup,dash
```

退出码：**0** 全部通过；**1** 两端对不上（脚本会打印是哪一项）；**3** 前置条件不成立
（`csc` 不在、对端没编译出来、桥没起来）。

端口默认 **12401** 而不是 14455，共享内存用 `/kitchenos_conformance` +
`/kitchenos_conformance_sem`，所以这一轮可以和真插件、或别人的 W8 运行同时进行，不会互相踩。

## 3. 它验了什么

对端只用插件自己的构造器产生字节（`Msg.Hello` / `Msg.Ack` / `Msg.Heartbeat`，以及用
`JsonBuilder` 按 `StateReader.BuildStateJson` 的形状搭出来的 `state_update`），读回来的每一帧
都按 `InboundMessage` 的字段与 `MsgType` 的名字解析。判据是桥自己的 verbose 日志与对端退出码：

- **阶段 1：分帧探针**（裸 socket，故意不发 hello）
  - 一行拆成两次写（2026-10-15 实测：先 7 字节，150 ms 后 612 字节，整行 619 字节）→ 桥必须缓存重组；
  - 两行并在一次写里（`hello` 138 B + `state_update` 623 B，共 761 B）→ 桥必须切成两行；
  - 心跳、`echo` 往返（token 里带引号、反斜杠和中文，验转义与 UTF-8）；
  - 容量探针：50 个资源键 + 一个 41 字符长键 → 桥的 `resources=48 dropped=2 truncated=1` 必须
    与预测值完全一致（计数器没接上会被抓住，而不是永远显示 0）。
  - 三个探针帧都带不可能出现的 `game_time` 标记（**4242 / 4243 / 4244**），脚本就是靠它把桥
    日志里某一行钉到"产生它的那一个探针"上——不是"有东西来了就算过"。
- **阶段 2：TcpLink 客户端会话**（插件默认配置：`Network.Mode=client`、`HeartbeatMs=2000`、
  `HeartbeatTimeoutMs=0`、`ReconnectDelayMs=3000`）
  - 连上后延迟一帧发 `hello`（与 `Plugin.Update()` 的时序一致，避开 TcpLink 清空队列的竞态）；
  - 以 60 Hz 发写实的 `state_update`（两个厨师含坐标与手持物、两张订单其中一张菜谱为 null、
    锅/灶/工位/`plate_stack` 等五种资源形状，外加一个 C 侧归不了类的键）；
  - 解码桥回的每一条 `action`：`action` 名必须在 `Plugin.HostActions` 的 8 个之内（线上的
    枚举是其中 5 个：`move`/`stop`/`interact`/`pickup`/`dash`，见 `protocol/protocol.md` §3.6）、
    `chef_id` 必须是本对端上报过的、`move` 必须带有限数值的 `x`/`y`；同时按 `Plugin.HandleAction`
    的做法回 `Msg.Ack`（这条路径也因此被 C 侧的 `plugin_ack` 覆盖到）；
  - 桥日志要求：`parse_error=0`、`frame_error=0`、`plugin_timeout=0`。

## 4. 它没验什么（别当成全覆盖）

- **Unity 的 `JsonUtility`**：`Plugin.cs` 用它解析入站 JSON，但它离不开 Unity 引擎，链接不进控制台
  程序。所以入站方向验的是**插件的字段契约**（反射 `InboundMessage` 的公开字段 + 自己写的严格
  RFC 8259 读取器 + 未知键/类型不符即失败），**不是** Unity 解析器的行为。JSON 解析正确、只有
  JsonUtility 才会犯的怪癖，本测试不覆盖。
- **`state_update` 不是 `Messages.cs` 生成的**（它在 `StateReader.cs` 里，依赖 `UnityEngine`）：
  对端是按 `StateReader.BuildStateJson` 的精确形状、用插件的 `JsonBuilder` 自己搭的，源码里每条
  资源都注了对应 `StateReader` 的位置。
- **没跑真游戏、没有 W8**：真游戏与 W8 的验收仍然是 `tools/run-real-game.sh` 的事，本测试不需要 Unity。
- 默认一轮只观察到 `move` 与 `stop` 两种 action（worker stub 默认写 `move`，其余是桥的空转
  `stop`）。`-StubKind` 能把 `move`/`stop`/`interact`/`pickup`/`dash` 里的一到两个写进 slot 0/1：
  2026-10-15 实测 `-StubKind pickup,dash` 一轮，对端各解出 109/110 条、0 条被拒，`VERDICT: PASS`。
  插件的 `press`/`release`/`tap` 没有线上表达，所以谁也产生不了它们。
- 未覆盖：1 MiB **行**上限的边界（本对端不发超长行；这一条由 C 侧单测
  `bridge/tests/test_framing.c` 与 `tools/fake_plugin.py` 的 `bad-frames` 场景覆盖）、
  心跳超时真正触发后的暂停/恢复、非有限浮点数。

## 5. 证据文件（`logs/`，已 gitignore）

| 文件 | 内容 |
|---|---|
| `conformance-run.out` | 一次完整运行的输出（编译 + 两端输出 + 判定），默认 `-StubKind move` 那一轮 |
| `conformance-run-pickup-dash.out` | `-RequireKinds pickup,dash -StubKind pickup,dash` 那一轮 |
| `conformance-peer.out` | 对端 stdout：探针细节、解出的每一条 action、汇总与结论 |
| `conformance-peer-move.out` / `conformance-peer-pickup-dash.out` | 上面两个文件在各轮结束时的副本（后一轮会覆盖原文件） |
| `conformance-bridge.log` | 桥的 verbose 日志（唯一权威的解析证据） |
| `conformance-bridge-move.log` / `conformance-bridge-pickup-dash.log` | 同上，逐轮副本 |
| `conformance-stub.out` | slot 0 的 worker stub 日志 |
| `conformance-stub-slot1.out` | slot 1 的 worker stub 日志（只在给了两个 `-StubKind` 时产生） |
| `conformance-csc.out` | 编译器输出；0 字节表示编译器一声没吭 |

## 6. 环境事实（踩过的坑，别再踩）

- **`csc` 不能从 WSL interop 驱动**：工作目录是本仓库（路径含中文）时，它打不开传给它的任何源
  文件，报 `Source file '<仓库>\PluginPeer.cs' could not be opened`；同一条命令在 Windows 侧
  干净通过。所以编译放在 `.ps1` 里，跨边界只传 ASCII 且不含空格的参数。
- **`run-conformance.ps1` 带 UTF-8 BOM**：Windows PowerShell 5.1 在没有 BOM 时按 ANSI 代码页读脚本，
  中文注释会被读坏，甚至报"字符串缺少终止符"。`.cs` 与 `.sh` 保持无 BOM —— 实测本机 `csc`
  读无 BOM 的 UTF-8 C# 完全正常（中文字面量按预期输出），而 `bash` 有 BOM 会连 shebang 一起坏掉。
- `wsl.exe -e bash <脚本> <ASCII 无空格参数>` 是唯一能原样穿过边界的调用形式；把脚本体塞进
  `wsl.exe -c` 会被它用空格重拼、静默吃掉引号，跑起来的是别的东西。
- **worker stub 必须在桥建好共享段之后再起，而且桥活着时不要重启它**：slot 的 `frame_id` 是单调的，
  从 1 重新开始的 stub 对 `last_frame` 已经越过它的桥等于隐形（表现为该 slot 永远空转）。

## 7. 顺手记下的三处不一致（复核后的处置）

1. **已修**：`protocol/schema/state_update.schema.json` 的 `orders[].recipe` 改成
   `["string","null"]`。两端实现都接受 JSON `null`（`StateReader` 对解析不出的菜谱就发 null，
   C 侧 `json_object_get_string` 明确接受 null，`protocol/protocol.md` §3 也说 null 表示缺省），
   所以该改的是 schema，不是代码。`holding` / `content` 早就是可空的，这三处现在一致了；
   其余非必填标量字段的 `null` 也按缺省处理，schema 还没逐个标注（README §3 已写明）。
2. **已修文档**：`protocol/protocol.md` §3.6 原先只说 `interact` 的 `target_object` 必填，容易被
   读成"插件按它挑目标"。插件 `InputInjector` 处理 `interact` 时确实不读它（对厨师脚下的工位
   按键），契约表里现在写明了这一点；桥仍然总会带上，所以不炸。
3. **已记入契约与决策**：桥的存活判定**只看心跳**（`bridge/src/bridge.c`：`last_heartbeat_ms`
   只被心跳刷新），`state_update` 一直流也不算活着。桥默认 5000 ms，插件默认 `HeartbeatMs=2000`，
   余量 3 秒；插件若把心跳配到 ≥5 s，就会边发状态边被判失联。`protocol/protocol.md` §3.2/§4
   原先写的"任何消息"已改成与两端实现一致，配对与余量记进
   `decisions.md`: 2026-10-15 - 链路存活只看心跳。
