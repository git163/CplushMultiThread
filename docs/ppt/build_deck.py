#!/usr/bin/env python3
"""生成《task_runner 项目汇报》PPT（.pptx，16:9，可直接编辑）。

依赖与运行：
    python3 -m venv .venv
    .venv/bin/pip install python-pptx
    .venv/bin/python docs/ppt/build_deck.py

输出：
    docs/ppt/task_runner_overview.pptx

改内容：直接改下面的 build_* 函数里的文字即可；版式由 helper 统一控制。
"""
from __future__ import annotations

import os

from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_CONNECTOR, MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.oxml.ns import qn
from pptx.util import Emu, Inches, Pt

# ---------------------------------------------------------------- 设计系统
NAVY = "16324F"      # 主色：深海军蓝
TEAL = "0E7C86"      # 强调色：青
INK = "1A1A1A"       # 正文
MUTED = "6B7280"     # 次要文字
LINE = "D9E2EC"      # 分隔线
PANEL = "F4F7FA"     # 浅底面板
PANEL2 = "EAF3F4"    # 强调浅底
CODE_BG = "1E2430"   # 代码块底
CODE_FG = "E8EBF0"   # 代码文字
WHITE = "FFFFFF"
OK = "1B7F5A"        # 绿：表示框架已兜住

FONT_LATIN = "Calibri"
FONT_EA = "Microsoft YaHei"   # Office for Mac/Windows 都自带
FONT_CODE = "Consolas"

SW, SH = Inches(13.333), Inches(7.5)
ML = Inches(0.72)                 # 左右页边距
CW = SW - ML * 2                  # 内容宽度
TITLE_TOP = Inches(0.42)
BODY_TOP = Inches(1.55)


def _ea(run, typeface=FONT_EA):
    """设置东亚字体（python-pptx 的 font.name 只管拉丁字体）。"""
    rPr = run._r.get_or_add_rPr()
    ea = rPr.find(qn("a:ea"))
    if ea is None:
        ea = rPr.makeelement(qn("a:ea"), {})
        latin = rPr.find(qn("a:latin"))
        if latin is not None:
            latin.addnext(ea)
        else:
            rPr.append(ea)
    ea.set("typeface", typeface)


def style_run(run, size=16, bold=False, color=INK, mono=False, italic=False):
    f = run.font
    f.size = Pt(size)
    f.bold = bold
    f.italic = italic
    f.name = FONT_CODE if mono else FONT_LATIN
    f.color.rgb = RGBColor.from_string(color)
    _ea(run, FONT_CODE if mono else FONT_EA)
    return run


def textbox(slide, left, top, width, height, align=PP_ALIGN.LEFT, anchor=MSO_ANCHOR.TOP):
    tb = slide.shapes.add_textbox(left, top, width, height)
    tf = tb.text_frame
    tf.word_wrap = True
    tf.margin_left = tf.margin_right = tf.margin_top = tf.margin_bottom = 0
    tf.vertical_anchor = anchor
    tf.paragraphs[0].alignment = align
    return tf


def para(tf, first=False, space_before=0, space_after=6, level=0, line=None,
         align=PP_ALIGN.LEFT):
    p = tf.paragraphs[0] if first else tf.add_paragraph()
    p.space_before = Pt(space_before)
    p.space_after = Pt(space_after)
    p.level = level
    p.alignment = align          # 显式设对齐：autoshape 默认居中，会让代码块/正文跑偏
    if line:
        p.line_spacing = line
    return p


def block(slide, left, top, width, height, fill=PANEL, line=None, radius=None,
          shape=MSO_SHAPE.ROUNDED_RECTANGLE):
    sh = slide.shapes.add_shape(shape, left, top, width, height)
    sh.shadow.inherit = False
    if fill:
        sh.fill.solid()
        sh.fill.fore_color.rgb = RGBColor.from_string(fill)
    else:
        sh.fill.background()
    if line:
        sh.line.color.rgb = RGBColor.from_string(line)
        sh.line.width = Pt(1)
    else:
        sh.line.fill.background()
    sh.text_frame.word_wrap = True
    if radius is not None and shape == MSO_SHAPE.ROUNDED_RECTANGLE:
        sh.adjustments[0] = radius
    return sh


# ---------------------------------------------------------------- 版式 helper
def new_slide(prs):
    return prs.slides.add_slide(prs.slide_layouts[6])


def header(slide, title, kicker=None):
    if kicker:
        tf = textbox(slide, ML, TITLE_TOP - Inches(0.06), CW, Inches(0.3))
        style_run(para(tf, first=True, space_after=0).add_run(), 12, True, TEAL).text = kicker
    tf = textbox(slide, ML, TITLE_TOP + Inches(0.2), CW, Inches(0.55))
    style_run(para(tf, first=True, space_after=0).add_run(), 28, True, NAVY).text = title
    bar = block(slide, ML, TITLE_TOP + Inches(0.86), Inches(1.1), Inches(0.055),
                fill=TEAL, shape=MSO_SHAPE.RECTANGLE)
    bar.text_frame.text = ""


def footer(slide, page):
    tf = textbox(slide, ML, SH - Inches(0.5), CW, Inches(0.3))
    p = para(tf, first=True, space_after=0)
    style_run(p.add_run(), 10, color=MUTED).text = "task_runner · 可执行 / 可停止任务框架"
    tf2 = textbox(slide, SW - ML - Inches(1.0), SH - Inches(0.5), Inches(1.0), Inches(0.3),
                  align=PP_ALIGN.RIGHT)
    style_run(para(tf2, first=True, space_after=0).add_run(), 10, color=MUTED).text = f"{page:02d}"


def bullets(slide, items, left=ML, top=BODY_TOP, width=CW, size=15, gap=7, line=1.15):
    """items: [(level, text, bold)]，level 0/1。"""
    tf = textbox(slide, left, top, width, SH - top - Inches(0.6))
    for i, it in enumerate(items):
        lvl, text = it[0], it[1]
        bold = it[2] if len(it) > 2 else False
        color = INK if lvl == 0 else MUTED
        p = para(tf, first=(i == 0), space_after=gap, level=lvl, line=line)
        mark = "▪ " if lvl == 0 else "– "
        style_run(p.add_run(), size if lvl == 0 else size - 1.5, bold, color).text = mark + text
    return tf


def code_box(slide, code, left, top, width, height, size=11.5):
    sh = block(slide, left, top, width, height, fill=CODE_BG, radius=0.04)
    tf = sh.text_frame
    tf.margin_left = tf.margin_right = Inches(0.16)
    tf.margin_top = tf.margin_bottom = Inches(0.12)
    tf.vertical_anchor = MSO_ANCHOR.TOP
    for i, ln in enumerate(code.strip("\n").split("\n")):
        p = para(tf, first=(i == 0), space_after=0, line=0.95)
        style_run(p.add_run(), size, mono=True, color=CODE_FG).text = ln if ln else " "
    return sh


