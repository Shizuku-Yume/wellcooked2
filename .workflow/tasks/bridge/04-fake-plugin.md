# 04: 无游戏联调工具 fake_plugin.py

**Effort:** bridge
**Blocked by:** 01
**Task type:** feature
**Covers:** W2, W3, W4, W5, W7
**Delivers:** `tools/fake_plugin.py`：协议级模拟插件，能在不启动游戏的前提下驱动 Bridge 走完
全部场景；每个场景以退出码表达结论，可被脚本批量调用。
**Files:** `tools/fake_plugin.py`, `tools/run-bridge-scenarios.sh`
**Read first:** `.workflow/specs/bridge.md`（How we will know it works）,
`protocol/`（01 的产出，分帧与字段名）。
**Done when:**
- [ ] `--scenario happy`：连上、发 `hello`、60 Hz 发 `state_update`、收动作并回 `ack`
- [ ] `--scenario one-slot`：只写 1/N 个输入槽，验证 Bridge 对缺失厨师下发 idle
- [ ] `--scenario no-heartbeat`：停发心跳，验证 Bridge 判定断开；随后重连恢复
- [ ] `--scenario bad-frames`：发 >1 MiB 帧、非法 JSON、截断帧各一次，验证 Bridge 存活
- [ ] `--scenario connect-mode`：反转拓扑，自己监听 14455 等 Bridge 连进来
- [ ] 每个场景自己判定并通过/失败退出码报告，不需要人看日志下结论
- [ ] 退出码：0 通过；非 0 表示该场景断言失败，并在 stderr 说明失败点

**Check:** `cd core && make && bash ../tools/run-bridge-scenarios.sh` 全部场景退出码 0。
