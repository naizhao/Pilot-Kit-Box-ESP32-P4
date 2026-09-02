#!/usr/bin/env python3
"""去耦有效性量化：把"电容离引脚 X mm"换算成"该引脚实际看到的电源阻抗"。

为什么需要它：手册只说 "place close to the pin"，但"远"到底算不算问题？
"距离 9mm" 这种定性说法无法自证，也容易夸大——**同一电源网络上的电容是并联的**，
只盯最近那一颗会得出严重得多的错误结论（本工具第一版就犯了这个错，把 BGA2817
算成 41Ω，按并联重算实为 9.7Ω）。

模型：
    每颗电容一条支路  Z_i = 1/(j2πf·C_i) + j2πf·(0.5nH/mm · d_i + 0.6nH + ESL_i)
    引脚看到的阻抗    Z   = 1 / Σ(1/Z_i)     ← 全网并联
判读：不要看"是否呈感性"（100MHz 以上陶瓷电容普遍已过 SRF，无区分度），
      要看**阻抗绝对值**，以及**与同网其他引脚的横向对比**。

参数：多层板带完整地平面时窄走线约 0.4–0.6 nH/mm 取 0.5；过孔对 0.6nH；
      ESL 0402≈0.5nH / 0603≈0.85nH / 0805≈1.0nH。量级正确即可支撑判断。

运行：python3 hardware/tools/decoupling_check.py
"""

import math
import re
import sys
from pathlib import Path

PCB = Path(__file__).resolve().parents[1] / "expansion-board-v4/kicad/expansion-board-v4.kicad_pcb"
L_PER_MM = 0.5e-9
L_VIA_PAIR = 0.6e-9
ESL_BY_PKG = (("0402", 0.5e-9), ("0603", 0.85e-9), ("0805", 1.0e-9), ("1206", 1.2e-9))


def _blocks(t, tag):
    for m in re.finditer(re.escape(tag), t):
        s = m.start(); d = 0; i = s
        while i < len(t):
            c = t[i]
            if c == "(":
                d += 1
            elif c == ")":
                d -= 1
                if d == 0:
                    break
            i += 1
        yield t[s:i + 1]


def load_pcb(path=PCB):
    """返回 (pads, value, footprint)；pads 为 (ref, pad, net, x, y) 的绝对坐标列表。"""
    t = path.read_text()
    pads, value, fps = [], {}, {}
    for blk in _blocks(t, "(footprint "):
        r = re.search(r'\(property "Reference" "([^"]+)"', blk)
        at = re.search(r"^\s*\(at ([\d.\-]+) ([\d.\-]+)(?: ([\d.\-]+))?\)", blk, re.M)
        if not r or not at:
            continue
        ref = r.group(1)
        fx, fy, rot = float(at.group(1)), float(at.group(2)), float(at.group(3) or 0)
        v = re.search(r'\(property "Value" "([^"]*)"', blk)
        value[ref] = v.group(1) if v else ""
        f = re.search(r'^\(footprint "([^"]*)"', blk)
        fps[ref] = f.group(1) if f else ""
        a = math.radians(-rot)
        for pb in _blocks(blk, '(pad "'):
            num = re.match(r'\(pad "([^"]*)"', pb).group(1)
            pat = re.search(r"\(at ([\d.\-]+) ([\d.\-]+)", pb)
            nm = re.search(r'\(net "([^"]*)"\)', pb)
            if not pat:
                continue
            px, py = float(pat.group(1)), float(pat.group(2))
            pads.append((ref, num, nm.group(1) if nm else "",
                         fx + px * math.cos(a) - py * math.sin(a),
                         fy + px * math.sin(a) + py * math.cos(a)))
    return pads, value, fps


def parse_cap(s):
    m = re.match(r"([\d.]+)\s*(pF|nF|uF|µF)", s, re.I)
    if not m:
        return None
    mult = {"pf": 1e-12, "nf": 1e-9, "uf": 1e-6, "µf": 1e-6}[m.group(2).lower()]
    return float(m.group(1)) * mult


def esl_of(fp_name):
    for key, v in ESL_BY_PKG:
        if key in fp_name:
            return v
    return 0.5e-9


def pin_impedance(pads, value, fps, ref, pad, net, f):
    """该引脚看到的全网并联去耦阻抗，以及各支路明细。"""
    tgt = [(x, y) for r, n, _, x, y in pads if r == ref and n == pad]
    if not tgt:
        return None, []
    px, py = tgt[0]
    Y, detail = 0j, []
    for r, n, nn, cx, cy in pads:
        if nn != net or not r.startswith("C"):
            continue
        C = parse_cap(value.get(r, ""))
        if not C:
            continue
        d = math.dist((px, py), (cx, cy))
        L = L_PER_MM * d + L_VIA_PAIR + esl_of(fps.get(r, ""))
        Z = 1j * 2 * math.pi * f * L + 1 / (1j * 2 * math.pi * f * C)
        Y += 1 / Z
        detail.append((d, r, value[r], abs(Z)))
    detail.sort()
    return (abs(1 / Y) if Y != 0 else float("inf")), detail


CASES = [
    ("U12", "1", "3V3_RF", 1090e6, "BGA2817 VCC"),
    ("U11", "7", "3V3_RF", 1090e6, "QPL9547（同网对照）"),
    ("U14", "1", "3V3_RF", 1090e6, "AD8313（同网对照）"),
    ("U15", "4", "3V3_RF", 1090e6, "TLV3501（同网对照）"),
    ("U8", "23", "RP_1V1", 500e6, "RP2040 DVDD pin23"),
    ("U8", "50", "RP_1V1", 500e6, "RP2040 DVDD pin50（对照）"),
    ("U8", "44", "3V3_DIG", 100e6, "RP2040 VREG_VIN"),
    ("U8", "22", "3V3_DIG", 200e6, "RP2040 IOVDD pin22"),
    ("U8", "33", "3V3_DIG", 200e6, "RP2040 IOVDD pin33（对照）"),
    ("U10", "45", "SUBG_VDDR", 500e6, "CC1312R VDDR"),
]

if __name__ == "__main__":
    if not PCB.exists():
        sys.exit(f"找不到 PCB: {PCB}")
    pads, value, fps = load_pcb()
    print("各电源引脚看到的【全网并联】去耦阻抗\n")
    print(f"{'引脚':30s} {'频率':>8} {'并联阻抗':>10} {'电容数':>6}  最近三颗")
    print("-" * 108)
    for ref, pad, net, f, label in CASES:
        z, det = pin_impedance(pads, value, fps, ref, pad, net, f)
        if z is None:
            continue
        near = ", ".join(f"{r}({v[:6]})@{d:.1f}mm" for d, r, v, _ in det[:3])
        print(f"{label:30s} {f/1e6:6.0f}MHz {z:9.2f}Ω {len(det):5d}   {near}")
    print()
    print("判读：横向比同一网络上各引脚的阻抗差异，比单看绝对值更有意义。")
