# 06: 链路加固与跨实现一致性检查（补记）

**Effort:** bridge
**Blocked by:** 03, 04, 05
**Task type:** feature
**Covers:** W1, W2, W5, W7
**Delivers:** 把"只在极端时序下才出现"的链路问题钉成可重跑的检查，并给两端一个不依赖游戏的对端。
**Files:** `plugin/tools/conformance/`、`tools/run-bridge-scenarios.sh`、`tools/fake_plugin.py`、`core/src/bridge.c`、`core/src/net.c`、`core/src/protocol.c`、`core/include/kitchenos/protocol.h`、`core/tests/test_protocol.c`、`core/tools/worker_stub.c`、`protocol/README.md`、`protocol/schema/`
**Read first:** `.workflow/specs/bridge.md`、`plugin/tools/conformance/README.md`
**Done when:**
- [x] 有一个不用游戏、不用游戏程序集就能跑的跨实现对端，能与真的 Bridge 双向收发
- [x] 场景套件覆盖"接上不出声 / 只有一格输入 / 断连风暴 / 两种对端抢位 / 数据形状"
- [x] 全部检查在干净目录里一条命令复现，退出码可信

**Check:** `bash tools/run-bridge-scenarios.sh` 10 个场景全 PASS 且退出码 0；
`bash plugin/tools/conformance/run-conformance.sh` 的 peer 输出 `VERDICT: PASS` 且 0 malformed，
另一轮 `-StubKind pickup,dash -RequireKinds pickup,dash` 同样 PASS。

## Progress

**这份档案是补记。** 本任务对应的两个提交 —— `6b87e6f`（跨实现对端）与 `297961e`（4 个加固
场景 + 对端 fd 替换日志）—— 当初直接落到 main，没有任务文件、没有 `## Progress`，也没有被
spec 的验收结果引用。它们承载了这套交付里**唯一**"两端分别实现、由第三方校验"的证据，
所以补上档案，让 `flow-close` 与下一轮复核看得见它做过什么、证据在哪。

### 做了什么

1. **跨实现对端**（`plugin/tools/conformance/`，`6b87e6f`）：用插件自己的
   `Protocol/Messages.cs` + `Net/TcpLink.cs`（**未改一行**）编译出一个 .NET 对端，与真的
   Bridge 二进制对跑：Bridge 监听 → peer 连入 → 互发 `hello`/`heartbeat`/`state_update`/`echo`
   → peer 逐条校验收到的 `action` 字段与取值。它同时补上了 Windows/WSL2 边界的三个坑
   （`csc` 不能从 WSL 侧驱动、无 BOM 的 UTF-8、`wsl.exe -e bash <脚本> <参数>` 才能原样传参）。
2. **4 个新场景**（`297961e`）：`reconnect-storm`、`silent-connect`、`data-shapes`、
   `second-peer`；并把对端 fd 的替换写进日志，让"现在是谁在服务"可观测。
3. 场景套件从 5 个变成 **9 个**（`tools/run-bridge-scenarios.sh`），每个场景独立 run id，
   `--strict` / `--allow-skip` 把退出码语义写清楚。

### Check 结果（2026-10-15，本机实测）

```
bash tools/run-bridge-scenarios.sh
  合计: 9 个场景, 通过 9, 失败 0, 跳过 0
  /dev/shm: 本次运行的 IPC 对象已清空
  进程: 没有残留的 kitchenos-bridge / kitchenos-worker-stub
  结论: 全部通过            （退出码 0）
```

关键场景的观测（`logs/scenarios/*`）：

| 场景 | 观测到的 |
|---|---|
| `happy` | 481 个动作、`bridge_writes_state_shm`、`echo_reply` 往返、SIGTERM 后 0.05 s 退出码 0 |
| `one-slot` | 缺槽厨师 240/240 全 `stop`，活跃槽收到 `move`，`slots_missing ... missing=0,1` 点名 |
| `no-heartbeat` | `plugin_timeout silent_ms=801 timeout_ms=800` → 暂停（0 动作）→ 恢复 119 个动作 |
| `bad-frames` | 超长帧断链、坏 JSON 保持同一条连接、零长帧保持连接、截断帧存活 → 之后 71 个动作 |
| `connect-mode` | 来自 127.0.0.1:40929 的拨入被接受，119 个动作 |

```
bash plugin/tools/conformance/run-conformance.sh
  PASS（run-conformance.sh 退出码=0）
  peer: VERDICT: PASS，move=99 stop=265，0 malformed
```

两个脚本的输出都写在 `logs/`（`logs/` 不入库，所以上面的数字与结论抄进了本文件与 spec 的
验收结果）。

