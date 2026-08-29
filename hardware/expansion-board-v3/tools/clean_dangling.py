#!/usr/bin/env python3
"""精准删除 DRC 点名的悬空走线/过孔（track_dangling / via_dangling）。

## 为什么不用 route_fix.py clean

`route_fix clean` 删的是"孤立断头铜"——按连通块判定，把所有没接到焊盘的碎铜
一起删。实测在本板上是**负收益**：未连通 16 → 20，还留下 3 个悬空过孔。
因为有些碎铜实际压在覆铜上是连通的，删了反而暴露新缺口。

这里只删 KiCad DRC **明确报为 dangling** 的那几段——判据来自官方 DRC 而不是
自算连通块，不会误伤。悬空走线本来就没接上任何东西，删掉不影响连通性。

典型来源：freerouting 的 SES 导入会带进 0.0001mm 的零长度残段（过孔位置重合
所致），KiCad 直接判 track_dangling。

⚠️ 必须单独进程跑：删除后 SWIG 对象会失效，同一进程里再遍历 board 拿到的是
野指针（route_fix.py:26-29 记的是同一个坑）。

用法：clean_dangling.py <drc.json> [apply]
"""
import json
import math
import os
import re
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

DRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD, "drc.json")
APPLY = len(sys.argv) > 2 and sys.argv[2] == "apply"

def _parse_len(desc):
    """从 "Track [NET] on F.Cu, length 1.1359 mm" 里取出长度（mm）。"""
    m = re.search(r"length\s+([0-9.]+)\s*mm", desc)
    return float(m.group(1)) if m else -1.0


board = pcbnew.LoadBoard(PCB)
LNAME = {board.GetLayerID(n): n for n in
         ("F.Cu", "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "B.Cu")}

want = []
for v in json.load(open(DRC)).get("violations", []):
    if v.get("type") not in ("track_dangling", "via_dangling"):
        continue
    for it in v.get("items", []):
        d = it.get("description", "")
        p = it.get("pos", {})
        if "pos" not in it:
            continue
        want.append((v["type"], d, float(p.get("x", 0)), float(p.get("y", 0))))

print(f"DRC 点名的悬空项：{len(want)} 个")
for t, d, x, y in want:
    print(f"   [{t}] {d[:70]} @({x},{y})")

hits = []
for t in board.GetTracks():
    is_via = isinstance(t, pcbnew.PCB_VIA)
    a = (pcbnew.ToMM(t.GetStart().x), pcbnew.ToMM(t.GetStart().y))
    b = (pcbnew.ToMM(t.GetEnd().x), pcbnew.ToMM(t.GetEnd().y))
    L = math.hypot(b[0] - a[0], b[1] - a[1])
    ly = LNAME.get(t.GetLayer(), "")
    nm = t.GetNetname()
    for kind, desc, x, y in want:
        if (kind == "via_dangling") != is_via:
            continue
        if f"[{nm}]" not in desc:
            continue
        if not is_via and ly and f"on {ly}" not in desc:
            continue
        # DRC 给的 pos 落在这一段的端点上（容差 1µm），且长度对得上
        near = min(math.hypot(x - a[0], y - a[1]), math.hypot(x - b[0], y - b[1])) < 0.002
        same_len = is_via or abs(L - _parse_len(desc)) < 0.002
        if near and same_len:
            hits.append((t, nm, ly, L, a, b, is_via))
            break

print(f"\n匹配到 {len(hits)} 条，准备{'删除' if APPLY else '删除（plan，未写盘）'}：")
for t, nm, ly, L, a, b, is_via in hits:
    what = "过孔" if is_via else f"走线 {L:.4f}mm"
    print(f"   {nm:12s} {ly:7s} {what}  ({a[0]:.3f},{a[1]:.3f})→({b[0]:.3f},{b[1]:.3f})")

if APPLY and hits:
    for t, *_ in hits:
        board.Remove(t)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(PCB)
    print(f"\n已删 {len(hits)} 条 → {PCB}")
elif not hits:
    print("\n没有可删的（DRC 干净，或描述格式对不上——检查上面的点名清单）")
else:
    print("\n（plan 模式，未写盘）")
