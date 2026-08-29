#!/usr/bin/env python3
"""元件归位——把被"装箱溢出"扔错地方的元件挪回它该待的地方。

## 病根

gen_pcb.py 按区域装箱摆位，装不下就溢出到 OVERFLOW 指定的区（多半是
"LEFT_EDGE 左边缘空地"）。装箱只关心"塞得下"，完全不管这个元件要连到哪儿去。
结果 R20 被扔到 x=55.6，而它伺候的 U13 在 x=134.8——**飞线 79mm，横跨整块板**，
还违反 LAYOUT_CONSTRAINTS §2.5「RTADJ(R20) 紧贴引脚」。

评审的意见：画 PCB 不是网格是血管，讲求头对脚、脚对头，网格式排布除了好看一无是处。

## 判据

每个元件的**理想位置 = 它所有连接对象的位置重心**（覆铜网不计，那些靠平面就近入）。
实际位置离理想位置越远，说明摆得越离谱。挪回去时在理想位置附近螺旋找空位，
避开已有元件和禁区。

只动两脚小元件（R/L/C 的 0402/0603）——大芯片和连接器的位置是结构约束定的，
不能自动挪。

用法：replace_opt.py [plan|apply] [最小偏离mm，默认15]
"""
import math
import os
import re
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
PLACE_PY = os.path.join(T, "tools", "PLACEMENT.py")

MODE = sys.argv[1] if len(sys.argv) > 1 else "plan"
FAR_MIN = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

PLANE_NETS = {"GND", "3V3_DIG", "3V3_RF", "RP_1V1"}
BOARD = (52.0, 50.0, 150.0, 112.0)          # 大致板框，找位时别跑出去

board = pcbnew.LoadBoard(PCB)

# 网络 → [(x,y,ref)]；元件 → 自身几何
NET_PADS, SIZE, POS, ROT = {}, {}, {}, {}
for f in board.GetFootprints():
    ref = f.GetReference()
    c = f.GetPosition()
    POS[ref] = (pcbnew.ToMM(c.x), pcbnew.ToMM(c.y))
    ROT[ref] = f.GetOrientationDegrees()
    xs, ys = [], []
    for p in f.Pads():
        pp = p.GetPosition()
        px, py = pcbnew.ToMM(pp.x), pcbnew.ToMM(pp.y)
        xs.append(px); ys.append(py)
        nm = p.GetNetname()
        if nm and not nm.startswith("unconnected"):
            NET_PADS.setdefault(nm, []).append((px, py, ref))
    # ⚠️ 占位尺寸必须用 **courtyard(元件外框)**，不能用"焊盘范围 + 余量"。
    # KiCad 判元件重叠看的就是 courtyard，比焊盘大一圈：上一版拿焊盘 +1.2mm 当
    # 判据，挪完直接 24 条 courtyards_overlap + 8 条 shorting_items（元件压元件）。
    box = None
    try:
        poly = f.GetCourtyard(pcbnew.F_CrtYd)
        if poly.OutlineCount() > 0:
            bb = poly.BBox()
            box = (pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight()))
    except Exception:
        box = None
    if box and box[0] > 0.1 and box[1] > 0.1:
        SIZE[ref] = (box[0] + 0.2, box[1] + 0.2)     # 再留 0.2mm 呼吸
    elif xs:
        SIZE[ref] = (max(xs) - min(xs) + 1.4, max(ys) - min(ys) + 1.4)


FANOUT_MAX = int(os.environ.get("PK_FANOUT_MAX", "6"))


def ideal_of(ref):
    """连接重心：该元件每个脚所连网络里、别人的焊盘位置的平均。

    ⚠️ 只算**小网络**（焊盘数 ≤ FANOUT_MAX）。大网络（VCC_5V、3V3_GNSS 这种
    挂十几个焊盘的）取平均没有物理意义——去耦电容连的就是大电源网，按平均位置
    去挪会把它从"贴着芯片脚"挪到板子中间，正好挪反。去耦电容该贴它伺候的芯片，
    那是电气要求，不是几何优化能算出来的。"""
    pts = []
    for f in board.GetFootprints():
        if f.GetReference() != ref:
            continue
        for p in f.Pads():
            nm = p.GetNetname()
            if not nm or nm in PLANE_NETS or nm.startswith("unconnected"):
                continue
            same = NET_PADS.get(nm, ())
            if len(same) - 1 > FANOUT_MAX:      # 减掉自己这一脚
                continue
            for qx, qy, owner in same:
                if owner != ref:
                    pts.append((qx, qy))
    if not pts:
        return None
    return (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))


