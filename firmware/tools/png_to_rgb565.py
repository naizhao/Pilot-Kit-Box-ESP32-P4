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
from PIL import Image, ImageChops

# Composite background (white) for the boot splash logo container —
# matches the white rounded card that boot_splash.c draws behind the
# logo. If the firmware ever changes the container colour, update
# this and re-run the script.
LOGO_BG = (255, 255, 255)


def auto_crop_to_content(img):
    """Trim surrounding solid-white padding so the logo content fills the
    output canvas. Many vendor "export" PNGs come with a huge white
    margin around the artwork (here: only ~18% of the canvas is
    actual logo, the rest is white). Without cropping, the logo
    appears as a tiny mark in the corner of whatever destination we
    scale it into."""
    if img.mode != "RGB":
        img = img.convert("RGB")
    white = Image.new("RGB", img.size, (255, 255, 255))
    diff = ImageChops.difference(img, white)
    bbox = diff.getbbox()
    if bbox is None:
        return img        # All-white image — nothing to crop
    # Add a 2% margin around the detected bbox so the artwork doesn't
    # touch the edges of the output.
    w, h = img.size
    pad = max(2, min(w, h) // 50)
    cropped = img.crop((max(0, bbox[0] - pad),
                        max(0, bbox[1] - pad),
                        min(w, bbox[2] + pad),
                        min(h, bbox[3] + pad)))
    print(f"  auto-cropped: {img.size} → {cropped.size} (content bbox {bbox})")
    return cropped


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    src = sys.argv[1]
    dst = sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) == 4 else 128

    img = Image.open(src).convert("RGBA")
    # Composite onto white BACKGROUND first so the auto-crop sees a
    # consistent picture (any alpha would have shown as black/garbage
    # under the difference). The firmware's rounded white card behind
    # the logo will continue the background colour seamlessly.
    bg = Image.new("RGB", img.size, LOGO_BG)
    bg.paste(img, mask=img.split()[3])
    img = bg

    img = auto_crop_to_content(img)

    img = img.resize((size, size), Image.LANCZOS)

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
