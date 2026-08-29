#!/usr/bin/env python3
"""IFA 谐振长度标定 —— 只用从 gerber 解析出的一手几何 + NanoVNA 实测频率。

⚠️ 这个脚本存在的理由：2026-08-23 之前的所有长度推算都是错的。
   错法是拿 **V3.2 改造态**（切开短路腿、切断过孔、外接 6mm 铜线、装盒）测到的
   1220MHz 去反推「主臂 + 腿」的 f·L 常数——输入几何和输出几何根本不是同一个东西，
   常数从根上就脏。由它推出的「需要 49.8mm」比正确值长了 5mm。
   **凡是长度结论，一律跑这个脚本重算，不要相信任何文档里写死的数字。**

   几何来源：~/Downloads/jlc backup/ 里两个 JLC 打样 zip 的 gerber，
   由 ifa_gerber_parse.py 解析（D 码是老式 G54D12*，%FSLAX45Y45 除以 1e5）。

纪律：不使用任何记忆中的数字。几何来自 parse_gerber.py，频率来自实测记录。

IFA 的谐振长度定义（教科书口径）：
    L = 开路端 → 短路腿的主臂长度 + 短路腿长度
两块板的短路腿都在主臂末端，所以 L = 主臂中心线跨度 + 腿中心线长度。
"""
import json

C = 299_792.458          # mm·MHz

BOARDS = {
    "V1": dict(arm=56.000, leg=6.000, stub=14.024, D=5.000, f=867.0, R=43.75,
               gnd="天线区上下全镂空；天线正下方地平面上沿只到 y=64.188",
               path="腿末端→0.254mm×4.396→0.508mm×9.628→进地平面(61.976,58.420)"),
    "V2": dict(arm=39.488, leg=6.000, stub=1.466, D=5.000, f=1408.0, R=30.33,
               gnd="顶层大铺铜与天线区重叠；馈电腿被包围、短路腿完整",
               path="腿末端→0.508mm×1.466→直接进顶层铺铜(65.763,58.443)"),
}

print("=== 谐振长度口径 ===")
print("L = 主臂中心线 + 腿 + **接地引线**，一直算到电流真正进入地平面那一点。")
print("接地引线**不能漏**：漏掉它两块板的 f·L 差 19.1%，算上它只差 0.3%。\n")

for k, b in BOARDS.items():
    b["L"] = b["arm"] + b["leg"] + b["stub"]
    b["K"] = b["f"] * b["L"]
    b["lam0"] = C / b["f"]
    b["eeff"] = (b["lam0"] / (4 * b["L"])) ** 2
    print(f"{k}: 主臂 {b['arm']:.3f} + 腿 {b['leg']:.3f} + 引线 {b['stub']:.3f} = L {b['L']:.3f}mm")
    print(f"    f {b['f']:.0f}MHz   f·L = {b['K']:,.0f} MHz·mm   εeff {b['eeff']:.3f}   实测 R {b['R']:.2f}Ω")
    print(f"    接地路径: {b['path']}")
    print(f"    地平面:   {b['gnd']}")

K1, K2 = BOARDS["V1"]["K"], BOARDS["V2"]["K"]
K = (K1 + K2) / 2
print(f"\n两块板 f·L 相差 {abs(K2/K1-1)*100:.1f}%  →  取平均 K = {K:,.0f} MHz·mm")
print("（作为对照：漏掉接地引线、只算「主臂+腿」时，两者相差 19.1%）")

print("\n=== 外推 1090MHz ===")
L = K / 1090.0
print(f"  所需 L（主臂+腿+引线）= {L:.3f}mm")
print(f"\n  引线长度是**设计变量**，它同时决定频率和阻抗。给定腿 6.0mm：")
for stub in (0.0, 1.5, 4.4, 10.0, 14.0, 20.0):
    arm = L - 6.0 - stub
    print(f"    引线 {stub:5.1f}mm → 主臂中心线 {arm:6.3f}mm → 铜箔包络 {arm+1.5:6.3f}mm")

print("\n=== 引线长度 → 输入电阻（两点实测外推）===")
b1, b2 = BOARDS["V2"], BOARDS["V1"]          # 短引线 → 长引线
slope = (b2["R"] - b1["R"]) / (b2["stub"] - b1["stub"])
print(f"  V2 引线 {b1['stub']:.3f}mm → R {b1['R']:.2f}Ω")
print(f"  V1 引线 {b2['stub']:.3f}mm → R {b2['R']:.2f}Ω")
print(f"  斜率 {slope:.3f} Ω/mm（另有 V3.2：引线≈0（中途入地）→ R 25.79Ω，趋势一致）")
need = b2["stub"] + (50.0 - b2["R"]) / slope
print(f"  外推 R=50Ω 需引线 ≈ {need:.1f}mm  →  主臂中心线 {L-6.0-need:.3f}mm")
print("  ⚠️ 两点外推，且 R 还受净空/地平面尺寸影响，只用于判断量级。")

print("\n=== dF/dL 灵敏度 ===")
for dL in (1.0, 2.0, 3.0):
    print(f"  L ±{dL:.0f}mm → 频率 {K/(L+dL)-1090:+.0f} / {K/(L-dL)-1090:+.0f} MHz")

json.dump({k: {kk: vv for kk, vv in v.items()} for k, v in BOARDS.items()},
          open("/tmp/ifa_cal/calibration.json", "w"), ensure_ascii=False, indent=2)
print("\n落盘: /tmp/ifa_cal/calibration.json")
