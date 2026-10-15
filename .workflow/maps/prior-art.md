# Map: 参考项目与相似项目的复用边界

**Destination:** 把"哪些现成项目进我们的仓库、哪些只学设计、哪些不用"定下来，作为 Supervisor /
Scheduler / Worker / 配置层的输入，避免重造已有的轮子。
**Started:** 2026-10-15   **Status:** working

## Route so far

- supercharged 无许可证（仓库根没有 `LICENSE`，GitHub API 也返回 null）→ 只能当"游戏类型/字段名"的事实来源，不复制代码（`decisions.md: 2026-10-14` 已定）。
- 它不是 4 个关卡的旧 TAS 工具了：master 现在是 planner/headless/tas-plugin/modules/native 的新框架，但关卡仍是 4 份手工标注，且用了 `user32/kernel32` 与 32 位偏移，整体不能当跨平台底座。
- `toasterparty/oc2-modding` MIT 原文是 `Copyright (c) 2022 toasterparty`，可以拿：固定随机源（`FixedRNG.cs`）、场景目录查询、配置分层。它没有工位几何，也没有 bot 控制协议。
- `gua248/Overcooked2-TAS` 里最值钱的不是回放，而是 `Patch.cs:189-241` 的分阶段固定菜单补丁（Harmony Prefix 接管 `RoundData/DynamicRoundData.GetNextRecipe`）；它不控制订单到达时刻。
- gua248 全家的 `LICENSE` 都是没填的 MIT 模板（`Copyright (c) [year] [fullname]`）；`Overcooked2-LevelEditor` 与 `Overcooked2-DIYLevelFastInit` 根本没有 `LICENSE`。
- 新发现与我们架构最像的项目：`Phantivia/cortico-world-overcooked2`（MIT，行式 JSON over TCP，插件侧），比 supercharged 更贴近我们，但同样只值"状态与事件设计"。
- 同类 in-process bot 里没有比 GPL-3.0 的 `Extremelyd1/OvercookedAI` 更宽松、又能直接用的实现；cortico 是 MIT，但功能窄。
- 仿真侧：`overcooked_ai` 一年没提交、Python 锁 3.10、没有切菜/洗盘工序和限时订单队列，不能当原作得分预筛器；自己写最小离散事件模型更便宜。
- OS 侧：监督器/IPC/事件循环库一律不引入（课设要求自写原语），只学 runit/s6/supervisord/systemd 的重启与回收设计；调度算法看 SimSo(EDF/RM)、gAgent(Contract Net)、CBBA。
- 关卡数据：没有任何许可证可接受、覆盖本体 144 包的现成工位坐标数据集；我们自己的 `WorldState.cs` 已经在导出工位、地形和菜谱，转换即可，不用再找数据源。
- 监控面板没有开箱即用的，方案 §8.1 的面板要自己接数据；FTXUI 只是渲染底子。
- 落点：方案 §6.1 与 §7 已按这些证据改写，新增三条决策（相似项目只作设计参考、Bridge 端口可配置、固定菜单复用 TAS Prefix）；`Q1/Q2/Q4` 已答，`Q3`（订单到达时刻）留给自制关卡落地后再定。

## 取证口径

- 2026-10-15 用 `git clone --depth 1` 把下列仓库读到 `/tmp/prior-art/`，日期取 `git log -1 --format=%cI`
  或 `gh api repos/<o>/<r>/commits?per_page=1` 的 committer date，近一年提交数用
  `commits?since=2025-10-15T00:00:00Z` 分页计数，星数/归档/许可取仓库 API。
- 关键结论已由我在本机复核：TAS 的 LICENSE 占位符、`Patch.cs` 的固定订单补丁、LevelEditor 与
  supercharged 没有 LICENSE、supercharged 的 `controller/Data/Levels/` 只有 4 个文件、
  supercharged `patch/Injection/InjectorServer.cs:46` 的 14455、cortico 的 MIT 原文与 47640 端口、
  oc2-modding 的 MIT 原文、游戏 exe 的 PE 头（`Overcooked2.exe` machine `0x14c` = 32 位 x86）。
- 未验证：没有编译或运行过这些项目，没有在 Fedora+Proton 或 WSL2 上实测，没有测过原作得分差异。
  标 `[推断]` 的是工作量估计，不是实测结果。
