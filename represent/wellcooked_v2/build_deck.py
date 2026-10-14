"""WellCooked 课堂汇报 PPT 构建脚本（v2）。

依赖：python-pptx、Pillow。坐标按 1280x720 设计稿（px）书写，统一换算为 EMU。
运行：python3 build_deck.py  →  输出到 represent/ 目录。
"""
from pathlib import Path

from lxml import etree
from PIL import Image
from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_CONNECTOR, MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.oxml.ns import qn
from pptx.util import Emu, Pt

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
IMG_GAMEPLAY = ROOT / "report_source/images/overcooked_gameplay.jpg"
IMG_KITCHEN = ROOT / "report_source/images/overcooked_kitchen.jpg"
OUT = ROOT / "WellCooked_课堂小组汇报_v2.pptx"

# ---------- 设计令牌（沿用旧版配色） ----------
BG = "F8FAFC"
NAVY = "0F172A"
NAVY2 = "1E293B"
TEXT = "1E293B"
MUTED = "64748B"
LINE = "E2E8F0"
WHITE = "FFFFFF"
BLUE = "2563EB"
ORANGE = "D97706"
GREEN = "059669"
RED = "DC2626"
PURPLE = "7C3AED"
CYAN = "0891B2"
LBLUE = "EFF6FF"
LORANGE = "FFF7ED"
LGREEN = "ECFDF5"
LRED = "FEF2F2"
LPURPLE = "F5F3FF"
LCYAN = "ECFEFF"
FONT = "Microsoft YaHei"
LATIN = "Arial"
MONO = "Consolas"

PX = 9525  # 1px @ 1280 宽 = 9525 EMU


def E(v):
    return Emu(int(round(v * PX)))


prs = Presentation()
prs.slide_width = E(1280)
prs.slide_height = E(720)
BLANK = prs.slide_layouts[6]


# ---------- 基础绘制 ----------
def _clean(shape):
    """去掉主题样式引用，避免默认阴影/描边。"""
    st = shape._element.find(qn("p:style"))
    if st is not None:
        shape._element.remove(st)


def _font(run, size, color, bold=False, mono=False, italic=False):
    f = run.font
    f.size = Pt(size)
    f.bold = bold
    f.italic = italic
    f.color.rgb = RGBColor.from_string(color)
    rpr = run._r.get_or_add_rPr()
    latin = MONO if mono else LATIN
    for tag, face in (("a:latin", latin), ("a:ea", FONT), ("a:cs", latin)):
        el = rpr.find(qn(tag))
        if el is None:
            el = etree.SubElement(rpr, qn(tag))
        el.set("typeface", face)


def _fill_tf(tf, text, size, color, bold, align, anchor, mono, spacing, margin):
    tf.word_wrap = True
    tf.auto_size = None
    ml, mt, mr, mb = margin
    tf.margin_left, tf.margin_top, tf.margin_right, tf.margin_bottom = E(ml), E(mt), E(mr), E(mb)
    tf.vertical_anchor = anchor
    paras = text if isinstance(text, list) else [text]
    for i, p_item in enumerate(paras):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = align
        p.line_spacing = spacing
        # 段落可为 str 或 [(文本, {覆盖参数})...] 富文本
        runs = p_item if isinstance(p_item, list) else [(p_item, {})]
        for seg, opt in runs:
            r = p.add_run()
            r.text = seg
            _font(r, opt.get("size", size), opt.get("color", color), opt.get("bold", bold), opt.get("mono", mono))


def text(sl, x, y, w, h, content, size=14, color=TEXT, bold=False, align=PP_ALIGN.LEFT,
         anchor=MSO_ANCHOR.TOP, mono=False, spacing=1.15, margin=(0, 0, 0, 0)):
    tb = sl.shapes.add_textbox(E(x), E(y), E(w), E(h))
    _fill_tf(tb.text_frame, content, size, color, bold, align, anchor, mono, spacing, margin)
    return tb


def box(sl, x, y, w, h, fill=WHITE, line=None, lw=1.0, radius=None, shape=None, dash=False,
        content=None, size=13, color=TEXT, bold=False, align=PP_ALIGN.CENTER,
        anchor=MSO_ANCHOR.MIDDLE, mono=False, margin=(6, 3, 6, 3), spacing=1.1):
    kind = shape or (MSO_SHAPE.ROUNDED_RECTANGLE if radius else MSO_SHAPE.RECTANGLE)
    s = sl.shapes.add_shape(kind, E(x), E(y), E(w), E(h))
    _clean(s)
    if fill is None:
        s.fill.background()
    else:
        s.fill.solid()
        s.fill.fore_color.rgb = RGBColor.from_string(fill)
    if line is None:
        s.line.fill.background()
    else:
        s.line.color.rgb = RGBColor.from_string(line)
        s.line.width = Pt(lw)
        if dash:
            s.line.dash_style = 4  # dash
    if radius and kind == MSO_SHAPE.ROUNDED_RECTANGLE:
        s.adjustments[0] = min(0.5, radius / min(w, h))
    if content is not None:
        _fill_tf(s.text_frame, content, size, color, bold, align, anchor, mono, spacing, margin)
    return s


def arrow(sl, x1, y1, x2, y2, color=MUTED, w=1.5, dash=False, head=True, both=False):
    c = sl.shapes.add_connector(MSO_CONNECTOR.STRAIGHT, E(x1), E(y1), E(x2), E(y2))
    _clean(c)
    c.line.color.rgb = RGBColor.from_string(color)
    c.line.width = Pt(w)
    ln = c.line._get_or_add_ln()
    if dash:
        pd = etree.SubElement(ln, qn("a:prstDash"))
        pd.set("val", "dash")
    if both:
        he = etree.SubElement(ln, qn("a:headEnd"))
        he.set("type", "triangle")
        he.set("w", "med")
        he.set("len", "med")
    if head:
        te = etree.SubElement(ln, qn("a:tailEnd"))
        te.set("type", "triangle")
        te.set("w", "med")
        te.set("len", "med")
    return c


def pill(sl, x, y, w, h, label, fill, color=WHITE, size=11, bold=True):
    return box(sl, x, y, w, h, fill=fill, radius=h / 2, content=label, size=size, color=color,
               bold=bold, margin=(2, 0, 2, 0))


def num(sl, cx, cy, r, label, fill, size=12):
    return box(sl, cx - r, cy - r, 2 * r, 2 * r, fill=fill, shape=MSO_SHAPE.OVAL, content=str(label),
               size=size, color=WHITE, bold=True, margin=(0, 0, 0, 0))


def picture(sl, path, x, y, w, h):
    """等比铺满目标框（居中裁切）。"""
    iw, ih = Image.open(path).size
    pic = sl.shapes.add_picture(str(path), E(x), E(y), E(w), E(h))
    tr, ir = w / h, iw / ih
    if ir > tr:
        c = (1 - tr / ir) / 2
        pic.crop_left = pic.crop_right = c
    else:
        c = (1 - ir / tr) / 2
        pic.crop_top = pic.crop_bottom = c
    return pic


def notes(sl, s):
    sl.notes_slide.notes_text_frame.text = s


