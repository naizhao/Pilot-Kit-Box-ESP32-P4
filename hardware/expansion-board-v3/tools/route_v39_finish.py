#!/usr/bin/env python3
"""收尾三条射频线：SW1_J1 拉回 F.Cu，补 ANT1090_IFA 与 ANT_GNSS_INT。

三条都是射频，freerouting 和 route_fix 都没能给出可用解：

  SW1_J1        已连通，但走了 B.Cu 带 2 个过孔。这条是 1090 接收链的**主信号
                路径**（天线 → U16 开关公共口 → C54 隔直 → LNA1_IN），不是调试线。
                1090MHz 穿层往返，两个过孔各带约 1nH 寄生 + 阻抗不连续，还打穿了
                In1 参考面 —— LNA 前的每一分损耗都直接进噪声系数。拆掉重走 F.Cu。
  ANT1090_IFA   天线 pad1 是一整块自定义焊盘，锚点在主臂上，pad-to-pad 的布线器
                会从主臂中间拉线横穿净空区，所以自动布线器一直交不出解。
  ANT_GNSS_INT  J8.1 ↔ L15.2，短距离但夹在已布满的区域里。

走线一律 45°/正交，不产生任意角度——DSN 导出对非 45° 会崩在 PolylineTrace.combine
的无限递归上，而且射频拐直角本身就有阻抗不连续。

用法：KiCad 自带 python3 tools/route_v39_finish.py [--dry-run]
"""
import itertools
import math
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
PCB = ROOT / "kicad" / "expansion-board-v3.kicad_pcb"
MM = 1_000_000
RF_WIDTH = int(0.15 * MM)          # RF50 netclass
CLEARANCE = int(0.20 * MM)         # 走线到异网铜的最小间隙，比板规再宽一点


def pad_of(board, reference, number):
    # 不用 FindFootprintByReference：它返回的是裸 SwigPyObject，连 Pads() 都没有。
    footprint = next((f for f in board.GetFootprints()
                      if f.GetReference() == reference), None)
    if footprint is None:
        raise SystemExit(f"板上没有 {reference}")
    for pad in footprint.Pads():
        if pad.GetNumber() == number:
            return pad
    raise SystemExit(f"{reference} 没有焊盘 {number}")


def obstacles(board, net_name):
    """异网的铜：焊盘 + 走线 + 过孔。同网的不算障碍。"""
    boxes = []
    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            if pad.GetNetname() != net_name:
                boxes.append(pad.GetBoundingBox())
    for track in board.GetTracks():
        if track.GetNetname() != net_name and track.IsOnLayer(pcbnew.F_Cu):
            boxes.append(track.GetBoundingBox())
    return boxes


def clear(a, b, boxes, width):
    """线段 a→b 是否避开所有障碍。用包围盒近似，宁可保守。"""
    box = pcbnew.BOX2I(pcbnew.VECTOR2I(min(a[0], b[0]), min(a[1], b[1])),
                       pcbnew.VECTOR2I(abs(a[0] - b[0]) or 1, abs(a[1] - b[1]) or 1))
    box.Inflate(width // 2 + CLEARANCE)
    return not any(box.Intersects(other) for other in boxes)


def l_paths(start, end):
    """两点之间的 45°/正交折线候选，按总长排序。

    先给纯 L（横竖），再给带 45° 斜边的 Z 形——后者拐角更缓，射频上更好。
    """
    sx, sy = start
    ex, ey = end
    routes = [
        [start, (ex, sy), end],
        [start, (sx, ey), end],
    ]
    dx, dy = ex - sx, ey - sy
    if dx and dy:
        step = min(abs(dx), abs(dy))
        sgx = 1 if dx > 0 else -1
        sgy = 1 if dy > 0 else -1
        routes.append([start, (sx + sgx * step, sy + sgy * step), end])
        routes.append([start, (ex - sgx * step, ey - sgy * step), end])
    def length(path):
        return sum(math.dist(path[i], path[i + 1]) for i in range(len(path) - 1))
    return sorted(routes, key=length)


def is_45(a, b):
    dx, dy = abs(b[0] - a[0]), abs(b[1] - a[1])
    return dx == 0 or dy == 0 or abs(dx - dy) < 2000    # 2000 nm 容差


def lay(board, net, path, layer=pcbnew.F_Cu, width=RF_WIDTH):
    added = []
    for i in range(len(path) - 1):
        a, b = path[i], path[i + 1]
        if a == b:
            continue
        track = pcbnew.PCB_TRACK(board)
        track.SetStart(pcbnew.VECTOR2I(*a))
        track.SetEnd(pcbnew.VECTOR2I(*b))
        track.SetWidth(width)
        track.SetLayer(layer)
        track.SetNet(net)
        board.Add(track)
        added.append(track)
    return added


def connect(board, net_name, a, b, label, boxes):
    """a / b 是预先取好的 (x, y)，boxes 是预先收集的障碍——见 main() 里的说明。"""
    net = board.FindNet(net_name)
    for path in l_paths(a, b):
        if not all(is_45(path[i], path[i + 1]) for i in range(len(path) - 1)):
            continue
        if all(clear(path[i], path[i + 1], boxes, RF_WIDTH)
               for i in range(len(path) - 1)):
            segments = lay(board, net, path)
            total = sum(math.dist(path[i], path[i + 1])
                        for i in range(len(path) - 1)) / MM
            print(f"  ✅ {label}: {len(segments)} 段 / {total:.2f}mm 全 F.Cu 无过孔")
            return True
    print(f"  ❌ {label}: 直连路径都被占，需要人工绕行")
    return False


def main() -> int:
    dry = "--dry-run" in sys.argv
    board = pcbnew.LoadBoard(str(PCB))

    # ⚠️ 坐标必须在动板之前一次取全。`board.Remove()` 之后，之前那些
    # GetFootprints() 拿到的 Python 对象全部退化成裸 SwigPyObject，
    # 连 GetReference() 都没有——同一个坑在 Save() 后也会出现。
    # 只剩 IFA 这一条。另外两条的归宿：
    #   SW1_J1       保持罩哥手工的 B.Cu 走法。试过挪 C54 贴近 U16.5（11.37→3.59mm），
    #                route_fix 确实布通了 LNA1_IN，但为了布通 SW1_J1 引入 2 处短路、
    #                2 个射频过孔，还把射频甩到 In2.Cu——比原状更差，已整体回退。
    #   ANT_GNSS_INT 已由 route_fix 布通。
    jobs = (
        ("ANT1090_IFA", ("ANT1", "1"), ("ZP1", "1"), "ANT1090_IFA  ANT1.1 → ZP1.1"),
    )
    anchors = {}
    for net_name, first, second, _label in jobs:
        for reference, number in (first, second):
            pad = pad_of(board, reference, number)
            position = pad.GetPosition()
            anchors[(reference, number)] = (position.x, position.y)
    # 障碍物同理，也得在动板之前收齐。这里会把 SW1_J1 那两段 B.Cu 也算进
    # 另外两条线的障碍（拆完其实就不在了），偏保守，不会导致布出违规的线。
    barriers = {net: obstacles(board, net) for net, _f, _s, _l in jobs}

    for net_name, first, second, label in jobs:
        connect(board, net_name, anchors[first], anchors[second], label,
                barriers[net_name])

    if dry:
        print("(--dry-run，不落盘)")
        return 0
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(str(PCB))
    print(f"已落盘 → {PCB.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
