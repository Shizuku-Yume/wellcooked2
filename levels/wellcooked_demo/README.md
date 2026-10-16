# wellcooked_demo 自制关卡交接与构建文档

本文档用于指导在 Windows 机器上使用 Unity 编辑器工程完成 `wellcooked_demo` 自制演示关卡的 AssetBundle 打包，并将产物部署到游戏中供调度系统进行联调验收。

---

## 1. 这个关卡是什么、为什么这样做

### 1.1 关卡配置与特性
- **厨师数量**：固定支持 4 个厨师（包含 1p 到 4p 的完整出单与局时配置）。
- **厨房地形**：单一连通房间，无任何隔断门、无动态机关、无传送带、无矿车轨道、无移动浮台或交互开关。所有可行区域连为一体，通道宽度大于 1 格（厨师碰撞半径为 0.40 米）。
- **菜谱覆盖**：包含 3 道菜品，覆盖切、煮、煎、装盘四种基本烹饪工序：
  - `Burrito_Chicken`（鸡肉卷）：米需要用煮锅在灶台上煮熟；鸡肉需要先在砧板上切碎，再放入煎锅在炸台上煎炸；玉米饼无需加工直接从料箱取出；三者组装装盘后送往上菜口。
  - `Sushi_PlainFish`（原味鱼寿司）：寿司鱼需要切碎，装盘送往上菜口。
  - `Sushi_PlainPrawn`（原味虾寿司）：寿司虾需要切碎，装盘送往上菜口。

| 菜品名 | 工序要求 | 涉及工位与器具 |
|---|---|---|
| `Burrito_Chicken` | 煮饭 + 切鸡肉 + 炸鸡肉 + 取玉米饼 + 装盘 | 灶台+煮锅、炸台+炸锅、砧板、玉米饼料箱、盘堆、上菜口 |
| `Sushi_PlainFish` | 切鱼肉 + 装盘 | 砧板、鱼肉料箱、盘堆、上菜口 |
| `Sushi_PlainPrawn` | 切虾肉 + 装盘 | 砧板、虾肉料箱、盘堆、上菜口 |

### 1.2 资源争用设计
寿司与鸡肉卷共用 2 块砧板与干净盘子堆。当两个或多个订单并发到达时，多个厨师需要争夺有限的切菜工位与盘子资源。资源争用与等待是调度策略演示要观察的目标现象。

### 1.3 用途
该关卡作为纯净基准场景，用于联调与验证以下内容：
- **任务 05**：双厨师协同完成订单；
- **任务 09**：多厨师工序调度与路径规划；
- **任务 12**：长工序与异常恢复流程；
- **阶段 c / d / e**：多厨师高并发、资源争用下的调度策略演示。

---

## 2. 为什么必须由 Windows 侧构建

### 2.1 游戏与打包平台约束
《胡闹厨房2》本体为 Windows 平台可执行程序，其运行时加载的 AssetBundle 必须指定 `BuildTarget.StandaloneWindows` 进行构建。编辑器工程中的打包代码 `Assets/Editor/CreateAssetBundles.cs` 已将该目标平台写死。

### 2.2 Linux 编辑器缺失构建模块
Unity 官方提供的 Linux 平台编辑器仅附带 `LinuxStandaloneSupport`。经核对，Unity 官方下载源上的 Windows 跨平台构建支持包（`LinuxEditorInstaller/UnitySetup-Windows-Support-*.tar.xz`）均返回 HTTP 404，Linux 宿主环境无法安装 Windows 目标平台的导出模块。

### 2.3 Linux 本机 Wine 环境实测结论
在 Linux 本机 Wine 环境下尝试运行 Windows 版 Unity 编辑器，安装程序与批处理命令能够执行，但许可证激活流程被阻断（实测记录见 `.workflow/tasks/game-bridge/13-diy-demo-level.md` 的 Progress）：
1. **手动文件激活（`-createManualActivationFile`）**：在无头模式、带虚拟显示、添加 `-quit` 等多种组合下实测 3 次，进程均停滞在 `DisplayProgressbar: Unity license`，未生成 `.alf` 激活申请文件；Unity 官方支持文档明确说明 Personal 许可证已停止支持手动文件激活。
2. **命令行凭据激活（`-username` / `-password`）**：执行后同样停滞在 `DisplayProgressbar: Unity license`，未向认证服务器发起请求。
3. **图形界面激活**：在 Wine 虚拟桌面下启动 `Unity.exe` 时编辑器直接崩溃，弹出 Unity Bug Reporter 错误窗口，无法展示登录界面。
4. **服务连通性验证**：网络探测显示 `license.unity3d.com`、`activation.unity3d.com` 与 `id.unity.com` 均正常返回 200 或 302，说明激活服务在线，阻断原因为 Wine 环境无法运行该版本 Unity 的激活界面。

