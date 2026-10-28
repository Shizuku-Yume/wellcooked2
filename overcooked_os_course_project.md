# 操作系统课设：厨房协作调度系统

> **路线决策（已确认）：**
> 1. **必须先在实际游戏跑通**——游戏接入是项目基础，不是可选项；模拟器只是辅助工具。
> 2. **双实验室并行**：本机 Fedora 原生 Linux（Proton 运行游戏）+ 组员 WSL2/Windows 混合环境（游戏原生运行）。
> 3. **OS 核心代码统一**：同一份 POSIX 实现，两个平台都无条件编译运行，不用条件编译区分平台。
> 4. **游戏插件统一**：BepInEx + Harmony，协议层与宿主平台无关。

## 1. 项目概述

### 1.1 项目名称

暂定：**基于多任务调度的多人厨房协作系统**

英文可用：**WellCooked: Multi-Agent Cooking Scheduler**

以《胡闹厨房》式合作厨房作为可视化载体，把订单、加工工序和共享厨房设备抽象为操作系统中的作业、任务、资源和同步事件。调度系统自动控制多个厨师角色，在有限时间内安排订单和工序，**以原作游戏得分作为唯一优化目标**。

本项目的核心是展示操作系统原理在实际协作任务中的应用，而不是单纯做一个游戏AI。重点展示：
- 进程控制：fork、exec、wait、守护进程创建；
- 信号处理：SIGCHLD、SIGALRM、SIGTERM、SIGHUP、SIGUSR1；
- 进程间通信：管道、POSIX消息队列、共享内存、信号量；
- 同步互斥：锅、切菜台、盘子等共享资源的互斥访问，读者写者问题；
- 文件系统：日志、pid文件锁、检查点原子写入；
- 调度算法：FCFS、SJF、EDF、HRRN等在多厨师任务分配中的应用；
- 可靠性：守护进程健康检查、故障重启、持锁进程崩溃恢复。

### 1.3 现实问题背景

本项目可以类比以下现实场景：

- **外卖高峰期中央厨房调度**：多个厨师共享有限的灶台、切菜台和烹饪设备，需要在订单截止时间前完成并交付，同时最大化吞吐量和客户满意度。
- **多机器人仓储协作**：多个AGV机器人在仓库中搬运货物，需要协调路径、避免碰撞、共享充电桩和货架资源。
- **医院手术室排班**：多个手术间和医疗团队需要处理紧急度不同的手术任务，同时考虑设备占用、术前准备和恢复时间。

《胡闹厨房2》作为这类问题的数字孪生，提供了可视化、可重复、可量化的实验环境，同时保留了真实场景中的资源竞争、时间约束和协作复杂性。

### 1.4 目标用户体验

启动系统后，用户看到的是接近商业游戏的厨房画面，而不是简陋的算法动画：

1. 选择一张固定厨房地图和一组订单；
2. 选择调度策略，例如FCFS、SJF、EDF或改进策略；
3. 系统自动控制两名或多名厨师完成取料、加工、烹饪、装盘和交付；
4. 游戏画面展示实际行动；
5. 旁侧控制台展示订单队列、任务状态、资源占用、锁等待、调度决策、得分和日志；
6. 可以在相同订单输入下切换策略并比较结果；
7. 可以停止业务进程，展示守护进程发现故障并重新拉起服务。
## 2. 课件要求映射

课件文件：`/home/shizuku/下载/操作系统原理课程设计.pdf`

### 2.1 必须满足的要求

| 课件内容 | 项目中的对应实现 |
|---|---|
| 结合实际问题实现较完整的软件系统 | 合作厨房调度系统，包含游戏端、调度端、监督端、日志和实验面板 |
| 进程调度、线程调度 | 订单/工序作为可调度任务，厨师作为执行资源；实现FCFS、SJF、EDF等策略 |
| 同步/互斥处理 | 锅、切菜台、盘子、交付台等共享资源的互斥预订和释放 |
| 消息传递、管道、消息队列或共享内存 | 游戏插件、调度器、监督进程之间采用明确的IPC接口，至少落地一种IPC机制 |
| 信号处理 | SIGCHLD子进程退出、SIGALRM定时健康检查、SIGTERM优雅退出、SIGHUP热切换策略、SIGUSR1状态快照 |
| 文件处理和文件锁 | 订单配置、运行日志、结果文件、检查点或重放数据；多进程写日志需加锁或统一写入 |
| 图形界面 | 原作游戏画面加调度监控面板 |
| 必须有守护进程 | 独立Supervisor Daemon后台运行，负责业务进程健康检查、异常重启和日志记录 |

### 2.2 守护进程最低实现

课件第7—12页明确强调守护进程，并介绍了`fork`、`setsid`、`umask`、工作目录、文件描述符重定向和周期任务等流程。本项目至少应做到：

- Supervisor以独立后台进程运行；
- 监督Scheduler和必要的Worker进程；
- 通过心跳、管道消息或进程状态检测服务是否存活；
- 服务异常退出时自动重启；
- 记录启动、停止、异常和重启次数；
- 正常退出时清理子进程和IPC资源；
- 能在答辩现场停止Scheduler，展示Supervisor重新拉起它。

“普通后台线程”不能作为守护进程的唯一实现。两个实验室的守护进程都必须是完整的 Linux 守护进程：本机组跑在 Fedora 原生，组员组跑在 WSL2 内；Windows 侧不放监督进程。实现细节见第6章。

### 2.2.1 双实验室部署方案

两个方案**并行推进、代码统一**，不分主次；互为参考和兜底。

#### 实验室甲：Fedora + Proton（本机）

**架构**：
```
Fedora Linux
├── Supervisor守护进程（标准Linux daemon）
├── Scheduler进程（POSIX API）
├── IPC机制（管道、共享内存、信号量、消息队列）
└── 游戏通过Proton运行
    └── BepInEx插件 → TCP连接到Linux调度器
```

**优势**：
- OS核心完全符合课件要求（fork、setsid、信号等）
- 守护进程实现标准，答辩时无需解释对应关系
- 展示Linux环境下的完整系统编程

**需要验证**：
- Proton环境下BepInEx + Harmony稳定性
- Mono/.NET在Wine中的兼容性

**负责人**：shizuku（本机Fedora环境）

---

#### 实验室乙：WSL2 + Windows（组员环境）

**架构**：
```
WSL2 (Ubuntu/Fedora)              Windows 11
├── Supervisor守护进程            ├── 游戏本体（Steam原生）
├── Scheduler进程                 └── BepInEx + Harmony插件
├── IPC机制（完整POSIX）               │
└── TCP Server (0.0.0.0:12345) ←──────┘
         跨系统边界通信
```

**特点**：
- OS核心在WSL2中完全按Linux标准实现，与实验室甲同一份代码；
- 游戏在Windows原生运行，稳定性高，插件开发调试方便；
- TCP跨WSL-Windows边界，属跨系统IPC，答辩时可展开讲；
- 可展示"Linux守护进程监控运行在Windows上的游戏进程"。

**需要验证**：
- Windows → WSL2 的TCP连通性（先试 localhost，不行再用 `ip addr show eth0` 取IP）；
- Windows防火墙规则；
- WSL2 网络模式（NAT 或 mirrored）。

**负责人**：组员（WSL2 + Windows环境）

---

#### 技术栈统一要求

**OS核心层（两个实验室完全相同）**：
- 语言：C/C++或Rust
- API：纯POSIX（fork, setsid, pipe, sem_open, mmap, shm_open, signal）
- 通信：TCP socket（跨平台标准）
- 文件锁：fcntl或flock
- **不使用条件编译区分平台**

**游戏插件层（两个实验室完全相同）**：
- 语言：C# (.NET Framework 4.8 或 .NET 6+)
- 框架：BepInEx 5.x + Harmony 2.x
- 功能：读取游戏状态、控制角色、TCP客户端
- **通信协议与平台无关**（JSON或Protobuf）

**唯一差异**：
- 网络配置：Fedora用localhost直连；WSL2可能需要取IP或配端口转发
- 启动脚本：Fedora用shell脚本；WSL2常用 .sh + Windows侧 .bat 启动器
- 游戏运行位置：Fedora在Proton下；WSL2方案在Windows原生

详细排期见第9节"实施计划"。

---

### 2.2.2 守护进程标准实现

课件第7—12页给出的标准流程必须严格遵守：

```c
// 守护进程创建流程（课件标准）
int create_daemon() {
    // 1. fork创建子进程，父进程退出
    pid_t pid = fork();
    if (pid > 0) exit(0);  // 父进程退出
    if (pid < 0) return -1;
    
    // 2. 子进程调用setsid()创建新会话
    if (setsid() < 0) return -1;
    
    // 3. 设置umask
    umask(022);
    
    // 4. 修改工作目录
    chdir("/home/wellcooked/");  // 或项目目录
    
    // 5. 关闭继承的文件描述符
    for (int fd = 0; fd < sysconf(_SC_OPEN_MAX); fd++) {
        close(fd);
    }
    
    // 6. 重定向标准输入输出到/dev/null
    int fd = open("/dev/null", O_RDWR);
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    
    // 7. 核心业务逻辑
    supervisor_main_loop();
    
    return 0;
}
```

### 2.3 课件进度与交付

课件第13页给出的节奏：

| 时间 | 应完成内容 |
|---|---|
| 第1周 | 组队、分工、熟悉API、确定题目 |
| 第2周 | 需求分析、概要设计 |
| 第3周 | 小组方案汇报，提交PPT、UML/原型/思维导图等 |
| 第4—5周 | 集中设计、编码、联调 |
| 第6周 | 展示可运行系统、交流、验收，提交代码 |
| 第7周 | 个人整理汇报PPT |
| 第8周 | 个人答辩、提交材料 |

### 2.4 评分关注点

课件给出的总成绩结构：

