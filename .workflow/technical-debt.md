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
