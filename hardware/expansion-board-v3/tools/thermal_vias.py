#!/usr/bin/env python3
"""QFN/DFN 散热焊盘(EP)打过孔阵列。

## 为什么必须有

EP 是 QFN 的散热+接地通路，**必须打过孔阵列下到内层地平面**。本板 U8(RP2040)、
U10(CC1312R)、U11(QPL9547 LNA)、U13(AD8319 检波器) 四个 EP **一个过孔都没有**，
后果是：
  · 顶层那块 EP 铜只有 SMD 焊盘、没有穿层连接，KiCad 报
    "Zone[GND] on F.Cu ↔ Zone[GND] on In4.Cu 未连接"——这就是那两条一直查不出来的
    GND 未连通的真身
  · 芯片的热出不去（RP2040/CC1312R 满载都是几百 mW）
  · 射频芯片(U11/U13)的地回流被迫绕远，直接影响噪声系数和检波精度

LAYOUT_CONSTRAINTS §2.4 对 TA0970A 写了「长地焊盘下方打 ≥3 个 GND 过孔」，
主控和射频芯片反而漏了。

## 过孔怎么选

散热过孔用 **0.4mm 盘 / 0.2mm 孔**，与全板其余过孔一致。

⚠️ 曾经用 0.3/0.15（理由是"盘小才排得密"），但 2026-08-14 下单时发现
**嘉立创对 0.15mm 最小孔径额外收 200 元工艺费**（5 片打样从 300 涨到 500+）。
而 1.2mm 的孔心间距下，0.4mm 盘和 0.3mm 盘都排得开——"排得密"这个理由在
现在的孔距下根本不成立，等于白付这笔钱。改回 0.4/0.2 之后全板最小孔径 0.20mm，
属普通档不加收，将来批量下单也一直省着。

代价：U13(AD8319) 的 EP 只有 0.45mm 宽，放不下 0.4mm 盘（嘉立创要求 0.2mm 孔
至少配 0.4mm 盘，环宽 0.1），那 3 个孔已删除。它是检波器、功耗几十毫瓦不需要散热，
EP 顶层仍连在 F.Cu 的 GND 覆铜上，只是少了直下内层地的低阻抗路径。

位置在 EP 内细扫（0.1mm）后贪心选取，由内向外、彼此保持 PITCH_MM（1.2mm）间距，
避开所有层的走线/过孔/非 GND 焊盘。边缘留白避免盘超出 EP 边界。
孔数由 ep_targets.EP_VIAS 定死（U8 9 个 / U10 16 个），**够数就收手**——
不是铺满。为什么不铺满见 ep_targets.py 的 EP_VIAS 注释。

用法：thermal_vias.py [plan|apply]
"""
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

MODE = sys.argv[1] if len(sys.argv) > 1 else "plan"
VIA_D, VIA_DRILL = 0.45, 0.30
VIA_R = VIA_D / 2
EDGE = 0.35              # EP 边缘留白：盘半径 0.15 + 一点余量
# 孔心间距取自 ep_targets.PITCH_MM（1.2mm，业界推荐区间上限）。
# 原来是 0.8——那是"物理上最密能排多少"，不是"该排多少"，U10 因此打了 33 个。
HOLE_GAP = 0.2

board = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM

