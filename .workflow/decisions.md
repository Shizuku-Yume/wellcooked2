# Decisions

Every choice that mattered, with the option it beat. Newest last.

This file exists so the same argument is not had twice. A decision recorded here
is settled. The file is strictly append-only: to reverse or reopen a decision,
append a new entry that cites the old one. The one exception is below. Git merges
it with the union driver (`.gitattributes`), so parallel branches can each append.

## Why the runner-up is required

A decision recorded without the alternative it beat will be reopened next session:
the reader cannot see why the obvious option was rejected.

Draft entries during `flow-grill`, while the options are fresh. Write the strongest
honest case for each alternative. If writing "Instead of" feels like inventing a
strawman, that option was never real; find the real one, or accept the decision was
obvious and doesn't need logging.

## Format

This is the only copy of the format; other documents point here. Each entry is a
level-2 heading `## <YYYY-MM-DD> - <title>` under `## Entries`. Subsections (`###`),
fenced code blocks and comments are not entries. Cite an entry by its heading text:
`decisions.md: <date> - <title>`.

Every entry has all five fields, none empty:

- **Decided:** what was chosen
- **Instead of:** the real alternative that was seriously considered
- **Because:** the constraint, measurement, or reasoning that eliminated it
- **Mine:** yes if an agent decided, no if the user decided
- **Revisit when:** the observation that would make this wrong

Optional fields:

- **Effort:** the effort slug this decision belongs to. Find one effort's entries
  with `grep -n 'Effort:\*\* <slug>' .workflow/decisions.md`. Omit it when the
  decision is not tied to one effort.
- **Supersedes:** the heading of the entry this one reverses. The only edit allowed
  to an old entry is then appending one line to it: `**Superseded by:** <new
  heading>`. An entry with `Superseded by` is history: follow the pointer before
  acting on it.

```markdown
## 2026-01-31 - Retries capped at 3, no backoff

- **Decided:** three immediate retries, then fail.
- **Instead of:** exponential backoff with jitter.
- **Because:** the only caller retries within a single user request; a 30-second
  backoff would outlive the request it is serving.
- **Mine:** no (asked the user).
- **Revisit when:** a background job starts calling this.
```

Field names stay in English for `workflow validate`; their content can be in any
language.

`Mine` shows which calls came from an agent: still real decisions, but ones the
user may overturn on sight. `Revisit when` of "never" is allowed but suspicious;
`flow-close` reads it for every entry of the effort it closes.

## Entries

## 2026-10-15 - Bridge 同时实现两种 TCP 拓扑，默认监听 0.0.0.0:12345

- **Decided:** Bridge 默认以 server 模式监听 `0.0.0.0:12345`，插件作客户端连入；同时实现
  `--link connect --host <ip> --port 14455` 模式，由 Bridge 作为客户端去连插件开的服务端。
  两种模式跑同一套分帧与消息协议。
- **Instead of:** 只按文档 §2.2.1/§5.1/阶段B 实现 server 模式，把 §6.1、§6.2 里"插件开
  TCP 服务器 14455、Bridge 作客户端"当作笔误忽略。
- **Because:** 文档四处表述指向 server 模式（§2.2.1 架构图写死 `TCP Server (0.0.0.0:12345)`、
  §2.2.1 技术栈表把插件功能写成"TCP客户端"、§5.1 的 C 样例 `htons(12345)` 且注释"调度器侧
  （服务端）"、阶段B"监听 TCP 端口"），两处（§6.1、§6.2）写反。用户明确要求两个方向的链路
  都要有。两种拓扑共用连接抽象，额外成本很小，且插件的 TcpLink 本来就支持 client/server
  双模式，两种都能实机跑。
- **Mine:** no
- **Revisit when:** 实机验证证明某一方向在 WSL2 网络模式下不可用（例如 NAT 与 mirrored 模式
  切换导致 Windows 连不进 WSL2）。
- **Effort:** bridge
- **Superseded by:** 2026-10-15 - 端口按组员决策统一成 14455（只推翻默认监听地址与端口那一半；
  "两种拓扑都要有"仍然成立）

## 2026-10-15 - 帧屏障以 state_update 为帧边界，等待 Worker 而非等待插件

- **Decided:** Bridge 把每个 `state_update` 当作一帧边界，收到后等待全部 N 个 Worker 输入
  槽就绪（`sem_timedwait` 有上限），超时或槽缺失的厨师下发 idle 动作；它不要求插件在帧内
  阻塞应答。
- **Instead of:** 按文档 §5.1 "每帧发送状态消息，阻塞等待输入响应"实现严格的一问一答帧同步，
  即先改插件让它每帧阻塞。
- **Because:** 文档 §6.1 同时要求"当 Scheduler/Worker 崩溃时，Bridge 继续运行并向游戏发送
  idle 输入"，所以 Bridge 无论如何必须能在参与者缺席时继续工作；把屏障定义成"等 Worker 且
  有超时降级"既满足文档语义，又能在 Scheduler/Worker 尚不存在时独立验证。改插件是插件侧的
  独立工作量，不该阻塞 Bridge。
- **Mine:** yes
- **Revisit when:** Worker 进程落地、需要按真实帧率严格同步时，重新评估是否要求插件帧内应答。
- **Effort:** bridge

## 2026-10-15 - OS 核心用 C + GNU make，放 bridge/；协议契约放 protocol/

- **Decided:** Linux 侧 OS 核心用 C（C99），构建用 GNU make，代码放 `bridge/`；跨端协议说明与
  JSON Schema 放 `protocol/`，作为两端唯一契约。
- **Instead of:** 用 C++17 + CMake，或 Rust + Cargo。
- **Because:** 文档 §2.2.1 允许 C/C++/Rust。本机 WSL2 里 cmake 与 gdb 都没装而 gcc/make 齐备，
  课程实验室环境更可能只有 gcc/make。C 与文档里那些 POSIX 调用片段逐行对应，答辩时能直接对照
  课件讲。
- **Mine:** yes
- **Revisit when:** 代码规模增长到手工维护 Makefile 变得昂贵，或需要跨平台构建矩阵。
- **Effort:** bridge

## 2026-10-15 - JSON 自己写最小解析器，不引第三方库

- **Decided:** 在 `bridge` 内实现只覆盖本项目消息所需子集（对象、数组、字符串、数字、布尔、
  null）的递归下降解析器与写入器。
- **Instead of:** 引入 cJSON 或 jansson，把 JSON 解析外包给成熟库。
- **Because:** 文档 §2.2.1 要求纯 POSIX 技术栈，`.workflow/standards.md` 规定不引入第三方 C 库
  （实验室机器不能假设能联网装包）。本项目消息结构浅且固定，子集解析器可完整单测。
- **Mine:** yes
- **Revisit when:** 协议扩展到需要通用 schema 校验或深层嵌套结构。
- **Effort:** bridge

## 2026-10-15 - 零长帧当作空消息交付，不当作流失步

- **Decided:** 长度前缀为 0 的帧照常交付（`len == 0` 的一帧），由解析器拒绝该消息并记
  `parse_error`，连接保持。只有长度前缀越界（< 0 或 > 1 MiB）才断链。
- **Instead of:** 原实现把 `len == 0` 判为 framing desync 并断开连接。
- **Because:** `framing.h` 明确承诺会产出 `len == 0` 的帧，参考实现
  `plugin/WellCooked.Plugin/Net/TcpLink.cs:287` 也只拒绝 `len < 0 || len > MaxFrameBytes`。
  文档 §5.3 把"消息畸形"（记录并丢弃该消息）与"失步"（付出断连代价）分成两类，空消息属于
  前者；拿一次连接闪断去换一个解析器本来就会拒绝的帧，代价与收益完全不成比例。
- **Mine:** yes
- **Revisit when:** 有第三方实现给零长帧赋予特殊语义（例如当心跳），或协议需要区分
  "空消息"与"没有消息"。
- **Effort:** bridge

## 2026-10-15 - 共享状态用 seqlock；就绪信号量另起名字

- **Decided:** 世界状态用 seqlock 保护；"本帧输入已就绪"的通知信号量命名为
  `/kitchenos_input_ready`，不采用文档代码示例里的 `/kitchenos_sem`。
- **Instead of:** 走 §6.2.3 的另一条路——用初值为 1 的 `/kitchenos_sem` 当互斥量，
  由 Bridge 在写临界区前后 `sem_wait`/`sem_post`。
- **Because:** §6.2.3 原文是"使用 seqlock **或**读写信号量保护"，择一即可。文档 553 行的
  `/kitchenos_sem` 初值为 1，是**互斥量**那一路的名字；而"通过信号量通知 Bridge 本帧输入
  已就绪"（869 行）的那个信号量文档没有给名字。本项目的访问模式是 Bridge 单写、
  Scheduler 与多个 Worker 并发读，正是 seqlock 的适用形状，且读者永不被写者阻塞。
- **Mine:** yes
- **Revisit when:** 出现需要写者之间互斥的共享区（seqlock 只解决单写多读），或课程验收
  明确点名要看到 `sem_wait`/`sem_post` 保护临界区。
- **Effort:** bridge

## 2026-10-15 - 实机验收不要求改游戏侧配置

- **Decided:** Bridge 的默认监听端口取插件默认连接的那个端口（12345），因此 W8 不需要编辑
  游戏侧任何配置文件即可完成；插件的 server 模式（14455）保留给相反拓扑使用。
