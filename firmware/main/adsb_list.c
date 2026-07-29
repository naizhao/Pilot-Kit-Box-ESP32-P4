/*
 * adsb_list.c — 空管看板：当前跟踪到的全部 ADS-B 目标，一行一架。
 *
 * 与交通页的分工
 * --------------
 * 交通页回答「谁会撞上我」——极坐标、只画量程内、信息压到最少。
 * 本页回答「天上现在都有谁」——表格、全部目标、每架把能解出来的字段摊开。
 * 同一份 aircraft_state 快照，两种读法，所以两页的**排序、配色、箭头语汇
 * 必须一致**，否则同一架飞机在两页看起来像两架。
 *
 * 布局（800 × 480，spec §5.3）
 * ---------------------------
 *   y   0..48    顶栏（与 PFD / 交通页同高）
 *   y  48..78    列标题
 *   y  78..462   数据行 8 × 48
 *   y 462..480   底部：滚动位置
 *
 * spec 写的是 7 行 × 48 px。实测 78 + 8×48 = 462 正好落在屏内，多一行就多
 * 一架能一眼看到的飞机，故取 8 行——spec 的行数是估算，屏高是硬的。
 *
 * 为什么单行而不是交通页那种两行卡片
 * ----------------------------------
 * 右栏卡片只有 260 px 宽，塞不下七列，只能折两行。这里有 768 px，七列一行
 * 排得开；而表格的价值恰恰在于**同一列纵向可比**——高度、距离、速度扫一眼
 * 就能排出大小，折行会把这个能力毁掉。
 */

#include "adsb_list.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_attr.h"
#include "esp_timer.h"

#include "aircraft_state.h"
#include "baro.h"
#include "display.h"
#include "imu_task.h"
#include "mag_var.h"
#include "own_ship.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_icon_font.h"
#include "pfd_layout.h"
#include "traffic_geom.h"
#include "ui_state.h"

/* ── 版面 ───────────────────────────────────────────────────── */

#define LST_TOP       PFD_BAR_BOT              /* 顶栏下沿 */
#define HDR_H         30                       /* 列标题行 */
#define ROW0_Y        (LST_TOP + HDR_H)
#define ROW_H         48
#define ROW_N         8
#define LIST_BOT      (ROW0_Y + ROW_N * ROW_H) /* 462 */

#define PAD_L         16
#define PAD_R         16

/*
 * 列的 x 与宽度。
 *
 * 数字列一律**右对齐**：右对齐后个位数纵向落在同一条线上，扫一眼就能比出
 * 16700 和 9900 谁高；左对齐则要逐个数位数。呼号是文本，左对齐。
 *
 *   BRG   CALL        DIST     ALT      V/S      GS     TRK
 *   ↗045  CES2116W    12.3    16700    ↑1500    395    011°
 */
/* 右缘退到 FAB 之外：FAB 直径 56、右边距 16（pk_ui_nav.c），左缘落在 728。
 * 初版把 TRK 列右对齐到 784，结果 FAB 正好盖住那一行的航向——七列信息密度
 * 这么高的表格，最右列被遮掉一个数就得靠猜。
 *
 * 不做「FAB 在哪边表格就往另一边让」的动态版面：FAB 可拖动，版面会跟着跳，
 * 而表格列位一旦会动，纵向对齐这个唯一的好处就没了。 */
#define FAB_LEFT_EDGE (PK_DISPLAY_W - 16 - 56)       /* 728 */
#define CONTENT_R     (FAB_LEFT_EDGE - 12)           /* 716 */

/*
 * 八列的 x。宽度按各列**最长可能内容**算，间隙统一 16 px：
 *
 *   BRG 箭头26+3位45=71   CALL 8字符×15=120   FLAG 徽章 XS 5字符=50
 *   DIST "12.3"=60        ALT "34322"=75      V/S 箭头12+4位60=72
 *   GS "450"=45           TRK "355"=45        SEEN "47s" XS=30
 *
 * 加上 7 个间隙正好落在 716 内。加 SEEN 之前间隙是 23 px，看着更松快，但
 * 「刚才还在、现在多久没消息了」比那点呼吸感值钱——尤其它是判断一条数据
 * 还能不能信的唯一依据。
 *
 * SEEN 用 XS 档：它是元数据（这行有多新），不是飞行数据，不该和高度速度
 * 抢同一个视觉层级。
 */
#define COL_BRG_X     PAD_L                          /*  16 */
#define COL_CALL_X    103
/* 紧急码徽章自成一列，不跟在呼号后面浮动。
 * 初版让它紧跟呼号，8 字符满宽呼号 + "NO RADIO" 直接压进了 DIST 列，把
 * 距离盖掉一半——表格里任何"跟着内容长度走"的元素迟早会撞上邻列。 */
#define COL_FLAG_X    239
#define COL_DIST_R    365                            /* 右对齐基准 */
#define COL_ALT_R     456
#define COL_VS_R      544
#define COL_GS_R      605
#define COL_TRK_R     666
#define COL_SEEN_R    CONTENT_R                      /* 716 */

