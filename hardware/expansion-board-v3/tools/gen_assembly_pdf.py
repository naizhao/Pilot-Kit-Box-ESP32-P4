#!/usr/bin/env python3
"""生成 V3 装配图 PDF —— 按板上实际位置、方向、外形标出每个位号该装什么。

一次出两份，内容同源：

    ASSEMBLY_MAP.pdf          公开·英文
    ASSEMBLY_MAP-zh_CN.pdf    公开·中文

每份两页：装配图 / 位号速查表。

## 与 V4 那份的区别

V4 分七类，其中「电源区 / 无电源版」来自它的两个装配变体。**V3 没有变体**——
它没有 CH224K/SY6970/SY7069 那套板载电源，全板只有一种贴法。照抄七类会凭空多出
两个永远为空的图例。这里是五类：

    常规 / 天线切换 / IFA匹配 / 调试位 / 不贴

V3 还是**单面装配**（B 面禁止元件），所以不需要 V4 那个"（反面）"文字维度。

## 三个视觉维度互不重叠

    颜色 = 类型（五类）      线型 = 贴不贴（实线贴 / 虚线不贴）
    位号画在框外，元件值画在框内

## 渲染路径：手写 SVG → headless Chrome 打印 PDF

沿用 V4 的做法。不用 reportlab（两个解释器都没装）、也不用 cairosvg（系统缺
原生 libcairo）。Chrome 本来就在，中文直接走系统 fontconfig，不必操心字体嵌入。

## 几何取自板子本身

位置、旋转、courtyard 外形都从 .kicad_pcb 读，不读 PLACEMENT.py —— 后者是输入，
板子才是结果，两者一旦不同步，以板为准。

用法：**KiCad 的 python3** tools/gen_assembly_pdf.py
"""
import html as html_mod
import os
import subprocess
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from board_meta import BOARD_REV, PCB_BASENAME          # noqa: E402

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, f"{PCB_BASENAME}.kicad_pcb")
PW, PH = 420, 297                                        # A3 横向 mm

DNP_REFS = {"R24", "R25", "R30", "R36", "ZP1", "ZP2"}
IFA_MATCH = {"ZP1", "ZS1", "ZP2"}
ANT_SWITCH = {"U16", "U17", "Q2", "Q3", "Q4", "Q5", "R17", "R18",
              "R22", "R23", "R26", "R27", "F3", "F4", "F5",
              "L2", "L14", "L15", "C30", "C53", "C54", "C55", "C56",
              "C57", "C58", "C59"}
DEBUG_PADS = {f"TP{i}" for i in range(1, 8)} | {"SW1", "SW2"}
NO_PART_FP = ("TestPoint", "MountingHole", "SolderJumper", "ANT_IFA")

CATEGORIES = (
    ("normal", "常规", "Standard", "#3b75b8"),
    ("ant", "天线切换", "Antenna switching", "#d98421"),
    ("ifa", "IFA 匹配", "IFA matching", "#8c59b3"),
    ("debug", "调试位", "Debug pads", "#4c9e66"),
    ("skip", "不贴", "Not populated", "#8c8c8c"),
)
COLOR = {k: c for k, _z, _e, c in CATEGORIES}


def classify(reference, footprint_name):
    # ⚠️ 顺序有讲究。TP1-7 / SW1/SW2 的封装是 TestPoint / SolderJumper，
    # 都落在 NO_PART_FP 里 —— 先判"无器件"的话，调试位这一类永远是空的
    # （实测第一版就是 debug=0，而板上明明有 9 个）。
    # 它们确实不装器件，但在装配图上要单独成类：那是拿探针和短接线的人要找的东西，
    # 跟安装孔、天线铜箔不是一回事。
    if reference in DEBUG_PADS:
        return "debug"
    if reference in IFA_MATCH:
        return "ifa"
    if reference in DNP_REFS or any(s in footprint_name for s in NO_PART_FP):
        return "skip"
    if reference in ANT_SWITCH:
        return "ant"
    return "normal"


