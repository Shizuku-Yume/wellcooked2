# 跨实现一致性测试：C 桥 ↔ 插件自己的协议代码

`tools/fake_plugin.py` 是对线格式的**重新实现**：只要它和 C 桥错得一样，两边就会互相点头，
而 `tools/run-real-game.sh` 里最怕的正是"连上了、但两端对线上的理解不一致"。能证伪这一点的
只有插件自己的代码。本目录的对端程序就是拿交付插件的 `plugin/Link/Messages.cs` +
`plugin/Link/TcpLink.cs` **原样**编译出来的，一个字节都不改。

## 1. 这些文件

| 文件 | 作用 |
|---|---|
| `PluginPeer.cs` | 对端程序（C# 5）。与被测的交付插件源文件一起编译；阶段 1 用裸 socket 造分帧探针，阶段 2 用插件自己的 `TcpLink` 跑真实会话 |
| `PluginPeer.csproj` | 编译入口：`PluginPeer.cs` + `plugin/Link/Messages.cs` + `plugin/Link/TcpLink.cs`，Linux 用 `dotnet build` |
| `run-conformance.sh` | Linux 半场：自己编译对端，起桥与 worker stub、跑对端、读桥日志并与对端退出码一起判定 |
| `run-conformance.ps1` | Windows 备用编译路径：用 Windows 自带的 `csc.exe` 编译对端，再把运行交给 WSL（本机有 dotnet 时用不到） |

编译产物 `bin/` 与 `logs/` 都不入库（`plugin/.gitignore` 的 `bin/`、`obj/`，根 `.gitignore` 的 `logs/`）。

被测源文件是**交付插件**的 `plugin/Link/Messages.cs` + `plugin/Link/TcpLink.cs`（不是
`plugin/WellCooked.Plugin/` 里的参考实现）；`PluginPeer.cs` 只补两件 Unity 控制台程序做不到的事：
裸 socket 分帧探针、以及按 `StateFrame.cs` 的形状搭 `state_update`。

## 2. 怎么跑

Linux 本机（需要 .NET 9 SDK；`~/.dotnet/dotnet` 或 PATH 里的都行，本机是 9.0.319）：

```bash
# 默认：端口 12401，60 Hz 跑 3 秒，要求观察到 move 与 stop
bash plugin/tools/conformance/run-conformance.sh

# 换端口/时长/必须出现的 action 种类
bash plugin/tools/conformance/run-conformance.sh 12401 3 move,stop

# 让 stub 去写别的动作（最多两个，依次进 slot 0 / slot 1），用来把 action 枚举
# 也交给插件自己的解码器验一遍：
bash plugin/tools/conformance/run-conformance.sh 12402 3 pickup,dash pickup,dash
```

没有 dotnet 的 WSL 可以用 Windows 侧编译路径（编译完仍由 `run-conformance.sh` 起桥判定）：

```powershell
powershell -NoProfile -File plugin/tools/conformance/run-conformance.ps1
```

`run-conformance.sh` 每次都会用当前磁盘上的源码重新 `dotnet build`；源文件比产物新时脚本
拒绝运行，避免拿旧字节得出这一轮的结论。

退出码：**0** 全部通过；**1** 两端对不上（脚本会打印是哪一项）；**3** 前置条件不成立
（没有编译器、对端没编译出来、桥没起来）。

端口默认 **12401** 而不是 14455，共享内存用 `/kitchenos_conformance` +
`/kitchenos_conformance_sem`，所以这一轮可以和真插件、或别人的 W8 运行同时进行，不会互相踩。

## 3. 它验了什么

对端只用插件自己的构造器产生字节（`Msg.Hello` / `Msg.Ack` / `Msg.Heartbeat`，以及用
`JsonBuilder` 按 `Link/StateFrame.cs` 的形状搭出来的 `state_update`），读回来的每一帧
都按 `InboundMessage` 的字段与 `MsgType` 的名字解析。判据是桥自己的 verbose 日志与对端退出码：

- **阶段 1：分帧探针**（裸 socket，故意不发 hello）
  - 一行拆成两次写（2026-10-15 实测：先 7 字节，150 ms 后 612 字节，整行 619 字节）→ 桥必须缓存重组；
  - 两行并在一次写里（`hello` 138 B + `state_update` 623 B，共 761 B）→ 桥必须切成两行；
  - 心跳、`echo` 往返（token 里带引号、反斜杠和中文，验转义与 UTF-8）；
  - 容量探针：50 个资源键 + 一个 41 字符长键 → 桥的 `resources=48 dropped=2 truncated=1` 必须
    与预测值完全一致（计数器没接上会被抓住，而不是永远显示 0）。
  - 三个探针帧都带不可能出现的 `game_time` 标记（**4242 / 4243 / 4244**），脚本就是靠它把桥
    日志里某一行钉到"产生它的那一个探针"上——不是"有东西来了就算过"。
- **阶段 2：TcpLink 客户端会话**（LinkRunner 参数：client、心跳 2000 ms、心跳超时关、
  重连 2000 ms）
  - 连上后延迟一帧发 `hello`（与 `LinkRunner.Tick` 的时序一致，避开 TcpLink 清空队列的竞态）；
  - 以 60 Hz 发写实的 `state_update`（两个厨师含坐标与手持物、两张订单其中一张菜谱为 null、
    锅/灶/工位/`plate_stack` 等五种资源形状，外加一个 C 侧归不了类的键）；
  - 解码桥回的每一条 `action`：`action` 名必须在 `LinkRunner.HostActions` 的 5 个之内
    （`move`/`stop`/`interact`/`pickup`/`dash`，见 `protocol/protocol.md` §3.6）、
    `chef_id` 必须是本对端上报过的、`move` 必须带有限数值的 `x`/`y`；同时按
    `LinkRunner.HandleAction` 的做法回 `Msg.Ack`（这条路径也因此被 C 侧的 `plugin_ack` 覆盖到）；
  - 桥日志要求：`parse_error=0`、`frame_error=0`、`plugin_timeout=0`。

