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
#include "i18n.h"
#include "pfd_layout.h"
#include "pfd_statusbar.h"   /* pk_ui_topbar_right_limit —— 给 DEMO 徽标让位 */
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

/* 文字统一走抗锯齿字体。原先满页 5×7 位图，在 217 PPI 上既小又糊，与已改好
 * 的其余页面也不是一套。
 * 2026-08-01：配套的 TFC_PUTS_XS 已删——卡片里的次要列直接调 pk_aa_puts(…,
 * PK_AA_XS)，那个宏从落地起就没有使用者。 */
#define TFC_PUTS(fb, x, y, s, col) \
        pk_aa_puts((fb), PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_M)
/* 顶栏其余读数与标题同一条基线，所以直接取标题的纵坐标。 */
#define TFC_HDR_TY    PK_UI_TITLE_Y

/* ── 右栏（spec §5.2：280 px，4 张卡片可滚动）───────────────── */
#define SIDE_X        TFC_SIDE_X
/* 行高与内边距直接取 PFD 信息框的值（PFD_IB_ROW_H/_GAP/_PAD）。
 *
 * 一开始自己造了「卡片」：一张 76~97 px、三行内容、带边框。做出来又大又
 * 看不懂。PFD 右下角那三行（B / ALT / VS）早就把这类信息排明白了——一行一
 * 条、半透明底、左标签右数值，没有边框。同一台设备上不该有两套列表语言。 */
#define ROW_PAD       PFD_IB_PAD
#define ROW_GAP       PFD_IB_ROW_GAP
/* 两行一条：五个字段在 280 px 里排不进一行——试过，呼号会和距离撞上。
 * 主行放「在哪边 + 是谁 + 高度差」，次行放「多远 + 多快」。 */
#define ROW_H         (PK_AA_M_H + PK_AA_XS_H + 8)
#define ROW_N         ((PK_DISPLAY_H - PFD_BAR_BOT - 12) / (ROW_H + ROW_GAP))

/* ── 雷达区上的操作按钮 ───────────────────────────────────────
 *
 * 朝向切换放左下角，与高德/Google Maps 的位置习惯一致——那是拇指最容易够到
 * 的角落，而且不压住雷达中心。量程 +/- 叠在右下角，同理。
 *
 * 命中区比图形本身大一圈（BTN_HIT_PAD）：手指按下的落点与眼睛看到的中心
 * 常差几毫米，按钮画多大与该给多大命中区是两回事。 */
#define BTN_D         56
/* 命中半径要凑够 9 mm 的手指目标：屏 8.4 px/mm，56 px 的图形只有 6.7 mm，
 * 加 12 px 外扩后命中 80 px ≈ 9.5 mm。视觉仍是 56——按钮画大了会压掉雷达
 * 的可用面积，而命中区不占像素。（原来 pad=8 得到 72 px = 8.6 mm，差一点。）*/
#define BTN_HIT_PAD   12
#define BTN_M         16                      /* 距屏幕边 */
#define BTN_ORI_X     BTN_M
#define BTN_ORI_Y     (PK_DISPLAY_H - BTN_M - BTN_D)
#define BTN_ZIN_X     (TFC_RADAR_W - BTN_M - BTN_D)
#define BTN_ZIN_Y     (PK_DISPLAY_H - BTN_M - BTN_D * 2 - 10)
#define BTN_ZOUT_X    BTN_ZIN_X
#define BTN_ZOUT_Y    (PK_DISPLAY_H - BTN_M - BTN_D)

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

/*
 * 本机符号：Material Symbols 的 flight 字形，按航向整体旋转。
 *
 * 早先这里分了两条路——heading-up 用字形、north-up 用手绘矢量剪影——理由是
 * 「字形是预渲染的、转不了」。那是错的：位图旋转只是一次反向映射加采样，
 * 缺的是函数不是可能性。补上 pk_aa_blit_4bpp_rot() 之后两条路合成一条，
 * 本机、PFD 罗盘中心从此是同一架飞机。
 *
 * heading-up 时传 0°：机头恒指屏幕上方，此时旋转路径退化成 1:1 采样，
 * 与直接 blit 等价，不必再分支。
 */
