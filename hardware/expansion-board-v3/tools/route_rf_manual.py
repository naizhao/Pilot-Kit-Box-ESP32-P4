#!/usr/bin/env python3
"""【历史脚本，禁止直接用于当前六层RF】射频残线逐条精修。

脚本默认0.34mm是四层旧值；当前JLC06161H-3313 RF50为0.15mm。保留只为追溯。

## 为什么另起一个

`route_fix.py` 用 0.15mm 全板栅格，对数字线够用，但射频这几条卡在 QFN 引脚缝里：
U10 的 0.5mm pitch 下，邻居焊盘边缘到中心线只有 0.375mm，而 0.34mm 射频线需要
0.17+0.15=0.32mm——余量 0.055mm，**0.15mm 的栅格表达不了**，于是全被判成走不通。

这里换两处：
  · 栅格 0.05mm（细 3 倍），能表达 0.05mm 级的余量；
  · 只在"缺口两端点的包围盒 + margin"这一小块里搜，格数才压得住
    （全板 0.05 栅格是 240 万格/层，局部通常只有 5-15 万格）。

射频硬约束（与 check_route.py 的四条断言一致）：
  · 只走 F.Cu —— In1 是它的 50Ω 参考面
  · 零过孔 —— 打孔就在参考面上开洞
  · 历史线宽0.34mm（仅四层JLC04161H-7628；当前六层禁用）
  · 45° 拐角，不走直角（直角有阻抗突变；也是 freerouting 那个
    PolylineTrace.combine 爆栈的诱因，见 krt_route.sh）

落盘前一律走 route_fix.stub_clear 的连续几何复核，不过就不落——宁可这条不布，
也不往板里塞 DRC 违例。

用法：
  route_rf_manual.py plan            全部 RF 残线，只算不写
  route_rf_manual.py apply           落盘
  route_rf_manual.py apply SUBG_RFN  只做指定网络
"""
import heapq
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")   # 中间产物落在工程内，可核对、不会被系统清掉
os.makedirs(BUILD, exist_ok=True)
sys.path.insert(0, os.path.join(T, "tools"))

MODE = sys.argv[1] if len(sys.argv) > 1 else "plan"
ONLY = sys.argv[2] if len(sys.argv) > 2 else None
DRC = os.environ.get("PK_DRC", os.path.join(BUILD, "drc.json"))

os.environ.setdefault("PK_DRC_ARG", DRC)
sys.argv = ["route_fix", "plan", DRC]
import route_fix as R                                             # noqa: E402

GRID = 0.05
MARGIN = float(os.environ.get("PK_RF_MARGIN", "7.0"))   # 局部搜索区外扩量
# 出焊盘段要 neck-down。0.5mm pitch 的 QFN 引脚焊盘本身只有 0.25mm 宽，从它引出
# 0.34mm 的线本来就不合理：实测 SUBG_RFN 到邻居 SUBG_RFP 走线的余量只剩 +0.010mm，
# 这种量级手工画也画不进去。收到 0.2mm 后同一处余量变成 +0.080mm。
# 代价：0.2mm 段阻抗约 70-80Ω 而非 50Ω，但只有出口这一两毫米，
# 在 1090MHz（介质中 λ≈100mm）不到 λ/50，可接受——这也是 QFN 射频出线的常规做法。
W = float(os.environ.get("PK_RF_W", "0.15"))
# 允许换层的层索引（route_fix 的 LI 口径）：F.Cu / In2 / B.Cu。
# In3 是 3V3_DIG 电源平面，不能走信号（走了就把平面切碎）。
# 射频网**不用**这个——它必须留在 F.Cu 单层、零过孔，In1 是它的参考面。
MULTI_LI = (0, 1, 3)
VIA_COST_G = 300
# RF50 净空。0.15 是我们自己设的；嘉立创 6 层标准档能到 0.09。
# 收净空对射频是有代价的（串扰），但在 QFN 引脚出口那一两毫米是所有射频设计
# 都得做的妥协——芯片 pitch 就 0.5mm，没有别的选择。
CLR = float(os.environ.get("PK_RF_CLR", "0.15"))
TURN = 4                         # 拐弯代价（射频要少拐弯）
DIRS = ((1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1))
COST = (10, 14, 10, 14, 10, 14, 10, 14)


