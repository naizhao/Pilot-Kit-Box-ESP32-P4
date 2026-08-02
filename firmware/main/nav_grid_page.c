/*
 * nav_grid_page.c — 见 nav_grid_page.h。
 *
 * 为什么是模态层、为什么自绘不用 LVGL：见头文件。这里只再强调一句版面
 * 来源——本文件的常量与命中判定几何**必须**与 `sim/proto-navgrid/proto.c`
 * 的 `render_c()` / `draw_bright_pop()` 保持一致，那份原型是评审通过的
 * 版面真源，改版面先改原型再抄回来，不要在这里独立发挥。
 *
 * Task 2 范围：纯函数区（分页切分 + 命中判定）；Task 4 补上平台区的
 * 打开/关闭与渲染。触摸状态机（拖动切页、亮度 pop 的开合、点了往哪跳）
 * 仍留给 Task 5。
 */
#include "nav_grid_page.h"

/*
 * 亮度快调 pop 的几何：**命中判定与渲染共用这一组数**。
 *
 * 照 proto.c 的 draw_bright_pop()：三按钮 120×64、间距 10，四边留白同一个
 * pad=10（面板高 = 按钮高 + 2×pad），面板底边离动作条 12 px。初版把面板底边
 * 钉死、按钮顶边从另一头反推，得到上 10 下 20 的不对称，产品负责人当场指出。
 *
 * 提成宏是因为两边各写一份迟早会走偏，而走偏的症状是"看得见的不是点得中的"
 * ——屏上按钮在这儿、命中区在那儿，用户只会觉得屏幕坏了。
 * 这里出现的 800 与头文件同理（display.h 拖 esp_err.h，host 单测编不过）。
 */
#define POP_BW        120
#define POP_BH        64
#define POP_PAD       10
#define POP_W         (POP_BW * 3 + POP_PAD * 2)
#define POP_X0        (800 / 2 - POP_W / 2)
#define POP_BOTTOM    (PK_NAV_ACT_TOP - 12)
#define POP_PANEL_H   (POP_BH + POP_PAD * 2)
#define POP_PY0       (POP_BOTTOM - POP_PANEL_H)
#define POP_Y0        (POP_PY0 + POP_PAD)      /* 按钮顶：离面板顶正好一个 pad */

/* ═══════════════════════════════════════════════════════════════════
 * 纯函数区（无 OS / 无全局状态）——host 单测直接把本文件拉进翻译单元。
 * ═══════════════════════════════════════════════════════════════════ */

int pk_nav_page_first(int page) { return page ? PK_NAV_PAGE1_CNT : 0; }

int pk_nav_page_count(int page)
{
    return page ? (PK_NAV_ITEM_CNT - PK_NAV_PAGE1_CNT) : PK_NAV_PAGE1_CNT;
}

/* index 5「记录」（飞行记录页）、index 6「工具」（工具页）尚未实现——
 * 不是设计上永久禁用，是版面先钉死等实现跟上（产品负责人 2026-08-02 定，
 * 见 nav_grid_page.h 文件头）。做出来之后把这两个 case 删掉即可，其余项
 * 一律可点。越界 index（<0 或 >=PK_NAV_ITEM_CNT）一律当不可用，调用方
 * 不必自己先做范围检查。 */
bool pk_nav_item_enabled(int index)
{
    if (index < 0 || index >= PK_NAV_ITEM_CNT) return false;
    switch (index) {
    case 5: /* 记录：飞行记录页还没写 */
    case 6: /* 工具：工具页还没写 */
        return false;
    default:
        return true;
    }
}

pk_nav_hit_t pk_nav_hit_test(int x, int y, int page, bool pop_open)
{
    pk_nav_hit_t r = { PK_NAV_HIT_NONE, -1 };

    if (pop_open) {
        /* 面板几何取自上面那组 POP_* 宏，与 draw_bright_pop() 同源。 */
        for (int i = 0; i < 3; ++i) {
            const int bx = POP_X0 + i * (POP_BW + POP_PAD);
            if (x >= bx && x < bx + POP_BW &&
                y >= POP_Y0 && y < POP_Y0 + POP_BH) {
                r.kind = PK_NAV_HIT_BRIGHT_STEP;
                r.index = i;
                return r;
            }
        }
        return r;   /* 点别处 = 收起 pop，由调用方处理 */
    }

    /* x 越界（<0 或 >=800）在网格与动作条两段都要挡：C 的整数除法向零
     * 取整，负 x 会算出负 col/负 slot，`slot >= count` 这类上界检查挡不住
     * 负数；x 偏大则会算出 col>=COLS，落进不存在的列。两段用同一条判据，
     * 与 y 方向已有的边界检查（PK_NAV_BAR_BOT / 网格区下沿）对齐。 */
    if (x < 0 || x >= 800) return r;

    if (y >= PK_NAV_ACT_TOP) {
        const int third = 800 / 3;
        r.kind = (x < third) ? PK_NAV_HIT_LEVEL
               : (x < third * 2) ? PK_NAV_HIT_BRIGHT : PK_NAV_HIT_CLOSE;
        return r;
    }

    if (y < PK_NAV_BAR_BOT || y >= PK_NAV_BAR_BOT + PK_NAV_ROWS * PK_NAV_CELL_H)
        return r;

    const int col = x / PK_NAV_CELL_W;
    const int row = (y - PK_NAV_BAR_BOT) / PK_NAV_CELL_H;
    const int slot = row * PK_NAV_COLS + col;
    if (slot >= pk_nav_page_count(page)) return r;   /* 空格什么都不做 */

    const int index = pk_nav_page_first(page) + slot;
    /* 置灰项不可点：命中判定要在这里挡住，不能等调用方拿到 index 再判断——
     * 否则 CELL 这个返回值本身就已经"看得见的是点得中的"，与置灰的视觉承诺
     * 矛盾。点了没反应，不弹提示，置灰视觉本身就是信号（产品负责人
     * 2026-08-02 定）。 */
    if (!pk_nav_item_enabled(index)) return r;

    r.kind = PK_NAV_HIT_CELL;
    r.index = index;
    return r;
}

