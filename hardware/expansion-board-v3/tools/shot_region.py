#!/usr/bin/env python3
"""按 PCB 坐标截取局部图——自己画，坐标完全可控。

⚠️ 不要指望 magick 渲染这个 SVG：它的 svg 代理是 rsvg-convert（本机没装），
回退到内置 MSVG 渲染器，而**内置渲染器根本不画 <line>**——rect 出得来、走线全丢，
调了半天以为是坐标或线宽问题，其实是渲染器不支持。
所以这里输出 **JSON 几何**，由 draw_region.py（系统 python3 + PIL）直接画 PNG。
SVG 仍然一并写出，方便丢进浏览器/Inkscape 看。

## 为什么不用 kicad-cli export svg

它的 SVG 有说不清的偏移：viewBox 是 0-99.9998 而板框是 100.1mm 宽，反推出来的
原点对不上，裁出来的图完全不是目标位置（试过两次都截错地方）。与其猜它的坐标
系，不如直接读 pcbnew 的几何自己生成 SVG——viewBox 直接用 PCB 的 mm 坐标，
一一对应，不会错。

画什么：F.Cu 红 / In2 橙 / B.Cu 蓝 / 过孔 深灰 / 焊盘 按层 / 缺口两端画标记。
飞线（未连通的那一段）画成绿色虚线，一眼能看出"该连没连"。

用法：shot_region.py <net> [半径mm]      按该网络的缺口自动定位
      shot_region.py --at x y [半径mm]   指定中心
"""
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

board = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM
LN = {board.GetLayerID(n): n for n in
      ("F.Cu", "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "B.Cu")}
COLOR = {"F.Cu": "#d03030", "In2.Cu": "#e08010", "B.Cu": "#3050c0",
         "In1.Cu": "#808080", "In3.Cu": "#40a040", "In4.Cu": "#a040a0"}

args = sys.argv[1:]
if args and args[0] == "--at":
    CX, CY = float(args[1]), float(args[2])
    RAD = float(args[3]) if len(args) > 3 else 6.0
    NET, TAG = None, f"at_{CX:.0f}_{CY:.0f}"
else:
    NET = args[0]
    RAD = float(args[1]) if len(args) > 1 else 6.0
    TAG = NET
    # 用未连通的两块铜之间的缺口中点定位
    sys.argv = ["route_fix", "plan", os.path.join(BUILD, "drc-final.json")]
    import route_fix as R                                          # noqa: E402
    R.build()
    n = board.FindNet(NET)
    blks = R.blocks_of(n.GetNetCode())
    best = None
    for i in range(len(blks)):
        for j in range(i + 1, len(blks)):
            for a in R.block_cells(blks[i]):
                for b in R.block_cells(blks[j]):
                    if a[2] != b[2]:
                        continue
                    d = (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2
                    if best is None or d < best[0]:
                        best = (d, a, b)
    assert best, f"{NET} 找不到同层缺口"
    ax, ay = R.g2m(best[1][0], best[1][1])
    bx, by = R.g2m(best[2][0], best[2][1])
    CX, CY = (ax + bx) / 2, (ay + by) / 2
    GAP = ((ax, ay), (bx, by))

x0, y0, x1, y1 = CX - RAD, CY - RAD, CX + RAD, CY + RAD
W = 2 * RAD
SCALE = 100.0            # 1mm = 100 SVG 单位，见文件头的说明


def S(v):
    return v * SCALE


def inbox(px, py, m=2.0):
    return x0 - m <= px <= x1 + m and y0 - m <= py <= y1 + m


out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="1400" height="1400" '
       f'viewBox="{S(x0):.1f} {S(y0):.1f} {S(W):.1f} {S(W):.1f}">',
       f'<rect x="{S(x0):.1f}" y="{S(y0):.1f}" width="{S(W):.1f}" '
       f'height="{S(W):.1f}" fill="#101820"/>']

# 焊盘
for f in board.GetFootprints():
    for p in f.Pads():
        pp = p.GetPosition()
        px, py = mm(pp.x), mm(pp.y)
        if not inbox(px, py):
            continue
        bb = p.GetBoundingBox()
        w, h = mm(bb.GetWidth()), mm(bb.GetHeight())
        col = "#e8c020" if p.GetDrillSizeX() > 0 else "#c8c8c8"
        out.append(f'<rect x="{S(px-w/2):.2f}" y="{S(py-h/2):.2f}" width="{S(w):.2f}" '
                   f'height="{S(h):.2f}" rx="5" fill="{col}" opacity="0.9"/>')

# 走线
for t in board.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA):
        continue
    a = (mm(t.GetStart().x), mm(t.GetStart().y))
    b = (mm(t.GetEnd().x), mm(t.GetEnd().y))
    if not (inbox(*a) or inbox(*b)):
        continue
    ly = LN.get(t.GetLayer(), "?")
    out.append(f'<line x1="{S(a[0]):.2f}" y1="{S(a[1]):.2f}" x2="{S(b[0]):.2f}" '
               f'y2="{S(b[1]):.2f}" stroke="{COLOR.get(ly,"#888")}" '
               f'stroke-width="{S(mm(t.GetWidth())):.2f}" stroke-linecap="round" '
               f'opacity="0.85"/>')

# 过孔
for t in board.GetTracks():
    if not isinstance(t, pcbnew.PCB_VIA):
        continue
    p = t.GetPosition()
    px, py = mm(p.x), mm(p.y)
    if not inbox(px, py):
        continue
    out.append(f'<circle cx="{S(px):.2f}" cy="{S(py):.2f}" '
               f'r="{S(mm(t.GetWidth())/2):.2f}" fill="#909090"/>')
    out.append(f'<circle cx="{S(px):.2f}" cy="{S(py):.2f}" '
               f'r="{S(mm(t.GetDrill())/2):.2f}" fill="#101820"/>')

# 缺口标记：两端画圈 + 绿色虚线连起来（= 该连没连的那一段）
if NET:
    (gx1, gy1), (gx2, gy2) = GAP
    out.append(f'<line x1="{S(gx1):.2f}" y1="{S(gy1):.2f}" x2="{S(gx2):.2f}" '
               f'y2="{S(gy2):.2f}" stroke="#20ff20" stroke-width="12" '
               f'stroke-dasharray="30 20"/>')
    for gx, gy in ((gx1, gy1), (gx2, gy2)):
        out.append(f'<circle cx="{S(gx):.2f}" cy="{S(gy):.2f}" r="40" fill="none" '
                   f'stroke="#20ff20" stroke-width="10"/>')

out.append("</svg>")
os.makedirs(os.path.join(BUILD, "shots"), exist_ok=True)
dst = os.path.join(BUILD, "shots", f"region-{TAG}.svg")
open(dst, "w").write("\n".join(out))

# JSON 几何：给 draw_region.py 用（PIL 画图，绕开 magick 不画 line 的毛病）
import json
geo = {"cx": CX, "cy": CY, "rad": RAD, "net": NET,
       "gap": [list(GAP[0]), list(GAP[1])] if NET else None,
       "pads": [], "tracks": [], "vias": []}
for f in board.GetFootprints():
    for p in f.Pads():
        pp = p.GetPosition()
        px, py = mm(pp.x), mm(pp.y)
        if not inbox(px, py):
            continue
        bb = p.GetBoundingBox()
        geo["pads"].append([px, py, mm(bb.GetWidth()), mm(bb.GetHeight()),
                            1 if p.GetDrillSizeX() > 0 else 0,
                            f"{f.GetReference()}.{p.GetNumber()}", p.GetNetname()])
for t in board.GetTracks():
    a = (mm(t.GetStart().x), mm(t.GetStart().y))
    b = (mm(t.GetEnd().x), mm(t.GetEnd().y))
    if isinstance(t, pcbnew.PCB_VIA):
        if inbox(*a):
            geo["vias"].append([a[0], a[1], mm(t.GetWidth()), mm(t.GetDrill()),
                                t.GetNetname()])
    elif inbox(*a) or inbox(*b):
        geo["tracks"].append([a[0], a[1], b[0], b[1], mm(t.GetWidth()),
                              LN.get(t.GetLayer(), "?"), t.GetNetname()])
jdst = os.path.join(BUILD, "shots", f"region-{TAG}.json")
json.dump(geo, open(jdst, "w"))
print(f"{jdst}   中心({CX:.2f},{CY:.2f}) 半径{RAD}mm  "
      f"焊盘{len(geo['pads'])} 走线{len(geo['tracks'])} 过孔{len(geo['vias'])}")
