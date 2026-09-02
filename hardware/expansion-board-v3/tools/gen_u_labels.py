#!/usr/bin/env python3
"""在每颗 IC 的中心标上芯片型号。

2026-08-13 评审："在空间足够的前提下，在每个 U 的中心我希望丝印模块类型或者名字。"

用途是**空板上一眼看出这里该焊什么**——贴片之后芯片会盖住，但那时也不需要了；
调试、返修、核对 BOM 时对着空板看最有用。

## 两种情况分开处理（这是必须的，不是讲究）

    中心是空的（13 个）  → F.SilkS，真丝印，印得出来
    中心是 EP 焊盘（4 个：U8/U10/U11/U13）→ F.Fab

QFN/DFN 的中心是散热焊盘，那里有阻焊开窗和锡膏开窗，**丝印油墨根本印不上去**
（就算硬放，DRC 也会报 silk_over_copper，出厂前还是得删）。F.Fab 是装配图层，
不参与印刷、不影响制造，但 KiCad 里看得见、assembly-top.pdf 里也有。

## 字号

按本体尺寸自适应：字宽 ≈ 0.6×字高，要让整串型号横着放得下，
同时不小于 0.8mm（板规 min_text_height，也是嘉立创丝印可印下限）。
放不下就退到 F.Fab——**宁可不印，也不印成一团糊**。

型号取自 sheet_*.py 里 s.place() 的符号名/value（BOM 用的也是这个来源），
写死在下面是因为 sheet 文件里几种写法不统一，解析出来反而不可靠。

用法：gen_u_labels.py [plan|apply]     幂等，重复跑不叠加
"""
import os
import re
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
MODE = sys.argv[1] if len(sys.argv) > 1 else "plan"

# 型号来源：sheet_*.py 的 s.place() 符号名 / value=，与 gen_bom.py 同一口径。
# 括号里的备注（"备,二选一"之类）不进丝印——板上要的是焊什么，不是采购说明。
# 长型号在小封装上横着放不下（TPS7A2033 放进 SOT-23-5 只有 0.70mm 字高，
# 低于可印下限），此时用**功能标识**代替型号——对着空板调试时，你想知道的
# 本来也是"这颗出什么电/干什么的"，而不是厂商料号。料号在 BOM 和装配图里有。
MODELS = {
    "U1": "3V3-D", "U2": "3V3-RF", "U3": "3V3-G",     # 三路 LDO，标输出轨比标型号有用
    "U4": "BNO085", "U5": "BMP388", "U6": "QMC5883", "U7": "ATGM336H",
    "U8": "RP2040", "U9": "W25Q128", "U10": "CC1312R",
    "U11": "LNA1", "U12": "LNA2", "U14": "AD8313",
    "U15": "TLV3501", "U16": "SW1090", "U17": "SWGNSS",
}

MIN_H = 0.8          # 板规 min_text_height，也是嘉立创丝印可印下限
MAX_H = 1.2
# ⚠️ KiCad 描边字体的**字宽 ≈ 字高**，再加约 0.1×h 的字距，所以每字符约占 1.1×h。
# 第一版按 0.62 算，估算"放得下"的文字实际宽了近一倍，一放上去就压焊盘
# （DRC 报 17 条 silk_over_copper）。这个系数错了，后面所有几何判断都白算。
CHAR_W = 1.10
THICK = 0.15         # 嘉立创丝印线宽下限

board = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM

# 幂等：先清掉本脚本之前加的（按文字内容匹配，且在 IC 中心附近）
old = 0
_want = set(MODELS.values())
for d in list(board.GetDrawings()):
    if d.GetClass() in ("PCB_TEXT", "PCB_TEXTBOX") and d.GetText() in _want:
        board.Remove(d)
        old += 1
if old:
    board.Save(PCB)
    board = pcbnew.LoadBoard(PCB)      # Remove 之后 SWIG 状态不可靠，重新载入

