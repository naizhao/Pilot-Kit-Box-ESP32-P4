#!/usr/bin/env python3
"""GNSS 切换子系统布线器（v3 天线升级配套，A2 收拢布局）。

分工：
- INT/EXT 偏置链手布折点（A* 在此拥挤域顺序死锁；折点按实测 pad/障碍推演）
- SW2_J*/GNSS_RF_IN/ANT_SEL/3V3 等其余 12 网走 0.05mm 整数格双层 A*
  （F.Cu+In2 经 via，RF 网强制单层 F.Cu；障碍=异网铜+封装禁铜区）
- 链式单锚（每段从上一目标出发，无分叉断头）
"""
import heapq
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

G = 0.05
XI0, XI1, J0, J1 = 1006, 1336, 1192, 1592
CLEAR = 0.26      # 线到铜 0.25+余量
PAD_CLR = 0.27   # 孔到铜 0.25+余量
VIA_COST = 40
LAYERS = (pcbnew.F_Cu, pcbnew.In2_Cu)

board = pcbnew.LoadBoard(PCB)

# ── 幂等头: GNSS 域网络的线/via 全删(快照+手布双份孔/线的根因)。
# board.Remove 毒化同进程全部 SWIG 代理(含重载对象) → 删完只 Save,删挪子进程。
import subprocess as _sp
_r = _sp.run([__import__("sys").executable, "-c", f"""
import pcbnew
b = pcbnew.LoadBoard({PCB!r})
N = {{"ANT_GNSS_INT", "ANT_GNSS_EXT", "GNSS_RF_IN", "SW2_J1", "SW2_J2", "SW2_J3",
     "GNSS_INT_FEED", "GNSS_INT_FUSE", "GNSS_EXT_FEED", "GNSS_EXT_FUSE"}}
# 3V3 单独限左条 x<77.5(U3/R17/Q2 偏置群 x>77 靠快照独占)
# ANT_SEL_A/B/SW1_J2 不 purge: 成果已固化快照独占, 不再手布重放
n = 0
for t in list(b.GetTracks()):
    net = t.GetNetname()
    if net in N:
        b.Remove(t); n += 1
    elif net == "3V3_GNSS":
        q = t.GetStart()
        if pcbnew.ToMM(q.x) < 77.5:
            b.Remove(t); n += 1
b.Save({PCB!r})
print("purge", n)
"""], capture_output=True, text=True)
if _r.returncode == 0 and "purge" in _r.stdout:
    board = pcbnew.LoadBoard(PCB)   # 本进程未 Remove, 代理健康
_codes = board.GetNetInfo().NetsByNetcode()
nets = {_codes[c].GetNetname(): _codes[c] for c in _codes}


def pad_ij(ref, num):
    f = board.FindFootprintByReference(ref)
    assert f, ref
    p = f.FindPadByNumber(str(num))
    q = p.GetPosition()
    return int(round(pcbnew.ToMM(q.x) / G)), int(round(pcbnew.ToMM(q.y) / G))


def _stamp(obs, ci, cj, ri, rj):
    for i in range(ci - ri, ci + ri + 1):
        for j in range(cj - rj, cj + rj + 1):
            obs.add((i, j))