#define LST_PUTS(fb, x, y, s, col, sz) \
        pk_aa_puts((fb), PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), (sz))

/*
 * 列表：标题文字、绘制位置、点击命中区、排序键——四件事写在同一行。
 *
 * 分开写迟早会飘：改一次列位就得记得同步改命中区，而漏改的表现是「点标题
 * 排错了列」，在真机上极难看出来（点 ALT 结果按 GS 排了，数字都还在动）。
 *
 * right_align 与数据行的对齐方式一致：数字列右对齐，文本列左对齐，标题跟着
 * 数据走，否则标题和它管的那列对不上。
 */
typedef enum {
    SORT_BRG = 0, SORT_CALL, SORT_DIST, SORT_ALT, SORT_VS, SORT_GS, SORT_TRK,
    SORT_SEEN,
    SORT_COUNT
} sort_key_t;

typedef struct {
    const char *title;
    int         x;            /* right_align ? 右缘 : 左缘 */
    bool        right_align;
    int         hit_x0, hit_x1;
} col_def_t;

/* 命中区把列间空隙一并吃掉：手指落在两列标题中间时总得归给某一列，
 * 留缝隙只会让人以为「点了没反应」。 */
static const col_def_t COLS[SORT_COUNT] = {
    [SORT_BRG]  = { "BRG",      COL_BRG_X,  false,  PAD_L - 8, 95  },
    [SORT_CALL] = { "CALLSIGN", COL_CALL_X, false,  95,        231 },
    /* FLAG 不排序（徽章有无不构成一种顺序），命中区并进 DIST——中间夹一个
     * 点了没反应的列，比少一个可点的列更让人困惑。 */
    [SORT_DIST] = { "DIST",     COL_DIST_R, true,   231,       359 },
    [SORT_ALT]  = { "ALT",      COL_ALT_R,  true,   359,       450 },
    [SORT_VS]   = { "V/S",      COL_VS_R,   true,   450,       544 },
    [SORT_GS]   = { "GS",       COL_GS_R,   true,   544,       611 },
    [SORT_TRK]  = { "TRK",      COL_TRK_R,  true,   611,       678 },
    /* 标题写 AGE 不写 SEEN：SEEN 是 4 字符 40 px，比它管的那列数据
     * （"47s" = 30 px）还宽，右对齐后会顶到左边那条线上。 */
    [SORT_SEEN] = { "AGE",      COL_SEEN_R, true,   678,       CONTENT_R + 8 },
};

/* 排序状态。默认距离升序——最近的威胁排最前，这是打开这一页最常见的意图。
 *
 * 不落 NVS：排序是「我现在想这么看」的临时视角，不是设置。下次开机回到
 * 距离升序才是对的默认，把上次点的 TRK 降序记住反而会让人困惑。 */
static sort_key_t s_sort   = SORT_DIST;
static bool       s_sort_desc;

/* 顶栏右上角：排序说明 + RESET 按钮的位置。RESET 贴 CONTENT_R 右缘，
 * 说明文字排在它左边。 */
#define RESET_X1      (CONTENT_R + 8)
#define RESET_X0      (RESET_X1 - 66)
#define SORT_LBL_X    (RESET_X0 - 150)
#define HDR_HIT_RESET SORT_COUNT      /* s_hdr_down 里给 RESET 留的编号 */

#define SORT_DEFAULT_KEY  SORT_DIST

static bool sort_is_default(void)
{
    return s_sort == SORT_DEFAULT_KEY && !s_sort_desc;
}

#ifdef PK_SIM_BUILD
/* 模拟器专用：用环境变量摆布排序状态，好把各列各方向都截出来核对。
 * 固件不编译这段——排序只该由手指改。 */
#include <stdlib.h>
static void sim_sort_override(void)
{
    const char *k = getenv("PK_SIM_SORT");
    if (k) { int v = atoi(k); if (v >= 0 && v < SORT_COUNT) s_sort = (sort_key_t)v; }
    const char *d = getenv("PK_SIM_SORT_DESC");
    if (d) s_sort_desc = (d[0] == '1');
}
#endif
static int        s_hdr_down = -1;   /* 按下高亮的列，-1 = 无 */

/* 右对齐：先量宽再倒推起点。CJK/箭头是宽字形，这里的列全是 ASCII，
 * 用 cell_w × 字符数即可（等宽字体）。 */
static void puts_right(uint16_t *fb, int right_x, int y, const char *s,
                       uint16_t col, pk_aa_size_t sz)
{
    const int w = (int)strlen(s) * pk_aa_cell_w(sz);
    LST_PUTS(fb, right_x - w, y, s, col, sz);
}

