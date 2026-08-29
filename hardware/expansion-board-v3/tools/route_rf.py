#!/usr/bin/env python3
"""RF 关键链路布线 + GND 过孔墙 + 四层覆铜填充。

只布 RF 段（相邻元件间的短直连），数字段交给自动布线器。
依据 LAYOUT_CONSTRAINTS.md：RF50 线宽 **0.15mm**、链路直线短走、两侧 GND 过孔栅栏。
（这里原先写 0.34mm——那是四层板的值，6 层上只有 31.82Ω，见 IFA_ANTENNA.md §3.2。
 代码里的 RF_W 常量早就改成 0.15 了，这行注释一直没跟上。）

运行：~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 route_rf.py

断言：每段布线两端焊盘必须同网络；布线后该网络的未连接数必须减少。
"""
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from track45 import add_track45   # noqa: E402

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")   # 副本上跑整条链用
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

RF_W = pcbnew.FromMM(0.15)      # 50Ω @ JLC06161H-3313 外层（L1 参考 L2，介质 0.0994mm）
# ⚠️ 0.34 是 **4 层** JLC04161H-7628（L1-L2 介质 0.21mm）的值。96fc34e 转 6 层后
# 介质薄到 0.0994mm，同样线宽只有 **31.82Ω**（嘉立创官方阻抗计算器反算，2026-08-21）。
# 50Ω 对应 0.1509mm，取 0.15。失配的馈线会把天线谐振读数一起拖走。
# ⚠️ 同步板规 0.4/0.2。这两行原本写死 0.6/0.3，是改板规**之前**的旧值，
# 而本脚本打的平面缝合过孔有 242 个——占全板 451 个过孔的 **54%**，
# 等于一大半过孔都比该有的胖了 0.1mm 半径，在 QFN 引脚出口那种地方就是致命的：
# 挡住 U8.23[RP_1V1] 的正是其中一个 3V3_DIG 缝合孔，两孔间净空 0.325mm，
# 换成板规尺寸后是 0.425mm。在 KiCad 里想手动拉开它们、拉不动，
# 就是因为周围真的没有合法落点。
# 缝合过孔只承担平面间连接，载流需求低，缩小无风险。
# 同族问题（改了尺寸，散落各处写死的副本没跟着改）这是第 5 次：
# 改尺寸/层数之前，先全仓搜一遍还有没有写死的副本。
VIA_D = pcbnew.FromMM(0.45)
VIA_DRILL = pcbnew.FromMM(0.3)

board = pcbnew.LoadBoard(PCB)
FP = {f.GetReference(): f for f in board.GetFootprints()}

# 幂等：先清掉本脚本上次产生的走线与过孔（平面网 GND/3V3），避免重复叠加。
# ⚠️ 保留信号网（SUBG_*/RP_1V1/DEMOD*/I2C_* 等）的扇出过孔——那些是 fanout_v31.py
# 在本脚本之前放的，清掉会让 QFN 扇出丢失。route_rf 只产平面网铜，绝不碰信号网。
_PLANE_NETS = {"GND", "3V3_DIG", "3V3_RF", "3V3_GNSS"}
_old = list(board.GetTracks())
_removed = 0
for t in _old:
    if t.GetNetname() in _PLANE_NETS:
        board.Remove(t); _removed += 1
if _removed:
    print(f"清理旧平面网走线/过孔 {_removed} 项（信号网扇出已保留）")


def pad(ref, num):
    fp = FP.get(ref)
    assert fp, f"找不到元件 {ref}"
    for p in fp.Pads():
        if p.GetNumber() == num:
            return p
    raise AssertionError(f"{ref} 没有焊盘 {num}")


def _seg(a, b, net):
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(a)
    t.SetEnd(b)
    t.SetWidth(RF_W)
    t.SetLayer(pcbnew.F_Cu)
    t.SetNet(net)
    board.Add(t)


