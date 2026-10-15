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
cd core && make            # 构建全部二进制
cd core && make test       # 单元测试，全部通过为绿
cd core && make test-san   # 同上，带 ASan/UBSan；对象写 build/san/，不污染普通 make

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

## Layout

- `overcooked_os_course_project.md` — 课设方案：需求、调度模型、IPC、协议、排期
- `1.txt` — 课堂汇报口播稿
- `represent/` — 课堂汇报材料（pptx、生成脚本、SVG、UML、思维导图），不是运行时依赖
- `core/` — Linux 侧 OS 核心（C，纯 POSIX，GNU make）。`core/include/kitchenos/` 下的头文件就是
  模块契约。组员的 standards 把这一层命名为 `bridge/`，改名对齐尚未做，见 technical-debt。
- `core/tests/` — C 单元测试，`make test` 驱动
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
