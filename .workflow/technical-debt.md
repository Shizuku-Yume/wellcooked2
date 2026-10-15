# Technical Debt

Code that makes the next change expensive, one entry per finding, so reviews don't
rediscover it and the reason it wasn't fixed yet is kept. `flow-architect` adds
entries; any skill that finds debt may add one.

## Format

Each entry is a level-2 heading naming the concrete symptom, under `## Entries`.
Refer to an entry by its heading.

```markdown
## Order total computed in three places

- **Where:** <files or modules>
- **Added:** <YYYY-MM-DD>
- **Priority:** fix-now | worth-doing | only-if-grows
- **Problem:** <what is wrong, with evidence>
- **Impact:** <what gets harder while it stays>
- **Solution:** <the change, in plain terms>
- **Cost:** <what the fix risks, how big it is>
```

An entry leaves this file in one of two ways:

- Fixed: the task that fixes it deletes the entry in its own commit.
- Declined by the user: delete the entry and append a decision entry to
  `decisions.md` (Decided: not fixing <symptom>; Instead of: fixing it; Because;
  Mine: no; Revisit when), so reviews don't re-propose it.

## Entries

## 插件 server 模式默认端口与文档的 14455 不一致

- **Where:** `plugin/WellCooked.Plugin/Plugin.cs`（`Network.Port` 默认 12345）与
  `Net/TcpLink.cs` 的 `AcquireServer`；文档侧 `.workflow/specs/bridge.md` 的 W7 行与 §6.2.4
  对照行、`decisions.md: 2026-10-15 - 实机验收不要求改游戏侧配置`
- **Added:** 2026-10-15
- **Priority:** worth-doing
- **Problem:** 课程文档 §6.2.4 与 spec 都写"插件在 localhost:14455 起 TCP 服务器"，而插件两种
  模式共用 `Network.Port`（默认 12345），只有 Bridge 的 `--link connect` 默认指向 14455。
  也就是说"Bridge 主动连插件"这个拓扑在默认配置下连不上真插件 —— W7 只被
  `tools/fake_plugin.py` 覆盖过。`decisions.md: …实机验收不要求改游戏侧配置` 里"插件的
  server 模式（14455）"这句与代码不符。
- **Impact:** 谁按文档去跑相反拓扑，都会先撞一次"连不上"，然后才发现要改插件配置；答辩被问
  "两个方向都验过吗"，答案是"其中一个只验过模拟对端"。
- **Solution:** 给插件加 `Network.ServerPort`（默认 14455），server 模式用它、client 模式继续用
  `Network.Port`；或者反过来把 `--link connect` 的默认端口改成 12345 并在文档里说明理由。
- **Cost:** 插件在本机无法编译（缺 BepInEx 与游戏程序集），改完只能靠读代码 + 追一次实机；
  前一种改法还要同步 `specs/bridge.md`、`protocol/README.md` 与那条决策的端口说明。

