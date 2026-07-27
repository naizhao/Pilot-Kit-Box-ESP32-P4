/*
 * pfd_hsi_traffic.c — HSI 半圆外圈叠加前方 traffic。
 *
 * 复用 pfd_hsi.c 的半圆几何（虚拟圆心在屏下方 (160,240)，R=65，只见上半
 * 弧）。交通目标画在 R+14 的外圈：相对方位 rel 投到 rose_deg = 90 - rel，
 * 只画 |rel| ≤ 95（前方），后方计数显示在左下。几何用 pk_traffic_rel_calc
 * （磁北系，含磁偏角），与雷达页一致。
 */
#include "pfd_hsi_traffic.h"

#include <math.h>
#include <stdio.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "display.h"
#include "pfd_layout.h"
#include "pfd_draw.h"
#include "pfd_aa_text.h"
#include "pfd_font.h"

#include "aircraft_state.h"
#include "own_ship.h"
#include "imu_task.h"
#include "baro.h"
#include "traffic_geom.h"
#include "mag_var.h"

/* 与 pfd_hsi.c 共用同一套半圆几何。此前这里自带一份 160/240/65 的硬编码
 * 副本（320 的值），换屏后与罗盘对不上——几何只能有一个来源。 */
#define HSI_CX          PFD_HSI_CX
#define HSI_CY          PFD_HSI_CY
#define HSI_R           PFD_HSI_R
#define HSI_TRAFFIC_R   PFD_HSI_TRAFFIC_R

/* 与 pfd_hsi.c 同一套等比缩放：菱形与标签跟着罗盘半径走，320 档比例为 1。 */
#define ROSE_SC(v)      ((v) * HSI_R / 65)

#if PK_DISPLAY_W >= 800
#  define TGT_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_S)
#  define TGT_LBL_H     PK_AA_S_H
#else
#  define TGT_PUTS(fb, x, y, s, col) \
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), 1)
#  define TGT_LBL_H     6
#endif

static EXT_RAM_BSS_ATTR aircraft_t s_scratch[AIRCRAFT_TABLE_CAPACITY];

static int std_alt_ft_from_pa(float pa)
{
    float alt_m = 44330.0f * (1.0f - powf(pa / 101325.0f, 0.190295f));
    return (int)lroundf(alt_m * 3.28084f);
}

static void fill_diamond(uint16_t *fb, int x, int y, int s, uint16_t c)
{
    pk_pfd_draw_triangle(fb, x, y - s, x - s, y, x + s, y, c);
    pk_pfd_draw_triangle(fb, x - s, y, x + s, y, x, y + s, c);
}

void pk_pfd_hsi_traffic_render(uint16_t *fb)
{
    int64_t now_us = esp_timer_get_time();

    pk_imu_sample_t s;
    if (!pk_imu_sample_get(&s)) return;     /* 无航向无法定相对方位 */
    float yaw = s.yaw_deg;

    aircraft_t own = {0};
    pk_own_src_t src;
    if (!pk_own_ship_resolve(now_us, (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL,
                             &own, &src))
        return;                              /* 无本机位置 */

    pk_baro_state_t baro;
    bool baro_ok = pk_baro_get(&baro);
    /* 相对高度的本机基准:绑定 own 时优先用其 ADS-B 气压高度(与目标 Mode-C 同
     * 1013.25 基准),否则用 baro 标准气压高度兜底。照 traffic_page.c:273-280
     * 同一逻辑——原来恒用 baro,绑定高空 own 时相对高度符号会全错。 */
    int own_palt;
    if (own.have_altitude)          own_palt = own.altitude_ft;
    else if (baro_ok && baro.valid) own_palt = std_alt_ft_from_pa(baro.pressure_pa);
    else                            own_palt = PK_ALT_UNAVAIL;
    float mag_var = pk_mag_var_lookup(own.lat, own.lon);

    size_t n = aircraft_state_snapshot(
        s_scratch, AIRCRAFT_TABLE_CAPACITY, now_us, AIRCRAFT_STALE_AGE_US);

    const uint16_t COL_TGT    = pk_rgb565(  0, 210, 235);
    const uint16_t COL_LBL    = pk_rgb565(207, 211, 220);
    /* 后方计数落在褐色地面上，原来的暗褐色几乎与背景同色——提高亮度并偏
     * 琥珀，保证在天与地两种背景上都能认出来。 */
    const uint16_t COL_BEHIND = pk_rgb565(240, 180,  90);

    int behind = 0;
    for (size_t i = 0; i < n; i++) {
        aircraft_t *t = &s_scratch[i];
        if (own.icao24 != 0 && t->icao24 == own.icao24) continue;

        pk_traffic_rel_t rel = pk_traffic_rel_calc(
            true, own.lat, own.lon, yaw, mag_var, own_palt,
            t->have_position, t->lat, t->lon,
            t->have_altitude, t->altitude_ft, t->vert_rate_fpm);
        if (!rel.valid) continue;

        float r = rel.rel_bearing;
        /* 外圈 R+14=79、半圆心在屏底 (160,240)：|rel| 越大目标越贴屏幕下边，
         * |rel|>~86.4°(=arccos(5/79)) 时菱形(size 4)会落到 y≥240 被 clip 静默
         * 消失。向内收到 85° 保证完整可见；85~95° 计入后方计数。 */
        if (r < -85.0f || r > 85.0f) { behind++; continue; }

        float rad = (90.0f - r) * (float)M_PI / 180.0f;
        int tx = HSI_CX + (int)lroundf(HSI_TRAFFIC_R * cosf(rad));
        int ty = HSI_CY - (int)lroundf(HSI_TRAFFIC_R * sinf(rad));
        fill_diamond(fb, tx, ty, ROSE_SC(4), COL_TGT);

        if (rel.rel_alt_valid) {
            int hh = rel.rel_alt_ft / 100;
            if (hh >  99) hh =  99;
            if (hh < -99) hh = -99;
            char b[12];
            snprintf(b, sizeof(b), "%+03d", hh);
            TGT_PUTS(fb, tx + ROSE_SC(6), ty - TGT_LBL_H / 2, b, COL_LBL);
        }
    }

    if (behind > 0) {
        char b[16];
        snprintf(b, sizeof(b), "%c%d", PK_FONT_ARROW_S, behind);  /* ▼N 后方 */
        /* 放右下角 VS 框(y≤228)下方的空隙：避开左下 own-ship badge
         * (x[0,78] y[210,232])，否则会被 pfd.c 后画的 badge darken+文字覆盖
         * 导致永久不可见。 */
        /* 落在左下角、本机来源徽标的右侧。原先放右下，那里现在是三行信息框，
         * 会被后画的它们整个盖掉——不可见的告警等于没有。 */
        TGT_PUTS(fb, PFD_BADGE_W + 10, PK_DISPLAY_H - TGT_LBL_H - 4,
                 b, COL_BEHIND);
    }
}
