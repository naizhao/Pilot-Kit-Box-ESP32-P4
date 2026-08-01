/*
 * map_page.c — SD 离线地图页。north-up、本机居中跟随，PMTiles 栅格底图 +
 * ADS-B 目标叠加。设计依据
 * docs/superpowers/specs/2026-08-01-sd-offline-map-design.md。
 *
 * 数据获取照 traffic_page.c 的 PFD 分支：own_ship 取位置、
 * aircraft_state_snapshot 取目标。绘制原语（pk_pfd_draw_aircraft /
 * pk_aa_puts / pk_pfd_darken_rect …）照抄 traffic_page.c / pfd.c 的用法，
 * 不发明新的绘制抽象层。
 *
 * 与 traffic_page 的关键差异：traffic 是"本机为原点的极坐标"，本页是
 * "地理坐标 → Web Mercator 世界像素 → 屏幕像素"的平移+缩放视口，且整数
 * zoom（0..12，无小数级连续缩放——单指触摸硬件不支持捏合，spec 已把这条
 * 手势砍掉，缩放只有 +/− 两档步进，见 touch_gt911.c 顶部注释）。
 *
 * 渲染顺序严格按 spec：底图 blit（含缺瓦片占位）→ ADS-B 目标 → 本机符号 →
 * 比例尺 + 署名 → 最后是页头/按钮等 UI 铬层。
 */
#include "map_page.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "sdkconfig.h"

#include "display.h"
#include "i18n.h"
#include "pfd_layout.h"
#include "pfd_statusbar.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_font.h"
#include "pfd_icon_font.h"

#include "aircraft_state.h"
#include "own_ship.h"
#include "pk_sdcard.h"
#include "pk_tile_loader.h"
#include "pk_ui_nav.h"

/* ── 视口 / 常量 ─────────────────────────────────────────────────── */
#define MAP_TOP        PFD_BAR_BOT
#define MCX            (PK_DISPLAY_W / 2)
#define MCY            (MAP_TOP + (PK_DISPLAY_H - MAP_TOP) / 2)

#define MAP_ZOOM_MIN   0
#define MAP_ZOOM_MAX   12
#define MAP_ZOOM_DEFAULT 10

/* 底部 UI 铬层：先留出一条署名/比例尺条，按钮贴着它上方，避免互相压。 */
#define FOOTER_H       18
#define FOOTER_Y0      (PK_DISPLAY_H - FOOTER_H)

#define BTN_D          56
#define BTN_HIT_PAD    12
#define BTN_M          16
#define BTN_GAP_ABOVE_FOOTER  8
#define BTN_ZOUT_X     (PK_DISPLAY_W - BTN_M - BTN_D)
#define BTN_ZOUT_Y_DEF (FOOTER_Y0 - BTN_GAP_ABOVE_FOOTER - BTN_D)
#define BTN_ZIN_X      BTN_ZOUT_X
#define BTN_RECENTER_X BTN_M
#define BTN_RECENTER_Y_DEF (FOOTER_Y0 - BTN_GAP_ABOVE_FOOTER - BTN_D)

/* FAB 可拖动，固定坐标躲不开它——每帧按 FAB 当前占位动态避让：竖直方向
 * 与本列按钮堆相交时，把整堆抬到 FAB 上沿之上（顶到 MAP_TOP 为止）。
 * render 与 touch 用同一套结果，保证看见的位置就是点得中的位置。 */
static void btn_layout(int *zin_y, int *zout_y, int *recenter_y)
{
    int zo = BTN_ZOUT_Y_DEF, zi = BTN_ZOUT_Y_DEF - BTN_D - 10, rc = BTN_RECENTER_Y_DEF;
    int fx, fy, fw, fh;
    if (pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh)) {
        const int gap = 10;
        bool right_col = fx + fw > BTN_ZOUT_X - BTN_HIT_PAD;
        bool left_col  = fx < BTN_RECENTER_X + BTN_D + BTN_HIT_PAD;
        if (right_col && fy < zo + BTN_D + gap && fy + fh > zi - gap) {
            zo = fy - gap - BTN_D;            /* 堆底贴 FAB 上沿 */
            zi = zo - BTN_D - 10;
            if (zi < MAP_TOP + gap) {         /* 上方不够就翻到 FAB 下方 */
                zi = fy + fh + gap;
                zo = zi + BTN_D + 10;
            }
        }
        if (left_col && fy < rc + BTN_D + gap && fy + fh > rc - gap) {
            rc = fy - gap - BTN_D;
            if (rc < MAP_TOP + gap) rc = fy + fh + gap;
        }
    }
    if (zin_y) *zin_y = zi;
    if (zout_y) *zout_y = zo;
    if (recenter_y) *recenter_y = rc;
}

