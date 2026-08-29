#!/usr/bin/env python3
"""【历史脚本，禁止直接用于当前六层RF】确定性 A* 迷宫布线器。

文件中曾保留0.34mm四层旧口径；当前JLC06161H-3313 RF50为0.15mm。保留只为追溯。

## 为什么需要它

freerouting 有两个本质问题，调参数解决不了：

1. **它不知道哪根线金贵**。实测它把 `SW1_J1/J2/J3`（射频开关三个口）和 `LNA1_IN`
   甩到 In2 内层——In2 夹在两个平面之间没有紧邻参考面，射频走那里阻抗完全失控。
2. **收敛到边界后就在噪声里打转**。三个走线层跑满仍剩 16 条，多跑几轮只是换个随机结果。

本脚本对指定网络做确定性布线：给定层、给定线宽、避开所有已有铜箔，A* 求最短路径。
射频段布完后在 DSN 里标成 protected，freerouting 不会再动它们。

## 网格与代价

- 栅格 0.15mm：0.34mm 线宽 + 0.2mm 间距需要 0.37mm 半净空，0.15 栅格足够表达
- 障碍膨胀 = 线宽/2 + 间距 + 余量；**同网络的铜箔不算障碍**
- 代价：直行 1、拐弯 +2（少拐弯=射频更短更直）、换层 +40（过孔昂贵，射频禁用）

运行：
  route_maze.py rf      只布射频段（F.Cu 单层，禁过孔）
  route_maze.py rest    布剩余未连通的（三层，允许过孔）
"""
import heapq
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB = os.path.join(T, "kicad", "expansion-board-v3.kicad_pcb")

GRID = 0.15                       # 栅格 mm
X0, Y0, X1, Y1 = 50.0, 50.0, 150.0, 112.0
EDGE = 0.5                        # 铜到板边
CLR = 0.20                        # DRC 间距（POWER 网络类口径，取最严）

# 历史注释：0.34mm仅适用于四层旧叠层；当前六层严禁直接运行。
RF_NETS = [
    "ANT1090_IFA", "ANT1090_EXT", "SW1_J1", "SW1_J2", "SW1_J3",
    "LNA1_IN", "LNA1_OUT", "SAW1_IN", "SAW1_OUT",
    "LNA2_IN", "LNA2_OUT", "SAW2_IN", "SAW2_OUT",
    "DET_IN", "DET_INLO",
    "SUBG_RFP", "SUBG_RFN", "SUBG_RXTX", "SUBG_N3", "SUBG_N4", "SUBG_N5", "ANT_978",
    "ANT_GNSS_INT", "ANT_GNSS_EXT", "GNSS_RF_IN",
]
# ⚠️ 0.34 是**四层板** JLC04161H-7628 的 50Ω 线宽；本板是 6 层 JLC06161H-3313，
# L1-L2 介质只有 0.0994mm，0.34mm 宽实测反算只有 31.82Ω。见 IFA_ANTENNA.md §3.2。
RF_W = 0.15

_MODE = sys.argv[1] if len(sys.argv) > 1 else "rf"

# ⚠️ 删除走线必须在**独立进程**里做。pcbnew 的 SWIG 运行时一旦调用过 board.Remove()，
#    本进程内所有 board/footprint/pad 代理都会退化成裸 SwigPyObject，
#    连重新 LoadBoard() 也救不回来（实测 GetFootprints() 抛 AttributeError）。
#    所以 `route_maze.py clean-rf` 单独跑一趟，再跑 `route_maze.py rf`。
if _MODE == "clean-rf":
    _b = pcbnew.LoadBoard(PCB)
    _codes = {_b.FindNet(n).GetNetCode() for n in RF_NETS if _b.FindNet(n)}
    _n = 0
    for _t in list(_b.GetTracks()):
        if _t.GetNetCode() in _codes:
            _b.Remove(_t); _n += 1
    _b.Save(PCB)
    print(f"清掉射频旧走线/过孔 {_n} 项（含被甩到内层的部分）")
    sys.exit(0)

