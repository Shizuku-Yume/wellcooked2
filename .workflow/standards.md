# Standards

## What this project is

WellCooked / KitchenOS：《胡闹厨房2》的订单、工序、厨具被当作作业、任务、共享资源，
在 Linux 上用多进程（Supervisor 守护进程 + Scheduler + Worker + Bridge）和 POSIX IPC
完成调度，游戏画面是可视化和验收载体。使用者是课程教师和答辩评审，唯一优化目标是游戏得分。
设计基线是仓库根的 `overcooked_os_course_project.md`。

设计规格的权威文档是仓库根的 `overcooked_os_course_project.md`；代码与它冲突时以它为准。

## Running it

```sh
# OS 核心 / Bridge（Linux，C + GNU make）
cd bridge && make            # 构建全部二进制
cd bridge && make test       # 单元测试，全部通过为绿
cd bridge && make test-san   # 同上，带 ASan/UBSan；对象写 build/san/，不污染普通 make

# 无游戏联调：协议级模拟插件，替代反复启动游戏
python3 tools/fake_plugin.py --scenario happy
bash tools/run-bridge-scenarios.sh        # 全部场景
bash tools/run-real-game.sh               # 真游戏 W8 验收（需要人进关卡）

# Windows 侧插件
dotnet build plugin/WellCooked.Plugin/WellCooked.Plugin.csproj -c Debug
plugin/tools/deploy-plugin.ps1 -Build     # 编译并部署到游戏目录
```

```sh
# 游戏接入（组员那套脚本，Fedora + Proton 机器）
(cd represent && python3 wellcooked_v2/build_deck.py)   # 重建课堂汇报 pptx（子 shell，不改当前目录）

export GAME_DIR="${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}"  # 换机器改这一行
bash scripts/install-bepinex.sh        # 把 BepInEx 装进游戏目录（按游戏 exe 的 PE 头选架构）
bash scripts/set-winhttp-override.sh   # 让 Proton 加载游戏目录的 winhttp.dll（每台机器做一次）
bash scripts/build-plugin.sh           # dotnet build plugin/ 并拷进 BepInEx/plugins/
bash scripts/run-game.sh               # steam -applaunch 728880
grep -E 'BepInEx 5|WellCooked\.Smoke|smoke patch alive' "$GAME_DIR/BepInEx/LogOutput.log"   # 验证
```

本机环境（bridge effort 那台）：WSL2 发行版 Ubuntu 26.04，已装 gcc / make / python3；
**没有 cmake 和 gdb**。仓库在 Windows 盘，WSL 内是 `/mnt/d`（9p 文件系统），因此进程间共享
内存与信号量走 `/dev/shm`（tmpfs），运行时目录（pid、日志）必须可配置，默认指向 Linux 原生路径。
游戏装在 `D:\EPIC\Games\Overcooked2`。插件编译用仓库自带的 `.tools/dotnet8/dotnet.exe`
（系统 PATH 上没有 `dotnet`，也没装 .NET SDK，`plugin/tools/deploy-plugin.ps1 -Build` 会自动用它）。

组员那台的前置条件：.NET SDK（装在 `~/.dotnet`，`scripts/build-plugin.sh` 默认用它）、游戏目录
（默认本机 Steam 库路径，其他机器用 `GAME_DIR` 覆盖）、Steam 在运行；改注册表和装 BepInEx 前先退出游戏。

跑测试命令之前不要声称任何东西能用。

自动进关卡（重启游戏 → 虚拟手柄在前端按 A 把玩家位占上 → 进关卡 → 打位置/走位日志）：

```sh
bash scripts/dev-cycle.sh 20 s_sushi_1_1   # 第二参数是要进的关卡（场景名）；省略时进存档里世界地图光标停着的那一关
```

给了场景名才是可复现的：不给的时候进哪一关由存档决定（实测进过带矿车轨道的 `s_mine_4_6`）。
脚本会自己写 `wellcooked-takeover.txt` 的 `frontend_press` 与 `diag` 并在退出时还原。

插件的开关都在 `$GAME_DIR/BepInEx/wellcooked-takeover.txt`（改文件 5 秒内生效，不用重新编译）：

```sh
printf 'work=auto\nslots=1,2\ndiag=true\n' > "$GAME_DIR/BepInEx/wellcooked-takeover.txt"
# work=auto   工序模式：按订单自动做菜（默认关；关着是"来回走 + 周期按交互"的最小策略）
# slots=1,2   要占的玩家位（1 基）
# diag=true   过程行（每帧/每 2 秒的位置、路点、手柄状态、寻路记下的边）——默认关，只留结果行
# frontend_press=true  虚拟手柄在前端也按 A（进关卡要用；它会抢菜单导航，所以默认关）
# bridge=127.0.0.1:14455  连 Bridge 上报世界状态（默认就开着；写 bridge=off 关掉）
# state_ms=0  上报间隔毫秒；0 = 每帧（默认，协议与 spec 要求每帧）
```

