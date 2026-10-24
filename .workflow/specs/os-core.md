# OS 核心：Supervisor + Scheduler + Chef Worker

**Status:** 进行中（Supervisor、Scheduler、Worker、共享资源租约与 metrics 已落地；
W8/W11 有自动化检查，W10 的实机部分仍未验证）
**Priority:** high

## What this is for

Bridge 已经能把游戏状态写进 `/kitchenos_state`、把每个厨师的动作从输入槽发回游戏，但它
上面没有调度者，下面没有执行者：订单不会变成工序，工序不会变成某个厨师的路径，进程死了
没有东西把它拉起来。课程要求的两级调度（订单级 + 工序级）、共享资源、信号处理与守护进程
目前一条都不存在。本 effort 补上另外四类进程，并给出它们之间每条链路的归属。

设计基线是仓库根 `overcooked_os_course_project.md`（§2.2、§4.2–4.5、§5.4、§6.1–6.6、
§8.2、§9）。与它冲突时以它为准；本文只记录实现层面的取舍。

## What it does

### 进程树

```
kitchenos-supervisor                 完整 Linux 守护进程，单实例（pid 文件 fcntl 锁）
├── kitchenos-bridge                 TCP ⇄ 插件；唯一世界状态写者；帧屏障
├── kitchenos-scheduler              订单/工序队列、策略、资源租约、决策日志
└── kitchenos-worker --chef-id 0..N-1
                                     A* 路径规划 → 每帧写自己那格输入槽
```

四个可执行文件都由 `bridge/Makefile` 的 `make` 生成，Supervisor 用 `fork` + `execv`
拉起其余三个（`--bridge-bin` / `--scheduler-bin` / `--worker-bin` 可覆盖路径，默认取
Supervisor 自己所在目录）。Scheduler 与 Worker 是同一个源码树里的两个 `main`，不是线程。

### Supervisor

- 默认走完整守护进程流程：`fork` → 父进程等就绪字节后退出 → `setsid` → `umask(022)` →
  `chdir("/")` → 关掉继承 fd → `dup2` 到 `/dev/null`。`--foreground` 跳过这一整套，
  给测试与排障用；两种模式共用同一个主循环。
- 启动前先把运行目录解析成绝对路径（daemon 之后 `chdir("/")` 会让相对路径失效）。
- `runtime/kitchend.pid` 用 `fcntl(F_SETLK, F_WRLCK)` 加锁；第二个实例拿不到锁时退出码
  **3**，stderr 写明 pid 文件路径，绝不碰第一个实例的日志。daemon 模式下父进程通过就绪
  管道拿到这个退出码，所以 `kitchenos-supervisor` 命令本身能报告"已在运行"。
- 每个子进程一条匿名管道做心跳：子进程每 `--supervisor-heartbeat-ms` 写 1 字节，
  Supervisor 在主循环里非阻塞读走；`SIGALRM` 健康检查发现超过
  `--heartbeat-timeout-ms` 没有字节时记 `child_unresponsive` 并发 `SIGKILL`，随后按普通
  崩溃走重启流程。管道写端断掉（Supervisor 已死）时子进程自己退出，不留孤儿。
- `SIGCHLD` 只置标志，主循环用 `waitpid(-1, ..., WNOHANG)` 收尸；退出原因（退出码或
  终止信号）写进日志与 `status.json`。
- 重启策略按 §6.1：第一次立即；窗口内第二次等 `--backoff-ms` 列表的第 2 项（默认 2 s）；
  第三次起等第 3 项（默认 10 s）。窗口默认 60 s，`--crash-limit` 默认 5：窗口内第 5 次
  退出后记 `child_given_up`，不再拉起这个子进程，其余子进程照常监督。列表最后一项重复使用。
- `SIGALRM` 由 `setitimer(ITIMER_REAL)` 周期触发（`--health-interval-ms`，默认 2000）；
  健康检查做三件事：读心跳管道、`kill(pid, 0)` 探测、把 `running/pending/given_up` 计数
  写进 DEBUG 日志。定时器在每次处理完信号后重新装填。
