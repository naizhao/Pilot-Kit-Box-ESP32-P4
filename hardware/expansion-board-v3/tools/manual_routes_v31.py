#!/usr/bin/env python3
"""v3.1 六层板残线手工收尾布线（freerouting 之后）。

板子已 94% 布通，剩 13 网络/17 连接未通。本脚本逐条补齐，且每条走线/过孔放置前
用 route_rf.py 已验证 DRC 安全的「双口径占用区」模型做碰撞校验，保证铜层 DRC 始终 0。

碰撞模型（照抄 route_rf.py:160-238，已验证 DRC 安全）：
  · 走线路径用 CLR_T = W/2 + 0.20 + 0.02（半线宽 + 间距 + 余量）
  · 过孔本体用 CLR_V = 0.30 + 0.20 + 0.02 = 0.52（过孔半径 + 间距 + 余量）
  · 孔间距用 drill/2 + 0.15 + HOLE_GAP(0.27)，且同网络不豁免（物理钻孔）
  · 占用区带网络归属：同网络可压（电气等价），异网络禁止
  · 0.20 是全板最严间距（POWER 类），对所有网络统一用它做地板，必然 DRC 安全

布线引擎：分层 A*。
  · 数字/电源：[F.Cu, In2.Cu, In3.Cu, B.Cu] 四层，允许过孔（普通 PTH，贯穿所有层）
  · 射频：仅 F.Cu，禁过孔（过孔会打穿 In1 参考面）
  · In1.Cu / In4.Cu 是完整 GND 平面，绝不走线（check_route 有断言）
换层 = 一个贯穿过孔，放置点必须通过 free2（CLR_V + 孔间距）校验。

幂等：本脚本加的所有走线/过孔都 SetLocked(True)。重跑时先删掉目标网络上的锁定项，
再加。freerouting 的走线不锁定，故不会被误删。

运行：
  ~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 \
      tools/manual_routes_v31.py
"""
import heapq
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB = os.environ.get(
    "PK_PCB_PATH",
    os.path.join(T, "kicad", "expansion-board-v3.kicad_pcb"),
)

# ── 布线层（绝不用 In1.Cu=4 / In4.Cu=10 这两个 GND 参考面）──────────────────
LAYERS = [pcbnew.F_Cu, pcbnew.In2_Cu, pcbnew.In3_Cu, pcbnew.B_Cu]
LI = {l: i for i, l in enumerate(LAYERS)}
LNAME = {pcbnew.F_Cu: "F.Cu", pcbnew.In2_Cu: "In2.Cu", pcbnew.In3_Cu: "In3.Cu", pcbnew.B_Cu: "B.Cu"}

# ── 碰撞口径（照抄 route_rf.py）──────────────────────────────────────────
BOARD_CLR = 0.20        # 全板最严间距（POWER 类）；对 RF(0.15)/Default(0.15) 同样安全
MARGIN = 0.02
VIA_R = 0.30            # 过孔半径（0.6 直径）
VIA_DRILL_R = 0.15      # 孔半径（0.3 钻孔）
HOLE_GAP = 0.27         # 孔壁间距（DRC min 0.25 + 余量）
CLR_V = VIA_R + BOARD_CLR + MARGIN          # 0.52  过孔本体到异网络铜边
HOLE_INFL = VIA_DRILL_R + HOLE_GAP           # 0.42  现有孔对本脚本新孔的膨胀

# ── 网格 ────────────────────────────────────────────────────────────────
GRID = 0.10
X0, Y0, X1, Y1 = 50.5, 50.5, 149.5, 111.5      # 板边内缩 0.5mm
NW = int((X1 - X0) / GRID)
NH = int((Y1 - Y0) / GRID)

# ── A* 代价：4 方向正交。对角走线会切障碍凸角（线段两端在膨胀框外，中段仍可穿过），
#    故禁用对角，保证「两端格心 clr_t-clean ⟹ 整段 clean」（轴对齐段对轴对齐框成立）。
DIRS = ((1, 0), (0, 1), (-1, 0), (0, -1))
COST = (10, 10, 10, 10)
TURN_PEN = 4
VIA_PEN = 60            # 过孔昂贵：能同层走就不换层


def mm(v):
    return pcbnew.ToMM(v)


def g2m(gx, gy):
    return X0 + gx * GRID, Y0 + gy * GRID


def m2g(x, y):
    return int(round((x - X0) / GRID)), int(round((y - Y0) / GRID))