board = pcbnew.LoadBoard(PCB)
NW = int((X1 - X0 - 2 * EDGE) / GRID)
NH = int((Y1 - Y0 - 2 * EDGE) / GRID)
LAYERS_ALL = [pcbnew.F_Cu, pcbnew.In2_Cu, pcbnew.In3_Cu, pcbnew.B_Cu]   # In1/In4 是完整地平面，绝不使用
LI = {l: i for i, l in enumerate(LAYERS_ALL)}


def g2m(gx, gy):
    return X0 + EDGE + gx * GRID, Y0 + EDGE + gy * GRID


def m2g(x, y):
    return int(round((x - X0 - EDGE) / GRID)), int(round((y - Y0 - EDGE) / GRID))


_CNT = {}          # width -> 全板覆盖计数（3 层 × NW*NH 的 bytearray）


def _rects(net_code=None):
    """产出障碍矩形 (x0,y0,x1,y1,layers)。net_code 给定时只产出该网络的。"""
    for pd in PADS_SNAP:
        if pd["layers"] and (net_code is None or pd["net"] == net_code):
            yield (*pd["box"], pd["layers"])
    if net_code is None:
        for k in KEEPOUTS:                      # U.FL 等自带禁布区，谁都不许走
            yield (*k, range(len(LAYERS_ALL)))
    for t in board.GetTracks():
        if net_code is not None and t.GetNetCode() != net_code:
            continue
        if isinstance(t, pcbnew.PCB_VIA):
            ls = range(len(LAYERS_ALL))
        elif t.GetLayer() in LI:
            ls = [LI[t.GetLayer()]]
        else:
            continue
        bb = t.GetBoundingBox()
        yield (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
               pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom()), ls)


def _cells(rect, inflate):
    x0, y0, x1, y1, layers = rect
    a, b = m2g(x0 - inflate, y0 - inflate)
    c, d = m2g(x1 + inflate, y1 + inflate)
    for li in layers:
        for gy in range(max(0, b), min(NH, d + 1)):
            row = gy * NW
            for gx in range(max(0, a), min(NW, c + 1)):
                yield li, row + gx


def build_occ(net_code, width):
    """占用栅格：True=不可走。只被本网络盖住的格子放行（电气等价）。

    两个坑都踩过：
      · 每条网络重刷一遍全板（1403 段 × 3 层 × 26.8 万格）——一条要几分钟；
      · 预存"每个网络占哪些格子"——GND 覆盖全板，光索引就 965MB。
    现在：全板覆盖**计数**只算一次并缓存；当前网络的格子临时算（它自己的铜就那么几段）。
    用计数而不是布尔，因为一个格子可能同时压着本网络和别人的铜，那种不能放行。
    """
    inflate = width / 2 + CLR
    if width not in _CNT:
        cnt = [bytearray(NW * NH) for _ in LAYERS_ALL]
        for r in _rects():
            for li, k in _cells(r, inflate):
                if cnt[li][k] < 255:
                    cnt[li][k] += 1
        _CNT[width] = cnt
    cnt = _CNT[width]

    mine = {}
    for r in _rects(net_code):
        for li, k in _cells(r, inflate):
            mine[(li, k)] = mine.get((li, k), 0) + 1

    occ = [bytearray(map(bool, c)) for c in cnt]
    for (li, k), n in mine.items():
        if cnt[li][k] <= n:                     # 除了我没别人盖 → 可以走
            occ[li][k] = 0
    return occ


# ---- 过孔专用碰撞模型（修 codex 记录的 route_maze 过孔缺陷）----
# 原本层切换（过孔）用 track 占用区（inflate=width/2+CLR≈0.33）校验，但过孔盘 0.6mm
# 需 inflate 0.3(半径)+0.2(间距)=0.5；且完全不查 hole_to_hole。结果新过孔贴着邻铜/邻孔，
# 引入大量 clearance/shorting/hole_clearance 违例。这里加一套过孔专用占用区 + 孔间距表。
VIA_INFLATE = 0.30 + CLR + 0.02                 # 过孔半径 + 间距 + 余量（与 route_rf CLR_V 一致）
NEW_VIA_HOLE_R = 0.15                           # 新过孔钻半径（0.3 drill/2）
HOLE_GAP_MIN = 0.25                             # 板规 min_hole_to_hole
_occ_via_cache = {}


