# 03: Bridge 进程主循环

**Effort:** bridge
**Blocked by:** 01, 02
**Task type:** feature
**Covers:** W2, W5, W6, W7
**Delivers:** 可执行的 `wellcooked-bridge`：TCP 双拓扑链路、按帧写共享内存、帧屏障与 idle 降级、
心跳超时暂停、断线重连、`logs/bridge.log`、SIGTERM/SIGINT 优雅退出与 IPC 清理。
**Files:** `bridge/include/wellcooked/{net,log,bridge}.h`, `bridge/src/{net,log,bridge}.c`,
`bridge/tests/test_net.c`
**Read first:** `.workflow/specs/bridge.md`（全篇）, 本 effort 的 01 与 02 产出的头文件。
**Done when:**
- [x] 默认 `--link listen --port 12345`；`--link connect --host H --port P` 可切换（默认 14455）

> **后续修正（2026-10-15，integrator 记录）：** 上面这条 `Done when` 里的默认端口已变。两种模式
> 现在默认同一个端口 **14455**，默认绑定地址是 **127.0.0.1**（`decisions.md: 2026-10-15 - 端口按
> 组员决策统一成 14455`）。本文件其余部分是当时运行的原始输出，逐字保留，没有回改。
- [x] 每个 `state_update` 一帧：`sem_timedwait` 等 N 个槽，超时对缺失厨师下发 idle 并点名
- [x] Scheduler/Worker 完全不存在时照常连接、照常按帧下发 idle，进程不退出
- [x] 心跳超时 → 判定断开、暂停下发并记录；重连后恢复
- [x] 非法 JSON、超长帧、截断帧 → 记录并丢弃，进程存活；失步则断开重连
- [x] `SIGTERM` 1 秒内退出，`shm_unlink`/`sem_unlink` 清理干净，端口可立即重新绑定
- [x] 日志包含：启动退出、连接与断开、每帧摘要、动作下发、缺失槽、心跳超时、解析失败、信号

**Check:** `cd bridge && make && ./build/wellcooked-bridge --link listen --port 12345 &` 后，
`tools/fake_plugin.py` 能完成 happy 场景；`kill -TERM` 后进程退出且 `ls /dev/shm | grep wellcooked`
为空。

## Progress

**状态：完成。**

`bridge/src/bridge.c`（1387 行）为单线程事件循环：前台运行、不 fork 不 daemonize、在插件/
Scheduler/Worker 全都不存在时照常运行。`bridge.h` 未创建 —— CLI、状态与屏障辅助函数都是
文件内静态的，没有跨编译单元需要共享的契约，为它造一个头文件只会是空壳。

### Check 实测

```
$ cd bridge && make test                    → exit 0，5 套件全绿（test_net 190 项检查）
$ python3 tools/fake_plugin.py --scenario happy --listen-port 12345
  HARD ok  bridge_accepts_plugin_connection — 127.0.0.1:12345
  HARD ok  actions_dispatched — got 480, need >= 20 in 4.0s
  HARD ok  actions_wellformed — 0 malformed
  HARD ok  bridge_writes_state_shm — /dev/shm/wellcooked_c04_happy
  HARD ok  bridge_alive_after_stream
  info     bridge exited on SIGTERM after 0.05s (code 0)
  info     no shm objects left after exit
  PASS happy

$ ./build/wellcooked-bridge --link listen --port 12345 --log /tmp/manual.log &
  12345 已监听: 1
$ kill -TERM $!
  bridge 退出码=0
  12345 是否已释放: 0        （立即可重新绑定）
  退出后 /dev/shm 计数: 0
```

Bridge 自己写下退出路径的两条日志：`signal name=SIGTERM` → `bridge_stop reason=signal
frames=0 dispatched=0 paused=0`。

> **Check 的一处限定（来自 02 的实测）**：裸跑 `ls /dev/shm | grep wellcooked` 在**别人也有
> Bridge 活着**时不会是 0 —— 那些对象属于并发运行。本次所有场景用的都是各自唯一的
> `--state-name`，实测残留为 0。

### 独立验证（超出 Check）

作者另建了一个一次性驱动（不入库），8 个场景 113 项检查全过：`cli`、`noplugin`、`happy`、
`malformed`、`heartbeat`、`connect`、`maxframes`、`worker`。其中值得一提的：

- **`noplugin`**：3 秒内没有任何连接，进程仍活着；SIGTERM 后 **0.020 s** 退出、退出码 0、
  `/dev/shm` 干净、端口立即可重绑、正常运行时 stdout/stderr 为空。
- **`worker`**：自己的槽写入者只喂 0 号槽 → chef0 拿到 93 个 `move` 且目标精确
  （`{x:12.5,y:6.25}`），chef1 拿到 96 个 `stop`，`slots_missing frame=1 chefs=2 mask=0x2 missing=1`。
- **`malformed`**：非法 JSON / 缺 type / 非对象 / 成员类型错各自留下带 `reason=` 的
  `parse_error`，连接保持，下一个合法帧照常处理（74 个动作）。
- **`heartbeat`**：停发心跳 → `plugin_timeout silent_ms=801` → 暂停下发（0 动作）→ 重连 →
  `plugin_resumed reason=link_up silent_ms=2261 timeout_ms=800` → 恢复（59 个动作）。