/* 八向箭头，与交通页同一张表——同一个方位在两页必须长得一样。 */
static const char *bearing_arrow(float rel_deg)
{
    static const char *kArrow[8] = {
        "↑", "↗", "→", "↘",
        "↓", "↙", "←", "↖",
    };
    float d = rel_deg;
    while (d < 0.0f)    d += 360.0f;
    while (d >= 360.0f) d -= 360.0f;
    return kArrow[((int)((d + 22.5f) / 45.0f)) & 7];
}

/*
 * 紧急应答机码 → 短标签。无则返回 NULL。
 *
 * spec §5.3 把 SQK 归到详情抽屉里。三个国际通用紧急码是例外，必须留在主表：
 *
 *   7500  劫机   7600  通信失效   7700  一般紧急
 *
 * 它们是「这架飞机现在有事」的最强信号，而抽屉一次只能开一架——真出事时
 * 要挨个点开十几行才能找到那一架，等于没有。放在呼号右边，扫一眼就到。
 *
 * 只认这三个，不显示普通 SQK：普通码（1200、2000、管制分配的四位数）在
 * 主表里既不影响决策也占地方，那才是抽屉该干的活。
 */
static const char *emergency_tag(const aircraft_t *a)
{
    if (!a->have_squawk) return NULL;
    switch (a->squawk) {
    /* 三个都压到 3 字符：徽章列只有 30 px（见列位那段的宽度账），
     * NORDO 这种 5 字符的标准缩写塞不下，只能退到 RDO。 */
    case 7500: return "HJK";   /* 劫机 */
    case 7600: return "RDO";   /* 通信失效 */
    case 7700: return "EMG";   /* 一般紧急 */
    default:   return NULL;
    }
}

/* 呼号；没广播过就退回 ICAO 十六进制。留空会被当成渲染坏了。 */
static void callsign_of(const aircraft_t *a, char *out, size_t cap)
{
    if (a->have_callsign && a->callsign[0]) {
        size_t j = 0;
        for (size_t i = 0; i < sizeof(a->callsign) && a->callsign[i]; ++i)
            if (a->callsign[i] != ' ' && j + 1 < cap) out[j++] = a->callsign[i];
        out[j] = '\0';
        if (j) return;
    }
    snprintf(out, cap, "%06lX", (unsigned long)a->icao24);
}

/* 气压 → 1013.25 标准高度(ft)，与目标 Mode-C 同基准（同 traffic_page）。 */
static int std_alt_ft_from_pa(float pa)
{
    float alt_m = 44330.0f * (1.0f - powf(pa / 101325.0f, 0.190295f));
    return (int)lroundf(alt_m * 3.28084f);
}

/* 一行的数据 + 算好的相对几何。 */
typedef struct {
    aircraft_t       *ac;
    pk_traffic_rel_t  rel;
    int               age_s;   /* 距上次收到报文的秒数，0..60 */
} row_t;

/*
 * 威胁判据：**同高度且靠得近**。
 *
 * 高度差 ±1000 ft 是全项目统一的「同高度」阈值（PFD 交通标签、交通页高度差
 * 都用它）；再叠一个 5 NM 的距离闸门，否则 60 NM 外的同高度飞机也会标红，
 * 整屏红光一片——那等于没有告警。
 *
 * 没有相对高度数据时**不标红**：不知道高度差就不知道是不是威胁，标红是在
 * 编造信息，而红色在座舱里意味着「立刻处理」。
 */
static bool is_threat(const pk_traffic_rel_t *r)
{
    return r->valid && r->rel_alt_valid &&
           r->rel_alt_ft > -1000 && r->rel_alt_ft < 1000 &&
           r->dist_nm < 5.0f;
}

/*
 * 排序键取值。缺数据统一映射到一个哨兵值，让它**恒沉到最底**——不管升序
 * 还是降序。
 *
 * 这一点不能靠「缺数据给极大值」糊弄：升序时它确实沉底，一旦点成降序就全
 * 浮到最上面，整屏都是 --- 的行，能用的信息反而被挤出屏幕。所以缺数据要在
 * 比较函数里单独判，不参与方向翻转。
 */
static bool row_has_key(const row_t *r, sort_key_t k)
{
    switch (k) {
    case SORT_BRG:
    case SORT_DIST: return r->rel.valid;
    case SORT_ALT:  return r->ac->have_altitude;
    case SORT_VS:
    case SORT_GS:
    case SORT_TRK:  return r->ac->have_velocity;
    case SORT_SEEN:                          /* last_seen 恒有值——能进快照就说明收到过 */
    case SORT_CALL: default: return true;   /* 呼号总有值（退回 ICAO hex） */
    }
}

static float row_key(const row_t *r, sort_key_t k)
{
    switch (k) {
    case SORT_BRG:  return r->rel.abs_bearing;
    case SORT_DIST: return r->rel.dist_nm;
    case SORT_ALT:  return (float)r->ac->altitude_ft;
    case SORT_VS:   return (float)r->ac->vert_rate_fpm;
    case SORT_GS:   return (float)r->ac->ground_speed_kt;
    case SORT_TRK:  return (float)(r->ac->heading_deg % 360);
    case SORT_SEEN: return (float)r->age_s;
    default:        return 0.0f;
    }
}