- `SIGTERM`/`SIGINT`：进入关闭流程，顺序是 **Workers → Scheduler → Bridge**（先停生产者，
  再停决策者，最后停唯一连游戏的进程），每个子进程先 `SIGTERM`，超过
  `--stop-timeout-ms` 再 `SIGKILL`；全部收尸后 `shm_unlink`/`sem_unlink` 掉配置的 IPC
  名字、删除 pid 文件、留下最终 `status.json` 供事后查。
- `SIGHUP` 转发给 Scheduler（策略热切换归它）；Scheduler 不在时记 `sighup_no_scheduler`。
- `SIGUSR1` 原子写 `runtime/status.json`（临时文件 + `rename`）并记 `status_dump`。
- 运行目录：`--runtime-dir` > `$KITCHENOS_RUNTIME_DIR` > `$XDG_RUNTIME_DIR/kitchenos` >
  `/tmp/kitchenos-<uid>`；`mkdir -p` 出 `logs/` 与 `runs/`。默认值刻意不是仓库路径：
  仓库可能挂在 9p 上，fcntl 锁与共享内存都要求 Linux 原生文件系统（decisions.md:
  2026-10-24 - 运行目录默认放 Linux 原生 tmpfs）。

```
<runtime>/
  kitchend.pid             Supervisor pid（fcntl 写锁）
  status.json              Supervisor 原子快照（SIGUSR1 与每次子进程状态变化）
  logs/supervisor.log
  logs/scheduler.log
  logs/worker-0.log …
  logs/bridge.log
  runs/<YYYYmmdd-HHMMSS>/  Scheduler 每局一个目录（W9）
```

### Scheduler

> **状态：**已实现（任务 02，W6/W7/W11）。订单来自 config/orders.json 的 menu 与 shm
> 世界状态；FCFS/EDF 在 `policy.h` 的接口后面；任务走 mq；SIGHUP 切换策略。

- 事件驱动主循环：订单到达、工序完成、资源释放、Worker 事件。
- 维护业务层订单模型（不能直接拿游戏订单池当任务队列）：

  ```text
  Order { id, dish, arrival, deadline, est_cost, reward, status,
          tasks: Task[] }
  Task  { id, order_id, op(fetch|cut|cook|mix|assemble|plate|deliver),
          resource, duration, prerequisites[], assigned_chef,
          status(ready|running|blocked|done|cancelled) }
  ```

  工序图来自 `config/recipes.json`（`nodes[].op/resource/duration`，`edges[]` 是
  子 → 父前置，`dishes[].nodes` 已拓扑序）。
- 订单级策略只有一个接口：

  ```c
  /* 只依赖订单视图与资源模型，不碰游戏 API、不碰 socket、不碰 shm 布局 */
  typedef struct kos_policy kos_policy;
  const char *kos_policy_name(const kos_policy *);
  int kos_policy_pick(const kos_policy *, const kos_order_view *orders, int n,
                      const kos_resource_view *res, int res_n,
                      int64_t now_ms, int *chosen_index);
  ```

  第一批实现 FCFS 与 EDF；SJF/HRRN/LLF 只要满足同一接口即可加（§4.3）。
- 工序级用列表调度：就绪队列 = 前置全部完成且所需资源可预约的工序；每个空闲 Worker 取
  优先级最高的就绪工序（关键路径优先，短工序与等待时间做加权），通过消息队列下发。
- 每次决策写 `runs/<ts>/decisions.jsonl`：策略名、参与比较的键值、选中项、理由。
- 每次派发/完成/失败/策略切换/资源申请与回收写 `runs/<ts>/events.jsonl`；
  `metrics.json` 在关机与 `SIGUSR1` 时用临时文件 + rename 原子写出（W10 本机部分）。
- 共享内存世界状态只读；不写输入槽（那是 Worker 的），不写世界状态（那是 Bridge 的）。
- `SIGHUP` 重读配置并切换策略；`SIGUSR1` 原子导出状态快照（临时文件 + rename）。

### Chef Worker

> **状态：**已实现（任务 03，W9/W11）。Worker 用 kitchen.json 的网格做 A*，按路径写
> move、到站写 interact，完成后通过 `/kitchenos_events` 回事件。

