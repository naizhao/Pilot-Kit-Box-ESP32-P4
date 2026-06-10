/*
 * traffic_page.c — 360° 交通雷达页。
 *
 * 数据获取照 pfd.c 的 PFD 分支：own_ship 取位置、IMU yaw 取机头磁航向、
 * baro 算标准气压高度、aircraft_state_snapshot 取目标。几何用纯函数
 * pk_traffic_rel_calc（全程磁北系，含磁偏角修正）。绘制用 pfd_draw /
 * pfd_font 原语。像素布局参照原型 traffic_radar_interactive.html。
 *
 * 朝向：
 *   HEADING-UP — 屏幕上方=机头，目标用 rel_bearing 投影，罗盘随 yaw 转。
 *   NORTH-UP   — 屏幕上方=磁北，目标用 abs_bearing 投影（本机三角随 yaw
 *                旋转标记朝向：Task8 完善，本版三角暂朝上）。
 */
#include "traffic_page.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "display.h"
#include "pfd_draw.h"
#include "pfd_font.h"

#include "aircraft_state.h"
#include "own_ship.h"
#include "imu_task.h"
#include "baro.h"

#include "config_traffic.h"
#include "traffic_geom.h"
#include "mag_var.h"

/* ── 布局（320×240）─────────────────────────────────────────── */
#define CX     160
#define CY     126
#define RMAX    94

/* 目标快照缓冲——放 PSRAM，避免吃任务栈（照 pfd.c 的 scratch）。 */
static EXT_RAM_BSS_ATTR aircraft_t s_scratch[AIRCRAFT_TABLE_CAPACITY];

/* 每档量程的距离环刻度（0 = 不画） */
static const int RINGS[4][3] = {
    {1, 2, 0},   /* 2  NM */
    {1, 3, 5},   /* 5  NM */
    {2, 5, 10},  /* 10 NM */
    {5, 10, 20}, /* 20 NM */
};

/* 极坐标 → 像素：screen_deg 0=正上(机头/北)，顺时针为正，90=右。 */
static void polar(float screen_deg, float r, int *ox, int *oy)
{
    float a = (screen_deg - 90.0f) * (float)M_PI / 180.0f;
    *ox = CX + (int)lroundf(r * cosf(a));
    *oy = CY + (int)lroundf(r * sinf(a));
}

static void fill_diamond(uint16_t *fb, int x, int y, int s, uint16_t c)
{
    pk_pfd_draw_triangle(fb, x, y - s, x - s, y, x + s, y, c);
    pk_pfd_draw_triangle(fb, x - s, y, x + s, y, x, y + s, c);
}

/* 气压 → 1013.25 标准气压高度(ft)，与目标 Mode-C 同基准。 */
static int std_alt_ft_from_pa(float pa)
{
    float alt_m = 44330.0f * (1.0f - powf(pa / 101325.0f, 0.190295f));
    return (int)lroundf(alt_m * 3.28084f);
}

