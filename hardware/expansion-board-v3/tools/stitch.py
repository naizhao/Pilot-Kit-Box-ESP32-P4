#!/usr/bin/env python3
"""布线后补缝：给仍未连通的 GND / 3V3_DIG 焊盘补过孔到内层平面。

与 route_rf.py 的布线前缝合同一套碰撞校验（带网络归属的占用区 + 引线路径采样
+ 按实际孔径的 hole_to_hole 距离），但搜索半径更大，因为板面已被走线占满。

用法: stitch.py <drc.json>
断言：不得引入新的 DRC 违例（由调用方复检）；至少补上一个。
"""
import json
import math
import os
import sys
import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB = os.path.join(T, "kicad", "expansion-board-v3.kicad_pcb")
PLANE_NETS = {"GND", "3V3_DIG"}
VIA_D, VIA_DRILL = pcbnew.FromMM(0.6), pcbnew.FromMM(0.3)
# 扇出引线/占用区口径照抄 route_rf.py 的双口径修复（原单口径 CLR_V=0.62 在 0.4mm pitch
# QFN 邻脚间一个缝合孔都放不下）。
STUB_W = 0.15               # 扇出引线线宽（0.4mm pitch QFN 必须收窄）
BOARD_CLR = 0.20            # 板子默认网络类间距（保守取 0.20）
CLR_V = 0.30 + BOARD_CLR + 0.02        # 过孔本体占用（过孔半径 0.3 + 间距 + 余量）
CLR_T = STUB_W / 2 + BOARD_CLR + 0.02  # 走线路径占用（半线宽 + 间距 + 余量）
NEW_VIA_R = 0.15            # 过孔钻半径（hole_to_hole 检查用）
HOLE_GAP = 0.27

board = pcbnew.LoadBoard(PCB)

occ, occ_t = [], []
def add_occ(bb, net, pad=0.0, lst=None):
    (occ if lst is None else lst).append(
        (pcbnew.ToMM(bb.GetLeft()) - pad, pcbnew.ToMM(bb.GetTop()) - pad,
         pcbnew.ToMM(bb.GetRight()) + pad, pcbnew.ToMM(bb.GetBottom()) + pad, net))

holes = []
for f in board.GetFootprints():
    for p in f.Pads():
        add_occ(p.GetBoundingBox(), p.GetNetname(), CLR_V)
        add_occ(p.GetBoundingBox(), p.GetNetname(), CLR_T, occ_t)
        dr = pcbnew.ToMM(p.GetDrillSizeX())
        if dr > 0:
            holes.append((pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y), dr / 2))
    for z in f.Zones():
        add_occ(z.GetBoundingBox(), "\0KEEPOUT", CLR_V)
        add_occ(z.GetBoundingBox(), "\0KEEPOUT", CLR_T, occ_t)
for t in board.GetTracks():
    add_occ(t.GetBoundingBox(), t.GetNetname(), CLR_V)
    add_occ(t.GetBoundingBox(), t.GetNetname(), CLR_T, occ_t)
    if isinstance(t, pcbnew.PCB_VIA):
        holes.append((pcbnew.ToMM(t.GetPosition().x), pcbnew.ToMM(t.GetPosition().y),
                      pcbnew.ToMM(t.GetDrill()) / 2))


def free(x, y, net):
    if not (50.8 < x < 149.2 and 50.8 < y < 109.2):
        return False
    for hx, hy, hr in holes:
        if math.hypot(x - hx, y - hy) < hr + NEW_VIA_R + HOLE_GAP:
            return False
    for x0, y0, x1, y1, n in occ:
        if x0 <= x <= x1 and y0 <= y <= y1 and n != net:
            return False
    return True


def path_free(x1, y1, x2, y2, net):
    """引线路径采样：按**走线**间距 CLR_T 检查（不是过孔间距 CLR_V）。
    单口径会让 0.4mm pitch QFN 邻脚占用区大到焊盘自身中心落在邻脚区内，放不下孔。"""
    d = math.hypot(x2 - x1, y2 - y1)
    steps = max(2, int(d / 0.1))
    for i in range(steps + 1):
        x, y = x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps
        for x0, y0, x3, y3, n in occ_t:
            if x0 <= x <= x3 and y0 <= y <= y3 and n != net:
                return False
    return True


drc = json.load(open(sys.argv[1]))
targets = {}
for u in drc.get("unconnected_items", []):
    for it in u.get("items", []):
        d = it.get("description", "")
        if d.startswith("Pad ") and "[" in d:
            net = d.split("[")[1].split("]")[0]
            if net in PLANE_NETS:
                targets[(round(it["pos"]["x"], 3), round(it["pos"]["y"], 3))] = net
print(f"待补缝焊盘 {len(targets)} 个")

added, skipped = 0, 0
for (px, py), netname in sorted(targets.items()):
    net = board.FindNet(netname)
    assert net, f"网络不存在 {netname}"
    spot = None
    for r in [0.8 + 0.2 * i for i in range(12)]:        # 0.8 → 3.0mm
        for k in range(24):
            a = 2 * math.pi * k / 24
            cx, cy = px + r * math.cos(a), py + r * math.sin(a)
            if free(cx, cy, netname) and path_free(px, py, cx, cy, netname):
                spot = (cx, cy); break
        if spot:
            break
    if not spot:
        skipped += 1
        continue
    cx, cy = spot
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I_MM(cx, cy))
    v.SetWidth(VIA_D); v.SetDrill(VIA_DRILL); v.SetNet(net)
    board.Add(v)
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pcbnew.VECTOR2I_MM(px, py)); t.SetEnd(pcbnew.VECTOR2I_MM(cx, cy))
    t.SetWidth(pcbnew.FromMM(STUB_W)); t.SetLayer(pcbnew.F_Cu); t.SetNet(net)
    board.Add(t)
    occ.append((cx - CLR_V, cy - CLR_V, cx + CLR_V, cy + CLR_V, netname))
    occ_t.append((cx - CLR_T, cy - CLR_T, cx + CLR_T, cy + CLR_T, netname))
    holes.append((cx, cy, NEW_VIA_R))
    added += 1

print(f"补缝过孔 {added} 个，{skipped} 个仍无空位")
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
# 分母只算覆铜区：keepout(rule area) 没有网络也不产生填充多边形，IsFilled() 恒为假。
# 同 route_rf.py 那处——V3.9 从 V3.8 母版找回 4 个 RF keepout 之后，
# 用 len(board.Zones()) 当分母会误报"覆铜未全填"。
_pours = [z for z in board.Zones() if not z.GetIsRuleArea()]
assert sum(1 for z in _pours if z.IsFilled()) == len(_pours), "覆铜未全填"
board.Save(PCB)
print("saved:", PCB)
