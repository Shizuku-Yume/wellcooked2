# 01: BepInEx 在 Proton 下加载，Harmony 补丁生效

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>

**Delivers:** 启动《胡闹厨房2》到主菜单后，游戏目录出现 `BepInEx/LogOutput.log`，里面有 BepInEx 启动行、`WellCooked.Smoke` 插件加载行和该插件 Harmony 补丁的日志行——插件链路打通，后面读状态和注入输入才有地方落。

**Blocked by:** None
**Files:** `plugin/`（新 C# 工程）、`scripts/install-bepinex.sh`、`scripts/build-plugin.sh`、`scripts/run-game.sh`、`.gitignore`、游戏目录 `/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2/`

**Read first:** spec「How it is built」「How we will know it works」W1、W2；方案 §9 阶段A1、§2.2.1
**Covers:** W1, W2

**Check:** `bash scripts/install-bepinex.sh && bash scripts/build-plugin.sh && bash scripts/run-game.sh`，等游戏到主菜单后 `grep -E 'BepInEx 5|WellCooked\.Smoke|smoke patch' "$GAME/BepInEx/LogOutput.log"` → 三行都出现，且日志里没有 `Failed to load`。

- [ ] BepInEx 5.4.23.5 `win_x64` 部署进游戏目录（`winhttp.dll`、`doorstop_config.ini`、`BepInEx/`），脚本幂等，重跑不报错
- [ ] 插件工程目标框架与 Unity 2017.4 的 Mono 运行时兼容（netstandard2.0 或 net35），能 `dotnet build`
- [ ] 插件里有一个 Harmony 补丁，执行时打印一行可辨识日志
- [ ] 启动游戏后 `LogOutput.log` 同时出现 BepInEx 启动行、插件加载行、补丁日志行
- [ ] 游戏目录里的 BepInEx 二进制和游戏 DLL 都在 `.gitignore` 之外，仓库只提交源码与脚本
- [ ] 顺带记录：可用关卡列表（为 spec 开放问题「第一版固定地图」提供输入）、实际生效的 Proton 启动参数

**Progress:**