def track(p1, p2):
    """连接两个焊盘：同 y 走直线；不同 y 走"水平-竖直-水平"折线，
    竖直段落在两焊盘之间的空当，避免斜穿自家其它焊盘。"""
    n1, n2 = p1.GetNetname(), p2.GetNetname()
    assert n1 == n2, f"两端网络不同: {n1} vs {n2}"
    net = p1.GetNet()
    a, b = p1.GetPosition(), p2.GetPosition()
    if abs(a.y - b.y) < pcbnew.FromMM(0.05):
        _seg(a, b, net)
    else:
        xm = (a.x + b.x) // 2
        m1 = pcbnew.VECTOR2I(xm, a.y)
        m2 = pcbnew.VECTOR2I(xm, b.y)
        _seg(a, m1, net); _seg(m1, m2, net); _seg(m2, b, net)
    return n1


# ---------------- 1090：天线切换 + 接收链（相邻件短直连）----------------
SWITCH_1090 = [
    ("C30", "2", "U16", "3"),     # 外接 SMA 支路 → 开关 J2
    ("C53", "2", "U16", "1"),     # 板载 IFA 支路 → 开关 J3
    ("U16", "5", "C54", "1"),     # 开关公共口 J1 → 隔直
]
CHAIN_1090 = [
    ("C54", "2", "U11", "2"),     # → LNA① 输入
    ("U11", "7", "C31", "1"),
    ("C31", "2", "FL1", "B"),     # → SAW①
    ("FL1", "E", "C32", "1"),
    ("C32", "2", "U12", "6"),     # → LNA②
    ("U12", "3", "C33", "1"),
    ("C33", "2", "FL2", "B"),     # → SAW②
    ("FL2", "E", "C34", "1"),
    ("C34", "2", "U13", "1"),     # → 检波器 INHI
]
# ---------------- GNSS 天线切换 ----------------
SWITCH_GNSS = [
    ("C57", "2", "U17", "5"),     # 公共口 → GNSS 模块
    ("C58", "2", "U17", "3"),     # 外接支路
    ("C59", "2", "U17", "1"),     # 内置 patch 支路
]
# ---------------- 978 匹配（同行串联主路径）----------------
CHAIN_978 = [
    ("L9", "2", "L11", "1"), ("L11", "2", "L12", "1"), ("L12", "2", "C39", "1"),
    ("C39", "2", "J5", "1"), ("L13", "2", "C45", "1"),
]

# 【已停用】朴素点对点布线只在真正相邻时成立，跨件会穿过别人的焊盘。
# 布线全部交给 freerouting（RF50 网络类保证线宽，V3.5 起为 0.15mm——0.34 是 4 层
# 叠层的值，在 JLC06161H-3313 六层上只有 31.8Ω），本脚本只做缝合过孔与过孔墙。
# 唯一的例外是 IFA 馈电脚那一段，见 route_ifa_feed.py：它要按脚末端的几何来布，
# 且两侧要挖无铜走廊，freerouting 表达不了。
routed = []
for a, pa, b_, pb in []:
    # 仅区内相邻件；跨区长连接交给 freerouting 绕障
    routed.append(track(pad(a, pa), pad(b_, pb)))
print(f"布线 {len(routed)} 段，覆盖网络: {sorted(set(routed))}")

# ---------------- GND 过孔栅栏（1090 链两侧）----------------
gnd = board.FindNet("GND")
assert gnd, "GND 网络不存在"
# 占用区 = 焊盘实体（不能用 GetBoundingBox：含丝印位号文字，会把整板判为占用）
# 外扩 = 过孔半径 0.3 + 间距 0.15 + 余量 0.25
CLR = 0.7
occupied = []
for f in board.GetFootprints():
    for p in f.Pads():
        bb = p.GetBoundingBox()
        occupied.append((pcbnew.ToMM(bb.GetLeft()) - CLR, pcbnew.ToMM(bb.GetTop()) - CLR,
                         pcbnew.ToMM(bb.GetRight()) + CLR, pcbnew.ToMM(bb.GetBottom()) + CLR))
# 已布的 RF 走线同样要避让
for t in board.GetTracks():
    if isinstance(t, pcbnew.PCB_TRACK) and not isinstance(t, pcbnew.PCB_VIA):
        bb = t.GetBoundingBox()
        occupied.append((pcbnew.ToMM(bb.GetLeft()) - CLR, pcbnew.ToMM(bb.GetTop()) - CLR,
                         pcbnew.ToMM(bb.GetRight()) + CLR, pcbnew.ToMM(bb.GetBottom()) + CLR))


def free(x, y):
    return not any(x0 <= x <= x1 and y0 <= y <= y1 for x0, y0, x1, y1 in occupied)


