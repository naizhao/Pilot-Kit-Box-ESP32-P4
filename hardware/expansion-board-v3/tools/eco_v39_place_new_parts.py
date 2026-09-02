#!/usr/bin/env python3
"""阶段 C：把回灌新增的四个器件摆到候选板上。

    C87  10nF C0G  USB_VBUS_SENSE 对地，贴 R50/R51 的分压中点
    R55  10k       3V3_DIG → IMU_RST，贴 U4.11
    R56  10k       3V3_DIG → SUBG_CSN，贴 U10.17
    R57  33R       PULSES_RAW → PULSES，**必须贴 U15.5**（源端阻尼，<6mm）

判空要同时算五样：courtyard、走线、过孔、别人的焊盘、F.Silkscreen。
V4 那轮在这里栽过两次——先漏了铜、补上之后又漏了丝印，各返工一次。
丝印看起来无关紧要，但位号被压掉之后装配时根本认不出哪颗是哪颗。

搜索方式是从锚点起做同心环扫描，取第一个放得下的点。不做全局最优：
这四颗都是"离锚点越近越好"的单目标，环扫的第一个解就是最近解。

幂等：先按位号删掉已存在的同名件再摆。

用法：KiCad 自带 python3 tools/eco_v39_place_new_parts.py
"""
import math
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE = ROOT / "internal" / "work" / "v3.9" / "expansion-board-v3-v39-candidate.kicad_pcb"

MM = 1_000_000

# (位号, 锚点位号, 锚点焊盘, 距离上限mm, 取自哪个已有件的封装)
NEW_PARTS = (
    ("C87", "R51", "2", 8.0, "C21"),   # 0603 电容，借 C21 的封装几何
    ("R55", "U4", "11", 8.0, "R17"),   # 0603 电阻
    ("R56", "U10", "17", 8.0, "R17"),
    ("R57", "U15", "5", 6.0, "R17"),   # 硬上限：清单 C5 判据要求 <6mm
)

CLEARANCE = int(0.25 * MM)      # 与既有铜/焊盘的间隙，比最严 netclass 再宽一点
SILK_GAP = int(0.20 * MM)       # 与既有丝印的间隙


def obstacles(board, exclude):
    """收集所有要避开的东西，按层分。"""
    cu, silk, courts = [], [], []
    for footprint in board.GetFootprints():
        if footprint.GetReference() in exclude:
            continue
        courts.append(footprint.GetCourtyard(pcbnew.F_CrtYd).BBox())
        for pad in footprint.Pads():
            cu.append(pad.GetBoundingBox())
        for item in footprint.GraphicalItems():
            if item.GetLayer() in (pcbnew.F_SilkS,):
                silk.append(item.GetBoundingBox())
        ref = footprint.Reference()
        if ref.IsVisible() and ref.GetLayer() == pcbnew.F_SilkS:
            silk.append(ref.GetBoundingBox())
    for track in board.GetTracks():
        if track.IsOnLayer(pcbnew.F_Cu):
            cu.append(track.GetBoundingBox())
    for drawing in board.GetDrawings():
        if drawing.GetLayer() == pcbnew.F_SilkS:
            silk.append(drawing.GetBoundingBox())
    return cu, silk, courts


def inflated(box, amount):
    """放大一个包围盒。BOX2I 没有拷贝构造，只能用 (origin, size) 重建——
    直接 pcbnew.BOX2I(box) 会报 'Wrong number or type of arguments'。"""
    grown = pcbnew.BOX2I(box.GetOrigin(), box.GetSize())
    grown.Inflate(amount)
    return grown


def fits(box, cu, silk, courts):
    grown = inflated(box, CLEARANCE)
    for other in cu:
        if grown.Intersects(other):
            return False
    for other in courts:
        if grown.Intersects(other):
            return False
    silk_box = inflated(box, SILK_GAP)
    for other in silk:
        if silk_box.Intersects(other):
            return False
    return True


def main() -> int:
    board = pcbnew.LoadBoard(str(CANDIDATE))
    refs = {fp.GetReference(): fp for fp in board.GetFootprints()}
    edge = board.GetBoardEdgesBoundingBox()

    placed = []
    for reference, anchor_ref, anchor_pad, limit, donor_ref in NEW_PARTS:
        if reference in refs:                       # 幂等
            board.Remove(refs[reference])
            refs = {fp.GetReference(): fp for fp in board.GetFootprints()}
        anchor = refs[anchor_ref]
        pad = {p.GetNumber(): p for p in anchor.Pads()}.get(anchor_pad)
        origin = pad.GetPosition() if pad else anchor.GetPosition()

        donor = refs[donor_ref]
        try:
            clone = donor.Duplicate(False)
        except TypeError:
            clone = donor.Duplicate()
        clone = pcbnew.Cast_to_FOOTPRINT(clone)
        clone.SetParent(board)
        board.Add(clone)
        clone.SetReference(reference)
        clone.SetValue("")
        for p in clone.Pads():                      # 网络留给 bind 步骤统一绑
            p.SetNet(board.FindNet("")) if board.FindNet("") else None

        cu, silk, courts = obstacles(board, exclude={reference})
        spot = None
        for radius_mm in [x * 0.25 for x in range(2, int(limit / 0.25) + 1)]:
            for angle in range(0, 360, 10):
                x = origin.x + int(radius_mm * MM * math.cos(math.radians(angle)))
                y = origin.y + int(radius_mm * MM * math.sin(math.radians(angle)))
                for rotation in (0, 90):
                    clone.SetOrientationDegrees(rotation)
                    clone.SetPosition(pcbnew.VECTOR2I(x, y))
                    box = clone.GetCourtyard(pcbnew.F_CrtYd).BBox()
                    if not edge.Contains(box):
                        continue
                    if fits(box, cu, silk, courts):
                        spot = (x, y, rotation, radius_mm)
                        break
                if spot:
                    break
            if spot:
                break
        if not spot:
            raise SystemExit(f"{reference} 在 {limit}mm 内找不到空位——需要人工腾挪")
        clone.SetOrientationDegrees(spot[2])
        clone.SetPosition(pcbnew.VECTOR2I(spot[0], spot[1]))
        placed.append((reference, spot[0] / MM, spot[1] / MM, spot[2], spot[3],
                       f"{anchor_ref}.{anchor_pad}"))
        refs = {fp.GetReference(): fp for fp in board.GetFootprints()}

    board.Save(str(CANDIDATE))
    for reference, x, y, rot, dist, anchor in placed:
        print(f"  {reference:<4} ({x:7.3f},{y:7.3f}) rot{rot:<3} 距 {anchor:<7} {dist:.2f}mm")
    return 0


if __name__ == "__main__":
    sys.exit(main())