def build_occ_via(net_code):
    """过孔专用占用栅格：铜按 VIA_INFLATE 膨胀。同网络放行。"""
    if net_code in _occ_via_cache:
        return _occ_via_cache[net_code]
    cnt = [bytearray(NW * NH) for _ in LAYERS_ALL]
    for r in _rects():
        for li, k in _cells(r, VIA_INFLATE):
            if cnt[li][k] < 255:
                cnt[li][k] += 1
    mine = {}
    for r in _rects(net_code):
        for li, k in _cells(r, VIA_INFLATE):
            mine[(li, k)] = mine.get((li, k), 0) + 1
    occ = [bytearray(map(bool, c)) for c in cnt]
    for (li, k), n in mine.items():
        if cnt[li][k] <= n:
            occ[li][k] = 0
    _occ_via_cache[net_code] = occ
    return occ


# 钻孔表（过孔 + PTH 焊盘），用于 hole_to_hole 检查
HOLES = []


def _snap_holes():
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            p = t.GetPosition()
            HOLES.append((pcbnew.ToMM(p.x), pcbnew.ToMM(p.y), pcbnew.ToMM(t.GetDrill()) / 2))
    for f in board.GetFootprints():
        for p in f.Pads():
            dr = pcbnew.ToMM(p.GetDrillSizeX())
            if dr > 0:
                pp = p.GetPosition()
                HOLES.append((pcbnew.ToMM(pp.x), pcbnew.ToMM(pp.y), dr / 2))


_snap_holes()


def hole_ok(x, y):
    """候选过孔位置(mm)的孔到所有现有孔间距是否 ≥ 板规。"""
    for hx, hy, hr in HOLES:
        if math.hypot(x - hx, y - hy) < hr + NEW_VIA_HOLE_R + HOLE_GAP_MIN:
            return False
    return True


# 8 方向：射频走 45° 斜角是标准做法（直角拐弯有阻抗突变），
# 同时大幅减少段数——纯直角栅格路径拐点太密，实测把 freerouting 的
# PolylineTrace.combine 递归撑爆（StackOverflowError，-Xss256m 也救不回来）。
TURN, VIA = 3, 40
DIRS = ((1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1))
COST = (10, 14, 10, 14, 10, 14, 10, 14)    # 斜向 √2≈1.4，用整数 ×10 避免浮点


came = {}


def astar(occ, starts, goals, allow_via, occ_via=None):
    """多起点多终点 A*。starts/goals 是 (gx, gy, li) 集合。
    occ_via 给了则层切换（过孔）按过孔占用区 + 孔间距校验，避免过孔贴邻铜/邻孔。"""
    goalset = set(goals)
    if not goalset:
        return None
    gxs = [g[0] for g in goals]; gys = [g[1] for g in goals]

    def h(n):
        # 8 方向的可采纳启发式：对角距离 × 10
        best = 1 << 30
        for x, y in zip(gxs, gys):
            dx, dy = abs(n[0] - x), abs(n[1] - y)
            best = min(best, 10 * max(dx, dy) + 4 * min(dx, dy))
        return best

    pq = []
    best = {}
    for s in starts:
        st = (s[0], s[1], s[2], -1)          # 末位 = 进入方向，用于算拐弯代价
        best[st] = 0
        heapq.heappush(pq, (h(s), 0, st))
    while pq:
        _, g, cur = heapq.heappop(pq)
        if g > best.get(cur, 1 << 30):
            continue
        if (cur[0], cur[1], cur[2]) in goalset:
            path = []
            n = cur
            while n in came:
                path.append((n[0], n[1], n[2]))
                n = came[n]
            path.append((n[0], n[1], n[2]))
            return path[::-1]
        for di, (dx, dy) in enumerate(DIRS):
            nx, ny = cur[0] + dx, cur[1] + dy
            if not (0 <= nx < NW and 0 <= ny < NH):
                continue
            if occ[cur[2]][ny * NW + nx]:
                continue
            ng = g + COST[di] + (TURN if cur[3] != -1 and cur[3] != di else 0)
            nn = (nx, ny, cur[2], di)
            if ng < best.get(nn, 1 << 30):
                best[nn] = ng; came[nn] = cur
                heapq.heappush(pq, (ng + h(nn), ng, nn))
        if allow_via:
            chk = occ_via if occ_via is not None else occ
            for nl in range(len(LAYERS_ALL)):
                if nl == cur[2] or chk[nl][cur[1] * NW + cur[0]]:
                    continue
                # hole_to_hole：候选过孔位置到现有孔间距必须 ≥ 板规 0.25mm
                wx, wy = g2m(cur[0], cur[1])
                if occ_via is not None and not hole_ok(wx, wy):
                    continue
                ng = g + VIA * 10
                nn = (cur[0], cur[1], nl, -1)
                if ng < best.get(nn, 1 << 30):
                    best[nn] = ng; came[nn] = cur
                    heapq.heappush(pq, (ng + h(nn), ng, nn))
    return None