/* a 是否该排在 b 前面。 */
static bool row_less(const row_t *a, const row_t *b)
{
    const bool ha = row_has_key(a, s_sort), hb = row_has_key(b, s_sort);
    if (ha != hb) return ha;              /* 有数据的永远在前 */
    if (!ha) return false;                /* 都没有，保持原序 */

    if (s_sort == SORT_CALL) {
        /* 呼号按字典序。两架都没广播呼号时比的是 ICAO hex，正好也是稳定的。 */
        char ca[AIRCRAFT_CALLSIGN_LEN], cb[AIRCRAFT_CALLSIGN_LEN];
        callsign_of(a->ac, ca, sizeof(ca));
        callsign_of(b->ac, cb, sizeof(cb));
        const int d = strcmp(ca, cb);
        return s_sort_desc ? d > 0 : d < 0;
    }
    const float ka = row_key(a, s_sort), kb = row_key(b, s_sort);
    return s_sort_desc ? ka > kb : ka < kb;
}

static void draw_header(uint16_t *fb, int n, uint16_t col_hdr, uint16_t col_dim)
{
    const int ty = (PFD_BAR_BOT - PK_AA_M_H) / 2;
    LST_PUTS(fb, PAD_L, ty, "AIRCRAFT", col_hdr, PK_AA_M);

    /* 目标数用 PFD 状态栏那枚 connecting_airports——同一台设备上「ADS-B 目标
     * 数」只该有一个符号。绿=有效在线，与状态栏、交通页一致。 */
    const uint16_t COL_ADSB = pk_rgb565(0, 220, 60);
    const uint8_t *ic = pk_icon_bitmap[pk_aa_get_weight()]
                      + (size_t)PK_ICON_ADSB
                        * (((size_t)PK_ICON_W * PK_ICON_H + 1) / 2);
    pk_aa_blit_4bpp(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                    200, (PFD_BAR_BOT - PK_ICON_H) / 2,
                    ic, PK_ICON_W, PK_ICON_H, COL_ADSB);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", n);
    LST_PUTS(fb, 200 + PK_ICON_W + 6, ty, buf, COL_ADSB, PK_AA_M);

    /*
     * 右上角：当前排序 + 重置。
     *
     * 曾经写死过一行 "SORT BY DIST"，表头改成可点之后它就成了假的。现在跟着
     * 排序状态走：`SORT DIST↑`。列标题上的高亮说的是「哪一列」，这里说的是
     * 「现在整张表是按什么排的」——扫顶栏一眼就知道，不必回头找哪个标题亮着。
     *
     * 非默认排序时右边额外冒出 RESET：一路点下来换了好几列之后，想回
     * 到「最近的排最前」得记住原来是哪列、什么方向，不如给一个回原点的按钮。
     * 默认状态下它不出现——一个按下去什么都不变的按钮只会让人怀疑没点中。
     */
    char sbuf[24];
    snprintf(sbuf, sizeof(sbuf), "SORT %s%s",
             COLS[s_sort].title, s_sort_desc ? "↓" : "↑");
    /* 箭头是 3 字节 UTF-8 但只占一个字形宽，strlen 会多算 2 个 cell。 */
    const int sw = ((int)strlen(sbuf) - 2) * pk_aa_cell_w(PK_AA_S)
                 - pk_aa_cell_w(PK_AA_S) + PK_AA_S_CJK_W;
    LST_PUTS(fb, SORT_LBL_X, (PFD_BAR_BOT - PK_AA_S_H) / 2, sbuf,
             col_dim, PK_AA_S);
    (void)sw;

    if (!sort_is_default()) {
        const uint16_t COL_RST = pk_rgb565(255, 210, 60);
        const int ty2 = (PFD_BAR_BOT - PK_AA_XS_H) / 2;
        if (s_hdr_down == HDR_HIT_RESET)
            pk_pfd_darken_rect(fb, RESET_X0, 4, RESET_X1, PFD_BAR_BOT - 4, 60);
        LST_PUTS(fb, RESET_X0 + 8, ty2, "RESET", COL_RST, PK_AA_XS);
    }
}

/*
 * 列标题 = 排序按钮。
 *
 * 当前排序列高亮 + 跟一个方向箭头，其余列暗色。这是表格的通用语汇（表格
 * 软件、网页表头都这样），不必再额外写一行「点标题可排序」的说明——那行
 * 说明本身就会占掉一条数据行的位置。
 *
 * 按下时整列标题铺一层亮底：10 fps 下没有即时反馈的话，点下去像是没反应，
 * 手指会不自觉地再点一次，于是排序方向被翻了两次又转回原样。
 */
