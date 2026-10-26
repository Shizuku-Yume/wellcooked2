# 厨房控制：多厨师走位、宏动作与调度代价

**Status:** in progress
**Priority:** high

## What this is for

os-core 已经把 Supervisor / Scheduler / Worker 进程树、消息队列、FCFS/EDF 和列表调度
做出来了，但在四张自制地图（`s_wellcooked_demo_1..4`）上，系统控制的厨师还完成不了一单：

- Worker 的 A*（`nav`）只管单个厨师，不知道别的厨师在哪。两个厨师在 1 格宽的过道相遇会
  互相顶住；`_3` 是环形厨房，四个厨师同时走就会堵死。插件 `Navigation.cs` 收到的 `move`
  目标是终点，它会再规划一遍，自己判断"卡住"，所以 Worker 和插件各管一段，谁都看不到全貌
  （game-bridge/13 记录的侧向错身/卡住就是这个）。
- `worker_task` 用内部速度 3000 mm/s 推算位置，从不读 shm 里厨师的实际坐标；`WORKING`
  阶段只是计时，切菜没有对应的按键（协议只有 move/stop/interact/pickup/dash，切菜要按住
  加工键），交互前也不转向工位。
- Scheduler 选工序时不看路程：两个空闲厨师谁离料箱近都一样，锅也不会被优先启动。
  `_4` 炸台在 x=-11、砧板在 x=29，路程占了大半工时。
- 策略对比只能在实机上一局一局跑：一次只开一个游戏，`BepInEx/` 下的日志、状态、指令文件和
  端口、mq/shm 名字都只有一份，课程要求的批量实验（方案 §4.4、§6.5）攒不出样本。

不做这些，os-core 的 W10（实机两个厨师完成一单）和 game-bridge/05 一直卡着，四张地图
也无法用于演示调度策略的差别。

## What it does

- 四张地图都能交给系统控制：不用手动输入，四个厨师自己取料、切、煮、装盘、交付。
- 厨师在过道里错身或排队，不会顶住不动；`_3` 环形过道里四个厨师同时走全部到达，不靠超时
  兜底。走位是一个中央进程 `navd` 统一规划的：每一步用 PIBT 决定所有厨师的下一格，被挡路的
  人继承优先级让开，执行时按依赖图放行，厨师连续走、不一格一停。
- 走位算法可以切换：PIBT 与优先级时空 A*（对照基线）在同一订单序列下的等待步数和得分并列进
  报告，和调度策略一样能拿来比较。
- 一个厨师卡住（实际坐标 2 s 没动），`navd` 给他重规划；连续 3 次仍不动，Worker 向 Scheduler
  报 `BLOCKED`，Scheduler 把这道工序改派给别人。
- Scheduler 派工时把路程算进代价：离得近的厨师先接活，锅优先开火，脏盘有人洗。订单级
  策略在 FCFS/EDF 之外多了 LLF 和 HRRN，`SIGHUP` 切换方式不变。
- 可以选两种派工模式：灵活（谁空谁干）或分区流水线（`_2` 左半区备料、右半区烹饪，在
  交界的台面上交接），两种模式在同一张地图上的得分可以直接比较。
- 一台机器同时开 3 个小窗口（640×360）的 Overcooked2，每个实例有自己的 `BepInEx/` 目录、端口
  和一整棵 Supervisor 进程树，互不串扰；`tools/batch.py` 把一组（地图, 策略, 订单序列）分给各
  实例跑完整局，结果汇成一张表。实验数据全部来自真游戏，不另写模拟器。
- 每张地图都有一个离线上界（已知全部订单时用束搜索求出，纯计算、不开游戏），报告里写"在线
  策略达到上界的百分之多少"。

## How it is built

本 effort 叠在 os-core 之上（`.workflow/specs/os-core.md`）：进程树、mq、租约、FCFS/EDF、
列表调度都不重做；新增走位进程 `navd`，换掉 Worker 的执行，给 Scheduler 加代价模型，再让
游戏能多开。分三层，每层只依赖下一层的契约：

| 层 | 在哪 | 负责 | 不负责 |
|---|---|---|---|
| L1 走位 | `navd`（新进程）+ Worker 跟随 | 站位格、PIBT 规划、执行依赖图放行、卡住重规划 | 选工序 |
| L2 宏动作 | Worker（取代 `worker_task`） | 把一道工序拆成走到/转向/按键/等待，按世界状态判完成 | 路径、优先级 |
| L3 调度 | Scheduler（`assign`、`policy`） | 订单策略、(厨师, 工序) 代价、锅/盘子/洗盘 | 格子、按键 |

插件只剩执行：沿给定路点连续走、转向工位、按键、按住加工键。它不再为系统下发的移动做 A*。
路径规划在 Linux 侧的 `navd`，Scheduler 仍只派高层目标，见 decisions.md: 2026-10-25 -
走位改由中央 navd 进程用 PIBT 规划。

### 每张地图的配置

`config/maps/<scene>/` 放 `kitchen.json`、`recipes.json`、`schedule.json`、`zones.json`，
四张地图各一份。前两个用 `scripts/export-kitchen.py --map-dir config/maps` 从实机导出，导出
结果为准（README §6 的工位表与 `layout_*.tsv` 有出入，不手抄）。`schedule.json` 是调度代价
权重（L3），Scheduler 从 `kitchen.json` 同目录读，任务 11 的网格搜索写回选定值。
`zones.json` 手写，只给分区流水线用。Scheduler 与 Worker 沿用现有的 kitchen/recipes 路径开关。
`grid.areas.count` 可以大于 1（`_2` 是两条互不相通的厨房条带），导航和距离场按连通块处理，
不要把多个区域当成一块。

`_2` 的两条带不是可由导航算法跨越的“软分区”：两条带被 z=-6 一排台面隔开，`layout_2.tsv`
每条带各有 2 个出生点（下带 (4,-12)/(12,-12)，上带 (4,-2)/(11,-1)），只有 4 个玩家位都占上，
两条带才都有厨师。只占 2 个位时两人都出生在下带（实机 `logs/real-game/20261026-110357-_2-finalfresh`），
上带的 Rice、Tortilla、锅与出菜口没人能到，Scheduler 记录 `task_station_unreachable` 并拒绝派发
是正确行为。跨带只能靠台面交接（分区流水线的 `drop`/`fetch`），不得用改 mask、假传送或放宽距离场伪造连通。

### L1 走位

**站位格表与距离场（`stand`）。** 加载 `kitchen.json` 时为每个工位算出站位格：在 +x/-x/+z/-z
四个方向上各取离工位最近、且垂直偏移不超过半格的可走格（工位在台面/墙的几何中心，不在
`grid.cells` 里；四张地图实测都是 1.2 m，即两格，正好是游戏 1.2 m 交互格的一步）。每个工位
最多 4 个，按到工位的距离排序；每个站位格带朝向（+x/-x/+z/-z）。工位那一格不可走，所以
"朝向工位的 mask 位"必为 0；若它其实可走，那个格会被选成更近的站位格——测试按这条双向
检查站位格与网格一致。没有站位格的工位在加载时报错并带工位 id，不留到运行时。
同一模块算距离场：以一个工位的全部站位格为多源做 BFS（`uint16`，64 工位 × 2048 格 ≈
256 KB，不可达记 `UINT16_MAX`）。本任务只提供接口；`navd` 在任务 04 用它给候选格排序，
Scheduler 在任务 07 用它估路程。厨师走到该工位任一站位格都算到达。
见 decisions.md: 2026-10-25 - 站位格取工位每个方向最近的可走格。

**导航表（`/kitchenos_nav`）。** 一块独立共享内存（不挤进 16 KB 的世界状态段），由 Supervisor
创建与 unlink，命名信号量 `/kitchenos_nav_lock` 做互斥。默认名字就是上面两个；Supervisor 在
`--state-name` 不是默认值时派生成 `<state-name>_nav`/`<state-name>_nav_lock`，同机多个场景/实例
各有一份。每个厨师两个槽，各只有一个写者：

- 目标槽（Worker 写）：目标工位、目标序号、紧迫度（Scheduler 随任务下发的订单排名）、
  `pinned`（正在工位前干活，不能被推开）。
- 计划槽（`navd` 写）：已放行的路点（≤ 8 个格中心）、计划序号、状态
  `moving/arrived/stuck/blocked`、`navd` 写入时的游戏时钟。

**PIBT 规划。** `navd` 每收到一版新的世界状态就推进一次。它为每个厨师维护一条从实际所在格
往前最多 H = 8 步的计划；计划不够长时，在计划末端的位形上跑一步 PIBT（Okumura 等，2022）：

1. 厨师按优先级排序：离上次换目标经过的步数 + 紧迫度，并列取 `chef_id` 小的。
2. 从优先级最高的开始，每人在"四个邻格 + 原地"里按距离场从近到远挑下一格，跳过这一步已被
   别人占的格，也跳过和别人对换位置的格。距离场按"固定障碍"重算：`pinned` 的厨师与已经到站
   的目标厨师占着的格不可通过，否则最近的邻格被挡时会在两格之间来回走。
3. 挑中的格上站着还没决定的人，那人继承当前优先级，必须先给自己找到下一格（递归）；他找
   不到，就回退，换下一个候选格。所有候选都不行时原地不动。
4. `pinned` 的厨师当障碍处理，不参与推人。已到站的目标厨师优先级清零并当固定障碍（它的
   Worker 读到 `arrived` 后也会把 `pinned` 置 1）；没任务的厨师目标就是自己所在格，可以被
   推开。见 decisions.md: 2026-10-25 - 已到站的目标厨师在 PIBT 里当固定障碍。

论文的保证是：图上每对相邻格都在某个长度 ≥ 3 的简单环上时，每个厨师都会在有限步内到达
目标。`_3` 的环形过道满足；死胡同里的站位格和被 `pinned` 厨师截断的过道不满足，这些地方
由下面的卡住重规划兜底。一步 PIBT 对 4 个厨师是 O(4 × 5) 次查表，微秒级。

