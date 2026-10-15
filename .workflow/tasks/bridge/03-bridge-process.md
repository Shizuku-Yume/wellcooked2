# 03: Bridge 进程主循环

**Effort:** bridge
**Blocked by:** 01, 02
**Task type:** feature
**Covers:** W2, W5, W6, W7
**Delivers:** 可执行的 `kitchenos-bridge`：TCP 双拓扑链路、按帧写共享内存、帧屏障与 idle 降级、
心跳超时暂停、断线重连、`logs/bridge.log`、SIGTERM/SIGINT 优雅退出与 IPC 清理。
**Files:** `core/include/kitchenos/{net,log,bridge}.h`, `core/src/{net,log,bridge}.c`,
`core/tests/test_net.c`
**Read first:** `.workflow/specs/bridge.md`（全篇）, 本 effort 的 01 与 02 产出的头文件。
**Done when:**
- [ ] 默认 `--link listen --port 12345`；`--link connect --host H --port P` 可切换（默认 14455）
- [ ] 每个 `state_update` 一帧：`sem_timedwait` 等 N 个槽，超时对缺失厨师下发 idle 并点名
- [ ] Scheduler/Worker 完全不存在时照常连接、照常按帧下发 idle，进程不退出
- [ ] 心跳超时 → 判定断开、暂停下发并记录；重连后恢复
- [ ] 非法 JSON、超长帧、截断帧 → 记录并丢弃，进程存活；失步则断开重连
- [ ] `SIGTERM` 1 秒内退出，`shm_unlink`/`sem_unlink` 清理干净，端口可立即重新绑定
- [ ] 日志包含：启动退出、连接与断开、每帧摘要、动作下发、缺失槽、心跳超时、解析失败、信号

**Check:** `cd core && make && ./build/kitchenos-bridge --link listen --port 12345 &` 后，
`tools/fake_plugin.py` 能完成 happy 场景；`kill -TERM` 后进程退出且 `ls /dev/shm | grep kitchenos`
为空。
