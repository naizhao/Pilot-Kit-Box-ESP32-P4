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

/*
 * 电源 pop 的几何（同样是**命中判定与渲染共用这一组数**，理由见上）。
 *
 * 与亮度 pop 的区别只有两点：两个按钮而不是三个，以及按钮上方多一行提示。
 * 面板整体居中，不复用 POP_X0（那是按三个按钮算的）。
 *
 * 宽度 240 是**按英文量出来的，不是估的**，而且是两个约束里更狠的那个：
 *   按钮：最长标签 "Power Off" 9 字符 × PK_AA_L_W(21) = 189 px；
 *   提示行：最长 41 字符 × PK_AA_S_W(11) = 451 px —— 它才是决定宽度的那条。
 * 面板内宽 PWR_W = 240×2+10 = 490 → 按钮余 51、提示行余 39，都够。
 * 面板总宽 510，居中后 x∈[145,655]，屏内有余。
 *
 * 两次踩坑都记在这儿：初版 170（照着中文「关机」2×32=64 想当然）英文
 * "Off" 直接压出按钮右缘；改 210 之后按钮好了，**提示行仍溢出 21 px**，
 * 而这一次截图上看不出来——它居中，两侧各超 10 px 正好压在圆角边框上。
 * 结论：按钮宽度靠英文截图验，提示行只能靠算。改文案或改宽度时两样都做，
 * 下面那两条 _Static_assert 会在编译期兜住。
 *
 * 提示行（PWR_HINT_H）写的是"关机后需插 USB 唤醒；重启只重启主控"——
 * /QON 悬空导致关机后只能插 USB 唤醒（SY6970 DS p.30），不写这句用户会
 * 以为设备坏了。它只是文字，不参与命中。
 */
#define PWR_BW        240
#define PWR_BH        64
#define PWR_PAD       10
#define PWR_HINT_H    28
#define PWR_W         (PWR_BW * 2 + PWR_PAD)
#define PWR_X0        (800 / 2 - PWR_W / 2)
#define PWR_BOTTOM    (PK_NAV_ACT_TOP - 12)
#define PWR_PANEL_H   (PWR_HINT_H + PWR_BH + PWR_PAD * 2)
#define PWR_PY0       (PWR_BOTTOM - PWR_PANEL_H)
#define PWR_Y0        (PWR_PY0 + PWR_PAD + PWR_HINT_H)  /* 按钮顶在提示行下 */

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