#define TILE_PX        256

/* ── 状态（跨帧持久，单渲染任务，无需加锁）───────────────────────── */
static uint8_t  s_zoom        = MAP_ZOOM_DEFAULT;
static double   s_center_lon  = 0.0;
static double   s_center_lat  = 0.0;
static bool     s_follow      = true;    /* true=本机居中跟随；false=手动平移 */
static bool     s_have_last_own = false; /* 是否曾经有过一次有效本机位置 */
static double   s_last_own_lat, s_last_own_lon;

static bool     s_press_active = false;  /* 正在拖动/按下 */
static int      s_press_lx, s_press_ly;  /* 上一帧触点，算增量用 */
static int      s_btn_down = -1;         /* 0=recenter 1=zoom-in 2=zoom-out，-1=无 */

/* ── Web Mercator 世界像素 ↔ 经纬度（度）── */
static void lonlat_to_world(double lon, double lat, uint8_t z, double *wx, double *wy)
{
    if (lat >  85.0511) lat =  85.0511;
    if (lat < -85.0511) lat = -85.0511;
    double n = (double)(1u << z) * (double)TILE_PX;
    double latrad = lat * M_PI / 180.0;
    *wx = (lon + 180.0) / 360.0 * n;
    *wy = (0.5 - log(tan(M_PI / 4.0 + latrad / 2.0)) / (2.0 * M_PI)) * n;
}

static void world_to_lonlat(double wx, double wy, uint8_t z, double *lon, double *lat)
{
    double n = (double)(1u << z) * (double)TILE_PX;
    *lon = wx / n * 360.0 - 180.0;
    double yfrac = wy / n;
    double latrad = atan(sinh(M_PI * (1.0 - 2.0 * yfrac)));
    *lat = latrad * 180.0 / M_PI;
}

/* ── 圆形按钮底（照抄 traffic_page.c 的 draw_btn_plate，本页自成一份：
 * 那份是 static，不跨文件导出，同一套视觉语言直接照抄写法）── */
static void draw_btn_plate(uint16_t *fb, int x, int y, bool down)
{
    const uint16_t face = down ? pk_rgb565( 62,  84, 112) : pk_rgb565( 22,  30,  42);
    const uint16_t edge = down ? pk_rgb565(210, 228, 245) : pk_rgb565(120, 145, 175);
    const int r = BTN_D / 2, cx = x + r, cy = y + r;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            const int px = cx + dx, py = cy + dy;
            if (px < 0 || px >= PK_DISPLAY_W || py < 0 || py >= PK_DISPLAY_H) continue;
            fb[py * PK_DISPLAY_W + px] = (d2 >= (r - 2) * (r - 2)) ? edge : face;
        }
    }
}

static bool hit_btn(int x, int y, int bx, int by)
{
    const int r = BTN_D / 2 + BTN_HIT_PAD;
    const int dx = x - (bx + BTN_D / 2);
    const int dy = y - (by + BTN_D / 2);
    return dx * dx + dy * dy <= r * r;
}

