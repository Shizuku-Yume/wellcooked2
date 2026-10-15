# Bridge 进程

**Status:** proposed
**Priority:** high

## What this is for

KitchenOS 的 OS 核心要和 Windows 上跑的游戏说话，但两边没有任何一条已验证的链路。现在仓库
里只有游戏侧插件，Linux 侧一个进程都没有：插件的 TCP 连出去没有对端，插件上报的状态没有
人收，调度决策没有路回到游戏。没有 Bridge，整个项目只能演示插件本身，阶段 A 之后的
Supervisor/Scheduler/Worker 全都无处落脚。

## What it does

一个独立进程 `kitchenos-bridge`，跑在 WSL2/Linux 侧，是唯一与游戏通信的进程。

- 默认以 TCP **服务端**监听 `0.0.0.0:12345`，接受插件连入；`--link connect` 时可切换为
  客户端，主动去连插件开的服务端（`--host` / `--port`，默认 14455）。两种模式跑同一套协议。
- 收插件上报的 `state_update`，把世界状态写进共享内存 `/kitchenos_state`。
- 把每个 `state_update` 当作一帧：等全部 N 个输入槽就绪（有超时上限），汇总成动作下发插件；
  槽缺失或超时的厨师下发 idle，并记录是哪几个。
- 当 Scheduler/Worker 根本不存在或中途死掉时，Bridge 照常连接游戏、照常按帧下发 idle，
  日志里说明原因。它自己不会因为同伴缺席而退出。
- 心跳超时判定插件断开：暂停下发、记录，连接恢复后自动继续。
- 收到 `SIGTERM`/`SIGINT` 优雅退出：记录退出事件，`shm_unlink`/`sem_unlink` 清理 IPC 资源，
  释放端口（立刻重启能重新绑定）。
- 全程把事件写进 `logs/bridge.log`：启动退出、IPC 连接与断开、每帧状态摘要、动作下发、
  缺失输入槽、心跳超时、JSON 解析失败、信号处理。

## How it is built

模块与各自职责（头文件在 `core/include/kitchenos/`，即模块契约）：

| 模块 | 负责 | 不负责 |
|---|---|---|
| `json` | 递归下降解析与写入，覆盖对象/数组/字符串/数字/布尔/null 子集 | schema 校验、流式解析 |
| `framing` | 4 字节大端长度前缀 + UTF-8 的分帧与解帧，1 MiB 上限 | 语义、重传 |
| `protocol` | 消息类型常量；解析 `state_update`/`hello`/`ack`/`heartbeat`/`echo`/`config`；构造 `action`/`ack`/`echo_reply`/`heartbeat`/`hello` | 业务决策 |
| `shm` | `/kitchenos_state` 的布局、seqlock、输入槽与就绪信号量 `/kitchenos_input_ready` | 谁该占哪口锅（Scheduler 的事） |
| `net` | TCP 监听/连接、断线重连、把 socket 接进 `framing` | 消息语义 |
| `log` | `bridge.log` 追加写、时间戳、事件类别 | 日志轮转 |
| `bridge` | 主循环：select/poll 收包 → 分派 → 写 shm → 帧屏障 → 下发 | 上面任何模块的内部 |

数据形状（跨端字段名以 `protocol/` 为准）：

- **世界状态**：`score`、`game_time`、`chefs[]{id,x,y,holding,state}`、
  `orders[]{id,recipe,time_remaining,ingredients[]}`、
  `resources{pot_N{state,content,time_left}, cutting_board_N{state,chef_id}, plate_stack{count}}`。
- **输入槽**：每厨师一格，含 `chef_id`、`frame_id`、动作内容。
- **并发规则**：世界状态由 Bridge 独占写、他人并发读 → **seqlock**（写者递增奇数序号、
  写、递增回偶数；读者前后取值不等则重读）。输入槽由对应 Worker 独占写、Bridge 读 →
  每槽一个 seqlock，写完 `sem_post` 就绪信号量；Bridge `sem_timedwait` 等 N 次或超时。
  这两条直接对应文档 §5.1"共享内存 + 信号量"和 §6.1 的读者写者与屏障要求。

失败行为：分帧或 JSON 解析失败 → 丢弃该消息并记录，不断连接；帧长越界或负数 → 视为流已
失步，断开重连并记录；写 shm 前先确认对端结构版本号，不匹配则拒绝启动。

## What it must not do

- **不做调度决策**。选谁做哪道工序属于 Scheduler；Bridge 只搬运。
- **不做路径规划**。把动作算成每帧输入属于 Worker。
- **不改插件协议**。分帧与字段名以插件现有实现为基准；要改先改 `protocol/` 并同步两端。
- **不管进程守护**。daemonize、fork/exec 子进程、SIGCHLD 收尸属于 Supervisor（后续任务）。
- **不做 Supervisor/Scheduler/Worker 本身**，本次只做 Bridge 与它依赖的共享层。

## What was considered and rejected

