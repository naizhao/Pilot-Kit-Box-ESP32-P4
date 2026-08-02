/* 由 firmware/scripts/gen_lv_font.py 生成，请勿手改。 */
#pragma once

#include <stdint.h>

/* 419 个字符的子集，95088 字节。字符集由 i18n_catalog.py 决定，
 * 新增中文文案后必须重跑生成脚本，否则屏幕上会出现豆腐块。 */
extern const uint8_t pk_lv_font_zh_ttf[];
extern const unsigned pk_lv_font_zh_ttf_size;
