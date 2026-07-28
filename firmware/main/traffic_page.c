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
#include "pfd_layout.h"
#include "pfd_aa_text.h"
#include "pfd_aa_font.h"
#include "pfd_icon_font.h"
#include "pfd_draw.h"
#include "pfd_font.h"

#include "aircraft_state.h"
#include "own_ship.h"
#include "imu_task.h"
#include "baro.h"

#include "config_traffic.h"
#include "traffic_geom.h"
#include "mag_var.h"
#include "aircraft_db.h"
#include "ui_state.h"

/* ── 布局（800×480，spec §5.2）───────────────────────────────
 *
 *   0            520                800
 *   ├─────────────┼──────────────────┤
 *   │   雷达区     │  右栏 4 张卡片    │
 *   │  本机居中     │  方位/呼号/距离   │
 *   │             │  /高度/速度       │
 *
 * 半径受**高度**限制而不是宽度：顶栏之下只剩 432 px，而左栏有 520 px 宽。
 * 取 RMAX 200，上下各留 16 px 呼吸；水平仍居中在 520 的中线上。 */
#define TFC_RADAR_W   520
#define TFC_SIDE_X    TFC_RADAR_W          /* 右栏左边界 */
#define TFC_TOP       PFD_BAR_BOT          /* 顶栏与 PFD 等高 */

#define CX     (TFC_RADAR_W / 2)
#define CY     (TFC_TOP + (PK_DISPLAY_H - TFC_TOP) / 2)
#define RMAX   200

/* 文字统一走抗锯齿字体：normal 档做正文，XS 做卡片里的次要列。
 * 原先满页 5×7 位图，在 217 PPI 上既小又糊，与已改好的其余页面也不是一套。 */
#define TFC_PUTS(fb, x, y, s, col) \
        pk_aa_puts((fb), PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_M)
#define TFC_PUTS_XS(fb, x, y, s, col) \
        pk_aa_puts((fb), PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_XS)
#define TFC_HDR_TY    ((PFD_BAR_BOT - PK_AA_M_H) / 2)

/* ── 右栏（spec §5.2：280 px，4 张卡片可滚动）───────────────── */
#define SIDE_X        TFC_SIDE_X
#define SIDE_W        (PK_DISPLAY_W - SIDE_X)
#define SIDE_PAD      12
/* 行高与内边距直接取 PFD 信息框的值（PFD_IB_ROW_H/_GAP/_PAD）。
 *
 * 一开始自己造了「卡片」：一张 76~97 px、三行内容、带边框。做出来又大又
 * 看不懂。PFD 右下角那三行（B / ALT / VS）早就把这类信息排明白了——一行一
 * 条、半透明底、左标签右数值，没有边框。同一台设备上不该有两套列表语言。 */
#define ROW_H         PFD_IB_ROW_H
#define ROW_GAP       PFD_IB_ROW_GAP
#define ROW_PAD       PFD_IB_PAD
#define ROW_N         8            /* 280 px 宽 × 30 px 行，装得下 8 条 */

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

/* 把点绕盘心顺时针旋转 deg 度（屏幕 y 向下，正角=顺时针）。 */
static void rot_point(int px, int py, float deg, int *ox, int *oy)
{
    float a  = deg * (float)M_PI / 180.0f;
    float dx = (float)(px - CX), dy = (float)(py - CY);
    *ox = CX + (int)lroundf(dx * cosf(a) - dy * sinf(a));
    *oy = CY + (int)lroundf(dx * sinf(a) + dy * cosf(a));
}

/* 本机飞机符号(机身 + 主翼 + 平尾)，机头朝上；rot_deg 绕盘心旋转
 * (NORTH-UP 时按磁航向标朝向)。和交互原型 / HSI 的飞机图标一致。 */
