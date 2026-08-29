#!/usr/bin/env python3
"""对比两块板的布局与布线差异——人工改完 PCB 后看动了什么、值不值。

不做 git diff：.kicad_pcb 是 3MB 文本，逐行 diff 读不出"哪个元件挪了多少"。
这里按对象比：

  ① 元件：位置变化、旋转变化、增删
  ② 布线：各层走线段数、过孔数
  ③ 飞线：MST 总长 + 交叉数（布局质量，跟 ratsnest_diag 同口径）
  ④ 射频硬约束：是否仍全在 F.Cu、是否仍 0 过孔

用法：diff_layout.py <基线.kicad_pcb> [新板.kicad_pcb]
      不给新板就用 kicad/ 下的当前板。
"""
import collections
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")

OLD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD, "pcb.baseline.kicad_pcb")
NEW = (sys.argv[2] if len(sys.argv) > 2
       else os.path.join(BDIR, "expansion-board-v3.kicad_pcb"))
MOVE_MIN = float(os.environ.get("PK_MOVE_MIN", "0.05"))   # 小于这个当没动

PLANE_NETS = {"GND", "3V3_DIG", "3V3_RF", "RP_1V1"}


def snap(path):
    # ⚠️ LoadBoard 对没有 .kicad_pcb 扩展名的文件返回 None（不抛异常），
    # 后面才在 GetFootprints 上炸出莫名其妙的 NoneType。备份文件一律带扩展名。
    assert path.endswith(".kicad_pcb"), f"必须是 .kicad_pcb: {path}"
    b = pcbnew.LoadBoard(path)
    assert b is not None, f"加载失败: {path}"
    fps, nets = {}, {}
    for f in b.GetFootprints():
        ref = f.GetReference()
        c = f.GetPosition()
        fps[ref] = (pcbnew.ToMM(c.x), pcbnew.ToMM(c.y),
                    round(f.GetOrientationDegrees()) % 360, f.GetValue())
        for p in f.Pads():
            nm = p.GetNetname()
            if not nm or nm.startswith("unconnected") or nm in PLANE_NETS:
                continue
            pp = p.GetPosition()
            nets.setdefault(nm, []).append((pcbnew.ToMM(pp.x), pcbnew.ToMM(pp.y)))
    trk = collections.Counter()
    nvia = 0
    rf_layers = collections.Counter()
    rf_via = 0
    try:
        from gen_sch import RF50_NETS
    except Exception:
        RF50_NETS = set()
    for t in b.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            nvia += 1
            if t.GetNetname() in RF50_NETS:
                rf_via += 1
        else:
            ly = b.GetLayerName(t.GetLayer())
            trk[ly] += 1
            if t.GetNetname() in RF50_NETS:
                rf_layers[ly] += 1
    return fps, nets, trk, nvia, rf_layers, rf_via


def mst_len(pts):
    n = len(pts)
    if n < 2:
        return 0.0, []
    used = [False] * n
    used[0] = True
    best = [(math.hypot(pts[i][0] - pts[0][0], pts[i][1] - pts[0][1]), 0)
            for i in range(n)]
    tot, edges = 0.0, []
    for _ in range(n - 1):
        k, bd = -1, None
        for i in range(n):
            if not used[i] and (bd is None or best[i][0] < bd):
                k, bd = i, best[i][0]
        if k < 0:
            break
        used[k] = True
        tot += best[k][0]
        edges.append((pts[best[k][1]], pts[k]))
        for i in range(n):
            if not used[i]:
                d = math.hypot(pts[i][0] - pts[k][0], pts[i][1] - pts[k][1])
                if d < best[i][0]:
                    best[i] = (d, k)
    return tot, edges


