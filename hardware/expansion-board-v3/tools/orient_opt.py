#!/usr/bin/env python3
"""两脚元件朝向优化——按"最近引脚"原则决定 pin1/pin2 谁朝哪边。

## 为什么需要

PLACEMENT.py 里几乎所有 0402 都是 0° 摆放，从来没按"哪个脚该朝向谁"排过。
两脚无极性元件（R/L/C）转 180° 电气上完全等价，但朝向决定了走线是短直还是
"绕过元件本体再拐回来"——后者不但自己变长，还会把旁边的通道堵死。

实测例子：C57(GNSS_RF_IN) 的 pin1 朝着 U7 那侧、pin2 朝外，结果本该 1mm 的连接
绕了十几毫米，顺带把 SW2 那片的通道占了。

## 判据

对每个两脚元件：
    当前成本 = |pin1 → net1 最近的外部焊盘| + |pin2 → net2 最近的外部焊盘|
    翻转成本 = |pin2位 → net1 最近点|      + |pin1位 → net2 最近点|
翻转成本明显更小就建议转 180°。这就是"最近引脚摆放"。

只处理 R/L/C 且两脚网络都不是 GND/电源平面的（那些靠覆铜就近入平面，朝向无所谓，
算出来的"收益"是噪声）。二极管等有极性的一律跳过——转 180° 会改变电气行为。

用法：orient_opt.py [plan|apply]   apply 会改写 PLACEMENT.py 的旋转角
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
GAIN_MIN = float(os.environ.get("PK_GAIN_MIN", "0.5"))   # 收益小于这个不折腾(mm)

# 覆铜网络：这些脚靠就近过孔入平面，朝向不影响，算进来全是噪声
PLANE_NETS = {"GND", "3V3_DIG", "3V3_RF", "RP_1V1"}

board = pcbnew.LoadBoard(PCB)

# 全板焊盘位置索引：网络 → [(x, y, 所属位号)]
NET_PADS = {}
for f in board.GetFootprints():
    ref = f.GetReference()
    for p in f.Pads():
        nm = p.GetNetname()
        if not nm:
            continue
        pp = p.GetPosition()
        NET_PADS.setdefault(nm, []).append(
            (pcbnew.ToMM(pp.x), pcbnew.ToMM(pp.y), ref))


def nearest_outside(net, ref, x, y):
    """该网络上不属于 ref 的最近焊盘距离。找不到返回 None。"""
    best = None
    for px, py, owner in NET_PADS.get(net, ()):
        if owner == ref:
            continue
        d = math.hypot(px - x, py - y)
        if best is None or d < best:
            best = d
    return best


# ---- 占位表：转 90/270 会长宽互换，必须检查转完撞不撞邻居 ----
# 转 180° 元件占地不变（同一个矩形转半圈还是它自己），所以老版本不检查也没事；
# 一旦放开 90/270，长宽互换就可能压到旁边——实测不检查直接 10 条
# courtyards_overlap + 8 条 shorting_items。KiCad 判重叠看的是 courtyard，
# 不是焊盘范围（这个坑 replace_opt.py 已经栽过一次）。
BOX = {}
for _f in board.GetFootprints():
    _r = _f.GetReference()
    _c = _f.GetPosition()
    try:
        _poly = _f.GetCourtyard(pcbnew.F_CrtYd)
        if _poly.OutlineCount() > 0:
            _bb = _poly.BBox()
            BOX[_r] = (pcbnew.ToMM(_c.x), pcbnew.ToMM(_c.y),
                       pcbnew.ToMM(_bb.GetWidth()), pcbnew.ToMM(_bb.GetHeight()))
    except Exception:
        pass


def fits_rotated(ref, deg):
    """把 ref 转 deg 度之后，courtyard 还放得下吗（不压到任何邻居）。"""
    if ref not in BOX:
        return False
    x, y, w, h = BOX[ref]
    if deg in (90, 270):
        w, h = h, w                      # 长宽互换
    for oref, (ox, oy, ow, oh) in BOX.items():
        if oref == ref:
            continue
        if abs(x - ox) < (w + ow) / 2 - 1e-6 and abs(y - oy) < (h + oh) / 2 - 1e-6:
            return False
    return True


cands = []
for f in board.GetFootprints():
    ref = f.GetReference()
    if not re.match(r"^[RLC]\d+$", ref):        # 只动 R/L/C，跳过二极管等有极性件
        continue
    pads = [p for p in f.Pads() if p.GetNumber() in ("1", "2")]
    if len(pads) != 2:
        continue
    pads.sort(key=lambda p: p.GetNumber())
    (n1, n2) = (pads[0].GetNetname(), pads[1].GetNetname())
    if not n1 or not n2 or n1 == n2:
        continue
    if n1 in PLANE_NETS and n2 in PLANE_NETS:
        continue
    p1, p2 = pads[0].GetPosition(), pads[1].GetPosition()
    x1, y1 = pcbnew.ToMM(p1.x), pcbnew.ToMM(p1.y)
    x2, y2 = pcbnew.ToMM(p2.x), pcbnew.ToMM(p2.y)
    fc = f.GetPosition()
    cx, cy = pcbnew.ToMM(fc.x), pcbnew.ToMM(fc.y)

    # 平面网络那一侧不计入成本（靠覆铜，朝向无关）
    def cost(ax, ay, bx, by):
        c = 0.0
        if n1 not in PLANE_NETS:
            d = nearest_outside(n1, ref, ax, ay)
            if d is None:
                return None
            c += d
        if n2 not in PLANE_NETS:
            d = nearest_outside(n2, ref, bx, by)
            if d is None:
                return None
            c += d
        return c

    # 四个朝向都试，不只是 180°。手工把 ZP1/ZP2/ZS1/C1/C53/F2 转成 90/270——
    # 那是**换轴向**（元件从横躺变竖立），只试 180° 的话结构上就找不出来。
    # 做法：把两脚相对元件中心的偏移向量绕中心旋转 Δ，得到该朝向下的脚位置。
    def rot(px, py, deg):
        a = math.radians(deg)
        dx, dy = px - cx, py - cy
        return (cx + dx * math.cos(a) - dy * math.sin(a),
                cy + dx * math.sin(a) + dy * math.cos(a))

    now = cost(x1, y1, x2, y2)
    if now is None:
        continue
    best_d, best_c = 0, now
    for d in (90, 180, 270):
        if not fits_rotated(ref, d):     # 转完会压到邻居，直接不考虑
            continue
        a1, a2 = rot(x1, y1, d), rot(x2, y2, d)
        c = cost(a1[0], a1[1], a2[0], a2[1])
        if c is not None and c < best_c:
            best_d, best_c = d, c
    if best_d and now - best_c > GAIN_MIN:
        cands.append((now - best_c, ref, round(now, 2), round(best_c, 2), n1, n2,
                      f.GetOrientationDegrees(), best_d))

# ---- 贪心接受：按收益从大到小逐个定，每接受一个就更新占位表 ----
# 光检查"转这一个会不会压到当前邻居"是不够的：C46/C47/C49 和 R31~R34 是**同时**
# 被转的，各自检查时对方还没转，转完互相压——实测这么放过去 11 对 courtyard 重叠。
# 必须一个一个定，后面的看到的是前面已经转过的样子。
cands.sort(reverse=True)
_accepted = []
for _c in cands:
    _ref, _deg = _c[1], _c[7]
    if not fits_rotated(_ref, _deg):
        continue
    _accepted.append(_c)
    if _ref in BOX:                       # 接受了就把它的新占地写回表里
        _x, _y, _w, _h = BOX[_ref]
        if _deg in (90, 270):
            _w, _h = _h, _w
        BOX[_ref] = (_x, _y, _w, _h)
if len(_accepted) != len(cands):
    print(f"（贪心去重叠：{len(cands)} 个建议里接受 {len(_accepted)} 个，"
          f"其余与已接受的转向互相压）")
cands = _accepted
print(f"建议转向的元件：{len(cands)} 个（90/180/270 都试，收益 > {GAIN_MIN}mm）\n")
print(f"  {'位号':6s} {'收益mm':>7s} {'现在':>6s} {'转后':>6s} {'转角':>5s}  pin1网络 / pin2网络")
for g, ref, now, flip, n1, n2, cur, dd in cands:
    print(f"  {ref:6s} {g:7.2f} {now:6.2f} {flip:6.2f} {dd:4d}°  {n1} / {n2}")
tot = sum(c[0] for c in cands)
print(f"\n合计可省 {tot:.1f}mm 连线长度")

if MODE == "apply" and cands:
    todo = {c[1]: c[7] for c in cands}      # 位号 → 该转多少度
    out, n = [], 0
    # 逐行改，别用一个大正则去套整个文件——PLACEMENT 的行内空白对齐不统一，
    # 正则一复杂就容易静默漏改（改配置文件必须能断言改了几条，见项目历史教训）。
    line_re = re.compile(
        r"^(\s*\(')([A-Z]+\d+)('\s*,\s*[0-9.\-]+\s*,\s*[0-9.\-]+\s*,\s*)"
        r"([0-9.\-]+)(\s*\),?\s*)$")
    for line in open(PLACE_PY, encoding="utf-8").read().splitlines(keepends=True):
        m = line_re.match(line.rstrip("\n"))
        if m and m.group(2) in todo:
            new_rot = int((float(m.group(4)) + todo[m.group(2)]) % 360)
            out.append(f"{m.group(1)}{m.group(2)}{m.group(3)}{new_rot}{m.group(5)}\n")
            n += 1
        else:
            out.append(line)
    assert n == len(cands), f"只改到 {n} 条，应为 {len(cands)} 条——正则没匹配上，检查 PLACEMENT.py 格式"
    open(PLACE_PY, "w", encoding="utf-8").write("".join(out))
    print(f"\n已改写 PLACEMENT.py：{n} 个元件转向（断言通过）")
    print("⚠️ 接着要跑 gen_pcb.py 让布局断言校验，再重跑布线流程")
else:
    print("\n（plan 模式，未改 PLACEMENT.py）")