/*
 * 滑动翻页的两道门槛（见头文件）。
 *
 * 60 px 在 4.3″ 屏上约 7 mm——短于此更像手抖或按压时的轻微位移，翻页会显得
 * 「我什么都没干它自己跳了」。颠簸中的座舱里这个下限只会需要更大，不会更小。
 *
 * `adx < ady * 2` 这一条挡的是斜划：网格本身不纵向滚动，但手指从格子上抬起
 * 时带一点弧线是常态，只看横向位移会把「点了一下、手滑了」判成翻页。
 */
#define NAV_SWIPE_MIN_DX  60      /* 约 7 mm，短于此更像手抖 */

int pk_nav_swipe_dir(int dx, int dy)
{
    const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx < NAV_SWIPE_MIN_DX || adx < ady * 2) return 0;
    return dx > 0 ? -1 : 1;
}

#ifndef PK_NAV_GRID_HOST_TEST

/* ═══════════════════════════════════════════════════════════════════
 * 平台区：打开/关闭 + 渲染 + 触摸状态机。
 * ═══════════════════════════════════════════════════════════════════ */

#include "esp_timer.h"

#include "display.h"
#include "i18n.h"
#include "nav_icon_font.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_layout.h"
#include "pk_ui_nav.h"
#include "search_page.h"
#include "ui_state.h"

/* 头文件里的版面常量用的是字面量 800/480（它不能 include display.h，见那边
 * 的说明）。这里能 include，就把两处对上——数对不上时编译期就炸，不必等到
 * 屏上出现"网格画在半屏外"。 */
_Static_assert(PK_NAV_BAR_BOT == PFD_BAR_BOT,
               "网格顶栏高度与 PFD 顶栏不一致");
_Static_assert(PK_NAV_COLS * PK_NAV_CELL_W == PK_DISPLAY_W,
               "网格列宽乘不满屏宽");
_Static_assert(PK_NAV_ACT_TOP + PK_NAV_ACT_H == PK_DISPLAY_H,
               "动作条没有贴着屏底");

/* ── 调色板：逐值照抄 pk_ui_nav.c（dock 用的就是这几个色号），网格与它
 * 同源，切换形态时颜色不跳。COL_SEL_BG / COL_OFF 是 dock 没有的两档，
 * 出处见下面各自的注释。 ── */
#define COL_FAB     pk_rgb565(0x2E, 0x6D, 0xF0)   /* --sel  主操作色 */
#define COL_BG      pk_rgb565(0x0A, 0x0F, 0x1C)   /* --bar  顶/底栏底色 */
#define COL_DIM     pk_rgb565(0x93, 0xA8, 0xC4)   /* --dim  次要文字 */
#define COL_ON      pk_rgb565(0xE2, 0xEC, 0xF8)   /* --txt  主要文字 */
#define COL_LINE    pk_rgb565(0x1C, 0x27, 0x40)   /* --line 分隔 */
#define COL_ACT     pk_rgb565(0xFF, 0xB4, 0x3F)   /* --warn 动作区 */
#define COL_WHITE   pk_rgb565(0xFF, 0xFF, 0xFF)
/* 选中格的底色。不在 dock 那份调色板里——那份只有边框色，没有"选中卡片的
 * 底"这一档。取 --sel 主色压到约 20% 明度：与深色背景拉得开，又不会亮到把
 * 图标标签压下去。 */
#define COL_SEL_BG  pk_rgb565(0x12, 0x22, 0x44)
/* 置灰项（记录 / 工具，页面还没写）。比 COL_DIM 再暗一档，与"能点的"一眼
 * 分得开；同时**不画选中框**，双重信号。 */
#define COL_OFF     pk_rgb565(0x3D, 0x4D, 0x6B)

/*
 * 覆盖层的遮蔽强度（proto.c 的 OVERLAY_DARKEN，逐值照搬）。
 *
 * 初版取 210（保留 18% 原亮度），实测**不够**：PFD 的高度带、速度带、气压框
 * 都是白底黑字，18% 的白仍有 46 级灰，在深色菜单上是一块块显眼的浅色矩形，
 * 与图标标签直接抢注意力（原型截图里"列表"格右边压着半个 24422）。
 *
 * 232 → 保留 9.4%，白降到 24 级。地平线的明暗分界仍能隐约辨出（飞行员不会
 * 在菜单打开的这两秒里彻底失去姿态参照），但不再有能读出内容的高亮块。
 */
#define NAV_OVERLAY_DARKEN  232

