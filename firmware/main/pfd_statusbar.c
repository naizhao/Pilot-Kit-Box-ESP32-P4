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

#include "esp_timer.h"

#include "config_demo.h"   /* pk_demo_enabled —— 中段要给 DEMO 徽标让宽度 */
#include "display.h"
#include "pfd_layout.h"
#include "pfd_aa_text.h"
#include "pfd_statusbar_icons.h"
#include "pfd_icon_font.h"
#include "pfd_draw.h"

/* pk_ui_topbar_status_collect 汇总的数据源——与 pfd.c PFD 分支同一批取法，
 * 见 pfd_statusbar.h 里该函数的头注。 */
#include "battery.h"
#include "ble_gatt.h"
#include "gps.h"
#include "pk_sdcard.h"
#include "soc_temp.h"
#include "ui_state.h"      /* pk_ui_cal_hint_active —— 罗盘校准提示的电平 */

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
#define BAR_RIGHT_W     (pk_bar_icon_width(PK_BAR_ICON_SD) \
                       + pk_bar_icon_width(PK_BAR_ICON_BLE) \
                       + pk_bar_icon_width(PK_BAR_ICON_BATT) \
                       + BAR_TEXT_W(4) + PFD_BAR_GAP_LABEL)

int pk_ui_topbar_right_limit(int dflt)
{
    if (!pk_demo_enabled()) return dflt;
    const int lim = PK_UI_DEMO_BADGE_X - PFD_BAR_GAP_WORD;
    return dflt < lim ? dflt : lim;
}

/* 中段"保底两项"（SAT/ADSB——罩哥点名要常驻的三个图标里除 SD 之外的两个）
 * 的 worst-case 宽度——与上面 BAR_RIGHT_W 同一个道理：按可能出现的最长
 * 文案预留，不随当前实际显示了几项而伸缩。
 *
 * 这一份宽度要在 PFD / 交通 / 地图 / 列表四页之间共用同一个左边界（见下面
 * pk_ui_status_group_left_x），只有边界是固定值，四页在同一帧读到的才会是
 * 完全相同的 x。
 *
 *   SAT   "NO FIX"（6 字符，比两位数星数更长；有定位时是数字，NO FIX 与
 *          数字互斥，不会同时出现，6 字符已经是这一项的真实上限）
 *   ADSB  2 位数目标数——AIRCRAFT_TABLE_CAPACITY=64，两位数已经封顶，不必
 *          按 3 位留
 *
 * REC/TEMP 不在保底之列：把它们也按 worst-case 算的话，四页公用的这一整块
 * 会占掉小屏（800 宽）三分之一还多，挤得交通页的量程、列表页的排序说明+
 * RESET 几乎没有立足之地（实测非默认排序 + 英文列名时 RESET 会被推出
 * 屏幕）。它们仍然会画——中段自带的"装不下就按优先级丢"会在当前 avail
 * 里自然地把它们排进去，只是不作为四页布局都要让路的硬预留；REC/TEMP
 * 本身也比 SAT/ADSB 少见得多（REC 只在录制时出现，TEMP 只在超温时出现），
 * 换来的是 RESET/NM 这类页面自身部件始终有真实可用的空间，不必对着一个
 * 几乎不会同时占满的 worst-case 让路。 */
#define BAR_MID_GUARANTEED_W ( \
      pk_bar_icon_width(PK_BAR_ICON_SAT)  + BAR_TEXT_W(6) \
    + PFD_BAR_GAP_WORD \
    + pk_bar_icon_width(PK_BAR_ICON_ADSB) + BAR_TEXT_W(2) )

/*
 * 顶栏「设备状态组」（中段 + 右段整体）的左界。
 *
 * 从右边界（pk_ui_topbar_right_limit，已经处理了 DEMO 徽标让位）往左依次
 * 减去右段 worst-case 宽度、段间词距、中段保底宽度——全部是固定量，不看 s
 * 里的任何字段。这正是四个页面能共用同一个 x 的原因：只要屏宽和 DEMO 开关
 * 相同，这个函数在哪一页调用结果都一样。
 *
 * 这里退出来的宽度**正好等于** BAR_MID_GUARANTEED_W（下面 avail 的推导），
 * 不多留：中段能且只能保证画出 SAT+ADSB，REC/TEMP 能不能挤进去看当前文案
 * 的实际长度，挤不进去就被现有的优先级丢弃逻辑吃掉。 */
static int pk_ui_status_group_left_x(void)
{
    const int right_edge = pk_ui_topbar_right_limit(PK_DISPLAY_W - PFD_BAR_MARGIN_R);
    const int right_start = right_edge - BAR_RIGHT_W - PFD_BAR_GAP_WORD;
    /* avail = right_start - left_end - 2*GAP_WORD 要求 == BAR_MID_GUARANTEED_W，
     * 反解出 left_end。 */
    return right_start - 2 * PFD_BAR_GAP_WORD - BAR_MID_GUARANTEED_W;
}