def build_obstacles(skip_net):
    obs = [set(), set()]
    via_obs = [set(), set()]   # via 版: 障碍膨胀 0.075/0.15, 供换层孔位检查
    for f in board.GetFootprints():
        for p in f.Pads():
            nm = p.GetNetname()
            if nm in ("", skip_net):
                continue
            q = p.GetPosition()
            w = pcbnew.ToMM(p.GetSize().x) / 2 + PAD_CLR
            h = pcbnew.ToMM(p.GetSize().y) / 2 + PAD_CLR
            ci, cj = int(pcbnew.ToMM(q.x) / G), int(pcbnew.ToMM(q.y) / G)
            if p.IsOnLayer(pcbnew.F_Cu):
                _stamp(obs[0], ci, cj, int(w / G) + 1, int(h / G) + 1)
                _stamp(via_obs[0], ci, cj, int((w + 0.15) / G) + 1, int((h + 0.15) / G) + 1)
            if p.IsOnLayer(pcbnew.In2_Cu):
                _stamp(obs[1], ci, cj, int(w / G) + 1, int(h / G) + 1)
                _stamp(via_obs[1], ci, cj, int((w + 0.15) / G) + 1, int((h + 0.15) / G) + 1)
        for z in f.Zones():
            bb = z.GetBoundingBox()
            tgt = obs[0] if z.GetLayer() == pcbnew.F_Cu else obs[1]
            for i in range(int(pcbnew.ToMM(bb.GetLeft()) / G), int(pcbnew.ToMM(bb.GetRight()) / G) + 1):
                for j in range(int(pcbnew.ToMM(bb.GetTop()) / G), int(pcbnew.ToMM(bb.GetBottom()) / G) + 1):
                    tgt.add((i, j))
    for t in board.GetTracks():
        nm = t.GetNetname()
        if t.GetClass() == "PCB_VIA":
            if nm == skip_net:
                continue
            q = t.GetPosition()
            r = pcbnew.ToMM(t.GetBoundingBox().GetWidth()) / 2 + PAD_CLR
            ci, cj = int(pcbnew.ToMM(q.x) / G), int(pcbnew.ToMM(q.y) / G)
            rr = int(r / G) + 1
            _stamp(obs[0], ci, cj, rr, rr)
            _stamp(obs[1], ci, cj, rr, rr)
            _stamp(via_obs[0], ci, cj, int((r + 0.075) / G) + 1, int((r + 0.075) / G) + 1)
            _stamp(via_obs[1], ci, cj, int((r + 0.075) / G) + 1, int((r + 0.075) / G) + 1)
            continue
        if nm == skip_net:
            continue
        for li, lay in enumerate(LAYERS):
            if not t.IsOnLayer(lay):
                continue
            s, e = t.GetStart(), t.GetEnd()
            w = pcbnew.ToMM(t.GetWidth()) / 2 + CLEAR
            i1, j1 = int(pcbnew.ToMM(s.x) / G), int(pcbnew.ToMM(s.y) / G)
            i2, j2 = int(pcbnew.ToMM(e.x) / G), int(pcbnew.ToMM(e.y) / G)
            di, dj = i2 - i1, j2 - j1
            steps = max(abs(di), abs(dj))
            rr = int(w / G) + 1
            for k in range(steps + 1):
                f_ = k / max(steps, 1)
                ci, cj = i1 + di * f_, j1 + dj * f_
                for a in range(int(ci) - rr, int(ci) + rr + 1):
                    for b in range(int(cj) - rr, int(cj) + rr + 1):
                        if (a - ci) ** 2 + (b - cj) ** 2 <= (w / G) ** 2 * 1.1:
                            obs[li].add((a, b))
                rr2 = int((w + 0.075) / G) + 1
                for a in range(int(ci) - rr2, int(ci) + rr2 + 1):
                    for b in range(int(cj) - rr2, int(cj) + rr2 + 1):
                        if (a - ci) ** 2 + (b - cj) ** 2 <= ((w + 0.075) / G) ** 2 * 1.1:
                            via_obs[li].add((a, b))
    return obs, via_obs