pk_nav_hit_t pk_nav_hit_test(int x, int y, int page, pk_nav_pop_t pop)
{
    pk_nav_hit_t r = { PK_NAV_HIT_NONE, -1 };

    if (pop == PK_NAV_POP_BRIGHT) {
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

    if (pop == PK_NAV_POP_POWER) {
        /* 几何取自 PWR_* 宏，与 draw_power_pop() 同源。左关机、右重启，
         * 顺序与渲染一致；别处一律 NONE（点面板外 = 收起，调用方处理）。 */
        if (y >= PWR_Y0 && y < PWR_Y0 + PWR_BH) {
            if (x >= PWR_X0 && x < PWR_X0 + PWR_BW) {
                r.kind = PK_NAV_HIT_POWER_OFF;
                return r;
            }
            const int bx = PWR_X0 + PWR_BW + PWR_PAD;
            if (x >= bx && x < bx + PWR_BW) {
                r.kind = PK_NAV_HIT_POWER_RESTART;
                return r;
            }
        }
        return r;
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

#include "esp_system.h"   /* esp_restart —— 电源 pop 的「重启」 */
#include "esp_timer.h"

#include "display.h"
#include "power_service.h" /* power_service_snapshot —— 关机前查 VBUS 在不在 */
#include "power_sy6970.h" /* power_sy6970_shutdown —— 电源 pop 的「关机」 */
#include "i18n.h"
#include "nav_icon_font.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_layout.h"
#include "apt_detail_page.h"   /* pk_ui_fab_sync —— FAB 显隐的唯一入口 */
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
/*
 * 头文件与 pk_nav_hit_test() 里的屏宽是**字面量 800**（那边不能 include
 * display.h，理由见头文件），而渲染这侧用的是 PK_DISPLAY_W。两处写的是同一个
 * 数却没有语法上的联系——动作条三等分尤其危险：命中判定按 800/3 分，渲染按
 * PK_DISPLAY_W/3 分，屏宽一改就会变成"看得见的不是点得中的"，而且是**静默**的。
 * 钉在这里：换屏时编译期就炸，不必等到手指点空。
 */
_Static_assert(PK_DISPLAY_W == 800,
               "头文件里的字面量 800 与 display.h 的 PK_DISPLAY_W 不一致");
_Static_assert(PK_DISPLAY_H == 480,
               "头文件里的字面量 480 与 display.h 的 PK_DISPLAY_H 不一致");
/*
 * 电源 pop 的按钮要装得下最长标签。9 = "Power Off" 的字符数（英文比中文
 * 宽得多，中文「关机」只要 2×PK_AA_L_CJK_W）。初版 PWR_BW=170 时英文在
 * 截图上直接溢出按钮右缘，而中文那张完全看不出问题——所以这条断言钉的是
 * **英文**下限。以后若把文案改得更长，这里会在编译期炸，而不是等谁去翻
 * 英文截图才发现。
 */
_Static_assert(PWR_BW >= 9 * PK_AA_L_W,
               "电源 pop 按钮装不下英文 \"Power Off\"（9 × PK_AA_L_W）");
/*
 * 提示行比按钮更容易溢出，而且溢出**在截图上看不出来**（居中，两侧各超一点
 * 正好压在圆角边框上）——PWR_BW=210 那一版就是这么混过目视检查的。
 * 41 = "Power off: USB to wake. Restart: P4 only." 的字符数。
 */
_Static_assert(PWR_W >= 41 * PK_AA_S_W,
               "电源 pop 面板装不下英文提示行（41 × PK_AA_S_W）");
/* 面板别撑出屏外。 */
_Static_assert(PWR_X0 - PWR_PAD >= 0 &&
                   PWR_X0 + PWR_W + PWR_PAD <= PK_DISPLAY_W,
               "电源 pop 面板超出屏宽");

/* ── 调色板：逐值照抄 spec 视觉稿（docs/ux/box-4.3-ux-spec.html 的 --sel /
 * --bar / --dim / --txt / --line / --warn），与 pk_ui_nav.c 的 FAB、返回栏
 * 同源，两层叠在一起颜色不跳。COL_SEL_BG / COL_OFF 是视觉稿里没有的两档，
 * 出处见下面各自的注释。 ── */
#define COL_FAB     pk_rgb565(0x2E, 0x6D, 0xF0)   /* --sel  主操作色 */
#define COL_BG      pk_rgb565(0x0A, 0x0F, 0x1C)   /* --bar  顶/底栏底色 */
#define COL_DIM     pk_rgb565(0x93, 0xA8, 0xC4)   /* --dim  次要文字 */
#define COL_ON      pk_rgb565(0xE2, 0xEC, 0xF8)   /* --txt  主要文字 */
#define COL_LINE    pk_rgb565(0x1C, 0x27, 0x40)   /* --line 分隔 */
#define COL_ACT     pk_rgb565(0xFF, 0xB4, 0x3F)   /* --warn 动作区 */
#define COL_WHITE   pk_rgb565(0xFF, 0xFF, 0xFF)
/* 选中格的底色。视觉稿里没有这一档——那份只给了边框色，没有"选中卡片的
 * 底"。取 --sel 主色压到约 20% 明度：与深色背景拉得开，又不会亮到把图标
 * 标签压下去。 */
#define COL_SEL_BG  pk_rgb565(0x12, 0x22, 0x44)
/* 置灰项（记录 / 工具，页面还没写）。比 COL_DIM 再暗一档，与"能点的"一眼
 * 分得开；同时**不画选中框**，双重信号。 */
#define COL_OFF     pk_rgb565(0x3D, 0x4D, 0x6B)
/*
 * 调平长按的进度填充底色 —— COL_ACT 压到约 35%（255/180/63 → 89/63/22）。
 *
 * 为什么是「压暗的同色」而不是另起一个色相：填充是**背景**、橙色标签是
 * **前景**，同色系才让人一眼读成"这一格自己在充满"；换个色相就变成"这一格
 * 旁边多了个别的东西"。
 *
 * 35% 是两头夹出来的：再亮一档，压在上面的 COL_ACT 标签对比度掉到 4.5:1
 * 以下（WCAG AA 的下限），字就被填充吃掉了；再暗一档，与动作条底色 COL_BG
 * 分不开，进度走到哪儿看不出来。这一档实测对标签 5.5:1、对底色 2.0:1。
 */
#define COL_ACT_FILL pk_rgb565(0x59, 0x3F, 0x16)
/*
 * 完成绿闪。调色板里没有绿档——座舱里绿 = 正常/确认，必须与 COL_ACT 的
 * 橙 = 需注意 拉开，所以不能拿现成的任何一档改亮度凑出来。取值参照仪表面板
 * 那种偏青的「正常」绿：够亮，200 ms 里余光也能捕捉到；不刺眼，夜航时不会
 * 在暗适应的眼睛里留下残像。
 */
#define COL_OK      pk_rgb565(0x2F, 0xB2, 0x62)

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
    /* 电源：不是一站 mode，点它弹出「关机 / 重启」二选一（activate_item
     * 里按词条 id 认，同搜索那条的理由）。 */
    { PK_NAVICON_POWER,  PK_TR_NAV_POWER,    MODE_NONE            },
};
/* 表长与 PK_NAV_ITEM_CNT 分开写迟早走偏：proto.c 里就出过一次——项数常量
 * 硬编码成 11，后来数组少了一项，items[10] 取到数组外的垃圾当 icon id 去算
 * 位图偏移，pk_aa_blit_4bpp 里 SIGBUS。钉在编译期。 */
_Static_assert(sizeof(ITEMS) / sizeof(ITEMS[0]) == PK_NAV_ITEM_CNT,
               "项表长度与 PK_NAV_ITEM_CNT 对不上");

/* ── 动作条与亮度 pop 的文案（全部走 i18n catalog）────────────────
 *
 * 三条都是**借用现成词条**，不在本页另立一份：
 *   ACT_LEVEL  → PK_TR_ACT_LEVEL，dock 的动作页签当年用的就是它，同词同动作；
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
static pk_nav_pop_t s_pop;

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

/*
 * ── 调平长按的两个时长 ────────────────────────────────────────────
 *
 * 长按阈值 1 s 的来由见下面触摸状态机那一节开头的大注释。定义提到这里，是
 * 因为**渲染也要用它**：进度填充按「已按住时长 / 阈值」算宽度，与判定共用
 * 同一个数、同一个 s_press_us，绝不另起一个时钟——两份真值迟早会走偏，而
 * 走偏的症状是"填充走满了却没生效"。
 *
 * 绿闪 200 ms：网格打开时这块屏约 6 FPS（docs/ui_performance-zh_CN.md），
 * 短于此连一帧都保不住；长于此就成了"卡了一下"而不是"闪了一下"。
 */
#define NAV_LEVEL_HOLD_MS   1000
#define NAV_LEVEL_FLASH_MS  200

/* ── 调平的两个视觉反馈态（spec §6 的 ③④）────────────────────────
 *
 * ③ 进度填充**没有**自己的状态量：它整个由上面那组按压态算出来（见
 *    level_hold_progress()）。滑出按钮 / 翻页 / 已结算把按压作废时，填充自动
 *    跟着消失，不需要谁记得去清它。
 * ④ 绿闪要在按压结束之后还活 200 ms，这才需要一个自己的时间戳。 */
static int64_t      s_flash_us;          /* 绿闪起点；0 = 不在闪 */
static bool         s_close_after_flash; /* 手已松、闪没完：闪完由 render 关 */

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
 * 电源 pop：点第 2 页的「电源」格弹出，横向两档 —— 关机 / 重启。
 *
 * 版面与亮度 pop 同构（同一套圆角双描边、同样的 PK_AA_L 文字），只多一行
 * 提示。两个键都不做"选中态"高亮：它们不是档位，是一次性动作，高亮其中
 * 一个会让人以为当前处于那个状态。
 *
 * 关机键用警示橙（COL_ACT）而不是主操作蓝：整板断电（含 RP2040）在这块板
 * 上不可撤销——/QON 悬空，醒过来只能靠插 USB（power_sy6970.h）。重启只影响
 * P4，用常规蓝。颜色本身就是这个区别的第一道提示，提示行是第二道。
 */
static void draw_power_pop(uint16_t *fb)
{
    const int lh = pk_aa_cell_h(PK_AA_L);

    pk_pfd_fill_round_rect(fb, PWR_X0 - PWR_PAD, PWR_PY0,
                           PWR_X0 + PWR_W + PWR_PAD, PWR_BOTTOM, 14, COL_LINE);
    pk_pfd_fill_round_rect(fb, PWR_X0 - PWR_PAD + 2, PWR_PY0 + 2,
                           PWR_X0 + PWR_W + PWR_PAD - 2, PWR_BOTTOM - 2,
                           12, COL_BG);

    /* 提示行：关机后怎么开回来 + 重启只重启主控。用 S 档塞得下一行。 */
    {
        const char *h  = pk_i18n_text(PK_TR_POWER_HINT);
        const int   hw = pk_aa_text_width(h, PK_AA_S);
        const int   hh = pk_aa_cell_h(PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   PWR_X0 + (PWR_W - hw) / 2,
                   PWR_PY0 + PWR_PAD + (PWR_HINT_H - hh) / 2,
                   h, COL_DIM, PK_AA_S);
    }

    static const uint16_t KEYS[2] = { PK_TR_POWER_OFF, PK_TR_POWER_RESTART };
    for (int i = 0; i < 2; ++i) {
        const int bx = PWR_X0 + i * (PWR_BW + PWR_PAD);
        pk_pfd_fill_round_rect(fb, bx, PWR_Y0, bx + PWR_BW, PWR_Y0 + PWR_BH,
                               10, i == 0 ? COL_ACT : COL_FAB);

        const char *t  = pk_i18n_text((pk_tr_id_t)KEYS[i]);
        const int   tw = pk_aa_text_width(t, PK_AA_L);
        /* 橙底上用近黑，蓝底上用白——各自对比度最高的那一档，同动作条
         * 绿闪那处的理由（见 render 里 COL_BG 那条注释）。 */
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   bx + (PWR_BW - tw) / 2, PWR_Y0 + (PWR_BH - lh) / 2,
                   t, i == 0 ? COL_BG : COL_WHITE, PK_AA_L);
    }
}

/*
 * 无操作自动收起——三条退路的第三条（另两条是动作条的「关闭」与第 0 页
 * 右滑）。飞行中忘记收起是常态，不能让菜单一直盖着 PFD。
 *
 * 初值 5000 沿用自 dock 的 DOCK_IDLE_MS，图的是不打破既有手感；当初刻意
 * **没有**去引用那个宏而是在这里另立一份，随后 dock 整段删掉（pk_ui_nav.c），
 * 引用过去就等于多一处返工。
 *
 * 2026-08-02 真机走查后调到 6000：网格的内容比 dock 多（两页十项，还要翻页
 * 才看得全），5 s 不够看完就被收走。这个值只能上机试出来，不要在桌面上推。
 */
#define NAV_IDLE_MS   6000

/* 判定放在 render 里：本层没有后台任务，而 render 恰好"网格活着时每帧调
 * 一次、收起后一次不调"，正是这个倒计时需要的心跳。判定时手指必然不在屏上
 * （touch/drag 每一帧都在刷新 s_last_act_us），所以这里关闭不会撞上下面那条
 * 「关闭一律等松手」的规矩。 */
static bool idle_expired(void)
{
    return (esp_timer_get_time() - s_last_act_us) >= (int64_t)NAV_IDLE_MS * 1000;
}

/*
 * 调平长按已经按了多少（0.0 ~ 1.0）；不在长按中返回 -1。
 *
 * 判据逐条对齐 drag() 里那段长按分支——"看得见的"与"点得中的"必须是同一件
 * 事：手指滑出按钮、这一下已被判成翻页、按压已经结算，那边一作废，这边的
 * 填充就必须同帧消失，否则屏上会留下一条走不完也退不掉的橙条。
 */
static float level_hold_progress(void)
{
    if (!s_press_valid || s_swiped || s_press_hit.kind != PK_NAV_HIT_LEVEL)
        return -1.0f;

    const int64_t held = esp_timer_get_time() - s_press_us;
    if (held <= 0) return 0.0f;
    const float p = (float)held / (float)((int64_t)NAV_LEVEL_HOLD_MS * 1000);
    return p > 1.0f ? 1.0f : p;
}

static bool flash_active(void)
{
    return s_flash_us != 0 &&
           esp_timer_get_time() - s_flash_us < (int64_t)NAV_LEVEL_FLASH_MS * 1000;
}

/* 绿闪态归零。与 press_reset() 分开：touch_up() 要先 press_reset() 再判绿闪
 * （闪没走完就得把关闭推后），混成一个函数会把刚点起来的那一闪当场抹掉。 */
static void flash_reset(void)
{
    s_flash_us          = 0;
    s_close_after_flash = false;
}

void pk_nav_grid_page_render(uint16_t *fb)
{
    /*
     * 绿闪结算：手已经松了、闪还没走完时，touch_up() 把"关网格"推到这里
     * （见那边的说明）。判定放在 render 与下面那条无操作自动收起是同一个
     * 理由——本层没有后台任务，render 就是唯一的心跳。
     *
     * 这**不**违反「关闭一律等松手」：那条规矩挡的是**手指还按着**就清
     * s_active（本次按压的剩余帧会顺着 touch_gt911.c 的分派落到底页去）。
     * 走到这儿手指必然已经离屏，剩下的 200 ms 里没有属于那次按压的帧。
     */
    if (s_close_after_flash && !flash_active()) {
        pk_nav_grid_page_close();
        return;
    }

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

        /*
         * 「调平」那一格的两个反馈态（spec §6 的 ③④）。
         *
         * 画在文字**之前**：填充是底、文字是面。次序反过来整块颜色会盖掉标签，
         * 而"填充走到一半把字吃了"恰恰是这条反馈最不该有的样子。
         *
         * 不做插值 / 缓动：网格打开时这块屏约 6 FPS（docs/ui_performance-zh_CN.md），
         * 1 s 只有 6 帧，每帧按当前时刻直接算一次宽度就足够看出"它在走"；为
         * 6 帧引一个定时器或动画框架，只会多一个要记得删的对象。
         */
        const int  lvl_w   = PK_DISPLAY_W / 3;   /* 与 pk_nav_hit_test 的 third 同源 */
        const int  lvl_top = PK_NAV_ACT_TOP + 1; /* 让开顶上那条 COL_LINE 分隔线 */
        const bool flash   = flash_active();
        if (flash) {
            pk_pfd_fill_rect(fb, 0, lvl_top, lvl_w, PK_DISPLAY_H, COL_OK);
        } else {
            const float p = level_hold_progress();
            /* 四舍五入而不是截断：6 帧里每一帧都该往前挪一截，截断会让第一帧
             * 停在 0 px（看上去像"没反应"）。 */
            if (p >= 0.0f)
                pk_pfd_fill_rect(fb, 0, lvl_top, (int)((float)lvl_w * p + 0.5f),
                                 PK_DISPLAY_H, COL_ACT_FILL);
        }

        for (int i = 0; i < 3; ++i) {
            /* 调平是唯一会改变飞机状态显示的动作，用警示橙与另两个分开。
             * 亮度被点开时它自己也高亮，否则 pop 弹出来会像凭空冒出的一块。 */
            uint16_t c = (i == 0) ? COL_ACT : COL_DIM;
            if (i == 1 && s_pop == PK_NAV_POP_BRIGHT) c = COL_ON;
            /* 绿底上再摆橙字读不出来（对比 1.6:1）。换成动作条底色那档近黑，
             * 对绿是 7:1——绿闪于是整体读成"这一格反白了"，比只换字色更强的
             * 完成信号，而且用的还是同一张调色板，不必再多定义一个前景色。 */
            if (i == 0 && flash) c = COL_BG;
            const int cx = PK_DISPLAY_W * (2 * i + 1) / 6;   /* 三等分的格心 */
            const int tw = pk_aa_text_width(acts[i], PK_AA_L);
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                       cx - tw / 2, PK_NAV_ACT_TOP + (PK_NAV_ACT_H - lh) / 2,
                       acts[i], c, PK_AA_L);
        }
    }

    if (s_pop != PK_NAV_POP_NONE) {
        /* pop 弹出时把网格再压一档：不压的话两层内容一样亮，看不出焦点在哪
         * 一层，而这时候唯一能点的只有 pop 里那三个档（pk_nav_hit_test 的
         * pop_open 分支已经把网格整层吞掉了）。动作条不压——「亮度」那一格
         * 正高亮着，压暗它等于把"是我弹出来的"这条线索抹掉。 */
        pk_pfd_darken_rect(fb, 0, PK_NAV_BAR_BOT, PK_DISPLAY_W,
                           PK_NAV_ACT_TOP, 120);
        if (s_pop == PK_NAV_POP_BRIGHT) draw_bright_pop(fb);
        else                            draw_power_pop(fb);
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
    s_pop = PK_NAV_POP_NONE;
    press_reset();
    flash_reset();
}