- 平时成绩：10%；
- 课堂实验设计：15%；
- 实验运行：15%；
- 答辩：60%。

实验运行重点：

- 能运行，守护进程有效；
- 功能完整且符合需求；
- 用户体验和界面友好；
- 性能和资源利用率；
- 可靠性和异常处理；
- 创新性。

个人答辩重点：

- 需求分析和理解；
- 详细设计，建议准备时序图；
- 运行结果和问题改进；
- OS基本原理应用是否充分；
- 选题价值和创新性；
- Gitee提交质量。

## 3. 需求定义

### 3.1 功能需求

#### 核心功能

- 启动和结束一局厨房任务；
- 加载固定地图和订单序列；
- 读取角色、订单、工作台和烹饪状态；
- 生成、排序并分配任务；
- 控制多个厨师移动和交互；
- 处理任务前置依赖；
- 处理锅、切菜台、盘子等共享资源；
- 记录得分、完成订单、超时订单和失败原因；
- 在相同输入下切换调度算法；
- 输出可比较的实验结果；
- 监督进程并在业务进程异常时重启。

#### 展示功能

- 厨师当前任务、位置、手持物和状态；
- 订单队列及每个订单的到达时间、预计工时、截止时间、收益和进度；
- 共享资源状态：空闲、占用、等待、故障；
- 当前调度策略和最近一次决策原因；
- IPC连接状态、心跳状态和进程状态；
- 当前分数、完成数、超时率、平均等待时间和厨师利用率；
- 调度事件时间线和运行日志。

### 3.2 非功能需求

- 能在一台演示机上稳定运行；
- 核心演示不依赖网络；
- 配置和订单可通过文件复现实验；
- 调度策略与游戏接入层解耦；
- 进程异常后不会让整个系统无提示地失效；
- 所有成员都有可解释、可演示的代码贡献。

### 3.3 范围控制

#### 第一版必须完成

- 一张固定地图；
- 两名自动厨师；
- 少量固定订单；
- FCFS、SJF、EDF、HRRN四种基线策略；
- 一种资源感知的改进策略；
- 游戏状态读取和动作控制；
- IPC；
- 至少一个真实互斥资源；
- Supervisor守护进程；
- 运行日志和策略对比面板。

#### 有余力再完成

- 四名厨师；
- 多地图；
- 动态订单到达；
- 任务抢占或优先级老化；
- 断点恢复；
- 更精细的资源死锁检测；
- 录制和回放；
- 更复杂的策略优化。

#### 不应作为第一阶段目标

- 强化学习训练；
- 直接修改或重做整个商业游戏；
- 支持所有关卡和所有版本；
- 同时维护多个游戏外壳（原作、Unity复刻、二维模拟）；

## 4. 调度模型

### 4.1 基本抽象

| 厨房对象 | 操作系统抽象 |
|---|---|
| 订单 | 作业/任务集合 |
| 取料、切菜、烹饪、装盘、交付 | 具有前置关系的子任务 |
| 厨师 | 执行任务的处理资源 |
| 锅、切菜台、盘子 | 互斥或有限容量共享资源 |
| 等待锅空闲、等待盘子、等待加工完成 | 阻塞/等待状态 |
| 任务完成 | 事件通知和任务唤醒 |
| 订单截止时间 | 调度优先级和超时约束 |
| 订单收益 | 目标函数的一部分 |

注意：这是应用层调度模型，不是替换Linux内核调度器。系统要展示的是操作系统原理在实际协作任务中的应用。

### 4.2 任务数据结构

每个订单建议包含：

```text
Order {
  id: string
  recipe: string
  arrival_time: int
  deadline: int
  estimated_cost: int
  reward: int
  status: pending/running/completed/expired/failed
  subtasks: Task[]
}
```

每个工序建议包含：

```text
Task {
  id: string
  order_id: string
  type: get/cut/cook/plate/deliver
  estimated_duration: int
  required_resource: string?
  prerequisites: TaskId[]
  assigned_chef: int?
  status: ready/running/blocked/completed/cancelled
}
```

游戏原生的订单池不应直接当作完整的操作系统任务队列。业务层需要维护自己的订单模型，包含到达时间、预计工时和截止时间；游戏端负责真实规则、状态和得分反馈。

**关于游戏原生计分：**

《胡闹厨房2》的小费倍率机制会影响调度策略的比较：
- 社区资料显示，按界面从左到右顺序交付订单可以维持小费倍率（最高x4），乱序交付会打断倍率；
- 这意味着订单级的FCFS可能天然接近游戏得分最优；
- 若订单时限相同，EDF和FCFS排出的顺序完全一致。

应对策略：

1. **订单生成控制**：使用Harmony补丁接管游戏的订单生成器，按orders.json定义的到达时间、菜谱和时限生成订单，保证画面显示和业务模型一致；
2. **调度重心下沉到工序级**：真正有深度的问题不是"先做哪一单"，而是"N个厨师如何并行执行一张带前置约束的工序图"。这是多处理机调度问题（列表调度、关键路径优先），比单纯的订单排序更符合OS调度原理。

### 4.3 调度策略

#### 订单级策略

用于决定"接下来优先推进哪个订单"。

**FCFS：先来先服务**

- 按订单到达时间排序；
- 最简单的基线策略；
- 在小费倍率机制下可能接近游戏得分最优；
- 可能导致短订单被长订单阻塞。

**SJF：短作业优先**

- 按预计剩余制作时间排序；
- 目标是降低平均等待时间、提高吞吐量；
- 可能造成长订单饥饿，需要在日志中记录该现象。

**EDF：最早截止期优先**

- 按订单截止时间排序；
- 目标是降低超时率；
- 若所有订单时限相同，退化为FCFS；
- 需要判断前置工序和资源是否可用。

**HRRN：高响应比优先**

- 响应比 = (等待时间 + 预计服务时间) / 预计服务时间
- 短作业优先，但长时间等待的作业会逐步提高优先级；
- 天然抗饥饿，比手工设计starvation_bonus更有理论依据；
- 课本经典算法，答辩时容易解释。

**LLF：最小松弛度优先**

- 松弛度 = 截止时间 − 当前时间 − 剩余工时
- 松弛度越小，留给该订单的富余时间越少，优先级越高；
- 比EDF多考虑了剩余工时，更精确地反映紧迫性；
- 适合截止时间和工作量都有差异的场景。

#### 工序级策略

用于决定"N个厨师并行执行哪些工序"。

这是带资源约束的多处理机调度问题，每个订单可以展开成一张有向无环图（DAG），节点是工序，边是前置依赖。

**列表调度（List Scheduling）**

1. 为每个工序计算优先级（如最早开始时间、关键路径长度）；
2. 维护就绪队列：前置工序已完成且资源可用的工序；
3. 每个空闲厨师从就绪队列取优先级最高的工序执行。

**关键路径优先（Critical Path First）**

- 计算每个工序到订单完成的最长路径长度（关键路径）；
- 优先调度关键路径上的工序；
- 目标是缩短订单总完成时间。

**资源感知调度**

改进启发式，综合考虑：

```text
priority(task) =
  α * urgency(task.order)              // 所属订单的紧迫度
+ β * (1 / estimated_duration)         // 短工序优先
+ γ * resource_availability           // 资源空闲时优先占用
+ δ * path_criticality                // 关键路径上的工序优先
- ε * chef_travel_distance            // 移动成本低的优先
+ ζ * wait_time                       // 等待过久的提高优先级
```

权重通过在模拟器上网格搜索确定，记录搜索过程和最优配置。

#### 锅-CPU类比：I/O设备调度

锅和炸锅是自动烹饪设备，类似I/O设备或DMA：
- 厨师把食材放进锅，烹饪自动进行（类似发起I/O请求）；
- 烹饪完成触发事件（类似I/O中断）；
- 厨师在锅工作期间可以切换到其他任务（CPU-I/O重叠）。

切菜、洗盘需要厨师持续操作，类似CPU计算任务。

好的调度应该：
- 优先启动烹饪任务，然后让厨师去切菜；
- 避免厨师空等锅完成；
- 最大化CPU和I/O设备的并行利用率。

这对应老师的急诊室示例："病人做检查时产生的碎片时间"——医生可以同时处理其他病人，不应该空等检查结果。

#### 调度层次类比

可以对应操作系统的两级调度：

1. **作业调度（订单级）**：决定哪些订单进入活跃队列，控制并发订单数；
2. **进程调度（工序级）**：把活跃订单的就绪工序分配给N个厨师。

厨师从一个工序切换到另一个工序时，移动距离相当于上下文切换开销，可以统计"任务切换次数"和"总移动距离"作为额外指标。

### 4.4 实验与最优性

**关于"最优调度"：**

带前置约束、资源约束和移动成本的多厨师调度是NP难问题，在线算法无法保证全局最优。

但可以计算离线最优解作为上界（类似页面置换里的Belady OPT）：

- 对小规模实例（2个厨师、5个订单、10个工序），用分支限界、束搜索或整数线性规划（ILP）求解离线最优；
- 在模拟器上运行在线策略，报告达到离线最优的百分比；
- 这样"最优调度"有可验证的定量含义，也是一个好的创新点。

**实验设计：**

主要实验在自研离散事件模拟器上进行：

- 固定地图和N组订单序列；
- 每组订单在每种策略下重复运行K次（模拟器可加速且确定性）；
- 记录每次的得分、完成数、超时率、等待时间、资源利用率；
- 用箱线图或误差棒展示分布，用统计检验（t检验或Wilcoxon）判断策略间差异显著性。

原作游戏用于：

- 最终演示和视觉呈现；
- 抽样验证模拟器的准确性（对比几组典型订单的得分）。

不应该只在原作上跑一局就下结论，因为：
- 原作一局要几分钟，跑不够重复次数；
- 物理引擎不完全确定，"相同输入"也可能有细微差异。