/*
 * 本机符号。
 *
 * heading-up 时机头恒指屏幕上方，直接用 PFD 罗盘那枚 Material Symbols 的
 * flight 字形——两处是同一架飞机，形态该一致；字形是专业设计过的轮廓，比
 * 手拼矩形干净得多（pfd_hsi.c 里有同样的说明）。
 *
 * north-up 时符号要按航向转任意角度，而字形表是预渲染的、转不了，只能退回
 * 手绘。这也是 PFD 罗盘能一直用图标的原因：那里恒 heading-up。
 */
static void draw_own_aircraft(uint16_t *fb, float rot_deg, uint16_t col)
{
    if (fabsf(rot_deg) < 0.5f) {
        const uint8_t *ac = pk_icon_bitmap[pk_aa_get_weight()]
                          + (size_t)PK_ICON_OWNSHIP
                            * (((size_t)PK_ICON_W * PK_ICON_H + 1) / 2);
        pk_aa_blit_4bpp(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                        CX - PK_ICON_W / 2, CY - PK_ICON_H / 2,
                        ac, PK_ICON_W, PK_ICON_H, col);
        return;
    }

    /* 需要旋转：手绘机身 + 机翼 + 尾翼。 */
    const int nose = 18, tail = 12, wing = 15, tailw = 7;
    int x1, y1, x2, y2;
    rot_point(0, -nose, rot_deg, &x1, &y1);
    rot_point(0,  tail, rot_deg, &x2, &y2);
    pk_pfd_draw_line_aa(fb, (float)x1, (float)y1, (float)x2, (float)y2, 3.0f, col);
    rot_point(-wing, 0, rot_deg, &x1, &y1);
    rot_point( wing, 0, rot_deg, &x2, &y2);
    pk_pfd_draw_line_aa(fb, (float)x1, (float)y1, (float)x2, (float)y2, 3.0f, col);
    rot_point(-tailw, tail - 3, rot_deg, &x1, &y1);
    rot_point( tailw, tail - 3, rot_deg, &x2, &y2);
    pk_pfd_draw_line_aa(fb, (float)x1, (float)y1, (float)x2, (float)y2, 2.5f, col);
}

static void draw_hsi_sector(uint16_t *fb, pk_map_orient_t orient,
                            float own_heading, bool hdg_valid)
{
    if (orient != PK_MAP_HEADING_UP && !hdg_valid) return;
    float center = (orient == PK_MAP_HEADING_UP) ? 0.0f : own_heading;
    float back = (center + 180.0f) * (float)M_PI / 180.0f;
    float bx = sinf(back), by = -cosf(back);       /* 正后方向量(screen→屏幕) */
    const uint16_t fov = pk_rgb565(45, 75, 100);
    const float R2 = (float)RMAX * (float)RMAX;
    for (int y = CY - RMAX; y <= CY + RMAX; y++) {
        for (int x = CX - RMAX; x <= CX + RMAX; x++) {
            float dx = (float)(x - CX), dy = (float)(y - CY);
            float r2 = dx * dx + dy * dy;
            if (r2 > R2 || r2 < 1.0f) continue;
            float s = dx * bx + dy * by;            /* 投影到正后方 */
            if (s > 0.0f && s * s > 0.0076f * r2) continue;  /* 后方 ±85° 锥,排除 */
            pk_pfd_blend_pixel(fb, x, y, fov, 75);  /* 前方 190° 扇区,半透明青 */
        }
    }
}

/* 一个可显示目标：指向本帧 snapshot 的飞机 + 算好的相对几何。 */
typedef struct {
    aircraft_t       *ac;
    pk_traffic_rel_t  rel;
} vis_t;

/* 呼号(去尾空格)；无呼号回退 ICAO 十六进制。 */
static void callsign_of(const aircraft_t *a, char *out, size_t cap)
{
    if (a->have_callsign && a->callsign[0]) {
        size_t i = 0;
        for (; i + 1 < cap && a->callsign[i]; i++) out[i] = a->callsign[i];
        out[i] = '\0';
        while (i > 0 && out[i - 1] == ' ') out[--i] = '\0';
        if (out[0]) return;
    }
    snprintf(out, cap, "%06lX", (unsigned long)a->icao24);
}