- **Instead of:** 把"改 `BepInEx/config` 里的 `Network.Host`/`Network.Port`"写进 W8 的步骤。
- **Because:** 每多一个手改配置的步骤，就多一种"验收失败其实是配置没改"的假失败，而排查
  这种失败要花的时间远超过它省下的。两边的默认值本来就该指向同一个端口，维持它们相等是
  零成本的；`127.0.0.1` 若跨不过 WSL2 边界，回退到 WSL IP 是一条命令的事，已写在失败提示里。
- **Mine:** yes
- **Revisit when:** 插件的默认端口发生变更，或 Bridge 需要一个不与游戏共存的固定端口。
- **Effort:** bridge
- **Superseded by:** 2026-10-15 - 端口按组员决策统一成 14455（端口取值那一半；"实机验收不要
  求改游戏侧配置"仍然成立，而且现在两端默认值相同，更不需要改）

## 2026-10-15 - 插件由本 effort 补写，两端同轮交付

- **Decided:** bridge effort 连 Windows 侧插件一起交付（`plugin/WellCooked.Plugin`，`a67ea3b`），
  并把 `protocol/`（README + `schema/`）当作两端唯一契约，而不是假定插件已经存在。
- **Instead of:** 只交付 Bridge 与它依赖的共享层，把 W8 记为"无法运行"，等后续插件 effort 再补。
- **Because:** 用户要的是能实机跑通；当时仓库里没有任何插件，Bridge 在真游戏上一天也没跑过，
  `state_update` 只能来自 `tools/fake_plugin.py`，协议就只能是自测自证。补写插件的成本可控
  （BepInEx + Harmony + 一个自有 TCP 客户端），换来 W8 从"不可运行"变成"可运行"。
  这条是**补记**：当时没记，随后几份文档因此把插件当成了"现有实现" —— 见
  `decisions.md: 2026-10-15 - 零长帧当作空消息交付` 里的"参考实现 `TcpLink.cs:287`"、
  `protocol/protocol.md` 的开头与 `specs/bridge.md` 的"以插件现有实现为基准"。那些措辞已更正；
  它们各自的**决定**不受影响。
- **Mine:** yes
- **Revisit when:** 出现第三个实现方（例如同学独立写的插件），或课程要求两端由不同人实现 ——
  那时"两端同轮写成"不再可接受，跨端一致性必须由第三方实现来证。
- **Effort:** bridge

## 2026-10-15 - 链路存活只看心跳

- **Decided:** Bridge 用"最近一次收到 `heartbeat` 的时间"判断插件是否活着
  （`--heartbeat-timeout-ms`，默认 3000 ms，0 = 关掉判定），`state_update` 不刷新它；插件侧同理
  （`Network.HeartbeatTimeoutMs`，默认 0 = 关掉该判定）。
- **Instead of:** 按文档 §5.3 的字面意思用"任何消息都没来"判定断开（那样 `state_update` 也算
  活着），或者干脆取消超时判定、只靠 TCP 断连。
- **Because:** 状态上报高频、心跳低频。若任何消息都算活着，插件持续发状态而心跳线程死掉时
  Bridge 永远发现不了，"插件还在"与"插件的心跳还在"变成同一件事，暂停/恢复那套逻辑就没意义了。
  代价是两侧默认余量只有 1 秒（插件 `HeartbeatMs=2000` vs Bridge 3000），已连同四个开关写进
  `protocol/protocol.md` §3.2 的表格；要更稳就把 `--heartbeat-timeout-ms` 调到 6000，或把插件的
  `HeartbeatMs` 调小。README §3.2/§4 原先写的"任何消息"与代码不符，已改。
- **Mine:** yes
- **Revisit when:** 实机上出现"状态流正常却被判失联"的误断（先看 `logs/bridge.log` 里的
  `plugin_timeout silent_ms=`），或插件改成由状态上报本身承担心跳。
- **Effort:** bridge

## 2026-10-15 - 链路不做认证，默认监听 0.0.0.0

- **Decided:** Bridge 与插件之间不做认证与加密。Bridge 默认监听 `0.0.0.0:12345`，任何连上的
  对端都会被当成插件（新连接顶掉旧连接），它发来的 `state_update` 会写进 `/kitchenos_state`；
  `--link connect` 模式下 Bridge 发来的 `action` 会被插件当作游戏输入执行。
- **Instead of:** 在 `hello` 里带一个共享令牌、两端校验，或默认只绑 `127.0.0.1`。
- **Because:** 单机课设：两端同机，插件默认连 `127.0.0.1`，文档也没要求认证。加令牌要同时改
  两端并重做一次实机验证，而插件在本机**无法编译**（缺 BepInEx 与游戏程序集），改了也验不了。
  风险边界写在复核记录里，明说而不是默认：同机或同网段的任何人都能冒充插件、往共享内存灌
  伪造世界状态、或把 `logs/bridge.log` 刷满（日志不轮转）。解析路径本身没有内存安全问题
  （长度上限、深度上限、数组上限、真实原子屏障，ASan/UBSan 下 824 项检查全绿）。
- **Mine:** yes
- **Revisit when:** 换到多人共用的机器、需要跨网段演示，或端口要暴露到局域网之外。
- **Effort:** bridge
- **Superseded by:** 2026-10-15 - 端口按组员决策统一成 14455（只推翻"默认监听 `0.0.0.0`"这一半，
  也就是"任何连上的对端都成为被服务的插件"那条暴露面；"链路不做认证"本身仍然成立）

## 2026-10-15 - action 枚举扩到 5 个：pickup / dash 上线

- **Decided:** 线上 `action` 从 move/stop/interact 扩到**五个**，补上 `pickup` 与 `dash`：
  `protocol.h` 加 `PROTO_ACTION_PICKUP` / `PROTO_ACTION_DASH` 与
  `protocol_build_action_pickup()` / `_dash()`（与 `stop` 共用"只有名字"的构造体），
  `bridge/src/bridge.c` 的 `build_action_from_slot()` 直接映射这两个 kind，
  `protocol/protocol.md` §3.6 与 `protocol/schema/action.schema.json` 同步放宽；插件的
  `press`/`release`/`tap` 仍然只留在插件里。
- **Instead of:** 保持三种动作，让 `build_action_from_slot()` 把 pickup/dash 降级为 idle stop、
  只把日志从 DEBUG 提到 WARN（这是复核时 agent 先选的那条路，被用户否掉）。
- **Because:** 缺口只在 C 侧：插件 `InputInjector` 本来就实现八个动作并在 `hello` 里宣告，
  `kos_action_kind` 也一直有这两个取值，缺的只是 C 侧不去构造它们 —— 所以这次扩张**不需要碰
  插件、也不需要 C# 工具链**，可以用 `bridge/tests/test_protocol.c` 的构造/往返用例、
  `tools/fake_plugin.py` 的 `pickup-dash` 场景（两个 Worker 分别写 pickup/dash，断言原样到达
  对端）和放宽后的 schema 来验证。只记 WARN 的代价是：Worker 写了 dash 却永远等不到它生效，
  而那在日志里只是一条"降级"、不是一个错误。
- **Mine:** no
- **Revisit when:** 需要按帧精确调度原始按键（`press`/`release`/`tap`），或插件的动作语义与
  输入槽取值再次分叉。
- **Effort:** bridge

## 2026-10-15 - 重启时清掉崩溃残留的共享内存，不拒绝启动

- **Decided:** Bridge 启动时用 `KOS_SHM_CREATE` 打开 `/kitchenos_state`；对象已存在就记一条
  `shm_recreate` WARN、unlink 后重建，而不是报错退出。魔数/结构版本/尺寸校验只用于**挂接**
  （`KOS_SHM_ATTACH`，Scheduler/Worker 走这条），不匹配就明确拒绝。
- **Instead of:** 按 spec 早先那句"写 shm 前先确认对端结构版本号，不匹配则拒绝启动"，Bridge
  遇到任何已存在的段都退出。
- **Because:** 上一次运行被 SIGKILL 或崩溃之后段会留在 `/dev/shm`。若拒绝启动，用户就得先手工
  `rm /dev/shm/kitchenos_state*` 才能再跑，而这条命令不在任何文档里 —— 那是个只有作者知道的隐性
  前置条件。段的唯一写入者就是 Bridge 自己，重建不会破坏别人的数据；真正的多进程读者走 ATTACH，
  那条路上三项校验一个不少。spec 里那句话已改成这个行为。
- **Mine:** yes
- **Revisit when:** 出现第二个长期存活的世界状态写入者，或 Scheduler 需要在 Bridge 重启后沿用
  上一帧的状态。
- **Effort:** bridge

## 2026-10-15 - 插件 server 模式单独用 14455，不再与 client 模式共用端口

- **Decided:** 插件新增 `Network.ServerPort`（默认 14455），server 模式监听它；client 模式继续
  用 `Network.Port`（默认 12345）。Bridge 的 `--link connect` 默认拨 14455 不变。
- **Instead of:** 把 Bridge 的 `--link connect` 默认端口改成 12345 去迁就插件，让两端继续共用
  一个端口号。
- **Because:** 课程文档 §6.2.4 明写"插件在游戏进程内启动 TCP 服务器（localhost:14455）"，而
  `decisions.md: …实机验收不要求改游戏侧配置` 已经定下"验收不该要求手改游戏侧配置"。共用一个
  端口号会让这两个要求互相打架：照文档跑相反拓扑必然先撞一次"连不上"，然后才发现要改配置 ——
  正是那条决策要消灭的假失败。两种模式的默认值各归其位之后，两个方向都按文档默认值直接可用。
