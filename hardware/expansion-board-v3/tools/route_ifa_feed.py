#!/usr/bin/env python3
"""布 v3 IFA 的两段受控50Ω走线：

1. ANT1封装内taper窄端 → ZP1.1 → ZS1.1（ANT1090_IFA）；
2. ZS1.2 → ZP2.1 → J7.1 → C53.1（IFA_MATCH）。

1.5→0.15mm、长1.5mm的taper已经是ANT1 pad1的自定义铜箔，并从馈电脚真实铜边
开始。本脚本从`ifa_geom.FEED_TAPER_END`走等宽0.15mm微带。布局冻结后，从馈电腿
中心线端点到铜边为0.750mm等宽段，随后是1.500mm taper和2.853mm微带，总计5.103mm。

π网络采用标准拓扑：ZP1/ZP2竖放，信号焊盘在上、GND焊盘在下；ZS1水平串联。
旧布局三颗都水平放，从ZP1.1到ZS1.1的直线会穿过ZP1.2 GND焊盘，本脚本不再兼容那个错误摆法。

## 为什么单独一个脚本

V3.5 把天线整体左移 6mm，馈点从 x=102 挪到 96，原来那 4 段走线还停在旧位置，
DRC 报 1 条未连通（Pad 1 [ANT1090_IFA] of ANT1 ↔ Track，length 0.4596mm）。

这些线本该由 route_rf.py 管——射频段要受控直连，不能交给 freerouting（原来那 4 段
带斜折的就是 freerouting 布的）。链路已补进 route_rf.py 的 SWITCH_1090 表。
但**不能为了这一条线去跑 route_rf.py**：它开头会清掉所有 GND/3V3_* 网络的走线，
包括 freerouting 布的那一大片，而它自己只补缝合过孔、不重布，GND 未连通会暴涨。
完整重跑要走 run_route.sh 那条 8 步链，且 freerouting 单次结果不可比。

所以这里只动 ANT1090_IFA / IFA_MATCH 两个网络，别的一律不碰。

## 走 45°

射频走线一律 45°，不走直角——route_rf.py 的 _link 走的是直角折线，那是数字段的
习惯。这里手写成"水平段 + 45° 收尾"。

逻辑幂等：先删这两个网络的所有走线再布，重复跑后的几何、网络和DRC结果一致。
KiCad会为重建的走线/规则区分配新UUID，因此整份PCB文本的文件指纹不保证相同。

运行：~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 route_ifa_feed.py
"""
import math
import os
import subprocess
import sys
import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
NET_ANT = "ANT1090_IFA"
NET_MATCH = "IFA_MATCH"

board = pcbnew.LoadBoard(PCB)
FP = {f.GetReference(): f for f in board.GetFootprints()}


def pad_xy(ref, num):
    fp = FP.get(ref)
    assert fp, f"板上没有 {ref}"
    for p in fp.Pads():
        if p.GetNumber() == num:
            return (pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y),
                    p.GetNetname())
    raise AssertionError(f"{ref} 没有 pad {num}")

# ⚠️ 所有坐标必须在 Remove() 之前读成纯数值：Remove 会让本进程里既有的 SWIG 代理
# 全部失效（import_routes.py:50 同一个坑），之后连 board.GetTracks() 都不可迭代。
#
# 🔴 起点**不能用 pad.GetPosition()**。天线 pad1 是一整块自定义焊盘（主臂+馈电脚+taper），
# 它的锚点落在**主臂**上；从那里出线会横穿整个净空区，把天线毁掉。
# 正确起点是封装内taper的窄端，坐标由 gen_ifa_footprint.py 导出，不在这里写死。
from ifa_geom import (FEED_LEG_END, FEED_LEG_COPPER_END, FEED_TAPER_END,
                      RF_WIDTH, TAPER_LENGTH)  # noqa: E402

RF_W = RF_WIDTH      # 50Ω单端微带 @ JLC06161H-3313（0.1509mm→50.0Ω）
_a = FP["ANT1"]
_ax0, _ay0 = pcbnew.ToMM(_a.GetPosition().x), pcbnew.ToMM(_a.GetPosition().y)

