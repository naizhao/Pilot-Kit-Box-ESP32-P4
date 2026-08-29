#!/usr/bin/env python3
"""收尾布线器——把 freerouting + route_maze 之后剩下的残线逐条布通。

## 为什么再写一个而不是改 route_maze

route_maze 卡的不是复杂度，是**障碍模型**：它拿 `track.GetBoundingBox()` 当障碍
（route_maze.py:100-103）。本板 1308 段走线里 610 段是 45° 斜线，实测 bbox 面积
合计 3290mm²，而实际铜面积只有 778mm² —— **4.2 倍的假障碍**，还没算 0.37mm 膨胀。
假障碍糊满整图，A* 每次都得把整个搜索空间翻完才报"走不通"，于是"单条几分钟"。

本脚本换两处：

1. **线段光栅化**代替 bbox：沿线按 GRID/2 步长采样，逐点盖圆形邻域。
   代价正比于线长而不是包围盒面积，且不再产生假障碍。
2. **(count, owner) 双数组**代替 per-net 重建占用区：
   每格记「几条铜盖着」+「第一个盖它的网络」。查询 `blocked(li,k,nc)` 是 O(1)：
   count==0 放行；count==1 且 owner==nc 放行（自己的铜，电气等价）；否则挡。
   route_maze 每布一条就要重刷 mine 字典 + 复制 3×26.8 万字节，这里一次建图全程复用。
3. **增量更新**：emit 出去的新走线立刻光栅化进图。route_maze 的 `_CNT` 缓存建完
   就不再更新，后布的线看不见先布的线（目前侥幸没撞上，但那是运气）。

## 用法

  route_fix.py plan  <drc.json>   只规划打印，不写盘（KiCad 开着时用这个）
  route_fix.py apply <drc.json>   布线并保存
  route_fix.py clean <drc.json>   删孤立断头铜（必须单独进程跑，见下）

⚠️ board.Remove() 会让本进程内所有 SWIG 代理退化成裸 SwigPyObject，
   重新 LoadBoard() 也救不回来。所以 clean 单独一趟，跑完再跑 apply。
"""
import json
import math
import os
import re
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")   # 中间产物落在工程内，可核对、不会被系统清掉
os.makedirs(BUILD, exist_ok=True)
sys.path.insert(0, os.path.join(T, "tools"))
# PK_BOARD_DIR 指到别处时整套都在那儿跑（含 .kicad_pro 的网络类）。
# 迭代 rip-up 要反复读写板子，而 KiCad 开着时原文件动不得——所以在 /tmp 开工作副本。
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

MODE = sys.argv[1] if len(sys.argv) > 1 else "plan"
DRC = sys.argv[2] if len(sys.argv) > 2 else os.path.join(BUILD, "drc.json")
RIPUP_OUT = os.environ.get("PK_RIPUP_OUT", os.path.join(BUILD, "ripup.json"))

GRID = 0.15
X0, Y0, X1, Y1 = 50.0, 50.0, 150.0, 112.0
EDGE = 0.5
# 跟 .kicad_pro 的板规保持一致：过孔已从 0.6/0.3 收到 0.4/0.2（嘉立创 6 层标准档
# 最小是 0.25/0.15，我们留了一倍裕度）。这两个数字忘了同步的话，占用图会按 0.6mm
# 盘去膨胀障碍，比实际严 0.1mm，白白布不通几条。
VIA_R = 0.225                   # 过孔盘半径 0.45/2
VIA_HOLE_R = 0.15               # 钻孔半径 0.3/2
HOLE_GAP = 0.25                 # 板规 min_hole_to_hole

from gen_sch import RF50_NETS                                   # noqa: E402
RF = set(RF50_NETS)          # 不从 route_maze 取：那个模块 import 即 LoadBoard

# ── 网络类（来自 .kicad_pro net_settings）────────────────────────────
# 之前全板按 clr=0.20 / 线宽 0.25 一刀切，比板规严得多：Default 类其实允许
# 0.15 线宽 + 0.15 净空。差出来的 0.145mm 接近一整格，在 CC1312R 那种 0.5mm
# pitch 的 QFN 引脚缝里就是"进得去"和"进不去"的分界——3V3_DIG 那处 13×13 格
# 全被判死，就是这么来的。所以必须按网络类分档。
import fnmatch                                                   # noqa: E402
from drc_classify import (                                       # noqa: E402
    classify_unconnected_item,
    extract_layer_name,
    extract_net_name,
)
from route_geometry import closest_point_on_block                # noqa: E402
_pro = json.load(open(os.path.join(BDIR, "expansion-board-v3.kicad_pro")))
_ns = _pro["net_settings"]
CLS = {c["name"]: c for c in _ns["classes"]}
_PAT = _ns.get("netclass_patterns", [])


def net_class(name):
    for p in _PAT:
        if fnmatch.fnmatch(name, p["pattern"]):
            return p["netclass"]
    return "Default"


def clr_of(name):
    return CLS.get(net_class(name), CLS["Default"]).get("clearance", 0.15)


# 布线用的线宽：按类取，但电源补片不必按 0.5 推荐值走满——DRC 只卡最小值，
# 0.3mm 内层载流 ~0.8A，够 3V3_DIG 这种补碎片用，而 0.5mm 挤不进去。
PROFILE = {                              # 类 → (线宽, 我方净空)
    "Default":  (0.15, 0.15),
    "POWER":    (0.30, 0.20),
    # POWER_LO = 低压轨（3V3_*/RP_1V1/SUBG_VDDR/馈电）。类线宽 0.25，净空同
    # Default 的 0.15，所以占用图走 Default 那套（见下方 si_ 的 my_clr>=0.2 判据），
    # 落盘前的连续几何复核传真实半宽，不会因此漏判。
    "POWER_LO": (0.25, 0.15),
    "RF50":     (0.15, 0.15),   # 0.34→0.15：6 层叠层下 0.34 只有 31.82Ω，见 route_rf.py
}

board = pcbnew.LoadBoard(PCB)
NW = int((X1 - X0 - 2 * EDGE) / GRID)
NH = int((Y1 - Y0 - 2 * EDGE) / GRID)
LAYERS_ALL = [pcbnew.F_Cu, pcbnew.In2_Cu, pcbnew.In3_Cu, pcbnew.B_Cu]  # In1/In4 是地平面
LI = {l: i for i, l in enumerate(LAYERS_ALL)}
NL = len(LAYERS_ALL)


def g2m(gx, gy):
    return X0 + EDGE + gx * GRID, Y0 + EDGE + gy * GRID


def m2g(x, y):
    return int(round((x - X0 - EDGE) / GRID)), int(round((y - Y0 - EDGE) / GRID))


# ── 占用图 ────────────────────────────────────────────────────────────
# 四套图，按"我方是什么线"分：三个网络类各一套 + 过孔一套。
# 膨胀量 = 障碍半宽 + max(障碍类净空, 我方类净空) + 我方半宽。
# clearance 取 max 是 KiCad 的规则，不可分解成两边各自的量，所以只能按我方类分套建。
# POWER_LO 单独一套而不是复用 POWER：POWER 套是半宽 0.15/净空 0.20，拿它当
# POWER_LO(半宽 0.125/净空 0.15) 用会保守 0.05mm，白白布不通几条。
SETS = ["Default", "POWER", "POWER_LO", "RF50", "VIA"]
SI = {n: i for i, n in enumerate(SETS)}
MY = {                                   # 套 → (我方半宽, 我方净空)
    "Default":  (PROFILE["Default"][0] / 2,  PROFILE["Default"][1]),
    "POWER":    (PROFILE["POWER"][0] / 2,    PROFILE["POWER"][1]),
    "POWER_LO": (PROFILE["POWER_LO"][0] / 2, PROFILE["POWER_LO"][1]),
    "RF50":     (PROFILE["RF50"][0] / 2,     PROFILE["RF50"][1]),
    "VIA":      (VIA_R,                      0.20),
}