n_via = 0
for y in (64.9, 74.0):
    x = 82.0
    while x <= 144.0:
        if free(x, y):
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pcbnew.VECTOR2I_MM(x, y))
            v.SetWidth(VIA_D)
            v.SetDrill(VIA_DRILL)
            v.SetNet(gnd)
            board.Add(v)
            n_via += 1
        x += 2.0
print(f"GND 过孔栅栏: {n_via} 个")

# ---------------- 平面缝合过孔（必须在自动布线之前做）----------------
# L2=GND 平面、L3=3V3_DIG 平面。给每个该网络的焊盘就近配一个过孔直接入平面，
# 这样自动布线器会把它们当障碍绕开；事后再补会撞已有走线。
import math

# 可缝合的网络及其有效范围，**直接从板上的覆铜区读出**，不在这里硬编码坐标——
# gen_pcb.py 改了电源岛位置，这里自动跟着变。范围外的焊盘打过孔会连到空处（via_dangling）。
PLANE_RANGE = {}          # 网络名 -> [(x0,y0,x1,y1), ...]
for _z in board.Zones():
    _bb = _z.GetBoundingBox()
    PLANE_RANGE.setdefault(_z.GetNetname(), []).append(
        (pcbnew.ToMM(_bb.GetLeft()), pcbnew.ToMM(_bb.GetTop()),
         pcbnew.ToMM(_bb.GetRight()), pcbnew.ToMM(_bb.GetBottom())))
PLANE_NETS = tuple(sorted(PLANE_RANGE))
print(f"可缝合网络（读自覆铜区）: {PLANE_NETS}")


def in_plane(netname, px, py):
    return any(x0 <= px <= x1 and y0 <= py <= y1
               for x0, y0, x1, y1 in PLANE_RANGE.get(netname, ()))
# 板子默认网络类间距 = 0.2mm（DRC 报文 "clearance 0.2000 mm" 为准，不是 0.15）
BOARD_CLR = 0.20
# 扇出引线线宽。0.4mm pitch 的 QFN：焊盘中心到邻脚边缘 = 0.4-0.1 = 0.3mm，
# 需 STUB_W/2 + 0.2 ≤ 0.3 → STUB_W ≤ 0.2。取 0.15（JLC 四层最小 0.09，安全）。
# 用 0.25 会实测差到 0.175mm，必违例——细间距芯片扇出必须收窄线宽。
STUB_W = 0.15
CLR_V = 0.30 + BOARD_CLR + 0.02   # 【过孔本体】过孔半径 + 间距 + 余量（自焊盘边缘起算）
CLR_T = STUB_W / 2 + BOARD_CLR + 0.02  # 【走线路径】半线宽 + 间距 + 余量
assert CLR_T < 0.3, f"CLR_T={CLR_T:.3f} ≥ 0.3mm，0.4mm pitch QFN 扇不出来，须收窄 STUB_W"
# 过孔和走线必须分开算间距。曾经两者都用 CLR_V=0.47：0.4mm pitch 的 QFN 上
# 邻脚占用区半宽 = 焊盘半宽0.1 + 0.47 = 0.57 > 0.4，焊盘自身中心就落在邻脚占用区内，
# path_free 的第一个采样点即判失败 → U8/U10/U4 等细间距芯片一个缝合过孔都放不下。
# 实际 DRC 对走线只要求边缘间距 0.15mm，中心到邻脚边缘有 0.3mm，是合法的。
# 占用区带网络归属：同网络可以压（电气等价），异网络禁止
occ, occ_t = [], []


def add_occ(bb, net, pad=0.0, lst=None):
    (occ if lst is None else lst).append(
        (pcbnew.ToMM(bb.GetLeft()) - pad, pcbnew.ToMM(bb.GetTop()) - pad,
         pcbnew.ToMM(bb.GetRight()) + pad, pcbnew.ToMM(bb.GetBottom()) + pad, net))