### 2.4 结论
AssetBundle 打包必须在 Windows 物理机上完成。需在 Windows 环境安装 Unity 2017.4.8f1（或 2017.4.40f1，同属 2017.4 minor 版本，AssetBundle 数据格式一致），登录 Unity ID 完成 Personal（免费）许可证激活。

---

## 3. 开工前要准备的东西

在 Windows 电脑上开始操作前，需备齐以下 5 项材料（对应 `levels/TRANSFER.md` 清单）：

### 3.1 Unity 编辑器安装包
- Unity 2017.4.8f1 官方 Windows 64 位安装程序直链：
  `https://download.unity3d.com/download_unity/5ab7f4878ef1/Windows64EditorInstaller/UnitySetup64-2017.4.8f1.exe`
- 若通过 Unity Hub 安装，可选用同版本的 Unity 2017.4.40f1。

### 3.2 关卡编辑器开源工程
- GitHub 仓库地址：`https://github.com/gua248/Overcooked2-LevelEditor`
- 通过 Git 克隆至本地目录。

### 3.3 游戏资源目录 `StreamingAssets/Windows`（7.7 GB）
- 来源：从《胡闹厨房2》游戏安装目录的 `Overcooked2_Data/StreamingAssets/Windows` 完整拷贝（Linux 本机 Steam 默认路径为 `/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2/Overcooked2_Data/StreamingAssets/Windows`）。
- 目标：放置于编辑器工程的 `Assets/StreamingAssets/Windows`。

### 3.4 游戏反编译代码目录 `Assembly-CSharp`（压缩包 1 MB / 解压后 18 MB）
- 目标：放置于编辑器工程的 `Assets/Scripts/Assembly-CSharp`。
- 来源：Linux 本机已使用 AssetRipper 1.3.14 从游戏 `Overcooked2_Data` 完整导出代码（共 2365 个 `.cs` 文件，解压后约 18 MB），压缩包（约 1 MB）存放于本仓库宿主机的 `oc2-transfer/`。解压后直接拷入 Windows 工程。
- 替代方案：若未获取该传输包，需在 Windows 上运行 AssetRipper 1.3.14 自行导出游戏项目的脚本代码。

### 3.5 编辑器工程代码补丁 `Assembly-CSharp-Patch/`
- 来源：关卡编辑器工程自带的 `Assembly-CSharp-Patch/` 目录（包含 44 个代码文件）。
- 操作：将该目录下的全部 44 个文件直接覆盖拷贝到 `Assets/Scripts/Assembly-CSharp/` 中。

---

## 4. 十条步骤

在 Windows 机器上按以下 10 个步骤顺序执行：

### 第 1 步：安装并激活 Unity 2017.4.8f1（或 2017.4.40f1）
- **操作**：运行 `UnitySetup64-2017.4.8f1.exe` 安装编辑器，启动 Unity，登录个人 Unity ID，选择 Personal（个人免费版）许可证完成激活。
- **检验标准**：启动 Unity 后正常显示项目管理窗口，无许可证报错提示，可正常新建或打开工程。

### 第 2 步：获取关卡编辑器工程
- **操作**：在工作目录下使用 Git 克隆工程仓库：
  ```cmd
  git clone https://github.com/gua248/Overcooked2-LevelEditor
  ```
- **检验标准**：本地目录生成 `Overcooked2-LevelEditor`，内部包含 `Assets/`、`Assembly-CSharp-Patch/`、`Docs/` 等结构。

### 第 3 步：补齐三项工程依赖
- **操作**：
  1. 将游戏的 `Overcooked2_Data/StreamingAssets/Windows` 复制到工程的 `Assets/StreamingAssets/Windows`（约 7.7 GB）；
  2. 将传输包中的 `Assembly-CSharp` 代码解压到工程的 `Assets/Scripts/Assembly-CSharp`；
  3. 将工程自带的 `Assembly-CSharp-Patch/` 内 44 个文件覆盖复制进 `Assets/Scripts/Assembly-CSharp/`。
