from pathlib import Path
from html import escape

W, H = 1280, 720
OUT = Path(__file__).parent / 'svg_output'
OUT.mkdir(parents=True, exist_ok=True)

BG = '#F8F9FA'
NAVY = '#0F172A'
TEXT = '#1E293B'
MUTED = '#64748B'
BLUE = '#2563EB'
ORANGE = '#D97706'
GREEN = '#059669'
YELLOW = '#EAB308'
RED = '#DC2626'
SLATE = '#334155'
GRID = '#E2E8F0'
WHITE = '#FFFFFF'
FONT = 'Microsoft YaHei, Arial'
MONO = 'Consolas, Courier New'


def esc(s):
    return escape(str(s), quote=True)


def t(x, y, text, size=24, fill=TEXT, weight='normal', anchor='start', family=FONT, opacity=1.0):
    return f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" font-weight="{weight}" fill="{fill}" text-anchor="{anchor}" opacity="{opacity}">{esc(text)}</text>'


def multiline(x, y, lines, size=22, fill=TEXT, weight='normal', leading=1.35, family=FONT):
    out = [f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" font-weight="{weight}" fill="{fill}">']
    for i, line in enumerate(lines):
        out.append(f'<tspan x="{x}" dy="{0 if i == 0 else size*leading}">{esc(line)}</tspan>')
    out.append('</text>')
    return ''.join(out)


def rect(x, y, w, h, fill=WHITE, stroke='none', sw=0, r=16, opacity=1.0):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}" opacity="{opacity}"/>'


def line(x1, y1, x2, y2, stroke=GRID, sw=2, dash=''):
    d = f' stroke-dasharray="{dash}"' if dash else ''
    return f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{stroke}" stroke-width="{sw}"{d}/>'


def circle(cx, cy, r, fill=WHITE, stroke='none', sw=0, opacity=1.0):
    return f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}" opacity="{opacity}"/>'


def pill(x, y, label, color=BLUE, w=None):
    w = w or max(76, len(label)*15+24)
    return rect(x, y, w, 30, color, 'none', 0, 15) + t(x+w/2, y+21, label, 15, WHITE, 'bold', 'middle')


