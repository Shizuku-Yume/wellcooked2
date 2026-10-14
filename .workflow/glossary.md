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

- **订单（Order）**: 游戏里的一条菜谱需求，在系统里是作业（job），带到达时间、截止时间、预计工时和收益。
- **工序（Task）**: 订单展开后的一步（取料、切菜、烹饪、装盘、交付），是调度单位，有前驱依赖。
  - _Avoid_: 用"任务"同时指订单和工序；订单叫作业，工序叫任务。
- **厨具（Workstation）**: 砧板、锅、盘子、交付台等共享资源，分别映射为互斥锁、计数信号量、有界缓冲区。
- **世界状态（World state）**: 插件每帧上报的游戏状态快照（厨师、订单、厨具、得分），Bridge 写入共享内存供其他进程读。
- **输入槽（Input slot）**: 共享内存里每个厨师每帧的动作槽，Worker 写、Bridge 汇总后下发游戏。
- **Bridge**: Linux 侧唯一连接游戏的进程，TCP 服务端，负责状态转发和输入汇总。
- **Supervisor**: 守护进程，fork/exec 启动子进程、收心跳、按退避策略重启。
- **Scheduler**: 调度决策进程，维护订单队列和资源锁表，用 POSIX 消息队列派发任务。
- **Chef Worker**: 每个厨师一个进程，做寻路和动作分解，把结果写进输入槽。
- **策略（Strategy）**: 订单级（FCFS/SJF/EDF/HRRN）或工序级（列表调度、关键路径优先）的调度算法。