def local_route(net, verbose=True, single_layer=True):
    nc = R.board.FindNet(net).GetNetCode()
    blks = R.blocks_of(nc)
    if len(blks) < 2:
        return None, "已连通"

    # 取"最近的两块"——射频链是一条线，缺口就在相邻两段之间
    best = None
    for i in range(len(blks)):
        for j in range(i + 1, len(blks)):
            ca = {c for c in R.block_cells(blks[i]) if c[2] == 0}
            cb = {c for c in R.block_cells(blks[j]) if c[2] == 0}
            if not ca or not cb:
                continue
            for a in ca:
                for b in cb:
                    d = (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2
                    if best is None or d < best[0]:
                        best = (d, i, j, a, b)
    if best is None:
        return None, "两端都不在 F.Cu 上"
    _, i, j, ga, gb = best
    ax, ay = R.g2m(ga[0], ga[1])
    bx, by = R.g2m(gb[0], gb[1])

    # 局部区域
    x0 = min(ax, bx) - MARGIN
    x1 = max(ax, bx) + MARGIN
    y0 = min(ay, by) - MARGIN
    y1 = max(ay, by) + MARGIN
    nw = int((x1 - x0) / GRID) + 1
    nh = int((y1 - y0) / GRID) + 1

    def m2g(x, y):
        return int(round((x - x0) / GRID)), int(round((y - y0) / GRID))

    def g2m(gx, gy):
        return x0 + gx * GRID, y0 + gy * GRID

    # 局部占用图。单层(射频)时只建 F.Cu；多层时每个可布线层各一张。
    pads, trks, vias = R.snap_obstacles()
    LAYERS = (0,) if single_layer else MULTI_LI
    occ = {li: bytearray(nw * nh) for li in LAYERS}
    # 过孔专用占用图：通孔盘(VIA_R=0.2)比走线半宽(0.075)大得多，且穿**全部**层，
    # 所以要单独一张、按盘半径膨胀、把所有层的障碍合并进去。
    # 拿走线的图去判能不能打孔，就会挑中盘放不下的位置——实测落盘后
    # 12 条 clearance + 1 条 shorting + 3 条 hole_clearance。
    occv = bytearray(nw * nh)
    half = W / 2
    VHALF = R.VIA_R
    VCLR = max(CLR, 0.2)

    def stamp_via_rect(box, clr_o):
        inf = VHALF + max(clr_o, VCLR)
        a, b = m2g(box[0] - inf, box[1] - inf)
        c, d = m2g(box[2] + inf, box[3] + inf)
        for gy in range(max(0, b), min(nh, d + 1)):
            row = gy * nw
            for gx in range(max(0, a), min(nw, c + 1)):
                occv[row + gx] = 1

    def stamp_via_seg(p, q, oh, clr_o):
        r = VHALF + oh + max(clr_o, VCLR)
        L = math.hypot(q[0] - p[0], q[1] - p[1])
        n = max(1, int(L / (GRID * 0.5)))
        rr = r * r
        for k in range(n + 1):
            t = k / n
            cx, cy = p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t
            a, b = m2g(cx - r, cy - r)
            c, d = m2g(cx + r, cy + r)
            for gy in range(max(0, b), min(nh, d + 1)):
                wy = y0 + gy * GRID
                row = gy * nw
                for gx in range(max(0, a), min(nw, c + 1)):
                    wx = x0 + gx * GRID
                    if (wx - cx) ** 2 + (wy - cy) ** 2 <= rr:
                        occv[row + gx] = 1

    def stamp_rect(box, clr_o, lis=None):
        inf = half + max(clr_o, CLR)
        a, b = m2g(box[0] - inf, box[1] - inf)
        c, d = m2g(box[2] + inf, box[3] + inf)
        for li in (lis if lis is not None else LAYERS):
            if li not in occ:
                continue
            o = occ[li]
            for gy in range(max(0, b), min(nh, d + 1)):
                row = gy * nw
                for gx in range(max(0, a), min(nw, c + 1)):
                    o[row + gx] = 1

    def stamp_seg(p, q, oh, clr_o, lis=None):
        r = half + oh + max(clr_o, CLR)
        L = math.hypot(q[0] - p[0], q[1] - p[1])
        n = max(1, int(L / (GRID * 0.5)))
        for k in range(n + 1):
            t = k / n
            cx, cy = p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t
            a, b = m2g(cx - r, cy - r)
            c, d = m2g(cx + r, cy + r)
            rr = r * r
            for li in (lis if lis is not None else LAYERS):
                if li not in occ:
                    continue
                o = occ[li]
                for gy in range(max(0, b), min(nh, d + 1)):
                    wy = y0 + gy * GRID
                    row = gy * nw
                    for gx in range(max(0, a), min(nw, c + 1)):
                        wx = x0 + gx * GRID
                        if (wx - cx) ** 2 + (wy - cy) ** 2 <= rr:
                            o[row + gx] = 1

    for onc, ols, box, oclr in pads:
        if onc == nc or not (set(ols) & set(LAYERS)):
            continue
        if box[2] < x0 or box[0] > x1 or box[3] < y0 or box[1] > y1:
            continue
        stamp_rect(box, oclr, [li for li in ols if li in occ])
        stamp_via_rect(box, oclr)
    for onc, oli, g, oh, oclr in trks:
        if onc == nc or oli not in occ:
            continue
        if max(g[0], g[2]) < x0 or min(g[0], g[2]) > x1 or \
           max(g[1], g[3]) < y0 or min(g[1], g[3]) > y1:
            continue
        stamp_seg((g[0], g[1]), (g[2], g[3]), oh, oclr, [oli])
        stamp_via_seg((g[0], g[1]), (g[2], g[3]), oh, oclr)
    # PK_RF_FREE_GND=1：把 GND 缝合过孔当"可让路"障碍先排除，用于判定
    # "射频布不通到底是不是 GND 过孔墙造成的"。GND 缝合过孔全板有 183+58 个，
    # 局部少几个不影响接地完整性，但要真删得先确认它确实是瓶颈。
    FREE_GND = os.environ.get("PK_RF_FREE_GND") == "1"
    # U.FL 连接器自带禁布区：谁都不许走。snap_obstacles() 不返回这些
    # （route_fix 只在建全局占用图时处理，见 route_fix.py:216），漏掉的话射频线会
    # 直接从连接器身上压过去——实测 4 条 items_not_allowed + 3 条 solder_mask_bridge，
    # 全在 J2/J6/J8 三个天线口上。
    for _fp in R.board.GetFootprints():
        for _z in _fp.Zones():
            _bb = _z.GetBoundingBox()
            _box = (pcbnew.ToMM(_bb.GetLeft()), pcbnew.ToMM(_bb.GetTop()),
                    pcbnew.ToMM(_bb.GetRight()), pcbnew.ToMM(_bb.GetBottom()))
            if _box[2] < x0 or _box[0] > x1 or _box[3] < y0 or _box[1] > y1:
                continue
            stamp_rect(_box, 0.20)

    for onc, p, oclr in vias:                 # 过孔穿全层，F.Cu 上也有盘
        if onc == nc:
            continue
        if FREE_GND and R.NETNAME.get(onc) == "GND":
            continue
        if p[0] < x0 - 1 or p[0] > x1 + 1 or p[1] < y0 - 1 or p[1] > y1 + 1:
            continue
        stamp_seg(p, p, R.VIA_R, oclr, list(occ.keys()))
        stamp_via_seg(p, p, R.VIA_R, oclr)

    # 起终点集：本网络在局部区内的 F.Cu 铜
    def own_cells(blk):
        out = set()
        for kind, g, ls in blk:
            if kind == "pad":
                for li in ls:
                    if li not in occ:
                        continue
                    a, b = m2g(g[0], g[1])
                    c, d = m2g(g[2], g[3])
                    for gy in range(max(0, b), min(nh, d + 1)):
                        for gx in range(max(0, a), min(nw, c + 1)):
                            out.add((gx, gy, li))
            elif kind == "via":
                gx, gy = m2g(g[0], g[1])
                for li in occ:
                    if 0 <= gx < nw and 0 <= gy < nh:
                        out.add((gx, gy, li))
            elif kind == "trk":
                li = ls[0]
                if li not in occ:
                    continue
                n = max(2, int(math.hypot(g[2] - g[0], g[3] - g[1]) / (GRID * 0.5)))
                for k in range(n + 1):
                    t = k / n
                    gx, gy = m2g(g[0] + (g[2] - g[0]) * t, g[1] + (g[3] - g[1]) * t)
                    if 0 <= gx < nw and 0 <= gy < nh:
                        out.add((gx, gy, li))
        return out

    S, G = own_cells(blks[i]), own_cells(blks[j])
    if not S or not G:
        return None, "端点块不在局部区内"
    for gx, gy, li in list(S) + list(G):       # 自己的铜放行
        if 0 <= gx < nw and 0 <= gy < nh and li in occ:
            occ[li][gy * nw + gx] = 0

    gxs = [g[0] for g in G]
    gys = [g[1] for g in G]
    tx0, tx1, ty0, ty1 = min(gxs), max(gxs), min(gys), max(gys)

    def h(x, y):
        dx = tx0 - x if x < tx0 else (x - tx1 if x > tx1 else 0)
        dy = ty0 - y if y < ty0 else (y - ty1 if y > ty1 else 0)
        return 10 * max(dx, dy) + 4 * min(dx, dy)

    came, bestg, pq = {}, {}, []
    for s in S:
        st = (s[0], s[1], s[2], -1)
        bestg[st] = 0
        heapq.heappush(pq, (h(s[0], s[1]), 0, st))
    goal = set(G)
    found = None
    pops = 0
    while pq:
        _, g, cur = heapq.heappop(pq)
        if g > bestg.get(cur, 1 << 30):
            continue
        pops += 1
        if (cur[0], cur[1], cur[2]) in goal:
            found = cur
            break
        for di, (dx, dy) in enumerate(DIRS):
            nx, ny = cur[0] + dx, cur[1] + dy
            if not (0 <= nx < nw and 0 <= ny < nh) or occ[cur[2]][ny * nw + nx]:
                continue
            ng = g + COST[di] + (TURN if cur[3] not in (-1, di) else 0)
            nn = (nx, ny, cur[2], di)
            if ng < bestg.get(nn, 1 << 30):
                bestg[nn] = ng
                came[nn] = cur
                heapq.heappush(pq, (ng + h(nx, ny), ng, nn))
        # 换层：非射频网才允许。通孔穿全层，每层同位置都要空，且孔间距要够。
        if len(occ) > 1:
            wx, wy = g2m(cur[0], cur[1])
            if not occv[cur[1] * nw + cur[0]] and R.hole_ok(wx, wy):
                for nli in occ:
                    if nli == cur[2]:
                        continue
                    nn = (cur[0], cur[1], nli, -1)
                    ng = g + VIA_COST_G
                    if ng < bestg.get(nn, 1 << 30):
                        bestg[nn] = ng
                        came[nn] = cur
                        heapq.heappush(pq, (ng + h(cur[0], cur[1]), ng, nn))
    if not found:
        return None, f"局部无解（{nw}×{nh} 格，扩展 {pops} 节点）"

    path = []
    n = found
    while n in came:
        path.append((n[0], n[1], n[2]))
        n = came[n]
    path.append((n[0], n[1], n[2]))
    path.reverse()

    # 合并同方向格点成线段
    segs, vias_out = [], []
    k = 0
    while k < len(path) - 1:
        if path[k + 1][2] != path[k][2]:          # 换层 = 打过孔
            vias_out.append(g2m(path[k][0], path[k][1]))
            k += 1
            continue
        dx = path[k + 1][0] - path[k][0]
        dy = path[k + 1][1] - path[k][1]
        m = k + 1
        while (m + 1 < len(path) and path[m + 1][2] == path[k][2]
               and (path[m + 1][0] - path[m][0],
                    path[m + 1][1] - path[m][1]) == (dx, dy)):
            m += 1
        segs.append((g2m(path[k][0], path[k][1]),
                     g2m(path[m][0], path[m][1]), path[k][2]))
        k = m
    return (segs, nw, nh, pops, vias_out), None


def main():
    global W
    R.build()
    R.NETNAME.update({n.GetNetCode(): n.GetNetname()
                      for n in R.board.GetNetsByNetcode().values()})
    # ONLY 可以点名**任意**网络，不限射频。0.05mm 栅格对数字线同样有用：
    # route_fix 的 0.15mm 栅格抓不住 0.07~0.23mm 的窄窗口，实测 SUBG_X48N /
    # I2C_SDA / DEMOD3 的挡路余量都是正的（+0.227/+0.114/+0.067mm），
    # 几何上过得去，只是粗栅格的格点正好落在障碍上。
    if ONLY:
        nets = [n for n in ONLY.split(",")
                if R.board.FindNet(n)
                and len(R.blocks_of(R.board.FindNet(n).GetNetCode())) > 1]
    else:
        nets = [n for n in R.RF if R.board.FindNet(n) and len(R.blocks_of(
            R.board.FindNet(n).GetNetCode())) > 1]
    print(f"射频残线 {len(nets)} 条: {nets}\n")

    done = []
    _W0 = W
    # 一条射频网络常有 3 块以上铜（芯片脚 + 中间元件 + 天线口），local_route 一次
    # 只接最近的一对，所以要循环到这条网络真正连通为止。漏了这层循环的话，板上
    # 看着"27 条全布通"，实际每条都还剩缺口——实测未连通反而从 17 升到 37。
    for net in sorted(nets):
        nc = R.board.FindNet(net).GetNetCode()
        # 非射频网按它自己的类线宽走（RF50 才是 0.34，Default 只有 0.15）
        W = _W0 if net in R.RF else R.PROFILE.get(R.net_class(net),
                                                  R.PROFILE["Default"])[0]
        for _round in range(8):
            if len(R.blocks_of(nc)) < 2:
                break
            res, err = local_route(net, single_layer=(net in R.RF))
            if not res:
                if _round == 0:
                    print(f"  {net:14s} ❌ {err}")
                break
            segs, nw, nh, pops, vias_out = res
        # 连续几何复核（栅格只保证格点安全，线段中点可能更近）
            bad = None
            for a, b, li in segs:
                ok, mg = R.stub_clear(li, a[0], a[1], b[0], b[1], W / 2, nc, CLR)
                if not ok:
                    bad = mg
                    break
            # ⚠️ 新打的过孔也必须复核，只验走线是不够的——漏了这一步落盘后直接
            # 12 条 clearance + 1 条 shorting + 3 条 hole_clearance。
            # 通孔穿**全部**层，所以每一层都要单独验盘的净空；孔间距另算。
            if bad is None:
                for vx, vy in vias_out:
                    if not R.hole_ok(vx, vy):
                        bad = -9.999
                        break
                    for _li in range(R.NL):
                        ok, mg = R.stub_clear(_li, vx, vy, vx, vy,
                                              R.VIA_R, nc, max(CLR, 0.2))
                        if not ok:
                            bad = mg
                            break
                    if bad is not None:
                        break
            if bad is not None:
                print(f"  {net:14s} ⚠️  几何复核不过（余量 {bad:+.3f}mm），弃用")
                break
            L = sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b, _ in segs)
            print(f"  {net:14s} ✅ 第{_round+1}段 {len(segs)} 折 / {L:.2f}mm"
                  f" / {len(vias_out)} 过孔 （{nw}×{nh} 格，{pops} 节点）")
            done.append((net, nc, segs))
            # 立刻挂到 board 上，下一轮的 blocks_of / snap_obstacles 才看得见它，
            # 否则会对着同一个缺口反复布同一条线
            _n = R.board.FindNet(net)
            for a, b, li in segs:
                t = pcbnew.PCB_TRACK(R.board)
                t.SetStart(pcbnew.VECTOR2I_MM(round(a[0], 4), round(a[1], 4)))
                t.SetEnd(pcbnew.VECTOR2I_MM(round(b[0], 4), round(b[1], 4)))
                t.SetWidth(pcbnew.FromMM(W))
                t.SetLayer(R.LAYERS_ALL[li])
                t.SetNet(_n)
                R.board.Add(t)
            for vx, vy in vias_out:
                v = pcbnew.PCB_VIA(R.board)
                v.SetPosition(pcbnew.VECTOR2I_MM(round(vx, 4), round(vy, 4)))
                v.SetWidth(pcbnew.FromMM(R.VIA_R * 2))
                v.SetDrill(pcbnew.FromMM(R.VIA_HOLE_R * 2))
                v.SetNet(_n)
                R.board.Add(v)
            R.OBST = None                      # 清障碍快照缓存

    if MODE == "apply" and done:
        # 走线在循环里已经挂上 board 了，这里只负责重灌覆铜 + 落盘
        pcbnew.ZONE_FILLER(R.board).Fill(R.board.Zones())
        R.board.Save(R.PCB)
        print(f"\n落盘 {len(done)} 条 → {R.PCB}")
    else:
        print("\n（plan 模式，未写盘）" if MODE != "apply" else "\n没有可落盘的")


if __name__ == "__main__":
    main()
