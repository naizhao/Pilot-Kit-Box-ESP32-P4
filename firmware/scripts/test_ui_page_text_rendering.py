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

        # 关于页 3afa39c 起改用 pfd_aa_text 的 pk_aa_puts（中西文同一份字体、
        # 同一档 cell 高，不需要 page_* 那套垂直补偿）。老的 8×8
        # pk_text_puts_title 仍然禁止——那套字在这块屏上只有 1.0 mm。
        about = (ROOT / "main" / "about_page.c").read_text(encoding="utf-8")
        self.assertNotIn("pk_text_puts_title", about)
        self.assertIn("pk_aa_puts", about)

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

    def test_about_title_uses_shared_middle_ui_size(self) -> None:
        """标题字号仍是 M 档，但不再按语言分叉。

        旧实现给中英文各留一条分支（ABOUT_HEADER_TITLE_Y / ABOUT_HEADER_UI_Y）。
        3afa39c 之后中西文由同一份 AA 字体生成、cell 高一致，两条分支和两个
        专属 Y 都没了，位置/字号/颜色统一取 pfd_layout.h 的 PK_UI_TITLE_*。
        这里断言的是"没有回潮"：一次绘制、一个共享档位。
        """
        text = (ROOT / "main" / "about_page.c").read_text(encoding="utf-8")
        self.assertNotIn("ABOUT_HEADER_TITLE_Y", text)
        self.assertNotIn("ABOUT_HEADER_UI_Y", text)
        self.assertNotIn("if (lang == PK_LANG_ZH)", text)
        self.assertRegex(
            text,
            r"pk_aa_puts\([^;]+PK_UI_TITLE_Y,[^;]+"
            r"pk_i18n_text\(PK_TR_ABOUT_TITLE\)[^;]+PK_UI_TITLE_SIZE",
        )
        layout = (ROOT / "main" / "pfd_layout.h").read_text(encoding="utf-8")
        self.assertRegex(layout, r"#define\s+PK_UI_TITLE_SIZE\s+PK_AA_M\b")

    def test_cjk_solid_rendering_uses_alpha_threshold(self) -> None:
        text = (ROOT / "main" / "text.c").read_text(encoding="utf-8")
        self.assertIn("CJK_SOLID_ALPHA4_THRESHOLD", text)
        self.assertIn("alpha4 < CJK_SOLID_ALPHA4_THRESHOLD", text)
        self.assertNotIn("ascii_scale <= 1);", text)

    def test_page_renderers_keep_ascii_and_cjk_on_matching_ladders(self) -> None:
        """两个 page_* 渲染器各自的拉丁档位必须配得上它取的 CJK 字库。

        旧断言盯的是 pk_font_putchar(..., 2) / (..., 1) 这套点阵缩放。拉丁侧
        早已整体换成 AA 字体（aa_putc + PK_AA_*），点阵路径在 text.c 里不再
        出现。配对关系本身没变，只是换了表达：
          page_title → PK_AA_L 配大号 pk_text_cjk_glyph
          page_body  → PK_AA_M 配 UI 号 pk_text_cjk_ui_glyph
        错配就会出现"汉字比旁边的字母矮一圈"。
        """
        text = (ROOT / "main" / "text.c").read_text(encoding="utf-8")
        self.assertIn("int pk_text_puts_page_title", text)
        self.assertIn("int pk_text_puts_page_body", text)
        self.assertNotIn("pk_font_putchar", text)

        title = text[text.index("int pk_text_puts_page_title"):
                     text.index("int pk_text_puts_page_body")]
        self.assertIn("aa_putc(fb, fb_w, fb_h, x, y, cp, color, PK_AA_L)", title)
        self.assertIn("pk_text_cjk_glyph(cp)", title)

        body = text[text.index("int pk_text_puts_page_body"):]
        self.assertIn("aa_putc(fb, fb_w, fb_h, x, y, cp, color, PK_AA_M)", body)
        self.assertIn("pk_text_cjk_ui_glyph(cp, &cw)", body)
        # solid=false：CJK 走真灰度混合，不压实成硬阶梯。
        self.assertIn("PK_TEXT_CJK_UI_CELL_H, color, false", body)

    def test_antialiased_cjk_uses_raw_glyph_alpha(self) -> None:
        """4.3 寸屏上不再重映射 CJK 的 alpha 曲线。

        旧断言要求一张 CJK_AA_LCD_ALPHA4[16] 查表把 alpha≥5 一律拉满。那是
        2.8 寸 167 PPI 的补偿；这块 217 PPI 屏字号已提到 21..30 px，再压实等
        于扔掉抗锯齿，汉字会"发破"。所以这里反过来钉住：查表不许回来，
        alpha 必须是字库灰度的线性展开（4bpp × 17 → 0..255）。
        """
        text = (ROOT / "main" / "text.c").read_text(encoding="utf-8")
        # 只看代码，不看注释——text.c 的注释里留了这张表的来历，那是有意为之。
        self.assertNotRegex(text, r"CJK_AA_LCD_ALPHA4\s*\[\s*16\s*\]\s*=")
        self.assertNotIn("alpha4 = CJK_AA_LCD_ALPHA4[alpha4]", text)
        self.assertIn("if (alpha4 == 0) continue;", text)
        self.assertIn("uint8_t alpha = (uint8_t)(alpha4 * 17);", text)

        # solid 路径（小字压实）保留，用阈值而不是查表。
        self.assertIn("CJK_SOLID_ALPHA4_THRESHOLD", text)
        match = re.search(r"#define\s+CJK_SOLID_ALPHA4_THRESHOLD\s+(\d+)", text)
        self.assertIsNotNone(match)
        self.assertGreaterEqual(int(match.group(1)), 1)


if __name__ == "__main__":
    unittest.main()