lx, ly = _ax0 + FEED_LEG_END[0], _ay0 + FEED_LEG_END[1]
wx, wy = _ax0 + FEED_LEG_COPPER_END[0], _ay0 + FEED_LEG_COPPER_END[1]
ax, ay = _ax0 + FEED_TAPER_END[0], _ay0 + FEED_TAPER_END[1]
an = pad_xy("ANT1", "1")[2]
zx, zy, zn = pad_xy("ZP1", "1")
s1x, s1y, s1n = pad_xy("ZS1", "1")
s2x, s2y, s2n = pad_xy("ZS1", "2")
p2x, p2y, p2n = pad_xy("ZP2", "1")
jx, jy, jn = pad_xy("J7", "1")
cx, cy, cn = pad_xy("C53", "1")
assert an == zn == s1n == NET_ANT, \
    f"天线侧网络不符: ANT1.1={an} ZP1.1={zn} ZS1.1={s1n}"
assert s2n == p2n == jn == cn == NET_MATCH, \
    f"匹配侧网络不符: ZS1.2={s2n} ZP2.1={p2n} J7.1={jn} C53.1={cn}"

# 冻结几何断言：这些不是“看起来差不多”，而是HFSS后的制造输入。
microstrip = ((zx - ax) ** 2 + (zy - ay) ** 2) ** 0.5
wide = ((wx - lx) ** 2 + (wy - ly) ** 2) ** 0.5
feed_total = wide + TAPER_LENGTH + microstrip
assert abs(zx - ax) < 0.001, f"taper窄端与ZP1.1未竖直对齐: dx={zx-ax:.4f}mm"
assert abs(wide - 0.750) < 0.001, f"中心线端点→铜边 {wide:.4f}mm != 0.750mm"
assert abs(microstrip - 2.853) < 0.002, f"外部50Ω馈线 {microstrip:.4f}mm != 2.853mm"
assert abs(feed_total - 5.103) < 0.002, f"腿末端→ZP1总路径 {feed_total:.4f}mm != 5.103mm"
assert abs(((ax - wx) ** 2 + (ay - wy) ** 2) ** 0.5 - TAPER_LENGTH) < 0.001
# ---- π 网络：钉死，不跟随布局 ----
# ZP1/ZS1/ZP2 必须紧贴天线馈点成一条水平线，这是匹配网络的电气要求，不是排版偏好：
# 馈点到 ZP1 的 2.853mm 微带是 HFSS 的模型输入，π 网络本身也必须在这条线上串起来，
# 中间插入任何拐弯都会引入 HFSS 没算过的寄生。**这三颗不要在 GUI 里挪。**
assert abs(zy - s1y) < 0.001 and s1x > zx, "ZP1.1→ZS1.1必须水平向右（π网络钉死件）"
assert abs(s2y - p2y) < 0.001 and p2x > s2x, "ZS1.2→ZP2.1必须水平向右（π网络钉死件）"

# ---- ZP2 → J7 → C53：跟随布局，随便摆 ----
# 这两段没有冻结几何：J7 是 U.FL 测试口、C53 是进 U16 的隔直电容，它们的位置纯粹
# 是版面问题。以前这里写死"必须水平向右"和"C53 必须在 J7 上方"，于是人一挪件脚本
# 就断言失败，只好去手描 zone——2026-08-26 连着四轮回归就是这么来的
# （走廊没跟着动 → 手工重建 → 改宽到 ±0.885 → 又把 ZS1 处该有的断口"补"掉）。
#
# 现在改成：给定任意相对位置，自动生成"长边直行 + 45° 收尾"的折线。射频一律 45°，
# 不走直角。走廊由 add_corridor() 按实际线段包络生成，因此自动跟随。
TOL = 0.001


def leg(p, q):
    """p→q 的中间拐点（不含两端）。同行/同列/纯45°直连，否则先走长边再45°收尾。"""
    (x1, y1), (x2, y2) = p, q
    dx, dy = x2 - x1, y2 - y1
    assert abs(dx) > TOL or abs(dy) > TOL, f"两点重合，无法布线: {p}"
    if abs(dx) < TOL or abs(dy) < TOL or abs(abs(dx) - abs(dy)) < TOL:
        return []                                   # 正交或正45°，直连
    if abs(dx) > abs(dy):                           # 横向为主：先水平，再45°
        return [(x2 - math.copysign(abs(dy), dx), y1)]
    return [(x1, y2 - math.copysign(abs(dx), dy))]  # 纵向为主：先垂直，再45°


match_pts = [(s2x, s2y), (p2x, p2y)]
for _nxt in ((jx, jy), (cx, cy)):
    match_pts += leg(match_pts[-1], _nxt) + [_nxt]