def astar(starts, goal, obs, ext, single=False, via_obs=None):
    ihi = (2640 if ext == 2 else 1640) if ext else XI1   # ext=2 全域 x132(U8/R24 远端)
    # 走廊式豁免: goal ±1 + 沿 4 正交方向逐格穿透障碍直到自由格(QFN/0402 邻 pad
    # 障碍厚 0.3-0.7mm, ±1 格(0.05) 穿不透 → goal 中心被围死)
    gs = {goal, (goal[0] + 1, goal[1]), (goal[0] - 1, goal[1]),
          (goal[0], goal[1] + 1), (goal[0], goal[1] - 1)}
    if via_obs is None:
        via_obs = obs
    for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        ci, cj = goal[0] + di, goal[1] + dj
        for _ in range(30):                     # 最多穿 1.5mm
            gs.add((ci, cj))
            if ((ci, cj) not in obs[0] and (ci, cj) not in obs[1]
                    and (ci, cj) not in via_obs[0] and (ci, cj) not in via_obs[1]):
                break
            ci, cj = ci + di, cj + dj
    openq = []
    for s in starts:
        heapq.heappush(openq, (abs(s[0] - goal[0]) + abs(s[1] - goal[1]), 0, s[0], s[1], 0, None))
    came, gsc = {}, {(s[0], s[1], 0): 0 for s in starts}
    while openq:
        _, gc, i, j, l, parent = heapq.heappop(openq)
        cur = (i, j, l)
        if cur in came:
            continue
        came[cur] = parent
        if (i, j) in gs:
            path = [cur]
            while came[path[-1]] is not None:
                path.append(came[path[-1]])
            if path[0][:2] != goal:
                path.insert(0, (goal[0], goal[1], path[0][2]))
            return path[::-1]
        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nxt = (i + di, j + dj, l)
            if not (XI0 <= nxt[0] <= ihi and J0 <= nxt[1] <= J1):
                continue
            if nxt[:2] in obs[l] and nxt[:2] not in gs:
                continue
            turn = 0
            if parent is not None and (i - parent[0], j - parent[1]) != (di, dj) and parent[2] == l:
                turn = 8
            ng = gc + 1 + turn
            if ng < gsc.get(nxt, 1 << 30):
                gsc[nxt] = ng
                heapq.heappush(openq, (ng + abs(nxt[0] - goal[0]) + abs(nxt[1] - goal[1]), ng, nxt[0], nxt[1], nxt[2], cur))
        if not single:
            nxt = (i, j, 1 - l)
            vo = via_obs or obs
            # via 落孔不享 goal 豁免: 孔是通孔,打穿全部层,走廊豁免只该给走线
            if (nxt[:2] in vo[0]) or (nxt[:2] in vo[1]):
                pass
            else:
                ng = gc + VIA_COST
                if ng < gsc.get(nxt, 1 << 30):
                    gsc[nxt] = ng
                    heapq.heappush(openq, (ng + abs(i - goal[0]) + abs(j - goal[1]), ng, i, j, 1 - l, cur))
    return None


def commit(net, path, w):
    ni = nets[net]
    n = 0
    for (i1, j1, l1), (i2, j2, l2) in zip(path, path[1:]):
        if l1 != l2:
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(i1 * G), pcbnew.FromMM(j1 * G)))
            v.SetViaType(pcbnew.VIATYPE_THROUGH)
            v.SetWidth(pcbnew.FromMM(0.45)); v.SetDrill(pcbnew.FromMM(0.3))
            v.SetNet(ni)
            board.Add(v)
            n += 1
            continue
        if (i1, j1) == (i2, j2):
            continue
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(i1 * G), pcbnew.FromMM(j1 * G)))
        t.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(i2 * G), pcbnew.FromMM(j2 * G)))
        t.SetWidth(pcbnew.FromMM(w))
        t.SetLayer(LAYERS[l1])
        t.SetNet(ni)
        board.Add(t)
        n += 1
    return n


def route_net(net, padlist, w, ext=0, single=False):
    if net in FAILED:
        return
    cur = pad_ij(*padlist[0])
    total = 0
    for tgt_p in padlist[1:]:
        tgt = pad_ij(*tgt_p)
        obs, via_obs = build_obstacles(net)
        st = [(cur[0], cur[1], 0), (cur[0] + 1, cur[1], 0), (cur[0] - 1, cur[1], 0),
              (cur[0], cur[1] + 1, 0), (cur[0], cur[1] - 1, 0)]
        path = astar(st, tgt, obs, ext, single, via_obs)
        if not path:
            FAILED.append(net)
            print(f"  {net:18s} ✗ 无路 @ 段 {tgt_p} cur={cur}")
            return
        if path[0][:2] != cur:
            path = [(cur[0], cur[1], path[0][2])] + list(path)
        simp = [path[0]]
        for k in range(1, len(path) - 1):
            d1 = (path[k][0] - path[k - 1][0], path[k][1] - path[k - 1][1])
            d2 = (path[k + 1][0] - path[k][0], path[k + 1][1] - path[k][1])
            if d1 != d2 or path[k][2] != path[k - 1][2]:
                simp.append(path[k])
        simp.append(path[-1])
        total += commit(net, simp, w)
        cur = tgt
    print(f"  {net:18s} {total} 段")


