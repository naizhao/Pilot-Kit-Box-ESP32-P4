#!/usr/bin/env python3
"""
png_to_rgb565.py — convert a PNG (typically RGBA) into a raw
little-endian RGB565 byte stream sized for the ST7789 240×320 panel.

Usage:
    png_to_rgb565.py <in.png> <out.rgb565> <size>

The output file is `size * size * 2` bytes — exactly what the
framebuffer expects when copied with memcpy into an aligned
uint16_t row.

RGBA composites onto the firmware's background colour
COL_BG = pk_rgb565(12, 12, 16) so a logo with transparency drops
cleanly onto the boot splash without a halo. PNGs already in RGB
are converted in-place.
"""

import sys
import os
from PIL import Image

# Match firmware/main/about_page.c / cal_wizard.c COL_BG (12, 12, 16)
BG = (12, 12, 16)


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    src = sys.argv[1]
    dst = sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) == 4 else 128

    img = Image.open(src).convert("RGBA").resize((size, size), Image.LANCZOS)
    # Composite alpha onto BG so transparent regions match the firmware's clear colour.
    bg = Image.new("RGB", img.size, BG)
    bg.paste(img, mask=img.split()[3])
    img = bg

    out = bytearray()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b = img.getpixel((x, y))
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            # Little-endian so a uint16_t[] cast on a RISC-V/Xtensa
            # target reads the value correctly without byte-swap.
            out += bytes([v & 0xFF, (v >> 8) & 0xFF])

    with open(dst, "wb") as f:
        f.write(out)
    print(f"wrote {dst}: {img.width}x{img.height} = {len(out)} bytes "
          f"(expected {img.width * img.height * 2})")


if __name__ == "__main__":
    main()