static void draw_col_titles(uint16_t *fb, uint16_t col_dim, uint16_t col_hi)
{
    const int ty = LST_TOP + (HDR_H - PK_AA_XS_H) / 2;

    for (int k = 0; k < SORT_COUNT; ++k) {
        const col_def_t *c = &COLS[k];
        const bool active = ((sort_key_t)k == s_sort);

        if (k == s_hdr_down)
            pk_pfd_darken_rect(fb, c->hit_x0, LST_TOP, c->hit_x1, ROW0_Y - 1, 60);

        const uint16_t cc = active ? col_hi : col_dim;
        /* 箭头贴在标题外侧：左对齐列跟在后面，右对齐列因为要保住右缘对齐，
         * 得把标题整体左推一个箭头宽再画。 */
        const char *ar = !active ? NULL : (s_sort_desc ? "↓" : "↑");
        const int arw = ar ? PK_AA_XS_CJK_W : 0;

        if (c->right_align) {
            puts_right(fb, c->x - arw, ty, c->title, cc, PK_AA_XS);
            if (ar) LST_PUTS(fb, c->x - arw, ty, ar, cc, PK_AA_XS);
        } else {
            const int w = (int)strlen(c->title) * pk_aa_cell_w(PK_AA_XS);
            LST_PUTS(fb, c->x, ty, c->title, cc, PK_AA_XS);
            if (ar) LST_PUTS(fb, c->x + w + 2, ty, ar, cc, PK_AA_XS);
        }
    }
}

/*
 * 列分隔线。只画在**数据区**，表头一条不画。
 *
 * 八列挤进 700 px 后列间距只剩 17 px，"0 340 270" 会连成一片、眼睛串列，
 * 竖线是不占宽度的分组手段。但表头不能跟着画：标题比数据长（"CALLSIGN"
 * 八个字符），带排序箭头的列还要多挂 12 px，8 px 留白根本不够——真机上
 * DIST 的箭头、ALT 的 T、V/S 的 S 全压在线上。表头靠高亮区分排序列，本来
 * 就不需要线。
 *
 * 颜色压到几乎贴着背景：它是辅助线，一旦比数据显眼就成了干扰。初版取
 * (30,38,50)，在模拟器里几乎看不见，真机上却是明晃晃的亮蓝——这块 4.3″
 * 是普通 IPS，不是 2.4″ 那块半透屏，暗色不会被环境光冲淡。
 *
 * rows 是本屏实际画了几行：空列表时一条都不画，否则满屏竖线会穿过
 * "NO CONTACTS"。
 */
static void draw_col_seps(uint16_t *fb, int rows)
{
    if (rows <= 0) return;
    const uint16_t COL_SEP = pk_rgb565(20, 26, 36);
    const int y0 = ROW0_Y + 2, y1 = ROW0_Y + rows * ROW_H - 4;

    for (int k = 1; k < SORT_COUNT; ++k)
        pk_pfd_fill_rect(fb, COLS[k].hit_x0 - 1, y0, COLS[k].hit_x0, y1, COL_SEP);
    /* 徽章列的线单独补：它不在 COLS 里（不参与排序），视觉上仍是一列。 */
    pk_pfd_fill_rect(fb, COL_FLAG_X - 9, y0, COL_FLAG_X - 8, y1, COL_SEP);
}