**执行依赖图放行。** 计划按逻辑步排，但厨师的实际速度会有快慢，所以 `navd` 不按时间放行，
而按依赖图（Hönig 等，2019 的 action dependency graph）：对每个格记下计划里先后经过它的人，
厨师 A 的某个路点要同时满足两件事才放行：计划里更早经过该格的人已经执行到那一步，并且他
现在已经不在那格（实际坐标所在格变了）。这样不论谁走得快，都不会有两个人同时进同一格，慢
的人也不会被快的人从后面越过计划顺序。`navd` 把每个厨师已执行到第几个路点按实际坐标对上
（离路点 ≤ 0.3 m 算到过），执行进度不靠推算。

**跟随。** Worker 读自己的计划槽，计划序号变了就发一条 `path`，把已放行的路点交给插件；插件
沿路点连续走，转角处偏离 ≤ 0.15 m，走到最后一个已放行路点就停下等续放。

**卡住与崩溃。** 有已放行路点但游戏时钟 2 s 内坐标没动，算卡住：`navd` 丢掉这人没执行的
计划并重规划；连续 3 次，计划槽状态写 `blocked`，Worker 向 Scheduler 报 `BLOCKED`（`detail`
写原因），Scheduler 改派：先把该工序放回就绪队列并暂时扣住报 blocked 的厨师，等它派给
另一个厨师后再放这个厨师接新活。游戏暂停时时钟不走，不判卡住。`navd` 崩溃重启后只从世界状态和
目标槽重建计划，计划序号继续加一；计划槽的写入时钟落后 500 ms 时 Worker 发 `stop`，等
`navd` 回来。Worker 重启后在 RECOVERED 前先清空自己的目标槽，期间它的厨师当作没任务的人。

**对照基线：优先级时空 A\*。** `navd --nav pa` 换成优先级规划：按同样的优先级依次做时空 A\*
（4 邻域 + 原地等待），避开前面的人在 `navd` 内存预约表里的格子与对换，放行仍走依赖图。
预约表只是 `navd` 进程内存，不新增共享内存。启动参数选择（Supervisor 的 `--nav` 原样传给
`navd`，日志 `nav_start ... nav=pibt|pa` 固定），一局内不切换。两种算法都把每个厨师的
总步数、等待步数、重规划次数、卡住次数和一次规划推进耗时（一版世界状态从重建计划到写出
计划槽，p50/p99）写进**进程树运行目录**（`--runtime-dir`，与 `logs/`、`runs/` 同级）的
`nav_metrics.json`，每局一棵进程树一份，报告里并列比较。步数/等待步数只数已执行的计划步
（等待 = 计划里原地停留的一步），不是每个世界状态重算一次；卡住数 = `nav_stuck` latch
次数。`pa` 的计划终点必须是后续时刻没人使用的格，找不到安全终点时本次规划失败
（`nav_pa_failed`）而不是产出冲突路径；它是保守基线，不承担 PIBT 的可达性保证。

**插件动作契约。** 动作种类从 5 个加到 8 个，`shm.h` 的 `kos_action_kind`、
`protocol/schema/action.schema.json`、插件 `ActionDriver` 与 `tools/fake_plugin.py` 同一次改；
`kos_action` 加一个 8 项路点数组和路点数，shm 版本号加一：

| kind | 字段 | 插件做什么 | 结束 |
|---|---|---|---|
| `path` | `waypoints` = 1..8 个相邻格中心，`seq` | 沿路点连续走，不寻路；同 `seq` 的新消息只追加路点 | 到最后一个路点 0.15 m 内停下，回 ack（带 `seq`） |
| `face` | `target_object` 或 `target` | 原地转向 | 朝向误差 < 15° 回 ack |
| `work` | `target_object` | 按住加工键（切菜/洗盘） | 收到下一条动作才松开 |

原 `move`（插件内 A*）保留给手动调试与旧场景，系统控制路径不再发它。

`state_update` 同时补三项事实，L2 的完成条件要读（现在 `StateReader` 的砧板只报
`free/occupied`，盘子只报干净盘数）：砧板/工作台的 `content` 与 `progress`（0..1，切到哪一段）；
`plate_stack` 之外新增 `dirty_plates`（脏盘台上的数量）与水槽的 `progress`；厨师的 `facing`
（弧度）。shm 的 `kos_resource` 已有 `content`，新增 `progress` 浮点；`kos_chef` 新增 `facing`。

**时钟。** Linux 侧的计时（规划步、卡住、超时、截止期）一律用世界状态里的游戏时钟，不用
墙钟：世界状态加 `game_time_ms`（`int64`，现有 `game_time` 是整秒，不够用），shm 版本号加一。
游戏暂停时时钟不走，不会误判卡住或超时。见 decisions.md: 2026-10-25 - Linux 侧计时统一用
游戏时钟。

### L2 宏动作

Scheduler 仍按 os-core 的 `kos_mq_msg` 派一道工序（`op` + `station_id` + `resource`），
Worker 把它展开成宏动作序列，逐个执行；完成以世界状态为准，不以计时为准：

| 宏动作 | 动作 | 完成条件（读 shm） | 超时 |
|---|---|---|---|
| `goto(station)` | 写目标槽，等计划槽 `arrived` + `face` | 计划槽 arrived 且世界状态 `facing` 与工位方向误差 < 15°（插件只在这个误差内回 ack；ack 不下发 Worker） | 按路程 ×2 + 3 s |
| `pickup` | `interact` | `holding` 变成目标物品 | 1.5 s |
| `drop` | `interact` | `holding` 变空 | 1.5 s |
| `chop` | `work` | 砧板 `progress` 到 1 | 工时 + 3 s |
| `start_cook` | `interact` | 锅的 `state` 变成 `cooking` | 1.5 s |
| `wait_ready` | `stop`（原地或离开） | 锅的 `state` 变成 `cooked` | 工时 + 5 s |
| `wash` | `work` | 水槽 `progress` 到 1 且干净盘数加一（没有 `plate_stack` 可读时只认 progress） | 工时 + 3 s |
| `deliver` | `interact` | 该订单从世界状态订单表消失 | 1.5 s |

Worker keeps each interaction intent alive for 1000 ms so the plugin can issue its immediate A
pulse and the retry after 600 ms (held for 250 ms). The interaction still completes only when
the corresponding world-state condition changes; an input acknowledgement is not proof of pickup.

Navigation arrival and station interaction are separate geometric checks: a `path` endpoint is
acknowledged only when the chef's actual world coordinates are within 0.3 m of the final navd cell
waypoint; the nearest reachable stand cell for every station must itself be within 1.2 m of that
station. Never replace the navd cell target with the station center or widen either bound to mask a
mapping error.

工序到宏动作的映射是 Worker 里的一张表：`fetch` = goto(料箱) + pickup，`cut` = goto(砧板) +
drop + chop + pickup，`cook` = goto(锅) + drop/start_cook（启动成功后放下就走，但 Scheduler 将该
订单的锅令牌与工位归属保留给其 `plate` 工序），`plate` = goto(干净盘堆) + pickup(Plate) +
goto(本订单cook预留锅) + wait_ready + pickup(菜)，`deliver` = goto(出菜口) + deliver，
`wash` = goto(水槽) + drop + wash。cook 完成事件先结束 Worker 对该工序的租约，再把锅令牌
转为订单级预留；预留状态保存在 Scheduler 现有订单/task 内存记录，不新增持久化层。plate 的
成功完成、FAILED、订单取消/淘汰时，Scheduler 释放对应锅令牌并清除预留；在 plate 完成前，派工
不得把该物理锅分给其他订单。Scheduler 进程重启会重新创建资源信号量、丢弃内存订单与预留，
现有 IPC 不提供任务/锅归属恢复，故重启后的恢复保证仅限 Supervisor/Worker 既有重排语义，
不声称可恢复在制锅订单。导出配置里还有 `mix`（与 `cut` 同样是按住加工键到 progress=1）与
`assemble`（goto 容器 + drop），Worker 一并支持；未知工序直接报 `FAILED`，`detail` 写
`macro-map`。宏动作超时或完成条件被打破，工序报 `FAILED`，`detail` 写宏动作与所等条件；
Scheduler 已有的重排队逻辑接手。每个宏动作的开始与结束写进 Worker 日志（`macro_start` /
`macro_done` / `macro_failed`），`macro_done` 带游戏时钟的 `waited_ms`。

**盘子配置边界：** plate 工序必须同时得到可达的干净盘工位与本订单 cook 预留锅。当前
`s_wellcooked_demo_1` 没有导出的 `plate_stack` 工位，且洗盘产物的世界状态只报告计数、不报告
可拿取位置；在补充可验证的干净盘来源位置前，该地图无法满足 plate 宏动作，不能把计数虚构为
空间工位。其它地图使用配置中的 `plate_stack` 工位。

game-bridge 插件里 `WorkOrder.cs` 那套在插件内选厨师、走工序的替身逻辑，在系统控制下不再
启用；它的 `_looseApproach` 1.4 m 兜底和"按住 X + 超时"切菜由上面的站位格与 `chop` 完成条件
取代。

### L3 调度

订单级策略接口（`policy.h`）不变，新增 LLF（松弛 = 截止期 − 现在 − 剩余关键路径工时）与
HRRN（(等待 + 估计工时) / 估计工时）；`--strategy` 与 `SIGHUP` 都认 `fcfs/edf/llf/hrrn`
四个名字，SIGHUP 按 FCFS → EDF → LLF → HRRN → FCFS 轮换。四种策略主键并列时都退化为 FCFS
（到达时间，再按下标），同一输入永远挑出同一个订单。订单视图里的 `est_cost_ms` 由 Scheduler
填"从该订单现在这道工序到做完还要多久"：未开工订单取菜谱关键路径，已开工订单取未完成节点的
最长剩余路径。`now` 目前由 Scheduler 的单调时钟提供；任务 02 的 `game_time_ms` 落地后只改
Scheduler 取时的那一处（策略接口仍收一个 `now_ms`），LLF/HRRN 与截止期就一起改用游戏时钟。

