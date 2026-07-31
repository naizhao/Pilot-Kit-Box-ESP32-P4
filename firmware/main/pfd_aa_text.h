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
 * 只有一档。2026-07-30 去掉了 bold：它当初是给半反半透屏的反射态预留的，
 * 而那块屏没上，设置页里从来没做出对应开关，pk_aa_set_weight() 全仓零调用
 * 者——字重恒为 regular。可 bold 那 9 张字形表被下面的静态字体表静态引用，
 * 链接期照单全收，实测占 app 分区约 347 KB。需要时重新加一档即可。
 */
#pragma once

#include <stdint.h>

#include "pfd_aa_font.h"

typedef enum {
    /* 以 M（18 px）为 normal，上下各展两档。
     *
     * XS/S 是**降级档**：容不下长值时降到它们显示，而不是把内容截断。两档都
     * 覆盖到 0x7F——没有字母就承载不了真实文本。
     *
     * XL 只覆盖 0x20..0x3F（数字与符号），它只服务 PFD 的高度/速度大数字。 */
    PK_AA_XS = 0,   /* 12 px — 极密集：列表次要列、角标 */
    PK_AA_S,        /* 14 px — 次要；长文本降级承载      */
    PK_AA_M,        /* 18 px — **正文主力（normal）**     */
    PK_AA_L,        /* 26 px — 页面标题                  */
    PK_AA_XL,       /* 43 px — PFD 当前值大数字          */
    PK_AA_SIZE_COUNT
} pk_aa_size_t;

/* 单个字形的 cell 尺寸 —— 定宽，布局计算直接乘字符数即可。 */
int pk_aa_cell_w(pk_aa_size_t size);

/*
 * 文本显示宽度(px)。**排版一律用它，不要用 strlen × cell_w**——后者数的是
 * 字节，一个汉字 3 字节却只画一个字形，而且 CJK 字形比拉丁宽。
 * 推进规则与 pk_aa_puts 同源。
 */
int pk_aa_text_width(const char *s, pk_aa_size_t size);
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

/*
 * 同上，但把位图**绕自身中心旋转** deg 度后画到 (cx, cy)——(cx, cy) 落在
 * 旋转后图形的中心，不是左上角。deg 顺时针为正，与航向角一致。
 *
 * 有了它，Material Symbols 里那些本来就有方向的字形（flight、near_me…）
 * 可以直接当可旋转符号使，不必再为「同一个东西但要转」手绘第二套几何。
 */
void pk_aa_blit_4bpp_rot(uint16_t *fb, int fb_w, int fb_h, int cx, int cy,
                         const uint8_t *bitmap, int w, int h,
                         float deg, uint16_t color);