- 每个厨师一个进程，`--chef-id N` 绑定输入槽 N。
- 从自己的任务队列 `/kitchenos_tasks_<chef>` 收高层任务；向 `/kitchenos_events` 上报
  accepted/done/failed；资源令牌由 Scheduler 在派发前取得、完成后释放。
- 路径规划：在 `config/kitchen.json` 的 `grid.cells` + `grid.mask` 上做 A*（0.6 m 网格，
  `mask` 的位含义见 `edge_bits`），把"到哪个工位/哪一格"变成逐帧 `move`，到达后发
  `interact`/`pickup`，完成后回一个事件。
- 每帧（或每 `--slot-interval-ms`）把动作写进自己的输入槽并 `sem_post` 门铃；没有任务时
  写 `stop`。
- 不直接碰资源信号量：Scheduler 用 `sem_trywait` 取得令牌后才派发任务，并把
  `(kind, chef, task_id, acquired, deadline)` 记进租约表；Worker 只执行动作。
  这也保证令牌只有一个释放者，崩溃回收不会与 Worker 的释放竞争。
- 不解释订单/策略；不写别人的槽；不碰 socket。

### 共享资源

> **状态：**已实现（任务 04，W8）。Supervisor 按 kind 创建并 unlink
> `/kitchenos_res_<kind>`；Scheduler attach 后用 `sem_trywait`/`sem_post` 管理容量，
> 租约到期或 Worker 崩溃时回收且只回收一次；死锁图与 victim 规则已实现并有单元检查。

`config/kitchen.json` 的工位按 `kind` 归类：`cooker`（锅/煎锅）、`mixer`、`container`
（砧板/备料台）、`plate_stack`（盘子）、`dispenser`、`serving`、`sink`、`bin`。计数信号量
命名 `/kitchenos_res_<kind>[_<i>]`，容量取该类可用工位数（盘子取 `plate_stack` 的数量）。
信号量只保证互斥/计数，**不保证崩溃回收** —— 回收靠 Scheduler 的租约表（§6.4 方案 2），
因为 POSIX 命名信号量没有 robust 语义，而进程是独立进程不是线程，不能靠
`PTHREAD_MUTEX_ROBUST`。租约长度由 `--lease-ttl-ms` 配置（默认 30000 ms）。释放路径
统一为"先按 task_id 删除租约，删除成功才 `sem_post`"，因此重复的 DONE/FAILED、
Worker RECOVERED 与关机清理都不可能把同一令牌还两次。死锁检测维护资源分配图，检测到
环时按 decisions.md 的 victim 规则（环上最大 chef_id）回退一个持有者，并记
`deadlock_detected` / `deadlock_recovered`。当前每个任务只持有一个资源，所以正常调度
不会成环；检测器与单元测试为以后多资源任务保留。

### 进程间链路

| 链路 | 机制 | 谁写 → 谁读 | 理由 |
|---|---|---|---|
| 世界状态 | `shm_open("/kitchenos_state")` + seqlock | Bridge → 全部 | 已有实现；一写多读，拷贝一份自洽快照 |
| 输入槽 | 每厨师 seqlock + `/kitchenos_input_ready` | Worker → Bridge | 已有实现；每格独占写，门铃只表示"可能有人写了" |
| 任务 | POSIX mq，每厨师一条 `/kitchenos_tasks_<chef>` | Scheduler → Worker | 消息有边界、有优先级、可定时接收；按厨师分队列避免一个 Worker 取走别人的任务 |
| 事件 | POSIX mq `/kitchenos_events` | Worker → Scheduler | 多生产者单消费者，消息自带 chef_id/task_id |
| 心跳 | 匿名管道，每子进程一条 | 子进程 → Supervisor | 最基础的 IPC（§6.2.1）；只写 1 字节，无协议负担 |
| 单实例 | `fcntl` 写锁 | Supervisor | §6.2.5 明确要求 |
| 日志 | 每进程一个文件（单写者） | 各进程 | 避免多进程追加锁；跨进程事件用 mq 汇总到 Scheduler 的 jsonl |
| 游戏 | TCP | Bridge ⇄ 插件 | 已有实现 |