def arrow(x1, y1, x2, y2, stroke=BLUE, sw=3, dash=''):
    d = f' stroke-dasharray="{dash}"' if dash else ''
    return f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{stroke}" stroke-width="{sw}" marker-end="url(#arrow)"{d}/>'


def base(title, page, kicker=None, dark=False):
    bg = NAVY if dark else BG
    title_fill = WHITE if dark else NAVY
    body_fill = '#CBD5E1' if dark else TEXT
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" data-pptx-page-role="content" font-family="{FONT}">']
    s.append('<defs><marker id="arrow" markerWidth="10" markerHeight="10" refX="8" refY="5" orient="auto"><path d="M0 0 L10 5 L0 10 Z" fill="#2563EB"/></marker><filter id="shadow" x="-20%" y="-20%" width="140%" height="150%"><feDropShadow dx="0" dy="4" stdDeviation="6" flood-color="#0F172A" flood-opacity="0.10"/></filter></defs>')
    s.append(f'<rect id="background" data-pptx-role="background" x="0" y="0" width="{W}" height="{H}" fill="{bg}"/>')
    s.append('<g id="header" data-pptx-bounds="56 38 1168 88">')
    if kicker:
        s.append(t(60, 58, kicker.upper(), 14, BLUE if not dark else '#93C5FD', 'bold'))
    s.append(t(60, 104, title, 32, title_fill, 'bold'))
    s.append(line(60, 126, 1220, 126, '#334155' if dark else GRID, 2))
    s.append('</g>')
    s.append(f'<g id="footer" data-pptx-bounds="60 680 1160 24">{t(60, 698, "WellCooked · 操作系统原理课程设计", 13, '#94A3B8' if dark else MUTED)}{t(1220, 698, f'{page:02d}', 13, '#94A3B8' if dark else MUTED, 'bold', 'end')}</g>')
    return s


def end(s):
    s.append('</svg>')
    return ''.join(s)


def slide1():
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" data-pptx-page-role="cover" font-family="{FONT}">']
    s.append('<defs><linearGradient id="bgGrad" x1="0" y1="0" x2="1" y2="1"><stop offset="0%" stop-color="#0F172A"/><stop offset="100%" stop-color="#1D4ED8"/></linearGradient><marker id="arrow" markerWidth="10" markerHeight="10" refX="8" refY="5" orient="auto"><path d="M0 0 L10 5 L0 10 Z" fill="#2563EB"/></marker><filter id="shadow" x="-20%" y="-20%" width="140%" height="150%"><feDropShadow dx="0" dy="8" stdDeviation="10" flood-color="#020617" flood-opacity="0.25"/></filter></defs>')
    s.append('<rect id="background" data-pptx-role="background" x="0" y="0" width="1280" height="720" fill="url(#bgGrad)"/>')
    s.append('<circle cx="1120" cy="-10" r="300" fill="#60A5FA" opacity="0.13"/><circle cx="1040" cy="720" r="260" fill="#F59E0B" opacity="0.10"/>')
    s.append('<g id="cover-title" data-pptx-bounds="72 120 610 390">')
    s.append(t(78, 154, '操作系统原理课程设计 · 小组汇报', 16, '#BFDBFE', 'bold'))
    s.append(t(78, 244, 'WellCooked', 64, WHITE, 'bold'))
    s.append(t(82, 296, '基于多任务调度的多人厨房协作系统', 28, '#E2E8F0', 'bold'))
    s.append(multiline(82, 360, ['把“厨房协作”抽象成操作系统问题：', '订单是作业，厨师是执行单元，锅具是有限资源。'], 22, '#DBEAFE', leading=1.45))
    s.append(pill(82, 468, '调度', BLUE, 92) + pill(184, 468, '并发', ORANGE, 92) + pill(286, 468, 'IPC', GREEN, 86) + pill(382, 468, '守护进程', '#7C3AED', 132))
    s.append('</g>')
    # original vector illustration
    s.append('<g id="cover-illustration" data-pptx-bounds="700 116 500 480">')
    s.append(rect(735, 154, 430, 334, '#F8FAFC', 'none', 0, 26, 0.96))
    s.append(t(770, 190, '厨房状态空间', 17, SLATE, 'bold'))
    # conveyor
    s.append(rect(780, 350, 340, 54, '#CBD5E1', 'none', 0, 27))
    for cx in [815, 885, 955, 1025, 1095]:
        s.append(circle(cx, 377, 14, '#94A3B8'))
    s.append(rect(824, 294, 68, 58, '#DBEAFE', BLUE, 2, 12) + t(858, 329, '订单', 16, BLUE, 'bold', 'middle'))
    s.append(rect(948, 294, 68, 58, '#FEF3C7', ORANGE, 2, 12) + t(982, 329, '资源', 16, ORANGE, 'bold', 'middle'))
    s.append(rect(1006, 428, 82, 58, '#D1FAE5', GREEN, 2, 12) + t(1047, 463, '交付', 16, GREEN, 'bold', 'middle'))
    s.append(arrow(894, 323, 940, 323, BLUE, 3))
    s.append(arrow(1018, 380, 1042, 426, BLUE, 3))
    # workers
    s.append(circle(814, 518, 30, BLUE) + t(814, 527, 'W0', 17, WHITE, 'bold', 'middle'))
    s.append(circle(916, 518, 30, ORANGE) + t(916, 527, 'W1', 17, WHITE, 'bold', 'middle'))
    s.append(arrow(838, 518, 886, 518, BLUE, 2, '6,6'))
    s.append(arrow(940, 518, 986, 518, BLUE, 2, '6,6'))
    s.append(t(770, 574, 'Scheduler', 18, WHITE, 'bold') + t(900, 574, 'Shared resources', 18, WHITE, 'bold'))
    s.append('</g>')
    s.append(t(82, 668, '小组实验设计汇报 · 方案阶段', 14, '#BFDBFE'))
    s.append(t(1198, 668, '01', 14, '#BFDBFE', 'bold', 'end'))
    return end(s)


def slide2():
    s = base('为什么做：课程要求 × 项目价值', 2, '01 / 立意')
    s.append('<g id="course-goal" data-pptx-bounds="60 160 520 430">')
    s.append(rect(60, 160, 520, 430, WHITE, GRID, 1, 22))
    s.append(t(92, 206, '课程要求', 22, NAVY, 'bold'))
    rows = [('进程 / 线程', '用Supervisor与Worker体现进程控制'), ('信号 / 守护', '心跳监控、故障重启、优雅退出'), ('IPC / 同步', '管道、消息队列、共享内存、信号量'), ('资源调度', '把订单与锅具转化为可比较的调度问题')]
    y = 258
    for i, (a, b) in enumerate(rows):
        s.append(circle(108, y-6, 16, [BLUE, ORANGE, GREEN, '#7C3AED'][i]) + t(108, y, str(i+1), 14, WHITE, 'bold', 'middle'))
        s.append(t(144, y, a, 18, NAVY, 'bold'))
        s.append(t(144, y+28, b, 15, MUTED))
        y += 78
    s.append('</g>')
    s.append('<g id="project-value" data-pptx-bounds="640 160 580 430">')
    s.append(rect(640, 160, 580, 430, NAVY, 'none', 0, 22))
    s.append(t(676, 206, '项目价值', 22, WHITE, 'bold'))
    value = [('真实载体', '游戏场景提供可复现的并发与资源冲突'), ('可量化', '得分、吞吐量、等待时间可用于比较'), ('可扩展', '从2名厨师逐步扩展到更多执行单元')]
    y = 270
    for i, (a, b) in enumerate(value):
        s.append(rect(678, y-30, 86, 34, [BLUE, ORANGE, GREEN][i], 'none', 0, 17) + t(721, y-7, a, 15, WHITE, 'bold', 'middle'))
        s.append(t(786, y-6, b, 18, '#E2E8F0'))
        y += 90
    s.append(rect(676, 500, 500, 52, '#1E293B', 'none', 0, 14) + t(700, 532, '核心判断：不是做“游戏脚本”，而是做“可观测的OS调度系统”。', 17, '#BFDBFE', 'bold'))
    s.append('</g>')
    return end(s)


def slide3():
    s = base('我们要做什么：MVP边界与验收闭环', 3, '02 / 范围')
    s.append('<g id="mvp-flow" data-pptx-bounds="60 145 1160 195">')
    s.append(t(60, 168, '第一阶段只锁定一条可跑通闭环', 20, NAVY, 'bold'))
    nodes = [('固定关卡', '1个基础场景'), ('2名厨师', '独立Worker'), ('确定订单', '可复现实验'), ('全流程', '取料→切配→烹饪→交付')]
    x = 60
    for i, (a,b) in enumerate(nodes):
        s.append(rect(x, 205, 250, 92, WHITE, GRID, 1, 18, 1.0))
        s.append(circle(x+32, 238, 16, [BLUE, ORANGE, GREEN, '#7C3AED'][i]) + t(x+32, 244, str(i+1), 14, WHITE, 'bold', 'middle'))
        s.append(t(x+60, 240, a, 18, NAVY, 'bold'))
        s.append(t(x+60, 270, b, 15, MUTED))
        if i < 3: s.append(arrow(x+262, 251, x+292, 251, BLUE, 2))
        x += 300
    s.append('</g>')
    s.append('<g id="scope-left" data-pptx-bounds="60 390 540 220">')
    s.append(rect(60, 390, 540, 220, '#EFF6FF', '#BFDBFE', 1, 20))
    s.append(t(88, 432, '必须完成', 20, BLUE, 'bold'))
    for i, line_text in enumerate(['FCFS / SJF / EDF / HRRN + 1种改进策略', 'Supervisor守护进程 + 心跳 + 自动重启', '至少4类IPC：Pipe / MQ / SHM / Semaphore']):
        s.append(circle(94, 475+i*42, 5, BLUE) + t(112, 481+i*42, line_text, 16, TEXT))
    s.append('</g>')
    s.append('<g id="scope-right" data-pptx-bounds="640 390 580 220">')
    s.append(rect(640, 390, 580, 220, '#FFF7ED', '#FED7AA', 1, 20))
    s.append(t(668, 432, '明确不做', 20, ORANGE, 'bold'))
    for i, line_text in enumerate(['不修改商业游戏源代码 / 不重写3D引擎', '不做黑盒强化学习端到端训练', '不维护多套不兼容前端外壳']):
        s.append(circle(674, 475+i*42, 5, ORANGE) + t(692, 481+i*42, line_text, 16, TEXT))
    s.append('</g>')
    return end(s)


def slide4():
    s = base('系统概要：四层多进程架构', 4, '03 / 架构')
    # nodes
    layers = [
        (60, 170, 1160, 82, '#EEF2FF', '#6366F1', '游戏宿主 / BepInEx + Harmony', '读取世界状态 · 注入按帧输入'),
        (60, 282, 1160, 92, '#EFF6FF', BLUE, 'Bridge · 游戏桥接进程', 'TCP长连接 · 状态同步 · 动作汇聚'),
        (60, 404, 1160, 92, '#ECFDF5', GREEN, 'Scheduler · 调度核心', '订单队列 · 工序DAG · 策略热切换'),
        (60, 526, 1160, 92, '#FFF7ED', ORANGE, 'Worker 0 / Worker 1 · 执行进程', '寻路与动作规划 · 共享内存写入')]
    for x,y,w,h,fill,accent,name,desc in layers:
        s.append(rect(x, y, w, h, fill, 'none', 0, 18))
        s.append(rect(x, y, 10, h, accent, 'none', 0, 5))
        s.append(t(x+30, y+35, name, 20, NAVY, 'bold'))
        s.append(t(x+30, y+66, desc, 16, MUTED))
    s.append(arrow(640, 252, 640, 276, BLUE, 3))
    s.append(arrow(640, 374, 640, 398, BLUE, 3))
    s.append(arrow(640, 496, 640, 520, BLUE, 3))
    s.append('<g id="supervisor" data-pptx-bounds="965 180 190 72">')
    s.append(rect(965, 180, 190, 72, NAVY, 'none', 0, 16) + t(1060, 208, 'Supervisor', 18, WHITE, 'bold', 'middle') + t(1060, 234, '守护 / 重启 / 收割', 14, '#CBD5E1', 'normal', 'middle'))
    s.append(arrow(965, 216, 900, 216, BLUE, 2, '6,6'))
    s.append('</g>')
    s.append(rect(60, 646, 1160, 40, NAVY, 'none', 0, 12) + t(640, 672, '设计原则：职责隔离，状态共享；业务崩溃时，守护层负责恢复，桥接层保持长连接。', 16, WHITE, 'bold', 'middle'))
    return end(s)


def slide5():
    s = base('调度核心：订单级排序 + 工序级并发', 5, '04 / 调度')
    s.append('<g id="mindmap" data-pptx-bounds="60 145 1160 485">')
    # central
    s.append(circle(640, 365, 92, NAVY) + t(640, 354, 'WellCooked', 24, WHITE, 'bold', 'middle') + t(640, 386, '调度核心', 18, '#BFDBFE', 'normal', 'middle'))
    branches = [
        (270, 238, BLUE, '订单级', ['FCFS', 'SJF', 'EDF', 'HRRN']),
        (1010, 238, ORANGE, '工序级', ['DAG依赖', '关键路径', '移动距离']),
        (270, 502, GREEN, '资源级', ['锅具信号量', '盘子队列', '临界区']),
        (1010, 502, '#7C3AED', '评估级', ['得分', '吞吐量', '平均等待'])]
    for x,y,c,title,items in branches:
        s.append(line(640, 365, x, y, c, 3))
        s.append(circle(x, y, 58, c) + t(x, y-7, title, 18, WHITE, 'bold', 'middle') + t(x, y+20, '模型', 14, WHITE, 'normal', 'middle'))
        yy = y + (88 if y < 365 else -106)
        s.append(rect(x-132, yy, 264, 76, WHITE, GRID, 1, 14))
        for i,it in enumerate(items):
            s.append(t(x-112, yy+22+i*18, '· '+it, 14, TEXT))
    s.append(rect(420, 594, 440, 42, '#EFF6FF', '#BFDBFE', 1, 12) + t(640, 621, '改进策略：松弛度 + 关键路径 + 曼哈顿距离', 17, BLUE, 'bold', 'middle'))
    s.append('</g>')
    return end(s)


def slide6():
    s = base('OS机制落地：IPC矩阵与守护进程', 6, '05 / 机制')
    s.append('<g id="ipc-matrix" data-pptx-bounds="60 145 660 485">')
    s.append(t(60, 166, '谁和谁通信？', 20, NAVY, 'bold'))
    headers = ['机制', '连接对象', '用途']
    xs = [60, 210, 420]
    widths = [150, 210, 300]
    for x,w,hdr in zip(xs,widths,headers):
        s.append(rect(x, 190, w, 44, NAVY, 'none', 0, 8) + t(x+14, 219, hdr, 16, WHITE, 'bold'))
    rows = [('Pipe', 'Supervisor ↔ 子进程', '心跳 / 保活'), ('Message Queue', 'Scheduler ↔ Worker', '带优先级任务分发'), ('Shared Memory', 'Bridge ↔ Worker', '世界状态 / 动作槽'), ('Semaphore', 'Worker ↔ 共享资源', '锅具与盘子互斥'), ('fcntl lock', '进程 ↔ 文件系统', '单实例 / 快照保护')]
    y = 234
    for i,row in enumerate(rows):
        fill = WHITE if i%2==0 else '#F1F5F9'
        s.append(rect(60, y, 660, 54, fill, GRID, 1, 0))
        s.append(t(74, y+34, row[0], 15, [BLUE, ORANGE, GREEN, '#7C3AED', RED][i], 'bold'))
        s.append(t(224, y+34, row[1], 15, TEXT))
        s.append(t(434, y+34, row[2], 15, TEXT))
        y += 54
    s.append('</g>')
    s.append('<g id="daemon-flow" data-pptx-bounds="770 160 450 470">')
    s.append(t(770, 166, 'Supervisor生命周期', 20, NAVY, 'bold'))
    steps = [('fork()', BLUE), ('setsid()', BLUE), ('umask / chdir', BLUE), ('重定向 /dev/null', BLUE), ('SIGALRM心跳', ORANGE), ('SIGCHLD收割', GREEN), ('异常重启', RED)]
    y=206
    for i,(label,c) in enumerate(steps):
        s.append(circle(808, y, 16, c) + t(808, y+5, str(i+1), 13, WHITE, 'bold', 'middle') + t(842, y+6, label, 17, TEXT, 'bold' if i<4 else 'normal'))
        if i<6: s.append(line(808, y+16, 808, y+40, GRID, 2))
        y += 53
    s.append(rect(770, 576, 450, 54, '#FEF2F2', '#FECACA', 1, 14) + t(792, 609, '故障注入：kill -9 Scheduler → 自动拉起', 16, RED, 'bold'))
    s.append('</g>')
    return end(s)


def slide7():
    s = base('原型与演示：让系统状态可见', 7, '06 / 原型')
    s.append('<g id="prototype" data-pptx-bounds="60 170 760 450">')
    s.append(rect(60, 170, 760, 450, '#0B1220', 'none', 0, 20))
    s.append(rect(84, 194, 712, 44, '#111827', 'none', 0, 10) + t(106, 222, 'WellCooked Monitor', 16, '#E2E8F0', 'bold') + circle(760, 216, 6, GREEN) + t(775, 222, 'LIVE', 13, '#86EFAC', 'bold'))
    # left pane
    s.append(rect(84, 260, 328, 328, '#111827', '#334155', 1, 12))
    s.append(t(108, 294, 'Ready Queue', 16, '#93C5FD', 'bold'))
    orders = [('O-01', '鱼排', BLUE), ('O-02', '汉堡', ORANGE), ('O-03', '沙拉', GREEN)]
    y=332
    for oid,food,c in orders:
        s.append(rect(108, y, 280, 52, '#1F2937', 'none', 0, 8) + circle(132, y+26, 7, c) + t(150, y+23, oid, 14, '#CBD5E1', 'bold') + t(204, y+23, food, 14, WHITE) + t(360, y+23, 'READY', 12, '#94A3B8', 'bold', 'end'))
        y += 66
    s.append(t(108, 552, '平均等待  18.4s', 15, '#CBD5E1') + t(108, 574, '资源锁     2 / 4', 15, '#CBD5E1'))
    # right pane
    s.append(rect(432, 260, 364, 328, '#111827', '#334155', 1, 12))
    s.append(t(456, 294, 'Process Health', 16, '#93C5FD', 'bold'))
    procs = [('Supervisor', 'UP', GREEN), ('Scheduler', 'UP', GREEN), ('Worker-0', 'RUN', ORANGE), ('Worker-1', 'WAIT', YELLOW), ('Bridge', 'UP', GREEN)]
    y=332
    for name,status,c in procs:
        s.append(t(456, y, name, 15, '#E2E8F0', 'bold') + pill(672, y-22, status, c, 78))
        y += 44
    s.append(line(456, 536, 770, 536, '#334155', 1))
    s.append(t(456, 562, 'log  12:04:18  heartbeat ok', 13, '#94A3B8', family=MONO))
    s.append(t(456, 582, 'log  12:04:19  dispatch O-01 → W0', 13, '#94A3B8', family=MONO))
    s.append('</g>')
    s.append('<g id="demo-script" data-pptx-bounds="880 170 340 450">')
    s.append(rect(880, 170, 340, 450, WHITE, GRID, 1, 20))
    s.append(t(912, 210, '现场演示脚本', 20, NAVY, 'bold'))
    demos = [('1', '启动守护进程'), ('2', '跑通订单闭环'), ('3', '注入故障 kill -9'), ('4', '观察自动重启'), ('5', '切换调度策略')]
    y=264
    for num,label in demos:
        s.append(circle(930, y, 18, [BLUE, BLUE, RED, GREEN, ORANGE][int(num)-1]) + t(930, y+6, num, 14, WHITE, 'bold', 'middle') + t(964, y+6, label, 17, TEXT, 'bold'))
        y += 58
    s.append(rect(912, 536, 276, 52, '#EFF6FF', '#BFDBFE', 1, 12) + t(1050, 568, '游戏画面 + 监控台同屏', 16, BLUE, 'bold', 'middle'))
    s.append('</g>')
    return end(s)


def slide8():
    s = base('难点、分工与下一步', 8, '07 / 计划', dark=False)
    s.append('<g id="risks" data-pptx-bounds="60 160 560 470">')
    s.append(rect(60, 160, 560, 470, NAVY, 'none', 0, 22))
    s.append(t(94, 204, '可能的难点', 22, WHITE, 'bold'))
    risks = [('接入', 'BepInEx / Harmony Hook 与跨平台网络', BLUE), ('并发', 'DAG依赖、共享状态与死锁风险', ORANGE), ('可靠性', '守护进程、信号抖动、异常自愈', RED), ('边界', '先保MVP，再做4厨师与模拟器', GREEN)]
    y=264
    for title,desc,c in risks:
        s.append(circle(112, y-5, 13, c) + t(140, y, title, 18, WHITE, 'bold') + t(220, y, desc, 15, '#CBD5E1'))
        y += 76
    s.append('</g>')
    s.append('<g id="plan" data-pptx-bounds="680 160 540 470">')
    s.append(rect(680, 160, 540, 470, WHITE, GRID, 1, 22))
    s.append(t(714, 204, '下一步：按模块拆开做', 22, NAVY, 'bold'))
    members = [('A', 'Supervisor / Daemon', BLUE), ('B', 'Scheduler / 策略', ORANGE), ('C', 'IPC / 共享内存', GREEN), ('D', 'Bridge / 原型监控', '#7C3AED')]
    y=258
    for tag,role,c in members:
        s.append(circle(730, y-4, 17, c) + t(730, y+2, tag, 14, WHITE, 'bold', 'middle') + t(766, y, role, 17, TEXT, 'bold'))
        s.append(line(766, y+18, 1152, y+18, GRID, 1))
        y += 66
    s.append(rect(714, 540, 472, 56, '#ECFDF5', '#A7F3D0', 1, 14) + t(950, 575, '目标：第6周可运行验收；每人有独立模块与提交记录。', 16, GREEN, 'bold', 'middle'))
    s.append('</g>')
    return end(s)


slides = [slide1(), slide2(), slide3(), slide4(), slide5(), slide6(), slide7(), slide8()]
for idx, content in enumerate(slides, 1):
    (OUT / f'{idx:02d}_slide.svg').write_text(content, encoding='utf-8')
print(f'wrote {len(slides)} SVG slides to {OUT}')
