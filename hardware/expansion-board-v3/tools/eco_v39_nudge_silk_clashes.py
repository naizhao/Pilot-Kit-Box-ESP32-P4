#!/usr/bin/env python3
"""阶段 C 收尾：微调撞上母版丝印的新件本体位置。

上一步（eco_v39_fix_new_silk.py）把新件自己的位号挪开了，从 25 处降到 8 处。
剩下的 8 处性质不同：**冲突的另一方是母版件的位号**——C54/C10/R10/U8 的参考字段
压在了 C48/R53/R51/C83 的焊盘或封装丝印上。

那几个位号是罩哥手工排过的，不许动。所以只能挪新件本体。这些件的坐标是从工作区
中间态移植来的，而中间态早就把母版丝印弄丢了，自然不会跟它们打架——冲突是移植
到母版上之后才出现的。

挪动是安全的：这些区域的铜在阶段 A 已经拆干净，还没有布线可破坏。
每个件都带一个锚点约束（去耦得贴着芯片、分压得贴着搭档），不会为了避丝印把件甩远。

用法：KiCad 自带 python3 tools/eco_v39_nudge_silk_clashes.py
"""
import math
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE = ROOT / "internal" / "work" / "v3.9" / "expansion-board-v3-v39-candidate.kicad_pcb"

MM = 1_000_000
# (要挪的件, 锚点件, 锚点焊盘, 离锚点的上限mm)
NUDGE = (
    ("C48", "U11", None, 12.0),   # 3V3_RF 去耦，跟着 LNA1
    ("R53", "U4", "16", 12.0),    # BNO_ENV_SDA 上拉；I2C 上拉对位置不敏感，可以放宽
    ("C83", "U8", "50", 6.0),     # RP_1V1 本地去耦，**必须**贴 RP2040，宁可留丝印冲突
    ("R51", "R50", None, 10.0),   # VBUS 分压下臂；分压比与走线长度无关，可以放宽
)
CLEARANCE = int(0.25 * MM)
SILK_GAP = int(0.20 * MM)


def inflated(box, amount):
    grown = pcbnew.BOX2I(box.GetOrigin(), box.GetSize())
    grown.Inflate(amount)
    return grown


def obstacles(board, skip):
    cu, silk, courts = [], [], []
    for footprint in board.GetFootprints():
        if footprint.GetReference() == skip:
            continue
        courts.append(footprint.GetCourtyard(pcbnew.F_CrtYd).BBox())
        for pad in footprint.Pads():
            cu.append(pad.GetBoundingBox())
        for item in footprint.GraphicalItems():
            if item.GetLayer() == pcbnew.F_SilkS:
                silk.append(item.GetBoundingBox())
        field = footprint.Reference()
        if field.IsVisible() and field.GetLayer() == pcbnew.F_SilkS:
            silk.append(field.GetBoundingBox())
    for track in board.GetTracks():
        if track.IsOnLayer(pcbnew.F_Cu):
            cu.append(track.GetBoundingBox())
    for drawing in board.GetDrawings():
        if drawing.GetLayer() == pcbnew.F_SilkS:
            silk.append(drawing.GetBoundingBox())
    return cu, silk, courts


def main() -> int:
    board = pcbnew.LoadBoard(str(CANDIDATE))
    refs = {fp.GetReference(): fp for fp in board.GetFootprints()}
    edge = board.GetBoardEdgesBoundingBox()
    moved, stuck = [], []

    for reference, anchor_ref, anchor_pad, limit in NUDGE:
        footprint = refs.get(reference)
        anchor = refs.get(anchor_ref)
        if footprint is None or anchor is None:
            continue
        if anchor_pad:
            pad = {p.GetNumber(): p for p in anchor.Pads()}.get(anchor_pad)
            origin = pad.GetPosition() if pad else anchor.GetPosition()
        else:
            origin = anchor.GetPosition()
        was = footprint.GetPosition()
        cu, silk, courts = obstacles(board, reference)

        def clean(point, rotation):
            footprint.SetOrientationDegrees(rotation)
            footprint.SetPosition(point)
            court = footprint.GetCourtyard(pcbnew.F_CrtYd).BBox()
            if not edge.Contains(court):
                return False
            grown = inflated(court, CLEARANCE)
            if any(grown.Intersects(o) for o in cu):
                return False
            if any(grown.Intersects(o) for o in courts):
                return False
            # 本体丝印 + 自己的位号都不能压到别人
            boxes = [i.GetBoundingBox() for i in footprint.GraphicalItems()
                     if i.GetLayer() == pcbnew.F_SilkS]
            field = footprint.Reference()
            if field.IsVisible():
                boxes.append(field.GetBoundingBox())
            for box in boxes:
                mine = inflated(box, SILK_GAP)
                if any(mine.Intersects(o) for o in silk):
                    return False
                if any(inflated(box, 0).Intersects(o) for o in cu):
                    return False
            return True

        spot = None
        for radius_mm in [x * 0.25 for x in range(2, int(limit / 0.25) + 1)]:
            for angle in range(0, 360, 10):
                point = pcbnew.VECTOR2I(
                    origin.x + int(radius_mm * MM * math.cos(math.radians(angle))),
                    origin.y + int(radius_mm * MM * math.sin(math.radians(angle))),
                )
                for rotation in (0, 90, 180, 270):
                    if clean(point, rotation):
                        spot = (point, rotation, radius_mm)
                        break
                if spot:
                    break
            if spot:
                break
        if spot:
            footprint.SetOrientationDegrees(spot[1])
            footprint.SetPosition(spot[0])
            moved.append((reference, spot[0].x / MM, spot[0].y / MM,
                          spot[1], spot[2], anchor_ref))
        else:
            footprint.SetPosition(was)
            stuck.append(reference)
        refs = {fp.GetReference(): fp for fp in board.GetFootprints()}

    board.Save(str(CANDIDATE))
    for reference, x, y, rotation, distance, anchor in moved:
        print(f"  {reference:<4} → ({x:7.3f},{y:7.3f}) rot{rotation:<4} 距 {anchor} {distance:.2f}mm")
    if stuck:
        print(f"⚠️ 挪不动、维持原位: {stuck}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