### 4.5 评价指标

在相同地图、订单序列和时间限制下比较：

- **游戏原生得分**（主要目标）；
- 完成订单数；
- 超时订单数和超时率；
- 平均订单等待时间；
- 厨师空闲时间和利用率；
- 共享资源等待时间；
- 资源冲突次数；
- 调度决策耗时；
- 进程重启次数和恢复时间；
- 达到离线最优的百分比（若计算了OPT）。

## 5. IPC机制与通信协议

本节给出各类IPC的使用场景与协议设计；具体的实现清单和进程职责见第6章。

### 5.1 各IPC机制的使用场景

#### 管道（Pipe）

**用途**：守护进程 ↔ 调度器进程的心跳与控制指令。

```c
int pipe_fd[2];
pipe(pipe_fd);

pid_t scheduler_pid = fork();
if (scheduler_pid == 0) {
    close(pipe_fd[1]);          // 子进程：关写端，读命令
} else {
    close(pipe_fd[0]);          // 父进程：关读端，写命令/读心跳
}
```

**答辩演示**：父子进程通过管道通信，符合课件要求。

---

#### 共享内存 + 信号量（Shared Memory + Semaphore）

**用途**：多进程共享的世界状态与资源占用表（锅、切菜台、盘子）。

```c
int shm_fd = shm_open("/wellcooked_state", O_CREAT | O_RDWR, 0666);
ftruncate(shm_fd, sizeof(SharedState));
SharedState *state = mmap(NULL, sizeof(SharedState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

sem_t *sem = sem_open("/wellcooked_sem", O_CREAT, 0666, 1);
sem_wait(sem);
/* 临界区：检查并占用锅 */
sem_post(sem);
```

**为什么不用普通互斥锁**：`pthread_mutex_t` 默认只在同进程线程间有效，跨进程需要设置 `PTHREAD_PROCESS_SHARED` 属性，或直接使用 POSIX 信号量。

**答辩演示**：多进程共享内存 + 信号量保护临界区。

---

#### POSIX消息队列（Message Queue，可选）

**用途**：调度器向工作进程分发任务（如果采用多进程worker架构）

```c
// 调度器创建消息队列
mqd_t mq = mq_open("/wellcooked_tasks", O_CREAT | O_WRONLY, 0666, NULL);

// 发送任务
TaskMessage task = {.chef_id = 0, .action = MOVE, .x = 10, .y = 5};
mq_send(mq, (char*)&task, sizeof(task), 0);

// 工作进程接收任务
mqd_t mq = mq_open("/wellcooked_tasks", O_RDONLY);
TaskMessage task;
mq_receive(mq, (char*)&task, sizeof(task), NULL);
```

**注意**：消息队列比管道更适合多对多通信，支持优先级和阻塞等待。如果架构简单（单调度器+TCP到游戏），可以不用消息队列，改用管道或直接TCP。

---

#### 文件锁（File Lock）

**用途**：
1. PID文件锁防止多个守护进程同时运行
2. 日志文件锁防止多进程同时写入

```c
// PID文件锁
int pid_fd = open("/var/run/wellcooked.pid", O_CREAT | O_RDWR, 0644);
struct flock lock = {
    .l_type = F_WRLCK,
    .l_whence = SEEK_SET,
    .l_start = 0,
    .l_len = 0
};
if (fcntl(pid_fd, F_SETLK, &lock) == -1) {
    fprintf(stderr, "Another instance is running\n");
    exit(1);
}

// 写入当前PID
char pid_str[16];
sprintf(pid_str, "%d\n", getpid());
write(pid_fd, pid_str, strlen(pid_str));
```

**答辩演示**：展示文件锁的实际应用，防止重复启动。

---

#### TCP Socket（跨系统IPC）

**用途**：调度器（Linux/WSL2）↔ 游戏插件（Windows）

```c
// 调度器侧（服务端）
int server_fd = socket(AF_INET, SOCK_STREAM, 0);
struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_addr.s_addr = INADDR_ANY,
    .sin_port = htons(12345)
};
bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
listen(server_fd, 5);

int client_fd = accept(server_fd, NULL, NULL);
// 收发JSON消息
```

```csharp
// 游戏插件侧（客户端）
using System.Net.Sockets;

TcpClient client = new TcpClient();
client.Connect("localhost", 12345);  // Fedora环境
// 或 client.Connect("172.x.x.x", 12345);  // WSL2动态IP

NetworkStream stream = client.GetStream();
// 发送状态、接收指令
```

**WSL2网络配置**：
- Windows可以通过`localhost`访问WSL2监听的端口（Windows 10 1903+）
- 如果有问题，可以用`ip addr show eth0`查看WSL2 IP，Windows用该IP连接
- 或者在WSL2中用`netsh interface portproxy`端口转发

---

### 5.2 通信协议设计

**协议格式**：JSON（便于调试）或Protobuf（高性能）

#### 消息类型

**1. 状态上报（插件 → 调度器）**

```json
{
  "type": "state_update",
  "timestamp": 1234567890,
  "game_time": 180,
  "score": 450,
  "chefs": [
    {
      "id": 0,
      "position": {"x": 10.5, "y": 5.2},
      "holding": "tomato",
      "state": "moving"
    },
    {
      "id": 1,
      "position": {"x": 3.0, "y": 8.0},
      "holding": null,
      "state": "idle"
    }
  ],
  "orders": [
    {
      "id": "order_001",
      "recipe": "salad",
      "time_remaining": 45,
      "ingredients": ["lettuce", "tomato"]
    }
  ],
  "resources": {
    "pot_0": {"state": "occupied", "content": "soup", "time_left": 10},
    "pot_1": {"state": "free"},
    "cutting_board_0": {"state": "occupied", "chef_id": 0},
    "plate_stack": {"count": 5}
  }
}
```

**2. 控制指令（调度器 → 插件）**

```json
{
  "type": "action",
  "chef_id": 0,
  "action": "move",
  "target": {"x": 12, "y": 6}
}
```

```json
{
  "type": "action",
  "chef_id": 1,
  "action": "interact",
  "target_object": "cutting_board_0"
}
```

**3. 心跳（双向）**

```json
{
  "type": "heartbeat",
  "timestamp": 1234567890
}
```

**4. 配置下发（调度器 → 插件）**

```json
{
  "type": "config",
  "strategy": "EDF",
  "order_list": [
    {"id": "order_001", "recipe": "salad", "arrival_time": 10, "deadline": 60}
  ]
}
```

---

### 5.3 协议实现要求

**开发阶段**：
- 在`protocol/`目录下定义JSON schema或`.proto`文件
- 双方独立实现序列化/反序列化
- 单元测试验证消息解析正确性

**联调阶段**：
- 先用简单的echo测试（插件发什么，调度器原样返回）
- 再测试单个指令（发move指令，观察游戏中角色移动）
- 最后测试完整流程（状态上报 → 调度决策 → 指令下发 → 状态更新）

**错误处理**：
- 超时未收到心跳：调度器认为插件断开，暂停调度
- JSON解析失败：记录错误日志，丢弃该消息
- TCP连接断开：调度器等待重连，守护进程检测并可能重启

---

### 5.4 IPC选型总结

| IPC机制 | 使用场景 | 课件要求 | 实现优先级 |
|---------|---------|---------|-----------|
| 管道 | 守护进程 ↔ 调度器心跳 | ✓ | 必须 |
| 共享内存 + 信号量 | 资源状态表 | ✓ | 必须 |
| 文件锁 | PID文件、日志文件 | ✓ | 必须 |
| TCP socket | 调度器 ↔ 游戏插件 | - | 必须 |
| 消息队列 | 任务分发（多worker） | ✓ | 可选 |

**至少实现前4种**，即可覆盖课件所有IPC要求。消息队列看架构需要决定。

---

## 6. 系统架构与OS机制

```text
┌─────────────────────────────────────────────────────────────┐
│  Fedora Linux 开发环境                                       │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌──────────────────┐      ┌────────────────────────┐     │
│  │ Supervisor       │ fork │ Scheduler Service      │     │
│  │ (守护进程)        │─────>│ 事件驱动调度决策        │     │
│  │ - setsid         │ wait │ - 订单队列              │     │
│  │ - SIGCHLD/waitpid│<─────│ - 策略选择              │     │
│  │ - SIGALRM 心跳   │ pipe │ - 资源预订              │     │
│  │ - pid文件锁      │<────>└─────────┬──────────────┘     │
│  └──────────────────┘                │ mq_send (任务)     │
│                                      │ mq_receive (事件)  │
│           ┌──────────────────────────┴────────────┐       │
│           │                                       │       │
│  ┌────────▼────────┐                   ┌─────────▼──────┐│
│  │ Chef Worker 0   │                   │ Chef Worker 1  ││
│  │ (独立进程)       │                   │ (独立进程)      ││
│  │ - 路径规划       │                   │ - 动作执行      ││
│  │ - 交互判断       │                   │ - 事件上报      ││
│  └────────┬────────┘                   └─────────┬──────┘│
│           │                                       │       │
│           └───────────────┬───────────────────────┘       │
│                           │ 共享内存 (世界状态)             │
│                  ┌────────▼──────────┐                    │
│                  │ Bridge Process    │                    │
│                  │ - 汇总N个厨师输入  │                    │
│                  │ - 写世界状态到shm  │                    │
│                  │ - 按帧同步         │                    │
│                  └────────┬──────────┘                    │
│                           │ TCP (localhost)               │
└───────────────────────────┼───────────────────────────────┘
                            │
┌───────────────────────────▼───────────────────────────────┐
│  Windows/Proton 游戏环境                                   │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  Overcooked! 2 (Steam + Proton)                     │ │
│  │  ┌───────────────────────────────────────────────┐  │ │
│  │  │ BepInEx 插件                                   │  │ │
│  │  │ - Harmony patch 状态读取                       │  │ │
│  │  │ - 多角色输入注入                                │  │ │
│  │  │ - 帧同步控制                                    │  │ │
│  │  └───────────────────────────────────────────────┘  │ │
│  └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  监控 UI (可选)                                              │
│  - WebSocket 订阅 Scheduler 事件                            │
│  - 或读取共享内存                                            │
└─────────────────────────────────────────────────────────────┘
```