for f in board.GetFootprints():
    for p in f.Pads():
        add_occ(p.GetBoundingBox(), p.GetNetname(), CLR_V)
        add_occ(p.GetBoundingBox(), p.GetNetname(), CLR_T, occ_t)
    for z in f.Zones():                       # U.FL 等封装自带禁布区：任何网络都不许进
        # 外扩量必须和焊盘同一套口径：判的是过孔/走线**中心**，铜边还要再占半个身位。
        # 曾经写死 0.1 —— 一直往外打时碰不到，改成优先往内侧打后立刻炸出 8 条 items_not_allowed。
        add_occ(z.GetBoundingBox(), "\0KEEPOUT", CLR_V)
        add_occ(z.GetBoundingBox(), "\0KEEPOUT", CLR_T, occ_t)
for t in board.GetTracks():
    add_occ(t.GetBoundingBox(), t.GetNetname(), CLR_V)
    add_occ(t.GetBoundingBox(), t.GetNetname(), CLR_T, occ_t)


# 钻孔表 (x, y, 孔半径)：同网络电气上可重叠，但钻孔物理上不行（hole_to_hole 规则）。
# 必须同时收录过孔与 PTH 焊盘（J1 排母是通孔件，孔径远大于过孔）。
NEW_VIA_R = pcbnew.ToMM(VIA_DRILL) / 2
HOLE_GAP = 0.27         # 孔壁间距（DRC min 0.25 + 余量）
holes = []
for t in board.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA):
        holes.append((pcbnew.ToMM(t.GetPosition().x), pcbnew.ToMM(t.GetPosition().y),
                      pcbnew.ToMM(t.GetDrill()) / 2))
for f in board.GetFootprints():
    for p in f.Pads():
        dr = pcbnew.ToMM(p.GetDrillSizeX())
        if dr > 0:
            holes.append((pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y), dr / 2))


def free2(x, y, net):
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
    """引线路径采样：整段都必须避开异网络占用区（按**走线**间距 CLR_T，不是过孔间距）。"""
    d = math.hypot(x2 - x1, y2 - y1)
    steps = max(2, int(d / 0.1))
    for i in range(steps + 1):
        x, y = x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps
        for x0, y0, x3, y3, n in occ_t:
            if x0 <= x <= x3 and y0 <= y <= y3 and n != net:
                return False
    return True


# ---------------- 飞线避让 ----------------
# 缝合过孔撒下去时只看了"元件内侧优先"，没看**别的网络要从哪儿走**。结果同一个
# 坑踩了三次：GND 缝合过孔正好压在 I2C_SDA 的必经走廊上，每轮布完都要手工删一次
# （527de88 删过两个、后面两轮各删一个）。
#
# 这里在布线**之前**先把信号网络的飞线（每网络的最小生成树边）算出来，选过孔位置
# 时同一半径圈内优先挑离飞线最远的点。飞线是直线、实际走线会绕，所以这只是启发式；
# 但对 I2C_SDA 那种 6mm 的短飞线，直线和实际路径基本重合，很管用。
_SIG_PADS = {}
for _f in board.GetFootprints():
    for _p in _f.Pads():
        _nm = _p.GetNetname()
        if not _nm or _nm in PLANE_NETS or _nm.startswith("unconnected"):
            continue
        _pp = _p.GetPosition()
        _SIG_PADS.setdefault(_nm, []).append(
            (pcbnew.ToMM(_pp.x), pcbnew.ToMM(_pp.y)))


def _mst_edges(pts):
    n = len(pts)
    if n < 2:
        return []
    used = [False] * n
    used[0] = True
    best = [(math.hypot(pts[i][0] - pts[0][0], pts[i][1] - pts[0][1]), 0)
            for i in range(n)]
    out = []
    for _ in range(n - 1):
        k, bd = -1, None
        for i in range(n):
            if not used[i] and (bd is None or best[i][0] < bd):
                k, bd = i, best[i][0]
        if k < 0:
            break
        used[k] = True
        out.append((pts[best[k][1]], pts[k]))
        for i in range(n):
            if not used[i]:
                d = math.hypot(pts[i][0] - pts[k][0], pts[i][1] - pts[k][1])
                if d < best[i][0]:
                    best[i] = (d, k)
    return out


RATS = []
for _nm, _pts in _SIG_PADS.items():
    for _a, _b in _mst_edges(_pts):
        RATS.append((_a[0], _a[1], _b[0], _b[1]))
print(f"飞线避让：信号飞线 {len(RATS)} 条纳入过孔选位")
RAT_CARE = 3.0          # 只关心 3mm 内的飞线，远的不影响选位