def table(slide, headers, rows, left, top, width, col_ratios=None, size=12.5,
          row_h=Inches(0.42), head_h=Inches(0.44), first_bold=True):
    n_rows, n_cols = len(rows) + 1, len(headers)
    shp = slide.shapes.add_table(n_rows, n_cols, left, top, width, head_h + row_h * len(rows))
    tbl = shp.table
    tbl.first_row = False
    tbl.horz_banding = False
    if col_ratios:
        total = sum(col_ratios)
        for i, r in enumerate(col_ratios):
            # width 是 EMU 整数（Emu 相减后不再是 Emu 类型），直接按 EMU 分配列宽
            tbl.columns[i].width = Emu(int(width * r / total))
    tbl.rows[0].height = head_h
    for r in range(1, n_rows):
        tbl.rows[r].height = row_h

    def fill_cell(cell, text, bold, color, bg):
        cell.fill.solid()
        cell.fill.fore_color.rgb = RGBColor.from_string(bg)
        cell.margin_left = cell.margin_right = Inches(0.1)
        cell.margin_top = cell.margin_bottom = Inches(0.03)
        cell.vertical_anchor = MSO_ANCHOR.MIDDLE
        tf = cell.text_frame
        tf.word_wrap = True
        tf.paragraphs[0].alignment = PP_ALIGN.LEFT
        tf.paragraphs[0].space_after = Pt(0)
        style_run(tf.paragraphs[0].add_run(), size, bold, color).text = text

    for c, h in enumerate(headers):
        fill_cell(tbl.cell(0, c), h, True, WHITE, NAVY)
    for r, row in enumerate(rows, start=1):
        bg = WHITE if r % 2 else PANEL
        for c, cell_text in enumerate(row):
            fill_cell(tbl.cell(r, c), cell_text, first_bold and c == 0,
                      INK if c == 0 else "374151", bg)
    return tbl


def note(slide, text, left, top, width, height, fill=PANEL2, size=13, color=INK, bold=False):
    sh = block(slide, left, top, width, height, fill=fill, radius=0.06)
    tf = sh.text_frame
    tf.margin_left = tf.margin_right = Inches(0.16)
    tf.margin_top = tf.margin_bottom = Inches(0.1)
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    # 注意：OOXML 的 <a:t> 里 "\n" 不是换行，必须拆成多个段落
    lines = text.split("\n")
    for i, ln in enumerate(lines):
        p = para(tf, first=(i == 0), space_after=1 if i < len(lines) - 1 else 0, line=1.1)
        style_run(p.add_run(), size, bold, color).text = ln
    return sh


def metric(slide, left, top, width, height, value, label, vcolor=TEAL):
    block(slide, left, top, width, height, fill=PANEL, radius=0.08)
    tf = textbox(slide, left, top + Inches(0.22), width, height - Inches(0.3),
                 align=PP_ALIGN.CENTER)
    style_run(para(tf, first=True, space_after=2, align=PP_ALIGN.CENTER).add_run(),
              30, True, vcolor).text = value
    style_run(para(tf, space_after=0, align=PP_ALIGN.CENTER).add_run(),
              12, False, MUTED).text = label


# ------------------------------------------------- 图元（框架图 / 类图 / 状态机）
HAIRLINE = Emu(9525)  # 1px ≈ 0.01in，用来画分隔线


def arrow(slide, x1, y1, x2, y2, color=MUTED, width=1.4, dash=False):
    """画一条带箭头的直线连接符。"""
    conn = slide.shapes.add_connector(MSO_CONNECTOR.STRAIGHT, x1, y1, x2, y2)
    conn.line.color.rgb = RGBColor.from_string(color)
    conn.line.width = Pt(width)
    ln = conn.line._get_or_add_ln()
    if dash:  # a:prstDash 必须排在 solidFill 之后、headEnd/tailEnd 之前
        ln.append(ln.makeelement(qn("a:prstDash"), {"val": "dash"}))
    ln.append(ln.makeelement(qn("a:tailEnd"),
                             {"type": "triangle", "w": "med", "len": "med"}))
    return conn


def arrow_label(slide, text, left, top, width=Inches(1.7), size=9.5, color=MUTED):
    tf = textbox(slide, left, top, width, Inches(0.22))
    style_run(para(tf, first=True, space_after=0, align=PP_ALIGN.CENTER).add_run(),
              size, False, color).text = text
    return tf


def uml_class(slide, left, top, width, name, attrs=(), methods=(), accent=NAVY,
              stereotype=None, size=10, line_h=0.205):
    """画一个简化类框（类名 / 字段 / 方法），返回底边 y。"""
    head_h = Inches(0.34)
    body_h = Inches((len(attrs) + len(methods)) * line_h + 0.15)
    hd = block(slide, left, top, width, head_h, fill=accent, shape=MSO_SHAPE.RECTANGLE)
    tfh = hd.text_frame
    tfh.margin_left = tfh.margin_right = Inches(0.1)
    tfh.vertical_anchor = MSO_ANCHOR.MIDDLE
    p = para(tfh, first=True, space_after=0)
    if stereotype:
        style_run(p.add_run(), size - 0.5, False, "BBD9E0").text = stereotype + " "
    style_run(p.add_run(), size + 1, True, WHITE).text = name

    bd = block(slide, left, top + head_h, width, body_h, fill=WHITE, line=LINE,
               shape=MSO_SHAPE.RECTANGLE)
    tfb = bd.text_frame
    tfb.margin_left = tfb.margin_right = Inches(0.1)
    tfb.margin_top = Inches(0.07)
    tfb.margin_bottom = Inches(0.04)
    tfb.vertical_anchor = MSO_ANCHOR.TOP
    first = True
    for a in attrs:      # 字段：灰色
        style_run(para(tfb, first=first, space_after=0, line=1.0).add_run(),
                  size, False, MUTED).text = a
        first = False
    for m in methods:    # 方法：深色
        style_run(para(tfb, first=first, space_after=0, line=1.0).add_run(),
                  size, False, INK).text = m
        first = False
    if attrs and methods:  # 字段 / 方法之间的分隔线
        y = top + head_h + Inches(0.07 + len(attrs) * line_h)
        block(slide, left, y, width, HAIRLINE, fill=LINE, shape=MSO_SHAPE.RECTANGLE)
    return top + head_h + body_h


def state_box(slide, left, top, width, height, text, accent=NAVY, fill=PANEL):
    sh = block(slide, left, top, width, height, fill=fill, line=accent, radius=0.16)
    tf = sh.text_frame
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    style_run(para(tf, first=True, space_after=0, align=PP_ALIGN.CENTER).add_run(),
              13, True, accent).text = text
    return sh


