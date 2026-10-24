# 02: Scheduler 订单模型、FCFS/EDF 与 mq 派发

**Effort:** os-core
**Task type:** feature
**Base commit:** <commit-sha>
**Covers:** W6, W7（列表调度部分）

**Delivers:** Scheduler 不再是骨架：从 `config/{kitchen,recipes,orders}.json` 建订单与工序
DAG，按 FCFS/EDF 选单、按列表调度把就绪工序派给空闲 Worker；每次决策与事件写进
`runs/<ts>/{decisions.jsonl,events.jsonl}`；SIGHUP 切策略；SIGUSR1 导出快照。

**Blocked by:** 01
**Files:** `bridge/include/kitchenos/{policy,mq}.h`、`bridge/src/{policy,mq}.c`、
`bridge/src/scheduler.c`、`bridge/tests/test_policy.c`、`config/*.json` 读取器
**Read first:** `.workflow/specs/os-core.md` 的 Order/Task 模型、策略接口、W6/W7；
`config/recipes.example.json` 的 `nodes/edges/dishes`；`config/kitchen.example.json` 的
`stations`
**Check:** `test_policy` 的 `fcfs_orders_by_arrival`、`edf_orders_by_deadline`、
`edf_ties_match_fcfs`、`ready_queue_cook_blocked_until_fetch`、
`ready_queue_cook_ready_after_fetch`、`list_schedule_is_deterministic` 全过；
os-loop 的 `decisions.jsonl` 里 FCFS 与 SIGHUP 后的 EDF 决策都能读出策略名。

- [x] 订单模型含 `arrival/deadline/est_cost/reward/status`，从固定订单序列读入
- [x] 工序 DAG 从 `recipes.json` 构建，边方向与 `dishes[].nodes` 对齐
- [x] `kos_policy` 接口只吃订单/资源视图，不引用 game/protocol/shm
- [x] FCFS、EDF；截止期相同时 EDF 与 FCFS 选择一致
- [x] 列表调度：前置完成 + 资源可预约才进就绪队列；关键路径优先
- [x] `/kitchenos_tasks_<chef>` 与 `/kitchenos_events` 创建、定时收发、退出清理
- [x] SIGHUP 重读配置并切换策略；决策日志记录策略名与理由
- [x] 两个策略在同一输入上产出可比较的 `decisions.jsonl`

**Done when:** Check 全绿，且 `runs/<ts>/decisions.jsonl` 能从一次真实运行里读出
"谁在什么时候因为什么被选中"。

## Progress

**2026-10-24：任务 02 落地。**

新增 `policy.{h,c}`（FCFS/EDF，只吃订单/资源视图）、`recipes.{h,c}`（DAG/关键路径/
menu）、`kitchen.{h,c}`（网格+工位，与 Worker 共用）、`mq.{h,c}` 的定长消息与定时收发；
`src/scheduler.c` 完成订单模型、列表调度、mq 派发、runs/ 两个 JSONL、SIGHUP 切换与
SIGUSR1 checkpoint。SIGHUP 的重读会真正读 recipes/kitchen：有活跃订单时记
`config_reload_deferred`，空闲时替换并重建资源表。

实测：

```
cd bridge && make test
  [test_policy] 检查 36 项，失败 0 项
  === 8 test suites, 0 failed ===
bash tools/run-bridge-scenarios.sh --only os-loop
  PASS os-loop: dispatch=4 chefs=[0, 1]; policies={'FCFS': 2, 'EDF': 2}
```

双向证明（临时破坏 → 失败 → 恢复）：

| 破坏 | 失败 |
|---|---|
| EDF 比较器改用到达时间 | `edf_orders_by_deadline` |
| `kos_recipe_node_ready` 无条件返回 1 | `ready_queue_cook_blocked_until_fetch`、`ready_queue_plate_blocked_until_cook` |
| `dispatch_ready` 直接 return | os-loop 的 `scheduler_dispatched_each_chef`、`task_events_each_chef`、`decisions_name_fcfs`、三条 Worker 动作检查 |
| `strategy_switch` 不调用 `kos_policy_set_kind` | os-loop 的 `sighup_flips_policy` |

过程中抓到的真实缺陷：决策行漏了 `json_writer_object_end`，`decisions.jsonl` 9 行全部
不是合法 JSON（场景的 `decisions_parse_as_json`/`decisions_nonempty` 先失败，修好后过）；
`mq_maxmsg=64` 超过本机 `/proc/sys/fs/mqueue/msg_max=10`，Scheduler 启动即 EINVAL，
改为按 Linux 默认上限建队。

**未做：** 命名信号量租约、死锁检测、metrics.json、真机闭环（任务 04/W10）。