OCC = [(POS[r][0], POS[r][1], SIZE[r][0], SIZE[r][1], r) for r in POS if r in SIZE]


def free_at(x, y, w, h, me):
    if not (BOARD[0] + w / 2 < x < BOARD[2] - w / 2
            and BOARD[1] + h / 2 < y < BOARD[3] - h / 2):
        return False
    for ox, oy, ow, oh, oref in OCC:
        if oref == me:
            continue
        if abs(x - ox) < (w + ow) / 2 and abs(y - oy) < (h + oh) / 2:
            return False
    return True


def find_spot(ix, iy, w, h, me):
    """从理想位置螺旋往外找第一个空位。"""
    for r in [0] + [0.5 * k for k in range(1, 41)]:
        steps = max(1, int(r * 8))
        for s in range(steps):
            a = 2 * math.pi * s / steps
            x, y = ix + r * math.cos(a), iy + r * math.sin(a)
            if free_at(x, y, w, h, me):
                return (round(x, 3), round(y, 3), round(r, 2))
    return None


cands = []
for ref in sorted(POS):
    if not re.match(r"^[RLC]\d+$", ref):
        continue
    if ref not in SIZE or SIZE[ref][0] > 3 or SIZE[ref][1] > 3:   # 只动 0402/0603
        continue
    ideal = ideal_of(ref)
    if not ideal:
        continue
    now = POS[ref]
    off = math.hypot(now[0] - ideal[0], now[1] - ideal[1])
    if off < FAR_MIN:
        continue
    w, h = SIZE[ref]
    spot = find_spot(ideal[0], ideal[1], w, h, ref)
    if not spot:
        cands.append((off, ref, now, ideal, None, 0))
        continue
    newpos = (spot[0], spot[1])
    gain = off - math.hypot(newpos[0] - ideal[0], newpos[1] - ideal[1])
    cands.append((off, ref, now, ideal, newpos, gain))

cands.sort(reverse=True)
print(f"离连接重心超过 {FAR_MIN}mm 的两脚元件：{len(cands)} 个\n")
print(f"  {'位号':6s} {'偏离':>6s} {'现在位置':>16s} {'该在(重心)':>16s} {'建议挪到':>16s}")
ok = 0
for off, ref, now, ideal, new, gain in cands:
    tgt = f"({new[0]:.1f},{new[1]:.1f})" if new else "找不到空位"
    print(f"  {ref:6s} {off:6.1f} ({now[0]:7.1f},{now[1]:6.1f}) "
          f"({ideal[0]:7.1f},{ideal[1]:6.1f}) {tgt:>16s}")
    if new:
        ok += 1
print(f"\n可挪 {ok} 个 / 共 {len(cands)} 个")

if MODE == "apply" and ok:
    todo = {c[1]: c[4] for c in cands if c[4]}
    line_re = re.compile(
        r"^(\s*\(')([A-Z]+\d+)('\s*,\s*)([0-9.\-]+)(\s*,\s*)([0-9.\-]+)"
        r"(\s*,\s*[0-9.\-]+\s*\),?\s*)$")
    out, n = [], 0
    for line in open(PLACE_PY, encoding="utf-8").read().splitlines(keepends=True):
        m = line_re.match(line.rstrip("\n"))
        if m and m.group(2) in todo:
            nx, ny = todo[m.group(2)]
            out.append(f"{m.group(1)}{m.group(2)}{m.group(3)}{nx:.3f}"
                       f"{m.group(5)}{ny:.3f}{m.group(7)}\n")
            n += 1
        else:
            out.append(line)
    assert n == len(todo), f"只改到 {n} 条，应为 {len(todo)} 条"
    open(PLACE_PY, "w", encoding="utf-8").write("".join(out))
    print(f"\n已改写 PLACEMENT.py：{n} 个元件归位")
    print("⚠️ 接着跑 gen_pcb.py 让区域/板边断言校验")
else:
    print("\n（plan 模式，未改 PLACEMENT.py）")
