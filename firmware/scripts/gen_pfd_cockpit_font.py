#!/usr/bin/env python3
"""Generate a fixed-cell cockpit-style 1-bit glyph subset for PFD readouts.

The source is deliberately pixel-native instead of TTF-derived. Each glyph is
drawn into the legacy scale-2 10x14 ink box inside a 12x16 cell, then packed
into the same 4bpp container consumed by pfd_font.c. Values are either 0x0 or
0xF.
"""

from __future__ import annotations

import argparse
from pathlib import Path


WIDTH = 12
HEIGHT = 16
BODY_WIDTH = 10
BODY_HEIGHT = 14

DEFAULT_CODES = (
    [0x20]
    + list(range(ord("0"), ord("9") + 1))
    + list(range(ord("A"), ord("Z") + 1))
    + [ord(c) for c in "+-./:_()"]
    + [0x7F]
)


def c_char_label(code: int) -> str:
    if code == 0x20:
        return "space"
    if code == 0x7F:
        return "degree"
    return chr(code)


def blank() -> list[list[int]]:
    return [[0 for _ in range(WIDTH)] for _ in range(HEIGHT)]


def put(g: list[list[int]], x: int, y: int) -> None:
    if 0 <= x < WIDTH and 0 <= y < HEIGHT:
        g[y][x] = 1


def rect(g: list[list[int]], x0: int, y0: int, x1: int, y1: int) -> None:
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            put(g, x, y)


def line(g: list[list[int]], x0: int, y0: int, x1: int, y1: int, thick: int = 2) -> None:
    dx = abs(x1 - x0)
    sx = 1 if x0 < x1 else -1
    dy = -abs(y1 - y0)
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    x = x0
    y = y0
    while True:
        for oy in range(thick):
            for ox in range(thick):
                put(g, x + ox, y + oy)
        if x == x1 and y == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x += sx
        if e2 <= dx:
            err += dx
            y += sy


def seg(g: list[list[int]], name: str) -> None:
    if name == "a":
        rect(g, 1, 0, 8, 1)
    elif name == "b":
        rect(g, 8, 1, 9, 6)
    elif name == "c":
        rect(g, 8, 7, 9, 12)
    elif name == "d":
        rect(g, 1, 12, 8, 13)
    elif name == "e":
        rect(g, 0, 7, 1, 12)
    elif name == "f":
        rect(g, 0, 1, 1, 6)
    elif name == "g":
        rect(g, 1, 6, 8, 7)
    elif name == "m":
        rect(g, 4, 1, 5, 12)
    elif name == "ul":
        line(g, 1, 1, 4, 6)
    elif name == "ur":
        line(g, 8, 1, 5, 6)
    elif name == "ll":
        line(g, 1, 12, 4, 7)
    elif name == "lr":
        line(g, 8, 12, 5, 7)
    elif name == "slash":
        line(g, 1, 12, 8, 1)
    elif name == "backslash":
        line(g, 1, 1, 8, 12)
    else:
        raise ValueError(f"unknown segment {name}")


SEGMENTS: dict[str, tuple[str, ...]] = {
    "0": ("a", "b", "c", "d", "e", "f"),
    "1": ("b", "c"),
    "2": ("a", "b", "g", "e", "d"),
    "3": ("a", "b", "g", "c", "d"),
    "4": ("f", "g", "b", "c"),
    "5": ("a", "f", "g", "c", "d"),
    "6": ("a", "f", "g", "e", "c", "d"),
    "7": ("a", "b", "c"),
    "8": ("a", "b", "c", "d", "e", "f", "g"),
    "9": ("a", "b", "c", "d", "f", "g"),
    "A": ("a", "b", "c", "e", "f", "g"),
    "B": ("f", "e", "g", "c", "d"),
    "C": ("a", "f", "e", "d"),
    "D": ("a", "b", "c", "d", "e"),
    "E": ("a", "f", "e", "g", "d"),
    "F": ("a", "f", "e", "g"),
    "G": ("a", "f", "e", "d", "c", "g"),
    "H": ("f", "e", "b", "c", "g"),
    "I": ("a", "m", "d"),
    "J": ("b", "c", "d", "e"),
    "K": ("f", "e", "slash", "backslash"),
    "L": ("f", "e", "d"),
    "M": ("f", "e", "b", "c", "ul", "ur"),
    "N": ("f", "e", "b", "c", "backslash"),
    "O": ("a", "b", "c", "d", "e", "f"),
    "P": ("a", "b", "f", "e", "g"),
    "Q": ("a", "b", "c", "d", "e", "f", "lr"),
    "R": ("a", "b", "f", "e", "g", "lr"),
    "S": ("a", "f", "g", "c", "d"),
    "T": ("a", "m"),
    "U": ("f", "e", "b", "c", "d"),
    "V": ("f", "b", "ll", "lr"),
    "W": ("f", "e", "b", "c", "ll", "lr"),
    "X": ("slash", "backslash"),
    "Y": ("ul", "ur", "m"),
    "Z": ("a", "slash", "d"),
}

