# 调度策略补齐与浏览器控制台

**Status:** in progress
**Priority:** high

## What this is for

答辩要现场演示三件事：同一张图上换调度算法、结果有差别；kill Scheduler 后 Supervisor 把它拉回来；面板上看得到订单、厨师、进程和事件（方案 §3.1 展示功能、§8.2 第 5–9 步）。现在这三件事都只能在终端里敲命令、翻日志：订单级缺 SJF（方案点名要比 FCFS/SJF/EDF），工序级只有一种代价模型可比，策略只能 SIGHUP 轮换不能直接指定，也没有任何面板。

## What it does

- 浏览器打开本地控制台，选场景、厨师数、订单级策略、工序级策略、派工模式，点“开始”。游戏窗口自己进关卡开打，画面在游戏窗口里；浏览器同步显示分数、倒计时、订单队列、厨师位置与手持、锅与工位、当前策略和最近一次派工理由、进程表（pid、状态、重启次数）、事件时间线。
- 面板上能直接切到指定策略（不必轮换），能一键 kill Scheduler，进程表上看到 pid 变化与重启次数加一。
- 一局结束后进历史表；按策略分组给出平均分和局数，用柱状图对比。
- 订单级演示四种：FCFS（基线）、SJF（新增，估计工时短的先开）、LLF（松弛最小先开）、HRRN（响应比最高先开）。EDF 代码保留、页面可选，但 demo3 的订单时限全是 120 s，EDF 排出来和 FCFS 一样，所以不放进对比的主列表。
- 工序级三档：`fifo`（就绪工序按订单开工顺序再按工序下标，空闲厨师取第一个能做的，不看路程）、`cp`（关键路径最长的先做）、`cost`（现有五项加权代价 + 匹配，默认）。三档共用同一套就绪判定和安全闸门（锅熟才倒、按序交付、开单闸门），只换排序。

## How it is built

三个部分，各自只依赖下一层的文件契约：

1. **Scheduler 策略（C）。** `policy` 模块加 SJF，四种主键并列都退化为 FCFS；SIGHUP 轮换为 FCFS→EDF→LLF→HRRN→SJF→FCFS。Scheduler 新增 `--task-policy fifo|cp|cost`，Supervisor 原样透传，和 `--strategy` 同一条路。运行中指定策略：往 `<runtime-dir>/strategy.request` 写一行 `order=<名> task=<名>`（两键都可选），再发 SIGHUP；文件存在就按文件切并删除，不存在就照旧轮换，内容不合法写 WARN、删除、不切。Scheduler 启动和每次切换后原子写 `<runtime-dir>/policy.json`（`{"order","task","since_ms"}`），`decisions.jsonl` 的 `kind=strategy` 带 `policy` 与 `task_policy`。
2. **`wellcooked-monitor`（C，只读）。** 新进程，只读映射 Bridge 写的 shm 世界状态，按现有读者的同步方式取一致快照，每 200 ms 原子写 `<runtime-dir>/live.json`（厨师、订单、工位/锅、分数、计时、资源占用，shm 里没有的字段不写）。shm 未就绪时写 `{"ok":false}` 继续重试；SIGTERM 干净退出。它不是 Supervisor 的子进程，由 `run-real-game.sh` 在 Supervisor 起来后拉起、退出时停掉。
3. **浏览器控制台（Python 标准库 + 原生 HTML/JS）。** `tools/webui/server.py` 只监听 127.0.0.1。`POST /api/run` 在后台跑 `tools/run-real-game.sh`（策略经环境变量 `STRATEGY`/`TASK_POLICY`/`DISPATCH` 传入，白名单校验，列表参数调用不拼 shell），同时只允许一局；`GET /api/stream` 用 SSE 每 500 ms 推 live.json、status.json、policy.json、events/decisions 的增量尾部和 metrics；`POST /api/strategy` 写 strategy.request 后对 status.json 里的 scheduler pid 发 SIGHUP；`POST /api/kill-scheduler` 发 SIGKILL；`GET /api/runs` 解析历史局的 summary.txt（脚本新增 `policy:` 一行）。

## What it must not do

- 不改订单时限（用户 2026-10-28 决定），所以 EDF 在 demo3 上不和 FCFS 区分。
- 不做抢占：已开工订单不改派，料在手上或锅里没法“换出”（`decisions.md: 2026-10-25 - 订单按开工顺序交付，策略只决定下一单`）。
- 不改闸门与走位：三档工序级只换排序，不放宽锅、盘、交付顺序的判定。
- 网页不直接读 shm、不开外网端口、不提供任意命令执行。
- 不在浏览器里转播游戏画面：画面就在游戏窗口。

## What was considered and rejected

- **纯 C 终端面板。** 不加语言、最贴课程。被否：画不出策略对比图，答辩观众看终端字符面板吃力；用户选了浏览器。
- **网页直接解析 shm。** 少一个进程。被否：Python 要照抄 C 结构体布局和信号量协议，布局一改就静默读错；C 的 monitor 复用同一份头文件。
- **WebSocket 推送。** 双向。被否：只需要服务器推，SSE 用标准库就能写，不引第三方包；控制走普通 POST。
- **新加一条 FIFO/socket 控制通道给 Scheduler。** 被否：文件 + SIGHUP 复用现有信号处理，Scheduler 主循环不用多监听一个 fd。
- **订单级五种全上演示。** 被否：demo3 时限相同，EDF≡FCFS，多一栏只会让对比表多一行重复数据。

## How we will know it works

- **W1.** `cd bridge && make test` 全绿；`test_policy` 有 SJF 顺序、并列退化、轮换顺序、名字解析、strategy.request 解析（合法 / 单键 / 坏内容）。
- **W2.** 无游戏起 Supervisor 树带 `--strategy sjf --task-policy cp`：policy.json 为 `SJF/cp`；写 `order=llf task=fifo` 并 SIGHUP 后 policy.json 变为 `LLF/fifo`，decisions.jsonl 多一条 `kind=strategy`；坏内容不切、有 WARN。
- **W3.** 三档工序级在同一确定性场景下派工顺序不同，且都遵守锅熟才倒（场景测试）。
- **W4.** monitor 测试：自建 shm 写入已知状态，快照 JSON 里厨师坐标、手持、订单、分数与写入一致；shm 不存在时输出 `ok:false` 不崩。
- **W5.** 控制台：用假运行目录，`/api/stream` 推出五类数据，`/api/runs` 解析真实历史 summary；第二次 `/api/run` 返回 409；页面三块都渲染（截图）。
- **W6.** 实机一局（需要游戏）：网页开局 → 游戏窗口进 demo3 → 面板分数随游戏变化 → 面板切 SJF/cp，policy.json 与 decisions 跟着变 → kill Scheduler，进程表 pid 变化、spawns+1、游戏继续出单 → 结束后历史表出现这一局。

## Open questions

- 工序级 `fifo` / `cp` 在实机上会不会因为不看路程造成更多走位冲突、得分掉到很低：W6 跑完才知道，由实测决定是否在演示里只展示 `cp` 与 `cost`。

## Notes

相关：`.workflow/specs/kitchen-control.md`（代价模型、LLF/HRRN）、`.workflow/specs/os-core.md`（策略接口、SIGHUP）、方案 `overcooked_os_course_project.md` §3.1、§4.3、§8。