插件与 Bridge 的链路（任务 03）：连上后立刻发 `hello` 并每帧发 `state_update`，收心跳与 `action`；
断线后等 2 秒再连、连上重新握手；**对端立刻拒绝**时约 2 秒一次，**连接建立阶段超时**时还会叠加最多
3 秒 connect 等待（日志里有重试次数）。`state_ms` 这一版不做范围钳制：`0` 每帧、正数就是毫秒间隔。桥侧用 `bash tools/run-bridge-scenarios.sh` 不用游戏就能验协议，
进关卡验真机用 `bash tools/run-real-game.sh`（它自己起 Bridge 并判定 hello/state_update 条数）。

`diag` 是任务 10 收敛出来的唯一诊断开关（`plugin/Diag.cs`）：**结果行**（工序每步的开始/结果、失败原因、
指令执行）不受它管；**过程行**默认不打 —— 收敛前"每帧一行"的代码散在三处、各判各的标志。

进关卡与调试（游戏跑起来后，往 `$GAME_DIR/BepInEx/wellcooked-command.txt` 写一行，1 秒内执行并删文件）：

```sh
printf 'load=s_sushi_1_2\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"  # 进指定关卡（场景名 = 资源包名）
printf 'restart=1\n' ...        # 重开当前关
printf 'mech=1\n' ...           # 数当前场景真实存在的机制组件与工位
printf 'levels=city\n' ...      # 打印关卡表（菜单位置 ↔ 场景名 ↔ 解锁/通关），可按场景名过滤
printf 'unlock=1\n' ...         # 解锁当前存档全部关卡（含 IsLevelUnlocked 自检）
printf 'orders=reload\n' ...    # 重读订单配置并打印当前配置与已生成单数
```

固定订单（策略对比要用同一组订单，游戏自己随机出单做不到）：

```sh
cp config/orders.example.json "$GAME_DIR/BepInEx/wellcooked-orders.json"   # 一份配置=一个阶段一列菜名
# 菜名 = 关卡菜谱里的名字；写错时日志会列出本关可用菜名（`订单: ... 本关可用菜名：0:Sushi_Fish, ...`）
grep -E '订单: ' "$GAME_DIR/BepInEx/LogOutput.log"                          # 每次生成单都有一行，含阶段/第几单/菜名
```

`config/orders.example.json` 里的菜名是 `s_city_h5` 的（复制过去就能用）；`level` 键只是给人看的，插件不读。
`seed` 只管随机源（回合开始时 `Random.InitState`），`menu` 只管每阶段出哪道菜——两者不是一回事，日志分开记。
它不控制订单的到达时刻（那由订单控制器决定），见 `decisions.md: 2026-10-15 - 固定菜单复用 TAS 的 GetNextRecipe Prefix`。

挑关卡（本体 144 个关卡资源包，包名就是场景名）：

```sh
/tmp/.ocenv/bin/python scripts/scan-levels.py          # 离线按物体名统计工位/机制，先筛掉明显不能用的
bash scripts/eval-levels.sh s_city_h5                  # 每关重开一局，量菜谱 + 连通性 + 机制组件
```

自制演示关卡（任务 13）：构建必须在 **Windows** 侧做（`BuildTarget.StandaloneWindows`；Linux 编辑器没有跨平台构建模块，Windows 编辑器在本机 Wine 下激活界面跑不通）。交接文档、布局数据与生成器脚本都在 `levels/wellcooked_demo/`：

```sh
# Windows 侧（组员）：装 Unity 2017.4.8f1 → 按 levels/wellcooked_demo/README.md 走十条步骤
#   产物 = Assets/AssetBundles/info_wellcooked_demo + s_wellcooked_demo_1
# 装进游戏：拷到 "$GAME_DIR/BepInEx/plugins/OC2DIYLevel/levels/wellcooked_demo/"
# Linux 侧验收：
bash scripts/dev-cycle.sh 20 s_wellcooked_demo_1
printf 'mech=1\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"   # 应输出"机制 = 无（只有通用系统）"
bash scripts/eval-levels.sh s_wellcooked_demo_1                  # 主连通块占比 ≥ 90%
```

待传的三样东西与体积见 `levels/TRANSFER.md`；`Assembly-CSharp` 的压缩包在 `~/oc2-transfer/`。

厨房配置（OS 侧的资源与地面模型，方案 §6.6）：

```sh
printf 'menu=1\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"   # 进关卡后发一次：导出这一关的菜谱表
python3 scripts/export-kitchen.py                      # 读 state/terrain/menu → config/kitchen.json + config/recipes.json
python3 scripts/export-kitchen.py --out /tmp/k.json --out-recipes /tmp/r.json   # 换输出位置（同一份输入两次输出字节相同）
```