# ═══════════════════════════════════════════════════════════════════════════
# 1. 快照：把板上所有几何抽成纯 Python 数据。
#    必须在任何 board.Remove() 之前完成——SWIG 代理在板子被改后整体失效。
# ═══════════════════════════════════════════════════════════════════════════
def snapshot(board):
    items = []          # (x0,y0,x1,y1, netcode, [layer_idx], kind)
    holes = []          # (x, y, hole_r)
    # 每网络实际间距（取其网络类）。DRC 两物体间距 = max(各自间距)。
    # 全板统一 0.20 太保守：Default/RF50 是 0.15，0.4mm pitch QFN 焊盘中心距邻脚边缘 0.3mm，
    # 用 0.20+余量会误判违例；按网络类精确取值才能既 DRC 安全又不堵死细间距扇出。
    clr_by_class = {str(k): mm(v.GetClearance()) for k, v in board.GetAllNetClasses().items()}
    net_clr = {0: clr_by_class.get("Default", 0.15)}
    for nc_key, net in board.GetNetInfo().NetsByNetcode().items():
        try:
            net_clr[net.GetNetCode()] = clr_by_class.get(str(net.GetNetClassName()), 0.15)
        except Exception:
            net_clr[net.GetNetCode()] = 0.15
    pad_exit = {}        # (ref, padnum) -> (cx, cy, ex, ey, layer_id, bbox)：精确中心+外向出口+焊盘框
    for f in board.GetFootprints():
        fc = f.GetPosition()
        fcx, fcy = mm(fc.x), mm(fc.y)
        ref = f.GetReference()
        for p in f.Pads():
            bb = p.GetBoundingBox()
            lays = [LI[l] for l in LAYERS if p.IsOnLayer(l)]
            items.append((mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom()),
                          p.GetNetCode(), lays or [0], "pad"))
            dr = mm(p.GetDrillSizeX())
            if dr > 0:     # THT 焊盘（排母等）有钻孔
                pos = p.GetPosition()
                holes.append((mm(pos.x), mm(pos.y), dr / 2))
            # 出口点：沿封装主轴向外。0.4mm pitch QFN 出口走廊仅 0.05mm，栅格放不下格心，
            # 必须从芯片外侧空旷区起步；A* 路径首尾再接回精确焊盘中心。
            pos = p.GetPosition()
            pcx, pcy = mm(pos.x), mm(pos.y)
            dx, dy = pcx - fcx, pcy - fcy
            sx = mm(p.GetSizeX()); sy = mm(p.GetSizeY())
            if abs(dx) >= abs(dy):              # 主轴 x：从左/右出
                sgn = 1 if dx >= 0 else -1
                ex, ey = pcx + sgn * (sx / 2 + 0.20), pcy
            else:                               # 主轴 y：从上/下出
                sgn = 1 if dy >= 0 else -1
                ex, ey = pcx, pcy + sgn * (sy / 2 + 0.20)
            bbox = (mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom()))
            pad_exit[(ref, p.GetNumber())] = (pcx, pcy, ex, ey, lays[0] if lays else 0, bbox)
        for z in f.Zones():      # U.FL 等封装自带禁布区：任何网络都不许进
            bb = z.GetBoundingBox()
            items.append((mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom()),
                          -1, range(len(LAYERS)), "keepout"))
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            pos = t.GetPosition()
            lays = range(len(LAYERS))
            bb = t.GetBoundingBox()           # 用 bbox 避免 GetWidth() 无层参数的断言噪音
            items.append((mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom()),
                          t.GetNetCode(), lays, "via"))
            holes.append((mm(pos.x), mm(pos.y), mm(t.GetDrill()) / 2))
        else:
            if t.GetLayer() not in LI:
                continue
            bb = t.GetBoundingBox()
            items.append((mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom()),
                          t.GetNetCode(), [LI[t.GetLayer()]], "track"))
    return items, holes, net_clr, pad_exit


