/*
 * adsb_drawer.c — 每机详情抽屉的实现（列表页与地图页共用，见 .h）。
 */
#include "adsb_drawer.h"

#include <stdio.h>
#include <string.h>

#include "ui_state.h"        /* 绑定动作：own-ICAO / toast / 跳 PFD */

/* host 单测只编译可移植面；渲染段依赖 AA 字体与 i18n，见 .h。 */
#ifndef PK_ADSB_DRAWER_HOST_TEST
#include "i18n.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"

#include "aircraft_db.h"     /* 注册号 / 机型回退 */
#include "airline_codes.h"   /* 呼号前缀 → 航司 */
#include "icao_country.h"    /* ICAO 段 → 国籍 */
#include "pk_callsign.h"     /* 呼号显示回退链 */
#endif /* !PK_ADSB_DRAWER_HOST_TEST */

/* ═══ 可移植面（host 单测覆盖） ══════════════════════════════════ */

/* 命中区 = 按钮矩形向外放宽（x ±16、y 上 12 下到屏底）。半开区间，
 * 与 pk_pfd_fill_rect 同约——边界值归左/上侧。 */
bool pk_adsb_drawer_bind_hit(int x, int y)
{
    return x >= PK_ADSB_DRAWER_BIND_X0 - 16 &&
           x <  PK_ADSB_DRAWER_BIND_X0 + PK_ADSB_DRAWER_BIND_W + 16 &&
           y >= PK_ADSB_DRAWER_BIND_Y0 - 12 &&
           y <  PK_DISPLAY_H;
}

uint32_t pk_adsb_drawer_tap_target(uint32_t cur, uint32_t tapped)
{
    return (cur == tapped) ? 0 : tapped;
}

void pk_adsb_drawer_bind_activate(uint32_t icao24)
{
    if (pk_ui_get_own_icao() == icao24) {
        pk_ui_clear_own_icao();
        pk_ui_toast_show(PK_TR_TOAST_OWN_CLEARED, false);
        /* 解绑留在当前页，不跳 */
    } else {
        pk_ui_set_own_icao(icao24);
        pk_ui_toast_show(PK_TR_TOAST_OWN_BOUND, false);
        pk_ui_set_mode(PK_UI_MODE_PFD);   /* 绑定后跳 PFD 看效果 */
    }
}

const char *pk_adsb_emergency_tag(const aircraft_t *a)
{
    if (!a->have_squawk) return NULL;
    switch (a->squawk) {
    /* 三个都压到 3 字符：抽屉标题旁的徽章没有多宽的地方，
     * NORDO 这种 5 字符的标准缩写塞不下，只能退到 RDO。 */
    case 7500: return "HJK";   /* 劫机 */
    case 7600: return "RDO";
    case 7700: return "EMG";   /* 一般紧急 */
    default:   return NULL;
    }
}

/* ═══ 渲染（host 切掉） ═════════════════════════════════════════
 * 2026-10-03 从 adsb_list.c 的 draw_drawer 逐行平移：版面、键值矩阵、
 * 绑定按钮全部原样，只有三处随搬迁换名——s_drawer_icao 换成 ac->icao24
 * （own 态按目标机现查）、emergency_tag/callsign 换成共享/直调实现。 */
#ifndef PK_ADSB_DRAWER_HOST_TEST

#include "pfd_layout.h"     /* PK_UI_PAD_L / PK_UI_TITLE_* 版面 token */

#define DRW_PUTS(fb, x, y, s, col, sz) \
        pk_aa_puts((fb), PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), (sz))

/* 键与值的最小间隙。10 > 列表页的列间距标准 7（SEP_GAP）：列与列之间隔的是
 * 一条分隔线，键与值之间什么都没有，靠留白分组就得多给一点。 */
#define KEY_GAP  10
_Static_assert(KEY_GAP >= 7, "键值间距不得低于列表页列间距标准(7)");

/* 值的排版：放不下先降字号（M→S→XS），再放不下才截断 + ".."。自 adsb_list.c
 * 的 put_val 逐字平移（唯一使用者是抽屉）——宽度一律问渲染器（中文按码点），
 * UTF-8 按码点步进，余量按最坏 4 字节码点留。 */