### 没做的 / 顺手记下的

- 对端只覆盖协议层：它不发真状态、不注入真输入。它证明的是"两端对同一份协议的读写一致"，
  **不是**"游戏里角色会动" —— 后者只有 W8 能证。
- 真插件的 server 模式（14455）没有被任何一次运行覆盖，见
  technical-debt.md: 插件 server 模式默认端口与文档的 14455 不一致。
- 本任务落地时没有 review 轮次记录（CONVENTIONS §7）—— 这个缺口由下面的 round 1 补上。

## Review boundary（Brief A，round 1，2026-10-15）

```
Review boundary: b61b6f7 + core/, plugin/, protocol/, tools/, .workflow/; round: 1; briefs: A; boundaries: TCP 链路(0.0.0.0:12345 监听与主动连接)+分帧/JSON/协议解码, 插件侧字节解析与输入注入, 日志与 /dev/shm 路径, shell/ps1 脚本; findings: 7 blocking / 7 nonblocking
```

复核对象是 integration diff `git diff b61b6f7..HEAD`（17 个提交、60 个文件），**不是**各任务
自己的 base。7 条 blocking 的处置：

| # | finding | 处置 |
|---|---|---|
| 1 | 插件是本 effort 写的，四份文档却把它当独立基准 | 已改 `protocol/README.md`、`protocol.h`、`specs/bridge.md`（What-this-is-for / must-not-do / Notes）；新增 decisions.md: 插件由本 effort 补写 |
| 2 | `protocol/README.md` §3.2/§4 的心跳事实与代码不符（写 500ms、"任何消息"） | README 改成四个开关的表格并写明 1 秒余量；新增 decisions.md: 链路存活只看心跳 |
| 3 | `6b87e6f`/`297961e` 没有任务档案，最强证据只在 gitignore 的 `logs/` | 本文件 + spec 验收结果引用 |
| 4 | 01–05 都没有 review 轮次记录（CONVENTIONS §7） | 本条 boundary 行 + 本文件；不追改 5 份归档记录的正文，由 `flow-close` 的 retrospective 记下这件事 |
| 5 | `state_update.schema.json` 的 `orders[].recipe` 拒收两端都接受的 `null` | schema 改为 `["string","null"]`；README §3 写明哪些字段标了 null、其余按缺省处理 |
| 6 | `decisions.md: …实机验收不要求改游戏侧配置` 说插件 server 模式是 14455，代码是 12345 | 关档后用户裁定**按文档改代码**：插件新增 `Network.ServerPort`（默认 14455），server 模式用它、client 继续用 `Network.Port`（12345）；spec 的 W7 行与 §6.2.4 对照行同步改写，`technical-debt.md` 的条目按约定随修复删除。**订正一处此前的判断**：这里原写"本机无法编译插件"，是错的 —— 仓库自带 `.tools/dotnet8/dotnet.exe`，`plugin/tools/deploy-plugin.ps1 -Build` 就是用它编的，只是 PATH 上没有 `dotnet`、系统也没装 .NET SDK。插件已重新编译（0 警告 0 错误）并部署。**仍未运行验证**：插件 server 模式没被任何一次实机覆盖，W7 的措辞只声称静态对齐 |
| 7 | 输入槽的 PICKUP/DASH 被静默降级为 `stop`，`shm.h` 却写"名字可以直接下发" | 用户选择**扩协议**：`action` 枚举从 3 个扩到 5 个（补 `pickup`/`dash` 的 C 常量与构造器、`build_action_from_slot` 直接映射、README §3.6 与 `action.schema.json` 同步），见 decisions.md: action 枚举扩到 5 个；`shm.h` 措辞一并改正。原先"只记 WARN、不扩协议"的处置已被用户否掉 |

7 条 nonblocking 的处置：

1. `sem_timedwait` 措辞（spec §并发规则 + 措辞差异 #1）→ 已改成"不阻塞探测 + 帧截止时间"。
2. 无 `bridge.h` 而 spec 的模块表列了 `bridge` → spec 表头已说明它是可执行程序，没有头文件。
3. 验收表数字过时（5 个场景、缺一致性结果、W8 仍 `⏳`）→ 已更新成 9 个场景 + 一致性 PASS +
   W8 "一半"。
4. "版本号不匹配则拒绝启动"与 `KOS_SHM_RECREATE` 冲突 → spec 失败行为改写；新增
   decisions.md: 重启时清掉崩溃残留的共享内存。
5. `.gitattributes` 的 `* text=auto eol=lf` 在 bridge 任务里改成、没有记录 → 补记在本文件；
   另加 `.bat`/`.cmd` 的 CRLF 例外。