消息队列的**名字与定长消息布局**是跨进程契约：`/kitchenos_tasks_<chef>`、
`/kitchenos_events`，消息是 `kos_mq_msg`（`mq.h`）。Scheduler 创建并在退出时
`mq_unlink`；Supervisor 在关闭末尾对同一批名字再扫一遍（幂等），兜住 Scheduler 被
`SIGKILL` 的情况。锁住它的是 `test_policy` 的 `mq_roundtrip`/`mq_priority_orders_messages`
与 `test_supervisor` 的 `cleanup_logged`。每局消息带
`{task_id, order_id, chef_id, priority, deadline_ms, op, resource, duration_ms, payload}`。

### 信号

| 信号 | 发给 | 处理 |
|---|---|---|
| SIGCHLD | Supervisor | 置标志；主循环 `waitpid(-1, WNOHANG)` 循环收尸，决定重启 |
| SIGALRM | Supervisor | 置标志；`setitimer` 周期健康检查（心跳管道 + `kill(pid,0)`） |
| SIGTERM / SIGINT | 全部 | 置停止标志；Supervisor 按序停子进程后清理 IPC；子进程记日志后退出 |
| SIGHUP | Supervisor → Scheduler | Supervisor 只转发；Scheduler 重读配置/切策略 |
| SIGUSR1 | Supervisor / Scheduler | 原子导出状态快照（Supervisor 已实现；Scheduler 的在任务 02） |
| SIGPIPE | 全部 | 忽略；写管道/ socket 失败走返回码 |

所有处理函数用 `sigaction` 注册，函数体只写 `volatile sig_atomic_t`。

### 评分与指标

> **状态：**未实现（任务 02/04，W10）。

`runs/<YYYYmmdd-HHMMSS>/events.jsonl` 每行一个事件（订单到达/开始/完成/超时、任务、
资源申请/获得/释放/回收、进程重启），`decisions.jsonl` 每行一次策略选择，
`metrics.json` 是同一局的汇总：完成数、超时率、平均等待、厨师利用率、资源等待、
调度耗时、重启次数。字段名固定为 `score/completed/expired/avg_wait_ms/chef_busy_ms/
resource_wait_ms/decisions/restarts`，供对比脚本读。

## How it is built

| 模块 | 负责 | 不负责 |
|---|---|---|
| `daemon` | 运行目录解析、`mkdir -p`、pid 文件 fcntl 锁、标准 daemonize + 就绪管道 | 监督策略、子进程表 |
| `supervise` | 子进程状态机：崩溃窗口计数、退避延迟、`given_up` 判定（纯函数，可单测） | fork/exec、信号、日志 |
| `child` | Scheduler/Worker 共用的启动骨架：参数、日志、挂接 shm 重试、心跳、SIGTERM/SIGHUP | 调度、路径规划 |
| `supervisor`（可执行） | 上述模块 + fork/exec、SIGCHLD/SIGALRM、状态文件、关闭顺序、IPC 清理 | 任何业务策略 |
| `kitchen` / `recipes` | 解析配置、网格索引、工序 DAG、关键路径与就绪判定 | 调度决策、路径 |
| `policy` | FCFS / EDF 的订单选择 | 工序图、资源分配、IPC |
| `nav` | A* 与邻接判定 | 工位语义、动作发布 |
| `worker_task` | 单个工序的移动/交互/工作状态机 | 选任务、写共享内存 |
| `mq` | 创建/打开/定时收发定长消息 | 调度策略 |
| `scheduler`（可执行） | 订单/工序模型、策略接口、mq 派发、租约表、runs/ 记录 | 游戏 API、shm 布局内部 |
| `worker`（可执行） | A*、每帧输入槽、资源信号量、事件上报 | 选单、决定优先级 |
| `bridge`（已有） | TCP、帧屏障、世界状态 | 调度、路径 |

头文件是模块契约，都在 `bridge/include/kitchenos/`：

| 头文件 | 职责 |
|---|---|
| `kitchen.h` / `recipes.h` | 读 kitchen.json 的网格/工位与 recipes.json 的 DAG、关键路径 |
| `policy.h` | 只吃订单/资源视图的 FCFS/EDF 接口 |
| `nav.h` | 网格 A* |
| `worker_task.h` | move → interact → work → done 状态机 |
| `mq.h` | 每厨师任务队列 / events 的名字与定长消息布局 |