# ⚠️ 散热过孔是**通孔，穿透全部 6 层**。第一版只查了孔到孔间距就下手，
# 结果 63 个过孔在 In2/B.Cu 上撞出 **38 条短路 + 29 条 clearance**——
# EP 底下看着空，内层却横穿着 SUBG_IRQ / SUBG_VDDR / 3V3_GNSS 一堆走线。
# 必须把**所有层**的走线、过孔、非 GND 焊盘全部纳入净空检查。
HOLES, OBST = [], []
for f in board.GetFootprints():
    for p in f.Pads():
        if p.GetDrillSizeX() > 0:
            pp = p.GetPosition()
            HOLES.append((mm(pp.x), mm(pp.y), mm(p.GetDrillSizeX()) / 2))
        # ⚠️ 必须先确认这个"焊盘"在**铜层**上。QFN 的 EP 在封装里会被切成一堆
        # **F.Paste 锡膏分格**（U10 是 4×4 个 1.04mm 方块，钢网开窗的标准做法），
        # 它们 net 为空、不是 GND，于是被当成障碍——EP 正中央报"缺 0.173mm"，
        # 而那 0.173 正好是锡膏格之间的缝。**锡膏不是铜，压根不挡过孔。**
        # 我一度据此断言"U8/U10 打不下孔，因为内层走线把 EP 切碎了"——内层走线
        # 确实有（已由 export_dsn 的 EP keepout 清掉），但真正卡住的是这个。
        # （LSET 不支持 & 运算，用 IsOnCopperLayer；U10 实测 49 铜 / 16 锡膏格）
        if not p.IsOnCopperLayer():
            continue
        if p.GetNetname() != "GND":
            bb = p.GetBoundingBox()
            OBST.append(("rect", mm(bb.GetLeft()), mm(bb.GetTop()),
                         mm(bb.GetRight()), mm(bb.GetBottom()), 0.0))
for t in board.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA):
        p = t.GetPosition()
        HOLES.append((mm(p.x), mm(p.y), mm(t.GetDrill()) / 2))
        if t.GetNetname() != "GND":
            OBST.append(("seg", mm(p.x), mm(p.y), mm(p.x), mm(p.y),
                         mm(t.GetWidth()) / 2))
    elif t.GetNetname() != "GND":
        OBST.append(("seg", mm(t.GetStart().x), mm(t.GetStart().y),
                     mm(t.GetEnd().x), mm(t.GetEnd().y), mm(t.GetWidth()) / 2))

CLR = 0.2                # 对非 GND 的净空，取板规最严那档


def hole_ok(x, y):
    for hx, hy, hr in HOLES:
        if math.hypot(x - hx, y - hy) < hr + VIA_DRILL / 2 + HOLE_GAP:
            return False
    for kind, a, b, c, d, half in OBST:
        if kind == "rect":
            dist = math.hypot(max(a - x, 0, x - c), max(b - y, 0, y - d))
        else:
            dx, dy = c - a, d - b
            L = dx * dx + dy * dy
            if L < 1e-12:
                dist = math.hypot(x - a, y - b)
            else:
                t = max(0.0, min(1.0, ((x - a) * dx + (y - b) * dy) / L))
                dist = math.hypot(x - (a + dx * t), y - (b + dy * t))
        if dist < VIA_R + half + CLR:
            return False
    return True


# 名单与 export_dsn.py（负责给这些 EP 在 In2/B.Cu 留内层禁布区）**共用同一份**，
# 分叉了就会出现"留出来的地方没人打孔、打孔的地方没留位置"，而且 DRC 全绿、
# 只是孔数偏少，很难发现。为什么必须点名而非自动识别，见 ep_targets.py。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ep_targets import (TARGETS, EP_MIN_SIDE_MM,      # noqa: E402
                        EP_VIAS, PITCH_MM as PITCH_MIN)

