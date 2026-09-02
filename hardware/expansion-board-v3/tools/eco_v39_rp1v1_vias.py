#!/usr/bin/env python3
"""给 RP2040 内核电源的去耦补 RP_1V1 接入过孔。

背景：`RP_1V1` 的覆铜只在 B.Cu，而 C83、U8.23 这些焊盘在 F.Cu，必须靠过孔穿层
才能接到那片铜。现在 C83.1 最近的同网过孔有 1.55mm（合同阈值 1.5mm），
U8.23 干脆一个都没有——内核去耦等于没接到它该接的平面。

注意这不是"没接通"：DRC 未连接是 0，电流可以经 F.Cu 上那 25 段 RP_1V1 走线绕过去。
问题是**回流路径长**——去耦电容的作用全靠低电感回路，绕一圈走线就失去意义了。

打孔位置的要求：
  · 距目标焊盘 < 1.5mm（合同阈值，来自"焊盘到过孔那段走线是主导项"的论证）
  · 落在 B.Cu 的 RP_1V1 填充铜内，否则打了也接不上
  · 避开异网铜、焊盘、既有过孔

用法：KiCad 自带 python3 tools/eco_v39_rp1v1_vias.py [--dry-run]
"""
import math
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
PCB = ROOT / "kicad" / "expansion-board-v3.kicad_pcb"
MM = 1_000_000
NET = "RP_1V1"
VIA_D = int(0.45 * MM)          # 与全板统一：0.45 盘 / 0.3 孔（嘉立创免费档）
DRILL = int(0.30 * MM)
MAX_MM = 1.40                   # 留 0.1mm 余量给合同的 1.5mm 阈值
CLEARANCE = int(0.25 * MM)

TARGETS = (("C83", "1"), ("U8", "23"), ("U8", "45"))


def polygons(board):
    for zone in board.Zones():
        if zone.GetNetname() != NET or zone.GetIsRuleArea():
            continue
        filled = zone.GetFilledPolysList(pcbnew.B_Cu)
        for i in range(filled.OutlineCount()):
            outline = filled.Outline(i)
            yield [(outline.CPoint(k).x, outline.CPoint(k).y)
                   for k in range(outline.PointCount())]


def inside(point, poly):
    x, y = point
    hit = False
    for i in range(len(poly)):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % len(poly)]
        if ((y1 > y) != (y2 > y)) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            hit = not hit
    return hit


def main() -> int:
    dry = "--dry-run" in sys.argv
    board = pcbnew.LoadBoard(str(PCB))
    footprints = {f.GetReference(): f for f in board.GetFootprints()}
    islands = list(polygons(board))
    if not islands:
        raise SystemExit("板上没有 RP_1V1 覆铜，无法补接入过孔")

    # 障碍：异网的焊盘/走线/过孔 + 同网既有过孔（别叠在一起）
    blockers = []
    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            if pad.GetNetname() != NET:
                blockers.append(pad.GetBoundingBox())
    for track in board.GetTracks():
        if track.Type() == pcbnew.PCB_VIA_T:
            blockers.append(track.GetBoundingBox())
        elif track.GetNetname() != NET:
            blockers.append(track.GetBoundingBox())

    net = board.FindNet(NET)
    placed = []
    for reference, number in TARGETS:
        footprint = footprints.get(reference)
        if footprint is None:
            continue
        pad = next((p for p in footprint.Pads() if p.GetNumber() == number), None)
        if pad is None:
            continue
        origin = pad.GetPosition()
        spot = None
        for radius_mm in [x * 0.1 for x in range(6, int(MAX_MM * 10) + 1)]:
            for angle in range(0, 360, 5):
                x = origin.x + int(radius_mm * MM * math.cos(math.radians(angle)))
                y = origin.y + int(radius_mm * MM * math.sin(math.radians(angle)))
                if not any(inside((x, y), poly) for poly in islands):
                    continue
                box = pcbnew.BOX2I(pcbnew.VECTOR2I(x - VIA_D // 2, y - VIA_D // 2),
                                   pcbnew.VECTOR2I(VIA_D, VIA_D))
                box.Inflate(CLEARANCE)
                if any(box.Intersects(other) for other in blockers):
                    continue
                spot = (x, y, radius_mm)
                break
            if spot:
                break
        if spot is None:
            print(f"  ❌ {reference}.{number} 在 {MAX_MM}mm 内找不到能落进 RP_1V1 铜面的空位")
            continue
        x, y, distance = spot
        via = pcbnew.PCB_VIA(board)
        via.SetPosition(pcbnew.VECTOR2I(x, y))
        via.SetWidth(VIA_D)
        via.SetDrill(DRILL)
        via.SetNetCode(net.GetNetCode())
        board.Add(via)
        blockers.append(via.GetBoundingBox())
        placed.append((reference, number, x / MM, y / MM, distance))
        print(f"  ✅ {reference}.{number} 旁 {distance:.2f}mm 打 RP_1V1 过孔 "
              f"({x/MM:.3f},{y/MM:.3f})")

    if dry:
        print("(--dry-run，不落盘)")
        return 0
    board.BuildConnectivity()
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(str(PCB))
    print(f"新增 {len(placed)} 个过孔，已落盘 → {PCB.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