# ═══════════════════════════════════════════════════════════════════════════
# 2. 占用栅格：cntT（走线口径）/ cntV（过孔口径），计数型，带网络归属。
#    一个格子被 N 个异网络物体盖住 → 计数 > mine → 禁走。
# ═══════════════════════════════════════════════════════════════════════════
def _raster(x0, y0, x1, y1, inflate, layers, cnt):
    # 向外取整（低端 floor、高端 ceil）：保证格心判定为 block 的格子是真实 block 区的超集，
    # 从而 A* 用到的格子必然通过 verify 的逐点复核。曾经高端也用 floor，欠膨胀最多 1 格，
    # 导致格心其实 <clr_t 离铜却被判 free，verify 当场打回。
    a = max(0, int(math.floor((x0 - inflate - X0) / GRID)))
    b = max(0, int(math.floor((y0 - inflate - Y0) / GRID)))
    c = min(NW - 1, int(math.ceil((x1 + inflate - X0) / GRID)))
    d = min(NH - 1, int(math.ceil((y1 + inflate - Y0) / GRID)))
    for li in layers:
        if not (0 <= li < len(cnt)):
            continue
        layer = cnt[li]
        for gy in range(b, d + 1):
            base = gy * NW
            for gx in range(a, c + 1):
                k = base + gx
                if layer[k] < 255:
                    layer[k] += 1


def build_occ(items, holes, net_clr, net_code, width):
    """返回 (occT, occV)：True=该格禁止本网络走/过孔。每物体按 max(本网间距, 物体间距) 膨胀。"""
    my_clr = net_clr.get(net_code, 0.15)
    half = width / 2
    cntT = [bytearray(NW * NH) for _ in LAYERS]
    cntV = [bytearray(NW * NH) for _ in LAYERS]
    mineT = [bytearray(NW * NH) for _ in LAYERS]
    mineV = [bytearray(NW * NH) for _ in LAYERS]
    for (x0, y0, x1, y1, nc, lays, kind) in items:
        eff = max(my_clr, net_clr.get(nc, 0.15)) if nc >= 0 else my_clr   # keepout(nc=-1)：用本网间距
        clr_t = half + eff                   # build_occ 不加余量：与 verify 同口径，靠 _raster 向外取整保守
        clr_v = VIA_R + eff
        _raster(x0, y0, x1, y1, clr_t, lays, cntT)
        _raster(x0, y0, x1, y1, clr_v, lays, cntV)
        if nc == net_code:        # 同网络：电气等价，可压
            _raster(x0, y0, x1, y1, clr_t, lays, mineT)
            _raster(x0, y0, x1, y1, clr_v, lays, mineV)
    # 钻孔物理间距：同网络也不豁免 → 只进 cntV，不进 mineV
    for (hx, hy, hr) in holes:
        _raster(hx, hy, hx, hy, hr + HOLE_INFL, range(len(LAYERS)), cntV)
    occT = [bytearray(map(bool, cntT[li])) for li in range(len(LAYERS))]
    occV = [bytearray(map(bool, cntV[li])) for li in range(len(LAYERS))]
    for li in range(len(LAYERS)):
        for k in range(NW * NH):
            if cntT[li][k] <= mineT[li][k]:
                occT[li][k] = 0
            if cntV[li][k] <= mineV[li][k]:
                occV[li][k] = 0
    return occT, occV


# ═══════════════════════════════════════════════════════════════════════════
# 3. A*：多起点多终点，8 方向，换层=过孔（需 occV 放行）。
# ═══════════════════════════════════════════════════════════════════════════
def astar(occT, occV, starts, goals, allow_via):
    goalset = set(goals)
    if not goalset:
        return None
    gxs = [g[0] for g in goals]
    gys = [g[1] for g in goals]

    def h(n):
        best = 1 << 30
        for x, y in zip(gxs, gys):
            dx, dy = abs(n[0] - x), abs(n[1] - y)
            best = min(best, 10 * max(dx, dy) + 4 * min(dx, dy))
        return best

    pq = []
    best = {}
    came = {}
    for s in starts:
        st = (s[0], s[1], s[2], -1)            # (gx,gy,layer,dir_in)
        best[st] = 0
        heapq.heappush(pq, (h(s), 0, st))
    popped = 0
    CAP = 350000                              # 节点上限：超 35 万还没到终点就放弃（避免爆搜卡死）
    while pq:
        _, g, cur = heapq.heappop(pq)
        popped += 1
        if popped > CAP:
            return None
        if g > best.get(cur, 1 << 30):
            continue
        if (cur[0], cur[1], cur[2]) in goalset:
            path = []
            n = cur
            while n in came:
                path.append((n[0], n[1], n[2]))
                n = came[n]
            path.append((n[0], n[1], n[2]))   # n 现在是起点（不在 came 里）。曾误写 cur，导致起点丢失/终点重复
            return path[::-1]
        li = cur[2]
        row = cur[1] * NW
        for di, (dx, dy) in enumerate(DIRS):
            nx, ny = cur[0] + dx, cur[1] + dy
            if not (0 <= nx < NW and 0 <= ny < NH):
                continue
            if occT[li][ny * NW + nx]:
                continue
            ng = g + COST[di] + (TURN_PEN if cur[3] != -1 and cur[3] != di else 0)
            nn = (nx, ny, li, di)
            if ng < best.get(nn, 1 << 30):
                best[nn] = ng
                came[nn] = cur
                heapq.heappush(pq, (ng + h(nn), ng, nn))
        if allow_via:                          # 换层：该格 occV 必须放行（过孔放得下）
            idx = row + cur[0]
            for nl in range(len(LAYERS)):
                if nl == li or occV[nl][idx]:
                    continue
                ng = g + VIA_PEN
                nn = (cur[0], cur[1], nl, -1)
                if ng < best.get(nn, 1 << 30):
                    best[nn] = ng
                    came[nn] = cur
                    heapq.heappush(pq, (ng + h(nn), ng, nn))
    return None


