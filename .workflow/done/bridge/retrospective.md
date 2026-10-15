# Retrospective: bridge

## Goal

让 KitchenOS 的 OS 核心能和 Windows 上跑的游戏说话：交付一个 Linux/WSL2 侧的
`kitchenos-bridge` 进程，独占与游戏的 TCP+JSON 链路，把世界状态写进共享内存、按帧汇总
Worker 的输入槽并下发动作，同时作为阶段 A 之后 Supervisor/Scheduler/Worker 的落脚点
（`.workflow/specs/bridge.md`）。

## What shipped

一条跑得通的链路，以及证明它跑得通的四层检查。用户可以：

- 起 `kitchenos-bridge --link listen`（默认 `0.0.0.0:12345`）或 `--link connect`
  （默认 dial 14455），两种拓扑一份协议；
- 用 `python3 tools/fake_plugin.py --scenario <name>` 在**不开游戏**的前提下跑协议级联调，
  `bash tools/run-bridge-scenarios.sh` 一条命令跑完 10 个场景；
- 用 `plugin/tools/conformance/` 把**插件自己的** `Messages.cs` + `TcpLink.cs` 编译成对端，
  让 C 侧对着真实插件代码受审（这是唯一一条不依赖游戏的独立跨端证据，其余跨端一致是自证的）；
- 用 `bash tools/run-real-game.sh` 一条命令完成唯一需要游戏的验收。

Spec 验收：**W1–W7 全部通过**（840 项 C 检查含 ASan/UBSan、10/10 场景、SIGTERM 0.0057 s
退出且端口立即可重绑、`/dev/shm` 无残留）；**W8 只做到一半** —— `hello` + 76 条
`state_update` + 0 解析错误全部达成，但"进关卡后共享内存里厨师/订单/资源非空"没有被观察到
（那次实机游戏停在菜单），残缺项与补齐方法写在 `done/bridge/05-real-game.md` 与
spec 的 W8 行里。

## Decisions that shaped it

- **Bridge 同时实现两种 TCP 拓扑** —— 文档四处说 Bridge 作服务端、两处说反，用户裁定两种都
  要；实机证明 listen 方向可用，connect 方向只被模拟对端覆盖。
- **帧屏障以 `state_update` 为帧边界，等待 Worker 而非等待插件** —— 文档既要求"阻塞等待"又
  要求崩溃时继续发 idle，有界等待是唯一能同时满足两者的形状。
- **OS 核心用 C + GNU make** —— 课程环境只有 gcc/make。
- **JSON 自己写最小解析器** —— 实验室机器不能假设能联网装包。
- **零长帧当作空消息交付，不当作流失步** —— 交付空帧、由解析器拒收，连接保持。
- **共享状态用 seqlock；就绪信号量另起名字** —— 文档允许 seqlock 或读写信号量，择一。
- **实机验收不要求改游戏侧配置** —— 两边默认端口对齐，零配置即可跑通；实机证实。
- **action 枚举扩到 5 个** —— 插件能执行 8 种动作，线上只表达了 3 种，`pickup`/`dash` 被
  静默降级；复查时用户选择补齐而不是收窄。
- **链路存活只看心跳** —— 课程文档 §5.3 的措辞是"超时未收到心跳"，代码照此实现；此前
  `protocol/protocol.md` 写反了，已改正。
- **插件由本 effort 补写** —— 见下"计划 vs 实际"第 1 条，这是本次复查翻出来最重要的一件事。

## Plan versus actual

- **任务：拆分时 5 个，后加 1 个，拆分 0 个，丢弃 0 个。** 后加的 `06` 是*补记*：它对应的
  2900 行（跨实现一致性对端 + 4 个加固场景）先落了地、没有任务档案，是本次 effort 级复查
  发现的，事后补写。
- **spec 在确认之后改过 2 次**：`c177055` 增加"验收结果"一节（只加结果，未改需求），以及本次
  关闭时的更新（W8 改成如实的"一半"、场景数 5→10、引用一致性对端的运行）。
- **没有任务走过 `blocked`。** 但 `05` 是带着一条未满足的 `Done when`（共享内存内容非空）
  落地的 —— 这条本该在落地时就摆到用户面前，实际是复查时才被点出来。