def net_labels(board):
    """调试位显示它接的**信号名**，不显示封装名。

    TP1-7 的 Value 是 "TestPoint"、SW1/SW2 是 "RESET短接焊盘"，这些字对拿探针的人
    没有信息量——他要找的是"SWCLK 在哪"。所以这几个位号的框内文字换成网络名。
    SW1/SW2 取非 GND 的那个网络（两脚一个接地、一个接信号）。
    """
    labels = {}
    for footprint in board.GetFootprints():
        reference = footprint.GetReference()
        if reference not in DEBUG_PADS:
            continue
        nets = [p.GetNetname() for p in footprint.Pads()
                if p.GetNetname() and p.GetNetname() != "GND"]
        if nets:
            labels[reference] = nets[0]
    return labels


def collect(board):
    labels = net_labels(board)
    items = []
    for footprint in board.GetFootprints():
        reference = footprint.GetReference()
        name = footprint.GetFPIDAsString().split(":")[-1]
        box = footprint.GetCourtyard(pcbnew.F_CrtYd).BBox()
        if box.GetWidth() == 0 or box.GetHeight() == 0:
            box = footprint.GetBoundingBox(False, False)
        items.append({
            "ref": reference,
            "value": labels.get(reference, footprint.GetValue()),
            "cat": classify(reference, name),
            "x": box.GetLeft() / 1e6, "y": box.GetTop() / 1e6,
            "w": box.GetWidth() / 1e6, "h": box.GetHeight() / 1e6,
            "cx": footprint.GetPosition().x / 1e6,
            "cy": footprint.GetPosition().y / 1e6,
            "rot": footprint.GetOrientationDegrees(),
        })

    def key(item):
        digits = "".join(c for c in item["ref"] if c.isdigit())
        prefix = "".join(c for c in item["ref"] if not c.isdigit())
        return (prefix, int(digits or 0))
    return sorted(items, key=key)


def esc(text):
    return html_mod.escape(str(text))


def page_map(items, edge, lang):
    bx, by = edge.GetLeft() / 1e6, edge.GetTop() / 1e6
    bw, bh = edge.GetWidth() / 1e6, edge.GetHeight() / 1e6
    margin, top = 16, 26
    scale = min((PW - 2 * margin) / bw, (PH - top - 34) / bh)
    ox, oy = margin, top

    def X(v):
        return ox + (v - bx) * scale

    def Y(v):
        return oy + (v - by) * scale

    zh = lang == "zh"
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{PW}mm" '
           f'height="{PH}mm" viewBox="0 0 {PW} {PH}">',
           '<rect width="100%" height="100%" fill="#fff"/>',
           f'<text x="{margin}" y="12" font-size="6" font-weight="600">'
           f'{esc("扩展板 " + BOARD_REV + " 装配图" if zh else "Expansion Board " + BOARD_REV + " Assembly Map")}</text>',
           f'<text x="{margin}" y="19" font-size="3.1" fill="#444">'
           f'{esc("单面装配：所有元件在正面 F.Cu，背面无元件；虚线框 = 不贴" if zh else "Single-sided: all parts on F.Cu, back side empty; dashed = not populated")}</text>',
           f'<rect x="{X(bx):.2f}" y="{Y(by):.2f}" width="{bw*scale:.2f}" '
           f'height="{bh*scale:.2f}" fill="none" stroke="#222" stroke-width="0.4"/>']
    for it in items:
        color = COLOR[it["cat"]]
        dash = ' stroke-dasharray="0.8,0.8"' if it["cat"] == "skip" else ""
        w, h = max(it["w"] * scale, 1.4), max(it["h"] * scale, 1.4)
        out.append(f'<rect x="{X(it["x"]):.2f}" y="{Y(it["y"]):.2f}" '
                   f'width="{w:.2f}" height="{h:.2f}" fill="{color}" '
                   f'fill-opacity="0.14" stroke="{color}" stroke-width="0.28"{dash}/>')
        out.append(f'<text x="{X(it["cx"]):.2f}" y="{Y(it["cy"]) - h/2 - 0.5:.2f}" '
                   f'font-size="1.7" text-anchor="middle">{esc(it["ref"])}</text>')
        if it["value"] and it["cat"] != "skip" and (w > 5 or it["cat"] == "debug"):
            out.append(f'<text x="{X(it["cx"]):.2f}" y="{Y(it["cy"]) + 0.6:.2f}" '
                       f'font-size="1.35" fill="#555" text-anchor="middle">'
                       f'{esc(it["value"][:14])}</text>')
    ly = PH - 16
    for i, (key, zh_name, en_name, color) in enumerate(CATEGORIES):
        x = margin + i * 74
        dash = ' stroke-dasharray="0.8,0.8"' if key == "skip" else ""
        count = sum(1 for it in items if it["cat"] == key)
        out.append(f'<rect x="{x}" y="{ly}" width="9" height="5" fill="{color}" '
                   f'fill-opacity="0.14" stroke="{color}" stroke-width="0.35"{dash}/>')
        out.append(f'<text x="{x + 11}" y="{ly + 3.8}" font-size="3.4">'
                   f'{esc((zh_name if zh else en_name) + f" ({count})")}</text>')
    out.append('</svg>')
    return out


