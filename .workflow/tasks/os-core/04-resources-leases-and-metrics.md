# 04: 共享资源租约、死锁检测与 metrics

**Effort:** os-core
**Task type:** feature
**Base commit:** <commit-sha>
**Covers:** W8, W10（指标部分）

**Delivers:** 锅/砧板/盘子等按 `kitchen.json` 建命名计数信号量；Worker 申请/释放带租约；
Scheduler 周期回收过期租约并记 `resource_reclaimed`；维护资源分配图并检测环；一局结束
写出 `runs/<ts>/{events.jsonl,decisions.jsonl,metrics.json}`。

**Blocked by:** 02, 03
**Files:** `bridge/include/wellcooked/{resource,metrics}.h`、
`bridge/src/{resource,metrics}.c`、`bridge/src/scheduler.c`、`bridge/src/worker.c`、
`bridge/tests/test_resources.c`、`bridge/tests/test_metrics.c`
**Read first:** `.workflow/specs/os-core.md` 的共享资源段、W8/W10；§6.4 方案 2 的租约；
§4.5 的指标清单
**Check:** `test_resources` 的 `wait_then_acquire`、`lease_expiry_reclaims`、
`deadlock_cycle_detected`、`test_metrics` 的 `run_dir_layout`、`metrics_fields_complete`
全过；`kill -9` 持锅 Worker 后另一个 Worker 在租约到期时拿到锅。

- [x] 按 `kind` 建信号量与容量（盘子取 `plate_stack.count`）
- [x] 申请/获得/释放事件都带 chef/task/资源名与等待时长
- [x] 租约表按截止时间回收，回收后 `sem_post` 一次且只一次
- [x] 资源分配图检测环，回退一个持有者并记 `deadlock_detected/recovery`
- [x] `runs/<ts>/` 三个文件按 spec 字段写出；同一局的 `metrics.json` 可被脚本读取
- [x] 对比脚本能读出两种策略的 completed/expired/avg_wait

**Done when:** Check 全绿，且上面 `kill -9` 的回收在日志与 `events.jsonl` 里都能看到。

## Progress

**实现（2026-10-24，worktree os-core）**

- `bridge/include/wellcooked/resource.h` / `resource.c`：按 kitchen station kind 建
  `/wellcooked_res_<kind>` 计数信号量（容量 = 该 kind 工位数），Supervisor 创建并
  unlink；Scheduler attach 后 `sem_trywait`/`sem_post`。租约表
  `(kind, chef, task_id, acquired_ms, deadline_ms)`，`kos_lease_remove()` 返回 1 才
  `sem_post`，从实现上排除 double release。资源分配图环检测 + victim（环上最大
  chef_id）见 decisions.md。
- `bridge/include/wellcooked/metrics.h` / `metrics.c`：`metrics.json.tmp` + rename
  原子写出；`test_metrics` 额外用 inode 变化证明 rename，而不是只检查文件存在。
- `scheduler.c`：派发前取令牌并建租约，DONE/失败释放一次；tick 里 `expire_leases`
  和 `detect_deadlock`；Worker `RECOVERED` 只走 `reclaim_leases_for_chef`；关机释放
  剩余租约并写 metrics；SIGUSR1 写 reason=sigusr1 的快照。资源事件均带
  kind/chef/task/reason，获得事件带 wait_ms。等待中的 pending 任务也计入 metrics 的
  wait_ms。
- `worker.c`：事件里带 generation，供 Scheduler 区分首次启动与重启（worker_restarts）。
- `supervisor.c`：启动时加载 kitchen、创建资源信号量；关机时先打印
  `resource_values`（unlink 前的 value/capacity），再 unlink `/wellcooked_res_*`；
  SIGUSR1 转发给 Scheduler（操作者只信号 Supervisor，metrics 由 Scheduler 原子写）。
- 场景 `tools/fake_plugin.py --scenario os-resources`：临时 kitchen 只留 1 个 cooker，
  2 个厨师，lease_ttl=8000ms；先向 Supervisor 发 SIGUSR1（验证转发与快照），
  持 cooker 租约时 SIGKILL worker-0，等重启/回收，再停树；断言 SIGUSR1 快照、
  争用 wait、worker-recovered 回收、lease-expired 回收、metrics 字段、关机
  value=capacity、/dev/shm 无 `sem.wellcooked*` 残留。