6. sanitizer 运行不可复现 → `core/Makefile` 新增 `make test-san`。
7. 链路无认证 + 日志无轮转 → 新增 decisions.md: 链路不做认证，默认监听 0.0.0.0（写明风险
   边界与代价）。

## 收尾补记（2026-10-15，用户指示"除启动游戏外全部修完"）

改动清单（`git status --short`：21 个改动 + 1 个新文件；`git diff --stat` = 579 insertions / 108
deletions）：

```
 M .gitattributes                                  bat/cmd 的 CRLF 例外
 M .workflow/decisions.md                          新增 5 条（上表 #1/#2/#4/#7 与"链路不做认证"；
                                                   #7 已按用户选择改写成"action 枚举扩到 5 个"）
 M .workflow/technical-debt.md                     新增 1 条（插件 server 模式端口）；
                                                   "输入槽的动作词表比线上协议宽"随协议扩张删除
 M .workflow/specs/bridge.md                       前提/失败行为/W 表/验收结果/模块表/§5.2 对照行/测试层次
 M .workflow/done/bridge/05-real-game.md           补记：插件作者、"真实插件"的含义、W8 仍缺一半
 M core/Makefile                                   test-san 目标（ASan/UBSan，独立 build 目录）
 M core/include/kitchenos/protocol.h               pickup/dash 常量与构造器声明；跨端契约基准写明
 M core/include/kitchenos/shm.h                    动作词表与线上枚举一比一，写明
 M core/src/bridge.c                               pickup/dash 直接映射 + actions_sent 计数；
                                                   stop/none 之外的构造失败记 WARN
 M core/src/protocol.c                             build_bare_action() 复用于 stop/pickup/dash
 M core/tests/test_protocol.c                      5 个 action 的构造/往返/缓冲区不足用例（210 → 226 项）
 M core/tools/worker_stub.c                        --kind move|stop|interact|pickup|dash
 M protocol/README.md                              §0 基准、§3 null 规则、§3.2 心跳表、§3.6 五个动作、§4
 M protocol/schema/action.schema.json              action 枚举扩到 5 个 + 例子
 M protocol/schema/hello.schema.json               actions 的描述跟着枚举
 M protocol/schema/state_update.schema.json        orders[].recipe 允许 null
 M plugin/tools/conformance/README.md              §2/§3/§4/§5/§6：-StubKind 用法、两轮证据文件、BOM 坑
 M plugin/tools/conformance/run-conformance.ps1    第 4 个参数 -StubKind（**文件必须保留 UTF-8 BOM**）
 M plugin/tools/conformance/run-conformance.sh     第 4 个参数 stub-kind（最多两个槽位，每槽位一份 stub 日志）
 M tools/fake_plugin.py                            新增 pickup-dash 场景；动作名校验收紧到线上五个；
                                                   start_stub() 支持 kind
 M tools/run-bridge-scenarios.sh                   场景列表 9 → 10
?? .workflow/done/bridge/06-link-hardening-and-conformance.md   本文件（补记）
```

### 补记：action 枚举扩到 5 个（用户选择，2026-10-15）

复核时给的处置是"不扩协议，只把 pickup/dash 的日志从 DEBUG 提到 WARN"，**用户否掉了它**，
选择"补 builder、扩协议到 5 个动作"。缺口只在 C 侧：插件 `InputInjector` 本来就实现八个动作
（`Plugin.cs` 的 `HostActions`）并在 `hello` 里宣告，`kos_action_kind` 也一直有这两个取值，缺的
只是 C 侧不去构造它们 —— 所以这次扩张没有碰插件，也不需要 C# 工具链：

- `protocol.h` 加 `PROTO_ACTION_PICKUP`/`PROTO_ACTION_DASH` 与两个构造器；`protocol.c` 把
  stop/pickup/dash 这三个"只有名字"的动作收进一个 `build_bare_action()`（`stop` 的输出逐字节
  不变，已有用例保证）；`bridge.c` 的 `build_action_from_slot()` 直接映射这两个 kind，成功分支按
  kind 分别计数并写进 `actions_sent`（新增 `pickup=`/`dash=`）。现在构造失败只剩两类：`stop`/`none`
  （空转，DEBUG）与"参数不可用的 move/interact、未知 kind"（WARN，`reason=slot-action-rejected`）。
  这两条都被实测过（下节 `logs/warn-probe.py`）：槽里写合法的 `stop` 时 0 条 WARN；把 stub 改成
  "写 interact 但 `target_object` 为空"时，桥记 42 条
  `msg_ignored reason=slot-action-rejected kind=interact chef=0 detail=required field is missing
  (fell back to idle stop)`，对端一条 `interact` 都没收到、只收到降级后的 `stop`。
