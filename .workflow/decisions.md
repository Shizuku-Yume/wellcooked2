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
