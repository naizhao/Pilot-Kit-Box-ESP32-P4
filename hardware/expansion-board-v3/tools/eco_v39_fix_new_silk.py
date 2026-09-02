#!/usr/bin/env python3
"""阶段 C 收尾：只给回灌新增的那 22 个件重排位号丝印。

**作用范围是硬边界。** 罩哥排过的丝印一个都不许动——这块板的丝印是他手工调过的，
自动重排脚本会把本来紧挨元件的位号甩到很远的地方。所以这里维护一份显式白名单，
名单外的位号连读都不改，只当障碍物避让。

要避开四样：母版和其它件的丝印线段、其它位号文本、所有焊盘（silk_over_copper 判的
就是丝印压焊盘）、以及板框外。搜索从元件中心起做同心环，取第一个放得下的点——
位号离本体越近越好认，第一个解就是最近解。

幂等：每次都从当前位置重算，跑多少遍结果一致。

用法：KiCad 自带 python3 tools/eco_v39_fix_new_silk.py
"""
import math
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE = ROOT / "internal" / "work" / "v3.9" / "expansion-board-v3-v39-candidate.kicad_pcb"

MM = 1_000_000
# 阶段 A 移植进来的 14 件 + 4 件换面 + 阶段 C 新摆的 4 件
AGENT_REFS = {
    "R47", "R50", "R51", "R52", "R53", "R54",
    "C81", "C82", "C83", "C84", "C85", "C86", "D4", "D5",
    "C21", "C36", "C48", "R11",
    "C87", "R55", "R56", "R57",
}
SILK_GAP = int(0.15 * MM)
PAD_GAP = int(0.10 * MM)


def inflated(box, amount):
    """BOX2I 没有拷贝构造，只能 (origin, size) 重建。"""
    grown = pcbnew.BOX2I(box.GetOrigin(), box.GetSize())
    grown.Inflate(amount)
    return grown


def collect(board, skip_ref):
    """除 skip_ref 自己的位号外，所有要避开的东西。"""
    silk, pads = [], []
    for footprint in board.GetFootprints():
        reference = footprint.Reference()
        if footprint.GetReference() != skip_ref and reference.IsVisible() \
                and reference.GetLayer() == pcbnew.F_SilkS:
            silk.append(reference.GetBoundingBox())
        for item in footprint.GraphicalItems():
            if item.GetLayer() == pcbnew.F_SilkS:
                silk.append(item.GetBoundingBox())
        for pad in footprint.Pads():
            pads.append(pad.GetBoundingBox())
    for drawing in board.GetDrawings():
        if drawing.GetLayer() == pcbnew.F_SilkS:
            silk.append(drawing.GetBoundingBox())
    return silk, pads


def main() -> int:
    board = pcbnew.LoadBoard(str(CANDIDATE))
    edge = board.GetBoardEdgesBoundingBox()
    moved, stuck = [], []

    for footprint in board.GetFootprints():
        reference_name = footprint.GetReference()
        if reference_name not in AGENT_REFS:
            continue
        field = footprint.Reference()
        if not field.IsVisible():
            continue
        silk, pads = collect(board, reference_name)
        origin = footprint.GetPosition()
        start = field.GetPosition()

        def ok(point):
            field.SetPosition(point)
            box = field.GetBoundingBox()
            if not edge.Contains(box):
                return False
            grown = inflated(box, SILK_GAP)
            if any(grown.Intersects(other) for other in silk):
                return False
            pad_box = inflated(box, PAD_GAP)
            return not any(pad_box.Intersects(other) for other in pads)

        if ok(start):                       # 本来就没冲突，别动它
            continue
        spot = None
        for radius_mm in [x * 0.25 for x in range(2, 25)]:
            for angle in range(0, 360, 15):
                candidate = pcbnew.VECTOR2I(
                    origin.x + int(radius_mm * MM * math.cos(math.radians(angle))),
                    origin.y + int(radius_mm * MM * math.sin(math.radians(angle))),
                )
                if ok(candidate):
                    spot = (candidate, radius_mm)
                    break
            if spot:
                break
        if spot:
            field.SetPosition(spot[0])
            moved.append((reference_name,
                          spot[0].x / MM, spot[0].y / MM, spot[1]))
        else:
            field.SetPosition(start)        # 找不到就还原，绝不留在半路
            stuck.append(reference_name)

    board.Save(str(CANDIDATE))
    print(f"重排位号 {len(moved)} 个:")
    for reference, x, y, distance in moved:
        print(f"  {reference:<4} → ({x:7.3f},{y:7.3f})  距本体 {distance:.2f}mm")
    if stuck:
        print(f"⚠️ 放不下、维持原位的 {len(stuck)} 个: {stuck}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