static void draw_own_aircraft(uint16_t *fb, float rot_deg, uint16_t col)
{
    const uint8_t *ac = pk_icon_bitmap
                      + (size_t)PK_ICON_OWNSHIP
                        * (((size_t)PK_ICON_W * PK_ICON_H + 1) / 2);

    pk_aa_blit_4bpp_rot(fb, PK_DISPLAY_W, PK_DISPLAY_H, CX, CY,
                        ac, PK_ICON_W, PK_ICON_H, rot_deg, col);
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

/* 目标在雷达上的落点。 */
static void target_pos(const vis_t *v, pk_map_orient_t orient, int range_nm,
                       int *tx, int *ty)
{
    const float screen = (orient == PK_MAP_HEADING_UP) ? v->rel.rel_bearing
                                                       : v->rel.abs_bearing;
    polar(screen, v->rel.dist_nm / range_nm * RMAX, tx, ty);
}

/* 配色照搬 PFD 罗盘外圈那份（pfd_hsi_traffic.c）：颜色本身就是「威胁等级」
 * ——同高度琥珀、高于青、低于蓝灰、无高度灰。飞行员在 PFD 上已经按这套
 * 读了，交通页再换一套只会让人重新学。 */
#define TFC_COL_SEL  pk_rgb565(255, 210,  60)
#define TFC_COL_LBL  pk_rgb565(207, 211, 220)

static uint16_t target_color(const pk_traffic_rel_t *rel, bool selected)
{
    const uint16_t COL_LEVEL = pk_rgb565(255, 176,   0);   /* ±1000 ft 内 */
    const uint16_t COL_ABOVE = pk_rgb565(  0, 210, 235);
    const uint16_t COL_BELOW = pk_rgb565( 95, 150, 190);
    const uint16_t COL_NOALT = pk_rgb565(150, 155, 165);
    return selected                  ? TFC_COL_SEL
         : !rel->rel_alt_valid       ? COL_NOALT
         : (rel->rel_alt_ft >  1000) ? COL_ABOVE
         : (rel->rel_alt_ft < -1000) ? COL_BELOW
                                     : COL_LEVEL;
}

static void draw_target_symbol(uint16_t *fb, const vis_t *v, int tx, int ty,
                               pk_map_orient_t orient, float own_heading,
                               float mag_var, bool selected, uint16_t col)
{
    /*
     * 目标符号：**可旋转的飞机剪影**，不是菱形。
     *
     * 直接用 PFD 那个 pk_pfd_draw_aircraft()——它已经调好了后掠翼与尾部凹口的
     * 比例（注释里记着初版「又扁又胖像回旋镖」的教训）。之前这里自己画菱形，
     * 于是迎面飞来的和同向飞离的长得一模一样，而这恰恰是防撞最要紧的信息
     * （spec §5.2 点名：「一眼可辨迎面/同向，此信息三角形无法表达」）。
     *
     * 旋转角取目标自己的 ADS-B 航迹。无航迹数据就退回菱形——那表示「不知道
     * 朝向」，画一个朝某方向的飞机等于编造信息。
     *
     * 角度换算交给 pk_traffic_symbol_rot_deg()：它和算落点的
     * pk_traffic_rel_calc() 在同一个文件里，共用「地图参考北 = 真北 - mag_var」
     * 这一条约定，也就不会再出现符号与落点分属两套参考系的情况。此前这里内联
     * 写的是 `heading_deg - (screen - rel_bearing)`，而该分支中 screen 恒等于
     * rel_bearing，减法恒为 0——两种朝向都只用了目标航迹本身，于是机头朝上时
     * 迎面与同向画出来一模一样，而 PFD 罗盘外圈那份（pfd_hsi_traffic.c）算的
     * 是对的，同一架飞机在两个页面上机头差了一个 mag_var + 本机航向。
     */
    if (v->ac->have_velocity) {
        const float rot = pk_traffic_symbol_rot_deg(
            orient == PK_MAP_HEADING_UP, (float)v->ac->heading_deg,
            mag_var, own_heading);
        pk_pfd_draw_aircraft(fb, tx, ty, rot, selected ? 15 : 11, col);
    } else {
        fill_diamond(fb, tx, ty, selected ? 6 : 5, col);
    }
}

/* ── 标签防遮挡 ──────────────────────────────────────────────────
 *
 * 症状：目标扎堆时（mock 里 40°~52° 那三架就是专门压这个的）各自的高度差标签
 * 互相压住，读者分不出 "+42" 到底是哪一架的。
 *
 * 解法是三件事叠起来：
 *
 *  1. **候选槽位**。每条标签有 8 个候选落点（右/左 × 中/上/下/更上更下），
 *     依次试，取第一个不与已占矩形相撞的。
 *  2. **按威胁排队占位**。顺序 = 选中 > 近 > 远（vis 已按距离升序）。近的先挑，
 *     8 个槽都挑不到的必然是外围的远目标，它的标签**整条丢掉**、只留符号——
 *     叠着画等于两条都读不出来，还不如明说「这架的高度差请去右栏读」。
 *  3. **稳定性压倒最优解**。座舱里字在跳比字被压住更难读：同一架飞机上一帧
 *     用了哪个槽，这一帧先复用哪个，只有真撞上了才重新找。每帧从 0 号槽重排的
 *     写法在目标缓慢移动时会让标签在左右两侧来回弹。
 *
 * 偏离默认位置的槽位补一条引线：标签一旦不在符号正右方，「这条属于谁」就不再
 * 不言自明，一条 1 px 的线是最省像素的答案。
 */
#define LBL_GAP    12                     /* 标签与符号的水平间隙 */
#define LBL_STEP   (PK_AA_XS_H + 3)       /* 上下让位的步距 */
#define LBL_SLOTS  14

/* side: +1 = 符号右侧, -1 = 左侧；step: 纵向让位的步数。
 *
 * 顺序即偏好：先左右、后上下、再远。槽位数是实测出来的——只给 8 个时，中心区
 * 那一簇（mock 里 2.4~6 NM 三四架挤在本机符号周围）会集体找不到落点而全部隐藏，
 * 而它们恰恰是威胁最高、最该有标签的。 */
static const struct { int8_t side; int8_t step; } kLblSlot[LBL_SLOTS] = {
    { +1,  0 }, { -1,  0 },        /* 正右 / 正左：贴着符号，最易读，优先 */
    { +1, -1 }, { -1, -1 },
    { +1, +1 }, { -1, +1 },
    { +1, -2 }, { -1, -2 },
    { +1, +2 }, { -1, +2 },
    { +1, -3 }, { -1, -3 },
    { +1, +3 }, { -1, +3 },
};

typedef struct {
    int      tx, ty;          /* 符号中心 */
    int      w, h;            /* 标签整体尺寸（含箭头；选中时含呼号那一行） */
    int      x0, y0;          /* 落位左上角，slot >= 0 时有效 */
    int      slot;            /* -1 = 本帧放不下，只画符号 */
    uint32_t icao;
    bool     selected;
    bool     climb;           /* 箭头朝上（决定绿/橙） */
    char     lab[16];
    const char *arrow;
    char     cs[10];          /* 仅选中目标用 */
} lbl_t;

/* 上一帧每架飞机用过的槽位，按 ICAO 记。这份「记忆」就是防抖动的全部机制——
 * 没有它，两架擦身而过时标签会在左右两侧反复横跳。 */
static struct { uint32_t icao; int8_t slot; } s_lbl_mem[AIRCRAFT_TABLE_CAPACITY];
static int s_lbl_mem_n;

static int lbl_mem_get(uint32_t icao)
{
    for (int i = 0; i < s_lbl_mem_n; ++i)
        if (s_lbl_mem[i].icao == icao) return s_lbl_mem[i].slot;
    return -1;
}

/* 矩形相交（半开区间，双方已含各自的外扩边距）。 */
static bool rect_hit(const int *a, const int *b)
{
    return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}

/* 槽位 slot 下这条标签占的矩形，含暗底那 2 px 外扩。 */
static void lbl_rect(const lbl_t *e, int slot, int *r)
{
    const int x = (kLblSlot[slot].side > 0) ? e->tx + LBL_GAP
                                            : e->tx - LBL_GAP - e->w;
    const int y = e->ty - PK_AA_XS_H / 2 + kLblSlot[slot].step * LBL_STEP;
    r[0] = x - 2;         r[1] = y - 1;
    r[2] = x + e->w + 2;  r[3] = y + e->h + 1;
}

/*
 * 占位求解。
 *
 * 占用表里**先放所有目标的符号**：标签盖住别人的飞机剪影和标签盖住标签一样是
 * 在丢信息——剪影的朝向正是「迎面还是同向」那一眼。
 */
static void place_labels(lbl_t *L, int n, int sel_idx)
{
    static int occ[AIRCRAFT_TABLE_CAPACITY * 2][4];
    const int occ_cap = (int)(sizeof(occ) / sizeof(occ[0]));
    int nocc = 0;

    /* 本机符号也要占位：中心那架白飞机被标签盖住时，「我在哪」这条最基本的
     * 参照就没了。半宽按图标尺寸取。 */
    occ[nocc][0] = CX - PK_ICON_W / 2;  occ[nocc][1] = CY - PK_ICON_H / 2;
    occ[nocc][2] = CX + PK_ICON_W / 2;  occ[nocc][3] = CY + PK_ICON_H / 2;
    nocc++;

    for (int i = 0; i < n && nocc < occ_cap; ++i) {
        /* 符号半宽取 9：剪影外接方框是 11（选中 15），但那是机头到翼尖的
         * 对角极值，实际填充的是一个很瘦的三角。按外接框判撞会让中心区扎堆的
         * 几架一个槽都找不到——而它们正是最近、最该有标签的那几架。 */
        occ[nocc][0] = L[i].tx - 9;  occ[nocc][1] = L[i].ty - 9;
        occ[nocc][2] = L[i].tx + 9;  occ[nocc][3] = L[i].ty + 9;
        nocc++;
    }

    for (int k = 0; k < n; ++k) {
        /* 选中的先占：那是用户此刻正盯着的一架，绝不能被别人挤掉。 */
        const int i = (sel_idx < 0)  ? k
                    : (k == 0)       ? sel_idx
                    : (k <= sel_idx) ? k - 1
                                     : k;
        lbl_t *e = &L[i];
        const int prev = lbl_mem_get(e->icao);
        int chosen = -1;

        /* t = -1 这一轮先试上一帧的槽——稳定压倒最优。 */
        for (int t = -1; t < LBL_SLOTS && chosen < 0; ++t) {
            const int s = (t < 0) ? prev : t;
            if (s < 0 || s >= LBL_SLOTS) continue;
            if (t >= 0 && s == prev) continue;     /* t=-1 已经试过了 */

            int r[4];
            lbl_rect(e, s, r);
            /* 越出雷达区就不是候选：右栏是列表的地盘，标签压过去会和行文字
             * 打架；顶栏同理。 */
            if (r[0] < 2 || r[2] > TFC_RADAR_W - 2)            continue;
            if (r[1] < TFC_TOP + 2 || r[3] > PK_DISPLAY_H - 2) continue;

            bool clash = false;
            for (int j = 0; j < nocc && !clash; ++j)
                if (rect_hit(r, occ[j])) clash = true;
            if (!clash) chosen = s;
        }

        /* 选中的那架永不隐藏：它是用户刚点过的目标，标签消失会被当成「点了没
         * 反应」。宁可让它压住别人——它本来就该压在最上层（画的顺序也是最后）。 */
        if (chosen < 0 && e->selected) chosen = 0;

        e->slot = chosen;
        if (chosen >= 0) {
            int r[4];
            lbl_rect(e, chosen, r);
            e->x0 = r[0] + 2;
            e->y0 = r[1] + 1;
            if (nocc < occ_cap) {
                occ[nocc][0] = r[0]; occ[nocc][1] = r[1];
                occ[nocc][2] = r[2]; occ[nocc][3] = r[3];
                nocc++;
            }
        }
    }

    /* 写回记忆表。放不下的记 -1，下一帧仍从 0 号槽重试——「这一帧藏起来」
     * 不该固化成「永远藏起来」。 */
    s_lbl_mem_n = 0;
    for (int i = 0; i < n && s_lbl_mem_n < AIRCRAFT_TABLE_CAPACITY; ++i) {
        s_lbl_mem[s_lbl_mem_n].icao = L[i].icao;
        s_lbl_mem[s_lbl_mem_n].slot = (int8_t)L[i].slot;
        s_lbl_mem_n++;
    }
}

static void draw_label(uint16_t *fb, const lbl_t *e, uint16_t col_sym)
{
    if (e->slot < 0) return;

    /* 升降箭头单独着色：爬升绿、下降橙。
     *
     * 和高度差数字分开画，是因为两者说的是不同的事——数字是「差多少」（静态
     * 位置），箭头是「在往哪走」（趋势）。同色的话趋势会被淹没在数字里，而
     * 判断会不会冲突恰恰要先看趋势。绿/橙这对在本项目里已经用于 VS 与温度，
     * 语义一致：绿=正在离开、橙=需要留意。 */
    const uint16_t COL_UP   = pk_rgb565( 90, 220, 120);
    const uint16_t COL_DOWN = pk_rgb565(255, 170,  70);

    /* 引线：只有落在 0 号槽（正右、贴着符号）才不需要——那时归属一目了然。
     * 其余槽位一律连线，用目标自己的颜色，与符号同源。 */
    if (e->slot != 0) {
        const int ex = (kLblSlot[e->slot].side > 0) ? e->x0 - 2
                                                    : e->x0 + e->w + 2;
        pk_pfd_draw_line(fb, e->tx, e->ty, ex, e->y0 + PK_AA_XS_H / 2, col_sym);
    }

    /* 标签压一层暗底再写字：雷达上目标扎堆时，白字叠白字谁也读不出来。
     * PFD 的交通标签同样这么做。 */
    pk_pfd_darken_rect(fb, e->x0 - 2, e->y0 - 1,
                       e->x0 + e->w + 2, e->y0 + e->h + 1, 120);

    const int lw = pk_aa_text_width(e->lab, PK_AA_XS);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, e->x0, e->y0, e->lab,
               e->selected ? TFC_COL_SEL : TFC_COL_LBL, PK_AA_XS);
    if (e->arrow[0]) {
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, e->x0 + lw, e->y0, e->arrow,
                   e->climb ? COL_UP : COL_DOWN, PK_AA_XS);
    }
    /* 呼号只给选中的那架：14 个目标每架都标呼号，雷达会糊成一片字。
     * 想看全部就看右栏列表，那里一行一条排得整整齐齐。 */
    if (e->selected && e->cs[0]) {
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   e->x0, e->y0 + PK_AA_XS_H + 2, e->cs, TFC_COL_SEL, PK_AA_XS);
    }
}

