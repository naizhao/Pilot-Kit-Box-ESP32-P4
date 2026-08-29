#!/usr/bin/env python3
"""从射频残线的一端做洪水填充，量出"能走到哪"，并找出封锁边界上挡着谁。

线宽/净空档位全试过都无解时用它：能区分「通道太窄」和「根本被围死」。
用法: rf_flood.py [net]
"""
import math
import os
import sys
from collections import deque

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")
ONLY = sys.argv[1] if len(sys.argv) > 1 else None
sys.argv = ["route_rf_manual", "plan"]

import route_rf_manual as M                                        # noqa: E402
import route_fix as R                                              # noqa: E402
from gen_sch import RF50_NETS                                      # noqa: E402

R.build()
R.NETNAME.update({n.GetNetCode(): n.GetNetname()
                  for n in R.board.GetNetsByNetcode().values()})
NAME = R.NETNAME
pads, trks, vias = R.snap_obstacles()

for net in sorted(RF50_NETS):
    if ONLY and net != ONLY:
        continue
    n = R.board.FindNet(net)
    if not n or len(R.blocks_of(n.GetNetCode())) < 2:
        continue
    res, err = M.local_route(net)
    nc = n.GetNetCode()
    blks = R.blocks_of(nc)
    # 用和 local_route 一样的口径重建局部图，然后 BFS
    best = None
    for i in range(len(blks)):
        for j in range(i + 1, len(blks)):
            ca = {c for c in R.block_cells(blks[i]) if c[2] == 0}
            cb = {c for c in R.block_cells(blks[j]) if c[2] == 0}
            for a in ca:
                for b in cb:
                    d = (a[0]-b[0])**2 + (a[1]-b[1])**2
                    if best is None or d < best[0]:
                        best = (d, i, j, a, b)
    _, i, j, ga, gb = best
    ax, ay = R.g2m(*ga[:2])
    bx, by = R.g2m(*gb[:2])
    G, MG = M.GRID, M.MARGIN
    x0, x1 = min(ax, bx) - MG, max(ax, bx) + MG
    y0, y1 = min(ay, by) - MG, max(ay, by) + MG
    nw, nh = int((x1-x0)/G)+1, int((y1-y0)/G)+1
    half, CLR = M.W/2, M.CLR

    def free(gx, gy):
        x, y = x0 + gx*G, y0 + gy*G
        for onc, ols, box, oclr in pads:
            if onc == nc or 0 not in ols:
                continue
            if R.seg_rect_dist(x, y, x, y, box) < half + max(oclr, CLR):
                return False, NAME.get(onc, "?")
        for onc, oli, g, oh, oclr in trks:
            if onc == nc or oli != 0:
                continue
            if R.seg_seg_dist((x, y), (x, y), (g[0], g[1]), (g[2], g[3])) < half+oh+max(oclr, CLR):
                return False, NAME.get(onc, "?")
        for onc, p, oclr in vias:
            if onc == nc:
                continue
            if math.hypot(x-p[0], y-p[1]) < half + R.VIA_R + max(oclr, CLR):
                return False, NAME.get(onc, "?")
        return True, None

    sgx, sgy = int(round((ax-x0)/G)), int(round((ay-y0)/G))
    tgx, tgy = int(round((bx-x0)/G)), int(round((by-y0)/G))
    seen = {(sgx, sgy)}
    dq = deque([(sgx, sgy)])
    border = {}
    while dq:
        cx, cy = dq.popleft()
        for dx, dy in ((1,0),(1,1),(0,1),(-1,1),(-1,0),(-1,-1),(0,-1),(1,-1)):
            nx, ny = cx+dx, cy+dy
            if not (0 <= nx < nw and 0 <= ny < nh) or (nx, ny) in seen:
                continue
            ok, w = free(nx, ny)
            if ok:
                seen.add((nx, ny)); dq.append((nx, ny))
            else:
                seen.add((nx, ny))
                border[w] = border.get(w, 0) + 1
    reach = [(x0+p[0]*G, y0+p[1]*G) for p in seen]
    rx = [p[0] for p in reach]; ry = [p[1] for p in reach]
    hit = (tgx, tgy) in seen
    print(f"\n=== {net} ===")
    print(f"  起点({ax:.2f},{ay:.2f}) → 目标({bx:.2f},{by:.2f})  局部 {nw}×{nh} 格")
    print(f"  可达 {len(seen)} 格，范围 x{min(rx):.2f}~{max(rx):.2f} y{min(ry):.2f}~{max(ry):.2f}")
    print(f"  目标可达: {'是' if hit else '否 ❌'}   （A* 结论: {'有解' if res else err}）")
    print(f"  封锁边界上挡着谁（格数）: "
          f"{dict(sorted(border.items(), key=lambda z: -z[1])[:6])}")
