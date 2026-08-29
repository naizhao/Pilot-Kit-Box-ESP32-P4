#!/usr/bin/env python3
"""打印某点周围 F.Cu 的可走图，看射频线到底能不能出得来。

'.'=可走  'X'=被占。行首标 y，'<' 标记中心行。
比"最近点余量"准：余量表给的是焊盘**边缘**格点到对方的距离，天然偏小；
真正决定能不能出线的是焊盘中心那条轴向通道有没有连续的 '.'。
用法: rf_occmap.py <net> <cx> <cy> [线宽] [净空]
"""
import math
import os
import sys

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")
NET, CX, CY = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
W = float(sys.argv[4]) if len(sys.argv) > 4 else 0.15   # 6 层 50Ω；0.34 是四层板的值
CLR = float(sys.argv[5]) if len(sys.argv) > 5 else 0.15
sys.argv = ["route_fix", "plan", os.environ.get("PK_DRC", os.path.join(BUILD, "drc.json"))]

import route_fix as R                                              # noqa: E402

R.build()
NAME = {n.GetNetCode(): n.GetNetname() for n in R.board.GetNetsByNetcode().values()}
pads, trks, vias = R.snap_obstacles()
nc = R.board.FindNet(NET).GetNetCode()
half = W / 2


def who(x, y):
    for onc, ols, box, oclr in pads:
        if onc == nc or 0 not in ols:
            continue
        if R.seg_rect_dist(x, y, x, y, box) < half + max(oclr, CLR):
            return NAME.get(onc, "?")
    for onc, oli, g, oh, oclr in trks:
        if onc == nc or oli != 0:
            continue
        if R.seg_seg_dist((x, y), (x, y), (g[0], g[1]), (g[2], g[3])) < half + oh + max(oclr, CLR):
            return NAME.get(onc, "?")
    for onc, p, oclr in vias:
        if onc == nc:
            continue
        if math.hypot(x - p[0], y - p[1]) < half + R.VIA_R + max(oclr, CLR):
            return NAME.get(onc, "?")
    return None


RAD = float(os.environ.get("PK_MAP_RAD", "1.6"))
STEP = RAD / 16
N = 33
print(f"=== {NET} @({CX},{CY}) 线宽{W} 净空{CLR} 需{half+CLR:.3f}mm 范围±{RAD}mm 步长{STEP:.3f} ===")
blk = {}
for j in range(N):
    y = CY - RAD + j * STEP
    row = ""
    for i in range(N):
        x = CX - RAD + i * STEP
        w = who(x, y)
        if w is None:
            row += "."
        else:
            row += "X"
            blk[w] = blk.get(w, 0) + 1
    print(f"  {y:6.2f}{'<' if abs(y-CY) < STEP/2 else ' '}{row}")
print("  挡路者:", dict(sorted(blk.items(), key=lambda z: -z[1])[:5]))