- **检验标准**：`Assets/StreamingAssets/Windows` 下包含游戏原始 bundle 文件；`Assets/Scripts/Assembly-CSharp` 下包含 2365 个代码文件且补丁文件已被覆盖替换。

### 第 4 步：放置生成器脚本与布局数据
- **操作**：
  1. 将本仓库的 `levels/wellcooked_demo/BuildWellCookedDemo.cs` 复制到工程的 `Assets/Editor/` 目录下；
  2. 在工程中创建目录 `Assets/LevelSets/wellcooked_demo/`，并将本仓库的 `levels/wellcooked_demo/layout.tsv` 复制到该目录下（即路径为 `Assets/LevelSets/wellcooked_demo/layout.tsv`）。
- **检验标准**：确认工程中存在 `Assets/Editor/BuildWellCookedDemo.cs` 与 `Assets/LevelSets/wellcooked_demo/layout.tsv` 两个文件。

### 第 5 步：启动 Unity 打开工程并检查 Console
- **操作**：在 Unity 启动器中打开克隆的工程目录。首次打开会执行资源导入与代码编译。导入结束后，打开 Console 窗口（快捷键 `Ctrl + Shift + C` 或菜单 `Window → Console`）。
- **检验标准**：Console 窗口中没有红色的编译错误日志。若有代码引用或类型缺失报错，需核对步骤 3 中的代码导入与补丁覆盖是否完整。

### 第 6 步：运行生成器脚本
- **操作**：点击 Unity 顶部菜单栏 `Tools → WellCooked → 生成关卡集与场景`。
- **检验标准**：Console 窗口打印带有 `[wellcooked_demo]` 前缀的日志，包含 `[wellcooked_demo] 摆了 20 个物体` 以及 `[wellcooked_demo] 完成。下一步：Tools → Toggle Prepare For Building → 保存场景 → Tools → Build AssetBundles`。Project 视图中自动生成了 `Assets/LevelSets/wellcooked_demo/data/wellcooked_demo_1/` 下的 `LevelInfo_wellcooked_demo_1.asset`、`config_1p..4p.asset`、关卡集资产 `Assets/LevelSets/wellcooked_demo/LevelSetInfo.asset` 以及场景 `Assets/LevelSets/wellcooked_demo/scenes/s_wellcooked_demo_1.unity`；Hierarchy 视图中已自动打开该场景并摆好了各个台面、器具与 4 个厨师出生点。

### 第 7 步：手工备用方案（脚本报错时兑底）
- **操作**：若生成脚本执行中断且无法快速修复，依照 `Docs/zh/tutorial.md` 教程手工兑底：
  1. 复制 `Assets/Template/s_template.unity` 为 `Assets/LevelSets/wellcooked_demo/scenes/s_wellcooked_demo_1.unity`；
  2. 在 `Assets/LevelSets/wellcooked_demo/data/wellcooked_demo_1/` 下复制 `Assets/Template/levelinfo_template.asset` 为 `LevelInfo_wellcooked_demo_1.asset`，指定 3 道菜谱（`Burrito_Chicken_SO`、`Sushi_PlainFish_SO`、`Sushi_PlainPrawn_SO`）及 1p 至 4p 对应的配置资产；
  3. 在 `Assets/LevelSets/wellcooked_demo/` 目录下创建 `LevelSetInfoSO`，文件命名严格为 `LevelSetInfo.asset`，关卡集名字填 `wellcooked_demo` 并将上述 LevelInfo 添加至关卡列表；
  4. 双击打开场景 `s_wellcooked_demo_1.unity`，选中场景根节点挂有 `PseudoPrefabManagerStub` 的物体，将其 `levelInfo` 字段关联为 `LevelInfo_wellcooked_demo_1`；
  5. 对照 `layout.tsv` 中的坐标与预制体定义，手动在场景中摆放各个工位台面、器具与 4 个厨师 spawn 点。
- **检验标准**：场景层级结构、配置引用及工位坐标与 `layout.tsv` 一致。

