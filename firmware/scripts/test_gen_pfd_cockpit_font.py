#!/usr/bin/env python3
"""Tests for the fixed-cell PFD cockpit font generator."""

from __future__ import annotations

import unittest

import gen_pfd_cockpit_font as fontgen


def bbox(glyph: list[list[int]]) -> tuple[int, int, int, int]:
    pts = [(x, y) for y, row in enumerate(glyph) for x, px in enumerate(row) if px]
    if not pts:
        raise AssertionError("glyph has no pixels")
    xs = [x for x, _ in pts]
    ys = [y for _, y in pts]
    return min(xs), min(ys), max(xs), max(ys)


class CockpitFontTest(unittest.TestCase):
    def test_readout_glyphs_keep_legacy_scale2_metrics(self) -> None:
        for ch in "HDGADSBK0123456789O":
            with self.subTest(ch=ch):
                x0, y0, x1, y1 = bbox(fontgen.glyph_for(ord(ch)))
                self.assertGreaterEqual(x0, 0)
                self.assertEqual(y0, 0)
                self.assertLessEqual(x1, 9)
                self.assertEqual(y1, 13)

    def test_zero_has_diagonal_slash_that_distinguishes_it_from_letter_o(self) -> None:
        zero = fontgen.glyph_for(ord("0"))
        letter_o = fontgen.glyph_for(ord("O"))

        self.assertNotEqual(zero, letter_o)
        self.assertGreaterEqual(sum(zero[2][6:10]), 3)
        self.assertGreaterEqual(sum(zero[6][2:6]), 2)
        self.assertGreaterEqual(sum(zero[8][0:4]), 3)
        self.assertLessEqual(sum(zero[6][0:10]), 7)
        self.assertLessEqual(sum(letter_o[6][0:10]), 4)

    def test_b_is_distinct_from_digit_8(self) -> None:
        letter_b = fontgen.glyph_for(ord("B"))
        digit_8 = fontgen.glyph_for(ord("8"))

        self.assertNotEqual(letter_b, digit_8)
        self.assertEqual("".join("#" if px else "." for px in letter_b[0]), "########....")
        self.assertEqual("".join("#" if px else "." for px in digit_8[0]), ".########...")

    def test_k_uses_clear_left_stem_and_diagonal_arms(self) -> None:
        glyph = fontgen.glyph_for(ord("K"))

        for y in range(14):
            with self.subTest(row=y):
                self.assertEqual(glyph[y][0], 1)
                self.assertEqual(glyph[y][1], 1)

        self.assertEqual("".join("#" if px else "." for px in glyph[0]), "##.....##...")
        self.assertEqual("".join("#" if px else "." for px in glyph[6]), "#####.......")
        self.assertEqual("".join("#" if px else "." for px in glyph[13]), "##......##..")

    def test_y_has_diagonal_arms_and_centered_stem(self) -> None:
        glyph = fontgen.glyph_for(ord("Y"))

        # 上半是两条斜臂，从两侧外缘开始向中心收拢
        self.assertEqual("".join("#" if px else "." for px in glyph[0]), "##......##..")
        # 顶部中部必须留空：旧版误用贯穿竖线段 "m"，导致此处多一道竖线
        self.assertEqual(glyph[1][4], 0)
        self.assertEqual(glyph[1][5], 0)
        # 下半是一条居中竖线
        self.assertEqual("".join("#" if px else "." for px in glyph[13]), "....##......")

    def test_v_is_full_diagonal_converging_to_a_point(self) -> None:
        glyph = fontgen.glyph_for(ord("V"))

        # 顶部为两条外缘斜臂（旧版顶行为空，笔画从 row1 才开始）
        self.assertEqual("".join("#" if px else "." for px in glyph[0]), "##......##..")
        # 上半必须斜向内收，而非旧版的平行竖直"方肩"：第 3 行外缘应已让出
        self.assertEqual(glyph[3][0], 0)
        self.assertEqual(glyph[3][9], 0)
        # 底部汇聚成居中竖尖（旧版底部仍是分开的两笔）
        self.assertEqual("".join("#" if px else "." for px in glyph[13]), "....##......")


if __name__ == "__main__":
    unittest.main()
