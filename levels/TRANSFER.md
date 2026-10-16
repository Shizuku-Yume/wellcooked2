# 交给 Windows 侧的三样东西

在 Windows 机器上按 `levels/wellcooked_demo/README.md` 构建时需要：

| 东西 | 从哪来 | 大小 |
|---|---|---|
| `Assets/StreamingAssets/Windows` | 游戏目录 `Overcooked2_Data/StreamingAssets/Windows`（本机：`/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2/Overcooked2_Data/StreamingAssets/Windows`） | 7.7 GB |
| `Assets/Scripts/Assembly-CSharp` | AssetRipper 1.3.14 导出游戏后的 `ExportedProject/Assets/Scripts/Assembly-CSharp`（本机已导出在 `~/oc2-export/ExportedProject/...`，18 MB） | 18 MB |
| `Assembly-CSharp-Patch/` 的内容 | 编辑器工程自带，覆盖进上面那个 `Assembly-CSharp/` | 44 个文件 |

大件只有 StreamingAssets；`Assembly-CSharp` 的压缩包放在 `~/oc2-transfer/`。
