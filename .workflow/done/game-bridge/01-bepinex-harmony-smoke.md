# 01: BepInEx 在 Proton 下加载，Harmony 补丁生效

**Effort:** game-bridge
**Task type:** feature
**Base commit:** 3f566f251e24250c1735c96299977d8af6796a26
**Started:** 2026-10-14T13:31:24+08:00

**Delivers:** 启动《胡闹厨房2》后（标题画面即可，日志在这之前就写完了），游戏目录出现 `BepInEx/LogOutput.log`，里面有 BepInEx 启动行、`WellCooked.Smoke` 插件加载行和该插件 Harmony 补丁的日志行——插件链路打通，后面读状态和注入输入才有地方落。

**Blocked by:** None
**Files:** `plugin/`（新 C# 工程）、`scripts/install-bepinex.sh`、`scripts/build-plugin.sh`、`scripts/run-game.sh`、`.gitignore`、游戏目录 `/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2/`

**Read first:** spec「How it is built」「How we will know it works」W1、W2；方案 §9 阶段A1、§2.2.1
**Covers:** W1, W2

**Check:** 前置：游戏没在运行（写注册表那步会拒绝）。

```sh
export GAME_DIR="${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}"  # 换机器只改这一行
bash scripts/install-bepinex.sh && bash scripts/set-winhttp-override.sh \
  && bash scripts/build-plugin.sh
rm -f "$GAME_DIR/BepInEx/LogOutput.log"    # 先清掉上一次的日志，否则旧日志会让下面的断言先通过
bash scripts/run-game.sh                   # 到标题画面即可，日志在这之前就写完了
# 起游戏后 30 秒内日志应出现下面三行；第 4 条必须是 0（grep -c 输出 0 时退出码是 1，别用 && 串）
grep -E 'BepInEx 5' "$GAME_DIR/BepInEx/LogOutput.log"
grep -F 'Loading [WellCooked.Smoke' "$GAME_DIR/BepInEx/LogOutput.log"
grep -F 'smoke patch alive:' "$GAME_DIR/BepInEx/LogOutput.log"
grep -c 'Failed to load' "$GAME_DIR/BepInEx/LogOutput.log"
```

- [x] BepInEx 5.4.23.5 部署进游戏目录（`winhttp.dll`、`doorstop_config.ini`、`BepInEx/`），脚本幂等，重跑不报错。包按游戏 exe 的 PE 头选 `win_x86`（原计划写 `win_x64` 是错的：`Overcooked2.exe` 是 32 位）
- [x] 插件工程目标框架与 Unity 2017.4 的 Mono 运行时兼容（`net35`，CLR 报 2.0.50727），能 `dotnet build`
- [x] 插件里有两个 Harmony 补丁，执行时打印可辨识日志
- [x] 启动游戏后 `LogOutput.log` 同时出现 BepInEx 启动行、插件加载行、补丁日志行
- [x] 游戏目录里的 BepInEx 二进制和游戏 DLL 都在 `.gitignore` 之外，仓库只提交源码与脚本
- [x] 记录实际生效的启动方式：`steam -applaunch 728880` 加前缀注册表里的 `winhttp=native,builtin` 覆盖
- [x] 额外产出：`BepInEx/welcooked-inventory.txt`（Assembly-CSharp 里 543 个相关类型的方法清单，任务 03/04 的起点）
- [x] 关卡列表交付物转交任务 04（读列表要先能注入输入，04 才有这个能力），spec 开放问题 1 和任务 04 的验收清单已同步

**Progress:**
Dirty at start: none

Check before: 脚本和插件工程都还不存在，`scripts/install-bepinex.sh` 无法执行，Check 跑不起来。
本机事实：`~/.dotnet` 只有 .NET 9 运行时（无 SDK，无法编译 C#）；游戏 `Overcooked2_Data/Managed/` 里没有 `netstandard.dll`，所以插件必须按 `net35` 编译。

Check after（脚本重跑一次，日志取自当前运行实例）：

