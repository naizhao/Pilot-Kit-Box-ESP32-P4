#!/usr/bin/env python3
"""把 U.FL→SMA 尾线（pigtail）从 NanoVNA 读数里去掉，还原板上真实阻抗。

## 为什么需要它

U.FL 没有现成的 SOL 校准件，所以校准面只能停在 VNA 的 SMA 口，尾线留在测量
链路里。尾线对读数做**两件**事，别只记住第一件：

    ① 相位旋转 → 改变 R/X 的分配。X 过零的频率被挪动，
                 「Smith 图何时穿实轴」这个判据不可用。
    ② 损耗衰减 → 把 |Γ| 拉向圆心，**SWR 被压得偏乐观**。
                 已匹配的负载几乎不受影响，严重失配的负载失真很大。

    实测对照（同一根尾线，往返 0.55dB）：
        J7 天线   |Γ|=0.04   SWR 1.083 → 1.089    可忽略
        J6 开路线 |Γ|=0.885  SWR 10.84 → 16.40    压低了三分之一

**唯一不受影响的是「SWR 曲线最低点的频率位置」**——损耗随频率缓变，不会挪动
最低点。所以：**中心频率照 SWR 曲线判断，始终可靠；阻抗数值必须 de-embed。**

2026-09-04 的实测教训：V4.4 板 J6（开路线）读到 4.92-j12.75（等效 11.45pF），
而理论只有 1.6pF，差 7 倍，一度以为板子坏了。去掉尾线后是 4.87-j38.50
（3.79pF），与「走线 1.47pF + 焊盘 0.84pF + U.FL 座寄生」对得上账。
同一次测量里 J7（天线）却看着完全正常——因为它本来就匹配，
**50Ω 线转不动 50Ω 负载**，尾线对已匹配的负载几乎不起作用。
这个不对称正是最容易误判的地方。

## 用法

    先测一次**尾线空接**（末端什么都不接）的开路读数，记下 R 和 X：

        python3 vna_deembed.py --open 9.984 -115.2 --meas 4.917 -12.75

    --open 是空尾线开路读数，--meas 是接了被测物的读数，单位都是欧姆。
    每换一次校准/换一根尾线，都要重新测 --open。

## 原理

开路时 Zoc = Z0·coth(γl)，所以 tanh(γl) = Z0/Zoc **直接由测量给出**，
不需要知道尾线多长、速度因子多少、损耗多大。

    Zin = Z0·(ZL + Z0·tanh(γl)) / (Z0 + ZL·tanh(γl))
    ZL  = Z0·(Zin - Z0·tanh(γl)) / (Z0 - Zin·tanh(γl))

⚠️ tanh 的周期是 jπ，所以 tanh(γl) 对 βl 的 180° 多值性**免疫**——
尾线是 20mm 还是 220mm 都不影响结果，这是这个方法比"量长度再算"可靠的原因。
"""

import argparse
import cmath
import math

Z0 = 50.0


def tanh_gl(z_open):
    """由空尾线的开路读数反解 tanh(γl)。Zoc = Z0·coth(γl)。"""
    return Z0 / z_open


def deembed(z_meas, t):
    """去掉尾线，得到板上真实阻抗。"""
    return Z0 * (z_meas - Z0 * t) / (Z0 - z_meas * t)


def gamma(z):
    return (z - Z0) / (z + Z0)


def swr(z):
    g = abs(gamma(z))
    return float("inf") if g >= 1 else (1 + g) / (1 - g)


def describe(tag, z, f_hz):
    g = abs(gamma(z))
    rl = float("inf") if g == 0 else -20 * math.log10(g)
    line = (f"  {tag:14s} {z.real:8.2f} {z.imag:+8.2f}j Ω   "
            f"SWR {swr(z):6.3f}   回损 {rl:5.2f} dB")
    if abs(z.imag) > 1e-9 and f_hz:
        c = 1 / (2 * math.pi * f_hz * abs(z.imag))
        l = abs(z.imag) / (2 * math.pi * f_hz)
        line += (f"   等效 {c * 1e12:.2f} pF" if z.imag < 0
                 else f"   等效 {l * 1e9:.2f} nH")
    return line


def main():
    p = argparse.ArgumentParser(
        description="把 U.FL 尾线从 NanoVNA 读数里去掉",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="例: vna_deembed.py --open 9.984 -115.2 --meas 54.09 -0.819 -f 1090")
    p.add_argument("--open", nargs=2, type=float, required=True, metavar=("R", "X"),
                   help="尾线**空接**时的开路读数（欧姆）")
    p.add_argument("--meas", nargs=2, type=float, required=True, metavar=("R", "X"),
                   help="接上被测物后的读数（欧姆）")
    p.add_argument("-f", "--freq", type=float, default=1090.0,
                   help="频率 MHz，仅用于换算等效电容/电感（默认 1090）")
    a = p.parse_args()

    z_open = complex(*a.open)
    z_meas = complex(*a.meas)
    f = a.freq * 1e6

    t = tanh_gl(z_open)
    gl = cmath.atanh(t)
    z_board = deembed(z_meas, t)

    print(f"\n尾线特性（由空接开路读数反解，频率 {a.freq:.1f} MHz）")
    print(f"  开路读数 {z_open.real:.3f} {z_open.imag:+.2f}j Ω")
    print(f"  γl = {gl.real:.5f} + {gl.imag:.5f}j"
          f"   → 衰减 {gl.real * 8.686:.3f} dB(单程)，相移 {math.degrees(gl.imag):.2f}°")
    print(f"  tanh(γl) = {t.real:.5f} {t.imag:+.5f}j")

    print("\n结果")
    print(describe("含尾线", z_meas, f))
    print(describe("板上真实", z_board, f))

    d_swr = swr(z_board) - swr(z_meas)
    g_meas, g_board = abs(gamma(z_meas)), abs(gamma(z_board))
    print(f"\n  X   变化 {z_board.imag - z_meas.imag:+8.2f}Ω   ← 相位旋转的后果。"
          f"「Smith 图何时穿实轴」这个判据被污染，不可用")
    print(f"  SWR 变化 {d_swr:+8.4f}    ← 损耗的后果（本尾线往返 "
          f"{2 * gl.real * 8.686:.2f} dB）")
    if g_meas > 0.5:
        print(f"       ⚠️ |Γ|={g_meas:.3f} 属严重失配，尾线损耗把 SWR "
              f"**压得偏乐观**（真实值更差）。这种读数不 de-embed 会看走眼。")
    else:
        print(f"       |Γ|={g_meas:.3f} 已接近匹配，损耗影响可忽略，测得值直接可用。")
    print("\n  记住两者的分工：")
    print("    · 中心频率 → 看 SWR 曲线**最低点的位置**。损耗随频率缓变，")
    print("      不会挪动最低点，所以频率判断始终可靠，不必 de-embed。")
    print("    · 阻抗数值 → 必须 de-embed。失配越重，未处理的读数越失真。\n")


if __name__ == "__main__":
    main()