static void put_val(uint16_t *fb, int x, int y_top, int avail,
                    const char *val, uint16_t col)
{
    pk_aa_size_t sz = PK_AA_M;
    if (pk_aa_text_width(val, PK_AA_M) > avail)
        sz = (pk_aa_text_width(val, PK_AA_S) <= avail) ? PK_AA_S : PK_AA_XS;

    const int y = y_top + (PK_AA_M_H - pk_aa_cell_h(sz)) / 2;

    if (pk_aa_text_width(val, sz) <= avail) {
        DRW_PUTS(fb, x, y, val, col, sz);
        return;
    }

    const int dots = pk_aa_text_width("..", sz);
    char tmp[64];
    size_t out = 0;
    int w = 0;
    for (const char *p = val; *p && out + 4 + 3 <= sizeof(tmp); ) {
        /* UTF-8 首字节决定这个码点占几字节；续字节一律 10xxxxxx。 */
        int n = 1;
        const unsigned char c = (unsigned char)*p;
        if      ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        char one[5];
        memcpy(one, p, (size_t)n);
        one[n] = '\0';
        const int cw = pk_aa_text_width(one, sz);
        if (w + cw + dots > avail) break;
        memcpy(tmp + out, one, (size_t)n);
        out += (size_t)n;
        w   += cw;
        p   += n;
    }
    /* 一个码点都放不下也要给个交代，否则那一行整个空掉。 */
    if (out == 0) { DRW_PUTS(fb, x, y, "..", col, sz); return; }
    tmp[out++] = '.';
    tmp[out++] = '.';
    tmp[out]   = '\0';
    DRW_PUTS(fb, x, y, tmp, col, sz);
}

/* 一枚按钮：圆角底 + 居中文字。照抄 apt_detail_page.c:479 的 draw_button
 * （同一套视觉语言，各页各留一份 static 函数）。 */
static void draw_button(uint16_t *fb, int x0, int y0, int w, int h,
                        const char *label, uint16_t bg, uint16_t fg,
                        pk_aa_size_t size)
{
    pk_pfd_fill_round_rect(fb, x0, y0, x0 + w, y0 + h, 10, bg);
    const int tw = pk_aa_text_width(label, size);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               x0 + (w - tw) / 2, y0 + (h - pk_aa_cell_h(size)) / 2,
               label, fg, size);
}