def manual_route(net, w, pts, layer=pcbnew.F_Cu):
    n = 0
    for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(x1), pcbnew.FromMM(y1)))
        t.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x2), pcbnew.FromMM(y2)))
        t.SetWidth(pcbnew.FromMM(w))
        t.SetLayer(layer)
        t.SetNet(nets[net])
        board.Add(t)
        n += 1
    return n


def mvia(net, x, y):
    for t in board.GetTracks():               # 去重: 快照已有同网 0.06 内孔则跳过
        if t.GetClass() == "PCB_VIA" and t.GetNetname() == net:
            q = t.GetPosition()
            if abs(pcbnew.ToMM(q.x) - x) < 0.06 and abs(pcbnew.ToMM(q.y) - y) < 0.06:
                return
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
    v.SetViaType(pcbnew.VIATYPE_THROUGH)
    v.SetWidth(pcbnew.FromMM(0.45))
    v.SetDrill(pcbnew.FromMM(0.3))
    v.SetNet(nets[net])
    board.Add(v)


FAILED = []

NET_SHORT = [
]
for net, pads, kw in NET_SHORT:
    route_net(net, pads, **kw)

# ── 手布(实测 pad: U17.5(59.64,63.3);C57@270°: C57.1 下(63.4,62.82)/C57.2 上(63.4,63.78);
#    L2@90°: L2.1 上(61.8,65.285)/L2.2 下(61.8,64.315);L15@90°: L15.1 上(58,67.87)/L15.2 下(58,66.93)) ──
manual_route("ANT_GNSS_INT", 0.34, [(52.900, 63.075), (52.900, 63.100), (57.050, 63.100)])
manual_route("ANT_GNSS_INT", 0.34, [(55.500, 63.100), (55.500, 62.400), (55.830, 62.400)])
# INT 分支: 竖干北上 y67.0 → 西端 L15.2 底进
mvia("ANT_GNSS_INT", 56.900, 63.100)
manual_route("ANT_GNSS_INT", 0.34, [(56.900, 63.100), (56.900, 67.000)], layer=pcbnew.In2_Cu)
mvia("ANT_GNSS_INT", 56.900, 67.000)
manual_route("ANT_GNSS_INT", 0.34, [(56.900, 67.000), (58.000, 67.000), (58.000, 67.130)])
# EXT: L2.2 下出→y64.6 西行(穿 U17.3 障碍上缝)→x58.6 竖干→y66.4 主横线西延 54.2;C58.1 东 L 接入;J2 支线南下
manual_route("ANT_GNSS_EXT", 0.34, [(61.800, 64.315), (61.800, 64.600), (58.800, 64.600),
                                     (58.800, 66.400), (54.200, 66.400)])
manual_route("ANT_GNSS_EXT", 0.34, [(56.500, 65.070), (56.500, 66.300)])
manual_route("ANT_GNSS_EXT", 0.34, [(54.200, 66.300), (54.200, 72.975), (52.800, 72.975)])
# EXT_FEED: L2.1 上出→y65.7 东行→F4.2 右上进
manual_route("GNSS_EXT_FEED", 0.34, [(61.800, 65.285), (61.800, 65.800), (63.300, 65.800),
                                      (63.300, 67.400), (62.740, 67.400)])
# INT_FEED: L15.1 上出→y68.5 西行→F5.2 顶进
manual_route("GNSS_INT_FEED", 0.34, [(58.000, 68.070), (58.000, 68.500), (57.340, 68.500),
                                      (57.340, 69.700)])
# GNSS_RF_IN: C57.1 下出→y62.95 东行(U7.10/12 缝内)→U7.11 左进
manual_route("GNSS_RF_IN", 0.34, [(63.400, 62.820), (63.400, 62.950), (65.100, 62.950),
                                   (65.100, 63.100), (65.550, 63.100)])
# SW2_J1: U17.5 西行→x62.7 北上→C57.2 左进(避 C57.1 障碍)
manual_route("SW2_J1", 0.15, [(59.640, 63.300), (62.700, 63.300), (62.700, 63.780), (63.400, 63.780)])
manual_route("SW2_J2", 0.15, [(57.960, 63.950), (57.500, 63.950), (57.500, 63.900),
                               (56.500, 63.900), (56.500, 64.130)])
