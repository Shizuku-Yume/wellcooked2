# Standards

## What this project is

WellCooked / KitchenOS：《胡闹厨房2》的订单、工序、厨具被当作作业、任务、共享资源，
在 Linux 上用多进程（Supervisor 守护进程 + Scheduler + Worker + Bridge）和 POSIX IPC
完成调度，游戏画面是可视化和验收载体。使用者是课程教师和答辩评审，唯一优化目标是游戏得分。
设计基线是仓库根的 `overcooked_os_course_project.md`。

## Running it

代码尚未开始，仓库里只有文档和汇报材料。目前唯一能跑的是汇报 deck 生成脚本：

```sh
cd represent && python3 wellcooked_v2/build_deck.py   # 重建 WellCooked_课堂小组汇报_v2.pptx
```

OS 核心和游戏插件落地时必须同时补上构建、测试命令；在那之前，验证只能靠实际运行游戏。

## Layout

- `overcooked_os_course_project.md` — 课设方案：需求、调度模型、IPC、协议、排期
- `1.txt` — 课堂汇报口播稿
- `represent/` — 课堂汇报材料（pptx、生成脚本、SVG、UML、思维导图）
- 代码落地后按方案第 6 章分层：supervisor / scheduler / worker / bridge / protocol / 监控面板

## How code is written here

- OS 核心：C/C++ 或 Rust，纯 POSIX API；两个实验室（Fedora+Proton、WSL2+Windows）共用同一份代码。
- 调度策略与游戏接入层解耦：策略只依赖订单和资源模型，不依赖游戏 API。
- 进程间接口按方案 §5.2 的 JSON 消息定义，schema 放在 `protocol/`，两侧各自实现序列化。
- 多进程写同一日志文件必须加锁或统一由一方写入。
- 游戏插件：BepInEx 5.x + Harmony，C#，协议与宿主平台无关。

## What we do not do

- 不用条件编译区分平台（§2.2.1）：一份 POSIX 代码，两个平台都无条件编译运行。
- 不把离散事件模拟器当演示主平台：它只用于策略预筛，可随时裁剪。
- Windows 侧不放监督进程；守护进程必须是完整 Linux daemon（fork/setsid/umask/chdir/dup2）。
- 不提交游戏 DLL 和二进制文件（§11.6）。

## Branches and landing

Landing: pr

远端 `origin` 与组员共享，任务分支推送后走 PR 合并；主分支 `main`。