- 契约同步：`protocol/README.md` §3.6 的动作列表与"只有名字的动作"段落、
  `action.schema.json` 的 `enum` + 例子、`hello.schema.json` 的 `actions` 描述。
- 四层验证：① `core/tests/test_protocol.c` 增 16 项（210 → 226）：两个新构造器的逐字节输出、
  往返解析、"缓冲区装不下要报错"；② 场景套件新增 `pickup-dash`：两个 stub 分别往 slot 0/1 写
  `pickup`/`dash`，断言各 ≥3 条原样到达对端、0 条被对端拒收、Bridge 存活；③ 跨实现对端多跑一轮
  `-RequireKinds pickup,dash -StubKind pickup,dash`，插件自己的 `InboundMessage` 解出
  `pickup=109 dash=110`、`VERDICT: PASS`；④ `logs/schema-check.py`（一次性证据脚本，写在已
  gitignore 的 `logs/` 下，不是交付物）把 150+150 条**真实帧**逐条过 `action.schema.json`，并验证
  负例（`press`/`release`/`tap`、move 少 target、interact 少 target_object、缺 action）确实被拒。
- `technical-debt.md` 的"输入槽的动作词表比线上协议宽"随这次改动删除（改完即删是本仓库的约定）。

### 顺带修掉的一处自伤：`test-san` 会让下一次 `make` 失败

`make test-san` 原先 `clean` 之后用同一套 `build/obj` 编译带 sanitizer 的目标文件，跑完留在树里；
下一次裸 `make` 拿这些 `.o` 去链接（命令行里没有 `-fsanitize=address`）就死在一串
`undefined reference to __asan_init` —— 失败由上一个命令造成，看起来却像当前改动坏了。现在
sanitizer 的目标文件写到 `build/san/`，正常构建与它互不干扰；`test-san` 之后裸 `make` 退出码 0。
`build/san` 随 `core/build` 一起被 gitignore 覆盖。

### 验证命令与输出（2026-10-15，本机；最终状态）

```
cd core && make clean && make          exit=0（-Wall -Wextra -Werror，0 warning）
cd core && make test                   exit=0（framing 151 + json 166 + protocol 226
                                       + net 202 + shm 95 = 840 项检查，0 失败）
cd core && make test-san               exit=0（ASan/UBSan 走 build/san，5 套件 0 失败、0 sanitizer 报告）
cd core && make                        exit=0（紧接 test-san 之后；不再出现 __asan 链接错误）
python3 logs/schema-check.py           exit=0（SCHEMA CHECK: PASS；真实帧 pickup=150 dash=150 全过，
                                       6 条负例全被拒）
python3 logs/warn-probe.py             exit=0（WARN PROBE: PASS；合法 stop 0 条 WARN，
                                       空 target 的 interact 42 条 WARN 且降级为 stop）
bash tools/run-bridge-scenarios.sh     exit=0（合计: 10 个场景, 通过 10, 失败 0, 跳过 0；
                                       /dev/shm 已清空，无残留进程）
bash .workflow/bin/workflow validate   Workflow is valid（6 task files）
powershell -NoProfile -File plugin/tools/conformance/run-conformance.ps1
                                       exit=0（VERDICT: PASS，move=106 stop=258）
  … -RequireKinds pickup,dash -StubKind pickup,dash
                                       exit=0（VERDICT: PASS，pickup=109 dash=110 stop=145）
```

本文件头部的 `Check:` 由这次收尾从"9 个场景"改成"10 个场景 + 第二轮一致性"，上面就是改后的实测；
`### Check 结果` 那一节保留的是 `297961e` 落地当天的原始输出（9 个场景），两者不矛盾，是先后两次运行。

干净目录复现（`tar` 排除 `.git`/`core/build`/`logs` 导出 **203 个文件**到 `/tmp/verify3`）：

```
clean-dir make exit=0
clean-dir make test exit=0        （=== 5 test suites, 0 failed ===，840 项检查）
clean-dir make test-san exit=0    （=== 5 test suites, 0 failed ===）
clean-dir 场景套件 exit=0         （合计: 10 个场景, 通过 10, 失败 0, 跳过 0 ── 结论: 全部通过）
```

**唯一没做的是启动游戏**：W8 的后半（进关卡后 `chefs`/`orders` 非空）按用户指示不在本轮范围内，
它需要的是一次 `bash tools/run-real-game.sh` 加进关卡。
