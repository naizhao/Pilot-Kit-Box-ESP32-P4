#!/usr/bin/env python3
"""Tests for low-resolution LCD text rendering choices."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def code_only(text: str) -> str:
    """剥掉 C 注释后再断言。

    这些反向断言要钉的是"符号不许回来"，而记录「为什么删」的注释里必然会
    写出这些符号名——不剥注释就等于禁止解释历史，下一个人只会把注释也删掉。
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


# ══════════════════════════════════════════════════════════════════════
# 2026-07-30 的决定：legacy 渲染器与 UI 档字库整条链路删除，不许回潮。
#
# 背景（产品负责人拍板，「不复用了，硬件都已经更换了，不会再退回去」）：
#   · settings_page.c / diag_page.c 里的 *_render_legacy() 是 320×240 时代的
#     逐行版面，改成 8 项设置 + 2×4 诊断卡片后就挂上 __attribute__((unused))
#     「留着供详情层复用」——详情层最终自己写了，legacy 一年零调用者。
#   · 它们是 text.c 里 pk_text_puts_ui / pk_text_puts_page_title /
#     pk_text_puts_page_body / pk_text_ui_width 的唯一调用者，而前三个又是
#     21 px UI 变宽字库 text_font_cjk_ui.c（4143 行）的唯一使用者。
#   · 硬件已从 2.4″ 换成 4.3″ 800×480 触摸屏，各页统一走 pfd_aa_text 的
#     pk_aa_puts（中西文同一份 AA 字体、同一档 cell 高），不存在退回场景；
#     当时 app 分区余量只剩 10%，留着纯属占地方。
#
# 所以下面这几个测试是**反向断言**：这些符号一旦重新出现，就说明有人在往回
# 走，应当先回到这条决定本身，而不是让测试给死代码背书。
# ══════════════════════════════════════════════════════════════════════

# 已删除的 UI 档渲染链路：text.c 的渲染器 + 它们背后的字库。
REMOVED_UI_FONT_SYMBOLS = (
    "pk_text_puts_ui",
    "pk_text_puts_page_title",
    "pk_text_puts_page_body",
    "pk_text_ui_width",
    "pk_text_cjk_ui_glyph",
    "text_font_cjk_ui",
    "PK_TEXT_CJK_UI_CELL_H",
)


class UiPageTextRenderingTest(unittest.TestCase):
    def test_legacy_renderers_stay_deleted(self) -> None:
        """两个 *_render_legacy() 及其专属辅助函数不许回来。"""
        settings = code_only((ROOT / "main" / "settings_page.c").read_text(encoding="utf-8"))
        diag = code_only((ROOT / "main" / "diag_page.c").read_text(encoding="utf-8"))

        self.assertNotRegex(settings, r"static\s+void\s+settings_render_legacy\s*\(")
        self.assertNotRegex(diag, r"static\s+void\s+diag_render_legacy\s*\(")
        # legacy 专属辅助：settings 的 render_row/render_row_col、diag 的
        # draw_diag_row。draw_snr_row / fmt_clock 不在此列——详情层还在用。
        self.assertNotRegex(settings, r"static\s+void\s+render_row(_col)?\s*\(")
        self.assertNotRegex(diag, r"static\s+void\s+draw_diag_row\s*\(")
        # 整仓不许再出现 __attribute__((unused)) 的整页渲染器。
        self.assertNotIn("__attribute__((unused))", settings)
        self.assertNotIn("__attribute__((unused))", diag)

        # 详情层仍在用的那两个工具必须还在（防止"删过头"）。
        self.assertRegex(diag, r"static\s+void\s+draw_snr_row\s*\(")
        self.assertRegex(diag, r"static\s+void\s+fmt_clock\s*\(")

    def test_ui_weight_font_stays_deleted(self) -> None:
        """UI 档字库与它的三个渲染器不许回到任何固件源码里。"""
        main_dir = ROOT / "main"
        self.assertFalse((main_dir / "text_font_cjk_ui.c").exists())
        self.assertFalse((main_dir / "text_font_cjk_ui.h").exists())

        for path in sorted(main_dir.glob("*.[ch]")):
            body = code_only(path.read_text(encoding="utf-8"))
            for sym in REMOVED_UI_FONT_SYMBOLS:
                with self.subTest(file=path.name, symbol=sym):
                    self.assertNotIn(sym, body)

        for cmake in (ROOT / "main" / "CMakeLists.txt",
                      ROOT.parent / "sim" / "CMakeLists.txt"):
            self.assertNotIn("text_font_cjk_ui", cmake.read_text(encoding="utf-8"))

    def test_pages_render_through_aa_text(self) -> None:
        """各页 render 入口仍在，且都走 pfd_aa_text 的 pk_aa_puts。"""
        entries = {
            "settings_draw.c": "void pk_settings_page_render(uint16_t *fb)",
            "diag_page.c": "void pk_diag_page_render(uint16_t *fb)",
            "about_page.c": "void pk_about_page_render(uint16_t *fb)",
            "adsb_list.c": "void pk_adsb_list_render(uint16_t *fb)",
            "traffic_page.c": "void pk_traffic_page_render(uint16_t *fb)",
        }
        for name, signature in entries.items():
            body = (ROOT / "main" / name).read_text(encoding="utf-8")
            with self.subTest(file=name):
                self.assertIn(signature, body)
                self.assertIn("pk_aa_puts", body)
                # 老的 8×8 点阵标题在这块 217 PPI 屏上只有 1.0 mm，一直是禁的。
                self.assertNotIn("pk_text_puts_title", body)

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

    def test_remaining_text_renderers_keep_ascii_and_cjk_on_matching_ladders(self) -> None:
        """text.c 剩下的两个渲染器，拉丁档位必须配得上它取的 CJK 字库。

        page_title / page_body / puts_ui 那三档随 legacy 一起删了（见文件头的
        决定说明），只剩校准向导在用的 pk_text_puts 与 pk_text_puts_title：
          puts_title → PK_AA_L 配大号 pk_text_cjk_glyph
          puts       → 按 ascii_scale 自适应挑档（aa_size_for_cjk）
        错配就会出现"汉字比旁边的字母矮一圈"。
        """
        text = (ROOT / "main" / "text.c").read_text(encoding="utf-8")
        self.assertIn("int pk_text_puts_title", text)
        self.assertIn("int pk_text_puts(", text)
        self.assertNotIn("pk_font_putchar", text)

        title = text[text.index("int pk_text_puts_title"):]
        self.assertIn("aa_putc(fb, fb_w, fb_h, x, y, cp, color, PK_AA_L)", title)
        self.assertIn("pk_text_cjk_glyph(cp)", title)
        # solid=false：CJK 走真灰度混合，不压实成硬阶梯。
        self.assertIn("PK_TEXT_CJK_CELL_H, color, false", title)

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