可执行程序的契约是命令行开关加日志事件名；MQ 两端只通过 `mq.h` 解释字节。

## What it must not do

- Supervisor 不做调度、不解析游戏状态；它只保证"该活的进程活着、该死的时候死干净"。
- Scheduler 不碰游戏 API、不写世界状态、不写输入槽；它只产出任务与决策。
- Worker 不选单、不算优先级；它只把分配到的工序走完。
- Bridge 仍然不解释策略（既有约束不变）。
- 插件仍然不调度、不做 A*（既有约束不变）。
- 不在插件/Windows 侧放监督进程。
- 不用线程替代进程；不用第三方 C 库；不引入条件编译区分平台。
- 多进程不共写同一个日志文件（单写者，除非后来改成 fcntl 追加锁并给出检查）。
- 文档里写了行为就必须能指出锁住它的检查；没有检查的写"未验证"。

## What was considered and rejected

- **Scheduler→Worker 用管道而不是 POSIX mq。** 管道更简单，且能塞进 Supervisor 已有的
  fork 结构里。被否掉：管道没有消息边界与优先级，EDF 算出的优先级要自己编码成帧再排序；
  Worker 数量在启动时确定，每厨师一条 mq 的名字稳定；mq 的 `mq_timedreceive` 让 Worker
  没有任务时不必阻塞在无法响应 SIGTERM 的读上。见 decisions.md:
  2026-10-24 - 任务/事件链路用 POSIX 消息队列。
- **所有进程追加同一个 `logs/kitchenos.log`，用 fcntl 锁保护。** 好处是只有一份时间线。
  被否掉：fcntl 锁只保护单次 `write`，多行事件的原子性还得自己保证；一个进程写坏日志会
  影响全部。每进程一个文件本来就是 §6.6 的建议，且 Scheduler 的 jsonl 已经把跨进程事件
  汇总成一条时间线。见 decisions.md: 2026-10-24 - 日志每进程一个文件、单写者。
- **pid 文件放 `/var/run/kitchend.pid`（§6.1 原文）。** 被否掉：课程实验室机器上非 root
  用户写不了 `/var/run`，而仓库可能挂在 9p 上，fcntl 锁的语义不可靠；运行目录必须可配置
  且默认 Linux 原生。见 decisions.md: 2026-10-24 - 运行目录默认放 Linux 原生 tmpfs。
- **PTHREAD_MUTEX_ROBUST 回收共享资源。** §6.4 方案 1。被否掉：robust mutex 是
  `pthread_mutex`，放在共享内存里对**独立进程**虽然可行，但它保护的是临界区而不是"锅被
  谁占着"这种带身份的租约；租约能在日志里说清谁持有、何时到期、为什么被回收，也更贴合
  "检测并恢复死锁"的课程要求。见 decisions.md: 2026-10-24 - 共享资源用计数信号量 + 租约。
- **严格按 §6.1 的退避表写成函数里的 if，不暴露参数。** 被否掉：崩溃循环要能在测试里
  几秒内复现，窗口/次数/退避必须可配；默认值仍取 §6.1 的 0/2 s/10 s、5 次/60 s。
- **Scheduler 负责 A*，Worker 只发原始动作。** 被否掉：路径要按每个厨师当前位置算，
  而位置每帧在变；放在 Worker 里就不用把整张路径通过 mq 传，且 Worker 崩溃重启后能自己
  重新规划。见 decisions.md: 2026-10-24 - 路径规划放 Worker。

## How we will know it works

**W1. 守护进程与单实例（已实现，有检查）。** `--foreground` 与 daemon 两种启动方式都能
跑；同一运行目录的第二个实例退出码 3 且不写第一个实例的日志；pid 文件在退出后删除。
锁住它的是 `bridge/tests/test_supervisor.c` 的 `pidfile_acquire_blocks_second`、
`pidfile_release_allows_next`、`second_supervisor_refused`、`sigterm_removes_pidfile`。

