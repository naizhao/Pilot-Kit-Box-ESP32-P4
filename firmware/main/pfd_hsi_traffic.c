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
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "display.h"
#include <stdbool.h>

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

/* 相对高度标签用 XS 档（18 px，spec §2 硬下限）。
 *
 * 它和罗盘的刻度数字都挤在同一圈上，同为 S 档时角度接近的两者会读成一团。
 * 拉开一档是最直接的区分——而且这类标签本就属于 spec 说的「极次要」：
 * 飞行员先看菱形在哪个方位，才会去读它高多少。 */
#if PK_DISPLAY_W >= 800
#  define TGT_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_M)
#  define TGT_LBL_W     PK_AA_M_W
#  define TGT_LBL_H     PK_AA_M_H
/* 「后方 N 架」前缀的下箭头。
 *
 * 必须按渲染器分两份：AA 字库走 UTF-8 解码 + 码位二分查表（pfd_aa_text.c
 * utf8_next / cjk_index），它的箭头存在 U+2193；而旧 5×7 位图字体没有 UTF-8
 * 这一说，箭头挂在私有码位 0x84 上。此前两档共用 PK_FONT_ARROW_S(0x84)，喂给
 * pk_aa_puts 就是一个非法 UTF-8 前导字节 → U+FFFD → 查表落空 → 只推进 15 px
 * 不画：屏上永远只剩一个孤零零的数字，左边空着 15 px。 */
#  define TGT_ARROW_BEHIND  "↓"
/* 标签底的压暗强度。目标常落在天地交界或罗盘刻度上，纯文字会糊进背景。
 * 只压暗、不描边：十几个目标各带一个方框，外圈立刻显得杂乱。 */
#  define TGT_LBL_BG    90
#else
#  define TGT_PUTS(fb, x, y, s, col) \
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), 1)
#  define TGT_LBL_W     6
#  define TGT_LBL_H     6
#  define TGT_LBL_BG    0
#  define TGT_ARROW_BEHIND  "\x84"      /* 位图字体的私有码位，见上 */
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

/* ── 标签避让 ──────────────────────────────────────────────────
 *
 * 此前标签一律沿半径向外推固定距离。方位接近的两个目标，标签就会叠在一起；
 * 正前方那个还会顶到航向框上。改成候选位置逐个试：优先仍是径向外（读起来
 * 最自然），撞了就沿切向挪、再撞就往外推一档。
 *
 * 占位表每帧重置，只记录本帧已落位的标签与航向框。规模是十几个矩形，
 * O(n²) 的朴素比对完全够用，不值得上空间索引。 */
typedef struct { int16_t x0, y0, x1, y1; } lbl_rect_t;

static lbl_rect_t s_used[AIRCRAFT_TABLE_CAPACITY + 1];
static int        s_n_used;

static bool rect_hit(const lbl_rect_t *a, const lbl_rect_t *b)
{
    return !(a->x1 <= b->x0 || b->x1 <= a->x0 ||
             a->y1 <= b->y0 || b->y1 <= a->y0);
}

static bool place_ok(const lbl_rect_t *r)
{
    if (r->x0 < 0 || r->x1 > PK_DISPLAY_W || r->y1 > PK_DISPLAY_H) return false;
    /* 不许压到罗盘上——那里是刻度和方位数字的地盘。 */
    int mx = (r->x0 + r->x1) / 2, my = (r->y0 + r->y1) / 2;
    int dx = mx - HSI_CX, dy = my - HSI_CY;
    if (dx * dx + dy * dy < (HSI_R + 4) * (HSI_R + 4)) return false;

    for (int i = 0; i < s_n_used; ++i)
        if (rect_hit(r, &s_used[i])) return false;
    return true;
}

static void mark_used(const lbl_rect_t *r)
{
    if (s_n_used < (int)(sizeof(s_used) / sizeof(s_used[0])))
        s_used[s_n_used++] = *r;
}