#ifdef PK_SIM_BUILD
static void sim_setup(void);
#endif

void pk_nav_grid_page_open(void)
{
    /* 每次都从第 1 页、pop 收起开始：菜单是个瞬时动作，上次翻到第 2 页不代表
     * 这次还想看第 2 页，而"打开就在熟悉的那一屏"比"记住上次"更省认知。 */
    s_page     = 0;
    s_pop = PK_NAV_POP_NONE;
    s_active   = true;
    press_reset();
    flash_reset();
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
    pk_ui_fab_sync();   /* s_active 已置真 → 必然算成"藏" */
#ifdef PK_SIM_BUILD
    sim_setup();
#endif
}

#ifdef PK_SIM_BUILD
/*
 * 截图钩子（同 search_page.c 的 sim_setup_once、settings_draw.c 的
 * pk_settings_sim_scroll 那条先例）：
 *
 *   PK_SIM_MENU=1         打开菜单，其余取默认（第 1 页、亮度 pop 收起）
 *   PK_SIM_MENU_PAGE=<n>  打开后翻到第 n 页（0 起，=1 就是第 2 页那一屏：
 *                         3 项 + 5 格空位，验"末页不居中"）
 *   PK_SIM_MENU_BRIGHT=1  打开后展开亮度快调 pop（网格再压一档 + 三档面板）
 *   PK_SIM_MENU_LEVEL=<pct>   停在「调平」长按进行中（spec §6 的 ③），pct 是
 *                             0~100 的进度百分比
 *   PK_SIM_MENU_LEVEL_DONE=1  停在长按满 1 s 之后的绿闪那一帧（同 ④）
 *
 * 摆的是本模块自己那几个状态量，**不导出 setter**：内部状态一旦对外可写，
 * 真机那侧就多了一条绕过触摸状态机的路。入口仍是正规的 open()，所以截出来
 * 的就是用户点 FAB 之后看到的那一屏，连"藏掉 FAB"这个副作用都一并带上。
 */