小费倍率要求按顺序交付，所以策略只决定"下一单开哪个"：未开工订单的工序不进候选；更早开工的
订单没做完时，后面订单的 `deliver` 不进就绪队列。开工窗口不预先固定单数：已开工订单的就绪
工序不够填满空闲厨师时，按策略一单一单往下开。这样进程空闲时不会因为策略挑中的那一单暂时
没有就绪工序而空转，也不会把所有订单同时铺开。

工序级仍是事件驱动的列表调度，os-core 的就绪队列与关键路径保留；变的是"谁接哪道"。
新模块 `assign` 在每次有厨师空闲时，对 (空闲厨师, 就绪工序) 算代价：

```text
cost = w_d * 路程秒(距离场, 厨师当前格 → 工位最近站位格)
     + w_t * 工时秒
     - w_c * 该工序到订单完成的剩余关键路径秒
     - w_u * 订单紧迫度(策略给的排名, 0..1)
     + w_z * 跨区惩罚(仅分区模式, 厨师不在工位所在区时为 1)
```

同时有多个空闲厨师时，厨师 × 工序 ≤ 8 × 32，用匈牙利算法求总代价最小的配对（O(n³)，
微秒级）。矩形版本：厨师多于工序时先转置，保证每道工序都有人接、多出的厨师空闲；并列取
下标小的配对。候选在算代价前按资源 kind 预扣本轮可用令牌，同一轮不会把两个厨师派给只剩
一个令牌的资源。路程用任务 03 的距离场（`kos_stand_distance`），厨师坐标取世界状态的格子，
不读导航表、不算避让等待。

权重从 `config/maps/<scene>/schedule.json`（`kitchen.json` 同目录）读，缺省值：

| 键 | 缺省 | 含义 |
|---|---|---|
| `w_d` | 1.0 | 每秒路程（距离场步数 × `cell_ms`） |
| `w_t` | 0.2 | 每秒工时 |
| `w_c` | 0.5 | 每秒剩余关键路径 |
| `w_u` | 2.0 | 每单位订单紧迫度（策略排名映射到 0..1，1 最紧急） |
| `w_z` | 1.0 | 跨区惩罚（分区模式 1，灵活模式 0） |
| `cell_ms` | 107 | 每格移动毫秒（任务 01 在 `_2` 实测 0.107 s/格） |
| `wash_ms` | 3000 | 生成的 `wash` 工序工时 |

没有 `schedule.json` 时用上表；文件存在但解析失败时 Scheduler 记 `schedule_config_failed`
并退回缺省值。任务 11 的网格搜索把选定值写回四张地图的这份配置。

`decisions.jsonl` 的 `w_d/w_t/w_c/w_u/w_z` 是**带符号的加权分量**，`cost` 正好是五项之和；
每条决策都写这五个键（`order_open`、`strategy` 等非派工决策为 0），分析脚本不用按 kind
分支。派工行另写 `travel_cells`、`travel_s`、`work_s`、`critical_s`、`urgency`、`zone`、
`weight_*`、`station`，公式里的每个输入都能从日志复算。

资源规则：

- **锅先启动。** 锅是长工时、无人值守的设备（方案 §4.3 的 I/O 设备类比）：`cook` 及其全部
  前置（沿菜谱边反向可达的节点，加载时标记）的 `w_c` 自动乘 2，让锅尽早开火；放下即走，
  Worker 不在锅前等。
- **盘子是有界缓冲。** 干净盘数是世界状态里 `plate_stack` 的 `count` 之和（os-core 的命名
  信号量照旧），脏盘数是 `dirty_plates.count`；出菜后游戏延时送回脏盘，与水槽构成生产者—
  消费者。干净盘 ≤ 1、有脏盘、当前没有在跑的 `wash` 时，Scheduler 生成一道 `wash` 工序。
  它不属于任何订单，是合成的维护工序（`op=wash`、`resource` 取水槽工位的 kind、
  `duration_ms=wash_ms`），和订单工序走同一个候选列表、匈牙利配对与资源租约，不另起队列，
  也不进订单级 metrics；同一时刻最多一道，干净盘恢复、Worker 崩溃或租约过期时丢弃，下一轮
  按计数重新生成。水槽工位按 kind 与 `sink` 互相包含识别。`dirty_plates` 由任务 02 上报、
  wash 的宏动作由任务 06 实现；两者落地前基础分支没有脏盘字段，实机不会生成可执行的 wash。
  `_1` 没有盘子堆，盘子全靠洗。
- **台面交接。** 分区模式下跨区的中间产物放在 `zones.json` 指定的交接台面上，生成一对
  `drop`/`fetch` 工序；灵活模式不生成。

### 多实例批跑

实测（2026-10-25，本机 16 核 / 30 GB / RTX 4060 8 GB）：不经 Steam 的 applaunch，直接用
`Proton - Experimental/proton run ./Overcooked2.exe` 起游戏，两个进程能同时跑，各自加载了
BepInEx 和插件，每个约 1 GB 内存、2 核 CPU、1.3 GB 显存。把 exe、`UnityPlayer.dll`、
`steam_api.dll`、doorstop 与 `BepInEx/` 复制到一个单独目录、`Overcooked2_Data` 用符号链接
（每个实例 59 MB），从那个目录启动也能加载插件，日志和状态文件写在该目录自己的 `BepInEx/`
下。没解决的一点：`-screen-width 640 -screen-height 360 -screen-fullscreen 0` 被前缀注册表
`Software\Team17\Overcooked2` 里的全屏与分辨率记录盖掉，窗口仍是 4096×2304。

2026-10-25 任务 08 实测：上面那条“没解决的一点”在任务 08 已解决——插件在进关前按
`window=640x360` 调 `Screen.SetResolution(w,h,false)`，3 个实例的日志都出现
`takeover: 窗口 = 640x360（全屏=False）`，不需要改前缀注册表。同时 3 个 640×360 实例
可同时运行（GPU 7.5/8.2 GB、可用内存约 2 GB，余量很小）；3 实例连跑 10 局全部成功、
无实例掉线，默认 N 定为 3。任务 08 基座上 Scheduler/Worker 能把任务链走完但游戏不认交付，
同局 score 为负（-300/-390），全尺寸与小窗口同局得分相同；正分等任务 09 的宏动作与闭环。

据此 `tools/instances.py` 管理 N 个实例（默认 3，按本机内存定）：

- **目录。** `~/.cache/wellcooked/inst/<k>/`，按上面的方式从游戏目录生成，插件与配置从仓库
  的构建产物复制。游戏目录本身不改。
- **分辨率。** 插件启动时读 `wellcooked-takeover.txt` 的 `window=640x360`，调
  `Screen.SetResolution(w, h, false)`，不依赖命令行与注册表。窗口按实例号平铺。
- **端口与 IPC 名。** 实例 k 的插件连 `14455 + k`；Linux 侧每个实例一整棵进程树，Supervisor
  带 `--instance k` 启动：mq、shm、信号量、导航表与资源信号量的名字都加 `_<k>` 后缀，运行目录
  与日志目录按 k 分开。`--instance` 不给时名字与今天一样，os-core 的检查不受影响。
- **共享的东西。** 所有实例共用一个 Proton 前缀和存档；关卡用插件的 `load=` 指令直接进，
  不经过存档里的进度。

`tools/batch.py` 读一个实验清单（每行：地图、订单策略、走位算法、派工模式、订单序列、种子），
把它们排进空闲实例，每局跑完整时长，从每棵进程树的 `metrics.json` / `nav_metrics.json` 和
插件上报的最终得分汇成 `results.csv`；一个实例连续两局没出 `state_update` 就重启它，那一局
记失败重排，不静默丢掉。窗口小、画面低，不影响游戏逻辑与计分 [推断：逻辑帧与渲染分辨率无关，
由任务 08 的同局对照验证]。

离线上界（`tools/opt.py`）不开游戏：读 `config/maps/<scene>/` 和订单序列，已知全部订单，对派工
顺序做束搜索（宽度默认 64），路程取距离场、不算避让等待，工时取 `recipes.json`。少了等待只会
让上界更松，不会低于真实最优。

## What it must not do

- 不做强化学习：方案 §3.3 把它列在第一阶段之外，四张地图上可解释的启发式已经够比较策略。
- 不做 CBS/RHCR 之类的窗口内最优多智能体寻路：最多 4 个厨师，PIBT 已有可达性保证（理由见下）。
- 插件不为系统下发的移动寻路，不判卡住，不选厨师；它只执行 `path/face/work/interact`。
- 不改 os-core 的 mq 名字与消息布局、资源租约与重启语义；进程树只多一个 `navd` 子进程，
  IPC 名字只在给了 `--instance` 时加后缀。
- Scheduler 不认识格子、不碰导航表；它只用距离场估路程。Worker 不规划路径，只写目标、读计划。
- 不写离散事件模拟器：实验数据来自多开的真游戏。
- 不手抄 README 里的工位表；工位以 `export-kitchen.py` 从实机导出的为准。

## What was considered and rejected

- **避让放在插件 `Navigation.cs`。** 插件每帧都知道所有厨师的精确位置，局部避让（谁让谁）
  延迟最低，而且已有一套 A*。被否掉（用户决定）：调度与寻路是课程要展示的 Linux 侧机制；
  放进插件，Linux 侧的路径和游戏里实际走的路就是两份，game-bridge/13 的"卡住"正是两边各算
  一遍造成的。