int pk_ui_topbar_content_right_limit(int dflt)
{
    const int lim = pk_ui_status_group_left_x() - PFD_BAR_GAP_WORD;
    return dflt < lim ? dflt : lim;
}

void pk_ui_topbar_status_collect(pk_pfd_status_t *st)
{
    const int64_t now_us = esp_timer_get_time();

    pk_gps_state_t gps;
    pk_gps_get(&gps);
    st->gps_have_fix = gps.have_fix;
    st->gps_sats     = (uint8_t)(gps.sats < 0 ? 0 : (gps.sats > 99 ? 99 : gps.sats));

    st->ble_connected = ble_gatt_is_connected();
    st->sd_mounted     = pk_sdcard_is_mounted();
    /* 1.2 s 周期（600 ms 半周期），与 pfd.c PFD 分支同一个相位公式——
     * 各页必须算出同一个相位，否则闪烁在切页时会跳一下，且非 PFD 页会
     * 显得"没在闪"或"恒定"，见 IMPLEMENTATION_PLAN 的验收要求。 */
    st->sd_alert_blink_on = ((now_us / 600000) & 1) != 0;
    st->uptime_ms = (uint32_t)(now_us / 1000);

    st->temp_warn = pk_soc_temp_get(&st->temp_c);

    /* 罗盘校准提示。取的是 advisor 的结论电平（HINT 档），四页共用这一处，
     * 与 PFD 分支同一个真值——PFD 也走本函数，没有第二份取法。 */
    st->cal_hint = pk_ui_cal_hint_active();

    pk_batt_t b;
    pk_batt_get(&b);
    st->batt_valid    = b.valid;
    st->batt_pct      = (uint8_t)b.pct;
    st->batt_charging = b.charging;
}

void pk_ui_topbar_status_render(uint16_t *fb, const pk_pfd_status_t *s)
{
    char      buf[12];
    const int ty = PFD_BAR_TEXT_Y;

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
     *   4  CAL   罗盘精度低。排在 TEMP 之下是因为它**不是故障**：航向还在出，
     *            只是可能不准，而且转两圈就好了。前面四项任何一项要位置，
     *            它都该先让出来
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

        bar_item_t items[5] = {0};
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
        if (s->cal_hint) {
            /* 图标独自成立，不带文字：精度数值（0..3）是校准页与诊断页的事，
             * 顶栏这一枚只回答"要不要去校准"，多一个数字反而要用户先学会
             * 0..3 各是什么意思。
             *
             * COL_WARN 常亮，**不闪**。与没插卡那枚 SD_ALERT 的红闪刻意区分：
             * 红闪说的是"现在就有东西坏了/没在录"，而罗盘精度低是"注意，航向
             * 可能不准"——航向照样在出，飞行也照样能继续。座舱里闪烁是抢注意
             * 力的最强手段，留给真出事的那一档，否则闪多了就没人再看了。 */
            items[n].col  = COL_WARN;
            items[n].icon = PK_BAR_ICON_COMPASS;
            n++;
        }

        /* 中段可用区间：左端是四页共用的固定锚点（见 pk_ui_status_group_left_x
         * 头注——不是量出来的 HDG/标题宽度，否则四页各不相同）、右端设备状态
         * 之前，各留一个词距。 */
        int left_end   = pk_ui_status_group_left_x();
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

            rx -= PFD_BAR_GAP_LABEL;
        }

        /* SD 卡。与蓝牙/电量同属"设备自身状态"组，常驻不参与中段的降级——
         * 插没插卡、能不能写，用户任何时候都该知道，不该因为 GPS/ADSB 抢
         * 位置就被挤掉。 */
        {
            rx -= pk_bar_icon_width(PK_BAR_ICON_SD);
            if (s->sd_mounted) {
                pk_bar_icon_draw(fb, rx, BAR_ICON_Y, PK_BAR_ICON_SD, NULL, COL_GREEN);
            } else {
                /* 闪烁不做「画/不画」的有无切换，而是亮红↔暗红——理由同
                 * ADS-B LOST（pfd_infobox.c）：静态截图也能截出告警配色，
                 * 图标不会在灭的那半周期里凭空消失、占位跟着跳动。相位由
                 * 调用方（pfd.c/sim）算好放进 s->sd_alert_blink_on，本函数
                 * 只管按位取色，不存状态。 */
                uint16_t col = s->sd_alert_blink_on ? COL_RED
                                                     : pk_rgb565(110, 34, 26);
                pk_bar_icon_draw(fb, rx, BAR_ICON_Y, PK_BAR_ICON_SD_ALERT, NULL, col);
            }
        }
    }
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

    pk_ui_topbar_status_render(fb, s);
}