### 6.1 分层职责说明

#### Bridge 进程

职责：

- 唯一连接游戏的进程，通过TCP与BepInEx插件通信；
- 接收游戏每帧状态并写入共享内存（使用seqlock或读写信号量保护）；
- 从共享内存读取N个Worker计算的输入槽，汇总后发送给游戏；
- 按帧同步：每帧阻塞等待所有Worker完成输入计算（屏障同步）；
- 当Scheduler/Worker崩溃时，Bridge继续运行并向游戏发送idle输入，保持连接不断。

设计优势：

- Scheduler崩溃不会导致游戏连接断开；
- Supervisor重启Scheduler后，从共享内存恢复世界状态，继续当前局。

#### Scheduler 进程

职责：

- 事件驱动：监听订单到达、工序完成、资源释放事件；
- 维护订单队列、任务前置关系、资源锁表；
- 运行调度策略（FCFS/SJF/EDF/HRRN/改进）；
- 通过POSIX消息队列向Worker派发高层任务（消息优先级对应调度优先级）；
- 记录每次调度决策和原因到日志文件；
- 接收SIGHUP信号时热切换调度策略；
- 接收SIGUSR1信号时导出状态快照到检查点文件。

#### Chef Worker 进程（每个厨师一个）

职责：

- 从消息队列接收高层任务（如"go to pot-0 and wait for cooking"）；
- 执行路径规划：A*或简单网格寻路；
- 将路径转换为每帧的移动方向和交互动作；
- 将计算结果写入共享内存的输入槽；
- 通过信号量通知Bridge本帧输入已就绪；
- 任务完成或阻塞时通过消息队列上报事件给Scheduler。

设计要点：

- Worker必须是独立进程，不能用线程，否则无法展示进程控制和进程间通信；
- Worker崩溃时，Supervisor通过SIGCHLD + waitpid检测到并重启；
- 持锁Worker崩溃时，使用健壮互斥锁（pthread_mutex with PTHREAD_MUTEX_ROBUST）或超时租约自动回收资源。

#### Supervisor 守护进程

职责：

- 按课件要求完整实现守护进程创建流程：
  - fork + exit(父进程)
  - setsid 创建新会话
  - umask(0)
  - chdir("/")
  - 关闭继承的文件描述符，重定向到/dev/null
- fork并exec启动Scheduler、多个Worker和Bridge；
- 通过管道接收子进程心跳；
- 注册SIGCHLD信号处理，调用waitpid收割僵尸进程；
- 设置setitimer + SIGALRM实现周期检查（与课件示例一致）；
- 检测到子进程退出时，记录日志并按退避策略重启；
- 使用fcntl在/var/run/kitchend.pid加锁，保证单实例；
- 接收SIGTERM时优雅关闭所有子进程并清理IPC资源。

退避策略：

- 首次重启：立即；
- 10秒内再次失败：等待2秒；
- 1分钟内失败3次：等待10秒；
- 1分钟内失败5次：停止自动重启，写入告警日志。


职责：
- Harmony patch游戏代码，读取厨师、订单、工作台和烹饪状态；
- 接收多角色输入并注入到游戏输入系统；
- 启动TCP服务器（localhost:14455）与Bridge通信；
- 每帧发送状态消息，阻塞等待输入响应（帧同步）。

参考现成项目（2026-10-15 复检，详见 §7 与 `.workflow/maps/prior-art.md`）：
- hpmv/overcooked-supercharged **没有许可证**，不能提取它的 `InjectorServer`/`ControllerHandler` 代码，
  只用来查游戏自身的类型名、字段名，以及看帧同步握手的形状（`decisions.md: 2026-10-14 - 不复制
  overcooked-supercharged 的代码，只借用游戏类型与字段名`）；
- 可复制的只有两处 MIT：`gua248/Overcooked2-TAS`（分阶段固定菜单补丁、输入状态 schema）与
  `toasterparty/oc2-modding`（固定随机源、场景目录查询、配置分层）；
- 插件侧的状态读取、虚拟手柄入座、A* 导航已经自己实现（`plugin/`），不再从这些仓库搬这一层。

注意事项：
- 不要把游戏DLL文件（Assembly-CSharp.dll等）提交到Gitee；
- 每个组员需要有自己的游戏副本；
- 菜单阶段如何让多个虚拟玩家加入游戏，需要纳入阶段A验收项。


### 6.2 IPC机制清单

本项目必须实现至少三种不同的IPC机制，并在报告中解释选择理由：

#### 1. 管道（Pipe）

用途：Supervisor与子进程之间的心跳通信

实现：
- Supervisor fork子进程前创建匿名管道；
- 子进程每N秒向管道写入一个字节；
- Supervisor使用select/poll监听管道，超时则判定子进程无响应。

课件对应：管道是最基础的IPC，课件第5页明确列出。

#### 2. POSIX消息队列（mq_open/mq_send/mq_receive）

用途：Scheduler向Worker派发任务，Worker向Scheduler上报事件

实现：
- 创建两个消息队列：`/wellcooked-tasks`和`/wellcooked-events`；
- 任务消息带优先级，高优先级任务优先被Worker取出；
- 消息优先级直接对应调度策略计算的优先级（EDF/HRRN的输出）。

设计优势：
- POSIX消息队列天然支持优先级，与调度策略完美对应；
- 进程间解耦，Scheduler崩溃不影响Worker接收已派发任务。

课件对应：课件第5页"消息队列"。

#### 3. 共享内存 + 信号量（shm_open + sem_open）

用途：Bridge、Scheduler和Worker共享世界状态

实现：
- 创建共享内存段存储：
  - 游戏当前状态（厨师位置、订单列表、资源状态）
  - N个Worker的输入槽（每帧各厨师的动作）
- 使用seqlock或读写信号量保护：
  - Bridge独占写（每帧更新游戏状态）
  - Scheduler和Worker并发读
  - Worker独占写各自的输入槽

这是典型的读者写者问题。

课件对应：课件第5页"共享内存"和"信号量"。

#### 4. TCP Socket

用途：Bridge与游戏插件之间跨进程/跨主机通信

实现：
- BepInEx插件在游戏进程内启动TCP服务器（localhost:14455）；
- Linux侧Bridge作为客户端连接；
- 协议：每帧游戏状态（JSON或Thrift），返回N个厨师输入。

设计优势：
- 支持Linux调度器控制Windows/Proton游戏；
- 可以在不同机器上运行游戏和调度器（演示时可选）。

课件对应：虽然课件未明确提及Socket，但作为跨平台IPC是合理扩展。

#### 5. 文件锁（fcntl）

用途：
- pid文件锁：保证Supervisor单实例运行；
- 日志文件锁：多进程安全追加日志；
- 检查点文件锁：原子写入状态快照。

实现：
```c
int fd = open("/var/run/kitchend.pid", O_CREAT|O_RDWR, 0644);
struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
if (fcntl(fd, F_SETLK, &lock) == -1) {
    fprintf(stderr, "Another instance is running\n");
    exit(1);
}
```

课件对应：课件第6页"文件锁机制"。

### 6.3 信号处理清单

课件第4页明确要求信号处理，本项目必须实现：

| 信号 | 发送者 | 接收者 | 用途 | 课件对应 |
|------|--------|--------|------|----------|
| SIGCHLD | 内核 | Supervisor | 子进程退出，调用waitpid收割僵尸 | 可靠信号 |
| SIGALRM | 内核（setitimer） | Supervisor | 周期健康检查，每2秒触发一次 | 课件示例代码 |
| SIGTERM | 用户/系统 | Supervisor/Scheduler/Worker | 优雅退出，清理资源 | 信号处理 |
| SIGHUP | 管理命令 | Scheduler | 热切换调度策略，无需重启 | 信号处理程序 |
| SIGUSR1 | 管理命令 | Scheduler | 导出当前状态到检查点文件 | 信号处理程序 |

所有信号必须使用sigaction注册，而不是signal，在答辩时可以讲解可靠信号和SA_RESTART标志。

实现要点：
- SIGCHLD处理函数中使用waitpid(-1, &status, WNOHANG)循环收割所有退出的子进程；
- setitimer设置周期定时器，与课件示例代码一致；
- Scheduler收到SIGHUP时，重新加载配置文件，切换调度策略枚举值；
- SIGUSR1触发时，使用临时文件+rename保证检查点原子写入。

### 6.4 互斥资源与死锁

至少实现两个真实的互斥资源：


### 6.5 离散事件模拟器（辅助工具）

**定位**：模拟器是**辅助验证工具**，用于批量跑算法对比实验；它不替代实际游戏接入。游戏接入是项目基础，必须跑通。

**为什么需要模拟器：**

- 原作游戏一局要几分钟真实时间，策略对比需要多组订单、多次重复，在原作上跑不现实；
- 原作物理引擎不完全确定，即使"相同输入"也可能有细微差异；
- 模拟器可以加速、确定性重复、无头运行，适合批量实验。

**架构设计：**

定义抽象接口KitchenEnv，提供两个实现：

```python
class KitchenEnv:
    def reset(self, map_config, orders):
        """初始化环境"""
        pass
    
    def step(self, chef_actions):
        """执行一步，返回新状态和事件"""
        pass
    
    def get_state(self):
        """获取当前状态"""
        pass

class SimEnv(KitchenEnv):
    """离散事件模拟器，确定性、可加速"""
    pass

class GameEnv(KitchenEnv):
    """连接原作游戏的环境"""
    pass
```