def snapshot_pads():
    """把所有焊盘的几何抽成纯 Python 数据。
    **必须在任何 board.Remove() 之前做**：SWIG 的焊盘代理在板子被修改后整体失效，
    再访问会拿到裸 SwigPyObject（实测 AttributeError: no attribute 'GetLeft'）。"""
    out = []
    for f in board.GetFootprints():
        for p in f.Pads():
            bb = p.GetBoundingBox()
            out.append(dict(
                net=p.GetNetCode(),
                box=(pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                     pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())),
                ctr=(pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y)),
                layers=[LI[l] for l in LAYERS_ALL if p.IsOnLayer(l)]))
    return out


def snapshot_keepouts():
    out = []
    for f in board.GetFootprints():
        for z in f.Zones():
            bb = z.GetBoundingBox()
            out.append((pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                        pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())))
    return out


PADS_SNAP = snapshot_pads()
KEEPOUTS = snapshot_keepouts()


def pad_cells(pd):
    """焊盘覆盖的栅格（焊盘实体范围缩 0.05 避免贴边）"""
    x0, y0, x1, y1 = pd["box"]
    a, b = m2g(x0 + 0.05, y0 + 0.05)
    c, d = m2g(x1 - 0.05, y1 - 0.05)
    out = []
    for li in pd["layers"]:
        for gy in range(max(0, b), min(NH, d + 1)):
            for gx in range(max(0, a), min(NW, c + 1)):
                out.append((gx, gy, li))
    if out:
        return out
    gx, gy = m2g(*pd["ctr"])
    return [(gx, gy, li) for li in (pd["layers"] or [0])]


def track_cells(net_code):
    """该网络已有走线覆盖的栅格——已连通的部分可作为新路径的起点"""
    out = []
    for t in board.GetTracks():
        if t.GetNetCode() != net_code or isinstance(t, pcbnew.PCB_VIA):
            continue
        if t.GetLayer() not in LI:
            continue
        li = LI[t.GetLayer()]
        x1, y1 = pcbnew.ToMM(t.GetStart().x), pcbnew.ToMM(t.GetStart().y)
        x2, y2 = pcbnew.ToMM(t.GetEnd().x), pcbnew.ToMM(t.GetEnd().y)
        n = max(2, int(math.hypot(x2 - x1, y2 - y1) / GRID))
        for i in range(n + 1):
            gx, gy = m2g(x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n)
            if 0 <= gx < NW and 0 <= gy < NH:
                out.append((gx, gy, li))
    return out


