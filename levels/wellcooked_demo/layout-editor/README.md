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
| `WELLCOOKED_GAME_DIR` | `<repo>/Overcooked 2!` | 游戏安装目录；处理时间 JSON 写到它的 `BepInEx\wellcooked-processing.json` |
| `WELLCOOKED_BUILD_PS1` | 空 | `build-only.ps1` 的路径；不设置时“构建并部署”按钮会报“未配置” |
| `WELLCOOKED_STATUS_FILE` | `layout-editor/build-status.txt` | 构建状态文件路径 |

例如本机：

```powershell
$env:WELLCOOKED_BUILD_PS1 = "D:\oc2-level-build\build-only.ps1"
$env:WELLCOOKED_STATUS_FILE = "D:\oc2-level-build\build-status.txt"
$env:WELLCOOKED_GAME_DIR = "D:\2026-2027秋季学期\操作系统课设\wellcooked2\Overcooked 2!"
python server.py
```

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
