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

## 游戏目录默认值和解析逻辑在两个脚本里各写一份

- **Where:** `scripts/install-bepinex.sh`、`scripts/build-plugin.sh`，另 `plugin/WellCooked.Smoke.csproj` 里有一份 IDE 兜底默认值；`APPID` 在 `scripts/run-game.sh` 和 `scripts/set-winhttp-override.sh` 各一份
- **Added:** 2026-10-14
- **Priority:** worth-doing
- **Problem:** 同一台机器的游戏目录默认路径写了三处，「检查目录存在 + 转成绝对路径」六行在 `install-bepinex.sh` 和 `build-plugin.sh` 里逐字重复。路径一变要改三处，漏一处就会出现"装到一个目录、构建读另一个目录"——任务 01 的审查（brief B 的 B3）修的就是这个症状。
- **Impact:** 组员换机器或 Steam 库搬家时，两边指向不同目录且都不报错。
- **Solution:** 抽 `scripts/lib.sh` 的 `resolve_game_dir()`，默认路径只留一份，由各脚本 source；csproj 那份保留为 IDE 直接 build 的兜底并在注释里说明。
- **Cost:** 小，纯重构；但会给已经审过的任务 01 多加一个文件、移动审查边界，所以留到下一个要碰这些脚本的任务里顺手做。

## one-slot 场景的裕度太薄，负载高时会假失败

- **Where:** `tools/fake_plugin.py` 的 `one-slot` 场景，加 `bridge/tools/worker_stub.c` 的发布节奏
- **Added:** 2026-10-15
- **Priority:** worth-doing
- **Problem:** 该场景断言"被写了槽的厨师至少收到一个 `move`"，但实测裕度只有个位数：单独连跑三次，
  240 帧里 `move` 分别是 **21 / 7 / 89**；在整批套件里跑（机器同时在编译）时掉到 **0**，场景判 FAIL。
  同一次失败的桥日志写的是 `slots_missing ... missing=0,1`，worker stub 自报"10 秒里只发布了 166 帧"
  （30 Hz 应约 300 帧）—— 即失败来自 stub 没跑满，不是协议行为错。
- **Impact:** 整批套件在负载高的机器上会随机报红，最坏情况是让人去查一个并不存在的并发 bug；也让人
  不敢相信"10/10 通过"这句话本身。
- **Solution:** 让 stub 的发布节奏以单调时钟为基准并补偿欠账（现在每轮固定睡 `1/hz`，被调度延迟吃掉
  的份额不会补回来），或在输出里同时打印 stub 的实际发布帧数、把断言按实际帧数缩放。
- **Cost:** 中，要动 worker stub 的节奏逻辑。只改断言更便宜，但会把"真的丢了输入"这类回归一起放过，
  所以先改 stub 这一侧。

## 工序"就位"卡住后退化成 1.4 米内直接按、不校验朝向

- **Where:** `plugin/WorkOrder.cs`（`_looseApproach`、`_arriveAt`、`_looseApproach ? 1.4f : ReachRadius`）
- **Added:** 2026-10-15
- **Priority:** worth-doing
- **Problem:** 精确就位（0.25 米 + 面前必须是工位）在工位相邻格站不进去时会一直打转，于是加了"卡住 6 秒就放宽到 1.4 米、不再校验朝向"的兜底。出菜口嵌在墙里、盘子堆旁边被台面挡，都会走到这条路上；此时按 A 是按"大概方向"按的，站错格就可能按到别的东西（垃圾桶那次就是这么把鱼扔掉的）。
- **Impact:** 交付/拿盘子这类步骤偶发失败但日志看起来"正常在按"，排查时容易误判成按键问题。
- **Solution:** 把"站哪一格"交给游戏自己的网格判定（`GridNavSpace`/`GetFacingGridOccupant` 反推可用格），或按工位类型分别定义"哪个方向能站"，去掉半径放宽。
- **Cost:** 中，要动工序与导航的到达判定（任务 08 的边界），需要逐关验证。

## 切菜靠"按住 X + 超时重试"，没有按 HasFinished() 收手

- **Where:** `plugin/WorkOrder.cs`（`HandleChop`）
- **Added:** 2026-10-15
- **Priority:** fix-now（并入任务 12）
- **Problem:** 切菜进度在 `ClientWorkableItem` 上（`m_progress`/`HasFinished()`），但当前实现是"按住 X 一直等"，超时（60 秒）才重试；进度读数只用于日志。
- **Impact:** 切完不及时松手，多切/切糊都有可能；超时值成了隐式行为参数。
- **Solution:** 任务 12 里改成按 `HasFinished()` 收手，并把熟度/进度超时改为"报出当前进度"。

## 关卡评估数字依赖"有厨师在场"，且探针比游戏更严

- **Where:** `plugin/WorldState.cs`（`SampleGameGrid`/`KeepLargestComponent`）、`scripts/eval-levels.sh`
- **Added:** 2026-10-15
- **Priority:** only-if-grows
- **Problem:** 没厨师时用兜底半径 0.35，通道会被判死、连通块虚高；有厨师时用实测半径 0.40，又比游戏自己的通行判定严（游戏网格 1.2 米、厨师连续移动能过 0.9 米的缝），于是"主连通块占比"偏悲观。
- **Impact:** 挑关卡时会把可用关卡判成"碎"，需要人肉再用 4 邻接复核（任务 09 的 Progress 里就是这么做的）。
- **Solution:** 评估模式单独放宽边判定（节点仍用真实半径），或在日志里同时给出"严格/宽松"两个数字。