CNT = [[bytearray(NW * NH) for _ in range(NL)] for _ in SETS]
OWN = [[[0] * (NW * NH) for _ in range(NL)] for _ in SETS]


def _stamp(li, cx, cy, r, nc, which):
    """把以 (cx,cy) 为心、半径 r 的圆盖进占用图。"""
    a, b = m2g(cx - r, cy - r)
    c, d = m2g(cx + r, cy + r)
    cnt, own = CNT[which][li], OWN[which][li]
    rr = r * r
    for gy in range(max(0, b), min(NH, d + 1)):
        wy = Y0 + EDGE + gy * GRID
        row = gy * NW
        for gx in range(max(0, a), min(NW, c + 1)):
            wx = X0 + EDGE + gx * GRID
            if (wx - cx) ** 2 + (wy - cy) ** 2 > rr:
                continue
            k = row + gx
            if cnt[k] == 0:
                own[k] = nc
                cnt[k] = 1
            elif own[k] != nc and cnt[k] < 255:
                cnt[k] += 1


def add_seg(li, x1, y1, x2, y2, half, nc, clr_o):
    """线段光栅化：沿线按半格步长盖圆。代价正比线长，不是包围盒面积。"""
    L = math.hypot(x2 - x1, y2 - y1)
    n = max(1, int(L / (GRID * 0.5)))
    for si, s in enumerate(SETS):
        my_h, my_c = MY[s]
        r = half + max(clr_o, my_c) + my_h
        for i in range(n + 1):
            t = i / n
            _stamp(li, x1 + (x2 - x1) * t, y1 + (y2 - y1) * t, r, nc, si)


def add_rect(layers, x0, y0, x1, y1, nc, clr_o):
    """焊盘用矩形——矩形焊盘的 bbox 是精确的，不存在斜线那种虚胖。"""
    for si, s in enumerate(SETS):
        my_h, my_c = MY[s]
        inf = max(clr_o, my_c) + my_h
        a, b = m2g(x0 - inf, y0 - inf)
        c, d = m2g(x1 + inf, y1 + inf)
        for li in layers:
            cnt, own = CNT[si][li], OWN[si][li]
            for gy in range(max(0, b), min(NH, d + 1)):
                row = gy * NW
                for gx in range(max(0, a), min(NW, c + 1)):
                    k = row + gx
                    if cnt[k] == 0:
                        own[k] = nc
                        cnt[k] = 1
                    elif own[k] != nc and cnt[k] < 255:
                        cnt[k] += 1


HOLES = []
PADS = []


def build():
    npad = ntrk = nvia = nko = 0
    clr_cache = {}

    def clr_nc(netcode, netname):
        if netcode not in clr_cache:
            clr_cache[netcode] = clr_of(netname)
        return clr_cache[netcode]

    for f in board.GetFootprints():
        for p in f.Pads():
            bb = p.GetBoundingBox()
            ls = [LI[l] for l in LAYERS_ALL if p.IsOnLayer(l)]
            co = clr_nc(p.GetNetCode(), p.GetNetname())
            PADS.append(dict(net=p.GetNetCode(), layers=ls,
                             box=(pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                                  pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom()))))
            if ls:
                add_rect(ls, pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                         pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom()),
                         p.GetNetCode(), co)
                npad += 1
            dr = pcbnew.ToMM(p.GetDrillSizeX())
            if dr > 0:
                pp = p.GetPosition()
                HOLES.append((pcbnew.ToMM(pp.x), pcbnew.ToMM(pp.y), dr / 2))
        for z in f.Zones():                       # U.FL 自带禁布区：谁都不许走
            bb = z.GetBoundingBox()
            add_rect(range(NL), pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                     pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom()), -1, 0.20)
            nko += 1
    for t in board.GetTracks():
        co = clr_nc(t.GetNetCode(), t.GetNetname())
        if isinstance(t, pcbnew.PCB_VIA):
            p = t.GetPosition()
            x, y = pcbnew.ToMM(p.x), pcbnew.ToMM(p.y)
            for si, s in enumerate(SETS):
                my_h, my_c = MY[s]
                for li in range(NL):
                    _stamp(li, x, y, VIA_R + max(co, my_c) + my_h, t.GetNetCode(), si)
            HOLES.append((x, y, pcbnew.ToMM(t.GetDrill()) / 2))
            nvia += 1
        elif t.GetLayer() in LI:
            add_seg(LI[t.GetLayer()],
                    pcbnew.ToMM(t.GetStart().x), pcbnew.ToMM(t.GetStart().y),
                    pcbnew.ToMM(t.GetEnd().x), pcbnew.ToMM(t.GetEnd().y),
                    pcbnew.ToMM(t.GetWidth()) / 2, t.GetNetCode(), co)
            ntrk += 1
    return npad, ntrk, nvia, nko


def blocked(which, li, k, nc):
    c = CNT[which][li][k]
    return c != 0 and not (c == 1 and OWN[which][li][k] == nc)


def hole_ok(x, y):
    for hx, hy, hr in HOLES:
        if math.hypot(x - hx, y - hy) < hr + VIA_HOLE_R + HOLE_GAP:
            return False
    return True


# ── A* ────────────────────────────────────────────────────────────────
import heapq                                                     # noqa: E402

DIRS = ((1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1))
COST = (10, 14, 10, 14, 10, 14, 10, 14)
TURN, VIA_COST = 3, 400
FREE = set()                     # 起终点邻域强制放行（端点必压在自己的铜上）