static void draw_target(uint16_t *fb, const vis_t *v, pk_map_orient_t orient,
                        int range_nm, bool selected)
{
    const pk_traffic_rel_t *rel = &v->rel;
    float screen = (orient == PK_MAP_HEADING_UP) ? rel->rel_bearing : rel->abs_bearing;
    int tx, ty;
    polar(screen, rel->dist_nm / range_nm * RMAX, &tx, &ty);

    /* 选中用黄色高亮(区别于本机的白色,避免混淆);未选中青色。 */
    uint16_t cyan   = pk_rgb565(  0, 210, 235);
    uint16_t yellow = pk_rgb565(255, 210,  60);
    uint16_t lbl    = pk_rgb565(207, 211, 220);
    uint16_t col    = selected ? yellow : cyan;
    uint16_t txtcol = selected ? yellow : lbl;
    fill_diamond(fb, tx, ty, selected ? 6 : 4, col);

    /* 相对高度(百ft,最多 3 位) + 上/下箭头。箭头基于"相对高度方向"
     * (目标在本机上方=^ / 下方=v / 同高=-),不是升降率。 */
    char alt[12];
    if (rel->rel_alt_valid) {
        int hh = rel->rel_alt_ft / 100;
        if (hh >  999) hh =  999;
        if (hh < -999) hh = -999;
        snprintf(alt, sizeof(alt), "%+d", hh);
    } else {
        snprintf(alt, sizeof(alt), "?");
    }
    char arrow = !rel->rel_alt_valid ? ' '
               : rel->rel_alt_ft > 0 ? '^'
               : rel->rel_alt_ft < 0 ? 'v' : '-';
    char lab[16];
    snprintf(lab, sizeof(lab), "%s%c", alt, arrow);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, tx + 6, ty - 3, lab, txtcol, 1);

    /* 每个目标都标呼号(与交互原型一致)。 */
    char cs[10];
    callsign_of(v->ac, cs, sizeof(cs));
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, tx + 6, ty + 6, cs, txtcol, 1);
}

/* 底部详情条：选中目标的机型/注册 + 高度/地速/距离/磁方位。 */
static void draw_detail_bar(uint16_t *fb, const vis_t *v)
{
    const aircraft_t       *a   = v->ac;
    const pk_traffic_rel_t *rel = &v->rel;

    pk_pfd_darken_rect(fb, 0, 221, PK_DISPLAY_W, PK_DISPLAY_H, 150);

    char cs[10];
    callsign_of(a, cs, sizeof(cs));
    const char *code = pk_aircraft_type_code(a->icao24);
    const char *desc = pk_aircraft_type_desc(a->icao24);
    const char *reg  = pk_aircraft_registration(a->icao24);
    char line1[48];
    int p = snprintf(line1, sizeof(line1), "%s", cs);
    if (code && code[0] && p > 0 && (size_t)p < sizeof(line1))
        p += snprintf(line1 + p, sizeof(line1) - p, "  %s", code);
    if (desc && desc[0] && p > 0 && (size_t)p < sizeof(line1))
        p += snprintf(line1 + p, sizeof(line1) - p, " %s", desc);
    if (reg && reg[0] && p > 0 && (size_t)p < sizeof(line1))
        snprintf(line1 + p, sizeof(line1) - p, "  %s", reg);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 4, 223, line1,
                 pk_rgb565(0, 210, 235), 1);

    char alts[12];
    if (a->have_altitude) snprintf(alts, sizeof(alts), "%d", a->altitude_ft);
    else                  snprintf(alts, sizeof(alts), "----");
    int gs     = a->have_velocity ? a->ground_speed_kt : 0;
    int dist10 = (int)lroundf(rel->dist_nm * 10.0f);
    if (dist10 < 0) dist10 = 0;
    int brg = ((int)lroundf(rel->abs_bearing) % 360 + 360) % 360;
    char line2[80];
    snprintf(line2, sizeof(line2), "ALT%s GS%d %d.%dNM BRG%03d",
             alts, gs, dist10 / 10, dist10 % 10, brg);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 4, 232, line2,
                 pk_rgb565(207, 211, 220), 1);
}

