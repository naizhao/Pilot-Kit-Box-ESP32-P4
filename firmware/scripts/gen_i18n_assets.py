#!/usr/bin/env python3
"""Generate built-in i18n lookup tables and CJK glyph subsets."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import i18n_catalog


# ── 词条 ID 台账 ────────────────────────────────────────────────────────
#
# PK_TR_* 的枚举值**不再**由词条在 i18n_catalog.STRINGS 里的出现顺序决定，
# 而是由 i18n_ids.json 这份台账固定下来：一个 key 拿到的 ID 终身不变，删掉
# 也只留空洞、绝不复用。
#
# 起因：2026-08 真机上演示模式徽章显示成「(数据为模」。当时生成器按出现顺序
# 发 ID，有人把新词条插在中间，后面所有词条整体后移一位（PK_TR_DEMO_BADGE
# 62→63）；恰好 pk_ui_nav.c.o 是陈旧的、仍按旧 ID 62 取文案，而字符串表已重
# 编，62 号早换成了另一条。编译零警告、烧录校验通过、串口日志正常——完全静默，
# 而它是提示「当前数据是模拟的」的安全件。
#
# ID 钉死之后，陈旧 .o 用的旧 ID 仍指向同一条文案；最坏是新词条还没进那份 .o，
# 那是链接期就会报的显性错误，不会再变成屏上一句错话。
LEDGER_PATH = Path(__file__).resolve().parent / "i18n_ids.json"


DEFAULT_UI_FONT = (
    Path(__file__).resolve().parents[1]
    / "assets" / "fonts" / "NotoSansSC-VariableFont_wght.ttf"
)
DEFAULT_CJK_FALLBACK_FONT = (
    Path(__file__).resolve().parents[1]
    / "assets" / "fonts" / "NotoSansSC-VariableFont_wght.ttf"
)
DEFAULT_TITLE_FONT = DEFAULT_UI_FONT
DEFAULT_BODY_FONT = DEFAULT_UI_FONT

# ── 字库尺寸（spec §2 字体阶梯）──────────────────────────────────────
#
# 这些数字曾经只活在某次手敲的命令行里，脚本自己的默认值还停在 320×240 时代
# 的 16/8/12 px。于是「改完 catalog 重跑一遍脚本」这个本该无脑的动作，会把三
# 套 CJK 字库**静默降级**回 spec §2 明令禁止的档位（「低于 18 px 一律禁止出
# 现」），而且降级后照样编译、照样出图，只是屏上汉字全变小。
#
# 所以默认值就是真值：L30 / M26 / S21，与 3afa39c 重生成时用的一致。改档位请
# 改这里，不要再靠命令行参数——命令行会随着终端历史一起消失。
#
# 2026-07-30 去掉了第三档「UI 变宽字库」（text_font_cjk_ui.c，21 px、ASCII 8 /
# CJK 12 px 变宽）。它只喂 text.c 里那三个 page_*/ui 渲染器，而那三个只被
# settings/diag 两页已删除的 *_render_legacy() 调用。硬件换成 4.3″ 800×480 后
# 各页改走 pfd_aa_text，不会再退回去，字库跟着一起删。
# 本脚本**仍然**负责生成词条表 i18n_catalog.c/h —— 那部分与字库无关。
DEFAULT_TITLE_CELL = 30       # 页面标题档
DEFAULT_TITLE_PT = 29
DEFAULT_BODY_CELL = 26        # 正文主力档
DEFAULT_BODY_PT = 25


def enum_label(prefix: str, name: str) -> str:
    return f"{prefix}_{name.upper()}"


def c_string(value: str) -> str:
    return (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
    )


def validate_catalog(strings: list[tuple[str, dict[str, str]]]) -> None:
    seen: set[str] = set()
    lang_set = set(i18n_catalog.LANGS)
    for text_id, translations in strings:
        if text_id in seen:
            raise ValueError(f"duplicate text id: {text_id}")
        seen.add(text_id)
        if set(translations) != lang_set:
            raise ValueError(f"{text_id} languages {sorted(translations)} != {sorted(lang_set)}")
        for lang, text in translations.items():
            if not text:
                raise ValueError(f"{text_id}.{lang} is empty")


class LedgerError(RuntimeError):
    """台账缺失或损坏。

    刻意不做「回退到按顺序分配」——那正是这次事故的成因，静默回退等于修复白做。
    """


def load_ledger(path: Path = LEDGER_PATH) -> tuple[dict, dict[str, int]]:
    """读取并校验 ID 台账，返回 (整份文档, key→id)。"""
    if not path.exists():
        raise LedgerError(
            f"ID 台账 {path} 不存在。它是 PK_TR_* 枚举值的唯一权威来源，"
            f"不能靠按顺序重新分配来重建——那会让所有 ID 平移。"
            f"请从 git 恢复该文件。"
        )
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise LedgerError(f"ID 台账 {path} 不是合法 JSON：{exc}") from exc
    if not isinstance(doc, dict):
        raise LedgerError(f"ID 台账 {path} 顶层必须是对象")

    ids = doc.get("ids")
    if not isinstance(ids, dict) or not ids:
        raise LedgerError(f"ID 台账 {path} 缺少非空的 \"ids\" 对象")

    by_id: dict[int, str] = {}
    for key, value in ids.items():
        if not isinstance(key, str) or not key:
            raise LedgerError(f"ID 台账里有非法 key: {key!r}")
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise LedgerError(f"ID 台账 {key} 的 ID {value!r} 不是非负整数")
        if value in by_id:
            raise LedgerError(
                f"ID 台账里 ID {value} 被 {by_id[value]} 和 {key} 同时占用；"
                f"ID 必须唯一，且已分配的绝不能改"
            )
        by_id[value] = key

    labels: dict[str, str] = {}
    for key in ids:
        label = enum_label("PK_TR", key)
        if label in labels:
            raise LedgerError(f"词条 {labels[label]} 与 {key} 生成同一个枚举名 {label}")
        labels[label] = key
    return doc, ids


def save_ledger(doc: dict, ids: dict[str, int], path: Path = LEDGER_PATH) -> None:
    """按 ID 升序回写台账——新词条永远追加在末尾，diff 就是新增的那几行。"""
    doc["ids"] = dict(sorted(ids.items(), key=lambda kv: kv[1]))
    path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")


def assign_new_ids(ids: dict[str, int],
                   strings: list[tuple[str, dict[str, str]]]) -> list[tuple[str, int]]:
    """给台账里还没有的 key 分配 max(id)+1；已有的 key 一律沿用台账值。"""
    next_id = max(ids.values()) + 1
    added: list[tuple[str, int]] = []
    for key, _ in strings:
        if key in ids:
            continue
        ids[key] = next_id
        added.append((key, next_id))
        next_id += 1
    return added


def build_slots(
    ids: dict[str, int],
    strings: list[tuple[str, dict[str, str]]],
) -> list[tuple[str, dict[str, str] | None] | None]:
    """摊成一个下标即 ID 的稠密数组。

    槽位有三种：
      - (key, 译文)  正常词条
      - (key, None)  台账里有、catalog 里已删除 —— ID 保留占位，不复用
      - None         台账里就没有这个 ID（历史空洞）—— 同样占位
    """
    by_key = dict(strings)
    slots: list[tuple[str, dict[str, str] | None] | None] = [None] * (max(ids.values()) + 1)
    for key, text_id in ids.items():
        slots[text_id] = (key, by_key.get(key))
    return slots


def slot_label(index: int, slot: tuple[str, dict[str, str] | None] | None) -> str:
    if slot is not None and slot[1] is not None:
        return enum_label("PK_TR", slot[0])
    return f"PK_TR_RESERVED_{index}"


def collect_cjk_codepoints(strings: list[tuple[str, dict[str, str]]]) -> set[int]:
    return {
        ord(ch)
        for _, translations in strings
        for text in translations.values()
        for ch in text
        if ord(ch) > 0x7F
    }


def default_font_chain(primary: Path | None = None,
                       fallback: Path | None = None) -> list[Path]:
    primary = primary or DEFAULT_UI_FONT
    fallback = fallback or DEFAULT_CJK_FALLBACK_FONT
    fonts = [primary]
    if fallback != primary:
        fonts.append(fallback)
    return fonts


def emit_i18n_header(path: Path,
                     slots: list[tuple[str, dict[str, str] | None] | None]) -> None:
    lines = [
        "/* Generated by firmware/scripts/gen_i18n_assets.py.",
        " * 枚举值来自 firmware/scripts/i18n_ids.json（ID 台账），不是词条顺序。",
        " * ID 一经分配终身不变；删掉的词条留下 PK_TR_RESERVED_<id> 占位，不复用。",
        " */",
        "#pragma once",
        "",
        "typedef enum {",
    ]
    for idx, lang in enumerate(i18n_catalog.LANGS):
        lines.append(f"    PK_LANG_{lang.upper()} = {idx},")
    lines.extend(
        [
            "    PK_LANG_COUNT,",
            "} pk_lang_t;",
            "",
            "typedef enum {",
        ]
    )
    for idx, slot in enumerate(slots):
        label = f"    {slot_label(idx, slot)} = {idx},"
        if slot is None:
            label += "  /* 历史空洞，ID 保留不复用 */"
        elif slot[1] is None:
            label += f"  /* 已删除词条 {slot[0]}，ID 保留不复用 */"
        lines.append(label)
    lines.extend(
        [
            "    PK_TR_COUNT,",
            "} pk_tr_id_t;",
            "",
            "const char *pk_i18n_catalog_text(pk_lang_t lang, pk_tr_id_t id);",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def emit_i18n_source(path: Path,
                     slots: list[tuple[str, dict[str, str] | None] | None]) -> None:
    lines = [
        "/* Generated by firmware/scripts/gen_i18n_assets.py.",
        " * 下标即 firmware/scripts/i18n_ids.json 里的永久 ID；空洞填空串，",
        " * 因为 pk_i18n_text() 的调用方直接把返回值喂给 pfd_aa_puts，NULL 会崩。",
        " */",
        '#include "i18n_catalog.h"',
        "",
        "static const char *const s_text[PK_LANG_COUNT][PK_TR_COUNT] = {",
    ]
    for lang in i18n_catalog.LANGS:
        lines.append(f"    [PK_LANG_{lang.upper()}] = {{")
        for idx, slot in enumerate(slots):
            label = slot_label(idx, slot)
            if slot is not None and slot[1] is not None:
                lines.append(f'        [{label}] = "{c_string(slot[1][lang])}",')
            else:
                lines.append(f'        [{label}] = "",')
        lines.append("    },")
    lines.extend(
        [
            "};",
            "",
            "const char *pk_i18n_catalog_text(pk_lang_t lang, pk_tr_id_t id)",
            "{",
            "    if (lang < 0 || lang >= PK_LANG_COUNT) lang = PK_LANG_EN;",
            "    if (id < 0 || id >= PK_TR_COUNT) return \"?\";",
            "    const char *text = s_text[lang][id];",
            "    if (!text) text = s_text[PK_LANG_EN][id];",
            "    /* 表里每个槽位都写了字面量，理论上到不了这里；但调用方拿到就直接",
            "     * 渲染，宁可显示 \"?\" 也不能把 NULL 递出去。 */",
            "    return text ? text : \"?\";",
            "}",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def render_glyph(
    magick: str,
    font: Path,
    point_size: int,
    width: int,
    height: int,
    code: int,
) -> bytes:
    if code == 0x20:
        return bytes(width * height)

    with tempfile.TemporaryDirectory() as tmp:
        label_path = Path(tmp) / "glyph.txt"
        raw_path = Path(tmp) / "glyph.gray"
        label_path.write_text(chr(code), encoding="utf-8")
        subprocess.run(
            [
                magick,
                "-background",
                "black",
                "-fill",
                "white",
                "-font",
                str(font),
                "-pointsize",
                str(point_size),
                "-gravity",
                "center",
                "-size",
                f"{width}x{height}",
                f"label:@{label_path}",
                "-depth",
                "8",
                f"gray:{raw_path}",
            ],
            check=True,
        )
        gray = raw_path.read_bytes()
    expected = width * height
    if len(gray) != expected:
        raise RuntimeError(f"glyph U+{code:04X} produced {len(gray)} bytes, expected {expected}")
    return gray


def glyph_has_ink(gray: bytes) -> bool:
    return any(gray)


def render_glyph_with_fallback(
    magick: str,
    fonts: list[Path],
    point_size: int,
    width: int,
    height: int,
    code: int,
) -> bytes:
    if code == 0x20:
        return bytes(width * height)

    last_gray = bytes(width * height)
    for font in fonts:
        gray = render_glyph(magick, font, point_size, width, height, code)
        if glyph_has_ink(gray):
            return gray
        last_gray = gray
    return last_gray


def pack_4bpp(gray: bytes) -> bytes:
    packed = bytearray()
    for i in range(0, len(gray), 2):
        hi = min(15, (gray[i] + 8) // 17)
        lo = 0
        if i + 1 < len(gray):
            lo = min(15, (gray[i + 1] + 8) // 17)
        packed.append((hi << 4) | lo)
    return bytes(packed)


def normalize_glyph_alpha(gray: bytes) -> bytes:
    """Stretch a glyph mask so its strongest ink reaches full opacity.

    Tiny variable-font glyphs can render with a low maximum gray value
    when their strokes land between pixels. Packing that raw mask makes
    the firmware blend the stroke body at only 25-50% alpha. Normalizing
    per glyph keeps anti-aliased edge levels while making the main stroke
    opaque on the LCD.
    """
    max_v = max(gray) if gray else 0
    if max_v <= 0 or max_v == 255:
        return gray
    return bytes((v * 255 + max_v // 2) // max_v if v else 0 for v in gray)


def pack_readable_4bpp(gray: bytes) -> bytes:
    return pack_4bpp(normalize_glyph_alpha(gray))


def glyph_bytes_4bpp(width: int, height: int) -> int:
    return (width * height + 1) // 2


def emit_cjk_header(path: Path, width: int, height: int,
                    macro_prefix: str, glyph_func: str) -> None:
    path.write_text(
        f"""/* Generated by firmware/scripts/gen_i18n_assets.py. */