def emit(path, net, width):
    """把栅格路径转成 KiCad 走线与过孔（同层连续段合并成一根直线）"""
    n_seg = n_via = 0
    i = 0
    while i < len(path) - 1:
        j = i
        while j + 1 < len(path) and path[j + 1][2] == path[i][2]:
            j += 1
        if j > i:      # 同层一段：按拐点切分
            k = i
            while k < j:
                m = k + 1
                dx = path[m][0] - path[k][0]
                dy = path[m][1] - path[k][1]
                while m + 1 <= j and (path[m + 1][0] - path[m][0]) == dx \
                        and (path[m + 1][1] - path[m][1]) == dy:
                    m += 1
                ax, ay = g2m(path[k][0], path[k][1])
                bx, by = g2m(path[m][0], path[m][1])
                t = pcbnew.PCB_TRACK(board)
                t.SetStart(pcbnew.VECTOR2I_MM(round(ax, 4), round(ay, 4)))
                t.SetEnd(pcbnew.VECTOR2I_MM(round(bx, 4), round(by, 4)))
                t.SetWidth(pcbnew.FromMM(width))
                t.SetLayer(LAYERS_ALL[path[k][2]])
                t.SetNet(net)
                board.Add(t)
                n_seg += 1
                k = m
        if j + 1 < len(path):     # 换层 = 过孔
            vx, vy = g2m(path[j][0], path[j][1])
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pcbnew.VECTOR2I_MM(round(vx, 4), round(vy, 4)))
            v.SetWidth(pcbnew.FromMM(0.6)); v.SetDrill(pcbnew.FromMM(0.3))
            v.SetNet(net)
            board.Add(v)
            n_via += 1
        i = j + 1
    return n_seg, n_via


def route_net(name, width, allow_via):
    """把一个网络的所有焊盘连通。返回 (成功段数, 过孔数, 未连通的焊盘数)"""
    global came
    net = board.FindNet(name)
    if not net:
        return 0, 0, 0
    nc = net.GetNetCode()
    # 焊盘必须在删除走线**之前**取：board.Remove() 之后 SWIG 的封装迭代器会失效，
    # 再取 GetFootprints() 拿到的是裸 SwigPyObject（实测 AttributeError: no attribute 'Pads'）。
    pads = [pd for pd in PADS_SNAP if pd["net"] == nc]
    if len(pads) < 2:
        return 0, 0, 0
    groups = [set(pad_cells(p)) for p in pads]
    conn = set(groups[0]) | set(track_cells(nc))
    todo = list(range(1, len(pads)))
    seg = via = fail = 0
    while todo:
        occ = build_occ(nc, width)
        occ_via = build_occ_via(nc) if allow_via else None
        best = None
        for idx in todo:
            came = {}
            path = astar(occ, list(conn), list(groups[idx]), allow_via, occ_via)
            if path and (best is None or len(path) < len(best[1])):
                best = (idx, path)
        if best is None:
            fail += len(todo)
            break
        idx, path = best
        s, v = emit(path, net, width)
        seg += s; via += v
        conn |= set(path) | groups[idx]
        todo.remove(idx)
    return seg, via, fail