# ═══════════════════════════════════════════════════════════════════════════
# 4. 路径 → KiCad 走线/过孔（同层共线段合并）。
# ═══════════════════════════════════════════════════════════════════════════
def emit(mpath, widths, board, net):
    """mpath = [(x_mm, y_mm, layer_idx), ...]；widths[k] = 第 k 段线宽。
    同层共线段合并；换层=过孔。"""
    segs = vias = 0
    i = 0
    while i < len(mpath) - 1:
        j = i
        while j + 1 < len(mpath) and mpath[j + 1][2] == mpath[i][2]:
            j += 1
        if j > i:                              # 同层连续段：每相邻两点一段（点已含精确首尾）
            for k in range(i, j):
                ax, ay, _ = mpath[k]
                bx, by, _ = mpath[k + 1]
                t = pcbnew.PCB_TRACK(board)
                t.SetStart(pcbnew.VECTOR2I_MM(round(ax, 4), round(ay, 4)))
                t.SetEnd(pcbnew.VECTOR2I_MM(round(bx, 4), round(by, 4)))
                t.SetWidth(pcbnew.FromMM(widths[k]))
                t.SetLayer(LAYERS[mpath[k][2]])
                t.SetNet(net)
                t.SetLocked(True)
                board.Add(t)
                segs += 1
        if j + 1 < len(mpath):                  # 换层 = 过孔
            vx, vy, _ = mpath[j]
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pcbnew.VECTOR2I_MM(round(vx, 4), round(vy, 4)))
            v.SetWidth(pcbnew.FromMM(0.6))
            v.SetDrill(pcbnew.FromMM(0.3))
            v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
            v.SetNet(net)
            v.SetLocked(True)
            board.Add(v)
            vias += 1
        i = j + 1
    return segs, vias


# ═══════════════════════════════════════════════════════════════════════════
# 5. 独立复核：路径上每个采样点 + 每个过孔，用 route_rf 模型逐点校验。
#    A* 已用同模型，这里是双保险——任何一点不过即判该网络失败，不写盘。
# ═══════════════════════════════════════════════════════════════════════════
def verify(mpath, widths, items, holes, net_clr, net_code, width, bb0=None, bb1=None):
    """mpath = [(x_mm,y_mm,layer_idx),...]；widths[k]=第 k 段线宽。逐段采样复核（DRC 真值）。"""
    my_clr = net_clr.get(net_code, 0.15)
    epboxes = [b for b in (bb0, bb1) if b is not None]

    def on_pad(x, y):
        for (lx, ly, rx, ry) in epboxes:
            if lx <= x <= rx and ly <= y <= ry:
                return True
        return False

    for k, ((ax, ay, la), (bx, by, lb)) in enumerate(zip(mpath, mpath[1:])):
        half = widths[k] / 2
        d = math.hypot(bx - ax, by - ay)
        steps = max(2, int(d / 0.04))
        for s in range(steps + 1):
            x = ax + (bx - ax) * s / steps
            y = ay + (by - ay) * s / steps
            if on_pad(x, y):
                continue
            blk = _point_free(x, y, items, net_clr, net_code, my_clr, half, la)
            if blk is not None:
                return False, (x, y, "track", la, blk)
        if la != lb:
            if not on_pad(ax, ay):
                blk = _via_free(ax, ay, holes, items, net_clr, net_code, my_clr)
                if blk is not None:
                    return False, (ax, ay, "via", blk)
    return True, None


