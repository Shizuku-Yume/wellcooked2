# 课堂汇报新版

最终交付：

- `WellCooked_课堂小组汇报_v2.pptx`（12 页，由 `wellcooked_v2/build_deck.py` 生成）

重新生成：

```bash
cd represent
python3 wellcooked_v2/build_deck.py
```

截图检查注意：`officecli view ... screenshot` 会按文件路径缓存渲染结果。重建后先把 PPTX 复制到新路径再截图，否则可能看到旧内容。

v2 调整：

- 第 3 页注明这是应用层调度模型，不替换 Linux 内核调度器。
- 架构、IPC 与信号、同步互斥、两级调度、订单时序、开源借鉴、难点与计划各页重排，消除文字溢出和卡片重叠。
- 补充双实验室部署说明：Fedora + Proton / WSL2 + Windows，同一份 POSIX 代码。

历史版本：

- `基于胡闹厨房2的厨房调度策略设计_课堂小组汇报.pptx` 为上一版（只读保留），不作为本次汇报文件。
