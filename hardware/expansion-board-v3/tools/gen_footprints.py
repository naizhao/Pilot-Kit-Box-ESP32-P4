#!/usr/bin/env python3
"""生成自定义封装（expansion-board-v3.pretty/*.kicad_mod）。

数据来源：
  BNO085_LGA-28     : datasheet Fig 7-2 600dpi 程序化实测（tools/measure_bno_land.py，
                      28 焊盘、包络 3.803×5.211 校验通过），坐标已 0.025 网格取整
  ATGM336H_LCC-18   : 中科微手册 §2.1/2.2：pad 0.9×0.8，pitch 1.1，列距 9.7，
                      首末 pad 中心距边 0.65（几何闭合 0.65+8×1.1+0.65=10.1 ✓）
  AD8319_LFCSP-8-EP : ADI CP-8-23 outline（pitch 0.5 / lead 0.4×0.23 / EP 1.74×0.45），
                      pad 按 IPC nominal 外延 0.35/内缩 0.05 → 0.80×0.30

断言：每个封装焊盘数与期望一致；生成后必须过 kicad-cli fp 解析。
"""
import os
import sys

HDR = '(footprint "{name}"\n\t(version 20231120)\n\t(generator "gen_footprints.py")\n\t(layer "F.Cu")\n\t(attr smd)\n'

def prop(k, v, y, hide=False):
    h = "\n\t\t(hide yes)" if hide else ""
    return (f'\t(property "{k}" "{v}"\n\t\t(at 0 {y} 0)\n\t\t(layer "F.SilkS" )'
            f'\n\t\t(effects (font (size 1 1) (thickness 0.15))){h}\n\t)\n').replace(' )', '")').replace('"F.SilkS")', '"F.SilkS")')

def prop2(k, v, y, layer="F.Fab", hide=False):
    h = "\n\t\t(hide yes)" if hide else ""
    return (f'\t(property "{k}" "{v}"\n\t\t(at 0 {y} 0)\n\t\t(layer "{layer}"){h}\n'
            f'\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)\n')

def pad(num, x, y, w, h):
    return (f'\t(pad "{num}" smd roundrect\n\t\t(at {x:g} {y:g})\n\t\t(size {w:g} {h:g})\n'
            f'\t\t(layers "F.Cu" "F.Paste" "F.Mask")\n\t\t(roundrect_rratio 0.25)\n\t)\n')

def rect(layer, x0, y0, x1, y1, width=0.1):
    return (f'\t(fp_rect (start {x0:g} {y0:g}) (end {x1:g} {y1:g})\n'
            f'\t\t(stroke (width {width:g}) (type solid)) (fill no) (layer "{layer}")\n\t)\n')

def circle(layer, cx, cy, r, width=0.15):
    return (f'\t(fp_circle (center {cx:g} {cy:g}) (end {cx + r:g} {cy:g})\n'
            f'\t\t(stroke (width {width:g}) (type solid)) (fill no) (layer "{layer}")\n\t)\n')


def footprint(name, desc, pads, body_w, body_h, pin1_xy):
    out = [HDR.format(name=name)]
    out.append(f'\t(descr "{desc}")\n')
    out.append(prop2("Reference", "REF**", -body_h / 2 - 1.5, "F.SilkS"))
    out.append(prop2("Value", name, body_h / 2 + 1.5, "F.Fab"))
    out.append(prop2("Datasheet", "", body_h / 2 + 3, "F.Fab", hide=True))
    out.append(prop2("Description", desc, body_h / 2 + 4.5, "F.Fab", hide=True))
    for p in pads:
        out.append(pad(*p))
    # 外形（Fab）+ 禁布（Courtyard，四边 +0.5）
    out.append(rect("F.Fab", -body_w / 2, -body_h / 2, body_w / 2, body_h / 2))
    ext_w = max([abs(p[1]) + p[3] / 2 for p in pads] + [body_w / 2]) + 0.5
    ext_h = max([abs(p[2]) + p[4] / 2 for p in pads] + [body_h / 2]) + 0.5
    out.append(rect("F.CrtYd", -ext_w, -ext_h, ext_w, ext_h, 0.05))
    # pin1 标记
    out.append(circle("F.SilkS", pin1_xy[0], pin1_xy[1], 0.15))
    out.append(')\n')
    return "".join(out)


FOOTPRINTS = {}

