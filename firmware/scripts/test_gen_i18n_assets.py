#!/usr/bin/env python3
"""Tests for generated i18n strings and CJK glyph subsets."""

from __future__ import annotations

import unittest

import gen_i18n_assets as gen
import i18n_catalog


class I18nAssetGeneratorTest(unittest.TestCase):
    def test_every_text_id_has_every_language(self) -> None:
        for text_id, translations in i18n_catalog.STRINGS:
            with self.subTest(text_id=text_id):
                self.assertEqual(set(translations), set(i18n_catalog.LANGS))
                for lang in i18n_catalog.LANGS:
                    self.assertTrue(translations[lang])

    def test_zh_footer_keeps_command_tokens_separated(self) -> None:
        strings = dict(i18n_catalog.STRINGS)
        self.assertIn("MODE ", strings["ABOUT_FOOTER"]["zh"])
        self.assertIn("UP/DOWN ", strings["ABOUT_FOOTER"]["zh"])
        self.assertIn("MODE ", strings["SETTINGS_FOOTER"]["zh"])
        self.assertIn("UP/DOWN ", strings["SETTINGS_FOOTER"]["zh"])
        self.assertIn("TARE ", strings["SETTINGS_FOOTER"]["zh"])

    def test_cjk_subset_contains_only_non_ascii_chars_from_catalog(self) -> None:
        expected = {
            ord(ch)
            for _, translations in i18n_catalog.STRINGS
            for text in translations.values()
            for ch in text
            if ord(ch) > 0x7F
        }
        self.assertEqual(gen.collect_cjk_codepoints(i18n_catalog.STRINGS), expected)
        self.assertIn(ord("设"), expected)
        self.assertIn(ord("中"), expected)
        self.assertNotIn(ord("A"), expected)

    def test_ui_subset_contains_ascii_degree_and_catalog_cjk(self) -> None:
        codes = gen.collect_ui_codepoints(i18n_catalog.STRINGS)
        self.assertIn(ord("A"), codes)
        self.assertIn(ord("z"), codes)
        self.assertIn(ord("0"), codes)
        self.assertIn(ord(":"), codes)
        self.assertIn(0x00B0, codes)
        self.assertIn(ord("设"), codes)
        self.assertIn(ord("中"), codes)

    def test_default_ui_font_is_noto_sans_sc(self) -> None:
        self.assertEqual(gen.DEFAULT_UI_FONT.name, "NotoSansSC-VariableFont_wght.ttf")
        self.assertEqual(gen.DEFAULT_TITLE_FONT, gen.DEFAULT_UI_FONT)
        self.assertEqual(gen.DEFAULT_BODY_FONT, gen.DEFAULT_UI_FONT)
        self.assertEqual(gen.default_font_chain()[0], gen.DEFAULT_UI_FONT)

    def test_cjk_glyphs_are_packed_as_4bpp_alpha(self) -> None:
        gray = bytes([0, 17, 128, 255])
        self.assertEqual(gen.pack_4bpp(gray), bytes([0x01, 0x8F]))

    def test_threshold_4bpp_keeps_body_ui_glyphs_solid(self) -> None:
        gray = bytes([0, 95, 96, 255])
        self.assertEqual(gen.threshold_4bpp(gray, threshold=96), bytes([0x00, 0xFF]))

    def test_ui_threshold_keeps_low_contrast_cjk_pixels(self) -> None:
        gray = bytes([0, 47, 48, 255])
        self.assertEqual(gen.threshold_4bpp(gray), bytes([0x00, 0xFF]))

    def test_ui_pack_preserves_antialias_alpha_levels(self) -> None:
        gray = bytes([0, 48, 96, 255])
        packed = gen.pack_ui_4bpp(gray)
        levels = [(byte >> 4, byte & 0x0F) for byte in packed]
        flat = [level for pair in levels for level in pair]
        self.assertEqual(packed, gen.pack_4bpp(gray))
        self.assertTrue(any(0 < level < 15 for level in flat))

    def test_ui_pack_normalizes_small_glyphs_to_full_alpha(self) -> None:
        fonts = gen.default_font_chain()
        for ch in ("A", "0", "项", "版", "于"):
            with self.subTest(ch=ch):
                gray = gen.render_glyph_with_fallback(
                    "magick", fonts, 11, gen.ui_glyph_width(ord(ch)), 12, ord(ch)
                )
                packed = gen.pack_ui_4bpp(gray)
                flat = [level for byte in packed for level in (byte >> 4, byte & 0x0F)]
                self.assertEqual(max(flat), 15)
                self.assertTrue(any(0 < level < 15 for level in flat))

    def test_readable_pack_normalizes_title_cjk_glyphs_to_full_alpha(self) -> None:
        fonts = gen.default_font_chain()
        for ch in ("关", "于", "语", "言"):
            with self.subTest(ch=ch):
                gray = gen.render_glyph_with_fallback("magick", fonts, 15, 16, 16, ord(ch))
                packed = gen.pack_ui_4bpp(gray)
                flat = [level for byte in packed for level in (byte >> 4, byte & 0x0F)]
                self.assertEqual(max(flat), 15)
                self.assertTrue(any(0 < level < 15 for level in flat))

    def test_cjk_title_and_body_glyphs_have_distinct_storage_sizes(self) -> None:
        self.assertEqual(gen.glyph_bytes_4bpp(16, 16), 128)
        self.assertEqual(gen.glyph_bytes_4bpp(10, 10), 50)
        self.assertEqual(gen.glyph_bytes_4bpp(8, 8), 32)

    def test_ui_glyph_width_is_narrow_for_ascii_wide_for_cjk(self) -> None:
        self.assertEqual(gen.ui_glyph_width(ord("A")), 8)
        self.assertEqual(gen.ui_glyph_width(ord(" ")), 8)
        self.assertEqual(gen.ui_glyph_width(0x00B0), 8)
        self.assertEqual(gen.ui_glyph_width(ord("中")), 12)

    def test_fallback_fonts_cover_catalog_cjk(self) -> None:
        fonts = gen.default_font_chain()
        for code in gen.collect_cjk_codepoints(i18n_catalog.STRINGS):
            with self.subTest(code=f"U+{code:04X}", char=chr(code)):
                gray = gen.render_glyph_with_fallback(
                    "magick", fonts, 11, gen.ui_glyph_width(code), 12, code
                )
                self.assertGreater(max(gray), 0)


if __name__ == "__main__":
    unittest.main()