### 第 8 步：出包构建 AssetBundle
- **操作**：
  1. 点击顶部菜单 `Tools → Toggle Prepare For Building`，清理场景中的临时编辑物体；
  2. 按快捷键 `Ctrl + S` 保存当前场景；
  3. 核对 AssetBundle 标签：选中场景文件 `Assets/LevelSets/wellcooked_demo/scenes/s_wellcooked_demo_1.unity`，在 Inspector 底部确认其 AssetBundle 名字设为 `s_wellcooked_demo_1`；选中目录 `Assets/LevelSets/wellcooked_demo`，确认其 AssetBundle 名字设为 `info_wellcooked_demo`（也可点击顶部菜单 `Tools → WellCooked → 自检（bundle 名与文件）` 核对检测结果）；
  4. 点击顶部菜单 `Tools → Build AssetBundles` 执行打包。
- **检验标准**：打包顺利完成且无报错，在工程根目录的 `Assets/AssetBundles/` 目录下生成 `s_wellcooked_demo_1` 与 `info_wellcooked_demo` 两个文件。

### 第 9 步：安装进游戏
- **操作**：
  1. 打开游戏目录 `Overcooked! 2/BepInEx/plugins/OC2DIYLevel/levels/`；
  2. 新建子目录 `wellcooked_demo`；
  3. 将步骤 8 打包出的 `info_wellcooked_demo` 与 `s_wellcooked_demo_1` 两个文件复制进该目录；
  4. 启动游戏。
- **检验标准**：游戏主菜单中 OC2DIYLevel 加载器成功识别到 `WellCooked 演示关卡集`，可正常进入关卡场景。

### 第 10 步：将产物拷贝回仓库
- **操作**：
  1. 将构建生成的 `info_wellcooked_demo` 与 `s_wellcooked_demo_1` 两个 bundle 文件拷贝回本代码仓库的 `levels/wellcooked_demo/` 目录下；
  2. 连同更新的文档提交至 Git 当前开发分支。
- **检验标准**：`git status` 显示仓库 `levels/wellcooked_demo/` 下已包含两个二进制 bundle 文件。

---

## 5. 生成器脚本干了什么 / 怎么改布局

### 5.1 生成器脚本执行逻辑
生成器脚本为 `BuildWellCookedDemo.cs`，入口为菜单 `Tools → WellCooked → 生成关卡集与场景`，执行内容如下：
1. **建立目录**：创建 `Assets/LevelSets/wellcooked_demo/`、`scenes/`、`data/`、`data/wellcooked_demo_1/`。
2. **复制并修改关卡配置**：
   - 从 `Assets/Template/levelinfo_template.asset` 复制生成 `LevelInfo_wellcooked_demo_1.asset`；
   - 写入关卡标识 `wellcooked_demo_1`、显示名 `WellCooked 演示关卡`、场景名 `s_wellcooked_demo_1`；
   - 挂接 3 道菜谱：`Burrito_Chicken_SO`、`Sushi_PlainFish_SO`、`Sushi_PlainPrawn_SO`；
   - 从 `Assets/Template/config_1p.asset` .. `config_4p.asset` 复制生成 1 人至 4 人的配置资产，写入局时、订单生成间隔与订单存活时间。
3. **创建关卡集描述**：
   - 在 `Assets/LevelSets/wellcooked_demo/` 目录下创建 `LevelSetInfoSO`；
   - **该资产文件名必须为 `LevelSetInfo.asset`**（DIY 加载器 `OC2DIYLevel.dll` 严格通过该固定文件名加载关卡集）；
   - 写入集合标识 `wellcooked_demo`、显示名 `WellCooked 演示关卡集`，并将上述 LevelInfo 纳入关卡列表。
4. **生成场景**：
   - 复制模板场景 `Assets/Template/s_template.unity` 为 `Assets/LevelSets/wellcooked_demo/scenes/s_wellcooked_demo_1.unity`，并在编辑器中打开。
5. **按 layout.tsv 摆放台面与物体**：
   - 读取 `Assets/LevelSets/wellcooked_demo/layout.tsv` 数据行；
   - 根据预制体名称实例化台面或器具；
   - 将格坐标 `x` 与 `z` 乘以 0.6 换算为 Unity 世界坐标（`Vector3(x * 0.6f, 0, z * 0.6f)`）；
   - 关联预制体对应的数据资产（`PseudoPrefabSO`）；
   - 为料箱关联出料食材资产（`spawnerItemPrefabSO`）；
   - 为干净盘堆设置初始盘子数（`plateCount = 12`）；
   - 为烹饪锅具设置容纳量（`capacity = 4`）；
   - 为 4 个厨师在 `Design` 分组下创建初始出生点（`spawn`）。