- 许可结论只是工程判断，不是法律意见。宽松许可 = 法律上可复制（保留声明）；**进我们的仓库仍需
  `decisions.md` 新增一条**，现有 `decisions.md: 2026-10-14 - 不复制 overcooked-supercharged 的代码`
  只批准了 `gua248/Overcooked2-TAS` 与 `toasterparty/oc2-modding`。

## 身份与许可总表

| 项目 | 许可（原文） | 最后提交 | 近一年提交 | ★ | 判定 |
|---|---|---|---|---|---|
| hpmv/overcooked-supercharged | **无 LICENSE**（API null） | 2026-09-16 `3db24d3` | 26 | 11 | 只学设计 |
| toasterparty/oc2-modding | MIT，`Copyright (c) 2022 toasterparty` | 2026-05-20 `bd60579` | 10 | 10 | 作为基础改造 |
| gua248/Overcooked2-TAS | MIT，`Copyright (c) [year] [fullname]`（占位） | 2025-06-13 `946aea0` | 0 | 4 | 作为基础改造 |
| gua248/Overcooked2-LevelEditor | **无 LICENSE** | 2026-09-06 `bb317ec` | 141 | 29 | 用它的发布工具，不复制源码 |
| gua248/Overcooked2-ManyRecipes | MIT（占位） | 2024-08-19 `52cdccf` | 0 | 3 | 只学菜谱结构，不安装 |
| gua248/coc34robot | MIT（占位） | 2024-08-19 `58880d1` | 0 | 3 | 只学设计 |
| kfyangc/Overcooked2-DIYLevelFastInit | **无 LICENSE** | 2026-10-13 `c2501ab` | 15 | 1 | 不用 |
| Phantivia/cortico-world-overcooked2 | MIT，`Copyright (c) 2026 Phantivia` | 2026-10-14 `68843ad` | 19 | 0 | 只学设计（可选新来源） |
| Extremelyd1/OvercookedAI | GPL-3.0（未见 or-later 声明） | 2020-05-20 `4916b55` | 0 | 9 | 只学设计，零拷贝 |
| py-thok/overcooked2-ai | **无 LICENSE** | 2026-10-10 `466a2f0` | 86 | 0 | 只学设计，零拷贝 |
| Lyall/Overcooked2Fix | MIT，`Copyright (c) 2022 Lyall` | 2022-05-19 | 0 | 26 | 不用 |
| Shyngyskhan-333/overcooked2-eight-players | MIT，`Copyright (c) 2026 Shyngyskhan-333` | 2026-10-12 | 2 | 0 | 只学设计 |
| Robben-Ge/overcooked2-replay-robot | MIT，`Copyright (c) 2026 Robben-Ge` | 2026-08-02 | 4 | 0 | 只学设计 |
| BryanLDC/OC2ControllerIcons | MIT，`Copyright (c) 2026 Bryan Lindo` | 2026-10-04 | 4 | 0 | 不用 |
| MuelsyseKen/Overcooked2-Menu-Mod | MIT，`Copyright (c) 2026 long_ken` | 2026-07-17 | 2 | 0 | 不用 |
| polus314/OvercookedBot | **无 LICENSE** | 2023-06-18 | 0 | 0 | 只学设计 |
| HumanCompatibleAI/overcooked_ai | MIT，`Copyright (c) 2019 Center for Human-Compatible AI` | 2025-03-22 | 0 | 1020 | 只学设计 |
| bold-lab-ai/JaxMARL | Apache-2.0，`Copyright 2023 FLAIR` | 2026-09-10 | 152 | 857 | 只学设计（备选） |
| rosewang2008/gym-cooking | MIT，`Copyright (c) 2020 Rose E. Wang` | 2021-04-25 | 0 | 226 | 只学设计 |
| Farfi55/CookedUp | MIT，`Copyright (c) 2023 Alessio Farfaglia` | 2023-10-04 | 0 | 28 | 只学设计 |
| ArchipelagoMW/Archipelago（OC2 子目录） | 根 MIT（2017-2025 四行版权） | 2026-04-07 | 1 | 1718 | 只学设计（ID 表可选） |
| K0lb3/UnityPy | MIT，`Copyright (c) 2019-2026 K0lb3` | — | — | — | 已用（离线扫关卡） |