# ---------------------------------------------------------------- 各页内容
def s01_cover(prs):
    s = new_slide(prs)
    band = block(s, Inches(0), Inches(0), SW, Inches(2.9), fill=NAVY, shape=MSO_SHAPE.RECTANGLE)
    band.text_frame.text = ""
    tf = textbox(s, ML, Inches(0.85), CW, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14, True, "7FB3C8").text = \
        "CplushMultiThread · C++17"
    tf = textbox(s, ML, Inches(1.25), CW, Inches(0.9))
    style_run(para(tf, first=True, space_after=0).add_run(), 48, True, WHITE).text = "task_runner"
    tf = textbox(s, ML, Inches(2.12), CW, Inches(0.5))
    style_run(para(tf, first=True, space_after=0).add_run(), 20, False, "BFD7E4").text = \
        "可执行 / 可停止任务框架 —— 业务只填内容，编排交给框架"

    bullets(s, [
        (0, "仅头文件（4 个 .hpp，652 行）· 零第三方依赖（标准库 + pthread）· C++17", True),
        (0, "mac / Linux 已验证；拷一个目录即可移植到任何项目", False),
        (0, "52 项测试全绿（含 9 项压测）· TSan 扫描 0 数据竞争 · 5 个可运行示例", False),
    ], top=Inches(3.4), size=16, gap=12)

    note(s, "面向「界面有『执行』『停止』两个按钮、任务耗时、需要进度与结果」这类场景",
         ML, Inches(5.55), CW, Inches(0.75), fill=PANEL2, size=15, color=NAVY, bold=True)
    return s


def s02_value(prs):
    s = new_slide(prs)
    header(s, "一句话价值：把「线程那点事」一次性做对", kicker="为什么需要它")

    half = (CW - Inches(0.4)) / 2
    note(s, "问题\n一个「执行 / 停止」按钮背后，藏着 6 类并发问题，\n每做一个新任务都要重新踩一遍。",
         ML, BODY_TOP, half, Inches(1.75), fill="FBEEEE", size=15, color="8C2F39")
    note(s, "方案\n把这 6 类问题收敛成一个可复用框架，\n业务代码里只剩下业务。",
         ML + half + Inches(0.4), BODY_TOP, half, Inches(1.75), fill=PANEL2, size=15, color=OK)

    mw = (CW - Inches(0.6)) / 3
    metric(s, ML, Inches(3.7), mw, Inches(1.75), "652 行", "仅头文件，4 个 .hpp")
    metric(s, ML + mw + Inches(0.3), Inches(3.7), mw, Inches(1.75), "52 / 52", "测试全绿（含 9 项压测）")
    metric(s, ML + (mw + Inches(0.3)) * 2, Inches(3.7), mw, Inches(1.75), "0", "TSan 数据竞争报告")

    note(s, "业务侧代码量：一个可中断、带进度、带收尾的同步任务 ≈ 15 行",
         ML, Inches(5.75), CW, Inches(0.62), fill=PANEL, size=13.5, color=MUTED)
    return s


def s03_pain(prs):
    s = new_slide(prs)
    header(s, "手写一遍，要处理这 6 件事", kicker="痛点")
    table(s, ["坑", "后果"], [
        ("忘记 join 线程", "析构时 std::terminate / 崩溃"),
        ("停止标志裸读写", "数据竞争，未定义行为（TSan 必报）"),
        ("在任务线程里调 stop()", "join 自身 → 死锁"),
        ("用户连点两次「执行」", "两个线程跑同一个任务对象"),
        ("业务抛异常逃出线程", "std::terminate，进程直接挂"),
        ("任务自己跑完了", "没人知道 → 后处理漏掉，或只能轮询"),
    ], ML, BODY_TOP, CW, col_ratios=[4, 6], size=14, row_h=Inches(0.52))

    note(s, "这些不是业务逻辑，是每个任务都要重复写一遍的「样板 + 陷阱」",
         ML, Inches(6.0), CW, Inches(0.6), fill="FBEEEE", size=14, color="8C2F39")
    return s


def s04_arch(prs):
    s = new_slide(prs)
    header(s, "方案：三层结构，业务只碰最上面一层", kicker="总体设计")

    layers = [
        ("业务层", "继承 runnable_task，只实现 run()", "可选：on_tick() 推状态、on_finished() 收尾", PANEL2, TEAL),
        ("框架层", "task_controller —— 状态机 + 线程编排", "防重入 · 错误码 · 异常兜底 · 结果判定", PANEL, NAVY),
        ("工具层", "stop_control / stop_token、periodic_publisher", "可脱离控制器独立复用（心跳上报 / 跨线程取消）", PANEL, NAVY),
    ]
    top = BODY_TOP
    for name, line1, line2, bg, color in layers:
        sh = block(s, ML, top, CW, Inches(1.05), fill=bg, radius=0.08)
        tf = sh.text_frame
        tf.margin_left = Inches(0.28)
        tf.vertical_anchor = MSO_ANCHOR.MIDDLE
        p = para(tf, first=True, space_after=2)
        style_run(p.add_run(), 16, True, color).text = f"{name}   "
        style_run(p.add_run(), 14.5, False, INK).text = line1
        p2 = para(tf, space_after=0)
        style_run(p2.add_run(), 12.5, False, MUTED).text = "        " + line2
        top += Inches(1.22)

    note(s, "核心承诺：业务代码里不出现 std::thread、mutex、join  ——  这些全部在框架层",
         ML, Inches(5.55), CW, Inches(0.62), fill=PANEL2, size=14.5, color=NAVY, bold=True)
    return s


def s05_components(prs):
    s = new_slide(prs)
    header(s, "四个组件，职责一眼看清", kicker="模块设计 · 组件职责")
    table(s, ["组件", "头文件", "职责"], [
        ("stop_control / stop_token", "stop_token.hpp", "协作式停止源与只读视图（可触发的 / 只能感知的）"),
        ("periodic_publisher", "periodic_publisher.hpp", "通用周期回调执行器，自带线程，可独立复用"),
        ("runnable_task", "runnable_task.hpp", "业务接口：run() + 可选 on_tick() / on_finished()"),
        ("task_controller", "task_controller.hpp", "编排器：状态机 + 线程 + 防重入 + 错误码"),
    ], ML, BODY_TOP, CW, col_ratios=[3, 3, 6], size=13.5, row_h=Inches(0.62))
    note(s, "业务通常只需 #include \"task_runner/task_controller.hpp\"",
         ML, Inches(4.55), CW, Inches(0.6), fill=PANEL, size=13.5, color=MUTED)
    return s


def diamond(slide, cx, cy, size=Inches(0.14), filled=True, color=NAVY):
    """UML 菱形（复合 / 聚合）。"""
    s = int(size / 2)
    sh = slide.shapes.add_shape(MSO_SHAPE.DIAMOND, int(cx) - s, int(cy) - s, size, size)
    sh.shadow.inherit = False
    sh.fill.solid()
    sh.fill.fore_color.rgb = RGBColor.from_string(color if filled else WHITE)
    sh.line.color.rgb = RGBColor.from_string(color)
    sh.line.width = Pt(1)
    return sh


