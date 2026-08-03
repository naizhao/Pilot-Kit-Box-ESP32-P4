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

#
# 2026-08-03 的续集：剩下的两个渲染器（pk_text_puts / pk_text_puts_title）也没
# 有调用者了——最后一个是磁力计校准向导，它随 4.3″ 版面改造换到了 pfd_aa_text
# 的 pk_aa_puts。于是 text.c / text.h 连同 L30、M26 两档位图字库
# （text_font_cjk.c 1.0 MB、text_font_cjk_body.c 0.8 MB 源码）整套删除，
# 生成侧对应地不再产出这两档（gen_i18n_assets.py 现在只产词条表）。
# ══════════════════════════════════════════════════════════════════════

# 已删除的整条位图 CJK 渲染链路：text.c 的渲染器 + 它们背后的三档字库。
# 只列符号名，文件名交给下面的 exists() 断言——"text.c" 这种名字是
# "pfd_aa_text.c" 的子串，放进来会把现役字体一起误伤。
REMOVED_UI_FONT_SYMBOLS = (
    "pk_text_puts_ui",
    "pk_text_puts_page_title",
    "pk_text_puts_page_body",
    "pk_text_ui_width",
    "pk_text_cjk_ui_glyph",
    "text_font_cjk_ui",
    "PK_TEXT_CJK_UI_CELL_H",
    # ── 2026-08-03 追加 ──
    "pk_text_puts",          # 同时覆盖 pk_text_puts_title（子串）
    "pk_text_width",
    "pk_text_title_width",
    "pk_text_cjk_glyph",     # 同时覆盖 pk_text_cjk_body_glyph
    "PK_TEXT_CJK_CELL_H",
    "PK_TEXT_CJK_BODY_CELL_H",
)

# 已删除的源文件。CMakeLists 里的构建条目单独断言（见下），那边不能只搜
# 文件名——注释里要写清"为什么删"，搜名字会把解释历史的注释也禁掉。
REMOVED_FONT_FILES = (
    "text.c",
    "text.h",
    "text_font_cjk.c",
    "text_font_cjk.h",
    "text_font_cjk_body.c",
    "text_font_cjk_body.h",
    "text_font_cjk_ui.c",
    "text_font_cjk_ui.h",
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

    def test_bitmap_cjk_font_stays_deleted(self) -> None:
        """位图 CJK 字库与它的渲染器不许回到任何固件源码或构建里。"""
        main_dir = ROOT / "main"
        for name in REMOVED_FONT_FILES:
            with self.subTest(file=name):
                self.assertFalse((main_dir / name).exists(), f"{name} 又回来了")

        for path in sorted(main_dir.glob("*.[ch]")):
            body = code_only(path.read_text(encoding="utf-8"))
            for sym in REMOVED_UI_FONT_SYMBOLS:
                with self.subTest(file=path.name, symbol=sym):
                    self.assertNotIn(sym, body)

        # 构建条目：固件侧是带引号的裸文件名，模拟器侧是 ${FW}/ 前缀。两边都
        # 只匹配"条目"的形状，注释里出现文件名不算——那是在解释为什么删。
        fw_cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
        sim_cmake = (ROOT.parent / "sim" / "CMakeLists.txt").read_text(encoding="utf-8")
        for name in REMOVED_FONT_FILES:
            if not name.endswith(".c"):
                continue
            with self.subTest(entry=name):
                self.assertNotIn(f'"{name}"', fw_cmake)
                self.assertNotIn(f"${{FW}}/{name}", sim_cmake)

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

    def test_cal_wizard_renders_through_aa_text(self) -> None:
        """校准向导是 text.c 的最后一个调用者，它必须留在 pfd_aa_text 这边。

        原来那三个断言（CJK_SOLID_ALPHA4_THRESHOLD 阈值、alpha 线性展开、
        puts_title 的档位配对）测的都是 text.c 的内部实现，随文件一起删。
        真正要守住的是**这一页不许退回去**：一旦它重新 include text.h，
        整套 1.8 MB 字库就会被链接回来。
        """
        body = (ROOT / "main" / "cal_wizard.c").read_text(encoding="utf-8")
        self.assertIn("void pk_cal_wizard_render(uint16_t *fb)", body)
        self.assertIn("pk_aa_puts", body)
        self.assertIn('#include "pfd_aa_text.h"', body)
        self.assertNotIn('#include "text.h"', body)


if __name__ == "__main__":
    unittest.main()