def new_slide(page, kicker, title, dark=False):
    sl = prs.slides.add_slide(BLANK)
    box(sl, 0, 0, 1280, 720, fill=NAVY if dark else BG)
    text(sl, 60, 34, 700, 18, kicker, 11, BLUE, bold=True)
    text(sl, 60, 54, 1160, 42, title, 24, NAVY, bold=True, anchor=MSO_ANCHOR.MIDDLE)
    box(sl, 60, 104, 1160, 2, fill=LINE)
    text(sl, 60, 690, 400, 16, "WellCooked · 操作系统原理课程设计", 10, "94A3B8")
    text(sl, 1120, 690, 100, 16, f"{page:02d}", 10, "94A3B8", bold=True, align=PP_ALIGN.RIGHT)
    return sl


def table(sl, x, y, cols, header, rows, row_h, head_h=34, size=12, head_fill=NAVY,
          first_colors=None, mono_cols=(), zebra=True):
    """cols: 各列宽；rows: [[cell,...]]；首列可着色。"""
    total = sum(cols)
    box(sl, x, y, total, head_h, fill=head_fill)
    cx = x
    for i, hname in enumerate(header):
        text(sl, cx + 12, y, cols[i] - 16, head_h, hname, size, WHITE, bold=True, anchor=MSO_ANCHOR.MIDDLE)
        cx += cols[i]
    yy = y + head_h
    for r, row in enumerate(rows):
        box(sl, x, yy, total, row_h, fill=(WHITE if (r % 2 == 0 or not zebra) else "F1F5F9"))
        if first_colors:
            box(sl, x, yy, 4, row_h, fill=first_colors[r])
        cx = x
        for i, cell in enumerate(row):
            col = first_colors[r] if (first_colors and i == 0) else TEXT
            text(sl, cx + 12, yy, cols[i] - 18, row_h, cell, size - (1 if i in mono_cols else 0), col,
                 bold=(i == 0), anchor=MSO_ANCHOR.MIDDLE, mono=(i in mono_cols))
            cx += cols[i]
        box(sl, x, yy + row_h - 1, total, 1, fill=LINE)
        yy += row_h
    return yy


# =====================================================================
# 01 封面
# =====================================================================
sl = prs.slides.add_slide(BLANK)
box(sl, 0, 0, 1280, 720, fill=NAVY)
picture(sl, IMG_GAMEPLAY, 600, 0, 680, 720)
box(sl, 600, 0, 6, 720, fill=BLUE)
text(sl, 72, 150, 500, 20, "操作系统原理课程设计 · 课堂实验设计小组汇报", 13, "93C5FD", bold=True)
text(sl, 72, 190, 520, 80, "WellCooked", 54, WHITE, bold=True)
text(sl, 72, 282, 520, 34, "基于《胡闹厨房2》的厨房调度策略设计", 22, WHITE, bold=True)
text(sl, 72, 336, 490, 90, [
    "把厨房当成一台“操作系统”：订单是作业，工序是任务，",
    "厨师是处理机，锅具砧板盘子是临界资源。",
    "用 Linux 多进程、信号、IPC 与同步机制完成自动出餐。",
], 14, "CBD5E1", spacing=1.35)
for i, (lb, c) in enumerate([("进程调度", BLUE), ("同步互斥", ORANGE), ("进程通信", GREEN), ("守护进程", PURPLE)]):
    pill(sl, 72 + i * 104, 448, 94, 30, lb, c, size=12)
text(sl, 72, 650, 500, 18, "图片来源：Steam / Overcooked! 2 官方商店页面", 10, "64748B")
notes(sl, "各位老师同学好，我们组的项目叫 WellCooked，基于《胡闹厨房2》做厨房调度。"
          "核心思路是把厨房看成一台操作系统：订单是作业，工序是任务，厨师是处理机，锅、砧板、盘子是临界资源。"
          "整个系统用 Linux 的多进程、信号、进程间通信和同步机制，自动控制游戏里的厨师完成出餐。")

# =====================================================================
# 02 选题背景
# =====================================================================
sl = new_slide(2, "01 / 选题背景", "为什么《胡闹厨房2》天然是一个操作系统问题？")
picture(sl, IMG_KITCHEN, 60, 130, 470, 264)
box(sl, 60, 394, 470, 104, fill=NAVY)
text(sl, 80, 406, 430, 22, "多人实时协作的厨房动作游戏", 15, WHITE, bold=True)
text(sl, 80, 434, 430, 60, "玩家分工完成取料、切菜、烹饪、装盘与洗盘；订单限时、通道狭窄、厨具有限，"
                           "混乱的根源正是“缺乏调度”。", 12, "CBD5E1", spacing=1.3)
box(sl, 60, 514, 470, 150, fill=LBLUE, line="BFDBFE", radius=12)
text(sl, 80, 528, 430, 22, "呼应课件示例：医院急诊室", 14, BLUE, bold=True)
text(sl, 80, 556, 430, 100, [
    "病人 = 进程，医生 = 处理机，调度 = 动态优先级；",
    "病人去做检查时的碎片时间可以被利用。",
    [("厨房里同样如此：", {"bold": True}), ("锅在煮的时候，厨师不必空等，可以去切下一道菜。", {})],
], 12.5, TEXT, spacing=1.35)

text(sl, 570, 128, 650, 24, "一张订单 = 一条有前驱依赖的任务链", 16, NAVY, bold=True)
steps = [("取料", "输入", BLUE), ("切菜", "CPU 计算", ORANGE), ("烹饪", "I/O 等待", RED),
         ("装盘", "同步汇合", GREEN), ("交付", "完成事件", PURPLE)]
for i, (a, b, c) in enumerate(steps):
    x = 570 + i * 132
    box(sl, x, 162, 112, 84, fill=WHITE, line=LINE, radius=10)
    num(sl, x + 56, 186, 13, i + 1, c, 11)
    text(sl, x, 204, 112, 20, a, 14, NAVY, bold=True, align=PP_ALIGN.CENTER)
    text(sl, x, 225, 112, 16, b, 11, c, bold=True, align=PP_ALIGN.CENTER)
    if i < 4:
        arrow(sl, x + 114, 204, x + 130, 204, color="94A3B8", w=1.5)

text(sl, 570, 272, 650, 24, "多人协作反而更乱：三类典型冲突", 16, NAVY, bold=True)
conf = [
    ("两人抢同一块砧板", "都想切菜，谁先用？另一个人只能等", "互斥 / 临界区", BLUE, LBLUE),
    ("锅里的汤快烧焦了", "厨师还在给下一单切菜，没人去关火装盘", "截止期 / 优先级调度", RED, LRED),
    ("干净盘子用完了", "菜已做好却无盘可装，后续全线停滞", "有限资源 / 生产者消费者", GREEN, LGREEN),
]
for i, (a, b, tag, c, lc) in enumerate(conf):
    y = 306 + i * 118
    box(sl, 570, y, 650, 104, fill=WHITE, line=LINE, radius=10)
    box(sl, 570, y, 5, 104, fill=c)
    num(sl, 606, y + 52, 16, i + 1, c, 13)
    text(sl, 638, y + 24, 330, 24, a, 16, NAVY, bold=True)
    text(sl, 638, y + 54, 330, 22, b, 12.5, MUTED)
    pill(sl, 990, y + 36, 210, 32, tag, lc, color=c, size=12.5)
notes(sl, "为什么选《胡闹厨房2》？它本身就是一个调度问题。每张订单都是一条有依赖的任务链：取料、切菜、烹饪、装盘、交付。"
          "多人协作时会出现三类冲突：抢砧板是互斥，锅快烧焦是截止期和优先级，盘子用完是有限资源和生产者消费者。"
          "这和课件里急诊室的例子是一样的：锅在煮的时候厨师不该空等，要去做别的工序。")

