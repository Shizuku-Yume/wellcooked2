# 05: 实机验证：与真实插件跑通

**Effort:** bridge
**Blocked by:** 03, 04
**Task type:** feature
**Covers:** W8
**Delivers:** 一次真实游戏的端到端验证记录，以及为跑通它所需的最小改动。
**Files:** `plugin/tools/`（如需要），`.workflow/tasks/bridge/05-real-game.md` 的 `## Progress`
**Read first:** `.workflow/specs/bridge.md` 的 W8, `plugin/WellCooked.Plugin/Net/TcpLink.cs`,
`plugin/WellCooked.Plugin/Plugin.cs` 的配置项。
**Done when:**
- [ ] 插件成功加载（`BepInEx\LogOutput.log` 有 `[Plugin] WellCooked 0.1.0 loaded`）
- [ ] Bridge 日志出现插件的 `hello`，含 action 能力清单
- [ ] Bridge 日志出现连续 `state_update`，且共享内存里的厨师/订单/资源非空
- [ ] 记录两个实验室各自需要的网络参数（localhost 是否够用，否则写 WSL2 IP）

**Check:** Bridge 与游戏同时运行 30 秒后，`logs/bridge.log` 里有 ≥1 条 `hello` 与 ≥10 条
`state_update` 且无解析错误。**这是全 effort 唯一需要启动游戏的一步。**