def rat_dist(x, y):
    """该点到最近信号飞线的距离；超过 RAT_CARE 一律当作 RAT_CARE（不再加分）。"""
    best = RAT_CARE
    for x1, y1, x2, y2 in RATS:
        if min(x1, x2) - best > x or x > max(x1, x2) + best:
            continue
        if min(y1, y2) - best > y or y > max(y1, y2) + best:
            continue
        dx, dy = x2 - x1, y2 - y1
        L2 = dx * dx + dy * dy
        if L2 < 1e-12:
            d = math.hypot(x - x1, y - y1)
        else:
            t = max(0.0, min(1.0, ((x - x1) * dx + (y - y1) * dy) / L2))
            d = math.hypot(x - (x1 + dx * t), y - (y1 + dy * t))
        if d < best:
            best = d
    return best


n_st, n_skip = 0, 0
for f in board.GetFootprints():
    for p in f.Pads():
        netname = p.GetNetname()
        if netname not in PLANE_NETS:
            continue
        px, py = pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y)
        if not in_plane(netname, px, py):   # 岛外焊盘：打了也连不上，交给布线器走线
            continue
        spot = None
        # 方向优先级：**先往元件内侧打**。QFN/LGA 这类外围引脚封装内部是空的
        # （U8 引脚在半径 3.2mm、散热焊盘边缘 1.6mm，中间 1.6mm 空环），
        # 而外侧 3.5–5mm 正是所有信号脚扇出必经的通道。把电源过孔打向外侧等于
        # 自己堵死出线口——实测 U8 外侧环带堆了 11+ 个过孔，11 个信号脚布不通。
        # 内侧打完全避开出线通道，且离散热焊盘 0.85mm > 需要的 0.5mm。
        fc = f.GetPosition()
        a_in = math.atan2(pcbnew.ToMM(fc.y) - py, pcbnew.ToMM(fc.x) - px)
        angles = sorted((2 * math.pi * k / 12 for k in range(12)),
                        key=lambda a: abs((a - a_in + math.pi) % (2 * math.pi) - math.pi))
        for r in (0.75, 1.0, 1.3, 1.7):
            ring = []
            for a in angles:
                cx, cy = px + r * math.cos(a), py + r * math.sin(a)
                if free2(cx, cy, netname) and path_free(px, py, cx, cy, netname):
                    ring.append((cx, cy))
            if ring:
                # 同一圈内全部可行位置里，挑离信号飞线最远的那个。
                # 保持"半径由内向外、角度内侧优先"的原有次序：只在**同圈**里择优，
                # 不会为了躲飞线跑到外圈去堵别人的扇出通道。
                spot = max(ring, key=lambda c: rat_dist(c[0], c[1]))
                break
        if not spot:
            n_skip += 1
            continue
        cx, cy = spot
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I_MM(cx, cy))
        v.SetWidth(VIA_D); v.SetDrill(VIA_DRILL); v.SetNet(p.GetNet())
        board.Add(v)
        # 45°/正交两段，理由同 fanout_ring：直连过孔是 30°/60°，freerouting
        # 的 45 度化后处理消化不了，卡死在 "N traces not 45 degree" 不出 SES。
        _pp = p.GetPosition()
        add_track45(board, pcbnew.ToMM(_pp.x), pcbnew.ToMM(_pp.y), cx, cy,
                    STUB_W, pcbnew.F_Cu, p.GetNet())
        occ.append((cx - CLR_V, cy - CLR_V, cx + CLR_V, cy + CLR_V, netname))
        occ_t.append((cx - CLR_T, cy - CLR_T, cx + CLR_T, cy + CLR_T, netname))
        holes.append((cx, cy, NEW_VIA_R))
        n_st += 1
print(f"平面缝合过孔: {n_st} 个（{n_skip} 个焊盘无空位，靠覆铜连接）")

# ---------------- 四层覆铜填充 ----------------
filler = pcbnew.ZONE_FILLER(board)
filler.Fill(board.Zones())
filled = sum(1 for z in board.Zones() if z.IsFilled())
assert filled == len(board.Zones()), f"覆铜填充 {filled}/{len(board.Zones())} 层未全填"
print(f"覆铜填充: {filled}/{len(board.Zones())} 区（4 层平面 + 电源岛）")

board.Save(PCB)
print("saved:", PCB)