# =====================================================================
# 03 概念建模
# =====================================================================
sl = new_slide(3, "02 / 概念建模", "建立映射：把厨房翻译成操作系统")
rows = [
    ["订单", "作业 Job", "到达时间、截止期、预计工时 → 进入订单队列"],
    ["一道菜的工序", "任务 Task", "DAG 节点，前驱完成才进入就绪队列"],
    ["厨师 ×2", "处理机 / Worker 进程", "每名厨师对应一个独立进程并发执行"],
    ["砧板", "互斥资源", "二值信号量，同一时刻只允许一人使用"],
    ["锅 ×2", "有限资源", "计数信号量，初值为 2"],
    ["盘子", "有界缓冲区", "empty / full 信号量，生产者消费者模型"],
    ["等锅、等空盘", "阻塞态 Blocked", "sem_wait 挂起，资源释放后 sem_post 唤醒"],
]
colors = [BLUE, BLUE, ORANGE, GREEN, GREEN, GREEN, RED]
table(sl, 60, 130, [170, 200, 400], ["厨房实体", "操作系统概念", "在系统中的体现"], rows, 64,
      size=13, first_colors=colors)
box(sl, 60, 624, 770, 36, fill=LBLUE, radius=8)
text(sl, 76, 624, 740, 36, [[("说明：", {"bold": True, "color": BLUE}),
                            ("这是应用层调度模型，不替换 Linux 内核调度器，而是用 OS 原理解决协作任务分配。", {})]],
     12, TEXT, anchor=MSO_ANCHOR.MIDDLE)

box(sl, 860, 130, 360, 530, fill=NAVY, radius=14)
text(sl, 884, 150, 320, 22, "关键类比", 12, "93C5FD", bold=True)
text(sl, 884, 174, 320, 34, "锅 = I/O 设备", 24, WHITE, bold=True)
pairs = [("放入食材", "发起 I/O 请求"), ("锅自动加热", "设备独立工作（类 DMA）"),
         ("烹饪完成提示", "I/O 完成中断"), ("厨师去切下一道菜", "CPU 与 I/O 重叠")]
for i, (a, b) in enumerate(pairs):
    y = 228 + i * 70
    box(sl, 884, y, 312, 58, fill=NAVY2, radius=8)
    text(sl, 900, y + 8, 290, 20, a, 13, "FCD34D", bold=True)
    text(sl, 900, y + 31, 290, 20, "→ " + b, 13, WHITE)
text(sl, 884, 516, 312, 130, [
    [("切菜、洗盘", {"bold": True, "color": WHITE}), ("需要厨师持续操作，相当于 CPU 密集任务；", {})],
    [("好调度 = ", {"bold": True, "color": "FCD34D"}), ("先把锅启动起来，再让厨师去切菜，别让人空等锅。", {})],
], 12.5, "CBD5E1", spacing=1.4)
notes(sl, "这一页是整个项目的建模基础。订单对应作业，工序是 DAG 里的任务，两名厨师是两个独立的 Worker 进程。"
          "砧板用二值信号量互斥，两口锅用初值为 2 的计数信号量，盘子是有界缓冲区。等锅、等盘子就是进程阻塞。"
          "右边是我们最重要的类比：锅就像 I/O 设备，放进去就自己煮，煮好相当于中断，好的调度要让 CPU 和 I/O 重叠。")

# =====================================================================
# 04 系统架构
# =====================================================================
sl = new_slide(4, "03 / 系统架构", "总体架构：五个进程各司其职，Linux 用户态多进程协同")
box(sl, 60, 124, 836, 546, fill="F1F5F9", line="CBD5E1", radius=12)
text(sl, 76, 132, 400, 18, "Linux（Fedora 原生 / WSL2）· 纯 POSIX 实现", 11, MUTED, bold=True)
# Supervisor
box(sl, 80, 156, 796, 64, fill=NAVY, radius=10)
text(sl, 100, 162, 300, 26, "Supervisor 守护进程", 16, WHITE, bold=True)
text(sl, 100, 190, 330, 20, "fork + setsid 脱离终端，后台常驻", 11.5, "93C5FD")
text(sl, 440, 164, 420, 48, ["fork / exec 拉起全部子进程 · pipe 心跳",
                             "SIGCHLD + waitpid 收割 · pid 文件 fcntl 锁"], 11.5, WHITE, spacing=1.35)
# Scheduler
box(sl, 180, 264, 596, 70, fill=WHITE, line="CBD5E1", radius=10)
box(sl, 180, 264, 6, 70, fill=ORANGE)
text(sl, 202, 272, 300, 24, "Scheduler 调度进程", 16, NAVY, bold=True)
text(sl, 202, 300, 560, 22, "订单队列 · 工序 DAG · 资源分配表 · FCFS / SJF / EDF / HRRN 策略", 12, MUTED)
# MQ label
text(sl, 520, 342, 250, 18, "POSIX 消息队列：任务 ↓ / 事件 ↑", 11, ORANGE, bold=True)
# Workers
for i, (x, c) in enumerate([(140, GREEN), (504, CYAN)]):
    box(sl, x, 374, 312, 64, fill=WHITE, line="CBD5E1", radius=10)
    box(sl, x, 374, 6, 64, fill=c)
    text(sl, x + 22, 382, 280, 22, f"Chef Worker {i}", 15, NAVY, bold=True)
    text(sl, x + 22, 408, 280, 20, "独立进程 · A* 寻路 · 动作执行", 12, MUTED)
arrow(sl, 380, 334, 296, 374, color=ORANGE, w=1.75, both=True)
arrow(sl, 576, 334, 660, 374, color=ORANGE, w=1.75, both=True)
# SHM
box(sl, 80, 480, 796, 52, fill=LGREEN, line="A7F3D0", radius=10)
text(sl, 100, 480, 760, 52, [[("共享内存 SHM  ", {"bold": True, "color": GREEN, "size": 14}),
                              ("世界状态（厨师 / 订单 / 厨具） + 每名厨师的输入槽 · seqlock + 信号量保护", {})]],
     12.5, TEXT, anchor=MSO_ANCHOR.MIDDLE)
arrow(sl, 296, 438, 296, 480, color=GREEN, w=1.75)
arrow(sl, 660, 438, 660, 480, color=GREEN, w=1.75)
text(sl, 306, 450, 120, 16, "写输入槽", 10.5, GREEN, bold=True)
text(sl, 670, 450, 120, 16, "写输入槽", 10.5, GREEN, bold=True)
arrow(sl, 800, 480, 800, 334, color=GREEN, w=1.5, dash=True)
text(sl, 808, 400, 70, 32, ["读世界", "状态"], 10.5, GREEN, bold=True)
# Bridge
box(sl, 180, 574, 596, 70, fill=WHITE, line="CBD5E1", radius=10)
box(sl, 180, 574, 6, 70, fill=BLUE)
text(sl, 202, 582, 300, 24, "Bridge 桥接进程", 16, NAVY, bold=True)
text(sl, 202, 610, 560, 22, "唯一连接游戏的进程 · 每帧写入世界状态、汇总输入槽 · 帧同步", 12, MUTED)
arrow(sl, 478, 574, 478, 532, color=BLUE, w=1.75, both=True)
# supervisor dashed fan-out
for x in (230, 700):
    arrow(sl, x, 220, x, 264, color=PURPLE, w=1.25, dash=True)