def astar(starts, goals, nc, allow_via, si, budget=400000, goal_layers=()):
    """多起点多终点：starts/goals 是连通块覆盖的格子集合。
    goal_layers：该网络有整层覆铜时给出层号——走到那一层就算连上了（过孔即接平面），
    不必千辛万苦横穿到对面那个焊盘。3V3_DIG 在 In3.Cu 是满板平面，U10 的 3V3 引脚
    被 QFN 引脚走廊封死时，唯一出路就是就地下钻接平面。
    budget = 最多扩展节点数，走不通时不再翻遍全图。"""
    goalset = set(goals)
    ZL = list(goal_layers)
    GL = {z[0] for z in ZL}
    if (not goalset and not ZL) or not starts:
        return None, 0
    gxs = [g[0] for g in goalset] or [s[0] for s in starts]
    gys = [g[1] for g in goalset] or [s[1] for s in starts]
    # 目标是一整块铜时，逐个算距离取 min 太贵（块可能上千格）。
    # 用包围盒到点的距离做启发式：可采纳（不高估），且是 O(1)。
    gx0, gx1 = min(gxs), max(gxs)
    gy0, gy1 = min(gys), max(gys)

    def h(x, y, li):
        if li in GL:                     # 已在平面层上，到手了
            return 0
        dx = gx0 - x if x < gx0 else (x - gx1 if x > gx1 else 0)
        dy = gy0 - y if y < gy0 else (y - gy1 if y > gy1 else 0)
        d = 10 * max(dx, dy) + 4 * min(dx, dy)
        return min(d, VIA_COST) if GL else d

    came = {}
    best = {}
    pq = []
    for s in starts:
        st = (s[0], s[1], s[2], -1)
        best[st] = 0
        heapq.heappush(pq, (h(s[0], s[1], s[2]), 0, st))
    pops = 0
    while pq:
        _, g, cur = heapq.heappop(pq)
        if g > best.get(cur, 1 << 30):
            continue
        pops += 1
        if pops > budget:
            return None, pops
        _hit_plane = (cur[2] in GL and cur[3] == -1        # cur[3]==-1 = 刚打完过孔落到本层
                      and cur in came                     # 排除起点自身，否则原地"成功"
                      and on_plane(ZL, cur[2], *g2m(cur[0], cur[1])))
        if (cur[0], cur[1], cur[2]) in goalset or _hit_plane:
            path, n = [], cur
            while n in came:
                path.append((n[0], n[1], n[2]))
                n = came[n]
            path.append((n[0], n[1], n[2]))
            return path[::-1], pops
        # 平面层只对它自己的网络开放同层移动（见 PLANE_NC_OF_LI 注释）
        _same_layer_ok = PLANE_NC_OF_LI.get(cur[2], nc) == nc
        for di, (dx, dy) in enumerate(DIRS) if _same_layer_ok else ():
            nx, ny = cur[0] + dx, cur[1] + dy
            if not (0 <= nx < NW and 0 <= ny < NH):
                continue
            k = ny * NW + nx
            if (cur[2], k) not in FREE and blocked(si, cur[2], k, nc):
                continue
            ng = g + COST[di] + (TURN if cur[3] not in (-1, di) else 0)
            nn = (nx, ny, cur[2], di)
            if ng < best.get(nn, 1 << 30):
                best[nn] = ng
                came[nn] = cur
                heapq.heappush(pq, (ng + h(nx, ny, cur[2]), ng, nn))
        if allow_via:
            k = cur[1] * NW + cur[0]
            wx, wy = g2m(cur[0], cur[1])
            SV = SI["VIA"]
            # ⚠️ 通孔穿透**所有**层，不是只占起止两层。只校验 cur 层和目标层的话，
            # 过孔会在中间层撞上别人的走线——实测这么放过去 9 条 shorting/clearance，
            # 全是 "Via[X] on F.Cu-B.Cu <-> Track[Y] on In2/In3"。
            if any(blocked(SV, L_, k, nc) for L_ in range(NL)):
                continue
            for nl in range(NL):
                if nl == cur[2]:
                    continue
                if not hole_ok(wx, wy):
                    continue
                ng = g + VIA_COST
                nn = (cur[0], cur[1], nl, -1)
                if ng < best.get(nn, 1 << 30):
                    best[nn] = ng
                    came[nn] = cur
                    heapq.heappush(pq, (ng + h(cur[0], cur[1], nl), ng, nn))
    return None, pops


def trim(path, startset, goalset):
    """路径两端可能贴着起/终点块内部游走了一截——那些格子本来就已经连通，
    留着只会在已有铜上叠一层重复走线。裁到最后一次接触起块 / 第一次接触终块。"""
    i = 0
    for k, n in enumerate(path):
        if (n[0], n[1], n[2]) in startset:
            i = k
    j = len(path) - 1
    for k in range(i, len(path)):
        if (path[k][0], path[k][1], path[k][2]) in goalset:
            j = k
            break
    return path[i:j + 1]


def verify_segs(segs, vias, nc, width, my_clr):
    """落盘前用连续几何复核。栅格只保证**格点**在安全圈外，可走线是格点之间的
    连线——45° 步长 0.212mm，中点偏离格点 0.106mm，正好能蹭进净空里。
    实测就漏过一条：Via[SUBG_RESET] 与 3V3_DIG 走线 actual 0.1586 < 0.2。
    宁可这条不布，也不能往板子里塞 DRC 违例。"""
    for li, (ax, ay), (bx, by) in segs:
        ok, _ = stub_clear(li, ax, ay, bx, by, width / 2, nc, my_clr)
        if not ok:
            return False
    pads, trks, _vias = snap_obstacles()
    for vx, vy in vias:
        for onc, ols, box, oclr in pads:      # 过孔穿全层，逐层查焊盘
            if onc == nc:
                continue
            if seg_rect_dist(vx, vy, vx, vy, box) < VIA_R + max(oclr, my_clr):
                return False
        for onc, oli, g, ohalf, oclr in trks:
            if onc == nc:
                continue
            if seg_seg_dist((vx, vy), (vx, vy), (g[0], g[1]), (g[2], g[3])) \
                    < VIA_R + ohalf + max(oclr, my_clr):
                return False
    return True


def emit(path, net, width, nc, clr_o, my_clr=0.2, verify=True):
    """栅格路径 → KiCad 走线/过孔，并把新铜增量盖进占用图。"""
    segs, vias = [], []
    i = 0
    while i < len(path) - 1:
        j = i
        while j + 1 < len(path) and path[j + 1][2] == path[i][2]:
            j += 1
        k = i
        while k < j:
            m = k + 1
            dx, dy = path[m][0] - path[k][0], path[m][1] - path[k][1]
            while m + 1 <= j and (path[m + 1][0] - path[m][0], path[m + 1][1] - path[m][1]) == (dx, dy):
                m += 1
            segs.append((path[k][2], g2m(path[k][0], path[k][1]), g2m(path[m][0], path[m][1])))
            k = m
        if j + 1 < len(path):
            vias.append(g2m(path[j][0], path[j][1]))
        i = j + 1

    if verify and not verify_segs(segs, vias, nc, width, my_clr):
        return None
    for li, (ax, ay), (bx, by) in segs:
        # 零长度段要丢掉：过孔位置重合时会生成 0.0001mm 的残段，
        # KiCad 直接判 track_dangling（实测 USB_VBUS 上出过）。
        if math.hypot(bx - ax, by - ay) < 1e-3:
            continue
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I_MM(round(ax, 4), round(ay, 4)))
        t.SetEnd(pcbnew.VECTOR2I_MM(round(bx, 4), round(by, 4)))
        t.SetWidth(pcbnew.FromMM(width))
        t.SetLayer(LAYERS_ALL[li])
        t.SetNet(net)
        board.Add(t)
        add_seg(li, ax, ay, bx, by, width / 2, nc, clr_o)
    for vx, vy in vias:
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I_MM(round(vx, 4), round(vy, 4)))
        # ⚠️ 尺寸必须取自 VIA_R/VIA_HOLE_R，不能再写死。这两行原本硬编码 0.6/0.3，
        # 而上一轮把常量同步板规改成了 0.4/0.2 —— **只改了常量，漏了落盘**。
        # 后果很隐蔽：净空复核按盘 0.4 算，实际却往板子里塞盘 0.6，
        # 复核说余量 +0.058mm，KiCad 一跑 DRC 就是 actual 0.108 < 0.150。
        # 差的 0.1mm 正好是两个半径之差。同一族问题（改了尺寸没全仓复查）
        # 这是第四次，前三次是封装禁铜层、gen_pcb 板规下限、route_fix 过孔常量本身。
        v.SetWidth(pcbnew.FromMM(VIA_R * 2))
        v.SetDrill(pcbnew.FromMM(VIA_HOLE_R * 2))
        v.SetNet(net)
        board.Add(v)
        for si, s in enumerate(SETS):
            my_h, my_c = MY[s]
            for li in range(NL):
                _stamp(li, vx, vy, VIA_R + max(clr_o, my_c) + my_h, nc, si)
        HOLES.append((vx, vy, VIA_HOLE_R))
    return len(segs), len(vias)