/* ── 缺瓦片占位：深色网格 + z/x/y 小字（FlightMate 同款思路，spec 错误态表）── */
static void draw_missing_tile(uint16_t *fb, int x0, int y0, uint8_t z, uint32_t tx, uint32_t ty)
{
    const uint16_t col_grid = pk_rgb565(40, 46, 56);
    const uint16_t col_bg   = pk_rgb565(18, 21, 26);
    int x1 = x0 + TILE_PX, y1 = y0 + TILE_PX;
    if (x0 < 0) x0 = 0;
    if (y0 < MAP_TOP) y0 = MAP_TOP;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    if (x0 >= x1 || y0 >= y1) return;
    pk_pfd_fill_rect(fb, x0, y0, x1, y1, col_bg);
    for (int gx = x0; gx < x1; gx += 32) pk_pfd_draw_line(fb, gx, y0, gx, y1 - 1, col_grid);
    for (int gy = y0; gy < y1; gy += 32) pk_pfd_draw_line(fb, x0, gy, x1 - 1, gy, col_grid);
    /* z 最大 12(<100)、x/y 在本项目 zoom 范围内最多 4 位十进制，40 字节绰绰
     * 有余——固定宽度上限让 gcc 的 -Wformat-truncation 静态分析满意
     * （它按 uint32_t 的理论最大 10 位数估算，24 字节会被判定"可能截断"）。 */
    char buf[40];
    snprintf(buf, sizeof(buf), "%u/%lu/%lu", (unsigned)z, (unsigned long)tx, (unsigned long)ty);
    if (x1 - x0 > 60 && y1 - y0 > 16)
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x0 + 4, y0 + 4, buf,
                    pk_rgb565(90, 98, 110), 1);
}

/* ── 整页错误态：无 SD / 无 maps 目录 / 无有效包（同一症状，HINT 分因）── */
static void draw_no_data_state(uint16_t *fb, bool sd_mounted)
{
    const uint16_t col_bg    = pk_rgb565(7, 10, 16);
    const uint16_t col_amber = pk_rgb565(255, 176, 0);
    const uint16_t col_hint  = pk_rgb565(170, 182, 200);

    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, col_bg);

    const char *title = pk_i18n_text(PK_TR_MAP_NO_DATA_TITLE);
    const char *hint  = pk_i18n_text(sd_mounted ? PK_TR_MAP_HINT_NO_PACK
                                                : PK_TR_MAP_HINT_NO_CARD);
    const int tw = pk_aa_text_width(title, PK_AA_L);
    const int hw = pk_aa_text_width(hint, PK_AA_S);
    const int cy = MAP_TOP + (PK_DISPLAY_H - MAP_TOP) / 2;

    pk_pfd_darken_rect(fb, PK_DISPLAY_W / 2 - (tw > hw ? tw : hw) / 2 - 16,
                       cy - PK_AA_L_H / 2 - 8,
                       PK_DISPLAY_W / 2 + (tw > hw ? tw : hw) / 2 + 16,
                       cy + PK_AA_L_H / 2 + PK_AA_S_H + 16, 190);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_DISPLAY_W / 2 - tw / 2,
              cy - PK_AA_L_H / 2, title, col_amber, PK_AA_L);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_DISPLAY_W / 2 - hw / 2,
              cy + PK_AA_L_H / 2 + 8, hint, col_hint, PK_AA_S);
}