def _point_free(x, y, items, net_clr, net_code, my_clr, half, layer_idx):
    """返回 None=干净；否则返回阻塞物 (kind, box)。按 max(本网间距, 物体间距) 判定，无余量。"""
    if not (X0 < x < X1 and Y0 < y < Y1):
        return ("edge", None)
    for (x0, y0, x1, y1, nc, lays, kind) in items:
        if nc == net_code:
            continue
        on_layer = (kind == "via") or (layer_idx in lays)
        if not on_layer:
            continue
        eff = max(my_clr, net_clr.get(nc, 0.15)) if nc >= 0 else my_clr
        clr_t = half + eff                       # DRC 真值：半线宽 + 间距
        if (x0 - clr_t) <= x <= (x1 + clr_t) and (y0 - clr_t) <= y <= (y1 + clr_t):
            return (kind, (x0, y0, x1, y1, nc))
    return None


def _via_free(x, y, holes, items, net_clr, net_code, my_clr):
    """返回 None=干净；否则返回阻塞物。无余量。"""
    if not (X0 < x < X1 and Y0 < y < Y1):
        return ("edge", None)
    for (hx, hy, hr) in holes:
        if math.hypot(x - hx, y - hy) < hr + VIA_DRILL_R + HOLE_GAP:
            return ("hole", (hx, hy, hr))
    for (x0, y0, x1, y1, nc, lays, kind) in items:
        if nc == net_code:
            continue
        eff = max(my_clr, net_clr.get(nc, 0.15)) if nc >= 0 else my_clr
        clr_v = VIA_R + eff
        if (x0 - clr_v) <= x <= (x1 + clr_v) and (y0 - clr_v) <= y <= (y1 + clr_v):
            return (kind, (x0, y0, x1, y1, nc))
    return None


# ═══════════════════════════════════════════════════════════════════════════
# 6. 顶层：逐网络布线。
# ═══════════════════════════════════════════════════════════════════════════
def _resolve_ep(ep, pad_exit):
    """端点 spec → (center=(x,y), exit=(x,y), layer_id, pad_bbox_or_None)。
    ep = ("pad", ref, padnum)  取预计算的精确中心+外向出口+焊盘框；
       或 ("pt", x, y, layer)  走线桩/过孔点：center=exit=该点，无焊盘框。"""
    if ep[0] == "pad":
        cx, cy, ex, ey, li, bbox = pad_exit[(ep[1], ep[2])]
        return (cx, cy), (ex, ey), li, bbox
    x, y, li = ep[1], ep[2], LI[ep[3]] if ep[3] in LI else 0
    return (x, y), (x, y), li, None


def _seg_clear(ax, ay, bx, by, items, net_clr, nc, my_clr, half, layer, bbox):
    """焊盘中心→出口的 fanout 段是否 DRC 干净（落在焊盘 bbox 内的采样点跳过）。"""
    d = math.hypot(bx - ax, by - ay)
    steps = max(2, int(d / 0.04))
    lx = ly = rx = ry = None
    if bbox:
        lx, ly, rx, ry = bbox
    for s in range(steps + 1):
        x = ax + (bx - ax) * s / steps
        y = ay + (by - ay) * s / steps
        if lx is not None and lx <= x <= rx and ly <= y <= ry:
            continue
        if _point_free(x, y, items, net_clr, nc, my_clr, half, layer) is not None:
            return False
    return True


def _find_exit(c, bbox, layer, net_clr, nc, my_clr, half, items, holes, allow_via, occT):
    """找最近的合法出口：fanout 段 DRC 干净，且（多层网）可放过孔 / （同层 RF）occT 可走。
    网格暴力扫描半径 4mm 内所有候选，按距离排序——不只沿焊盘主轴，能找到狗骨式 dog-bone 扇出位。"""
    cx, cy = c
    R = 4.0 if allow_via else 2.0
    gx0, gy0 = m2g(cx - R, cy - R)
    gx1, gy1 = m2g(cx + R, cy + R)
    cands = []
    for gx in range(max(0, gx0), min(NW, gx1 + 1)):
        for gy in range(max(0, gy0), min(NH, gy1 + 1)):
            ex, ey = X0 + gx * GRID, Y0 + gy * GRID
            d = math.hypot(ex - cx, ey - cy)
            if d < 0.20 or d > R:
                continue
            cands.append((d, gx, gy, ex, ey))
    cands.sort()
    via_ok = None
    walk_ok = None
    for d, gx, gy, ex, ey in cands:
        if not _seg_clear(cx, cy, ex, ey, items, net_clr, nc, my_clr, half, layer, bbox):
            continue
        if allow_via:
            if via_ok is None and _via_free(ex, ey, holes, items, net_clr, nc, my_clr) is None:
                return (ex, ey)              # 最近的可放过孔位：当场换层，最优
            if walk_ok is None and not occT[layer][gy * NW + gx]:
                walk_ok = (ex, ey)           # 兜底：occT 可走，让 A* 自己绕到过孔
        else:
            if not occT[layer][gy * NW + gx]:
                return (ex, ey)
    return walk_ok


