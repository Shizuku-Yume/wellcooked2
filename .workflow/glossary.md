# Glossary

What things in this project are called, and what those words mean.

One entry per concept. If two people would use the word differently, that is a
bug in the project, not in the conversation: fix it here.

Add a term the moment it is agreed, not later. A name that exists only in a
conversation is a name that will drift. Git merges this file with the union driver
(`.gitattributes`), so parallel branches can each add terms.

## Format

```markdown
- **Term**: what it means, in one sentence, ideally naming where it shows up in the
  code.
- _Avoid_: words that mean the same thing but should not be used (optional; omit if no synonyms exist).
```

The `Avoid` line does real work when it exists. "Order" and "Purchase" being two
names for one thing is a common source of confusion. But if a concept has no
competing names, omit the line rather than inventing fake synonyms.

## Terms

- **Bridge**: 唯一与游戏通信的进程；通过 TCP 收发 JSON，把世界状态写进共享内存，把 Worker
  算出的输入汇总后下发插件。它不解释调度策略。
- **世界状态（world state）**: 游戏当前局面（得分、时间、厨师、订单、厨具），由 Bridge 独占
  写、其他进程并发读，放在共享内存 `/kitchenos_state` 里。_Avoid_: 状态快照、快照（那是
  Scheduler 的 SIGUSR1 检查点，不是这个）。
- **输入槽（input slot）**: 共享内存里每个厨师一格的动作槽位，由对应 Worker 独占写、Bridge
  读。_Avoid_: 动作队列。
- **帧（frame）**: 一次世界状态更新所代表的时间片。Bridge 收到一个 `state_update` 就是一帧。
- **帧屏障（frame barrier）**: 一帧内 Bridge 等待全部输入槽就绪才下发动作；超时则对缺失的
  厨师下发 idle。_Avoid_: 帧同步（那是插件侧的阻塞应答行为，见 decisions.md:
  2026-10-15 - 帧屏障以 state_update 为帧边界）。
- **idle**: 让某个厨师不动的输入，用于 Scheduler/Worker 缺席时保持连接和局面不失控。
- **state_update**: 插件上报世界状态的 JSON 消息类型名，字段见 `protocol/`。
- **action**: Bridge（源自 Scheduler）下发给插件控制某个厨师的动作消息类型名。
- **分帧（framing）**: TCP 字节流上的消息边界：4 字节大端长度前缀 + UTF-8 JSON，单帧上限
  1 MiB。两端必须完全一致，见 `plugin/WellCooked.Plugin/Net/TcpLink.cs`。
- **effort bridge**: 这一轮把 Bridge 进程及其共享内存、协议、无游戏联调工具做出来的工作单元。