paths = {
    NET_ANT: [(ax, ay), (zx, zy), (s1x, s1y)],
    NET_MATCH: match_pts,
}
# 折返检查：J7/C53 挪到 ZP2 西边会让走线掉头穿回π网络，走廊也会自相重叠。
for _i, ((_x1, _y1), (_x2, _y2)) in enumerate(zip(match_pts[:-1], match_pts[1:])):
    assert (_x2 - _x1) > -TOL, (
        f"IFA_MATCH 第{_i+1}段向西折返 ({_x1:.3f}→{_x2:.3f})——"
        f"J7/C53 不能摆到 ZP2.1(x={p2x:.3f}) 西边")
net_items = {name: board.FindNet(name) for name in paths}
assert all(net_items.values()), f"缺少射频网络: {net_items}"

# Remove会让已有SWIG容器代理失效，所以必须先把待删对象一次性快照。
old_tracks = [t for t in board.GetTracks() if t.GetNetname() in paths]
KO_PREFIX = "ifa_rf50_corridor"
KO_NAMES = {name: f"{KO_PREFIX}_{name}" for name in (NET_ANT, NET_MATCH)}
old_zones = [z for z in board.Zones()
             if z.GetZoneName() == "ifa_feed_corridor"
             or z.GetZoneName().startswith(KO_PREFIX)]
for item in old_tracks + old_zones:
    board.Remove(item)

