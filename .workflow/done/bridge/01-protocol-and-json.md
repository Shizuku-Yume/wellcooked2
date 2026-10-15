# 01: 跨端协议契约与 C 侧 JSON/分帧/消息编解码

**Effort:** bridge
**Blocked by:** None
**Task type:** feature
**Covers:** W1
**Delivers:** `protocol/` 下的协议说明与 JSON Schema；`bridge/` 的 `json`、`framing`、`protocol`
三个模块及其单元测试；`bridge/Makefile`。
**Files:** `protocol/protocol.md`, `protocol/schema/*.json`, `bridge/include/kitchenos/{json,framing,protocol}.h`,
`bridge/src/{json,framing,protocol}.c`, `bridge/tests/test_{json,framing,protocol}.c`, `bridge/Makefile`
**Read first:** `.workflow/specs/bridge.md`（How it is built 的表与数据形状）,
`plugin/WellCooked.Plugin/Net/TcpLink.cs`（分帧的唯一基准）,
`plugin/WellCooked.Plugin/Protocol/Messages.cs`（消息类型名）,
`overcooked_os_course_project.md` §5.2 的四个 JSON 样例。
**Done when:**
- [x] 分帧与 `TcpLink.cs` 完全一致：4 字节大端长度前缀 + UTF-8，单帧上限 1048576 字节
- [x] 消息类型名与字段名与 §5.2 及插件实现逐字段对齐
- [x] `cd bridge && make test` 全绿，覆盖粘包、拆包、零长帧、超长帧、非法 JSON、转义
- [x] `protocol/schema/` 至少覆盖 state_update / action / heartbeat / config / ack / hello / echo

**Check:** `cd bridge && make test` 退出码 0，且测试输出里 json/framing/protocol 三组均无 FAIL。

## Progress

**状态：完成。**

三组单测在最终树上全绿（`make test` 退出码 0，5 个套件 0 失败）：

```
[PASS] build/test_framing     151 checks, 0 failures
[PASS] build/test_json        166 checks, 0 failures
[PASS] build/test_protocol    210 checks, 0 failures
=== 5 test suites, 0 failed ===
```

Check 里"退出码 0"这一条在 02 的 `test_shm` 修好之前无法整体成立 —— 那是共享的
`make test` 判据，卡在别的任务的抖动用例上，与本任务无关。02 修完后已实测退出码 0。

除最低要求外还做了：

- ASan + UBSan 下重编三个 suite 全绿、无 sanitizer 报告；ASan 借此抓到测试自身的一个真错
  （`test_framing.c` 里声明 8 字节数组却传 `out_cap=9`），已修。
- 解析器 fuzz：30 万条随机/变异输入（含样例截断与字节翻转），ASan 下 0 崩溃、0 泄漏。
- Schema 正确性：8 个 schema 均为合法 JSON，且用 `jsonschema` 对 8 条真实消息样本逐条验证通过；
  另验证两个边界用例 —— `action` 的 idle 形式（`stop` 不带 `target`）、`state_update` 的
  `resources` 动态键（`pot_7`、未知键）都必须被接受，否则 Bridge 会拒掉插件上报的合法状态。
- Makefile 前瞻性：把 bridge 拷到 `/tmp` 模拟后续落地 `net.c`、`bridge.c`、`test_net.c`，
  **不改 Makefile** 即 `make && make test` 成功。

**偏离与说明：**

- 构造器缓冲区不足时返回 `JSON_ERR_NOSPACE`（`json.h` 的错误码）而不是 `PROTO_ERR_*`，
  已在 `protocol.h` 注释说明。
- 未给 `state_request` 建 schema：只在 `protocol/protocol.md` 里保证类型名。任务要求的最低
  7 个 + `echo_reply` 已覆盖。
- 数组上限（chefs 16 / orders 32 / ingredients 8 / resources 48）是本模块的固定布局选择，
  超限按"保留前 N 条 + `proto_diag` 计数"处理，不整条失败；`protocol/protocol.md` §5 与 schema
  的 `maxItems` 一致。

**后续修正（integrator 记录）：** `bridge/include/kitchenos/framing.h` 第 52 行的注释
`*payload/*len` 含嵌套 `/*`，触发 `-Werror=comment`，会让所有依赖它的 agent 编译失败；
措辞已改为 "payload and len describe one frame"。

**后续修正（2026-10-15，integrator 记录）：** 上面那条分帧判据（"与 `TcpLink.cs` 完全一致：
4 字节大端长度前缀 + UTF-8，单帧上限 1048576 字节"）与 "Read first" 里"`TcpLink.cs` 是分帧的
唯一基准"都已被 decisions.md: 2026-10-15 - 分帧改成一行一条 JSON 取代：线格式是一行一条
UTF-8 JSON、`\n` 结尾，单行上限仍是 1 MiB（不含 `\n`），契约以 `protocol/protocol.md` §1 为准
（队友的 `.workflow/specs/game-bridge.md` 原文就是这么写的，`TcpLink.cs` 只是其中一个实现）。
本任务交付的 `json` 与 `protocol` 两个模块、全部消息字段与 schema 都没受影响；`framing` 模块
删掉了 `FRAMING_HEADER_LEN` 与 `framing_peek_length()`，换成
`FRAMING_TERMINATOR`/`FRAMING_TERMINATOR_LEN`/`FRAMING_MAX_FRAME`，`bridge/tests/test_framing.c`
重写为 194 项检查（含上限边界、零长行、1 MiB 往返与"坏行不会挪动下一行"）。
