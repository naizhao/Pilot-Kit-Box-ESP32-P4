#!/usr/bin/env python3
"""把贴太近的元件拉开到能用热风枪返修的间距——连带走线一起挪，不重布。

## 为什么

2026-08-12 评审："你也要考虑一个人类用热风枪去吹这些 L、C 和 U 是否方便吹上去。
**请不要以计算机的标准来要求人类**。"

实测板上有 14 对元件边到边 <0.5mm，其中 5 对是 **0.00mm 完全贴边**：
    C19↔C20   C29↔C28   C29↔R9   R4↔C20   R9↔R10
热风枪吹其中一个，旁边那个一起被吹跑；镊子也伸不进去。

这些不是布局工具挪出来的（place_fix 强制 HAND_GAP=0.5），是 `gen_pcb.py` 区域
装箱器的历史遗留——**它只保证 courtyard 不重叠，没有"返修间距"这个概念**。

## 为什么能不重布

只挪 0.2~0.5mm 这个量级，且**把连在该元件焊盘上的走线端点一起平移**。
走线另一端不动，于是那一小段变斜一点、长一点，但拓扑不变。
这跟 via_relief 那次"删线交给 route_fix 重布"完全不同——
那次是删掉重来（结果补不回来），这次是**整体平移，一根线都不删**。

挪完必须跑 kicad-cli drc 兜底：几何检查再仔细也可能漏，
而这块板是手工布通的，退步一点都不值。

## 挪谁、往哪挪

- 一对里挪**焊盘少**的那个（走线跟随的代价小）；相同则挪面积小的
- 方向：沿两元件中心连线、远离对方
- 距离：刚好让边到边达到 HAND_GAP，再加一点余量
- FROZEN 里的不挪；挪完要与**所有**元件保持 HAND_GAP（不能拉开这对撞上第三个）

用法：spread_tight.py [plan|apply]
"""
import collections
import itertools
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
MODE = sys.argv[1] if len(sys.argv) > 1 else "plan"
# ⚠️ 一次挪一批是不行的。实测挪 7 个（走线端点跟随 32 处）之后：
#     clearance 12 / shorting_items 10 / hole_clearance 3 / solder_mask_bridge 7
# 原因：端点跟着挪了、**走线中段没动**，那一小截被拉斜后撞上周围的铜
# （U6 挪 0.49mm 后 QMC_C1 那段就撞了过孔）。挪得越多，撞的越多，而且互相叠加。
# 所以默认一次只挪一个，由调用方跑 DRC 验证后再决定要不要挪下一个。
ONLY = os.environ.get("PK_ONLY_REF", "")

HAND_GAP = 0.5          # 手工返修最小间距（边到边）
MARGIN = 0.05
# ANT1 的 courtyard 罩住整片天线区，跟周围一堆元件"贴边"是正常的，不算数
IGNORE = {"ANT1"}
FROZEN = {"J1", "ANT1", "L8", "D3", "C39", "J5", "J4", "H1", "H2", "H3", "H4",
          "J2", "J8", "J6"}

board = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM

FP = {}
for f in board.GetFootprints():
    ref = f.GetReference()
    cy = f.GetCourtyard(pcbnew.F_CrtYd).BBox()
    if cy.GetWidth() <= 0:
        cy = f.GetBoundingBox()
    c = f.GetPosition()
    FP[ref] = dict(obj=f, x=mm(c.x), y=mm(c.y),
                   box=[mm(cy.GetLeft()), mm(cy.GetTop()),
                        mm(cy.GetRight()), mm(cy.GetBottom())],
                   npads=len(list(f.Pads())))

MOVED = {}


def box_at(ref, dx=0.0, dy=0.0):
    b = FP[ref]["box"]
    mx, my = MOVED.get(ref, (0.0, 0.0))
    return (b[0] + mx + dx, b[1] + my + dy, b[2] + mx + dx, b[3] + my + dy)


def gap(p, q):
    a, b = box_at(p), box_at(q)
    return math.hypot(max(a[0] - b[2], b[0] - a[2], 0),
                      max(a[1] - b[3], b[1] - a[3], 0))


def gap_boxes(a, b):
    return math.hypot(max(a[0] - b[2], b[0] - a[2], 0),
                      max(a[1] - b[3], b[1] - a[3], 0))


