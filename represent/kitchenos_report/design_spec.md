# Design Spec — KitchenOS 小组汇报

## I. Communication Contract
- Primary language: zh-CN
- Audience: 操作系统课程教师与同学；已掌握基础OS概念，但不了解本项目内部实现。
- Communication intent: 用课堂汇报方式说明已选题目的必要性、MVP范围、OS原理映射、概要架构、实验原型与主要风险。
- Audience outcome: 听众能在短时间内回答“做什么、为什么适合本课程、怎么落地、哪里最难”。
- Core message: KitchenOS把多人厨房协作转化为可观测、可比较、可恢复的多进程调度系统。
- Delivery context: 课堂现场讲解；PPT为主，讲解补充细节。
- Artifact afterlife: 作为后续编码、分工和阶段验收的共同基线。
- Reading mode: presentation
- Canvas: ppt169 / 1280×720
- Page count: 8
- Content divergence: balanced；保留源文件事实，重组为课堂叙事。

## II. Direction
- Mode: briefing
- Visual style: custom
- Style behavior: 浅灰底、深海军蓝结构、蓝橙绿语义色；用原生几何图示替代截图堆砌；卡片只承载真实模块，页面以流程、分层和对比为主。
- Image strategy: none；本次视觉素材均为可编辑SVG原生图形。

## III. Theme
- Motif: “订单—资源—进程”三种圆角节点和蓝色方向线反复出现。
- Continuity job: 让游戏隐喻始终服务于OS关系，不把汇报带成游戏展示。

## IV. Typography
- Target: Windows PowerPoint safe fonts
- Heading: Microsoft YaHei, Arial, bold
- Body: Microsoft YaHei, Arial
- Code: Consolas, Courier New
- Body size: 24px baseline; page title 32px; section label 14–16px; body 15–18px; footer 13px.

## V. Colors
- background: #F8F9FA
- primary: #0F172A
- secondary_bg: #FFFFFF
- accent: #2563EB
- secondary_accent: #D97706
- positive: #059669
- warning: #EAB308
- negative: #DC2626
- body_text: #1E293B
- secondary_text: #64748B
- divider: #E2E8F0

## VI. Icons
- Base identity: custom project-native geometry; no external icon dependency.
- Stroke language: 1.5–3px lines, 16–24px nodes.

## VII. Visualization References
- P04: qualitative architecture diagram; no numeric chart.
- P05: qualitative mind map; no numeric chart.
- P06: semantic IPC table.
- P07: prototype wireframe; scenario UI, not measured data.

## VIII. Resources
- Acquire Via: none
- Reason: classroom deck prioritizes editability and direct explanation; no external image is required to understand the design.

## IX. Content Outline
### Slide 01 — 项目立意
- Audience move: 先建立“厨房问题 = OS调度问题”的共同框架。
- Relationships: order — 订单、资源、执行单元三种隐喻共同指向调度核心。
- Page rhythm: anchor

### Slide 02 — 课程要求 × 项目价值
- Audience move: 说明项目不是游戏脚本，而是OS原理综合实践。
- Relationships: contrast — 课程要求与项目价值双栏对照。
- Page rhythm: dense

### Slide 03 — MVP边界与验收闭环
- Audience move: 让听众看到可控的第一阶段范围，避免“做不完”。
- Relationships: order — 固定场景→双厨师→确定订单→全流程闭环。
- Page rhythm: dense

### Slide 04 — 四层多进程架构
- Audience move: 解释模块边界与状态流向。
- Relationships: hierarchy — 游戏宿主、Bridge、Scheduler、Worker分层承接。
- Page rhythm: dense

### Slide 05 — 调度核心
- Audience move: 说明订单级、工序级、资源级、评估级四个观察面。
- Relationships: center-surround — 调度核心连接四类模型。
- Page rhythm: breathing

### Slide 06 — IPC与守护进程
- Audience move: 把课程要求映射为具体系统机制。
- Relationships: contrast — 左侧IPC矩阵，右侧Daemon生命周期。
- Page rhythm: dense

### Slide 07 — 原型与演示
- Audience move: 让系统状态可见，并给出可执行的现场演示脚本。
- Relationships: contrast — 监控原型与演示步骤并列。
- Page rhythm: dense

### Slide 08 — 难点、分工与下一步
- Audience move: 结束在“风险已识别、任务可拆、下一步明确”。
- Relationships: contrast — 风险与分工/计划对照。
- Page rhythm: anchor

## X. Production
- Speaker Notes: disabled; on-slide copy is intentionally short but self-contained for classroom explanation.
- Custom Animations: disabled.
- Narration Audio: disabled.
