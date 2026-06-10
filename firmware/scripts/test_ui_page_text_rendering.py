#!/usr/bin/env python3
"""Tests for low-resolution LCD text rendering choices."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class UiPageTextRenderingTest(unittest.TestCase):
    def test_settings_and_about_use_page_renderers(self) -> None:
        settings = (ROOT / "main" / "settings_page.c").read_text(encoding="utf-8")
        self.assertNotIn("pk_text_puts_title", settings)
        self.assertIn("pk_text_puts_page_title", settings)
        self.assertIn("pk_text_puts_page_body", settings)

        about = (ROOT / "main" / "about_page.c").read_text(encoding="utf-8")
        self.assertNotIn("pk_text_puts_title", about)
        self.assertIn("pk_text_puts_page_title", about)
        self.assertIn("pk_text_puts_page_body", about)

    def test_settings_rows_use_unified_ui_font_and_vertical_centering(self) -> None:
        text = (ROOT / "main" / "settings_page.c").read_text(encoding="utf-8")
        self.assertIn("#define SETTINGS_HEADER_UI_Y", text)
        self.assertIn("#define SETTINGS_ROW_TOP", text)
        self.assertIn("#define SETTINGS_ROW_H", text)
        self.assertIn("#define SETTINGS_ROW_GAP", text)
        self.assertIn("#define SETTINGS_ROW_COUNT       6", text)
        self.assertIn("pk_lang_t lang = pk_i18n_get_lang();", text)
        self.assertIn("if (lang == PK_LANG_ZH)", text)
        self.assertIn(
            "int text_y_ui = row_y + (SETTINGS_ROW_H - 12) / 2;",
            text,
        )
        self.assertRegex(
            text,
            r"pk_text_puts_page_title\([^;]+pk_i18n_text\(PK_TR_SETTINGS_TITLE\)",
        )
        self.assertRegex(
            text,
            r"pk_text_puts_ui\([^;]+SETTINGS_HEADER_UI_Y,[^;]+"
            r"pk_i18n_text\(PK_TR_SETTINGS_TITLE\)",
        )
        self.assertIn("22, text_y_ui, key_str, COL_KEY", text)
        self.assertIn("182, text_y_ui, val_str, val_col", text)
        self.assertNotRegex(
            text,
            r"pk_text_puts_page_title\([^;]+PK_TR_SETTINGS_LANGUAGE",
        )
        self.assertEqual(text.count("pk_text_puts_page_title"), 1)
        self.assertGreaterEqual(text.count("pk_text_puts_ui"), 3)
        self.assertIn("pk_i18n_text(PK_TR_SETTINGS_FOOTER)", text)

    def test_about_title_uses_middle_ui_size_for_english(self) -> None:
        text = (ROOT / "main" / "about_page.c").read_text(encoding="utf-8")
        self.assertIn("#define ABOUT_HEADER_TITLE_Y", text)
        self.assertIn("#define ABOUT_HEADER_UI_Y", text)
        self.assertIn("pk_lang_t lang = pk_i18n_get_lang();", text)
        self.assertIn("if (lang == PK_LANG_ZH)", text)
        self.assertRegex(
            text,
            r"pk_text_puts_page_title\([^;]+ABOUT_HEADER_TITLE_Y,[^;]+"
            r"pk_i18n_text\(PK_TR_ABOUT_TITLE\)",
        )
        self.assertRegex(
            text,
            r"pk_text_puts_ui\([^;]+ABOUT_HEADER_UI_Y,[^;]+"
            r"pk_i18n_text\(PK_TR_ABOUT_TITLE\)",
        )

    def test_cjk_solid_rendering_uses_alpha_threshold(self) -> None:
        text = (ROOT / "main" / "text.c").read_text(encoding="utf-8")
        self.assertIn("CJK_SOLID_ALPHA4_THRESHOLD", text)
        self.assertIn("alpha4 < CJK_SOLID_ALPHA4_THRESHOLD", text)
        self.assertNotIn("ascii_scale <= 1);", text)

    def test_page_renderers_keep_ascii_hard_and_cjk_antialiased(self) -> None:
        text = (ROOT / "main" / "text.c").read_text(encoding="utf-8")
        self.assertIn("int pk_text_puts_page_title", text)
        self.assertIn("int pk_text_puts_page_body", text)
        self.assertIn("pk_font_putchar(fb, fb_w, fb_h, x, y, (char)cp, color, 2)", text)
        self.assertIn("pk_font_putchar(fb, fb_w, fb_h, x, y, (char)cp, color, 1)", text)
        self.assertIn("pk_text_cjk_glyph(cp)", text)
        self.assertIn("pk_text_cjk_ui_glyph(cp, &cw)", text)
        self.assertIn("PK_TEXT_CJK_UI_CELL_H, color, false", text)

    def test_antialiased_cjk_uses_lcd_readable_alpha_curve(self) -> None:
        text = (ROOT / "main" / "text.c").read_text(encoding="utf-8")
        self.assertIn("CJK_AA_LCD_ALPHA4", text)
        self.assertIn("alpha4 = CJK_AA_LCD_ALPHA4[alpha4]", text)
        self.assertIn("if (alpha4 == 0) continue;", text)

        match = re.search(r"CJK_AA_LCD_ALPHA4\[16\]\s*=\s*\{([^}]+)\}", text)
        self.assertIsNotNone(match)
        levels = [int(value) for value in re.findall(r"\d+", match.group(1))]
        self.assertEqual(len(levels), 16)
        self.assertEqual(levels[:3], [0, 0, 0])
        self.assertGreaterEqual(levels[4], 12)
        self.assertEqual(levels[5], 15)
        self.assertEqual(levels[-1], 15)


if __name__ == "__main__":
    unittest.main()
