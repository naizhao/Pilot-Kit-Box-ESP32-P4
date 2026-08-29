#!/usr/bin/env python3
"""针对性拆线重布——按 route_fix 给出的 ripup.json 拆掉挡路者，再让它重布。

## 跟"全局 rip-up"的区别

全局 rip-up 在本板实测是负收益（缺线在 14→18→17 之间震荡，还引入 20 条
clearance/shorting），因为它无差别拆一大片，自研 A* 重布不回 freerouting 的原布法。

这里只拆 route_fix **明确点名**挡住某条残线的那几段（通常 1-3 项），
拆一条布两条——只要都布通就是净赚，布不通就整体回退。判据是**未连通总数**，
不是"补上几条"（补上的可能还没拆掉的多）。

⚠️ 必须单独进程跑：删除后 SWIG 对象失效（route_fix.py:26-29 同一个坑）。

用法：ripup_retry.py <ripup.json> [apply] [只拆这些网络,逗号分隔]
"""
import json
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

RIPUP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD, "ripup.json")
APPLY = len(sys.argv) > 2 and sys.argv[2] == "apply"
ONLY = set(sys.argv[3].split(",")) if len(sys.argv) > 3 else None

items = json.load(open(RIPUP))
if ONLY:
    items = [it for it in items if it.get("net") in ONLY]
print(f"拆解清单 {len(items)} 项" + (f"（只限 {sorted(ONLY)}）" if ONLY else ""))

board = pcbnew.LoadBoard(PCB)
hits, miss = [], []
for it in items:
    kind, net, geo, layer = it["kind"], it["net"], it["geo"], it["layer"]
    found = None
    for t in board.GetTracks():
        if t.GetNetname() != net:
            continue
        is_via = isinstance(t, pcbnew.PCB_VIA)
        if (kind == "via") != is_via:
            continue
        if is_via:
            p = t.GetPosition()
            if math.hypot(pcbnew.ToMM(p.x) - geo[0], pcbnew.ToMM(p.y) - geo[1]) < 0.01:
                found = t
                break
        else:
            if t.GetLayer() != layer:
                continue
            a = (pcbnew.ToMM(t.GetStart().x), pcbnew.ToMM(t.GetStart().y))
            b = (pcbnew.ToMM(t.GetEnd().x), pcbnew.ToMM(t.GetEnd().y))
            if (math.hypot(a[0] - geo[0], a[1] - geo[1]) < 0.01
                    and math.hypot(b[0] - geo[2], b[1] - geo[3]) < 0.01) or \
               (math.hypot(a[0] - geo[2], a[1] - geo[3]) < 0.01
                    and math.hypot(b[0] - geo[0], b[1] - geo[1]) < 0.01):
                found = t
                break
    (hits if found else miss).append((it, found))

print(f"  匹配到 {len(hits)} 项 / 没找到 {len(miss)} 项")
for it, _ in hits:
    g = it["geo"]
    where = (f"({g[0]:.2f},{g[1]:.2f})" if it["kind"] == "via"
             else f"({g[0]:.2f},{g[1]:.2f})→({g[2]:.2f},{g[3]:.2f})")
    print(f"    拆 {it['net']:14s} {it['kind']:4s} {where}")
for it, _ in miss:
    print(f"    ⚠️ 没找到 {it['net']} {it['kind']} {it['geo']}")

if APPLY and hits:
    for _, t in hits:
        board.Remove(t)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(PCB)
    print(f"\n已拆 {len(hits)} 项 → {PCB}")
    print("⚠️ 接着跑 DRC + route_fix apply 重布，用**未连通总数**验收；不赚就回退")
else:
    print("\n（plan 模式，未写盘）")