def cross_count(all_edges):
    def cr(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    n = 0
    for i in range(len(all_edges)):
        n1, a1, b1 = all_edges[i]
        for j in range(i + 1, len(all_edges)):
            n2, a2, b2 = all_edges[j]
            if n1 == n2:
                continue
            if max(a1[0], b1[0]) < min(a2[0], b2[0]) or max(a2[0], b2[0]) < min(a1[0], b1[0]):
                continue
            if max(a1[1], b1[1]) < min(a2[1], b2[1]) or max(a2[1], b2[1]) < min(a1[1], b1[1]):
                continue
            d1, d2 = cr(a2, b2, a1), cr(a2, b2, b1)
            d3, d4 = cr(a1, b1, a2), cr(a1, b1, b2)
            if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
                n += 1
    return n


def rats(nets):
    tot, edges = 0.0, []
    for nm, pts in nets.items():
        L, es = mst_len(pts)
        tot += L
        edges += [(nm, a, b) for a, b in es]
    return tot, cross_count(edges), len(edges)


print(f"基线: {OLD}\n新板: {NEW}\n")
of, on_, ot, ov, orf, orv = snap(OLD)
nf, nn, nt, nv, nrf, nrv = snap(NEW)

print("=" * 62)
print("① 元件变化")
print("=" * 62)
moved, rotated, added, removed = [], [], [], []
for ref in sorted(set(of) | set(nf)):
    if ref not in nf:
        removed.append(ref)
        continue
    if ref not in of:
        added.append(ref)
        continue
    ox, oy, orot, oval = of[ref]
    nx, ny, nrot, nval = nf[ref]
    d = math.hypot(nx - ox, ny - oy)
    if d >= MOVE_MIN:
        moved.append((d, ref, (ox, oy), (nx, ny), oval))
    if orot != nrot:
        rotated.append((ref, orot, nrot))
moved.sort(reverse=True)
print(f"  挪位 {len(moved)} 个 / 转向 {len(rotated)} 个 / 新增 {len(added)} / 删除 {len(removed)}")
if moved:
    print("\n  挪位（按距离）:")
    for d, ref, o, n2, val in moved:
        print(f"    {ref:7s} {d:6.2f}mm  ({o[0]:7.2f},{o[1]:6.2f}) → "
              f"({n2[0]:7.2f},{n2[1]:6.2f})   {val[:26]}")
if rotated:
    print("\n  转向:")
    for ref, a, b in rotated:
        print(f"    {ref:7s} {a}° → {b}°")
if added:
    print(f"\n  新增: {added}")
if removed:
    print(f"\n  删除: {removed}")

print()
print("=" * 62)
print("② 布线变化")
print("=" * 62)
print(f"  {'层':10s} {'基线':>8s} {'新板':>8s} {'差':>8s}")
for ly in sorted(set(ot) | set(nt)):
    a, b = ot.get(ly, 0), nt.get(ly, 0)
    print(f"  {ly:10s} {a:8d} {b:8d} {b-a:+8d}")
print(f"  {'过孔':10s} {ov:8d} {nv:8d} {nv-ov:+8d}")

print()
print("=" * 62)
print("③ 飞线（布局质量，越小越好）")
print("=" * 62)
oL, oC, oN = rats(on_)
nL, nC, nN = rats(nn)
print(f"  MST 总长   {oL:8.1f}mm → {nL:8.1f}mm   {nL-oL:+.1f}mm"
      f"  ({(nL-oL)/oL*100:+.1f}%)")
print(f"  交叉数     {oC:8d}   → {nC:8d}     {nC-oC:+d}")
print(f"  飞线条数   {oN:8d}   → {nN:8d}     {nN-oN:+d}")

print()
print("=" * 62)
print("④ 射频硬约束")
print("=" * 62)
print(f"  基线: {dict(orf)}  过孔 {orv}")
print(f"  新板: {dict(nrf)}  过孔 {nrv}")
bad = [ly for ly in nrf if ly != "F.Cu"]
print("  " + ("❌ 射频跑到非 F.Cu 层: " + str(bad) if bad else "✅ 射频仍全在 F.Cu"))
print("  " + ("❌ 射频出现过孔" if nrv else "✅ 射频仍 0 过孔"))
