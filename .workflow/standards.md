# Standards

## What this project is

WellCooked / KitchenOS：《胡闹厨房2》的订单、工序、厨具被当作作业、任务、共享资源，
在 Linux 上用多进程（Supervisor 守护进程 + Scheduler + Worker + Bridge）和 POSIX IPC
完成调度，游戏画面是可视化和验收载体。使用者是课程教师和答辩评审，唯一优化目标是游戏得分。
设计基线是仓库根的 `overcooked_os_course_project.md`。

## Running it

```sh
(cd represent && python3 wellcooked_v2/build_deck.py)   # 重建课堂汇报 pptx（子 shell，不改当前目录）

export GAME_DIR="${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}"  # 换机器改这一行
bash scripts/install-bepinex.sh        # 把 BepInEx 装进游戏目录（按游戏 exe 的 PE 头选架构）
bash scripts/set-winhttp-override.sh   # 让 Proton 加载游戏目录的 winhttp.dll（每台机器做一次）
bash scripts/build-plugin.sh           # dotnet build plugin/ 并拷进 BepInEx/plugins/
bash scripts/run-game.sh               # steam -applaunch 728880
grep -E 'BepInEx 5|WellCooked\.Smoke|smoke patch alive' "$GAME_DIR/BepInEx/LogOutput.log"   # 验证
```

前置条件：.NET SDK（本机装在 `~/.dotnet`，`scripts/build-plugin.sh` 默认用它）、游戏目录
（默认本机 Steam 库路径，其他机器用 `GAME_DIR` 覆盖）、Steam 在运行；改注册表和装 BepInEx 前先退出游戏。
还没有测试命令：OS 核心和 Bridge 尚未开始，插件侧只能靠启动游戏看日志验证。

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

注意两点：`load` 只换场景，**菜谱要新进程才刷新**；`load` 前要等虚拟手柄被游戏自己加入，否则没有厨师，
地形采样会退化成兜底半径（连通性数字不可信）。

## Layout

- `overcooked_os_course_project.md` — 课设方案：需求、调度模型、IPC、协议、排期
- `1.txt` — 课堂汇报口播稿
- `represent/` — 课堂汇报材料（pptx、生成脚本、SVG、UML、思维导图）
- `plugin/` — BepInEx 插件（C#，net35）；`scripts/` — 部署、构建、启动、验证脚本
- 尚未开始：`protocol/`（JSON 协议与 schema）、`bridge/`（Linux 侧 C11），之后是 Supervisor / Scheduler / Worker

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