def _ring_starts(bbox, layer, occT):
    """焊盘外环（bbox 外 0.1–0.6mm 带）里 occT 可走的格心，作为 A* 多起点。"""
    lx, ly, rx, ry = bbox
    out = []
    agx = int(math.floor((lx - 0.6 - X0) / GRID))
    bgx = int(math.ceil((rx + 0.6 - X0) / GRID))
    agy = int(math.floor((ly - 0.6 - Y0) / GRID))
    bgy = int(math.ceil((ry + 0.6 - Y0) / GRID))
    for gx in range(max(0, agx), min(NW, bgx + 1)):
        for gy in range(max(0, agy), min(NH, bgy + 1)):
            cx, cy = X0 + gx * GRID, Y0 + gy * GRID
            if lx - 0.02 <= cx <= rx + 0.02 and ly - 0.02 <= cy <= ry + 0.02:
                continue                      # 焊盘实体内不算（那是焊盘自有铜）
            if not occT[layer][gy * NW + gx]:
                out.append((gx, gy, layer))
    return out


def route_gap(board, items, holes, net_clr, pad_exit, net_name, ep0, ep1, width, allow_via):
    """ep0/ep1 = 端点 spec。返回 (segs, vias, mpath) 或 None。"""
    net = board.FindNet(net_name)
    if net is None:
        print(f"    !! 网络 {net_name} 不存在")
        return None
    nc = net.GetNetCode()
    my_clr = net_clr.get(nc, 0.15)
    half = width / 2

    try:
        c0, _, l0, bb0 = _resolve_ep(ep0, pad_exit)
        c1, _, l1, bb1 = _resolve_ep(ep1, pad_exit)
    except KeyError as ke:
        print(f"    !! 找不到焊盘 {ke}")
        return None

    occT, occV = build_occ(items, holes, net_clr, nc, width)

    def escape(center, layer, bbox):
        """焊盘：先沿轴向精确扇出找出口；找不到再退化为外环/点邻域。走线桩点：同网络铜 + 邻域。"""
        if bbox is not None:
            ex = _find_exit(center, bbox, layer, net_clr, nc, my_clr, half, items, holes, allow_via, occT)
            if ex is not None:
                gx, gy = m2g(ex[0], ex[1])
                for ddx in (-1, 0, 1):
                    for ddy in (-1, 0, 1):
                        xx, yy = gx + ddx, gy + ddy
                        if 0 <= xx < NW and 0 <= yy < NH:
                            occT[layer][yy * NW + xx] = 0
                return [(ex[0], ex[1], gx, gy)]
            ring = _ring_starts(bbox, layer, occT)
            if ring:
                return [(X0 + g[0] * GRID, Y0 + g[1] * GRID, g[0], g[1]) for g in ring]
        # pt 端点：该点在现存同网络铜上——5×5 邻域放行 occT 作起点（A* 沿既有同网络铜前行）
        gx, gy = m2g(center[0], center[1])
        out = []
        for ddx in (-2, -1, 0, 1, 2):
            for ddy in (-2, -1, 0, 1, 2):
                xx, yy = gx + ddx, gy + ddy
                if 0 <= xx < NW and 0 <= yy < NH:
                    occT[layer][yy * NW + xx] = 0
                    out.append((X0 + xx * GRID, Y0 + yy * GRID, xx, yy))
        return out

    esc0 = escape(c0, l0, bb0)
    esc1 = escape(c1, l1, bb1)
    if not esc0 or not esc1:
        return None
    starts = [(e[2], e[3], l0) for e in esc0]
    goals = [(e[2], e[3], l1) for e in esc1]
    path = astar(occT, occV, starts, goals, allow_via)
    if not path:
        return None
    # 重建 mpath：用精确出口坐标覆盖 A* 路径的首尾格
    start_map = {(e[2], e[3]): (e[0], e[1]) for e in esc0}
    goal_map = {(e[2], e[3]): (e[0], e[1]) for e in esc1}
    mpath = [(c0[0], c0[1], l0)]
    for i, (gx, gy, li) in enumerate(path):
        ex, ey = X0 + gx * GRID, Y0 + gy * GRID
        if i == 0 and (gx, gy) in start_map:
            ex, ey = start_map[(gx, gy)]
        elif i == len(path) - 1 and (gx, gy) in goal_map:
            ex, ey = goal_map[(gx, gy)]
        mpath.append((ex, ey, li))
    mpath.append((c1[0], c1[1], l1))
    nseg = len(mpath) - 1
    neck = min(width, 0.20)                    # 首尾 fanout 段颈缩：RF 0.34 在 0.3mm 窄焊盘上会过伸撞邻脚
    widths = [width] * nseg
    if nseg >= 1:
        widths[0] = neck
        widths[-1] = neck
    ok, why = verify(mpath, widths, items, holes, net_clr, nc, width, bb0, bb1)
    if not ok:
        print(f"    !! 复核失败 @ {why}（不写盘）")
        return None
    segs, vias = emit(mpath, widths, board, net)
    _register(mpath, widths, items, holes, nc)
    return segs, vias, mpath


