# Glossary

What things in this project are called, and what those words mean.

One entry per concept. If two people would use the word differently, that is a
bug in the project, not in the conversation: fix it here.

Add a term the moment it is agreed, not later. A name that exists only in a
conversation is a name that will drift. Git merges this file with the union driver
(`.gitattributes`), so parallel branches can each add terms.

## Format

```markdown
- **Term**: what it means, in one sentence, ideally naming where it shows up in the
  code.
- _Avoid_: words that mean the same thing but should not be used (optional; omit if no synonyms exist).
```

The `Avoid` line does real work when it exists. "Order" and "Purchase" being two
names for one thing is a common source of confusion. But if a concept has no
competing names, omit the line rather than inventing fake synonyms.

## Terms

- **Bridge**: 唯一与游戏通信的进程；通过 TCP 收发 JSON，把世界状态写进共享内存，把 Worker
  算出的输入汇总后下发插件。它不解释调度策略。
- **世界状态（world state）**: 游戏当前局面（得分、时间、厨师、订单、厨具），由 Bridge 独占
  写、其他进程并发读，放在共享内存 `/kitchenos_state` 里。_Avoid_: 状态快照、快照（那是
  Scheduler 的 SIGUSR1 检查点，不是这个）。
- **输入槽（input slot）**: 共享内存里每个厨师一格的动作槽位，由对应 Worker 独占写、Bridge
  读。_Avoid_: 动作队列。
- **帧（frame）**: 一次世界状态更新所代表的时间片。Bridge 收到一个 `state_update` 就是一帧。
- **帧屏障（frame barrier）**: 一帧内 Bridge 等待全部输入槽就绪才下发动作；超时则对缺失的
  厨师下发 idle。_Avoid_: 帧同步（那是插件侧的阻塞应答行为，见 decisions.md:
  2026-10-15 - 帧屏障以 state_update 为帧边界）。
- **idle**: 让某个厨师不动的输入，用于 Scheduler/Worker 缺席时保持连接和局面不失控。
- **state_update**: 插件上报世界状态的 JSON 消息类型名，字段见 `protocol/`。
- **action**: Bridge（源自 Scheduler）下发给插件控制某个厨师的动作消息类型名。
- **分帧（framing）**: TCP 字节流上的消息边界：一行一条 UTF-8 JSON，`\n` 结尾，单行上限
  1 MiB（不含 `\n`）。两端必须完全一致，契约见 `protocol/protocol.md` §1，三个实现对
  `bridge/src/framing.c`、`plugin/WellCooked.Plugin/Net/TcpLink.cs`、`tools/fake_plugin.py`。
- **effort bridge**: 这一轮把 Bridge 进程及其共享内存、协议、无游戏联调工具做出来的工作单元。

- **订单（Order）**: 游戏里的一条菜谱需求，在系统里是作业（job），带到达时间、截止时间、预计工时和收益。
- **工序（Task）**: 订单展开后的一步（取料、切菜、烹饪、装盘、交付），是调度单位，有前驱依赖。
  - _Avoid_: 用"任务"同时指订单和工序；订单叫作业，工序叫任务。
- **厨具（Workstation）**: 砧板、锅、盘子、交付台等共享资源，分别映射为互斥锁、计数信号量、有界缓冲区。
- **世界状态（World state）**: 插件每帧上报的游戏状态快照（厨师、订单、厨具、得分），Bridge 写入共享内存供其他进程读。
- **输入槽（Input slot）**: 共享内存里每个厨师每帧的动作槽，Worker 写、Bridge 汇总后下发游戏。
- **Bridge**: Linux 侧唯一连接游戏的进程，TCP 服务端，负责状态转发和输入汇总。
- **Supervisor**: 守护进程，fork/exec 启动子进程、收心跳、按退避策略重启。
- **Scheduler**: 调度决策进程，维护订单队列和资源锁表，用 POSIX 消息队列派发任务。
- **Chef Worker**: 每个厨师一个进程，做动作分解（宏动作），把结果写进输入槽；路径由 `navd` 规划。
- **策略（Strategy）**: 订单级（FCFS/EDF/LLF/HRRN）或工序级（列表调度、关键路径优先）的
  调度算法。LLF 按松弛（截止期 − 现在 − 剩余工时）从紧到松，HRRN 按响应比
  `(等待 + 估计工时) / 估计工时` 从高到低；并列时都退化为 FCFS。
- **站位格（stand cell）**: 厨师操作某个工位时必须站的可走格，取工位每个方向（+x/-x/+z/-z）
  最近的一个，加载 `config/maps/<scene>/kitchen.json` 时算出，见
  `.workflow/specs/kitchen-control.md`。工位本身在台面里，没有自己的可走格。
  _Avoid_: 工位坐标（那是工位本身的位置，不可走）。
- **距离场（distance field）**: 从某个工位全部站位格出发的 BFS 步数表（`uint16`，不可达为
  `UINT16_MAX`），`navd` 用它给候选格排序，Scheduler 用它估路程代价；不含避让等待。
- **派工代价（assignment cost）**: 一个 (空闲厨师, 就绪工序) 配对的标量分数：路程与工时
  加成，剩余关键路径与订单紧迫度减分，分区模式再加跨区惩罚。多个空闲厨师用匈牙利算法取
  总代价最小的配对；各加权分量写进 `decisions.jsonl`。
- **洗盘工序（wash）**: Scheduler 在干净盘 ≤ 1 且有脏盘时合成的维护工序，不属于订单，和
  订单工序走同一个候选列表与资源租约。_Avoid_: 把 wash 当成一张订单（会污染订单级 metrics）。
- **navd**: Supervisor 下的走位进程，统一为所有厨师规划路径（PIBT，或对照基线 `pa`），
  通过共享内存导航表 `/kitchenos_nav` 与 Worker 交换目标和计划。
- **PIBT**: Priority Inheritance with Backtracking。每一步按优先级给每个厨师挑下一格，挡路的人
  继承优先级先让开，让不开就回退换格；换目标后步数 + 紧迫度是优先级，`pinned` 与已到站的
  目标厨师是固定障碍。_Avoid_: 死锁退让（PIBT 不靠事后检测死锁）。
- **执行依赖图（action dependency graph）**: `navd` 放行路点的依据：某格的下一位使用者要等
  上一位实际离开该格才放行，厨师快慢不一时也不会同格。
- **游戏实例（instance）**: 一个独立目录里启动的 Overcooked2 进程，配一棵 `--instance k` 的
  Linux 进程树，端口 `14455 + k`，IPC 名字带 `_<k>` 后缀。
- **宏动作（macro action）**: Worker 执行一道工序时的一步高层动作（goto/pickup/drop/chop/
  start_cook/wait_ready/wash/deliver），按世界状态判断完成，日志前缀 `macro_`。
  _Avoid_: 原子动作（那是线上的 `step/face/work/interact`）。
- **分区流水线（zoned pipeline）**: 每个厨师只在 `zones.json` 指定的区域内派工，跨区靠交接
  台面传递半成品的调度模式；与"灵活派工"对比。
