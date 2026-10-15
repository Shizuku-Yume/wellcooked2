# 09: 插件侧工序执行面：按订单自动完成一单

**Effort:** game-bridge
**Task type:** feature
**Base commit:** <commit-sha>
**Status:** paused

**Delivers:** 插件把"工序"跑通：给定当前订单，自动走完 取料 → 切 → 装盘 → 交付，游戏得分从 0 变正、订单数减一，日志逐步打出"做了什么、目标是谁、结果如何"。

这是任务 04/05 里**游戏侧的那一半**：原子动作执行面（移动/交互/等待，每个动作有明确结果与超时）+ 工序状态机（按菜谱决定下一步、按工位内容物与进度判断能不能做）。它不依赖 Bridge 就能验证；等 02/03（组员的协议与客户端）落地，把"谁来下动作"从本地脚本换成 Bridge 下发即可，执行器不动。

**Blocked by:** None
**Files:** `plugin/`（新增 `Actions.cs`、`WorkOrder.cs`、`Kitchen.cs`；复用 `Navigation.cs`、`WorldState.cs`、`Takeover.cs`）
**Read first:** spec「What it does」「状态语义」；任务 07/08 的 Findings（世界状态字段、导航与到达判定）；`wellcooked-state.json` 现有字段
**Covers:** W6
**Note:** 只覆盖 W6 的游戏侧；传输侧（Bridge 下发动作）仍由 04/05 覆盖。

**Check:** `bash scripts/dev-cycle.sh` 进关卡后，日志出现 取料 / 切 / 装盘 / 交付 四步都成功，`wellcooked-state.json` 的 `score` 从 0 变正且订单数减一；同一序列连跑两次都能完成至少 1 单。

- [ ] 原子动作：`MoveTo`（复用任务 08 的导航）、`Interact`（按下→松开一次）、`WaitFor`（按状态判定 + 超时）；每个动作返回明确结果（成功 / 超时 / 做不到），不静默失败
- [ ] 工序状态机：按当前订单的菜谱决定步骤顺序，能读工位内容物与进度决定"这一步现在能不能做"
- [ ] 决策来源隔离：谁下动作由一个接口决定；本地脚本是替身并在代码里注明"由 Scheduler 接管"
- [ ] 日志：每一步的动作、目标工位、耗时、结果；失败有超时与原因
- [ ] 不依赖 Bridge：任务 02/03 未落地时也能跑通本 Check

**Done when:** 上面每条都打勾，且 Check 的两次连跑都有日志与 `score` 证据。

## Progress（2026-10-14：工序状态机已成型，卡在导航可靠性）

已提交在 `game-bridge/08-navigation`（沿用同一分支）。配置 `work=auto` 打开工序模式。

**已经跑通的**（实测日志）：

| 环节 | 证据 |
|---|---|
| 读订单/菜谱 | `指令: 活跃订单菜名=Sushi_PlainFish`、`步骤叶子=SushiFish`、`料箱 DispenserCrate 3 (4) 给=SushiFish (7.2,-3.6)`、`盘子堆/出菜台` 都读得到 |
| 1/5 取料 | `工序: 1/5 去取料「SushiFish」` → `按交互` → 手上出现 `SushiFish` |
| 2/5 放台面 | 手上变空，料留在台面 |
| 3/5 拿盘子 | 手上出现 `Plate 1 (4)` |
| 4/5 装盘 / 5/5 交付 | 步骤能推进，但见下 |

**卡住的地方**：第 4/5 步（回到放料的台面装盘）常走不到台面——`寻路=侧向错身`、
20 秒只挪半米，于是拿着盘子在空中按 A，装盘不成立、交付也就无从谈起，`score` 仍为 0。
根因还是任务 08 里那条：**物理采样的网格与游戏真实碰撞对不齐**，某些位置会卡住；
错身逻辑本来是为"两个厨师对撞"写的，被用到长距离移动上就成了乱走（已限制到 3 米内，仍未根治）。

**这一轮顺带确认的游戏机制**（都写进代码注释了）：

