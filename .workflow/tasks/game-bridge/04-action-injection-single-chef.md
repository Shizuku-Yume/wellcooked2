# 04: 通过 Bridge 指令控制单个厨师

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>

**Delivers:** Bridge 下发一条 `action`，游戏里 0 号厨师真的动起来：走到指定坐标、抓取食材、放到砧板上切完。动作全集（移动、交互、抓取/放下、投掷）都能由外部指令触发，并能在上报的状态里看到结果。

**Blocked by:** 03
**Files:** `plugin/`（输入注入层、action 分发）、`bridge/`（`action` 下发）、`protocol/protocol.md`（按实测修正字段）、`scripts/`

**Read first:** spec「How it is built」「How we will know it works」W5、开放问题「输入注入的具体路径」；方案 §9 阶段A3、§5.2 `action` 消息
**Covers:** W5

**Check:** 游戏进一局、Bridge 连上后，用 `python3 tests/send_action.py --chef 0 --move 12,6` → 游戏里 0 号厨师走到 (12,6) 一格以内，且 `bridge.log` 随后一条 `state_update` 里该厨师位置随之变化；再发 `--interact cutting_board_0` 让它取番茄并放上砧板切完，画面与状态都对得上。

- [ ] 读出可用关卡列表（进游戏界面读；进菜单本身就要靠这条任务做出来的输入注入），写进 spec 开放问题 1，供固定地图的决策使用
- [ ] 输入注入路径选定并写进 `plugin/README.md`：为什么走这条路、代价是什么（对应 spec 开放问题）
- [ ] 移动用连续方向输入实现，不是瞬移；到达判定带容差，避免在目标点抖动
- [ ] 交互、抓取/放下、投掷各有独立动作，未实现的动作返回明确错误而不是静默失败
- [ ] 动作有超时（默认 10 秒），超时回一条失败事件，避免上层永远等
- [ ] 一条 `action` 的执行结果能从后续 `state_update` 里看出来
- [ ] 记录一次真实操作日志片段到 `plugin/README.md`，作为答辩素材

**Progress:**