同一份Scheduler代码可以连接SimEnv或GameEnv。实验数据来自SimEnv，原作游戏负责演示和抽样验证。

**模拟器实现要点：**

- 离散事件驱动：维护事件队列（订单到达、工序完成、资源释放）；
- 网格地图：厨师位置、障碍物、工作台坐标；
- 简化路径：曼哈顿距离或A*，不模拟连续物理；
- 工序时长：取料1秒、切菜3秒、烹饪5秒、装盘2秒、交付1秒（可调）；
- 资源模型：锅2个、切菜台2个、盘子5个（有限容量）；
- 得分模型：基础分+小费倍率（按序交付x4，乱序重置）；
- 支持设置随机种子，保证可重复。

**验证策略：**

- 在几个简单订单序列上，对比模拟器得分和原作得分；
- 误差在10%以内认为模拟器足够准确；
- 在报告中说明模拟器简化了哪些因素（连续物理、碰撞、投掷、传送带）。
#### 锅（计数信号量）

- 假设厨房有2个锅，用sem_init(&pot_sem, 1, 2)初始化；
- Worker在开始烹饪前sem_wait，完成后sem_post；
- 锅不够时Worker阻塞，Scheduler可以先调度其他任务。

#### 盘子（有界缓冲区）

- 脏盘子回收和洗盘构成生产者消费者问题；
- 使用两个信号量：empty（可用盘子槽）和full（脏盘子数）；
- 洗盘Worker相当于消费者，装盘Worker相当于生产者。

#### 死锁场景

具体例子：
- 两个厨师在窄道相遇，互相阻挡对方去往目标；
- 厨师A持有锅等待盘子，厨师B持有盘子等待锅。

检测方法：
- 维护资源分配图（RAG）：进程节点、资源节点、请求边、分配边；
- 定期检测是否有环；
- 检测到环时，选择一个进程回退（类似银行家算法的回滚）。

恢复策略：
- 释放部分资源，回到就绪队列；
- 在日志中记录死锁检测和恢复事件。

#### 持锁进程崩溃

问题：Worker持有pot_mutex时崩溃，其他Worker永久阻塞。

解决方案：

方案1：健壮互斥锁（POSIX）
```c
pthread_mutexattr_t attr;
pthread_mutexattr_init(&attr);
pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
pthread_mutex_init(&pot_mutex, &attr);

// Worker锁定
int ret = pthread_mutex_lock(&pot_mutex);
if (ret == EOWNERDEAD) {
    // 上个持锁者已死，恢复资源状态
    recover_pot_state();
    pthread_mutex_consistent(&pot_mutex);
}
```

方案2：租约机制
- Worker获得锁时记录时间戳；
- Supervisor周期检查：锁持有超过阈值且Worker无响应，强制释放锁；
- 在日志中记录强制释放事件。

这是答辩讲可靠性的重要素材。

### 6.6 日志和文件

建议目录：

```text
config/
  orders.json      # 固定订单序列
  kitchen.json     # 地图配置
runs/
  run-YYYYMMDD-HHMMSS/
    events.jsonl        # 订单、任务、资源事件流
    decisions.jsonl     # 调度决策记录
    metrics.json        # 最终指标
    final_state.json    # 结束时状态快照
logs/
  supervisor.log   # Supervisor日志
  scheduler.log    # Scheduler日志
  worker-0.log     # Worker 0日志
  worker-1.log     # Worker 1日志
/var/run/
  kitchend.pid     # Supervisor pid文件（加fcntl锁）
```

日志至少记录：

- 进程启动、退出、重启（包含退出码和重启原因）；
- IPC连接和断开；
- 订单进入、分配、完成、超时；
- 资源申请、获得、释放、等待、超时；
- 每次调度决策及原因（策略名称、优先级计算、选中任务）；
- 角色动作失败和重试；
- 信号接收和处理；
- 死锁检测和恢复；
- 最终分数和指标。

多进程写日志的处理：
- 方案1：每个进程写独立文件；
- 方案2：统一写入，使用fcntl文件锁保护追加操作；
- 方案3：使用syslog或journald。

## 7. 参考项目与使用边界

本节结论来自 2026-10-15 的复检：逐个浅克隆读源码，并用 GitHub API 核对许可证、最后提交与活跃度；
证据、逐项判定和"未验证的东西"在 `.workflow/maps/prior-art.md`。判定分四档：**直接拿来用**
（保留许可声明）、**作为基础改造**、**只学设计**（不复制代码）、**不用**。

三条边界：

- 可复制的代码来源只有两处 MIT：`gua248/Overcooked2-TAS` 与 `toasterparty/oc2-modding`
  （`decisions.md: 2026-10-14 - 不复制 overcooked-supercharged 的代码，只借用游戏类型与字段名`）。
  gua248 的 MIT 文件版权行是没填的模板（`Copyright (c) [year] [fullname]`），复制片段要保留原文本，
  并写明来源与提交号。
- 没有许可证的仓库（supercharged、LevelEditor、DIYLevelFastInit、py-thok/overcooked2-ai、
  polus314/OvercookedBot）只能用来查游戏自身的类型与字段名，不能复制代码。
- GPL-3.0 的 `Extremelyd1/OvercookedAI` 不能抄代码，只能按自己的数据模型重写其中的想法。

### 7.1 直接复用：gua248/Overcooked2-TAS（MIT）

链接：<https://github.com/gua248/Overcooked2-TAS>；提交 `946aea0`（2025-06-13），4★，近一年无提交。

能直接拿的：

- **分阶段固定菜单**：`plugin/Patch.cs:189-241` 用 Harmony Prefix 接管 `RoundData.GetNextRecipe` /
  `DynamicRoundData.GetNextRecipe`，按 `script.menu[phase][menuCount]` 返回原生 `RecipeList.Entry`。
  它决定"第几阶段出哪道菜"，**不决定订单何时生成**（调用时机在订单控制器手里）；`menu` 索引绑定
  关卡、阶段和玩家数，换关卡或换人数要重录。方案 §4.2 要的到达时间与时限不在这个接口里。
- **输入状态 schema**：`plugin/TASControl.cs:116-125,458-475` 的 `TASScript`（`level`/`menu[][]`/
  `gamepadStates`/`rngAssigners`/`positionCorrections`），每帧每玩家
  `[b_pickup,b_interact,b_dash,b_emote,x,y,(b_shift)]`；`:263-395` 的逐帧边沿计算与 `:398-419` 的
  冻结/单帧。我们只借结构，不搬它的 Regex 手解析和 Windows 路径。
- **游戏类型/字段事实**：`DebugItemOverlay.cs:42-99,390-417`、`Extension.cs:11-69` 里的反射路径
  （锅/搅拌/洗/加热进度、盘子计时、`GridManager.m_gridOccupancy`、`RoundData.m_recipes`）。
  **这两个文件不能整份复制**：`DebugItemOverlay.cs:1-3` 自己标注来源是无许可证的
  `hpmv/overcooked-supercharged`，TAS 根目录的 MIT 覆盖不到它。只取字段事实，自己写状态快照；
  引用其它 TAS 文件前先看文件头的来源声明。

不要拿：Python 侧的 `vgamepad`/ViGEm 虚拟手柄（Windows 驱动；我们用进程内 `VirtualPad`）、录制器里
写死的 Windows 路径。

### 7.2 作为基础改造：toasterparty/oc2-modding（MIT）

链接：<https://github.com/toasterparty/oc2-modding>；提交 `bd60579`（2026-05-20），10★，近一年 10 次。
`LICENSE` 原文是 `Copyright (c) 2022 toasterparty`。

- **固定随机源**：`OC2Modding/FixedRNG.cs:59-140` 在 `GetNextRecipe` 前后设置/恢复 Unity RNG。
  固定 seed ≠ 指定菜谱序列（仍受人数、阶段、菜谱池影响）。
- **场景目录查询**：`OC2Helpers.cs:155-235` 反射 `T17FrontendFlow.m_CoopGameSessionPrefabs` 取
  `GameProgress.GetSceneDirectory().Scenes`（DLC 数组 `{-1,2,3,5,7,8}`），可补 `scripts/scan-levels.py`。
- **配置分层**：`OC2ModdingConfiguration.cs:101-120,485-509`（`CustomLevelOrder`/`PseudoSave`/事件）与
  `ArchipelagoClient.cs:552-573` 把 SlotData 落成 `OC2Modding-INIT.json` 的做法：初始实验配置与本次
  运行结果分开存。
- **工程事实**：steam 工程是 net35 + BepInEx 5.4.21 + UnityEngine 2017.4.8（与我们的环境一致），
  epic 工程是 net46 + BepInEx 6，不要混用；`tools/*.bat` 是 Windows 脚本，不能当我们的构建脚本。

不要整包安装：它默认改订单、开作弊、改 `fixedDeltaTime`，会污染"真实得分是唯一指标"的实验。

### 7.3 只当事实来源：hpmv/overcooked-supercharged（无许可证）

链接：<https://github.com/hpmv/overcooked-supercharged>；提交 `3db24d3`（2026-09-16），11★，近一年 26 次。
仓库根没有 `LICENSE`，`gh api repos/hpmv/overcooked-supercharged --jq .license` 返回 `null`。

现状：master 已经不是"patch + controller"两件套，还有 `planner/`、`headless/`、`tas-plugin/`、
`modules/`、`native/`；`patch/Injection/InjectorServer.cs:46` 在 loopback **14455** 上跑 Thrift
（可用 `OC2SC_PORT` 覆盖），另有 17636 长度前缀 JSON 与 17637 HTTP；`modules/body-restore/` 用 32 位
指针偏移、`NativeWindowControl` 用 user32/kernel32，整体不能当跨平台底座。

