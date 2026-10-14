# 07: 进入关卡后自动接管并驱动厨师

**Effort:** game-bridge
**Task type:** feature
**Base commit:** a897c08adf298fc3b0a8f27690e258e00ea18635
**Started:** 2026-10-14T15:26:44+08:00
**Status:** active

**Delivers:** 用户在菜单里自己导航并进入关卡，插件自动接管：进关卡前把空着的玩家位用虚拟手柄占上（所以关卡里有两名厨师），进关卡后立刻驱动我们那几名厨师移动、周期按交互键——人看着屏幕就能看到小人自己动起来。

**Blocked by:** None
**Files:** `plugin/`（`Takeover.cs`、`VirtualPad.cs`、csproj 引用）、`scripts/`
**Read first:** spec「What it does」「How it is built」「游戏内部接口的已知起点」；spike `game-bridge/06` 的 Findings（设备层的坑）；方案 §9 阶段A3/A4
**Covers:** W5

**Check:** `bash scripts/run-game.sh` 起游戏，手动导航进入任意一张地图的关卡 → `BepInEx/LogOutput.log` 出现接管行与每名厨师的驱动行，画面里被接管的厨师自己来回移动并周期按交互键（两帧截图位置不同）。

- [ ] 进关卡前把空玩家位用虚拟手柄占上，人数可配置（默认补到 2）
- [ ] 关卡开始的判定不误触发：前端/菜单里不驱动，只在出现本地厨师控制器后开始
- [ ] 第一条策略写死在代码里并注明"这是最小策略，调度算法由后续任务接"：每名被接管的厨师沿一条直线来回走 + 周期按一次交互键
- [ ] 日志有接管时刻、每个厨师的玩家号与手柄号、开始/停止驱动
- [ ] 与用户的物理手柄共存：不碰用户占着的那一位
- [ ] 离开关卡回到前端时停止驱动，不残留按键状态

**Progress:**
Dirty at start: none