def s06_module_files(prs):
    s = new_slide(prs)
    header(s, "模块划分：四个头文件，依赖单向无环", kicker="模块设计 ①")

    def node(left, top, width, height, title, sub, fill, accent):
        sh = block(s, left, top, width, height, fill=fill, line=LINE, radius=0.1)
        tf = sh.text_frame
        tf.margin_left = tf.margin_right = Inches(0.1)
        tf.vertical_anchor = MSO_ANCHOR.MIDDLE
        p = para(tf, first=True, space_after=1)
        style_run(p.add_run(), 11.5, True, accent).text = title
        style_run(p.add_run(), 9.5, False, MUTED).text = "  " + sub
        return sh

    # 依赖关系图：箭头指向「被依赖」（越往下越基础）
    node(Inches(1.7), Inches(1.55), Inches(4.3), Inches(0.6),
         "task_controller.hpp", "361 行 · 编排器", PANEL2, NAVY)
    node(Inches(0.72), Inches(2.75), Inches(3.0), Inches(0.78),
         "periodic_publisher.hpp", "118 行 · 周期回调", PANEL, NAVY)
    node(Inches(4.05), Inches(2.75), Inches(3.05), Inches(0.78),
         "runnable_task.hpp", "80 行 · 业务接口", PANEL, NAVY)
    node(Inches(2.1), Inches(4.05), Inches(3.5), Inches(0.66),
         "stop_token.hpp", "93 行 · 停止源 / 只读视图", PANEL2, TEAL)

    arrow(s, Inches(2.22), Inches(2.15), Inches(2.22), Inches(2.72), color=NAVY)
    arrow(s, Inches(5.57), Inches(2.15), Inches(5.57), Inches(2.72), color=NAVY)
    arrow(s, Inches(2.22), Inches(3.53), Inches(3.25), Inches(4.03), color=TEAL)
    arrow(s, Inches(5.57), Inches(3.53), Inches(4.55), Inches(4.03), color=TEAL)
    arrow_label(s, "都依赖它", Inches(3.30), Inches(3.54), width=Inches(1.5), color=TEAL)

    right = ML + Inches(7.0)
    rw = CW - Inches(7.0)
    tf = textbox(s, right, BODY_TOP, rw, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 15, True, NAVY).text = "各文件职责"
    table(s, ["头文件", "职责"], [
        ("task_controller.hpp", "状态机 + 线程编排 + 防重入 + 结果判定"),
        ("periodic_publisher.hpp", "固定周期回调执行器（自带线程，可独立复用）"),
        ("runnable_task.hpp", "业务接口 + task_mode / error_code / run_result"),
        ("stop_token.hpp", "协作式停止源 stop_control + 只读视图 stop_token"),
    ], right, BODY_TOP + Inches(0.45), rw, col_ratios=[3.4, 6], size=10.5, row_h=Inches(0.62))

    note(s, "依赖单向、无环 —— 想只用「周期回调」可以只拷 periodic_publisher.hpp + stop_token.hpp，"
            "不必带上编排器。",
         ML, Inches(5.05), CW * 0.47, Inches(0.85), fill=PANEL, size=12, color=MUTED)
    note(s, "runnable_task 不反向依赖 controller —— 业务接口不会被编排器「污染」，"
            "换掉控制器实现业务代码不用动。",
         ML + CW * 0.47 + Inches(0.28), Inches(5.05), CW * 0.53 - Inches(0.28), Inches(0.85),
         fill=PANEL2, size=12, color=NAVY)
    return s


def s07_class_diagram(prs):
    s = new_slide(prs)
    header(s, "类关系：run_phase 是唯一的资源所有者", kicker="模块设计 ② · 框架图")

    uml_class(s, ML, Inches(1.45), Inches(5.0), "task_controller",
              attrs=["- mutex_ : mutex",
                     "- task_ : shared_ptr<runnable_task>",
                     "- phase_ : shared_ptr<run_phase>"],
              methods=["+ start() : error_code",
                       "+ stop(on_stopped) : error_code",
                       "+ running() / last_run_result()",
                       "+ run_exception() / tick_exception()",
                       "+ finish_exception()"],
              accent=NAVY, size=10)

    uml_class(s, Inches(6.2), Inches(1.45), CW - Inches(5.48), "runnable_task",
              methods=["+ run(stop_token) = 0",
                       "+ async_completion() : shared_future<void>",
                       "+ on_tick()",
                       "+ on_finished(run_result, exception_ptr)"],
              accent=TEAL, stereotype="«interface»", size=10)

    uml_class(s, ML, Inches(3.82), Inches(5.0), "task_controller::run_phase",
              attrs=["- control : shared_ptr<stop_control>",
                     "- task : shared_ptr<runnable_task>",
                     "- worker : thread",
                     "- publisher : unique_ptr<periodic_publisher>",
                     "- result / run_exc / tick_exc / finish_exc"],
              methods=["+ run_body(ph) / finalize(ph, exc)"],
              accent=NAVY, size=10)

    uml_class(s, ML, Inches(5.8), Inches(2.9), "periodic_publisher",
              attrs=["- thread_ / active_ / period_"],
              methods=["+ start(token) / stop()", "+ thread_id() / tick_exception()"],
              accent="374151", size=9.5)

    uml_class(s, Inches(3.9), Inches(5.8), Inches(3.2), "stop_control / stop_token",
              methods=["+ create() / request_stop() / wake_all() / token()",
                       "+ stop_requested() / wait_for(ms)"],
              accent=TEAL, size=9.5)

    # 关系：■ 复合 / □ 聚合 / ┄ 持有或使用（含义见页脚图例）
    arrow(s, Inches(3.22), Inches(3.58), Inches(3.22), Inches(3.80), color=NAVY)
    diamond(s, Inches(3.22), Inches(3.70), filled=True, color=NAVY)

    arrow(s, Inches(5.72), Inches(2.0), Inches(6.18), Inches(2.0), color=TEAL, dash=True)

    arrow(s, Inches(2.17), Inches(5.54), Inches(2.17), Inches(5.78), color="374151")
    diamond(s, Inches(2.17), Inches(5.67), filled=True, color="374151")

    arrow(s, Inches(4.60), Inches(5.54), Inches(4.60), Inches(5.78), color=TEAL)
    diamond(s, Inches(4.60), Inches(5.67), filled=False, color=TEAL)

    arrow(s, Inches(3.62), Inches(6.0), Inches(3.88), Inches(6.0), color=TEAL, dash=True)

    right = Inches(7.5)
    rw = CW - Inches(6.78)
    bullets(s, [
        (0, "run_phase 是唯一持有资源的地方：停止源、worker 线程、发布器、结果都在它里面", True),
        (0, "task_controller 不直接管线程 —— 它只管「当前是哪一轮 phase」和状态", False),
        (0, "每轮 start() 新建一个 run_phase，旧的整体替换，join 全在 phase 内部完成", False),
        (0, "periodic_publisher 只拿到 stop_token（只读）—— 它无法主动停任何人", False),
    ], left=right, top=Inches(3.6), width=rw, size=12, gap=11)

    note(s, "■ 复合：拥有生命周期　□ 聚合：shared_ptr 共享　┄ 持有 / 使用",
         right, Inches(5.8), rw, Inches(0.5), fill=PANEL, size=10, color=MUTED)
    return s