# ── 焊盘逃逸 stub（精确几何，不走栅格）────────────────────────────────
# 为什么必须绕开栅格：QFN 引脚出线的正确方向是**沿引脚轴向往外**，不是横穿相邻
# 引脚的缝。精确算 U10（0.5mm pitch，引脚宽 0.25）：邻居焊盘边缘到我方中心线是
# 0.5 - 0.125 = 0.375mm，而 0.3mm 电源线只要 0.15 + 0.2 = 0.35mm —— 过得去，
# 余量 0.025mm。可 0.15mm 的栅格根本表达不了 0.025mm 的余量，栅格点又不会正好
# 落在引脚中心线上，于是整排引脚被判成"几何封死"。洪水填充说 U10.44 只能到达
# 14 格（就是焊盘自己），就是这么来的假结论。
# 这里对被封死的焊盘单独做一段轴向 stub，碰撞用连续几何算，算完再并回栅格图。
def seg_rect_dist(x1, y1, x2, y2, r):
    rx0, ry0, rx1, ry1 = r

    def pt_rect(px, py):
        dx = max(rx0 - px, 0, px - rx1)
        dy = max(ry0 - py, 0, py - ry1)
        return math.hypot(dx, dy)

    best = min(pt_rect(x1, y1), pt_rect(x2, y2))
    n = max(2, int(math.hypot(x2 - x1, y2 - y1) / 0.02))
    for i in range(n + 1):
        t = i / n
        best = min(best, pt_rect(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t))
        if best == 0:
            return 0.0
    return best


def seg_seg_dist(a, b, c, d):
    # ⚠️ 必须先判相交。"四端点到对方线段取最小"这个公式对**交叉**的线段会返回
    # 正值：两条线真穿过彼此时，四个端点离对方线段都可能很远。实测 SWDIO 的
    # (104.890,91.253)→(104.890,92.353) 与 SUBG_CSN 的
    # (122.332,91.980)→(103.615,91.980) 在 (104.890,91.980) 交叉，
    # 公式却给出 0.3733mm、余量 +0.07 判为合规——短路就这么被写进板子
    # （KiCad DRC 报 tracks_crossing）。相交时距离必须是 0。
    def _cross(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    _d1, _d2 = _cross(c, d, a), _cross(c, d, b)
    _d3, _d4 = _cross(a, b, c), _cross(a, b, d)
    if ((_d1 > 0) != (_d2 > 0)) and ((_d3 > 0) != (_d4 > 0)):
        return 0.0

    def pt_seg(p, s, e):
        dx, dy = e[0] - s[0], e[1] - s[1]
        L2 = dx * dx + dy * dy
        if L2 < 1e-12:
            return math.hypot(p[0] - s[0], p[1] - s[1])
        t = max(0.0, min(1.0, ((p[0] - s[0]) * dx + (p[1] - s[1]) * dy) / L2))
        return math.hypot(p[0] - (s[0] + dx * t), p[1] - (s[1] + dy * t))
    return min(pt_seg(a, c, d), pt_seg(b, c, d), pt_seg(c, a, b), pt_seg(d, a, b))


# In3 现在是锁死的 3V3_DIG 电源平面（export_dsn 的 PLANE_LAYERS 同步锁了）。
# 别的网络可以打通孔**穿过**它（通孔本来就穿全层），但不能在上面**走线**——
# 走线会让覆铜避让挖空，把平面切碎，这正是 3V3_DIG 六项未连通的根因。
# 实测没这条限制时 USB_VBUS 的补线在 In3 上走了 11 段。
PLANE_NC_OF_LI = {}
_in3 = LI.get(pcbnew.In3_Cu)
if _in3 is not None:
    for _z in board.Zones():
        if _z.IsFilled() and _z.GetLayerSet().Contains(pcbnew.In3_Cu):
            PLANE_NC_OF_LI[_in3] = _z.GetNetCode()
            break

OBST = None


def snap_obstacles():
    """精确几何障碍表（不膨胀，膨胀量在校验时按双方网络类算）"""
    global OBST
    if OBST is not None:
        return OBST
    pads, trks, vias = [], [], []
    for f in board.GetFootprints():
        for p in f.Pads():
            bb = p.GetBoundingBox()
            pads.append((p.GetNetCode(),
                         [LI[l] for l in LAYERS_ALL if p.IsOnLayer(l)],
                         (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                          pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())),
                         clr_of(p.GetNetname())))
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            p = t.GetPosition()
            # ⚠️ 半径必须读**实际**尺寸，不能套 VIA_R 常量。板上过孔不是一种尺寸：
            # route_rf 的 GND 缝合过孔是盘 0.6、thermal_vias 的散热过孔是盘 0.3、
            # 本模块新布的是盘 0.4。按常量 0.2 算半径，遇上 0.6 的就低估 0.1mm，
            # 复核放行、KiCad 报 actual 0.105 < 0.150。
            vias.append((t.GetNetCode(), (pcbnew.ToMM(p.x), pcbnew.ToMM(p.y)),
                         clr_of(t.GetNetname()), pcbnew.ToMM(t.GetWidth()) / 2))
        elif t.GetLayer() in LI:
            trks.append((t.GetNetCode(), LI[t.GetLayer()],
                         (pcbnew.ToMM(t.GetStart().x), pcbnew.ToMM(t.GetStart().y),
                          pcbnew.ToMM(t.GetEnd().x), pcbnew.ToMM(t.GetEnd().y)),
                         pcbnew.ToMM(t.GetWidth()) / 2, clr_of(t.GetNetname())))
    OBST = (pads, trks, vias)
    return OBST


def stub_clear(li, x1, y1, x2, y2, half, nc, my_clr):
    """一段走线在连续几何下是否满足所有净空。返回 (是否合法, 最紧余量)"""
    pads, trks, vias = snap_obstacles()
    margin = 9.9
    for onc, ols, box, oclr in pads:
        if onc == nc or li not in ols:
            continue
        need = half + max(oclr, my_clr)
        d = seg_rect_dist(x1, y1, x2, y2, box)
        margin = min(margin, d - need)
        if d < need:
            return False, margin
    for onc, oli, g, ohalf, oclr in trks:
        if onc == nc or oli != li:
            continue
        need = half + ohalf + max(oclr, my_clr)
        d = seg_seg_dist((x1, y1), (x2, y2), (g[0], g[1]), (g[2], g[3]))
        margin = min(margin, d - need)
        if d < need:
            return False, margin
    for onc, p, oclr, orad in vias:
        if onc == nc:
            continue
        need = half + orad + max(oclr, my_clr)
        d = seg_seg_dist((x1, y1), (x2, y2), p, p)
        margin = min(margin, d - need)
        if d < need:
            return False, margin
    return True, margin