- **每个 Worker 各自做时空 A\* + 共享预约表（原方案）。** 不加进程，符合"路径放 Worker"。
  被否掉（用户决定）：后规划的人可能无路可走，`_3` 环形上会互等成环，只能靠死锁检测加退让
  兜底；逐格 `step` 让厨师一顿一顿。保留为 `navd --nav pa` 对照基线。见 decisions.md:
  2026-10-25 - 走位改由中央 navd 进程用 PIBT 规划。
- **Worker 内窗口化协作 A\*（WHCA\*）。** 不改架构，滚动重规划、优先级轮换。被否掉：环形图
  上仍没有可达性保证，还是要死锁兜底。
- **RHCR（窗口内 PBS/ECBS）。** 解的质量最好。被否掉：实现最复杂，最坏指数级，每次有人卡住
  或换任务都要重算窗口，会挤占调度策略的时间；4 个厨师下 PIBT 的等待步数已经少。
- **Scheduler 兼做路径规划。** 少一个进程。被否掉：Scheduler 是事件驱动的，走位要跟着每版
  世界状态推进；把两种节奏塞进一个循环，派工决策会被规划耽误，Scheduler 崩溃时厨师也一起停。
- **8 邻域。** 路径更短。被否掉：`Navigation.cs` 实测斜向输入会让厨师原地卡住（4 名厨师
  12 秒坐标不变），而且斜向会切过两个格子，依赖图管不住。
- **自写离散事件模拟器做批量实验（原方案）。** 无头、能快进。被否掉（用户决定）：要再建一遍
  锅、盘子、订单和计分的模型，还要单独校准精度；实测本机能同时开 3 个游戏实例，直接用真游戏
  批跑，实验数据和演示是同一份代码、同一个游戏。见 decisions.md: 2026-10-25 - 批量实验用多开
  游戏实例，不写模拟器。
- **多实例共用一个 BepInEx 目录、只改端口。** 不用复制目录。被否掉：实测第二个实例启动时把
  第一个的 `LogOutput.log` 轮转成 `.log.1`，状态、指令、接管文件也只有一份，会互相覆盖。
- **继续用墙钟计时。** 不用改协议。被否掉：游戏暂停时会误判卡住、截止期会被提前判超时。
  见 decisions.md: 2026-10-25 - Linux 侧计时统一用游戏时钟。

## How we will know it works

C 部分是 `cd bridge && make test` 的单元检查；多进程行为用 `tools/fake_plugin.py` 的新场景
（无游戏，能在 CI 跑）；标"需要游戏"的在实机或多实例上跑。

- **W1. 站位格表。** 对 `config/maps/` 下四张地图的 `kitchen.json`，每个工位至少 1 个站位格，
  最多 4 个、按距离排序；每个站位格可走，朝向是工位实际所在的一侧；朝工位方向的相邻格可走时
  它的 `mask` 位必须为 1、不可走时必须为 0；距离场在站位格上为 0、每个非 0 格有 `dist-1` 的
  父格、相邻可达格差 ≤ 1，跨连通区为不可达。没有站位格的工位让加载失败并报出工位 id。
- **W2. 规划无冲突且能到达。** 单元检查：四张地图各 100 组随机起终点、2–4 个厨师，PIBT 与
  `pa` 各跑：任一步没有两人同格、没有两人对换，每步满足 `mask`；同一输入两次结果相同。
  `_3` 上 PIBT 每组都在 200 步内全部到达；`pa` 只检查无冲突与确定性（保守停靠可能让它在
  200 步内没走完，不作为失败）。依赖图单元检查：随机打乱各厨师的执行快慢，放行序列下任何
  时刻都没有两人在同一格。
- **W3. 窄道错身。** fake_plugin 场景 `nav-corridor`：1 格宽过道两端各一个厨师对向走，两人都
  在 10 s（游戏时钟）内到达，期间没有任何一帧两人同格。
- **W4. 环形不堵死。** 场景 `nav-ring` 用 `_3` 的真实 `config/maps/s_wellcooked_demo_3/
  kitchen.json`：四个 world order 让 Scheduler 一次派满四个厨师，4 个厨师（速度
  0.5×/0.75×/1.25×/1.5×）走到环上对面的四个料箱站位。PIBT 下全部在 10 s 游戏时钟内到达、
  `nav_stuck` 为 0，任何一帧没有两人同格；`--nav pa` 跑同一场景也全部到达并写
  `nav_metrics.json`。场景从导航表目标槽读每个厨师实际分到的工位，不假设 chef i 拿第 i 张单；
  检查是 fake_plugin 的 `nav-ring` 断言，入口
  `bash tools/run-bridge-scenarios.sh --only nav-ring [--nav pa]`。
- **W5. 卡住上报与崩溃恢复。** 场景 `nav-stuck`：插件对某个厨师的 `path` 一直不动 → `navd`
  重规划 3 次后计划槽 `blocked`，Worker 发 `BLOCKED`，Scheduler 改派，日志能对上这条链。
  `SIGKILL` `navd`：Worker 在 500 ms 内发 `stop`，Supervisor 重启 `navd` 后厨师继续走到目标。
- **W6. 宏动作按世界状态完成。** 场景 `macro`：fake_plugin 延迟 2 s 才让 `holding`/`progress`/
  锅 `state` 变化，宏动作一直等到变化才 `macro_done`；不变化时按表里的超时 `macro_failed`
  且工序 `FAILED`。
- **W7. 派工代价。** 单元检查：给定 2 个空闲厨师、3 道就绪工序与距离场，`assign` 的配对与
  手算的匈牙利结果一致；`decisions.jsonl` 每条有 `w_d/w_t/w_c/w_u/w_z` 各分量。
- **W8. LLF / HRRN。** 单元检查：同一组订单的选择顺序与手算一致；截止期/比值相同时退化为
  FCFS；`SIGHUP` 能切到这两个名字。
- **W9. 洗盘与锅优先。** `_1` 上跑一局：出现 `wash` 工序，没有订单因为缺盘超时；
  `decisions.jsonl` 里 `cook` 前置与等长的非烹饪工序同时可派时先派 `cook`。需要游戏。
- **W10. 多实例隔离与一致。** `tools/instances.py up 3` → 3 个 640×360 窗口，各自的 `BepInEx/`
  里有自己的日志，3 棵进程树的 IPC 名字互不相同；同一（地图, 策略, 订单序列, 种子）在单个
  全尺寸实例和 3 实例小窗口里各跑一局，得分差 < 10%。需要游戏。
- **W11. 实机闭环。** `_3` 上四个厨师（4 个玩家位全占）在系统控制下（插件 `work=auto` 关掉替身逻辑）
  按灵活派工完成至少一单，全程无手动输入。这也满足 os-core 的 W10 实机部分和 game-bridge/05。
  `_2` 的四人分区流水线（每条带 2 人、跨带经交接台面）归 W14。需要游戏。
- **W12. 离线上界。** 每张地图上 `opt.py` 的得分 ≥ 四种订单策略批跑的得分；报告表格列出每种
  策略占上界的百分比。需要游戏（批跑那一半）。
- **W13. 四图演示。** 四张地图各跑一局完整时长，4 个厨师系统控制，没有一次需要人工解围。
  需要游戏。
- **W14. 分区流水线。** `_2` 的 `zones.json` 下批跑：除交接工序外没有跨区派工，每个跨区中间
  产物生成一对交接台面上的 `drop`/`fetch`；同一订单序列下灵活与分区两种模式的得分并列写进
  报告表格。需要游戏。
- **W15. 走位算法对比。** `_3` 上 4 个厨师、同一订单序列，`--nav pibt` 与 `--nav pa` 各批跑
  3 局：报告表格并列总等待步数、卡住次数、单次规划耗时 p99 与得分；PIBT 的卡住次数为 0。
  需要游戏。

出问题时的信号：`navd` 日志的 `nav_replan` 次数突增、`nav_stuck`、`BLOCKED` 事件、
`macro_failed`；`nav_metrics.json` 里等待步数上升；Scheduler 的 `metrics.json` 里超时率上升。

值得测的边界：厨师出生点不在网格上（取最近格）；站位格被另一个正在干活（`pinned`）的厨师
占着（换下一个站位格）；死胡同里的站位格（PIBT 保证不成立，靠重规划）；`navd` 或 Worker 被
`SIGKILL`；游戏暂停中（时钟不走，不判卡住）；同一格两个工位共用站位格；批跑中某个实例崩溃。

## W11 实机遗留（2026-10-26 交接，复盘后重写）

任务 09 在 `_3` 四人实机上连跑 8 轮（fix1–fix8），每局 `score=-300`、0 单。修过什么、每局证据见
任务 09 Progress 的"`_3` 四人联调修复"（最后一局 `logs/real-game/20261026-172424-_3-fix8`）。分支
`kitchen-control/09-real-game-two-chefs`（worktree `~/Project/overcooked-schedule-wt/kc-A`）。

**复盘。** 8 轮里每轮只修日志中最显眼的那个超时，分数一次没动，也没回头核对前提：
- 工序图本身做不成。`recipes.json` 没有切这一步，组合与装盘拆成两步、都落在盘堆上；派下去的
  `drop`/`pickup` 多数是游戏规则上不可能成功的动作，后面的超时、回队都是这个错误的放大。
- 只看日志，没看画面。用户看到厨师左右偏头乱晃、反复瞎碰，日志里没有这类指标，我没发现。
- "宏失败回队"让不可能的动作被同一个厨师重复做，直接造成"瞎碰"。

下面按顺序做，前一项没过不做后一项。标 [待核实] 的是从反编译代码推出来、还没在实机上确认的。

### P0 工序图按游戏规则重建

已核对的事实（`/tmp/kc09-all` 反编译，`BepInEx/wellcooked-menu.json` 与 `wellcooked-state.json` 的
料箱 `prefab_components`；临时目录会丢，需要时重新反编译 `Assembly-CSharp.dll`）：