/*
 * 右栏目标列表。
 *
 * 视觉语言照搬 PFD 的信息框（pfd_infobox.c）：一行一条、半透明底、左边呼号
 * 右边数值，不画边框。相对高度也沿用 PFD 交通标签的写法——百英尺为单位、
 * 带符号、后缀升降箭头（"+26^"），飞行员在 PFD 上已经读惯了这个格式。
 *
 * 按距离由近到远排，最近的在最上面。
 */
static void draw_side_list(uint16_t *fb, const vis_t *vis, int nv, int sel_row)
{
    const uint16_t COL_SEL  = pk_rgb565(255, 210,  60);
    const uint16_t COL_TXT  = pk_rgb565(235, 240, 248);
    const uint16_t COL_DIM  = pk_rgb565(155, 170, 190);

    const int x0 = SIDE_X + 8;
    const int x1 = PK_DISPLAY_W - 8;

    for (int i = 0; i < ROW_N && i < nv; ++i) {
        const int y0 = TFC_TOP + 6 + i * (ROW_H + ROW_GAP);
        const vis_t *v = &vis[i];
        const bool sel = (i == sel_row);

        pk_pfd_darken_rect(fb, x0, y0, x1, y0 + ROW_H, sel ? 120 : 170);

        const int ty = y0 + (ROW_H - PK_AA_M_H) / 2;
        char cs[10];
        callsign_of(v->ac, cs, sizeof(cs));
        TFC_PUTS(fb, x0 + ROW_PAD, ty, cs, sel ? COL_SEL : COL_TXT);

        /* 右侧：距离 + 相对高度，格式与 PFD 交通标签一致。 */
        char val[20];
        if (v->rel.rel_alt_valid) {
            const int h100 = v->rel.rel_alt_ft / 100;
            const char vs = (v->rel.vs_fpm >  200) ? '^'
                          : (v->rel.vs_fpm < -200) ? 'v' : ' ';
            snprintf(val, sizeof(val), "%.0fNM %+d%c", v->rel.dist_nm, h100, vs);
        } else {
            snprintf(val, sizeof(val), "%.0fNM  ---", v->rel.dist_nm);
        }
        const int vw = (int)strlen(val) * pk_aa_cell_w(PK_AA_M);
        TFC_PUTS(fb, x1 - ROW_PAD - vw, ty, val, sel ? COL_SEL : COL_DIM);
    }
}

