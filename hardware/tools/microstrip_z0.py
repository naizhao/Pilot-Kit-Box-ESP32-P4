#!/usr/bin/env python3
"""外层微带阻抗正/反算（Hammerstad-Jensen，含铜厚修正）。

用途：换叠层时重算 50Ω 线宽，以及反标定嘉立创计算器的「等效 Dk」。

⚠️ 这是**裸微带**公式，不含阻焊层。绿油会拉高有效介电常数，所以直接用材料标称
Dk（FR-4 约 4.1）会把线算粗。正确用法是先用官方计算器的一个数据点反标定出等效
Dk，再用这个 Dk 在同一叠层内插值。**任何要下单的线宽以官方计算器为准。**

    python3 microstrip_z0.py                # 跑内置的嘉立创叠层校验
    python3 microstrip_z0.py 0.15 0.0994 4.725   # W h Dk -> Z0

数据来源见 ../JLC_STACKUP_IMPEDANCE.md
"""

import math
import sys


def z0(w, h, t, er):
    """线宽 w、介质厚 h、铜厚 t、介电常数 er（mm）→ (Z0, eps_eff)。"""
    if w / h > 1 / (2 * math.pi):
        we = w + (t / math.pi) * (1 + math.log(2 * h / t))
    else:
        we = w + (t / math.pi) * (1 + math.log(4 * math.pi * w / t))
    u = we / h
    if u <= 1:
        ee = (er + 1) / 2 + (er - 1) / 2 * ((1 + 12 / u) ** -0.5 + 0.04 * (1 - u) ** 2)
        return 60 / math.sqrt(ee) * math.log(8 / u + u / 4), ee
    ee = (er + 1) / 2 + (er - 1) / 2 * (1 + 12 / u) ** -0.5
    return 120 * math.pi / (math.sqrt(ee) * (u + 1.393 + 0.667 * math.log(u + 1.444))), ee


def _bisect(f, lo, hi, descending):
    for _ in range(200):
        mid = (lo + hi) / 2
        if (f(mid) > 0) == descending:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def solve_w(target_z, h, t, er):
    """给定目标阻抗求线宽（线越宽阻抗越低）。"""
    return _bisect(lambda w: z0(w, h, t, er)[0] - target_z, 0.01, 20.0, True)


def solve_er(w, h, t, target_z):
    """由官方计算器的一个 (W, Z0) 数据点反标定等效 Dk。"""
    return _bisect(lambda er: z0(w, h, t, er)[0] - target_z, 1.0, 20.0, True)


# 嘉立创阻抗计算器官方数据点：(叠层, h, 外层铜厚 t, 线宽 W, 阻抗 Z0)
JLC_POINTS = [
    ("JLC0216A        2层", 1.4300, 0.0300, 2.6916, 50.0),
    ("JLC04161H-7628  4层", 0.2104, 0.0350, 0.3586, 50.0),
    ("JLC06161H-3313  6层", 0.0994, 0.0350, 0.1509, 50.0),
    ("JLC06161H-3313  6层", 0.0994, 0.0350, 0.3400, 31.8238),
    ("JLC06161H-1080C 6层", 0.0764, 0.0350, 0.1072, 50.0),
]


def main():
    if len(sys.argv) == 4:
        w, h, er = (float(x) for x in sys.argv[1:])
        zz, ee = z0(w, h, 0.035, er)
        print(f"W={w} h={h} Dk={er} -> Z0={zz:.3f}Ω (eps_eff={ee:.3f})")
        return
    print(f"{'叠层':<22}{'h':>8}{'W':>9}{'官方Z0':>9}{'反标定Dk':>10}")
    for name, h, t, w, zz in JLC_POINTS:
        print(f"{name:<22}{h:>8.4f}{w:>9.4f}{zz:>9.4f}{solve_er(w, h, t, zz):>10.3f}")


if __name__ == "__main__":
    main()