/* 图标与标签之间的间距，以及分页点的直径 / 点距（proto.c 的 render_c）。 */
#define NAV_ICON_GAP   10
#define NAV_DOT_D      12
#define NAV_DOT_PITCH  26

/* ── 项表：index 顺序即版面顺序，与纯函数区的 index 语义一致 ────────
 *
 * mode 是点中之后要切到的 pk_ui_mode_t，同时也是**反查"当前在哪一格"**的
 * 依据（渲染时用 pk_ui_get_mode() 回查）。三种情况没有对应的 mode：
 *   - 搜索：它自己就是个模态层，不是 pk_ui_mode_t 的一站（search_page.h）；
 *   - 记录 / 工具：页面还没写，pk_nav_item_enabled() 已把它们置灰。
 * 这三项填 MODE_NONE；反查时跳过，于是它们永远不会被画成选中态。 */
#define MODE_NONE  (-1)

typedef struct {
    uint8_t  icon;    /* pk_navicon_id_t */
    uint16_t label;   /* pk_tr_id_t —— 屏上文案一律走 catalog，见文件头 */
    int8_t   mode;    /* pk_ui_mode_t，或 MODE_NONE */
} nav_item_t;

static const nav_item_t ITEMS[] = {
    /* ── 第 1 页：飞行中会用的 ── */
    { PK_NAVICON_PFD,    PK_TR_NAV_PFD,      PK_UI_MODE_PFD       },
    { PK_NAVICON_TRF,    PK_TR_NAV_TRAFFIC,  PK_UI_MODE_TRAFFIC   },
    { PK_NAVICON_MAP,    PK_TR_NAV_MAP,      PK_UI_MODE_MAP       },
    { PK_NAVICON_LIST,   PK_TR_NAV_LIST,     PK_UI_MODE_ADSB_LIST },
    { PK_NAVICON_SEARCH, PK_TR_NAV_SEARCH,   MODE_NONE            },
    { PK_NAVICON_REC,    PK_TR_NAV_LOGBOOK,  MODE_NONE            },
    { PK_NAVICON_TOOL,   PK_TR_NAV_TOOLS,    MODE_NONE            },
    /* ── 第 2 页：地面才碰的 ── */
    { PK_NAVICON_DIAG,   PK_TR_NAV_DIAG,     PK_UI_MODE_DIAG      },
    { PK_NAVICON_SET,    PK_TR_NAV_SETTINGS, PK_UI_MODE_SETTINGS  },
    { PK_NAVICON_ABOUT,  PK_TR_NAV_ABOUT,    PK_UI_MODE_ABOUT     },
};
/* 表长与 PK_NAV_ITEM_CNT 分开写迟早走偏：proto.c 里就出过一次——项数常量
 * 硬编码成 11，后来数组少了一项，items[10] 取到数组外的垃圾当 icon id 去算
 * 位图偏移，pk_aa_blit_4bpp 里 SIGBUS。钉在编译期。 */
_Static_assert(sizeof(ITEMS) / sizeof(ITEMS[0]) == PK_NAV_ITEM_CNT,
               "项表长度与 PK_NAV_ITEM_CNT 对不上");

/* ── 动作条与亮度 pop 的文案（全部走 i18n catalog）────────────────
 *
 * 三条都是**借用现成词条**，不在本页另立一份：
 *   ACT_LEVEL  → PK_TR_ACT_LEVEL，dock 的动作页签用的就是它，同词同动作；
 *   ACT_BRIGHT → PK_TR_SETTINGS_BRIGHTNESS（"屏幕亮度" / "BRIGHTNESS"），
 *                设置页那一行的标题，指的是同一个东西；catalog 里没有更短
 *                的「亮度」，而屏上任何硬编码中文都会绕过 catalog——切英文
 *                时漏在那儿，CJK 字形又是 catalog 驱动的子集，绕过它的字
 *                根本不会被生成进字库（渲染成空白）。宽度账：动作条一格
 *                800/3 = 266 px，中文 4 字 × PK_AA_L_CJK_W(32) = 128，
 *                英文 10 字 × PK_AA_L_W(21) = 210，都摆得下。
 *   ACT_CLOSE  → PK_TR_SEARCH_CLOSE（"关闭" / "CLOSE"），搜索页页首那枚
 *                关闭键用的词条，通用词，不含"搜索"语义。
 * 借用的代价是改那几条要顺手看一眼这里；另立一份的代价是两处迟早说岔。 */
#define TXT_ACT_LEVEL   pk_i18n_text(PK_TR_ACT_LEVEL)
#define TXT_ACT_BRIGHT  pk_i18n_text(PK_TR_SETTINGS_BRIGHTNESS)
#define TXT_ACT_CLOSE   pk_i18n_text(PK_TR_SEARCH_CLOSE)

/* ── 状态 ────────────────────────────────────────────────────────
 * 这三个变量决定 render 画出什么，触摸状态机（下面那一节）是改它们的唯一入口。 */
static bool s_active;
static int  s_page;
static bool s_pop_open;

/* ── 触摸的按压态 ─────────────────────────────────────────────────
 *
 * 归属按下即定死（pk_touch_arbiter.h），所以这一组量在 touch() 里一次性记全，
 * 之后 drag / touch_up 只读不重判——「这一下是点在哪儿的」不允许随手指移动
 * 而改变，否则就是「拖 FAB 被列表抢走」的另一种形状。 */