def escape_stub(nc, pad_pos, fp_center, box, li, half, my_clr):
    """从焊盘沿引脚轴向朝芯片外侧拉一段 stub，直到粗栅格上"自由"为止。
    返回 [(x1,y1,x2,y2)] 或 None。"""
    px, py = pad_pos
    dx, dy = px - fp_center[0], py - fp_center[1]
    x0, y0, x1_, y1_ = box
    w, h = x1_ - x0, y1_ - y0
    # 引脚是长条：轴向 = 长边方向；朝外 = 远离封装中心
    if h >= w:
        d = (0.0, 1.0 if dy >= 0 else -1.0)
        start = (px, y1_ if dy >= 0 else y0)
    else:
        d = (1.0 if dx >= 0 else -1.0, 0.0)
        start = (x1_ if dx >= 0 else x0, py)
    for L in (0.45, 0.6, 0.8, 1.0, 1.3, 1.6, 2.0):
        ex, ey = start[0] + d[0] * L, start[1] + d[1] * L
        okc, _ = stub_clear(li, px, py, ex, ey, half, nc, my_clr)
        if not okc:
            continue
        gx, gy = m2g(ex, ey)
        if not (0 <= gx < NW and 0 <= gy < NH):
            continue
        si_ = SI["POWER"] if my_clr >= 0.2 else SI["Default"]   # 0.15 净空的都走 Default 套
        if not blocked(si_, li, gy * NW + gx, nc):
            return [(px, py, ex, ey)]
    return None


# ── 从 DRC 报告读活儿 ──────────────────────────────────────────────────
def load_jobs():
    need, orphan = [], []
    for it in json.load(open(DRC))["unconnected_items"]:
        ds = [x["description"] for x in it["items"]]
        m = extract_net_name(it)
        if not m or m.startswith("unconnected-"):
            continue
        pts = []
        for d, x in zip(ds, it["items"]):
            pts.append(((x["pos"]["x"], x["pos"]["y"]), extract_layer_name(d)))
        classification = classify_unconnected_item(it)
        if classification == "plane":
            continue                                  # 覆铜没接上，靠缝合过孔，不是布线
        if classification == "need":
            need.append((m, pts[0], pts[1]))
            continue
        # ⚠️ 两端都不是焊盘 ≠ 孤立碎铜。原来这里无脑归 orphan（交给 clean 删掉），
        # 实测漏判：3V3_RF 有两处 Track↔Track / Via↔Track 缺口跨 **26mm 和 10.7mm**,
        # 两端各自都连着一大片铜和焊盘——那是"两大块该连没连"，是真缺线，
        # 被当成碎铜就永远没人去布它，还差点被 clean 删掉。
        # 正确判据：两端所在的**连通块是否各自都含焊盘**。都含 → 真缺线；
        # 有一端不含 → 那端确实是没接上任何东西的断头铜，交给 clean。
        _net = board.FindNet(m)
        if _net is None:
            orphan.append((m, pts[0], pts[1]))
            continue
        _b0 = _nearest_block(_net.GetNetCode(), pts[0][0])
        _b1 = _nearest_block(_net.GetNetCode(), pts[1][0])
        _has = (_b0 and any(e[0] == "pad" for e in _b0)
                and _b1 and any(e[0] == "pad" for e in _b1))
        (need if _has else orphan).append((m, pts[0], pts[1]))
    return need, orphan


_BLK_CACHE = {}


def _nearest_block(nc, pt):
    """DRC 报的坐标落在哪个连通块上——取几何最近的那块。
    不用栅格匹配：DRC 给的是元素锚点（走线端点、焊盘中心），
    换算成格子后未必正好落在该块的 cells 里，差一格就找不到。"""
    if nc not in _BLK_CACHE:                    # blocks_of 是 O(n²)，每个网络只算一次
        _BLK_CACHE[nc] = blocks_of(nc)
    best, bd = None, None
    for blk in _BLK_CACHE[nc]:
        for kind, geo, _ls in blk:
            if kind == "pad":
                x0, y0, x1, y1 = geo
                d = math.hypot(max(x0 - pt[0], 0, pt[0] - x1),
                               max(y0 - pt[1], 0, pt[1] - y1))
            elif kind == "via":
                d = math.hypot(pt[0] - geo[0], pt[1] - geo[1])
            else:
                ax, ay, bx, by = geo
                dx, dy = bx - ax, by - ay
                L = dx * dx + dy * dy
                t = 0.0 if L < 1e-12 else max(0.0, min(1.0, ((pt[0] - ax) * dx + (pt[1] - ay) * dy) / L))
                d = math.hypot(pt[0] - (ax + dx * t), pt[1] - (ay + dy * t))
            if bd is None or d < bd:
                bd, best = d, blk
    return best


# ── 连通块 ────────────────────────────────────────────────────────────
# 为什么必须有这一步：DRC 说的是"这个网络的**这两块铜**没连上"，而不是"这两个点
# 之间缺一根线"。只拿两个点当起终点，A* 从焊盘中心出发，四周被自身的扇出铜和邻居
# 围死，扩展一百多个节点就宣告走不通——上一轮 3V3_DIG/I2C_SDA/SUBG_SCK 三处
# 都卡在这里（固定 145 节点）。正确做法是把端点所在的**整块铜**都当起点：
# 块边缘任意一处能出去就行。
def net_elements(nc):
    """该网络的全部铜元素 → [(kind, 几何, 覆盖层)]"""
    els = []
    for pd in PADS:
        if pd["net"] == nc and pd["layers"]:
            els.append(("pad", pd["box"], pd["layers"]))
    for t in board.GetTracks():
        if t.GetNetCode() != nc:
            continue
        if isinstance(t, pcbnew.PCB_VIA):
            p = t.GetPosition()
            els.append(("via", (pcbnew.ToMM(p.x), pcbnew.ToMM(p.y)), list(range(NL))))
        elif t.GetLayer() in LI:
            els.append(("trk",
                        (pcbnew.ToMM(t.GetStart().x), pcbnew.ToMM(t.GetStart().y),
                         pcbnew.ToMM(t.GetEnd().x), pcbnew.ToMM(t.GetEnd().y)),
                        [LI[t.GetLayer()]]))
    return els


def _touch(a, b):
    """两个铜元素是否电气相接。必须先判层有没有交集——F.Cu 的线和 In2 的线
    在俯视图上叠着也毫不相干，不判层会把互不相连的碎片粘成一块。"""
    if not (set(a[2]) & set(b[2])):
        return False
    EPS = 0.06

    def pts(e):
        if e[0] == "pad":
            x0, y0, x1, y1 = e[1]
            return [((x0 + x1) / 2, (y0 + y1) / 2)]
        if e[0] == "via":
            return [e[1]]
        return [(e[1][0], e[1][1]), (e[1][2], e[1][3])]

    def inside(e, p):
        if e[0] != "pad":
            return False
        x0, y0, x1, y1 = e[1]
        return x0 - EPS <= p[0] <= x1 + EPS and y0 - EPS <= p[1] <= y1 + EPS

    for p in pts(b):
        if inside(a, p):
            return True
    for p in pts(a):
        if inside(b, p):
            return True
    if a[0] == "pad" and b[0] == "pad":
        return False
    for p in pts(a):
        for q in pts(b):
            if math.hypot(p[0] - q[0], p[1] - q[1]) < EPS:
                return True
    # 过孔落在走线中段：端点比对抓不到，得算点到线段距离
    for e, o in ((a, b), (b, a)):
        if e[0] == "via" and o[0] == "trk":
            x, y = e[1]
            x1, y1, x2, y2 = o[1]
            dx, dy = x2 - x1, y2 - y1
            L2 = dx * dx + dy * dy or 1e-9
            t = max(0.0, min(1.0, ((x - x1) * dx + (y - y1) * dy) / L2))
            if math.hypot(x - (x1 + dx * t), y - (y1 + dy * t)) < EPS:
                return True
    return False