void pk_traffic_page_render(uint16_t *fb)
{
    const uint16_t COL_BG    = pk_rgb565(  7,  10,  16);
    const uint16_t COL_RING  = pk_rgb565( 34,  38,  46);
    const uint16_t COL_RINGL = pk_rgb565( 90,  94, 102);
    const uint16_t COL_CARD  = pk_rgb565(150, 150, 160);
    const uint16_t COL_N     = pk_rgb565(207, 211, 220);
    const uint16_t COL_OWN   = pk_rgb565(255, 255, 255);
    const uint16_t COL_TGT   = pk_rgb565(  0, 210, 235);
    const uint16_t COL_LBL   = pk_rgb565(207, 211, 220);
    const uint16_t COL_HDR   = pk_rgb565(235, 235, 235);
    const uint16_t COL_CYAN  = pk_rgb565(  0, 210, 235);
    const uint16_t COL_GREY  = pk_rgb565(120, 120, 128);
    const uint16_t COL_AMBER = pk_rgb565(255, 176,   0);

    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* ── 取数据（照 pfd.c PFD 分支）── */
    int64_t now_us = esp_timer_get_time();

    pk_imu_sample_t s;
    bool have = pk_imu_sample_get(&s);
    float yaw = have ? s.yaw_deg : 0.0f;     /* 本机机头磁航向 */

    aircraft_t own = {0};
    pk_own_src_t src;
    bool own_valid = pk_own_ship_resolve(
        now_us, (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL, &own, &src);

    pk_baro_state_t baro;
    bool baro_ok = pk_baro_get(&baro);
    int own_palt = (baro_ok && baro.valid)
                       ? std_alt_ft_from_pa(baro.pressure_pa) : PK_ALT_UNAVAIL;

    float mag_var = own_valid ? pk_mag_var_lookup(own.lat, own.lon) : 0.0f;

    pk_map_orient_t orient = pk_map_orient_get();
    int range_idx = pk_traffic_range_idx_get();
    int range_nm  = pk_traffic_range_nm(range_idx);

    size_t n = aircraft_state_snapshot(
        s_scratch, AIRCRAFT_TABLE_CAPACITY, now_us, AIRCRAFT_STALE_AGE_US);

    /* ── 顶栏 ── */
    char buf[24];
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 4, 3, "TRAFFIC", COL_HDR, 1);
    if (have) {
        snprintf(buf, sizeof(buf), "HDG %03d", ((int)lroundf(yaw) + 360) % 360);
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 120, 3, buf, COL_CYAN, 1);
    } else {
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 120, 3, "HDG ---", COL_AMBER, 1);
    }
    snprintf(buf, sizeof(buf), "TFC %d", (int)n);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 206, 3, buf, COL_GREY, 1);
    snprintf(buf, sizeof(buf), "%dNM", range_nm);
    {
        int w = (int)strlen(buf) * 6;
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     PK_DISPLAY_W - 4 - w, 3, buf, COL_GREY, 1);
    }

    /* ── 距离环 ── */
    for (int k = 0; k < 3; k++) {
        int nm = RINGS[range_idx][k];
        if (nm <= 0 || nm > range_nm) continue;
        float r = (float)nm / range_nm * RMAX;
        pk_pfd_draw_arc_aa(fb, CX, CY, r, 0.0f, 360.0f, 1.0f, COL_RING);
        snprintf(buf, sizeof(buf), "%d", nm);
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     CX + 2, CY - (int)r - 7, buf, COL_RINGL, 1);
    }

    /* ── 罗盘刻度 + 主向字母（磁北系）── */
    for (int d = 0; d < 360; d += 30) {
        float screen = (orient == PK_MAP_HEADING_UP) ? (float)d - yaw : (float)d;
        int x1, y1, x2, y2;
        polar(screen, RMAX, &x1, &y1);
        polar(screen, RMAX - (d % 90 == 0 ? 8 : 5), &x2, &y2);
        pk_pfd_draw_line(fb, x1, y1, x2, y2, COL_RING);
    }
    {
        const char *cards[4] = { "N", "E", "S", "W" };
        for (int i = 0; i < 4; i++) {
            int d = i * 90;
            float screen = (orient == PK_MAP_HEADING_UP) ? (float)d - yaw : (float)d;
            int lx, ly;
            polar(screen, RMAX - 16, &lx, &ly);
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         lx - 2, ly - 3, cards[i], i == 0 ? COL_N : COL_CARD, 1);
        }
    }

    /* ── 目标 ── */
    if (!own_valid) {
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     CX - 30, CY - 4, "NO OWN POS", COL_AMBER, 1);
    } else {
        for (size_t i = 0; i < n; i++) {
            aircraft_t *t = &s_scratch[i];
            if (own.icao24 != 0 && t->icao24 == own.icao24) continue;  /* 不画自己 */

            pk_traffic_rel_t rel = pk_traffic_rel_calc(
                true, own.lat, own.lon, yaw, mag_var, own_palt,
                t->have_position, t->lat, t->lon,
                t->have_altitude, t->altitude_ft, t->vert_rate_fpm);
            if (!rel.valid) continue;
            if (rel.dist_nm > (float)range_nm) continue;   /* 量程外不画 */

            float screen = (orient == PK_MAP_HEADING_UP)
                               ? rel.rel_bearing : rel.abs_bearing;
            int tx, ty;
            polar(screen, rel.dist_nm / range_nm * RMAX, &tx, &ty);
            fill_diamond(fb, tx, ty, 4, COL_TGT);

            /* 相对高度(百ft,±) + 升降箭头 */
            char alt[12];
            if (rel.rel_alt_valid) {
                int hh = rel.rel_alt_ft / 100;
                if (hh >  99) hh =  99;
                if (hh < -99) hh = -99;
                snprintf(alt, sizeof(alt), "%+03d", hh);
            } else {
                snprintf(alt, sizeof(alt), "?");
            }
            char arrow = (rel.vs_fpm > 50) ? '^' : (rel.vs_fpm < -50) ? 'v' : ' ';
            snprintf(buf, sizeof(buf), "%s%c", alt, arrow);
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, tx + 6, ty - 3, buf, COL_LBL, 1);
        }
    }

    /* ── 本机三角（机头朝上；NORTH-UP 旋转留 Task8）── */
    pk_pfd_draw_triangle(fb, CX, CY - 7, CX - 6, CY + 6, CX + 6, CY + 6, COL_OWN);
}
