#!/usr/bin/env python3
"""LC 网络频响 / 匹配计算器（ABCD 矩阵级联，纯 numpy，不需要商业 EDA）。

用途：把"这个 LC 网络在某个频率上表现如何"从推测变成可计算的判据。
本轮就是靠它推翻了"978MHz 偏离 TI 参考设计中心频率会失配"这个错误推断。

元件表用 [(kind, value), ...] 描述，按信号方向从源到负载排列：
    ('L',  6.8e-9)   串联电感
    ('Cs', 3.6e-12)  串联电容
    ('C',  2.7e-12)  并联电容（对地）
    ('Lp', 3.6e-9)   并联电感（对地）

运行：python3 hardware/tools/lc_network_sim.py
"""

import numpy as np

Z0 = 50.0


def _series(Z):
    return np.array([[1, Z], [0, 1]], dtype=complex)


def _shunt(Y):
    return np.array([[1, 0], [Y, 1]], dtype=complex)


def abcd(f, comps):
    """级联元件表，返回 ABCD 矩阵。"""
    w = 2 * np.pi * f
    M = np.eye(2, dtype=complex)
    for kind, val in comps:
        if kind == "L":
            M = M @ _series(1j * w * val)
        elif kind == "Cs":
            M = M @ _series(1 / (1j * w * val))
        elif kind == "C":
            M = M @ _shunt(1j * w * val)
        elif kind == "Lp":
            M = M @ _shunt(1 / (1j * w * val))
        else:
            raise ValueError(f"未知元件类型: {kind}")
    return M


def sparam(f, comps, zs=Z0, zl=Z0):
    """返回 (S21, S11)。zs/zl 允许非 50Ω（天线匹配用）。"""
    A, B, C, D = abcd(f, comps).ravel()
    den = A * zl + B + C * zs * zl + D * zs
    S21 = 2 * np.sqrt(zs.real if hasattr(zs, "real") else zs) * np.sqrt(
        zl.real if hasattr(zl, "real") else zl
    ) / den
    S11 = (A * zl + B - C * zs.conjugate() * zl - D * zs.conjugate()) / den
    return S21, S11


def db(x):
    return 20 * np.log10(np.abs(x))


def cutoff_3db(comps, lo=100e6, hi=5e9, n=40001):
    fs = np.linspace(lo, hi, n)
    s = np.array([db(sparam(f, comps)[0]) for f in fs])
    idx = np.argmax(s < -3.0)
    return fs[idx] if idx else float("nan")


def input_impedance(f, comps, zl):
    """从源端看进去的输入阻抗（用于判断匹配网络把负载变换到了什么）。"""
    A, B, C, D = abcd(f, comps).ravel()
    return (A * zl + B) / (C * zl + D)


if __name__ == "__main__":
    # ---- ① 978/Sub-GHz 通道的 5 阶 π 型低通（板上 = LAUNCHXL-CC1312R1 同值）----
    lpf = [("C", 2.7e-12), ("L", 6.8e-9), ("C", 6.2e-12), ("L", 6.8e-9), ("C", 3.0e-12)]
    print("① Sub-GHz LC 低通 (C41/L11/C42/L12/C43) —— 50Ω 系统，理想元件")
    print(f"{'频率(MHz)':>10} {'S21(dB)':>9} {'S11(dB)':>9}  说明")
    for f, note in [
        (868e6, "TI 目标频段下沿"),
        (915e6, "TI 目标频段上沿"),
        (978e6, "本板实际使用 (UAT)"),
        (1736e6, "868 二次谐波"),
        (1830e6, "915 二次谐波"),
        (1956e6, "978 二次谐波"),
    ]:
        S21, S11 = sparam(f, lpf)
        print(f"{f/1e6:10.0f} {db(S21):9.2f} {db(S11):9.2f}  {note}")
    print(f"   −3dB 截止 ≈ {cutoff_3db(lpf)/1e6:.0f} MHz")
    print(f"   915→978 插损差 = {abs(db(sparam(978e6,lpf)[0]) - db(sparam(915e6,lpf)[0])):.3f} dB")

    # ---- ② 1090 IFA 的 π 型匹配：验证 BOM_IFA_TUNING 的 HFSS 起点值 ----
    # HFSS 估算天线在 1090MHz 约 21.15Ω（见 BOM_IFA_TUNING-zh_CN.md）
    # 板上拓扑：天线 ─┬─ ZS1(串) ─┬─ 电台侧
    #                ZP1(并)     ZP2(并)
    # 起点：ZP1=DNP、ZS1=3.6nH、ZP2=3.3pF
    print()
    print("② 1090 IFA π 型匹配 —— 验证 HFSS 起点 ZS1=3.6nH / ZP2=3.3pF / ZP1=DNP")
    f0 = 1090e6
    zant = 21.15 + 0j
    # ⚠️ 顺序按「从电台侧往天线看」排：先遇到并联的 ZP2，再遇到串联的 ZS1，最后是天线。
    #    传反了会得出完全错误的 VSWR —— 本工具第一版就栽在这里。
    for label, net in [
        ("BOM 起点 3.6nH/3.3pF", [("C", 3.3e-12), ("L", 3.6e-9)]),
        ("HFSS 理想 3.607nH/3.411pF", [("C", 3.411e-12), ("L", 3.607e-9)]),
    ]:
        zin = input_impedance(f0, net, zant)
        gamma = (zin - Z0) / (zin + Z0)
        vswr = (1 + abs(gamma)) / (1 - abs(gamma))
        print(f"   {label:28s} → 电台侧 {zin.real:6.2f}{zin.imag:+6.2f}j Ω"
              f"   RL={-db(gamma):5.1f} dB   VSWR={vswr:.2f}")
    # 手算校验：L 型匹配 21.15Ω → 50Ω
    q = np.sqrt(Z0 / zant.real - 1)
    print(f"   L 型公式校验: Q={q:.3f}  Xs={q*zant.real:.1f}Ω→{q*zant.real/(2*np.pi*f0)*1e9:.2f}nH"
          f"  Xp={Z0/q:.1f}Ω→{1/(2*np.pi*f0*Z0/q)*1e12:.2f}pF")
    print("   （天线阻抗取自 HFSS 估算，装壳后必须用 J7 口 VNA 实测复核）")
