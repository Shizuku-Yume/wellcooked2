# 13: 用 OC2DIYLevel 自建演示关卡（一屋子、无机关、菜谱有料）

**Effort:** game-bridge
**Task type:** feature
**Base commit:** 24a18a7
**Base branch:** game-bridge/04-action-injection-single-chef
**Started:** 2026-10-16T12:00:00+08:00
**Status:** blocked（关卡已验收；`work=auto` 一单等导航修好，菜谱共享中间产物待定，见文末）

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

- [x] 关卡集骨架与 4 人配置（`config_1p..4p`；仓库里是 `s_wellcooked_demo_1..4` 四个场景包）
- [x] 厨房布局：一屋子、无机关、料箱/砧板/锅灶/盘子/出菜口齐全，通道够宽（0.6 米网格上厨师半径 0.40 能过）
      —— 四张地图的厨师位置与关键工位都在主连通块内、`path=` 探测 A* 可达；`_1`/`_3`/`_4` 的
      3/8/12 块是 3 米采样边距里的口袋，不是可玩区断点（判据见 Progress）
- [ ] 菜谱：含多料 + 加工（切 + 煮/煎），至少两种菜共享一个中间产物（制造资源争用）
      —— 切/煮/煎已覆盖（`workable` 标记鱼、虾、鸡肉；`BoiledRice`/`FriedChicken`），
      但导出的 DAG 里没有两道菜共享的中间产物节点；争用目前靠共用 2 块砧板与盘子堆，见 Progress
- [x] 构建出 AssetBundle 并装进 `BepInEx/plugins/OC2DIYLevel/levels/`
- [x] 关卡资源与"怎么装、怎么改"写进仓库（换机器能复现）
- [x] 用 `mech` + `eval-levels.sh` 量一遍并留证据（机制、连通性、工位数、菜谱）

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


## Progress（2026-10-19 验收：四张地图，关键工位均可达；多块是采样口袋）

把仓库里的 5 个 bundle（`info_wellcooked_demo` + `s_wellcooked_demo_1..4`）从 `main` 装进
`$GAME_DIR/BepInEx/plugins/OC2DIYLevel/levels/wellcooked_demo/`。四张地图不是人数变体，是四套不同布局；
每张都带 `config_1p..4p`：

| 场景 | 显示名 | 工位 | 菜谱 | 连通性（主块 / 丢弃） | 结论 |
|---|---|---|---|---|---|
| `s_wellcooked_demo_1` | Basic Kitchen | 43 | Burrito + SushiPrawn | 3 块；216 / 丢弃 168（≈56%）；关键工位 A* 2-5 路点 | 可用（多块是采样口袋） |
| `s_wellcooked_demo_2` | Twin Lines | 44 | Burrito + SushiFish + SushiPrawn | **1 块；154 / 丢弃 0（100%）** | **可用** |
| `s_wellcooked_demo_3` | Ring Bar | 44 | Burrito + SushiFish + SushiPrawn | 8 块；151 / 丢弃 264（≈36%）；关键工位 A* 1-3 路点 | 可用（多块是采样口袋） |
| `s_wellcooked_demo_4` | Big Kitchen | 45 | 实测 Burrito + SushiPrawn（TSV 写 3 道） | 12 块；219 / 丢弃 296（≈43%）；关键工位 A* 1-6 路点 | 可用（多块是采样口袋）；菜谱与 bundle 可能不同步 |

下表的详细验收只跑了 `_2`：

| 检查 | 结果 |
|---|---|
| 进入关卡 | `bash scripts/dev-cycle.sh 10 s_wellcooked_demo_2` → `scene=s_wellcooked_demo_2`，厨师正常加入，无加载报错 |
| 机制 | `mech=1` 没有 `Rail`/`Conveyor`/`Portal`/`Cannon`/`Switch`/`Geyser`/`Bridge`/`Raft`/`Trolley`；只有基础 prefab 自带的通用组件（`TabletopConveyenceWindReceiver`、`WindAccumulator`、`Teleportable*`、`FireExtinguish*`、`PlayerSwitchingManager`），本体关卡也有 |
| 连通性 | `eval-levels.sh s_wellcooked_demo_2`：`连通块 1 个（丢掉 0 格），可走格 154/220`，主块占比 100% |
| 工位 | 合并后 17 个：料箱 5、砧板 2、加热 4（2 灶台 + 2 炸台）、水槽 1、干净盘堆 1、装盘台 1、出菜口 1、脏盘台 1、锅具 4 |
| 菜谱 | `Chicken_Burrito`、`Sushi_PlainFish`、`Sushi_PlainPrawn`；DAG 16 节点/13 边 |
| 切/煮/煎 | `workable=True` 的料：`BurritoChicken`、`SushiFish`、`SushiPrawn`（要切）；`cook:BoiledRice`、`cook:FriedChicken`（要加热）。导出的 DAG 没有显式 `chop` 节点（导出的既定行为：菜谱树只写组成，切由 `workable` 决定） |
| 共享中间产物 | **没有**：两道寿司各用各的鱼/虾，和鸡肉卷也没有共同节点；争用靠共用 2 块砧板与干净盘堆（README 的设计口径） |
| `work=auto` 完成 1 单 | **未通过**：31 秒内订单 5 个、`score=-60`，两名厨师在 `侧向错身`/`卡住` 处停住，没有交付。与任务 05 的导航阻塞是同一件事 |

`export-kitchen.py` 的输出（`/tmp/wc-kitchen.json`、`/tmp/wc-recipes.json`）与 `eval-levels.sh`
的数字一致。四张地图都能加载；从厨师位置用 `path=` 探测料箱/加热/出菜/水槽/盘堆，全部返回
1-6 个路点，说明这些关键工位都在主连通块里。`_1`/`_3`/`_4` 多出来的 3/8/12 块是采样 3 米边距
里的口袋（墙外、台面后），不是可玩区的断点；**“主块占比 ≥90%”这个判据会把它们误判成不可用**，
应改成“所有 spawn + 关键工位在主块内且 A* 可达”。另外 `_4` 的 TSV 写了 3 道菜，实测菜单只有
2 道，可能是 bundle 与最新 layout 不同步。

## Blocked

- **2026-10-19：连通性判据要改，不是地图不能用。** `eval-levels.sh` 报的 `_1/_3/_4` 3/8/12 块，
  是 3 米采样边距里的口袋（墙外/台面后）；四张地图的厨师位置与关键工位都在主块内，`path=` 探测
  A* 1-6 路点可达。任务 Check 的“主连通块 ≥90%”应改成“所有 spawn + 关键工位在主块内且两两
  A* 可达”，否则会把可用地图误判成不可用。若仍要主块占比，得先把采样边距/口袋从分母里去掉。
- **2026-10-19：`_4` 的菜谱与 bundle 可能不同步。** `layout_4.tsv` 写 3 道菜，实测菜单只有
  2 道（Burrito + SushiPrawn）；需要在编辑器里按当前 layout 重新生成并打包，或核对 bundle 版本。
- **2026-10-19：`work=auto` 在 `_2` 完成 1 单被导航阻塞**（技术债「导航的第一路点被挡：
  A* 说可达，转向却原地不动或侧向错身」）。`_2` 已能加载、连通、出单；导航修好后回来重跑
  这一项即可。
- **菜谱共享中间产物**：任务清单要求“至少两种菜共享一个中间产物”，当前 `_2` 的 DAG 没有共享节点，
  只有共用工位/盘子。要么把一道菜改成与 `Chicken_Burrito` 共用 `BoiledRice`（或其他中间产物），
  要么把这条判据改成“共享工位/资源”。这是设计决定，等用户定。