- **Mine:** no
- **Revisit when:** 需要让同一个游戏实例同时充当 server 与 client，或课程验收要求两种模式共用
  一个端口号。
- **Effort:** bridge
- **Superseded by:** 2026-10-15 - 端口按组员决策统一成 14455（两种模式现在就是共用一个端口号：
  这正是本条目 `Revisit when` 里那两个条件之一，由用户裁定触发）

## 2026-10-14 - OS 核心用 C11

- **Decided:** Supervisor、Scheduler、Worker、Bridge 四个进程用 C11 + 纯 POSIX API 实现。
- **Instead of:** C++17（容器和 RAII 省事）或 Rust（内存安全、进程管理库成熟）。
- **Because:** 方案 §2.2.2 的守护进程示例和课件示例都是 C；本机已装 gcc 16.2.1、g++、make、cmake、ninja，没有 rustc/cargo，第4周装工具链不划算；答辩占 60%，C 与课件系统调用（fork/setsid/mq_open/shm_open/fcntl）的对应关系最直接。
- **Mine:** no (asked the user).
- **Revisit when:** 共享内存布局和消息序列化开始出现只能靠手工约定的复杂度，或组员侧有 Rust 经验且愿意承担工具链。

## 2026-10-14 - 先在 GitHub 开发，答辩前镜像到 Gitee

- **Decided:** 日常开发和 PR 走 GitHub（origin = Shizuku-Yume/wellcooked2），答辩前把最终历史同步到 Gitee。
- **Instead of:** 立刻把仓库迁到 Gitee，之后只在 Gitee 上开发。
- **Because:** 方案 §2.4 和 §12 提到 Gitee 提交质量，但当前远端和组员访问都在 GitHub，第4周时间紧，迁移会打断编码。
- **Mine:** no (asked the user).
- **Revisit when:** 老师明确要求以 Gitee 链接提交，或组员访问不了 GitHub。

## 2026-10-14 - TCP 连接由 Bridge 监听，游戏插件作为客户端

- **Decided:** Linux 侧 Bridge 监听 TCP（默认 `127.0.0.1:14455`，跨机时改绑定地址），BepInEx 插件作为客户端连接并重连。
- **Instead of:** 插件在游戏进程内监听、Bridge 主动连接（方案 §6.1、§6.2 的写法）。
- **Because:** 方案 §2.2.1 的架构图和 §9 阶段B 都要求 Bridge 监听；游戏由人手动启动和重启，插件做客户端只需要一个重连循环，比维护监听生命周期简单；WSL2 默认开启 localhostForwarding，Windows 侧能直连 WSL2 里的监听端口，反向则要取宿主 IP 并处理防火墙。
- **Mine:** yes.
- **Revisit when:** 组员在 Windows 侧验证发现连不到 WSL2 的监听端口，而 Windows 侧监听能被 WSL2 连上。
- **Effort:** game-bridge

## 2026-10-14 - 不复制 overcooked-supercharged 的代码，只借用游戏类型与字段名

- **Decided:** 参考项目里只复用 MIT 许可的 `gua248/Overcooked2-TAS` 和 `toasterparty/oc2-modding` 的代码片段（保留版权声明）；`hpmv/overcooked-supercharged` 只用来查游戏自身的类型名和字段名这类事实，不复制它的代码。
- **Instead of:** 按方案 §6.1 的建议直接提取 supercharged 的 `InjectorServer` 和 `ControllerHandler` 逻辑。
- **Because:** `gh api repos/hpmv/overcooked-supercharged/license` 返回 404，仓库没有声明许可证，未授权代码不能进我们的仓库；游戏类型和字段名是游戏自身的接口事实，不受其许可约束。另外这两个类不只是"状态序列化与输入注入"，还含 TCP/Thrift 服务、暂停与帧提交，照方案的说法提取会把它的架构一起搬进来。
- **Mine:** yes.
- **Revisit when:** 该仓库补上许可证，或我们自己实现了状态读取和输入注入后不再需要参考它。
- **Effort:** game-bridge

## 2026-10-14 - 输入注入走进程内 InControl 设备

- **Decided:** 插件在游戏进程里建一个 `InControl.InputDevice`（`AddControl` 注册控制项），用 `InputManager.AttachDevice` 挂上，每帧设置按键状态；不动 `PlayerInputLookup` 的逻辑按钮层。
- **Instead of:** Harmony 补 `PlayerInputLookup.GetButton/GetValue`，返回自己的 `ILogicalButton`/`ILogicalValue` 包装对象（supercharged 的 `InputPatching.cs`、TAS 的 `TASControl.cs` 都这么做）。
- **Because:** spike `game-bridge/06` 实测：逻辑层替换本身成立（游戏把我们的对象轮询了 2400 多次），但本次测的按钮和场景（标题画面的继续/加入流程）完全不响应，画面不动；换成进程内虚拟手柄后，每 3 秒按 0.3 秒 A 键，卷帘门升起、进入队伍大厅（连拍前后两帧 + 另两次运行都成立）。**推断（没有直接证明）**：前端的"按 A 继续"走设备/加入流程而不是逻辑按钮层——这两次观察符合这个解释，但没有反证过其它解释。逻辑层在关卡内能不能用这次没测，不作结论。
- **Mine:** yes.
- **Revisit when:** 关卡内的动作指令（移动、交互）在设备层表达不了，或游戏改版换掉 InControl。
- **Effort:** game-bridge
- **Superseded by:** 2026-10-15 - 输入注入改走逻辑层（借 gua248/Overcooked2-TAS 的方案）（正是本条
  `Revisit when` 的第一个条件触发的：任务 09 在"切菜键是 X 不是 A"上丢过一次交互。本条对**前端**
  的结论仍然成立 —— 前端加入走设备层）

## 2026-10-15 - 分帧改成一行一条 JSON，不再用 4 字节长度前缀

- **Decided:** TCP 上的消息边界改成一个 `\n`（0x0A）：一帧 = UTF-8 JSON 载荷 + `\n`，载荷上限
  仍是 1 MiB（不含 `\n`，`TcpLink.MaxFrameBytes` 不变）。契约写在 `protocol/protocol.md` §1；
  三处实现一起改：`bridge/src/framing.c`、`plugin/WellCooked.Plugin/Net/TcpLink.cs`、
  `tools/fake_plugin.py`。`FRAMING_HEADER_LEN` 与 `framing_peek_length()` 删除，换成
  `FRAMING_TERMINATOR`/`FRAMING_TERMINATOR_LEN`/`FRAMING_MAX_FRAME`。
- **Instead of:** 保留 4 字节大端长度前缀（当时三处实现已经写完并且跑绿，改它是纯粹的返工）。
- **Because:**
  - 队友的 `.workflow/specs/game-bridge.md` "关键接口"写的是"一行一条 JSON，UTF-8，`\n` 结尾"，
    而 `overcooked_os_course_project.md` 对分帧一个字也没写。两份文档冲突时前者是唯一的书面
    分帧契约，长度前缀只是我们自己挑的实现方式，没有文档背书。
  - 文档 §5.3 把"JSON 解析失败：记录错误日志，丢弃该消息"归为可恢复错误。换行分帧能在下一个
    `\n` 重新对齐，坏消息只丢它自己；前缀分帧读到一个坏长度就无法定位下一条，只能断链——
    一个字节的错要拿一次连接闪断去换。
  - 删除而不替换 `framing_peek_length()`：它存在只是为了让调用方先看一眼前缀长度。行分帧下
    "这一行不可能存在"（无 `\n` 且超过上限）与"还没到齐"由解码器自己的 `FRAMING_ERR_LENGTH`
    与 `FRAMING_NEED_MORE` 分开回答，再留一个 peek 只会多一处能和线格式说法不一致的地方。
  - "空行"= 空消息的结论不变（decisions.md: 2026-10-15 - 零长帧当作空消息交付）：空行照样交付
    `len == 0`，由解析器拒绝该消息并记 `parse_error`，连接保持；只有"这一行永远不会结束"
    （超过上限仍无 `\n`）才断链。
- **Mine:** yes
- **Revisit when:** 需要在线格式里传二进制（非 UTF-8）载荷，或某个对端要求消息体里能出现裸 `\n`
  ——那时要么恢复长度前缀，要么给载荷加转义。
- **Effort:** bridge

## 2026-10-15 - 端口按课程文档 §5.1：Bridge 作服务端绑 12345，14455 留作反向模式

- **Decided:** Bridge 默认 `--link listen` 绑 `0.0.0.0:12345` 作服务端、插件作客户端连
  `127.0.0.1:12345`；`--link connect` 默认拨 `127.0.0.1:14455`、插件 `Network.Mode=server`
  默认监听 14455。据此订正 `.workflow/specs/game-bridge.md` 的关键接口行（原文写"Bridge 监听
  `127.0.0.1:14455`（跨机时改绑定地址），插件作为客户端连接"）。