plan = []
for f in sorted(board.GetFootprints(), key=lambda z: z.GetReference()):
    ref = f.GetReference()
    if ref not in MODELS:
        continue
    txt = MODELS[ref]
    c = f.GetPosition()
    cx, cy = mm(c.x), mm(c.y)
    cy_box = f.GetCourtyard(pcbnew.F_CrtYd).BBox()
    if cy_box.GetWidth() <= 0:
        cy_box = f.GetBoundingBox()
    bw, bh = mm(cy_box.GetWidth()), mm(cy_box.GetHeight())

    # ⚠️ 判据必须用**文字的包围盒**，不能只看中心点落没落在焊盘上。
    # 第一版只查中心点，结果 BNO085 那串宽 4.5mm、横跨整颗芯片，两端压在左右
    # 两排引脚上——DRC 一口气报 29 条 silk_over_copper。
    # 文字压焊盘的后果是实打实的：阻焊开窗处印不上油墨，而且会影响焊接。
    # ⚠️ 必须查**所有元件**的焊盘，不能只查自己的：实测 U14 的 'AD8313' 压到了
    # 隔壁 R21 的焊盘。文字压在谁的焊盘上都一样印不出来。
    pads = []
    for _f2 in board.GetFootprints():
        for p in _f2.Pads():
            if not p.IsOnCopperLayer():
                continue
            bb = p.GetBoundingBox()
            if abs(mm(bb.GetLeft()) - cx) > 12 or abs(mm(bb.GetTop()) - cy) > 12:
                continue                      # 12mm 外的不可能压到，跳过省时间
            pads.append((f"{_f2.GetReference()}.{p.GetNumber()}",
                         mm(bb.GetLeft()), mm(bb.GetTop()),
                         mm(bb.GetRight()), mm(bb.GetBottom())))

    def hits(h, rot):
        """字高 h、转角 rot 时，文字包围盒压到哪个焊盘"""
        tw, th = len(txt) * CHAR_W * h, h
        if rot:
            tw, th = th, tw
        a = (cx - tw / 2 - 0.05, cy - th / 2 - 0.05,
             cx + tw / 2 + 0.05, cy + th / 2 + 0.05)      # 留 0.05 净空
        for num, x0, y0, x1, y1 in pads:
            if not (a[2] <= x0 or a[0] >= x1 or a[3] <= y0 or a[1] >= y1):
                return num
        return None

    # 横排、竖排都试，每种都从最大字号往下缩到 MIN_H。
    # **横排压焊盘不代表竖排也压**：SOT-23-5 这类两侧排焊盘的封装，中间那条竖缝
    # 往往容得下竖排文字，而横排必然横跨两侧。第一版只在"横排放不下"时才试竖排
    # （压焊盘不算放不下），于是 17 颗里只有 2 颗能印。
    best = None
    for _rot in (0, 90):
        lim_along = (bh if _rot else bw) - 0.3       # 文字长度方向可用尺寸
        lim_across = (bw if _rot else bh) - 0.3      # 字高方向
        _h = min(MAX_H, lim_along / (len(txt) * CHAR_W), lim_across)
        while _h >= MIN_H:
            if not hits(round(_h, 2), _rot):
                best = (round(_h, 2), _rot)
                break
            _h = round(_h - 0.05, 2)
        if best:
            break
    if best:
        h, rot, on_pad, layer = best[0], best[1], None, "F.SilkS"
        why = "竖排" if rot else ""
    else:
        h, rot, layer = MIN_H, 0, "F.Fab"
        on_pad = hits(MIN_H, 0)
        why = ("压 pad%s" % on_pad) if on_pad else "空间不足"
    h = max(h, MIN_H)
    plan.append((ref, txt, cx, cy, round(h, 2), rot, layer, why))

print(f"{'件号':5s} {'型号':11s} {'字高':5s} {'转':3s} {'层':9s} 说明")
for ref, txt, x, y, h, rot, ly, why in plan:
    print(f"{ref:5s} {txt:11s} {h:4.2f}  {rot:3d} {ly:9s} {why}")
n_silk = sum(1 for p in plan if p[6] == "F.SilkS")
print(f"\nF.SilkS {n_silk} 个（真丝印）/ F.Fab {len(plan)-n_silk} 个（装配图层，中心是焊盘或放不下）")

if MODE == "apply":
    LID = {"F.SilkS": pcbnew.F_SilkS, "F.Fab": pcbnew.F_Fab}
    for ref, txt, x, y, h, rot, ly, _ in plan:
        t = pcbnew.PCB_TEXT(board)
        t.SetText(txt)
        t.SetPosition(pcbnew.VECTOR2I_MM(round(x, 3), round(y, 3)))
        t.SetLayer(LID[ly])
        t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(h), pcbnew.FromMM(h)))
        t.SetTextThickness(pcbnew.FromMM(min(THICK, h * 0.18)))
        t.SetTextAngleDegrees(rot)
        t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_CENTER)
        board.Add(t)
    board.Save(PCB)
    print(f"\n已加 {len(plan)} 条 → {PCB}")
    print("⚠️ 跑一次 kicad-cli drc：F.SilkS 那些要确认没压到焊盘")
else:
    print("\n（plan 模式，未写盘）")