/* ── 顶栏 / 底部铬层 ── */
static void draw_chrome(uint16_t *fb, double meters_per_px)
{
    /* 顶栏底色：地图瓦片可能画到 y<MAP_TOP（视口边缘取整误差），用一块实底
     * 盖掉，与其它整屏页一致（页头永远是不透明的一条）。 */
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, MAP_TOP, pk_rgb565(7, 10, 16));
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_UI_PAD_L, PK_UI_TITLE_Y,
              pk_i18n_text(PK_TR_MAP_TITLE), PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);
    char zbuf[8];
    snprintf(zbuf, sizeof(zbuf), "Z%u", s_zoom);
    const int zw = pk_aa_text_width(zbuf, PK_UI_TITLE_SIZE);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
              pk_ui_topbar_right_limit(PK_DISPLAY_W - 24) - zw, PK_UI_TITLE_Y,
              zbuf, pk_rgb565(205, 214, 228), PK_UI_TITLE_SIZE);

    /* 底部铬层：署名 + 比例尺，压一层暗底保证在任何底图颜色上可读——
     * FlightMate 把署名画成黑底黑字看不见的教训，这里必须是实际可见色。 */
    pk_pfd_darken_rect(fb, 0, FOOTER_Y0, PK_DISPLAY_W, PK_DISPLAY_H, 170);
    const char *attr = pk_i18n_text(PK_TR_MAP_ATTRIBUTION);
    const int aw = pk_aa_text_width(attr, PK_AA_XS);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_DISPLAY_W - 8 - aw,
              FOOTER_Y0 + (FOOTER_H - PK_AA_XS_H) / 2, attr,
              pk_rgb565(225, 230, 238), PK_AA_XS);

    /* 比例尺：从一组"整数好读"的 NM 值里挑一个像素长度落在 [50,150] 的。 */
    if (meters_per_px > 0.0) {
        static const double kNm[] = { 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000 };
        double nm = kNm[0];
        double bar_px = 0;
        for (size_t i = 0; i < sizeof(kNm) / sizeof(kNm[0]); i++) {
            double px = kNm[i] * 1852.0 / meters_per_px;
            nm = kNm[i];
            bar_px = px;
            if (px >= 50.0) break;
        }
        if (bar_px > 4 && bar_px < PK_DISPLAY_W / 2) {
            int bx0 = 8, by = FOOTER_Y0 + FOOTER_H / 2;
            pk_pfd_fill_rect(fb, bx0, by - 1, bx0 + (int)bar_px, by + 1,
                            pk_rgb565(225, 230, 238));
            char sbuf[16];
            if (nm < 1.0) snprintf(sbuf, sizeof(sbuf), "%.1fNM", nm);
            else          snprintf(sbuf, sizeof(sbuf), "%dNM", (int)nm);
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, bx0 + (int)bar_px + 6,
                        by - 3, sbuf, pk_rgb565(225, 230, 238), 1);
        }
    }

    int zin_y, zout_y, rc_y;
    btn_layout(&zin_y, &zout_y, &rc_y);
    draw_btn_plate(fb, BTN_ZIN_X, zin_y, s_btn_down == 1);
    draw_btn_plate(fb, BTN_ZOUT_X, zout_y, s_btn_down == 2);
    {
        const uint16_t ink = pk_rgb565(225, 235, 248);
        const int cw = pk_aa_cell_w(PK_AA_L);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                  BTN_ZIN_X + (BTN_D - cw) / 2, zin_y + (BTN_D - PK_AA_L_H) / 2,
                  "+", ink, PK_AA_L);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                  BTN_ZOUT_X + (BTN_D - cw) / 2, zout_y + (BTN_D - PK_AA_L_H) / 2,
                  "-", ink, PK_AA_L);
    }

    if (!s_follow) {
        draw_btn_plate(fb, BTN_RECENTER_X, rc_y, s_btn_down == 0);
        const char *lbl = pk_i18n_text(PK_TR_MAP_RECENTER);
        const int lw = pk_aa_text_width(lbl, PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                  BTN_RECENTER_X + (BTN_D - lw) / 2,
                  rc_y + (BTN_D - PK_AA_S_H) / 2,
                  lbl, pk_rgb565(225, 235, 248), PK_AA_S);
    }
}

/* ── 目标标签：简化版防遮挡——按距屏幕中心（约=本机）近→远的顺序占位，
 * 与已占用矩形相撞就只画符号、不画标签。不做 traffic_page 那套 14 槽位/
 * 帧间记忆的完整解法（那套是为雷达上密集扎堆调的），地图上目标本来就按
 * 地理位置分散，这条更轻量的规则已经能保证"标签之间不重叠"这条底线。 ── */
#define MAP_LBL_MAX  AIRCRAFT_TABLE_CAPACITY
typedef struct { int x0, y0, x1, y1; } rect_t;

