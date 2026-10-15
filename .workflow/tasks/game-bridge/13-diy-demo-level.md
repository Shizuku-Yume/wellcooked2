# 13: 用 OC2DIYLevel 自建演示关卡（一屋子、无机关、菜谱有料）

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>

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
