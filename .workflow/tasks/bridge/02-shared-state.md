# 02: 共享内存世界状态与输入槽

**Effort:** bridge
**Blocked by:** None
**Task type:** feature
**Covers:** W1, W3
**Delivers:** `core/` 的 `shm` 模块：`/kitchenos_state` 布局、世界状态 seqlock、每厨师一个
输入槽、就绪信号量 `/kitchenos_input_ready`，以及把它单独跑起来的单元测试。
**Files:** `core/include/kitchenos/shm.h`, `core/src/shm.c`, `core/tests/test_shm.c`
**Read first:** `.workflow/specs/bridge.md`（How it is built 的并发规则）,
`overcooked_os_course_project.md` §5.1「共享内存 + 信号量」与 §6.1 Bridge 职责。
**Done when:**
- [ ] 世界状态：Bridge 独占写、他人并发读，seqlock 读写一致（读者永不看到撕裂的中间态）
- [ ] 输入槽：每槽独立 seqlock，Worker 写完 `sem_post`，读者可判定"第 N 帧该槽是否就绪"
- [ ] 结构带版本号，版本不匹配时拒绝挂接并给出明确错误
- [ ] `make test` 里 seqlock 的一致性测试在并发读写下有确定结论
- [ ] `/dev/shm` 下创建，路径与名字可配置以便测试隔离

**Check:** `cd core && make test` 退出码 0，且 shm 组测试包含一个读写并发用例并通过。