| 料箱给的 | prefab 上的组件 | 含义 |
|---|---|---|
| `SushiFish`、`BurritoChicken` | 有 `WorkableItem`，没有 `IngredientPropertiesComponent`、没有 `CookableProperties` | 原样既不能装盘也不能下锅；切完由 `ServerWorkableItem` 生成 `WorkableItem.m_nextPrefab` |
| `Rice` | `IngredientPropertiesComponent` + `CookableProperties[Pot]` | 直接下煮锅 |
| `SushiPrawn` | `WorkableItem` + `CookableProperties[Pot, FryingPan]` | 可切也可煮；`Sushi_PlainPrawn` 到底要哪步 [待核实] |
| `Tortilla` | `PreparationContainer` + `IngredientContainer` | 它本身是容器：收 `IngredientPropertiesComponent` 料，或整口锅/平底锅倒进来（`PreparationContainer.GetOrderDefinitionOfCarriedItem`） |

- 装盘规则（`Plate.CanPlaceOnPlate`）：手上的东西必须实现 `IContainerTransferBehaviour`/有订单定义，
  且放进去后的组合能查到 `GetOrderPlatingPrefab`。没切的生鱼不满足，这是 `assemble:Sushi_*` 在盘堆
  `drop` 一直超时的原因 [待核实：实机看一次手拿生鱼按盘子没反应]。
- 锅里熟了以后是"端起锅往盘子/卷饼上倒"，不是把料拿在手上（`CookableContainer` 分支）。
- `kitchen.json` 里没有砧板：`export-kitchen.py` 的工位表不含 `ClientWorkstation`。另有一个映射错误：
  `container-0..5` 导出的是炸篮/平底锅/煮锅，而 `macro.c` 的 `find_station_resource` 把
  `container-N` 映射成 `cutting_board_N`。

解决方案：
1. **先手动打一遍，记下真实步骤。** 进 `_3`，每道菜由人从料箱做到上菜，插件日志开
   `manual_log=true`（`slots=none` + `work=auto` + `bridge=off`；插件没有 `work=manual`，
   `work=off` 仍会用"来回走 + 周期按交互"驱动已占的位，必须一个位都不占），逐步写下：
   拿什么、在哪个工位、按哪个键（interact/切菜键）、之后手上/工位上变成什么物体名。
   产出一张"每道菜的动作表"，写进本节，作为后面一切的对照。
   重点确认 4 件事：鱼和鸡切完的 prefab 名；虾在 `Sushi_PlainPrawn` 里要切还是要煮；卷饼的
   组合顺序（卷饼放盘上、两口锅倒进卷饼，还是别的）；装盘后是否还要额外一步。
2. **导出补砧板。** `export-kitchen.py` 的工位表加 `ClientWorkstation`（kind `board`），`kitchen.json`
   里出现 `board-N`；锅具 utensil 不再当 `container`，改成挂在 cooker 上的属性（已有 `step`），
   `macro.c` 的映射对应改成 `board-N → cutting_board_N`、`cooker-N → pot_N`。
   （2026-10-26 离线已完成：插件地图快照导出 `ClientWorkstation`，与 `cutting_board_N` 共用
   "名字含 Chop/Cut"的规则并按 x、z、name 排序编号；utensil 组跳过，只把 `step` 交给同格 cooker；
   `config/maps/*/kitchen.json` 的重导要等手动局，checked-in 导出尚未改。）
3. **`build_recipes` 按 prefab 生成工序，不再按菜单树直译。** 料箱 prefab 有 `WorkableItem` 且订单
   要的是切后的料 → 插 `cut` 节点（资源 `board`）；有 `CookableProperties` 且菜单是
   `CookedCompositeOrderNode` → `cook`；`Composite` 的组合点由第 1 步的动作表决定：寿司是
   "把切好的鱼/虾放到盘堆上 → 取盘"一步（删掉独立的 `assemble`；不是"先取盘再放料"），卷饼是
   "玉米饼放普通台面当容器 → 把锅里的熟料倒进去 → 整只玉米饼上盘"（见下面第 1 步实测表）。
   早先注释说"自动插 chop 会给出多余工序"，那是没有按 prefab 判断；这次以动作表为准，
   每个插入都要能在表里找到。
   数据前提：`wellcooked-menu.json` 必须重新导出——checkout 里那份是 2026-10-26 17:21 的旧关卡
   （只有 3 道菜），而现在的 `_3` 菜单是 5 道（`layout_3.tsv` 的 `# menu:` 行，游戏日志的菜谱
   下标 0–4 与之一致）。
4. **离线校验工序图。** `test_export_kitchen.py` 用 `scripts/fixtures/s_wellcooked_demo_3/` 的
   state/menu 快照断言五道菜的节点序列与动作表一致（夹具是离线推导的，等实机重导后替换）；
   `scheduler` 启动时对每个节点检查"上一步产物能被这一步的工位接受"（用导出的 `cook_steps`/`step`/
   资源类型），不满足就报 `recipe_infeasible` 并拒绝派这道菜，而不是派下去等超时。

#### P0 第 1 步：`_3` 实测动作表（2026-10-27）

数据：`logs/manual/20261027-022317/manual.log`（3954 行，单局；切点 L4，t=30.04；同目录
`LogOutput.log` 给订单与工位）。`L###` 指 `manual.log` 行号。用户复核过四条：`Sushi_Plain*`
就是"切好的鱼/虾单料"（没有米、没有海苔）；5 盘全部得分；卷饼以"玉米饼放台面当容器 →
倒熟料 → 上盘"为正则（"手持玉米饼去锅里舀"的变体不建模）；冲刺、掉落/捡回不算工序。

键位：A = 拿/放/交互，X = 加工键（按住）。工位：砧板0 `(3.00,-1.20)`、砧板1 `(6.60,-1.20)`；
平底锅灶 `(1.80,-6.00)`、`(3.00,-6.00)`；汤锅灶 `(6.60,-6.00)`；装盘台/干净盘堆 `(3.00,1.20)`；
传菜口 `(4.80,1.20)`；卷饼容器台面 `(0.60,-6.00)`；料箱一排 `z=-8.40`（x：0.60 虾 / 1.80 玉米饼 /
3.00 米 / 4.20 鸡 / 5.40 牛肉 / 6.60 蘑菇 / 7.80 鱼）。

**寿司两道（`Sushi_PlainPrawn` 先做先交，`Sushi_PlainFish` 其次）**

| 步骤 | 结果 | 证据（t 段 / 行） |
|---|---|---|
| 1 货箱 → A | 手「SushiPrawn」/「SushiFish」 | 41.27–41.28 L72–L74；49.74–49.75 L693–L695 |
| 2 砧板 → A | 料上台面，子进度 0 | 43.08 L94–L116；51.14 L723–L736（虾用砧板0，鱼用砧板1） |
| 3 砧板 → X 按住 | 子进度 0→7（8 格）→「ChoppedSushiPrawn」/「ChoppedSushiFish」 | 43.40–45.09 L117–L531；51.34–52.96 L740–L1135 |
| 4 砧板 → A | 手「Chopped…」 | 45.52–45.57 L617–L619；53.35–53.36 L1136–L1138 |
| 5 装盘台 → A | 手空；盘 `equipment_plate_01`=[SushiPrawn]/[SushiFish] | 46.51–46.62 L636–L644；54.37–54.48 L1157–L1187 |
| 6 装盘台 → A | 手盘 | 46.84–46.85 L645–L647；54.73–54.74 L1188–L1190 |
| 7 传菜口 → A | 手空，盘落台，约 0.5 s 后消失 | 47.23–47.90 L669–L692；55.26–56.01 L1207–L1232 |

**卷饼三道（Chicken / Meat / Mushroom；三者同构，只有料、切完的名字和用哪口锅不同）**

| 步骤 | 结果 | 证据（Chicken / Meat / Mushroom） |
|---|---|---|
| 1 货箱 Tortilla → A | 手「Tortilla」 | L1233–L1235 / L2757–L2759 / L3622–L3624 |
| 2 台面 (0.60,-6.00) → A | 手空，玉米饼成为容器 | L1257–L1259 / L2780–L2783 / L3642–L3644 |
| 3 货箱料 → A | 手「BurritoChicken」/「BurritoMeat」/「Mushroom」 | L1283–L1285 / L2129–L2131 / L3076–L3078 |
| 4 砧板0 → A | 料上台面 | L1293–L1295 / L2153–L2155 / L3140–L3142 |
| 5 砧板0 → X 按住 | 8 格 →「BurritoChoppedChicken」/「DicedMeat」/「ChoppedMushroom」 | L1314–L1700 / L2178–L2582 / L3159–L3477 |
| 6 砧板0 → A | 手切好的料 | L1778–L1780 / L2662–L2664 / L3513–L3515 |
| 7 平底锅灶 → A | 锅 [BurritoChicken]/[BurritoMeat]/[Mushroom] | L1809–L1833 / L2679–L2711 / L3534–L3548（蘑菇用另一口锅 `(3.00,-6.00)`） |
| 8 货箱 Rice → A | 手「Rice」 | L1838–L1840 / L2716–L2718 / L3552–L3554 |
| 9 汤锅灶 → A | 锅 [Rice] | L1863–L1885 / L2731–L2749 / L3602–L3607 |
| 10 等熟 → 取锅 | 12.1 s 后"已熟"；手「utensil_frying_pan_01」[料] | L1924–L1929 / L2826–L2830 / L3694–L3697 |
| 11 台面 → A（手持锅） | 手仍持锅、锅内空；容器「Tortilla」=[Cooked…] | L1946–L1963 / L2847–L2870 / （本轮是变体，见下） |
| 12 平底锅灶 → A | 空锅回灶，手空 | L1972–L1974 / L2873–L2875 / — |
| 13 米熟 → 取锅 | 手「utensil_pot_01」[Rice] | L1982–L1988 / L2900–L2903 / L3745–L3803 |
| 14 台面 → A（手持锅） | 手仍持锅；容器「Tortilla」=[Cooked+Cooked] | L1994–L2008 / L2916–L2929 / L3819–L3843 |
| 15 汤锅灶 → A | 空锅回灶 | L2009–L2011 / L2991–L2993 / L3844–L3846 |
| 16 台面 → A | 手「Tortilla」[Cooked+Cooked] | L2031–L2033 / L3009–L3011 / L3864–L3866 |
| 17 装盘台 → A | 手空；盘=[CompositeAssembledNode] | L2069–L2076 / L3033–L3050 / L3913–L3918 |
| 18 装盘台 → A | 手盘 | L2084–L2086 / L3054–L3056 / L3924 |
| 19 传菜口 → A | 盘落台，约 0.5 s 后消失 | L2107–L2128 / L3072–L3075 / L3935–L3954 |

