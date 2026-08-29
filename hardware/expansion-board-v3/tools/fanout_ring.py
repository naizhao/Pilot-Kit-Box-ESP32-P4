#!/usr/bin/env python3
"""QFN 规整扇出环——在 freerouting **之前**占住引脚净空环。

## 为什么要它（fanout_v31.py 的教训）

`fanout_v31.py` 只给 11 个 pin 扇出（U8 八个 + U10 两个），理由是"几何上轻松
（11 个孔 vs 104 个）"。实测下来，**部分扇出恰恰是病根**：

先扇的那几个孔按"贪心找空位"落在引脚出口正前方，把还没扇出的引脚堵死了。收尾
布线时挡住 U10.44 / U8.13 / U8.47 的，全是这类扇出过孔和 freerouting 顺着钻进
净空环的走线——`route_fix.py` 的诊断显示，挡路者里**焊盘类 0 项**。

而元件其实早就让开了：实测 U8 焊盘外 1mm 内 0 个元件，U10 焊盘外 2mm 内 0 个
元件。空的环带被走线和过孔占掉了，不是元件挤的。

所以这里改成**规整环**：按边分排、落在引脚中心线延长线上，一次性把环带占满，
freerouting 只能从环外接。位置确定性，不再是贪心的随机结果。

## 跳过哪些引脚

- RF50 网络：必须 F.Cu 零过孔（In1 是它的参考面，打孔就在参考面上开洞）
- GND：走 EP + 就近缝合过孔，不占环位
- unconnected-*：没有网络，扇了也是悬空铜
- 放不下的：留给 freerouting 走 F.Cu。U8 下边净空只有 1.22mm（C28/R9/C29），
  只放得下 1 排 9 孔而该边有 14 脚——剩下 5 个正好把 F.Cu 走廊让出来。

用法：fanout_ring.py [plan|apply]   （PK_BOARD_DIR 可指到副本）
"""
import math
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from track45 import add_track45   # noqa: E402

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

from gen_sch import RF50_NETS                                    # noqa: E402

MODE = sys.argv[1] if len(sys.argv) > 1 else "plan"
TARGETS = ["U8", "U10"]

VIA_D, VIA_DRILL = 0.45, 0.3     # 嘉立创 0.3mm 免费档（外径 0.4/0.45）
VIA_R = VIA_D / 2
STUB_W = 0.15                    # Default 类最小线宽，出引脚用最细的
CLR = 0.15                       # Default 净空；遇 POWER 邻居时按 max 取
ROW_STEP = 0.70                  # 排间径向距离（0.4 盘 + 0.15 净空 + 余量）
ROW0 = 0.45                      # 第一排过孔中心距焊盘外沿（盘半径 0.2 + 净空 0.15 + 余量）
# 同排过孔中心距下限。这个数不是"排得下就行"，而是**中间要能过一条线**——
# 交错之后，第一排两个孔之间正是第二排那根引脚的 stub 要穿过去的地方：
#     0.45(盘) + 2×0.15(净空) + 0.15(线宽) = 0.90
# 评审问的"多个针脚过孔是否要留下足够空间"，答案就是这条式子。
# 电源线（0.25 宽）走这里会差 0.1mm，所以电源脚优先安排在最外排。
PITCH_MIN = 0.90
# 孔到孔：钻孔 0.30 + 板规 min_hole_to_hole 0.20 = 0.50mm 孔心距。
# ⚠️ 这个值随钻孔径变，不是常数——孔从 0.2 涨到 0.3，孔心距要求就从 0.40 涨到 0.50。
# 之前写 0.25 是按 0.2mm 孔算的（0.2+0.05 余量），孔一变大就不够了。
HOLE_GAP = 0.20                  # 板规 min_hole_to_hole；实际判据是 孔径 + 这个值

board = pcbnew.LoadBoard(PCB)


def clr_of(nm):
    return 0.2 if nm in ("VCC_5V", "USB_VBUS", "RP_1V1", "SUBG_VDDR") \
        or nm.startswith("3V3") else 0.15


# ── 障碍表（连续几何，不用 bbox 膨胀）───────────────────────────────────
PADS, TRKS, VIAS, HOLES = [], [], [], []
for f in board.GetFootprints():
    for p in f.Pads():
        bb = p.GetBoundingBox()
        PADS.append((p.GetNetname(),
                     (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                      pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom()))))
        dr = pcbnew.ToMM(p.GetDrillSizeX())
        if dr > 0:
            pp = p.GetPosition()
            HOLES.append((pcbnew.ToMM(pp.x), pcbnew.ToMM(pp.y), dr / 2))
    for z in f.Zones():
        bb = z.GetBoundingBox()
        PADS.append(("\0KEEPOUT",
                     (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                      pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom()))))