`config/kitchen.json` 里：`stations[]` 是工位（`kind` 归类、`id` 稳定、`pos` 是水平面 (x,z)、料箱带 `gives`
与它那个 prefab 的组件名），`grid.cells` 是 0.6 米采样的可走格，`grid.mask` 是每格的邻居掩码
（`edge_bits` 说明位含义，掩码对称）。
`config/recipes.json` 里：`nodes[]` 是工序（`op` 取 fetch/cook/mix/assemble/plate/deliver，带资源类型与工时），
`edges[]` 是"子 → 父"的前置依赖，`dishes[]` 每道菜的工序顺序（拓扑序）；节点 id 是 `工序:游戏里的节点名`，
两道菜共用同一中间产物时只有一个节点。工时目前是方案 §6.5 的默认值，文件里写了来源。
仓库里的 `config/kitchen.example.json` 与 `config/recipes.example.json` 是 `s_city_h5` 的实测导出
（20 个工位 / 546 格 / 4 道菜 / 20 个工序节点），Scheduler 与模拟器可以直接拿它们离线开发。

注意两点：`load` 只换场景，**菜谱要新进程才刷新**；`load` 前要等虚拟手柄被游戏自己加入，否则没有厨师，
地形采样会退化成兜底半径（连通性数字不可信）。

## Layout

- `overcooked_os_course_project.md` — 课设方案：需求、调度模型、IPC、协议、排期
- `1.txt` — 课堂汇报口播稿
- `represent/` — 课堂汇报材料（pptx、生成脚本、SVG、UML、思维导图），不是运行时依赖
- `bridge/` — Linux 侧 OS 核心（C，纯 POSIX，GNU make）。`bridge/include/kitchenos/` 下的头文件就是
  模块契约。目录名取自组员 standards 的命名；原先叫 `core/`，2026-10-15 改名对齐。
- `bridge/tests/` — C 单元测试，`make test` 驱动
- `protocol/` — 两端共享的协议说明与 JSON Schema，是跨端唯一契约
- `tools/` — 开发期工具，最重要的是不依赖游戏的协议级模拟插件与场景 harness
- `plugin/` — Windows 侧 BepInEx 插件。`plugin/WellCooked.Plugin/`（net46，bridge effort 写的，
  含 TCP 客户端、状态读取、输入注入）；`plugin/SmokePlugin.cs`（net35，组员写的 BepInEx+Harmony 冒烟）
- `scripts/` — 组员的部署、构建、启动、验证脚本

Workflow files live where CONVENTIONS §2 says.

## How code is written here

- OS 核心只用纯 POSIX API，不用条件编译区分平台（文档 §2.2.1）；两个实验室（Fedora+Proton、
  WSL2+Windows）共用同一份代码。
- 错误用返回码表达，不 abort；每个失败的系统调用都记录 errno 和上下文。
- 头文件是模块契约：模块之间只通过头文件声明的函数交互，不跨模块摸对方的结构体内部。
- 信号处理函数只置 `volatile sig_atomic_t` 标志，实际工作在主循环里做。
- 跨端消息的字段名以 `protocol/` 为准，两端都不许私自改名；schema 放 `protocol/`，两侧各自实现序列化。
- 调度策略与游戏接入层解耦：策略只依赖订单和资源模型，不依赖游戏 API。
- 多进程写同一日志文件必须加锁或统一由一方写入。
- 游戏插件：BepInEx 5.x + Harmony，C#，协议与宿主平台无关。
- 注释写"为什么"，不写"做了什么"。
- **检查必须双向证明过。** 新增或修改一条检查时，先拿已知坏掉的输入让它失败一次，再让它
  通过；只能通过的检查不算检查。W8 检查曾数着 DEBUG 级日志却没加 `--verbose`，于是永远不
  可能通过，把一次合格运行报成失败。
- **关于代码行为的文档陈述，必须指出哪条检查锁住它**；写不出检查就写明"未验证"。文档说错话
  比没有文档更糟，因为它会被相信 —— 而同一个仓库里两份文档说法相反时，靠"再读一遍"发现不了。

## What we do not do

- **不引入第三方 C 库**。JSON 解析和网络都自己写：实验室机器不能假设能联网装包。
- **不用线程替代进程**。Worker 必须是独立进程，否则展示不了进程控制和进程间通信。
- **不让 Bridge 解释调度策略**。策略属于 Scheduler；Bridge 只搬状态、汇总输入、下发动作。
- **不在插件里做调度**。插件是执行器，收到什么动作就做什么动作。
- 不把离散事件模拟器当演示主平台：它只用于策略预筛，可随时裁剪。
- Windows 侧不放监督进程；守护进程必须是完整 Linux daemon（fork/setsid/umask/chdir/dup2）。
- 不提交游戏 DLL 和二进制文件（§11.6）。

## Branches and landing

Landing: pr

远端 `origin` 与组员共享，任务分支推送后走 PR 合并；主分支 `main`。