def blocks_of(nc):
    els = net_elements(nc)
    par = list(range(len(els)))

    def find(i):
        while par[i] != i:
            par[i] = par[par[i]]
            i = par[i]
        return i

    for i in range(len(els)):
        for j in range(i + 1, len(els)):
            if find(i) != find(j) and _touch(els[i], els[j]):
                par[find(i)] = find(j)
    out = {}
    for i, e in enumerate(els):
        out.setdefault(find(i), []).append(e)
    return list(out.values())


def block_cells(blk):
    """一个连通块覆盖的栅格 —— 作为 A* 的起点集/终点集"""
    out = set()
    for kind, geo, ls in blk:
        if kind == "pad":
            x0, y0, x1, y1 = geo
            a, b = m2g(x0 + 0.03, y0 + 0.03)
            c, d = m2g(x1 - 0.03, y1 - 0.03)
            if c < a:
                a = c = (a + c) // 2
            if d < b:
                b = d = (b + d) // 2
            for li in ls:
                for gy in range(max(0, b), min(NH, d + 1)):
                    for gx in range(max(0, a), min(NW, c + 1)):
                        out.add((gx, gy, li))
        elif kind == "via":
            gx, gy = m2g(*geo)
            for li in ls:
                if 0 <= gx < NW and 0 <= gy < NH:
                    out.add((gx, gy, li))
        else:
            x1, y1, x2, y2 = geo
            n = max(2, int(math.hypot(x2 - x1, y2 - y1) / (GRID * 0.5)))
            for i in range(n + 1):
                gx, gy = m2g(x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n)
                if 0 <= gx < NW and 0 <= gy < NH:
                    out.add((gx, gy, ls[0]))
    return out


def zone_layers(nc):
    """该网络的覆铜层 → [(层号, zone, 层ID)]。打过孔落到**填充铜实体**上就接上了平面。

    ⚠️ 判据必须是 HitTestFilledArea 而不是"到了那一层"：In3 虽然是 3V3_DIG 平面，
    上面同时躺着 181 段别的网络的走线，覆铜会避让它们挖空。只判层号的话，起点块
    自己在 In3 上有一段走线就立刻"命中"，A* 一个节点都不扩展就报成功——上一轮
    3V3_DIG 两处 "0 段 0 过孔 / 1 节点" 就是这种假成功。"""
    out = []
    for z in board.Zones():
        if z.GetNetCode() != nc or not z.IsFilled():
            continue
        for l in z.GetLayerSet().Seq():
            if l in LI:
                out.append((LI[l], z, l))
    return out


def on_plane(zl, li, x, y):
    for zli, z, lid in zl:
        if zli == li and z.HitTestFilledArea(lid, pcbnew.VECTOR2I_MM(round(x, 4), round(y, 4))):
            return True
    return False


def block_on_plane(zl, blk):
    """这块铜是不是已经压在平面填充铜上了（已连平面，不需要再钻）"""
    for kind, geo, ls in blk:
        pts = ([( (geo[0]+geo[2])/2, (geo[1]+geo[3])/2 )] if kind == "pad"
               else [geo] if kind == "via"
               else [(geo[0], geo[1]), (geo[2], geo[3])])
        for li in ls:
            for x, y in pts:
                if on_plane(zl, li, x, y):
                    return True
    return False


def near_block(blocks, p, li):
    """DRC 端点落在哪个连通块上（取最近的）"""
    best, bd = None, 1e9
    for blk in blocks:
        for kind, geo, ls in blk:
            if kind == "pad":
                x0, y0, x1, y1 = geo
                cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
                d = math.hypot(p[0] - cx, p[1] - cy)
            elif kind == "via":
                d = math.hypot(p[0] - geo[0], p[1] - geo[1])
            else:
                x1, y1, x2, y2 = geo
                dx, dy = x2 - x1, y2 - y1
                L2 = dx * dx + dy * dy or 1e-9
                t = max(0.0, min(1.0, ((p[0] - x1) * dx + (p[1] - y1) * dy) / L2))
                d = math.hypot(p[0] - (x1 + dx * t), p[1] - (y1 + dy * t))
            if d < bd:
                bd, best = d, blk
    return best


def cell(p, lname, isrf):
    lid = board.GetLayerID(lname)
    if lid not in LI or isrf:                         # In1 是参考面；射频一律钉在 F.Cu
        lid = pcbnew.F_Cu
    gx, gy = m2g(*p)
    if not (0 <= gx < NW and 0 <= gy < NH):
        return None
    return (gx, gy, LI[lid])


# ── rip-up：拆掉挡在逃逸路上的那几个对象 ───────────────────────────────
# 诊断结论：8 处布不通里，挡路的**焊盘类 0 项**，全是走线和过孔，而且每处只挡
# 1~2 个对象、差 0.019~0.425mm。所以"几何封死"是假的，拆掉这几个就通。
# 被拆的网络下一轮 DRC 会重新报成缺线，由本脚本重布——这就是 rip-up & reroute。
#
# 不拆的：RF50（射频布线是手工调过的，重布风险远大于收益）、以及本网络自己的铜。
def collect_blockers(nc, netname, w, my_clr, blk, isrf):
    """返回挡在这块焊盘逃逸路上的对象清单（已排除不可拆的）"""
    if isrf or any(k != "pad" for k, _, _ in blk):
        return []
    pads, trks, vias = snap_obstacles()
    best = None
    for _, box, ls in blk:
        cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
        rec = min((r for r in pad_index() if r[0] == nc),
                  key=lambda r: math.hypot(r[1][0] - cx, r[1][1] - cy), default=None)
        if rec is None or math.hypot(rec[1][0] - cx, rec[1][1] - cy) > 0.1:
            continue
        px, py = rec[1]
        x0, y0, x1_, y1_ = rec[2]
        fc = rec[3]
        li = rec[4][0] if rec[4] else 0
        if (y1_ - y0) >= (x1_ - x0):
            d = (0.0, 1.0 if py - fc[1] >= 0 else -1.0)
            start = (px, y1_ if py - fc[1] >= 0 else y0)
        else:
            d = (1.0 if px - fc[0] >= 0 else -1.0, 0.0)
            start = (x1_ if px - fc[0] >= 0 else x0, py)
        for L in (0.6, 0.8, 1.0, 1.3, 1.6):
            ex, ey = start[0] + d[0] * L, start[1] + d[1] * L
            gx, gy = m2g(ex, ey)
            if not (0 <= gx < NW and 0 <= gy < NH):
                continue
            hard, soft = 0, []
            for onc, ols, bx, oclr in pads:
                if onc == nc or li not in ols:
                    continue
                if seg_rect_dist(px, py, ex, ey, bx) < w / 2 + max(oclr, my_clr):
                    hard += 1                      # 焊盘拆不了，这个 L 直接废
            for onc, oli, g, oh, oclr in trks:
                if onc == nc or oli != li:
                    continue
                if seg_seg_dist((px, py), (ex, ey), (g[0], g[1]), (g[2], g[3])) \
                        < w / 2 + oh + max(oclr, my_clr):
                    nm = NETNAME.get(onc, "")
                    if nm in RF:
                        hard += 1                  # 射频线不拆
                    else:
                        soft.append({"kind": "trk", "net": nm, "layer": oli,
                                     "geo": [round(v, 4) for v in g]})
            for onc, p, oclr, orad in vias:
                if onc == nc:
                    continue
                if seg_seg_dist((px, py), (ex, ey), p, p) < w / 2 + orad + max(oclr, my_clr):
                    nm = NETNAME.get(onc, "")
                    if nm in RF:
                        hard += 1
                    else:
                        soft.append({"kind": "via", "net": nm, "layer": -1,
                                     "geo": [round(p[0], 4), round(p[1], 4)]})
            if hard:
                continue
            if best is None or len(soft) < len(best[1]):
                best = (rec[5], soft)
    return [] if best is None else best[1]


