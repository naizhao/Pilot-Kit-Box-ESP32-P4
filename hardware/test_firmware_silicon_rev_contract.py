#!/usr/bin/env python3
"""ESP32-P4 硅片代际（rev <3.0 / >=3.0）在固件侧的源级合同。

为什么放在 hardware/ 而不是 firmware/test/
------------------------------------------
和 test_firmware_board_profile_contract.py 同理：这里断言的不是数值，而是
**芯片硬件事实与固件常量之间的一致性**。硅片代际是硬件属性，判据来自 IDF 的
soc/hal 头文件，不是某次构建的产物。

为什么需要这份合同（真实事故，2026-09-30）
------------------------------------------
拿到一块 rev v3.2 的新 P4，按 v3.x 重编后板子起不来，~6 秒一轮重启循环，
日志停在 `NimBLE: GAP procedure initiated: advertise` 之后就 abort——**看起来
完全像 BLE / ESP-Hosted 的问题**，先花时间烧了 C6 从机固件（那一步本身也该做，
但跟这个 abort 无关）。

真实根因在 display.c：

    .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,

这个宏名字像"默认值"，实为 clk_tree_defs.h 里的别名：

    #define MIPI_DSI_PHY_CLK_SRC_DEFAULT \\
            MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY

恒等于 `SOC_MOD_CLK_PLL_F20M`。而 IDF 在同一个枚举上明确标注 PLL_F20M
"only available on esp32p4 version < 3.0"。

IDF 本意是让调用方留 0，由它按编译期代际二选一（esp_lcd_mipi_dsi_bus.c）：

    if (phy_clk_src == 0) {
    #if SOC_IS(ESP32P4) && HAL_CONFIG(CHIP_SUPPORT_MIN_REV) < 300
        phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY;  // PLL_F20M
    #else
        phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT;         // XTAL
    #endif
    }

（`HAL_CONFIG_CHIP_SUPPORT_MIN_REV` 就是 `CONFIG_ESP_REV_MIN_FULL`。）

写死那个宏 = 拿走按代分叉，让 v3.x 芯片也用 v1.x 的 20 MHz PLL 参考时钟。
后果是 `mipi_dsi_host_ll_set_escape_clock_division()` 里的
`HAL_ASSERT(div > 1 && div < 256)`（mipi_dsi_host_ll.h:98）失败并 abort。

为什么这个坑必须用合同守住，而不是靠注释
----------------------------------------
1. 崩溃点（DSI）与日志断点（NimBLE 广播）隔了好几步初始化，指向性极差；
2. 该宏**在 v1.x 上恰好是对的**，所以本地用老板子怎么测都正常，只有换到 v3.x
   硅片才暴露——而那时人已经在排查别的方向了；
3. `MIPI_DSI_PHY_CLK_SRC_DEFAULT` 这个写法看起来比 `0` 更"规范"，
   后来者很容易"整理"回去。

数值/编译期层面的正确性由构建本身保证（v1.x 变体编不过来就说明配置错），
本文件只守「源码里不许写死代际相关的时钟源」这一层。
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISPLAY_C = ROOT / "firmware" / "main" / "display.c"
SDKCONFIG_DEFAULTS = ROOT / "firmware" / "sdkconfig.defaults"
SDKCONFIG_P4REV1 = ROOT / "firmware" / "sdkconfig.defaults.p4rev1"

# 写死即短路 IDF 按代分叉的那些宏。`_LEGACY` 单独列出来，因为它才是真正
# 被解析到的目标值，直接写它也构成同一个错误。
BANNED_DSI_CLK_MACROS = (
    "MIPI_DSI_PHY_CLK_SRC_DEFAULT",
    "MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY",
    "MIPI_DSI_PHY_CLK_SRC_PLL_F20M",
)


def strip_c_comments(src: str) -> str:
    """剥掉 C 注释。

    **必须剥**：解释性注释里天然会提到被禁的宏名（本仓库的 display.c 就是
    这么写的，讲清楚为什么不能用它）。不剥的话守卫会匹配到注释、永远红，
    然后被人当成"误报"关掉——那才是真正的失效。
    """
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    src = re.sub(r"//[^\n]*", "", src)
    return src


class SiliconRevContractTest(unittest.TestCase):
    def test_display_does_not_hardcode_a_revision_specific_dsi_clock_source(self):
        # 反面判据：源码（剥注释后）不许出现任何写死的 DSI PHY 参考时钟源宏。
        code = strip_c_comments(DISPLAY_C.read_text(encoding="utf-8"))
        for macro in BANNED_DSI_CLK_MACROS:
            with self.subTest(macro=macro):
                self.assertNotIn(
                    macro, code,
                    f"display.c 写死了 {macro}，会把 IDF 按硅片代际自动选择 DSI PHY "
                    f"参考时钟源的分叉短路掉（v3.x 上会 abort 在 escape clock 断言）。"
                    f"应该留 0，让 esp_lcd_new_dsi_bus() 自己按代选。",
                )

    def test_display_leaves_the_clock_source_to_idf(self):
        # 正面判据：确实传了 0。只做上面那条 assertNotIn 的话，
        # "整段删掉不建 DSI 总线"也能变绿——这里把目标行为钉死。
        code = strip_c_comments(DISPLAY_C.read_text(encoding="utf-8"))
        self.assertRegex(
            code, r"\.phy_clk_src\s*=\s*0\s*,",
            "display.c 的 DSI bus_config 必须显式 .phy_clk_src = 0，"
            "把参考时钟源交给 IDF 按编译期硅片代际选择。",
        )

    def test_both_silicon_generations_have_a_first_class_build_config(self):
        # 两代各有一条可复现的构建路径：默认 v3.x，v1.x 走变体片段。
        # 缺任何一边，另一边就成了"要自己记得改"的二等公民。
        base = SDKCONFIG_DEFAULTS.read_text(encoding="utf-8")
        self.assertIn("CONFIG_ESP32P4_REV_MIN_301=y", base,
                      "sdkconfig.defaults 必须显式声明默认目标是 v3.x 硅片")
        self.assertNotIn("CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y", base,
                         "默认配置不能是 v1.x 家族；那是 sdkconfig.defaults.p4rev1 的职责")

        self.assertTrue(SDKCONFIG_P4REV1.is_file(),
                        "缺少 v1.x 硅片的构建片段 sdkconfig.defaults.p4rev1")
        v1 = SDKCONFIG_P4REV1.read_text(encoding="utf-8")
        self.assertIn("CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y", v1)
        self.assertIn("CONFIG_ESP32P4_REV_MIN_100=y", v1)
        self.assertNotIn("CONFIG_ESP32P4_REV_MIN_301=y", v1,
                         "v1.x 片段里不能再出现 v3.x 的最低版本，否则两代互斥的选择会打架")


if __name__ == "__main__":
    unittest.main()