tight = sorted((gap(p, q), p, q) for p, q in itertools.combinations(FP, 2)
               if not ({p, q} & IGNORE) and gap(p, q) < HAND_GAP)

print(f"边到边 <{HAND_GAP}mm 的元件对: {len(tight)} 对")
for g, p, q in tight:
    print(f"    {g:.2f}mm  {p} ↔ {q}")

# 挪动：一对里挪焊盘少的那个，沿中心连线远离对方
plan, failed = [], []
for g, p, q in tight:
    if gap(p, q) >= HAND_GAP:          # 前面挪动可能已经顺带解决
        continue
    cands = [r for r in (p, q) if r not in FROZEN]
    if ONLY:
        cands = [r for r in cands if r == ONLY]
        if not cands:
            continue
    if not cands:
        failed.append((p, q, "两个都是钉死件"))
        continue
    cands.sort(key=lambda r: (FP[r]["npads"],
                              (FP[r]["box"][2] - FP[r]["box"][0])
                              * (FP[r]["box"][3] - FP[r]["box"][1])))
    ok = False
    for ref in cands:
        other = q if ref == p else p
        vx = FP[ref]["x"] - FP[other]["x"]
        vy = FP[ref]["y"] - FP[other]["y"]
        L = math.hypot(vx, vy) or 1.0
        vx, vy = vx / L, vy / L
        need = HAND_GAP - gap(p, q) + MARGIN
        for k in range(1, 13):         # 逐步加码，够了就停
            d = need * k / 4.0
            dx, dy = vx * d, vy * d
            nb = box_at(ref, dx, dy)
            # 挪完不能撞上第三者
            bad = [o for o in FP if o != ref and o not in IGNORE
                   and gap_boxes(nb, box_at(o)) < HAND_GAP]
            if bad:
                continue
            MOVED[ref] = (MOVED.get(ref, (0, 0))[0] + dx,
                          MOVED.get(ref, (0, 0))[1] + dy)
            plan.append((ref, other, g, d, dx, dy))
            ok = True
            break
        if ok:
            break
    if not ok:
        failed.append((p, q, "挪任何一个都会撞上第三个元件"))

print(f"\n可拉开 {len(plan)} 对 / 拉不开 {len(failed)} 对")
for ref, other, g0, d, dx, dy in plan:
    print(f"    {ref:5s} 挪 {d:.2f}mm ({dx:+.2f},{dy:+.2f})  远离 {other}"
          f"   间距 {g0:.2f} → {HAND_GAP}mm")
for p, q, why in failed:
    print(f"    {p} ↔ {q}: {why}")

if MODE == "apply" and MOVED:
    # 走线端点跟随：端点落在该元件任一焊盘内的，跟着平移同样的量。
    # 不删线、不重布——只是把那一小段拉斜一点。
    pad_boxes = collections.defaultdict(list)
    for ref in MOVED:
        for p in FP[ref]["obj"].Pads():
            bb = p.GetBoundingBox()
            pad_boxes[ref].append((mm(bb.GetLeft()), mm(bb.GetTop()),
                                   mm(bb.GetRight()), mm(bb.GetBottom())))

    def owner(x, y):
        for ref, boxes in pad_boxes.items():
            for b in boxes:
                if b[0] - 0.01 <= x <= b[2] + 0.01 and b[1] - 0.01 <= y <= b[3] + 0.01:
                    return ref
        return None

    nt = 0
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            continue
        for get, setr in ((t.GetStart, t.SetStart), (t.GetEnd, t.SetEnd)):
            pt = get()
            r = owner(mm(pt.x), mm(pt.y))
            if r:
                dx, dy = MOVED[r]
                setr(pcbnew.VECTOR2I_MM(round(mm(pt.x) + dx, 4),
                                        round(mm(pt.y) + dy, 4)))
                nt += 1
    for ref, (dx, dy) in MOVED.items():
        FP[ref]["obj"].Move(pcbnew.VECTOR2I_MM(round(dx, 4), round(dy, 4)))
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(PCB)
    print(f"\n已挪 {len(MOVED)} 个元件、跟随调整 {nt} 个走线端点 → {PCB}")
    print("⚠️ 必须跑 kicad-cli drc 兜底：几何检查可能漏，而这块板是手工布通的，退步不值")
else:
    print("\n（plan 模式，未写盘）")
