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

## 2026-10-15 - OS 核心用 C + GNU make，放 core/；协议契约放 protocol/

- **Decided:** Linux 侧 OS 核心用 C（C99），构建用 GNU make，代码放 `core/`；跨端协议说明与
  JSON Schema 放 `protocol/`，作为两端唯一契约。
- **Instead of:** 用 C++17 + CMake，或 Rust + Cargo。
- **Because:** 文档 §2.2.1 允许 C/C++/Rust。本机 WSL2 里 cmake 与 gdb 都没装而 gcc/make 齐备，
  课程实验室环境更可能只有 gcc/make。C 与文档里那些 POSIX 调用片段逐行对应，答辩时能直接对照
  课件讲。
- **Mine:** yes
- **Revisit when:** 代码规模增长到手工维护 Makefile 变得昂贵，或需要跨平台构建矩阵。
- **Effort:** bridge

## 2026-10-15 - JSON 自己写最小解析器，不引第三方库

- **Decided:** 在 `core` 内实现只覆盖本项目消息所需子集（对象、数组、字符串、数字、布尔、
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

## 2026-10-15 - 插件由本 effort 补写，两端同轮交付

- **Decided:** bridge effort 连 Windows 侧插件一起交付（`plugin/WellCooked.Plugin`，`a67ea3b`），
  并把 `protocol/`（README + `schema/`）当作两端唯一契约，而不是假定插件已经存在。
- **Instead of:** 只交付 Bridge 与它依赖的共享层，把 W8 记为"无法运行"，等后续插件 effort 再补。
- **Because:** 用户要的是能实机跑通；当时仓库里没有任何插件，Bridge 在真游戏上一天也没跑过，
  `state_update` 只能来自 `tools/fake_plugin.py`，协议就只能是自测自证。补写插件的成本可控
  （BepInEx + Harmony + 一个自有 TCP 客户端），换来 W8 从"不可运行"变成"可运行"。
  这条是**补记**：当时没记，随后几份文档因此把插件当成了"现有实现" —— 见
  `decisions.md: 2026-10-15 - 零长帧当作空消息交付` 里的"参考实现 `TcpLink.cs:287`"、
  `protocol/README.md` 的开头与 `specs/bridge.md` 的"以插件现有实现为基准"。那些措辞已更正；
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
  `protocol/README.md` §3.2 的表格；要更稳就把 `--heartbeat-timeout-ms` 调到 6000，或把插件的
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

## 2026-10-15 - action 枚举扩到 5 个：pickup / dash 上线

- **Decided:** 线上 `action` 从 move/stop/interact 扩到**五个**，补上 `pickup` 与 `dash`：
  `protocol.h` 加 `PROTO_ACTION_PICKUP` / `PROTO_ACTION_DASH` 与
  `protocol_build_action_pickup()` / `_dash()`（与 `stop` 共用"只有名字"的构造体），
  `core/src/bridge.c` 的 `build_action_from_slot()` 直接映射这两个 kind，
  `protocol/README.md` §3.6 与 `protocol/schema/action.schema.json` 同步放宽；插件的
  `press`/`release`/`tap` 仍然只留在插件里。
- **Instead of:** 保持三种动作，让 `build_action_from_slot()` 把 pickup/dash 降级为 idle stop、
  只把日志从 DEBUG 提到 WARN（这是复核时 agent 先选的那条路，被用户否掉）。
- **Because:** 缺口只在 C 侧：插件 `InputInjector` 本来就实现八个动作并在 `hello` 里宣告，
  `kos_action_kind` 也一直有这两个取值，缺的只是 C 侧不去构造它们 —— 所以这次扩张**不需要碰
  插件、也不需要 C# 工具链**，可以用 `core/tests/test_protocol.c` 的构造/往返用例、
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