- **不能拿着盘子直接取料**：拿着盘子去料箱按 A，盘子会被放下、只拿到料。
  正确流程是 取料 → 放台面 → 拿盘子 → 回到料那儿装盘 → 交付（工序就是按这个写的）。
- **放料的台面要离料箱/盘子堆 2.4 米以上**：站太近时按 A，游戏在"料箱取料"和"台面拿放"
  之间乱选，手上东西在料和盘子之间跳。
- **交互要"按一次等效果"**：连按会把刚拿到的东西放下。
- 手上东西要带内容物摘要（`Plate 1 (4)[SushiFish]`），只看名字分不清"拿着生料"和"装好盘"。

**下一步**：先把导航可靠性做扎实（这是所有工序步骤的地基），再回来跑通 4/5、5/5。
可选路子见任务 08 的 Findings 末尾（改用游戏自己的 `GridNavSpace.FindPath` 出全局路线，
或把到达判定与目标点按工序重新定义）。

## Progress（2026-10-15：按真实菜谱重写工序，整单跑通，score 从 0 变正）

上一轮"卡在 4/5 装盘"的根因不是导航，是**流程本身写错了**：这一轮读了参考项目
[Extremelyd1/OvercookedAI](https://github.com/Extremelyd1/OvercookedAI) 的动作分解，
再用 `ilspycmd` 反编译核对，把工序改成真实菜谱（**鱼要切**）：

```
1/7 取料 → 2/7 放砧板 → 3/7 切（按住 X）→ 4/7 拿盘子 → 5/7 装盘 → 6/7 拿走装好的盘子 → 7/7 交付
```

**证据**：`wellcooked-state.json` 的 `score = 28`（原来一直 0）；另一次日志 `工序: 完成一单（累计 1 单）`。
装盘成功可从工位内容物看出：砧板 `SushiFish` → `ChoppedSushiFish` → `Plate 1 (4)`，
而 `ClientIngredientContainer` 里出现 `IngredientAssembledNode`。

**这一轮确认的游戏机制**（都写进代码注释）：

| 机制 | 事实 |
|---|---|
| 切菜键 | 是 **X（`WorkstationInteract`）不是 A**：`PlayerControls.ControlSchemeData.m_worksurfaceUseButton`；`PlayerInputLookup.LogicalToAmbiButton` → `AmbiPadButton.Two` → `ControlPad.Button.X` → InControl `Action3`。参考项目用键盘 Ctrl |
| 切要按住 | `ClientWorkstation.InteractionIsSticky`，按住才持续切；只点一下不会开始，空手点一下反而把料拿走 |
| 切菜进度 | 在 `ClientWorkableItem` 上（`m_progress` 是**整数阶段**、`m_subProgress` 是当前阶段刀数、`HasFinished()`）；板上有没有料要看 `ClientAttachStation.m_item`（`workstation.m_item` 只在有人用时才非空） |
| 装盘语义 | 把盘子放下去，料进盘子、**手上变空**，装好的盘子留在台面上；交付前要再拿起来（参考项目 `PlateHoldingAction` 判 `!IsCarrying`） |
| 交互判定 | `InteractWithItemHelper.GetFacingGridOccupant` 取"最对齐的邻居格"的占位物、**不看距离**；名字不可靠（盘子堆挂在台面上时返回台面本身），要比**格子** |
| 垃圾桶 | `ClientRubbishBin` 也挂 `ClientAttachStation`，挑台面/工位必须排除（实测把鱼扔进去过） |

**当前不稳的地方**（已记入 `technical-debt.md`）：

- 就位卡住 6 秒后退化成"1.4 米内直接按、不校验朝向"（出菜口嵌在墙里那次的经验），是个兜底不是机制。
- 切菜那步靠"按住 X + 超时重试"推进，没有按 `HasFinished()` 主动收手。
- 工序只覆盖"切 + 装"，**没有煮/煎**，所以只做得了寿司 1-1 这类单料菜；卷饼、煮饭类订单走不通。

**这一轮的额外产出**（挑关卡用，见任务 13）：`scripts/scan-levels.py`（离线扫 144 个关卡资源包）、
`scripts/eval-levels.sh`（每关重开一局量菜谱/连通性/机制），以及插件指令
`mech`（数场景里真实存在的机制组件）、`levels`（打印关卡表）、`unlock`（解锁全部关卡，游戏自判 52/52）。

**下一步**：任务 10（插件整理）→ 11（接入 Overcooked2-TAS 的控制与进度读取）→ 12（工序补煮/煎）
→ 13（用 OC2DIYLevel 自建演示关卡）。

## Progress（2026-10-15 复核：Check 跑了一次成功一次失败，进关入口已改成确定性）

用 HEAD（`07bb031`）重新 `bash scripts/build-plugin.sh` 后按 Check 重跑，结论：**单次能成，连跑两次不成**。

| 第几次 | 进关 | 结果 |
|---|---|---|
| 1 | `s_sushi_1_1`，2 名厨师 | 1/6 取料（`SushiFish`，就位 0.20 米）→ 2/6 放砧板（0.16 米）→ 3/6 切 → 4/6 拿盘子 → 5/7 装盘 → 6/7 拿走 → 7/7 交付 → **`工序: 完成一单（累计 1 单）`**；`score = 28`（订单数 2→5，后续单是回合继续在出）。随后下一单卡在 `3/6 切料中`，反复 `Chop 超时 60 秒` |
| 2 | 同一条命令，进关时是 4 名厨师 | **0 单**：卡在 `4/6 去拿盘子`，反复 `GetPlate 超时 20 秒`，`score = 0` |

两次卡住的地方都不在"工序流程"上，而在**执行面**：`3/6 切料中` 那行反复打
`在用=False 游戏看到 X=True/刚按=False`（按住 X 没被工位接受），拿盘子那次是就位超时 ——
与 `technical-debt.md` 里"就位卡住退化成 1.4 米直接按""切菜靠按住 X + 超时重试"两条是同一件事，
也正是任务 11（逻辑层注入）要换掉的那条路。

**进关入口原来不可复现，已修（`scripts/dev-cycle.sh`）**：

1. `frontend_press` 默认 false，`wellcooked-takeover.txt` 里也没有这一行 → 脚本原样跑会停在
   StartScreen 三分半（日志只有 `smoke patch alive`，`chefs=0`）。脚本现在自己写这一行并在退出时还原。
2. 开了前端按键之后，进的是**存档里世界地图光标停着的那一关**（这次是 `s_mine_4_6`，带矿车轨道、
   挑关卡时已排除）—— 进哪一关不由我们决定，Check 就不可复现。
3. 现在支持 `bash scripts/dev-cycle.sh <观察秒数> <场景名>`：等 `游戏已自己加入 pad` 出现两次 →
   关掉前端按键 → `load=<场景名>`。实测 `s_sushi_1_1` 以 2 名厨师进入，地形按这一关重新采样
   （`连通块 3 个（丢掉 12 格），可走格 214/308，可走边 389`、`收集到 5 个目标点（工位）`）。
4. 两条踩过的坑记在这里：`load` 早于"游戏自己加入虚拟手柄"时关卡里 `chefs=0`（工序无从跑起）；
   `engage=1` 只在标题画面有效（要 `StartScreenFlow.Instance`），`continue=1` 单独用不会让手柄加入。

**没验证到的**：`s_sushi_1_1` 本身带 `TabletopConveyenceWindReceiver×18`、`WindAccumulator×2`、传送门 ×4
（`mech=1` 实测），台面上的东西会被风吹走 —— 它不属于挑关卡时要留的那一类，所以这一轮**不能判定
工序在合适关卡上也做不到**。要判定，得等任务 13 的自建关卡（无风、无轨道、菜谱含切+煮）。

**下一步（handoff）**：任务 10（插件整理）→ 11（逻辑层注入：切/拿盘子那两步就在这条路上）→ 12（补煮/煎）
→ 13（自建演示关卡）；13 的关卡到位后回到本任务复跑 Check，两次都成再归档。
工作树 `main`，无未提交改动；本任务的文件在 main 上（历史工作也是直接落在 main 的）。