**W2. SIGCHLD 收尸、重启与崩溃上限（已实现，有检查）。** `kill -9` 一个 Worker 后它在
退避时间内被重新拉起，日志有 `child_exit` 与 `child_restart`；窗口内达到
`--crash-limit` 后记 `child_given_up` 且不再重启，Supervisor 本身继续运行。
锁住它的是 `test_supervisor.c` 的 `backoff_schedule_matches_design`、
`crash_window_resets`、`crash_limit_gives_up`、`restart_after_sigkill`、
`crash_loop_gives_up`。

**W3. SIGTERM 有序关闭与清理（已实现，有检查）。** 所有子进程消失、pid 文件删除、
`/dev/shm` 下没有本实例的 shm/sem 残留；关闭顺序在日志里是 Workers → Scheduler →
Bridge。锁住它的是 `sigterm_children_gone`、`sigterm_unlinks_ipc`、
`shutdown_order_workers_first`、`cleanup_without_bridge`（Bridge 没参与时也必须由
Supervisor 清掉，证明清理不是 Bridge 顺手做的）。

**W4. 健康检查与心跳管道（已实现，有检查）。** `SIGALRM` 周期触发并在日志留
`health_tick`；停止写心跳的子进程在超时后被记 `child_unresponsive` 并被重启；Supervisor
被 `SIGKILL` 后子进程因管道断开自行退出，不留孤儿。锁住它的是
`health_tick_logged`、`silent_child_restarted`、`orphans_exit_when_pipe_breaks`。

**W5. SIGHUP / SIGUSR1（已实现，有检查）。** `SIGHUP` 转发到 Scheduler（Scheduler 日志
出现 `signal name=SIGHUP`）；`SIGUSR1` 写出可解析的 `status.json` 且含全部子进程。
锁住它的是 `sighup_forwarded`、`status_dump_on_sigusr1`。

**W6. Scheduler 订单级策略（已实现，有检查）。** 同一组订单分别跑 FCFS 与 EDF，
选择与手算一致；EDF 在截止期相同时退化为 FCFS。锁住它的是
`bridge/tests/test_policy.c` 的 `fcfs_orders_by_arrival`、`edf_orders_by_deadline`、
`edf_ties_match_fcfs`、`policy_empty_rejected`、`policy_kind_from_name_rejects`。

**W7. 工序级列表调度（已实现，有检查）。** 给定 `recipes.example.json` 的 DAG，
就绪队列只在前置完成后出现该工序；同一份输入两次运行产生相同的派发序列。锁住它的是
`test_policy.c` 的 `ready_queue_cook_blocked_until_fetch`、
`ready_queue_cook_ready_after_fetch`、`ready_queue_plate_blocked_until_cook`、
`critical_path_orders_tasks`、`list_schedule_is_deterministic`。

**W8. 共享资源与租约回收（已实现，有检查）。** Supervisor 为每个 kitchen kind 建
`/kitchenos_res_<kind>` 计数信号量并在关机时 unlink；Scheduler 用 `sem_trywait` 取得
令牌、记租约，`DONE`/失败只释放一次，租约到期或 Worker `RECOVERED` 也只回收一次。
`kill -9` 持锅 Worker 的场景在 `tools/fake_plugin.py --scenario os-resources`：一个
cooker、两个厨师，确认持 cooker 租约时 kill，随后 `worker_recovered` +
`resource_reclaimed`，另一个厨师继续并记录 `resource_wait`。锁住它的检查是
`test_resources.c` 的 `wait_then_acquire`、`lease_expiry_reclaims`、
`deadlock_cycle_detected`、`first_remove_returns_one`/`second_remove_returns_zero`，
以及场景的 `worker_killed_while_holding_lease`、
`killed_lease_reclaimed_by_worker_death`、`lease_expired_event`、
`contention_observed`、`other_chef_acquires_after_reclaim`、
`sem_value_back_to_capacity`、`no_semaphore_left_after_stop`。

**W9. Worker 路径与输入槽（已实现，有检查）。** 从 `kitchen.json` 任意可走格到任意工位
相邻格，A* 路径的每一步都满足 `grid.mask`；Worker 按路径写 `move`，到达后写 `interact`。
锁住它的是 `test_worker_nav.c` 的 `path_respects_mask`、
`path_reaches_station_neighbour`、`grid_mask_is_symmetric_along_path`、
`slot_contains_move_then_interact`、`worker_task_ends_with_stop`；Worker 重启的恢复由
os-loop 的 `worker_restart_detected`、`worker_restart_requeues_task`、
`worker_restart_gets_new_task`、`worker_process_restarted` 锁住。