text(sl, 240, 232, 200, 18, "拉起 + 心跳监测（所有子进程）", 10.5, PURPLE, bold=True)
# Game side
box(sl, 924, 124, 296, 300, fill=LORANGE, line="FED7AA", radius=12)
text(sl, 944, 138, 260, 18, "Windows / Proton", 11, ORANGE, bold=True)
text(sl, 944, 160, 260, 28, "Overcooked! 2", 20, NAVY, bold=True)
box(sl, 944, 204, 256, 196, fill=WHITE, line="FED7AA", radius=10)
text(sl, 962, 214, 230, 22, "BepInEx 插件", 15, NAVY, bold=True)
text(sl, 962, 244, 226, 150, ["· Harmony 补丁读取游戏状态", "· 多角色输入注入", "· 每帧上报 / 等待指令",
                              "· 按 orders.json 生成订单"], 12.5, TEXT, spacing=1.5)
arrow(sl, 776, 609, 1192, 609, color=BLUE, w=2, both=True)
arrow(sl, 1192, 609, 1192, 424, color=BLUE, w=2, both=True)
text(sl, 900, 580, 170, 20, "TCP Socket · JSON", 12, BLUE, bold=True)
box(sl, 924, 444, 244, 116, fill=WHITE, line=LINE, radius=10)
text(sl, 940, 454, 214, 100, [[("隔离设计：", {"bold": True, "color": NAVY})],
                             "进程崩溃不断开游戏连接，", "重启后从共享内存恢复。"],
     12, MUTED, spacing=1.35)
notes(sl, "系统一共五个进程。最上面是 Supervisor 守护进程，负责拉起和监控所有子进程。Scheduler 做调度决策，"
          "通过 POSIX 消息队列把任务派给两个 Chef Worker，每个 Worker 控制一名厨师。"
          "世界状态放在共享内存里，Bridge 是唯一和游戏通信的进程，通过 TCP 连接游戏内的 BepInEx 插件。"
          "这样拆分的好处是隔离：任何业务进程崩溃都不会断开游戏连接，重启后从共享内存恢复状态。")

# =====================================================================
# 05 守护进程
# =====================================================================
sl = new_slide(5, "04 / 守护进程", "Supervisor：按课件标准流程创建，负责拉起、监控与自愈")
box(sl, 60, 124, 520, 546, fill=WHITE, line=LINE, radius=12)
text(sl, 84, 140, 460, 24, "① 创建流程（课件第 9 页七步）", 16, NAVY, bold=True)
dsteps = [("fork()", "父进程 exit(0)，子进程继续"), ("setsid()", "新建会话，脱离控制终端"),
          ("umask(0)", "清除文件权限掩码"), ('chdir("/")', "切换工作目录"),
          ("close(fd)", "关闭继承的全部文件描述符"), ("dup2()", "0/1/2 重定向到 /dev/null"),
          ("main loop", "进入守护主循环")]
for i, (a, b) in enumerate(dsteps):
    y = 180 + i * 68
    if i < 6:
        box(sl, 105, y + 38, 2, 30, fill=LINE)
    num(sl, 106, y + 20, 14, i + 1, PURPLE if i < 6 else GREEN, 12)
    box(sl, 132, y + 5, 150, 30, fill=LPURPLE if i < 6 else LGREEN, radius=6, content=a, mono=True, size=13,
        color=PURPLE if i < 6 else GREEN, bold=True)
    text(sl, 298, y + 5, 262, 30, b, 13, TEXT, anchor=MSO_ANCHOR.MIDDLE)