#include <stdlib.h>

static void sim_setup(void)
{
    const char *pg = getenv("PK_SIM_MENU_PAGE");
    if (pg != NULL) {
        int p = atoi(pg);
        if (p < 0) p = 0;
        if (p >= PK_NAV_PAGES) p = PK_NAV_PAGES - 1;
        s_page = p;
    }
    if (getenv("PK_SIM_MENU_BRIGHT") != NULL) s_pop = PK_NAV_POP_BRIGHT;
    if (getenv("PK_SIM_MENU_POWER")  != NULL) s_pop = PK_NAV_POP_POWER;

    /* 调平的 ③ 进度填充 / ④ 绿闪都只在按住的那 1 s 与随后的 200 ms 里出现，
     * 靠环境变量开个页面是截不到的，只能把状态直接摆到那一刻。 */
    const char *lv = getenv("PK_SIM_MENU_LEVEL");
    if (lv != NULL) {
        int pct = atoi(lv);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        /* 摆的与真机 touch() 记的是同一组量：命中「调平」、按压有效、按下
         * 时刻往前挪 pct% 个长按时长——render 里 level_hold_progress() 算出来
         * 就正好是 pct%，走的是与真机逐字相同的那条算式。 */
        s_press_hit.kind  = PK_NAV_HIT_LEVEL;
        s_press_hit.index = -1;
        s_press_x     = PK_DISPLAY_W / 6;                    /* 「调平」格心 */
        s_press_y     = PK_NAV_ACT_TOP + PK_NAV_ACT_H / 2;
        s_press_page  = s_page;
        s_press_valid = true;
        s_press_us    = esp_timer_get_time()
                      - (int64_t)NAV_LEVEL_HOLD_MS * 1000 * pct / 100;
    }
    if (getenv("PK_SIM_MENU_LEVEL_DONE") != NULL)
        s_flash_us = esp_timer_get_time();
}
#endif /* PK_SIM_BUILD */

