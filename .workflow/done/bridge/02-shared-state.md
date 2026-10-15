# 02: 共享内存世界状态与输入槽

**Effort:** bridge
**Blocked by:** None
**Task type:** feature
**Covers:** W1, W3
**Delivers:** `bridge/` 的 `shm` 模块：`/kitchenos_state` 布局、世界状态 seqlock、每厨师一个
输入槽、就绪信号量 `/kitchenos_input_ready`，以及把它单独跑起来的单元测试。
**Files:** `bridge/include/kitchenos/shm.h`, `bridge/src/shm.c`, `bridge/tests/test_shm.c`
**Read first:** `.workflow/specs/bridge.md`（How it is built 的并发规则）,
`overcooked_os_course_project.md` §5.1「共享内存 + 信号量」与 §6.1 Bridge 职责。
**Done when:**
- [x] 世界状态：Bridge 独占写、他人并发读，seqlock 读写一致（读者永不看到撕裂的中间态）
- [x] 输入槽：每槽独立 seqlock，Worker 写完 `sem_post`，读者可判定"第 N 帧该槽是否就绪"
- [x] 结构带版本号，版本不匹配时拒绝挂接并给出明确错误
- [x] `make test` 里 seqlock 的一致性测试在并发读写下有确定结论
- [x] `/dev/shm` 下创建，路径与名字可配置以便测试隔离

**Check:** `cd bridge && make test` 退出码 0，且 shm 组测试包含一个读写并发用例并通过。

## Progress

**状态：完成。**

`make test` 退出码 0，`test_shm` **95 项检查 0 失败**，其中包含并发读写用例（读者进程
与写者进程真并发，并对"人为制造的不一致"做了反证 —— 自检本身能失败，因此"通过"是有信息量的）。

### 抖动问题（本任务的主要工作）

首版 `test_shm` 是**抖动的**：同一个二进制、同一份输入，连跑三次得到

```
exit codes: 1 0 1
run1: 检查 87 项，失败 2 项
run2: 检查 87 项，失败 0 项
run3: 检查 87 项，失败 2 项
```

失败项固定是"所有读者进程正常退出"与"没有读者把重试次数用尽"。抖动比失败更糟：
`make test` 退出码 0 同时是 01、02、03 三个任务的 Check，抛硬币的判据等于没有判据。

根因是 seqlock 的固有风险 —— 写者连续回写时不给读者留出偶数序号窗口，读者把重试预算
耗尽。修复方向是让读者在有界重试内必然成功（重试间让出 CPU），并把测试改成断言一个
**可复现的事实**，而不是去和调度器赛跑：写者按接近真实帧率的节奏写，读者不再满速空转。

验证方式：连跑 10 次以上，逐次粘贴退出码与"检查 N 项，失败 M 项"，全部一致。

### `/dev/shm` 泄漏（已修）

修好抖动后暴露出第二个缺陷：**每次 `make test` 都泄漏共享内存对象**。

```
$ rm -f /dev/shm/kitchenos* /dev/shm/sem.kitchenos*
$ ls /dev/shm | grep -c kitchenos
0
$ cd bridge && make test; echo "exit=$?"
exit=0
$ ls /dev/shm | grep -c kitchenos
6
kitchenos_test_open_<pid> / kitchenos_test_seq_<pid> / kitchenos_test_slots_<pid>
sem.kitchenos_test_sem_open_<pid> / _seq_<pid> / _slots_<pid>
```

三个 fixture 各泄漏一内存对象 + 一信号量；实测累积到 30 个。`/dev/shm` 是 tmpfs，
这就是实打实占着内存直到重启；而且 `ls /dev/shm | grep kitchenos` 为空是 task 03 的
字面 Check，这个泄漏会让它因为无关原因失败。修复后实测：

```
make test exit=0
立刻:   0
3秒后: 0
```

### 帧屏障缺格判定（最严重的一处，已修）

`kos_shm_wait_frame_after` 的 `missing_mask` **不是累积的**：它在推进水位之后才重算"谁不新"，
于是刚刚被满足的槽位立刻又显得不满足。实测到 `missing_mask=0x3 水位=[901, 1000]` —— 0 号槽
明明已经到达 901，却仍被报为缺失。