def page_index(items, lang):
    zh = lang == "zh"
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{PW}mm" '
           f'height="{PH}mm" viewBox="0 0 {PW} {PH}">',
           '<rect width="100%" height="100%" fill="#fff"/>',
           f'<text x="16" y="12" font-size="5.5" font-weight="600">'
           f'{esc("位号速查表" if zh else "Designator index")}</text>']
    per, colw = 66, 68
    for n, it in enumerate(items):
        col, row = divmod(n, per)
        x = 16 + col * colw
        y = 22 + row * 4.1
        out.append(f'<circle cx="{x + 1.4}" cy="{y - 1}" r="1.1" '
                   f'fill="{COLOR[it["cat"]]}"/>')
        out.append(f'<text x="{x + 4}" y="{y}" font-size="2.5">'
                   f'{esc(it["ref"]):<7}</text>')
        out.append(f'<text x="{x + 14}" y="{y}" font-size="2.5" fill="#333">'
                   f'{esc(it["value"][:16])}</text>')
        out.append(f'<text x="{x + 42}" y="{y}" font-size="2.3" fill="#777">'
                   f'({it["cx"]:.1f},{it["cy"]:.1f}) {it["rot"]:g}°</text>')
    out.append('</svg>')
    return out


def render(out_pdf, svgs):
    html_path = out_pdf.replace(".pdf", ".html")
    open(html_path, "w", encoding="utf-8").write(
        '<!doctype html><meta charset="utf-8">'
        f'<style>@page{{size:{PW}mm {PH}mm;margin:0}}'
        'html,body{margin:0;padding:0}svg{display:block;page-break-after:always}'
        '</style>' + "".join("".join(s) for s in svgs))
    chrome = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    if not os.path.exists(chrome):
        print(f"  ⚠️ 找不到 Chrome，已留下 {os.path.relpath(html_path, T)} 供手工打印")
        return False
    argv = [chrome, "--headless", "--disable-gpu", "--no-pdf-header-footer",
            f"--print-to-pdf={out_pdf}", f"file://{html_path}"]
    # 比对 mtime 而不是只判存在：Chrome 偶尔会卡住，失败模式是"旧文件还在、
    # 内容是上一版"，不报任何错。
    for attempt in (1, 2):
        before = os.path.getmtime(out_pdf) if os.path.exists(out_pdf) else 0
        try:
            result = subprocess.run(argv, capture_output=True, text=True, timeout=180)
            err = result.stderr[-300:]
        except subprocess.TimeoutExpired:
            err = "Chrome 超时 180s"
        if os.path.exists(out_pdf) and os.path.getmtime(out_pdf) > before:
            os.unlink(html_path)
            return True
        if attempt == 2:
            print(f"  ❌ Chrome 没重新生成 {os.path.basename(out_pdf)}：{err}")
            return False
        print(f"  {os.path.basename(out_pdf)} 没生成，重试一次…")
    return False


def main() -> int:
    board = pcbnew.LoadBoard(PCB)
    items = collect(board)
    edge = board.GetBoardEdgesBoundingBox()

    # 分类必须盖满：漏一个位号 = 装配图上少一条贴法约束
    assert all(it["cat"] in COLOR for it in items), "有位号没被分类"
    for lang, name in (("zh", "ASSEMBLY_MAP-zh_CN.pdf"), ("en", "ASSEMBLY_MAP.pdf")):
        out = os.path.join(T, name)
        ok = render(out, [page_map(items, edge, lang), page_index(items, lang)])
        if ok:
            size = os.path.getsize(out) / 1024
            print(f"  → {name}  {len(items)} 个位号 / 2 页 / {size:.0f} KB")
    counts = {}
    for it in items:
        counts[it["cat"]] = counts.get(it["cat"], 0) + 1
    print(f"  分类: {counts}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
