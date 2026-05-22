#!/usr/bin/env python3
"""Tests for UI page text colors on low color-depth LCDs."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SUBTITLE_BLUE = (180, 235, 255)
TEXT_WHITE = (255, 255, 255)


def color_macro(path: Path, name: str) -> tuple[int, int, int]:
    text = path.read_text(encoding="utf-8")
    match = re.search(
        rf"#define\s+{name}\s+pk_rgb565\(\s*(\d+),\s*(\d+),\s*(\d+)\)",
        text,
    )
    if not match:
        raise AssertionError(f"{path.name} missing {name}")
    return tuple(int(part) for part in match.groups())


def is_gray(rgb: tuple[int, int, int]) -> bool:
    r, g, b = rgb
    return abs(r - g) <= 20 and abs(g - b) <= 20 and abs(r - b) <= 20


class UiPageColorTest(unittest.TestCase):
    def assert_readable_text_color(self, path: Path, name: str) -> None:
        rgb = color_macro(path, name)
        with self.subTest(file=path.name, color=name, rgb=rgb):
            self.assertGreaterEqual(max(rgb), 220)
            if rgb != (255, 255, 255):
                self.assertFalse(is_gray(rgb), f"{name} is gray: {rgb}")

    def test_settings_text_colors_avoid_gray(self) -> None:
        path = ROOT / "main" / "settings_page.c"
        for name in ("COL_HEADER", "COL_KEY", "COL_VAL", "COL_DIM"):
            self.assert_readable_text_color(path, name)

    def test_about_text_colors_avoid_gray(self) -> None:
        path = ROOT / "main" / "about_page.c"
        for name in ("COL_HEADER", "COL_KEY", "COL_VAL", "COL_DIM"):
            self.assert_readable_text_color(path, name)

    def assert_page_uses_requested_text_palette(self, path: Path) -> None:
        for name in ("COL_HEADER", "COL_KEY"):
            rgb = color_macro(path, name)
            with self.subTest(file=path.name, color=name, rgb=rgb):
                self.assertEqual(rgb, SUBTITLE_BLUE)
        for name in ("COL_VAL", "COL_DIM"):
            rgb = color_macro(path, name)
            with self.subTest(file=path.name, color=name, rgb=rgb):
                self.assertEqual(rgb, TEXT_WHITE)

    def test_settings_uses_blue_subtitles_and_white_text(self) -> None:
        self.assert_page_uses_requested_text_palette(ROOT / "main" / "settings_page.c")

    def test_about_uses_blue_subtitles_and_white_text(self) -> None:
        self.assert_page_uses_requested_text_palette(ROOT / "main" / "about_page.c")


if __name__ == "__main__":
    unittest.main()
