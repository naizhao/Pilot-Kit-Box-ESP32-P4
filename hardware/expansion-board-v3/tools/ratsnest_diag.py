#!/usr/bin/env python3
"""飞线体检——布局好不好，看飞线通不通，不看排得齐不齐。

评审的意见："电路板讲求流水，能否通畅就看水管走向歪没歪。原先脚本把所有元件按
网格排，但画 PCB 不是网格是血管，讲求头对脚、脚对头，而不是网格式的头对头、
脚对脚——那种除了好看一无是处。"

这个工具把"歪没歪"量化成两个指标（都在**布线之前**算，只看布局）：

1. **飞线长度**：每个网络的最小生成树(MST)总长。某条 MST 边特别长 = 这两个焊盘
   该靠近却离得老远，多半是元件摆的位置不对（不是朝向问题，朝向最多差一个元件长）。

2. **飞线交叉**：不同网络的 MST 边互相穿过 = 布局"打结"。两条线注定要抢同一个
   通道，布线器只能让一条过、另一条绕远或干脆布不通。交叉数是布局质量最直接的
   信号——交叉在布局阶段消掉，比在布线阶段折腾布线器有效得多。

用法：ratsnest_diag.py [top_n]
"""
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
TOP_N = int(sys.argv[1]) if len(sys.argv) > 1 else 20

# 覆铜网络不算飞线：它们靠平面 + 就近过孔连接，MST 长度没有参考意义
PLANE_NETS = {"GND", "3V3_DIG", "3V3_RF", "RP_1V1"}

board = pcbnew.LoadBoard(PCB)

nets = {}
for f in board.GetFootprints():
    ref = f.GetReference()
    for p in f.Pads():
        nm = p.GetNetname()
        if not nm or nm.startswith("unconnected") or nm in PLANE_NETS:
            continue
        pp = p.GetPosition()
        nets.setdefault(nm, []).append(
            (pcbnew.ToMM(pp.x), pcbnew.ToMM(pp.y), f"{ref}.{p.GetNumber()}"))


def mst(pts):
    """Prim 最小生成树，返回 [(长度, 起点, 终点)]。"""
    n = len(pts)
    if n < 2:
        return []
    used = [False] * n
    used[0] = True
    best = [(math.hypot(pts[i][0] - pts[0][0], pts[i][1] - pts[0][1]), 0)
            for i in range(n)]
    edges = []
    for _ in range(n - 1):
        k, bd = -1, None
        for i in range(n):
            if not used[i] and (bd is None or best[i][0] < bd):
                k, bd = i, best[i][0]
        if k < 0:
            break
        used[k] = True
        edges.append((best[k][0], pts[best[k][1]], pts[k]))
        for i in range(n):
            if not used[i]:
                d = math.hypot(pts[i][0] - pts[k][0], pts[i][1] - pts[k][1])
                if d < best[i][0]:
                    best[i] = (d, k)
    return edges


all_edges, total = [], 0.0
for nm, pts in nets.items():
    for L, a, b in mst(pts):
        all_edges.append((L, nm, a, b))
        total += L

print(f"飞线总长 {total:.1f}mm / {len(all_edges)} 条 / {len(nets)} 个网络"
      f"（已排除覆铜网 {sorted(PLANE_NETS)}）\n")

all_edges.sort(reverse=True)
print(f"── 最长的 {TOP_N} 条飞线（这两个焊盘该靠近却离得远 = 位置摆错）──")
for L, nm, a, b in all_edges[:TOP_N]:
    print(f"  {L:6.2f}mm  {nm:18s} {a[2]:>10s} ({a[0]:.1f},{a[1]:.1f})"
          f" ↔ {b[2]:<10s} ({b[0]:.1f},{b[1]:.1f})")


def seg_cross(p1, p2, p3, p4):
    """真交叉才算（共端点/共线不算）。"""
    def cr(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    d1, d2 = cr(p3, p4, p1), cr(p3, p4, p2)
    d3, d4 = cr(p1, p2, p3), cr(p1, p2, p4)
    return ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0))


cross = []
for i in range(len(all_edges)):
    L1, n1, a1, b1 = all_edges[i]
    for j in range(i + 1, len(all_edges)):
        L2, n2, a2, b2 = all_edges[j]
        if n1 == n2:
            continue
        if max(a1[0], b1[0]) < min(a2[0], b2[0]) or max(a2[0], b2[0]) < min(a1[0], b1[0]):
            continue
        if max(a1[1], b1[1]) < min(a2[1], b2[1]) or max(a2[1], b2[1]) < min(a1[1], b1[1]):
            continue
        if seg_cross(a1, b1, a2, b2):
            cross.append((L1 + L2, n1, a1, b1, n2, a2, b2))

cross.sort(reverse=True)
print(f"\n── 飞线交叉（布局打结）：{len(cross)} 处，最严重的 {min(TOP_N, len(cross))} 处 ──")
for s, n1, a1, b1, n2, a2, b2 in cross[:TOP_N]:
    print(f"  {n1:16s} {a1[2]}↔{b1[2]}")
    print(f"    ✕ {n2:14s} {a2[2]}↔{b2[2]}   （两条合计 {s:.1f}mm）")

# 按元件统计"卷入交叉"的次数——次数高的就是该挪的那个
who = {}
for _, n1, a1, b1, n2, a2, b2 in cross:
    for p in (a1, b1, a2, b2):
        r = p[2].split(".")[0]
        who[r] = who.get(r, 0) + 1
print(f"\n── 卷入交叉最多的元件（优先动这些）──")
for r, c in sorted(who.items(), key=lambda z: -z[1])[:15]:
    print(f"  {r:8s} 卷入 {c} 次")