### 与文档的一处措辞差异（有意为之，理由如下）

文档 §"Bridge 进程"职责第 4 条写的是"按帧同步：每帧**阻塞**等待所有 Worker 完成输入计算
（屏障同步）"。本实现是**有界**等待：每个 `state_update` 起一帧，用
`kos_shm_wait_frame_after(..., 0, &missing)` 非阻塞探测，直到所有槽就绪**或**
`--frame-timeout-ms` 截止，两者先到者触发下发，截止时仍缺失的厨师拿到 idle `stop`。

之所以不真正阻塞在系统调用里：文档对同一个进程还要求"保持连接不断"（§职责第 5 条）与
`SIGTERM` 1 秒内退出（本任务 Done when 第 6 条）。真阻塞住就同时做不到这两条 —— 心跳与
信号都收不到。语义上"每帧等齐 N 个 Worker 的输入、等到就发真的、等不到就发 idle"没有变，
变的是等待期间这个进程还能不能干别的。这是 `.workflow/decisions.md` 里已记录的决策。

### 日志

16 个冻结事件名全部发出，格式为 `ISO8601(毫秒) 级别 事件名 key=value`：

```
2026-10-15T15:41:09.709 INFO  bridge_start link=listen host=127.0.0.1 port=12345 chefs=1 ...
2026-10-15T15:44:30.118 INFO  slots_missing frame=241 chefs=2 mask=0x0 missing= timed_out=0
2026-10-15T15:44:30.118 INFO  actions_sent frame=241 chefs=2 sent=2 idle=0 move=2 interact=0
2026-10-15T15:44:30.331 INFO  state_summary frames=60 chefs=2 orders=1 score=450 game_time=0
2026-10-15T15:41:11.771 WARN  parse_error reason=input ended inside a value detail="..." offset=32 len=32
```

额外还有 `shm_ready`、`net_ready`、`plugin_ack`、`heartbeat_sent`、`echo_reply_sent` 等
十个命名清晰的补充事件。事件名与 `key=value` 的契约是冻结的，改动它会让 harness 的
grep 全部失效，因此宁可加新事件也不改已有措辞。

### 零长帧：一处真实的规格不一致（已修）

原先 `net.c` 把零长帧判为失步并 `net_drop(net, "framing desync: zero-length frame")`。
但两个权威都不同意：`framing.h` 明确写零长帧合法（产出 `len == 0` 的一帧），参考实现
`plugin/WellCooked.Plugin/Net/TcpLink.cs:287` 只拒绝 `len < 0 || len > MaxFrameBytes`，
没有 `len == 0` 分支。按文档 §5.3 的两类故障划分，**消息畸形**应记录并丢弃该消息、
连接保持；只有**失步**才付出断连的代价。零长帧是空消息，不是失步。

修复后 `kos_net_recv_frame` 交付这个空帧，由 `bridge.c` 走 `parse_error` 丢弃并继续；
真正失步（长度前缀越界）仍然断链。harness 中对应断言的实测结果已从"断流"变为：

```
soft ok  zero_length_frame_keeps_connection — bridge kept the connection；
         零长帧的长度 0 未越界，参考实现按『正文为空的一帧』走完并丢弃，不该据此断流（link=open）
```

### 判断与取舍

- **存活判定只看心跳**（§5.3「超时未收到心跳 → 暂停调度」）。持续发状态但不发心跳的插件
  会被判定为已消失；`--heartbeat-timeout-ms 0` 可关闭该判定。超时走 `kos_net_drop()`
  （`net.h` 就是为这种半开连接提供的），`plugin_resumed` 在下一次心跳或重连时触发。
- `--link listen` 只在显式给了 `--host` 时才绑定它，否则用文档默认的 `0.0.0.0`
  （Windows 上的插件要能连进来）。
- `--chefs` 夹到 `KOS_SHM_MAX_CHEFS`(8) 并记 `chefs_clamped` 警告；`--chefs 0` 合法
  （每帧平凡完成）。
- `config` 只记录不执行 —— Bridge 不解释调度策略。
- 断链时刻意只记录 `net.h` 给出的原因文本，不回捞已解码但未取走的帧（规格如此）。

**后续修正（2026-10-15，integrator 记录）：** 分帧改成一行一条 JSON 后
（decisions.md: 2026-10-15 - 分帧改成一行一条 JSON），上面"零长帧"一节的措辞与它引用的
行号（`TcpLink.cs:287`）都过时了，**结论不变**：空行（一个孤立的 `\n`）照样交付 `len == 0`，
由解析器拒绝该消息、连接保持。真正失步的条件从"长度前缀越界/为负"变成"一行超过 1 MiB
仍然没有 `\n`"（有 `\n` 但内容超过 1 MiB 同样是失步），`net.c` 的原因文本相应改为
`framing desync: line exceeds the 1 MiB cap` —— `bridge.c` 仍按其中的 `framing` 字样归类为
`frame_error`。上面那段实测样本现在的措辞是：
`soft ok  zero_length_line_keeps_connection — bridge kept the connection；空行的载荷是空的，
分帧没有失步，参考实现按『正文为空的一行』走完并丢弃（link=open）`。