/* 把一个目标铺成一条待落位的标签（只算尺寸与文案，不碰 framebuffer）。 */
static void build_label(lbl_t *e, const vis_t *v, int tx, int ty, bool selected)
{
    const pk_traffic_rel_t *rel = &v->rel;

    e->tx       = tx;
    e->ty       = ty;
    e->icao     = v->ac->icao24;
    e->selected = selected;
    e->slot     = -1;
    e->cs[0]    = '\0';

    /* 标签：相对高度（百 ft）+ 方向箭头，格式与 PFD 一致。 */
    if (rel->rel_alt_valid) {
        int hh = rel->rel_alt_ft / 100;
        if (hh >  99) hh =  99;
        if (hh < -99) hh = -99;
        snprintf(e->lab, sizeof(e->lab), "%+d", hh);
    } else {
        snprintf(e->lab, sizeof(e->lab), "---");
    }
    e->climb = rel->vs_fpm > 0;

    const char *arrow = !rel->rel_alt_valid   ? ""
                      : rel->vs_fpm >  200    ? "\u2191"
                      : rel->vs_fpm < -200    ? "\u2193" : "";
    e->arrow = arrow;

    /* 宽度一律问渲染器：箭头是 3 字节 UTF-8，strlen 会把它数成 3 格，
     * 占位矩形就会宽出一截，扎堆时白白挤掉别人的槽位。 */
    e->w = pk_aa_text_width(e->lab, PK_AA_XS)
         + (arrow[0] ? pk_aa_text_width(arrow, PK_AA_XS) : 0);
    e->h = PK_AA_XS_H;

    if (selected) {
        callsign_of(v->ac, e->cs, sizeof(e->cs));
        const int cw = pk_aa_text_width(e->cs, PK_AA_XS);
        if (cw > e->w) e->w = cw;
        e->h += PK_AA_XS_H + 2;    /* 呼号另起一行，占位必须把它算进去 */
    }
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

/* 当前被按住的按钮（-1 = 无）。触摸层在按下时置位、松手时清除，绘制时据此
 * 高亮——在 10 fps 上没有这个反馈，点下去会像是没反应。 */
static int s_btn_down = -1;

/* 圆形按钮底：半透明深色 + 细边，压在雷达上仍看得清，又不抢图形的注意力。
 * 按住时底色提亮一档，这是按钮唯一的「我收到了」信号。 */
static void draw_btn_plate(uint16_t *fb, int x, int y, bool down)
{
    const uint16_t face = down ? pk_rgb565( 62,  84, 112)
                               : pk_rgb565( 22,  30,  42);
    const uint16_t edge = down ? pk_rgb565(210, 228, 245)
                               : pk_rgb565(120, 145, 175);
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

static void draw_buttons(uint16_t *fb, pk_map_orient_t orient, int range_nm)
{
    const uint16_t ink = pk_rgb565(225, 235, 248);

    /* 朝向切换：显示**当前**模式，点一下换到另一种（与高德的指北针同理）。
     *
     * 用图标不用文字。"HDG UP"/"N UP" 这类缩写要先读、再想它是什么意思；
     * navigation 那个导航箭头和 explore 那个指南针是通用符号，扫一眼就懂。
     * 两枚都取自内置的 Material Symbols 集（见 gen_pfd_icons.py）。 */
    draw_btn_plate(fb, BTN_ORI_X, BTN_ORI_Y, s_btn_down == 0);
    {
        const pk_icon_id_t id = (orient == PK_MAP_HEADING_UP)
                                  ? PK_ICON_NAV_HDG : PK_ICON_NAV_NORTH;
        const uint8_t *ic = pk_icon_bitmap
                          + (size_t)id * (((size_t)PK_ICON_W * PK_ICON_H + 1) / 2);
        pk_aa_blit_4bpp(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                        BTN_ORI_X + (BTN_D - PK_ICON_W) / 2,
                        BTN_ORI_Y + (BTN_D - PK_ICON_H) / 2,
                        ic, PK_ICON_W, PK_ICON_H, ink);
    }

    /* 量程 +/-。放大是「看得更近」，所以 + 对应更小的 NM 数。 */
    draw_btn_plate(fb, BTN_ZIN_X, BTN_ZIN_Y, s_btn_down == 1);
    draw_btn_plate(fb, BTN_ZOUT_X, BTN_ZOUT_Y, s_btn_down == 2);
    {
        const int cw = pk_aa_cell_w(PK_AA_L);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   BTN_ZIN_X + (BTN_D - cw) / 2,
                   BTN_ZIN_Y + (BTN_D - PK_AA_L_H) / 2, "+", ink, PK_AA_L);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   BTN_ZOUT_X + (BTN_D - cw) / 2,
                   BTN_ZOUT_Y + (BTN_D - PK_AA_L_H) / 2, "-", ink, PK_AA_L);
    }
    (void)range_nm;
}

