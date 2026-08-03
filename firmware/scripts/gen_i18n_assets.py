#!/usr/bin/env python3
"""Generate the built-in i18n lookup table (firmware/main/i18n_catalog.{h,c}).

本脚本还是另外三个生成器的**公共底座**：gen_pfd_aa_font.py / gen_pfd_icons.py /
gen_nav_icons.py 从这里 import render_glyph / render_glyph_with_fallback /
pack_4bpp。改那几个函数会同时影响它们三个。
"""

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

# ── 这里曾经还生成过位图 CJK 字库 ────────────────────────────────────
#
# 2026-07-30 先删掉第三档「UI 变宽字库」（text_font_cjk_ui.c）。
# 2026-08-03 把剩下的两档（text_font_cjk.c L30 / text_font_cjk_body.c M26）
# 连同它们唯一的渲染器 text.c 一起删了：最后一个调用者是磁力计校准向导，它
# 随 4.3″ 改版换到了 pfd_aa_text 的 pk_aa_puts，与其余各页共用同一份抗锯齿
# 字体。此后这两档字库零调用者，只是白占 app 分区。
#
# **屏上现在的汉字来自 gen_pfd_aa_font.py**（pfd_aa_font.c 里的 CJK 段，
# 码位表同样从 i18n_catalog 收集）。加了新汉字要重跑的是那一个，不是本脚本；
# 本脚本只管词条表 i18n_catalog.{h,c}，与字形无关。
#
# 下面 render_glyph / pack_4bpp 那一层留着不动：它是 gen_pfd_aa_font.py /
# gen_pfd_icons.py / gen_nav_icons.py 共用的渲染底座，不属于被删的字库。


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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parents[1] / "main")
    parser.add_argument("--ledger", type=Path, default=LEDGER_PATH,
                        help="词条 ID 台账（key→永久 ID），默认 scripts/i18n_ids.json")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    strings = i18n_catalog.STRINGS
    validate_catalog(strings)
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

    # 加了新汉字的话，屏上的字形还要重跑 gen_pfd_aa_font.py —— 见文件头。
    # 本脚本到此为止，不再产出任何字库。


if __name__ == "__main__":
    try:
        main()
    except LedgerError as exc:
        # 明确失败，绝不静默回退到「按出现顺序分配 ID」。
        print(f"ID 台账错误：{exc}", file=sys.stderr)
        raise SystemExit(2)