void pk_pfd_hsi_traffic_render(uint16_t *fb)
{
    int64_t now_us = esp_timer_get_time();

    pk_imu_sample_t s;
    const bool imu_ok = pk_imu_sample_get(&s);

    aircraft_t own = {0};
    pk_own_src_t src;
    if (!pk_own_ship_resolve(now_us, (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL,
                             &own, &src))
        return;                              /* 无本机位置 */

    /*
     * 航向口径必须与**这一圈罗盘本身**一致，而罗盘的刻度来自 pfd.c 的
     * pk_own_heading_resolve（ADS-B > IMU > GPS track，见 own_ship.h）。
     *
     * 此前这里直接读裸 IMU yaw。绑定了 ADS-B 本机时，罗盘刻度盘转的是 ADS-B
     * 真航迹、叠在它上面的交通层却按 IMU 磁航向摆位——两层错开一个磁偏角，
     * 中国境内 3~11°，而这一层的全部意义就是「目标相对刻度盘在哪个方位」。
     * 顺带修掉一处更硬的降级：原来 IMU 一失效就整层不画，哪怕 ADS-B 航向还
     * 在、罗盘照常在转。
     *
     * mag_var 跟着来源走，口径照抄 traffic_page.c:781-795：IMU 是磁北参考，
     * 要把目标的真方位/真航迹减到磁系；ADS-B / GPS track 本身就是真北，减了
     * 反而多转一个磁偏角。「地图参考北 = 航向来源的北」——只有这样刻度盘上
     * 的 030 和叠加层上的 030 才是同一个方向。
     */
    float yaw = 0.0f;
    pk_hdg_src_t hsrc;
    if (!pk_own_heading_resolve(true, src, &own, imu_ok, imu_ok ? s.yaw_deg : 0.0f,
                                &yaw, &hsrc))
        return;                              /* 无航向无法定相对方位 */

    pk_baro_state_t baro;
    bool baro_ok = pk_baro_get(&baro);
    /* 相对高度的本机基准:绑定 own 时优先用其 ADS-B 气压高度(与目标 Mode-C 同
     * 1013.25 基准),否则用 baro 标准气压高度兜底。照 traffic_page.c:273-280
     * 同一逻辑——原来恒用 baro,绑定高空 own 时相对高度符号会全错。 */
    int own_palt;
    if (own.have_altitude)          own_palt = own.altitude_ft;
    else if (baro_ok && baro.valid) own_palt = std_alt_ft_from_pa(baro.pressure_pa);
    else                            own_palt = PK_ALT_UNAVAIL;
    const float mag_var = (hsrc == PK_HDG_SRC_IMU)
                        ? pk_mag_var_lookup(own.lat, own.lon) : 0.0f;

    size_t n = aircraft_state_snapshot(
        s_scratch, AIRCRAFT_TABLE_CAPACITY, now_us, AIRCRAFT_STALE_AGE_US);

    /* 目标按**相对高度**分三档着色。
     *
     * 此前一律青色，只能靠旁边的 ±NN 文字读高差——而扫视时先看到的是符号，
     * 不是数字。分档阈值取 ±1000 ft，接近 TCAS 判定「同高度」的 ±1200 ft。
     *
     * 三档的**亮度**也是递减的（琥珀 > 青 > 暗青）。spec §附录 要求半反半透
     * 屏上威胁等级改用「形状 + 亮度」编码，因为反射态色域只有 16.5%、红黄难
     * 分；亮度本就分开，那套备份配色可以直接沿用这一层，不必推倒重来。 */
    const uint16_t COL_TGT_LEVEL = pk_rgb565(255, 176,   0);  /* ±1000 ft 内 */
    const uint16_t COL_TGT_ABOVE = pk_rgb565(  0, 210, 235);  /* 高于本机     */
    const uint16_t COL_TGT_BELOW = pk_rgb565( 95, 150, 190);  /* 低于本机     */
    const uint16_t COL_TGT_NOALT = pk_rgb565(150, 155, 165);  /* 无高度数据   */
    /* 后方计数落在褐色地面上，原来的暗褐色几乎与背景同色——提高亮度并偏
     * 琥珀，保证在天与地两种背景上都能认出来。 */
    const uint16_t COL_BEHIND = pk_rgb565(240, 180,  90);

    /* 每帧重置占位表，并先把航向框占上：它是固定元素，标签必须绕开它。 */
    s_n_used = 0;
    {
        lbl_rect_t hdg = { PFD_HDGBOX_X0, PFD_HDGBOX_Y0,
                           PFD_HDGBOX_X1, PFD_HDGBOX_Y1 };
        mark_used(&hdg);
    }

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
        /* 有航迹就画带朝向的剪影，没有才退回菱形。
         *
         * 不能一律画飞机：没有 track 的目标画成箭头等于凭空编出一个朝向，
         * 而「它朝我来还是背我去」正是飞行员据此决策的信息，编不得。
         *
         * 罗盘恒是 heading-up 的，所以屏幕上的朝向 = 目标航迹 - 本机航向，
         * 且要先把真北参考的 track 降到本图的参考北（见 mag_var 的注释）。
         * 这套换算与交通页共用 pk_traffic_symbol_rot_deg()——两处各推一遍的
         * 后果已经付过学费：同一架飞机在两页上机头差 74.6°。 */
        uint16_t tcol = !rel.rel_alt_valid            ? COL_TGT_NOALT
                      : (rel.rel_alt_ft >  1000)         ? COL_TGT_ABOVE
                      : (rel.rel_alt_ft < -1000)         ? COL_TGT_BELOW
                                                         : COL_TGT_LEVEL;
        if (t->have_velocity) {
            const float rot = pk_traffic_symbol_rot_deg(
                true, (float)t->heading_deg, mag_var, yaw);
            pk_pfd_draw_aircraft(fb, tx, ty, rot, ROSE_SC(7), tcol);
        } else {
            fill_diamond(fb, tx, ty, ROSE_SC(4), tcol);
        }

        if (rel.rel_alt_valid) {
            int hh = rel.rel_alt_ft / 100;
            if (hh >  99) hh =  99;
            if (hh < -99) hh = -99;
            char b[12];
            snprintf(b, sizeof(b), "%+03d", hh);
            /* 候选位置：先试径向外（读起来最自然，且必定落在罗盘之外），
             * 撞了就沿切向左右挪，再撞就往外推一档。全都不成才硬放第一个
             * ——宁可叠一次，也不能让某个目标的高度差凭空消失。 */
            static const struct { int r_step; float tan_deg; } CAND[] = {
                { 0,   0.0f }, { 0, +14.0f }, { 0, -14.0f },
                { 1,   0.0f }, { 1, +18.0f }, { 1, -18.0f },
                { 2,   0.0f }, { 2, +24.0f }, { 2, -24.0f },
            };
            int lw = (int)strlen(b) * TGT_LBL_W;
            int lx = 0, ly = 0;
            for (size_t ci = 0; ci < sizeof(CAND) / sizeof(CAND[0]); ++ci) {
                float a  = (90.0f - r + CAND[ci].tan_deg) * (float)M_PI / 180.0f;
                int   rr = HSI_TRAFFIC_R + ROSE_SC(16) + CAND[ci].r_step * ROSE_SC(13);
                int   px = HSI_CX + (int)lroundf(rr * cosf(a));
                int   py = HSI_CY - (int)lroundf(rr * sinf(a));
                lx = px - lw / 2;
                ly = py - TGT_LBL_H / 2;
                lbl_rect_t cand = { (int16_t)(lx - 2), (int16_t)(ly + 3),
                                    (int16_t)(lx + lw + 2),
                                    (int16_t)(ly + TGT_LBL_H - 3) };
                if (place_ok(&cand) || ci + 1 == sizeof(CAND) / sizeof(CAND[0])) {
                    mark_used(&cand);
                    break;
                }
            }
            if (TGT_LBL_BG) {
                /* cell 上下各有约 4 px 空白，底框相应内收，免得看着虚胖。 */
                pk_pfd_darken_rect(fb, lx - 2, ly + 3,
                                   lx + lw + 2, ly + TGT_LBL_H - 3, TGT_LBL_BG);
            }
            TGT_PUTS(fb, lx, ly, b, tcol);   /* 标签与符号同色，读作一体 */
        }
    }

    if (behind > 0) {
        char b[16];
        snprintf(b, sizeof(b), "%s%d", TGT_ARROW_BEHIND, behind);  /* ↓N 后方 */
        /* 放右下角 VS 框(y≤228)下方的空隙：避开左下 own-ship badge
         * (x[0,78] y[210,232])，否则会被 pfd.c 后画的 badge darken+文字覆盖
         * 导致永久不可见。 */
        /* 落在左右两块信息框之间的空当（罗盘左侧）。左右下角现在各是一块三行
         * 信息框，压在任何一块上都会被后画的它们盖掉——不可见的告警等于没有。 */
        TGT_PUTS(fb, PFD_IB_LEFT_X0 + PFD_IB_W + 8,
                 PK_DISPLAY_H - TGT_LBL_H - 4, b, COL_BEHIND);
    }
}
