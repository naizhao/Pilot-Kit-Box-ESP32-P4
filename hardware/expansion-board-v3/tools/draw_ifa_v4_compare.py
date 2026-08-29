#!/usr/bin/env python3
"""【历史作废方案】三个 v4 天线方案的净空占地对比图。

2026-08-24：A/B/C三组尺寸和共同的K外推长度均已退出当前决策。现行预研是保持标准IFA
图形、短路腿直接入地、最终主臂50.2mm/画板52.0mm；见draw_ifa_v4c.py和HFSS报告。

同一个总长 L=59.11mm（主臂+腿+接地引线），把长度在三个部件之间挪，
看**净空区**的形状差多少——这才是板面成本，不是天线本身的尺寸。
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

K, RATIO = 66012.0, 1250.0/1220.0
L = K/(1090.0*RATIO)
W, PAD, CLEAR, STUB_W, RF_W = 1.5, 1.5, 2.0, 0.3, 0.15
D = 5.0
SLOPE, R1, S1 = 1.069, 43.75, 14.024

PLANS = [
    ("历史A  V1配方（已否决）", 14.0, 6.0, "旧净空 = 天线矩形 + 一条14mm长尾巴"),
    ("历史B  长腿短引线（已否决）", 4.0, 12.0, "旧净空 = 单个矩形，尾巴只剩4mm"),
    ("历史C  直短路腿但旧尺寸（已否决）", 0.0, 12.0, "旧净空 = 单个矩形，无尾巴"),
]

S, M = 11, 56
BG=(28,32,38); CU=(198,108,58); GND=(70,96,76); KEEP=(52,58,68)
TXT=(232,234,238); ACC=(240,180,70); RED=(235,90,90); DIMC=(120,190,235)

def font(sz, bold=False):
    p = "/System/Library/Fonts/STHeiti Medium.ttc" if bold else "/System/Library/Fonts/STHeiti Light.ttc"
    try:
        f = ImageFont.truetype(p, sz, index=0)
        if f.getmask("主").getbbox(): return f
    except Exception: pass
    return ImageFont.load_default()
FB, F, FS = font(17, True), font(14), font(11)

arms = [L-leg-stub for _, stub, leg, _ in PLANS]
MAXW = max(a for a in arms) + 2*CLEAR + 30
# 每行高度 = 该方案的实际纵向占地（净空上沿 → 地平面下沿）+ 标题区
ROW_MM = max(leg + stub + 2*CLEAR + 4.5 for _, stub, leg, _ in PLANS)
ROW_PX = int(ROW_MM*S) + 56
IMG_W = int(MAXW*S) + 2*M
IMG_H = ROW_PX*len(PLANS) + 2*M

img = Image.new("RGB", (IMG_W, IMG_H), BG)
d = ImageDraw.Draw(img)
d.rectangle([0, 0, IMG_W, 44], fill=(100, 24, 24))
d.text((M, 11), "历史作废对比：三组尺寸均不得用于投板；当前值见 ifa_v4c.png / HFSS报告", font=FB,
       fill=(255, 225, 225))

def draw_plan(oy, title, stub, leg, note):
    arm = L - leg - stub
    R = R1 + (stub - S1)*SLOPE
    X0 = M + int((arm + CLEAR + 3)*S)
    def px(x,y): return (X0+int(x*S), oy+int(y*S))
    def rc(x1,y1,x2,y2,c):
        a,b = px(min(x1,x2),min(y1,y2)), px(max(x1,x2),max(y1,y2))
        d.rectangle([a,b], fill=c)
    def st(x1,y1,x2,y2,w,c):
        hw=w/2
        if abs(y1-y2)<1e-9: rc(x1-hw,y1-hw,x2+hw,y1+hw,c)
        elif abs(x1-x2)<1e-9: rc(x1-hw,min(y1,y2)-hw,x1+hw,max(y1,y2)+hw,c)
        else: d.line([px(x1,y1),px(x2,y2)], fill=c, width=max(2,int(w*S)))
    OPEN_X, SHORT_X, FEED_X = -arm, 0.0, -D
    LEG_END = leg
    GY = LEG_END + max(stub, 0.001) + (CLEAR if stub>0 else 0)
    # 净空
    rc(OPEN_X-CLEAR-W/2, -CLEAR-W/2, SHORT_X+CLEAR+W/2, LEG_END+CLEAR, KEEP)
    if stub > 0.05:
        rc(SHORT_X-CLEAR, LEG_END, SHORT_X+CLEAR, LEG_END+stub, KEEP)
    corr = RF_W/2+0.5
    rc(FEED_X-corr, LEG_END, FEED_X+corr, LEG_END+stub+2.0, KEEP)
    # 地平面
    gy0 = LEG_END + stub + (0 if stub<0.05 else 0)
    rc(OPEN_X-CLEAR-4, gy0, SHORT_X+CLEAR+7, gy0+3.4, GND)
    # 天线
    st(OPEN_X,0,SHORT_X,0,W,CU); st(FEED_X,0,FEED_X,LEG_END,W,CU); st(SHORT_X,0,SHORT_X,LEG_END,W,CU)
    for cx in (FEED_X,SHORT_X): rc(cx-PAD/2,LEG_END-PAD/2,cx+PAD/2,LEG_END+PAD/2,CU)
    if stub>0.05: st(SHORT_X,LEG_END,SHORT_X,LEG_END+stub,STUB_W,ACC)
    st(FEED_X,LEG_END,FEED_X,LEG_END+stub+2.0,RF_W,RED)
    # 标注
    d.text((M, oy-30), title, font=FB, fill=TXT)
    d.text((M+260, oy-27), note, font=FS, fill=(160,170,185))
    d.text(px(SHORT_X+3.0, -1.0),
           f"主臂 {arm:.2f}（包络 {arm+W:.2f}）  腿 {leg:.0f}  引线 {stub:.0f}", font=FS, fill=DIMC)
    d.text(px(SHORT_X+3.0, 1.4), f"预期 R ≈ {R:.0f}Ω → π 网络匹配到 50Ω", font=FS, fill=(200,200,210))
    kh = LEG_END+CLEAR+(stub if stub>0 else 0)+CLEAR/2
    aw, ah = arm+2*CLEAR+W, kh
    d.text(px(SHORT_X+3.0, 3.8), f"净空占地 {aw:.1f} × {ah:.1f} mm = {aw*ah:.0f} mm²", font=FS, fill=ACC)
    if stub > 8:
        d.text(px(SHORT_X+3.0, 6.2), "（引线图示为直线；折叠可省地，", font=FS, fill=(200,160,110))
        d.text(px(SHORT_X+3.0, 8.4), " 但相邻段电流反向会抵消电感）", font=FS, fill=(200,160,110))
    return arm

y = M + 30
for title, stub, leg, note in PLANS:
    draw_plan(y, title, stub, leg, note)
    y += ROW_PX

d.text((M, IMG_H-26), f"历史作废：旧总长 L={L:.2f}mm / K=66,012 MHz·mm 只保留追溯，"
                      f"不得当作当前50.2mm预研输入", font=FS, fill=(220,130,130))
out = sys.argv[1] if len(sys.argv)>1 else "build/ifa_v4_compare.png"
img.save(out); print("saved:", out, img.size)
