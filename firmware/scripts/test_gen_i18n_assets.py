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

    def test_ui_weight_font_generation_stays_removed(self) -> None:
        """21 px「UI 变宽字库」这一档不许回潮（2026-07-30 决定）。

        text_font_cjk_ui.c 只喂 text.c 里的 pk_text_puts_ui / page_* 渲染器，
        而那三个只被 settings/diag 两页的 *_render_legacy() 调用——那两个从上线
        起就挂着 __attribute__((unused))，一年零调用者。硬件已换成 4.3″ 800×480
        触摸屏，各页统一走 pfd_aa_text，不存在退回 2.4″ 逐行版面的场景，所以整
        条链路（渲染器 + 字库 + 这里的生成代码）一起删。

        本脚本仍必须生成词条表与另外两档 CJK 字库，由下面几个测试守住。
        """
        for name in ("collect_ui_codepoints", "ui_glyph_width",
                     "emit_ui_font_header", "emit_ui_font_source",
                     "pack_ui_4bpp"):
            self.assertFalse(hasattr(gen, name), f"{name} 不该回来")
        for name in ("DEFAULT_UI_BODY_H", "DEFAULT_UI_BODY_PT",
                     "DEFAULT_UI_ASCII_W", "DEFAULT_UI_WIDE_W"):
            self.assertFalse(hasattr(gen, name), f"{name} 不该回来")

    def test_catalog_and_two_cjk_ladders_are_still_generated(self) -> None:
        """删 UI 档时不能误伤词条表和 L30/M26 两档字库。"""
        for name in ("emit_i18n_header", "emit_i18n_source",
                     "emit_cjk_header", "emit_cjk_source",
                     "collect_cjk_codepoints"):
            self.assertTrue(hasattr(gen, name), f"{name} 丢了")
        self.assertEqual(gen.DEFAULT_TITLE_CELL, 30)
        self.assertEqual(gen.DEFAULT_BODY_CELL, 26)

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

    def test_readable_pack_preserves_antialias_alpha_levels(self) -> None:
        gray = bytes([0, 48, 96, 255])
        packed = gen.pack_readable_4bpp(gray)
        levels = [(byte >> 4, byte & 0x0F) for byte in packed]
        flat = [level for pair in levels for level in pair]
        self.assertEqual(packed, gen.pack_4bpp(gray))
        self.assertTrue(any(0 < level < 15 for level in flat))

    def test_readable_pack_normalizes_small_glyphs_to_full_alpha(self) -> None:
        fonts = gen.default_font_chain()
        for ch in ("A", "0", "项", "版", "于"):
            with self.subTest(ch=ch):
                gray = gen.render_glyph_with_fallback(
                    "magick", fonts, 11, 12, 12, ord(ch)
                )
                packed = gen.pack_readable_4bpp(gray)
                flat = [level for byte in packed for level in (byte >> 4, byte & 0x0F)]
                self.assertEqual(max(flat), 15)
                self.assertTrue(any(0 < level < 15 for level in flat))

    def test_readable_pack_normalizes_title_cjk_glyphs_to_full_alpha(self) -> None:
        fonts = gen.default_font_chain()
        for ch in ("关", "于", "语", "言"):
            with self.subTest(ch=ch):
                gray = gen.render_glyph_with_fallback("magick", fonts, 15, 16, 16, ord(ch))
                packed = gen.pack_readable_4bpp(gray)
                flat = [level for byte in packed for level in (byte >> 4, byte & 0x0F)]
                self.assertEqual(max(flat), 15)
                self.assertTrue(any(0 < level < 15 for level in flat))

    def test_cjk_title_and_body_glyphs_have_distinct_storage_sizes(self) -> None:
        self.assertEqual(gen.glyph_bytes_4bpp(16, 16), 128)
        self.assertEqual(gen.glyph_bytes_4bpp(10, 10), 50)
        self.assertEqual(gen.glyph_bytes_4bpp(8, 8), 32)

    def test_fallback_fonts_cover_catalog_cjk(self) -> None:
        fonts = gen.default_font_chain()
        for code in gen.collect_cjk_codepoints(i18n_catalog.STRINGS):
            with self.subTest(code=f"U+{code:04X}", char=chr(code)):
                # 12×12 只是「这个字形在字体里有墨」的探针尺寸，与真正落盘的
                # L30/M26 两档无关——档位由上面 DEFAULT_*_CELL 那条断言守。
                gray = gen.render_glyph_with_fallback(
                    "magick", fonts, 11, 12, 12, code
                )
                self.assertGreater(max(gray), 0)


if __name__ == "__main__":
    unittest.main()