#pragma once

#include <stdint.h>

#define {macro_prefix}_CELL_W {width}
#define {macro_prefix}_CELL_H {height}
#define {macro_prefix}_GLYPH_BYTES {glyph_bytes_4bpp(width, height)}

const uint8_t *{glyph_func}(uint32_t code);
""",
        encoding="utf-8",
    )


def emit_cjk_source(
    path: Path,
    header_name: str,
    codes: list[int],
    bitmaps: list[bytes],
    macro_prefix: str,
    glyph_func: str,
) -> None:
    lines = [
        "/* Generated by firmware/scripts/gen_i18n_assets.py.",
        " * Glyphs are 4bpp alpha masks, two pixels per byte.",
        " */",
        f'#include "{header_name}"',
        "",
        "#include <stddef.h>",
        "",
        f"static const uint32_t s_cjk_codes[{len(codes)}] = {{",
    ]
    for code in codes:
        lines.append(f"    0x{code:04X}, /* {chr(code)} */")
    lines.extend(["};", "", f"static const uint8_t s_cjk_bitmaps[{len(codes)}][{macro_prefix}_GLYPH_BYTES] = {{"])
    for code, bitmap in zip(codes, bitmaps):
        lines.append(f"    /* U+{code:04X} {chr(code)} */")
        lines.append("    {")
        for i in range(0, len(bitmap), 12):
            lines.append("        " + ", ".join(f"0x{b:02X}" for b in bitmap[i : i + 12]) + ",")
        lines.append("    },")
    lines.extend(
        [
            "};",
            "",
            f"const uint8_t *{glyph_func}(uint32_t code)",
            "{",
            "    for (size_t i = 0; i < sizeof(s_cjk_codes) / sizeof(s_cjk_codes[0]); ++i) {",
            "        if (s_cjk_codes[i] == code) return s_cjk_bitmaps[i];",
            "    }",
            "    return NULL;",
            "}",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def threshold_4bpp(gray: bytes, threshold: int = 48) -> bytes:
    """Pack a solid bitmap mask into the existing 4bpp glyph container."""
    packed = bytearray()
    for i in range(0, len(gray), 2):
        hi = 0x0F if gray[i] >= threshold else 0x00
        lo = 0x00
        if i + 1 < len(gray):
            lo = 0x0F if gray[i + 1] >= threshold else 0x00
        packed.append((hi << 4) | lo)
    return bytes(packed)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parents[1] / "main")
    parser.add_argument("--ledger", type=Path, default=LEDGER_PATH,
                        help="词条 ID 台账（key→永久 ID），默认 scripts/i18n_ids.json")
    parser.add_argument("--font", type=Path, default=DEFAULT_TITLE_FONT)
    parser.add_argument("--body-font", type=Path, default=DEFAULT_BODY_FONT)
    parser.add_argument("--fallback-font", type=Path, default=DEFAULT_CJK_FALLBACK_FONT)
    parser.add_argument("--body-fallback-font", type=Path, default=DEFAULT_CJK_FALLBACK_FONT)
    parser.add_argument("--magick", default="magick")
    parser.add_argument("--point-size", type=int, default=DEFAULT_TITLE_PT)
    parser.add_argument("--body-point-size", type=int, default=DEFAULT_BODY_PT)
    parser.add_argument("--width", type=int, default=DEFAULT_TITLE_CELL)
    parser.add_argument("--height", type=int, default=DEFAULT_TITLE_CELL)
    parser.add_argument("--body-width", type=int, default=DEFAULT_BODY_CELL)
    parser.add_argument("--body-height", type=int, default=DEFAULT_BODY_CELL)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    strings = i18n_catalog.STRINGS
    validate_catalog(strings)
    if not args.font.exists():
        raise FileNotFoundError(args.font)
    if not args.body_font.exists():
        raise FileNotFoundError(args.body_font)
    if not args.fallback_font.exists():
        raise FileNotFoundError(args.fallback_font)
    if not args.body_fallback_font.exists():
        raise FileNotFoundError(args.body_fallback_font)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    doc, ids = load_ledger(args.ledger)
    added = assign_new_ids(ids, strings)
    if added:
        save_ledger(doc, ids, args.ledger)
        for key, text_id in added:
            print(f"新词条 {key} 分配到永久 ID {text_id}（已写入 {args.ledger.name}）")
    retired = sorted(
        (key for key in ids if key not in dict(strings)),
        key=lambda k: ids[k],
    )
    if retired:
        print("台账保留的已删词条（ID 不复用）："
              + ", ".join(f"{k}={ids[k]}" for k in retired))

    slots = build_slots(ids, strings)
    emit_i18n_header(args.out_dir / "i18n_catalog.h", slots)
    emit_i18n_source(args.out_dir / "i18n_catalog.c", slots)

    codes = sorted(collect_cjk_codepoints(strings))
    title_fonts = default_font_chain(args.font, args.fallback_font)
    body_fonts = default_font_chain(args.body_font, args.body_fallback_font)
    bitmaps = [
        pack_readable_4bpp(render_glyph_with_fallback(args.magick, title_fonts,
                                                      args.point_size, args.width,
                                                      args.height, code))
        for code in codes
    ]
    emit_cjk_header(args.out_dir / "text_font_cjk.h",
                    args.width, args.height,
                    "PK_TEXT_CJK", "pk_text_cjk_glyph")
    emit_cjk_source(args.out_dir / "text_font_cjk.c",
                    "text_font_cjk.h", codes, bitmaps,
                    "PK_TEXT_CJK", "pk_text_cjk_glyph")

    body_bitmaps = [
        pack_readable_4bpp(render_glyph_with_fallback(args.magick, body_fonts,
                                                      args.body_point_size,
                                                      args.body_width,
                                                      args.body_height, code))
        for code in codes
    ]
    emit_cjk_header(args.out_dir / "text_font_cjk_body.h",
                    args.body_width, args.body_height,
                    "PK_TEXT_CJK_BODY", "pk_text_cjk_body_glyph")
    emit_cjk_source(args.out_dir / "text_font_cjk_body.c",
                    "text_font_cjk_body.h", codes, body_bitmaps,
                    "PK_TEXT_CJK_BODY", "pk_text_cjk_body_glyph")


if __name__ == "__main__":
    try:
        main()
    except LedgerError as exc:
        # 明确失败，绝不静默回退到「按出现顺序分配 ID」。
        print(f"ID 台账错误：{exc}", file=sys.stderr)
        raise SystemExit(2)