static bool         s_press_valid;   /* 本次按压还没被结算/作废 */
static int          s_press_x, s_press_y;
static int          s_press_page;    /* 按下那一刻在第几页（之后可能已翻页） */
static pk_nav_hit_t s_press_hit;     /* 按下那一刻命中了什么 */
static int64_t      s_press_us;      /* 按下时刻，调平长按据此计时 */
static bool         s_swiped;        /* 本次按压已经判定过滑动：不再算点击。
                                       * 翻到有效页之后仍可能继续滑，所以它
                                       * 不兼任「还要不要继续侦测」——那是
                                       * s_swipe_locked 的职责，见 drag()。 */
static bool         s_swipe_locked;  /* 滑动判定已到终局（关闭待定 / 末页
                                       * 到界），本次按压剩下的时间不再继续
                                       * 侦测，直到松手/取消。 */
static bool         s_close_on_up;   /* 松手时关闭网格（见 touch_up 上方） */
static int64_t      s_last_act_us;   /* 最后一次触摸，5 s 无操作自动收起用 */

/* ── 绘制 ────────────────────────────────────────────────────────── */

/* 图标走 pk_aa_blit_4bpp：与文字完全同一条 alpha 混合路径（同样的曲线、
 * 同样的大端换序）。图标与标签的边缘处理因此天然一致，不会出现"字是柔的、
 * 图是硬的"那种割裂感。(cx, cy) 是图标中心，故左上角要减去半个 cell。 */
static void draw_icon(uint16_t *fb, int cx, int cy, uint8_t id, uint16_t color)
{
    const uint8_t *bmp = pk_navicon_bitmap +
                         (size_t)id * (PK_NAVICON_W * PK_NAVICON_H / 2);
    pk_aa_blit_4bpp(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                    cx - PK_NAVICON_W / 2, cy - PK_NAVICON_H / 2,
                    bmp, PK_NAVICON_W, PK_NAVICON_H, color);
}

/*
 * 选中态：深蓝底 + 一圈圆角边。pfd_draw 只有填充版圆角矩形，边框用"大的画
 * 一个、小的再盖一个"叠出来——boot_splash 里的按钮也是这么做的。
 *
 * **绝对不要**在内层叠 pk_pfd_darken_rect() 去压"淡蓝底"：darken_rect 作用于
 * **矩形**，会把圆角外侧那四小块蓝边一起压暗，四个角的边框颜色掉下去，看上去
 * 就是个直角框（原型里踩过并修好的坑）。底色直接填一个深蓝常量，形状交给
 * fill_round_rect 自己保证。
 */
static void draw_cell_selected(uint16_t *fb, int x0, int y0, int x1, int y1)
{
    pk_pfd_fill_round_rect(fb, x0, y0, x1, y1, 16, COL_FAB);
    pk_pfd_fill_round_rect(fb, x0 + 3, y0 + 3, x1 - 3, y1 - 3, 13, COL_SEL_BG);
}

static void draw_cell(uint16_t *fb, int x, int y, const nav_item_t *it,
                      bool selected, bool enabled)
{
    const int w = PK_NAV_CELL_W, h = PK_NAV_CELL_H;

    /* 置灰项不画选中框——它们对应的页面还不存在，也就永远不是"当前所在页"。 */
    if (selected) draw_cell_selected(fb, x + 8, y + 6, x + w - 8, y + h - 6);

    const uint16_t color = enabled ? COL_ON : COL_OFF;
    const int cx = x + w / 2;

    /* 图标与标签作为一个整体在格内垂直居中：图标高 + 间距 + 字高。
     * 不是"图标居中、标签往下塞"——那样整组的视觉重心会偏上。 */
    const int lh    = pk_aa_cell_h(PK_AA_L);
    const int total = PK_NAVICON_H + NAV_ICON_GAP + lh;
    const int top   = y + (h - total) / 2;

    draw_icon(fb, cx, top + PK_NAVICON_H / 2, it->icon, color);

    /* 宽度必须走 pk_aa_text_width：strlen × cell_w 数的是字节，一个汉字
     * 3 字节却只画一个字形，中文标签会被推到格子左边去。 */
    const char *label = pk_i18n_text((pk_tr_id_t)it->label);
    const int   tw    = pk_aa_text_width(label, PK_AA_L);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               cx - tw / 2, top + PK_NAVICON_H + NAV_ICON_GAP,
               label, color, PK_AA_L);
}

/*
 * 当前在哪一格：拿 pk_ui_get_mode() 回查项表。
 *
 * 反查而不是自己存一个"上次点的是第几格"：模式还能从别的入口切（返回栏、
 * 调平向导自动进出、演示模式），存一份迟早与真值分家。查不到（比如
 * PK_UI_MODE_CAL_WIZARD 不在表里）返回 -1，此时一个选中框都不画。
 */
static int selected_index(void)
{
    const pk_ui_mode_t mode = pk_ui_get_mode();
    for (int i = 0; i < PK_NAV_ITEM_CNT; ++i) {
        if (ITEMS[i].mode >= 0 && (pk_ui_mode_t)ITEMS[i].mode == mode)
            return i;
    }
    return -1;
}

