/*
 * pfd_statusbar.c — G1000-style top status strip (18 px tall, full
 * panel width). Left: cyan "HDG" + green numeric heading from yaw.
 * Right: cyan "ADSB" + green count from aircraft_state_snapshot().
 *
 * Rendered every frame from the per-frame pk_pfd_status_t the PFD task
 * builds. Stale (no IMU) HDG renders as grey "---°".
 */

#include "pfd_statusbar.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config_demo.h"   /* pk_demo_enabled —— 中段要给 DEMO 徽标让宽度 */
#include "display.h"
#include "pfd_layout.h"
#include "pfd_aa_text.h"
#include "pfd_statusbar_icons.h"
#include "pfd_icon_font.h"
#include "pfd_draw.h"

#define STATUSBAR_TOP   0
#define STATUSBAR_BOT  PFD_BAR_BOT

#define COL_BG     pk_rgb565(  8,   8,  12)
#define COL_LABEL  pk_rgb565( 70, 220, 250)
#define COL_GREEN  pk_rgb565(  0, 220,  60)
#define COL_WHITE  pk_rgb565(255, 255, 255)
#define COL_STALE  pk_rgb565(100, 100, 100)
#define COL_WARN   pk_rgb565(255, 180,  63)
#define COL_RED    pk_rgb565(255,  80,  60)

/* 4.3 寸屏走 TTF 派生的抗锯齿字形（B612 Mono，见 pfd_aa_text.h）。
 * 位图字体整数倍放大会变成方块像素，在 217 PPI 上无法接受。 */
#  define BAR_GLYPH_W   PK_AA_M_W
#  define BAR_CELL_H    PK_AA_M_H
#  define BAR_PUTS(fb, x, y, str, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (str), (col), PK_AA_M)

/* 定宽字体下字符串的像素宽度。 */
#define BAR_TEXT_W(n)   ((n) * BAR_GLYPH_W)

/* 图标 cell 与文字 cell 顶对齐后的居中偏移（大屏两者同为 30 px，偏移 0）。 */
#define BAR_ICON_Y      (PFD_BAR_TEXT_Y + (BAR_CELL_H - PK_ICON_H) / 2)

/* 右段（蓝牙 + 电量）按**最坏宽度**预留，不随内容增减而伸缩。
 *
 * 若按实际内容算，蓝牙一断开中段就会整排平移 —— 顶栏元素的位置必须稳定，
 * 否则每次扫视都要重新找目标在哪。最坏情况是两个图标 + "100%" 四字符。 */
#define BAR_RIGHT_W     (pk_bar_icon_width(PK_BAR_ICON_BLE) \
                       + pk_bar_icon_width(PK_BAR_ICON_BATT) \
                       + BAR_TEXT_W(4) + PFD_BAR_GAP_LABEL)

int pk_ui_topbar_right_limit(int dflt)
{
    if (!pk_demo_enabled()) return dflt;
    const int lim = PK_UI_DEMO_BADGE_X - PFD_BAR_GAP_WORD;
    return dflt < lim ? dflt : lim;
}