/*
 * 相对方位 → 八向箭头。
 *
 * 用 Unicode 的 ↑↗→↘↓↙←↖，不用 ASCII 拼。这些是既有符号，一眼就懂；之前
 * 拿 "/" "^" "<" 去凑，读者只能猜。它们与汉字同走「非 ASCII 二分查表」那条
 * 路径（码位见 gen_pfd_aa_font.py 的 ARROW_CODES）。
 *
 * 箭头指的是**目标在本机的哪个方向**，机头朝上时正前方就是 ↑。
 *
 * 实现搬到了 traffic_geom.h 的 pk_bearing_arrow()：本页、ADS-B 列表页、
 * 搜索页原先各存一份一模一样的表，改一份漏一份就是同一方位在两页画得不同。
 */

/*
 * 右栏目标列表（spec §5.2：方位 + 呼号 + 距离 + 高度带升降率 + 速度）。
 *
 * 视觉语言取自 PFD 的信息框（pfd_infobox.c）：一行一条、半透明底、无边框、
 * 行高 30。相对高度沿用 PFD 交通标签的写法——百英尺、带符号、后缀升降箭头，
 * 飞行员在 PFD 上已经读惯。
 *
 * 五个字段各有各的用处，缺一个这行就废了：方位说「在我哪边」，距离说「多远」，
 * 高度差说「会不会撞上」，升降说「差距在缩小还是拉开」，速度说「追不追得上」。
 * 只写呼号和距离，飞行员拿不到任何决策依据。
 */
