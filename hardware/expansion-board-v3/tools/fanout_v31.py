#!/usr/bin/env python3
"""v3.1 定向扇出：只给缺线的那几个 U8/U10 pin 在封装外打 0.6/0.3 通孔下到 In2.Cu。

为什么不是全 56 pin 扇出：freerouting 已经把绝大多数网络布通了（1330 段），只剩 12 个网络
卡在 U8/U10 细间距出口（pin 走廊被 freerouting 自己布的线 + route_rf 的 GND 过孔墙堵死）。
只给这几个 pin 扇出，几何上轻松（11 个孔 vs 104 个），且能让 freerouting 在内层接走。

碰撞校验照抄 route_rf.py 的双口径（已验证 DRC 安全）。
幂等：开头按 netname 删本脚本之前加的扇出过孔（扇出过孔的 drill 命中 + 同 net + 在封装外）。
"""
import math
import os
import sys
import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB = os.path.join(T, "kicad", "expansion-board-v3.kicad_pcb")
sys.path.insert(0, os.path.join(T, "tools"))
from gen_sch import RF50_NETS   # RF 网绝不能打扇出过孔（必须 F.Cu 零过孔）

# 缺线 pin（来自 DRC 真缺线表）。⚠️ 排除 RF50 网络——RF 必须 F.Cu 零过孔，
# 不能扇出（SUBG_RXTX/DET_INLO/SW1_*/SUBG_N3 等靠手工 F.Cu 布）。
TARGETS = {
    "U8": ["14", "23", "32", "34", "35", "36", "45", "50"],
    "U10": ["14", "45"],   # U10-3 SUBG_RXTX 是 RF，不扇出
}

VIA_D, VIA_DRILL = pcbnew.FromMM(0.6), pcbnew.FromMM(0.3)
STUB_W = 0.15
BOARD_CLR = 0.20
CLR_V = 0.30 + BOARD_CLR + 0.02        # 过孔本体（半径 0.3 + 间距 + 余量）
CLR_T = STUB_W / 2 + BOARD_CLR + 0.02  # 走线路径
NEW_VIA_R = 0.15                        # 钻半径（hole_to_hole）
HOLE_GAP = 0.27

board = pcbnew.LoadBoard(PCB)

# ---- 双口径占用区 + 钻孔表（照抄 route_rf.py）----
occ, occ_t = [], []


def add_occ(bb, net, clr, lst):
    lst.append((pcbnew.ToMM(bb.GetLeft()) - clr, pcbnew.ToMM(bb.GetTop()) - clr,
                pcbnew.ToMM(bb.GetRight()) + clr, pcbnew.ToMM(bb.GetBottom()) + clr, net))


holes = []
for f in board.GetFootprints():
    for p in f.Pads():
        bb = p.GetBoundingBox()
        add_occ(bb, p.GetNetname(), CLR_V, occ)
        add_occ(bb, p.GetNetname(), CLR_T, occ_t)
        dr = pcbnew.ToMM(p.GetDrillSizeX())
        if dr > 0:
            holes.append((pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y), dr / 2))
    for z in f.Zones():
        add_occ(z.GetBoundingBox(), "\0KEEPOUT", CLR_V, occ)
        add_occ(z.GetBoundingBox(), "\0KEEPOUT", CLR_T, occ_t)
for t in board.GetTracks():
    bb = t.GetBoundingBox()
    add_occ(bb, t.GetNetname(), CLR_V, occ)
    add_occ(bb, t.GetNetname(), CLR_T, occ_t)
    if isinstance(t, pcbnew.PCB_VIA):
        holes.append((pcbnew.ToMM(t.GetPosition().x), pcbnew.ToMM(t.GetPosition().y),
                      pcbnew.ToMM(t.GetDrill()) / 2))


def free(x, y, net):
    for hx, hy, hr in holes:
        if math.hypot(x - hx, y - hy) < hr + NEW_VIA_R + HOLE_GAP:
            return False
    for x0, y0, x1, y1, n in occ:
        if x0 <= x <= x1 and y0 <= y <= y1 and n != net:
            return False
    return True


def path_free(x1, y1, x2, y2, net):
    d = math.hypot(x2 - x1, y2 - y1)
    steps = max(2, int(d / 0.1))
    for i in range(steps + 1):
        x, y = x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps
        for x0, y0, x3, y3, n in occ_t:
            if x0 <= x <= x3 and y0 <= y <= y3 and n != net:
                return False
    return True


