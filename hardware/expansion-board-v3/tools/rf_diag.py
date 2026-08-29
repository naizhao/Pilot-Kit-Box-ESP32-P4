#!/usr/bin/env python3
"""射频残线堵在哪——列出每条的缺口两端，以及端点附近谁挡着、余量多少。

余量 = 实际距离 - 需求距离(我方半宽 + 对方半宽 + max(两边净空))。
余量为负 = 现在就违例；余量 0.0x = 通道存在但窄到栅格抓不住，也不该投产。
"""
import math
import os
import sys

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")
sys.argv = ["route_fix", "plan", os.environ.get("PK_DRC", os.path.join(BUILD, "drc.json"))]

import route_fix as R                                              # noqa: E402
from gen_sch import RF50_NETS                                      # noqa: E402

R.build()
NAME = {n.GetNetCode(): n.GetNetname() for n in R.board.GetNetsByNetcode().values()}
pads, trks, vias = R.snap_obstacles()
# ⚠️ 0.34 是**四层板** JLC04161H-7628 的 50Ω 线宽；本板是 6 层 JLC06161H-3313，
# L1-L2 介质只有 0.0994mm，0.34mm 宽实测反算只有 31.82Ω。见 IFA_ANTENNA.md §3.2。
W, CLR = 0.15, 0.15
half = W / 2

for net in sorted(RF50_NETS):
    n = R.board.FindNet(net)
    if not n:
        continue
    blks = R.blocks_of(n.GetNetCode())
    if len(blks) < 2:
        continue
    nc = n.GetNetCode()
    # 缺口两端
    best = None
    for i in range(len(blks)):
        for j in range(i + 1, len(blks)):
            ca = {c for c in R.block_cells(blks[i]) if c[2] == 0}
            cb = {c for c in R.block_cells(blks[j]) if c[2] == 0}
            for a in ca:
                for b in cb:
                    d = (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2
                    if best is None or d < best[0]:
                        best = (d, a, b)
    if not best:
        continue
    ax, ay = R.g2m(best[1][0], best[1][1])
    bx, by = R.g2m(best[2][0], best[2][1])
    print(f"\n=== {net}  缺口 ({ax:.2f},{ay:.2f}) → ({bx:.2f},{by:.2f}) "
          f"= {math.hypot(bx-ax, by-ay):.2f}mm  共{len(blks)}块 ===")
    for tag, (cx, cy) in (("端A", (ax, ay)), ("端B", (bx, by))):
        hits = []
        for onc, ols, box, oclr in pads:
            if onc == nc or 0 not in ols:
                continue
            d = R.seg_rect_dist(cx, cy, cx, cy, box)
            need = half + max(oclr, CLR)
            if d < need + 0.5:
                hits.append((round(d - need, 3), "PAD", NAME.get(onc, "?"), round(d, 3)))
        for onc, oli, g, oh, oclr in trks:
            if onc == nc or oli != 0:
                continue
            d = R.seg_seg_dist((cx, cy), (cx, cy), (g[0], g[1]), (g[2], g[3]))
            need = half + oh + max(oclr, CLR)
            if d < need + 0.5:
                hits.append((round(d - need, 3), "TRK", NAME.get(onc, "?"), round(d, 3)))
        for onc, p, oclr in vias:
            if onc == nc:
                continue
            d = math.hypot(cx - p[0], cy - p[1])
            need = half + R.VIA_R + max(oclr, CLR)
            if d < need + 0.5:
                hits.append((round(d - need, 3), "VIA", NAME.get(onc, "?"), round(d, 3)))
        hits.sort()
        print(f"  {tag} 附近挡路者（余量从小到大）:")
        for m, k, nm, d in hits[:5]:
            flag = "❌违例" if m < 0 else ("⚠️极窄" if m < 0.05 else "")
            print(f"      {m:+.3f}mm  {k} {nm:20s} 实距{d:.3f} {flag}")
        if not hits:
            print("      （0.5mm 内无障碍——这一端是通的）")
