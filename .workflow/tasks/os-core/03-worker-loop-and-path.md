# 03: Worker 任务循环、A* 路径与输入槽

**Effort:** os-core
**Task type:** feature
**Base commit:** <commit-sha>
**Covers:** W9

**Delivers:** Worker 真正干活：从自己的 mq 收任务，用 `kitchen.json` 网格做 A*，按路径
逐帧写 `move`，到达工位后发 `interact`/`pickup`，完成/阻塞时回事件；没有任务时写 `stop`。

**Blocked by:** 02
**Files:** `bridge/include/kitchenos/{nav,worker_task}.h`、`bridge/src/{nav,worker_task}.c`、
`bridge/src/worker.c`、`bridge/tests/test_worker_nav.c`
**Read first:** `.workflow/specs/os-core.md` 的 Chef Worker 段与 W9；
`config/kitchen.example.json` 的 `grid.cells/mask/edge_bits`、`stations[].pos`；
`bridge/include/kitchenos/shm.h` 的 `kos_shm_slot_publish`
**Check:** `test_worker_nav` 的 `path_respects_mask`、`path_reaches_station_neighbour`、
`slot_contains_move_then_interact` 全过；`os-loop` 场景的
`worker_move_forwarded_each_chef`、`worker_interact_forwarded_each_chef`、
`worker_slot_nonstop_move`、`worker_restart_requeues_task` 全过；
`kitchenos-worker-stub` 的帧协议不被改动。

- [x] 网格索引（浮点坐标 ↔ cell）与 `mask` 邻接一致
- [x] A* 到"工位相邻的可走格"，不可达时回事件而不是原地重试
- [x] 路径转逐帧 `move`；到达阈值后 `interact`；手上/目标状态从 shm 读
- [x] 无任务时每帧写 `stop`（Bridge 永远拿到有效槽）
- [x] 任务 accepted/done/failed 事件带 task_id/chef_id
- [x] Worker 重启后从事件与 shm 恢复，不重复完成同一任务

**Done when:** Check 全绿：os-loop 里两个厨师都经 Bridge 收到 move/interact，Worker 被
`SIGKILL` 后 Supervisor 拉起新代、Scheduler 重派在途任务。

## Progress

**2026-10-24：任务 03 落地。**

新增 `nav.{h,c}`（0.6 m 网格 A*）、`worker_task.{h,c}`（move→interact→work→done）、
重写 `src/worker.c`（mq 收任务、读 shm 起点、按 slot-interval 写输入槽、事件上报）。
`test_worker_nav` 17 项检查；os-loop 场景里两个厨师各收到 400+ move、13 interact，
且首次 interact 之前没有 stop。

实测：

```
cd bridge && ./build/test_worker_nav
  [test_worker_nav] 检查 17 项，失败 0 项
bash tools/run-bridge-scenarios.sh --only os-loop
  PASS os-loop: moves={0: 524, 1: 522}; interacts={0: 13, 1: 13}
```

双向证明：

| 破坏 | 失败 |
|---|---|
| `kos_nav_adjacent` 恒为 false | `path_respects_mask`、`grid_mask_is_symmetric_along_path` |
| `fill_interact` 改成发 stop | `slot_contains_interact`、`slot_contains_move_then_interact` |
| Worker 发布动作前强制改成 stop | os-loop 的 `worker_move_forwarded_each_chef`、`worker_interact_forwarded_each_chef`、`worker_slot_nonstop_move` |
| Scheduler 忽略 RECOVERED 事件 | os-loop 的 `worker_restart_detected`、`worker_restart_requeues_task`、`worker_restart_gets_new_task`、chef0 的 interact/non-stop 检查 |

过程中抓到的真实缺陷：移动预算把 3000 mm/s 当成了 3000 m/s，路径两步"瞬移"完，move
只剩 2/0 条；os-loop 的 move 检查先失败，修成 `/1e6` 后通过。

Worker 每次拉起时 Supervisor 传 `--generation N`：第 1 代不发 RECOVERED（避免和刚派发的任务竞态），
第 2 代起才发，Scheduler 把该厨师仍在 RUNNING 的任务改回 PENDING 并重新派发。os-loop 在
约 6 s 时 `SIGKILL` 一个 Worker，断言 `worker_restart_requeues_task` 与
`worker_restart_gets_new_task`。

**未做：** 资源信号量申请/租约（任务 04）；游戏内真实坐标/碰撞与交互结果确认（W10）。