- **计划里最大的错误是前提本身。** 拆分时写的 spec 说"现在仓库里只有游戏侧插件"，而基线
  `b61b6f7` 里 `plugin/` 一个文件都没有（`git ls-tree -r b61b6f7 | grep -c '^plugin/'` = 0）：
  插件是这个 effort 自己写的（`a67ea3b`，挂在任务 05 名下，而 05 的 `Files:` 只写了
  `plugin/tools/`）。于是 `specs/bridge.md`、`protocol/protocol.md`、`protocol.h`、
  `decisions.md` 四处的"以插件**现有**实现为基准"全部落空 —— 跨端一致成了自证循环，唯一的
  独立证据只剩那次实机运行。计划里既没有这条转向的记录，也没有把"插件从零写"当成一件事排期。

## Review findings

本轮 effort 级复查（brief A，比较点 `b61b6f7`）是**这 5 个任务得到的第一次复查** ——
CONVENTIONS §7 要求的 `Review boundary` 行在 5 个任务的 `## Progress` 里一条都没有，这正是
下面这些能活到 HEAD 的原因。7 条 blocking / 7 条 nonblocking，按类别：

- **文档陈述与代码不符（4 条，同一个模式，也是本轮最大的类别）**
  1. 四处"以插件现有实现为基准"，而插件是本 effort 写的；
  2. `protocol/protocol.md` 说"没有任何消息即判定断开（不是只看心跳）"，代码只看心跳
     （错的是文档：课程文档 §5.3 写的就是"未收到心跳"）；
  3. `state_update.schema.json` 的 `recipe` 只允许 string，而代码发 `null`、README 也说 null 合法；
  4. `shm.h` 说动作名"Bridge 下发时可以直接用"，实际 `pickup`/`dash` 被静默降级成 idle。
- **记录缺失（2 条）**：最后 2900 行无任务档案；5 个任务无复查轮次记录。
- **接口两端对不上（1 条）**：插件 server 模式默认绑 12345，而 spec/文档/决策都说 14455；
  W7 因此只被模拟对端覆盖过。
- **过程**：`.gitattributes` 的仓库级改动落在 bridge 任务里，无记录。

## What changes

两条模式各变成一条标准（`.workflow/standards.md`）：

1. **检查必须双向证明过。** 只能通过的检查不算检查 —— 本轮三次栽在这里：W8 检查数的是
   DEBUG 级日志却没加 `--verbose`，于是**永远不可能通过**，一次满足全部条件的实机运行被报成
   失败；harness 把外部 SIGTERM 打在 `timeout` 上（退出码 124）误判成"场景超时"；一条软断言
   曾因为对端关闭的 socket 不 `recv` 就看不见而**为错误的理由通过**。三条都是"检查本身没被
   检查"。标准写法：新增或修改一条检查时，先拿已知坏掉的输入让它失败一次，再让它通过。
2. **关于代码行为的文档陈述必须指出哪条检查锁住它。** 写不出检查就写明"未验证" —— 文档说错
   话比没有文档更糟，因为它会被相信。本轮四条假陈述里，有两条已经被写进了同一个仓库的另一
   份文档（`conformance/README.md` 说对了心跳规则，`protocol/protocol.md` 说错了），说明这类
   错误靠"再读一遍"是发现不了的。

两条都属于判断类，因此落在 `standards.md` 而没有做成 lint。

## Debt left behind

`technical-debt.md`：**已清空。**

- **插件 server 模式默认端口与文档的 14455 不一致** —— 关闭后用户裁定按文档改代码：插件新增
  `Network.ServerPort`（默认 14455），server 模式用它、client 模式继续用 `Network.Port`
  （12345）。改动本身很小（一处配置 + 一处按模式取端口），但**它只做到静态对齐**：插件的
  server 模式仍然没有被任何一次运行覆盖过，所以 W7 那一行的措辞从"插件侧不满足"改成了
  "两个方向跑过的都是模拟对端"，没有把"默认值对了"说成"验过了"。
- 本轮修掉的"输入槽的动作词表比线上协议宽"已按约定在同一次改动里删除。

没有遗留的 hotfix follow-up 任务。

**未覆盖、也不算债务的两件事**（写在这里以免被当成已验证）：链路上**没有任何认证** ——
默认 `0.0.0.0:12345` 下能连上的局域网对端会成为被服务的插件并能向 `/kitchenos_state` 写入
伪造状态，未认证的 connect 对端可以让插件执行任意动作；单机课设可接受，已记入 spec。以及
**W8 的关卡内一半**：`chefs`/`orders` 非空从未被观察到，补齐只需进一次关卡 —— 但也因为插件
是本 effort 自己写的，那次实机连同一致性对端才是仅有的两条独立证据。