- **Instead of:** 照组员 spec 那句话把 Bridge 的默认监听端口改成 14455，让端口号与那份 spec 一致。
- **Because:** 那句话与课程文档两处都对不上。文档 §5.1 的示例代码写明 `// 调度器侧（服务端）`
  绑 `INADDR_ANY` + `sin_port = htons(12345)`、`// 游戏插件侧（客户端）` 写
  `client.Connect("localhost", 12345)`；而 §6.1/§6.2.4 说在 14455 上起服务端的是**插件**。
  组员那句取的是 §6.1 的端口号、用的是 §5.1 的角色分配，两边都不符，而 `standards.md` 明写
  "代码与课程文档冲突时以课程文档为准"。§5.1 那套是文档里唯一自洽的读法，也是本实现原来的
  默认值；§6.1/§6.2.4 那套按你的裁定实现为可切换的反向模式，两套都跑得通，所以不需要牺牲任何
  一边。**这是本仓库里唯一一处改了队友文档陈述的地方**，若组员认为该以 14455 为默认，改回来只是
  `bridge/src/bridge.c` 的 `DEFAULT_LISTEN_PORT` 加两处文档。
- **Mine:** yes
- **Revisit when:** 组员指出验收环境里 `127.0.0.1:14455` 已由别的服务占用，或课程验收明确要求
  Bridge 监听 14455。
- **Effort:** bridge
- **Superseded by:** 2026-10-15 - 端口按组员决策统一成 14455（用户裁定以组员决策为默认值；本条
  的推理过程保留在这里，因为它是"两份文档冲突时怎么判"的唯一记录）

## 2026-10-15 - 与 game-bridge 方案对齐目录名、文档名与上报节奏

- **Decided:** 按组员已有代码与文档对齐四处：Linux 侧源码目录 `core/` → `bridge/`（组员
  `standards.md` 的 Layout 命名，其 `.gitignore` 也已预留 `bridge/*.o`）；协议文档
  `protocol/README.md` → `protocol/protocol.md`（组员 spec 指定落点）；心跳超时默认
  3000 → 5000 ms 与插件重连间隔 3000 → 2000 ms（组员 spec 的失败行为与"2 秒后重试"）；状态
  上报由 500 ms 间隔改为每帧（组员 spec、`glossary.md` 的世界状态定义、方案 §5.1"每帧发送
  状态消息"）。分帧一项另见上一条。
- **Instead of:** 让两套设计在同一个仓库里并存 —— 两份 `standards.md`、两个源码目录名、两种
  分帧、两个端口归属。两边单看都不算错，但没人能照着一份文档把东西跑起来，而"照文档跑不起来"
  正是这次对齐要消灭的东西。
- **Because:** 两个 effort 从同一起点各自开工且互相看不见：组员的 `game-bridge` spec 说"仓库里
  还只有文档和汇报材料"，我这边也确认基线 `b61b6f7` 下一个 `plugin/` 文件都没有。分帧与源码
  目录这两件事课程文档根本没写（`grep` 分帧/长度前缀/换行/分隔符/粘包/大端**零命中**；§6.6
  的"建议目录"只列 `config/ runs/ logs/ /var/run/`，没有源码目录），端口则自相矛盾 —— 空白处
  两个人各填各的，必然不同。已有文档能判的地方按文档判，判不了的地方才新增选择。每帧上报这条
  不是新增选择：它是组员 spec 与我们 `glossary.md` 共同的定义，原来的 500 ms 才是我自己加的
  限流，阈值 `IntervalMs=0`（每帧）留作默认、正数仍可限流。
- **Mine:** no
- **Revisit when:** 组员那边的 `scripts/` 与这边的 `tools/` 需要合并成一套，或每帧上报在实机上
  产生明显开销（那时限流阈值一行配置即可回调）。
- **Effort:** bridge
## 2026-10-15 - 相似项目只作设计参考，不新增代码来源

- **Decided:** 本轮调研（`.workflow/maps/prior-art.md`）发现的相似项目一律只作设计参考，不复制代码：
  `Phantivia/cortico-world-overcooked2`（MIT）、`ArchipelagoMW/Archipelago` 的 OC2 关卡表（MIT）、
  `hdd-robot/gAgent`、`zehuilu/CBBA-Python`、`ArthurSonzogni/FTXUI`（MIT）、
  `bold-lab-ai/JaxMARL`（Apache-2.0）、`rosewang2008/gym-cooking`（MIT）、
  `HumanCompatibleAI/overcooked_ai`（MIT）；监督器与 IPC 库（runit/s6/supervisord/systemd、libuv/libev、
  libcgroup）只读不引。可复用的代码来源仍只有
  `decisions.md: 2026-10-14 - 不复制 overcooked-supercharged 的代码，只借用游戏类型与字段名`
  批准的那两处 MIT。
- **Instead of:** 从 cortico 拿状态与事件代码（它是最像我们的 BepInEx 插件）、从 Archipelago 拿关卡 ID
  表、引 FTXUI 做监控面板、把 overcooked_ai 或 JaxMARL 当策略预筛平台。
- **Because:** 状态、输入、导航、关卡扫描我们都已有能跑的等价实现（`plugin/WorldState.cs`、
  `Takeover.cs`、`Navigation.cs`、`scripts/scan-levels.py`），而 cortico 只有 3242 行 C#、建仓三天、
  只跑单 bot；Archipelago 只有 ID 对照，关卡扫描已经覆盖；FTXUI 是 C++，会把依赖带进 C11 核心；
  overcooked_ai 一年没提交、锁 Python 3.10、没有切菜工序与限时订单队列，当预筛平台要重写状态与计分
  语义；监督器与 IPC 原语是答辩主体，不能引库代写。关卡几何没有"许可证可接受 + 覆盖本体 144 包"的
  现成数据集（supercharged 只有 4 份手工标注，LevelEditor 的 35 个场景是 OC1/测试场景），我们自己的
  运行时导出是更好的来源。
- **Mine:** yes.
- **Revisit when:** 状态或事件层需要重写、面板成为阶段 E 的瓶颈、或预筛实验量超出自写模拟器的能力，
  并且愿意为此补一条新的许可决策。

## 2026-10-15 - Bridge 端口可配置，默认 14455

- **Decided:** Bridge 的监听端口做成可配置（环境变量或配置文件），默认仍是 `127.0.0.1:14455`，
  并把默认值与覆盖方式写进 `protocol/protocol.md`。
- **Instead of:** 端口写死在 Bridge 和插件两侧。
- **Because:** `hpmv/overcooked-supercharged` 的插件在同一个 loopback 端口上跑它的 Thrift 服务
  （`patch/Injection/InjectorServer.cs:46`，可用 `OC2SC_PORT` 覆盖）。同机装了两个 mod 时写死的端口
  会撞，而演示机器是谁的笔记本事先不知道。
- **Mine:** yes.
- **Revisit when:** 插件侧也改成可配置并约定固定端口，或确认演示机器不会装其它 mod。
- **Effort:** game-bridge

## 2026-10-15 - 固定菜单复用 TAS 的 GetNextRecipe Prefix

- **Decided:** 固定订单的菜谱顺序复用 `gua248/Overcooked2-TAS`（MIT，版权行是未填的模板，保留原
  LICENSE 与来源说明）的 Harmony Prefix：接管 `RoundData.GetNextRecipe` /
  `DynamicRoundData.GetNextRecipe`，按 `script.menu[phase][menuCount]` 返回原生 `RecipeList.Entry`。
  方案 §4.2 要的"到达时间与时限"不在这个接口里，先用自制关卡的全局 `timeBetweenOrders` +
  `orderLifeTime` 起一局量实际值，不够再去找订单创建入口。
- **Instead of:** 自己写一个完整的订单生成器（Harmony 接管订单到达与时限，按 `config/orders.json`
  逐单生成）。
- **Because:** TAS 已经把这套补丁写完并实测过（`plugin/Patch.cs:189-241`），MIT 允许保留声明后复用；
  `GetNextRecipe` 决定"生成哪道菜"，调用时机由订单控制器掌握，在同一个 Prefix 里按时钟挑 Entry
  保证不了任意到达时刻，还会破坏调用方对调用次数的假设。先用现成的把"每阶段固定菜单"拿下来，
  把自研成本留给真正缺的那一层。
- **Mine:** yes.
- **Revisit when:** 实验需要逐单的到达时刻或时限（EDF/HRRN 对比需要），或换关卡/换玩家数后 `menu`
  索引不再适用。
## 2026-10-14 - 插件把游戏伪装成"一直在前台"，后台也能跑

- **Decided:** 插件加载时设 `Application.runInBackground = true`，并用 Harmony 打两个补丁：`InGamePauseMenu.OnApplicationFocus(false)` 前缀返回 false（失焦不暂停游戏）、`UnityEngine.Application.isFocused` 的 getter 前缀恒返回 true。
- **Instead of:** 只设 `runInBackground`，或要求操作者把游戏窗口留在前台。
- **Because:** 反编译 `Assembly-CSharp.dll` 看到游戏把所有本地输入都卡在 `Application.isFocused` 上（`PlayerControls.cs:455`、`LogicalButtonBase.cs:76`、`LogicalPCEngagementButton.cs:23`、`LogicalPadValue.cs:26` 都是 `if (!Application.isFocused) return`）。实测：游戏失焦时 4 名厨师 12 秒坐标完全不动、两张相隔 2.5 秒的截图 0 个像素不同；窗口一激活立刻恢复移动。答辩和评测时 Supervisor 在终端、游戏必然在后台，不补这个开关整个实验会停摆。
- **Mine:** yes.
- **Revisit when:** 游戏改版不再看 `Application.isFocused`，或补丁导致前端在失焦时行为异常（例如自己吞掉按键）。
- **Effort:** game-bridge

## 2026-10-14 - 可走格自己按 0.6 米格做物理采样，不用游戏的导航网格

