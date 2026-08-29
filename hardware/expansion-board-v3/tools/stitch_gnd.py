#!/usr/bin/env python3
"""GND 网格缝合过孔——把各层地铜真正连成一片。

## 为什么需要

route_rf.py 的缝合过孔是**按焊盘**打的（每个 GND 焊盘就近打一个下平面），
覆铜的空白区一个都没有。结果 B.Cu 的 GND 被走线切成 102 块（其中 100 块 <1mm²），
块与块之间、以及顶层↔底层之间没有通路，KiCad 直接报
    Zone [GND] on F.Cu ↔ Zone [GND] on F.Cu
    Zone [GND] on F.Cu ↔ Zone [GND] on B.Cu
这不是"少了一根走线"，是地平面没缝合——射频板上更要命，回流路径会被迫绕远。

## 做法

在板面上按网格取点，凡是该点在 **F.Cu 和 B.Cu 的 GND 填充铜里都落得下**、
且与所有非 GND 的铜/孔保持净空的，就打一个通孔。通孔穿全部 6 层，一次把
F/In1/In4/B 四层地连起来。

判据用 HitTestFilledArea（真实填充铜），不是"在 zone 轮廓内"——轮廓内可能已被
走线避让挖空。

用法：stitch_gnd.py [plan|apply] [网格mm，默认2.5]
"""
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

MODE = sys.argv[1] if len(sys.argv) > 1 else "plan"
GRID = float(sys.argv[2]) if len(sys.argv) > 2 else 2.5
VIA_D, VIA_DRILL = 0.4, 0.2
VIA_R = VIA_D / 2
CLR = 0.2                     # 对非 GND 的净空，取板规最严的 POWER 那档
HOLE_GAP = 0.25               # 孔到孔

board = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM
GNDNC = board.FindNet("GND").GetNetCode()

# GND 在 F.Cu / B.Cu 的填充区
zf, zb = [], []
for z in board.Zones():
    if z.GetNetCode() != GNDNC or not z.IsFilled():
        continue
    for l in z.GetLayerSet().Seq():
        if l == pcbnew.F_Cu:
            zf.append((z, l))
        elif l == pcbnew.B_Cu:
            zb.append((z, l))

# 障碍：非 GND 的焊盘/走线/过孔，以及所有孔
PADS, TRKS, VIAS, HOLES = [], [], [], []
for f in board.GetFootprints():
    for p in f.Pads():
        bb = p.GetBoundingBox()
        box = (mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom()))
        if p.GetNetCode() != GNDNC:
            PADS.append(box)
        if p.GetDrillSizeX() > 0:
            pp = p.GetPosition()
            HOLES.append((mm(pp.x), mm(pp.y), mm(p.GetDrillSizeX()) / 2))
for t in board.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA):
        p = t.GetPosition()
        HOLES.append((mm(p.x), mm(p.y), mm(t.GetDrill()) / 2))
        if t.GetNetCode() != GNDNC:
            VIAS.append((mm(p.x), mm(p.y), mm(t.GetWidth()) / 2))
    elif t.GetNetCode() != GNDNC:
        TRKS.append((mm(t.GetStart().x), mm(t.GetStart().y),
                     mm(t.GetEnd().x), mm(t.GetEnd().y), mm(t.GetWidth()) / 2))


def pt_rect(x, y, r):
    return math.hypot(max(r[0] - x, 0, x - r[2]), max(r[1] - y, 0, y - r[3]))


def pt_seg(x, y, g):
    dx, dy = g[2] - g[0], g[3] - g[1]
    L = dx * dx + dy * dy
    if L < 1e-12:
        return math.hypot(x - g[0], y - g[1])
    t = max(0.0, min(1.0, ((x - g[0]) * dx + (y - g[1]) * dy) / L))
    return math.hypot(x - (g[0] + dx * t), y - (g[1] + dy * t))


def ok(x, y):
    for hx, hy, hr in HOLES:
        if math.hypot(x - hx, y - hy) < hr + VIA_DRILL / 2 + HOLE_GAP:
            return False
    for r in PADS:
        if pt_rect(x, y, r) < VIA_R + CLR:
            return False
    for vx, vy, vr in VIAS:
        if math.hypot(x - vx, y - vy) < VIA_R + vr + CLR:
            return False
    for g in TRKS:
        if pt_seg(x, y, g) < VIA_R + g[4] + CLR:
            return False
    return True


def in_gnd(zs, x, y):
    p = pcbnew.VECTOR2I_MM(round(x, 4), round(y, 4))
    return any(z.HitTestFilledArea(l, p) for z, l in zs)


bb = board.GetBoardEdgesBoundingBox()
x0, y0 = mm(bb.GetLeft()) + 1.0, mm(bb.GetTop()) + 1.0
x1, y1 = mm(bb.GetRight()) - 1.0, mm(bb.GetBottom()) - 1.0

spots = []
y = y0
while y <= y1:
    x = x0
    while x <= x1:
        if in_gnd(zf, x, y) and in_gnd(zb, x, y) and ok(x, y):
            spots.append((x, y))
            HOLES.append((x, y, VIA_DRILL / 2))     # 让后续点避开自己
        x += GRID
    y += GRID

print(f"网格 {GRID}mm，可放 GND 缝合过孔 {len(spots)} 个"
      f"（要求同时落在 F.Cu 和 B.Cu 的地铜实体上）")

if MODE == "apply" and spots:
    net = board.FindNet("GND")
    for x, y in spots:
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I_MM(round(x, 4), round(y, 4)))
        v.SetWidth(pcbnew.FromMM(VIA_D))
        v.SetDrill(pcbnew.FromMM(VIA_DRILL))
        v.SetNet(net)
        board.Add(v)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(PCB)
    print(f"已加 {len(spots)} 个 → {PCB}")
else:
    print("（plan 模式，未写盘）")