static void draw_row(uint16_t *fb, const row_t *r, int y0, bool sel)
{
    const uint16_t COL_TXT  = pk_rgb565(235, 240, 248);
    const uint16_t COL_DIM  = pk_rgb565(150, 162, 180);
    const uint16_t COL_SEL  = pk_rgb565(255, 210,  60);
    const uint16_t COL_ARR  = pk_rgb565(  0, 210, 235);
    const uint16_t COL_UP   = pk_rgb565( 90, 220, 120);
    const uint16_t COL_DOWN = pk_rgb565(255, 170,  70);
    const uint16_t COL_THR  = pk_rgb565(120,  26,  26);   /* 威胁行底 */

    const aircraft_t *a = r->ac;
    const bool threat = is_threat(&r->rel);

    /* 行底：威胁红底 > 选中亮底 > 斑马纹。
     * 斑马纹不是装饰——七列横跨 768 px，没有底色区分时视线很容易串行。 */
    if (threat)
        pk_pfd_fill_rect(fb, PAD_L - 8, y0, CONTENT_R + 8,
                         y0 + ROW_H - 2, COL_THR);
    else
        pk_pfd_darken_rect(fb, PAD_L - 8, y0, CONTENT_R + 8,
                           y0 + ROW_H - 2, sel ? 110 : 200);

    /* 选中行加一条左侧色带。只靠底色深浅在阳光下的半透屏上分不出来。 */
    if (sel) pk_pfd_fill_rect(fb, PAD_L - 8, y0, PAD_L - 4, y0 + ROW_H - 2, COL_SEL);

    const uint16_t ctxt = sel ? COL_SEL : COL_TXT;
    const int ty = y0 + (ROW_H - 2 - PK_AA_M_H) / 2;
    char buf[24];

    /* ── BRG：箭头 + 相对方位角 ──
     * 箭头给「大概哪个方向」，数字给「精确多少度」。只给数字要在脑子里换算，
     * 只给箭头则 8 个方向不够用来引导目视搜索。 */
    if (r->rel.valid) {
        LST_PUTS(fb, COL_BRG_X, ty, bearing_arrow(r->rel.rel_bearing),
                 sel ? COL_SEL : COL_ARR, PK_AA_M);
        snprintf(buf, sizeof(buf), "%03d",
                 ((int)lroundf(r->rel.abs_bearing) % 360 + 360) % 360);
        LST_PUTS(fb, COL_BRG_X + 26, ty, buf, ctxt, PK_AA_M);
    } else {
        LST_PUTS(fb, COL_BRG_X, ty, "---", COL_DIM, PK_AA_M);
    }

    /* ── CALLSIGN ── */
    char cs[AIRCRAFT_CALLSIGN_LEN];
    callsign_of(a, cs, sizeof(cs));
    LST_PUTS(fb, COL_CALL_X, ty, cs, ctxt, PK_AA_M);

    /* 紧急码标签：红底白字的小徽章，紧跟呼号。
     * 用 XS 档而不是正文档——它要显眼，但不能挤掉呼号本身的位置；红底已经
     * 提供了足够的对比，字号再大就是喧宾夺主。 */
    const char *tag = emergency_tag(a);
    if (tag) {
        const uint16_t COL_EMG = pk_rgb565(220, 40, 40);
        const int tx = COL_FLAG_X;
        const int tw = (int)strlen(tag) * pk_aa_cell_w(PK_AA_XS);
        const int tyy = y0 + (ROW_H - 2 - PK_AA_XS_H) / 2;
        pk_pfd_fill_rect(fb, tx - 4, tyy - 3, tx + tw + 3, tyy + PK_AA_XS_H + 2,
                         COL_EMG);
        LST_PUTS(fb, tx, tyy, tag, pk_rgb565(255, 255, 255), PK_AA_XS);
    }

    /* ── DIST ── */
    if (r->rel.valid) snprintf(buf, sizeof(buf), "%.1f", r->rel.dist_nm);
    else              snprintf(buf, sizeof(buf), "---");
    puts_right(fb, COL_DIST_R, ty, buf, r->rel.valid ? ctxt : COL_DIM, PK_AA_M);

    /* ── ALT：绝对气压高度 ──
     * 这里给绝对值而不是交通页那个相对差：看板是「天上都有谁」的视角，绝对
     * 高度才能和管制指令、航路高度层对上；相对差在交通页已经有了。 */
    if (a->have_altitude) snprintf(buf, sizeof(buf), "%d", a->altitude_ft);
    else                  snprintf(buf, sizeof(buf), "---");
    puts_right(fb, COL_ALT_R, ty, buf, a->have_altitude ? ctxt : COL_DIM, PK_AA_M);

    /* ── V/S：箭头分色 + 数值 ──
     * 与交通页同规：爬升绿、下降橙。±200 fpm 内算平飞，不画箭头——ADS-B 的
     * 升降率本身有噪声，几十 fpm 的抖动画成箭头是在报告不存在的机动。 */
    if (a->have_velocity && a->vert_rate_fpm > 200) {
        snprintf(buf, sizeof(buf), "%d", a->vert_rate_fpm);
        const int w = (int)strlen(buf) * pk_aa_cell_w(PK_AA_M);
        puts_right(fb, COL_VS_R, ty, buf, ctxt, PK_AA_M);
        LST_PUTS(fb, COL_VS_R - w - PK_AA_M_CJK_W, ty, "↑",
                 sel ? COL_SEL : COL_UP, PK_AA_M);
    } else if (a->have_velocity && a->vert_rate_fpm < -200) {
        snprintf(buf, sizeof(buf), "%d", -a->vert_rate_fpm);
        const int w = (int)strlen(buf) * pk_aa_cell_w(PK_AA_M);
        puts_right(fb, COL_VS_R, ty, buf, ctxt, PK_AA_M);
        LST_PUTS(fb, COL_VS_R - w - PK_AA_M_CJK_W, ty, "↓",
                 sel ? COL_SEL : COL_DOWN, PK_AA_M);
    } else {
        puts_right(fb, COL_VS_R, ty, a->have_velocity ? "0" : "---",
                   a->have_velocity ? COL_DIM : COL_DIM, PK_AA_M);
    }

    /* ── GS / TRK ──
     * 两者同源（DF17 metype 19），要缺一起缺，所以共用 have_velocity。
     * 缺了显 --- 而不是 0：0 kt 是合法读数（地面/悬停），拿它冒充缺数据
     * 比空着更危险。 */
    if (a->have_velocity) {
        snprintf(buf, sizeof(buf), "%d", a->ground_speed_kt);
        puts_right(fb, COL_GS_R, ty, buf, ctxt, PK_AA_M);
        snprintf(buf, sizeof(buf), "%03d", a->heading_deg % 360);
        puts_right(fb, COL_TRK_R, ty, buf, ctxt, PK_AA_M);
    } else {
        puts_right(fb, COL_GS_R,  ty, "---", COL_DIM, PK_AA_M);
        puts_right(fb, COL_TRK_R, ty, "---", COL_DIM, PK_AA_M);
    }

    /* ── SEEN：上次收到报文距今多少秒 ──
     * 这一列决定「上面那七列还能不能信」。ADS-B 目标掉出覆盖后，其余字段会
     * 原样停在最后一次的读数上——看不出它是在飞还是早就没消息了。
     *
     * 超过 15 s 转暗、30 s 转橙：60 s 才会被踢出快照，中间这段最危险，因为
     * 数据看起来仍然完整。用 XS 档，它是元数据不是飞行数据。 */
    const uint16_t COL_STALE = pk_rgb565(255, 170, 70);
    snprintf(buf, sizeof(buf), "%ds", r->age_s);
    const uint16_t cage = sel        ? COL_SEL
                        : r->age_s >= 30 ? COL_STALE
                        : r->age_s >= 15 ? COL_DIM
                                         : COL_TXT;
    puts_right(fb, COL_SEEN_R, y0 + (ROW_H - 2 - PK_AA_XS_H) / 2, buf,
               cage, PK_AA_XS);
}