## 判定与理由

### A. 已批准、可以直接拿或作为基础改造

**`gua248/Overcooked2-TAS`（MIT 占位版权行）** —— 除了我们已经借的逻辑层输入注入，还值得拿两处：

- **分阶段固定菜单**：`plugin/Patch.cs:189-241` 用 Harmony Prefix 接管
  `RoundData.GetNextRecipe` 与 `DynamicRoundData.GetNextRecipe`，按
  `script.menu[phase][menuCount]` 返回指定的 `RecipeList.Entry[]`，并更新 `CumulativeFrequencies`。
  这解决的是"第几阶段出哪道菜"；`menu` 索引绑定关卡、阶段和玩家数，换关卡或换人数要重录。
  注意它**不控制订单到达时刻**：什么时候调 `GetNextRecipe` 由订单控制器决定，只在 Prefix 里按时钟
  挑 Entry 保证不了任意 `orders.json` 的到达时刻，延迟/跳过还可能破坏调用方的假设。
  方案 §4.2 要的"到达时间 + 时限"要么先确认全局的 `timeBetweenOrders` + `orderLifeTime`
  （LevelEditor 的 `LevelConfigSetupPerPlayerCountSO` 可以配这两项）够不够，要么另找订单创建入口。
  同时不要和 `ManyRecipes` 一起启用（它会重写同一处并扩池）。
- **输入状态 schema**：`plugin/TASControl.cs:116-125,458-475` 的 `TASScript`（`level` / `menu[][]` /
  `gamepadStates` / `rngAssigners` / `positionCorrections`），每帧每玩家
  `[b_pickup,b_interact,b_dash,b_emote,x,y,(b_shift)]`；`:263-395` 的逐帧取状态、算
  justPressed/Released 的状态机，`:398-419` 的冻结/单帧。我们的 `protocol/` 只借结构，不搬它的
  Regex 手解析和 Windows 路径。
- 不拿：`controller/controller.py` 的 `vgamepad`/ViGEm 虚拟手柄（Windows 驱动；我们的 `VirtualPad`
  是进程内 InControl 设备）、录制器里写死的 `D:` 路径。
- 版权行是没填的模板：复制片段时保留原 `LICENSE` 文本，并在文件头写明来源与提交号。
- **MIT 覆盖不到整个仓库**：`plugin/DebugItemOverlay.cs:1-3` 自己标注来源是无许可证的
  `hpmv/overcooked-supercharged`。拿 TAS 的任何文件之前先看它的文件头来源声明；`DebugItemOverlay` 与
  `Extension` 里的反射路径只当"游戏自身的字段事实"，状态快照自己写。

**`toasterparty/oc2-modding`（MIT，`Copyright (c) 2022 toasterparty`）** —— 只拿三样：

- **固定随机源**：`OC2Modding/FixedRNG.cs:59-140` 在 `RoundData/DynamicRoundData.GetNextRecipe`
  前后设置/恢复 Unity RNG。注意：固定 seed ≠ 指定菜谱序列（仍受人数、阶段、菜谱池影响），
  要"指定序列"仍走 TAS 的菜单补丁。
- **场景目录查询**：`OC2Modding/OC2Helpers.cs:155-235` 反射 `T17FrontendFlow.m_CoopGameSessionPrefabs`
  取 `GameProgress.GetSceneDirectory().Scenes`（DLC 数组 `{-1,2,3,5,7,8}`），可补我们的关卡扫描。
- **配置分层**：`OC2ModdingConfiguration.cs:101-120,485-509` 的
  `CustomLevelOrder` / `PseudoSave` / 事件配置，以及 `ArchipelagoClient.cs:552-573` 把 SlotData 落成
  `OC2Modding-INIT.json` 的做法：初始实验配置与本次运行结果分开存。
- 工程事实：steam 版 csproj 是 net35 + BepInEx 5.4.21 + UnityEngine 2017.4.8（和我们的环境一致，
  游戏是 32 位 x86），epic 版是 net46 + BepInEx 6，不要混用；`tools/*.bat` 是 Windows 脚本，
  不能当我们的构建脚本。
- 不拿：整包安装。它默认改订单、开作弊、改 `fixedDeltaTime`，会污染"真实得分是唯一指标"的实验。