PATTERNS: dict[str, tuple[str, ...]] = {
    "0": (
        ".########.",
        "##########",
        "##.....###",
        "##....###.",
        "##...##.##",
        "##..##..##",
        "##.##...##",
        "####....##",
        "###.....##",
        "##......##",
        "##......##",
        "##......##",
        "##########",
        ".########.",
    ),
    "1": (
        "...##.....",
        "..###.....",
        ".####.....",
        "...##.....",
        "...##.....",
        "...##.....",
        "...##.....",
        "...##.....",
        "...##.....",
        "...##.....",
        "...##.....",
        "...##.....",
        "..######..",
        "..######..",
    ),
    "4": (
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##########",
        "##########",
        "........##",
        "........##",
        "........##",
        "........##",
        "........##",
        "........##",
    ),
    "7": (
        "##########",
        "##########",
        ".......###",
        "......###.",
        ".....###..",
        "....###...",
        "...###....",
        "...##.....",
        "...##.....",
        "...##.....",
        "...##.....",
        "...##.....",
        "...##.....",
        "...##.....",
    ),
    "A": (
        ".########.",
        "##########",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##########",
        "##########",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
    ),
    "B": (
        "########..",
        "#########.",
        "##.....##.",
        "##.....##.",
        "##.....##.",
        "##.....##.",
        "########..",
        "#########.",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "#########.",
        "########..",
    ),
    "D": (
        "########..",
        "#########.",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "#########.",
        "########..",
    ),
    "G": (
        ".########.",
        "#########.",
        "##........",
        "##........",
        "##........",
        "##........",
        "##..######",
        "##..######",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "#########.",
        ".########.",
    ),
    "H": (
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##########",
        "##########",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
        "##......##",
    ),
    "K": (
        "##.....##.",
        "##....##..",
        "##...##...",
        "##..##....",
        "##.##.....",
        "####......",
        "#####.....",
        "##.###....",
        "##..##....",
        "##...##...",
        "##....##..",
        "##.....##.",
        "##......##",
        "##......##",
    ),
    "S": (
        ".########.",
        "#########.",
        "##........",
        "##........",
        "##........",
        "##........",
        "#########.",
        ".#########",
        "........##",
        "........##",
        "........##",
        "........##",
        "#########.",
        ".########.",
    ),
}


def glyph_from_pattern(rows: tuple[str, ...]) -> list[list[int]]:
    if len(rows) != BODY_HEIGHT:
        raise ValueError(f"pattern has {len(rows)} rows, expected {BODY_HEIGHT}")
    glyph = blank()
    for y, row in enumerate(rows):
        if len(row) != BODY_WIDTH:
            raise ValueError(f"pattern row has {len(row)} cols, expected {BODY_WIDTH}: {row!r}")
        for x, ch in enumerate(row):
            if ch == "#":
                put(glyph, x, y)
    return glyph


def draw_special(ch: str) -> list[list[int]] | None:
    g = blank()
    if ch == " ":
        return g
    if ch == "-":
        seg(g, "g")
    elif ch == "+":
        seg(g, "g")
        rect(g, 4, 3, 5, 10)
    elif ch == ".":
        rect(g, 4, 12, 5, 13)
    elif ch == "/":
        seg(g, "slash")
    elif ch == ":":
        rect(g, 4, 3, 5, 4)
        rect(g, 4, 9, 5, 10)
    elif ch == "_":
        rect(g, 1, 13, 8, 13)
    elif ch == "(":
        rect(g, 2, 1, 3, 2)
        rect(g, 1, 3, 2, 10)
        rect(g, 2, 11, 3, 12)
    elif ch == ")":
        rect(g, 6, 1, 7, 2)
        rect(g, 7, 3, 8, 10)
        rect(g, 6, 11, 7, 12)
    elif ch == "\x7f":
        rect(g, 3, 0, 6, 0)
        rect(g, 2, 1, 3, 3)
        rect(g, 6, 1, 7, 3)
        rect(g, 3, 4, 6, 4)
    else:
        return None
    return g