void pk_adsb_list_render(uint16_t *fb)
{
    const uint16_t COL_BG   = pk_rgb565(  7,  10,  16);
    const uint16_t COL_HDR  = pk_rgb565(235, 235, 235);
    const uint16_t COL_DIM  = pk_rgb565(150, 162, 180);
    const uint16_t COL_TITL = pk_rgb565(125, 140, 160);
    const uint16_t COL_LINE = pk_rgb565( 38,  48,  62);

    /* 注意是 W / H 而不是 W-1 / H-1：pk_pfd_fill_rect 收的是**半开区间**
     * （pfd_draw.c 里 x < x1、y < y1）。写成 -1 会漏掉最右一列和最下一行，
     * 而 DIRECT 渲染下 framebuffer 是复用的，那一列一行就保留着上一帧的内容，
     * 真机上表现为右侧和底部各一条永不刷新的亮线。 */
#ifdef PK_SIM_BUILD
    sim_sort_override();
#endif
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    const int64_t now_us = esp_timer_get_time();
    static aircraft_t s_scratch[AIRCRAFT_TABLE_CAPACITY];
    size_t n = aircraft_state_snapshot(
        s_scratch, AIRCRAFT_TABLE_CAPACITY, now_us, AIRCRAFT_STALE_AGE_US);

    /* ── 本机基准：与 traffic_page 完全同一套取数 ──
     * 两页对同一架飞机算出的方位/距离必须一模一样，所以这里不另起炉灶。 */
    pk_imu_sample_t s;
    const bool have = pk_imu_sample_get(&s);

    aircraft_t own = {0};
    pk_own_src_t src;
    const bool own_valid = pk_own_ship_resolve(
        now_us, (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL, &own, &src);

    pk_baro_state_t baro;
    const bool baro_ok = pk_baro_get(&baro);

    float own_heading = 0.0f, mag_var = 0.0f;
    {
        pk_hdg_src_t hsrc;
        pk_own_heading_resolve(own_valid, src, &own, have,
                               have ? s.yaw_deg : 0.0f, &own_heading, &hsrc);
        if (hsrc == PK_HDG_SRC_IMU && own_valid)
            mag_var = pk_mag_var_lookup(own.lat, own.lon);
    }

    int own_palt;
    if (own_valid && own.have_altitude)      own_palt = own.altitude_ft;
    else if (baro_ok && baro.valid)          own_palt = std_alt_ft_from_pa(baro.pressure_pa);
    else                                     own_palt = PK_ALT_UNAVAIL;

    /* ── 组行 ──
     * 与交通页的关键差别：**没有量程过滤，也不因本机无位置而放弃**。
     * 本机位置未知时方位/距离两列显 ---，但呼号、高度、速度、航向照常显示——
     * 那些字段本来就与本机无关，一起藏掉等于白丢掉大半个页面。 */
    static row_t s_rows[AIRCRAFT_TABLE_CAPACITY];
    static uint32_t s_icaos[AIRCRAFT_TABLE_CAPACITY];
    int nr = 0;

    for (size_t i = 0; i < n; ++i) {
        aircraft_t *t = &s_scratch[i];
        if (own_valid && own.icao24 != 0 && t->icao24 == own.icao24) continue;
        s_rows[nr].ac = t;
        /* 上次收到报文距今多少秒。快照窗口是 60 s（AIRCRAFT_STALE_AGE_US），
         * 所以恒为两位数以内。 */
        {
            const int64_t d = now_us - t->last_seen_us;
            int a = (int)(d / 1000000);
            s_rows[nr].age_s = a < 0 ? 0 : (a > 99 ? 99 : a);
        }
        s_rows[nr].rel = pk_traffic_rel_calc(
            own_valid, own.lat, own.lon, own_heading, mag_var, own_palt,
            t->have_position, t->lat, t->lon,
            t->have_altitude, t->altitude_ft, t->vert_rate_fpm);
        nr++;
    }

    /* 按当前排序列排。n ≤ 64，插入排序足够。 */
    for (int a = 0; a < nr; ++a)
        for (int b = a + 1; b < nr; ++b)
            if (row_less(&s_rows[b], &s_rows[a])) {
                row_t t = s_rows[a]; s_rows[a] = s_rows[b]; s_rows[b] = t;
            }

    for (int k = 0; k < nr; ++k) s_icaos[k] = s_rows[k].ac->icao24;
    const int sel = pk_ui_list_resolve_row(s_icaos, (size_t)nr);

    draw_header(fb, nr, COL_HDR, COL_DIM);
    draw_col_titles(fb, COL_TITL, COL_HDR);
    pk_pfd_fill_rect(fb, 0, ROW0_Y - 1, PK_DISPLAY_W, ROW0_Y, COL_LINE);

    if (nr == 0) {
        /* 空列表也是一种状态，不能留白屏——留白让人以为页面没画出来。 */
        const char *msg = "NO CONTACTS";
        const int w = (int)strlen(msg) * pk_aa_cell_w(PK_AA_L);
        LST_PUTS(fb, (PK_DISPLAY_W - w) / 2, ROW0_Y + 120, msg, COL_DIM, PK_AA_L);
        return;
    }

    /*
     * 滚动窗口：让选中行始终留在屏内，且尽量居中。
     *
     * 不做「选中行走到底才滚一行」那种最小移动——表格里视线是往下扫的，
     * 把选中行钉在中间能同时看到它前后各三四架，比贴边好用。
     */
    int first = sel - ROW_N / 2;
    if (first > nr - ROW_N) first = nr - ROW_N;
    if (first < 0)          first = 0;

    int drawn = 0;
    for (int i = 0; i < ROW_N && first + i < nr; ++i, ++drawn)
        draw_row(fb, &s_rows[first + i], ROW0_Y + i * ROW_H, first + i == sel);

    /* 线画在行**之后**：行底（斑马纹 / 威胁红底 / 选中亮底）是整条铺过去的，
     * 先画线会被行底盖掉。 */
    draw_col_seps(fb, drawn);

    /* ── 底部：还有多少没看到 ──
     * 只画滚动条不够：条的长度只说得清「大概还有一些」，而「14 架里的 3-10」
     * 是个能直接对上的数。两者都给。 */
    if (nr > ROW_N) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d-%d / %d", first + 1, first + ROW_N, nr);
        puts_right(fb, CONTENT_R, LIST_BOT + 1, buf, COL_DIM, PK_AA_XS);

        const int track_w = 240;
        const int tx = PAD_L;
        const int ty = LIST_BOT + PK_AA_XS_H / 2;
        pk_pfd_fill_rect(fb, tx, ty, tx + track_w, ty + 3, COL_LINE);
        const int bar_w = track_w * ROW_N / nr;
        const int bar_x = tx + track_w * first / nr;
        pk_pfd_fill_rect(fb, bar_x, ty, bar_x + bar_w, ty + 3, COL_DIM);
    }
}