- **Decided:** 可走区域由插件自己采样：0.6 米一格，每格要求"脚下有地面 + 以厨师碰撞体半径在腰间查一圈没有东西"，相邻两格之间再过一次同样的检查（边），最后只保留最大连通块。
- **Instead of:** 用 `PlayerControls.GroundLayer` 向下打射线采样（任务 08 原本的写法），或用游戏自己的 `GridNavSpace`/`GridManager.GetGridOccupant` 节点图。
- **Because:** 两条路都实测失败。射线：台面碰撞体不在 `GroundLayer(512)` 上，射线从台面旁穿到台面底下的地板，台面被算成可走，厨师贴着台面推墙（x=20.6 处一动不动）。游戏节点图：它的格子是 1.2 米，而 `probe` 实测台面/工作台摆在 0.6 米的格子上（worktop 在 19.8/20.4/21.0），粗一半的网格表示不出中间障碍，厨师卡在两格正中间（A* 报不可达 560 次）。物理采样与厨师真实碰撞体一致，实测 169 条目标全部距离下降、0 次卡住。
- **Mine:** yes.
- **Revisit when:** 换到台面布局不同（多层、可推倒）的地图导致物理采样结果与游戏碰撞仍对不齐，或游戏给出官方的可行走 API。
- **Effort:** game-bridge

## 2026-10-14 - 进关卡走 ServerMessenger.LoadLevel(LoadKitchen → RunKitchen)

- **Decided:** 插件用 `MultiplayerController.StopSynchronisation()` + `ServerMessenger.LoadLevel(场景名, GameState.LoadKitchen, true, GameState.RunKitchen)` 进关卡（与暂停菜单"重开"同一条路），而不是 `GameUtils.LoadLevel`。
- **Instead of:** `GameUtils.LoadLevel(场景名)`。
- **Because:** `GameUtils.LoadLevel` 只是 `LoadScene`：场景换了、厨师和工位都在，但关卡流程没起来——实测订单为空、得分 `None`、计时全 0，厨师也不受控。走 `ServerMessenger` 那条之后订单=2、计时 `left=150` 都正常。
- **Mine:** yes.
- **Revisit when:** 游戏改版换掉关卡加载入口，或需要联机房间才能起关卡。
- **Effort:** game-bridge

## 2026-10-14 - 调试走日志 + 指令文件，不截图、不手动进关卡

- **Decided:** 插件读游戏目录的 `BepInEx/wellcooked-command.txt`（1 秒内执行并删文件），支持 `load/restart/continue/engage/dump/pause/probe/path`；配 `scripts/dev-cycle.sh`（重启 → 虚拟手柄自己走完前端进关卡 → 打位置与走位日志）。
- **Instead of:** 每轮靠截图看画面、靠人手进关卡。
- **Because:** 调试时人在终端、游戏在后台，截图既慢又要抢窗口焦点；而"进关卡"这一步原本每次都要人手点，一个验证循环要几分钟。有了指令通道，`restart=1` 10 秒内就能确定性地重开关卡，`path=x1,z1,x2,z2` 能直接问一次 A*、`probe=x,z` 能打出某点周围的碰撞体——都是可复现的证据，也留在日志里。
- **Mine:** yes.
- **Revisit when:** 指令通道被执行环境禁用（例如发布版不带调试文件），或调试改走单元测试不需要进游戏。
- **Effort:** game-bridge

## 2026-10-15 - 演示关卡自建，不迁就本体关卡

- **Decided:** 用 `gua248/Overcooked2-LevelEditor` + `OC2DIYLevel` 自制一个演示关卡（单一连通厨房、无机关、菜谱含切+煮/煎、至少两菜共享中间产物），而不是继续从本体 52 关里挑。
- **Instead of:** 用本体关卡（最接近的是 `s_city_h5`；`s_sushi_1_1` 太简单、`s_city_h4` 可行区碎）。
- **Because:** 离线扫了 144 个关卡资源包、游戏里又逐关量了机制组件与连通性：本体里"完全无地形物件"的只剩 3 关，其中两关分别太简单与太碎；带矿车轨道（`s_mine_4_6` 的 `mi_rails`×20）、传送带（`sushi_1_3/1_4` 的 ×21/×10）、传送门（`wizard_*`）的整片排除。用户已明确"矿石那关不达标"并拍板自建。
- **Mine:** yes.
- **Revisit when:** 编辑器环境（Unity 2017.4.8f1 + AssetRipper 1.3.14）搭不起来，或自制关卡在 Proton 下加载不了。
- **Effort:** game-bridge

## 2026-10-15 - 输入注入改走逻辑层（借 gua248/Overcooked2-TAS 的方案）

- **Decided:** 插件的游戏内输入改为替换 `PlayerControls.ControlScheme` 的 `m_pickupButton`/`m_worksurfaceUseButton`/`m_dashButton`/`m_moveX`/`m_moveY`（自实现 `ILogicalButton`/`ILogicalValue`，按 `CanButtonBePressed()` 门控）；前端"按 A 加入"仍走设备层虚拟手柄。
- **Instead of:** 继续用设备层 InControl 反射（`InputDevice.UpdateWithState` + 猜 `Action1`/`Action3`）。
- **Because:** 任务 09 在"切菜键是 X 不是 A"上丢过一次交互——设备层要猜控制项枚举（`Action3` 是不是 `ControlPad.Button.X`），而逻辑层直接对应 `ControlSchemeData` 的字段（`m_worksurfaceUseButton` 就是 `WorkstationInteract`），且天然 per-player、边沿（`JustPressed`）精确。前端加入必须走设备层：游戏自己枚举手柄，逻辑层替换不驱动加入流程（任务 07/08 实测）。
- **Mine:** yes.
- **Revisit when:** 游戏改版换掉 `ControlSchemeData` 的字段，或逻辑层注入影响联机同步。
- **Effort:** game-bridge

## 2026-10-15 - 端口按组员决策统一成 14455

- **Decided:** 链路默认端口 **14455**，两种模式同号：Bridge `--link listen` 默认绑
  `127.0.0.1:14455`，`--link connect` 默认拨 `127.0.0.1:14455`；插件 `Network.Port` 与
  `Network.ServerPort` 默认也都是 14455。默认绑定地址两侧都是 loopback。课程文档 §5.1 样例里的
  12345 与 `INADDR_ANY` 不再作默认值，但仍是合法的覆盖值（`--port` / `--host` /
  `Network.ListenAddress`）。
- **Instead of:** 保留上一轮按 §5.1 定的 `0.0.0.0:12345`，把组员"默认仍是 `127.0.0.1:14455`"
  当成需要被纠正的陈旧表述。这是本轮唯一被推翻的自家决策，推理过程留在上一条里。
- **Because:** 两条推理指向相反方向，都摆出来。
  - 支持 12345：方案 §5.1 的示例代码（`// 调度器侧（服务端）` + `htons(12345)`）与 §2.2.1 架构图
    `TCP Server (0.0.0.0:12345)`，以及 `standards.md` 的"代码与课程文档冲突时以课程文档为准"。
  - 支持 14455：组员的决策条目与 `game-bridge.md` 两处一致，是团队里对这个问题唯一做过的显式
    决策。而课程文档自相矛盾（§6.1/§6.2.4 说 14455 上起服务端的是插件），所以"以课程文档为准"
    这条规则在这里指不出方向 —— 它能裁决"代码 vs 文档"，裁决不了"文档 vs 文档"。
  - 用户裁定取组员决策。好处是组员的 spec 一个字符都不用改：他那行原文就是"Bridge 监听
    `127.0.0.1:14455`（跨机时改绑定地址），插件作为客户端连接并在 2 秒后重试"，现在代码与它
    逐字相符，团队文档自洽；代价是课程文档 §5.1 的 4 处 12345 与默认值不一致，已在
    `specs/bridge.md` 的逐条对照里写明，并给出覆盖方式。
  - 绑 loopback 那一半是独立判断：链路没有认证，绑 `0.0.0.0` 等于让同网段任何人冒充插件（那条
    暴露面曾作为已知风险记录在案）。原先"WSL2 的 localhost 转发够不到只绑回环的监听器"的担心
    **经实测证伪**：WSL2 里绑 `127.0.0.1:12399`、`ss` 确认在听、从 Windows 连接成功，所以 W8 的
    Windows→WSL2 路径不受影响。`0.0.0.0` 留给跨机演示，由 `--host` 显式选择。
  - 组员那条的 `Because`（参考项目的插件在同一个 loopback 端口跑 Thrift，写死的端口会撞）其实
    指向**避开** 14455；但它同时给的缓解办法是"做成可配置"，而本实现已经满足（Bridge `--port`、
    插件 `Network.Port`/`ServerPort`/`ListenAddress`），真的撞上时改一个配置项即可。
- **Mine:** no
- **Revisit when:** 验收明确要求默认值与 §5.1 样例一致，或参考项目 mod 与本实现同机同时运行并
  真的撞上 14455。
- **Effort:** bridge

## 2026-10-16 - 两个插件工程合并到 net35 那份（演示机这份）

- **Decided:** 存活工程是 `plugin/*.cs` + `WellCooked.Smoke.csproj`（net35，跑在 Fedora+Proton
  演示机上：世界状态、导航、工序、关卡接管、调试指令）。net46 `plugin/WellCooked.Plugin/` 只留作
  参考实现，它的链路层（`Net/TcpLink.cs`、`Protocol/Messages.cs`、每帧上报、动作注入接口）并进
  存活工程，之后只有一份插件往 `BepInEx/plugins/` 拷。