`Mushroom_Burrito` 本轮的第 11 步用的是变体（手持玉米饼在 (-0.60,-6.00) 的锅前把蘑菇吸进玉米饼，
L3762–L3766），按用户决定不建模；模型统一用 Chicken/Meat 的"料从锅倒进台面上的玉米饼"。

**四个待核实问题的答案**

1. 切完的 prefab 名：`ChoppedSushiFish`、`ChoppedSushiPrawn`、`BurritoChoppedChicken`、
   `DicedMeat`（牛肉，不是 `BurritoChoppedMeat`）、`ChoppedMushroom`。
2. 虾在 `Sushi_PlainPrawn` 里**只切不煮**：全程没进任何锅（L72–L692）。
3. 卷饼的组合顺序是"玉米饼先放普通台面当容器 → 熟料从锅/平底锅倒进去（倒时厨师端着锅，手不空）
   → 整只玉米饼上盘"。不是"卷饼放盘上"，也不是"料先上盘"。
4. 装盘之后没有额外一步：上盘 → 取盘 → 传菜口。

**工时（同一局实测，含当前 `_3` 调参：`chop_stages=2`、`chop_multiplier=8`、锅容量 1）**

| 工序 | 实测 | 依据 |
|---|---|---|
| 切 | 8 格，每格约 0.20–0.24 s，合计 1.6–1.75 s（X 按住；日志里每 0.5 s 一次的 ≤0.09 s 空档是记录抖动，不是松手） | L117–L531 等五段 |
| 煮/煎 | 12.07–12.15 s 到"已熟"（米、鸡、牛肉、蘑菇都一样） | L1924、L1982、L2826、L2900、L3694、L3745 |
| 装盘 | 一次 A，放上去到盘出现 ≤0.1 s | L636–L644 等 |
| 交付 | 盘落传菜口到消失约 0.5 s | L691–L692 等 |
| 拿料/取放/倒料 | 各一次 A，约 0.01–0.02 s | 全部 |
| 烧焦 | 未知；同一口锅在"已熟"后继续加热到 16.10 s 仍未烧焦 | L3745 前后（米第三次） |

**对 `build_recipes` 的约束**（这就是第 3 步要生成的图）

- 5 道菜：`Chicken_Burrito`、`Meat_Burrito`、`Mushroom_Burrito`、`Sushi_PlainFish`、
  `Sushi_PlainPrawn`（顺序同 `levels/wellcooked_demo/layout_3.tsv` 的 `# menu:`）。
- 寿司：`fetch → cut → plate → deliver`，没有 `assemble`；`plate` 的语义是"把手里整份菜放到
  盘堆上，再取盘"，不是"先取盘，再去锅里取料"。
- 卷饼：`fetch(Tortilla) → place(Tortilla 上普通台面)`；`fetch+cut+cook(料)`、`fetch+cook(米)`；
  `pour(料锅 → 玉米饼)`、`pour(米锅 → 玉米饼)`；`pickup(玉米饼)`；`plate`；`deliver`。
- 熟料的 `step` 取游戏菜单树：鸡/牛/蘑菇 `FryingPan`，米 `Pot`；寿司不煮。
- 切用 `cut`，资源 `board`；玉米饼放到 `counter`；倒料需要"端起锅 → 对着容器按 interact → 锅放回灶"
  的一步——当前 `macro.c` 没有这个 op（`assemble` 的 drop 以"手空"判完成，而实测倒完料锅还在手上）。

验收：一个厨师、`Sushi_PlainFish` 一道菜、系统控制完成一单，得分 > 0。

#### P0 第 5–7 步：状态上报的三个阻塞项（2026-10-27 审计）

这三步排在第 4 步之后、上面那段 P0 验收之前。完整清单与证据见下面的《状态上报缺口（G1–G14）》；
P0 验收仍是“一个厨师、`Sushi_PlainFish`、系统控制完成一单、得分 > 0”。

- **P0 第 5 步 — 锅键按灶台位置稳定（G1）。** 按 P3 第 1 条枚举 `ServerCookableContainer`，空锅报
  `state=free`，按 x、z 排序并与 `kitchen.json` 的 `cooker-N` 一一对应；`_3` 的 6 口锅实机核对坐标。
  完成信号：`cook:BoiledRice` 派到 `cooker-4` 时帧里有 `pot_4`，`start_cook` 不再出现
  `no-station-resource`（当前实机 0 ms 失败 3 次，`worker-3.log:20/27/33`）。
- **P0 第 6 步 — 干净盘数改用 `GetCount`（G2）。** `plugin/Link/StateFrame.cs:350` 的
  `ClientCleanPlateStack.GetSize()` 改成基类 `ClientPlateStackBase.GetCount()`（任务 02 的
  `dirty_plates` 已用同一方法）。完成信号：`state_update` 里有 `plate_stack.count`，洗盘判定不再
  恒 `clean=0`（`LogOutput.log:166-168`、`fix4/scheduler.log:114`）。
- **P0 第 7 步 — 交付与过期分开（G9）。** `macro.c:run_deliver` 只等订单从 `m_activeOrders` 消失，
  过期也会消失，可能把扣分当完成 [推断]。拿游戏侧结果：插件在 `OnSuccessfulDelivery`/
  `OnOrderExpired` 上记结果，或 Scheduler 用交付前后 `world.score` 差判定；`order_completed` 只在
  真的加分时写。完成信号：交付成功写 `order_completed` 且得分上升；过期只写失败，不写完成。

#### 执行顺序（状态缺口插入后）

1. P0 工序图（第 1/3/4 步）。
2. 进关重导 `config/maps/s_wellcooked_demo_3/{kitchen,recipes}.json`（5 道菜、砧板、utensil 归位）。
3. G1 / G2 / G9（上面第 5–7 步）。
4. P0 验收：一个厨师、`Sushi_PlainFish`、得分 > 0。
5. 任务 09 的 Check：`_3`、四个厨师、灵活派工。
6. `_1` 洗盘（W9）。
7. P1：状态消费与内容身份（G11/G3/G4/G5/G7）与 P1 走位抖动。
8. 任务 11：权重网格搜索与批跑。

### P1 走位抖动（偏头乱晃）

fix8 证据：navd 一局写了 987 条 `nav_plan`，每个厨师收到约 260 条 `path`；同一目标下 seq 每
50–100 ms 换一次，路点从 8 递减到 1。`navd.c` `write_plan` 只要放行的路点列表有任何变化（走过一格、
PIBT 少放行一格）就 `plan_seq + 1`；插件 `ActionDriver.Handle` 收到新 seq 就新建 `Command`
（`WaypointIndex=0`、`StartedAt` 重置），厨师每次都重新对准第一个路点。路点一缩短就转向新的
"第一个点"，加上 0.3 m 到站判定与转身时的漂移，表现就是左右晃头。

解决方案：
1. navd 只在**目标或路线变了**时换 seq：走过的格从队首弹出、PIBT 临时少放行，都不算新路线；
   用"剩余路线是否为旧路线的后缀"判断。
2. 插件同 seq 收到更短的列表时，按位置把 `WaypointIndex` 对齐到剩余路线（现在只处理追加）；
   只有 seq 变化才重建 `Command`。
3. 到站判定加滞回：进 0.3 m 算到，出 0.5 m 才算离开；转身阶段不再因漂移回到导航（`run_goto`
   已部分做了）。
4. 指标：`nav_metrics.json` 加每个厨师每秒的 `path` 条数和朝向变化角度累计；验收为单人走一条 10 格
   直线时 `path` ≤ 2 条、朝向反转次数 0。实机录屏或至少看一次画面确认。

### P1 状态消费与内容身份（2026-10-27 审计）

这一节只放 P1 走位抖动之外、审计发现的状态消费缺口。G1/G2/G9 属 P0，不在这里。

1. **Scheduler 看 `holding`（G11）。** `dispatch_ready`/`build_candidates` 只读坐标与任务状态，
   `kos_chef.holding` 进了 shm 却没有读者；手上拿着盘子/料的厨师会被派去 `fetch`，宏只能等超时
   （`worker-2.log:19` 的手持状态超时是同类现象 [推断]）。做法：候选过滤或代价里惩罚
   `holding[0] != '\0'` 的厨师，或给 `fetch` 加“先放下/交接”一步。验收：4 人局里不再因为
   “手不空”出现 `pickup`/`empty-hand` 超时。
2. **容器/盘/手持内容递归（G3/G4）。** 复用一套 walker：`CompositeAssembledNode.m_composition`
   递归，叶子读 `IngredientAssembledNode.m_ingriedientOrderNode.name`；盘优先
   `ClientPlate.GetOrderComposition()`。`StateFrame` 用它填手持内容与容器内容。验收：
   `state_update` 里玉米饼显示“米饭+鸡肉”，而不是 `CookedCompositeAssembledNode`；盘能看出是哪道菜。
   组合节点递归在实机未验证 [推断]，先拿手动局日志 `manual:2353`/`manual:2421` 对照。