能学的（只看，不复制）：握手形状（每帧 Flush 状态 → 收控制 → Pause/Resume → CommitFrame，
`patch/ControllerHandler.cs:14-69`）、用 `TimeManager.PauseLayer` 暂停（`patch/Helpers.cs:13-37`）、
单个 outstanding exchange + epoch 防旧回复（`InjectorServer.cs:30-84`）、高层动作集
（`controller/Data/GameAction.cs:19-62`）与 Dijkstra 寻路（`Action/GotoAction.cs:19-78`、
`GameMap.cs:151-230`）、四玩家 InControl 本地用户注册（`bridge/SessionSetup.cs:157-207`）。

关卡数据：`controller/Data/Levels/` 只有 4 个手工标注文件（Story 1-1、Carnival 3-1/3-2/3-4），
README 自己说 `capture_map` 只记厨师轨迹、多边形要手描——**不能当 144 关几何来源**。

结论：**只查游戏类型/字段名与设计形状，不复制代码。**

### 7.4 只学设计：Extremelyd1/OvercookedAI（GPL-3.0）

链接：<https://github.com/Extremelyd1/OvercookedAI>；提交 `4916b55`（2020-05-20），9★，近一年无提交。

- 动作分层：`Action/HandleOrderAction`（单订单状态机，烹饪等待期间做别的食材）→
  `HandleIngredientAction` → `GetRawIngredient`/`Process`/`Chop`/`Cook`/`PlateHolding`/`DeliverPlate`，
  以及 `PickDropAction` 用"手持物变化"判拾放成功。
- 注入方式与我们的 BepInEx 5 不兼容：`Loader.cs:9-12` 是旧式 Mono 注入，输入走 Windows
  `user32.dll!SendInput`（`Keyboard.cs:75`）。
- 寻路调游戏自己的 `GridNavSpace.FindPath`（`Action/PathFindAction.cs:21-38`），仓库里没有 A*。
- 只取第一个厨师（`Util/ObjectUtil.cs:10-12`），`Bot.cs:20-24` 停止时不清动作、丢单的暂停也没有实现
  ——它的取消机制不能当范例。

结论：**动作分层与"等待烹饪时让出执行"值得学；GPL-3.0 下零拷贝。**

### 7.5 只学设计：视觉与桌面注入类

- `gua248/coc34robot`（MIT 占位，2024-08-19）：Win32 桌面 DC 截图 + 模板/颜色匹配 + `keybd_event`，
  写死 1080/16:9 与固定 ROI（`robot.py:26-37,61-86,93-116`），Proton 下不可直接用；匹配是逐像素
  平均绝对差与颜色差（`robot.py:131-155`），不是 OpenCV 模板匹配；`inputs.py` 另有 BSD-3 来源。
- `polus314/OvercookedBot`（无许可证，2023-06-18）、`Robben-Ge/overcooked2-replay-robot`（MIT，
  2026-08-02）：pynput/pydirectinput 桌面注入与录放，弱于我们已有的进程内注入。

结论：**视觉路线只作对照，不进我们的实现。**

### 7.6 新发现：与我们架构最像的项目

- **`Phantivia/cortico-world-overcooked2`**（MIT，`Copyright (c) 2026 Phantivia`，提交 `68843ad`
  2026-10-14，建仓三天，0★）：BepInEx 5 / net35 / 0Harmony，插件监听 `127.0.0.1:47640`
  （`plugin/Plugin.cs:39` 可配），行式 UTF-8 JSON，收包排队到主线程；`plugin/Kitchen.cs:456-538` 导出
  分数、订单、厨师与工位；`Hooks.cs` 用 Harmony 拦游戏服务端操作形成拾取/交付事件；`Seat.cs` +
  `VirtualPad.cs` 挂虚拟手柄占玩家位。规模 12 个 C# 文件 3242 行，只跑单 bot + 操作员。
  **判定：只学设计**（我们的 `WorldState.cs`/`Takeover.cs`/`Navigation.cs` 已覆盖它的能力）。
- **`py-thok/overcooked2-ai`**（**无许可证**，近一年 86 次提交）：Python RL + 进程内 C# 状态/输入桥，
  TCP 8765；`SemanticActions.cs:9-48` 直接调 `ChefEventMessage` 绕过按钮管线，正是我们不该走的路。
  **判定：只学设计，零拷贝。**
- `Shyngyskhan-333/overcooked2-eight-players`（MIT，2026-10-12）：5–8 人 IL 改写与文件驱动的多虚拟
  手柄；我们最多 4 厨师，**不用**。
- 不要装的 mod：`Lyall/Overcooked2Fix`（改物理更新频率）、`MuelsyseKen/Overcooked2-Menu-Mod`
  （直接改 `TotalBaseScore/TotalTipsScore`）、`gua248/Overcooked2-ManyRecipes`（替换 `GetNextRecipe`
  扩池、改变订单分布）、`BryanLDC/OC2ControllerIcons`（只改图标）。它们都会污染分数实验。

### 7.7 仿真与 3D 外壳

- `HumanCompatibleAI/overcooked_ai`（MIT，提交 `739950a` 2025-03-22，1020★，近一年 0 次）：
  `mdp/overcooked_mdp.py:1375-1430` 的确定性 `get_state_transition(state, joint_action)`、
  `OvercookedState.to_dict/from_dict`、46 个字符网格 layout 值得抄状态表示；但 `pyproject.toml:9` 锁
  `>=3.10,<3.11`，没有砧板工序、盘子无限、交付不消费限时订单、没有按序小费连击。
  **判定：只学状态表示，不当预筛平台。**
- `bold-lab-ai/JaxMARL`（Apache-2.0，近一年 152 次）：`overcooked_v2` 支持多食材、配方与自定义 ASCII
  布局，但奖励是 ±20 而不是原作计分，且要 Python ≥3.11 + JAX。**备选，本次不引。**
- `rosewang2008/gym-cooking`（MIT，2021）：`recipe_planner/stripsworld.py` 的 STRIPS 前置条件/效果
  是工序 DAG 的好参照；运行环境锁 gym 0.17.2，不引。
- `Farfi55/CookedUp`（MIT，Unity 2022.3.8，提交 2023-10-04，有 Linux 发布包）：Bot 规则在
  `Assets/StreamingAssets/ThinkEngineer/.../Brains/*.asp`，ThinkEngine 上游没有明确许可、
  `Assets/_Assets/` 素材授权不明。**判定：只学设计，不作兜底首选。**

结论：**模拟器自己写最小离散事件模型**（`decisions.md: 2026-10-15 - 相似项目只作设计参考`）。

### 7.8 关卡与菜谱数据

- 没有"许可证可接受 + 覆盖本体 144 包"的现成工位坐标数据集：supercharged 只有 4 关手工标注；
  `gua248/Overcooked2-LevelEditor` 的 35 个场景是 OC1/测试场景（且无 LICENSE）；
  `ArchipelagoMW/Archipelago` 的 `worlds/overcooked2/Overcooked2Levels.py:35-91` 只有 140 条
  ID ↔ 显示名（根 LICENSE 为 MIT），没有坐标。
- 我们自己的导出已经是更好的来源：`plugin/WorldState.cs:283-334`（工位 `type/name/pos/height/gives`）、
  `:594-738`（地形采样）、`:1078-1148`（菜谱 `m_composition` 展开）、`Kitchen.cs:66-96`。
  **下一步是把它们转成 `config/kitchen.json` 与工序 DAG，不是再找数据源。** 已知缺口：工位去重与
  稳定 ID、订单未关联菜谱 ID、菜谱缺 `m_cookingStep`/`m_progress` 与工时、`WorldState.cs:720-729`
  过滤最大连通块后 cells 可能仍用过滤前的列表。
- DIY 关卡：用 `Overcooked2-LevelEditor` 的**发布工具**（无 LICENSE，Unity 2017.4.8f1，构建目标
  `StandaloneWindows`，产物 `info_xxx` + `s_xxx_1` 放 `BepInEx/plugins/OC2DIYLevel/levels/xxx`）；
  它没有 `load=<场景名>` 接口，进关走 OC2DIYLevel 自己的菜单/SceneDirectory。
  `kfyangc/Overcooked2-DIYLevelFastInit`（无 LICENSE）只是加速初始化，**不用**。
- 离线工具：`K0lb3/UnityPy`（MIT）已用于 `scripts/scan-levels.py`；AssetRipper 只作为外部工具
  （当前主分支 GPL-3.0，LevelEditor 教程要求 ≤1.3.14）。反编译产物与游戏资源包都不进仓库。

### 7.9 OS 侧：只学设计，不引库

- 监督器：runit 的 self-pipe 收 SIGCHLD + 主循环 `wait_nohang` 批量回收、s6 的防快速重启与日志解耦、
  supervisord 的 BACKOFF/RUNNING/FATAL 状态机（`supervisor/process.py:177-182,670-715`）、systemd 的
  启动限流与 TERM→CONT→超时强杀顺序。**都不进仓库**：课设要求自写 `fork/setsid/umask/chdir/dup2`
  与 `SIGCHLD` 回收。注意 runit 的 `lock_exnb.c:10-14` 是 `flock`/`lockf` 条件选择，不是纯 `fcntl` 范例。
- 调度算法：`MaximeCheramy/simso` 的 EDF/RM（CeCILL，只读）、`hdd-robot/gAgent` 的 Contract Net
  状态机（MIT）、`zehuilu/CBBA-Python` 的边际收益竞价（MIT）、Linux `SCHED_DEADLINE` 文档（只读）。
  **不要用 `sched_setattr` 给 Worker 设实时策略来冒充订单 EDF。**
- IPC 与事件循环：TLPI 的 `pmsg_*`（GPL，只读）、`sysprog21/concurrent-programs` 的
  `ringbuf-shm/ringbuffer.c`（BSD-2，单生产者单消费者）、Valkey `src/ae_epoll.c`（BSD-3）只作形状
  参考；我们的 fd 很少，自写 `poll`/`pselect` 处理 TCP、心跳与 self-pipe，不引 libuv/libev。