NETNAME = {}


def strip(plan_path):
    """按 ripup 清单删对象。必须独立进程跑：board.Remove() 会让本进程内所有
    SWIG 代理退化成裸 SwigPyObject（route_maze.py:56 记过同一个坑）。"""
    want = json.load(open(plan_path))
    tv = {(o["net"], tuple(o["geo"])) for o in want if o["kind"] == "via"}
    tt = {(o["net"], o["layer"], tuple(o["geo"])) for o in want if o["kind"] == "trk"}
    n = 0
    for t in list(board.GetTracks()):
        if isinstance(t, pcbnew.PCB_VIA):
            p = t.GetPosition()
            key = (t.GetNetname(), (round(pcbnew.ToMM(p.x), 4), round(pcbnew.ToMM(p.y), 4)))
            if key in tv:
                board.Remove(t)
                n += 1
        elif t.GetLayer() in LI:
            g = (round(pcbnew.ToMM(t.GetStart().x), 4), round(pcbnew.ToMM(t.GetStart().y), 4),
                 round(pcbnew.ToMM(t.GetEnd().x), 4), round(pcbnew.ToMM(t.GetEnd().y), 4))
            if (t.GetNetname(), LI[t.GetLayer()], g) in tt:
                board.Remove(t)
                n += 1
    print(f"拆掉 {n} 个挡路对象（清单 {len(want)} 项）")
    board.Save(PCB)
    print("saved:", PCB)


PADIDX = None
RIPUP = []
PENDING = []


def pad_index():
    """(网络, 中心) → (焊盘 bbox, 封装中心, 所在层, refdes.pin)"""
    global PADIDX
    if PADIDX is None:
        PADIDX = []
        for f in board.GetFootprints():
            fc = f.GetPosition()
            for p in f.Pads():
                bb = p.GetBoundingBox()
                pp = p.GetPosition()
                PADIDX.append((p.GetNetCode(),
                               (pcbnew.ToMM(pp.x), pcbnew.ToMM(pp.y)),
                               (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                                pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())),
                               (pcbnew.ToMM(fc.x), pcbnew.ToMM(fc.y)),
                               [LI[l] for l in LAYERS_ALL if p.IsOnLayer(l)],
                               f"{f.GetReference()}.{p.GetNumber()}"))
    return PADIDX


def try_escape(blk, nc, net, w, my_clr, isrf):
    """给"纯裸焊盘"的连通块拉逃逸 stub。返回新增段数（0 = 没做或做不成）。"""
    if any(k != "pad" for k, _, _ in blk):
        return 0, None
    added = 0
    tag = None
    for _, box, ls in blk:
        cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
        rec = min((r for r in pad_index() if r[0] == nc),
                  key=lambda r: math.hypot(r[1][0] - cx, r[1][1] - cy), default=None)
        if rec is None or math.hypot(rec[1][0] - cx, rec[1][1] - cy) > 0.1:
            continue
        li = 0 if isrf else (ls[0] if ls else 0)
        st = escape_stub(nc, rec[1], rec[3], rec[2], li, w / 2, my_clr)
        if not st:
            continue
        for x1, y1, x2, y2 in st:
            PENDING.append((li, x1, y1, x2, y2, w, net, nc))
            added += 1
        tag = rec[5]
    return added, tag


def commit_stubs():
    """A* 成功后才把 stub 真正落到板上，并计入占用图。"""
    global OBST
    for li, x1, y1, x2, y2, w, net, nc in PENDING:
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I_MM(round(x1, 4), round(y1, 4)))
        t.SetEnd(pcbnew.VECTOR2I_MM(round(x2, 4), round(y2, 4)))
        t.SetWidth(pcbnew.FromMM(w))
        t.SetLayer(LAYERS_ALL[li])
        t.SetNet(net)
        board.Add(t)
        add_seg(li, x1, y1, x2, y2, w / 2, nc, clr_of(net.GetNetname()))
    n = len(PENDING)
    PENDING.clear()
    if n:
        OBST = None                     # stub 也是障碍，下一条得看见它
    return n


def stub_cells(w, records=None):
    """未提交的 stub 覆盖的栅格——直接并进 A* 起点集，不用先落盘"""
    out = set()
    for li, x1, y1, x2, y2, _w, _net, _nc in (PENDING if records is None else records):
        n = max(2, int(math.hypot(x2 - x1, y2 - y1) / (GRID * 0.5)))
        for i in range(n + 1):
            gx, gy = m2g(x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n)
            if 0 <= gx < NW and 0 <= gy < NH:
                out.add((gx, gy, li))
    return out


def append_endpoint_snap(block, node, net, nc, width, my_clr):
    """把栅格端点精确接回真实铜，避免“规划成功、落盘仍断开”。"""
    gx, gy, li = node[:3]
    target = g2m(gx, gy)
    augmented = list(block)
    for pli, x1, y1, x2, y2, _w, _net, pnc in PENDING:
        if pnc == nc:
            augmented.append(("trk", (x1, y1, x2, y2), [pli]))
    anchor = closest_point_on_block(augmented, target, li)
    if anchor is None:
        return False
    if math.hypot(anchor[0] - target[0], anchor[1] - target[1]) < 1e-3:
        return True
    ok, _ = stub_clear(li, anchor[0], anchor[1], target[0], target[1],
                       width / 2, nc, my_clr)
    if not ok:
        return False
    PENDING.append((li, anchor[0], anchor[1], target[0], target[1], width, net, nc))
    return True


