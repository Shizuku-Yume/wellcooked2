# 05: 两名虚拟玩家加入同一局并完成订单

**Effort:** game-bridge
**Task type:** feature
**Base commit:** 24a18a7
**Base branch:** game-bridge/04-action-injection-single-chef
**Started:** 2026-10-16T15:10:00+08:00
**Status:** active

**Delivers:** 两名虚拟玩家进入同一张地图的本地合作局，由外部固定动作序列驱动，稳定完成 3 个订单并正常得分——这是阶段A 的最终验收条件，也是后面 Scheduler 接管决策的接口面。

**Blocked by:** 04
**Files:** `plugin/`（多角色输入注入、菜单加入、帧同步）、`bridge/`（多厨师动作下发）、`scripts/demo-multi-chef.sh`

**Read first:** spec「What it does」「How we will know it works」W6、开放问题「第一版固定地图」；方案 §9 阶段A3/A4、§6.1 插件职责
**Covers:** W6

**Check:** 跑 `bash scripts/demo-multi-chef.sh`，脚本启动游戏和 Bridge 后按固定序列驱动两名厨师完成 3 个订单 → 游戏结束或脚本收尾时 `state_update` 里的 `score` 大于 0、完成订单数等于 3；同一序列连跑两次结果一致。

- [ ] 菜单阶段能让两个虚拟玩家加入本地合作局，不需要人拿手柄按键
- [ ] 两名厨师的输入互不串扰，同一个 `action` 只影响指定的 `chef_id`
- [ ] 帧同步：输入不丢、不抖，连续 60 秒上报的厨师位置没有跳变
- [ ] 固定动作序列写成脚本可重复执行，不依赖人工操作
- [ ] 地图按 spec 开放问题「第一版固定地图」定下的关卡执行；定不下来就先把这个开放问题定掉，再跑验收
- [ ] 演示步骤（启动、跑一局、看日志）写进 `README.md`，答辩时照着做

**Progress:**

## Progress

Dirty at start: none（从 `24a18a7` 起，叠在 `game-bridge/04-action-injection-single-chef` 上）。

**场地**：spec 开放问题"第一版固定地图"已经定了（默认 Story 1-1 `s_sushi_1_1`，见 `specs/game-bridge.md`
开放问题 1）。自制演示关卡（任务 13）正在由组员的 Windows 机器构建，本任务的验收先在 `s_sushi_1_1` 上跑；
自制关卡到货后把这套脚本挪过去再跑一遍。

**接手要点（来自任务 04 的实测，直接影响本任务）**：

1. **真 Worker 必须按游戏帧发布输入槽**。桥每帧要求"这一格比上次更新"，Worker 的发布节拍是它自己的 ——
   实测 120Hz 的替身对 117fps 的游戏会有一批帧落在两次发布之间，那些帧现在靠 `--input-grace-ms`（默认
   250ms）沿用最后一版意图兜着。真 Worker 按游戏帧发布（一帧一版）才能不依赖这个宽限窗口。
2. `chef_id` = 输入槽（0 基），实测 `(int)PlayerIDProvider.GetID()` 也是 0 基，两者同一个数。
3. 动作全集已经能由外部指令触发（任务 04）：`move`/`stop`/`interact`/`pickup`/`dash`，`move` 到位会回
   `ack ok=true detail=到达…`、超时回 `ok=false`；未实现的动作名当场 `ok=false`。
4. 窗口失焦时游戏不处理输入 —— 跑真机前先 `xdotool search --name Overcooked2 windowactivate --sync`。