- **Instead of:** 反过来以 net46 那份为主（把工序/导航并进去）；或保留两份、各自补齐。
- **Because:** 演示与验收在这台 Fedora 机器上（BepInEx 已装、游戏在 `/run/media/.../Overcooked! 2`、
  `scripts/run-game.sh` 就是它的启动器），而工序、导航、关卡接管这些"踩出来的游戏事实"都在 net35
  那份里（`technical-debt.md: 两个插件工程并存，状态读取与输入注入各有一份` 的 Where 逐条列过）。
  反向合并得先把 net46 在 Proton 下加载起来，而那份的注释写的是 Unity 2018.4、本机是 2017.4.8f1，
  多一层不必要的风险。保留两份则同一条游戏事实（厨师坐标怎么取、交互键是哪个）改一处漂移一处，
  而且两份都往 `BepInEx/plugins/` 拷，装哪个只能靠人记。
- **Mine:** no (asked the user)
- **Revisit when:** 组员的 Windows 实验室需要一份独立部署的插件（例如课程要求两个实验室各自交付），
  或 net46 那份承载了 net35 编不出来的东西（例如只在新 Mono 上可用的 API）。
- **Effort:** game-bridge

## 2026-10-24 - 任务/事件链路用 POSIX 消息队列，每厨师一条任务队列

- **Decided:** Scheduler→Worker 用 POSIX mq：每个厨师一条 `/kitchenos_tasks_<chef>`，
  优先级字段 = 策略算出的紧急度；Worker→Scheduler 共用 `/kitchenos_events`。队列由
  Scheduler 创建与 `mq_unlink`。Supervisor 与子进程之间另有一条匿名管道专做心跳。
- **Instead of:** 用 pipe 传递任务，或让所有 Worker 抢同一条 mq。
- **Because:** 管道没有消息边界与优先级，EDF/列表调度的输出要自己编码成帧再排序；单条共享
  队列会让一个 Worker 取走本来派给另一个厨师的任务，而任务携带 chef_id 与位置相关目标。
  mq 的 `mq_timedreceive` 让 Worker 在没任务时也能按时响应 SIGTERM 与心跳。一条队列的
  创建/清理成本很小（名字由厨师数决定），换来优先级与解耦直接对应 §6.2.2。
- **Mine:** yes
- **Revisit when:** mq 的优先级反转在实测里伤害到调度效果，或需要把任务广播给所有 Worker
  （那时改为共享队列 + 任务认领）。
- **Effort:** os-core

## 2026-10-24 - 日志每进程一个文件、单写者，不共写同一文件

- **Decided:** `runtime/logs/{supervisor,scheduler,worker-N,bridge}.log` 各由一个进程写；
  跨进程事件由 Scheduler 汇总进 `runs/<ts>/events.jsonl`。多进程共写同一文件不加锁。
- **Instead of:** 所有进程追加同一个 `logs/kitchenos.log`，用 fcntl 写锁串行化。
- **Because:** fcntl 锁只保护单次 `write` 调用，一条事件跨多个 `write` 时仍会交错；而一个
  进程卡死会连累全部日志。§6.6 本来就列了"每个进程写独立文件"这一方案，且现有 `log` 模块
  的契约就是单写者。需要一条合并时间线时读 `events.jsonl`，不是读日志文件。
- **Mine:** yes
- **Revisit when:** 演示需要一个严格按墙钟排序的单一流水，且事件级锁的原子追加被证明必要。
- **Effort:** os-core

## 2026-10-24 - 运行目录默认放 Linux 原生 tmpfs，不用 /var/run

- **Decided:** 运行目录优先级 `--runtime-dir` > `$KITCHENOS_RUNTIME_DIR` >
  `$XDG_RUNTIME_DIR/kitchenos` > `/tmp/kitchenos-<uid>`；pid 文件是其中的
  `kitchend.pid`，用 `fcntl` 写锁保证单实例。
- **Instead of:** 按 §6.1 原文把 pid 文件放 `/var/run/kitchend.pid`。
- **Because:** 课程实验室里的普通用户写不了 `/var/run`；仓库在 WSL2 里可能位于 9p
  （`/mnt/d`），而 9p 上的 fcntl 锁语义不可靠，共享内存与信号量又必须放 `/dev/shm`
  （tmpfs）。默认 Linux 原生路径同时满足权限与锁语义，两个实验室都不用改代码。
  见 `standards.md` 的"运行时目录（pid、日志）必须可配置"。
- **Mine:** yes
- **Revisit when:** 系统以 root 部署并要求把状态放进 `/var/run`，或课程验收指定该路径。
- **Effort:** os-core

## 2026-10-24 - 共享资源用命名计数信号量 + 租约回收，不用 robust mutex

- **Decided:** 锅、砧板、盘子等按 `config/kitchen.json` 的 `kind` 建命名计数信号量
  `/kitchenos_res_<kind>`；Scheduler 在派发前 `sem_trywait`，同时记下租约
  `(kind, chef, task_id, acquired, deadline)`；tick 里回收过期租约并 `sem_post`。
  Worker 不碰信号量：它只执行任务，释放统一由 Scheduler 在收到 DONE/FAILED 时做。
- **Instead of:** 按 §6.4 方案 1 用放在共享内存里的 `PTHREAD_MUTEX_ROBUST` 保护资源。
- **Because:** robust mutex 保护的是临界区，没有"哪个厨师持有、租到什么时候"的身份信息；
  租约能在日志里说清持有者与回收理由，直接对应"检测并恢复死锁"的验收点，也不要求资源
  对象本身放在共享内存里。POSIX 命名信号量没有 robust 语义，所以回收必须由第三方
  （Scheduler）做。代价是回收延迟等于租约长度，可配。
- **Mine:** yes
- **Revisit when:** 租约超时无法与烹饪/切菜的长工序匹配，或需要把资源状态也放进 shm 给
  监控面板读。
- **Effort:** os-core

## 2026-10-24 - 死锁 victim 规则：环上 chef_id 最大的那个

- **Decided:** 资源分配图检测到环时，victim = 环上 `chef_id` 最大者；Scheduler 记
  `deadlock_detected`（带 cycle）与 `deadlock_recovered`，按 task_id 删除 victim 的租约，
  `sem_post` 一次，把它的任务改回 PENDING 并重新排队。victim 自己继续参与后续调度。
- **Instead of:** 选资源数最多/持锁最久的厨师，或按到达顺序 FIFO 回退。
- **Because:** 当前每个任务只持有一个资源，正常调度不会成环；这个规则是给以后多资源任务
  的兜底，必须一眼能复现。chef_id 最大是确定性的、无需额外记账，而且与 Worker 启动顺序
  一致，日志里 cycle 与 victim 可以直接对上。所有释放仍走同一条"先删租约、成功才
  sem_post"的路径，breaking deadlock 不会引入第二次释放。
- **Mine:** yes
- **Revisit when:** 有任务同时持有多把资源并需要最小化回退代价，或需要按订单价值而不是
  chef 编号选 victim。
- **Effort:** os-core

## 2026-10-24 - 路径规划放 Worker，Scheduler 只派高层目标

- **Decided:** Scheduler 派发的是 `{op, station/target, duration}` 这类高层任务；A* 与
  逐帧动作由 Worker 用 `config/kitchen.json` 的网格算，写入自己的输入槽。
- **Instead of:** Scheduler 算好整条路径再通过 mq 发给 Worker。
- **Because:** 厨师位置每帧都在变，路径必须在执行前按当前位置重算；把路径塞进 mq 既浪费
  带宽，又让"Worker 崩溃重启"必须回头找 Scheduler 要一份新路径。这也符合既有分工：
  Bridge 不规划路径，插件只执行动作。
- **Mine:** yes
- **Revisit when:** 多个 Worker 之间的避让需要全局协调（窄道相遇），那时 Scheduler 需要
  参与路径预约。
- **Effort:** os-core
- **Superseded by:** 2026-10-25 - 走位改由中央 navd 进程用 PIBT 规划

## 2026-10-24 - 重启退避用 §6.1 的三档（0 / 2 s / 10 s），不是几何翻倍

- **Decided:** 默认退避序列 `0,2000,10000`，窗口 60 s、上限 5 次；序列由
  `--backoff-ms` 覆盖，最后一项重复使用。
- **Instead of:** 严格的几何退避（1 s、2 s、4 s、8 s……）。
- **Because:** 课程文档 §6.1 把"首次立即、10 秒内再次失败等 2 秒、1 分钟内失败 3 次等
  10 秒、5 次后停止"写成了明确策略，答辩时要能逐条对上。几何翻倍与它冲突，而且
  0→2→10 已经覆盖了"快速恢复一次、随后迅速拉开"。需要严格翻倍做实验时用
  `--backoff-ms 0,1000,2000,4000,8000` 即可，不用改代码。
- **Mine:** yes
- **Revisit when:** 老师明确要求指数退避公式，或实验表明 2 s/10 s 的固定档在长时间崩溃
  循环下仍过于频繁（那时把默认序列换成几何）。
- **Effort:** os-core

## 2026-10-24 - SIGHUP 重读配置在有活跃订单时延后应用

- **Decided:** SIGHUP 每次都重新读取 recipes/kitchen；若还有活跃订单，只记
  `config_reload_deferred` 并切换策略，等订单全部结束后的下一次 SIGHUP 才替换 DAG/
  网格并重建资源表。
