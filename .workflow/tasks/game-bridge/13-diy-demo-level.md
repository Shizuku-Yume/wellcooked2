# 13: 用 OC2DIYLevel 自建演示关卡（一屋子、无机关、菜谱有料）

**Effort:** game-bridge
**Task type:** feature
**Base commit:** 24a18a7
**Base branch:** game-bridge/04-action-injection-single-chef
**Started:** 2026-10-16T12:00:00+08:00
**Status:** paused（构建环节改由组员的 Windows 电脑做，见文末 Handoff）

**Delivers:** 一个**自制关卡**能在游戏里加载并跑通调度演示：单一连通厨房、无传送带/轨道/开关/桥/平台，
菜谱包含多个料与加工步骤（切 + 煮/煎），并把"关卡怎么来的、怎么装"写进文档，使换台机器也能复现。

**为什么自建**：本体 52 关扫过一遍，几乎每关都有地形交互或隔断——"完全无地形物件"的只有
`s_sushi_1_1`（单料、太简单）、`s_city_h4`（可行区碎）、`s_city_h5`（10 料箱/3 砧板/6 盘/1 锅，
厨房 516 格连通，是本体里最接近的一关）；带矿车轨道/传送带/传送门的整片排除
（`s_mine_4_6` 有 `mi_rails`×20、`wizard_*` 有 `wizard_rail`×37、`sushi_1_3/1_4` 有传送带×21/×10）。
用户已定：不迁就本体关卡，自己搭。

**怎么做**（`gua248/Overcooked2-LevelEditor` + `OC2DIYLevel-v0.10`）：

1. Unity 2017.4.8f1 打开编辑器工程：把游戏 `StreamingAssets/Windows` 拷进 `Assets/StreamingAssets`，
   AssetRipper 1.3.14 导出 `Overcooked2_Data` 得到 `Assets/Scripts/Assembly-CSharp`，
   再覆盖仓库里的 `Assembly-CSharp-Patch`。
2. 新建关卡集：`LevelSets/<名字>/data/<关卡>/config_{1p,2p,3p,4p}`、`LevelInfoSO`、`LevelSetInfoSO`（必须叫 `LevelSetInfo`），
   模板场景 `Template/s_template` 复制成 `s_<名字>_1`，改根物体 `PseudoPrefabManager.levelInfo` 引用。
3. 摆厨房（只放桌台/砧板/锅灶/盘子/出菜口/料箱，不放任何机关），选菜谱（主线 34 个或自定义）。
4. `Tools - Toggle Prepare For Building` 清临时物体 → 保存 → 场景与集合目录各设 AssetBundle 名 → `Tools - Build AssetBundles`。
5. 产物丢进 `BepInEx/plugins/OC2DIYLevel/levels/<集合>/`（加载器 `OC2DIYLevel.dll` 同目录）；主菜单卡死时用
   `kfyangc/Overcooked2-DIYLevelFastInit` 的修复。

**Blocked by:** 12
**Files:** `levels/<集合>/`（新目录，放关卡资源包与说明）、`.workflow/standards.md`（安装与构建命令）
**Read first:** `/tmp/gua/Overcooked2-LevelEditor/Docs/zh/tutorial.md`、`reference.md`（物体与菜谱说明）
**Covers:** W6
**Check:** 装好加载器后进自建关卡：`wellcooked-state.json` 的 `scene` 是自制场景名、工位数与摆的一致；
`mech` 输出"机制 = 无（只有通用系统）"；`eval-levels.sh` 量到的厨房可行区是一块（主连通块占比 ≥ 90%）；
`work=auto` 在该关卡完成至少 1 单。

- [ ] 关卡集骨架与 4 人配置（`config_1p..4p`）
- [ ] 厨房布局：一屋子、无机关、料箱/砧板/锅灶/盘子/出菜口齐全，通道够宽（0.6 米网格上厨师半径 0.40 能过）
- [ ] 菜谱：含多料 + 加工（切 + 煮/煎），至少两种菜共享一个中间产物（制造资源争用）
- [ ] 构建出 AssetBundle 并装进 `BepInEx/plugins/OC2DIYLevel/levels/`
- [ ] 关卡资源与"怎么装、怎么改"写进仓库（换机器能复现）
- [ ] 用 `mech` + `eval-levels.sh` 量一遍并留证据（机制、连通性、工位数、菜谱）

**Done when:** 上面每条打勾，Check 的五项证据都有。

## Progress

**卡点：Unity 2017.4 的许可证在这台机器上拿不到**（2026-10-16 实测，四条路都堵）：