plan = []
for f in board.GetFootprints():
    ref = f.GetReference()
    if ref not in TARGETS:
        continue
    for p in f.Pads():
        bb = p.GetBoundingBox()
        w, h = mm(bb.GetWidth()), mm(bb.GetHeight())
        if max(w, h) <= EP_MIN_SIDE_MM or p.GetNetname() != "GND":
            continue
        if not p.GetLayerSet().Contains(pcbnew.F_Cu):
            continue
        pp = p.GetPosition()
        cx, cy = mm(pp.x), mm(pp.y)
        # 小 EP（如 U13 只有 0.45mm 宽）按固定 EDGE 会算出负的可用区，
        # 改成"至少留盘半径 + 0.05mm"，窄边就只打一列。
        e = min(EDGE, max(0.0, (min(w, h) - VIA_D) / 2 - 0.02))
        uw, uh = w - 2 * e, h - 2 * e                 # 可用区
        if uw <= 0 or uh <= 0:
            continue
        # ⚠️ 不能用固定均匀网格。第一版按 nx×ny 等距排，格点撞上走线就整个放弃，
        # 结果 U8/U10/U11 报"一个都打不下"——而截图一看，EP 底下的内层走线只是把
        # 空间切成网格状的**空隙**，空隙里明明放得下，只是要挪一挪。
        # 改成：在 EP 内按 0.1mm 细扫所有候选点，贪心挑（保持彼此 PITCH_MIN 间距），
        # 优先离 EP 中心近的（散热效率高）。
        want = EP_VIAS.get(ref)
        spots = []
        if want:
            # 大 EP 走**规则阵列**。export_dsn 已按同一份 EP_VIAS/PITCH_MM 在
            # In2/B.Cu 给中心区留了禁布区，那块地是干净的，规则排必然排得下。
            # 贪心细扫在这里反而吃亏：它按"离中心最近"取点，排不出规则格阵，
            # 实测 U10 只放到 13/16、U8 7/9——空出来的位置是被环带走线蹭掉的。
            k = int(round(want ** 0.5))
            span = (k - 1) * PITCH_MIN
            axis = [(-span / 2 + i * PITCH_MIN) for i in range(k)]
            for dy in axis:
                for dx in axis:
                    x, y = cx + dx, cy + dy
                    if not hole_ok(x, y):
                        print(f"  ⚠️ {ref} 阵列点 ({x:.2f},{y:.2f}) 被占，keepout 没起作用？")
                        continue
                    spots.append((x, y))
                    HOLES.append((x, y, VIA_DRILL / 2))
        else:
            # 小 EP（FL1/FL2 的 2.2×0.8 长地焊盘、U11/U13）用细扫贪心，
            # 间距放宽到 0.8mm——1.2mm 在这种尺寸上只塞得下 1 个，
            # 而 LAYOUT_CONSTRAINTS §2.4 对 TA0970A 明确要求 ≥3 个。
            # 这些孔的作用是**射频地回流**不是散热，密排本来就是对的。
            SMALL_PITCH = 0.8
            STEP = 0.1
            cand = []
            yy = cy - uh / 2
            while yy <= cy + uh / 2 + 1e-9:
                xx = cx - uw / 2
                while xx <= cx + uw / 2 + 1e-9:
                    cand.append((math.hypot(xx - cx, yy - cy), xx, yy))
                    xx += STEP
                yy += STEP
            cand.sort()                      # 由内向外
            for _, x, y in cand:
                if any(math.hypot(x - sx, y - sy) < SMALL_PITCH for sx, sy in spots):
                    continue
                if not hole_ok(x, y):
                    continue
                spots.append((x, y))
                HOLES.append((x, y, VIA_DRILL / 2))
        nx = ny = 0                      # 不再是规则阵列
        plan.append((ref, p.GetNumber(), w, h, nx, ny, spots))

print(f"散热过孔 {VIA_D}mm 盘 / {VIA_DRILL}mm 孔（依赖嘉立创 6 层默认的树脂塞孔+电镀）\n")
tot = 0
for ref, num, w, h, nx, ny, spots in plan:
    print(f"  {ref:5s} EP(pin{num:2s}) {w:.2f}x{h:.2f}mm  →  实放 {len(spots):2d} 个"
          f"   {TARGETS[ref]}")
    tot += len(spots)
print(f"\n合计 {tot} 个")

if MODE == "apply" and tot:
    net = board.FindNet("GND")
    for ref, num, w, h, nx, ny, spots in plan:
        for x, y in spots:
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pcbnew.VECTOR2I_MM(round(x, 4), round(y, 4)))
            v.SetWidth(pcbnew.FromMM(VIA_D))
            v.SetDrill(pcbnew.FromMM(VIA_DRILL))
            v.SetNet(net)
            board.Add(v)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(PCB)
    print(f"已落盘 → {PCB}")
else:
    print("（plan 模式，未写盘）")
