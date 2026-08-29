#!/usr/bin/env python3
"""【历史作废方案】画 v4 IFA 长接地引线方案（俯视，按比例）。

2026-08-24：本图的K外推、固定-30MHz装盒偏移和14mm接地尾巴均不再用于v4决策。
当前预研图是draw_ifa_v4c.py，结论见../IFA_HFSS_2026-08-24.md。保留本脚本只为追溯。

参数来自 IFA_ANTENNA.md §5.4 的设计空间表，选点是「照 V1 配方」：
接地引线 14mm —— V1 实测 R=43.75Ω / SWR 1.197，是唯一有干净实测背书的方案。

    K = 66,012 MHz·mm（V1+V2 双板标定，差 0.3%）
    装盒目标 1090MHz → L 60.561mm
    装盒下移 2.5%（V3.2 实测）→ 裸板目标 1117MHz → L 59.108mm
    L = 主臂 + 腿 + 接地引线 = 39.108 + 6.0 + 14.0
    画长待切：主臂画 41.11mm，出厂切 2mm 到 39.11mm

用法：draw_ifa_v4.py [输出png]
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "build/ifa_v4_plan.png"
S = 18                     # px per mm
M = 72                     # 边距 px

# ── 设计参数（mm）────────────────────────────────────────────────
ARM_DRAW = 41.11           # 主臂中心线，画长待切
ARM_FINAL = 39.11          # 切后目标
CUT = ARM_DRAW - ARM_FINAL
LEG = 6.0
D = 5.0                    # 两腿中心距
W = 1.5                    # 天线铜箔线宽
STUB_W = 0.3               # 接地引线宽（V1 用 0.254 起步，这里取 0.3 便于制造）
CLEAR = 2.0                # 天线四周净空
PAD = 1.5
RF_W = 0.15                # 50Ω 馈线

# 坐标系：主臂中心线 y=0，向下为 +y。开路端在左，短路腿在右（x 大）。
ARM_Y = 0.0
SHORT_X = 0.0              # 短路腿（主臂右端）
FEED_X = -D                # 馈电腿
OPEN_X = -ARM_DRAW         # 开路端
LEG_END = LEG              # 腿末端 y

# 接地引线：从短路腿末端往下折，总长 14mm，走到地平面
# 竖 4.0 → 45°斜 3.0 → 竖 4.5 → 水平 2.5 ≈ 14.0
# 折线展开长必须 = 14.0mm：竖3.0 + 45°斜3.0(边2.121) + 竖3.0 + 45°斜2.0(边1.414) + 竖3.0
STUB = [(SHORT_X, LEG_END), (SHORT_X, LEG_END + 3.0),
        (SHORT_X + 2.121, LEG_END + 5.121), (SHORT_X + 2.121, LEG_END + 8.121),
        (SHORT_X + 3.535, LEG_END + 9.535), (SHORT_X + 3.535, LEG_END + 12.535)]
GND_TOP_Y = LEG_END + 12.535       # 地平面上沿 = 引线终点

# 馈线：馈电腿末端往下，50Ω
FEED_LINE = [(FEED_X, LEG_END), (FEED_X, LEG_END + 6.0)]

W_MM = ARM_DRAW + 2 * CLEAR + 22
H_MM = GND_TOP_Y + 5 + CLEAR
IMG_W = int(W_MM * S) + 2 * M
IMG_H = int(H_MM * S) + 2 * M + 40

BG = (28, 32, 38)
CU = (198, 108, 58)
CU_CUT = (150, 60, 40)
GND = (70, 96, 76)
KEEP = (46, 52, 60)
TXT = (232, 234, 238)
DIM = (120, 190, 235)
ACC = (240, 180, 70)
RED = (235, 90, 90)

img = Image.new("RGB", (IMG_W, IMG_H), BG)
d = ImageDraw.Draw(img)

X0 = M + int((ARM_DRAW + CLEAR + 3) * S)      # 屏幕原点（短路腿处）


def px(x, y):
    return (X0 + int(x * S), M + int(y * S))


def rect_mm(x1, y1, x2, y2, fill, outline=None):
    a, b = px(min(x1, x2), min(y1, y2)), px(max(x1, x2), max(y1, y2))
    d.rectangle([a, b], fill=fill, outline=outline)


def strip(x1, y1, x2, y2, w, fill):
    hw = w / 2
    if abs(y1 - y2) < 1e-9:
        rect_mm(x1 - hw, y1 - hw, x2 + hw, y1 + hw, fill)
    elif abs(x1 - x2) < 1e-9:
        rect_mm(x1 - hw, min(y1, y2) - hw, x1 + hw, max(y1, y2) + hw, fill)
    else:
        d.line([px(x1, y1), px(x2, y2)], fill=fill, width=max(2, int(w * S)))


def font(sz, bold=False):
    """⚠️ PingFang.ttc 在 PIL 里渲染中文出方块（TTC 索引/字形表读不出来），
    实测 STHeiti 可用。改字体前先跑一次 getmask('主臂').getbbox() 验证。"""
    for p in ("/System/Library/Fonts/STHeiti Medium.ttc" if bold else
              "/System/Library/Fonts/STHeiti Light.ttc",
              "/System/Library/Fonts/STHeiti Light.ttc",
              "/System/Library/Fonts/Hiragino Sans GB.ttc"):
        if os.path.exists(p):
            try:
                f = ImageFont.truetype(p, sz, index=0)
                if f.getmask("主").getbbox():
                    return f
            except Exception:
                pass
    return ImageFont.load_default()


F, FB, FS = font(15), font(17, True), font(12)

# 源码注释不够：图片脱离仓库后也必须能一眼看出已经作废。
d.rectangle([0, 0, IMG_W, 46], fill=(100, 24, 24))
d.text((M, 12), "历史作废方案：39.11/41.11mm与14mm接地尾巴不得用于投板；当前值见ifa_v4c.png",
       font=FB, fill=(255, 225, 225))

# ── 无铜区（净空）──────────────────────────────────────────────────
# ⚠️ 必须**包住整条接地引线**，一直到它进入地平面那一点。
#    「两条腿全长不得被 GND 包围」是硬约束，而接地引线是短路腿的延续——
#    V3.2 就是腿走到一半被铺铜咬住，有效 D 塌成 4.988mm、R 掉到 25.79Ω。
rect_mm(OPEN_X - CLEAR - W / 2, ARM_Y - CLEAR - W / 2,
        SHORT_X + CLEAR + W / 2, LEG_END + CLEAR, KEEP)
sx = [p[0] for p in STUB]
rect_mm(min(sx) - CLEAR, LEG_END, max(sx) + CLEAR, GND_TOP_Y, KEEP)
# 50Ω 馈线两侧也要无铜走廊（§3.3）：半宽 = 半线宽 + 0.5mm ≈ 5 倍介质厚，
# 否则线旁 0.15mm 就是铜，实物是共面波导而不是微带，50Ω 白算。
CORR = RF_W / 2 + 0.5
fy = [p[1] for p in FEED_LINE]
rect_mm(FEED_X - CORR, min(fy), FEED_X + CORR, max(fy), KEEP)
# ── 地平面 ────────────────────────────────────────────────────────
rect_mm(OPEN_X - CLEAR - 4, GND_TOP_Y, SHORT_X + CLEAR + 6, GND_TOP_Y + 4.5, GND)
d.text(px(OPEN_X + 4, GND_TOP_Y + 1.5), "GND 地平面（天线区上下全层镂空）", font=F, fill=(185, 225, 195))

# ── 天线本体 ──────────────────────────────────────────────────────
strip(OPEN_X + CUT, ARM_Y, SHORT_X, ARM_Y, W, CU)              # 主臂（切后保留段）
strip(OPEN_X, ARM_Y, OPEN_X + CUT, ARM_Y, W, CU_CUT)           # 待切段
for _t in range(6):                                             # 斜纹：提示这段要切掉
    _x = OPEN_X + 0.15 + _t * (CUT - 0.3) / 6
    d.line([px(_x, ARM_Y - W / 2), px(_x + 0.5, ARM_Y + W / 2)], fill=(235, 150, 130), width=1)
strip(FEED_X, ARM_Y, FEED_X, LEG_END, W, CU)                   # 馈电腿
strip(SHORT_X, ARM_Y, SHORT_X, LEG_END, W, CU)                 # 短路腿
for cx in (FEED_X, SHORT_X):
    rect_mm(cx - PAD / 2, LEG_END - PAD / 2, cx + PAD / 2, LEG_END + PAD / 2, CU)
for a, b in zip(STUB[:-1], STUB[1:]):
    strip(a[0], a[1], b[0], b[1], STUB_W, ACC)
for a, b in zip(FEED_LINE[:-1], FEED_LINE[1:]):
    strip(a[0], a[1], b[0], b[1], RF_W, (235, 90, 90))

# ── 标注 ──────────────────────────────────────────────────────────
def dim_h(x1, x2, y, label, col=DIM, off=0):
    p1, p2 = px(x1, y), px(x2, y)
    d.line([p1, p2], fill=col, width=1)
    for p in (p1, p2):
        d.line([(p[0], p[1] - 4), (p[0], p[1] + 4)], fill=col, width=1)
    tw = d.textlength(label, font=FS)
    d.text(((p1[0] + p2[0]) / 2 - tw / 2, p1[1] - 16 + off), label, font=FS, fill=col)


def dim_v(y1, y2, x, label, col=DIM):
    p1, p2 = px(x, y1), px(x, y2)
    d.line([p1, p2], fill=col, width=1)
    for p in (p1, p2):
        d.line([(p[0] - 4, p[1]), (p[0] + 4, p[1])], fill=col, width=1)
    d.text((p1[0] + 6, (p1[1] + p2[1]) / 2 - 7), label, font=FS, fill=col)


dim_h(OPEN_X, SHORT_X, ARM_Y - CLEAR - 1.1, f"主臂中心线 {ARM_DRAW:.2f}mm（画长待切）")
dim_h(OPEN_X, OPEN_X + CUT, ARM_Y - 0.1, f"切 {CUT:.1f}", col=(240, 130, 110), off=-14)
dim_h(FEED_X, SHORT_X, LEG_END + 2.6, f"D={D:.1f}")
dim_v(ARM_Y, LEG_END, SHORT_X + 2.6, f"腿 {LEG:.1f}")
dim_v(LEG_END, GND_TOP_Y, SHORT_X + 7.4, f"接地引线 14.0mm（折线展开长）")

d.text(px(FEED_X - 13.5, LEG_END + 6.9), f"50Ω 馈线 {RF_W}mm → π 匹配 → J7 调试口", font=FS, fill=(235, 120, 120))
d.text(px(OPEN_X + 1.2, ARM_Y + 1.4), "开路端（从这头切）", font=FS, fill=(240, 150, 120))
d.text(px(FEED_X - 3.2, ARM_Y - 1.9), "馈电腿", font=FS, fill=TXT)
d.text(px(SHORT_X - 1.0, ARM_Y - 1.9), "短路腿", font=FS, fill=TXT)
d.text(px(OPEN_X - CLEAR + 0.3, LEG_END + CLEAR - 1.3), f"净空 {CLEAR:.1f}mm（四周无铜，上下全层镂空）",
       font=FS, fill=(155, 165, 180))
d.text(px(SHORT_X - 14.0, GND_TOP_Y - 1.4), "↑ 接地引线全长两侧无 GND（V3.2 就是这里被铺铜咬住，R 塌到 25.79Ω）",
       font=FS, fill=(240, 190, 110))

# ── 页脚参数表 ────────────────────────────────────────────────────
y0 = IMG_H - 66
d.text((M, y0 - 26), "历史作废：v4 长接地引线方案（仅保留追溯）", font=FB, fill=RED)
info = [
    f"L = 主臂 + 腿 + 接地引线 = 39.11 + 6.0 + 14.0 = 59.11mm  →  裸板 1117MHz",
    f"装盒下移 2.5%（V3.2 实测 1250→1220）→ 装盒 1090MHz    K = 66,012 MHz·mm（V1+V2 标定，差 0.3%）",
    f"预期 R ≈ 44Ω / SWR ≈ 1.2（V1 同配方实测 43.75Ω、SWR 1.197）   线宽 {W}mm   引线宽 {STUB_W}mm",
]
for i, t in enumerate(info):
    d.text((M, y0 + i * 19), t, font=FS, fill=(170, 180, 195))

# 最后再覆盖一次横幅，避免上方尺寸线/文字盖住“历史作废”提示。
d.rectangle([0, 0, IMG_W, 46], fill=(100, 24, 24))
d.text((M, 12), "历史作废方案：39.11/41.11mm与14mm接地尾巴不得用于投板；当前值见ifa_v4c.png",
       font=FB, fill=(255, 225, 225))

os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
img.save(OUT)
print(f"saved: {OUT}  ({IMG_W}×{IMG_H})")