后果不是崩溃而是**静默劣化**：Bridge 会给一个按时响应的厨师下发 idle `stop`，表现为"调度
算出了动作但游戏里角色不动"，且不会产生任何错误日志。修复后同一次等待里两个方向都被断言：
已到达的槽水位推进到 901，超时未到的槽停在 1000，且 `missing_mask == 0x2`。

### 重试预算耗尽（两个真 bug，不是一个）

原先 290 万次读里有约 260 万次返回 `KOS_ERR_BUSY`（85–90%）：一是把"看到奇数序号"也计作一次
重试，而奇数观测只花纳秒、写者持有奇数序号却是整个载荷写入的时长，读者在写者写完前就把 64 次
预算烧光；二是校验和放在写者的奇数窗口内做逐字节 FNV（7 KB 约 12 µs），进一步拉长了奇数窗口。
现在只有**真正的载荷拷贝**消耗预算，等待另有预算（有界自旋 4096 次后 `sched_yield()`，250 ms 上限），
校验和改为按 8 字节字折叠。`BUSY` 的含义因此变得可用：它现在表示"写者持有奇数序号超过 250 ms"，
也就是写者已经死了。修复后 16 次运行、每次约 50 万次读：0 BUSY、0 撕裂、0 其他错误。

另外，读一个**从未被写过**的槽（序号仍为 0）原先会穿过校验和判断落到 BUSY；"从没写过"是一个
确定的"否"，现在返回独立的 `KOS_ERR_EMPTY`。

### 确定性的做法

正向阶段写者按 **1 ms/帧**（真实 60 Hz Bridge 的 16 倍）的节奏写，而不是能多快就多快 ——
不打节奏时读者的 BUSY 次数是机器负载的函数，而不是代码性质的函数。此外加了一个**不依赖调度运气**
的确定性用例：测试自己调 `kos_shm_world_write_begin`（序号转奇）、塞一个写了一半的 score、再读，
断言返回 `KOS_ERR_BUSY`、**载荷拷贝尝试次数为 0**、输出缓冲区从未被触碰。于是"读者永不接受
写了一半的载荷"从一个赛跑撞上的事实，变成测试主动驱动的事实。并发是真的发生过的，也被记录
（每轮 20–28 个读者因为拷贝中途序号变化而重拷）；反证仍然保留（故意不同步的拷贝每轮能检出
约 20–29 万次不一致），所以"通过"是有信息量的。

### 对 task 03 Check 的一条重要限定

`make test` 自身残留为 0（逐套件实测 `test_framing/json/protocol/net/shm` 各自 残留=0）。
但**裸跑 `ls /dev/shm | grep -c kitchenos` 在别人有 Bridge 活着时不会是 0** —— 那些对象属于
并发运行的 harness。所以这条 Check 要么按本次运行使用的 `--state-name` 限定，要么在没有其他
KitchenOS 进程时取。

### worker stub（integrator 补交付）


`bridge/tools/worker_stub.c` 原属本任务，但长时间未交付；而 `tools/fake_plugin.py` 只认
`bridge/build/kitchenos-worker-stub` 这一个真槽写入者、没有 Python 兜底，缺了它 W3 无法复现
（一次 `make clean` 就把它连同证据一起删掉，且无法重建 —— 源文件不存在）。由 integrator
按冻结 CLI 补写：

    kitchenos-worker-stub --state-name NAME --sem-name NAME --chefs N --slot K --hz H --duration S

只挂接、绝不创建段（段不存在时明确报错并提示先启动 Bridge，退出码 1）。补写过程中发现并
修掉了自己的一个真缺陷：原实现用绝对时刻睡眠，被抢占落后时会**突发追赶**，用远少于
`duration` 的真实时间把帧发完然后提前退出，导致 live 槽位在测量窗口内"消失"
（`move=1/240`）。改为按**真实流逝时间**决定退出、并跳过错过的节拍而不补发。修复后连跑
三次：缺失槽三次都是 **240/240 全 `stop`**，live 槽稳定拿到 53–107 个 `move`。