def glyph_for(code: int) -> list[list[int]]:
    ch = "\x7f" if code == 0x7F else chr(code)
    if ch in PATTERNS:
        return glyph_from_pattern(PATTERNS[ch])

    special = draw_special(ch)
    if special is not None:
        return special

    g = blank()
    for name in SEGMENTS.get(ch, ()):
        seg(g, name)
    return g


def pack_4bpp(glyph: list[list[int]]) -> bytes:
    values: list[int] = []
    for row in glyph:
        values.extend(0xF if px else 0x0 for px in row)

    packed = bytearray()
    for i in range(0, len(values), 2):
        hi = values[i]
        lo = values[i + 1] if i + 1 < len(values) else 0
        packed.append((hi << 4) | lo)
    return bytes(packed)


def emit_header(path: Path) -> None:
    path.write_text(
        f"""/* Generated by firmware/scripts/gen_pfd_cockpit_font.py. */
#pragma once

#include <stdint.h>

#define PK_FONT_AA_CELL_W {WIDTH}
#define PK_FONT_AA_CELL_H {HEIGHT}
#define PK_FONT_AA_GLYPH_BYTES ((PK_FONT_AA_CELL_W * PK_FONT_AA_CELL_H) / 2)

const uint8_t *pk_font_aa_glyph(unsigned code);
""",
        encoding="utf-8",
    )


def emit_source(path: Path, header_name: str, codes: list[int], bitmaps: list[bytes]) -> None:
    lines: list[str] = [
        "/* Generated by firmware/scripts/gen_pfd_cockpit_font.py.",
        " * Source: built-in 10x14 cockpit bitmap strokes in a 12x16 cell.",
        " * Glyphs are packed as 1-bit masks in 4bpp cells, two pixels per byte.",
        " */",
        "",
        f'#include "{header_name}"',
        "",
        "#include <stddef.h>",
        "",
        f"static const uint8_t s_font_aa_codes[{len(codes)}] = {{",
    ]
    for code in codes:
        lines.append(f"    0x{code:02X}, /* {c_char_label(code)} */")
    lines.extend(["};", "", f"static const uint8_t s_font_aa_bitmaps[{len(codes)}][PK_FONT_AA_GLYPH_BYTES] = {{"])

    for code, bitmap in zip(codes, bitmaps):
        lines.append(f"    /* 0x{code:02X} {c_char_label(code)} */")
        lines.append("    {")
        for i in range(0, len(bitmap), 12):
            chunk = ", ".join(f"0x{b:02X}" for b in bitmap[i : i + 12])
            lines.append(f"        {chunk},")
        lines.append("    },")

    lines.extend(
        [
            "};",
            "",
            "const uint8_t *pk_font_aa_glyph(unsigned code)",
            "{",
            "    for (size_t i = 0; i < sizeof(s_font_aa_codes); ++i) {",
            "        if (s_font_aa_codes[i] == code) return s_font_aa_bitmaps[i];",
            "    }",
            "    return NULL;",
            "}",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def print_preview(codes: list[int]) -> None:
    for code in codes:
        label = c_char_label(code)
        print(f"--- {label} ---")
        glyph = glyph_for(code)
        for row in glyph:
            print("".join("#" if px else "." for px in row))
        print()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-c", type=Path)
    parser.add_argument("--out-h", type=Path)
    parser.add_argument("--chars", default=None)
    parser.add_argument("--preview", action="store_true")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    codes = DEFAULT_CODES if args.chars is None else sorted({ord(c) for c in args.chars})
    if 0x7F not in codes:
        codes.append(0x7F)
    codes = sorted(set(codes))

    if args.preview:
        print_preview(codes)

    if args.out_c or args.out_h:
        if not args.out_c or not args.out_h:
            raise SystemExit("--out-c and --out-h must be passed together")
        bitmaps = [pack_4bpp(glyph_for(code)) for code in codes]
        emit_header(args.out_h)
        emit_source(args.out_c, args.out_h.name, codes, bitmaps)


if __name__ == "__main__":
    main()