void pk_traffic_page_render(uint16_t *fb)
{
    const uint16_t COL_BG    = pk_rgb565(  7,  10,  16);
    /* 半透屏(transflective)下低亮度像素会和背景蓝融合而看不见,故距离环/
     * 刻度/罗盘标等灰色元素整体大幅提亮以保证可读性(真机实测)。 */
    const uint16_t COL_RING  = pk_rgb565(120, 145, 175);  /* 距离环 + 刻度 */
    const uint16_t COL_RINGL = pk_rgb565(170, 188, 210);  /* 环标数字 */
    const uint16_t COL_CARD  = pk_rgb565(220, 228, 238);  /* E/S/W */
    const uint16_t COL_N     = pk_rgb565(245, 250, 255);  /* N(最亮,突出北向) */
    const uint16_t COL_OWN   = pk_rgb565(255, 255, 255);
    const uint16_t COL_HDR   = pk_rgb565(235, 235, 235);
    const uint16_t COL_CYAN  = pk_rgb565(  0, 210, 235);
    const uint16_t COL_GREY  = pk_rgb565(120, 120, 128);
    const uint16_t COL_AMBER = pk_rgb565(255, 176,   0);

    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* ── 取数据（照 pfd.c PFD 分支）── */
    int64_t now_us = esp_timer_get_time();

    pk_imu_sample_t s;
    bool have = pk_imu_sample_get(&s);

    aircraft_t own = {0};
    pk_own_src_t src;
    bool own_valid = pk_own_ship_resolve(
        now_us, (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL, &own, &src);

    pk_baro_state_t baro;
    bool baro_ok = pk_baro_get(&baro);

    /* 本机航向 + 磁偏角：绑定 ADS-B own 且有速度时,优先用 own 的地速航向
     * (真北参考,与 pfd.c 一致,不再减磁偏角);否则回退 IMU 磁航向(磁北系,
     * 减查表磁偏角)。 */
    float own_heading = 0.0f;
    bool  hdg_valid   = false;
    float mag_var     = 0.0f;
    {
        pk_hdg_src_t hsrc;
        hdg_valid = pk_own_heading_resolve(own_valid, src, &own,
                                           have, have ? s.yaw_deg : 0.0f,
                                           &own_heading, &hsrc);
        /* IMU 是磁北系 → 减查表磁偏角转真北；ADS-B / GPS track 已是真北。 */
        if (hsrc == PK_HDG_SRC_IMU && own_valid)
            mag_var = pk_mag_var_lookup(own.lat, own.lon);
    }

    /* 相对高度的本机基准:绑定 own 用其 ADS-B 气压高度(与目标 Mode-C 同基准),
     * 否则用 baro 标准气压高度。原来恒用 baro,绑定高空 own 时所有目标都会
     * 算成大正数(全 +,钳到 +99)——这正是相对高度符号全错的根因。 */
    int own_palt;
    if (own_valid && own.have_altitude) {
        own_palt = own.altitude_ft;
    } else if (baro_ok && baro.valid) {
        own_palt = std_alt_ft_from_pa(baro.pressure_pa);
    } else {
        own_palt = PK_ALT_UNAVAIL;
    }

    pk_map_orient_t orient = pk_map_orient_get();
    int range_idx = pk_traffic_range_idx_get();
    int range_nm  = pk_traffic_range_nm(range_idx);

    /* HSI 可见扇区半透明填充 — 背景层,必须在距离环/罗盘/目标之前画。 */
    draw_hsi_sector(fb, orient, own_heading, hdg_valid);

    size_t n = aircraft_state_snapshot(
        s_scratch, AIRCRAFT_TABLE_CAPACITY, now_us, AIRCRAFT_STALE_AGE_US);

    /* ── 顶栏 ──
     * 与 PFD 状态栏同高（PFD_BAR_BOT），文字走统一字体的 normal 档。 */
    char buf[24];
    TFC_PUTS(fb, 24, TFC_HDR_TY, "TRAFFIC", COL_HDR);
    if (hdg_valid) {
        snprintf(buf, sizeof(buf), "HDG %03d~", ((int)lroundf(own_heading) + 360) % 360);
        TFC_PUTS(fb, 200, TFC_HDR_TY, buf, COL_CYAN);
    } else {
        TFC_PUTS(fb, 200, TFC_HDR_TY, "HDG ---~", COL_AMBER);
    }
    snprintf(buf, sizeof(buf), "TFC %d", (int)n);
    TFC_PUTS(fb, 400, TFC_HDR_TY, buf, COL_GREY);

    /* 朝向图例。两种投影下同一幅画面含义完全不同——机头上时罗盘随本机转、
     * 目标方位是相对的；正北上时罗盘固定、本机符号才转。不标出来，读者无从
     * 判断自己看到的是哪一种。 */
    TFC_PUTS(fb, 540, TFC_HDR_TY,
             (orient == PK_MAP_HEADING_UP) ? "HDG UP" : "NORTH UP",
             COL_CARD);
    snprintf(buf, sizeof(buf), "%dNM", range_nm);
    {
        int w = (int)strlen(buf) * pk_aa_cell_w(PK_AA_M);
        TFC_PUTS(fb, PK_DISPLAY_W - 24 - w, TFC_HDR_TY, buf, COL_GREY);
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
        float screen = (orient == PK_MAP_HEADING_UP) ? (float)d - own_heading : (float)d;
        int x1, y1, x2, y2;
        polar(screen, RMAX, &x1, &y1);
        polar(screen, RMAX - (d % 90 == 0 ? 8 : 5), &x2, &y2);
        pk_pfd_draw_line(fb, x1, y1, x2, y2, COL_RING);
    }
    {
        const char *cards[4] = { "N", "E", "S", "W" };
        for (int i = 0; i < 4; i++) {
            int d = i * 90;
            float screen = (orient == PK_MAP_HEADING_UP) ? (float)d - own_heading : (float)d;
            int lx, ly;
            polar(screen, RMAX - 16, &lx, &ly);
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         lx - 2, ly - 3, cards[i], i == 0 ? COL_N : COL_CARD, 1);
        }
    }

    /* ── 目标：算几何 → 按距离排序 → 选中跟踪(复用列表选中) ── */
    static vis_t    s_vis[AIRCRAFT_TABLE_CAPACITY];
    static uint32_t s_icaos[AIRCRAFT_TABLE_CAPACITY];
    int nv = 0;

    if (own_valid) {
        for (size_t i = 0; i < n; i++) {
            aircraft_t *t = &s_scratch[i];
            if (own.icao24 != 0 && t->icao24 == own.icao24) continue;  /* 不画自己 */
            pk_traffic_rel_t rel = pk_traffic_rel_calc(
                true, own.lat, own.lon, own_heading, mag_var, own_palt,
                t->have_position, t->lat, t->lon,
                t->have_altitude, t->altitude_ft, t->vert_rate_fpm);
            if (!rel.valid) continue;
            if (rel.dist_nm > (float)range_nm) continue;   /* 量程外不画 */
            s_vis[nv].ac  = t;
            s_vis[nv].rel = rel;
            nv++;
        }
        /* 按距离升序（由近到远，选择循环也是这个顺序）。n≤64，插入排序足够。 */
        for (int a = 0; a < nv; a++)
            for (int b = a + 1; b < nv; b++)
                if (s_vis[b].rel.dist_nm < s_vis[a].rel.dist_nm) {
                    vis_t tmp = s_vis[a]; s_vis[a] = s_vis[b]; s_vis[b] = tmp;
                }
    }

    for (int k = 0; k < nv; k++) s_icaos[k] = s_vis[k].ac->icao24;
    /* 雷达页独立选中(返回 -1 = 无选中,不高亮/不显详情;绝不 fallback row 0)。 */
    int sel_row = pk_ui_traffic_resolve(s_icaos, (size_t)nv);

    draw_side_list(fb, s_vis, nv, sel_row);

    if (!own_valid) {
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     CX - 30, CY - 4, "NO OWN POS", COL_AMBER, 1);
    } else {
        for (int k = 0; k < nv; k++) {
            if (k == sel_row) continue;                /* 选中最后画(置顶) */
            draw_target(fb, &s_vis[k], orient, range_nm, false);
        }
        if (nv > 0 && sel_row >= 0 && sel_row < nv)
            draw_target(fb, &s_vis[sel_row], orient, range_nm, true);
    }

    /* ── 本机飞机符号：HEADING-UP 机头朝上；NORTH-UP 按航向旋转标朝向 ── */
    draw_own_aircraft(fb, (orient == PK_MAP_NORTH_UP && hdg_valid) ? own_heading : 0.0f, COL_OWN);

    /* 详情不再压在雷达上：同样的信息已经在右栏卡片里（spec §5.2 也把它归到
     * 卡片），雷达区留给图形本身。draw_detail_bar() 暂时保留，等右栏补上机型
     * 与注册号之后一并删除。 */
    (void)draw_detail_bar;
}
