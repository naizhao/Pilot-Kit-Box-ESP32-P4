#!/usr/bin/env python3
"""Tests for UI page text colors on low color-depth LCDs."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


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
    def test_settings_text_colors_avoid_gray(self) -> None:
        """设置页的配色现在归 settings_draw.c（它拥有几何与绘制）。

        原断言盯的是 settings_page.c 里的 COL_HEADER/COL_KEY/COL_VAL/COL_DIM。
        那四个宏只服务该文件里的 settings_render_legacy() —— 320×240 逐行版面，
        挂着 __attribute__((unused))，2026-07-30 随硬件换代（2.4″ → 4.3″）整段
        删除，宏也一起走了。settings_page.c 如今只剩状态机与 NVS 写操作，一个
        像素都不画，不该再有配色宏。

        规矩本身没变，只是换了主人：正文别暗到在 RGB565 面板上糊掉。
        """
        path = ROOT / "main" / "settings_page.c"
        text = path.read_text(encoding="utf-8")
        self.assertNotRegex(text, r"#define\s+COL_[A-Z_]*\s+pk_rgb565")

        draw = (ROOT / "main" / "settings_draw.c").read_text(encoding="utf-8")
        key = re.search(r"V2_KEY\s*=\s*pk_rgb565\(\s*(\d+),\s*(\d+),\s*(\d+)\)", draw)
        self.assertIsNotNone(key, "settings_draw.c missing V2_KEY")
        self.assertGreaterEqual(min(int(p) for p in key.groups()), 200)

    def test_about_text_colors_avoid_gray(self) -> None:
        """关于页重写后（3afa39c）不再自带 COL_HEADER/COL_DIM。

        标题色改走 pfd_layout.h 的 PK_UI_TITLE_COL（五页统一），页内只剩
        数值 / 产品名 / 网址 / 标签四种。这里断言的仍是原来那条规矩：正文别
        暗到在 RGB565 面板上糊掉；退档的标签允许暗一点，但必须还带色相，
        否则和分隔线的中性灰混成一片。
        """
        path = ROOT / "main" / "about_page.c"

        for name in ("COL_VAL", "COL_NAME"):
            rgb = color_macro(path, name)
            with self.subTest(color=name, rgb=rgb):
                self.assertGreaterEqual(min(rgb), 220)   # 正文近白

        url = color_macro(path, "COL_URL")
        self.assertGreaterEqual(max(url), 220)
        self.assertFalse(is_gray(url), f"COL_URL is gray: {url}")

        # 标签刻意退一档（"让数值出挑"），只要求它比数值暗且不是中性灰。
        key = color_macro(path, "COL_KEY")
        self.assertLess(max(key), max(color_macro(path, "COL_VAL")))
        self.assertFalse(is_gray(key), f"COL_KEY is gray: {key}")

    def test_settings_title_color_comes_from_shared_layout(self) -> None:
        """设置页也不再自定标题色（与关于页同一条规矩）。

        旧断言要的是"淡蓝小标题 + 纯白正文"，那是 settings_render_legacy() 的
        配色。legacy 删掉后标题统一收到 pfd_layout.h 的 PK_UI_TITLE_COL，五页
        一个样；这里反过来断言"页内不许再冒出自己的标题色"。
        """
        draw = (ROOT / "main" / "settings_draw.c").read_text(encoding="utf-8")
        self.assertNotRegex(draw, r"#define\s+COL_HEADER\b")
        self.assertIn("PK_UI_TITLE_COL", draw)
        title_rgb = color_macro(ROOT / "main" / "pfd_layout.h", "PK_UI_TITLE_COL")
        self.assertGreaterEqual(min(title_rgb), 220)

    def test_about_title_color_comes_from_shared_layout(self) -> None:
        """关于页不再自定标题色。

        旧断言要求这一页也是"淡蓝小标题"。3afa39c 明确推翻了它：淡蓝在本页
        另有主人（COL_URL 那条可点链接），标题统一收到 pfd_layout.h 的
        PK_UI_TITLE_COL，五页一个样。所以这里反过来断言"页内不许再有自己的
        标题色宏"，防止有人把它加回来。
        """
        text = (ROOT / "main" / "about_page.c").read_text(encoding="utf-8")
        self.assertNotRegex(text, r"#define\s+COL_HEADER\b")
        self.assertIn("PK_UI_TITLE_COL", text)
        title_rgb = color_macro(ROOT / "main" / "pfd_layout.h", "PK_UI_TITLE_COL")
        self.assertGreaterEqual(min(title_rgb), 220)


if __name__ == "__main__":
    unittest.main()
