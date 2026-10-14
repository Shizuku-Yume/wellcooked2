# 06: 插件能不能控制厨师（输入注入可行性）

**Effort:** game-bridge
**Task type:** spike
**Base commit:** <commit-sha>

**Question:** 插件能不能控制游戏里的厨师（至少做出一次可见的输入效果）？走哪条路线，代价是什么？
**Time box:** 两条路线各试一次，先试补 `PlayerInputLookup`（游戏逻辑层），不成功再试进程内 `InControl.InputDevice`；不做完整动作集，不接 Bridge。

**Delivers:** Answer to: 插件能不能控制游戏，走哪条路线。

**Blocked by:** None
**Files:** `plugin/`（试验代码，可能丢弃）、游戏目录日志
**Read first:** spec「How it is built」「游戏内部接口的已知起点」；`.workflow/spike-tasks.md`

**Check:** 启动游戏，插件在脚本窗口内强制按下逻辑按钮 → 标题画面的卷帘门打开（截图）且日志列出被查询的 `LogicalButtonID`；答案与证据写进 `## Findings`。

- [ ] 列出标题画面阶段游戏查询了哪些逻辑按钮/轴 ID
- [ ] 至少一次可见的输入效果（画面截图 + 对应日志行）
- [ ] 记录这条路线的代价：绑了哪些私有类型、版本变化会不会断、帧同步要求
- [ ] 结论回写 spec 与 decisions；试验代码保留还是丢弃写清楚

## Progress

## Findings