- **只实现一种 TCP 拓扑。** 理由是文档 §2.2.1/§5.1/阶段B 三处一致指向 Bridge 作服务端，两处
  写反的可以当笔误。被否掉是因为用户明确要求两个方向都要，而两种拓扑共用连接抽象、增量很小，
  且插件本来就支持双模式。见 decisions.md: 2026-10-15 - Bridge 同时实现两种 TCP 拓扑。
- **严格一问一答帧同步（先改插件）。** 理由是文档 §5.1 写的是"每帧发送状态消息，阻塞等待输入
  响应"，照做最贴合原文。被否掉是因为 §6.1 又要求 Bridge 在 Scheduler/Worker 崩溃时继续发
  idle，说明 Bridge 必须在参与者缺席时可工作；把屏障定义为"等 Worker + 超时降级"同时满足两条，
  且不必阻塞在插件改造上。见 decisions.md: 2026-10-15 - 帧屏障以 state_update 为帧边界。
- **引入 cJSON 等第三方 JSON 库。** 理由是省事且成熟。被否掉是因为实验室机器不能假设能联网
  装包，且文档要求纯 POSIX 技术栈。见 decisions.md: 2026-10-15 - JSON 自己写最小解析器。
- **C++/CMake 或 Rust/Cargo。** 理由是有更好的抽象和构建。被否掉是因为本机 WSL2 没有 cmake
  而有 gcc/make，课程环境更可能只有 gcc/make。见 decisions.md: 2026-10-15 - OS 核心用 C + GNU make。

## How we will know it works

- **W1.** `cd core && make test` 全部通过：解析器（空对象、嵌套、转义、非法输入）、分帧
  （粘包、拆包、超长帧、零长帧）、协议解析与构造、seqlock 读写一致性。
- **W2.** 无游戏联调：`tools/fake_plugin.py --scenario happy` 与 Bridge 对跑，Bridge 日志出现
  收到 `state_update`、写入共享内存、下发动作，双方退出码 0。
- **W3.** 帧屏障与降级：模拟插件以 60 Hz 上报，测试只写 1/2 个输入槽 → Bridge 对缺失厨师
  下发 idle，日志点名是哪几个厨师，进程不退出。
- **W4.** 心跳超时：模拟插件停止心跳超过阈值 → Bridge 判定断开、暂停下发并记录；重新连接后
  恢复下发。
- **W5.** 异常输入：>1 MiB 的帧、非法 JSON、截断帧各来一次 → Bridge 记录并丢弃，进程存活，
  之后仍能正常处理一帧。
- **W6.** SIGTERM：发信号后进程在 1 秒内退出，日志有退出事件，`/dev/shm` 下不残留
  `/kitchenos_state` 与信号量，立刻重启能成功 bind 同一端口。
- **W7.** 双拓扑：`--link listen --port 12345` 与 `--link connect --host 127.0.0.1 --port 14455`
  两种模式各完成一次完整帧交换。
- **W8.** 实机：Windows 游戏启动后，Bridge 日志出现 `hello` 与 `state_update`（**唯一需要
  启动游戏的一次验证**）。

测试层次：`core/tests/` 是 C 单元测试，覆盖纯函数与共享内存原语（W1）；协议级端到端由
`tools/fake_plugin.py` 驱动（W2–W5、W7），它替代反复启动游戏；游戏实机只留 W8 一次。
出问题时的可观察信号：Bridge 进程退出码非 0、日志停止增长、`/dev/shm/kitchenos*` 残留、
`ss -ltnp` 看不到 12345 监听。

值得测的边界：空 `chefs` 数组、`resources` 里出现未知键（如 `pot_7`）、超大 `orders`、
只连上不发任何消息就断开、连续快速断开重连、`plate_stack.count` 为 0。

## 验收结果

除 W8 外全部实测通过。**最强的一条证据**：`git archive HEAD | tar -x -C /tmp/verify`
导出**已提交**的内容到全新目录，从零构建并跑完两套验证 —— 证明交付物自洽、不依赖工作区里的
未跟踪文件或陈旧产物：

```
干净构建 make exit=0        （0 个 error）
干净构建 make test exit=0   （=== 5 test suites, 0 failed ===）
干净树套件 exit=0            （合计: 5 个场景, 通过 5, 失败 0 ── 结论: 全部通过）
```

| 项 | 状态 | 证据 |
|---|---|---|
| **W1** | ✅ | 5 套件全绿：framing 151 + json 166 + protocol 210 + net 202 + shm 95 = **824 项检查**，`make test` 退出码 0 |
| **W2** | ✅ | `happy`：连上、`hello`、60 Hz `state_update`、480 个动作、`echo_reply` 往返、写共享内存，退出码 0 |
| **W3** | ✅ | `one-slot`：缺槽厨师 **240/240 全 `stop`**；活跃槽稳定收到 `move`；日志点名 `missing=1` |
| **W4** | ✅ | `no-heartbeat`：`plugin_timeout silent_ms=801` → 暂停下发（0 动作）→ 重连 → `plugin_resumed` → 恢复 |
| **W5** | ✅ | `bad-frames`：>1 MiB 帧、非法 JSON、截断帧各一次，进程存活、连接保持、随后照常处理 |
| **W6** | ✅ | SIGTERM 后 **0.05 s**（无插件时 0.020 s）退出、退出码 0、端口立即可重绑、`/dev/shm` 无残留 |
| **W7** | ✅ | `happy`（Bridge 监听 12345）+ `connect-mode`（Bridge 拨号插件 14455）各完成完整帧交换 |
| **W8** | ⏳ | 唯一需要启动游戏的一次，由 `tools/run-real-game.sh` 一条命令完成并自动判定 |

