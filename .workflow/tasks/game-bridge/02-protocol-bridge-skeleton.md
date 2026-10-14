# 02: 协议落地与 Bridge 骨架

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>

**Delivers:** 一个能跑的 Linux 侧 Bridge：监听 `127.0.0.1:14455`，按 JSON 行解析 `state_update`、`action`、`heartbeat`、`config`，写 `bridge.log`，回 `heartbeat`。用脚本假客户端就能验证，不依赖游戏。

**Blocked by:** None
**Files:** `protocol/protocol.md`、`bridge/`（C11，Makefile）、`tests/`、`scripts/run-bridge.sh`

**Read first:** spec「How it is built」；方案 §5.2、§5.3、§6.1 Bridge 职责；`decisions.md: 2026-10-14 - TCP 连接由 Bridge 监听，游戏插件作为客户端`

**Check:** `make -C bridge && make -C tests test && bash scripts/run-bridge.sh &` 然后用 `python3 tests/fake_client.py` 发一条 `state_update` 和一条 `heartbeat` → `bridge.log` 里出现解析后的得分与厨师数，假客户端收到 `heartbeat` 回复；`make -C tests test` 全部通过。

- [ ] `protocol/protocol.md` 写清四种消息的字段、单位、必填项和示例，与方案 §5.2 一致
- [ ] Bridge 用 C11 + POSIX socket 实现，一行一条 JSON，`\n` 分帧，UTF-8
- [ ] 解析失败的行走日志并丢弃，不断连接（方案 §5.3）
- [ ] 心跳：5 秒收不到就判定断开，记日志并回到监听状态
- [ ] 解析层有单元测试：正常样本、缺字段、超长行、非法 UTF-8
- [ ] `bridge.log` 每行带时间戳和消息类型，方便联调时人工比对

**Progress:**