static void draw_side_list(uint16_t *fb, const vis_t *vis, int nv, int sel_row)
{
    const uint16_t COL_SEL   = pk_rgb565(255, 210,  60);
    const uint16_t COL_TXT   = pk_rgb565(235, 240, 248);
    const uint16_t COL_DIM   = pk_rgb565(155, 170, 190);
    const uint16_t COL_ARROW = pk_rgb565(  0, 210, 235);
    /* 高度差配色沿用 PFD 交通标签：同高度是威胁，红；上下分开是安全，灰蓝。 */
    const uint16_t COL_NEAR  = pk_rgb565(255, 120,  90);

    const int x0 = SIDE_X + 8;
    const int x1 = PK_DISPLAY_W - 8;

    for (int i = 0; i < ROW_N && i < nv; ++i) {
        const int y0 = TFC_TOP + 6 + i * (ROW_H + ROW_GAP);
        const vis_t *v = &vis[i];
        const bool sel = (i == sel_row);

        pk_pfd_darken_rect(fb, x0, y0, x1, y0 + ROW_H, sel ? 120 : 170);

        const int ty1 = y0 + 4;                         /* 主行 */
        const int ty2 = ty1 + PK_AA_M_H + 1;            /* 次行 */
        int x = x0 + ROW_PAD;

        /* ── 主行：方位 + 呼号 …… 高度差 ── */
        x += pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x, ty1,
                        pk_bearing_arrow(v->rel.rel_bearing),
                        sel ? COL_SEL : COL_ARROW, PK_AA_M);
        x += 4;
        char cs[9];
        callsign_of(v->ac, cs, sizeof(cs));
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x, ty1, cs,
                   sel ? COL_SEL : COL_TXT, PK_AA_M);

        char l1[16];   /* 箭头是 3 字节 UTF-8，留够 */
        if (v->rel.rel_alt_valid) {
            const int h100 = v->rel.rel_alt_ft / 100;
            snprintf(l1, sizeof(l1), "%+d", h100);
        } else {
            snprintf(l1, sizeof(l1), "---");
        }
        /* 高度差贴右缘。±1000 ft 以内标红——同高度才是威胁，上下分得开就不是。
         * 这个阈值与 PFD 交通标签一致。 */
        const bool near_alt = v->rel.rel_alt_valid &&
                              v->rel.rel_alt_ft > -1000 && v->rel.rel_alt_ft < 1000;
        /* 箭头与数字分色，同 draw_target 的理由：数字说「差多少」，箭头说
         * 「在往哪走」，后者才是判断会不会冲突的第一眼。 */
        const uint16_t COL_UP   = pk_rgb565( 90, 220, 120);
        const uint16_t COL_DOWN = pk_rgb565(255, 170,  70);
        const char *ar = !v->rel.rel_alt_valid ? ""
                       : v->rel.vs_fpm >  200  ? "\u2191"
                       : v->rel.vs_fpm < -200  ? "\u2193" : "";
        const int aw = ar[0] ? PK_AA_M_CJK_W : 0;
        const int w1 = (int)strlen(l1) * pk_aa_cell_w(PK_AA_M);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   x1 - ROW_PAD - w1 - aw, ty1, l1,
                   sel ? COL_SEL : (near_alt ? COL_NEAR : COL_DIM), PK_AA_M);
        if (ar[0]) {
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x1 - ROW_PAD - aw, ty1, ar,
                       v->rel.vs_fpm > 0 ? COL_UP : COL_DOWN, PK_AA_M);
        }

        /* ── 次行：距离 + 地速 ── */
        char l2[20];
        if (v->ac->have_velocity) {
            snprintf(l2, sizeof(l2), "%.0f NM   %d kt",
                     v->rel.dist_nm, (int)lroundf(v->ac->ground_speed_kt));
        } else {
            snprintf(l2, sizeof(l2), "%.0f NM   -- kt", v->rel.dist_nm);
        }
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x0 + ROW_PAD + 22, ty2, l2,
                   sel ? COL_SEL : COL_DIM, PK_AA_XS);
    }
}