manual_route("SW2_J3", 0.15, [(57.960, 62.650), (57.960, 62.450), (56.770, 62.450), (56.770, 62.400)])

NET_TASKS = []   # A/B/SW1_J2 已固化快照(含via), 重放=双份孔
for net, pads, kw in NET_TASKS:
    route_net(net, pads, **kw)

if FAILED:
    print(f"✗ 失败: {FAILED}")
    sys.exit(2)

nv = 0
for ref in ("J2", "J8"):
    f = board.FindFootprintByReference(ref)
    for p in f.Pads():
        if p.GetNetname() == "GND":
            q0 = p.GetPosition()
            _dup = any(t.GetClass() == "PCB_VIA" and t.GetNetname() == "GND"
                       and abs(pcbnew.ToMM(t.GetPosition().x) - pcbnew.ToMM(q0.x)) < 0.06
                       and abs(pcbnew.ToMM(t.GetPosition().y) - pcbnew.ToMM(q0.y)) < 0.06
                       for t in board.GetTracks())
            if _dup:
                continue
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(p.GetPosition())
            v.SetViaType(pcbnew.VIATYPE_THROUGH)
            v.SetWidth(pcbnew.FromMM(0.45)); v.SetDrill(pcbnew.FromMM(0.3))
            v.SetNet(nets["GND"])
            board.Add(v)
            nv += 1
print(f"  GND 地孔 ×{nv}")

c53_2 = pad_ij("C53", 2)
for (x1, y1), (x2, y2) in (((110.90, 61.62), (c53_2[0] * G, 61.62)),
                           ((c53_2[0] * G, 61.62), (c53_2[0] * G, c53_2[1] * G))):
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(x1), pcbnew.FromMM(y1)))
    t.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x2), pcbnew.FromMM(y2)))
    t.SetWidth(pcbnew.FromMM(0.15))
    t.SetLayer(pcbnew.F_Cu)
    t.SetNet(nets["SW1_J3"])
    board.Add(t)
print("  SW1_J3→C53.2 补线 2 段")

# 3V3 主干:U7 右缘 F 段→via 下 In2 竖穿(串口线在 F)→y72.5 横→Q4.2
manual_route("3V3_GNSS", 0.25, [(75.250, 63.100), (76.300, 63.100), (76.300, 66.400)])
mvia("3V3_GNSS", 76.300, 66.400)
manual_route("3V3_GNSS", 0.25, [(76.300, 66.400), (76.300, 72.500)], layer=pcbnew.In2_Cu)
mvia("3V3_GNSS", 76.300, 72.500)
manual_route("3V3_GNSS", 0.25, [(76.300, 72.500), (61.450, 72.500), (61.450, 71.150)])
manual_route("3V3_GNSS", 0.25, [(61.450, 72.500), (61.450, 73.300)])
manual_route("3V3_GNSS", 0.25, [(61.450, 73.300), (61.450, 73.550), (56.860, 73.550)])
manual_route("3V3_GNSS", 0.25, [(60.800, 73.550), (60.800, 77.350), (58.500, 77.350),
                                 (58.260, 76.400)])
manual_route("3V3_GNSS", 0.25, [(75.250, 66.400), (76.300, 66.400)])
manual_route("3V3_GNSS", 0.25, [(76.300, 63.400), (79.850, 63.400), (79.850, 64.600),
                                 (79.850, 66.200), (77.825, 66.200), (77.825, 67.000)])
# GNSS_INT_FUSE: F5.1 西出→x55.0 南下→y72.6 东行进 Q5.3
manual_route("GNSS_INT_FUSE", 0.25, [(55.460, 69.700), (54.700, 69.700)])
mvia("GNSS_INT_FUSE", 55.000, 69.700)
manual_route("GNSS_INT_FUSE", 0.25, [(55.000, 69.700), (55.000, 72.600), (58.750, 72.600)],
             layer=pcbnew.In2_Cu)
mvia("GNSS_INT_FUSE", 58.750, 72.600)
# GNSS_EXT_FUSE: F4.1 下出→y68.65 东行→x62.05 南下进 Q4.3
manual_route("GNSS_EXT_FUSE", 0.25, [(60.862, 67.400), (60.862, 68.650), (62.050, 68.650),
                                     (62.050, 70.200), (62.740, 70.200)])