/*
 * 亮度快调 pop：从动作条的「亮度」上方弹出，横向三档。
 *
 * 底层全是现成的——display.h 的 PK_BL_STEP_LOW/MID/HIGH 与
 * pk_backlight_step_get()，档位的真值只有那一处，不会与设置页分家。
 * 几何走 POP_* 宏，与 pk_nav_hit_test() 同源（见那组宏上方的说明）。
 */
static void draw_bright_pop(uint16_t *fb)
{
    static const uint16_t STEPS[3] = {
        PK_TR_BRIGHT_LOW, PK_TR_BRIGHT_MID, PK_TR_BRIGHT_HIGH,
    };
    const int cur = (int)pk_backlight_step_get();
    const int lh  = pk_aa_cell_h(PK_AA_L);

    /* 面板本体：**不透明**。它盖在网格上，半透明会让底下的图标透出来搅在
     * 一起。同样是"大的画一个、小的再盖一个"叠出一圈边。 */
    pk_pfd_fill_round_rect(fb, POP_X0 - POP_PAD, POP_PY0,
                           POP_X0 + POP_W + POP_PAD, POP_BOTTOM, 14, COL_LINE);
    pk_pfd_fill_round_rect(fb, POP_X0 - POP_PAD + 2, POP_PY0 + 2,
                           POP_X0 + POP_W + POP_PAD - 2, POP_BOTTOM - 2,
                           12, COL_BG);

    for (int i = 0; i < 3; ++i) {
        const int  bx = POP_X0 + i * (POP_BW + POP_PAD);
        const bool on = (i == cur);
        if (on)
            pk_pfd_fill_round_rect(fb, bx, POP_Y0, bx + POP_BW,
                                   POP_Y0 + POP_BH, 10, COL_FAB);

        const char *t  = pk_i18n_text((pk_tr_id_t)STEPS[i]);
        const int   tw = pk_aa_text_width(t, PK_AA_L);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   bx + (POP_BW - tw) / 2, POP_Y0 + (POP_BH - lh) / 2,
                   t, on ? COL_WHITE : COL_DIM, PK_AA_L);
    }
}

/*
 * 5 s 无操作自动收起——三条退路的第三条（另两条是动作条的「关闭」与第 0 页
 * 右滑）。飞行中忘记收起是常态，不能让菜单一直盖着 PFD。
 *
 * 5000 的来历是 dock 的 DOCK_IDLE_MS（pk_ui_nav.c），沿用它是为了不打破用户
 * 已经养成的手感。**不直接引用那个宏**：Task 6 要连 dock 一起删掉，引用过去
 * 只会让删除时多一处返工，而且网格的内容比 dock 多（两页十项），上机后这个
 * 值多半要往上调——那时它该是本模块自己的参数，不是 dock 的遗产。
 */
#define NAV_IDLE_MS   5000

/* 判定放在 render 里：本层没有后台任务，而 render 恰好"网格活着时每帧调
 * 一次、收起后一次不调"，正是这个倒计时需要的心跳。判定时手指必然不在屏上
 * （touch/drag 每一帧都在刷新 s_last_act_us），所以这里关闭不会撞上下面那条
 * 「关闭一律等松手」的规矩。 */
static bool idle_expired(void)
{
    return (esp_timer_get_time() - s_last_act_us) >= (int64_t)NAV_IDLE_MS * 1000;
}

