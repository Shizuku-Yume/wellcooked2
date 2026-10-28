# 01: Supervisor 守护进程与三进程骨架

**Effort:** os-core
**Task type:** feature
**Base commit:** <commit-sha>
**Covers:** W1, W2, W3, W4, W5

**Delivers:** `kitchenos-supervisor` 完整守护进程（daemonize + pid 文件锁 + fork/exec
Bridge/Scheduler/N Worker + SIGCHLD 收尸重启 + SIGALRM 健康检查 + SIGTERM 有序关闭与 IPC
清理 + SIGHUP 转发 + SIGUSR1 状态导出），以及 `kitchenos-scheduler` / `kitchenos-worker`
的最小真实进程骨架（挂接 shm、写日志、心跳、响应 SIGTERM）。

**Blocked by:** 无（Bridge 已完成）
**Files:** `bridge/include/kitchenos/{daemon,supervise,child}.h`、
`bridge/src/{daemon,supervise,child,supervisor,scheduler,worker}.c`、
`bridge/tools/silent_child.c`、`bridge/tests/test_supervisor.c`、`bridge/Makefile`、
`bridge/src/bridge.c`（只加 supervisor 心跳开关）、`.workflow/specs/os-core.md`
**Read first:** `.workflow/specs/os-core.md` 的进程树、信号、W1–W5；
`bridge/include/kitchenos/shm.h`；`bridge/src/bridge.c` 的 `run_loop`
**Covers:** W1, W2, W3, W4, W5
**Check:** `cd bridge && make && make test` 退出码 0；`test_supervisor` 覆盖 W1–W5 全部
检查；`bash tools/run-bridge-scenarios.sh` 仍全绿。

- [x] 运行目录解析（`--runtime-dir`/env/XDG//tmp）与 pid 文件 fcntl 锁
- [x] 标准 daemonize（fork/setsid/umask/chdir/dup2）+ `--foreground`
- [x] fork/exec Bridge、Scheduler、N Worker，参数与日志路径固定
- [x] 每子进程一条心跳管道；SIGALRM 健康检查；超时 SIGKILL 后重启
- [x] SIGCHLD 收尸 + 按 §6.1 退避 + `--crash-limit` 后 `given_up`
- [x] SIGTERM 按 Workers → Scheduler → Bridge 停子进程，超时 SIGKILL
- [x] 关闭末尾 unlink shm/sem、删除 pid 文件、留最终 `status.json`
- [x] SIGHUP 转发 Scheduler；SIGUSR1 原子写 `status.json`
- [x] Scheduler/Worker 骨架：挂接 shm（缺失则重试）、心跳、SIGTERM/SIGHUP 日志
- [x] 每个检查双向证明（临时破坏 → 观察到失败 → 恢复）

**Done when:** 上面的检查在 `make test` 与 `make test-san` 下都是绿的，且两项破坏性实验的
输出记录在 Progress 里。

## Progress

**2026-10-24：Phase 1 spec + Phase 2 Supervisor/skeleton 落地。**

模块：`daemon`（运行目录、pid fcntl 锁、daemonize + 就绪管道）、`supervise`（崩溃窗口/
退避/given_up 纯状态机）、`child`（Scheduler/Worker 共用骨架）、`mq`（队列名与幂等清理）；
三个新可执行文件由 `make` 生成，`LINK_SRCS` 显式排除四个 main。

实测：

```
cd bridge && make              exit=0
cd bridge && make test         === 6 test suites, 0 failed ===
                                framing 194 + json 166 + net 204 + protocol 226
                                + shm 95 + supervisor 67 = 952 项检查
bash tools/run-bridge-scenarios.sh
                               合计: 11 个场景, 通过 11, 失败 0
cd bridge && make test-san     原样失败：本机没有 libasan.so.8.0.0 /
                               libubsan.so.1.0.0（安装需 root）。非 root 绕过后：
LD_LIBRARY_PATH=/tmp/kos-san-libs/usr/lib64 \
  make test-san LDFLAGS="-pthread -lrt -L/tmp/kos-san-libs/lib"
                               === 6 test suites, 0 failed ===（952 项检查，
                               无 ASan/UBSan 报告）。临时库路径未写进 Makefile。
```

**双向证明**（临时破坏 → 观察失败 → 恢复 → 重跑）：

| 破坏的行为 | 观察到的失败 | 恢复后 |
|---|---|---|
| `spawn_due` 直接 return（不再重启） | 10 项失败，含 `restart_after_sigkill`、`restart_logged`、`crash_loop_gives_up` | 67/67 |
| `unlink_ipc` 直接 return | `cleanup_without_bridge`、`cleanup_logged`、`shutdown_logs_ipc_unlink` | 67/67 |
| `fcntl(F_SETLK)` 失败被忽略（仅测试目录） | `pidfile_acquire_blocks_second` | 67/67 |
| 崩溃上限条件改为永不成立 | `crash_limit_gives_up`、`crash_loop_gives_up`、`crash_loop_status_given_up`、`crash_loop_stops_spawning` | 67/67 |
| `setitimer` 不装填 | `health_tick_logged`、`silent_child_restarted`、`silent_child_new_pid` | 67/67 |
| SIGHUP 转发与 SIGUSR1 dump 停掉 | `sighup_forwarded`、`sighup_reaches_scheduler`、`status_dump_on_sigusr1` | 67/67 |

反证过程中发现并修掉一个测试自身的问题：shutdown 顺序断言只读 4 KiB 日志，慢跑时
健康检查行把 `child_stop_sent` 挤出缓冲区，导致假失败；改成 32 KiB 后稳定。

**本任务没做（下一阶段）：** Scheduler 仍只有骨架（无订单/策略/mq），Worker 仍只有骨架
（无 A*/输入槽），MQ 消息体布局、租约与 metrics 都未实现。下一任务：
`.workflow/tasks/os-core/02-scheduler-policies-and-dispatch.md`。