static bool rect_overlap(const rect_t *a, const rect_t *b)
{
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

/* ── 渲染 ─────────────────────────────────────────────────────────── */
void pk_map_page_render(uint16_t *fb)
{
    const bool sd_mounted = pk_sdcard_is_mounted();
    if (!sd_mounted || pk_tile_loader_pack_count() == 0) {
        draw_no_data_state(fb, sd_mounted);
        return;
    }

    const uint16_t col_bg = pk_rgb565(18, 21, 26);
    pk_pfd_fill_rect(fb, 0, MAP_TOP, PK_DISPLAY_W, PK_DISPLAY_H, col_bg);

    int64_t now_us = esp_timer_get_time();
    uint32_t now_ms = (uint32_t)(now_us / 1000);

    aircraft_t own = {0};
    pk_own_src_t src;
    bool own_valid = pk_own_ship_resolve(
        now_us, (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL, &own, &src);
    if (own_valid) {
        s_have_last_own = true;
        s_last_own_lat = own.lat;
        s_last_own_lon = own.lon;
    }

    if (s_follow && own_valid) {
        s_center_lat = own.lat;
        s_center_lon = own.lon;
    }

    double cwx, cwy;
    lonlat_to_world(s_center_lon, s_center_lat, s_zoom, &cwx, &cwy);

    /* ── 底图：可见范围内的瓦片 blit（含缺瓦片占位）── */
    const int32_t ntiles = (int32_t)1 << s_zoom;
    double world_left  = cwx - MCX;
    double world_right = cwx + (PK_DISPLAY_W - MCX);
    double world_top    = cwy - (MCY - MAP_TOP);
    double world_bottom = cwy + (PK_DISPLAY_H - MCY);
    int32_t tx0 = (int32_t)floor(world_left  / TILE_PX);
    int32_t tx1 = (int32_t)floor(world_right / TILE_PX);
    int32_t ty0 = (int32_t)floor(world_top    / TILE_PX);
    int32_t ty1 = (int32_t)floor(world_bottom / TILE_PX);

    for (int32_t ty = ty0; ty <= ty1; ty++) {
        if (ty < 0 || ty >= ntiles) continue;
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            if (tx < 0 || tx >= ntiles) continue;
            int dst_x0 = MCX + (int)lround((double)tx * TILE_PX - cwx);
            int dst_y0 = MCY + (int)lround((double)ty * TILE_PX - cwy);

            pk_map_route_result_t route;
            bool found = pk_tile_loader_route((uint8_t)s_zoom, (uint32_t)tx, (uint32_t)ty, &route);
            bool blitted = false, neg = false;
            if (found) {
                blitted = pk_tile_loader_try_blit(&route, (uint32_t)tx, (uint32_t)ty,
                                                  fb, dst_x0, dst_y0, now_ms, &neg);
                if (!blitted && !neg) pk_tile_loader_request(&route);
            }
            if (!blitted) {
                /* 真瓦片还没到：先拿缓存里的上级瓦片放大顶上，画面不留空洞；
                 * 一级都找不到才退回网格占位。 */
                if (!pk_tile_loader_try_blit_ancestor((uint8_t)s_zoom, (uint32_t)tx,
                                                      (uint32_t)ty, fb, dst_x0, dst_y0,
                                                      now_ms, 4))
                    draw_missing_tile(fb, dst_x0, dst_y0, (uint8_t)s_zoom,
                                      (uint32_t)tx, (uint32_t)ty);
            }
        }
    }

    /* 越级放大提示：视口中心那格如果是 overzoom，弱化提示一下（spec §交互）。 */
    {
        pk_map_route_result_t center_route;
        int32_t ctx = (int32_t)floor(cwx / TILE_PX), cty = (int32_t)floor(cwy / TILE_PX);
        if (ctx >= 0 && ctx < ntiles && cty >= 0 && cty < ntiles &&
            pk_tile_loader_route((uint8_t)s_zoom, (uint32_t)ctx, (uint32_t)cty, &center_route) &&
            center_route.scale > 1) {
            char buf[24];
            snprintf(buf, sizeof(buf), pk_i18n_text(PK_TR_MAP_OVERZOOM_FMT),
                    (int)center_route.scale);
            const int w = pk_aa_text_width(buf, PK_AA_XS);
            pk_pfd_darken_rect(fb, PK_DISPLAY_W / 2 - w / 2 - 6, MAP_TOP + 4,
                               PK_DISPLAY_W / 2 + w / 2 + 6, MAP_TOP + 4 + PK_AA_XS_H + 6, 150);
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_DISPLAY_W / 2 - w / 2,
                      MAP_TOP + 7, buf, pk_rgb565(255, 176, 0), PK_AA_XS);
        }
    }

    /* ── ADS-B 目标叠加（own_valid 时才有意义算相对位置；无本机位置也照样
     * 能把目标画在地图上——目标的经纬度与本机是否已知无关）── */
    static aircraft_t s_scratch[AIRCRAFT_TABLE_CAPACITY];
    size_t n = aircraft_state_snapshot(s_scratch, AIRCRAFT_TABLE_CAPACITY,
                                       now_us, AIRCRAFT_STALE_AGE_US);
    static rect_t s_occ[MAP_LBL_MAX + 1];
    int nocc = 0;
    const uint16_t col_ac  = pk_rgb565(0, 210, 235);
    const uint16_t col_lbl = pk_rgb565(207, 211, 220);
    for (size_t i = 0; i < n && i < MAP_LBL_MAX; i++) {
        aircraft_t *a = &s_scratch[i];
        if (!a->have_position) continue;
        if (own_valid && own.icao24 != 0 && a->icao24 == own.icao24) continue;
        double wx, wy;
        lonlat_to_world(a->lon, a->lat, s_zoom, &wx, &wy);
        int sx = MCX + (int)lround(wx - cwx);
        int sy = MCY + (int)lround(wy - cwy);
        if (sx < -20 || sx > PK_DISPLAY_W + 20 || sy < MAP_TOP - 20 || sy > PK_DISPLAY_H + 20)
            continue;

        const float rot = a->have_velocity ? (float)a->heading_deg : 0.0f;
        pk_pfd_draw_aircraft(fb, sx, sy, rot, 9, col_ac);

        char cs[10];
        if (a->have_callsign && a->callsign[0]) {
            size_t k = 0;
            for (; k + 1 < sizeof(cs) && a->callsign[k]; k++) cs[k] = a->callsign[k];
            cs[k] = '\0';
            while (k > 0 && cs[k - 1] == ' ') cs[--k] = '\0';
        }
        if (!cs[0]) snprintf(cs, sizeof(cs), "%06lX", (unsigned long)a->icao24);

        const int lw = pk_aa_text_width(cs, PK_AA_XS);
        rect_t r = { sx + 10, sy - PK_AA_XS_H / 2 - 1, sx + 10 + lw + 2, sy + PK_AA_XS_H / 2 + 1 };
        bool clash = false;
        for (int j = 0; j < nocc && !clash; j++) if (rect_overlap(&r, &s_occ[j])) clash = true;
        if (!clash) {
            if (nocc <= MAP_LBL_MAX) s_occ[nocc++] = r;
            pk_pfd_darken_rect(fb, r.x0 - 2, r.y0, r.x1, r.y1, 120);
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, sx + 10, sy - PK_AA_XS_H / 2,
                      cs, col_lbl, PK_AA_XS);
        }
    }

    /* ── 本机符号：跟随模式画在视口中心；手动平移模式画在它真实的地理投影位置
     * （可能滚出视口之外，此时自然不画——离开可见范围本来就不该出现）。
     * GPS 无 fix：灰显于上次已知位置；从未有过位置则整个不画（同 traffic 页的
     * "无本机符号"降级逻辑，画一个我不知道在哪的"我在这"就是说谎）。 ── */
    if (own_valid) {
        int ox, oy;
        if (s_follow) { ox = MCX; oy = MCY; }
        else {
            double owx, owy;
            lonlat_to_world(own.lon, own.lat, s_zoom, &owx, &owy);
            ox = MCX + (int)lround(owx - cwx);
            oy = MCY + (int)lround(owy - cwy);
        }
        if (ox >= 0 && ox < PK_DISPLAY_W && oy >= MAP_TOP && oy < PK_DISPLAY_H) {
            const uint8_t *ac = pk_icon_bitmap
                              + (size_t)PK_ICON_OWNSHIP * (((size_t)PK_ICON_W * PK_ICON_H + 1) / 2);
            pk_aa_blit_4bpp_rot(fb, PK_DISPLAY_W, PK_DISPLAY_H, ox, oy, ac, PK_ICON_W, PK_ICON_H,
                               0.0f, pk_rgb565(255, 255, 255));
        }
    } else if (s_have_last_own) {
        double owx, owy;
        lonlat_to_world(s_last_own_lon, s_last_own_lat, s_zoom, &owx, &owy);
        int ox = MCX + (int)lround(owx - cwx);
        int oy = MCY + (int)lround(owy - cwy);
        if (ox >= 0 && ox < PK_DISPLAY_W && oy >= MAP_TOP && oy < PK_DISPLAY_H) {
            const uint8_t *ac = pk_icon_bitmap
                              + (size_t)PK_ICON_OWNSHIP * (((size_t)PK_ICON_W * PK_ICON_H + 1) / 2);
            pk_aa_blit_4bpp_rot(fb, PK_DISPLAY_W, PK_DISPLAY_H, ox, oy, ac, PK_ICON_W, PK_ICON_H,
                               0.0f, pk_rgb565(140, 148, 158));
        }
    }

    /* ── 比例尺基准：中心纬度的米/像素（Web Mercator 随纬度变形，用当前
     * 中心点的纬度算，跟屏幕中心那一列最准）── */
    double mpp = 156543.03392 * cos(s_center_lat * M_PI / 180.0) / (double)(1u << s_zoom);

    /* ── 拔卡提示留给 pk_tile_loader.c 的 toast（pfd.c 统一叠加），本页
     * 不重复画——两处都画会互相压。 ── */

    draw_chrome(fb, mpp);
}