3. **台面物品进帧（G5）。** `AppendContainers` 枚举 `ClientIngredientContainer`，只报有内容的，
   带 `pos`。这同时补上任务 09 卡住的 `counter` 工位（玉米饼放普通台面）。验收：放进台面的
   Tortilla 在帧里可见、有坐标和内容；`pour` 能按坐标找到它。
4. **每单分值进入优先级（G7）。** 插件发 `RecipeListEntry.m_scoreForMeal`（卷饼 80、寿司 20），
   Scheduler 不再用 fallback 10。先作订单级 tie-break；价值密度是否进 `cost` 留给任务 11 的
   网格搜索。验收：`order_arrived` 的 `reward` 是 80/20，而不是清一色 10；截止期相同时高分菜先开。

### P2 反复瞎碰（重试风暴）

fix8 证据：`macro_failed` 里 `drop` 9 次、`pickup` 4 次、`start_cook` 3 次、`goto` 3 次；同一个厨师对着
同一个工位隔 0.6 s 按 interact，没效果就再按一轮，失败后 `task_requeue` 又派回同一个厨师（料在他
手上，手持亲和规定只能派给他），同一动作最多重复 3 轮 × 每轮 2 下。

解决方案：
1. 每个宏步骤写明"预期的游戏变化"（手持物名变成什么、工位内容变成什么），按一次后在固定窗口
   （约 0.5 s 游戏时间）里检查；没变化就**不再按第二下**，直接报 `macro_failed`，detail 带按键时的
   位姿、面前物体名和手持物，便于判断是"对错了目标"还是"游戏不收"。去掉 `ActionDriver` 里
   "再按一次"的盲按。
2. 按失败原因分流，不一律回队：面前物体不是目标工位 → 重新 goto 一次（走位问题）；面前就是目标但
   游戏不收 → 判定工序图错误，记 `recipe_infeasible` 并让这道菜失败，不重试；手上东西丢了 → 订单失败。
3. 回队加退避（同一工序第 n 次失败后等 n × 2 s 再派），`SCHED_TASK_MAX_RETRIES` 维持 3。
4. `fake_plugin.py --scenario macro` 加一个"游戏永远不收"的场景：断言只按一下、1 次 `macro_failed`、
   不回队。

### P3 锅状态上报

`state_update` 里没有空锅（`pot_<N>` 缺失），`start_cook` 报 `no-station-resource`。原因：
`ServerCookableContainer.StartSynchronising` 把 `ServerCookingHandler.enabled` 设成 false，锅里有料时
`OnContentsChanged` 才打开；handler 在 `OnEnable` 才进静态表，所以 `GetCookingHandlers()` 只列出有料的锅。

解决方案：
1. `plugin/Kitchen.cs` 的 `SortedCookingHandlers` 改成枚举 `ServerCookableContainer`
   （`WorldState.FindAll`），每个取 `GetCookingHandler()`，位置用容器的 transform，按 x、再 z 排序；
   `StateFrame.AppendPots`、`Kitchen.TryStationPosition("pot_N")`、`WorldState.AppendCooking` 共用这份
   列表。空锅报 `state=free`。排序必须和 `kitchen.json` 的 `cooker-N`（同样按 x、z 排）一一对应，
   `_3` 的 6 口锅实机核对一次坐标。
2. 修两处反射错位：`ServerCookingHandler` 本身是 `Component`，现在 `Get(component, "m_cookingType")`、
   `Invoke(component, "GetContents")` 都落空（插件日志 `反射: 没找到 …`）。锅类型改读
   `AccessCookingType`；内容读容器的 `m_itemContainer.GetContents()`，节点名取
   `IngredientAssembledNode.m_ingriedientOrderNode.name`（参照 `WellCooked.Plugin/Game/StateReader.cs`
   的 `ContainerJson`/`CollectNodeNames`）。
3. `GetCookingProgress()` 是秒，不是 0..1（局末一口锅 `progress=24.013`）：`time_left =
   AccessCookingTime − progress`；`cooking` = 有料且未熟。`plugin/README.md` 的"锅"一行同步改。

### 状态上报缺口（G1–G14，2026-10-27 审计）

证据来自代码、`logs/real-game/20261027-015135-_3-record/logs/`、`logs/manual/20261027-022317/`
与 `Assembly-CSharp.dll` 的成员表（只读元数据）。P3 已解决的锅状态问题不重复；G1/G6 直接引用 P3。

**P0 — 阻塞（不修就完不成一单）**

| # | 缺什么 | 证据 | 阻塞 | 修复位置 |
|---|---|---|---|---|
| G1 | `pot_N` 用 `GetCookingHandlers()` 的当前枚举下标，空锅不在列表，键不对应灶台 | `worker-3.log:20/27/33` 三次 `no-station-resource`；`scheduler.log:41/52/59`；`bridge.log:3019→3031`（资源数 3→4，只多一口锅） | 阻塞 | P3 第 1 条；P0 第 5 步 |
| G2 | 干净盘数读 `ClientCleanPlateStack.GetSize()`，该类没有此成员 → `plate_stack` 从不进帧 | `LogOutput.log:166-168`（属性/字段/方法三连未找到）；`fix4/scheduler.log:114` `clean=0` | 阻塞 | `StateFrame.AppendPlateStack` 改 `GetCount()`；P0 第 6 步 |
| G9 | 交付成功与订单过期都表现为订单从 `m_activeOrders` 消失 [推断] | `macro.c:753 run_deliver`；`ServerKitchenFlowControllerBase` 有 `OnSuccessfulDelivery`/`OnOrderExpired`（元数据） | 阻塞 | 插件发每单结果，或 Scheduler 用 `world.score` 差；P0 第 7 步 |
| G10 | `_3` 已 5 道菜，导出 `recipes.json` 仍 3 道、无 `cut`；`workable` 未解析 | `scheduler.log:45` 起 52 次 `order_unknown_dish`；`BepInEx/wellcooked-menu.json` 17:21 只有 3 道；`recipes.h:32` 无 `workable`；`export-kitchen.py:379` 不生成 `cut` | 阻塞（离线部分已修） | P0 第 3 步已改 `build_recipes`，5 道菜与节点序列由 `test_export_kitchen.py` 锁住；`workable` 不是消费者字段，已从生成器移除（`recipes.c` 不解析）。剩余：进关重导 `config/maps/s_wellcooked_demo_3/` |

**P1 — 能跑但会送错/空等（做法见《P1 状态消费与内容身份》）**

| # | 缺什么 | 证据 | 阻塞 | 修复位置 |
|---|---|---|---|---|
| G11 | Scheduler 不读 `chef.holding`，手上拿盘的厨师会被派去 `fetch` | `scheduler.c:2193 dispatch_ready`/`:1979 build_candidates` 只读坐标与任务状态；`worker-2.log:19` 手持状态超时 [推断] | 高 | `dispatch_ready`/`build_candidates` 读 `world->chefs[i].holding` |
| G3 | 容器/盘内容只到 `CookedCompositeAssembledNode`/`CompositeAssembledNode` 类型名，馅料与菜名丢了 | `manual:2353` 玉米饼 `[Cooked…+Cooked…]`；`manual:2421` 盘 `[CompositeAssembledNode]`；组合节点递归在实机未验证 [推断] | 高 | `ManualLog.NodeName` 递归 `CompositeAssembledNode.m_composition`，叶子读 `IngredientAssembledNode.m_ingriedientOrderNode.name`；盘用 `ClientPlate.GetOrderComposition()`；`StateFrame` 上报容器 |
| G4 | 手持物只发名字，不发内容 | `manual:931` 手持盘有 `[SushiPrawn]`；`manual:2233` 手持锅有 `[BurritoChicken]` | 高 | `StateFrame.AppendChefs` 加 `holding_contents`（同一套递归） |
| G5 | 台面/地面/容器物品不在帧里 | `manual:3242` 容器有坐标与内容；`ManualLog.cs:158 ObserveStations` 已在枚举 `ClientIngredientContainer` | 高 | `StateFrame` 新增 `AppendContainers`（只报有内容的），协议加 `pos`；`export-kitchen.py` 加 `counter` kind |
| G7 | 每单分值没发，Scheduler 用 fallback 10；`kos_order_view.reward` 有槽位但没人读 | 游戏 `wellcooked-menu.json`：卷饼 80、寿司 20；`scheduler.log` `reward=10`；`policy.h:43` | 高 | `StateFrame.AppendOrder` 加 `score`；`poll_world_orders` 传值；优先级用法见 P1 节 |

**P2 — 优化/容错**

| # | 缺什么 | 证据 | 阻塞 | 修复位置 |
|---|---|---|---|---|
| G6 | 锅 `progress` 不进帧（只发 `time_left`）；烧焦剩余、火没有 | P3 第 3 条已明确 `GetCookingProgress()` 是秒；P0 动作表里“已熟”后 16.10 s 仍未烧焦 | 中 | `StateFrame.AppendPots` 补 `progress`；烧焦/火另立资源 |
| G8 | 回合剩余时间没发 | `ServerRoundTimer.m_timeLeft/m_timeLimit` 字段存在（元数据），运行时读取未验证 [推断] | 中 | `StateFrame.Build` + 协议/shm |
| G12 | 订单 `ingredients` 没发 | 协议/shm 有槽位，插件不填；Scheduler 不读 | 低 | 可选 |
| G13 | 锅/板内容不校验身份，放错料也当成功 | `macro.c:668 run_start_cook` 只判 `content` 非空；`macro.c:640 run_chop` 只看 progress | 中 | `macro.c` 比对 `content` 与节点 `label/ingredient` |
| G14 | 厨师 `state`（moving/idle）没发 | `StateFrame.AppendChefs` 不填 | 低 | 可选 |

