#!/usr/bin/env python3
"""从 TTF 生成 PFD 用的抗锯齿 ASCII 字形表（4bpp 灰度，固定 cell）。

唯一的 PFD 正文字体来源
----------------------
2026-07-30 之前还有一套 cockpit 字体（gen_pfd_cockpit_font.py → pfd_font_aa.c，
手绘像素的 12×16 单档字形），专为 320×240 小屏调过。那套连同生成脚本、测试，
以及 pfd_tape / pfd_speed_tape / pfd_hsi 里各自的 320×240 `#else` 分支一并删除：
硬件已全面换到 4.3″ 800×480，不再保留小屏兼容预览，那些分支在真机上一行都不会
执行，却要求每改一次版面同时维护两套版面常量、喂两套字库。

留下本脚本这一条路，是因为 cockpit 只有一档，整数倍放大后就是方块像素——在
800×480 / 217 PPI 上完全不能看（实测 scale-3 放大后锯齿明显）。本脚本走 TTF
派生路线，为每个目标字号单独渲染，任意字号都有正确的抗锯齿边缘。
（1~2 个字符的极小标注仍走 pfd_font.c 的 5×7 位图，见该文件头。）

字体选型
--------
拉丁/数字   B612 Mono —— Airbus + ENAC 为航空座舱开发，SIL OFL 授权。
            等宽是关键：高度带 23700→23800 跳变时字符不会左右抖动。
中日文      Noto Sans SC —— 沿用项目现有字体，通过 fallback 链接入。

字重
----
只有一档：B612 Mono Regular + Noto Sans SC wght 500（fontTools 把可变字重的
Noto 实例化到 500 后再交给 magick）。

2026-07-30 去掉了 bold 一档。它是给半反半透屏的反射态预留的，但那块屏最终
没上，设置页里从来没做出对应的开关，pk_aa_set_weight() 全仓零调用者——字重
恒为 regular。可 bold 那 9 张表被 pfd_aa_text.c 的静态字体表静态引用着，
链接期照单全收，实测占 app 分区约 347 KB（当时余量只剩 10%）。要恢复的话
是重新加一档，而不是留着半套死数据。

CJK 与拉丁的对齐口径
--------------------
结论：**同 pointsize，cell 宽取整一个 em**，对 CJK 不做任何缩小补偿。

这里原先有个 CJK_PT_RATIO=0.92，理由是「pt 21 时 B612 的『A』墨迹高
17 px，而 Noto 的『调』高 19 px，汉字字面更大，同 pointsize 混排会显得
大一圈」。观察没错，推论反了：拉丁的 cap height 天生只有 ~0.78 em，汉字
字面天生是 ~0.90 em，两者本就不该等高——这正是两款字体被设计成能按同一
尺寸混排的方式。把汉字压到与 cap height 齐平，就等于让中文整体小一档。
这与 backbar 那次踩的是同一个坑：**标称 px 在不同字体之间不可比**，必须
量墨迹。

实测（magick 同一条渲染路径，大画布量墨迹包围盒）：

    档  拉丁pt  cap高  旧CJKpt 旧汉字高  新CJKpt 新汉字高  新汉字高/cap高
    xs   15      13      14       12        15       13        1.00
    s    17      14      16       14        17       15        1.07
    m    22      17      20       18        22       20        1.18
    l    32      25      29       26        32       29        1.16

（xs/s 的比值偏低是 B612 在小 ppem 上被 hinting 撑高了 cap——pt15 的
cap/pt 实测 0.867，到 pt32 才回落到 0.781。同 em 混排仍是正确口径。）

cell 宽必须**正好一个 em**（即 pointsize）。Noto Sans SC 的汉字与箭头字面
是按满 em 画的：254 字在 pt=20/21/22 下实测最大墨迹宽恒等于 pt。旧口径取
round(拉丁pt × 0.81)，比 em 窄 10% 以上，结果 254 字里有 190~230 个被 cell
左右各切掉 1~2 px —— 边框不闭合、笔画缺角，于是又"薄"又小。
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

# ── CJK 字符集 ──
#
# 从 i18n catalog 收集，与屏上真正出现的文案严格同源：多存一个字就是白占
# flash，少存一个字就是屏上一个豆腐块。加新文案的流程因此是「先改 catalog，
# 再重跑本脚本」——见 gen_i18n_assets.py 的同类说明。
#
# 四档都生成非 ASCII 字形（XL 除外，它只服务 PFD 的纯数字大值）。
#
# 汉字在 12/14 档上确实不该用，但这张表里还有**方向箭头** ↑↓←→ 等——它们是
# 符号不是文字，小尺寸下照样可读，而雷达标签、列表次要行恰恰用的就是小档。
# 只给 M/L 生成的话，XS 档上的箭头会整个消失（查表落空 → 只推进宽度不画），
# 屏上表现为「+26」后面凭空少了升降指示。
#
# 代价是小档多存 99 个字形约 39 KB，换掉「小字号上没有符号可用」值得。
CJK_SIZES = ("xs", "s", "m", "l")


# 八向箭头。不是文案，但和汉字一样走「非 ASCII 二分查表」那条路径，所以并进
# 同一张码位表。用它们表示相对方位——这是既有符号，飞行员一眼就懂；拿 ASCII
# 的 "/" "^" "<" 去凑只会让人猜。
ARROW_CODES = [
    0x2191,  # ↑ 正前
    0x2197,  # ↗ 右前
    0x2192,  # → 正右
    0x2198,  # ↘ 右后
    0x2193,  # ↓ 正后
    0x2199,  # ↙ 左后
    0x2190,  # ← 正左
    0x2196,  # ↖ 左前
]


def collect_cjk_codes() -> list[int]:
    import i18n_catalog
    codes: set[int] = set(ARROW_CODES)

    def walk(o) -> None:
        if isinstance(o, str):
            codes.update(ord(ch) for ch in o if ord(ch) > 0x7F)
        elif isinstance(o, dict):
            for v in o.values():
                walk(v)
        elif isinstance(o, (list, tuple)):
            for v in o:
                walk(v)

    walk(i18n_catalog.STRINGS)
    return sorted(codes)


def cjk_pt(size: str) -> int:
    """CJK 的 pointsize —— 与同档拉丁**完全一致**，见文件头「对齐口径」。"""
    return SIZES[size]["pt"]


def cjk_cell(size: str) -> tuple[int, int]:
    """CJK 的 cell：宽取整一个 em（= pointsize），高与同档拉丁一致。

    宽必须是整 em 而不是"估出来的字面宽"：Noto Sans SC 的汉字与箭头就是照
    满 em 画的（实测数据见文件头），窄一个像素就切掉外框笔画。整 em 同时正
    是这套字体自己的横向步进，中文因此排成规整方阵，字与字不再贴死。

    高度对齐是关键——两者同高，混排时基线天然对上，渲染端不必再做垂直补偿。
    在此之前中文走的是另一套位图，cell 高度与拉丁不同，每个调用点都得自己
    算偏移，错一处就是一行字浮起来。

    垂直落点不另做补偿：实测汉字下缘落在拉丁基线下方 2/2/3/5 px（xs/s/m/l），
    与 CJK 字体把表意 em 框压到基线下 ~0.12 em 的通行画法吻合，magick 的
    gravity center 已经给出正确关系。
    """
    _, lat_h = SIZES[size]["cell"]
    return (cjk_pt(size), lat_h)


def render_cjk_face(magick: str, fonts, size: str, codes: list[int]) -> bytes:
    """渲染整段 CJK，返回拼接好的 4bpp 数据（顺序同 codes）。"""
    w, h = cjk_cell(size)
    pt = cjk_pt(size)
    blob = bytearray()
    for cp in codes:
        gray = render_glyph_with_fallback(magick, fonts, pt, w, h, cp)
        blob += pack_4bpp(gray)
    return bytes(blob)

# ── 字号档位 ──
#
# 以 **18 px 为 normal**（正文主力），上下各展两档。
#
# 这一版是按真机观感重定的。spec §2 原本把 18 px 定为「硬下限、仅极次要」，
# 阶梯从 21 px 起步——实测下来整体偏大：800×480 上 21 px 的标签配上不受控的
# 长值（版本号、构建串）就溢出，而且**没有可降级的小档**，一长只能截断。
#
# 12/14 两档补的正是这个缺口：容不下时降级显示，而不是把内容切掉。它们都
# 覆盖到 0x7F——没有字母就没法承载真实文本，这也是原 XS 档（只存 0x20..0x3F）
# 派不上用场的原因。
#
# 规格给的是**字高**（大写字母墨迹高度），不是 pointsize。实测 B612 Mono 的
# 关系为 cap高 ≈ pt × 0.81、advance ≈ pt × 0.65，cell 高取 pt × 1.15 容纳
# 升降部：
#
#     pt   cap高   cell(宽×高)   用途
#     15    12      10×17        极密集
#     17    14      11×20        次要 / 降级
#     22    18      15×26        正文主力
#     32    26      21×37        标题
#     56    43      37×64        PFD 当前值
#
# XL 仍只存 0x20..0x3F：它只服务 PFD 的高度/速度大数字，纯数字带正负号，
# 存整套字母是浪费。
SIZES = {
    "xs": dict(pt=15, cell=(10, 17), last=0x7F),  # 12 px  极密集：列表次要列、角标
    "s":  dict(pt=17, cell=(11, 20), last=0x7F),  # 14 px  次要；长文本降级承载
    "m":  dict(pt=22, cell=(15, 26), last=0x7F),  # 18 px  **正文主力（normal）**
    "l":  dict(pt=32, cell=(21, 37), last=0x7F),  # 26 px  页面标题
    "xl": dict(pt=56, cell=(37, 64), last=0x3F),  # 43 px  PFD 当前值（仅数字与符号）
}

# Noto Sans SC 的可变字重实例化点。500 而不是 400：400 在 217 PPI 的
# 半反半透面板上笔画偏细，反射态下汉字会"发飘"。
CJK_WGHT = 500


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


def emit_c(path: Path, faces: dict[str, bytes],
           cjk_faces: dict[str, bytes],
           cjk_codes: list[int]) -> None:
    lines = [
        "/* 由 firmware/scripts/gen_pfd_aa_font.py 生成，请勿手改。",
        " *",
        " * 拉丁 B612 Mono（Airbus/ENAC，SIL OFL）+ 中日文 Noto Sans SC。",
        " * 4bpp 灰度，固定 cell，每档字号独立渲染以保证抗锯齿质量。",
        " * 只有一档字重（见脚本头「字重」一节）。",
        " */",
        '#include "pfd_aa_font.h"',
        "",
    ]
    for size, blob in faces.items():
        w, h = SIZES[size]["cell"]
        n = SIZES[size]["last"] - FIRST_CODE + 1
        lines.append(f"/* {size}: cell {w}x{h}, "
                     f"{n} glyphs (0x{FIRST_CODE:02X}..0x{SIZES[size]['last']:02X}), "
                     f"{len(blob)} bytes */")
        lines.append(f"const uint8_t pk_aa_{size}[] = {{")
        for i in range(0, len(blob), 16):
            chunk = ", ".join(f"0x{b:02X}" for b in blob[i:i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
    for size, blob in cjk_faces.items():
        w, h = cjk_cell(size)
        lines.append(f"/* {size} CJK: cell {w}x{h}, "
                     f"{len(cjk_codes)} glyphs, {len(blob)} bytes */")
        lines.append(f"const uint8_t pk_aa_{size}_cjk[] = {{")
        for i in range(0, len(blob), 16):
            chunk = ", ".join(f"0x{b:02X}" for b in blob[i:i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
    if cjk_codes:
        lines.append(f"/* CJK 码位表，升序，供二分查找（{len(cjk_codes)} 项）。 */")
        lines.append("const uint16_t pk_aa_cjk_codes[] = {")
        for i in range(0, len(cjk_codes), 12):
            chunk = ", ".join(f"0x{c:04X}" for c in cjk_codes[i:i + 12])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def emit_h(path: Path, n_cjk: int) -> None:
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
        lines.append(f"extern const uint8_t pk_aa_{size}[];")
    lines.append("")
    lines.append(f"#define PK_AA_CJK_COUNT  {n_cjk}")
    for size in CJK_SIZES:
        w, h = cjk_cell(size)
        up = size.upper()
        lines += [f"#define PK_AA_{up}_CJK_W  {w}",
                  f"#define PK_AA_{up}_CJK_H  {h}"]
    lines.append("")
    lines.append("extern const uint16_t pk_aa_cjk_codes[];")
    for size in CJK_SIZES:
        lines.append(f"extern const uint8_t pk_aa_{size}_cjk[];")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--magick", default="magick")
    ap.add_argument("--latin-regular", type=Path, required=True)
    ap.add_argument("--cjk", type=Path, required=True,
                    help="Noto Sans SC 可变字重 TTF")
    ap.add_argument("--out-c", type=Path, required=True)
    ap.add_argument("--out-h", type=Path, required=True)
    ap.add_argument("--sizes", default="xs,s,m,xl")
    args = ap.parse_args()

    want = [s.strip() for s in args.sizes.split(",") if s.strip()]
    faces: dict[str, bytes] = {}
    cjk_faces: dict[str, bytes] = {}
    cjk_codes: list[int] = []

    with tempfile.TemporaryDirectory() as tmp:
        cjk_font = instantiate_cjk(args.cjk, CJK_WGHT, Path(tmp))

        for size in want:
            spec = SIZES[size]
            blob = render_face(args.magick, [args.latin_regular, cjk_font],
                               spec["pt"], spec["cell"], spec["last"])
            faces[size] = blob
            print(f"  {size}: {len(blob)} bytes "
                  f"(cell {spec['cell'][0]}x{spec['cell'][1]}, pt {spec['pt']})")

        cjk_codes = collect_cjk_codes()
        for size in want:
            if size not in CJK_SIZES:
                continue
            blob = render_cjk_face(args.magick, [cjk_font, args.latin_regular],
                                   size, cjk_codes)
            cjk_faces[size] = blob
            cw, ch = cjk_cell(size)
            print(f"  {size} CJK: {len(blob)} bytes "
                  f"(cell {cw}x{ch}, {len(cjk_codes)} 字)")

    emit_c(args.out_c, faces, cjk_faces, cjk_codes)
    emit_h(args.out_h, len(cjk_codes))
    total = sum(len(b) for b in faces.values())
    print(f"共 {len(faces)} 套字形，{total} bytes → {args.out_c.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