### B. 新发现、法律上可用但要先补决策

**`Phantivia/cortico-world-overcooked2`（MIT，`Copyright (c) 2026 Phantivia`）** —— 与我们架构最像：
插件是 BepInEx 5 / net35 / 0Harmony，监听 `127.0.0.1:47640`（`plugin/Plugin.cs:39` 可配），
行式 UTF-8 JSON，收包排队到主线程执行；`plugin/Kitchen.cs:456-538` 导出分数、订单、厨师、工位，
`Hooks.cs` 用 Harmony 拦游戏服务端操作形成拾取/交付事件；`Seat.cs` + `VirtualPad.cs` 是"挂虚拟手柄 +
`PlayerManager.StartPadEngagement` 占玩家位"。规模很小（C# 12 文件 3242 行，TS 15 文件 2680 行），
2026-10-11 建仓，只跑通 bot+操作员两席位，不是四厨师调度。
拿它只值"事件回执 + 状态字段"的设计；我们的 `WorldState.cs`/`Takeover.cs`/`Navigation.cs` 已经覆盖
它的能力，整体替换没有收益。**要引代码就得先加一条决策。**

**`ArchipelagoMW/Archipelago` 的 OC2 关卡表（根 LICENSE 为 MIT）** ——
`worlds/overcooked2/Overcooked2Levels.py:35-91` 有 140 条 `level_id_to_shortname`（Story 45 / Seasonal 32 /
Horde 20 / Campfire 15 / Carnival 15 / Surf 13）、DLC 与排除关信息。只有 ID 对照，没有坐标。
可选：用它给 `config/` 补关卡 ID ↔ 显示名映射，省得手抄。

**算法与渲染的备选（都要新决策，本次不建议引）** —— `hdd-robot/gAgent`（MIT，
`ContractNet.hpp:142-257` 的发标/报价/决标状态机）、`zehuilu/CBBA-Python`（MIT，边际收益竞价）、
`ArthurSonzogni/FTXUI`（MIT，C++ 终端 UI，可当监控面板渲染层，但会侵入 C11 核心）、
`bold-lab-ai/JaxMARL`（Apache-2.0，`overcooked_v2` 支持多食材与自定义 ASCII 布局，Python≥3.11）。

### C. 只学设计，零拷贝

- **`hpmv/overcooked-supercharged`（无许可证）**：能学的是握手形状——插件每帧 Flush 状态、收控制、
  处理 Pause/Resume、CommitFrame（`patch/ControllerHandler.cs:14-69`），暂停走
  `TimeManager.PauseLayer`（`patch/Helpers.cs:13-37`），loopback 14455 + 单个 outstanding exchange +
  epoch 防旧回复（`patch/Injection/InjectorServer.cs:30-84`）；高层动作集 `GameAction.cs:19-62`
  （Catch/Goto/Interact/PickupClosest/Throw/Wait/Drop + 等干净盘、切/煮/搅进度）与
  `Action/GotoAction.cs:19-78` + `GameMap.cs:151-230`（可视 + Dijkstra）。**不能拿**：它的回滚状态机、
  原生物理修复模块（`modules/body-restore/` 用 32 位偏移）、`NativeWindowControl`（user32/kernel32），
  也不能把它的 Thrift 协议当我们的行式 JSON。关卡数据只有 4 份手工标注（`controller/Data/Levels/`），
  README 自己说 `capture_map` 只记厨师轨迹、多边形要手描。
- **`Extremelyd1/OvercookedAI`（GPL-3.0）**：动作分层（`Action/` 下 HandleOrder → HandleIngredient →
  Chop/Cook/Plate/Deliver，`CookIngredientAction` 等待时让出执行）、"以手持物变化判拾放成功"、
  "暂停时松按键"。GPL-3.0 下不能抄代码，只能按自己的数据模型重写这些想法。
- **`py-thok/overcooked2-ai`（无许可证，86 次/年）**：状态采集与主线程执行分离、TCP 8765 的采集面；
  它的 `SemanticActions.cs:9-48` 直接调 `ChefEventMessage` 绕过按钮管线，正好是我们**不**该走的路。
