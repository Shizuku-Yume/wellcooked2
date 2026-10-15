# 05: 两名虚拟玩家加入同一局并完成订单

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>

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