# ---- BNO085_LGA-28（实测表，见 /tmp/bno_pads.tsv 存档副本 tools/bno_pads_measured.tsv）----
BNO_PADS = [
    ("1", 1.55, -2.25, 0.675, 0.25), ("2", 0.75, -2.325, 0.25, 0.575),
    ("3", 0.25, -2.325, 0.25, 0.575), ("4", -0.25, -2.325, 0.25, 0.575),
    ("5", -0.75, -2.325, 0.25, 0.575), ("6", -1.55, -2.25, 0.675, 0.25),
    ("7", -1.55, -1.75, 0.675, 0.25), ("8", -1.55, -1.25, 0.675, 0.25),
    ("9", -1.55, -0.75, 0.675, 0.25), ("10", -1.55, -0.25, 0.675, 0.25),
    ("11", -1.55, 0.25, 0.675, 0.25), ("12", -1.55, 0.75, 0.675, 0.25),
    ("13", -1.55, 1.25, 0.675, 0.25), ("14", -1.55, 1.75, 0.675, 0.25),
    ("15", -1.55, 2.25, 0.675, 0.25), ("16", -0.75, 2.325, 0.25, 0.575),
    ("17", -0.25, 2.325, 0.25, 0.575), ("18", 0.25, 2.325, 0.25, 0.575),
    ("19", 0.75, 2.325, 0.25, 0.575), ("20", 1.55, 2.25, 0.675, 0.25),
    ("21", 1.55, 1.75, 0.675, 0.25), ("22", 1.55, 1.25, 0.675, 0.25),
    ("23", 1.55, 0.75, 0.675, 0.25), ("24", 1.55, 0.25, 0.675, 0.25),
    ("25", 1.55, -0.25, 0.675, 0.25), ("26", 1.55, -0.75, 0.675, 0.25),
    ("27", 1.55, -1.25, 0.675, 0.25), ("28", 1.55, -1.75, 0.675, 0.25),
]
assert len(BNO_PADS) == 28
FOOTPRINTS["BNO085_LGA-28"] = footprint(
    "BNO085_LGA-28", "CEVA BNO08X LGA-28 3.8x5.2mm, land per DS 1000-3927 v1.16 Fig7-2 (measured)",
    BNO_PADS, 3.8, 5.2, (2.2, -2.25))

# ---- ATGM336H_LCC-18（手册数值）----
# 顶视：左列上→下 = 10..18，右列下→上 = 1..9；pad 0.9(X)×0.8(Y)，pitch 1.1，列心距 9.7
atgm = []
for i in range(9):  # 左列 10..18
    atgm.append((str(10 + i), -4.85, -4.4 + i * 1.1, 0.9, 0.8))
for i in range(9):  # 右列 9..1（上→下 9,8,...,1）
    atgm.append((str(9 - i), 4.85, -4.4 + i * 1.1, 0.9, 0.8))
assert len(atgm) == 18 and len({p[0] for p in atgm}) == 18
FOOTPRINTS["ATGM336H_LCC-18"] = footprint(
    "ATGM336H_LCC-18", "ZKW ATGM336H LCC-18 10.1x9.7mm, land per manual 2.1/2.2 (pad 0.9x0.8 pitch 1.1)",
    atgm, 9.7, 10.1, (5.9, 4.4))  # pin1 右下

# ---- AD8319_LFCSP-8-EP（CP-8-23 IPC 推导）----
# 引脚两长边各 4：pitch 0.5，行 span 1.5；引脚沿 X 方向伸出（体宽 2.0）
# pad 0.80×0.30，中心 X=±0.90（外缘 1.30，覆盖 lead tip 1.00 + toe 0.30）；EP 1.74×0.45 居中
ad = []
for i in range(4):  # 左列 1..4 上→下
    ad.append((str(1 + i), -0.90, -0.75 + i * 0.5, 0.80, 0.30))
for i in range(4):  # 右列 5..8 下→上
    ad.append((str(5 + i), 0.90, 0.75 - i * 0.5, 0.80, 0.30))
ad.append(("9", 0, 0, 0.45, 1.74))  # EP：datasheet 1.74 沿长轴(Y)，0.45 沿短轴(X)
assert len(ad) == 9
FOOTPRINTS["AD8319_LFCSP-8-EP"] = footprint(
    "AD8319_LFCSP-8-EP", "ADI CP-8-23 LFCSP 3x2mm P0.5, EP0.45x1.74 (IPC from outline, ADI no rec. pattern)",
    ad, 2.0, 3.0, (-1.6, -0.75))