- **`overcooked_ai`**：`mdp/overcooked_mdp.py:1375-1430` 的确定性 `get_state_transition(state,
  joint_action) → (new_state, infos)`、`OvercookedState.to_dict/from_dict`、46 个 `.layout` 的字符网格，
  适合当我们离散事件模拟器的状态表示参考。缺口：没有砧板工序、盘子无限、交付不消费限时订单、
  没有按序小费连击；`utils.py:31-33` 用 `eval` 读布局，不要照抄。
- **`gym-cooking`**：`recipe_planner/stripsworld.py` 的 STRIPS 前置条件/效果最接近工序 DAG；
  `delegation_planner/bayesian_delegator.py` 可看任务分工。运行环境锁 gym 0.17.2，不引。
- **`Farfi55/CookedUp`（MIT，Unity 2022.3.8）**：作为 3D 外壳兜底技术上可行（有 Linux 发布包），
  但 Bot 规则在 `Assets/StreamingAssets/ThinkEngineer/.../Brains/*.asp` 的 ASP 里，ThinkEngine 上游没有
  明确许可，素材（`Assets/_Assets/`）授权不明；真要用得禁掉 ASP 决策、重接状态与计分 [推断：人周级]。
  比继续用原作插件差，不建议换。
- **监督器**：runit 的 self-pipe 收 SIGCHLD + 主循环 `wait_nohang` 批量回收（`src/runsv.c`）、
  s6 的"避免快速重启循环 + finish 返回 125 停止重启 + 日志解耦"、supervisord 的
  BACKOFF/RUNNING/FATAL 状态机（`supervisor/process.py:177-182,670-715`）、systemd 的启动限流与
  TERM→CONT→超时强杀顺序。**都不进仓库**，课设要求自写 `fork/setsid/umask/chdir/dup2` + `SIGCHLD` 回收。
  注意 runit 的 `lock_exnb.c:10-14` 其实是 `flock`/`lockf` 条件选择，不是纯 `fcntl`，别当范例抄。
- **IPC/事件循环**：TLPI 的 `pmsg_*` 例子（GPL-3.0-or-later，只读）、
  `sysprog21/concurrent-programs` 的 `ringbuf-shm/ringbuffer.c`（BSD-2，head/tail acquire/release +
  `shm_open/ftruncate/MAP_SHARED`，但只是单生产者单消费者）、Valkey `src/ae_epoll.c`（BSD-3，Reactor
  形状）。我们的 fd 很少，自写 `poll`/`pselect` 处理 TCP、心跳和 self-pipe 就够，不引 libuv/libev。
- **`gua248/coc34robot`、`polus314/OvercookedBot`**：都是"截屏 + 模板/颜色匹配 + 模拟键盘"，
  Windows 专属、写死分辨率和坐标，在 Proton 下不可直接用；只说明"视觉路线"长什么样。
  它的匹配是逐像素平均绝对差 + 颜色差（`robot.py:131-155`），不是 OpenCV 模板匹配；`inputs.py` 另有
  BSD-3 来源（`Copyright (c) 2016, 2018: Zeth`），整个目录不能当纯 MIT。

### D. 不用

- `Lyall/Overcooked2Fix`：宽屏/物理频率，与调度无关，改 `fixedDeltaTime` 还会污染实验条件。
- `BryanLDC/OC2ControllerIcons`、`MuelsyseKen/Overcooked2-Menu-Mod`：前者只改图标；后者直接改
  `TotalBaseScore/TotalTipsScore` 并带 FreezeTime/Cook/Chop 作弊，绝不能出现在计分实验里。
- `gua248/Overcooked2-ManyRecipes`：会替换 `GetNextRecipe` 扩大菜谱池并重新加权随机，改变订单分布与
  分数可比性；只当"菜谱字段含义"的资料（`RecipeData{ingredients,prefab,name,id,score}`、
  `m_composition`/`m_cookingStep`/`m_progress`、`CompositeOrderNode`）。
- `Shyngyskhan-333/overcooked2-eight-players`：5–8 人 IL 改写，我们最多 4 厨师，不需要。
- `Robben-Ge/overcooked2-replay-robot`：pynput/pydirectinput 桌面注入，弱于我们已有的进程内注入。
- `kfyangc/Overcooked2-DIYLevelFastInit`：无许可证，且只是给 OC2DIYLevel 加速初始化，不解包本体、
  不给坐标。