for t in board.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA):
        p = t.GetPosition()
        VIAS.append((t.GetNetname(), (pcbnew.ToMM(p.x), pcbnew.ToMM(p.y))))
        HOLES.append((pcbnew.ToMM(p.x), pcbnew.ToMM(p.y), pcbnew.ToMM(t.GetDrill()) / 2))
    else:
        TRKS.append((t.GetNetname(),
                     (pcbnew.ToMM(t.GetStart().x), pcbnew.ToMM(t.GetStart().y),
                      pcbnew.ToMM(t.GetEnd().x), pcbnew.ToMM(t.GetEnd().y)),
                     pcbnew.ToMM(t.GetWidth()) / 2))


# 空间预筛：候选位置 5mm 外的障碍直接跳过。不加这一步是 O(引脚×候选×全部障碍)，
# 每条 stub 还要按 0.02mm 采样，实测 100 个引脚就跑过 2 分钟。
def _near(items, x, y, rad=5.0):
    out = []
    for it in items:
        g = it[1]
        if len(g) == 4 and not isinstance(g[0], tuple):      # 矩形或线段
            mx, my = (g[0] + g[2]) / 2, (g[1] + g[3]) / 2
            ext = max(abs(g[2] - g[0]), abs(g[3] - g[1])) / 2
        else:                                                 # 点
            mx, my, ext = g[0], g[1], 0.0
        if math.hypot(x - mx, y - my) <= rad + ext:
            out.append(it)
    return out


def pt_rect(px, py, r):
    return math.hypot(max(r[0] - px, 0, px - r[2]), max(r[1] - py, 0, py - r[3]))


def seg_pt(a, b, p):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L2 = dx * dx + dy * dy
    if L2 < 1e-12:
        return math.hypot(p[0] - a[0], p[1] - a[1])
    t = max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / L2))
    return math.hypot(p[0] - (a[0] + dx * t), p[1] - (a[1] + dy * t))


def seg_seg(a, b, c, d):
    """线段间最短距离。相交时必须返回 0——只取四端点到对方线段的最小值会把
    交叉判成有余量（route_fix 就是这么把 SWDIO 布得和 SUBG_CSN 短路的）。"""
    def cr(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    d1, d2, d3, d4 = cr(c, d, a), cr(c, d, b), cr(a, b, c), cr(a, b, d)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
        return 0.0
    return min(seg_pt(a, b, c), seg_pt(a, b, d), seg_pt(c, d, a), seg_pt(c, d, b))


def seg_rect(a, b, r):
    best = min(pt_rect(a[0], a[1], r), pt_rect(b[0], b[1], r))
    n = max(2, int(math.hypot(b[0] - a[0], b[1] - a[1]) / 0.05))
    for i in range(n + 1):
        t = i / n
        best = min(best, pt_rect(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, r))
    return best


def via_ok(x, y, nm):
    myc = clr_of(nm)
    _pads, _vias, _trks = _near(PADS, x, y), _near(VIAS, x, y), _near(TRKS, x, y)
    for hx, hy, hr in HOLES:
        if abs(x - hx) > 3 or abs(y - hy) > 3:
            continue
        if math.hypot(x - hx, y - hy) < hr + VIA_DRILL / 2 + HOLE_GAP:
            return False
    for onm, r in _pads:
        if onm == nm:
            continue
        if pt_rect(x, y, r) < VIA_R + max(myc, clr_of(onm)):
            return False
    for onm, p in _vias:
        if onm == nm:
            continue
        if math.hypot(x - p[0], y - p[1]) < VIA_R * 2 + max(myc, clr_of(onm)):
            return False
    for onm, g, oh in _trks:
        if onm == nm:
            continue
        if seg_pt((g[0], g[1]), (g[2], g[3]), (x, y)) < VIA_R + oh + max(myc, clr_of(onm)):
            return False
    return True


def stub_ok(a, b, nm):
    myc = clr_of(nm)
    mx, my = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
    _pads, _vias, _trks = _near(PADS, mx, my), _near(VIAS, mx, my), _near(TRKS, mx, my)
    for onm, r in _pads:
        if onm == nm:
            continue
        if seg_rect(a, b, r) < STUB_W / 2 + max(myc, clr_of(onm)):
            return False
    for onm, p in _vias:
        if onm == nm:
            continue
        if seg_pt(a, b, p) < STUB_W / 2 + VIA_R + max(myc, clr_of(onm)):
            return False
    for onm, g, oh in _trks:
        if onm == nm:
            continue
        d = seg_seg(a, b, (g[0], g[1]), (g[2], g[3]))
        if d < STUB_W / 2 + oh + max(myc, clr_of(onm)):
            return False
    return True


def snap_pads(f):
    """一次遍历把该封装所有焊盘抽成纯数据。

    ⚠️ 同一个 footprint 的 `f.Pads()` **只能遍历一次**。原来 padbox(f) 遍历一遍、
    收集边分类又遍历一遍，第二遍拿到的是失效的 SWIG 代理，读出来的 bbox 是垃圾
    ——实测算出 -764923mm 的过孔坐标，SetPosition 直接 OverflowError。
    fanout_v31.py 用 FindPadByNumber 单个取，恰好绕开了这个坑。"""
    out = []
    for p in f.Pads():
        bb = p.GetBoundingBox()
        box = (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
               pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom()))
        pp = p.GetPosition()
        out.append(dict(num=p.GetNumber(), net=p.GetNetname(),
                        x=pcbnew.ToMM(pp.x), y=pcbnew.ToMM(pp.y), box=box,
                        w=box[2] - box[0], h=box[3] - box[1]))
    return out


