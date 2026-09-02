#!/usr/bin/env python3
"""把 R53 挪回 U4.16 的 4mm 之内。

起因是我自己引入的回归：2026-09-03 处理丝印冲突时，`eco_v39_nudge_silk_clashes.py`
里 R53 的锚点半径被从 8mm 放宽到 12mm（当时的理由是"I2C 上拉对位置不敏感"），
结果它落到了离 U4.16 有 9.78mm 的地方，而合同
`test_bno085_environment_bus_has_required_pullups` 要求 <4.0mm。

"I2C 上拉对位置不敏感"这句话本身没错，但**它不能用来推翻一条已有的合同约束**。
上拉离总线太远会在走线上引入额外的分布电容和残桩，BNO085 的 ENV 总线本来就跑
400kHz；更重要的是，为了让丝印好看去动电气布局，方向就反了——丝印可以挪、
可以缩、必要时可以隐藏，电气约束不行。

这里只挪 R53，锚点 U4.16，半径上限 4.0mm，并且照旧避开 courtyard / 焊盘 / 丝印。
R52 已经在 2.95mm，不动。

用法：KiCad 自带 python3 tools/eco_v39_restore_r53.py [--dry-run]
"""
import math
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
PCB = ROOT / "kicad" / "expansion-board-v3.kicad_pcb"
MM = 1_000_000
TARGET = "R53"
ANCHOR = ("U4", "16")
LIMIT_MM = 4.0          # 合同 test_bno085_environment_bus_has_required_pullups
CLEARANCE = int(0.25 * MM)
SILK_GAP = int(0.15 * MM)


def inflated(box, amount):
    grown = pcbnew.BOX2I(box.GetOrigin(), box.GetSize())
    grown.Inflate(amount)
    return grown


def main() -> int:
    dry = "--dry-run" in sys.argv
    board = pcbnew.LoadBoard(str(PCB))
    footprints = {f.GetReference(): f for f in board.GetFootprints()}
    target = footprints[TARGET]
    anchor = next(p for p in footprints[ANCHOR[0]].Pads()
                  if p.GetNumber() == ANCHOR[1])
    origin = anchor.GetPosition()
    was, was_rot = target.GetPosition(), target.GetOrientationDegrees()

    courts, pads, silk, wires = [], [], [], []
    for footprint in board.GetFootprints():
        if footprint.GetReference() == TARGET:
            continue
        courts.append(footprint.GetCourtyard(pcbnew.F_CrtYd).BBox())
        for pad in footprint.Pads():
            pads.append(pad.GetBoundingBox())
        field = footprint.Reference()
        if field.IsVisible() and field.GetLayer() == pcbnew.F_SilkS:
            silk.append(field.GetBoundingBox())
        for item in footprint.GraphicalItems():
            if item.GetLayer() == pcbnew.F_SilkS:
                silk.append(item.GetBoundingBox())
    for drawing in board.GetDrawings():
        if drawing.GetLayer() == pcbnew.F_SilkS:
            silk.append(drawing.GetBoundingBox())
    # 走线也要避开：这一带已经布好线了，挪进去压线就得重布
    mine = {p.GetNetname() for p in target.Pads()}
    for track in board.GetTracks():
        if track.IsOnLayer(pcbnew.F_Cu) and track.GetNetname() not in mine:
            wires.append(track.GetBoundingBox())

    edge = board.GetBoardEdgesBoundingBox()
    best = None
    for radius_mm in [x * 0.25 for x in range(4, int(LIMIT_MM / 0.25) + 1)]:
        for angle in range(0, 360, 10):
            x = origin.x + int(radius_mm * MM * math.cos(math.radians(angle)))
            y = origin.y + int(radius_mm * MM * math.sin(math.radians(angle)))
            for rotation in (0, 90, 180, 270):
                target.SetOrientationDegrees(rotation)
                target.SetPosition(pcbnew.VECTOR2I(x, y))
                court = target.GetCourtyard(pcbnew.F_CrtYd).BBox()
                if not edge.Contains(court):
                    continue
                grown = inflated(court, CLEARANCE)
                if any(grown.Intersects(o) for o in courts + pads + wires):
                    continue
                boxes = [i.GetBoundingBox() for i in target.GraphicalItems()
                         if i.GetLayer() == pcbnew.F_SilkS]
                field = target.Reference()
                if field.IsVisible():
                    boxes.append(field.GetBoundingBox())
                if any(inflated(bx, SILK_GAP).Intersects(o)
                       for bx in boxes for o in silk):
                    continue
                # 判据是**焊盘到焊盘**，不是元件中心到焊盘
                pad2 = next(p for p in target.Pads() if p.GetNumber() == "2")
                gap = math.dist((pad2.GetPosition().x, pad2.GetPosition().y),
                                (origin.x, origin.y)) / MM
                if gap >= LIMIT_MM:
                    continue
                if best is None or gap < best[0]:
                    best = (gap, x, y, rotation)
        if best:
            break

    if best is None:
        target.SetPosition(was)
        target.SetOrientationDegrees(was_rot)
        print(f"❌ U4.16 周围 {LIMIT_MM}mm 内放不下 R53（要连丝印一起腾）")
        return 1

    gap, x, y, rotation = best
    target.SetOrientationDegrees(rotation)
    target.SetPosition(pcbnew.VECTOR2I(x, y))
    print(f"R53 ({was.x/MM:.2f},{was.y/MM:.2f}) → ({x/MM:.2f},{y/MM:.2f}) rot{rotation}")
    print(f"  R53.2 → U4.16  {gap:.2f}mm（原 9.78mm，合同上限 {LIMIT_MM}mm）")
    if dry:
        print("(--dry-run，不落盘)")
        return 0
    board.Save(str(PCB))
    print(f"已落盘 → {PCB.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