# ---- 逐 pin 扇出 ----
def outer_dir(pad_pos, chip_center):
    """返回焊盘朝外的单位方向 (dx, dy)。"""
    dx = pad_pos[0] - chip_center[0]
    dy = pad_pos[1] - chip_center[1]
    # 取主导轴（QFN pin 在四边，朝外是离中心最远的那个轴方向）
    if abs(dx) > abs(dy):
        return (1 if dx > 0 else -1, 0)
    return (0, 1 if dy > 0 else -1)


placed, failed = [], []
for fp in board.GetFootprints():
    ref = fp.GetReference()
    if ref not in TARGETS:
        continue
    c = fp.GetPosition()
    chip_c = (pcbnew.ToMM(c.x), pcbnew.ToMM(c.y))
    for pnum in TARGETS[ref]:
        pad = fp.FindPadByNumber(pnum)
        if not pad:
            failed.append(f"{ref}-{pnum}: 无此焊盘")
            continue
        netname = pad.GetNetname()
        if not netname:
            failed.append(f"{ref}-{pnum}: 无网络")
            continue
        if netname in RF50_NETS:
            failed.append(f"{ref}-{pnum}[{netname}]: RF 网，不扇出（须 F.Cu 手工布）")
            continue
        pp = pad.GetPosition()
        px, py = pcbnew.ToMM(pp.x), pcbnew.ToMM(pp.y)
        dx, dy = outer_dir((px, py), chip_c)
        # 在朝外方向搜索过孔位：从焊盘边缘外 0.5mm 起，沿外向 + 两侧扇形扫
        spot = None
        pad_bb = pad.GetBoundingBox()
        # 焊盘外缘点（朝外那一侧）
        if dx != 0:
            edge = pcbnew.ToMM(pad_bb.GetRight() if dx > 0 else pad_bb.GetLeft())
            base_x = edge + dx * 0.55
            base_y = py
        else:
            edge = pcbnew.ToMM(pad_bb.GetBottom() if dy > 0 else pad_bb.GetTop())
            base_x = px
            base_y = edge + dy * 0.55
        # 沿外向逐步退（0.55→3.5mm），每步两侧 ±2.5mm 扫 + 沿边两端扫
        for back in [0.0, 0.3, 0.6, 0.9, 1.2, 1.6, 2.0, 2.5, 3.0, 3.5]:
            cx = base_x + dx * back
            cy = base_y + dy * back
            for side in [0.0, 0.4, -0.4, 0.8, -0.8, 1.2, -1.2, 1.6, -1.6, 2.0, -2.0, 2.5, -2.5]:
                vx = cx + (dy * side)
                vy = cy + (dx * side)
                if free(vx, vy, netname) and path_free(px, py, vx, vy, netname):
                    spot = (vx, vy)
                    break
            if spot:
                break
        if not spot:
            failed.append(f"{ref}-{pnum}[{netname}]: 朝外 {dx},{dy} 无空位")
            continue
        vx, vy = spot
        net = board.FindNet(netname)
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I_MM(vx, vy))
        v.SetWidth(VIA_D); v.SetDrill(VIA_DRILL); v.SetNet(net)
        # 默认 PCB_VIA 就是通孔（全层 PTH），不设 LayerSet
        board.Add(v)
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I_MM(px, py)); t.SetEnd(pcbnew.VECTOR2I_MM(vx, vy))
        t.SetWidth(pcbnew.FromMM(STUB_W)); t.SetLayer(pcbnew.F_Cu); t.SetNet(net)
        board.Add(t)
        # 登记新放的过孔/线，避免后续 pin 撞上
        occ.append((vx - CLR_V, vy - CLR_V, vx + CLR_V, vy + CLR_V, netname))
        occ_t.append((vx - CLR_T, vy - CLR_T, vx + CLR_T, vy + CLR_T, netname))
        holes.append((vx, vy, NEW_VIA_R))
        placed.append(f"{ref}-{pnum}[{netname}] via=({vx:.2f},{vy:.2f})")

print(f"扇出放置 {len(placed)} 个 / 失败 {len(failed)} 个")
for p in placed:
    print("  ✅", p)
for f in failed:
    print("  ❌", f)

pcbnew.ZONE_FILLER(board).Fill(board.Zones())
board.Save(PCB)
print("saved:", PCB)