# ---- BMP388_LGA-10（Fig 26 bottom view 读数，footprint=顶视=底视 X 镜像；pin1 右上）----
# 底视编号顺时针：1,2 顶中(左,右)→3,4,5 右列→6,7 底中(右,左)→8,9,10 左列(下→上)
# 顶视(X 镜像)：1(+0.25,顶) 2(-0.25,顶)；3,4,5 左列上→下；6(-0.25,底) 7(+0.25,底)；8,9,10 右列下→上
_bmp = [
    ("1",  0.25, -0.7625, 0.250, 0.275), ("2", -0.25, -0.7625, 0.250, 0.275),
    ("3", -0.7625, -0.5, 0.275, 0.250), ("4", -0.7625, 0.0, 0.275, 0.250),
    ("5", -0.7625, 0.5, 0.275, 0.250),
    ("6", -0.25, 0.7625, 0.250, 0.275), ("7", 0.25, 0.7625, 0.250, 0.275),
    ("8", 0.7625, 0.5, 0.275, 0.250), ("9", 0.7625, 0.0, 0.275, 0.250),
    ("10", 0.7625, -0.5, 0.275, 0.250),
]
assert len(_bmp) == 10 and len({p[0] for p in _bmp}) == 10
FOOTPRINTS["BMP388_LGA-10"] = footprint(
    "BMP388_LGA-10", "Bosch BMP388 LGA-10 2x2mm, land=bottom view 1:1 per DS001-07 sec7.2 (cols 1.525, pitch 0.5, edge 0.1)",
    _bmp, 2.0, 2.0, (1.2, -0.7625))

# ---- TA0970A_SMD3838-6（TST Rev3.0 §F footprint 图：4.0×3.34，行 pitch 1.27，pad 高 0.8）----
# 顶视：左列 A(上,GND,2.2长) B(中,IN,1.2) C(下,GND,1.2)；右列 F(上,GND,1.2) E(中,OUT,1.2) D(下,GND,1.2)
_ta = [
    ("A", -0.9, -1.27, 2.2, 0.8),   # 2.2 长焊盘：-2.0..+0.2
    ("B", -1.4, 0.0, 1.2, 0.8),
    ("C", -1.4, 1.27, 1.2, 0.8),
    ("F", 1.4, -1.27, 1.2, 0.8),
    ("E", 1.4, 0.0, 1.2, 0.8),
    ("D", 1.4, 1.27, 1.2, 0.8),
]
assert len(_ta) == 6
FOOTPRINTS["TA0970A_SMD3838-6"] = footprint(
    "TA0970A_SMD3838-6", "TST TA0970A 1090MHz SAW 3.8x3.8, land per Rev3.0 secF (B=IN E=OUT, A long GND 2.2)",
    _ta, 3.8, 3.8, (-2.3, -1.27))

# ---- MM8930-2620（2026-08-01 已照 O30E 物理 p.14 原图复核）----
# 左图：信号纵向贯穿，馈线 0.80、非阻焊窗 0.70、land 0.30；右图：R/C land 条带高 0.45、
# 外缘跨距 1.70 → 条带中心 ±0.625；地 land 0.45×0.25，纵向内/外缘 1.30/1.80 → 中心 ±0.775；
# 钢网图横向 1.60/1.10 → 地列中心 ±0.675。投板前 DFM 再按 Murata "must follow" 口径核阻焊细节。
_mm = [
    ("R", 0.0, -0.625, 0.30, 0.45),
    ("C", 0.0, 0.625, 0.30, 0.45),
    ("G", -0.675, -0.775, 0.45, 0.25),
    ("G", 0.675, -0.775, 0.45, 0.25),
    ("G", -0.675, 0.775, 0.45, 0.25),
    ("G", 0.675, 0.775, 0.45, 0.25),
]
FOOTPRINTS["MM8930-2620"] = footprint(
    "MM8930-2620", "Murata SWH-2Way switch connector 1.6x1.6, verified vs O30E phys-p14 (mask detail per DFM)",
    _mm, 1.6, 1.6, (0.0, -1.3))


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for name, content in FOOTPRINTS.items():
        with open(os.path.join(outdir, f"{name}.kicad_mod"), "w") as f:
            f.write(content)
    assert len(FOOTPRINTS) == 6, f"期望 6 个封装，实际 {len(FOOTPRINTS)}"
    print(f"OK: {len(FOOTPRINTS)} footprints → {outdir}")


if __name__ == "__main__":
    main()
