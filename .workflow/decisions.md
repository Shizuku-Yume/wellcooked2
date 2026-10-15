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

