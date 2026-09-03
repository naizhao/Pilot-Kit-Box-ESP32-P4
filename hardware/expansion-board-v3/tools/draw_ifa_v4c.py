#!/usr/bin/env python3
"""v4 IFA 天线实测定型图 —— 标准 IFA 拓扑（短路腿直接入 GND）。

替代此前的 A 方案（靠 14mm 接地引线拉高 R）。A 方案作废，原因是**结构性**的：

    接地引线把地平面上沿推到 14mm 之外
      → 馈线必须跟着在净空区里裸奔 14mm 才能碰到参考面
      → 而 6 层板上 F.Cu 的参考面是 In1，天线区要求上下全层镂空、In1 也是挖空的
      → 那段馈线下方没有参考地，50Ω 无从谈起，它会变成辐射体

这是一维长度账摊到二维板面上时漏掉的连带影响。

## 本图的拓扑（工业界标准 IFA）

    短路腿走到净空区边缘，**直接撞入 GND**——提供最干净、最低寄生的 0Ω 锚点
    馈电腿末端也落在净空区边缘，**出了边缘立刻是有 In1 参考面的 50Ω 微带**
    净空区恰好包住天线本体，不再拖尾巴
    阻抗靠 D（馈电脚↔短路脚间距，即抽头位置）和 π 网络调，不靠接地长尾巴

## 2026-08-24 预研结果（历史）

    HFSS 使用 V1 原始 56.0mm / 867MHz 和切短 41.2mm / 1090MHz 双点校准。
    当前 v4 实际位置（板边净距1.012mm）下，历史D=5.0三点反求50.187mm；
    按PCB实际D=4.988/H=6锚点修正为50.230mm，工程仍取50.2mm；
    画板 52.0mm，留 1.8mm 从开路端逐刀切短。
    完整六层 HFSS @50.0mm X=0：1059.052MHz，R=20.951Ω；校准后局部斜率19.83MHz/mm。
    匹配只建议从 3.6nH 串 + 3.3pF 接收侧并起扫，同时准备3.6/3.9pF。

## 2026-09-03 实板定型结果

    V4.0实板从53.5mm外包络逐刀切短到50.0mm，装盒+电池落在1082.5MHz。
    装盒+电池：SWR 1.09、45+j0.5Ω；电池铺地拆盒：1080–1090MHz漂移。
    v3/v4量产生成值均改为50.0mm外包络（48.5mm中心线），不再预留切短段。
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "build/ifa_v4c.png"

# 保持当前v4核心图形，绘制V4.0实板装盒VNA定型尺寸。
LEG = 6.0
ARM = 48.5
D = 4.988
W, PAD, CLEAR, RF_W = 1.5, 1.5, 2.0, 0.15
EDGE_CLEAR = 1.012
FEED_PATH = 5.1032
TAP = 1.5
WIDE_END = W / 2
MICROSTRIP = FEED_PATH - WIDE_END - TAP

S, M = 13, 64
BG = (28, 32, 38); CU = (198, 108, 58)
GND = (70, 96, 76); GND2 = (58, 80, 64); KEEP = (50, 56, 66)
TXT = (232, 234, 238); DIMC = (120, 190, 235); RED = (235, 90, 90); ACC = (240, 180, 70)

ARM_Y, FEED_X = 0.0, 0.0
SHORT_X, OPEN_X = -D, -D + ARM
LEG_END = LEG                      # 腿末端 = 净空区下沿 = 地平面上沿
GND_EDGE = LEG_END + WIDE_END     # 腿真实铜边；taper从这里开始

W_MM = ARM + 2 * CLEAR + 30
H_MM = LEG_END + CLEAR + 12
IMG_W, IMG_H = int(W_MM * S) + 2 * M, int(H_MM * S) + 2 * M + 52
img = Image.new("RGB", (IMG_W, IMG_H), BG)
d = ImageDraw.Draw(img)
X0 = M + int((D + CLEAR + 5) * S)


def px(x, y): return (X0 + int(x * S), M + 14 + int(y * S))
def rc(x1, y1, x2, y2, c):
    a, b = px(min(x1, x2), min(y1, y2)), px(max(x1, x2), max(y1, y2))
    d.rectangle([a, b], fill=c)
def st(x1, y1, x2, y2, w, c):
    hw = w / 2
    if abs(y1 - y2) < 1e-9: rc(x1 - hw, y1 - hw, x2 + hw, y1 + hw, c)
    else: rc(x1 - hw, min(y1, y2) - hw, x1 + hw, max(y1, y2) + hw, c)
def font(sz, b=False):
    p = "/System/Library/Fonts/STHeiti Medium.ttc" if b else "/System/Library/Fonts/STHeiti Light.ttc"
    try:
        f = ImageFont.truetype(p, sz, index=0)
        if f.getmask("主").getbbox(): return f
    except Exception: pass
    return ImageFont.load_default()
FB, F, FS = font(18, True), font(14), font(12)

# 板框：主臂上方就是板边（V1/V2/V3.2 都这么放），板外是空气
BOARD_TOP = ARM_Y - W / 2 - EDGE_CLEAR
d.line([px(SHORT_X - CLEAR - 8, BOARD_TOP), px(OPEN_X + CLEAR + 8, BOARD_TOP)],
       fill=(120, 130, 145), width=2)
d.text(px(FEED_X + 3.2, BOARD_TOP - 1.2), f"板边 — 按当前 v4 位置建模，铜边净距 {EDGE_CLEAR:.3f}mm",
       font=FS, fill=(130, 140, 155))
# 主体净空区包到腿真实铜边；馈电脚下方另有2.4mm宽F.Cu通道。
rc(SHORT_X - CLEAR - W / 2, ARM_Y - CLEAR - W / 2, OPEN_X + CLEAR + W / 2, GND_EDGE, KEEP)
# 地平面主体从腿铜边开始，短路腿端帽直接撞进来。
rc(SHORT_X - CLEAR - 6, GND_EDGE, OPEN_X + CLEAR + 6, GND_EDGE + 8.5, GND)
rc(SHORT_X - CLEAR - 6, GND_EDGE, OPEN_X + CLEAR + 6, GND_EDGE + 0.35, GND2)

# 天线
st(SHORT_X, ARM_Y, OPEN_X, ARM_Y, W, CU)
st(FEED_X, ARM_Y, FEED_X, LEG_END, W, CU)
st(SHORT_X, ARM_Y, SHORT_X, LEG_END, W, CU)                # 端帽在GND_EDGE直接扎进GND
rc(FEED_X - PAD / 2, LEG_END - PAD / 2, FEED_X + PAD / 2, LEG_END + PAD / 2, CU)
# 馈线：净空区边缘之下立刻是 50Ω 微带。
# F.Cu 上它两侧要有 clearance（不与顶层地平面铜相接），参考面在 In1——
# 这正是 A 方案做不到的：那里 In1 也被挖空了，没有参考面。
CORR = RF_W / 2 + 0.5
rc(FEED_X - CORR, LEG_END, FEED_X + CORR, LEG_END + 7.4, (40, 46, 54))
# 中心线端点后先保留0.75mm等宽端帽，再从真实铜边做完整1.5mm渐变，最后接2.853mm微带。
for _i in range(14):
    _t0, _t1 = _i / 14, (_i + 1) / 14
    _w0 = W + (RF_W - W) * _t0
    rc(FEED_X - _w0 / 2, GND_EDGE + TAP * _t0, FEED_X + _w0 / 2, GND_EDGE + TAP * _t1, RED)
st(FEED_X, GND_EDGE + TAP, FEED_X, LEG_END + FEED_PATH, RF_W, RED)
d.text(px(FEED_X + 2.0, LEG_END + 1.0),
       f"等宽 {WIDE_END:.2f} + taper {TAP:.1f} + 微带 {MICROSTRIP:.3f}mm（总路径 {FEED_PATH:.3f}）",
       font=FS, fill=(235, 150, 150))

def dim_h(x1, x2, y, lab, col=DIMC, dy=-17):
    p1, p2 = px(x1, y), px(x2, y)
    d.line([p1, p2], fill=col, width=1)
    for p in (p1, p2): d.line([(p[0], p[1] - 4), (p[0], p[1] + 4)], fill=col, width=1)
    tw = d.textlength(lab, font=FS)
    d.text(((p1[0] + p2[0]) / 2 - tw / 2, p1[1] + dy), lab, font=FS, fill=col)

def dim_v(y1, y2, x, lab, col=DIMC):
    p1, p2 = px(x, y1), px(x, y2)
    d.line([p1, p2], fill=col, width=1)
    for p in (p1, p2): d.line([(p[0] - 4, p[1]), (p[0] + 4, p[1])], fill=col, width=1)
    d.text((p1[0] + 7, (p1[1] + p2[1]) / 2 - 8), lab, font=FS, fill=col)

dim_h(SHORT_X, OPEN_X, ARM_Y - CLEAR - 1.2, f"主臂中心线 {ARM:.2f}mm（50.0mm铜箔外包络，实板定型）")
dim_h(SHORT_X, FEED_X, LEG_END + 9.4, f"D = {D:.3f}mm（中心线↔中心线；调阻抗抽头）", dy=5)
dim_v(ARM_Y, LEG_END, SHORT_X - 5.0, f"腿 {LEG:.1f}mm")

d.text(px(SHORT_X - 1.5, LEG_END + 3.3), "短路腿直接入GND（0Ω锚点）", font=FS, fill=(180, 225, 195))
d.text(px(FEED_X + 2.0, LEG_END + 4.7), "馈电腿出净空后立刻进入有In1参考面的50Ω微带", font=FS, fill=(235, 130, 130))
d.text(px(FEED_X + 8.0, LEG_END - 1.5), f"六层净空 {CLEAR:.0f}mm：不拖接地长尾巴", font=FS, fill=(160, 170, 185))
d.text(px(OPEN_X - 8.5, ARM_Y + 1.5), "开路端（向右）", font=FS, fill=(240, 150, 120))

# 前面的净空矩形会盖住先画的板边文字，这里最后重画，保证输出可读。
d.line([px(SHORT_X - CLEAR - 8, BOARD_TOP), px(OPEN_X + CLEAR + 8, BOARD_TOP)],
       fill=(120, 130, 145), width=2)
d.text(px(FEED_X + 8.0, BOARD_TOP + 0.25), f"板边：按当前v4位置，铜边净距 {EDGE_CLEAR:.3f}mm",
       font=FS, fill=(150, 160, 175))

y0 = IMG_H - 74
d.text((M, y0 - 28), "v4 板载 1090MHz IFA — V4.0实板装盒VNA定型为50.0mm外包络", font=FB, fill=TXT)
for i, t in enumerate([
    f"实测定型：主臂中心线{ARM:.1f}mm / 铜箔外包络{ARM + W:.1f}mm；D={D:.3f}mm，H={LEG:.1f}mm",
    "装盒+电池 @1082.5MHz：SWR 1.09，Z=45+j0.5Ω；ZS1=0R、ZP1/ZP2=DNP",
    "电池铺地拆盒：1080–1090MHz漂移；装盒+电池状态为量产基准",
    "v3/v4生成器、封装库和PCB统一回灌50.0mm；到H2孔边21.228mm",
]):
    d.text((M, y0 + i * 20), t, font=FS, fill=(170, 180, 195))

os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
img.save(OUT)
print(f"saved: {OUT} {img.size}   主臂中心线 {ARM:.2f}  外包络 {ARM + W:.2f}  腿 {LEG:.0f}  D {D:.3f}")
