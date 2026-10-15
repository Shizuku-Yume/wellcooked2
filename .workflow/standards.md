# Standards

## What this project is

KitchenOS：把《胡闹厨房2》的厨房协作抽象成操作系统调度问题。Linux/WSL2 侧是一套纯
POSIX 多进程系统（Supervisor 守护进程、Scheduler、多个 Chef Worker、Bridge），Windows
侧是 BepInEx + Harmony 插件，两侧用 TCP + JSON 通信。用于操作系统原理课程设计验收。

设计规格的权威文档是仓库根的 `overcooked_os_course_project.md`；代码与它冲突时以它为准。

## Running it

```sh
# OS 核心（WSL2 / Linux，C + GNU make）
cd core && make            # 构建全部二进制
cd core && make test       # 单元测试，全部通过为绿

# Windows 侧插件（需要 .NET SDK）
dotnet build plugin/WellCooked.Plugin/WellCooked.Plugin.csproj -c Debug
plugin/tools/deploy-plugin.ps1 -Build    # 编译并部署到游戏目录

# 无游戏联调：协议级模拟插件，替代反复启动游戏
python3 tools/fake_plugin.py --scenario happy
```

本机环境：WSL2 发行版 Ubuntu 26.04，已装 gcc / make / python3；**没有 cmake 和 gdb**。
仓库在 Windows 盘，WSL 内是 `/mnt/d`（9p 文件系统），因此进程间共享内存与信号量走
`/dev/shm`（tmpfs），运行时目录（pid、日志）必须可配置，默认指向 Linux 原生路径。

跑测试命令之前不要声称任何东西能用。

## Layout

- `core/` — Linux 侧 OS 核心（C，纯 POSIX）。`core/include/kitchenos/` 下的头文件就是模块契约。
- `core/tests/` — C 单元测试，`make test` 驱动。
- `protocol/` — 两端共享的协议说明与 JSON Schema，是跨端唯一契约。
- `tools/` — 开发期工具，最重要的是不依赖游戏的协议级模拟插件。
- `plugin/WellCooked.Plugin/` — Windows 侧 BepInEx 插件（C#，net46）。
- `represent/` — 课程汇报材料，不是运行时依赖。

Workflow files live where CONVENTIONS §2 says.

## How code is written here

- OS 核心只用纯 POSIX API，不用条件编译区分平台（文档 §2.2.1）。
- 错误用返回码表达，不 abort；每个失败的系统调用都记录 errno 和上下文。
- 头文件是模块契约：模块之间只通过头文件声明的函数交互，不跨模块摸对方的结构体内部。
- 信号处理函数只置 `volatile sig_atomic_t` 标志，实际工作在主循环里做。
- 跨端消息的字段名以 `protocol/` 为准，两端都不许私自改名。
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

## Branches and landing

Landing: local-merge
Main branch: main