```
[Message:   BepInEx] BepInEx 5.4.23.5 - Overcooked2 (9/29/2026 5:54:28 PM)
[Info   :   BepInEx] Loading [WellCooked.Smoke 0.1.0]
[Info   :WellCooked.Smoke] WellCooked.Smoke loaded; Unity 2017.4.8f1, BepInEx 5.4.23.5
[Info   :WellCooked.Smoke] smoke patch applied: GamepadEngagementManager.Update
[Info   :WellCooked.Smoke] smoke patch applied: Input.GetKey
[Info   :WellCooked.Smoke] smoke patch alive: GamepadEngagementManager.Update call #15000
```

`grep -c 'Failed to load'` = 0。真实路径：`bash scripts/run-game.sh` 起游戏，窗口标题 `Overcooked2`，截图确认停在标题画面（按 A/空格继续），补丁每 600 次调用打一行、游戏跑着就一直在涨，说明补丁真的在游戏主循环里执行。

过程中查到的、任务文件里没写的事实：

- 游戏是 32 位：`Overcooked2.exe` 和 `UnityPlayer.dll` 都是 PE32/x86。第一版脚本装了 `win_x64` 包，`winhttp.dll` 是 x64，doorstop 根本不会被加载；现在脚本从 exe 的 PE 头读架构，自动选 `win_x86`。
- 装 .NET SDK：本机只有 .NET 9 运行时，用 `dotnet-install.sh --channel 9.0` 装到 `~/.dotnet`（用户级，不碰系统包）。
- 第一次启动（没有 DLL 覆盖）游戏进程起来了但游戏目录没有 `LogOutput.log`，与 BepInEx 文档一致：Wine 优先用自带 `winhttp`。`scripts/set-winhttp-override.sh` 把 `winhttp=native,builtin` 写进 Proton 前缀注册表后日志出现（前缀 `user.reg` 已备份为 `user.reg.wellcooked-backup`）。
- 补丁目标换了两次：先补 `UnityEngine.Input.GetKey`，游戏跑了 2 分钟一次都没被调用（游戏只在 XInput 手柄路径上读输入，InControl 不走旧键盘 API），于是改补游戏自己的 `GamepadEngagementManager.Update()`，立刻生效。这条结论直接决定任务 04 的注入路线。
- 游戏目录里有第三方代理 DLL（`version.dll`、`winmm.dll`，Koaloader/KoalaBox 那类加载器，都是 x64），和 BepInEx 的 `winhttp.dll` 不冲突；它们不是本项目的文件，没有改动。
- 插件不依赖 NuGet 上的 BepInEx 包（`BepInEx.Core` 不在 nuget.org 上），改成直接引用游戏目录 `BepInEx/core/` 里的 `BepInEx.dll` 和 `0Harmony.dll`。

计划外的一次改动：原验收项里的「可用关卡列表」转给了任务 04（不是删掉）。关卡列表要靠游戏界面点进去看，进菜单需要输入注入，而这正是任务 04 的能力；spec 开放问题 1 和任务 04 的验收清单已同步。这条改动需要用户确认。

**Review（flow-verify，2026-10-14）**

- brief A（按需求交付 + 过程）：blocked，4 blocking / 1 nonblocking。
- brief B（质量与安全）：blocked，3 blocking / 3 nonblocking。
- Review boundary: `3f566f251e24250c1735c96299977d8af6796a26` → 工作区（含 claim `613acc9`）; round: 1; briefs: A,B,sec; boundaries: 外部下载→本地与游戏目录、插件→游戏进程、脚本→Steam、脚本→Proton 注册表; findings: 7 blocking / 4 nonblocking（A 4+1、B 3+3）

处理（本轮一次性修完，然后重检）：