void pk_nav_grid_page_render(uint16_t *fb)
{
    if (idle_expired()) {
        /* 直接返回不画：本帧底页已经由 pfd.c 照常画完了（网格不进那条模态
         * if/else 链），少叠一层覆盖层就是"菜单收起"该有的样子。 */
        pk_nav_grid_page_close();
        return;
    }

    /* 遮罩。顶栏（y < PK_NAV_BAR_BOT）不遮——电量 / GPS / 蓝牙这些状态在菜单
     * 打开期间同样要能看见。
     *
     * darken_rect 是**就地**把已有像素压暗，所以底下那一页必须每帧重画一遍，
     * 否则同一批像素被逐帧反复压暗，两三帧就全黑了（canvas 是单块常驻缓冲，
     * lv_port.c 的 pk_lv_port_canvas_px）。分派处 pfd.c 已按这条写：网格不进
     * 那条模态 if/else 链，而是"底页照常画完，再把网格叠上去"。 */
    pk_pfd_darken_rect(fb, 0, PK_NAV_BAR_BOT, PK_DISPLAY_W, PK_DISPLAY_H,
                       NAV_OVERLAY_DARKEN);

    /*
     * 一律从左上角排起，**不**因为本页项数少就居中。
     *
     * 格子位置一旦随项数浮动，同一个功能在第 1 页和第 2 页就落在不同的坐标，
     * 手指记不住位置——而记住位置正是网格相对列表的全部优势。空就空着，位置
     * 必须钉死（产品负责人 2026-08-02 明确否掉了"末页居中"）。
     */
    const int first = pk_nav_page_first(s_page);
    const int n     = pk_nav_page_count(s_page);
    const int sel   = selected_index();

    for (int i = 0; i < n; ++i) {
        const int col = i % PK_NAV_COLS;
        const int row = i / PK_NAV_COLS;
        const int idx = first + i;
        draw_cell(fb, col * PK_NAV_CELL_W,
                  PK_NAV_BAR_BOT + row * PK_NAV_CELL_H,
                  &ITEMS[idx], idx == sel, pk_nav_item_enabled(idx));
    }

    /* 分页点：只是状态指示，不可点（pk_nav_hit_test 那段 y 范围压根不覆盖
     * 这一条）。proto.c 里写的是 W/2 - 16 + d*26，那是两点时的手写近似
     * （起点 384）；这里按页数算出真正的居中起点（两点是 381），差 3 px 肉眼
     * 不可见，但 PK_NAV_PAGES 一改就不用回来改这里。 */
    const int dots_w  = (PK_NAV_PAGES - 1) * NAV_DOT_PITCH + NAV_DOT_D;
    const int dots_x0 = (PK_DISPLAY_W - dots_w) / 2;
    for (int d = 0; d < PK_NAV_PAGES; ++d) {
        const int dx = dots_x0 + d * NAV_DOT_PITCH;
        pk_pfd_fill_round_rect(fb, dx, PK_NAV_DOT_Y,
                               dx + NAV_DOT_D, PK_NAV_DOT_Y + NAV_DOT_D,
                               NAV_DOT_D / 2,
                               d == s_page ? COL_FAB : COL_LINE);
    }

    /* 动作条：与导航格物理分离，误触调平的路被堵死。 */
    pk_pfd_fill_rect(fb, 0, PK_NAV_ACT_TOP, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);
    pk_pfd_fill_rect(fb, 0, PK_NAV_ACT_TOP, PK_DISPLAY_W, PK_NAV_ACT_TOP + 1,
                     COL_LINE);
    {
        const char *acts[3] = { TXT_ACT_LEVEL, TXT_ACT_BRIGHT, TXT_ACT_CLOSE };
        const int   lh      = pk_aa_cell_h(PK_AA_L);
        for (int i = 0; i < 3; ++i) {
            /* 调平是唯一会改变飞机状态显示的动作，用警示橙与另两个分开。
             * 亮度被点开时它自己也高亮，否则 pop 弹出来会像凭空冒出的一块。 */
            uint16_t c = (i == 0) ? COL_ACT : COL_DIM;
            if (i == 1 && s_pop_open) c = COL_ON;
            const int cx = PK_DISPLAY_W * (2 * i + 1) / 6;   /* 三等分的格心 */
            const int tw = pk_aa_text_width(acts[i], PK_AA_L);
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                       cx - tw / 2, PK_NAV_ACT_TOP + (PK_NAV_ACT_H - lh) / 2,
                       acts[i], c, PK_AA_L);
        }
    }

    if (s_pop_open) {
        /* pop 弹出时把网格再压一档：不压的话两层内容一样亮，看不出焦点在哪
         * 一层，而这时候唯一能点的只有 pop 里那三个档（pk_nav_hit_test 的
         * pop_open 分支已经把网格整层吞掉了）。动作条不压——「亮度」那一格
         * 正高亮着，压暗它等于把"是我弹出来的"这条线索抹掉。 */
        pk_pfd_darken_rect(fb, 0, PK_NAV_BAR_BOT, PK_DISPLAY_W,
                           PK_NAV_ACT_TOP, 120);
        draw_bright_pop(fb);
    }
}

/* ── 打开 / 关闭 ─────────────────────────────────────────────────── */

/* 没有后台任务、没有 NVS，状态全在上面那三个静态变量里，所以 init 只是把它们
 * 摆回初值。留着这个函数是为了与 search_page / keyboard_page 的生命周期惯例
 * 对齐（调用方不必记"这一个例外不用 init"）。幂等。 */
/* 按压态归零。open / close / cancel 三处共用——漏掉任何一处，上一次没结算完
 * 的按压就会跨过一次开合活下来（表现是"一打开网格就自己翻了一页"）。 */
static void press_reset(void)
{
    s_press_valid  = false;
    s_swiped       = false;
    s_swipe_locked = false;
    s_close_on_up  = false;
}

void pk_nav_grid_page_init(void)
{
    s_active   = false;
    s_page     = 0;
    s_pop_open = false;
    press_reset();
}

void pk_nav_grid_page_open(void)
{
    /* 每次都从第 1 页、pop 收起开始：菜单是个瞬时动作，上次翻到第 2 页不代表
     * 这次还想看第 2 页，而"打开就在熟悉的那一屏"比"记住上次"更省认知。 */
    s_page     = 0;
    s_pop_open = false;
    s_active   = true;
    press_reset();
    /* 倒计时从打开这一刻起算，而不是从第一次触摸起算——打开后一下都没碰，
     * 5 s 后同样该自己收起。 */
    s_last_act_us = esp_timer_get_time();
    /*
     * 藏掉 FAB，理由与 keyboard_page / search_page 完全相同：本层铺满全屏、
     * 命中判定排在 LVGL 之前，FAB 留着就是"它自己点不动、又盖住底下的格"。
     * 而且 FAB 可拖动且落点存 NVS，用户把它拖到哪它就挡住哪一格，挡哪一格
     * 还不可预测（原型 navgrid-A.png 里第 12 格就是这么被吃掉的）。
     * 出口写在屏上：动作条右边那枚「关闭」。
     */
    pk_ui_nav_set_fab_hidden(true);
}

bool pk_nav_grid_page_active(void) { return s_active; }

void pk_nav_grid_page_close(void)
{
    s_active   = false;
    s_pop_open = false;
    press_reset();
    pk_ui_nav_set_fab_hidden(false);
}