def main():
    import time
    t0 = time.time()
    NETNAME.update({n.GetNetCode(): n.GetNetname() for n in board.GetNetsByNetcode().values()})
    npad, ntrk, nvia, nko = build()
    print(f"占用图：{npad} 焊盘 / {ntrk} 走线(线段光栅化) / {nvia} 过孔 / {nko} 禁布区"
          f"  {NW}×{NH}×{NL}  用时 {time.time()-t0:.1f}s")

    need, orphan = load_jobs()
    print(f"\n待布 {len(need)} 处真缺线（另有 {len(orphan)} 处孤立断头铜，走 clean 模式）\n")

    ok = fail = tot_s = tot_v = 0
    for net, (p0, l0), (p1, l1) in sorted(need, key=lambda j: j[0]):
        isrf = net in RF
        cls = net_class(net)
        w, _ = PROFILE[cls]
        si = SI[cls]
        WIDTHS = [w] if cls == "RF50" else [w, 0.2, 0.15]   # 射频线宽定阻抗，不许退
        co = clr_of(net)
        nc = board.FindNet(net).GetNetCode()
        blks = blocks_of(nc)
        ba, bb_ = near_block(blks, p0, l0), near_block(blks, p1, l1)
        GL = [] if isrf else zone_layers(nc)        # 射频禁过孔，够不着平面
        if GL and ba is not None and bb_ is not None:
            # 谁没压在平面铜上，谁当起点——它才是那块"没扇出的裸焊盘"，
            # 就地下钻接平面即可，不必横穿半块板去够对面。
            if block_on_plane(GL, ba) and not block_on_plane(GL, bb_):
                ba, bb_ = bb_, ba
            elif block_on_plane(GL, ba) and block_on_plane(GL, bb_):
                GL = []                             # 两块都已接平面，那是别的问题，老实拉线
        if ba is None or bb_ is None or ba is bb_:
            print(f"  {net:16s} ❌ 端点定位不到两个不同连通块（{len(blks)} 块）")
            fail += 1
            continue
        # 裸焊盘先拉逃逸 stub 出引脚走廊，再交给栅格 A*
        PENDING.clear()
        esc = []
        endpoint_stubs = []
        for blk in (ba, bb_):
            pending_start = len(PENDING)
            n_, tg = try_escape(blk, nc, board.FindNet(net), w, PROFILE[cls][1], isrf)
            endpoint_stubs.append(list(PENDING[pending_start:]))
            if n_:
                esc.append(tg)
        ca, cb = block_cells(ba), block_cells(bb_)
        # 每个逃逸 stub 只属于创建它的那一个端点。旧实现按“距块 <12 格”分配，
        # 当 pad 与对侧铜只隔 0.6mm 时，同一 stub 会同时混入 ca/cb，A* 立刻以
        # 1 节点/0 过孔假成功，实际 F.Cu↔In2 从未连接。
        ca |= stub_cells(w, endpoint_stubs[0])
        cb |= stub_cells(w, endpoint_stubs[1])
        if isrf:                       # 射频钉死 F.Cu：起终点只留 F.Cu 上的格子
            ca = {c for c in ca if c[2] == 0} or ca
            cb = {c for c in cb if c[2] == 0} or cb
        # 起点块自身的格子必须放行：它们多半被自己的扇出铜盖着（cnt>1），
        # 不放行的话第一步就出不去。只放行块内格，不动块外，不会穿别人的铜。
        FREE.clear()
        for gx, gy, li in list(ca) + list(cb):
            FREE.add((li, gy * NW + gx))
        t1 = time.time()
        path, pops = None, 0
        for wtry in WIDTHS:
            si_try = si if wtry == w else SI["Default"]
            path, pops = astar(ca, cb, nc, not isrf, si_try, goal_layers=GL)
            if path:
                w = wtry
                break
        dt = time.time() - t1
        if not path:
            PENDING.clear()             # 丢弃没用上的 stub，不留悬空线头
            bl = []
            for blk in (ba, bb_):
                bl += collect_blockers(nc, net, w, PROFILE[cls][1], blk, isrf)
            RIPUP.extend(bl)
            print(f"  {'RF ' if isrf else '   '}{net:16s} ❌ 走不通  "
                  f"({cls} w={w} 块{len(ca)}/{len(cb)}格 {pops} 节点 {dt:.1f}s)"
                  + (f"  → 可拆 {len(bl)} 项: {[b['net'] for b in bl]}" if bl else "  → 无可拆项"))
            fail += 1
            continue
        path = trim(path, ca, cb if not GL else set())
        if not (append_endpoint_snap(ba, path[0], board.FindNet(net), nc, w,
                                     PROFILE[cls][1])
                and append_endpoint_snap(bb_, path[-1], board.FindNet(net), nc, w,
                                         PROFILE[cls][1])):
            PENDING.clear()
            print(f"  {'RF ' if isrf else '   '}{net:16s} ⚠️  栅格端点无法安全吸附真实铜，弃用")
            fail += 1
            continue
        r_ = emit(path, board.FindNet(net), w, nc, co, PROFILE[cls][1])
        if r_ is None:
            PENDING.clear()
            print(f"  {'RF ' if isrf else '   '}{net:16s} ⚠️  几何复核不过，弃用（避免引入 DRC 违例）")
            fail += 1
            continue
        tot_s += commit_stubs()
        s_, v_ = r_
        tot_s += s_
        tot_v += v_
        ok += 1
        print(f"  {'RF ' if isrf else '   '}{net:16s} ✅ {s_:2d} 段 {v_} 过孔  ({cls} w={w} {pops} 节点 {dt:.1f}s)")

    print(f"\n补上 {ok} 处 / 失败 {fail} 处；新增 {tot_s} 段 {tot_v} 过孔  总用时 {time.time()-t0:.1f}s")

    if RIPUP:
        seen, uniq = set(), []
        for o in RIPUP:
            k = (o["kind"], o["net"], o["layer"], tuple(o["geo"]))
            if k not in seen:
                seen.add(k)
                uniq.append(o)
        json.dump(uniq, open(RIPUP_OUT, "w"))
        print(f"\nrip-up 清单 {len(uniq)} 项 → {RIPUP_OUT}")

    if MODE == "apply":
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        board.Save(PCB)
        print("saved:", PCB)
    else:
        print("（plan 模式，未写盘）")


def clean():
    """删碎铜。两类都要清：
      · unconnected_items 里的 Track↔Track / Track↔Via —— 没接上任何焊盘的孤立铜
      · violations 里的 track_dangling —— 有一头悬空的线（rip-up 拆完的残端、
        以及逃逸 stub 拉出去却没接上下文的那一截）
    dangling 按 uuid 删，最准，不会误伤同坐标的邻线。"""
    d = json.load(open(DRC))
    _, orphan = load_jobs()
    kill = set()
    for net, (p0, _), (p1, _) in orphan:
        kill.add((round(p0[0], 3), round(p0[1], 3)))
        kill.add((round(p1[0], 3), round(p1[1], 3)))
    dang = {i["uuid"] for v in d["violations"] if v["type"] == "track_dangling"
            for i in v["items"]}
    n = m = 0
    for t in list(board.GetTracks()):
        if t.m_Uuid.AsString() in dang:
            board.Remove(t)
            m += 1
            continue
        if isinstance(t, pcbnew.PCB_VIA):
            continue
        a = (round(pcbnew.ToMM(t.GetStart().x), 3), round(pcbnew.ToMM(t.GetStart().y), 3))
        b = (round(pcbnew.ToMM(t.GetEnd().x), 3), round(pcbnew.ToMM(t.GetEnd().y), 3))
        if a in kill or b in kill:
            board.Remove(t)
            n += 1
    print(f"删掉 {n} 段孤立断头铜 + {m} 段悬空线头")
    board.Save(PCB)
    print("saved:", PCB)


if __name__ == "__main__":
    if MODE == "strip":
        strip(DRC)
    elif MODE == "clean":
        clean()
    else:
        main()