void pk_pfd_statusbar_render(uint16_t *fb, const pk_pfd_status_t *s)
{
    pk_pfd_fill_rect(fb, 0, STATUSBAR_TOP, PK_DISPLAY_W, STATUSBAR_BOT, COL_BG);

    char      buf[12];
    const int ty = PFD_BAR_TEXT_Y;

    /* ── 左段：HDG + 航向，左对齐 ───────────────────────────── */
    {
        int x = PFD_BAR_MARGIN_L;
        BAR_PUTS(fb, x, ty, "HDG", COL_LABEL);
        x += BAR_TEXT_W(3) + PFD_BAR_GAP_LABEL;

        if (s->imu_valid) {
            int hdg = ((int)s->yaw_deg + 360) % 360;
            snprintf(buf, sizeof(buf), "%03d~", hdg);
            BAR_PUTS(fb, x, ty, buf, COL_GREEN);
        } else {
            BAR_PUTS(fb, x, ty, "---~", COL_STALE);
        }
    }

    /* ── 中段：状态位，按优先级填充可用宽度 ──────────────────
     *
     * 中段能放多少完全取决于面板宽度：320 屏中段仅剩约 130 px（放得下
     * 一项），800 屏有 500 px 上下（放得下四五项）。与其为每种屏手算
     * 一套坐标，不如声明优先级后让代码自己裁 —— 以后新增状态位也不必
     * 重新推导布局。
     *
     * 优先级依据「不知道会出事」的程度：
     *   0  GPS   定位丢失直接影响本机位置与授时
     *   1  ADSB  周围有几架飞机是本机的核心输出，紧跟 GPS
     *   2  REC   以为在录、实际没录，是本产品最难受的失败
     *   3  TEMP  仅在超温时出现；产品定位就是"Garmin 热死时的备份"，
     *            自身温度异常必须让用户看见
     *
     * BLE 与电量不在此列 —— 它们是「设备自身」的状态而非「飞行」的状态，
     * 固定在右端，与左端的航向一样常驻不参与降级。
     */
    {
        typedef struct {
            char          text[16];
            uint16_t      col;
            pk_bar_icon_t icon;     /* 图标提供语义：孤立的「100%」看不出是电量 */
        } bar_item_t;

        bar_item_t items[4] = {0};
        int n = 0;

        if (s->gps_have_fix) {
            snprintf(items[n].text, sizeof(items[n].text), "%u", (unsigned)s->gps_sats);
            items[n].col = COL_GREEN;
        } else {
            snprintf(items[n].text, sizeof(items[n].text), "NO FIX");
            items[n].col = COL_RED;
        }
        items[n].icon = PK_BAR_ICON_SAT;
        n++;

        /* 目标计数用 "%u" 而不是 "%2u"：等宽字体下前导空格就是一个整字宽
         * 的空档，个位数时图标会被推得离数字老远。宽度稳定靠等宽字体本身，
         * 不需要靠补空格。 */
        snprintf(items[n].text, sizeof(items[n].text), "%u",
                 (unsigned)s->aircraft_count);
        items[n].col  = COL_GREEN;
        items[n].icon = PK_BAR_ICON_ADSB;
        n++;

        if (s->rec_active) {
            snprintf(items[n].text, sizeof(items[n].text), "REC");
            items[n].col  = COL_RED;
            items[n].icon = PK_BAR_ICON_REC;
            n++;
        }
        if (s->temp_warn) {
            snprintf(items[n].text, sizeof(items[n].text), "%d~C", s->temp_c);
            items[n].col  = COL_WARN;
            items[n].icon = PK_BAR_ICON_TEMP;   /* 温度计+感叹号，比通用三角更准确 */
            n++;
        }

        /* 中段可用区间：左端 HDG 之后、右端设备状态之前，各留一个词距。 */
        int left_end   = PFD_BAR_MARGIN_L + BAR_TEXT_W(3 + 4) + PFD_BAR_GAP_LABEL;
        /* 右段自身在演示模式下已经整体左移（见下面那段），中段跟着一起退——
         * 顶栏本来就有一套「装不下就按优先级丢」的机制，把新的可用宽度告诉它
         * 就够了，不必另写一套避让。 */
        int right_start= pk_ui_topbar_right_limit(PK_DISPLAY_W - PFD_BAR_MARGIN_R)
                         - BAR_RIGHT_W - PFD_BAR_GAP_WORD;
        int avail      = right_start - left_end - 2 * PFD_BAR_GAP_WORD;

        /* 从低优先级端逐个丢弃，直到装得下。items 已按优先级升序排列。 */
        int used;
        for (;;) {
            used = 0;
            for (int i = 0; i < n; ++i) {
                used += pk_bar_icon_width(items[i].icon);
                used += BAR_TEXT_W((int)strlen(items[i].text));
            }
            if (n > 1) used += (n - 1) * PFD_BAR_GAP_WORD;
            if (used <= avail || n <= 1) break;
            --n;                       /* 丢掉当前最低优先级的一项 */
        }

        int x = left_end + PFD_BAR_GAP_WORD + (avail - used) / 2;   /* 中段内居中 */
        for (int i = 0; i < n; ++i) {
            x += pk_bar_icon_draw(fb, x, BAR_ICON_Y, items[i].icon,
                                  NULL, items[i].col);
            if (items[i].text[0]) {
                BAR_PUTS(fb, x, ty, items[i].text, items[i].col);
                x += BAR_TEXT_W((int)strlen(items[i].text));
            }
            x += PFD_BAR_GAP_WORD;
        }
    }

    /* ── 右段：设备自身状态（蓝牙 + 电量），右对齐 ──────────
     *
     * 从右边界往左依次落位。图标与其数值**紧贴**，整组一起右对齐，而不是
     * 图标钉死、数值补前导空格右对齐 —— 后者在 100%→99% 时会让图标和数字
     * 之间凭空多出一个字宽的缝。电量位数一次飞行里最多跨位两次，整组平移
     * 那点位移远比缝隙忽宽忽窄自然。 */
    {
        /* 演示模式下整组左移，把最右端让给常驻的 DEMO 徽标——它画在控件层，
         * 不让位的话被盖住的正好是电量百分比。 */
        int rx = pk_ui_topbar_right_limit(PK_DISPLAY_W - PFD_BAR_MARGIN_R);

        if (s->batt_valid) {
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)s->batt_pct);
            /* 电量色阶。灰色是本项目里"数据失效"的专用色（见 tape/HSI 的
             * "---"），有效读数绝不能用灰，否则用户会以为电量读数不可信。
             *
             * 常态取白：航电惯例里白 = 正常/仅供参考，绿留给需要**主动确认
             * 有效**的读数（航向、星数、目标数）。电量满本就不是新闻，涂绿
             * 只会和它们争注意力。
             *
             * ≤6% 正是九档刻度里 alert 那一档的覆盖范围（见 batt_icon_for），
             * 让变红与换成告警图标同时发生，而不是各走各的阈值。
             *
             * 充电时一律取白：正在回升的低电量不是需要处置的异常，涂红只会
             * 制造假警报。 */
            uint16_t col = s->batt_charging      ? COL_WHITE
                         : (s->batt_pct <= 6)    ? COL_RED
                         : (s->batt_pct < 25)    ? COL_WARN
                                                 : COL_WHITE;
            const pk_bar_batt_t batt = {
                .pct       = s->batt_pct,
                .charging  = s->batt_charging,
                .uptime_ms = s->uptime_ms,
            };
            int tw = BAR_TEXT_W((int)strlen(buf));
            rx -= pk_bar_icon_width(PK_BAR_ICON_BATT) + tw;

            int x = rx;
            x += pk_bar_icon_draw(fb, x, BAR_ICON_Y, PK_BAR_ICON_BATT, &batt, col);
            BAR_PUTS(fb, x, ty, buf, col);

            /* 蓝牙与电量同属"设备自身状态"这一组，用近距而不是词距 ——
             * 词距是留给语义无关的相邻项的，用在组内会把它们拆成两摊。 */
            rx -= PFD_BAR_GAP_LABEL;
        }

        if (s->ble_connected) {       /* 蓝牙符号本身即可表意，无需文字 */
            rx -= pk_bar_icon_width(PK_BAR_ICON_BLE);
            pk_bar_icon_draw(fb, rx, BAR_ICON_Y, PK_BAR_ICON_BLE, NULL, COL_LABEL);
        }
    }
}
