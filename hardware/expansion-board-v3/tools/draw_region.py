#!/usr/bin/env python3
"""把 shot_region.py 导出的 JSON 几何画成 PNG（系统 python3 + PIL）。

为什么单独一个脚本：pcbnew 只在 KiCad 自带的 python 里有，而那个 python 没装 PIL；
系统 python3 有 PIL 但没有 pcbnew。所以拆成两步，中间用 JSON 传几何。
（magick 也画不了——它的 svg 代理 rsvg-convert 没装，内置渲染器不画 <line>。）

用法：python3 draw_region.py <region-xxx.json> [输出.png]
"""
import json
import math
import sys

from PIL import Image, ImageDraw, ImageFont

SRC = sys.argv[1]
DST = sys.argv[2] if len(sys.argv) > 2 else SRC.replace(".json", ".png")
PX = 1600                                   # 输出边长
g = json.load(open(SRC))
CX, CY, RAD = g["cx"], g["cy"], g["rad"]
S = PX / (2 * RAD)                          # mm → px
BG, FG = (16, 24, 32), (230, 230, 230)
COLOR = {"F.Cu": (208, 60, 60), "In2.Cu": (224, 140, 16), "B.Cu": (70, 110, 220),
         "In1.Cu": (128, 128, 128), "In3.Cu": (64, 160, 64), "In4.Cu": (160, 64, 160)}


def T(x, y):
    return ((x - (CX - RAD)) * S, (y - (CY - RAD)) * S)


img = Image.new("RGB", (PX, PX), BG)
d = ImageDraw.Draw(img, "RGBA")

for x, y, w, h, drl, ref, net in g["pads"]:            # 焊盘
    a, b = T(x - w / 2, y - h / 2)
    c, e = T(x + w / 2, y + h / 2)
    d.rounded_rectangle([a, b, c, e], radius=max(1, int(0.05 * S)),
                        fill=(200, 200, 200, 235) if not drl else (232, 192, 32, 235))

for x1, y1, x2, y2, w, ly, net in g["tracks"]:         # 走线
    d.line([T(x1, y1), T(x2, y2)], fill=COLOR.get(ly, (136, 136, 136)) + (220,),
           width=max(1, int(w * S)), joint="curve")

for x, y, dia, drl, net in g["vias"]:                  # 过孔
    a, b = T(x - dia / 2, y - dia / 2)
    c, e = T(x + dia / 2, y + dia / 2)
    d.ellipse([a, b, c, e], fill=(150, 150, 150))
    a, b = T(x - drl / 2, y - drl / 2)
    c, e = T(x + drl / 2, y + drl / 2)
    d.ellipse([a, b, c, e], fill=BG)

if g.get("gap"):                                        # 缺口：绿虚线 + 两端圈
    (x1, y1), (x2, y2) = g["gap"]
    p1, p2 = T(x1, y1), T(x2, y2)
    L = math.hypot(p2[0] - p1[0], p2[1] - p1[1])
    n = max(1, int(L / 18))
    for i in range(n):
        if i % 2:
            continue
        t0, t1 = i / n, min(1.0, (i + 1) / n)
        d.line([(p1[0] + (p2[0] - p1[0]) * t0, p1[1] + (p2[1] - p1[1]) * t0),
                (p1[0] + (p2[0] - p1[0]) * t1, p1[1] + (p2[1] - p1[1]) * t1)],
               fill=(32, 255, 32), width=6)
    for px, py in (p1, p2):
        r = 0.42 * S
        d.ellipse([px - r, py - r, px + r, py + r], outline=(32, 255, 32), width=6)

try:                                                    # 标注：网络名 + 缺口长度
    fnt = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 40)
    sml = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 26)
except Exception:
    fnt = sml = ImageFont.load_default()
if g.get("net"):
    (x1, y1), (x2, y2) = g["gap"]
    d.text((22, 18), f"{g['net']}   gap {math.hypot(x2-x1, y2-y1):.2f}mm",
           fill=(32, 255, 32), font=fnt)
d.text((22, PX - 42), f"center({CX:.2f},{CY:.2f})  {2*RAD:.0f}x{2*RAD:.0f}mm   "
       f"F.Cu=red In2=orange B.Cu=blue", fill=(150, 160, 170), font=sml)
# 焊盘位号（只标缺口那条网络的，免得糊成一团）
for x, y, w, h, drl, ref, net in g["pads"]:
    if g.get("net") and net == g["net"]:
        px, py = T(x, y)
        d.text((px + 6, py - 34), ref, fill=(255, 255, 255), font=sml)
img.save(DST)
print(f"{DST}  {PX}x{PX}px  ({S:.0f} px/mm)")
