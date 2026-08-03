#!/usr/bin/env python3
"""Tests for the generated i18n string table.

CJK 字形子集不再由本脚本生成（2026-08-03，见 gen_i18n_assets.py 文件头）：
屏上的汉字来自 gen_pfd_aa_font.py 的 pfd_aa_font.c。凡是「码位子集是否覆盖
catalog」这类断言，都改问那一个——问已经不产字库的脚本等于测了个寂寞。
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import gen_i18n_assets as gen
import gen_pfd_aa_font as aafont
import i18n_catalog


class I18nIdLedgerTest(unittest.TestCase):
    """守住「ID 一经分配终身不变」。

    2026-08 的事故：当时 ID 按词条在 STRINGS 里的出现顺序分配，有人插了一条在中
    间，PK_TR_DEMO_BADGE 62→63，一个陈旧的 .o 仍按 62 取文案，屏上把演示徽章画
    成了「(数据为模」。下面这些测试就是不让那件事重演。
    """

    def setUp(self) -> None:
        self.doc, self.ids = gen.load_ledger()

    def test_every_catalog_key_already_has_a_permanent_id(self) -> None:
        for key, _ in i18n_catalog.STRINGS:
            with self.subTest(key=key):
                self.assertIn(key, self.ids, f"{key} 不在台账里，请重跑 gen_i18n_assets.py")

    def test_ledger_ids_are_unique(self) -> None:
        self.assertEqual(len(set(self.ids.values())), len(self.ids))

    def test_new_key_gets_max_plus_one_and_moves_nothing(self) -> None:
        before = dict(self.ids)
        strings = (list(i18n_catalog.STRINGS)[:5]
                   + [("ZZ_UNIT_TEST_KEY", {lang: "x" for lang in i18n_catalog.LANGS})]
                   + list(i18n_catalog.STRINGS)[5:])
        added = gen.assign_new_ids(self.ids, strings)
        self.assertEqual(added, [("ZZ_UNIT_TEST_KEY", max(before.values()) + 1)])
        for key, text_id in before.items():
            self.assertEqual(self.ids[key], text_id, f"{key} 的 ID 被挪动了")

    def test_deleted_key_leaves_a_reserved_hole(self) -> None:
        ids = dict(self.ids)
        ids["ZZ_RETIRED_KEY"] = max(ids.values()) + 1
        hole = ids["ZZ_RETIRED_KEY"]
        slots = gen.build_slots(ids, i18n_catalog.STRINGS)
        self.assertEqual(len(slots), hole + 1, "PK_TR_COUNT 必须覆盖到空洞")
        self.assertEqual(slots[hole], ("ZZ_RETIRED_KEY", None))
        self.assertEqual(gen.slot_label(hole, slots[hole]), f"PK_TR_RESERVED_{hole}")

    def test_hole_is_emitted_as_empty_string_not_null(self) -> None:
        """pk_i18n_text() 的调用方直接把返回值喂给 pfd_aa_puts，NULL 会崩。"""
        ids = dict(self.ids)
        ids["ZZ_RETIRED_KEY"] = max(ids.values()) + 1
        hole = ids["ZZ_RETIRED_KEY"]
        slots = gen.build_slots(ids, i18n_catalog.STRINGS)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "i18n_catalog.c"
            gen.emit_i18n_source(path, slots)
            body = path.read_text(encoding="utf-8")
        self.assertIn(f'[PK_TR_RESERVED_{hole}] = "",', body)
        self.assertNotIn("= NULL,", body)

    def test_missing_or_corrupt_ledger_raises_instead_of_renumbering(self) -> None:
        """绝不允许静默回退到「按顺序分配」——那等于这次修复白做。"""
        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "absent.json"
            with self.assertRaises(gen.LedgerError):
                gen.load_ledger(missing)

            broken = Path(tmp) / "broken.json"
            broken.write_text("{not json", encoding="utf-8")
            with self.assertRaises(gen.LedgerError):
                gen.load_ledger(broken)

            empty = Path(tmp) / "empty.json"
            empty.write_text(json.dumps({"ids": {}}), encoding="utf-8")
            with self.assertRaises(gen.LedgerError):
                gen.load_ledger(empty)

            dup = Path(tmp) / "dup.json"
            dup.write_text(json.dumps({"ids": {"A": 1, "B": 1}}), encoding="utf-8")
            with self.assertRaises(gen.LedgerError):
                gen.load_ledger(dup)

    def test_demo_badge_id_is_pinned(self) -> None:
        """事故当事人，钉死。改这两个数字之前先读 i18n_ids.json 顶部的说明。"""
        self.assertEqual(self.ids["DEMO_BADGE"], 65)
        self.assertEqual(self.ids["SETTINGS_DEMO_HINT"], 64)


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
        """屏上的 CJK 码位表必须覆盖 catalog 里出现的每一个非 ASCII 字符。

        问的是 gen_pfd_aa_font（现役字库），不是 gen_i18n_assets——后者
        2026-08-03 起不再产字形。它比 catalog 多出的只有 ARROW_CODES 那八个
        方向箭头（符号不是文案，见那边的注释），所以是包含关系而不是相等。
        """
        expected = {
            ord(ch)
            for _, translations in i18n_catalog.STRINGS
            for text in translations.values()
            for ch in text
            if ord(ch) > 0x7F
        }
        codes = set(aafont.collect_cjk_codes())
        self.assertTrue(expected <= codes, f"字库漏字: {sorted(expected - codes)}")
        self.assertEqual(codes - expected, set(aafont.ARROW_CODES))
        self.assertIn(ord("设"), expected)
        self.assertIn(ord("中"), expected)
        self.assertNotIn(ord("A"), expected)

    def test_bitmap_cjk_font_generation_stays_removed(self) -> None:
        """本脚本产字库这条链路整条不许回潮。

        2026-07-30 先删 21 px「UI 变宽字库」（text_font_cjk_ui.c）：它只喂
        text.c 里的 pk_text_puts_ui / page_* 三个渲染器，而那三个只被
        settings/diag 两页的 *_render_legacy() 调用，从上线起就挂着
        __attribute__((unused))。

        2026-08-03 删掉剩下的 L30 / M26 两档（text_font_cjk.c /
        text_font_cjk_body.c）连同它们唯一的渲染器 text.c：最后一个调用者是
        磁力计校准向导，它随 4.3″ 改版换到了 pfd_aa_text 的 pk_aa_puts。

        所以这是反向断言：这些生成函数一旦回来，就说明有人在把位图字库那条
        路走回去，应当先回到「各页统一走 pfd_aa_text」这条决定本身。
        """
        for name in ("collect_ui_codepoints", "ui_glyph_width",
                     "emit_ui_font_header", "emit_ui_font_source",
                     "pack_ui_4bpp"):
            self.assertFalse(hasattr(gen, name), f"{name} 不该回来")
        for name in ("DEFAULT_UI_BODY_H", "DEFAULT_UI_BODY_PT",
                     "DEFAULT_UI_ASCII_W", "DEFAULT_UI_WIDE_W"):
            self.assertFalse(hasattr(gen, name), f"{name} 不该回来")
        for name in ("emit_cjk_header", "emit_cjk_source", "glyph_bytes_4bpp",
                     "pack_readable_4bpp", "normalize_glyph_alpha",
                     "threshold_4bpp", "collect_cjk_codepoints",
                     "DEFAULT_TITLE_CELL", "DEFAULT_BODY_CELL",
                     "DEFAULT_TITLE_PT", "DEFAULT_BODY_PT"):
            self.assertFalse(hasattr(gen, name), f"{name} 不该回来")

    def test_catalog_table_is_still_generated(self) -> None:
        """删字库时不能误伤词条表——那才是本脚本现在唯一的产物。"""
        for name in ("emit_i18n_header", "emit_i18n_source"):
            self.assertTrue(hasattr(gen, name), f"{name} 丢了")
        # 匹配的是**写盘那一行**的形状（out_dir / "text_font_cjk…"），不是
        # 裸文件名——文件头那段"为什么删"的注释里必然会写出这些名字，搜名字
        # 等于禁止解释历史，下一个人只会把注释也删掉。
        source = Path(gen.__file__).read_text(encoding="utf-8")
        self.assertNotIn('out_dir / "text_font_cjk', source)

    def test_glyph_rendering_base_stays_for_sibling_generators(self) -> None:
        """渲染底座留着：另外三个生成器 import 的就是这几个。"""
        for name in ("render_glyph", "render_glyph_with_fallback",
                     "pack_4bpp", "glyph_has_ink", "default_font_chain"):
            self.assertTrue(hasattr(gen, name), f"{name} 是共用底座，不能删")

    def test_default_ui_font_is_noto_sans_sc(self) -> None:
        self.assertEqual(gen.DEFAULT_UI_FONT.name, "NotoSansSC-VariableFont_wght.ttf")
        self.assertEqual(gen.default_font_chain()[0], gen.DEFAULT_UI_FONT)

    def test_cjk_glyphs_are_packed_as_4bpp_alpha(self) -> None:
        gray = bytes([0, 17, 128, 255])
        self.assertEqual(gen.pack_4bpp(gray), bytes([0x01, 0x8F]))

    def test_fallback_fonts_cover_catalog_cjk(self) -> None:
        fonts = gen.default_font_chain()
        for code in aafont.collect_cjk_codes():
            with self.subTest(code=f"U+{code:04X}", char=chr(code)):
                # 12×12 只是「这个字形在字体里有墨」的探针尺寸，与真正落盘的
                # xs/s/m/l 四档无关——档位由 gen_pfd_aa_font.SIZES 定。
                gray = gen.render_glyph_with_fallback(
                    "magick", fonts, 11, 12, 12, code
                )
                self.assertGreater(max(gray), 0)


if __name__ == "__main__":
    unittest.main()