totals = {}
segments_by_net = {}
for name, pts in paths.items():
    total = 0.0
    net_segments = []
    for (x1, y1), (x2, y2) in zip(pts[:-1], pts[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I_MM(round(x1, 4), round(y1, 4)))
        t.SetEnd(pcbnew.VECTOR2I_MM(round(x2, 4), round(y2, 4)))
        t.SetWidth(pcbnew.FromMM(RF_W))
        t.SetLayer(pcbnew.F_Cu)
        t.SetNet(net_items[name])
        board.Add(t)
        total += ((x2 - x1) ** 2 + (y2 - y1) ** 2) ** 0.5
        net_segments.append(((x1, y1), (x2, y2)))
    totals[name] = total
    segments_by_net[name] = net_segments

# ---------------- 50Ω 走线的无铜走廊 ----------------
# 线宽 0.15mm 是按**纯微带**算的（嘉立创 JLC06161H-3313 官方计算器：0.1509mm→50Ω，
# 参考面是 In1 那层完整地平面）。纯微带的前提是两侧没有近距离共面地。
#
# 而板上 GND 铺铜的 zone clearance 只有 0.15mm——走线两侧 0.15mm 就是铜，
# 实物是共面波导（CPWG）而不是微带，实际阻抗和设计模型不是一回事。
# 天线那边刚因为"腿被 GND 包围"栽过一次，这里不留同样的隐患。
#
# 走廊半宽 = 半线宽 + NO_CU：NO_CU 取 0.5mm ≈ 5 倍介质厚（L1–L2 prepreg 3313
# 单张 0.0994mm）。微带的边缘场大约在 3 倍介质厚内衰减掉，5 倍是留了余量。
# ZP1/ZP2的GND焊盘在信号线下方1.55mm，走廊外仍有足够铺铜可直接入地。
#
# ⚠️ 只挖 F.Cu。In1 是这条微带的参考地平面，挖了就没有回流路径了，
# 那不是"更干净"，是把传输线拆了。
#
# 2026-08-26：改成可调，但**带下限断言**。想让走廊窄一点腾地方时改这里／设环境变量，
# 不要去手改 zone 多边形——手改的下场见上面 J7/C53 那段注释。
#
#   PK_IFA_NO_CU=0.35 python3 route_ifa_feed.py
#
# 下限 0.30mm 的来历：边缘场大致在 3 倍介质厚内衰减完，L1–L2 prepreg 3313 单张
# 0.0994mm → 3×0.0994 ≈ 0.30mm。0.50 是 5 倍，留了余量；低于 3 倍就不再是纯微带，
# 50Ω 那个线宽的前提失效，必须重算线宽或重跑 HFSS。
#
# ⚠️ 但先看清楚这个旋钮**能换回多少地方**：走廊总宽 = 0.15 + 2×NO_CU。
# 从 0.50 收到 0.30，总宽 1.150 → 0.750，两侧各只多让出 0.20mm。
# 走廊真正占地方的是**长度**，而长度 = ZS1.2 到 C53.1 的距离，由布局决定，不在这里调。
NO_CU_FLOOR = 0.30
NO_CU = float(os.environ.get("PK_IFA_NO_CU", "0.50"))
assert NO_CU >= NO_CU_FLOOR - 1e-9, (
    f"NO_CU={NO_CU} 低于下限 {NO_CU_FLOOR}mm（3 倍介质厚）——"
    "两侧共面地靠太近，这条线就不是 50Ω 微带了，得先重算线宽")
HALF = RF_W / 2 + NO_CU


def add_corridor(net_name, segments):
    """给一条连续路径建立独立规则区；不同网络不能塞进同一个多岛 outline。"""
    ps = pcbnew.SHAPE_POLY_SET()
    chains = []
    for (x1, y1), (x2, y2) in segments:
        dx, dy = x2 - x1, y2 - y1
        length = (dx * dx + dy * dy) ** 0.5
        ux, uy = dx / length, dy / length
        nx, ny = -uy, ux                 # 法线
        # 两端各沿轴向外延 HALF，拐角处两段矩形会合并，不留缺口。
        x1e, y1e = x1 - ux * HALF, y1 - uy * HALF
        x2e, y2e = x2 + ux * HALF, y2 + uy * HALF
        ch = pcbnew.SHAPE_LINE_CHAIN()
        for cx, cy in (
                (x1e + nx * HALF, y1e + ny * HALF),
                (x2e + nx * HALF, y2e + ny * HALF),
                (x2e - nx * HALF, y2e - ny * HALF),
                (x1e - nx * HALF, y1e - ny * HALF)):
            ch.Append(pcbnew.VECTOR2I_MM(round(cx, 4), round(cy, 4)))
        ch.SetClosed(True)
        ps.AddOutline(ch)
        chains.append(ch)
    ps.Simplify()
    assert ps.OutlineCount() == 1, \
        f"{net_name} 无铜走廊不是单一连通区: {ps.OutlineCount()} 个 outline"

    ko = pcbnew.ZONE(board)
    ko.SetZoneName(KO_NAMES[net_name])
    ko.SetLayer(pcbnew.F_Cu)
    ko.SetIsRuleArea(True)
    ko.SetDoNotAllowZoneFills(True)       # 禁铺铜——这是挖走廊的本意
    ko.SetDoNotAllowTracks(False)         # 这条走线自己要走在里面
    # 过孔必须禁。走廊只挡铺铜的话，后续 GND 缝合孔仍可能落入走廊，形成阻抗突变。
    ko.SetDoNotAllowVias(True)
    # 焊盘不能禁：走廊两端和π网络交界都压着射频焊盘。
    ko.SetDoNotAllowPads(False)
    ko.SetDoNotAllowFootprints(False)
    ps.thisown = 0                        # ZONE 接管 outline；同时在本进程保活
    ko.SetOutline(ps)
    board.Add(ko)
    return ps, chains


# ZS1 把两段线路物理隔开。KiCad 的一个 zone outline 不能可靠保存两个不相连的岛，
# 必须按网络拆成两个规则区，否则重新载板时可能只剩 IFA_MATCH 一侧。
_corridor_keep = [add_corridor(name, segments_by_net[name]) for name in paths]

board.Save(PCB)

# Remove过对象后不在本进程里迭代Zones；子进程重新载板后再回填，
# 避开pcbnew SWIG代理被Remove连带失效的已知坑。
subprocess.run([
    sys.executable, "-c",
    "import pcbnew; p=%r; b=pcbnew.LoadBoard(p); "
    "pcbnew.ZONE_FILLER(b).Fill(b.Zones()); b.Save(p)" % PCB,
], check=True)

print(f"删旧 {len(old_tracks)} 段走线/{len(old_zones)} 个走廊，"
      f"重布 {sum(len(v)-1 for v in paths.values())} 段50Ω微带")
print(f"  馈电总路径: 等宽 {wide:.3f} + taper {TAPER_LENGTH:.3f} + "
      f"微带 {microstrip:.3f} = {feed_total:.3f}mm")
for name, pts in paths.items():
    print(f"  {name}: {' → '.join(f'({x:.3f},{y:.3f})' for x, y in pts)}  "
          f"走线 {totals[name]:.3f}mm")
print(f"  线宽 {RF_W}mm，全F.Cu无过孔")
print(f"  无铜走廊 {', '.join(KO_NAMES.values())}: "
      f"半宽 {HALF:.3f}mm（两侧各留 {NO_CU}mm 无铜），仅 F.Cu")