- A1 关卡列表：改为转交任务 04，spec 与任务 04 同步（见上）。
- A2 「到主菜单」：这是我写宽了。BepInEx 启动、插件加载、补丁执行都发生在标题画面之前，日志已经证明；W1 措辞改成「启动游戏后（标题画面即可）」，进主菜单的能力记在任务 04。
- A3 Check 命令补全：加上 `set-winhttp-override.sh`、定义 `GAME`、四个断言分开写。
- A4 spec 里「只能用 `BepInEx_win_x64`」改成按游戏 exe 的 PE 头选架构（本机 `win_x86`）。
- A5 `standards.md` 的 Running/Layout 补上插件构建与验证入口；spec「What this is for」改成实施前背景。
- B1 `set-winhttp-override.sh`：`STEAM_COMPAT_DATA_PATH` 是 compatdata 目录，前缀在它下面的 `pfx/`，已修正。
- B2 下载改原子：临时文件 + 完整性校验 + 改名；损坏的旧缓存会重新下载。
- B3 `install-bepinex.sh` / `build-plugin.sh` 统一把 `GAME_DIR` 解析成绝对路径。
- B4 `pgrep` 区分「没在跑」（1）和「查询失败」（其他），后者拒绝改注册表。
- B5 `user.reg` 改成同目录临时文件 + fsync + 原子替换。
- B6 补丁计数器改用 `MethodBase` 作键，不再每帧拼字符串。

修复后重跑（同一个 Check）：

```
grep -E 'BepInEx 5'            → [Message: BepInEx] BepInEx 5.4.23.5 - Overcooked2 (9/29/2026 5:54:28 PM)
grep -F 'Loading [WellCooked.Smoke' → [Info: BepInEx] Loading [WellCooked.Smoke 0.1.0]
grep -F 'smoke patch alive:'   → smoke patch alive: GamepadEngagementManager.Update call #1
grep -c 'Failed to load'       → 0
部署的 WellCooked.Smoke.dll 与构建产物 sha256 相同：2fbaed8f0bfe9d5ad656618bde79b7f267d4c5016c887dfb136eb111b69099a1
```

**重检（round 2，同一对比点）**

- brief A：1 blocking / 5 nonblocking。`Delivers` 里还留着「到主菜单」；`standards.md` 的验证命令用了没定义的 `$GAME`；任务 Check 的注释混用 `GAME`/`GAME_DIR`；四条断言可能被上一轮的旧日志蒙过；spec 与任务 04 对「怎么读关卡列表」说法不一致；Review boundary 行缺 §7 要求的字段。
- brief B：0 blocking / 4 nonblocking。下载物只做形状检查、没有校验和；`zip_ok` 对同一文件跑两遍 `unzip -l` 且用 `producer | grep -q`（在 pipefail 下脆弱）；游戏目录默认值散在三处；`GAME_DIR` 指向的目录缺 `Overcooked2.exe` 时抛 Python traceback。
- Review boundary（round 2）: `3f566f251e24250c1735c96299977d8af6796a26` → 工作区; round: 2; briefs: A,B,sec; boundaries: 外部下载→缓存→游戏目录、脚本→Proton 前缀注册表、脚本→shell(env)、插件→游戏进程/文件系统; findings: 1 blocking / 9 nonblocking

处理：

- `Delivers` 改成「启动《胡闹厨房2》后（标题画面即可）」；`standards.md` 和任务 Check 统一用 `GAME_DIR`，Check 里先删旧日志再起游戏，并注明 `grep -c` 输出 0 时退出码是 1。
- spec 与任务 04 的关卡列表说法对齐（都要进游戏界面读）。
- `install-bepinex.sh` 加发行包 sha256 校验：5.4.23.5 的 x86/x64 都钉了 GitHub release 的 asset digest，换版本/架构用 `BEPINEX_SHA256` 覆盖；`zip_ok` 改成一次 `unzip -l` 后对变量做匹配；缺 `Overcooked2.exe` 或不是 PE 文件时给中文报错。
- 「三处重复的游戏目录默认值」不在这轮修，记进 `.workflow/technical-debt.md`（条目里写了为什么不在这轮做）。

**Finished:** 2026-10-14T13:52:00+08:00