placed, skipped, failed = [], [], []
new_tracks = []

SNAP = {}
for f in board.GetFootprints():                 # 只遍历一次，当场抽干净
    if f.GetReference() in TARGETS:
        c = f.GetPosition()
        SNAP[f.GetReference()] = (pcbnew.ToMM(c.x), pcbnew.ToMM(c.y), snap_pads(f))

for ref in TARGETS:
    if ref not in SNAP:
        continue
    cx, cy, pads = SNAP[ref]
    real = [q for q in pads if max(q["w"], q["h"]) <= 3]     # 滤掉 EP 散热盘
    x0 = min(q["box"][0] for q in real); y0 = min(q["box"][1] for q in real)
    x1 = max(q["box"][2] for q in real); y1 = max(q["box"][3] for q in real)

    sides = {"U": [], "D": [], "L": [], "R": []}
    for q in real:
        px, py, w, h = q["x"], q["y"], q["w"], q["h"]
        s = ("D" if py > cy else "U") if h >= w else ("R" if px > cx else "L")
        sides[s].append((q["num"], q["net"], px, py, q["box"]))

    DIRV = {"U": (0, -1), "D": (0, 1), "L": (-1, 0), "R": (1, 0)}
    for s, pins in sides.items():
        dx, dy = DIRV[s]
        # 沿边排序，保证相邻引脚分到不同排（交错），同排间距才拉得开
        pins.sort(key=lambda z: z[3] if dx else z[2])
        todo = []
        for num, nm, px, py, box in pins:
            if not nm or nm.startswith("unconnected"):
                skipped.append(f"{ref}.{num} 无网络")
                continue
            if nm in RF50_NETS:
                skipped.append(f"{ref}.{num}[{nm}] RF·不扇出")
                continue
            if nm == "GND":
                skipped.append(f"{ref}.{num}[GND] 走 EP")
                continue
            todo.append((num, nm, px, py, box))
        # 同排间距要 ≥PITCH_MIN：引脚 pitch 0.4 → 每 3 个一排；0.5 → 每 2 个一排
        # 上下边的引脚沿 x 排开，左右边沿 y 排开——取错轴会算出 span=0，
        # 于是 pitch→0、stride 爆成 85 万，过孔坐标直接飞到 765000mm。
        if len(pins) > 1:
            span = abs(pins[-1][2] - pins[0][2]) if dx == 0 else abs(pins[-1][3] - pins[0][3])
            pitch = span / max(1, len(pins) - 1)
        else:
            pitch = 0.5
        assert pitch > 0.05, f"{ref} {s}边 pitch={pitch} 异常，边分类或排序有问题"
        stride = max(1, min(6, int(math.ceil(PITCH_MIN / pitch))))
        # ⚠️ 排号必须按**物理位置**算，不能按 todo 的序号。
        # todo 已经滤掉了 GND/RF/无网络的引脚，`i % stride` 于是和实际位置脱钩：
        # 两个物理上只隔一个 pitch(0.4mm) 的引脚，若中间跳过了 stride 整数倍个脚，
        # 就会拿到同一个 row，同排孔心距只有 0.4mm < PITCH_MIN(0.85)，via_ok 判失败。
        # 症状就是一堆"X边无位"——U8.23[RP_1V1] 这颗关键脚正是这么丢的，
        # 而它恰好是评审点名要救的两处未连通之一。
        # 用该边**全部**引脚的排序序号，物理相邻必然分到不同排，
        # 同排间距 = stride × pitch ≥ PITCH_MIN，与注释里的设计意图一致。
        pos_index = {z[0]: k for k, z in enumerate(pins)}
        for num, nm, px, py, box in todo:
            row = pos_index[num] % stride
            if dx:
                edge = box[2] if dx > 0 else box[0]
                base = (edge + dx * ROW0, py)
            else:
                edge = box[3] if dy > 0 else box[1]
                base = (px, edge + dy * ROW0)
            spot = None
            # 先试本排，再依次外推——外推是为了保持"同排等距"的规整性，
            # 不是贪心乱找位（贪心正是 fanout_v31 把引脚出口堵死的原因）
            for extra in (row, row + stride, row + 1, row + 2, row + 3, row + 4):
                vx = base[0] + dx * ROW_STEP * extra
                vy = base[1] + dy * ROW_STEP * extra
                if via_ok(vx, vy, nm) and stub_ok((px, py), (vx, vy), nm):
                    spot = (vx, vy)
                    break
            if not spot:
                failed.append(f"{ref}.{num}[{nm}] {s}边无位")
                if os.environ.get("PK_WHY", "") in (f"{ref}.{num}", "all"):
                    print(f"  ── 为什么 {ref}.{num}[{nm}] 放不下（row={row} stride={stride}）")
                    for extra in (row, row + stride, row + 1, row + 2, row + 3, row + 4):
                        vx = base[0] + dx * ROW_STEP * extra
                        vy = base[1] + dy * ROW_STEP * extra
                        why = []
                        if not via_ok(vx, vy, nm):
                            for onm, r in _near(PADS, vx, vy):
                                if onm != nm and pt_rect(vx, vy, r) < VIA_R + max(clr_of(nm), clr_of(onm)):
                                    why.append(f"焊盘[{onm}]缺{VIA_R + max(clr_of(nm), clr_of(onm)) - pt_rect(vx, vy, r):.3f}")
                            for onm, q in _near(VIAS, vx, vy):
                                if onm != nm and math.hypot(vx - q[0], vy - q[1]) < VIA_R * 2 + max(clr_of(nm), clr_of(onm)):
                                    why.append(f"过孔[{onm}]缺{VIA_R * 2 + max(clr_of(nm), clr_of(onm)) - math.hypot(vx - q[0], vy - q[1]):.3f}")
                            for onm, g, oh in _near(TRKS, vx, vy):
                                if onm != nm and seg_pt((g[0], g[1]), (g[2], g[3]), (vx, vy)) < VIA_R + oh + max(clr_of(nm), clr_of(onm)):
                                    why.append(f"走线[{onm}]缺{VIA_R + oh + max(clr_of(nm), clr_of(onm)) - seg_pt((g[0], g[1]), (g[2], g[3]), (vx, vy)):.3f}")
                        elif not stub_ok((px, py), (vx, vy), nm):
                            why.append("stub 撞了")
                        print(f"     extra={extra} 位({vx:.2f},{vy:.2f}) " + ("；".join(why[:3]) if why else "OK?"))
                continue
            VIAS.append((nm, spot))
            HOLES.append((spot[0], spot[1], VIA_DRILL / 2))
            TRKS.append((nm, (px, py, spot[0], spot[1]), STUB_W / 2))
            new_tracks.append((nm, (px, py), spot))
            placed.append(f"{ref}.{num}[{nm}] {s}边 → ({spot[0]:.2f},{spot[1]:.2f})")