/* ── 触摸状态机 ──────────────────────────────────────────────────
 *
 * 「调平」必须长按 1 s 才生效：误触把地平线归零，飞行中是要命的。四个状态
 * 与 dock 那枚调平键逐条对齐（pk_ui_nav.c 的 act_event_cb）：
 *
 *     按下       记下时刻
 *     满 1 s     pk_ui_nav_on_level()，真正执行
 *     提前松手   pk_ui_nav_on_level_hint()（提示"需长按 1 秒"）
 *     滑出按钮   同上（等价于 LVGL 的 PRESS_LOST）
 *
 * 计时用 esp_timer_get_time() 而不是 lv_timer：本层是自绘的，drag() 在手指
 * 按住期间**每一轮触摸轮询都会被调到**（touch_gt911.c 的 PK_TOUCH_ACTION_DRAG
 * 不要求手指移动），已经是一个现成的、比 1 s 密得多的心跳，再挂一个 LVGL
 * 定时器只是多一个要记得删的对象。反过来也不能用 LVGL 的 LONG_PRESSED——
 * 它的阈值 lv_indev_set_long_press_time() 是 indev 全局的，改了会一并影响
 * FAB 的起拖判定（那里要的是 200 ms）。
 *
 * 动作与执行分两级，且**关闭网格一律等到松手**（s_close_on_up）：
 * 手指还按着就把 s_active 清掉的话，下一轮触摸轮询在 touch_gt911.c 里算出的
 * pk_ui_modal_top() 已经不是 NAVGRID 了，这一次按压的剩余帧会落到底下那一页
 * 上——底页是地图时后果尤其具体：map_page.c:611 的状态机会把它当成一次全新
 * 按下，随后 map_page.c:696 的 tap 判定成立，手一松就跳进机场详情页。
 */
#define NAV_LEVEL_HOLD_MS  1000

/* 点中格子之后往哪跳。
 *
 * 复用上面那张 ITEMS 表，不另抄一份 index→动作的映射：抄的那份不会跟着版面
 * 变，改一次排序就会悄悄把「关于」接到「诊断」上去（pk_ui_nav_host.c 的
 * mode_for_tab 用词条 id 做键，也是同一个理由）。搜索同样按词条 id 认，
 * 不写死"第 5 格"。 */
static void activate_item(int index)
{
    if (index < 0 || index >= PK_NAV_ITEM_CNT) return;
    /* hit_test 已经挡过置灰项，这里再挡一次是因为本函数只信自己的入参。 */
    if (!pk_nav_item_enabled(index)) return;

    if (ITEMS[index].mode >= 0) {
        pk_ui_set_mode((pk_ui_mode_t)ITEMS[index].mode);
    } else if (ITEMS[index].label == PK_TR_NAV_SEARCH) {
        /* 搜索是模态层，不是 pk_ui_mode_t 的一站：只打开它，当前是哪一页不变
         * （见 search_page.h）。 */
        pk_search_page_open();
    } else {
        return;   /* 记录 / 工具：页面还没写，enabled 已挡，走不到这儿 */
    }
    pk_nav_grid_page_close();
}

bool pk_nav_grid_page_touch(int x, int y)
{
    if (!s_active) return false;

    s_press_hit    = pk_nav_hit_test(x, y, s_page, s_pop_open);
    s_press_x      = x;
    s_press_y      = y;
    s_press_page   = s_page;
    s_press_us     = esp_timer_get_time();
    s_last_act_us  = s_press_us;
    s_press_valid  = true;
    s_swiped       = false;
    s_swipe_locked = false;
    s_close_on_up  = false;

    /* 整屏都吃，命中与否都一样：网格铺满全屏且 FAB 已藏，底下没有任何该被
     * 点到的东西；更要紧的是横向滑动可以从任何一格上起手，归属必须在**按下
     * 这一刻**就定给本层，不能等划出阈值再抢（pk_touch_arbiter.h）。 */
    return true;
}