- `libcgroup`、FreeRTOS/Zephyr、`libuv`/`libev`、Helgrind（不覆盖跨进程共享内存竞争）。

## 对我们各模块的映射

| 模块 | 拿什么 | 从哪来 |
|---|---|---|
| 插件（状态） | 工位/地形/菜谱导出已经自己写好了；事件回执形状（拾取/交付成事件而不是每帧轮询） | 我们已有 `WorldState.cs`；设计看 cortico `Hooks.cs` |
| 插件（输入） | 已定的设备层 `VirtualPad` + 逻辑层注入保持不动 | 自己（`plugin/VirtualPad.cs`、`plugin/LogicalInput.cs`） |
| 插件（订单） | 固定订单序列：Harmony Prefix 接管 `GetNextRecipe`，返回指定 `RecipeList.Entry` | `Overcooked2-TAS` `Patch.cs:189-241` |
| Bridge | 行式 JSON + 心跳 + 解析失败丢弃已有设计；帧同步握手可参考"单 outstanding exchange + epoch" | 自己；形状参考 supercharged `InjectorServer.cs:30-84` |
| Supervisor | 自写；只学 self-pipe 收 SIGCHLD、批量 `wait_nohang`、重启退避、停止顺序与 TERM→KILL | runit / s6 / supervisord / systemd 文档与源码 |
| Scheduler | 自写；EDF/RM 的干净基线看 SimSo `schedulers/EDF.py:19-38`；竞价/契约网看 gAgent、CBBA | SimSo（CeCILL，只读）、gAgent（MIT） |
| Chef Worker | 动作分层与"等待烹饪时让出"的设计；寻路继续用我们已验证的 0.6 米物理采样 A* | 设计看 OvercookedAI `Action/`；实现用自己 |
| 关卡与菜谱配置 | 自制 DIY 场景 + 我们自己的运行时导出转 `config/kitchen.json`；菜谱展成 DAG | 自己（`WorldState.cs:283-334,594-738,1078-1148`、`Kitchen.cs:66-96`） |
| 模拟器 | 自己写最小离散事件模型；状态表示参考 `overcooked_ai`，工序前置条件参考 STRIPS | 自己；参考 overcooked_ai / gym-cooking |
| 监控面板 | 自己接业务数据；渲染层可选 FTXUI，纯 C ANSI 面板也够 | 自己；FTXUI（需决策） |
| 验证工具 | strace/pidstat/stress-ng/valgrind 直接在仓库外跑；rr 要先验环境 | 外部工具，不进仓库 |

## 现在就能落地的事（别再重造）

已经切成任务：**14** = 第 1、2 条（固定菜单 + 固定随机源），**15** = 第 3 条（`config/kitchen.json`），
**16** = 第 4 条（菜谱 DAG，Blocked by 15），**17** = 第 5 条（地形 JSON 一致性）。第 6 条（把场景目录查询
补进 `scripts/scan-levels.py`）暂时不排任务——下次真正要批量扫关卡时再做。第 7 条是边界，不是任务。

1. **固定菜单序列**：把 TAS 的 `GetNextRecipe` Prefix 方案接进 `plugin/`，输入改成 `protocol/` 的
   `config` 消息（方案 §4.2 第 1 条）。它给的是"第几阶段出哪道菜"，不是到达时刻；先量全局
   `timeBetweenOrders` + `orderLifeTime` 能不能满足实验需要，不能再去查订单创建入口。
2. **订单随机源**：`FixedRNG.cs` 的 seed 固定 + 记录本次 seed，作为固定序列的补充；两条路径都记日志，
   不要混为一谈。
3. **`config/kitchen.json`**：写一个转换器，吃 `wellcooked-state.json`（`stations[]` 已有
   `type/name/pos/height/gives`）和 `wellcooked-terrain.json`，输出工位稳定 ID + 分类 + 连通区域。
   不要另写坐标扫描器。
4. **菜谱 DAG**：把 `WorldState.cs:1078-1148` 的 `m_composition` 递归扩成"工序节点 + 前置边 +
   资源需求"，补上 `m_cookingStep`/`m_progress` 与工时（现在的深度上限 4、缺工时）。
