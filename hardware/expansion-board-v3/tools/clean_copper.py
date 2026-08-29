#!/usr/bin/env python3
"""清掉悬空的断头走线——两端都没搭上任何东西的铜，是布线器撕线留下的垃圾。

为什么需要：KiCad 的 unconnected_items 里混着三类东西，其中
`Track↔Track` / `Track↔Via` 那一类**不是缺连接，是孤立铜箔**——把它们当"飞线"
去布是白费力气。实测板上有 0.014mm 长的走线碎屑（KRT 撕线后没收干净）。

判据：一段走线的某个端点，如果没有碰到同网络的
  · 另一段走线的端点
  · 过孔
  · 焊盘
就是悬空端。**只删两端都悬空的**——单端悬空的可能是有意的短桩（比如天线馈点），
删了会破坏设计。删完再扫一遍，因为删掉一段会让相邻段变成两端悬空。

运行：clean_copper.py [--dry-run]
"""
import collections
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB = os.path.join(T, "kicad", "expansion-board-v3.kicad_pcb")
DRY = "--dry-run" in sys.argv

TOL = pcbnew.FromMM(0.001)      # 端点重合判据。走线坐标是整数纳米，本该精确相等，
                                # 留 1µm 容差吸收布线器的取整。

board = pcbnew.LoadBoard(PCB)

# 焊盘几何必须在任何 Remove 之前取完：board.Remove() 之后 SWIG 代理会失效，
# 同一进程里再取 GetFootprints() 拿到的是裸 SwigPyObject。
pads = []
for fp in board.GetFootprints():
    for pd in fp.Pads():
        pads.append((pd.GetNetCode(), pd.GetLayerSet(), pd.GetBoundingBox(), pd))

vias = [(v.GetNetCode(), v.GetPosition().x, v.GetPosition().y)
        for v in board.GetTracks() if isinstance(v, pcbnew.PCB_VIA)]
via_at = collections.defaultdict(set)
for nc, x, y in vias:
    via_at[(nc, x, y)].add(True)


def on_pad(nc, layer, x, y):
    p = pcbnew.VECTOR2I(x, y)
    for pnc, lset, bb, pd in pads:
        if pnc != nc or not lset.Contains(layer):
            continue
        if bb.Contains(p) and pd.HitTest(p, 0):
            return True
    return False


removed_total = 0
for round_no in range(1, 21):
    segs = [t for t in board.GetTracks() if not isinstance(t, pcbnew.PCB_VIA)]
    # 端点索引：同网络、同层的端点聚在一起
    ends = collections.defaultdict(list)
    for i, t in enumerate(segs):
        for p in (t.GetStart(), t.GetEnd()):
            ends[(t.GetNetCode(), t.GetLayer(), p.x, p.y)].append(i)

    doomed = []
    for i, t in enumerate(segs):
        nc, ly = t.GetNetCode(), t.GetLayer()
        free = 0
        for p in (t.GetStart(), t.GetEnd()):
            k = (nc, ly, p.x, p.y)
            touch = len([j for j in ends[k] if j != i]) > 0
            if not touch:
                touch = (nc, p.x, p.y) in via_at
            if not touch:
                touch = on_pad(nc, ly, p.x, p.y)
            if not touch:
                free += 1
        if free == 2:                      # 两端都悬空 → 纯垃圾
            doomed.append(t)

    if not doomed:
        break
    names = collections.Counter(t.GetNetname() for t in doomed)
    total_len = sum(pcbnew.ToMM(t.GetLength()) for t in doomed)
    print(f"第 {round_no} 轮: {len(doomed)} 段悬空铜 / {total_len:.3f}mm  {dict(names)}")
    removed_total += len(doomed)
    if DRY:
        break
    for t in doomed:
        board.Remove(t)

print(f"合计清除 {removed_total} 段" + ("（dry-run，没写盘）" if DRY else ""))
if not DRY and removed_total:
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(PCB)
    print("saved:", PCB)