### 与文档的逐条对照

| 文档位置 | 要求 | 实现 |
|---|---|---|
| §834-842「Bridge 进程」 | 唯一连游戏的进程、按帧写共享内存、读输入槽汇总下发、按帧同步、崩溃时发 idle 保持连接 | 全部实现；其中"阻塞等待"实现为**有界等待**，理由见下 |
| §543-553「共享内存 + 信号量」 | `shm_open("/kitchenos_state")` | 默认名一致 |
| §6.2.3（951-966） | "使用 **seqlock 或** 读写信号量保护"；Bridge 独占写、Scheduler/Worker 并发读、Worker 独占写各自输入槽；"这是典型的读者写者问题" | 选 seqlock；权限划分与文档逐条一致 |
| §6.2.4（968-975） | "BepInEx 插件在游戏进程内启动 TCP 服务器（localhost:14455）；Linux 侧 Bridge 作为客户端连接" | `--link connect --port 14455`，实测 PASS |
| §2.2.1 / §5.1 | Bridge 作 TCP 服务端 `0.0.0.0:12345` | `--link listen --port 12345`，实测 PASS |
| §5.2 | 消息类型名与字段 | 与 `Messages.cs` 九个类型名逐字相同，测试里做双向映射校验 |
| §5.3 | 解析失败 ⇒ 记录并丢弃；断开 ⇒ 等待重连 | 逐条实现，`bad-frames` / `no-heartbeat` 覆盖 |
| §6.3 | 用 `sigaction`（不用 `signal`） | `sigaction` + 处理函数只置 `volatile sig_atomic_t` |
| §6.6 | 日志目录与可读性 | `logs/bridge.log`（超集），`ISO8601(毫秒) 级别 事件名 key=value` |

**两处措辞差异，均为有意为之：**

1. **"每帧阻塞等待"实现为有界等待。** 文档同一节还要求"保持连接不断"与 1 秒内响应 SIGTERM；
   真阻塞在系统调用里就同时做不到这两条（心跳与信号都收不到）。语义未变 —— 每帧仍然等齐
   N 个 Worker 的输入，等到发真的、等不到发 idle；变的是等待期间进程还能服务 socket。
   已记入 `.workflow/decisions.md`。
2. **`/kitchenos_sem` 未被采用为默认名。** 文档 553 行那个初始值为 1 的信号量是**保护临界区的
   互斥量**，而 §6.2.3 允许用 seqlock 替代，本文选的就是 seqlock；"通知 Bridge 本帧输入已就绪"
   的信号量（869 行）文档**没有给名字**，故用 `/kitchenos_input_ready`。

**过程中发现的两个真实缺陷**（都会在实机上静默劣化，不崩溃）：

- `kos_shm_wait_frame_after` 的缺格判定不累积：推进水位后才重算"谁不新"，导致**按时响应的
  厨师被判为缺失并收到 idle**。表现为"调度算出了动作但角色不动"，且没有任何错误日志。
- 零长帧被当作失步而断链：`framing.h` 与参考实现 `TcpLink.cs:287` 都认为它是合法空帧，
  §5.3 也把"消息畸形"与"失步"分开。已改为交付空帧、由解析器拒绝该消息、连接保持。

## Open questions

- **Landing 模式**：`standards.md` 现填 `local-merge`。仓库有一个共享远端
  （Shizuku-Yume/wellcooked2），若确定多人同时推，应改成 `pr`。用户一句话即可定。
- **插件侧每帧阻塞**：文档 §6.1 把"每帧发送状态消息，阻塞等待输入响应"算作**插件**职责，
  当前插件是 500ms 定时推送。要不要改由插件 effort 决定；Bridge 两种都能接。

## Notes

- 版本基准：插件 `WellCooked.Plugin` 0.1.0，分帧见 `plugin/WellCooked.Plugin/Net/TcpLink.cs`，
  消息字段见 `plugin/WellCooked.Plugin/Protocol/Messages.cs` 与本文档 §5.2。
- 本机 WSL2 = Ubuntu 26.04，仓库在 `/mnt/d`（9p）；共享内存与信号量在 `/dev/shm`（tmpfs），
  运行时目录（pid、日志）可配置，默认 Linux 原生路径以避开 9p 的文件锁语义差异。
- 相关任务：`.workflow/tasks/bridge/`。