注意：对端的 `HostActions` 是**手抄** `LinkRunner.HostActions` 的常量，不随源文件编译；
它锁的是"线上枚举必须是这五个"，`LinkRunner` 那边改了列表要同步这里。

## 4. 它没验什么（别当成全覆盖）

- **Unity 的 `JsonUtility`**：`LinkRunner.cs` 用它解析入站 JSON，但它离不开 Unity 引擎，链接不进控制台
  程序。所以入站方向验的是**插件的字段契约**（反射 `InboundMessage` 的公开字段 + 自己写的严格
  RFC 8259 读取器 + 未知键/类型不符即失败），**不是** Unity 解析器的行为。JSON 解析正确、只有
  JsonUtility 才会犯的怪癖，本测试不覆盖。
- **`state_update` 不是 `Messages.cs` 生成的**（它在 `Link/StateFrame.cs` 里，依赖 `UnityEngine`）：
  对端是按 `StateFrame.cs` 的精确形状、用插件的 `JsonBuilder` 自己搭的，源码里每条
  资源都注了对应 `StateFrame` 的位置。
- **没跑真游戏、没有 W8**：真游戏与 W8 的验收仍然是 `tools/run-real-game.sh` 的事，本测试不需要 Unity。
- 默认一轮只观察到 `move` 与 `stop` 两种 action（worker stub 默认写 `move`，其余是桥的空转
  `stop`）。`stub-kind` 能把 `move`/`stop`/`interact`/`pickup`/`dash` 里的一到两个写进 slot 0/1：
  2026-10-15 实测 `pickup,dash` 一轮，对端各解出 109/110 条、0 条被拒，`VERDICT: PASS`。
  交付插件的 `press`/`release`/`tap` 没有线上表达（`LinkRunner` 只声明五个动作），所以谁也产生不了它们。
- **运行的运行时不是游戏里的 Mono**：对端在 .NET（Linux 下是 dotnet）上跑，验的是线格式与字段契约；
  `TcpLink` 在游戏 CLR 2.0 上的行为（如 `Socket.Poll` 的时序、线程调度）本测试不覆盖。
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
| `conformance-csc.out` | 编译器输出（`dotnet build` 或 `csc`） |

## 6. 环境事实（踩过的坑，别再踩）

- **Linux 本机用 dotnet，不走 csc**：`run-conformance.sh` 找到 dotnet（PATH 或 `~/.dotnet`）就用
  `PluginPeer.csproj` 编译并直接运行 DLL。只有没有 dotnet 的 WSL 才退回 `.ps1` 编出的 PE；
  `csc` 不能从 WSL interop 驱动（工作目录是本仓库、路径含中文时打不开源文件，报
  `Source file '<仓库>\PluginPeer.cs' could not be opened`），所以那条路必须在 Windows 侧编译。
  本轮只在 Fedora 上用 dotnet 跑通；本机没有 mono/mcs/csc，Windows 备用路径未复测。
- **`run-conformance.ps1` 带 UTF-8 BOM**：Windows PowerShell 5.1 在没有 BOM 时按 ANSI 代码页读脚本，
  中文注释会被读坏，甚至报"字符串缺少终止符"。`.cs` 与 `.sh` 保持无 BOM —— Windows 侧实测 `csc`
  读无 BOM 的 UTF-8 C# 完全正常（中文字面量按预期输出），而 `bash` 有 BOM 会连 shebang 一起坏掉。
- `wsl.exe -e bash <脚本> <ASCII 无空格参数>` 是唯一能原样穿过边界的调用形式；把脚本体塞进
  `wsl.exe -c` 会被它用空格重拼、静默吃掉引号，跑起来的是别的东西。
- **worker stub 必须在桥建好共享段之后再起，而且桥活着时不要重启它**：slot 的 `frame_id` 是单调的，
  从 1 重新开始的 stub 对 `last_frame` 已经越过它的桥等于隐形（表现为该 slot 永远空转）。

## 7. 顺手记下的三处不一致（复核后的处置）

1. **已修**：`protocol/schema/state_update.schema.json` 的 `orders[].recipe` 改成
   `["string","null"]`。两端实现都接受 JSON `null`（`Link/StateFrame.cs` 对解析不出的菜谱就发 null，
   C 侧 `json_object_get_string` 明确接受 null，`protocol/protocol.md` §3 也说 null 表示缺省），
   所以该改的是 schema，不是代码。`holding` / `content` 早就是可空的，这三处现在一致了；
   其余非必填标量字段的 `null` 也按缺省处理，schema 还没逐个标注（README §3 已写明）。
2. **已修文档**：`protocol/protocol.md` §3.6 原先只说 `interact` 的 `target_object` 必填，容易被
   读成"插件按它挑目标"。交付插件 `ActionDriver` 处理 `interact` 时确实不读它（对厨师脚下的工位
   按键），契约表里现在写明了这一点；桥仍然总会带上，所以不炸。
3. **已记入契约与决策**：桥的存活判定**只看心跳**（`bridge/src/bridge.c`：`last_heartbeat_ms`
   只被心跳刷新），`state_update` 一直流也不算活着。桥默认 5000 ms，交付插件 `LinkRunner` 写死
   心跳 2000 ms，余量 3 秒；插件若把心跳配到 ≥5 s，就会边发状态边被判失联。`protocol/protocol.md` §3.2/§4
   原先写的"任何消息"已改成与两端实现一致，配对与余量记进
   `decisions.md`: 2026-10-15 - 链路存活只看心跳。