text(sl, 612, 128, 600, 24, "② 守护职责：主循环里做什么", 16, NAVY, bold=True)
duties = [
    ("单实例", "fcntl(F_SETLK)", "给 pid 文件加写锁，防止重复启动", BLUE),
    ("拉起子进程", "fork + exec + pipe", "启动 Scheduler / Worker / Bridge，每个子进程一条管道", ORANGE),
    ("心跳检测", "write / poll", "子进程周期写心跳，poll 超时即判定无响应", GREEN),
    ("周期巡检", "setitimer + SIGALRM", "每 2 秒触发一次健康检查（同课件示例）", CYAN),
    ("崩溃自愈", "SIGCHLD + waitpid", "收割僵尸进程，按退避策略重启：立即 → 2s → 10s", RED),
    ("优雅退出", "SIGTERM", "通知子进程退出，清理 SHM / 消息队列 / 信号量", PURPLE),
]
for i, (a, api, b, c) in enumerate(duties):
    x = 612 + (i % 2) * 308
    y = 164 + (i // 2) * 170
    box(sl, x, y, 296, 156, fill=WHITE, line=LINE, radius=10)
    box(sl, x, y, 296, 5, fill=c)
    text(sl, x + 18, y + 18, 260, 24, a, 16, NAVY, bold=True)
    box(sl, x + 18, y + 50, 260, 28, fill="F1F5F9", radius=6, content=api, mono=True, size=12, color=c, bold=True,
        align=PP_ALIGN.LEFT, margin=(10, 0, 6, 0))
    text(sl, x + 18, y + 90, 260, 60, b, 12.5, MUTED, spacing=1.35)
notes(sl, "守护进程严格按课件第 9 页的七步创建：fork 后父进程退出，setsid 脱离终端，清 umask，切换目录，"
          "关闭继承的文件描述符并重定向到 /dev/null，然后进入主循环。"
          "主循环里：用 fcntl 给 pid 文件加锁保证单实例；fork 加 exec 拉起子进程，每个子进程一条心跳管道；"
          "setitimer 每两秒触发 SIGALRM 巡检；子进程崩溃时收到 SIGCHLD，waitpid 收割后按退避策略重启；收到 SIGTERM 时优雅退出并清理 IPC 资源。")

# =====================================================================
# 06 IPC 与信号
# =====================================================================
sl = new_slide(6, "05 / 进程通信与信号", "六种 IPC 各管一段数据流，五种信号驱动进程控制")
ipc = [
    ["管道 Pipe", "pipe / poll", "Supervisor ↔ 子进程", "父子进程天然共享，心跳最轻量"],
    ["消息队列", "mq_send / mq_receive", "Scheduler ↔ Worker", "自带优先级，直接映射调度优先级"],
    ["共享内存", "shm_open / mmap", "Bridge ↔ Scheduler / Worker", "每帧世界状态零拷贝共享"],
    ["信号量", "sem_wait / sem_post", "Worker ↔ 厨具", "临界区互斥与有限资源计数"],
    ["文件锁", "fcntl(F_SETLK)", "Supervisor / 日志", "单实例运行、多进程安全写日志"],
    ["TCP Socket", "socket / connect", "Bridge ↔ 游戏插件", "跨越 Linux 与 Windows/Proton 边界"],
]
cc = [BLUE, ORANGE, GREEN, PURPLE, RED, CYAN]
yb = table(sl, 60, 124, [118, 196, 214, 262], ["IPC 机制", "关键调用", "通信双方", "选用理由"], ipc, 62,
           size=12.5, first_colors=cc, mono_cols=(1,))
box(sl, 60, yb + 16, 790, 44, fill=LBLUE, radius=8)
text(sl, 76, yb + 16, 760, 44, [[("分工原则：", {"bold": True, "color": BLUE}),
                                ("控制流走管道与信号，任务流走消息队列，数据流走共享内存，跨系统走 TCP。", {})]],
     13, TEXT, anchor=MSO_ANCHOR.MIDDLE)

box(sl, 876, 124, 344, 546, fill=NAVY, radius=12)
text(sl, 898, 142, 300, 24, "信号处理清单", 16, WHITE, bold=True)
sigs = [("SIGCHLD", "Supervisor", "子进程退出 → waitpid 收割并重启", RED),
        ("SIGALRM", "Supervisor", "setitimer 周期健康检查", "F59E0B"),
        ("SIGTERM", "全部进程", "优雅退出，清理子进程与 IPC", "60A5FA"),
        ("SIGHUP", "Scheduler", "热切换调度策略，无需重启", "34D399"),
        ("SIGUSR1", "Scheduler", "导出状态快照（临时文件 + rename）", "A78BFA")]
for i, (a, who, b, c) in enumerate(sigs):
    y = 178 + i * 80
    box(sl, 898, y, 300, 74, fill=NAVY2, radius=8)
    text(sl, 914, y + 10, 140, 22, a, 15, c, bold=True, mono=True)
    text(sl, 1050, y + 12, 136, 20, who, 11, "94A3B8", align=PP_ALIGN.RIGHT)
    text(sl, 914, y + 40, 276, 24, b, 12, WHITE)
text(sl, 898, 590, 300, 60, "统一用 sigaction 注册 + SA_RESTART，被中断的系统调用自动重启。",
     11.5, "CBD5E1", spacing=1.35)
notes(sl, "IPC 方面我们用了六种机制，各管一段：管道做 Supervisor 和子进程的心跳；消息队列做任务派发，"
          "它自带优先级，正好对应调度优先级；共享内存放每帧的世界状态；信号量保护厨具；文件锁保证单实例和安全写日志；TCP 跨越 Linux 和游戏所在的 Windows。"
          "信号有五种：SIGCHLD 收割重启，SIGALRM 周期巡检，SIGTERM 优雅退出，SIGHUP 热切换策略，SIGUSR1 导出快照，全部用 sigaction 注册。")

# =====================================================================
# 07 同步互斥
# =====================================================================
sl = new_slide(7, "06 / 同步与互斥", "厨具就是临界资源：四类经典同步问题 + 两类可靠性问题")
res = [
    ("砧板 · 互斥锁", "同一时刻只能一人切菜", "sem_wait(board);\n  cut();\nsem_post(board);", BLUE, LBLUE),
    ("锅 ×2 · 计数信号量", "第 3 个烹饪请求阻塞，调度器改派其他工序", "sem_init(&pot, 1, 2);\nsem_wait(&pot); cook();", ORANGE, LORANGE),
    ("盘子 · 生产者消费者", "脏盘回收与洗盘构成有界缓冲区", "empty / full / mutex\n三个信号量", GREEN, LGREEN),
    ("世界状态 · 读者写者", "Bridge 每帧独占写，Scheduler / Worker 并发读", "seqlock：写时版本号+1\n读前后版本一致才有效", PURPLE, LPURPLE),
]
for i, (a, b, code, c, lc) in enumerate(res):
    x = 60 + (i % 2) * 380
    y = 124 + (i // 2) * 276
    box(sl, x, y, 364, 262, fill=WHITE, line=LINE, radius=10)
    box(sl, x, y, 364, 5, fill=c)
    text(sl, x + 20, y + 22, 324, 26, a, 17, NAVY, bold=True)
    text(sl, x + 20, y + 56, 324, 44, b, 13, MUTED, spacing=1.3)
    box(sl, x + 20, y + 112, 324, 128, fill=lc, radius=8)
    text(sl, x + 36, y + 120, 300, 112, code.split("\n"), 13, c, mono=True, bold=True, spacing=1.45,
         anchor=MSO_ANCHOR.MIDDLE)

# 死锁卡
box(sl, 836, 124, 384, 276, fill=WHITE, line=LINE, radius=10)
box(sl, 836, 124, 384, 5, fill=RED)
text(sl, 856, 144, 340, 24, "死锁：检测与恢复", 17, NAVY, bold=True)
# RAG mini
A = (880, 214, 64, 40)
B = (880, 318, 64, 40)
P = (1012, 214, 64, 40)
Q = (1012, 318, 64, 40)
box(sl, *A, fill=LBLUE, line=BLUE, shape=MSO_SHAPE.OVAL, content="厨师A", size=11.5, color=BLUE, bold=True)
box(sl, *B, fill=LBLUE, line=BLUE, shape=MSO_SHAPE.OVAL, content="厨师B", size=11.5, color=BLUE, bold=True)
box(sl, *P, fill=LORANGE, line=ORANGE, content="锅", size=12, color=ORANGE, bold=True)
box(sl, *Q, fill=LGREEN, line=GREEN, content="盘子", size=12, color=GREEN, bold=True)
arrow(sl, 1012, 234, 944, 234, color=ORANGE, w=1.5)   # 锅 → A 分配
arrow(sl, 912, 254, 1030, 318, color=RED, w=1.5)      # A → 盘子 请求
arrow(sl, 1012, 338, 944, 338, color=GREEN, w=1.5)    # 盘子 → B 分配
arrow(sl, 912, 318, 1030, 254, color=RED, w=1.5)      # B → 锅 请求
text(sl, 1090, 196, 124, 180, ["A 持锅等盘", "B 持盘等锅", "→ 环路等待", "",
                               "定期查环，", "让一方释放", "并回退重排"], 11.5, MUTED, spacing=1.3)
# 持锁崩溃卡
box(sl, 836, 414, 384, 256, fill=WHITE, line=LINE, radius=10)
box(sl, 836, 414, 384, 5, fill=NAVY)
text(sl, 856, 434, 340, 24, "持锁进程崩溃：健壮锁", 17, NAVY, bold=True)
text(sl, 856, 468, 344, 24, "持锁 Worker 被 kill，其他进程会永久阻塞。", 12.5, MUTED)
box(sl, 856, 512, 344, 96, fill="F1F5F9", radius=8)
text(sl, 870, 518, 320, 84, ["PTHREAD_MUTEX_ROBUST", "+ PTHREAD_PROCESS_SHARED", "lock() 返回 EOWNERDEAD →",
                             "恢复厨具状态 + consistent()"], 11.5, NAVY, mono=True, bold=True, spacing=1.3,
     anchor=MSO_ANCHOR.MIDDLE)
text(sl, 856, 620, 344, 40, "兜底：锁租约超时，Supervisor 强制回收。", 12.5, MUTED)
notes(sl, "同步互斥是把厨具当成临界资源。砧板是互斥锁；两口锅是初值为 2 的计数信号量，第三个烹饪请求会阻塞，调度器改派其他工序；"
          "盘子回收和洗盘是生产者消费者问题；世界状态 Bridge 每帧写、其他进程并发读，是读者写者问题，我们用 seqlock 实现。"
          "另外考虑两类可靠性问题：死锁，比如 A 拿锅等盘、B 拿盘等锅，用资源分配图查环后回退；"
          "持锁进程崩溃，用健壮互斥锁，下一个加锁者拿到 EOWNERDEAD 后恢复状态。")

# =====================================================================
# 08 两级调度
# =====================================================================
sl = new_slide(8, "07 / 调度设计", "两级调度：订单级选“先做哪单”，工序级定“谁做哪步”")
text(sl, 60, 124, 450, 24, "① 订单级（作业调度）", 16, NAVY, bold=True)
algos = [("FCFS", "先来先服务", "按到达时间排序", "基线策略，最直观", BLUE),
         ("SJF", "短作业优先", "按剩余工时排序", "吞吐高，长单可能饥饿", ORANGE),
         ("EDF", "最早截止期优先", "按截止时间排序", "压低超时率", RED),
         ("HRRN", "高响应比优先", "R = (等待 + 服务) / 服务", "兼顾长短单，天然抗饥饿", PURPLE)]
for i, (k, name, rule, feat, c) in enumerate(algos):
    y = 158 + i * 128
    box(sl, 60, y, 450, 116, fill=WHITE, line=LINE, radius=10)
    pill(sl, 78, y + 18, 72, 28, k, c, size=12.5)
    text(sl, 162, y + 18, 320, 28, name, 16, NAVY, bold=True, anchor=MSO_ANCHOR.MIDDLE)
    text(sl, 78, y + 58, 410, 22, rule, 13, c, bold=True, mono=(k == "HRRN"))
    text(sl, 78, y + 84, 410, 22, feat, 12.5, MUTED)

text(sl, 540, 124, 680, 24, "② 工序级（进程调度）：两名厨师并行推进工序 DAG", 16, NAVY, bold=True)
box(sl, 540, 158, 680, 190, fill=WHITE, line=LINE, radius=10)
text(sl, 560, 168, 400, 18, "示例：洋葱汤的工序 DAG", 12, MUTED, bold=True)
nodes = [("取洋葱", 560), ("切洋葱", 668), ("下锅煮", 776), ("装盘", 996), ("交付", 1104)]
for i, (lb, x) in enumerate(nodes):
    crit = i in (1, 2)
    box(sl, x, 202, 96, 40, fill=LORANGE if crit else LBLUE, line=ORANGE if crit else BLUE, radius=8,
        content=lb, size=13, color=ORANGE if crit else BLUE, bold=True)
for x1, x2 in ((656, 668), (764, 776), (1092, 1104)):
    arrow(sl, x1, 222, x2, 222, color=MUTED, w=1.5)
arrow(sl, 872, 222, 996, 222, color=MUTED, w=1.5)
text(sl, 878, 200, 116, 18, "煮 5s（I/O）", 10.5, RED, bold=True, align=PP_ALIGN.CENTER)
box(sl, 776, 276, 96, 36, fill="F1F5F9", line="94A3B8", radius=8, content="取盘子", size=12.5, color=MUTED, bold=True)
arrow(sl, 872, 294, 1044, 242, color="94A3B8", w=1.5, dash=True)
text(sl, 560, 262, 210, 74, ["橙色：关键路径（最长链）", "灰色：可与烹饪并行的分支"], 11.5, MUTED, spacing=1.4)
text(sl, 900, 290, 310, 40, "就绪条件：前驱完成 + 所需厨具空闲", 12, NAVY, bold=True, anchor=MSO_ANCHOR.MIDDLE)

text(sl, 540, 364, 680, 22, "调度规则：空闲厨师从就绪队列取优先级最高的工序", 14, NAVY, bold=True)
text(sl, 540, 392, 680, 22, "优先级 = 订单紧迫度 + 关键路径 + 厨具空闲 + 等待时长 − 移动距离", 12.5, MUTED, mono=False)

box(sl, 540, 424, 680, 246, fill=WHITE, line=LINE, radius=10)
text(sl, 560, 436, 640, 20, "甘特图：锅在煮的时候，厨师去做别的（CPU 与 I/O 重叠）", 12.5, NAVY, bold=True)
lanes = [("厨师 0", 480), ("锅 0", 540), ("厨师 1", 600)]
X0, UNIT = 640, 80
for lb, y in lanes:
    text(sl, 560, y, 72, 40, lb, 12.5, NAVY, bold=True, anchor=MSO_ANCHOR.MIDDLE)
    box(sl, X0, y + 38, 560, 1, fill=LINE)
bars = [(480, 0, 1, "取料", BLUE), (480, 1, 2, "切洋葱", ORANGE), (480, 3, 2, "切下一单", "94A3B8"),
        (480, 5, 2, "装盘交付", GREEN),
        (540, 3, 2, "煮汤（自动）", RED),
        (600, 0, 2, "洗盘", CYAN), (600, 2, 2, "取盘子", "94A3B8"), (600, 4, 2, "切番茄", ORANGE)]
for y, s0, d, lb, c in bars:
    box(sl, X0 + s0 * UNIT + 2, y + 6, d * UNIT - 4, 28, fill=c, radius=6, content=lb, size=11, color=WHITE,
        bold=True, margin=(2, 0, 2, 0))
text(sl, X0, 644, 560, 18, "时间 →", 10.5, MUTED)
notes(sl, "调度分两级，对应作业调度和进程调度。订单级决定先做哪一单，实现 FCFS、SJF、EDF 和 HRRN 四种策略，"
          "HRRN 同时考虑等待时间和服务时间，天然防止长订单饿死。工序级是更有深度的部分：每道菜展开成一张 DAG，"
          "前驱完成且厨具空闲的工序进入就绪队列，空闲厨师取优先级最高的执行，优先推进关键路径。"
          "下面的甘特图显示：锅在煮汤的时候，厨师 0 已经去切下一单，这就是 CPU 与 I/O 重叠。")

# =====================================================================
# 09 运行时序
# =====================================================================
sl = new_slide(9, "08 / 运行时序", "一张订单在系统里怎么流转：从游戏状态到厨师动作")
life = [("游戏插件", 180, ORANGE), ("Bridge", 415, BLUE), ("共享内存", 650, GREEN),
        ("Scheduler", 885, "B45309"), ("Worker 0", 1120, CYAN)]
for lb, x, c in life:
    box(sl, x - 90, 124, 180, 40, fill=c, radius=8, content=lb, size=14, color=WHITE, bold=True)
    arrow(sl, x, 164, x, 648, color="CBD5E1", w=1.25, dash=True, head=False)
LX = {lb: x for lb, x, _ in life}
msgs = [
    ("游戏插件", "Bridge", "① TCP 上报：厨师/订单/厨具"),
    ("Bridge", "共享内存", "② 写入世界状态（seqlock）"),
    ("共享内存", "Scheduler", "③ 读状态：新订单到达"),
    ("Scheduler", "Worker 0", "④ mq_send：切洋葱"),
    ("Worker 0", "Worker 0", "⑤ sem_wait(砧板)"),
    ("Worker 0", "共享内存", "⑥ A* 寻路 → 写输入槽"),
    ("共享内存", "Bridge", "⑦ 汇总两名厨师输入"),
    ("Bridge", "游戏插件", "⑧ 下发输入，厨师开切"),
    ("Worker 0", "Worker 0", "⑨ sem_post(砧板)"),
    ("Worker 0", "Scheduler", "⑩ mq_send：完成，后继就绪"),
]
for i, (a, b, lb) in enumerate(msgs):
    y = 196 + i * 46
    xa, xb = LX[a], LX[b]
    if a == b:
        box(sl, xa - 7, y - 12, 14, 26, fill=CYAN)
        text(sl, xa - 250, y - 12, 234, 24, lb, 12, CYAN, bold=True, mono=True, align=PP_ALIGN.RIGHT,
             anchor=MSO_ANCHOR.MIDDLE)
        continue
    c = next(col for n, _, col in life if n == a)
    arrow(sl, xa, y + 6, xb, y + 6, color=c, w=1.75)
    lx, rx = min(xa, xb), max(xa, xb)
    text(sl, lx + 8, y - 16, rx - lx - 16, 20, lb, 12, TEXT, bold=True, align=PP_ALIGN.CENTER)
box(sl, 60, 656, 1160, 28, fill=LBLUE, radius=6)
text(sl, 76, 656, 1130, 28, "循环直到装盘交付；每一次调度决策与资源申请 / 释放都写入 decisions.jsonl 与 events.jsonl，供复盘与监控台使用。",
     11.5, BLUE, bold=True, anchor=MSO_ANCHOR.MIDDLE)
notes(sl, "这张时序图展示一张订单怎么在系统里流转。游戏插件每帧通过 TCP 上报状态，Bridge 写进共享内存；"
          "Scheduler 读到新订单，拆成工序，用消息队列把“切洋葱”派给 Worker 0。Worker 先 sem_wait 申请砧板，"
          "然后寻路，把每帧输入写到共享内存的输入槽，Bridge 汇总后发回游戏，厨师就开始切菜。"
          "切完 sem_post 释放砧板，再通过消息队列上报完成，后继工序进入就绪队列。")

# =====================================================================
# 10 实现路径
# =====================================================================
sl = new_slide(10, "09 / 实现路径", "站在开源项目的肩膀上：游戏接入怎么落地")
refs = [
    ["overcooked-supercharged", "hpmv", "状态序列化、输入注入、帧级控制框架", BLUE],
    ["Overcooked2-TAS", "gua248", "最多四角色的按帧输入接管、插件结构", ORANGE],
    ["OvercookedAI", "Extremelyd1", "移动 / 拿放 / 切菜 / 装盘等高层动作分解", GREEN],
    ["oc2-modding", "toasterparty", "BepInEx + Harmony 工程与构建方式", PURPLE],
    ["overcooked_ai", "HumanCompatibleAI", "二维仿真环境，快速验证调度算法", CYAN],
]
box(sl, 60, 124, 700, 34, fill=NAVY)
text(sl, 76, 124, 250, 34, "参考项目", 12.5, WHITE, bold=True, anchor=MSO_ANCHOR.MIDDLE)
text(sl, 336, 124, 400, 34, "我们借鉴的部分", 12.5, WHITE, bold=True, anchor=MSO_ANCHOR.MIDDLE)
for i, (repo, owner, use, c) in enumerate(refs):
    y = 158 + i * 60
    box(sl, 60, y, 700, 60, fill=WHITE if i % 2 == 0 else "F1F5F9")
    box(sl, 60, y, 4, 60, fill=c)
    text(sl, 76, y + 8, 250, 22, repo, 13, c, bold=True, mono=True)
    text(sl, 76, y + 32, 250, 18, owner, 11, MUTED, mono=True)
    text(sl, 336, y, 410, 60, use, 13, TEXT, anchor=MSO_ANCHOR.MIDDLE)
    box(sl, 60, y + 59, 700, 1, fill=LINE)
box(sl, 60, 474, 700, 196, fill=WHITE, line=LINE, radius=10)
text(sl, 80, 488, 660, 22, "双实验室部署：同一份 POSIX 代码，两种运行环境", 15, NAVY, bold=True)
labs = [("实验室甲 · Fedora + Proton", "OS 核心跑在 Fedora 原生；游戏经 Proton 运行，插件经 localhost TCP 连 Bridge", BLUE, LBLUE),
        ("实验室乙 · WSL2 + Windows", "OS 核心跑在 WSL2；游戏在 Windows 原生运行，TCP 跨越 WSL2 与 Windows", GREEN, LGREEN)]
for i, (a, b, c, lc) in enumerate(labs):
    x = 80 + i * 334
    box(sl, x, 522, 318, 132, fill=lc, radius=8)
    text(sl, x + 16, 534, 290, 22, a, 13.5, c, bold=True)
    text(sl, x + 16, 564, 290, 80, b, 12, TEXT, spacing=1.4)

text(sl, 790, 124, 430, 24, "游戏接入链路", 16, NAVY, bold=True)
chain = [("Overcooked! 2", "Unity / Mono 游戏本体", ORANGE),
         ("BepInEx 5", "启动时加载我们的插件 DLL", PURPLE),
         ("Harmony 补丁", "挂钩游戏方法：读状态、注入输入", GREEN),
         ("TCP 客户端", "JSON 协议连到 Linux 侧 Bridge", BLUE)]
for i, (a, b, c) in enumerate(chain):
    y = 158 + i * 66
    box(sl, 790, y, 430, 50, fill=WHITE, line=LINE, radius=8)
    box(sl, 790, y, 5, 50, fill=c)
    text(sl, 808, y, 150, 50, a, 14, c, bold=True, anchor=MSO_ANCHOR.MIDDLE)
    text(sl, 960, y, 250, 50, b, 12.5, TEXT, anchor=MSO_ANCHOR.MIDDLE)
    if i < 3:
        arrow(sl, 1005, y + 50, 1005, y + 66, color="94A3B8", w=1.5)
box(sl, 790, 426, 430, 244, fill=NAVY, radius=10)
text(sl, 810, 440, 390, 18, "通信协议示例（JSON）", 12, "93C5FD", bold=True)
text(sl, 810, 468, 396, 190, [
    [("// 插件 → Bridge：状态上报", {"color": "94A3B8"})],
    '{"type":"state_update",',
    ' "chefs":[{"id":0,"holding":"onion"}],',
    ' "resources":{"pot_0":"cooking"}}',
    "",
    [("// Bridge → 插件：控制指令", {"color": "94A3B8"})],
    '{"type":"action","chef_id":1,',
    ' "action":"interact","target":"pot_0"}',
], 11.5, "E2E8F0", mono=True, spacing=1.3)
notes(sl, "游戏接入参考了几个开源项目：overcooked-supercharged 提供状态读取和输入注入框架，Overcooked2-TAS 提供多角色按帧接管，"
          "OvercookedAI 参考高层动作分解，oc2-modding 参考插件工程，overcooked_ai 用来快速验证算法。"
          "具体链路是：BepInEx 加载插件，Harmony 补丁读取状态、注入输入，再用 JSON 通过 TCP 连到 Linux 侧的 Bridge。"
          "我们有两个实验室环境，Fedora 加 Proton 和 WSL2 加 Windows，跑的是同一份 POSIX 代码。")

# =====================================================================
# 11 实验与演示
# =====================================================================
sl = new_slide(11, "10 / 实验与演示", "同一输入下比较策略，现场演示调度与故障自愈")
box(sl, 60, 124, 700, 176, fill=WHITE, line=LINE, radius=10)
text(sl, 80, 138, 660, 24, "受控对比实验", 16, NAVY, bold=True)
text(sl, 80, 168, 660, 22, "同一地图、同一订单序列、同一动作执行器，只切换调度策略。", 13, MUTED)
for i, (k, c) in enumerate([("FCFS", BLUE), ("SJF", ORANGE), ("EDF", RED), ("HRRN", PURPLE), ("改进策略", GREEN)]):
    pill(sl, 80 + i * 96, 202, 86, 28, k, c, size=12)
mets = ["游戏得分", "完成订单数", "超时率", "平均等待", "厨师利用率", "厨具等待"]
for i, m in enumerate(mets):
    box(sl, 80 + i * 108, 246, 100, 36, fill="F1F5F9", radius=6, content=m, size=12, color=NAVY, bold=True)

box(sl, 60, 316, 700, 354, fill=NAVY, radius=12)
text(sl, 82, 330, 450, 24, "WellCooked Monitor · 实时调度监控台", 14, WHITE, bold=True)
pill(sl, 632, 330, 108, 24, "策略：EDF", PURPLE, size=11)
box(sl, 82, 366, 300, 286, fill=NAVY2, radius=8)
text(sl, 98, 376, 270, 20, "订单队列", 12.5, "93C5FD", bold=True)
ords = [("O-01 洋葱汤", "RUNNING", "34D399"), ("O-02 沙拉", "READY", "60A5FA"),
        ("O-03 番茄汤", "BLOCKED 等锅", "F59E0B"), ("O-04 沙拉", "WAIT", "94A3B8")]
for i, (a, s, c) in enumerate(ords):
    y = 406 + i * 60
    box(sl, 98, y, 268, 48, fill=NAVY, radius=6)
    text(sl, 112, y, 130, 48, a, 12.5, WHITE, bold=True, anchor=MSO_ANCHOR.MIDDLE)
    text(sl, 230, y, 124, 48, s, 11, c, bold=True, align=PP_ALIGN.RIGHT, anchor=MSO_ANCHOR.MIDDLE, mono=True)
box(sl, 396, 366, 344, 286, fill=NAVY2, radius=8)
text(sl, 412, 376, 300, 20, "进程与资源", 12.5, "93C5FD", bold=True)
procs = [("Supervisor", "healthy", "34D399"), ("Scheduler", "RUNNING", "34D399"),
         ("Worker-0", "CUTTING", "60A5FA"), ("Worker-1", "WAIT pot", "F59E0B")]
for i, (a, s, c) in enumerate(procs):
    y = 404 + i * 30
    text(sl, 412, y, 150, 24, a, 12, WHITE, mono=True, anchor=MSO_ANCHOR.MIDDLE)
    text(sl, 580, y, 144, 24, s, 12, c, bold=True, mono=True, align=PP_ALIGN.RIGHT, anchor=MSO_ANCHOR.MIDDLE)
box(sl, 412, 530, 312, 1, fill="334155")
text(sl, 412, 540, 316, 104, ["[DISPATCH] O-01/cut -> W0", "[RESOURCE] pot-0 locked by W1",
                              "[SIGCHLD ] scheduler exited", "[RESTART ] scheduler pid 4121"],
     10.5, "94A3B8", mono=True, spacing=1.35)

text(sl, 790, 124, 430, 24, "现场演示脚本", 16, NAVY, bold=True)
demo = [("启动", "./kitchend start", "守护进程拉起全部子进程"),
        ("自动出餐", "load orders.json", "两名厨师在游戏中协作完成订单"),
        ("热切换策略", "kill -HUP <scheduler>", "FCFS → EDF，对比监控指标"),
        ("故障注入", "kill -9 <scheduler>", "SIGCHLD → 自动重启 → 恢复出餐"),
        ("复盘", "runs/*/metrics.json", "查看日志与各策略实验结果")]
for i, (a, cmd, b) in enumerate(demo):
    y = 158 + i * 102
    box(sl, 790, y, 430, 90, fill=WHITE, line=LINE, radius=10)
    num(sl, 818, y + 30, 14, i + 1, [BLUE, GREEN, PURPLE, RED, ORANGE][i], 12)
    text(sl, 844, y + 16, 150, 26, a, 15, NAVY, bold=True)
    box(sl, 990, y + 16, 214, 28, fill="F1F5F9", radius=6, content=cmd, mono=True, size=11.5, color=NAVY,
        bold=True, align=PP_ALIGN.LEFT, margin=(10, 0, 6, 0))
    text(sl, 844, y + 54, 360, 24, b, 12.5, MUTED)
notes(sl, "实验方面，我们固定地图、订单序列和动作执行器，只切换调度策略，比较得分、完成数、超时率、等待时间和利用率。"
          "左边是监控台原型，能看到订单队列、进程状态和调度事件。"
          "现场演示分五步：启动守护进程；两名厨师自动出餐；用 kill -HUP 热切换策略；用 kill -9 杀掉 Scheduler，"
          "展示守护进程通过 SIGCHLD 发现并自动重启；最后看日志和实验结果。")

# =====================================================================
# 12 难点与计划
# =====================================================================
sl = new_slide(12, "11 / 难点与计划", "技术难点、应对方案与后续安排")
chal = [("游戏接入", "Proton 插件稳定性、多角色输入", "双实验室并行，互为兜底", BLUE),
        ("状态一致性", "Bridge 写、多进程读，读到半帧", "seqlock 版本号校验 + 每帧屏障同步", GREEN),
        ("死锁与崩溃", "循环等待厨具、持锁进程被 kill", "资源分配图查环回退 + 健壮互斥锁", RED),
        ("故障隔离", "单个进程崩溃拖垮整个系统", "多进程隔离 + 守护进程退避重启", PURPLE)]
for i, (a, prob, sol, c) in enumerate(chal):
    y = 124 + i * 114
    box(sl, 60, y, 740, 100, fill=WHITE, line=LINE, radius=10)
    box(sl, 60, y, 6, 100, fill=c)
    text(sl, 86, y + 20, 300, 26, a, 17, c, bold=True)
    text(sl, 86, y + 56, 300, 24, prob, 12.5, MUTED)
    arrow(sl, 396, y + 50, 428, y + 50, color=c, w=2)
    text(sl, 444, y, 340, 100, sol, 14, NAVY, bold=True, anchor=MSO_ANCHOR.MIDDLE, spacing=1.3)

box(sl, 830, 124, 390, 446, fill=WHITE, line=LINE, radius=10)
text(sl, 852, 140, 340, 24, "进度安排", 16, NAVY, bold=True)
plan = [("第 1–2 周", "组队分工、需求分析、概要设计", False),
        ("第 3 周", "小组方案汇报（本次）", True),
        ("第 4–5 周", "集中编码与联调", False),
        ("第 6 周", "可运行系统展示与验收", False),
        ("第 7–8 周", "个人汇报与答辩", False)]
for i, (wk, b, cur) in enumerate(plan):
    y = 184 + i * 76
    if i < 4:
        box(sl, 867, y + 20, 2, 76, fill=LINE)
    num(sl, 868, y + 12, 11, i + 1, BLUE if cur else "94A3B8", 10)
    text(sl, 892, y, 120, 24, wk, 14, BLUE if cur else NAVY, bold=True)
    text(sl, 892, y + 26, 310, 40, b, 12.5, BLUE if cur else MUTED, bold=cur, spacing=1.3)

box(sl, 60, 584, 1160, 86, fill=NAVY, radius=12)
text(sl, 90, 584, 900, 86, [
    [("用 Linux 的进程、信号、IPC 与同步机制，", {"color": WHITE}),
     ("解决多厨师协作出餐的调度问题。", {"color": "FCD34D"})],
], 18, WHITE, bold=True, anchor=MSO_ANCHOR.MIDDLE)
text(sl, 1000, 584, 200, 86, "谢谢 · 欢迎提问", 16, "93C5FD", bold=True, align=PP_ALIGN.RIGHT, anchor=MSO_ANCHOR.MIDDLE)
notes(sl, "最后是难点和计划。游戏接入靠双实验室并行互为兜底；状态一致性用 seqlock 和帧同步；死锁和持锁崩溃用资源分配图和健壮锁；"
          "故障隔离靠多进程加守护进程重启。接下来第 4、5 周集中编码，第 6 周验收。"
          "总结一句：我们用 Linux 的进程、信号、IPC 和同步机制，解决多厨师协作出餐的调度问题。谢谢大家。")

prs.core_properties.title = "WellCooked：基于《胡闹厨房2》的厨房调度策略设计"
prs.save(OUT)
print("saved", OUT)
