#!/usr/bin/env python3
"""给自制封装生成 3D 外形包络（VRML），挂到 .kicad_mod 上。

## 为什么要

2026-08-13 评审："还缺少很多元件的 3D 图……比如 U5、U6，因为 U5 缺少 3D，
我不知道是否会碰撞或者无法吹上去。"

板上 20 个元件没有 3D 模型，但**真正需要建模的只有 5 个**：
    TestPoint ×7 / MountingHole ×4 / SolderJumper ×2 / ANT_IFA ×1
        → 纯焊盘或开孔，本来就没有实体，不需要
    BMP388 / BNO085 / ATGM336H / AD8319 / TA0970A ×2
        → 自制封装，KiCad 库里没有，必须自己建
（U6 其实已经有模型——它是标准 LGA-16，KiCad 库自带。U5↔U6 那 0.06mm 间距，
  缺的是 U5 这一半。）

## 为什么是长方体包络，不是精细外观

这批模型的用途是**判断机械干涉和热风枪可达性**，不是渲染好看：
  · 与 4.3 主板对扣后只有 7mm 净空（3.5+3.5），高度错了整个判断就废
  · 长方体是**保守**的——真实器件只会比包络小，包络不撞就一定不撞

所以高度数据一律取 datasheet 的 **max**，不取 nominal。

## 单位坑

KiCad 的 .wrl 模型单位是 **0.1 inch = 2.54mm**，不是 mm。
尺寸不除以 2.54 的话，模型会大 2.54 倍——U7 那个 10mm 的模块会变成 25mm，
直接盖住半块板，而且看起来"像那么回事"，很容易当成布局问题去查。

用法：gen_3d.py            生成 .wrl 并写进对应的 .kicad_mod
"""
import os
import re
import sys

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRETTY = os.path.join(T, "kicad", "expansion-board-v3.pretty")
SHAPES = os.path.join(T, "kicad", "expansion-board-v3.3dshapes")
os.makedirs(SHAPES, exist_ok=True)

VRML_UNIT = 2.54       # KiCad .wrl 的 1 单位 = 0.1 inch = 2.54mm

# (封装名, 本体L, 本体W, 高度max, 颜色, 数据来源)
# 高度一律取 datasheet 的 **max**：包络宁可高估，不能低估。
PARTS = [
    ("BMP388_LGA-10", 2.00, 2.00, 0.80, (0.25, 0.25, 0.28),
     "Bosch BST-BMP388-DS001: 2.0x2.0mm metal-lid LGA, 高度 max 0.80 (nom 0.75)"),
    ("BNO085_LGA-28", 3.80, 5.20, 1.10, (0.20, 0.20, 0.22),
     "CEVA BNO08X Datasheet Rev1.17: 28-pin LGA 3.8x5.2x1.1mm"),
    ("ATGM336H_LCC-18", 10.10, 9.70, 2.40, (0.15, 0.30, 0.15),
     "中科微 ATGM336H-5N 用户手册: 模块 9.7x10.1x2.4mm（含屏蔽罩）"),
    ("AD8319_LFCSP-8-EP", 3.00, 2.00, 0.80, (0.22, 0.22, 0.24),
     "ADI CP-8-23 outline: LFCSP 3x2mm, 高度 0.80/0.75/0.70 (max/nom/min)"),
    ("TA0970A_SMD3838-6", 3.80, 3.80, 1.30, (0.80, 0.78, 0.72),
     "TST TA0970A Rev3.0: 本体 3.8mm SQ(+0.20/-0.10)；outline 未标高度，"
     "取编带 Section A-A 口袋深度 1.3mm 作为上限"),
]

WRL = """#VRML V2.0 utf8
# {name} —— 外形包络（长方体），用于机械干涉与返修可达性判断，不是精细外观。
# {src}
# ⚠️ KiCad .wrl 单位 = 0.1 inch = 2.54mm，下面的数值已经除过 2.54。
#    实际尺寸 {L:.2f} x {W:.2f} x {H:.2f} mm
Transform {{
  translation 0 0 {z:.6f}
  children [
    Shape {{
      appearance Appearance {{
        material Material {{
          diffuseColor {r:.2f} {g:.2f} {b:.2f}
          specularColor 0.12 0.12 0.12
          shininess 0.25
        }}
      }}
      geometry Box {{ size {l:.6f} {w:.6f} {h:.6f} }}
    }}
  ]
}}
"""

MODEL = '''\t(model "${{KIPRJMOD}}/expansion-board-v3.3dshapes/{name}.wrl"
\t\t(offset (xyz 0 0 0))
\t\t(scale (xyz 1 1 1))
\t\t(rotate (xyz 0 0 0))
\t)
'''

n_wrl = n_mod = 0
for name, L, W, H, (r, g, b), src in PARTS:
    with open(os.path.join(SHAPES, f"{name}.wrl"), "w") as f:
        f.write(WRL.format(name=name, src=src, L=L, W=W, H=H,
                           l=L / VRML_UNIT, w=W / VRML_UNIT, h=H / VRML_UNIT,
                           z=H / 2 / VRML_UNIT, r=r, g=g, b=b))
    n_wrl += 1

    mod = os.path.join(PRETTY, f"{name}.kicad_mod")
    if not os.path.exists(mod):
        print(f"  ⚠️ 找不到 {name}.kicad_mod，跳过挂载")
        continue
    s = open(mod).read()
    if "(model " in s:
        print(f"  {name}: 已有 model，跳过")
        continue
    # 挂在末尾那个收尾括号之前
    i = s.rstrip().rfind(")")
    s = s[:i] + MODEL.format(name=name) + s[i:]
    open(mod, "w").write(s)
    n_mod += 1
    print(f"  {name:22s} {L:5.2f} x {W:5.2f} x {H:4.2f}mm   {src[:46]}")

print(f"\n生成 {n_wrl} 个 .wrl → {SHAPES}")
print(f"挂到 {n_mod} 个 .kicad_mod（gen_pcb 重新生成板子时会带上）")
