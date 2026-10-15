# 04: 无游戏联调工具 fake_plugin.py

**Effort:** bridge
**Blocked by:** 01
**Task type:** feature
**Covers:** W2, W3, W4, W5, W7
**Delivers:** `tools/fake_plugin.py`：协议级模拟插件，能在不启动游戏的前提下驱动 Bridge 走完
全部场景；每个场景以退出码表达结论，可被脚本批量调用。
**Files:** `tools/fake_plugin.py`, `tools/run-bridge-scenarios.sh`
**Read first:** `.workflow/specs/bridge.md`（How we will know it works）,
`protocol/`（01 的产出，分帧与字段名）。
**Done when:**
- [x] `--scenario happy`：连上、发 `hello`、60 Hz 发 `state_update`、收动作并回 `ack`
- [x] `--scenario one-slot`：只写 1/N 个输入槽，验证 Bridge 对缺失厨师下发 idle
- [x] `--scenario no-heartbeat`：停发心跳，验证 Bridge 判定断开；随后重连恢复
- [x] `--scenario bad-frames`：发 >1 MiB 帧、非法 JSON、截断帧各一次，验证 Bridge 存活
- [x] `--scenario connect-mode`：反转拓扑，自己监听 14455 等 Bridge 连进来
- [x] 每个场景自己判定并通过/失败退出码报告，不需要人看日志下结论
- [x] 退出码：0 通过；非 0 表示该场景断言失败，并在 stderr 说明失败点

**Check:** `cd core && make && bash ../tools/run-bridge-scenarios.sh` 全部场景退出码 0。

## Progress

**状态：完成。**

```
PASS happy (4s)
PASS one-slot (7s)
PASS no-heartbeat (9s)
PASS bad-frames (6s)
PASS connect-mode (2s)
合计: 5 个场景, 通过 5, 失败 0, 跳过 0
SUITE_EXIT=0
```

`--selftest` 另有 23 项 HARD 自检全过。退出码约定：0 通过 / 1 断言失败 / 2 环境错误 /
**77 跳过**（前置条件缺失），后两者刻意与"断言失败"区分开 —— 否则"跑不起来"会被误读成"跑通了"。

这个工具是整个 effort 能在不启动游戏的前提下完成验证的原因：W2–W5、W7 全部由它驱动，
只有 W8 需要真实游戏。

### 设计取舍

- **只认 C 的 worker stub，没有 Python 兜底。** `core/build/kitchenos-worker-stub` 不存在时
  `one-slot` 直接 SKIP（77）并说明原因，而不是用 Python 假装写槽。代价是一次 `make clean`
  会让 W3 失去证据；收益是"输入槽由 C 代码按 shm.h 的真实布局写入"这件事没有被偷换。
  这个代价真实发生过，见下。
- 非默认端口 + 可选 `--state-name/--sem-name/--log` 覆盖，使场景可以互相隔离。

### 已知问题与教训（integrator 记录）

- **stub 的 `--duration` 语义曾被误解。** harness 传的是 `args.duration + 6`（即 10 s），
  远大于 warmup(1.5s) + 测量窗口(4s)，所以它假设 stub 全程存活是安全的 —— 问题出在 stub
  自己：它按"发够 duration×hz 帧"计时，一旦被抢占落后就会**突发追赶**，用远少于 10 秒的
  真实时间把帧发完然后提前退出。表现是 warmup 阶段有 move、测量窗口几乎没有
  （`move=1/240`），并被我一度误判为"harness 抖动"。修在 stub（改按真实流逝时间退出、
  跳过错过的节拍而不补发）而不是改 harness，因为 harness 的假设本身是对的。
- **并发运行会互相踩。** 两次 harness 运行重叠时观察到伪失败：`FAIL happy —
  bridge_writes_state_shm`（另一次运行的 `clear_stale_shm` 把本此运行**正在使用**的
  `/dev/shm/kitchenos_c04_happy` 删掉了），以及三个 90 s 超时（`logs/scenario-*.out`
  被另一次运行同时覆盖）。runner 硬编码 `PREFIX=kitchenos_c04` 并写同一批日志文件。
  规避办法是串行执行，或用 `-- --state-name ... --sem-name ... --log ...` 隔离
  （PASSTHRU 放在最后可用，已实测）。这一点在多人/多 agent 环境下必须注意：
  **它产生的失败看起来像产品缺陷，实际是测试基建的冲突。**
- **零长帧**：`bad-frames` 里对"零长帧是否应该断链"的断言是 soft 而非 hard，因为
  `net.c` 当前把零长帧判为失步并断链，而 `framing.h` 与参考实现
  （`TcpLink.cs:287` 只拒绝 `len < 0 || len > MaxFrameBytes`）都认为它是合法的空帧。
  这是一处真实的规格不一致，已连同证据交回 net 的负责人修正；修好之前该断言保持 soft
  是正确的 —— 把它改成 hard 只会掩盖分歧，不会解决分歧。

### 收尾补记：三条问题的最终结论（integrator，任务已提交为 `d0f9bab`）

上面三条后来都被作者追到了根因，其中两条的**最初解释是错的**，值得更正：

- **"并发运行会互相踩"的真正机制不是日志互相覆盖，而是清理模式误杀。** runner 用
  `pkill -f "kitchenos-bridge .*--state-name X"` 清理，但这个模式**也会匹配到正在运行的
  fake_plugin 自己的命令行** —— 因为它的 cmdline 里同样含有 bridge 路径和 `--state-name X`。
  两个 runner 实例并存时，一个实例的清理会 SIGKILL 掉另一个实例正在跑的场景，
  症状是"0 秒 / 7 秒就超时"以及被截断的日志。**这与 Bridge 真的卡死在观感上无法区分**，
  是本 effort 里最具误导性的一处。修法是把模式锚定到二进制路径
  （`^<bridge path> .*--state-name X( |$)`），并用 `pgrep` 证明现在只匹配真正的 Bridge。
- **一条软断言曾经是因为 harness 自身的 bug 才通过的。** 对端关闭的 socket 不 `recv` 就
  看不见，而 Bridge 的心跳可能先于 FIN 到达，于是"连接保持"是在**根本没有查看**的情况下
  得出的结论。补上 `Link.probe_closed()`（在超时内排空到 EOF/error）后，超大帧断链
  从"软断言 + 说明"升格为**引用规格条文的硬断言**，读到的是真实状态（RST 或 EOF）。
  教训：一条一直通过的断言，可能只是因为它从来没真正观察过。
- **零长帧已修复**（`eaed4f5`）。现在 Bridge 交付这个空帧、由解析器拒绝该消息、连接保持，
  对应断言不再依赖 soft 兜底。

**最终状态**：连续三次全量运行（含一次 `--strict`，把所有 soft 断言升格为硬断言）
均为 5/5 PASS、退出码 0、零 soft WARN，被测二进制 sha256 在运行前后一致。
每次运行使用 `/kitchenos_c04_<run-id>_<scenario>` 与 `logs/bridge-<run-id>-<scenario>.log`
（run-id = runner pid），且只清理自己的对象，因此并行运行互不可见、互不可删；
`wait_listen` 还会校验监听 socket 的 inode 确实属于本次的 Bridge pid，
从根本上杜绝"连到了别人的 Bridge 上"（这种情况真实发生过一次，当时 12345 被占）。
