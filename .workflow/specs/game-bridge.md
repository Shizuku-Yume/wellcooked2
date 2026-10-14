# 游戏接入与 Bridge（阶段A）

**Status:** proposed
**Priority:** critical

## What this is for

调度系统要用游戏得分当唯一评价指标，就必须能读游戏状态、能控制游戏里的厨师。现在仓库里只有文档和汇报材料：游戏目录没有 BepInEx，没有插件，也没有 Linux 侧能和游戏对话的进程。缺了这一层，Supervisor、调度算法、策略对比实验全都无处落地，第6周的验收演示和占 60% 的答辩都拿不到分。方案 §9 把游戏接入定为项目基础并要求第3周跑通，现在已经到第4周。

## What it does

- 启动游戏后，游戏内的插件自动连上 Linux 侧的 Bridge；游戏重启后插件自己重连，不需要重启 Bridge。
- 游戏每帧上报世界状态：厨师位置与手持物、订单列表与剩余时间、厨具（锅、砧板、盘子）状态、当前得分。Bridge 解析后写日志。
- Bridge 能下发动作指令（移动到坐标、交互、抓取/放下、投掷），游戏里对应厨师按指令行动。
- 两名虚拟玩家能加入本地合作局，由指令驱动完成订单并正常得分。
- 任一侧断开，另一侧靠心跳超时发现并记日志，不静默失效。

## How it is built

四个部分，边界固定：

- **游戏插件（C#，BepInEx 5.4.x + Harmony 2）** — 跑在游戏进程内，负责状态读取、输入注入、TCP 客户端。它不做任何调度决策，只执行"某厨师做某动作"。工程放 `plugin/`，构建产物拷进游戏目录的 `BepInEx/plugins/`。
- **Bridge（C11，Linux）** — TCP 服务端，唯一连接游戏的进程。解析上报、写日志、回发控制指令；后续版本把世界状态写进共享内存供 Scheduler 和 Worker 读。源码放 `bridge/`。
- **协议（`protocol/`）** — JSON 行协议，四种消息 `state_update`、`action`、`heartbeat`、`config`，字段按方案 §5.2。schema、示例和字段含义落在 `protocol/protocol.md`，两侧各自实现序列化（方案 §5.3）。
- **部署脚本（`scripts/`）** — 安装 BepInEx 到游戏目录、构建插件、启动游戏和 Bridge 的可重复命令。游戏目录是外部依赖，仓库只放脚本和源码。

关键接口：Bridge 监听 `127.0.0.1:14455`（跨机时改绑定地址），插件作为客户端连接并在 2 秒后重试；一行一条 JSON，UTF-8，`\n` 结尾。

失败行为：心跳超时 5 秒判定对端断开；Bridge 记日志并继续监听，插件保留状态继续重连；JSON 解析失败只丢弃该行并记日志（方案 §5.3）。

平台差异只有地址和启动参数：Fedora 走 Proton 前缀内的同一个游戏目录，WSL2 侧插件跑在 Windows 原生游戏里，连 WSL2 的监听端口。代码同一份，不用条件编译（方案 §2.2.1）。

## What it must not do

- 不做调度决策。订单排序、任务分配、资源预订属于 Scheduler；插件和 Bridge 只搬运数据。
- 不用条件编译区分平台（方案 §2.2.1）。
- 不把离散事件模拟器当演示主平台，它只在阶段D 做策略预筛。
- 不提交游戏 DLL 和 BepInEx 二进制到仓库（方案 §11.6）。
- 不追求 Carnival 3-2 那种物理级复现（方案 §7.1）。第一版只要"能读状态、能控角色、能完成订单"。

## What was considered and rejected