6. **关联关卡信息**：
   - 检索场景根节点中的 `PseudoPrefabManagerStub` 组件，将其 `levelInfo` 字段关联为 `LevelInfo_wellcooked_demo_1.asset`。
7. **设置 AssetBundle 属性**：
   - 场景文件标记为 `s_wellcooked_demo_1`；
   - 关卡集目录标记为 `info_wellcooked_demo`。

### 5.2 layout.tsv 列定义说明
文件采用制表符（`\t`）分隔，固定 7 列：

| 列序 | 列名 | 取值与作用 | 示例 |
|---|---|---|---|
| 1 | `kind` | 工位种类：`dispenser`（料箱）、`chopping`（砧板）、`cooker`（灶台）、`frying`（炸台）、`plate_stack`（干净盘堆）、`serving`（上菜口）、`plate_return`（脏盘台）、`utensil`（独立器具）、`spawn`（厨师出生点） | `dispenser`、`chopping`、`spawn` |
| 2 | `prefab` | 预制体文件名。器具对应 `Assets/common01/prefabs/utensils/<prefab>.prefab`；台面对应 `Assets/common01/prefabs/counters/<prefab>.prefab` | `Dispenser`、`ChoppingCounter`、`Pot` |
| 3 | `so` | 数据定义文件名。器具对应 `Assets/common01/pseudo_prefab_so/utensils/<so>.asset`；台面对应 `Assets/common01/pseudo_prefab_so/counters/<so>.asset` | `Dispenser01SO`、`CookerSO`、`PotSO` |
| 4 | `ingredient` | 料箱食材名，对应 `Assets/common01/food/Ingredients/<ingredient>.asset`。非料箱类型留空 | `RiceSO`、`BurritoChickenSO`、`SushiFishSO` |
| 5 | `x` | 网格横坐标（0.6 米格，脚本计算世界坐标为 $x \times 0.6$） | `1`、`10`、`14` |
| 6 | `z` | 网格纵坐标（0.6 米格，脚本计算世界坐标为 $z \times 0.6$） | `2`、`7`、`11` |
| 7 | `note` | 说明文字（仅供阅读，脚本解析时忽略） | `玉米饼`、`砧板 1`、`厨师 1` |

### 5.3 布局调整方式
- **调整工位位置**：直接修改 `layout.tsv` 对应行的 `x` 或 `z` 数值。
- **替换工位种类或外观**：修改 `kind`、`prefab`、`so` 列。
- **调整出料食材**：修改或新增行，设置 `ingredient` 为目标 ScriptableObject 资产名。
- 修改保存后，在 Unity 菜单中重新点击 `Tools → WellCooked → 生成关卡集与场景` 即可自动重建场景。

### 5.4 脚本状态说明
`BuildWellCookedDemo.cs` **尚未在 Unity 编辑器中编译运行过**（Linux 机器无可用许可证）。初次在 Windows 编辑器中打开时可能需要根据 Unity 2017.4 的 API 提示修复少量类型或命名空间报错。脚本输出的全部日志均带有 `[wellcooked_demo]` 前缀，可在 Console 中直接过滤查看。

---

## 6. 产物往哪放

构建产物包含两个二进制 bundle 文件：`info_wellcooked_demo` 与 `s_wellcooked_demo_1`。

### 6.1 游戏目录部署位置
- **路径**：`Overcooked! 2/BepInEx/plugins/OC2DIYLevel/levels/wellcooked_demo/`
- **放置内容**：
  - `info_wellcooked_demo`
  - `s_wellcooked_demo_1`
- **层级关系说明**：
  - 加载器插件 `OC2DIYLevel.dll` 与公共资源包 `common01`、`common02` 等位于父级目录 `Overcooked! 2/BepInEx/plugins/OC2DIYLevel/`；
  - OC2DIYLevel v0.10 发行包自带 `test_level` 与 `oc1_story` 两个关卡集目录，结构与 `wellcooked_demo` 一致，可作为参考对照。

### 6.2 代码仓库保存位置
- **路径**：
  - `levels/wellcooked_demo/info_wellcooked_demo`
  - `levels/wellcooked_demo/s_wellcooked_demo_1`
- 随代码提交至 Git，供其他无需打开 Unity 的环境直接复制运行。

---

