# wellcooked_demo：自制演示关卡

4 个厨师、单一连通屋子、无任何机关；3 道菜覆盖 **切 / 煮 / 煎 / 装盘**，并且两道菜共用砧板与盘子堆
（资源争用是调度演示要看的东西）：

| 菜 | 工序 | 用到的工位 |
|---|---|---|
| `Burrito_Chicken` | 米要煮、鸡肉要切+炸、玉米饼直接拿、装盘 | 灶台+锅、炸台+锅、砧板、盘堆、上菜口 |
| `Sushi_PlainFish` | 鱼要切 | 砧板、盘堆、上菜口 |
| `Sushi_PlainPrawn` | 虾要切 | 砧板、盘堆、上菜口 |

布局数据在 `layout.tsv`（0.6 米格，7 列：kind / prefab / so / ingredient / x / z / note），
生成器是 `BuildWellCookedDemo.cs`。

## 为什么这台机器上得在 Windows 侧构建

游戏是 Windows 版，AssetBundle 必须用 `BuildTarget.StandaloneWindows` 构建，而
`Assets/Editor/CreateAssetBundles.cs` 把它写死了。Unity 的 **Linux** 编辑器只带
`LinuxStandaloneSupport`（`UnitySetup-Windows-Support-*.tar.xz` 在官方源上是 404），
Windows 版编辑器在本机 Wine 下能跑但**激活界面跑不起来**（`-createManualActivationFile` 不产出 `.alf`、
GUI 直接崩、CLI 激活停在弹窗；Unity 官方文档写明 Personal 已不再支持手动激活）。
所以：在 Windows 机器上装 Unity 2017.4.8f1（或 2017.4.40f1，同 minor 版本、AssetBundle 格式一致），
正常激活（Personal 免费），在那里构建一次。

## 在 Windows 机器上的步骤

1. **装 Unity 2017.4.8f1**（Windows 安装包：`download.unity3d.com/download_unity/5ab7f4878ef1/Windows64EditorInstaller/UnitySetup64-2017.4.8f1.exe`），
   用 Unity ID 激活 Personal 许可证。
2. **拿编辑器工程**：`git clone https://github.com/gua248/Overcooked2-LevelEditor`（本仓库的
   `~/oc2-level-editor` 就是它的副本）。
3. **补三样东西**（见本目录上一级的 `TRANSFER.md`）：
   - `Assets/StreamingAssets/Windows`：从游戏目录 `Overcooked2_Data/StreamingAssets/Windows` 拷（7.7 GB）；
   - `Assets/Scripts/Assembly-CSharp`：AssetRipper 1.3.14 导出游戏后的那个目录（本机已导出，18 MB，随传输包给）；
   - 再把工程自带的 `Assembly-CSharp-Patch/` 里的文件覆盖进 `Assets/Scripts/Assembly-CSharp/`。
4. **放生成器**：把本目录的 `BuildWellCookedDemo.cs` 拷到工程的 `Assets/Editor/`，
   `layout.tsv` 拷到 `Assets/LevelSets/wellcooked_demo/layout.tsv`。
5. **打开工程**，Console 应该没有编译错误（有的话照报错改，脚本里每条日志都带 `[wellcooked_demo]` 前缀）。
6. **菜单 Tools → WellCooked → 生成关卡集与场景**：生成 `LevelSets/wellcooked_demo/` 下的
   `LevelInfo_wellcooked_demo_1.asset`、`config_1p..4p.asset`、`LevelSetInfo.asset` 与场景
   `scenes/s_wellcooked_demo_1.unity`（从 `Assets/Template/s_template.unity` 复制并摆台）。
7. **（若脚本没跑通）手工版**：照 `Docs/zh/tutorial.md`——复制 `Template/s_template.unity` 成
   `scenes/s_wellcooked_demo_1.unity`、新建 `LevelSets/wellcooked_demo/data/wellcooked_demo_1/` 下的
   `LevelInfoSO` 与 `config_1p..4p`、集合根放一个**名字就叫 `LevelSetInfo`** 的 `LevelSetInfoSO`，
   场景根物体的 `PseudoPrefabManagerStub.levelInfo` 指到 `LevelInfoSO`，再按 `layout.tsv` 摆台。
8. **出包**：`Tools → Toggle Prepare For Building`（清临时物体）→ 保存场景 →
   场景的 AssetBundle 名设 `s_wellcooked_demo_1`、集合目录设 `info_wellcooked_demo` →
   `Tools → Build AssetBundles` → 产物在 `Assets/AssetBundles/`。
9. **装进游戏**：`info_wellcooked_demo` 与 `s_wellcooked_demo_1` 两个文件拷到
   `Overcooked! 2/BepInEx/plugins/OC2DIYLevel/levels/wellcooked_demo/`（加载器 `OC2DIYLevel.dll`
   与 `common*` 资源在同级目录，见加载器发行包 v0.10）。
10. **把产物拷回本仓库**：两个 bundle 文件放进 `levels/wellcooked_demo/`，连同这份 README 一起提交。

## 验收（回到 Linux 侧）

```sh
bash scripts/dev-cycle.sh 20 s_wellcooked_demo_1     # 进自制关卡
printf 'mech=1\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"   # 应输出"机制 = 无（只有通用系统）"
bash scripts/eval-levels.sh s_wellcooked_demo_1      # 可行区主连通块占比 ≥ 90%
# 工位数与 layout.tsv 一致；work=auto 至少完成 1 单
```