void pk_adsb_drawer_render(uint16_t *fb, const aircraft_t *ac, int age_s)
{
    /* 调色随版面一起平移：键=列表页 COL_TITL，值=COL_HDR。 */
    const uint16_t col_key = pk_rgb565(125, 140, 160);
    const uint16_t col_val = pk_rgb565(235, 235, 235);

    pk_pfd_fill_rect(fb, 0, PK_ADSB_DRAWER_TOP, PK_DISPLAY_W, PK_DISPLAY_H,
                     pk_rgb565(14, 20, 30));
    /* 顶边一条亮线，把抽屉和它盖住的内容分开——没有这条线，抽屉看起来像是
     * 底下的又一行，而它其实是浮在上面的。 */
    pk_pfd_fill_rect(fb, 0, PK_ADSB_DRAWER_TOP, PK_DISPLAY_W,
                     PK_ADSB_DRAWER_TOP + 2, pk_rgb565(70, 130, 190));

    char cs[AIRCRAFT_CALLSIGN_LEN];
    pk_callsign_display(ac->have_callsign, ac->callsign,
                        pk_aircraft_registration(ac->icao24), ac->icao24,
                        cs, sizeof(cs));

    /* 标题行：呼号用 L 档——抽屉一次只讲一架飞机，它是这块区域的主语。 */
    DRW_PUTS(fb, PK_UI_PAD_L + 8, PK_ADSB_DRAWER_TOP + 14, cs, col_val, PK_AA_L);

    const char *tag = pk_adsb_emergency_tag(ac);
    if (tag) {
        const int tx = PK_UI_PAD_L + 8 + pk_aa_text_width(cs, PK_AA_L) + 12;
        const int tw = pk_aa_text_width(tag, PK_AA_S);
        pk_pfd_fill_rect(fb, tx - 5, PK_ADSB_DRAWER_TOP + 16, tx + tw + 5,
                         PK_ADSB_DRAWER_TOP + 16 + PK_AA_S_H + 6,
                         pk_rgb565(220, 40, 40));
        DRW_PUTS(fb, tx, PK_ADSB_DRAWER_TOP + 19, tag,
                 pk_rgb565(255, 255, 255), PK_AA_S);
    }

    /* 两列键值。键用 XS 暗色、值用 M——扫的时候眼睛只需要抓值。
     * 右列起点 404：左列最长是机型全称，右列是航司全称（"China Southern
     * Airlines"），宽的这 16 px 从宽裕的一侧还给紧张的一侧（平移前的同一条
     * 版面账）。 */
    const int col_x[2]  = { PK_UI_PAD_L + 8, 404 };
    const int y0        = PK_ADSB_DRAWER_TOP + 62;
    const int line_h    = 34;
    const int content_r = PK_DISPLAY_W - PK_UI_PAD_L;

    /* 键存词条 id 而不是字符串：语言能在运行时切，static 的字符串表会停在
     * 旧语言上。 */
    static const pk_tr_id_t keys[2][4] = {
        { PK_TR_LIST_D_ICAO,    PK_TR_LIST_D_REG,     PK_TR_LIST_D_TYPE,
          PK_TR_LIST_D_MODEL },
        { PK_TR_LIST_D_AIRLINE, PK_TR_LIST_D_COUNTRY, PK_TR_LIST_D_SQUAWK,
          PK_TR_LIST_D_LAST_SEEN },
    };
    /* 键列宽按**本列最长的那条键**实测算，不写死（中英文键宽差一倍，
     * 写死必在某一头翻车——平移前的同一条教训）。 */
    int key_w[2];
    for (int c = 0; c < 2; ++c) {
        int w = 0;
        for (int i = 0; i < 4; ++i) {
            const int kw = pk_aa_text_width(pk_i18n_text(keys[c][i]), PK_AA_XS);
            if (kw > w) w = kw;
        }
        key_w[c] = w + KEY_GAP;
    }

    char v[8][48];

    snprintf(v[0], sizeof(v[0]), "%06lX", (unsigned long)ac->icao24);
    const char *reg = pk_aircraft_registration(ac->icao24);
    snprintf(v[1], sizeof(v[1]), "%s", (reg && reg[0]) ? reg : "---");
    const char *tc = pk_aircraft_type_code(ac->icao24);
    snprintf(v[2], sizeof(v[2]), "%s", (tc && tc[0]) ? tc : "---");
    const char *tm = pk_aircraft_type_model(ac->icao24);
    snprintf(v[3], sizeof(v[3]), "%s", (tm && tm[0]) ? tm : "---");

    /* 航司按呼号前三字母查。查不到不是错误：尾号呼号、军用呼号本来就不带
     * 航司前缀。 */
    const char *fnum = NULL;
    const pk_airline_t *al = ac->have_callsign
                           ? pk_airline_from_callsign(ac->callsign, &fnum) : NULL;
    snprintf(v[4], sizeof(v[4]), "%s", al ? al->name : "---");

    const pk_country_t *cn = pk_country_from_icao24(ac->icao24);
    snprintf(v[5], sizeof(v[5]), "%s", cn ? cn->name : "---");

    if (ac->have_squawk) snprintf(v[6], sizeof(v[6]), "%04d", ac->squawk);
    else                 snprintf(v[6], sizeof(v[6]), "---");

    snprintf(v[7], sizeof(v[7]), "%d %s", age_s,
             pk_i18n_text(PK_TR_LIST_D_AGO));

    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 4; ++i) {
            const int y = y0 + i * line_h;
            DRW_PUTS(fb, col_x[c], y + (PK_AA_M_H - PK_AA_XS_H) / 2,
                     pk_i18n_text(keys[c][i]), col_key, PK_AA_XS);
            const char *val = v[c * 4 + i];
            const int avail = (c == 0 ? col_x[1] - 16 : content_r)
                            - col_x[c] - key_w[c];
            put_val(fb, col_x[c] + key_w[c], y, avail, val, col_val);
        }
    #undef KEY_GAP

    /* 绑定为本机按钮：未绑定该机 → 蓝底「绑定为本机」；已绑定 → 绿底「取消
     * 绑定」。own 态按目标机现查（ui_state 是唯一真源）。 */
    {
        const bool is_own = (pk_ui_get_own_icao() == ac->icao24);
        const pk_tr_id_t label = is_own ? PK_TR_LIST_D_UNBIND
                                        : PK_TR_LIST_D_BIND;
        const uint16_t bg = is_own ? pk_rgb565( 60, 160,  80)   /* 绿：已绑定 */
                                   : pk_rgb565( 40, 100, 200);   /* 蓝：可绑定 */
        draw_button(fb, PK_ADSB_DRAWER_BIND_X0, PK_ADSB_DRAWER_BIND_Y0,
                    PK_ADSB_DRAWER_BIND_W, PK_ADSB_DRAWER_BIND_H,
                    pk_i18n_text(label), bg, pk_rgb565(255, 255, 255),
                    PK_AA_S);
    }
}
#endif /* !PK_ADSB_DRAWER_HOST_TEST */