5. **地形 JSON 一致性**：`WorldState.cs:720-729` 调 `KeepLargestComponent` 之后仍用过滤前的 `free`
   列表生成 cells，要核对输出与过滤后的网格是否一致（任务 08 的遗留）。
6. **场景目录**：`OC2Helpers.cs:155-235` 的反射查询可以补进 `scripts/scan-levels.py`，让离线扫描知道
   关卡属于哪个 DLC/进度。
7. **不做**：不引入 supercharged/cortico/py-thok 的代码，不装 ManyRecipes/Menu-Mod/Overcooked2Fix，
   不引 libuv/libev/systemd，不把 overcooked_ai 当预筛平台。

## 风险与坑

- **端口 14455 冲突**：supercharged 的插件在 loopback 14455 上跑它的 Thrift 服务
  （`patch/Injection/InjectorServer.cs:46`，可用 `OC2SC_PORT` 覆盖），我们的 Bridge 监听同一个端口。
  两个 mod 同时装就会撞。缓解：Bridge 的端口做成可配置（配置文件或环境变量），文档写清默认值。
- **DIY 关卡工具链**：`Overcooked2-LevelEditor` 是 Unity 2017.4.8f1 工程，构建目标
  `StandaloneWindows`（`Assets/Editor/CreateAssetBundles.cs:22-24`），流程要先从游戏
  `StreamingAssets` 复制资源、用 AssetRipper（≤1.3.14）导出 `Assembly-CSharp` 再替换 Patch 程序集，
  产物 `info_xxx` + `s_xxx_1` 放到 `BepInEx/plugins/OC2DIYLevel/levels/xxx`。它自己 35 个场景是
  OC1/测试场景，不是本体 144 关。**Linux/Proton 下能不能跑通整条链没验证**，任务 13 要先做这一条
  可行性验证；它也没有 `load=<场景名>` 接口（全仓库搜不到），自制关卡走 OC2DIYLevel 自己的菜单/
  SceneDirectory，我们插件那条 `load=` 调试通道能不能载入 AssetBundle 里的场景同样没验证。
- **无许可证的两个工具**：LevelEditor 与 DIYLevelFastInit 没有 `LICENSE`。用它们生成的关卡包可以，
  但它们的源码、以及从游戏反编译出来的程序集，不能进我们的仓库或对外分发。
- **版权行未填的 MIT**：gua248 的 MIT 模板里 `[year] [fullname]` 没填。复制片段时保留原文并在
  文件头写来源与提交号；不要在仓库里替它编造作者和年份。
- **位宽事实更正**：游戏是 32 位 x86（`Overcooked2.exe` machine `0x14c`，BepInEx 用 `win_x86`）。
  调查中出现的"本项目 x64"是笔误；cortico / py-thok / ManyRecipes 的 x86 前提与我们一致，
  差异在操作系统和注入方式，不在位宽。
- **supercharged 的"新框架"**：master 现在是 planner/headless/tas-plugin/modules/native 的新结构，
  旧文档（方案 §7.1）描述的 `InjectorServer`/`ControllerHandler`/`RealGameSimulator` 仍在，但
  已经不是它的全部；引用它时锁提交号 `3db24d3`。

## 方案文档里已经改过的地方（本次同 PR 修正）

- §6.1 插件小节原来写"参考 supercharged 的 patch 目录……建议锁定一个稳定的旧 commit，或提取
  InjectorServer 和 ControllerHandler 核心逻辑自己实现"——已改成指向 §7 与本 map（无许可证，不能提取）。
- §7 整节重写：判定改成四档并补上提交号、许可证原文与证据路径；原来的"7.1 首选 supercharged"降为
  "只当事实来源"，"7.7 overcooked_ai 适合作为验证底座"改为"只学状态表示"。
- §7 新增 7.6（cortico / py-thok / eight-players / 不要装的 mod）、7.8（关卡与菜谱数据）、
  7.9（OS 侧监督器/IPC/调度/面板/验证工具）、7.10（落地清单）。
- 仍成立但要按新证据理解的一条：TAS 主要是离线预录脚本、不是实时闭环；它新增的**分阶段固定菜单**
  （`Patch.cs:189-241`）才是我们最该拿的一块，而它不控制订单到达时刻。