/*
 * 表头点击 → 切排序列 / 翻方向。
 *
 * 与交通页那几个自绘按钮同一个机制：这些不是 LVGL 控件，命中判定只能由
 * touch_gt911.c 在 read_cb 里转发进来，返回 true 表示这一下被吃掉，不再
 * 传给底下的 FAB。
 */
bool pk_adsb_list_touch(int x, int y)
{
    /* 顶栏的 RESET：回到默认排序（距离升序）。只在非默认时才画，也只在
     * 非默认时才吃这一下——否则默认状态下这块区域会白白拦掉别的手势。 */
    if (y >= 0 && y < PFD_BAR_BOT &&
        x >= RESET_X0 && x < RESET_X1 && !sort_is_default()) {
        s_hdr_down  = HDR_HIT_RESET;
        s_sort      = SORT_DEFAULT_KEY;
        s_sort_desc = false;
        return true;
    }

    if (y < LST_TOP || y >= ROW0_Y) return false;

    for (int k = 0; k < SORT_COUNT; ++k) {
        if (x < COLS[k].hit_x0 || x >= COLS[k].hit_x1) continue;
        s_hdr_down = k;
        if (s_sort == (sort_key_t)k) {
            s_sort_desc = !s_sort_desc;   /* 同一列再点：翻方向 */
        } else {
            s_sort = (sort_key_t)k;
            /* 换列时**不继承**上一列的方向，一律回到升序：降序的距离
             * （最远的排最前）几乎没人想要，而「点一下换列结果顺序还是反的」
             * 会让人以为点错了列。 */
            s_sort_desc = false;
        }
        return true;
    }
    return false;
}

void pk_adsb_list_touch_up(void) { s_hdr_down = -1; }
