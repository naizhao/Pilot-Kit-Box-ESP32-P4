/*
 * pfd_aa_text.h — 抗锯齿文本渲染（TTF 派生字形表）。
 *
 * 与 pfd_font.h 的分工
 * --------------------
 * pfd_font.h 提供两种历史渲染器：
 *   - 5×7 位图 + 整数倍缩放：小屏原始尺寸下锐利，但放大后是方块像素；
 *   - cockpit 12×16 手绘子集：1-bit，同样只有一档、不可缩放。
 * 二者在 320×240 上工作良好，在 800×480 / 217 PPI 上都不可用。
 *
 * 本模块消费 gen_pfd_aa_font.py 生成的字形表：每档字号从 TTF 单独
 * 渲染，保有真正的灰度边缘，因此放大不糊。拉丁取 B612 Mono（Airbus
 * 与 ENAC 为航空座舱开发，等宽，数值跳变时字符不抖动），中日文经
 * fallback 链取 Noto Sans SC。
 *
 * 字重
 * ----
 * 生成时即产出 regular / bold 两套，运行时由设置项切换（见
 * pk_aa_set_weight）。半反半透屏反射态对比度低，加粗档为其预留。
 */
#pragma once

#include <stdint.h>

#include "pfd_aa_font.h"

typedef enum {
    PK_AA_S = 0,    /* 21 px ≈ 2.5 mm — 状态栏 / 标签 / 单位 */
    PK_AA_M,        /* 28 px ≈ 3.0 mm — 正文主力            */
    PK_AA_XL,       /* 43 px ≈ 5.0 mm — PFD 当前值大数字     */
    PK_AA_SIZE_COUNT
} pk_aa_size_t;

typedef enum {
    PK_AA_REGULAR = 0,
    PK_AA_BOLD,
} pk_aa_weight_t;

/* 全局字重。设置页写入，渲染时读取；切换后下一帧生效。 */
void           pk_aa_set_weight(pk_aa_weight_t w);
pk_aa_weight_t pk_aa_get_weight(void);

/* 单个字形的 cell 尺寸 —— 定宽，布局计算直接乘字符数即可。 */
int pk_aa_cell_w(pk_aa_size_t size);
int pk_aa_cell_h(pk_aa_size_t size);

/*
 * 绘制一个 ASCII 字符串（含 0x7F 度数符号，约定同 pfd_font.h）。
 * 字形以 4bpp 灰度与背景做 alpha 混合，故边缘平滑。
 * 返回推进的像素宽度，便于右对齐布局。
 */
int pk_aa_puts(uint16_t *fb, int fb_w, int fb_h,
               int x, int y, const char *s,
               uint16_t color, pk_aa_size_t size);

/*
 * 通用 4bpp 灰度位图混合绘制。文字与状态栏图标共用同一条路径，
 * 保证二者的边缘处理完全一致（同样的 alpha 曲线、同样的大端换序）。
 */
void pk_aa_blit_4bpp(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                     const uint8_t *bitmap, int w, int h, uint16_t color);