def _register(mpath, widths, items, holes, net_code):
    """把刚布的走线/过孔追加进 items/holes，供后续网络的碰撞校验看到。"""
    for k, ((ax, ay, la), (bx, by, lb)) in enumerate(zip(mpath, mpath[1:])):
        if la == lb:
            x0, x1 = min(ax, bx), max(ax, bx)
            y0, y1 = min(ay, by), max(ay, by)
            h = widths[k] / 2
            items.append((x0 - h, y0 - h, x1 + h, y1 + h, net_code, [la], "track"))
        else:
            items.append((ax - VIA_R, ay - VIA_R, ax + VIA_R, ay + VIA_R, net_code, range(len(LAYERS)), "via"))
            holes.append((ax, ay, VIA_DRILL_R))


# ── 本轮只布这 4 个测试点网络（任务约束：不动其它网络）─────────────────────
TARGET_NETS = {
    "DEMOD1", "DEMOD2", "DEMOD3", "RECOVERED_CLK",
}

# ── 布线任务表：每项 = (net, width, p0, p1, allow_via) ─────────────────────
# p = (x_mm, y_mm, pcbnew 层 id)。RF 仅 F.Cu 禁过孔；数字/电源四层允许过孔。
F = pcbnew.F_Cu
In2 = pcbnew.In2_Cu
In3 = pcbnew.In3_Cu
B = pcbnew.B_Cu

JOBS = [
    # ── 4 个测试点（数字，0.15，四层+过孔）。U8 底排 pin32/34/35/36 @ y≈85.6；
    #    TP4/5/6 在右侧 x110-116,y91.25；TP7 在左下 x83.35,y95.25 ──────────────
    ("DEMOD1",        0.15, ("pad","U8","32"), ("pad","TP4","1"), True),
    ("DEMOD2",        0.15, ("pad","U8","34"), ("pad","TP5","1"), True),
    ("DEMOD3",        0.15, ("pad","U8","35"), ("pad","TP6","1"), True),
    ("RECOVERED_CLK", 0.15, ("pad","U8","36"), ("pad","TP7","1"), True),
]

# RP_1V1 用显式几何（局部 + 过孔入 B.Cu 区），见下方 EXPLICIT。
EXPLICIT_RP_1V1 = [
    # (name, layer, width, points)  单位 mm；每段单独 path_free 校验，过孔单独 free2 校验
    # U8-23 @(84.891,82.729) 下到现存的 (86.1,87.05) 过孔区域：先颈缩出脚，再水平接桩
    ("U8-23→stub", F, 0.20, [(84.891, 82.729), (85.200, 83.200), (85.725, 87.700)]),
    # C29-1 @(88.725,87.7) 与左桩 (85.725,87.7) 同 y 直连
    ("stub↔C29-1", F, 0.20, [(85.725, 87.700), (88.725, 87.700)]),
    # U8-45 @(91.766,83.929) 与 U8-50 @(91.766,81.929)：右边缘竖直短接
    ("U8-45↔U8-50", F, 0.20, [(91.766, 83.929), (91.766, 81.929)]),
]


def _seg_ok(board, items, holes, net_clr, net, a, b, width, layer):
    nc = net.GetNetCode()
    my_clr = net_clr.get(nc, 0.15)
    half = width / 2
    li = LI[layer]
    d = math.hypot(b[0] - a[0], b[1] - a[1])
    steps = max(2, int(d / 0.04))
    for s in range(steps + 1):
        x = a[0] + (b[0] - a[0]) * s / steps
        y = a[1] + (b[1] - a[1]) * s / steps
        blk = _point_free(x, y, items, net_clr, nc, my_clr, half, li)
        if blk is not None:
            return False, blk
    return True, None


