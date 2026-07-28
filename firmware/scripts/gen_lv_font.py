#!/usr/bin/env python3
"""把 Noto Sans SC 按 i18n catalog 子集化，输出可嵌入固件的 TTF（C 数组）。

为什么是子集 TTF 而不是预渲染位图
--------------------------------
LVGL 的常规做法是用 lv_font_conv 把字体预渲染成固定字号的位图表。对拉丁文
那很划算，但中文不行：spec §2 的字号阶梯有 5 档，而每档都得存一份完整的
CJK 字形。实测 152 字（含 91 个汉字）在 26 px 一档就要约 45 KB，5 档就是
200 KB 以上——app 分区当前只剩 471 KB，放不下。

改用 LVGL 的 TinyTTF（stb_truetype）在运行时渲染，字体只需存一份轮廓：
同样 152 字子集化后仅 **约 30 KB**，且与字号无关，将来加档位不增体积。
代价是每个字形首次渲染的 CPU 开销，由 LVGL 的字形缓存摊掉。

另一个考量是工具链一致性：本项目已有 Python + fontTools + ImageMagick 那套
（见 gen_pfd_aa_font.py / gen_pfd_icons.py），lv_font_conv 是 npm 包，引入它
就多一条 Node 依赖链。fontTools.subset 落在现有工具链内。

字符集的唯一来源是 i18n_catalog
-------------------------------
和 gen_i18n_assets.py 一样，字符集从 catalog 推导，不在这里另立一份清单——
两份清单必然走偏，且漏字的表现是屏幕上显示豆腐块，不容易第一时间发现。
**新增中文文案的正确做法是先改 i18n_catalog.py，再重跑本脚本。**
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from i18n_catalog import STRINGS  # noqa: E402

# 目录之外还必然要用到的字符：数字、单位、标点。它们不出现在 catalog 里，
# 因为那些串是运行时 snprintf 拼出来的。
EXTRA = "0123456789.:%+-/°()、，。·"


def collect_chars() -> str:
    chars = set(EXTRA)
    for _, per_lang in STRINGS:
        for text in per_lang.values():
            chars |= set(text)
    return "".join(sorted(chars))


def subset_font(src: Path, wght: int, text: str, out: Path) -> int:
    from fontTools import subset
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer

    font = TTFont(str(src))
    # 先把可变字重固定下来：TinyTTF 不认 fvar，喂可变字体只会拿到默认实例。
    instancer.instantiateVariableFont(font, {"wght": wght}, inplace=True)

    opt = subset.Options()
    # 中文不需要连字/字距调整，砍掉这些表能省下可观体积。
    opt.layout_features = []
    opt.hinting = False
    opt.desubroutinize = True
    opt.drop_tables += ["GSUB", "GPOS", "GDEF", "morx", "kern"]

    s = subset.Subsetter(options=opt)
    s.populate(text=text)
    s.subset(font)
    font.save(str(out))
    return out.stat().st_size


def emit_c(path: Path, blob: bytes, symbol: str) -> None:
    lines = [
        "/* 由 firmware/scripts/gen_lv_font.py 生成，请勿手改。",
        " *",
        " * Noto Sans SC（SIL OFL）按 i18n catalog 子集化后的 TTF。",
        " * 供 LVGL 的 TinyTTF 在运行时渲染，一份轮廓服务所有字号。",
        " */",
        '#include "lv_font_zh.h"',
        "",
        f"const uint8_t {symbol}[] = {{",
    ]
    for i in range(0, len(blob), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in blob[i:i + 16]) + ",")
    lines += ["};", "", f"const unsigned {symbol}_size = sizeof({symbol});", ""]
    path.write_text("\n".join(lines), encoding="utf-8")


def emit_h(path: Path, symbol: str, n_chars: int, n_bytes: int) -> None:
    path.write_text("\n".join([
        "/* 由 firmware/scripts/gen_lv_font.py 生成，请勿手改。 */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"/* {n_chars} 个字符的子集，{n_bytes} 字节。字符集由 i18n_catalog.py 决定，",
        " * 新增中文文案后必须重跑生成脚本，否则屏幕上会出现豆腐块。 */",
        f"extern const uint8_t {symbol}[];",
        f"extern const unsigned {symbol}_size;",
        "",
    ]), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cjk", type=Path, required=True, help="Noto Sans SC 可变字重 TTF")
    ap.add_argument("--wght", type=int, default=400)
    ap.add_argument("--out-c", type=Path, required=True)
    ap.add_argument("--out-h", type=Path, required=True)
    args = ap.parse_args()

    text = collect_chars()
    n_cjk = sum(1 for c in text if ord(c) > 0x2E80)
    print(f"字符集 {len(text)} 个（CJK {n_cjk}），来源：i18n_catalog.STRINGS")

    tmp = args.out_c.with_suffix(".ttf.tmp")
    size = subset_font(args.cjk, args.wght, text, tmp)
    blob = tmp.read_bytes()
    tmp.unlink()
    print(f"子集 TTF {size / 1024:.1f} KB（wght={args.wght}）")

    symbol = "pk_lv_font_zh_ttf"
    emit_c(args.out_c, blob, symbol)
    emit_h(args.out_h, symbol, len(text), size)
    print(f"→ {args.out_c.name} / {args.out_h.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