- `fcntl` 追加日志的权威依据是 `fcntl_locking(2)`：记录锁不随 `fork` 继承、进程退出自动释放、
  同进程关闭指向该文件的任意 fd 会释放它持有的锁；短临界区要包住真正的 `write(2)`，不要在 stdio
  缓冲还没提交时解锁。
- 验证工具在仓库外直接用：`strace -ff -ttt`（看 setsid/umask/dup2/fcntl/wait4/execve）、`pidstat`、
  `stress-ng`、Valgrind（`--track-fds`）。Helgrind 不覆盖跨进程共享内存竞争，跨进程 IPC 要定向并发
  测试加退出后的对象检查。
- 监控面板（§8.1）没有开箱可用的：supervisord Web、htop、Glances 都没有订单、厨房锁与游戏分数。
  渲染层可选 FTXUI（MIT，C++）或纯 C ANSI 面板，业务数据必须自己接。

### 7.10 落地清单

现在就能做：

1. 分阶段固定菜单接进 `plugin/`（TAS 的 `GetNextRecipe` Prefix，见 7.1）；
2. 订单随机源固定并记录 seed（oc2-modding 的 `FixedRNG.cs`）；
3. 写 `wellcooked-state.json` / `wellcooked-terrain.json` → `config/kitchen.json` 的转换器；
4. 菜谱展成工序 DAG（补工时与依赖边）；
5. Bridge 端口可配置，默认 14455（`decisions.md: 2026-10-15 - Bridge 端口可配置`）。

不要做：不装会改订单/物理/分数的 mod；不引监督器与 IPC 库；不把 overcooked_ai / JaxMARL 当预筛平台；
不复制无许可证或 GPL 仓库的代码。

## 8. 最终形态建议

### 8.1 推荐最终演示画面

```text
+--------------------------------------------------------------+
|                    Overcooked! 2 游戏画面                    |
|                                                              |
|       厨师A自动取料、切菜、烹饪       厨师B自动装盘、交付      |
|       锅具/切菜台/盘子正常显示        订单倒计时正常显示       |
|                                                              |
+-----------------------------+--------------------------------+
| 调度监控面板                | 事件/日志面板                  |
| 策略：EDF+资源感知          | 12:03 chef-0 获得 order-2     |
| 当前分数：36                | 12:04 pot-0 locked             |
| 完成/超时：3/1              | 12:05 chef-1 blocked           |
|                             | 12:06 order-2 delivered        |
| 订单队列                    |                                |
| order-4 EDF priority 0.91   | Supervisor: healthy            |
| order-5 SJF cost 32         | Scheduler: heartbeat OK        |
|                             | Worker-0/1: running            |
+-----------------------------+--------------------------------+
```

### 8.2 必须能够现场演示的流程

1. 启动Supervisor；
2. Supervisor拉起Scheduler、Worker和游戏插件连接；
3. 选择固定订单序列和调度策略；
4. 游戏中两个厨师自动协作完成至少一单；
5. 面板显示订单、资源锁、进程和日志；
6. 使用相同输入运行FCFS、SJF、EDF和改进策略；
7. 展示至少一项指标差异；
8. 手动结束Scheduler；
9. Supervisor检测到故障并重启Scheduler；
10. 系统恢复通信并继续或重新开始一局；
11. 展示保存的日志和实验结果。

### 8.3 算法演示原则

- 所有策略使用相同地图、订单、时间限制和动作执行器；
- 不用改变游戏速度、瞬移、加速加工等功能制造得分优势；
- 明确区分游戏真实得分和业务层实验指标；
- 如果使用固定脚本作为演示，必须说明它是回放基线，不冒充实时调度算法；
- 结果至少重复多次或使用多组固定订单，避免只展示一局偶然结果。

## 9. 实施计划

### 时间节点提醒

- 第3周汇报前必须完成游戏接入验证，否则PPT里讲的是未验证路线；
- OS核心骨架从第1周就与游戏接入并行推进，不要等游戏接入完成；
- 真正集中编码在第4到5周，联调和验收在第6周，时间很紧；
- 游戏接入是本项目基础，必须跑通；卡住时组内同步技术方案，不轻易放弃。

### 阶段A：游戏接入（第1周开始，最高优先级）

**这是项目基础，不是可选验证。** 目标：在实际《胡闹厨房2》中，由外部程序控制角色完成一个订单。

#### A1：运行环境验证（1-2天）

**Fedora 实验室（shizuku）**：
- Steam + Proton 运行《Overcooked! 2》；
- BepInEx 5.x 在 Proton 下加载成功（检查 `BepInEx/LogOutput.log`）；
- Harmony 补丁生效；
- 记录 Proton 版本、启动参数、遇到的错误。

**WSL2/Windows 实验室（组员）**：
- Windows 原生运行游戏，BepInEx 加载成功；
- WSL2 内监听端口，Windows 侧能连通（先验证 `localhost`，不行再用 WSL2 IP）；
- 记录 WSL2 网络配置和防火墙规则。

#### A2：状态读取（2-3天）

- 读取厨师位置、朝向、手持物；
- 读取订单列表、菜品、剩余时间；
- 读取工作台状态（锅、切菜台、盘子）；
- 读取当前得分；
- 通过 TCP 上报到 Linux 侧（先写日志即可）。

#### A3：动作控制（3-5天）

- 注入输入：摇杆方向、交互键、投掷键；
- 控制单厨师移动到指定坐标；
- 控制抓取/放下、放置食材到锅、装盘、交付；
- 菜单阶段让虚拟玩家加入游戏（多人本地合作的前提）；
- **验收：控制两个厨师完成一个订单并交付，得分正常。**

#### A4：多角色与稳定控制

- 控制 2-4 个厨师协同；
- 帧同步：输入不丢失、不抖动；
- **验收：稳定完成 3 个订单。**

**关于 hpmv/overcooked-supercharged：**

- README 说明只有 Carnival 3-2 four-player 是正确的，且需要 DLC；
- 当前 master（2026-09-16 最后提交）正在做 Story 1-1 的 deterministic rewind，包含复杂物理状态恢复和时间回溯；
- 该项目是 TAS 开发框架，不是现成调度器；
- 只提取 InjectorServer 和 ControllerHandler 的状态序列化与输入注入逻辑，不依赖整个项目。

**若物理精度路线卡住**：先用其他关卡验证基本控制（移动、抓取、交付），不追求 Carnival 3-2 的精细复现；也可以退一步用键盘事件模拟手柄输入。游戏接入本身不能放弃。

### 阶段B：OS核心骨架（第1周与阶段A并行）

目标：实现完整的Linux守护进程、IPC和信号处理框架。

任务：

**Supervisor守护进程：**
- 完整实现守护进程创建流程：fork、setsid、umask、chdir、dup2；
- 注册SIGCHLD、SIGALRM、SIGTERM信号处理；
- 使用setitimer实现周期健康检查；
- 使用fcntl在pid文件加锁；
- fork并exec启动Scheduler和Worker进程；
- 通过管道接收心跳，使用waitpid收割僵尸进程；
- 子进程异常退出时按退避策略重启。

**Scheduler进程骨架：**
- 事件驱动主循环：监听订单到达、工序完成、资源释放事件；
- 通过POSIX消息队列（mq_send/mq_receive）向Worker派发任务；
- 维护资源锁表，处理锁申请和释放；
- 注册SIGHUP（热切换策略）和SIGUSR1（状态快照）信号处理；
- 记录每次调度决策到日志文件。

**Bridge进程骨架：**
- 监听TCP端口，接受游戏插件连接；
- 接收状态上报，转发给Scheduler；
- 将Scheduler的控制指令编码后下发插件；
- 断线检测与重连。

验收：守护进程后台运行，能重启被kill的Scheduler，日志正常写入。

### 阶段C：调度闭环（第2周）

目标：游戏 → 状态上报 → 调度决策 → 控制指令 → 游戏，跑通完整闭环。

任务：

- 定义订单数据结构（到达时间、截止时间、预计工时、收益）；
- 实现至少两种调度策略（FCFS + 一种有深度的，如EDF或列表调度）；
- 单订单调度：识别就绪工序 → 分配给空闲厨师 → 下发动作；
- 处理工序前置依赖；
- 处理资源冲突（等待锅空闲、等待盘子）；
- 记录得分、完成订单数、耗时。

验收：在真实游戏中用调度算法自动完成一局并取得正分。

### 阶段D：算法深度与实验（第3-4周）

目标：多策略对比，体现OS调度原理的应用深度。

任务：

- 实现订单级策略：FCFS、SJF、EDF、HRRN、LLF；
- 实现工序级策略：列表调度、关键路径优先、资源感知调度；
- 每种策略在游戏中跑同一组固定订单，记录得分与耗时；
- **辅助工具**：离散事件模拟器用于快速批量预验证策略，加速参数搜索；它不替代游戏实测；
- 生成对比图表与统计结果。

验收：有可重复的游戏实测数据和对比报告。

### 阶段E：监控面板与验收（第5-6周）

目标：实现监控UI，准备最终演示。

任务：

- 实现监控UI：显示订单队列、任务状态、资源占用、进程健康、日志流；
- 支持策略切换（向Scheduler发SIGHUP）；
- 准备演示脚本：
  1. 启动Supervisor；
  2. 自动完成一局；
  3. kill Scheduler，观察自动重启；
  4. 切换策略，对比结果；
  5. 展示日志和指标；
- 准备备用录像（防止现场演示故障）；
- 整理Gitee提交，确保每个成员有清晰贡献。

验收：能完整演示所有要求的流程。

## 10. 小组分工建议

适合4到6人。每个人必须绑定明确的OS原理主题，用于个人答辩（60%权重）。