**W10. 指标与实机闭环（本机部分已实现，实机未验证）。** 一局结束（以及 `SIGUSR1`）
生成 `runs/<ts>/{events.jsonl,decisions.jsonl,metrics.json}`；Supervisor 把 SIGUSR1
转发给 Scheduler（自己仍写 status.json），`metrics.json` 先写 `metrics.json.tmp` 再
rename，字段含订单 completed/failed/lateness、每资源
capacity/in_use/acquires/busy/wait/utilization、两策略 dispatch 计数与
worker_restarts。锁住本机部分的是 `test_metrics.c` 的 `run_dir_layout`、
`metrics_fields_complete`、`no_tmp_file_left`、`contents_fully_replaced`，以及
os-resources 场景的 `metrics_sigusr1_snapshot`、`metrics_fields_complete`、
`metrics_resource_wait`、`metrics_worker_restarts`；`tools/compare-runs.py` 读取两局
metrics 的 completed/failed/expired/avg_wait。**W10 的真游戏部分（实机两个厨师自动完成至少一单）仍未运行、
未验证**；运行脚本存在但不在本任务的证据里。

**W11. 无游戏闭环（已实现，有检查）。** `tools/fake_plugin.py --scenario os-loop`
启动 Supervisor 整棵树，经 Bridge 注入世界状态，Scheduler 派任务、Worker 走位写槽、
Bridge 把 move/interact 发回对端；SIGHUP 后 decisions.jsonl 里出现 EDF。锁住它的是
场景的 `scheduler_dispatched_each_chef`、`task_events_each_chef`、
`worker_move_forwarded_each_chef`、`worker_interact_forwarded_each_chef`、
`worker_slot_nonstop_move`、`decisions_name_fcfs`、`sighup_flips_policy`、
`worker_restart_requeues_task`、`worker_restart_gets_new_task`、
`supervisor_clean_shutdown`。

**当前实测（任务 04 交付，2026-10-24）：** `cd bridge && make clean && make test`
退出码 0，**10 个套件 0 失败**（framing 194 + json 166 + net 204 + policy 36 +
protocol 226 + shm 95 + supervisor 67 + worker_nav 17 + resources 35 + metrics 37 =
**1077 项检查**）。`bash tools/run-bridge-scenarios.sh` **13/13 场景通过**，新增
`os-resources`（一个 cooker、两个厨师、租约到期、持租约 Worker 被 SIGKILL）。
W8、W10 的本机部分都做了反证（临时破坏 → 对应检查失败 → 恢复 → 全绿），记录在任务 04
的 Progress；W1–W7、W9、W11 的反证记录在任务 01/02/03 的 Progress。

`make test-san` **原样在本机链接失败**：gcc 16 的 `libasan.so` 是只指向
`/usr/lib64/libasan.so.8.0.0` 的链接脚本，而 `libasan`/`libubsan` 运行时包没安装
（安装需要 root）。作为非 root 绕过，把发行版 rpm 下到 `/tmp/kos-san-libs`、解包后用
`LD_LIBRARY_PATH=/tmp/kos-san-libs/usr/lib64 make test-san
LDFLAGS="-pthread -lrt -L/tmp/kos-san-libs/lib"` 重跑：**10 套件 0 失败、同样
1077 项检查、无 ASan/UBSan 报告**。这套临时库路径没有写进 Makefile。

**W10 的真实游戏部分仍未验证**：本任务交付了 metrics 生成与读取脚本
（`tools/compare-runs.py`），但没有启动游戏，也没有实机两个厨师完成订单的录像/日志。
文档不声称 W10 实机通过。

### 值得测的边界

两个 Supervisor 同时启动、子进程启动即退出、子进程忽略 SIGTERM、`--workers 0`、
`--workers` 超过 `KOS_SHM_MAX_CHEFS`、运行目录不存在/只读、pid 文件存在但无人持锁、
Bridge 端口被占、Scheduler 先于 Bridge 启动（挂接重试）、MQ 名已存在、心跳管道被写满。