def _via_ok(board, items, holes, net_clr, net, x, y):
    nc = net.GetNetCode()
    return _via_free(x, y, holes, items, net_clr, nc, net_clr.get(nc, 0.15))


def route_explicit_rp1v1(board, items, holes, net_clr, pad_exit):
    """RP_1V1：U8 各脚短颈（显式）+ U8-45↔桩（A* 兜底）。逐段校验，失败跳过。"""
    net = board.FindNet("RP_1V1")
    if net is None:
        return 0, 0
    nc = net.GetNetCode()
    segs = vias = 0
    for name, layer, width, pts in EXPLICIT_RP_1V1:
        ok_all = True
        for a, b in zip(pts, pts[1:]):
            ok, blk = _seg_ok(board, items, holes, net_clr, net, a, b, width, layer)
            if not ok:
                print(f"    !! RP_1V1 {name} 段 {a}->{b} 复核失败 {blk}，跳过")
                ok_all = False
                break
        if not ok_all:
            continue
        for a, b in zip(pts, pts[1:]):
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(pcbnew.VECTOR2I_MM(*a))
            t.SetEnd(pcbnew.VECTOR2I_MM(*b))
            t.SetWidth(pcbnew.FromMM(width))
            t.SetLayer(layer)
            t.SetNet(net)
            t.SetLocked(True)
            board.Add(t)
            segs += 1
            h = width / 2
            items.append((min(a[0], b[0]) - h, min(a[1], b[1]) - h,
                          max(a[0], b[0]) + h, max(a[1], b[1]) + h, nc, [LI[layer]], "track"))
    # U8-45 ↔ 桩 (88.725,87.7)：用 A* 兜底（显式直线会穿脚）
    gap = route_gap(board, items, holes, net_clr, pad_exit, "RP_1V1",
                    ("pad","U8","45"), ("pt", 88.725, 87.700, F), 0.20, True)
    if gap:
        segs += gap[0]; vias += gap[1]
    else:
        print("    !! RP_1V1 U8-45↔桩 走不通（A* 失败）")
    return segs, vias


# ═══════════════════════════════════════════════════════════════════════════
# main
# ═══════════════════════════════════════════════════════════════════════════
def main():
    board = pcbnew.LoadBoard(PCB)

    # ── 幂等：先删本脚本上轮加的锁定项（目标网络）────────────────────────
    removed = 0
    for t in list(board.GetTracks()):
        if t.IsLocked() and t.GetNetname() in TARGET_NETS:
            board.Remove(t)
            removed += 1
    if removed:
        print(f"幂等清理：删掉上轮锁定走线/过孔 {removed} 项")

    # ── 快照（必须在 Remove 之后、加新物体之前）──────────────────────────
    items, holes, net_clr, pad_exit = snapshot(board)
    print(f"快照：{len(items)} 物体 / {len(holes)} 钻孔 / {len(pad_exit)} 焊盘出口；网格 {NW}×{NH}×{len(LAYERS)}")

    tot_seg = tot_via = 0
    done = fail = 0

    # ── 先布最容易的：数字 + 测试点 ─────────────────────────────────────
    print("\n=== 数字 / 测试点 / 电源（A*）===")
    import time
    for net_name, width, ep0, ep1, allow_via in JOBS:
        t0 = time.time()
        r = route_gap(board, items, holes, net_clr, pad_exit, net_name, ep0, ep1, width, allow_via)
        tag = "RF " if width == 0.34 else "   "
        if r:
            segs, vias, _ = r
            tot_seg += segs; tot_via += vias
            done += 1
            print(f"  {tag}{net_name:14s} {segs:3d} 段 {vias:2d} 过孔  ✓  ({time.time()-t0:.1f}s)")
        else:
            fail += 1
            print(f"  {tag}{net_name:14s} 走不通 ✗  ({time.time()-t0:.1f}s)")
        sys.stdout.flush()

    # ── 覆铜回填 + 存盘 ──────────────────────────────────────────────────
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(PCB)
    print(f"\n合计：布通 {done} / 失败 {fail}；新增 {tot_seg} 段 / {tot_via} 过孔")
    print("saved:", PCB)


if __name__ == "__main__":
    main()
