#!/usr/bin/env python3
"""重排已知拥挤位号，并移除 USB-C 板外丝印线。"""

import os

import pcbnew

from silk_geometry import first_clear_box


T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
TARGETS = ("U17", "C58", "L15", "C57", "F5", "J8", "Q5", "R27", "R26", "R34")
BOUNDS = (50.3, 50.3, 149.7, 111.7)
CLEARANCE = 0.18


board = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM
footprints = {footprint.GetReference(): footprint for footprint in board.GetFootprints()}
missing = sorted(set(TARGETS) - set(footprints))
assert not missing, f"缺少待排位号: {missing}"


def box_mm(item):
    box = item.GetBoundingBox()
    return tuple(mm(value) for value in
                 (box.GetLeft(), box.GetTop(), box.GetRight(), box.GetBottom()))


# J4 固定在左板边，官方封装有三条丝印伸到板外；保留内侧两条装配提示，
# 板外的三条移到 F.Fab，避免每次重建都产生 silk_edge_clearance。
j4 = footprints["J4"]
demoted = 0
for graphic in j4.GraphicalItems():
    if graphic.GetLayer() == pcbnew.F_SilkS and box_mm(graphic)[2] < 52.0:
        graphic.SetLayer(pcbnew.F_Fab)
        demoted += 1
assert demoted == 3, f"J4 板外丝印线数量变化: {demoted} != 3"

# 固化后的人工位号坐标来自 PLACEMENT.SILK_REF，重建时不得再用候选算法覆盖。
# J4 的封装内板外线仍需降到 F.Fab，因为它不属于可由快照保存的板级图形。
if os.environ.get("PK_PRESERVE_SILK_REF") == "1":
    board.Save(PCB)
    print(f"J4 板外丝印 {demoted} 条移到 F.Fab；保留全部冻结位号坐标")
    raise SystemExit(0)

for reference in TARGETS:
    footprints[reference].Reference().SetVisible(False)

obstacles = []
for footprint in board.GetFootprints():
    for pad in footprint.Pads():
        obstacles.append(box_mm(pad))
    for graphic in footprint.GraphicalItems():
        if graphic.GetLayer() == pcbnew.F_SilkS:
            obstacles.append(box_mm(graphic))
    field = footprint.Reference()
    if field.IsVisible() and field.GetLayer() == pcbnew.F_SilkS:
        obstacles.append(box_mm(field))
for drawing in board.GetDrawings():
    if drawing.GetLayer() == pcbnew.F_SilkS:
        obstacles.append(box_mm(drawing))

placements = {}
distances = (0.35, 0.6, 0.9, 1.2, 1.6, 2.1, 2.8, 3.6, 4.5, 5.5, 6.5)
for reference in TARGETS:
    footprint = footprints[reference]
    field = footprint.Reference()
    body = footprint.GetBoundingBox(False, False)
    left, top, right, bottom = (mm(body.GetLeft()), mm(body.GetTop()),
                                mm(body.GetRight()), mm(body.GetBottom()))
    center_x, center_y = (left + right) / 2, (top + bottom) / 2
    candidates = []
    for distance in distances:
        for angle in (0, 90):
            field.SetTextAngleDegrees(angle)
            width, height = (mm(field.GetTextWidth()), mm(field.GetTextHeight()))
            if angle == 90:
                width, height = height, width
            centers = (
                (center_x, top - distance - height / 2),
                (center_x, bottom + distance + height / 2),
                (left - distance - width / 2, center_y),
                (right + distance + width / 2, center_y),
            )
            for x, y in centers:
                field.SetPosition(pcbnew.VECTOR2I_MM(round(x, 3), round(y, 3)))
                candidates.append(((round(x, 3), round(y, 3), angle), box_mm(field)))
    chosen = first_clear_box(candidates, obstacles, BOUNDS, CLEARANCE)
    assert chosen, f"{reference} 在 6.5mm 范围内找不到合规丝印位置"
    x, y, angle = chosen
    field.SetPosition(pcbnew.VECTOR2I_MM(x, y))
    field.SetTextAngleDegrees(angle)
    field.SetVisible(True)
    final_box = box_mm(field)
    obstacles.append(final_box)
    placements[reference] = chosen

board.Save(PCB)
print(f"J4 板外丝印 {demoted} 条移到 F.Fab；重排位号 {len(placements)} 个")
for reference, (x, y, angle) in placements.items():
    print(f"  {reference:4s} → ({x:.3f}, {y:.3f}) {angle}°")
