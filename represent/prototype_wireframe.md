# KitchenOS Monitor 原型说明

## 页面目标
现场演示时，把“调度是否生效”从游戏画面中抽离出来，单屏呈现队列、资源锁、进程健康度与日志。

## 页面结构
- 顶部：系统名、运行状态、当前策略
- 左栏：Ready Queue；订单编号、菜品、优先级、状态
- 右栏：Process Health；Supervisor / Scheduler / Worker / Bridge
- 底部：资源锁占用、平均等待、实时事件日志

## 现场操作
1. 启动 Supervisor 与 Bridge。
2. 载入固定订单序列，观察 Scheduler 分发任务。
3. 在终端执行 `kill -9 Scheduler`。
4. 观察 SIGCHLD → 清理 → 自动重启 → 队列恢复。
5. 发送 SIGHUP，切换调度策略并继续运行。

## 原型边界
- 先做信息可见性与故障演示，不追求完整UI产品化。
- 数据先来自确定性实验；性能图表放入后续验收材料。