def s08_state_machine(prs):
    s = new_slide(prs)
    header(s, "状态机与关键时序", kicker="模块设计 ③")

    # 状态图
    state_box(s, ML, Inches(1.5), Inches(1.4), Inches(0.6), "idle")
    state_box(s, ML + Inches(1.9), Inches(1.5), Inches(1.5), Inches(0.6), "running", accent=TEAL)
    state_box(s, ML + Inches(3.9), Inches(1.5), Inches(1.5), Inches(0.6), "stopping", accent=NAVY)
    arrow(s, ML + Inches(1.42), Inches(1.8), ML + Inches(1.88), Inches(1.8), color=TEAL)
    arrow(s, ML + Inches(3.42), Inches(1.8), ML + Inches(3.88), Inches(1.8), color=NAVY)
    arrow_label(s, "start() → ok", ML + Inches(0.87), Inches(2.14), width=Inches(1.5), color=TEAL)
    arrow_label(s, "stop(cb)", ML + Inches(3.22), Inches(2.14), width=Inches(1.3), color=NAVY)
    # 回到 idle：stopping →（join 完成）/ running →（自然完成）
    arrow(s, ML + Inches(5.15), Inches(2.1), ML + Inches(5.15), Inches(2.6), color=MUTED)
    arrow(s, ML + Inches(5.15), Inches(2.6), ML + Inches(0.7), Inches(2.6), color=MUTED)
    arrow(s, ML + Inches(0.7), Inches(2.6), ML + Inches(0.7), Inches(2.12), color=MUTED)
    arrow_label(s, "join 完成 / 自然完成 → idle", ML + Inches(1.6), Inches(2.64),
                width=Inches(3.4))

    table(s, ["从", "事件", "到", "返回"], [
        ("idle", "start()", "running", "ok"),
        ("running", "start()", "running", "already_running"),
        ("running", "stop(cb)", "stopping", "ok"),
        ("running", "run() 返回 / 抛异常", "idle", "completed / failed"),
        ("stopping", "start() / stop()", "stopping", "already_stopping"),
        ("stopping", "worker join 完成", "idle", "ok"),
    ], ML, Inches(3.05), Inches(5.88), col_ratios=[1.7, 3.0, 1.6, 2.5], size=10,
        row_h=Inches(0.36), head_h=Inches(0.38))

    right = ML + Inches(6.9)
    rw = CW - Inches(6.9)
    tf = textbox(s, right, BODY_TOP, rw, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14, True, NAVY).text = "start() / stop() 时序"
    code_box(s, '''start()
  锁内: 校验 state -> 建 run_phase
        -> spawn worker + publisher
        -> state = running
  worker: task->run(token)
          [async 再 wait 完成句柄]

stop(cb)
  state = stopping
  request_stop() + wake_all()
  worker.join()          <- 等任务真正停下
    worker 收尾:
      result = first-wins(failed|stopped|completed)
      publisher.stop()   <- join 发布器
      on_finished(result, run_exc)
      state = idle       <- 收尾最后一步
  调 cb()                <- on_stopped，调用者线程''',
             right, Inches(1.95), rw, Inches(3.15), size=10)

    note(s, "不变式：running() == false  蕴含「收尾已完成」—— on_finished 已返回、发布器已 join。"
            "这是把「置 idle」放到收尾最后一步的直接结果，调用方可以放心用它判断结束。",
         ML, Inches(5.8), CW, Inches(0.75), fill=PANEL2, size=12.5, color=NAVY, bold=True)
    return s


def s09_advantage1(prs):
    s = new_slide(prs)
    header(s, "优势 ①  业务只填 run()，编排全部托管", kicker="核心优势")
    code_box(s, '''
class compress_job : public runnable_task {
public:
    void run(stop_token token) override {      // ★ 业务只写这里
        for (int i = 0; i < total_; ++i) {
            if (token.stop_requested()) break; // 感知停止（可选）
            compress_one(i);                   // 你的耗时内容
            token.wait_for(10ms);              // 睡眠 + 可被停止唤醒
            done_ = i + 1;
        }
    }
    void on_tick() override { report(done_.load()); }   // 每 1s 推进度
    void on_finished(run_result r, std::exception_ptr e) override { finish(r, e); }
};

auto job = std::make_shared<compress_job>();
task_controller ctl(job, task_mode::sync, 1s);
ctl.start();                            // 起 worker + 周期发布器，不阻塞
ctl.stop([]{ /* 停止后处理 */ });       // 阻塞：等真正停止 → 后处理
''', ML, Inches(1.65), CW * 0.735, Inches(3.7), size=12)

    right = ML + CW * 0.735 + Inches(0.28)
    rw = CW - CW * 0.735 - Inches(0.28)
    note(s, "框架替你做的：\n· 创建 / 回收线程\n· 状态机与重入保护\n· 结果判定与错误码\n· 异常兜底\n· 跨轮次 join 安全",
         right, Inches(1.65), rw, Inches(2.2), fill=PANEL, size=13, color=NAVY)
    note(s, "同步 / 异步只差一个参数：\ntask_mode::sync / async",
         right, Inches(4.15), rw, Inches(1.2), fill=PANEL2, size=13, color=TEAL, bold=True)

    note(s, "清单里的 6 类并发陷阱 —— 忘记 join · 停止标志竞态 · 停止时 join 自身 · "
            "任务抛异常逃出线程 · 重入 · 完成漏报 —— 框架全包了。",
         ML, Inches(5.5), CW, Inches(0.8), fill=PANEL2, size=13.5, color=NAVY, bold=True)
    return s


def s10_result(prs):
    s = new_slide(prs)
    header(s, "优势 ②  结果与错误码：每个分支都有明确语义", kicker="可预期性")

    half = (CW - Inches(0.4)) / 2
    tf = textbox(s, ML, BODY_TOP, half, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 15, True, NAVY).text = "运行结果 run_result"
    table(s, ["取值", "含义"], [
        ("completed", "正常跑完（无人停止）"),
        ("stopped", "运行期间收到停止请求后结束"),
        ("failed", "run() / 业务异步抛异常"),
        ("none", "没跑过 / 还没结束"),
    ], ML, BODY_TOP + Inches(0.42), half, col_ratios=[3, 6], size=12, row_h=Inches(0.46))

    l2 = ML + half + Inches(0.4)
    tf = textbox(s, l2, BODY_TOP, half, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 15, True, NAVY).text = "错误码 error_code"
    table(s, ["取值", "触发场景"], [
        ("ok", "成功"),
        ("already_running", "正在运行，禁止重入"),
        ("already_stopping", "正在停止，禁止重入"),
        ("not_running", "对空闲任务调 stop()"),
        ("self_stop_denied", "在控制器自身线程内调 start/stop"),
        ("resource_error", "线程创建失败（可重试）"),
    ], l2, BODY_TOP + Inches(0.42), half, col_ratios=[4, 6], size=12, row_h=Inches(0.42))

    note(s, "调用方不需要猜：start() 返回 already_running 就是「已在运行」，"
            "stop() 返回 not_running 就是「任务已自己结束」—— 都不是异常，是明确的状态答案。",
         ML, Inches(6.05), CW, Inches(0.68), fill=PANEL2, size=13.5, color=NAVY)
    return s