bool pk_nav_grid_page_active(void) { return s_active; }

void pk_nav_grid_page_close(void)
{
    s_active   = false;
    s_pop = PK_NAV_POP_NONE;
    press_reset();
    flash_reset();
    /*
     * 2026-08-04：这里原来是无条件 set_fab_hidden(false)，而 activate_item
     * 点「搜索」时的次序是**先开搜索页（藏）、再关网格（放）**——净效果是
     * FAB 浮在搜索页上，正是每一层的注释都在防的那件事。改走统一判据之后，
     * 「这一层关了」与「此刻还有没有别人」被分开，两个动作的先后就不再要紧。
     * 见 apt_detail_page.h 的 pk_ui_fab_hidden_for。
     */
    pk_ui_fab_sync();
}

/* ── 触摸状态机 ──────────────────────────────────────────────────
 *
 * 「调平」必须长按 1 s 才生效：误触把地平线归零，飞行中是要命的。四个状态
 * 与 dock 那枚调平键（已随 dock 删除）逐条对齐，规矩不变：
 *
 *     按下       记下时刻（s_press_us）
 *     按住       进度填充跟着长（render 的 level_hold_progress()，不另起时钟）
 *     满 1 s     pk_ui_nav_on_level()，真正执行 + 起绿闪（s_flash_us）
 *     提前松手   pk_ui_nav_on_level_hint()（提示"需长按 1 秒"）
 *     滑出按钮   同上（等价于 LVGL 的 PRESS_LOST）
 *
 * 阈值 NAV_LEVEL_HOLD_MS 与绿闪时长 NAV_LEVEL_FLASH_MS 定义在上面的状态区，
 * 因为渲染那侧也要用（见那两个宏的注释）。
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
 *
 * 绿闪（④）与这条规矩的协调
 * --------------------------
 * 绿闪要占 200 ms，而"满 1 s"这一刻手指多半还按着——那 200 ms 本来就落在
 * "等松手"这段里，一帧都不用额外拖。只有"按满就立刻松手"这一种走法会撞上：
 * 关掉网格，那一格连同绿闪一起没了，④ 在最常见的操作下等于不存在。
 *
 * 处理是**把关闭再往后推一小段**，推到 render 里（s_close_after_flash）。
 * 这没有破坏上面那条规矩——它挡的是「手指还按着就清 s_active」，而这时手已
 * 经松了，剩下的 200 ms 里不存在属于那次按压的帧。唯一的缝是"松手后 200 ms
 * 内又按了一下"：那一下由 touch() 接住，把关闭改挂回**新那次**按压的松手上
 * （见 touch() 里的 pending_close），于是任何时刻都不会在手指按着时关网格。
 */

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
        /*
         * 搜索是模态层，不是 pk_ui_mode_t 的一站：只打开它，当前是哪一页不变
         * （见 search_page.h）。**底下那一页因此可以是任何一页**——这正是
         * 「点搜索结果要显式切到地图」那条修改的由来，见 search_page.c 的
         * goto_item()。
         */
        pk_search_page_open();
    } else if (ITEMS[index].label == PK_TR_NAV_POWER) {
        /* 电源同样不是 pk_ui_mode_t 的一站：弹出「关机 / 重启」二选一，
         * **网格不关**——关掉的话 pop 就没有底了，而且点错想退出时连
         * "点面板外收起"这条退路都没有。与搜索那条的区别：搜索是另开一个
         * 模态层，这里是本页面自己的弹层，所以直接改 s_pop 就够。 */
        s_pop = PK_NAV_POP_POWER;
        return;
    } else {
        return;   /* 记录 / 工具：页面还没写，enabled 已挡，走不到这儿 */
    }
    pk_nav_grid_page_close();
}