- 新增的项目与 OS 侧参考（cortico、py-thok/overcooked2-ai、eight-players、replay-robot、
  LevelEditor / DIYLevelFastInit、JaxMARL、gym-cooking、Archipelago 关卡表、runit/s6/SimSo/gAgent）
  都写进了 §7 的新小节。

## Open questions

## Q1: 要不要新增"批准复用"的代码来源？

**Blocks:** 是否从 cortico 拿状态/事件代码、是否从 Archipelago 拿关卡 ID 表、是否引 FTXUI 做面板。
**Kind:** decide
**Depends on:** none
**Answer:** 不新增（`decisions.md: 2026-10-15 - 相似项目只作设计参考`）。cortico 的能力我们已有等价实现，
Archipelago 只有关卡 ID 表而关卡扫描已经够用，FTXUI 会把 C++ 依赖带进 C11 核心。

## Q2: Bridge 的监听端口是否改成可配置？

**Blocks:** 任务 02 的 Bridge 骨架与 `protocol/protocol.md` 里的默认值。
**Kind:** decide
**Depends on:** none
**Answer:** 做成可配置，默认仍是 14455（`decisions.md: 2026-10-15 - Bridge 端口可配置`）——
supercharged 的插件在同一个端口跑 Thrift，同机装了两个 mod 会撞。

## Q3: 订单到达时刻与时限怎么控制？

**Blocks:** 任务 13（自制演示关卡）与阶段 D 的实验输入；影响"策略对比用同一组订单"能不能成立。
**Kind:** build-a-bad-version
**Depends on:** none
**Answer:** 待答。已知：每阶段菜谱顺序可以直接复用 TAS 的 `GetNextRecipe` Prefix；到达时刻不在那里，
先用自制关卡的全局 `timeBetweenOrders` + `orderLifeTime` 起一局，量出实际到达间隔与时限，看够不够
支撑 EDF/HRRN 的对比；不够再去找订单创建入口。

任务 14 落地后实测到两条：`s_city_h5` 在有人上场时是"上一单交付或超时之后才要下一单"（一局只看到 1 次
`GetNextRecipe` 调用）；进关后场上没有玩家/走剧本那条路时，订单不是经 `RoundData.GetNextRecipe` 来的
（`订单: 接管` 计数为 0，场上却有 4 张订单）。所以"逐单到达时刻"这条更得自己掌握，不能靠游戏出单节奏。

## Q4: 模拟器做多大？

**Blocks:** 阶段 D 的策略预筛；影响是否要引 `overcooked_ai` 或 JaxMARL。
**Kind:** decide
**Depends on:** none
**Answer:** 自己写最小离散事件模型（`decisions.md: 2026-10-15 - 相似项目只作设计参考` 同条记录），
状态表示抄 `overcooked_ai` 的 `to_dict` 形状、工序前置条件抄 gym-cooking 的 STRIPS 思路；
不引运行时依赖。

## Not yet clear

- 自制 DIY 关卡在 Fedora+Proton 下能不能走完整条链（Unity 2017.4.8f1 编辑器、AssetRipper、
  AssetBundle 装载），只能动手做一次才知道。
- 固定订单补丁与我们已有插件的共存（都在 Harmony 上，谁先谁后、`RoundInstanceDataBase` 的形态是否
  随关卡类型变化）。
- WSL2 + Windows 原生那一侧能不能装同样的插件与自制关卡包。

## Ruled out

- 引入现成监督器 / IPC 包装库 / RTOS 内核：课设要求自写 `fork/setsid/umask/chdir/dup2`、管道、
  POSIX 消息队列、共享内存 + 信号量、`fcntl`、信号处理，只能学设计。
- 把 `overcooked_ai` 或 JaxMARL 当演示平台：方案 §8 要求真实游戏画面，模拟器只做预筛。
- 用 CookedUp 替换原作：素材与 ThinkEngine 许可不明，且要重接状态、计分与调度边界 [推断：人周级]。
- 从 GPL-3.0（OvercookedAI）或无许可证（supercharged、py-thok、coc34robot、LevelEditor、
  DIYLevelFastInit、polus314/OvercookedBot）的仓库复制代码进我们的源码。
- 装会改订单/物理/分数的 mod（ManyRecipes、Overcooked2Fix、Overcooked2-Menu-Mod）来"帮忙"。
