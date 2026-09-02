#!/usr/bin/env python3
"""把 C54 挪到能让 SW1_J1 与 LNA1_IN **两段都走 F.Cu** 的位置。

问题不在布线器，在布局：

    U16.1 → C53.2  (板载 IFA 支路)    3.13 mm
    U16.3 → C30.2  (外接 SMA 支路)    6.19 mm
    U16.5 → C54.1  (公共口 → LNA)    11.37 mm   ← 承载全部 1090 信号的那条最远

C54 是射频开关公共口的隔直电容。它被摆在靠 LNA 那一端，于是 U16.5 出来的信号要
横穿 11mm——而那片走廊被 SW1_J2 / SW1_J3（开关另外两个口）占满，布线器只能穿到
B.Cu 去。**过孔是布局逼出来的**，不是布线器无能。

但简单地把 C54 推到 U16 脚边会把问题对调：C54.2 → U11.2 会从 5mm 变成 12mm，
轮到 LNA1_IN 去穿那片拥挤区。所以这里不预设位置，而是搜索：沿 U16.5 → U11.2
的连线扫一遍候选点，对每个点同时尝试布通两段，取第一个两段全通的。

碰撞检测用**线段与矩形的精确相交**（slab 法），不用包围盒近似——斜线的 bbox
会虚胖到 4 倍，造出根本不存在的障碍，这是以前 route_maze 慢且布不通的真因。

用法：KiCad 自带 python3 tools/eco_v39_fix_sw1j1.py [--dry-run]
"""
import math
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
PCB = ROOT / "kicad" / "expansion-board-v3.kicad_pcb"
MM = 1_000_000
RF_WIDTH = int(0.15 * MM)
CLEARANCE = int(0.18 * MM)
MOVED = "C54"
LINKS = (("SW1_J1", ("U16", "5"), (MOVED, "1")),
         ("LNA1_IN", (MOVED, "2"), ("U11", "2")))


def seg_hits_box(a, b, box, margin):
    """线段 a→b 与矩形是否相交（矩形先外扩 margin）。slab 法，精确。"""
    lo_x, lo_y = box.GetLeft() - margin, box.GetTop() - margin
    hi_x, hi_y = box.GetRight() + margin, box.GetBottom() + margin
    t0, t1 = 0.0, 1.0
    for p, q, lo, hi in ((a[0], b[0] - a[0], lo_x, hi_x),
                         (a[1], b[1] - a[1], lo_y, hi_y)):
        if q == 0:
            if p < lo or p > hi:
                return False
            continue
        r0, r1 = (lo - p) / q, (hi - p) / q
        if r0 > r1:
            r0, r1 = r1, r0
        t0, t1 = max(t0, r0), min(t1, r1)
        if t0 > t1:
            return False
    return True


def snap45(a, b):
    """a→b 的 45°/正交两段折线，两种拐法。"""
    (ax, ay), (bx, by) = a, b
    dx, dy = bx - ax, by - ay
    out = []
    if dx == 0 or dy == 0:
        return [[a, b]]
    step = min(abs(dx), abs(dy))
    sx = 1 if dx > 0 else -1
    sy = 1 if dy > 0 else -1
    out.append([a, (ax + sx * step, ay + sy * step), b])   # 先斜后直
    out.append([a, (bx - sx * step, by - sy * step), b])   # 先直后斜
    return out


def paths(a, b, waypoints):
    """候选路径：先直连/两段折线，再考虑中转点。

    分层很重要：中转点组合是 O(len(waypoints)*4)，全开的话每个 C54 候选位置都要
    算上万条路径，扫一遍要几十分钟。先用便宜的候选筛，绝大多数位置在这一层就
    能判定；只有近处确实绕不过去时才付中转点的代价。
    """
    for path in snap45(a, b):
        yield path
    for w in waypoints:
        for first in snap45(a, w):
            for second in snap45(w, b):
                yield first[:-1] + second