bool pk_nav_grid_page_touch(int x, int y)
{
    if (!s_active) return false;

    /* 上一次调平的绿闪还没结算完就又按下来了（松手后 200 ms 内的第二次
     * 点击）。先记下来，等下面把按压态摆好再处理——那一段会把 s_close_on_up
     * 清成 false。 */
    const bool pending_close = s_close_after_flash;

    s_press_hit    = pk_nav_hit_test(x, y, s_page, s_pop);
    s_press_x      = x;
    s_press_y      = y;
    s_press_page   = s_page;
    s_press_us     = esp_timer_get_time();
    s_last_act_us  = s_press_us;
    s_press_valid  = true;
    s_swiped       = false;
    s_swipe_locked = false;
    s_close_on_up  = false;

    /* 绿闪待结算时又来一次按压：把"闪完就关"改挂到**这一次**的松手上。
     *
     * 不能在这儿直接关：手指正按着，清掉 s_active 之后本次按压的剩余帧会顺着
     * touch_gt911.c 的分派落到底页去，正是本节开头那条规矩要挡的事。
     *
     * 顺带把这一下的其它归宿全挡掉（s_close_on_up 一置位，touch_up 就只关
     * 网格、不走那条 switch）——调平刚做完、网格正要关，这一下多半是手抖或
     * 想再确认一次，不该因此跳进某一格。 */
    if (pending_close) {
        flash_reset();
        s_close_on_up = true;
    }

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
    if (!s_swipe_locked && s_pop == PK_NAV_POP_NONE) {
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
        if (pk_nav_hit_test(x, y, s_press_page, s_pop).kind
                != PK_NAV_HIT_LEVEL) {
            /* 滑出按钮 = 放弃，等同 LVGL 的 PRESS_LOST。 */
            s_press_valid = false;
            pk_ui_nav_on_level_hint();
        } else if (esp_timer_get_time() - s_press_us
                       >= (int64_t)NAV_LEVEL_HOLD_MS * 1000) {
            /* 满 1 s 当场执行（提示随即弹出），网格留到松手再关——理由见
             * 本节开头。绿闪从这一刻起算：s_press_valid 一清，进度填充就停在
             * 满格并被绿底接管，视觉上是"充满 → 变绿"一条连贯的线。 */
            s_press_valid = false;      /* 已消费，松手不再重复结算 */
            s_close_on_up = true;
            s_flash_us    = esp_timer_get_time();
            pk_ui_nav_on_level();
        }
    }
    return true;
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

    if (close_on_up) {
        /* 绿闪还没走完就先别关：关本身是安全的（手已离屏），但网格一没，那
         * 一格的绿闪也就跟着没了——「按满 1 s 立刻松手」是最常见的走法，④ 在
         * 它下面会等于不存在。把关闭推给 render 收尾，与本节开头那条规矩怎么
         * 协调见那段。 */
        if (flash_active()) { s_close_after_flash = true; return; }
        pk_nav_grid_page_close();
        return;
    }
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
            /* 这条路同样要绿闪，理由与 drag() 那条一样：不能因为触摸轮询稀疏
             * 就少给一次完成反馈。手已经松了，直接挂到 render 上收尾。 */
            s_flash_us          = esp_timer_get_time();
            s_close_after_flash = true;
        } else {
            pk_ui_nav_on_level_hint();
        }
        break;

    case PK_NAV_HIT_BRIGHT:
        s_pop = PK_NAV_POP_BRIGHT;
        break;

    case PK_NAV_HIT_CLOSE:
        /* 三条退路之一。 */
        pk_nav_grid_page_close();
        break;

    case PK_NAV_HIT_BRIGHT_STEP:
        /* index 与 display.h 的 PK_BL_STEP_LOW/MID/HIGH 同序（见 nav_grid_page.h
         * 那条枚举的注释）。档位真值只有 pk_backlight_* 一处，不与设置页分家。 */
        pk_backlight_step_set((uint8_t)hit.index);
        s_pop = PK_NAV_POP_NONE;
        break;

    case PK_NAV_HIT_POWER_OFF: {
        /*
         * 整板断电（含 RP2040）。**VBUS 在位时先拦下来**：BATFET 只是
         * 电池↔SYS 的开关，插着 USB 时 SYS 由 VBUS 供电，写 BATFET_DIS
         * 根本关不掉机（[DS] p.30）。
         *
         * 初版这里不拦、只让驱动层打一条串口 WARN，理由写的是"拔不拔 USB
         * 是用户的事"。2026-09-12 真机打脸：罩哥点关机"关不掉"，屏上零反馈
         * ——串口 WARN 用户根本看不见，而弹层提示说的是"关机**后**需插 USB
         * 唤醒"，没有一个字告诉他"关机**前**得先拔"。
         * 按不动还能解释成"设备坏了"，这里必须给出可见的原因。
         * 菜单**不关**：让提示和那两个键留在同一屏，拔完线直接再点。
         */
        const power_snapshot_t ps = power_service_snapshot();
        if (ps.vbus_present) {
            pk_ui_toast_show(PK_TR_POWER_NEED_UNPLUG, true);
            break;
        }
        s_pop = PK_NAV_POP_NONE;
        pk_nav_grid_page_close();
        pk_display_panel_off();
        power_sy6970_shutdown();
        break;
    }

    case PK_NAV_HIT_POWER_RESTART:
        /*
         * 只重启 P4。RP2040 挂 3V3_DIG，软复位不会让它掉电——要复位它
         * 只能走上面的关机（nav_grid_page.h 的 HIT 枚举注释）。
         *
         * 先 pk_display_panel_off() 再重启：P4 一复位，MIPI-DSI 控制器
         * 随之停掉，而 ST7701 面板还在扫描——**面板对"没有信号"的默认
         * 表现是蓝屏**，2026-09-12 真机上罩哥看到的就是它。那不是固件
         * 画出来的（本项目清屏一律 memset 0 = 黑），是面板自己的无信号态。
         * 显式发一条 display-off 命令，让它在失去信号前先黑掉。
         */
        s_pop = PK_NAV_POP_NONE;
        pk_nav_grid_page_close();
        pk_display_panel_off();
        esp_restart();
        break;

    case PK_NAV_HIT_NONE:
    default:
        /* pop 开着时命中判定只测该弹层自己的按钮，点别处一律 NONE = 收起 pop
         * （不关网格）。pop 没开时点空处什么都不做。 */
        s_pop = PK_NAV_POP_NONE;
        break;
    }
}

#endif /* !PK_NAV_GRID_HOST_TEST */
