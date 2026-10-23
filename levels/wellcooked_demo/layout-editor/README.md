# wellcooked_demo 布局编辑器

单文件前端 + Python 标准库 HTTP 服务。读写 `levels/wellcooked_demo/layout_*.tsv`，并提供游戏侧“处理时间”覆盖接口。

## 运行

```bash
python server.py
# 打开 http://127.0.0.1:8770/
```

依赖 Python 3.8+（只用标准库）。

## 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `WELLCOOKED_EDITOR_PORT` | `8770` | HTTP 端口 |
| `WELLCOOKED_GAME_DIR` | 自动探测 Steam 路径 | 游戏安装目录；处理时间 JSON 写到它的 `BepInEx/wellcooked-processing.json` |
| `WELLCOOKED_BUILD_PS1` | 空 | Windows 侧 `build-only.ps1` 的路径；有 `powershell`/`pwsh` 时先构建再部署 |
| `WELLCOOKED_BUILD_CMD` | 空 | 自定义构建命令（shell）；Linux 侧接远端构建机时用。成功后会再部署仓库里的 5 个 bundle |
| `WELLCOOKED_STATUS_FILE` | `layout-editor/build-status.txt` | 构建/部署状态文件路径 |

Windows 本机：

```powershell
$env:WELLCOOKED_BUILD_PS1 = "D:\oc2-level-build\build-only.ps1"
$env:WELLCOOKED_STATUS_FILE = "D:\oc2-level-build\build-status.txt"
$env:WELLCOOKED_GAME_DIR = "D:\2026-2027秋季学期\操作系统课设\wellcooked2\Overcooked 2!"
python server.py
```

Linux（本机没有 Unity 构建能力，默认只部署仓库里已有的 bundle）：

```bash
WELLCOOKED_GAME_DIR="/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2" python server.py
```

“构建并部署”按钮的行为：

1. 设置了 `WELLCOOKED_BUILD_CMD`：先跑它；退出码 0 后再部署。
2. 否则设置了 `WELLCOOKED_BUILD_PS1` 且本机有 `powershell`/`pwsh`：先构建再部署。
3. 否则：**只部署** `levels/wellcooked_demo/` 里现有的 5 个 bundle，并在状态里说明没有重建。

`WELLCOOKED_BUILD_CMD` 的契约：命令退出码为 0 时，`levels/wellcooked_demo/` 里的
`info_wellcooked_demo` 与 `s_wellcooked_demo_1..4` 必须已经是新产物；服务随后调用
`deploy.sh` 把它们拷进游戏目录。例如远端构建机：

```bash
WELLCOOKED_BUILD_CMD='ssh win "powershell -NoProfile -File D:/oc2-level-build/build-only.ps1"' \
WELLCOOKED_GAME_DIR="/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2" \
python server.py
```

Linux 不能生成 Unity AssetBundle（Unity 2017.4 的 Windows 构建模块和许可证都不在这台机器上）。
布局 TSV 比 bundle 新时，部署仍会执行，但状态里会警告“部署的是旧 bundle”；要让布局改动生效，
必须在 Windows 侧重新构建，或把 `WELLCOOKED_BUILD_CMD` 指向构建机。

也可以手动部署（不经过编辑器）：

```bash
bash levels/wellcooked_demo/deploy.sh
# 或指定目录: bash levels/wellcooked_demo/deploy.sh --game-dir "/path/to/Overcooked! 2"
```

部署完成后重启游戏，OC2DIYLevel 才会重新扫描关卡集。

## 功能

- 0.6 m 网格，工位按锚点格**中心对齐**，与生成器 `BuildWellCookedDemo.cs` 的占位模型一致。
- 放置/旋转后实时检查重叠：冲突物体红色边框，底部校验区列出 `(x,z) 的X与 (x,z) 的Y 占地重叠`。
- 放置后不选中；物体列表按类别分组（厨师出生点 / 台面 / 烹饪工位 / 料箱 / 盘子·洗涤 / 出餐口 / 锅具·工具）。
- 画布橡皮或物体列表右键 / Delete 可删除任何物体（包括 spawn）。
- “处理时间”面板写 `wellcooked-processing.json`：煮/炸时间 1–30s、切菜阶段 2–8、单人每阶段刀数 1–10；插件 `ProcessingTuner` 读取后用运行时覆盖原 prefab，不重建关卡。
- 保存时在 TSV 旁生成 `.bak` 备份（已在仓库 `.gitignore` 中忽略）。

## 与生成器的约定

- 工位 `(x,z)` 占 `w x h` 格：`x0=x-w/2, x1=x+w/2, z0=z-h/2, z1=z+h/2`。
- 水槽/出餐口 `w/h` 为 4×2；出餐口朝向永远由生成器 `ServingYaw()` 决定（箭头朝屋外），其他工位可用 `dir` 覆盖。
- 盘堆（`plate_stack`）自带底座；放在已有台面上时会直接挂到台面，不再生成第二个 `counter_base`。