bool pk_nav_grid_page_drag(int x, int y)
{
    if (!s_active) return false;
    s_last_act_us = esp_timer_get_time();
    if (!s_press_valid) return true;   /* 仍然吃掉：模态 */

    /* ① 滑动翻页：过阈值当场换页，不等松手（产品负责人 2026-08-02 反馈
     * "左右滑不跟手，尤其速度快的时候"——拖动全程改 s_page，而不是等
     * touch_up() 才判定）。pop 开着时仍不翻——那时网格整层已被压暗且不可点
     * （pk_nav_hit_test 的 pop_open 分支），底下悄悄翻页只会让人一头雾水。
     *
     * 「顺手收起 pop 再翻页」这条想过、没做：pop 面板总宽只有 380 px，起手点
     * 落在某个快调按钮上、手指再侧移五六十像素完全可能只是想点相邻档位，
     * 拿去当翻页/关闭信号会把一次正常的选档操作误判掉。松手时已有的兜底
     * （touch_up 的 default 分支：pop 开着时点面板外一律 PK_NAV_HIT_NONE，
     * 收起 pop）已经够用，不需要在 drag 里再抢一次。
     *
     * 落到有效页时（目标页存在）当场改 s_page，并把起点重置到当前指尖
     * 位置——这样一次长滑可以连续翻多页，不必翻一页就锁住等下一次按压。
     * 两种到界的终局（第 0 页继续右滑要关闭 / 末页继续同向不做事）不重置
     * 起点，判一次就锁死到松手：这两种没有"下一页"可去，继续侦测没有意义；
     * 锁死还避免了"关闭待定之后又被反向滑动悄悄推翻，但 s_close_on_up 没
     * 跟着清掉"这种状态不一致。s_swiped 记"这一下算不算点击"（只要滑动过就
     * 一直是 true，供 touch_up 用），s_swipe_locked 记"还要不要继续判"
     * （只在两种终局分支置位），两件事分开存正是为了不让上面这条边界情况
     * 无解——都塞进同一个标志位，翻到有效页后要么没法继续判，要么终局分支
     * 判完还能被继续判。 */
    if (!s_swipe_locked && !s_pop_open) {
        const int dir = pk_nav_swipe_dir(x - s_press_x, y - s_press_y);
        if (dir != 0) {
            s_swiped = true;   /* 不管落进哪个分支，这一下都不再算点击 */
            if (dir < 0 && s_page == 0) {
                /* 三条退路之二：第 0 页继续右滑 = 关闭。仍然延到松手结算
                 * （s_close_on_up），理由见 touch_up 上方大注释——手指还按着
                 * 就清 s_active 的话，剩余帧会落到底页，底页是地图时手一松
                 * 就会被当成一次新按下，跳进机场详情页。 */
                s_close_on_up  = true;
                s_swipe_locked = true;
            } else {
                const int np = s_page + dir;
                if (np >= 0 && np < PK_NAV_PAGES) {
                    s_page    = np;   /* 当场翻页 */
                    s_press_x = x;    /* 重置起点：支持一次长滑连续翻页 */
                    s_press_y = y;
                } else {
                    /* 最后一页继续左滑：不做任何事，也不循环回第 0 页。 */
                    s_swipe_locked = true;
                }
            }
            return true;
        }
    }

    /* ② 调平长按。翻过页的这一下不再算按钮操作。 */
    if (!s_swiped && s_press_hit.kind == PK_NAV_HIT_LEVEL) {
        if (pk_nav_hit_test(x, y, s_press_page, s_pop_open).kind
                != PK_NAV_HIT_LEVEL) {
            /* 滑出按钮 = 放弃，等同 LVGL 的 PRESS_LOST。 */
            s_press_valid = false;
            pk_ui_nav_on_level_hint();
        } else if (esp_timer_get_time() - s_press_us
                       >= (int64_t)NAV_LEVEL_HOLD_MS * 1000) {
            /* 满 1 s 当场执行（提示随即弹出，手感与 dock 一致），网格留到
             * 松手再关——理由见本节开头。 */
            s_press_valid = false;      /* 已消费，松手不再重复结算 */
            s_close_on_up = true;
            pk_ui_nav_on_level();
        }
    }
    return true;
}

void pk_nav_grid_page_touch_cancel(void)
{
    /* 本次按压作废，**不结算**：取消是"这一下当没发生过"，补一条"需长按
     * 1 秒"的提示反而是无中生有。 */
    press_reset();
}

void pk_nav_grid_page_touch_up(void)
{
    if (!s_active) return;

    /* 先取快照再清状态：下面的分支会调 close()，而 close() 也会清这几个量。 */
    const bool         close_on_up = s_close_on_up;
    const bool         valid       = s_press_valid;
    const bool         swiped      = s_swiped;
    const pk_nav_hit_t hit         = s_press_hit;
    const int64_t      held_us     = esp_timer_get_time() - s_press_us;
    press_reset();

    if (close_on_up) { pk_nav_grid_page_close(); return; }
    if (!valid || swiped) return;      /* 翻过页的这一下不再算点击 */

    switch (hit.kind) {
    case PK_NAV_HIT_CELL:
        activate_item(hit.index);
        break;

    case PK_NAV_HIT_LEVEL:
        /* 满 1 s 的那条路在 drag() 里就走完了（按压随即被消费掉），能走到这儿
         * 的实际上只有短按。仍然按时长判一次而不是无条件 hint：万一哪天触摸
         * 轮询稀疏到一次 drag 都轮不上，也不该把一次真正的长按提示成短按。 */
        if (held_us >= (int64_t)NAV_LEVEL_HOLD_MS * 1000) {
            pk_ui_nav_on_level();
            pk_nav_grid_page_close();
        } else {
            pk_ui_nav_on_level_hint();
        }
        break;

    case PK_NAV_HIT_BRIGHT:
        s_pop_open = true;
        break;

    case PK_NAV_HIT_CLOSE:
        /* 三条退路之一。 */
        pk_nav_grid_page_close();
        break;

    case PK_NAV_HIT_BRIGHT_STEP:
        /* index 与 display.h 的 PK_BL_STEP_LOW/MID/HIGH 同序（见 nav_grid_page.h
         * 那条枚举的注释）。档位真值只有 pk_backlight_* 一处，不与设置页分家。 */
        pk_backlight_step_set((uint8_t)hit.index);
        s_pop_open = false;
        break;

    case PK_NAV_HIT_NONE:
    default:
        /* pop 开着时命中判定只测那三个档位，点别处一律 NONE = 收起 pop
         * （不关网格）。pop 没开时点空处什么都不做。 */
        s_pop_open = false;
        break;
    }
}

#endif /* !PK_NAV_GRID_HOST_TEST */