| 试过 | 结果 |
|---|---|
| 工程本身需要 **Windows** 版编辑器 | `Assets/Editor/CreateAssetBundles.cs` 写死 `BuildTarget.StandaloneWindows`；Linux 版装完 `Editor/Data/PlaybackEngines/` 只有 `LinuxStandaloneSupport`，且 `LinuxEditorInstaller/UnitySetup-{Windows,Mac}-Support-*.tar.xz` 全是 404（Linux 宿主机没有跨平台构建模块） |
| Windows 版编辑器装在 Wine 下 | 能跑：`Unity.exe -batchmode` 只在最后报 `Unity has not been activated with a valid License`，其余正常 |
| `-createManualActivationFile`（无头 / 带显示 / 带 `-quit`，三次） | 都停在 `DisplayProgressbar: Unity license`，**不产出 `.alf`**；Unity 官方支持文档写明 "Personal 许可证已不再支持手动激活" |
| CLI 激活 `-username/-password` | 同样停在 `DisplayProgressbar: Unity license`，连"凭据错"都没到，说明没走到认证请求 |
| GUI（带 Wine 虚拟桌面） | **编辑器直接崩**，弹出 Unity Bug Reporter（截图 `/tmp/unity-window.png`），拿不到登录窗口 |

补充事实：`license.unity3d.com` / `activation.unity3d.com` / `id.unity.com` 三个域名都还在响应（200/302），
所以不是服务下线，而是 **Wine 下跑不通这个版本的激活界面**。

**已经就绪、与许可证无关的部分**（一旦有可用许可证，只差跑一次构建）：

- 编辑器工程 `~/oc2-level-editor`（206M 的 `gua248/Overcooked2-LevelEditor` 副本，不在 /tmp）。
- AssetRipper 1.3.14 已把游戏导出成 Unity 工程（42G，`~/oc2-export/ExportedProject`），
  `Assets/Scripts/Assembly-CSharp`（2365 个 .cs）已按教程第 5 步拷进编辑器工程第 6 步覆盖 `Assembly-CSharp-Patch`。
- 游戏本体 `StreamingAssets/Windows`（7.7G）已拷进 `Assets/StreamingAssets`。
- 加载器 `OC2DIYLevel-v0.10` 已下载（含 `test_level` / `oc1_story` 示例关卡集，可直接当模板）。
- 关卡数据源：本体这版的菜谱 DAG（`Chicken_Burrito` = 玉米饼 + 煮饭 + 炸鸡；`Sushi_PlainFish`/`Sushi_PlainPrawn` 需要切），
  正好覆盖"切 + 煮/煎 + 装盘"；桌台/器具清单见 `Docs/zh/reference.md`。

## Handoff（2026-10-16，改由组员的 Windows 电脑构建）

**为什么换人做**：本机拿不到 Unity 2017.4 的许可证（四条路实测见 Progress），而 AssetBundle 必须用
Windows 平台构建 —— 事件本身不该卡在这里，换一台能正常激活 Unity 的 Windows 机器做最省事。

**接手的人要做什么**（全部步骤在 `levels/wellcooked_demo/README.md`，输入清单在 `levels/TRANSFER.md`）：

1. 装 Unity 2017.4.8f1（Windows 安装包直链在 README 里；Personal 免费，正常激活）。
2. 拿 `gua248/Overcooked2-LevelEditor`，补 `Assets/StreamingAssets/Windows`（7.7 G，从游戏目录拷）、
   `Assets/Scripts/Assembly-CSharp`（本机已导出的压缩包 `~/oc2-transfer/Assembly-CSharp-patched.tar.gz`，1 MB），
   再把工程自带的 `Assembly-CSharp-Patch/` 覆盖进去。
3. 把 `levels/wellcooked_demo/BuildWellCookedDemo.cs` 放 `Assets/Editor/`、`layout.tsv` 放
   `Assets/LevelSets/wellcooked_demo/`，跑菜单 `Tools → WellCooked → 生成关卡集与场景`。
   **脚本没在编辑器里编译过**（本机没许可证），第一次跑可能要修一轮；Console 里日志带 `[wellcooked_demo]` 前缀。
4. `Tools → Toggle Prepare For Building` → 保存 → 设 bundle 名（场景 `s_wellcooked_demo_1`、集合目录
   `info_wellcooked_demo`）→ `Tools → Build AssetBundles`。
5. 产物两个文件：装进 `$GAME_DIR/BepInEx/plugins/OC2DIYLevel/levels/wellcooked_demo/`（加载器 v0.10
   与 `common*` 已经在游戏里装好了），同时拷一份回仓库 `levels/wellcooked_demo/`。

**我这边接下来做**（产物一到就做）：`dev-cycle.sh` 进自制关卡 → `mech=1` 应是"机制 = 无" →
`eval-levels.sh` 主连通块 ≥ 90% → 工位数与 `layout.tsv` 一致 → `work=auto` 至少完成 1 单；
然后把这一关当作 05/09/12 与阶段 c/d/e 的调试场地。

**已经做掉的**：Unity 两个版本试装与实测结论、AssetRipper 1.3.14 全量导出（42 G，已清理）、
编辑器工程组装（已清理）、`OC2DIYLevel-v0.10` 加载器（已装进游戏的 `BepInEx/plugins/`）、
关卡设计数据与生成器脚本（在仓库里）。

