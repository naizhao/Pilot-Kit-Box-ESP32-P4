#!/usr/bin/env python3
"""从 TTF 生成 PFD 用的抗锯齿 ASCII 字形表（4bpp 灰度，固定 cell）。

和 gen_pfd_cockpit_font.py 的分工
--------------------------------
cockpit 那套是**手绘像素**的 12×16 单档字形，专为 320×240 小屏调过，
在原始尺寸下最锐利。但它只有一档，整数倍放大后就是方块像素——这在
800×480 / 217 PPI 上完全不能看（实测 scale-3 放大后锯齿明显）。

本脚本走 TTF 派生路线，为每个目标字号单独渲染，因此任意字号都有正确
的抗锯齿边缘。两套并存：小屏继续用 cockpit，大屏用这里生成的。

字体选型
--------
拉丁/数字   B612 Mono —— Airbus + ENAC 为航空座舱开发，SIL OFL 授权。
            等宽是关键：高度带 23700→23800 跳变时字符不会左右抖动。
中日文      Noto Sans SC —— 沿用项目现有字体，通过 fallback 链接入。

字重
----
用户可在设置中切换"正常 / 加粗"，故每档字号生成两套：
    正常 = B612 Mono Regular + Noto Sans SC wght 500
    加粗 = B612 Mono Bold    + Noto Sans SC wght 600
Noto 是可变字重字体，用 fontTools 实例化到固定字重后再交给 magick。

CJK 字号补偿
------------
实测 pointsize 21 时，B612 Mono 的「A」墨迹高 17 px，而 Noto Sans SC
的「调」高 19 px —— 汉字字面更大。若同 pointsize 混排，汉字会显得比
拉丁大一圈，故 CJK 侧乘 CJK_PT_RATIO 补偿。
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_i18n_assets import pack_4bpp, render_glyph_with_fallback  # noqa: E402

# ── 字符集：可打印 ASCII + 度数符号（沿用 pfd_font.h 的 0x7F 约定）──
FIRST_CODE = 0x20
LAST_CODE = 0x7F
DEGREE_SOURCE = 0xB0          # 渲染时用真正的 '°'，存储位置在 0x7F

# ── CJK 相对拉丁的 pointsize 补偿（见文件头说明）──
CJK_PT_RATIO = 0.92

# ── 字号档位 ──
#
# 规格 §2 的阶梯给的是**字高**（大写字母墨迹高度），不是 pointsize。
# 实测 B612 Mono 的关系为 cap高 ≈ pt × 0.81，advance ≈ pt × 0.65：
#
#     pt   cap高   advance
#     26    21      17.1     ← S 级 2.5 mm
#     35    28      23.1     ← M 级 3.3 mm
#     56    43      36.1     ← XL 级 5.0 mm
#
# cell 需容纳升部与降部，取 pt × 1.15 向上圆整。
# 字符集按档裁剪：XL 只服务 PFD 当前值（高度 / 速度），显示的是纯数字，
# 存整套字母纯属浪费——app 分区只剩 5% 余量，这 150 KB 省得值。
# 取 0x20..0x3F 一段（空格、+ - . / 数字 : 等），索引仍连续。
#
# XS 同样只存 0x20..0x3F：它服务的是交通目标的相对高度标签（"+92" 这类），
# 纯数字带正负号，存整套字母同样是浪费。裁剪后双字重仅 12 KB。
#   pt      cap高   cell（宽×高）    末位码    用途
SIZES = {
    "xs": dict(pt=22, cell=(15, 26), last=0x3F),  # 18 px  硬下限，仅极次要信息
    "s":  dict(pt=26, cell=(18, 30), last=0x7F),  # 21 px  状态栏 / 标签 / 单位
    "m":  dict(pt=35, cell=(24, 40), last=0x7F),  # 28 px  正文主力
    "xl": dict(pt=56, cell=(37, 64), last=0x3F),  # 43 px  PFD 当前值（仅数字与符号）
}

WEIGHTS = ("regular", "bold")


def instantiate_cjk(src: Path, wght: int, out_dir: Path) -> Path:
    """把可变字重的 Noto Sans SC 实例化成固定字重，供 magick 使用。"""
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer

    out = out_dir / f"noto-{wght}.ttf"
    font = TTFont(str(src))
    instancer.instantiateVariableFont(font, {"wght": wght}, inplace=True)
    font.save(str(out))
    return out


def render_face(magick: str, fonts: list[Path], pt: int,
                cell: tuple[int, int], last: int) -> bytes:
    """渲染 ASCII 子集 [FIRST_CODE, last]，返回拼接好的 4bpp 数据。"""
    w, h = cell
    blob = bytearray()
    for code in range(FIRST_CODE, last + 1):
        src_code = DEGREE_SOURCE if code == 0x7F else code
        gray = render_glyph_with_fallback(magick, fonts, pt, w, h, src_code)
        blob += pack_4bpp(gray)
    return bytes(blob)


def emit_c(path: Path, faces: dict[tuple[str, str], bytes]) -> None:
    lines = [
        "/* 由 firmware/scripts/gen_pfd_aa_font.py 生成，请勿手改。",
        " *",
        " * 拉丁 B612 Mono（Airbus/ENAC，SIL OFL）+ 中日文 Noto Sans SC。",
        " * 4bpp 灰度，固定 cell，每档字号独立渲染以保证抗锯齿质量。",
        " */",
        '#include "pfd_aa_font.h"',
        "",
    ]
    for (size, weight), blob in faces.items():
        w, h = SIZES[size]["cell"]
        n = SIZES[size]["last"] - FIRST_CODE + 1
        lines.append(f"/* {size}/{weight}: cell {w}x{h}, "
                     f"{n} glyphs (0x{FIRST_CODE:02X}..0x{SIZES[size]['last']:02X}), "
                     f"{len(blob)} bytes */")
        lines.append(f"const uint8_t pk_aa_{size}_{weight}[] = {{")
        for i in range(0, len(blob), 16):
            chunk = ", ".join(f"0x{b:02X}" for b in blob[i:i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def emit_h(path: Path) -> None:
    lines = [
        "/* 由 firmware/scripts/gen_pfd_aa_font.py 生成，请勿手改。 */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define PK_AA_FIRST_CODE  0x{FIRST_CODE:02X}",
        "",
    ]
    for size, spec in SIZES.items():
        w, h = spec["cell"]
        up = size.upper()
        lines += [
            f"#define PK_AA_{up}_W     {w}",
            f"#define PK_AA_{up}_H     {h}",
            f"#define PK_AA_{up}_LAST  0x{spec['last']:02X}",
        ]
    lines.append("")
    for size in SIZES:
        for weight in WEIGHTS:
            lines.append(f"extern const uint8_t pk_aa_{size}_{weight}[];")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--magick", default="magick")
    ap.add_argument("--latin-regular", type=Path, required=True)
    ap.add_argument("--latin-bold", type=Path, required=True)
    ap.add_argument("--cjk", type=Path, required=True,
                    help="Noto Sans SC 可变字重 TTF")
    ap.add_argument("--out-c", type=Path, required=True)
    ap.add_argument("--out-h", type=Path, required=True)
    ap.add_argument("--sizes", default="xs,s,m,xl")
    args = ap.parse_args()

    want = [s.strip() for s in args.sizes.split(",") if s.strip()]
    faces: dict[tuple[str, str], bytes] = {}

    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        cjk_by_weight = {
            "regular": instantiate_cjk(args.cjk, 500, tmp_dir),
            "bold": instantiate_cjk(args.cjk, 600, tmp_dir),
        }
        latin_by_weight = {"regular": args.latin_regular, "bold": args.latin_bold}

        for size in want:
            spec = SIZES[size]
            for weight in WEIGHTS:
                chain = [latin_by_weight[weight], cjk_by_weight[weight]]
                blob = render_face(args.magick, chain, spec["pt"],
                                   spec["cell"], spec["last"])
                faces[(size, weight)] = blob
                print(f"  {size}/{weight}: {len(blob)} bytes "
                      f"(cell {spec['cell'][0]}x{spec['cell'][1]}, pt {spec['pt']})")

    emit_c(args.out_c, faces)
    emit_h(args.out_h)
    total = sum(len(b) for b in faces.values())
    print(f"共 {len(faces)} 套字形，{total} bytes → {args.out_c.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