## 7. 验收（回到 Linux 侧跑）

产物放回 Linux 运行环境后，执行以下 5 项验收命令与判据验证：

### 7.1 启动并进入自制关卡
```sh
bash scripts/dev-cycle.sh 20 s_wellcooked_demo_1
```
- **判据**：游戏进程正常拉起，虚拟手柄自动完成前端选人并执行 `load=s_wellcooked_demo_1`，成功载入自制关卡场景，终端正常打印出 20 秒的位置与移动日志。

### 7.2 检查关卡机制
```sh
printf 'mech=1\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"
```
- **判据**：查看 `$GAME_DIR/BepInEx/LogOutput.log` 输出，Mechanisms 项明确输出为“机制 = 无（只有通用系统）”，场景内不存在活动机关或特殊地形。

### 7.3 检查空间连通性
```sh
bash scripts/eval-levels.sh s_wellcooked_demo_1
```
- **判据**：评估脚本重新启动游戏并采样 0.6 米网格，输出中可行区主连通块占比 $\ge 90\%$，确认整个厨房处于同一连通块内，工位未将通道堵塞。

### 7.4 检查自动调度出单
在 `$GAME_DIR/BepInEx/wellcooked-takeover.txt` 中开启工序自动模式：
```sh
printf 'work=auto\nslots=1,2,3,4\n' > "$GAME_DIR/BepInEx/wellcooked-takeover.txt"
```
- **判据**：工序状态机自动接管 4 个厨师完成取料、加工、烹饪、装盘与交付，单局运行中至少成功完成 1 单交付。

### 7.5 核对工位数与布局一致性
- **判据**：检查 `wellcooked-state.json` 或 `mech` 打印的工位列表，工位总数与种类必须与 `layout.tsv` 一致（共 20 个摆放项：7 个料箱、2 个砧板、1 个灶台、1 个炸台、1 个干净盘堆、1 个上菜口、1 个脏盘台、2 个独立锅具、4 个厨师初始出生点）。

---

## 8. 卡住时的排查

遇到问题时，按以下依据排查：

1. **Unity 生成器脚本报错**
   - **排查依据**：在 Unity Console 搜索过滤 `[wellcooked_demo]` 前缀，查看具体报错行。
   - **处理方法**：若报空引用（`NullReferenceException`）或加载不到资产，检查步骤 3 中 `Assets/StreamingAssets/Windows` 与 `Assembly-CSharp-Patch/` 是否完整覆盖，确认引用的 prefab 或 ScriptableObject 是否存在于对应目录。

2. **提示找不到 layout.tsv**
   - **排查依据**：Console 打印报错 `[wellcooked_demo] 找不到 layout.tsv：把它拷到 Assets/LevelSets/wellcooked_demo/layout.tsv`。
   - **处理方法**：确认 `layout.tsv` 文件已复制到工程目录 `Assets/LevelSets/wellcooked_demo/layout.tsv`，检查文件名拼写与大小写。

3. **进入游戏后工位不生效或厨师无法互动**
   - **排查依据**：在游戏中写入 `printf 'mech=1\n' > "$GAME_DIR/BepInEx/wellcooked-command.txt"`，核对日志输出的工位列表与坐标。
   - **处理方法**：
     - 若工位缺失，检查 `layout.tsv` 中对应行的 `kind`、`prefab`、`so` 拼写是否正确，确认场景预制体是否挂载有 `PseudoPrefabStub` 组件；
     - 若厨师卡住无法走向工位，检查台面坐标是否占用了通道。当前台面布置在 $x=1..14$、$z=2..11$，中间 $z=3..10$ 全空，通道宽度大于 1 格（厨师半径 0.40 米）。

4. **游戏未识别自制关卡或菜单卡死**
   - **排查依据**：查看游戏目录下的 `Overcooked! 2/BepInEx/LogOutput.log`，搜索 `OC2DIYLevel` 相关日志。
   - **处理方法**：
     - 确认关卡包放置在 `Overcooked! 2/BepInEx/plugins/OC2DIYLevel/levels/wellcooked_demo/`；
     - 确认关卡集 bundle 文件名为 `info_wellcooked_demo`，场景 bundle 文件名为 `s_wellcooked_demo_1`；
     - 确认关卡集内部 ScriptableObject 资产名称严格为 `LevelSetInfo`（若重命名会导致加载器通过固定名检索失败）。