def seg_seg_gap(a, b, c, d):
    """两条线段之间的最短距离。**必须先判相交**——只取四个端点两两距离的最小值，
    会把两条交叉的线判成"还有余量"，据此布出来的就是短路。"""
    def cross(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    d1, d2 = cross(c, d, a), cross(c, d, b)
    d3, d4 = cross(a, b, c), cross(a, b, d)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
        return 0.0
    def point_seg(p, s, e):
        vx, vy = e[0] - s[0], e[1] - s[1]
        if vx == 0 and vy == 0:
            return math.dist(p, s)
        t = max(0.0, min(1.0, ((p[0] - s[0]) * vx + (p[1] - s[1]) * vy) / (vx * vx + vy * vy)))
        return math.dist(p, (s[0] + t * vx, s[1] + t * vy))
    return min(point_seg(a, c, d), point_seg(b, c, d),
               point_seg(c, a, b), point_seg(d, a, b))


def routable(path, boxes, wires=()):
    for i in range(len(path) - 1):
        p, q = path[i], path[i + 1]
        if p == q:
            continue
        dx, dy = abs(q[0] - p[0]), abs(q[1] - p[1])
        if not (dx == 0 or dy == 0 or abs(dx - dy) < 3000):
            return False
        if any(seg_hits_box(p, q, box, RF_WIDTH // 2 + CLEARANCE) for box in boxes):
            return False
        # 走线障碍按**线段**算，不按包围盒。一条 45° 斜线的 bbox 面积是它自身的
        # 好几倍，拿 bbox 当障碍会造出大片根本不存在的禁区——这正是以前
        # route_maze 又慢又布不通的真因，不能在这里重犯。
        for sx, sy, ex, ey, half in wires:
            if seg_seg_gap(p, q, (sx, sy), (ex, ey)) < RF_WIDTH / 2 + half + CLEARANCE:
                return False
    return True


def length(path):
    return sum(math.dist(path[i], path[i + 1]) for i in range(len(path) - 1)) / MM


def main() -> int:
    dry = "--dry-run" in sys.argv
    board = pcbnew.LoadBoard(str(PCB))
    footprints = {f.GetReference(): f for f in board.GetFootprints()}

    def pad_pos(reference, number):
        pad = next(p for p in footprints[reference].Pads()
                   if p.GetNumber() == number)
        q = pad.GetPosition()
        return (q.x, q.y)

    c54 = footprints[MOVED]
    origin = c54.GetPosition()
    offsets = {p.GetNumber(): (p.GetPosition().x - origin.x,
                               p.GetPosition().y - origin.y)
               for p in c54.Pads()}
    anchor_u16 = pad_pos("U16", "5")
    anchor_u11 = pad_pos("U11", "2")

    # 障碍：所有异网 F.Cu 铜 + 别人的 courtyard。C54/SW1_J1/LNA1_IN 自己的不算。
    mine = {"SW1_J1", "LNA1_IN"}
    boxes, courts = [], []
    for footprint in board.GetFootprints():
        if footprint.GetReference() != MOVED:
            courts.append(footprint.GetCourtyard(pcbnew.F_CrtYd).BBox())
        for pad in footprint.Pads():
            if pad.GetNetname() not in mine:
                boxes.append(pad.GetBoundingBox())
    keep, wires = [], []
    for track in board.GetTracks():
        if track.GetNetname() in mine:
            keep.append(track)          # 待拆
        elif track.IsOnLayer(pcbnew.F_Cu) and track.Type() == pcbnew.PCB_TRACE_T:
            s, e = track.GetStart(), track.GetEnd()
            wires.append((s.x, s.y, e.x, e.y, track.GetWidth() / 2))
        elif track.IsOnLayer(pcbnew.F_Cu):
            boxes.append(track.GetBoundingBox())   # 过孔仍按盒算，它本来就是圆的

    # 中转点：U16 与 U11 之间那片区域按 1mm 网格采样，用来绕开障碍
    # 中转点只沿 U16→U11 连线两侧各取几条带，不铺满整片区域：
    # 铺满是 20×15=300 个点、每点 4 种折法，乘上 231 个 C54 候选位置就是千万级。
    lo_x, hi_x = sorted((anchor_u16[0], anchor_u11[0]))
    lo_y, hi_y = sorted((anchor_u16[1], anchor_u11[1]))
    waypoints = []
    for step in range(1, 10):
        t_ = step / 10
        mx = int(anchor_u16[0] + (anchor_u11[0] - anchor_u16[0]) * t_)
        my = int(anchor_u16[1] + (anchor_u11[1] - anchor_u16[1]) * t_)
        for lateral in (-3, -2, -1, 1, 2, 3):
            waypoints.append((mx, my + lateral * MM))
            waypoints.append((mx + lateral * MM, my))

    best = None
    for t in [i / 20 for i in range(2, 19)]:          # 沿 U16.5→U11.2 连线扫
        for lateral in (0, 1, -1, 2, -2, 3, -3):
            cx = int(anchor_u16[0] + (anchor_u11[0] - anchor_u16[0]) * t)
            cy = int(anchor_u16[1] + (anchor_u11[1] - anchor_u16[1]) * t) + lateral * MM // 2
            for rotation in (0, 90, 180, 270):
                c54.SetOrientationDegrees(rotation)
                c54.SetPosition(pcbnew.VECTOR2I(cx, cy))
                court = c54.GetCourtyard(pcbnew.F_CrtYd).BBox()
                if any(court.Intersects(other) for other in courts):
                    continue
                cos = round(math.cos(math.radians(-rotation)))
                sin = round(math.sin(math.radians(-rotation)))
                pads = {}
                for number, (ox, oy) in offsets.items():
                    pads[number] = (cx + ox * cos - oy * sin,
                                    cy + ox * sin + oy * cos)
                found = {}
                for net_name, first, second in LINKS:
                    a = anchor_u16 if first[0] == "U16" else pads[first[1]]
                    b = pads[second[1]] if second[0] == MOVED else anchor_u11
                    hit = next((p for p in paths(a, b, waypoints)
                                if routable(p, boxes, wires)), None)
                    if hit is None:
                        break
                    found[net_name] = hit
                if len(found) == 2:
                    total = sum(length(p) for p in found.values())
                    if best is None or total < best[0]:
                        best = (total, cx, cy, rotation, found)
            if best:
                break
        if best:
            break

    if best is None:
        print("❌ 沿 U16.5→U11.2 扫了一遍，没有能让两段都走 F.Cu 的 C54 位置")
        print("   说明这片区域的通道容量真的到顶了，需要连 SW1_J2/J3 一起重排")
        return 1

    total, cx, cy, rotation, found = best
    print(f"C54 {origin.x/MM:.2f},{origin.y/MM:.2f} → {cx/MM:.2f},{cy/MM:.2f} rot{rotation}")
    print(f"  U16.5 → C54.1  {math.dist(anchor_u16, (cx, cy))/MM:.2f}mm（原 11.37mm）")
    for net_name, path in found.items():
        print(f"  {net_name:<9} {len(path)-1} 段 / {length(path):.2f}mm 全 F.Cu 无过孔")
    print(f"  两段合计 {total:.2f}mm")

    if dry:
        print("(--dry-run，不落盘)")
        return 0
    c54.SetOrientationDegrees(rotation)
    c54.SetPosition(pcbnew.VECTOR2I(cx, cy))
    for track in keep:
        board.Remove(track)
    for net_name, path in found.items():
        net = board.FindNet(net_name)
        for i in range(len(path) - 1):
            if path[i] == path[i + 1]:
                continue
            track = pcbnew.PCB_TRACK(board)
            track.SetStart(pcbnew.VECTOR2I(*path[i]))
            track.SetEnd(pcbnew.VECTOR2I(*path[i + 1]))
            track.SetWidth(RF_WIDTH)
            track.SetLayer(pcbnew.F_Cu)
            track.SetNet(net)
            board.Add(track)
    board.BuildConnectivity()
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(str(PCB))
    print(f"已落盘 → {PCB.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
