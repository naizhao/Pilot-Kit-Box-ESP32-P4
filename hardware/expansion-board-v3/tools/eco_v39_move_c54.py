#!/usr/bin/env python3
"""把 C54 挪到 U16.5 旁边，然后拆掉 SW1_J1 / LNA1_IN 让 route_fix 重布。

为什么挪：C54 是射频开关公共口的隔直电容，承载**全部** 1090 信号，却被摆在离
U16.5 有 11.37mm 的地方——同一颗开关另外两个口的隔直只有 3.13mm 和 6.19mm。
公共口那 11mm 的走廊被 SW1_J2/SW1_J3 占满，布线器只好穿到 B.Cu，两个过孔就落在
LNA 之前的信号路径上。过孔是布局逼出来的。

为什么不自己算路径：试过写一个"同时求 C54 位置和两段路径"的搜索，结论是无解。
但拿板上**已经通着**的 LNA1_IN 去测同一套判据，它同样说"判不出"——说明那个
"无解"是判据只会 2~3 段折线造成的，不是真的没有通道。所以这里只做布局改动，
布线交给 route_fix.py（它有真正的迷宫搜索和 rip-up）。

只挪 C54 一个件，不动别人。位置在 U16.5 周围做同心环搜索，避开 courtyard、
异网焊盘、既有丝印。

用法：KiCad 自带 python3 tools/eco_v39_move_c54.py [--dry-run]
"""
import math
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
PCB = ROOT / "kicad" / "expansion-board-v3.kicad_pcb"
MM = 1_000_000
TARGET = "C54"
ANCHOR = ("U16", "5")
RIP_NETS = {"SW1_J1", "LNA1_IN"}
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

    anchor_pad = next(p for p in footprints[ANCHOR[0]].Pads()
                      if p.GetNumber() == ANCHOR[1])
    origin = anchor_pad.GetPosition()
    was = target.GetPosition()
    was_rot = target.GetOrientationDegrees()

    courts, pads, silk = [], [], []
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

    # 目标不是"离 U16.5 越近越好"——那会把 C54 推到 U16 背面，LNA1_IN 反而从
    # 5mm 变成 17mm，问题只是对调。C54 要落在 U16.5 与 U11.2 **之间**，
    # 所以按两段之和 dist(C54,U16.5)+dist(C54,U11.2) 择优，等价于取椭圆内最优点。
    lna_pad = next(p for p in footprints["U11"].Pads() if p.GetNumber() == "2")
    lna = lna_pad.GetPosition()
    edge = board.GetBoardEdgesBoundingBox()
    spot = None
    best_cost = None
    for radius_mm in [x * 0.25 for x in range(6, 33)]:       # 1.5mm 起，到 8mm
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
                if any(grown.Intersects(o) for o in courts):
                    continue
                if any(grown.Intersects(o) for o in pads):
                    continue
                boxes = [i.GetBoundingBox() for i in target.GraphicalItems()
                         if i.GetLayer() == pcbnew.F_SilkS]
                field = target.Reference()
                if field.IsVisible():
                    boxes.append(field.GetBoundingBox())
                if any(inflated(bx, SILK_GAP).Intersects(o)
                       for bx in boxes for o in silk):
                    continue
                cost = (math.dist((x, y), (origin.x, origin.y))
                        + math.dist((x, y), (lna.x, lna.y)))
                if best_cost is None or cost < best_cost:
                    best_cost = cost
                    spot = (x, y, rotation, radius_mm)
        # 半径由小到大，一旦这一圈找到了解，再往外扩只会更长——但同一圈内要
        # 把所有角度都比完，否则会挑到"离 U16 近、离 LNA 远"的反方向点。
        if spot and radius_mm >= 4.0:
            break

    if spot is None:
        target.SetPosition(was)
        target.SetOrientationDegrees(was_rot)
        print("❌ U16.5 周围 8mm 内放不下 C54")
        return 1

    x, y, rotation, radius = spot
    target.SetOrientationDegrees(rotation)
    target.SetPosition(pcbnew.VECTOR2I(x, y))
    print(f"C54 ({was.x/MM:.2f},{was.y/MM:.2f}) → ({x/MM:.2f},{y/MM:.2f}) rot{rotation}")
    print(f"  距 U16.5 {radius:.2f}mm（原 11.37mm，对比 C53 的 3.13mm）")

    ripped = 0
    for track in list(board.GetTracks()):
        if track.GetNetname() in RIP_NETS:
            board.Remove(track)
            ripped += 1
    print(f"  拆掉 SW1_J1 / LNA1_IN 的旧铜 {ripped} 个对象，交给 route_fix 重布")

    if dry:
        print("(--dry-run，不落盘)")
        return 0
    board.BuildConnectivity()
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(str(PCB))
    print(f"已落盘 → {PCB.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
