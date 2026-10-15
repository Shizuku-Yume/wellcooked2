# 02: 协议落地与 Bridge 骨架

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>
**Finished:** 2026-10-15

**Delivers:** 一个能跑的 Linux 侧 Bridge：监听 `127.0.0.1:14455`，按 JSON 行解析 `state_update`、`action`、`heartbeat`、`config`，写 `bridge.log`，回 `heartbeat`。用脚本假客户端就能验证，不依赖游戏。

**Blocked by:** None
**Files:** `protocol/protocol.md`、`bridge/`（C11，Makefile）、`tests/`、`scripts/run-bridge.sh`

**Read first:** spec「How it is built」；方案 §5.2、§5.3、§6.1 Bridge 职责；`decisions.md: 2026-10-14 - TCP 连接由 Bridge 监听，游戏插件作为客户端`

**Check:** `make -C bridge && make -C tests test && bash scripts/run-bridge.sh &` 然后用 `python3 tests/fake_client.py` 发一条 `state_update` 和一条 `heartbeat` → `bridge.log` 里出现解析后的得分与厨师数，假客户端收到 `heartbeat` 回复；`make -C tests test` 全部通过。

- [x] `protocol/protocol.md` 写清四种消息的字段、单位、必填项和示例，与方案 §5.2 一致
      （`§3.1 state_update`、`§3.2 heartbeat`、`§3.6 action`、`§3.7 config`；另含 `hello`/`echo`/`ack`/`state_request`）
- [x] Bridge 用 C11 + POSIX socket 实现，一行一条 JSON，`\n` 分帧，UTF-8
      （`bridge/src/net.c`、`framing.c`；**标准是 `-std=c99` 不是 C11**，见 Progress 的偏差记录）
- [x] 解析失败的行走日志并丢弃，不断连接（方案 §5.3）
      （`bridge/tests/test_protocol.c:564 test_parse_errors`、`test_framing.c:428 test_bad_line_does_not_shift_the_next_one`；场景 `bad-frames` PASS）
- [x] 心跳：5 秒收不到就判定断开，记日志并回到监听状态
      （`bridge/src/bridge.c:73 DEFAULT_HEARTBEAT_TIMEOUT_MS 5000`；场景 `no-heartbeat` PASS）
- [x] 解析层有单元测试：正常样本、缺字段、超长行、非法 UTF-8
      （`test_protocol.c:159/564/738`、`test_framing.c:212/272`、`test_json.c:254`（`\ud800` 等 → `JSON_ERR_UTF8`）；5 套件 0 失败）
- [x] `bridge.log` 每行带时间戳和消息类型，方便联调时人工比对
      （`2026-10-15T22:34:45.713 INFO  bridge_start ...`、`... link_up ...`，事件名即消息类型）

**Progress:**

归档方式：**本工作树没有实现这个任务**（用户 2026-10-14 说明由组员在另一个工作树做），
交付物由已关闭的 `bridge` effort 产出并已落在 main 上（`done/bridge/01-protocol-and-json.md`、
`03-bridge-process.md`、`04-fake-plugin.md`、`06-link-hardening-and-conformance.md`；经 PR #11 合入）。
按 CONVENTIONS §6「archived in HEAD is finished」，在 HEAD 上核对 Check 后归档，`03/04` 随之就绪。

核对证据（2026-10-15，main `07bb031`，工作树干净）：

| 命令 | 结果 |
|---|---|
| `cd bridge && make` | `built: test_framing test_json test_net test_protocol test_shm kitchenos-bridge kitchenos-worker-stub` |
| `cd bridge && make test` | `5 test suites, 0 failed` |
| `bash tools/run-bridge-scenarios.sh` | `合计: 10 个场景, 通过 10, 失败 0, 跳过 0`（happy / one-slot / no-heartbeat / bad-frames / connect-mode / reconnect-storm / silent-connect / data-shapes / second-peer / pickup-dash） |
| `grep -n 'DEFAULT_LISTEN_PORT\|DEFAULT_HOST' bridge/src/bridge.c` | `127.0.0.1` + `14455`（两种链路模式同号，`decisions.md: 2026-10-15 - 端口按组员决策统一成 14455`） |

**Check 原文里的命令名已不存在**：`scripts/run-bridge.sh` 与 `tests/fake_client.py` 被
`tools/run-bridge-scenarios.sh` + `tools/fake_plugin.py` 取代（bridge effort 的 04/06 把"一个假客户端"
扩成"一套可批量判定的场景"）。判据不变：假客户端发 `state_update`/`heartbeat`，Bridge 解析后写
`logs/bridge-*.log` 并回 `heartbeat` —— 由 `happy` 场景覆盖（该场景断言收到 `ack` 与心跳）。

**偏差记录**：`bridge/Makefile:12` 用 `-std=c99`，而 `decisions.md: 2026-10-14 - OS 核心用 C11`
（用户拍板）说 C11。功能上无差别（代码没用到 C11 独有特性），但两处说法不一致；勾上面那条
checklist 时以"POSIX socket + JSON 行 + `\n` 分帧 + UTF-8"为准，标准名本身仍是待对齐项。

**未验证到的**：本归档没有跑真实游戏侧（那属于 03/04/05 的验收面，见 `done/bridge/05-real-game.md`
的 W8 记录）。