def s11_callbacks(prs):
    s = new_slide(prs)
    header(s, "优势 ③  两个回调，正好覆盖 UI 的两大诉求", kicker="对界面友好")
    table(s, ["回调", "触发方式", "典型用途"], [
        ("on_tick()", "每 tick_period（默认 1s）在独立线程调用", "进度条、状态文字"),
        ("on_finished(result, error)", "任务结束时调用一次，三种结果都触发", "上传结果 / 清理资源 / 通知界面"),
    ], ML, BODY_TOP, CW, col_ratios=[3.4, 5, 4], size=13.5, row_h=Inches(0.6))

    tf = textbox(s, ML, BODY_TOP + Inches(1.85), CW, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 15, True, NAVY).text = \
        "可以直接依赖的三条时序保证"
    bullets(s, [
        (0, "on_finished 在「自然完成」时也会触发 —— 没人调 stop()、任务自己跑完，照样回调", True),
        (0, "stop() 返回时 on_finished 保证已执行完毕，且先于 on_stopped", False),
        (0, "running() 返回 false 蕴含 on_finished 已返回 —— 轮询判结束不会与回调竞争", False),
    ], top=BODY_TOP + Inches(2.3), size=14, gap=11)

    note(s, "对比：只用 on_stopped 时，「任务自己跑完」这条路径没有任何回调，调用方只能轮询",
         ML, Inches(5.85), CW, Inches(0.62), fill=PANEL, size=13, color=MUTED)
    return s


def s12_thread_model(prs):
    s = new_slide(prs)
    header(s, "优势 ④  线程模型：杜绝跨轮次竞态", kicker="健壮性")

    # phase 图
    def phase_box(left, top, label, accent):
        sh = block(s, left, top, Inches(4.3), Inches(1.5), fill=PANEL, line=LINE, radius=0.08)
        tf = sh.text_frame
        tf.margin_left = Inches(0.18)
        tf.vertical_anchor = MSO_ANCHOR.MIDDLE
        style_run(para(tf, first=True, space_after=5).add_run(), 14, True, accent).text = label
        for t in ("stop_control（本轮停止源）", "worker 线程（跑 run()）",
                  "periodic_publisher 线程（跑 on_tick）"):
            style_run(para(tf, space_after=1).add_run(), 11.5, False, "374151").text = "· " + t

    phase_box(ML, BODY_TOP, "run_phase #1", MUTED)
    phase_box(ML, BODY_TOP + Inches(1.75), "run_phase #2", TEAL)

    note(s, "start()\n新建 + 整体替换", ML + Inches(4.5), BODY_TOP + Inches(0.55),
         Inches(2.0), Inches(1.1), fill=PANEL2, size=12.5, color=TEAL, bold=True)

    bullets(s, [
        (0, "每次 start() 创建一个全新的 run_phase（新停止源 + 新 worker + 新发布器）", True),
        (0, "旧 phase 整体替换；任何 join 都发生在 phase 内部", False),
        (0, "杜绝：跨轮次双重 join、成员写竞态、上一轮过期 token 干扰新一轮", False),
        (0, "同一控制器可反复 start / stop 复用，每轮结果独立（first-wins）", False),
    ], left=ML + Inches(6.85), top=BODY_TOP + Inches(0.35), width=CW - Inches(6.85),
        size=13.5, gap=10)

    note(s, "同一控制器同一时刻只有一个运行实例；要并发跑多个任务 → 用多个控制器 + 各自的任务实例",
         ML, Inches(5.7), CW, Inches(0.62), fill=PANEL, size=13, color=MUTED)
    return s


def s13_safety(prs):
    s = new_slide(prs)
    header(s, "优势 ⑤  异常不逃逸，误用被明确挡住", kicker="安全生产")

    half = (CW - Inches(0.4)) / 2
    tf = textbox(s, ML, BODY_TOP, half, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 15, True, NAVY).text = \
        "异常一律被捕获，不会 terminate"
    table(s, ["谁抛异常", "框架处理 / 你能拿到"], [
        ("run() / 业务异步", "run_result=failed + run_exception()"),
        ("on_tick()", "tick_exception()，run 不中断"),
        ("on_finished()", "finish_exception()，不影响结果"),
        ("on_stopped()", "记日志，不影响返回值"),
    ], ML, BODY_TOP + Inches(0.42), half, col_ratios=[4, 6], size=12, row_h=Inches(0.5))

    l2 = ML + half + Inches(0.4)
    tf = textbox(s, l2, BODY_TOP, half, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 15, True, NAVY).text = \
        "重入与自调用：返回错误码，而不是死锁"
    table(s, ["场景", "返回"], [
        ("运行中再 start()", "already_running"),
        ("停止中再 start() / stop()", "already_stopping"),
        ("对空闲任务 stop()", "not_running"),
        ("控制器自身线程内调 start/stop", "self_stop_denied"),
    ], l2, BODY_TOP + Inches(0.42), half, col_ratios=[6, 4], size=12, row_h=Inches(0.5))

    note(s, "设计取向：把「会变成死锁 / 崩溃 / UB 的误用」转成「有明确返回值的状态」",
         ML, Inches(6.05), CW, Inches(0.62), fill=PANEL2, size=14, color=NAVY, bold=True)
    return s