/* ── 触摸 ─────────────────────────────────────────────────────────── */
bool pk_map_page_touch(int x, int y)
{
    /* FAB 是浮在页面之上的 LVGL 控件，落在它身上的按下必须**不吃**、返回 false
     * 让给 LVGL——touch_gt911 的分发是 `eaten = (mode==MAP && 本函数())` 的短路
     * 契约，本函数以前无论点哪儿都返回 true，等于把 FAB 的点击全吞了（罩哥
     * 2026-08-01：点过缩放后发现 dock FAB 点不动）。
     *
     * 只在"还没拿到这次按压"时让路：已经在拖地图的过程中手指划过 FAB，那次
     * 拖动仍归本页面，不能中途易主。 */
    if (!s_press_active) {
        int fx, fy, fw, fh;
        if (pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh) &&
            x >= fx && x < fx + fw && y >= fy && y < fy + fh)
            return false;
    }

    if (!s_press_active) {
        int zin_y, zout_y, rc_y;
        btn_layout(&zin_y, &zout_y, &rc_y);
        /* 命中按钮同样要把这次按压标记为 active：触摸驱动在手指按住期间会
         * 持续上报，不置位的话每一帧都重新走一遍这里——一次点击涨好几级
         * zoom（罩哥 2026-08-01 实测）。s_btn_down 之后充当"本次按压已归属
         * 某个按钮"的凭据，下面的重复上报据此直接吃掉。 */
        if (hit_btn(x, y, BTN_ZIN_X, zin_y)) {
            s_btn_down = 1;
            if (s_zoom < MAP_ZOOM_MAX) { s_zoom++; pk_tile_loader_bump_view(); }
            s_press_active = true;
            return true;
        }
        if (hit_btn(x, y, BTN_ZOUT_X, zout_y)) {
            s_btn_down = 2;
            if (s_zoom > MAP_ZOOM_MIN) { s_zoom--; pk_tile_loader_bump_view(); }
            s_press_active = true;
            return true;
        }
        if (!s_follow && hit_btn(x, y, BTN_RECENTER_X, rc_y)) {
            s_btn_down = 0;
            s_follow = true;
            pk_tile_loader_bump_view();
            s_press_active = true;
            return true;
        }
        s_press_active = true;
        s_press_lx = x;
        s_press_ly = y;
        return true;
    }

    /* 本次按压归某个按钮所有：一次按压只算一次，手指赖在按钮上也不拖地图。
     * 松手由 pk_map_page_touch_up() 清 s_btn_down/s_press_active。 */
    if (s_btn_down >= 0) return true;

    const int dx = x - s_press_lx;
    const int dy = y - s_press_ly;
    if (dx != 0 || dy != 0) {
        double wx, wy;
        lonlat_to_world(s_center_lon, s_center_lat, s_zoom, &wx, &wy);
        wx -= dx;
        wy -= dy;
        world_to_lonlat(wx, wy, s_zoom, &s_center_lon, &s_center_lat);
        s_follow = false;
        s_press_lx = x;
        s_press_ly = y;
    }
    return true;
}

void pk_map_page_touch_up(void)
{
    s_press_active = false;
    s_btn_down = -1;
}
