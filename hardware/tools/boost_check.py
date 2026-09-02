#!/usr/bin/env python3
"""同步升压（SY7069）工作点核算：换电感后的纹波、峰值/谷值电感电流、最大输出能力。

背景：V4.3 把 L17 从 1.5µH 换成 4.7µH（XEL4030-472MEC）、C77 从 22µF/10V 换成 22µF/16V。
换电感会同时改变纹波、峰值电流和「受谷值限流约束的最大输出能力」，旧计算不再适用。

判据来源（`SY7069.pdf`）：
    VOUT = 1.2 × (1 + RH/RL)，板上 R41/R42 = 470k/150k → 4.96V
    Fsw = 1.0 MHz（典型）
    「Min 3A valley current limit」→ 限的是电感电流**谷值**，不是峰值
    OUT 对地 ≥22µF、IN 对地 ≥1µF

CCM 升压公式：
    D        = 1 − VIN/VOUT
    IL_avg   = IOUT / (1 − D)
    ΔIL      = VIN × D / (L × fsw)
    IPK      = IL_avg + ΔIL/2 ；  IVALLEY = IL_avg − ΔIL/2
    ΔVOUT    ≈ IOUT × D / (COUT_eff × fsw) + ΔIL × ESR

⚠️ 陶瓷电容的 DC bias 衰减必须计入：X5R 在接近额定电压时有效容值可掉 40–60%，
   这正是把 C77 从 10V 档换到 16V 档的原因。

⚠️⚠️ 本工具只算**稳态工作点**，算不了以下三件事，它们只能靠实测：
   1. **环路稳定性 / 相位裕量**。Boost 拓扑有右半平面零点 f_RHPZ = R_load·(1−D)²/(2π·L)，
      电感从 1.5µH 增到 4.7µH 会让该零点**左移约 3 倍**，侵蚀内部补偿的相位裕量。
      这是"换大电感"唯一可能变坏的地方，本工具完全看不到 → 必须做动态负载阶跃测试。
   2. **效率**。下面所有输出电流都是效率 100% 的理想上限；按 90%/85% 折算需再打折。
   3. 电池内阻、热降额、瞬态输入跌落。

运行：python3 hardware/tools/boost_check.py
"""

VOUT = 4.96
FSW = 1.0e6
IVALLEY_LIMIT = 3.0      # 手册 min 3A valley current limit
ESR = 0.005              # 陶瓷电容等效 ESR，5mΩ


def op_point(vin, iout, L, cout_eff):
    d = 1 - vin / VOUT
    il_avg = iout / (1 - d)
    dil = vin * d / (L * FSW)
    ipk = il_avg + dil / 2
    ival = il_avg - dil / 2
    vrip = iout * d / (cout_eff * FSW) + dil * ESR
    return d, il_avg, dil, ipk, ival, vrip


def max_iout(vin, L):
    """受谷值限流约束的最大输出电流：IL_avg − ΔIL/2 ≤ 3A"""
    d = 1 - vin / VOUT
    dil = vin * d / (L * FSW)
    return (IVALLEY_LIMIT + dil / 2) * (1 - d)


CONFIGS = [("旧 V4.2  L=1.5µH  C=22µF/10V", 1.5e-6, 22e-6 * 0.50),
           ("新 V4.3  L=4.7µH  C=22µF/16V", 4.7e-6, 22e-6 * 0.70)]

if __name__ == "__main__":
    print("SY7069 升压工作点核算（VOUT=4.96V, Fsw=1MHz, 谷值限流 3A）")
    print("电池电压取 3.0V(将耗尽) / 3.7V(标称) / 4.2V(满电)\n")

    for label, L, ceff in CONFIGS:
        print(f"■ {label}   有效容值按 DC bias 折算 = {ceff*1e6:.1f}µF")
        print(f"  {'VIN':>5} {'IOUT':>6} {'D':>6} {'ΔIL':>7} {'IPK':>7} {'谷值':>7} {'输出纹波':>9}")
        for vin in (3.0, 3.7, 4.2):
            for iout in (0.3, 0.8, 1.5):
                d, ila, dil, ipk, ival, vrip = op_point(vin, iout, L, ceff)
                flag = "  ← 谷值超限!" if ival > IVALLEY_LIMIT else ""
                print(f"  {vin:5.1f}V {iout:5.2f}A {d:6.3f} {dil*1000:6.0f}mA "
                      f"{ipk:6.2f}A {ival:6.2f}A {vrip*1000:8.1f}mV{flag}")
        print(f"  受谷值限流约束的最大输出：", end="")
        print("  ".join(f"VIN={v}V→{max_iout(v,L):.2f}A" for v in (3.0, 3.7, 4.2)))
        worst = max(op_point(v, max_iout(v, L), L, ceff)[3] for v in (3.0, 3.7, 4.2))
        print(f"  最坏峰值电感电流 = {worst:.2f}A  →  电感 Isat 建议 ≥ {worst*1.2:.1f}A（+20% 裕量）\n")