print(f"扇出环：放置 {len(placed)} / 放不下 {len(failed)} / 按规则跳过 {len(skipped)}")
byside = {}
for p in placed:
    k = p.split("边")[0].split("] ")[-1]
    byside[k] = byside.get(k, 0) + 1
print("  各边放置数:", byside)
if failed:
    print("  放不下（留 F.Cu 走廊给 freerouting）:")
    for x in failed:
        print("   ", x)

if MODE == "apply":
    for nm, (px, py), (vx, vy) in new_tracks:
        net = board.FindNet(nm)
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I_MM(round(vx, 4), round(vy, 4)))
        v.SetWidth(pcbnew.FromMM(VIA_D))
        v.SetDrill(pcbnew.FromMM(VIA_DRILL))
        v.SetNet(net)
        board.Add(v)
        # 焊盘→过孔必须走 45°/正交，不能直接连一条斜线：过孔在焊盘斜外侧，
        # 直连出来是 30°/60°，freerouting 的 45 度化后处理会卡死不出 SES。
        # 折线严格在起终点的包围矩形内，占地和直线一样，不会碰到别的东西。
        add_track45(board, px, py, vx, vy, STUB_W, pcbnew.F_Cu, net)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(PCB)
    print("saved:", PCB)
else:
    print("（plan 模式，未写盘）")
