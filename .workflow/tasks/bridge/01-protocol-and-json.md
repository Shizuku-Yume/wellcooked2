# 01: 跨端协议契约与 C 侧 JSON/分帧/消息编解码

**Effort:** bridge
**Blocked by:** None
**Task type:** feature
**Covers:** W1
**Delivers:** `protocol/` 下的协议说明与 JSON Schema；`core/` 的 `json`、`framing`、`protocol`
三个模块及其单元测试；`core/Makefile`。
**Files:** `protocol/README.md`, `protocol/schema/*.json`, `core/include/kitchenos/{json,framing,protocol}.h`,
`core/src/{json,framing,protocol}.c`, `core/tests/test_{json,framing,protocol}.c`, `core/Makefile`
**Read first:** `.workflow/specs/bridge.md`（How it is built 的表与数据形状）,
`plugin/WellCooked.Plugin/Net/TcpLink.cs`（分帧的唯一基准）,
`plugin/WellCooked.Plugin/Protocol/Messages.cs`（消息类型名）,
`overcooked_os_course_project.md` §5.2 的四个 JSON 样例。
**Done when:**
- [ ] 分帧与 `TcpLink.cs` 完全一致：4 字节大端长度前缀 + UTF-8，单帧上限 1048576 字节
- [ ] 消息类型名与字段名与 §5.2 及插件实现逐字段对齐
- [ ] `cd core && make test` 全绿，覆盖粘包、拆包、零长帧、超长帧、非法 JSON、转义
- [ ] `protocol/schema/` 至少覆盖 state_update / action / heartbeat / config / ack / hello / echo

**Check:** `cd core && make test` 退出码 0，且测试输出里 json/framing/protocol 三组均无 FAIL。
