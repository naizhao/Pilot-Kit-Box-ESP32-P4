#!/usr/bin/env python3
"""从 Material Symbols 生成状态栏图标字形表（4bpp 灰度，固定 cell）。

为什么不再手绘
--------------
初版图标是用绘图原语手工拼的（圆、三角、折线）。形状勉强能认，但
"像不像"这件事上手绘几何拼不过专业图形设计——尤其蓝牙符号和定位标记，
21 px 高度下手绘折线挤成一团。

Material Symbols（Google，Apache-2.0）覆盖全、形状标准，且是四轴可变
字体（FILL / GRAD / opsz / wght），可按小尺寸优化：取 opsz=20 让字体
自己切换到小字号优化的字形轮廓，wght 调粗以在深色背景上立得住。

实测 pointsize=N 时图标墨迹为 (N+1)×(N+1)，故 pt=20 得 21 px，正好与
B612 Mono 在 S 档的大写字母墨迹齐平。

字重跟随文字设置
----------------
生成 regular / bold 两套（wght 500 / 700），使图标在用户切换"加粗"时
与文字一同变粗，避免文字变粗而图标仍纤细的割裂感。整套仅约 8 KB。
"""
from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_i18n_assets import pack_4bpp, render_glyph  # noqa: E402

# ── 图标集 ──
#
# FILL 轴按图标取舍，不能一刀切：
#   FILL=1（实心）—— 录制点、警告、卫星、蓝牙、飞机。小尺寸下实心比线框
#                    清晰，深色背景上也更立得住，半反半透屏尤其需要。
#   FILL=0（轮廓）—— 电池。填成实心就看不出电量了，必须保留外框 + 电量格。
#
# 电池改用 **battery_android** 系列。此前的 battery_horiz_* 只有 000/050/075
# 三档，20% 与 4% 都渲染成同一个空壳，光靠颜色区分；android 系列是完整的
# 0…6 七格连续刻度，形态本身就是读数。
#
# 充电态用 battery_android_frame_1…6 + _frame_full 做逐帧动画：静态闪电符号
# 只说明"接了电"，动画才说明"电真的在进"。相位由 uptime_ms 算，与帧率无关。
#
#   枚举名        Material 图标名          码位     FILL
ICONS = [
    ("SAT",       "satellite_alt",       0xEB3A, 1),
    ("REC",       "fiber_manual_record", 0xE061, 1),
    ("WARN",      "warning",             0xE002, 1),
    ("TEMP",      "thermometer_alert",   0xFFFFB, 1),
    ("BLE",       "bluetooth",           0xE1A7, 1),
    ("SD",        "sd_card",             0xE1C2, 1),
    # ADS-B 目标数的前缀。取 connecting_airports（画的是**两架**飞机）而不是
    # plane_contrails（单机 + 尾迹）：后面跟着的是目标计数，"周围有 N 架"要
    # 的是复数语义，尾迹说的是航迹，对不上。
    ("ADSB",      "connecting_airports", 0xE7C9, 1),
    # HSI 罗盘中心的本机符号。flight 本身就是机头朝上的俯视剪影，不必旋转
    # ——而罗盘恒 heading-up，本机符号也从不旋转，正好对上。
    ("OWNSHIP",   "flight",              0xE539, 1),
    # 电量刻度：alert → _0…_6 → full 共九档，必须**连号**（batt_icon_for()
    # 直接按 BATT_ALERT+step 取，没有特判分支）。
    #
    # 最低档取 alert（电池内嵌感叹号）而不是让 _0 兼职：_0 说的是"快空了"，
    # alert 说的是"该处理了"，是两句不同的话。
    #
    # 末档取 full 而不是 _6：FILL=0 下白色实心是已充电部分、黑色镂空是空的
    # 部分，_6 内部仍留着一条黑，用它表示 100% 会一直显示成"还差一格"。
    ("BATT_ALERT","battery_android_alert",0xF306, 0),
    ("BATT_0",    "battery_android_0",   0xF30D, 0),
    ("BATT_1",    "battery_android_1",   0xF30C, 0),
    ("BATT_2",    "battery_android_2",   0xF30B, 0),
    ("BATT_3",    "battery_android_3",   0xF30A, 0),
    ("BATT_4",    "battery_android_4",   0xF309, 0),
    ("BATT_5",    "battery_android_5",   0xF308, 0),
    ("BATT_6",    "battery_android_6",   0xF307, 0),
    ("BATT_FULL", "battery_android_full",0xF304, 0),
    # 充电动画帧，同样必须连号（CHG_1…CHG_6 → CHG_FULL 循环播放）。
    ("BATT_CHG_1",   "battery_android_frame_1",    0xF257, 0),
    ("BATT_CHG_2",   "battery_android_frame_2",    0xF256, 0),
    ("BATT_CHG_3",   "battery_android_frame_3",    0xF255, 0),
    ("BATT_CHG_4",   "battery_android_frame_4",    0xF254, 0),
    ("BATT_CHG_5",   "battery_android_frame_5",    0xF253, 0),
    ("BATT_CHG_6",   "battery_android_frame_6",    0xF252, 0),
    ("BATT_CHG_FULL","battery_android_frame_full", 0xF24F, 0),
]