def s14_portable(prs):
    s = new_slide(prs)
    header(s, "优势 ⑥  仅头文件 · 零依赖 · 一个目录即可移植", kicker="集成成本")

    cw = (CW - Inches(0.45)) / 2
    ch = Inches(1.5)
    cards = [
        ("仅头文件", "4 个 .hpp，共 652 行\n拷贝 src/include/task_runner/ 即可用", TEAL),
        ("零第三方依赖", "只用标准库 + pthread\nC++17，无 boost / 无框架依赖", TEAL),
        ("两条命令接入", "target_include_directories + pthread\n或直接搬 INTERFACE 库定义", NAVY),
        ("平台已验", "mac（AppleClang）与 Linux 均已构建通过\n并跑完测试与 TSan", NAVY),
    ]
    for i, (title, desc, color) in enumerate(cards):
        left = ML + (cw + Inches(0.45)) * (i % 2)
        top = BODY_TOP + (ch + Inches(0.3)) * (i // 2)
        block(s, left, top, cw, ch, fill=PANEL, radius=0.08)
        tf = textbox(s, left + Inches(0.28), top + Inches(0.24), cw - Inches(0.5),
                     ch - Inches(0.4))
        style_run(para(tf, first=True, space_after=6).add_run(), 16, True, color).text = title
        for ln in desc.split("\n"):
            style_run(para(tf, space_after=2, line=1.05).add_run(), 12.5, False, "374151").text = ln

    code_box(s, '''# 方式 A：直接用本仓的 INTERFACE 目标
target_link_libraries(your_target PRIVATE task_runner)

# 方式 B：不用 CMake 目标，手工加路径
target_include_directories(your_target PRIVATE path/to/include)
target_link_libraries(your_target PRIVATE pthread)''',
             ML, Inches(4.85), CW, Inches(1.55), size=12)

    note(s, "两个工具（periodic_publisher / stop_control）可脱离 task_controller 单独使用",
         ML, Inches(6.48), CW, Inches(0.45), fill=PANEL2, size=12, color=TEAL)
    return s


def s15_case1(prs):
    s = new_slide(prs)
    header(s, "用例 ①  执行 / 停止按钮（最常见场景）", kicker="examples/sync_task.cpp")

    code_box(s, '''auto job = std::make_shared<compress_job>();
task_controller ctl(job, task_mode::sync, 1s);   // 装配一次，长期持有

// 「执行」按钮
if (ctl.start() != error_code::ok) show("任务已在运行");

// 「停止」按钮：stop() 是阻塞调用 → 放到独立线程，别卡界面
std::thread([&]{ ctl.stop([]{ show("已停止"); }); }).detach();''',
             ML, BODY_TOP, CW, Inches(2.3), size=12)

    tf = textbox(s, ML, Inches(3.95), CW, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14, True, NAVY).text = "实际运行输出"
    code_box(s, '''start(): ok
start() again: already_running        <- 防重入
  [sync] tick: 3/20                   <- on_tick 周期推进度
  [sync] stop requested at file 5/20  <- 业务感知停止，及时退出
  [sync] on_finished: stopped         <- 先于 on_stopped
  [sync] on_stopped: post-processing
stop(): ok
last_run_result: stopped
stop() when idle: not_running         <- 任务已结束，不是错误''',
             ML, Inches(4.4), CW, Inches(2.2), size=11.5)
    return s


def s16_case2(prs):
    s = new_slide(prs)
    header(s, "用例 ②  异步任务：两种完成句柄来源", kicker="examples/async_task.cpp")

    half = (CW - Inches(0.35)) / 2
    tf = textbox(s, ML, BODY_TOP, half, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14.5, True, TEAL).text = \
        "来源 ① 内部线程跑阻塞内容"
    code_box(s, '''void run(stop_token token) override {
  fut_ = std::async(std::launch::async,
      [this, token] {
        while (!token.stop_requested()) {
          fetch_chunk();
          token.wait_for(120ms);
        }
      }).share();       // run() 立即返回
}
std::shared_future<void>
async_completion() const override { return fut_; }''',
             ML, Inches(1.97), half, Inches(2.55), size=11)

    l2 = ML + half + Inches(0.35)
    tf = textbox(s, l2, BODY_TOP, half, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14.5, True, TEAL).text = \
        "来源 ② 真·事件驱动（第三方回调）"
    code_box(s, '''void run(stop_token token) override {
  stopper_ = token;
  start_sdk_chain([this] {
    done_promise_.set_value();   // 业务链完成
  });
}
std::shared_future<void>
async_completion() const override {
  return done_promise_.get_future().share();
}''', l2, Inches(1.97), half, Inches(2.55), size=11)

    note(s, "框架 wait 完成句柄来判定「真正完成」，不是轮询；期间 on_tick 照常发进度。"
            "  注意：业务链必须最终置值，否则 stop() 会一直等（协作式语义）。",
         ML, Inches(4.8), CW, Inches(1.25), fill=PANEL, size=13, color=MUTED)
    return s


def s17_case3(prs):
    s = new_slide(prs)
    header(s, "用例 ③  进度发布 + 任务结束收尾", kicker="examples/progress_and_finish.cpp")

    code_box(s, '''void on_tick() override {                     // 独立线程，每 1s 一次
    const int d = done_.load();               // 共享状态必须原子
    ui_post([d]{ progress_bar->set(d); });    // 投递到 UI 线程，别在这里碰控件
}

void on_finished(run_result r, std::exception_ptr e) override {
    switch (r) {                              // 三种结果都走这里
        case run_result::completed: notify_ui_success();    break;
        case run_result::stopped:   notify_ui_cancelled();  break;
        case run_result::failed:    report_ui_error(e);     break;
        case run_result::none:      break;
    }
}''', ML, BODY_TOP, CW, Inches(2.85), size=11.5)

    tf = textbox(s, ML, Inches(4.55), CW, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14, True, NAVY).text = \
        "实际运行输出（自然完成，全程无人调 stop()）"
    code_box(s, '''[part 1] natural completion
  [report] progress 23/40
  [report] progress 39/40
  [report] finished: completed            <- 自己跑完也回调
running()==false, on_finished count = 1   <- 不变式成立

[part 2] run() throws
last_run_result: failed
run_exception(): disk full                <- 异常被捕获，进程不挂''',
             ML, Inches(4.9), CW, Inches(2.0), size=11)
    return s


def s18_case4(prs):
    s = new_slide(prs)
    header(s, "用例 ④  多任务并发 / 全局互斥", kicker="examples/multi_task.cpp")

    half = (CW - Inches(0.35)) / 2
    tf = textbox(s, ML, BODY_TOP, half, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14.5, True, TEAL).text = \
        "要并发 → 多个控制器 + 各自实例"
    code_box(s, '''auto job_a = std::make_shared<download_job>("A", 10);
auto job_b = std::make_shared<download_job>("B", 6);

task_controller ctl_a(job_a, ...);
task_controller ctl_b(job_b, ...);

ctl_a.start();   // 互不影响
ctl_b.start();''', ML, Inches(1.9), half, Inches(1.8), size=11)

    l2 = ML + half + Inches(0.35)
    tf = textbox(s, l2, BODY_TOP, half, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14.5, True, TEAL).text = \
        "要全局只跑一个 → 单控制器多轮复用"
    code_box(s, '''task_controller ctl(std::make_shared<download_job>(...));

for (int round = 0; round < 3; ++round) {
    ctl.start();     // 返回值本身就是互斥的答案
    ctl.stop();      // 之后还能再 start()
}
// 不需要额外的锁''', l2, Inches(1.9), half, Inches(1.8), size=11)

    note(s, "提醒：框架的防重入是「控制器」粒度的。把同一个 task 对象交给两个控制器，"
            "框架拦不住 —— 那是业务侧数据竞争（详见 docs/task-runner-reentrancy.md）。",
         ML, Inches(3.85), CW, Inches(0.8), fill="FBEEEE", size=13, color="8C2F39")

    tf = textbox(s, ML, Inches(4.8), CW, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14, True, NAVY).text = "实际运行输出"
    code_box(s, '''start A: ok, start B: ok  <- 互不影响
  [B] on_finished: completed  done=6/6
  [A] on_finished: stopped  done=7/10
round 0 start(): ok   round 0 stop(): ok
round 1 start(): ok   round 1 stop(): ok   <- 复用同一控制器''',
             ML, Inches(5.2), CW, Inches(1.3), size=11)
    return s


def s19_case5(prs):
    s = new_slide(prs)
    header(s, "用例 ⑤  不要控制器：两个工具单独用", kicker="examples/reusable_components.cpp")

    half = (CW - Inches(0.35)) / 2
    tf = textbox(s, ML, BODY_TOP, half, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14.5, True, TEAL).text = \
        "周期回调（如每 30s 上报心跳）"
    code_box(s, '''auto ctrl = stop_control::create();
periodic_publisher pub(30s, []{ send_heartbeat(); });

pub.start(ctrl->token());
// ...
ctrl->request_stop();   // 或 pub.stop()，任一都能停
pub.stop();             // 退出并 join（幂等）''',
             ML, Inches(1.97), half, Inches(2.15), size=11)

    l2 = ML + half + Inches(0.35)
    tf = textbox(s, l2, BODY_TOP, half, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14.5, True, TEAL).text = \
        "跨线程协作式取消标记"
    code_box(s, '''auto ctrl = stop_control::create();
auto tok  = ctrl->token();       // 只读视图，发给工作线程

std::thread worker([tok]{
    while (!tok.stop_requested()) {
        tok.wait_for(100ms);     // 睡眠 + 可被唤醒
        do_step();
    }
});
ctrl->request_stop();  worker.join();''',
             l2, Inches(1.97), half, Inches(2.15), size=11)

    tf = textbox(s, ML, Inches(4.3), CW, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 14, True, NAVY).text = "实际运行输出"
    code_box(s, '''[publisher] heartbeat #1 / #2 / #3     <- 每 300ms 一次
stop:  ok                              <- 幂等
request_stop(): first
request_stop() again: already          <- 幂等
worker stopped after 3 steps''',
             ML, Inches(4.7), CW, Inches(1.35), size=11)

    note(s, "stop_control 可触发停止、stop_token 只能感知 —— 把这个区分做进类型系统，"
            "比到处传裸 atomic<bool> 更安全。",
         ML, Inches(6.15), CW, Inches(0.7), fill=PANEL2, size=13, color=NAVY)
    return s


def s20_quality(prs):
    s = new_slide(prs)
    header(s, "工程质量：不是 demo，是可交付的框架", kicker="可信度")

    mw = (CW - Inches(0.9)) / 4
    for i, (v, l) in enumerate([("52 / 52", "测试全绿（含 9 项压测）"),
                                ("0", "TSan 数据竞争报告"),
                                ("38", "场景矩阵用例（SC-01~38）"),
                                ("5", "可运行示例程序")]):
        metric(s, ML + (mw + Inches(0.3)) * i, Inches(1.5), mw, Inches(1.4), v, l)

    tf = textbox(s, ML, Inches(3.05), CW, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 15, True, NAVY).text = "测试与验证"
    bullets(s, [
        (0, "单元 / 压测：TaskController 30 项 · Stress 11 项 · StopToken 6 项 · Publisher 6 项 · Smoke 1 项", False),
        (0, "覆盖：状态机 · 防重入 · 协作停止 · 自然完成 · 异常 · 周期发布 · 析构 · 异步完成句柄 · 多轮复用", False),
        (0, "ThreadSanitizer：全部用例 + 示例 + 主程序 0 报告（ENABLE_TSAN 一键开启，附抑制文件）", False),
    ], top=Inches(3.42), size=12.5, gap=8)

    tf = textbox(s, ML, Inches(4.5), CW, Inches(0.4))
    style_run(para(tf, first=True, space_after=0).add_run(), 15, True, NAVY).text = "文档与示例"
    table(s, ["文档", "内容"], [
        ("docs/task-runner-usage-guide.md", "按场景索引的使用指南（13 个场景 + 排查清单）"),
        ("src/include/task_runner/README.md", "按组件索引的 API 参考"),
        ("docs/task-runner-reentrancy.md", "任务级防重入的需求分型与业务侧实现"),
        ("docs/data_race_detection.md", "多线程竞争检测（TSan / Clang 注解）"),
    ], ML, Inches(4.88), CW, col_ratios=[4.5, 6], size=11.5, row_h=Inches(0.34), head_h=Inches(0.4))
    return s


def s21_start(prs):
    s = new_slide(prs)
    header(s, "上手三步", kicker="怎么用起来")

    steps = [
        ("1", "接入", "拷贝 src/include/task_runner/ 一个目录\n链接 pthread，C++17 编译"),
        ("2", "实现", "继承 runnable_task，填 run()\n需要进度 / 收尾就加 on_tick / on_finished"),
        ("3", "驱动", "task_controller ctl(job, task_mode::sync, 1s);\nctl.start();  ...  ctl.stop(callback);"),
    ]
    top = BODY_TOP
    for num, title, desc in steps:
        block(s, ML, top, CW, Inches(1.25), fill=PANEL, radius=0.08)
        block(s, ML + Inches(0.22), top + Inches(0.24), Inches(0.78), Inches(0.78),
              fill=TEAL, shape=MSO_SHAPE.OVAL)
        tf = textbox(s, ML + Inches(0.22), top + Inches(0.42), Inches(0.78), Inches(0.5),
                     align=PP_ALIGN.CENTER)
        style_run(para(tf, first=True, space_after=0).add_run(), 22, True, WHITE).text = num

        tf = textbox(s, ML + Inches(1.25), top + Inches(0.22), CW - Inches(1.6), Inches(0.9))
        style_run(para(tf, first=True, space_after=4).add_run(), 17, True, NAVY).text = title
        for ln in desc.split("\n"):
            style_run(para(tf, space_after=1, line=1.05).add_run(), 13, False, "374151").text = ln
        top += Inches(1.42)

    note(s, "看例子最快：examples/ 下 5 个可运行程序（每个聚焦一个场景），"
            "对照 docs/task-runner-usage-guide.md",
         ML, Inches(5.85), CW, Inches(0.68), fill=PANEL2, size=14, color=NAVY, bold=True)
    return s


def s22_summary(prs):
    s = new_slide(prs)
    band = block(s, Inches(0), Inches(0), SW, Inches(2.2), fill=NAVY, shape=MSO_SHAPE.RECTANGLE)
    band.text_frame.text = ""
    tf = textbox(s, ML, Inches(0.6), CW, Inches(1.2))
    style_run(para(tf, first=True, space_after=6).add_run(), 30, True, WHITE).text = \
        "业务只填内容，编排交给框架"
    style_run(para(tf, space_after=0).add_run(), 16, False, "BFD7E4").text = \
        "652 行头文件，替业务挡掉 6 类并发陷阱"

    bullets(s, [
        (0, "省代码：一个可中断、带进度、带收尾的任务 ≈ 15 行业务代码", True),
        (0, "少踩坑：join / 竞态 / 死锁 / 重入 / 异常逃逸 / 完成漏报，全部在框架内解决", True),
        (0, "可预期：三态结果 + 分级错误码 + 明确时序保证（running()==false ⟹ 回调已返回）", True),
        (0, "易集成：仅头文件、零第三方依赖，拷一个目录即用", True),
        (0, "有背书：52 项测试全绿 · TSan 0 竞争 · 5 个示例 · 完整文档链", True),
    ], top=Inches(2.65), size=15.5, gap=13)

    note(s, "仓库内可直接运行：./build/CplushMultiThread   |   ./build/examples/sync_task",
         ML, Inches(6.35), CW, Inches(0.6), fill=PANEL, size=13, color=MUTED)
    return s


def main():
    prs = Presentation()
    prs.slide_width, prs.slide_height = SW, SH

    builders = [s01_cover, s02_value, s03_pain, s04_arch, s05_components,
                s06_module_files, s07_class_diagram, s08_state_machine,
                s09_advantage1, s10_result, s11_callbacks, s12_thread_model,
                s13_safety, s14_portable, s15_case1, s16_case2, s17_case3,
                s18_case4, s19_case5, s20_quality, s21_start, s22_summary]

    for i, b in enumerate(builders, start=1):
        slide = b(prs)
        if i > 1:  # 封面不编页码
            footer(slide, i)

    out_dir = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(out_dir, "task_runner_overview.pptx")
    prs.save(out)
    print(f"已生成：{out}（{len(builders)} 页）")


if __name__ == "__main__":
    main()