def route_gap(net_name, p0, l0, p1, l1, width, allow_via):
    """按 KiCad DRC 报告的两个端点直连。返回 (段数, 过孔数, 成功?)。

    为什么不自己推连通性：route_net 把一个网络已有的铜**当成一个连通块**
    （conn = 起始焊盘 ∪ 全部已有走线格），可实际它们是互不相连的碎片，
    于是它以为无事可做，报"0 段 ✅"，而 KiCad 那边明明还亮着飞线。
    KiCad 的 DRC 已经算好了真正的连通性并指出**具体哪两个物体、在哪**没连上，
    直接连这两点，比重新实现一遍连通性可靠。
    """
    global came
    net = board.FindNet(net_name)
    if not net:
        return 0, 0, False
    occ = build_occ(net.GetNetCode(), width)

    def cell(p, lname):
        # LI 的键是层 ID（整数），DRC 报告给的是层名字符串——必须先转，
        # 直接拿 "F.Cu" 去查 LI 永远查不到，会静默判成"走不通"。
        lid = board.GetLayerID(lname)
        if lid not in LI:                       # 比如 In1.Cu：那是参考面，不当布线层
            lid = pcbnew.F_Cu
        gx, gy = m2g(*p)
        if not (0 <= gx < NW and 0 <= gy < NH):
            return None
        return (gx, gy, LI[lid])

    a, b = cell(p0, l0), cell(p1, l1)
    if a is None or b is None:
        return 0, 0, False
    # 起终点自身多半压在已有铜/焊盘上，强行放行，否则第一步就走不动
    for gx, gy, li in (a, b):
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                x, y = gx + dx, gy + dy
                if 0 <= x < NW and 0 <= y < NH:
                    occ[li][y * NW + x] = 0
    came = {}
    occ_via = build_occ_via(net.GetNetCode()) if allow_via else None
    path = astar(occ, [a], [b], allow_via, occ_via)
    if not path:
        return 0, 0, False
    s_, v_ = emit(path, net, width)
    return s_, v_, True


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "rf"
    if mode == "rf":
        print("=== 射频段确定性布线（F.Cu 单层、禁过孔、0.34mm）===")
        print("目的：把射频从内层赶回 F.Cu。In2 夹在两平面之间没有紧邻参考面，")
        print("      射频走那里阻抗失控——freerouting 不懂这个，只能由我们钉死。\n")
        tot_s = tot_v = tot_f = 0
        for n in RF_NETS:
            if not board.FindNet(n):
                continue
            s, v, f = route_net(n, RF_W, allow_via=False)
            tot_s += s; tot_v += v; tot_f += f
            print(f"  {n:14s} {s:3d} 段 {v:2d} 过孔" + (f"   ❌ {f} 个焊盘未连通" if f else "   ✅"))
        print(f"\n合计 {tot_s} 段 / {tot_v} 过孔 / {tot_f} 个焊盘未连通")
    else:
        # 补漏模式：跑在 freerouting **之后**。
        # 顺序很重要——反过来（先补漏再喂 freerouting）会让 freerouting 卡死在
        # 读 DSN，它的导入器吃不下已布好的走线。放在最后就完全绕开那个 bug。
        print("=== 按 DRC 报告逐个补漏（freerouting 之后）===")
        import json
        import re as _re
        rpt = sys.argv[2] if len(sys.argv) > 2 else "/tmp/final-drc.json"
        RF = set(RF_NETS)

        jobs, skipped = [], 0
        for it in json.load(open(rpt))["unconnected_items"]:
            ds = [x["description"] for x in it["items"]]
            if any(d.startswith("Zone ") for d in ds):
                skipped += 1                    # 焊盘没接上覆铜，靠缝合过孔解决，不是布线
                continue
            m = _re.search(r"\[([^\]]*)\]", ds[0])
            if not m or m.group(1).startswith("unconnected-"):
                continue
            net = m.group(1)
            pts = []
            for d, x in zip(ds, it["items"]):
                lm = _re.search(r" on ([A-Za-z0-9.]+)", d)   # 过孔写成 "F.Cu - B.Cu"，取头一个
                pts.append(((x["pos"]["x"], x["pos"]["y"]), lm.group(1) if lm else "F.Cu"))
            jobs.append((net, pts[0], pts[1]))

        print(f"待补 {len(jobs)} 处（另有 {skipped} 处属覆铜问题，本模式不处理）")
        # PK_TARGET_NETS 给定时只补这些网络（其余跳过）——避免全板逐网络重建占用区太慢
        _tgt = os.environ.get("PK_TARGET_NETS", "").strip()
        if _tgt:
            _want = {n.strip() for n in _tgt.split(",") if n.strip()}
            jobs = [j for j in jobs if j[0] in _want]
            print(f"PK_TARGET_NETS 过滤后只补 {len(jobs)} 处：{[j[0] for j in jobs]}")
        print()
        ok = fail = tot_s = tot_v = 0
        for net, (p0, l0), (p1, l1) in sorted(jobs, key=lambda j: j[0]):
            isrf = net in RF
            # 射频只走 F.Cu、禁过孔：过孔会在 In1 参考面上打洞
            s_, v_, done = route_gap(net, p0, l0, p1, l1,
                                     RF_W if isrf else 0.25, allow_via=not isrf)
            tot_s += s_; tot_v += v_
            ok, fail = (ok + 1, fail) if done else (ok, fail + 1)
            tag = "RF " if isrf else "   "
            print(f"  {tag}{net:16s} {s_:3d} 段 {v_:2d} 过孔  " + ("✅" if done else "❌ 走不通"))
        print(f"\n补上 {ok} 处 / 失败 {fail} 处；新增 {tot_s} 段 {tot_v} 过孔")

    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    board.Save(PCB)
    print("saved:", PCB)