- **插件作为 TCP 服务端、Bridge 主动连接**（方案 §6.1、§6.2 的写法）。最强理由：游戏可以随时重启而 Bridge 不必重启，Bridge 只做主动连接。否决原因：方案 §2.2.1 架构图和 §9 阶段B 都要求 Bridge 监听；WSL2 默认 localhostForwarding 让 Windows 侧直连 WSL2 的监听端口，反向要取宿主 IP 并处理防火墙；游戏由人手动启动，插件重连比维护监听生命周期更简单。
- **BepInEx 6（bleeding edge）**。它对新版 Unity 的 doorstop 有更新。否决原因：游戏是 Unity 2017.4.8f1 的老 Mono 运行时，社区插件和文档集中在 5.4.x，第4周不该赌版本。
- **Linux 原生 BepInEx 包**。否决原因：安装的是 Windows 版 `Overcooked2.exe`（Proton 运行），doorstop 必须注入 `winhttp`，只能用 `BepInEx_win_x64`。
- **不装插件、从外部进程读内存或模拟键盘**。否决原因：跨不过 WSL/Windows 边界，也拿不到结构化的订单和厨具状态，答辩时讲不出 IPC 设计。
- **只用 overcooked_ai 仿真环境做实验**。否决原因：评分表要求真实游戏画面，模拟器只留作策略预筛（方案 §8）。

## How we will know it works

- **W1.** 启动游戏到主菜单后，游戏目录 `BepInEx/LogOutput.log` 里有 BepInEx 启动行和 `WellCooked.Smoke` 插件加载行，没有 `Failed to load`。
- **W2.** 同一次启动的 `LogOutput.log` 里出现 smoke 插件 Harmony 补丁的日志行（证明补丁真的执行，不只是程序集加载成功）。
- **W3.** Bridge 没启动时启动游戏，插件日志显示重连；随后启动 Bridge，5 秒内建立连接（`bridge.log` 出现 `client connected`）。
- **W4.** 游戏里跑一局，`bridge.log` 收到 `state_update`，其中 `score` 与画面得分一致，`chefs` 数量与场上厨师数一致（人工比对）。
- **W5.** 通过 Bridge 发一条 `action` 让 0 号厨师移动到指定坐标，游戏里该厨师到达目标附近（一格以内）。
- **W6.** 两名虚拟玩家进入同一局，用固定动作序列完成 3 个订单，游戏得分为正。
- **W7.** `kill` 掉 Bridge，游戏不崩溃，插件继续重连；重启 Bridge 后连接恢复。

测试层次：W1、W2 只能靠日志证据（插件无法单测）；W3、W4 用脚本假客户端和真实游戏各跑一次；W5、W6 只能实测（游戏行为没有可断言的单元）；W7 用真实游戏加 `kill`。Bridge 侧的协议解析做单元测试（`tests/` 里放 `state_update` 样本，断言字段解析），因为协议错误最容易到联调才暴露。

坏掉的信号：`LogOutput.log` 不再出现插件行（插件没加载）；`bridge.log` 长时间没有 `state_update`（插件崩了或断连）；游戏里角色不动（输入注入失效）。

## Open questions

- **第一版固定地图与订单集。** 用户 2026-10-14 决定暂缓。由 A1 验证时列出的可用关卡和与组员确认来定，默认 Story 1-1（方案 §12 建议：无传送带、无移动平台）。影响任务 05 的验收地图和阶段D 的实验输入。
- **WSL2 ↔ Windows 的连通方案。** 由组员在 Windows 侧验证：先试 localhost，再试 WSL2 IP 加防火墙规则（方案 §2.2.1）。只影响实验室乙的启动参数，不影响代码。
- **输入注入的具体路径。** 待任务 04 期间确定：Harmony 改游戏输入层，还是模拟手柄设备。游戏用 InControl 加 XInputDotNetPure，两条路都可能，代价不同。

## Notes

- 方案 §9 阶段A（A1–A4）是本 effort 的主体；§6.1 Bridge 职责、§5.2 协议、§5.3 联调顺序是接口依据。
- 任务文件：`.workflow/tasks/game-bridge/`。
- 阶段B（Supervisor、Scheduler、Worker 骨架）是另一个 effort，等本 effort 跑通状态读取后开始。
- 相关决策：`decisions.md: 2026-10-14 - TCP 连接由 Bridge 监听，游戏插件作为客户端`、`decisions.md: 2026-10-14 - OS 核心用 C11`。