# ANT_SEL_A v21: Q5.1 西出(x54.6 避 INT_FUSE 竖线)→R26.2;U17.4 分支经 via/In2
# ANT_SEL_B v21: Q4.1 左出(x59.95 绕 F4.1 下)→y67.9 东→x60.55 长竖北上 U17.6(全 F.Cu 无需 via)
# ANT_SEL_B R27.2 分支: Q4.1 左出 x60.4 南下→y74.2 东绕→R27.2 右进
# SW1_J2 v21c: C30.2 经 U16 上方绕(避 V1/SW1_J3 竖线);U16.3 下出经 x112.9 南下 R24.1
# A/B 的 U8 衔接(老过带线已删,经 y73 空走廊南下)
# A 网远端负载 R23.1 分支(In2 直落)+Q2.2/C17 东延补接
manual_route("3V3_GNSS", 0.25, [(79.850, 66.200), (83.120, 66.200), (83.120, 67.610)])

# ── 断头清理:GNSS 域网络里两端均不接 pad/其他线的孤立段,迭代删除 ──
GN_DOMAIN = {"3V3_GNSS", "ANT_SEL_GNSS_A", "ANT_SEL_GNSS_B", "ANT_GNSS_INT",
             "ANT_GNSS_EXT", "SW2_J1", "SW2_J2", "SW2_J3", "GNSS_RF_IN",
             "GNSS_INT_FEED", "GNSS_INT_FUSE", "GNSS_EXT_FEED", "GNSS_EXT_FUSE", "SW1_J2"}
removed = 0
for _round in range(6):
    segs, pads, vias = {}, {}, {}
    for t in board.GetTracks():
        n = t.GetNetname()
        if t.GetClass() == "PCB_VIA" and n in GN_DOMAIN:
            q = t.GetPosition()
            vias.setdefault(n, []).append((pcbnew.ToMM(q.x), pcbnew.ToMM(q.y)))
        elif t.GetClass() == "PCB_TRACK" and n in GN_DOMAIN:
            st, en = t.GetStart(), t.GetEnd()
            segs.setdefault(n, []).append((t, (pcbnew.ToMM(st.x), pcbnew.ToMM(st.y)),
                                           (pcbnew.ToMM(en.x), pcbnew.ToMM(en.y))))
    for f in board.GetFootprints():
        for pp in f.Pads():
            if pp.GetNetname() in GN_DOMAIN:
                q = pp.GetPosition()
                pads.setdefault(pp.GetNetname(), []).append((pcbnew.ToMM(q.x), pcbnew.ToMM(q.y)))

    def _linked(pt, n, seg):
        for pd in pads.get(n, []):
            if abs(pd[0] - pt[0]) < 0.7 and abs(pd[1] - pt[1]) < 0.7:
                return True
        for vv in vias.get(n, []):
            if abs(vv[0] - pt[0]) < 0.45 and abs(vv[1] - pt[1]) < 0.45:
                return True
        for (_, a, b) in segs.get(n, []):
            if (a, b) == seg:
                continue
            for q in (a, b):
                if abs(q[0] - pt[0]) < 0.08 and abs(q[1] - pt[1]) < 0.08:
                    return True
        return False

    hit = False
    for n, lst in list(segs.items()):
        for seg in list(lst):
            t, a, b = seg
            if not _linked(a, n, seg) or not _linked(b, n, seg):
                board.Remove(t)
                lst.remove(seg)
                removed += 1
                hit = True
    if not hit:
        break
print(f"  断头清理 {removed} 段")

for t in list(board.GetTracks()):
    if t.GetClass() == "PCB_TRACK" and t.GetNetname() == "GND" and t.GetLayer() == pcbnew.F_Cu:
        st, en = t.GetStart(), t.GetEnd()
        y1, y2 = pcbnew.ToMM(st.y), pcbnew.ToMM(en.y)
        x1, x2 = pcbnew.ToMM(st.x), pcbnew.ToMM(en.x)
        if max(y1, y2) < 59.6 or (100 < min(x1, x2) and max(y1, y2) < 62.5):
            board.Remove(t)
board.Save(PCB)
print(f"GNSS 子系统布线完成 → {os.path.relpath(PCB, T)}")