- `tools/compare-runs.py`：读两个 `runs/<ts>`，输出 completed/failed/expired、
  avg_wait_ms（Σwait_ms/Σwaits）、dispatch 与 restarts。

**验证（精确命令与结果）**

- `cd bridge && make clean && make test` → 退出码 0，`=== 10 test suites, 0 failed ===`。
  逐套件：framing 194 + json 166 + net 204 + policy 36 + protocol 226 + shm 95 +
  supervisor 67 + worker_nav 17 + resources 35 + metrics 37 = **1077 项检查**。
- `cd bridge && LD_LIBRARY_PATH=/tmp/kos-san-libs/usr/lib64 make test-san
  LDFLAGS="-pthread -lrt -L/tmp/kos-san-libs/lib"` → 退出码 0，10 套件 0 失败、
  1077 项检查、无 ASan/UBSan 报告。原样 `make test-san` 仍是本机缺 libasan 运行时，
  与任务 01 记录的同一 workaround，未改 Makefile。
- `bash tools/run-bridge-scenarios.sh` → **13/13 场景通过**（含 `os-loop` 与
  `os-resources`）；`os-resources` 本身 20 项 HARD 检查全 ok，尾行：
  `single cooker; acquired=5 released=2 reclaimed=3 expired=1 waits=1 killed_task=3`；
  回收后轮转起点让另一个厨师拿到 cooker（`other_chef_acquires_after_reclaim`）。
- `python3 tools/compare-runs.py <runA> <runB>` → 读出两局
  completed/failed/expired/avg_wait_ms/dispatch/restarts（同一局的 FCFS/EDF dispatch
  计数也能从 metrics.json 读出）。

**双向证明（临时破坏 → 命名检查失败 → 恢复 → 全绿）**

| 破坏 | 观察到的失败 | 恢复后 |
|---|---|---|
| `kos_resource_release` 不 `sem_post` | resources: `value_one_after_release`、`wait_then_acquire`、`single_post_for_single_lease`、`back_to_capacity`、`expiry_reclaimed_to_capacity` 等 7 项 FAIL | 35/35 ok |
| `kos_lease_remove` 永远返回 1、不删租约 | resources: `second_remove_returns_zero`、`expiry_not_released_twice` FAIL | 35/35 ok |
| 环检测分支永不执行 | resources: `deadlock_cycle_detected`、`cycle_contains_both_chefs`、`victim_is_highest_chef_id` FAIL | 35/35 ok |
| metrics 写入函数直接 return，不创建文件 | metrics: `run_dir_has_three_files`、`metrics_readable`、`metrics_json_parses` 及字段检查 FAIL | 37/37 ok |
| metrics 漏写 `worker_restarts` | metrics: `worker_restarts` FAIL（os-resources 的 `metrics_fields_complete` 也会红） | 37/37 ok |
| tmp 路径改成直接写目标文件（无 rename） | metrics: `atomic_rename_replaces_inode` FAIL | 37/37 ok |
| `reclaim_leases_for_chef` 直接 return 0 | os-resources: `killed_lease_reclaimed_by_worker_death` FAIL（任务到 13.9s 才被 `lease-expired` 回收） | 20/20 ok |
| 同时去掉 Worker 回收与关机租约归还 | os-resources: `metrics_fields_complete`（in_use=1）、`sem_value_back_to_capacity`（value=0 capacity=1）FAIL | 20/20 ok |
| SIGUSR1 不写 metrics + `mark_resource_wait` 直接 return | os-resources: `metrics_sigusr1_snapshot`（reason=None）、`contention_observed`（wait events=[]）、`metrics_resource_wait`（waits=0）FAIL | 20/20 ok |
| 回收后不轮转 chef（固定从 0 开始） | os-resources: `other_chef_acquires_after_reclaim` FAIL（cooker 全被 chef 0 拿走） | 20/20 ok |

**没做 / 明确未验证**

- W10 的真实游戏部分没有运行：没有启动游戏、没有实机两个厨师完成订单的证据；
  只验证了 metrics 文件、字段、原子写与读取脚本。
- 当前每个任务只持有一个资源，正常调度不会形成环；`deadlock_detected` 的完整
  Scheduler 分支只有单元级环检测证据，没有构造出运行时死锁场景（单资源任务模型下
  构造不出来）。