- **Instead of:** 收到 SIGHUP 立即热替换订单图与网格。
- **Because:** 已派发/运行中的任务用 dish 下标引用工序；运行中换表会让下一轮事件落到
  另一道菜的节点上，表现为"任务完成了但 DAG 没往前"且没有报错。延后应用保住一致性，
  策略切换仍然即时（演示要的是 FCFS/EDF 立刻可比较）。
- **Mine:** yes
- **Revisit when:** 任务记录改为按全局 node id 而不是 dish 下标引用，或 Scheduler 增加
  了可撤销/重放的调度日志能够迁移在途任务。
- **Effort:** os-core

## 2026-10-24 - 配置订单的到达/截止期由 Scheduler 合成

- **Decided:** `config/orders.example.json` 只有 menu（固定菜单），Scheduler 按它生成
  5 条配置订单：到达间隔 700 ms，截止期 = 到达 + 20 s + 2×工时 + (i%2)×8 s；同时把
  shm 世界状态里新出现的游戏订单按真实 `time_remaining` 收进同一个订单模型。
- **Instead of:** 扩写 orders.json 增加 arrival/deadline 字段（要同步插件与协议），或
  只依赖游戏上报的订单。
- **Because:** 课程阶段的 orders.json 契约是"一份配置=一个阶段一列菜名"，插件只读 menu；
  改字段会把一个纯演示配置变成跨端协议。合成值足以让 FCFS/EDF 在本机产生不同顺序、
  让无游戏闭环跑起来；仿真实验以后需要精确到达时刻时应新增独立实验输入，而不是偷偷改
  现有文件语义。
- **Mine:** yes
- **Revisit when:** 插件侧开始按配置文件生成带到达时刻的订单，或模拟器需要可复现的
  精确到达序列。
- **Effort:** os-core

## 2026-10-25 - 多厨师避让用时空 A* + 共享预约表

- **Decided:** 每个 Worker 在 4 邻域网格上做时空 A*（等待也是一步），按优先级依次把自己
  的路径写进共享预约表 `/kitchenos_resv`（64 时隙环，`/kitchenos_resv_lock` 保护），后来者
  避开已预约的格与对换；互等成环时按 `nav_deadlock` 规则让环上优先级最低、并列时 chef_id
  最大的那个退让。避让与寻路全部在 Worker（Linux 侧）。
- **Instead of:** 插件 `Navigation.cs` 做局部避让；或 CBS 全局最优多智能体寻路；或 Scheduler
  集中算所有路径。
- **Because:** 用户决定寻路放 Worker：调度与寻路是课程要展示的 Linux 侧机制，放进插件就会
  有两份路径（game-bridge/13 的卡住即由此而来）。共享内存 + 信号量的预约表给了全局协调，
  又不违反"路径规划放 Worker、Scheduler 只派高层目标"；CBS 最坏指数级且每次重规划要全体
  重算，4 个厨师下优先级规划加退让已够用、日志也好解释。
- **Mine:** no
- **Revisit when:** 厨师数超过 4 或 `_3` 上 `nav_deadlock` 频繁到影响得分，那时考虑 CBS
  或窗口化 CBS；或实测插件执行 `step` 的延迟让 Linux 侧跟随误差超过 0.15 m。
- **Effort:** kitchen-control
- **Superseded by:** 2026-10-25 - 走位改由中央 navd 进程用 PIBT 规划

## 2026-10-25 - Linux 侧计时统一用游戏时钟

- **Decided:** 插件在 `state_update` 里带 `game_time_ms`（游戏内毫秒，暂停不走），Worker 的
  预约时隙、卡住判定、宏动作超时与 Scheduler 的截止期、LLF/HRRN 全部用它；墙钟只用于
  链路心跳与进程重启退避。
- **Instead of:** 继续用 `CLOCK_MONOTONIC` 墙钟。
- **Because:** 游戏暂停或掉帧时墙钟照走，预约会过期、厨师会被误判卡住、订单截止期被提前
  判超时；模拟器要 `--speed` 快进，只有用同一个逻辑时钟，实机和模拟器跑的才是同一份代码。
- **Mine:** yes
- **Revisit when:** 插件拿不到稳定的游戏时钟（例如关卡切换时回退），那时在 Bridge 里做单调
  化处理而不是退回墙钟。
- **Effort:** kitchen-control

## 2026-10-25 - 走位改由中央 navd 进程用 PIBT 规划

- **Supersedes:** 2026-10-24 - 路径规划放 Worker，Scheduler 只派高层目标；2026-10-25 - 多厨师
  避让用时空 A* + 共享预约表
- **Decided:** Supervisor 下新增 `navd` 子进程，统一为所有厨师规划：每一步跑 PIBT（优先级继承 +
  回溯），按执行依赖图把路点放行给各厨师，插件用新动作 `path` 沿路点连续走。Worker 只写目标槽、
  读计划槽（共享内存 `/kitchenos_nav`，各槽单写者）。优先级时空 A* 保留为 `navd --nav pa` 对照
  基线。规划仍全部在 Linux 侧，Scheduler 仍只派高层目标。
- **Instead of:** 每个 Worker 各自做时空 A* + 共享预约表；Worker 内 WHCA*；中央 RHCR。
- **Because:** 用户要求走位"不要太初级"、要能当后续调度策略展示的地基，并选了这一档。PIBT 在
  每对相邻格都在长度 ≥ 3 的环上的图（`_3` 环形过道）上保证每个厨师都能到达，不靠死锁检测兜底；
  依赖图放行让厨师快慢不一时也不会同格，又不用一格一停。各自规划做不到全体同步推进，所以要
  一个看全局的进程；不放 Scheduler，免得走位节奏拖住派工、Scheduler 崩溃时厨师全停。
- **Mine:** no
- **Revisit when:** 厨师数超过 4 或 `nav_metrics.json` 里 PIBT 的等待步数明显高于 `pa`，那时
  考虑窗口化的 PBS；或 `navd` 单步规划 p99 超过一帧。
- **Effort:** kitchen-control

## 2026-10-25 - 批量实验用多开游戏实例，不写模拟器

- **Decided:** 不写 `tools/sim` 离散事件模拟器。批量实验在本机同时开 N 个（目标 3 个）
  640×360 的 Overcooked2 实例：不经 Steam applaunch，直接 `proton run`；每个实例一个独立目录
  （复制 exe/doorstop/`BepInEx/`，`Overcooked2_Data` 符号链接），插件端口 `14455 + k`，Linux 侧
  每个实例一棵 `--instance k` 的进程树，IPC 名字加 `_<k>` 后缀。离线上界改成不开游戏的纯计算脚本。
- **Instead of:** 自写模拟器无头快进；或多个实例共用游戏目录只改端口。
- **Because:** 用户认为模拟器成本太高，要求多开小窗口直接接管。2026-10-25 实测两个实例能同时
  跑、各自加载插件（各约 1 GB 内存、2 核、1.3 GB 显存），从独立目录启动也能加载插件；共用目录时
  第二个实例把第一个的 `LogOutput.log` 轮转掉，状态/指令文件也会互相覆盖，所以必须分目录。
  真游戏批跑省掉了模型重建和精度校准，实验和演示是同一份代码。
- **Mine:** no
- **Revisit when:** 实验量大到 N 个实例跑不完（例如权重网格超过一晚），或 Proton/Steam 更新后
  不再允许第二个实例启动。
- **Effort:** kitchen-control

## 2026-10-25 - 可走格保留所有含厨师的连通块，不取最大块

- **Decided:** 插件地形采样筛连通块时，保留所有含厨师（没有厨师时含工位锚点）的块；只有完全
  不含锚点的块当口袋丢掉。导出文件因此可以有多个连通区，导航与距离场按连通区分别处理。
- **Instead of:** 继续只保留最大连通块。
- **Because:** 任务 01 实测 `_2` 的外围装饰地面 281 格比每个厨房条带 175 格大，按最大块会把
  所有厨师和工位留在网格外；`_2` 又按设计分成两条互不相通的厨房条带，只留一块会丢掉一半地图。
  两次实机导出都复现了"厨师离最近格 6–8 米"，改后厨师都在 0.3 米内。
- **Mine:** yes
- **Revisit when:** 出现"有厨师但故意不可达"的区域，或某块只含工位、不含厨师而导航必须穿过它。
- **Effort:** kitchen-control

## 2026-10-25 - 地形采样等工位生成后再做，锚点用全部工位类型

- **Decided:** 插件 `DumpTerrainOnce` 在场景里工位组件数为 0 时不采样；工位数或厨师数比上次
  采样时变多就重采，直到两个数稳定。采样锚点从"只找 `ClientIngredientContainer`"改成与导出
  共用的 `StationTypes` 全表（出菜口、水槽、盘堆、锅、料箱等）。重导出了 `_3`、`_4` 两张图，
  `_1`、`_2` 已验证所有工位都在 1.2 m 内有正对的格，保持任务 01 的导出不变。
- **Instead of:** 场景一有厨师就采样（任务 01 的原实现）、锚点只取料箱；或把"含工位"的连通块
  也一并保留。
- **Because:** 实测加载顺序是"先有厨师，工位还是 0"，任务 01 因此把 `_3` 的采样范围切到
  x=-1.2..10.2（98 格）、`_4` 切到 x=-5.4..7.8（223 格），水槽/出菜口落在网格外，站位格
  根本不存在。保留"含工位"的块会更糟：`_3` 会把 380 格外围装饰地面和 45 格台面顶当可走格，
  厨师能走到台面上。等工位出现、范围覆盖全部工位后，厨师连通块就是完整厨房（`_3` 167 格，
  `_4` 381 格），23 个工位都能在 1.2 m 内找到正对的格。