**改动顺序与容量。** 新增字段固定按这个顺序改：`protocol/protocol.md` 与 `protocol/schema/` →
`bridge/include/kitchenos/protocol.h` 的 `proto_*` 与 `bridge/src/protocol.c` 的 `parse_*` →
`bridge/include/kitchenos/shm.h`（升 `KOS_SHM_VERSION`）与 `bridge/src/bridge.c:world_from_state_update`
→ 插件 `plugin/Link/StateFrame.cs`。`tools/fake_plugin.py` 与 `plugin/tools/conformance/` 同一次改，
否则协议级场景会漏测。`kos_resource.content` 只有 32 字节（`shm.h:60`），资源上限 32
（`shm.h:54`）；递归内容必须新开 `contents[]`，不能塞进 `content`。`_3` 有 42 个 Counter，
`AppendContainers` 只报**有内容**的台面/容器，否则会超过 32 个资源上限；要全报就同时升上限与
`KOS_SHM_VERSION`。

### P4 工作方法与验收顺序

1. 先单人单菜：`CHEFS=1`，依次做 `Sushi_PlainFish` → `Sushi_PlainPrawn` → `Chicken_Burrito`，每道都出单
   后再上 4 人。4 人时才处理让路（`goto arrived+face` 6–10 s 超时）。
2. 每局实机跑完必须同时看三样东西，写进任务 Progress：得分与 `order_completed`；P1 的抖动指标；
   每个 `macro_failed` 的分流原因计数。每局至少看一次游戏画面（或录屏），画面与日志对不上时以画面为准。
3. 同一个现象连修两轮分数不动，就停下来回到 P0 的动作表核对前提，不再继续调超时和余量。
4. 通过标准不变（任务 09 的 Check）：局内得分 > 0、有 `order_completed`、四个厨师都由 Worker 驱动、
   一单完整的 `macro_*` 链能在 Worker 日志里对上、全程无手动输入。

实机命令：`RUNTIME_DIR=$PWD/logs/real-game/<ts>-_3-<tag> WAIT_SECONDS=240 SCENE=s_wellcooked_demo_3
CHEFS=<n> REAL_GAME=1 bash tools/run-real-game.sh`；局后把游戏目录的 `BepInEx/LogOutput.log` 复制成
运行目录里的 `LogOutput.run.log`（运行目录里的 `LogOutput.previous.log` 是上一局的）。跑之前取
`/tmp/wellcooked-game.lock`，跑完释放。插件改动后 `bash scripts/build-plugin.sh`；重新导出地图见
`.workflow/standards.md`。

## Open questions

- **每格实际移动时间。** 已测（任务 01，2026-10-25）：`_2` 上让 1 号厨师沿 z=-7.2 的直线用
  轴向输入走 3 段约 10 格（5.88 / 6.00 / 6.12 m），游戏时钟用时 1.071 / 1.080 / 1.052 s，
  平均 **0.107 s/格**（约 5.5–5.8 m/s）；连续 10 格往返（20 格）实测 2.092–2.151 s。
  `path` 的 0.15 m 转角容差与 2 s 卡住阈值不用按地图调——2 s 约 18 格，远大于相邻路点间距。
  方法：插件的临时调试指令 `walk=slot,dx,dz,秒` 直接按轴喂输入（不走 Navigation），起止时刻取
  游戏时钟、坐标取日志；测完该指令已删除，原始日志留在任务 01 的 Progress。
- **砧板进度、脏盘数、厨师朝向能否从游戏读到。** 2026-10-25 任务 02 在 `s_city_h5` 实机确认：
  `facing`（`atan2(forward.x, forward.z)`，弧度）、`dirty_plates`（`ClientPlateStackBase.GetCount()`）、
  水槽 `progress`（`m_cleaningTimer / m_washingStation.m_cleanPlateTime`）都能读到并写进
  `state_update`；砧板 `progress` 走 `ClientWorkableItem.GetProgress()`，字段与方法已用
  ilspy 对照 `Assembly-CSharp.dll` 确认存在，但本轮实机没有把料放上砧板，数值读取仍待一次
  "放料后切一刀"的实机确认。本轮没有发现需要退回"工时 + 余量"的字段。
- **`Screen.SetResolution` 能否盖过注册表里的全屏记录。** 已测（任务 08，2026-10-25）：能。
  插件 `window=640x360` 后日志为 `takeover: 窗口 = 640x360（全屏=False）`，不用改前缀注册表。
  窗口位置 `window_pos` 在同一次实测里也生效（日志 `窗口位置 = 0,0`）。
- **本机能稳定跑几个实例。** 已测（任务 08，2026-10-25）：3 个 640×360 实例可同时跑，
  3 实例连跑 10 局全部 attempt 1、无实例掉线；默认 N = 3。余量很小（GPU 7.5/8.2 GB、
  可用内存约 2 GB），更大规模前先减窗口或换机器。
- **`_2` 分区的交接台面放哪块。** 由用户（地图作者）在 `zones.json` 里定；地图作者未定前，
  任务 10 在 `zones.json` 中配置默认台面 `counter_handoff_0`（坐标 `[4.2, -3.6]`，两侧站位格
  `[4.2, -4.8]` 与 `[4.2, -2.4]`，分属上下两连通区，两区厨师均可直达）。
- **代价权重默认值。** 任务 07 先写入一组初始值（`w_d=1.0, w_t=0.2, w_c=0.5, w_u=2.0,
  w_z=1.0`、`cell_ms=107`、`wash_ms=3000`），由任务 11 的多实例批跑网格搜索调整后写回各地图
  的 `schedule.json`。

## Notes

四张地图的调度特征（来自 `levels/wellcooked_demo/layout_*.tsv`，以导出为准）：

| 地图 | 特征 | 对调度的影响 |
|---|---|---|
| `_1` | 没有干净盘堆 | 盘子全靠洗，`wash` 是关键路径的一环 |
| `_2` | 两条互不相通的条带（z=-6 一排台面隔开），每带 2 个出生点；料箱、锅、炸台、出菜口在上带，砧板、水槽、盘堆在下带 | 只能分区流水线：每带 2 人，跨带靠台面交接 |
| `_3` | 环形过道，实机菜谱 3 道 | 多厨师避让与死锁的主要测试图 |
| `_4` | 跨度最大（炸台 x=-11、砧板 x=29） | 路程权重 `w_d` 影响最大 |

四张图的实测导出（2026-10-25，各在实机进关一次，`scripts/export-kitchen.py --map-dir config/maps`；
`_3`/`_4` 在任务 03 修掉采样时机后重导出过一次，`_1`/`_2` 沿用任务 01 的导出——两张图的工位
都已在 1.2 m 内有正对的格，见 decisions.md: 2026-10-25 - 地形采样等工位生成后再做）：

| 地图 | 可走格（连通区） | 逻辑工位 | 导出 kinds | `layout_*.tsv` 资源类物件 | 实机菜单 |
|---|---|---|---|---|---|
| `_1` | 288（1） | 20 | serving 1 / dispenser 6 / cooker 6 / sink 1 / container 6 | 17 | 2 道 |
| `_2` | 350（**2**） | 17 | serving 1 / dispenser 5 / cooker 4 / sink 1 / clean_plate_stack 1 / plate_stack 1 / container 4 | 15 | 3 道 |
| `_3` | 167（1） | 23 | serving 1 / dispenser 7 / cooker 6 / sink 1 / clean_plate_stack 1 / plate_stack 1 / container 6 | 19 | 3 道 |
| `_4` | 381（1） | 23 | serving 1 / dispenser 7 / cooker 6 / sink 1 / clean_plate_stack 1 / plate_stack 1 / container 6 | 19 | 3 道 |

`layout_*.tsv` 的资源类物件 = 料箱 + 砧板 + Cooker + FryingStation + 水槽 + 出菜口 +
PlateReturn + CleanPlateStack（不含 art、spawn、counter、utensil）。差异（以导出为准）：

- **砧板没有导出。** 每张 layout 有 2 个 `ChoppingCounter`，但插件 `AppendMap` 的组件表没有
  `ClientWorkstation`，raw state 里也没有它们，所以导出 `chopping=0`。四张图的菜谱没有 cut
  节点，本任务的加载检查不受影响；L2 的 `cut` 宏动作需要后续任务补这个导出字段。
- **PlateReturn 没有导出。** layout 每张 1 个，raw state 没有对应组件；不参与当前资源模型。
- `_1` 没有干净盘堆，`plate` 工序的 `resource=plate_stack` 找不到工位，Scheduler 启动会为
  `plate:*` 节点报 `recipe_no_station`；这是 spec Notes 已记的缺口，由 wash 任务补，不是
  kitchen/recipes 解析失败。
- `_2/_3/_4` 的 `CleanPlateStack` 物体同时带 `ClientPlate`，按物体名拆成同坐标的
  `clean_plate_stack` 与 `plate_stack` 两个工位；Scheduler 的 `plate` 工序用后者。
- `Cooker` 与 `FryingStation` 都带 `ClientCookingStation`，导出合并成 `cooker`；锅/炸篮等
  utensil 另算 `container`，所以导出工位数比 layout 的资源类物件多。
- `_2` 的两条厨房条带（各 175 格）互不相通，`grid.areas.count=2`；其余三张图各 1 个可走区。
- `layout_3.tsv`/`layout_4.tsv` 的 `# menu` 写 4 道（含 `Burrito_Beef_SO`），实机菜谱表是
  3 道（无 Beef）；以后者为准。

`levels/wellcooked_demo/README.md` §6 的工位数也与上表不一致（例如 `_1` 表写 4 料箱/3 砧板，
layout 是 6/2），两份都不如导出可靠；本 effort 只认 `config/maps/` 里的导出。

相关：`.workflow/specs/os-core.md`（进程树、mq、租约、FCFS/EDF）；game-bridge/05（两个厨师
完成订单）与 game-bridge/13（自制关卡、导航卡住的记录）；方案 `overcooked_os_course_project.md`
§4.3、§4.4、§6.4、§6.5。
