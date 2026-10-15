# 03: 插件连上 Bridge 并上报世界状态

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>

**Delivers:** 游戏跑起来后，插件自己连上 Bridge 并每帧上报世界状态；关掉 Bridge 游戏不崩、插件持续重连，Bridge 重启后连接自动恢复。Bridge 侧能看到与画面一致的得分和厨师数。

**Blocked by:** 01, 02
**Files:** `plugin/`（TCP 客户端、状态读取、Harmony patch）、`bridge/`（如需按实测修正解析）、`scripts/run-game.sh`

**Read first:** spec「What it does」「How it is built」「How we will know it works」W3、W4、W7；方案 §9 阶段A2、§5.2 `state_update` 字段；`protocol/protocol.md`
**Covers:** W3, W4, W7

**Note:** 链路层落到哪一份插件已定（`decisions.md: 2026-10-16 - 两个插件工程合并到 net35 那份`）：
本任务在 `plugin/*.cs`（net35，演示机那份）上做，`plugin/WellCooked.Plugin/Net/TcpLink.cs` 与
`Protocol/Messages.cs` 是搬运来源，那份之后只留作参考实现。合并本身的改动记在 `technical-debt.md`
的「两个插件工程并存」条目里，随本任务一起删掉那条债务。

**Check:** 启动 Bridge，再启动游戏进一局：`bridge.log` 5 秒内出现 `client connected`，随后持续收到 `state_update`，其中 `score` 与画面一致、`chefs` 数量与场上厨师数一致（人工比对一次）；`kill` 掉 Bridge，游戏继续运行且插件日志出现重连行，重启 Bridge 后 5 秒内重新连上。

- [ ] 插件用后台线程跑 TCP 客户端，不阻塞游戏主线程（帧率不掉到不可玩）
- [ ] 连接失败每 2 秒重试，日志打印重试次数；对端断开后同样重连
- [ ] 状态读取覆盖：厨师位置与朝向、手持物、订单列表与剩余时间、锅与砧板状态、盘子数量、当前得分
- [ ] 上报频率可配置，默认每帧不超过一次，单条消息小于 4 KB
- [ ] 游戏内读到的对象/字段记进 `plugin/README.md`（类型名、字段名、取值含义），供后面写调度用
- [ ] 关掉 Bridge 期间游戏不卡顿、不崩溃

**Progress:**