- **Mine:** yes
- **Revisit when:** 场景改为异步流式加载，工位数在采样后还会持续增加；或关卡里存在没有厨师的
  合法厨房区（那时要对工位连通块做单独的"可站"判定，而不是简单保留）。
- **Effort:** kitchen-control

## 2026-10-25 - 站位格取工位每个方向最近的可走格

- **Decided:** `stand` 模块在 +x/-x/+z/-z 四个方向上各取一个站位格：沿轴距离 > 半格、垂直
  偏移 ≤ 半格，选最近的可走格；最多 4 个，按到工位的距离排序，朝向存"从站位格看向工位"的
  方向位。距离场按 `grid.mask` 做多源 BFS，不可达保持 `UINT16_MAX`。
- **Instead of:** 按 spec 字面"离工位一个 step、朝工位方向的边未被 `mask` 挡"取相邻格。
- **Because:** 实机导出的工位坐标在台面/墙中心，不在可走格上：四张地图上离工位最近的格都在
  1.2 m（两格，正好是游戏 1.2 m 交互格的一步），工位那一格的 mask 位恒为 0（那一格不可走）。
  按字面规则取相邻格，四张地图所有工位都是 0 个站位格，加载必然失败。测试改为双向检查：
  朝工位方向的相邻格可走时 mask 位必须为 1、不可走时必须为 0；前者只在工位紧贴网格
  （`_3` 的 sink 偏 0.01 m）时出现。
- **Mine:** yes
- **Revisit when:** 导出里带上工位占用的格子或游戏 1.2 m 网格原点，能直接算交互邻居而不必按
  坐标取最近格。
## 2026-10-25 - 手柄竖轴与游戏世界 z 轴反向

- **Decided:** `ActionDriver.Feed` 在写设备层与逻辑层之前统一把方向向量的 y(z) 取反；
  `Navigation`/`face` 仍用世界坐标，`Feed` 是唯一的坐标到输入转换点。
- **Instead of:** 让每个动作（move/path/face）各自记住翻转，或改所有本地驾驶代码。
- **Because:** 2026-10-25 `s_city_h5` 实机验证：喂 `stick=(0,1)` 时厨师向 `-z` 走，path 的
  第一个 +z 路点越走越远（3.35 m 后超时）；翻转后同一条 4 路点 path 连续切换并到达。
- **Mine:** yes
- **Revisit when:** 游戏或输入注入层改变坐标约定（例如换 InControl 版本），用一条 +z path 复测。
- **Effort:** kitchen-control

## 2026-10-25 - game_time_ms 字段与计时消费者的任务边界

- **Decided:** 任务 02 只交付 `game_time_ms` 的插件上报、协议/ shm 字段与 fake/测试；
  现有 Scheduler/Worker 里仍用墙钟的计时路径，由后续任务 03/04/06/07 在改各自算法时接入。
- **Instead of:** 在 02 里跨任务改 Scheduler/Worker 的超时、预约和卡住判定。
- **Because:** 那些计时路径本来就要在导航、宏动作、派工任务里重写；在 02 提前改会与它们的边界重叠，
  也无法在本任务内证明新计时语义。02 的 Check 只要求字段暂停不走。
- **Mine:** yes
- **Revisit when:** 任务 04 或 06 落地后，`grep CLOCK_MONOTONIC` 仍能看到本应使用游戏时钟的计时。
- **Effort:** kitchen-control

## 2026-10-25 - 已到站的目标厨师在 PIBT 里当固定障碍

- **Decided:** 有目标且实际已在站位格的厨师，PIBT 当固定障碍处理（不移动、也不被推），
  并按这些障碍格重算每个厨师的 BFS 距离场；没任务（目标=当前格）的厨师仍可被推开。
- **Instead of:** 按字面把"到达目标优先级清零"理解成到站后仍可被推，并继续用 stand 的
  静态距离场排序候选。
- **Because:** `_3` 随机用例实测两种做法都会卡死：两人在同一条 1 格宽环上，若到站者立刻
  被推开又会马上抢回目标格，两个方向的追击无限循环；静态距离场下，最近邻被固定障碍挡住时
  又会在相邻两格间来回走。把到站者当障碍 + 障碍感知距离场后，test_pibt 四张图 100 组
  随机用例全部 200 步内到达。真机上 Worker 读到 `arrived` 后也会置 `pinned`，语义一致。
- **Mine:** yes
- **Revisit when:** 需要让另一个厨师从到站者的格子上硬挤过去（例如站位格被永久占死），
  那时改为依赖卡住重规划或换站位格，而不是放开推人。
- **Effort:** kitchen-control

## 2026-10-25 - BLOCKED 后先扣住原厨师，再把工序改派

- **Decided:** Scheduler 收到 `BLOCKED` 时把工序放回就绪队列，给任务记 `avoid_chef`，并
  暂时不把新任务派给报 blocked 的厨师；等该工序派给别的厨师后解除扣留。
- **Instead of:** 只把工序回队列、让下一轮轮到谁就谁接；或把整单直接判 FAILED。
- **Because:** 导航实测里单资源场景下，原厨师一空下来就抢走了下一单，被重排的工序反而在
  资源队列上等，永远到不了另一个厨师手里（nav-stuck 第一次跑就是这样）。扣住一个调度周期
  让重排真正生效，又不把整单失败掉。
- **Mine:** yes
- **Revisit when:** 所有厨师都被同一工序 block 过一遍，需要超时后判失败或扩大站位格集合。
- **Effort:** kitchen-control

## 2026-10-25 - 宏动作的 face 完成看世界朝向，不接插件 ack

- **Decided:** `goto` 在计划槽 `arrived` 后发 `face`，完成判据取世界状态的 `chef.facing` 与工位方向误差 < 15°；`ack` 仍只到 Bridge 日志。
- **Instead of:** 把插件的 ack 经 Bridge 或 shm 回传给 Worker 再判完成。
- **Because:** 现有链路里 ack 不下发 Worker，而插件只在同一个 15° 误差内才回成功 ack，读世界状态是同一个事实且不用改协议/shm 布局；macros 的其它步骤本来也以状态为准。
- **Mine:** yes
- **Revisit when:** Worker 需要按插件拒绝原因（找不到目标、位置不对）做不同重试策略。
- **Effort:** kitchen-control

## 2026-10-25 - plate 拆成 goto + wait_ready + pickup

- **Decided:** `plate` 工序在 pickup 前加 `wait_ready`，读该工位的锅 `state` 到 `cooked`；工位没有锅状态（例如只是盘子堆）时 `wait_ready` 直接通过，pickup 的 `holding` 仍是成功判据。
- **Instead of:** 只发 `pickup`，靠 1.5 s 超时后由 Scheduler 重排队等熟。
- **Because:** `cook` 明确放下即走、不等熟，锅没熟时盘工序的 pickup 必失败；把"等熟"放进同一道工序，失败原因和宏动作链能对上，也不增加 Scheduler 的工序类型。
- **Mine:** yes
- **Revisit when:** Scheduler 能按锅状态单独生成"等熟"工序，或能按订单精确匹配到某一口锅。
- **Effort:** kitchen-control
- **Superseded by:** 2026-10-26 - plate 保留订单专属锅直至取菜

## 2026-10-25 - 工位 id 与 shm 资源键按精确名/归一化/同类序号匹配

- **Decided:** Worker 找工位对应的 shm 资源时，先认精确名，再做 `-`/`_` 归一化比较，最后按工位 kind + 序号找 `pot_N`/`cutting_board_N`/`sink_N` 等插件键。
- **Instead of:** 让 Scheduler 改发插件资源键，或改 kitchen 导出改用 GameObject 名。
- **Because:** kitchen.json 用 `cooker-0`/`container-0`，插件上报 `pot_0`/`cutting_board_0`；任务 06 只做执行器，接口统一留给任务 09 的实机接入，先有一层可解释的匹配比在 Scheduler 里另造一套 id 污染更小。
- **Mine:** yes
- **Revisit when:** 任务 09 把导出的工位 id 与插件键统一，删掉后两级匹配。
- **Effort:** kitchen-control

## 2026-10-26 - plate 保留订单专属锅直至取菜

- **Decided:** Scheduler 在 cook 工序完成时从 Worker 租约表摘除该任务租约，但不归还 cooker 令牌；在同一订单/task 记录锅工位与预留状态，`plate` 仅使用该订单对应 cook 的预留锅。plate 成功、订单失败或取消时归还令牌并清除工位预留。Worker 收到 `source_station_id`，先取干净盘，再去该锅等待并拿菜。
- **Instead of:** 让 Worker 从共享状态猜锅，或只改 `plate` 的 `resource` 为 cooker；前者无法区分并行订单，后者丢失盘子堆站位与有限容量约束。
- **Because:** `kos_mq_msg` 原本无订单到锅映射，且 Scheduler 在 cook 完成时立即释放 cooker；两个独立缺口都需明确处理。Scheduler 现有订单/task 内存记录足以承载运行期预留，不引入持久化层。
- **Mine:** yes
- **Revisit when:** Scheduler 具备可恢复的持久化订单状态，或 recipe DAG 用显式产物位置替代 cook→plate 同名节点绑定。
- **Effort:** kitchen-control
- **Supersedes:** 2026-10-25 - plate 拆成 goto + wait_ready + pickup
