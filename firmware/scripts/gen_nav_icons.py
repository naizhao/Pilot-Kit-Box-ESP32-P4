#!/usr/bin/env python3
"""为「全屏导航网格」页生成 64 px 图标表（4bpp 灰度，固定 cell）。

原型评审于 sim/proto-navgrid/gen_nav_icons.py，评审通过后转正到这里，
产出 firmware/main/nav_icon_font.{c,h}，供 nav_grid_page.c 的图标网格
使用。

为什么不直接改 gen_pfd_icons.py
--------------------------------
那份脚本产出的 pfd_icon_font.c 是**状态栏**图标（30 px cell，opsz=20 的
小字号优化轮廓），全仓在用。导航网格要的是 64 px 大图标，两者的 cell 尺
寸、opsz 轴取值都不同，塞进同一张表会逼所有调用方一起改。

单独出一份图标表，与状态栏那份各自独立演进——是否合表是以后的事，不该
由这次改动绑架现役代码。码位一律从字体 cmap 反查（见 CODEPOINTS 注释），
不手抄。

字重与光学尺寸
--------------
opsz=40 而不是 gen_pfd_icons.py 的 20：Material Symbols 的 opsz 轴就是
为此设的，小字号轮廓在 64 px 下笔画交接处会显得笨重。
wght=400 而不是 500：500 是为 21 px 墨迹在深色背景上不发虚而调粗的，
64 px 下不存在这个问题，反而显胖。
"""
from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_i18n_assets import pack_4bpp, render_glyph  # noqa: E402

# 枚举名 / Material 图标名 / 码位 / FILL
#
# 码位由 fontTools 从 MaterialSymbolsRounded.ttf 的 cmap 反查得到，不是抄的
# 网页。校验点：flight = U+E539，与 gen_pfd_icons.py 里既有的那条一致。
#
# FILL 全取 0（轮廓）：64 px 下实心图标的墨迹面积太大，一屏 12 个铺开像一
# 堵墙；轮廓在大尺寸下反而更清楚。这与状态栏那批取 FILL=1 的理由（21 px
# 实心才立得住）正好相反，尺寸不同结论不同。
ICONS = [
    ("PFD",    "flight",               0xE539, 0),
    ("TRF",    "radar",                0xF04E, 0),
    ("MAP",    "map",                  0xE55B, 0),
    ("LIST",   "format_list_bulleted", 0xE241, 0),
    ("SEARCH", "search",               0xE8B6, 0),
    ("REC",    "history",              0xE28E, 0),
    ("TOOL",   "handyman",             0xF10B, 0),
    ("DIAG",   "monitor_heart",        0xEAA2, 0),
    ("SET",    "settings",             0xE8B8, 0),
    ("ABOUT",  "info",                 0xE88E, 0),
    ("LEVEL",  "straighten",           0xE41C, 0),
]

# 实测 pointsize=N 时墨迹为 (N+1)×(N+1)（见 gen_pfd_icons.py 文件头）。
POINT_SIZE = 63          # → 64 px 墨迹
CELL_W = CELL_H = 68     # 容纳 64 px 墨迹并留 2 px 边距
ICON_WGHT = 400
ICON_OPSZ = 40


def instantiate(src: Path, wght: int, fill: int, out_dir: Path) -> Path:
    """把四轴可变字体固定到指定字重与填充，取 opsz=40 的大字号优化轮廓。"""
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer

    out = out_dir / f"ms-nav-w{wght}-f{fill}.ttf"
    if out.exists():
        return out
    font = TTFont(str(src))
    instancer.instantiateVariableFont(
        font, {"FILL": fill, "GRAD": 0, "opsz": ICON_OPSZ, "wght": wght},
        inplace=True)
    font.save(str(out))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--magick", default="magick")
    ap.add_argument("--font", type=Path, required=True)
    ap.add_argument("--out-c", type=Path, required=True)
    ap.add_argument("--out-h", type=Path, required=True)
    args = ap.parse_args()

    blob = bytearray()
    with tempfile.TemporaryDirectory() as tmp:
        for _, mat, code, fill in ICONS:
            font = instantiate(args.font, ICON_WGHT, fill, Path(tmp))
            gray = render_glyph(args.magick, font, POINT_SIZE,
                                CELL_W, CELL_H, code)
            # 空图 = 码位错了。静默生成一片空白比报错难查得多。
            if not any(gray):
                raise RuntimeError(f"{mat} U+{code:04X} 渲染为空，码位有误")
            blob += pack_4bpp(gray)
    icons = bytes(blob)

    h = [
        "/* 由 firmware/scripts/gen_nav_icons.py 生成，请勿手改。 */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define PK_NAVICON_W  {CELL_W}",
        f"#define PK_NAVICON_H  {CELL_H}",
        "",
        "typedef enum {",
    ]
    for i, (name, mat, code, fill) in enumerate(ICONS):
        h.append(f"    PK_NAVICON_{name} = {i},   /* {mat}  U+{code:04X} */")
    h += [
        f"    PK_NAVICON_COUNT = {len(ICONS)}",
        "} pk_navicon_id_t;",
        "",
        "extern const uint8_t pk_navicon_bitmap[];",
        "",
    ]
    args.out_h.write_text("\n".join(h), encoding="utf-8")

    c = [
        "/* 由 firmware/scripts/gen_nav_icons.py 生成，请勿手改。 */",
        '#include "nav_icon_font.h"',
        "",
        "const uint8_t pk_navicon_bitmap[] = {",
    ]
    for i in range(0, len(icons), 16):
        c.append("    " + ", ".join(f"0x{b:02X}" for b in icons[i:i + 16]) + ",")
    c += ["};", ""]
    args.out_c.write_text("\n".join(c), encoding="utf-8")

    print(f"{len(ICONS)} 个图标，{len(icons)} bytes → {args.out_c.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