/*
 * 雷达区中央的降级提示。
 *
 * 「图上一架都没有」有三种成因，用户的下一步动作完全不同：没有本机位置要等
 * 定位或去绑定，全在量程外按一下 − 就看见，真的没收到就只能等。共用一句
 * 「无数据」等于什么也没说，而一个字都不写最糟——真机上就是一片黑，用户会
 * 当成页面没画出来。
 *
 * 压一层暗底再写字：底下是距离环与罗盘刻度，亮灰细线穿过笔画会把字切断。
 * 雷达上的目标标签同样这么做（draw_label）。
 */
static void draw_center_notice(uint16_t *fb, int cy, uint16_t col,
                               const char *msg, pk_aa_size_t msg_size,
                               const char *hint)
{
    const int mw = pk_aa_text_width(msg, msg_size);
    const int mh = pk_aa_cell_h(msg_size);
    const int hw = hint ? pk_aa_text_width(hint, PK_AA_S) : 0;
    const int hh = hint ? PK_AA_S_H + 8 : 0;      /* 8 = 与主行的行距 */

    const int w  = mw > hw ? mw : hw;
    const int y0 = cy - (mh + hh) / 2;

    /* 暗底比文字四周各外扩 14/8 px：贴着字沿切会显得像个补丁，留一圈才像
     * 一块「这里没有内容」的牌子。 */
    pk_pfd_darken_rect(fb, CX - w / 2 - 14, y0 - 8,
                       CX + w / 2 + 14, y0 + mh + hh + 8, 190);

    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, CX - mw / 2, y0, msg, col, msg_size);
    if (hint) {
        /* 提示语用中性灰而非主提示的琥珀：琥珀是「注意这里」，同色两行会让
         * 眼睛不知道先读哪句。 */
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, CX - hw / 2, y0 + mh + 8,
                   hint, pk_rgb565(170, 182, 200), PK_AA_S);
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
    /* 页面标题色见 PK_UI_TITLE_COL（pfd_layout.h），本页不再自留一份。 */
    const uint16_t COL_CYAN  = pk_rgb565(  0, 210, 235);
    /* 顶栏这几项（目标数、量程）曾用 120,120,128 的灰——在近黑底上对比度只有
     * 2:1 上下，日光下等于没有。它们是常驻读数不是次要注释，提到与 HDG 同级
     * 的亮度；真要压视觉重量该靠字号，不是靠让人看不清。 */
    const uint16_t COL_GREY  = pk_rgb565(205, 214, 228);
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
    /*
     * 没有航向就画不出「机头朝上」——那幅图的上方是哪儿全靠 own_heading，
     * 而此刻它只是个占位的 0。数值上此时 rel_bearing 恒等于 abs_bearing，
     * 画出来的**本来就是**一幅正北朝上的图；顶栏却照旧写着「航向朝上」，
     * 同一行里还并排着 "HDG ---"，自相矛盾。于是这里把朝向降级成 NORTH-UP，
     * 让扇区、罗盘、本机符号与顶栏那行字说同一件事。
     *
     * 左下角那枚按钮仍按 orient 画：它表达的是用户的**选择**，不是当前画面；
     * 航向一回来就该立刻按选择生效。
     */
    const pk_map_orient_t orient_eff = hdg_valid ? orient : PK_MAP_NORTH_UP;
    int range_idx = pk_traffic_range_idx_get();
    int range_nm  = pk_traffic_range_nm(range_idx);

    /* HSI 可见扇区半透明填充 — 背景层,必须在距离环/罗盘/目标之前画。
     * 无本机位置时不画：那片青色扇区说的是「我前方这一片」，而此刻连「我」
     * 在哪都不知道。它也正压在中央提示文字底下。 */
    if (own_valid) draw_hsi_sector(fb, orient_eff, own_heading, hdg_valid);

    size_t n = aircraft_state_snapshot(
        s_scratch, AIRCRAFT_TABLE_CAPACITY, now_us, AIRCRAFT_STALE_AGE_US);

    /* ── 顶栏 ──
     * 与 PFD 状态栏同高（PFD_BAR_BOT），文字走统一字体的 normal 档。 */
    char buf[24];
    /* 标题走全局层级（pfd_layout.h），不吃本页的 TFC_PUTS/COL_HDR。
     * 左缘原来硬编码 24，比同一页左下角朝向按钮的 BTN_M(16) 还往里缩——
     * 一页之内两个边距，切到 diag/list 更是差 8 px。 */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_UI_PAD_L, PK_UI_TITLE_Y,
               pk_i18n_text(PK_TR_TFC_TITLE), PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);
    if (hdg_valid) {
        snprintf(buf, sizeof(buf), "HDG %03d~", ((int)lroundf(own_heading) + 360) % 360);
        TFC_PUTS(fb, 200, TFC_HDR_TY, buf, COL_CYAN);
    } else {
        TFC_PUTS(fb, 200, TFC_HDR_TY, "HDG ---~", COL_AMBER);
    }
    /* 目标计数用图标而不是 "TFC" 三个字母——PFD 状态栏已经用这枚
     * connecting_airports 表示 ADS-B 目标数，同一台设备上同一件事该用同一个
     * 符号。 */
    {
        const uint8_t *ic = pk_icon_bitmap
                          + (size_t)PK_ICON_ADSB
                            * (((size_t)PK_ICON_W * PK_ICON_H + 1) / 2);
        /* 绿色取自 PFD 状态栏的 COL_GREEN——那里的星数、目标数都是这个绿。
         * 绿在本项目里表示「有效且在线」，灰是「数据失效」，用错色等于说谎。 */
        const uint16_t COL_ADSB = pk_rgb565(0, 220, 60);
        pk_aa_blit_4bpp(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                        392, (PFD_BAR_BOT - PK_ICON_H) / 2,
                        ic, PK_ICON_W, PK_ICON_H, COL_ADSB);
        snprintf(buf, sizeof(buf), "%d", (int)n);
        TFC_PUTS(fb, 392 + PK_ICON_W + 6, TFC_HDR_TY, buf, COL_ADSB);
    }
    /* 右上角：地图朝向 + 量程。
     *
     * 左下角那枚朝向按钮是**图标**，图标只说得清「点了会怎样」，说不清
     * 「现在是哪种」——尤其正北模式下本机符号本身也在转，光看图分不出是
     * 「机头朝上、地图在转」还是「地图朝北不动、本机在转」。所以这里补一行
     * 文字把当前模式讲明白，与量程排在同一处，构成「这幅图是怎么画的」一栏。
     *
     * 字号：朝向与量程同为 PK_UI_TITLE_SIZE(M)，与本页标题、HDG、目标计数
     * 全部同档。此前朝向刻意压到 S 档，理由是「量程随时要读、朝向确认一次就
     * 不看，该分主次」——产品决策否决：同一条 header 内不分主次，视觉一致
     * 优先，一行里冒出两种字号只会显得别扭。主次交给颜色（青/灰）去表达。 */
    {
        /* 朝向文案与设置页同一条词条：那里也是「地图朝向」这一项的两个选项，
         * 两处各写一份就会出现设置里叫一个名、雷达页上叫另一个名。 */
        const char *om = pk_i18n_text(orient_eff == PK_MAP_HEADING_UP
                                      ? PK_TR_MAP_ORIENT_HDG_UP
                                      : PK_TR_MAP_ORIENT_NORTH_UP);
        snprintf(buf, sizeof(buf), "%dNM", range_nm);

        /* 这两个宽度是右对齐的依据，中文侧一个汉字 3 字节，strlen 会把
         * 「机头朝上」算成 12 格，整块读数被推出屏幕右缘。 */
        const int nm_w = pk_aa_text_width(buf, PK_UI_TITLE_SIZE);
        const int om_w = pk_aa_text_width(om,  PK_UI_TITLE_SIZE);
        /* 右界走 pk_ui_topbar_right_limit：演示模式下顶栏右侧多了一枚常驻的
         * DEMO 徽标，它画在控件层、压在本页之上，不退让的话「北向朝上」会被
         * 盖掉一半。 */
        const int nm_x = pk_ui_topbar_right_limit(PK_DISPLAY_W - 24) - nm_w;

        TFC_PUTS(fb, nm_x, TFC_HDR_TY, buf, COL_GREY);
        /* 同档同高，与量程共用 TFC_HDR_TY(=PK_UI_TITLE_Y) 即可对齐基线。 */
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   nm_x - 16 - om_w, TFC_HDR_TY,
                   om, COL_CYAN, PK_UI_TITLE_SIZE);
    }

    /* ── 距离环 ──
     * 只有知道「我在哪」，「离我 5 NM」才有意义。无本机位置时这几圈是在给一个
     * 不存在的原点标刻度，而且 20 NM 档下最内圈 r=50 正好横穿中央那句提示。 */
    if (own_valid) {
        for (int k = 0; k < 3; k++) {
            int nm = RINGS[range_idx][k];
            if (nm <= 0 || nm > range_nm) continue;
            float r = (float)nm / range_nm * RMAX;
            pk_pfd_draw_arc_aa(fb, CX, CY, r, 0.0f, 360.0f, 1.0f, COL_RING);
            snprintf(buf, sizeof(buf), "%d", nm);
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         CX + 2, CY - (int)r - 7, buf, COL_RINGL, 1);
        }
    } else {
        /* 最外那一圈仍要画：它同时是罗盘的**边框**，与刻度、主向字母同属方向
         * 系统，没有位置也成立。第一版把它一起去掉了，结果 30 段刻度成了黑底
         * 上一把断线，比空白更像坏屏——那正是这次要消灭的观感。不标数字：
         * 「20」是距离，而这一态没有距离。 */
        pk_pfd_draw_arc_aa(fb, CX, CY, (float)RMAX, 0.0f, 360.0f, 1.0f, COL_RING);
    }

    /* ── 罗盘刻度 + 主向字母（磁北系）──
     * 这一圈**没有位置也成立**：它说的是方向不是地点，无本机位置时照画，
     * 页面才不至于塌成一片黑，而且半径 184~200，离中央提示很远，不遮挡。 */
    for (int d = 0; d < 360; d += 30) {
        float screen = (orient_eff == PK_MAP_HEADING_UP) ? (float)d - own_heading : (float)d;
        int x1, y1, x2, y2;
        polar(screen, RMAX, &x1, &y1);
        polar(screen, RMAX - (d % 90 == 0 ? 8 : 5), &x2, &y2);
        pk_pfd_draw_line(fb, x1, y1, x2, y2, COL_RING);
    }
    {
        const char *cards[4] = { "N", "E", "S", "W" };
        for (int i = 0; i < 4; i++) {
            int d = i * 90;
            float screen = (orient_eff == PK_MAP_HEADING_UP) ? (float)d - own_heading : (float)d;
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
    /* 被量程挡在外面的架数。空雷达时全靠它区分「按 − 就能看见」和「真的没
     * 收到」——两种情况屏上都是一片空，成因与对策却完全不同。 */
    int n_out_of_range = 0;

    if (own_valid) {
        for (size_t i = 0; i < n; i++) {
            aircraft_t *t = &s_scratch[i];
            if (own.icao24 != 0 && t->icao24 == own.icao24) continue;  /* 不画自己 */
            pk_traffic_rel_t rel = pk_traffic_rel_calc(
                true, own.lat, own.lon, own_heading, mag_var, own_palt,
                t->have_position, t->lat, t->lon,
                t->have_altitude, t->altitude_ft, t->vert_rate_fpm);
            if (!rel.valid) continue;
            if (rel.dist_nm > (float)range_nm) { n_out_of_range++; continue; }
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
    draw_buttons(fb, orient, range_nm);

    if (!own_valid) {
        /*
         * 这是降级态**唯一**的解释，必须一眼可读。
         *
         * 真机反馈「无本机位置几个字被挡住了」，挡它的是页面最后画的那枚本机
         * 符号：30×30 的白色 flight 字形正画在 (CX, CY)，而这句话也居中在
         * (CX, CY)，「机」字被整个抹掉（英文侧吃掉的是 OWN 的 N）。现在这一态
         * 不再画本机符号——不知道自己在哪的时候画一架「我在这」本身就是假话，
         * 顺带也就没人挡它了。距离环与前方扇区同理已在上面跳过。
         *
         * 字号提到 L 档，与看板页的「无目标」同档：两页的空态提示是同一件事，
         * 不该一页大一页小。必须走抗锯齿字体——pk_font_puts 是 5×7 位图只有
         * ASCII 字形，中文进去整句渲染成空白。
         */
        draw_center_notice(fb, CY, COL_AMBER,
                           pk_i18n_text(PK_TR_TFC_NO_OWN_POS), PK_AA_L,
                           pk_i18n_text(PK_TR_TFC_NO_OWN_HINT));
    } else if (nv == 0) {
        /* 有本机、却一架都画不出。两种成因分开说，见 n_out_of_range 的注释。
         * 位置抬到本机符号正上方：那枚符号此刻是有效信息（我在这、朝这边），
         * 不能像上一分支那样撤掉，于是提示让开它。 */
        draw_center_notice(fb, CY - 46, COL_AMBER,
                           pk_i18n_text(n_out_of_range > 0 ? PK_TR_TFC_ALL_OUT_RANGE
                                                           : PK_TR_LIST_NO_CONTACTS),
                           PK_AA_M, NULL);
    } else {
        /* 先摆位再画：标签要互相避让，就必须在动笔之前知道所有目标的落点。
         * 这也是符号与标签分成两趟画的原因——一趟画完，后画的会盖住先画的。 */
        static lbl_t s_lbls[AIRCRAFT_TABLE_CAPACITY];
        for (int k = 0; k < nv; k++) {
            int tx, ty;
            target_pos(&s_vis[k], orient_eff, range_nm, &tx, &ty);
            build_label(&s_lbls[k], &s_vis[k], tx, ty, k == sel_row);
        }
        place_labels(s_lbls, nv, (sel_row >= 0 && sel_row < nv) ? sel_row : -1);

        for (int k = 0; k < nv; k++) {
            if (k == sel_row) continue;                /* 选中最后画(置顶) */
            draw_target_symbol(fb, &s_vis[k], s_lbls[k].tx, s_lbls[k].ty,
                               orient_eff, own_heading, mag_var, false,
                               target_color(&s_vis[k].rel, false));
        }
        if (nv > 0 && sel_row >= 0 && sel_row < nv)
            draw_target_symbol(fb, &s_vis[sel_row],
                               s_lbls[sel_row].tx, s_lbls[sel_row].ty,
                               orient_eff, own_heading, mag_var, true,
                               target_color(&s_vis[sel_row].rel, true));

        for (int k = 0; k < nv; k++) {
            if (k == sel_row) continue;
            draw_label(fb, &s_lbls[k], target_color(&s_vis[k].rel, false));
        }
        if (nv > 0 && sel_row >= 0 && sel_row < nv)
            draw_label(fb, &s_lbls[sel_row],
                       target_color(&s_vis[sel_row].rel, true));
    }

    /* ── 本机飞机符号：HEADING-UP 机头朝上；NORTH-UP 按航向旋转标朝向 ──
     *
     * 无本机位置时不画。两条理由，任何一条都足够：
     *   1. 它是「我在这」的断言，而此刻恰恰不知道我在哪；
     *   2. 它是页面**最后**画的一层，30×30 白色字形正落在 (CX, CY)，把同样
     *      居中的那句「无本机位置」拦腰抹掉——真机反馈的遮挡就是这一处。 */
    if (own_valid)
        draw_own_aircraft(fb,
                          (orient_eff == PK_MAP_NORTH_UP && hdg_valid) ? own_heading : 0.0f,
                          COL_OWN);

    /* 详情不再压在雷达上：同样的信息已经在右栏卡片里（spec §5.2 也把它归到
     * 卡片），雷达区留给图形本身。draw_detail_bar() 暂时保留，等右栏补上机型
     * 与注册号之后一并删除。 */
    (void)draw_detail_bar;
}

/* 圆形按钮的命中判定，半径放宽 BTN_HIT_PAD。 */
static bool hit_btn(int x, int y, int bx, int by)
{
    const int r = BTN_D / 2 + BTN_HIT_PAD;
    const int dx = x - (bx + BTN_D / 2);
    const int dy = y - (by + BTN_D / 2);
    return dx * dx + dy * dy <= r * r;
}

void pk_traffic_page_touch_up(void) { s_btn_down = -1; }

bool pk_traffic_page_touch(int x, int y)
{
    if (hit_btn(x, y, BTN_ORI_X, BTN_ORI_Y)) {
        s_btn_down = 0;
        /* 在两种投影间来回切，与高德点指北针的行为一致。 */
        pk_map_orient_set(pk_map_orient_get() == PK_MAP_HEADING_UP
                              ? PK_MAP_NORTH_UP : PK_MAP_HEADING_UP);
        return true;
    }
    if (hit_btn(x, y, BTN_ZIN_X, BTN_ZIN_Y)) {
        s_btn_down = 1;
        /* 放大 = 看得更近 = 更小的 NM 数 = 更小的档位序号。 */
        const int idx = pk_traffic_range_idx_get();
        if (idx > 0) pk_traffic_range_idx_set(idx - 1);
        return true;
    }
    if (hit_btn(x, y, BTN_ZOUT_X, BTN_ZOUT_Y)) {
        s_btn_down = 2;
        const int idx = pk_traffic_range_idx_get();
        if (idx < 3) pk_traffic_range_idx_set(idx + 1);
        return true;
    }
    return false;
}