# 图标不能与文字等高。图形内部留白远多于字母笔画（fiber_manual_record
# 就是个小圆点），墨迹取字高时视觉上明显偏小。实测取字高的 1.25~1.3 倍
# 才与同排文字平衡，故 pt=26 → 墨迹 27 px（S 档字高为 21 px）。
POINT_SIZE = 26
CELL_W = CELL_H = 30     # 容纳 27 px 墨迹并留边距

WEIGHTS = {"regular": 500, "bold": 700}


def instantiate(src: Path, wght: int, fill: int, out_dir: Path) -> Path:
    """把四轴可变字体固定到指定字重与填充，并取 opsz=20 的小字号优化轮廓。"""
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer

    out = out_dir / f"ms-w{wght}-f{fill}.ttf"
    if out.exists():
        return out
    font = TTFont(str(src))
    instancer.instantiateVariableFont(
        font, {"FILL": fill, "GRAD": 0, "opsz": 20, "wght": wght}, inplace=True)
    font.save(str(out))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--magick", default="magick")
    ap.add_argument("--font", type=Path, required=True,
                    help="MaterialSymbols 可变字体 TTF")
    ap.add_argument("--out-c", type=Path, required=True)
    ap.add_argument("--out-h", type=Path, required=True)
    args = ap.parse_args()

    blobs: dict[str, bytes] = {}
    with tempfile.TemporaryDirectory() as tmp:
        for weight, wght in WEIGHTS.items():
            blob = bytearray()
            for _, _, code, fill in ICONS:
                font = instantiate(args.font, wght, fill, Path(tmp))
                gray = render_glyph(args.magick, font, POINT_SIZE,
                                    CELL_W, CELL_H, code)
                if not any(gray):
                    raise RuntimeError(f"图标 U+{code:04X} 渲染为空，码位可能有误")
                blob += pack_4bpp(gray)
            blobs[weight] = bytes(blob)
            print(f"  {weight}: {len(blob)} bytes（{len(ICONS)} 个图标）")

    # ── 头文件 ──
    h = [
        "/* 由 firmware/scripts/gen_pfd_icons.py 生成，请勿手改。",
        " *",
        " * 图标取自 Material Symbols Rounded（Google，Apache-2.0）。",
        " * 4bpp 灰度，固定 cell，与文字共用同一套 alpha 混合渲染。",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define PK_ICON_W  {CELL_W}",
        f"#define PK_ICON_H  {CELL_H}",
        "",
        "typedef enum {",
    ]
    for i, (name, mat, code, fill) in enumerate(ICONS):
        h.append(f"    PK_ICON_{name} = {i},   /* {mat}  U+{code:04X}  FILL={fill} */")
    h += [
        f"    PK_ICON_COUNT = {len(ICONS)}",
        "} pk_icon_id_t;",
        "",
        "/* [0] = regular, [1] = bold —— 与文字字重联动。 */",
        "extern const uint8_t *const pk_icon_bitmap[2];",
        "",
    ]
    args.out_h.write_text("\n".join(h), encoding="utf-8")

    # ── 源文件 ──
    c = [
        "/* 由 firmware/scripts/gen_pfd_icons.py 生成，请勿手改。 */",
        '#include "pfd_icon_font.h"',
        "",
    ]
    for weight, blob in blobs.items():
        c.append(f"static const uint8_t s_icons_{weight}[] = {{")
        for i in range(0, len(blob), 16):
            c.append("    " + ", ".join(f"0x{b:02X}" for b in blob[i:i + 16]) + ",")
        c += ["};", ""]
    c += [
        "const uint8_t *const pk_icon_bitmap[2] = {",
        "    s_icons_regular,",
        "    s_icons_bold,",
        "};",
        "",
    ]
    args.out_c.write_text("\n".join(c), encoding="utf-8")

    total = sum(len(b) for b in blobs.values())
    print(f"共 {total} bytes → {args.out_c.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