| 角色 | 主要产出 | OS主题（用于答辩） | 个人时序图内容 |
|------|----------|-------------------|---------------|
| Supervisor守护进程负责人 | 守护进程实现、信号处理、子进程管理 | fork/setsid/信号处理/waitpid | Supervisor启动流程、SIGCHLD处理、重启决策 |
| Scheduler与消息队列负责人 | 调度策略、消息队列、资源锁 | POSIX消息队列、信号量、调度算法 | 订单到达到任务派发的完整流程 |
| Worker与共享内存负责人 | Worker进程、共享内存、读者写者问题 | 共享内存、信号量、读者写者同步 | Worker接收任务到写入输入槽的流程 |
| 游戏接入与Bridge负责人 | BepInEx插件、TCP通信、动作接口 | TCP Socket、跨系统IPC、进程间数据交换 | 游戏状态读取到输入下发的时序 |
| 实验与数据分析负责人 | 策略对比实验、模拟器工具、数据分析 | 调度算法评价、实验设计 | 实验运行流程、指标采集逻辑 |
| 日志与文件系统负责人 | 日志系统、文件锁、检查点、监控UI | 文件锁（fcntl）、日志追加、pid文件锁 | 多进程日志写入、检查点原子保存流程 |

**重要：**

- 每个人负责的模块必须有独立的代码文件，体现在Gitee提交历史中；
- 答辩时要准备自己负责模块的UML类图和时序图；
- 要能解释自己用到的OS原理：对应课件哪一页、系统调用是什么、为什么这样设计；
- "文档与集成"不应该是独立角色，每个人都要写自己模块的设计文档；
- 游戏接入是核心模块之一，必须由专人负责并保证跑通，不能只当"附加工作"；
- 模拟器只是实验辅助工具，其工作量不应挤占游戏接入和OS核心的时间。

## 11. 主要风险与决策


### 11.1 平台与课件对齐风险

课件内容几乎全是Linux/POSIX：fork、setsid、信号、管道、消息队列、共享内存、文件锁。若OS核心运行在Windows上，答辩时很难对应课件要求。

**已确定方案（双实验室）：**

- OS核心（Supervisor、Scheduler、Worker、Bridge）在两个实验室都运行在Linux上：本机组为 Fedora 原生，组员组为 WSL2；
- 本机组游戏通过 Proton 运行，BepInEx 插件通过 TCP 与同机 Linux 侧 Bridge 通信；
- 组员组游戏在 Windows 原生运行，插件通过 TCP 连到 WSL2 内的 Bridge，属跨系统边界 IPC；
- 同一份 POSIX 代码在两个平台无条件编译运行，不使用条件编译区分平台；
- 守护进程、IPC、信号处理全部完整实现 POSIX 规范，Windows 侧不放守护进程。

### 11.2 游戏接入技术风险

**现状评估：**

- hpmv/overcooked-supercharged 当前master处于活跃开发，最新提交（2026-09-16）正在做Story 1-1的物理状态精确恢复；
- README明确说明只有 Carnival 3-2（DLC内容）是正确的；
- 该项目是TAS开发框架，包含复杂的时间回溯和warping机制，不是现成调度器；
- 低层运动控制（路径规划、对准、交互判断）仍需自己实现；
- Proton 下 BepInEx 的兼容性未知，需要在 Fedora 上实测。

**应对策略：**

- 游戏接入从第1周开始，是最高优先级工作项，不设"失败就放弃"的默认退路；
- 用双实验室并行降低单点风险：组员组在 Windows 原生环境下先行打通插件和动作控制，本机组在 Fedora/Proton 下同步验证；
- Windows 侧经验可以直接指导 Proton 侧排查（同一份插件代码）；
- 若 Carnival 3-2 因 DLC 或物理精度问题受阻，先在其他简单关卡验证基本控制（移动、抓取、交付），不追求精细复现；
- 模拟器仅用于加速策略预筛，不作为演示主平台。

### 11.3 调度策略评价公平性

**问题：**

《胡闹厨房2》的小费倍率机制要求按序交付才能维持x4倍率，这使得订单级的FCFS天然接近游戏得分最优，其他重排策略会被惩罚。

**应对策略：**

1. 使用Harmony补丁接管订单生成，按orders.json定义的时限和到达时间生成订单，保证画面和模型一致；
2. 调度重心放在工序级：N个厨师并行执行DAG任务图，这是多处理机调度问题，有丰富的比较空间；
3. 辅助的模拟器可以关闭小费倍率，用于单纯比较完成数和超时率；
4. 在报告中明确说明游戏计分规则对策略选择的影响。

### 11.4 时间压力与范围控制

**现实约束：**

- 真正编码时间只有第4到5周（2周）；
- 第3周需要汇报方案，必须有初步验证结果；
- 第6周就要展示可运行系统。

**范围控制：**

- 第一版只做：2名厨师、1张固定地图、少量固定订单、4种基线策略、1种改进策略；
- 守护进程、IPC、信号、资源锁、日志必须全部落实，这是课件硬性要求；
- 不做：多地图、动态订单、强化学习、完整死锁检测、断点恢复；
- 游戏接入是必做项；遇到困难时缩小关卡规模（先做简单关卡、先做2厨师），而不是取消接入。

### 11.5 个人答辩准备

**关键：**

答辩占60%权重，每个人要能讲清楚：

- 自己负责的模块用到哪些OS原理；
- 对应课件第几页；
- 用了哪些系统调用（fork、setsid、mq_send、sem_wait、fcntl等）；
- 为什么这样设计，有什么权衡；
- 自己的时序图和代码提交记录。

**风险：**

- "文档与集成"角色没有具体的OS原理可讲，答辩会吃亏；
- 游戏接入负责的C#部分属应用层，需要额外绑定IPC/TCP/跨系统通信的OS主题；
- 每个人必须绑定明确的OS主题（见第10节表格），并把工作分配到两个实验室。

### 11.6 合规和引用

- 保留参考项目许可证和版权说明；
- 在报告中列出所有使用的仓库、版本和改动；
- 不将第三方Bot已有功能作为本组原创成果；
- 原版游戏素材仅作为本地课程演示使用；
- 不要复制GPL项目（如Extremelyd1/OvercookedAI）的代码后混入闭源发布物；
- 不要把游戏DLL（Assembly-CSharp.dll等）提交到Gitee。

## 12. 待办清单

团队下一次讨论应明确：

**立即决策（第1周）：**

- [ ] 确认两个实验室的环境：本机 Fedora 版本 + Proton；组员的 WSL2 发行版 + Windows 版本；
- [ ] 分配成员到两个实验室，明确各自负责的模块；
- [ ] 分配角色和OS主题（见第9节表格）；
- [ ] 固定一张简单地图（无传送带、无移动平台），如 Story 1-1；
- [ ] 设计3到5组固定订单序列（简单、中等、复杂）；
- [ ] 阶段A（游戏接入）验收截止时间：具体日期（建议第1周结束前）；
- [ ] 定义TCP通信协议（JSON schema）并写入 `protocol/`；
- [ ] 创建Gitee仓库，设置.gitignore排除游戏DLL和二进制文件。

**第2周决策：**

- [ ] 汇总两个实验室的阶段A验证结果，确认插件、输入注入、菜单加入是否稳定；
- [ ] 确定IPC具体实现：管道+POSIX消息队列+共享内存+TCP+fcntl；
- [ ] 确定信号处理清单：SIGCHLD、SIGALRM、SIGTERM、SIGHUP、SIGUSR1；
- [ ] 定义订单数据格式：到达时间、菜谱、截止时间、预计工时；
- [ ] 定义评价指标优先级：游戏原生得分 > 完成数 > 超时率 > 等待时间；
- [ ] 确认 WSL2 ↔ Windows 的连通方案（localhost 或 WSL2 IP + 防火墙规则）。

**第3周汇报前：**

- [ ] 完成需求分析文档；
- [ ] 绘制系统架构图（进程、IPC、信号流），标注两个实验室的差异点；
- [ ] 绘制至少两个时序图（订单调度流程、进程重启流程）；
- [ ] 准备UML类图或组件图；
- [ ] 列出OS原理应用清单和对应课件页码；
- [ ] 游戏接入已有可演示结果（至少能读状态、控角色）。

## 13. 来源

### 课件

- 本地课件：`/home/shizuku/下载/操作系统原理课程设计.pdf`

### 原作控制和Mod

- <https://github.com/hpmv/overcooked-supercharged>
- <https://github.com/gua248/Overcooked2-TAS>
- <https://github.com/Extremelyd1/OvercookedAI>
- <https://github.com/gua248/coc34robot>
- <https://github.com/toasterparty/oc2-modding>

### 3D外壳和算法验证

- <https://github.com/Farfi55/CookedUp>
- <https://github.com/HumanCompatibleAI/overcooked_ai>

### 原作Mod平台说明

- <https://archipelago.gg/tutorial/Overcooked!%202/setup_en>


项目的核心目标：

> **在Linux上实现完整的守护进程、IPC和调度系统，用操作系统原理解决多厨师协作任务分配问题；调度结果通过《胡闹厨房2》真实游戏画面呈现。**

关键成功要素：

1. **游戏接入优先**：第1周开始攻游戏接入，读状态、控角色、加入多人，必须跑通；
2. **OS核心必须在Linux上**：守护进程、信号、POSIX IPC不能妥协，两个实验室共用同一份代码；
3. **双实验室并行**：Fedora+Proton 与 WSL2+Windows 同步推进，互为参考和兜底；
4. **每个人绑定OS主题**：答辩占60%，必须有明确的原理可讲；
5. **模拟器只做辅助**：用于加速策略预筛，不作为演示主平台，可随时裁剪。

本文档已更新为双实验室、游戏优先方案。实施中若遇资源紧张，优先保证游戏接入和OS核心完整性，模拟器可裁剪。
