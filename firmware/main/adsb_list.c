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

#include "aircraft_db.h"
#include "aircraft_state.h"
#include "airline_codes.h"
#include "baro.h"
#include "display.h"
#include "i18n.h"
#include "icao_country.h"
#include "imu_task.h"
#include "mag_var.h"
#include "own_ship.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_icon_font.h"
#include "pfd_layout.h"
#include "pfd_statusbar.h"   /* pk_ui_topbar_right_limit —— 给 DEMO 徽标让位 */
#include "traffic_geom.h"
#include "ui_state.h"

/* ── 版面 ───────────────────────────────────────────────────── */

#define LST_TOP       PFD_BAR_BOT              /* 顶栏下沿 */
#define HDR_H         30                       /* 列标题行 */
#define ROW0_Y        (LST_TOP + HDR_H)
#define ROW_H         48
#define ROW_N         8
#define LIST_BOT      (ROW0_Y + ROW_N * ROW_H) /* 462 */

/* 抽屉：贴底，高 230（spec §5.3）。开着时数据区只画得下 DRAWER_ROWS 行。 */
#define DRAWER_H      230
#define DRAWER_TOP    (PK_DISPLAY_H - DRAWER_H)      /* 250 */
#define DRAWER_ROWS   ((DRAWER_TOP - ROW0_Y) / ROW_H)

/* 整页左边距走 pfd_layout.h 的共用 token（值仍是 16）。反过来说：本页是把
 * PK_UI_PAD_L 钉在 16 的那个页面——BRG 列最宽 71 px，从 16 起画到 87，右边的
 * 分隔线 SEP_CALL 钉在 95，只剩 8 px 余量。改 PK_UI_PAD_L 之前先看这里。 */
#define PAD_L         PK_UI_PAD_L

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
 *   DIST "12.3"=60        ALT "34322"=75      V/S 箭头22+4位60=82
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
/* 2026-07-31：DIST/ALT 这两组基准与它们左侧的分隔线各左移 3 px。
 *
 * 起因不在本页：CJK cell 宽从 0.81 em 放到整 em 修「中文比英文偏小」，
 * PK_AA_M_CJK_W 18 → 22，而 V/S 列的宽度 W_VS 拿它当 ↑ 箭头宽——于是
 * 536 - 82 - 450 = 4，跌破 SEP_GAP=7，_Static_assert 直接编译失败。
 *
 * 把 4 px 的欠账往左摊：DIST 左侧原有 60 px 富余（徽章列到 DIST 之间），
 * 挪 3 px 之后仍剩 57 px，四列间距全部回到 8 px 以上、V/S 左缘正好 7 px。
 * 只动这四个数是因为右侧 GS/TRK/AGE 一路钉在 CONTENT_R 上，动它们要连
 * 内容区右缘一起改。 */
#define COL_DIST_R    348                            /* 右对齐基准 */
#define COL_ALT_R     439
#define COL_VS_R      536
#define COL_GS_R      603
#define COL_TRK_R     670
#define COL_SEEN_R    CONTENT_R                      /* 716 */

/*
 * 分隔线 x（同时是右侧那一列的命中区起点）。COLS[] 直接引用这些宏，不再
 * 另抄一份数字——右对齐基准和线位分成两处各写各的，正是本文件出过的错：
 * 改列位时只改了其中一份，数据右对齐到旧位置、线画在新位置，字被穿过去，
 * 而缩略图上完全看不出来。
 */
#define SEP_CALL      95
#define SEP_FLAG      231
#define SEP_DIST      231      /* 徽章与 DIST 共用一个命中区，故与上面同值 */
#define SEP_ALT       356      /* 与 COL_DIST_R 一同左移 3，见上方说明 */
#define SEP_VS        447
#define SEP_GS        544
#define SEP_TRK       611
#define SEP_SEEN      678

/* 各列最长内容宽度。改任何列位之前，先对着这张表把账算平。 */
#define W_BRG   (26 + 3 * PK_AA_M_W)              /* 箭头 + 三位方位 */
#define W_CALL  (8 * PK_AA_M_W)                   /* 满宽呼号 */
#define W_FLAG  (3 * PK_AA_XS_W)                  /* EMG / RDO / HJK */
#define W_DIST  (4 * PK_AA_M_W)                   /* "12.3" */
#define W_ALT   (5 * PK_AA_M_W)                   /* "34322" */
#define W_VS    (PK_AA_M_CJK_W + 4 * PK_AA_M_W)   /* "↑1500" */
#define W_GS    (3 * PK_AA_M_W)
#define W_TRK   (3 * PK_AA_M_W)
#define W_SEEN  (3 * PK_AA_XS_W)                  /* "47s" */

/* 内容与分隔线之间的最小留白。低于这个数，字看起来就是被线框住的。 */
#define SEP_GAP  7

/* 每列：左缘离左线 >= SEP_GAP，右缘离右线 >= SEP_GAP。
 * 任何一处改动破坏了这个关系，这里直接编译失败——比在真机上用肉眼找字被
 * 线穿过要快得多，也可靠得多。 */
_Static_assert(SEP_CALL - (COL_BRG_X + W_BRG)   >= SEP_GAP, "BRG 右缘贴线");
_Static_assert(COL_CALL_X - SEP_CALL            >= SEP_GAP, "CALLSIGN 左缘贴线");
_Static_assert(SEP_FLAG - (COL_CALL_X + W_CALL) >= SEP_GAP, "CALLSIGN 右缘贴线");
_Static_assert(COL_FLAG_X - SEP_FLAG            >= SEP_GAP, "徽章左缘贴线");
_Static_assert(COL_DIST_R - W_DIST - SEP_DIST   >= SEP_GAP, "DIST 左缘贴线");
_Static_assert(SEP_ALT  - COL_DIST_R            >= SEP_GAP, "DIST 右缘贴线");
_Static_assert(COL_ALT_R - W_ALT - SEP_ALT      >= SEP_GAP, "ALT 左缘贴线");
_Static_assert(SEP_VS   - COL_ALT_R             >= SEP_GAP, "ALT 右缘贴线");
_Static_assert(COL_VS_R - W_VS - SEP_VS         >= SEP_GAP, "V/S 左缘贴线");
_Static_assert(SEP_GS   - COL_VS_R              >= SEP_GAP, "V/S 右缘贴线");
_Static_assert(COL_GS_R - W_GS - SEP_GS         >= SEP_GAP, "GS 左缘贴线");
_Static_assert(SEP_TRK  - COL_GS_R              >= SEP_GAP, "GS 右缘贴线");
_Static_assert(COL_TRK_R - W_TRK - SEP_TRK      >= SEP_GAP, "TRK 左缘贴线");
_Static_assert(SEP_SEEN - COL_TRK_R             >= SEP_GAP, "TRK 右缘贴线");
_Static_assert(COL_SEEN_R - W_SEEN - SEP_SEEN   >= SEP_GAP, "AGE 左缘贴线");
_Static_assert(CONTENT_R + 8 - COL_SEEN_R       >= 0,       "AGE 超出内容区");

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

/* title 存的是**词条 id** 而不是字符串：语言可以在运行时切，这张表是 static
 * const，存字符串就意味着切语言后列头还停在旧语言上。 */
typedef struct {
    pk_tr_id_t  title;
    int         x;            /* right_align ? 右缘 : 左缘 */
    bool        right_align;
    int         hit_x0, hit_x1;
} col_def_t;

/* 当前语言下这一列的标题。列头与顶栏那行「SORT xxx」都从它取，两处不各查
 * 一次表——那正是本文件出过的错的模式（同一个数分两处各写各的）。 */
static const char *col_title(sort_key_t k);

/* 命中区把列间空隙一并吃掉：手指落在两列标题中间时总得归给某一列，
 * 留缝隙只会让人以为「点了没反应」。 */
static const col_def_t COLS[SORT_COUNT] = {
    [SORT_BRG]  = { PK_TR_LIST_COL_BRG,  COL_BRG_X,  false,  PAD_L - 8, SEP_CALL },
    [SORT_CALL] = { PK_TR_LIST_COL_CALL, COL_CALL_X, false,  SEP_CALL,  SEP_DIST },
    /* FLAG 不排序（徽章有无不构成一种顺序），命中区并进 DIST——中间夹一个
     * 点了没反应的列，比少一个可点的列更让人困惑。 */
    [SORT_DIST] = { PK_TR_LIST_COL_DIST, COL_DIST_R, true,   SEP_DIST,  SEP_ALT  },
    [SORT_ALT]  = { PK_TR_LIST_COL_ALT,  COL_ALT_R,  true,   SEP_ALT,   SEP_VS   },
    [SORT_VS]   = { PK_TR_LIST_COL_VS,   COL_VS_R,   true,   SEP_VS,    SEP_GS   },
    [SORT_GS]   = { PK_TR_LIST_COL_GS,   COL_GS_R,   true,   SEP_GS,    SEP_TRK  },
    [SORT_TRK]  = { PK_TR_LIST_COL_TRK,  COL_TRK_R,  true,   SEP_TRK,   SEP_SEEN },
    /* 标题写 AGE 不写 SEEN：SEEN 是 4 字符 40 px，比它管的那列数据
     * （"47s" = 30 px）还宽，右对齐后会顶到左边那条线上。中文侧同理取两字
     * 的「更新」（XS 档 24 px），仍窄于该列的 30 px。 */
    [SORT_SEEN] = { PK_TR_LIST_COL_AGE,  COL_SEEN_R, true,   SEP_SEEN,  CONTENT_R + 8 },
};

static const char *col_title(sort_key_t k)
{
    return pk_i18n_text(COLS[(k >= 0 && k < SORT_COUNT) ? k : SORT_DIST].title);
}

/* 排序状态。默认距离升序——最近的威胁排最前，这是打开这一页最常见的意图。
 *
 * 不落 NVS：排序是「我现在想这么看」的临时视角，不是设置。下次开机回到
 * 距离升序才是对的默认，把上次点的 TRK 降序记住反而会让人困惑。 */
static sort_key_t s_sort   = SORT_DIST;
static bool       s_sort_desc;

/* 顶栏右上角：排序说明 + RESET 按钮的位置。RESET 贴 CONTENT_R 右缘，
 * 说明文字右对齐排在它左边（起点由实测文本宽度算，不写死——中英文长度
 * 差得多，写死的那个数只对其中一种语言成立）。 */
/* 右界不是常量：演示模式下顶栏右侧多一枚常驻 DEMO 徽标（画在控件层、压在本页
 * 之上），RESET 与它左边那行排序说明必须整体左移，否则被盖住的正好是「现在按
 * 什么排的」这句话。做成函数而不是 #define 是因为它每帧都可能变——用户在设置页
 * 一开演示模式，回到本页就得是新的位置。 */
#define RESET_X1      pk_ui_topbar_right_limit(CONTENT_R + 8)
/* 按钮宽度按 header 统一字号（M）下的最宽文案算：英文 "RESET" 5×PK_AA_M_W
 * = 75，中文「重置」2×PK_AA_M_CJK_W = 44，取大者 + 左右各 8 px 内边距 = 91，
 * 进位到 92。原来是 66，那是 XS 档（"RESET" 只有 50 px）的尺寸——顶栏统一到
 * M 之后 66 装不下英文，靠缩字号塞进去正是这次要消除的做法，所以放宽盒子。
 * 命中区与按下高亮都从这两个数推出来，改这里三处一起跟着走。 */
#define RESET_W       92
#define RESET_X0      (RESET_X1 - RESET_W)
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
static int s_sim_drawer_row = -1;
static void sim_sort_override(void)
{
    const char *k = getenv("PK_SIM_SORT");
    if (k) { int v = atoi(k); if (v >= 0 && v < SORT_COUNT) s_sort = (sort_key_t)v; }
    const char *d = getenv("PK_SIM_SORT_DESC");
    if (d) s_sort_desc = (d[0] == '1');
}
#endif
static int        s_hdr_down = -1;   /* 按下高亮的列，-1 = 无 */

/* 详情抽屉。点行打开、再点同一行关闭（spec §5.3 还写了"下滑关闭"，那要等
 * 手势层做出来；在此之前必须有一条能靠单击退出的路，否则抽屉一开就关不掉）。
 *
 * 记的是 ICAO 而不是行号：排序一变、目标一进一出，行号指向的就是另一架飞机
 * 了，而抽屉里那些字段（机型/航司/国籍）全是按 ICAO 查的，对不上会非常
 * 误导人。 */
static uint32_t   s_drawer_icao;

/*
 * 本屏每一行画的是哪架飞机。
 *
 * 触摸回调（touch_gt911.c 的 read_cb）拿不到排序后的结果——那是 render 的
 * 局部量，而且排序键、滚动位置、目标进出都会让行号指向不同的飞机。所以
 * render 每帧把屏上这几行的 ICAO 留在这里，触摸时按行号查。
 *
 * 0 表示该行没画（列表不足一屏）。 */
static uint32_t   s_screen_icao[ROW_N];

/*
 * 触摸手势状态。
 *
 * 一次触摸要么是**点击**（开抽屉 / 切排序），要么是**拖动**（滚列表），
 * 按下的那一刻分不出来，所以按下只记起点，真正的动作留到松手时按「有没有
 * 移动过」来分派。分不清这两者的后果很具体：手指滑一下列表，松手时又把
 * 抽屉给开了。
 *
 * s_first_row 是 render 算出的当前顶行，拖动以它为基准换算；
 * s_scroll_first < 0 表示"没手动滚过"，此时顶行由选中行/抽屉自动锚定。
 */
static int  s_press_x, s_press_y;
static int  s_press_first;
static bool s_press_valid;      /* 按下点落在列表可消费的区域内 */
static bool s_moved;            /* 本次触摸已判定为拖动 */
static int  s_first_row;        /* render 写：本帧的顶行 */
static int  s_scroll_first = -1;/* 手动滚动到的顶行，-1 = 自动锚定 */

/* 超过这个位移就算拖动，不再当点击。48 px 是一行高，取它的 1/4——比手指
 * 在点击时的自然抖动大，又远小于一次有意的滑动。 */
#define DRAG_SLOP  12

/*
 * 表头的命中范围：视觉是 HDR_H=30 px，命中上下各外扩 6 px → 42 px。
 *
 * 30 px 只有 3.6 mm，远低于 9 mm 的手指目标（屏 8.4 px/mm）。这一档**没有**
 * 做到 9 mm，是权衡后的结果：每列横向有 60~140 px 宽，按 Fitts 定律宽度能
 * 补偿高度；而把表头抬到 76 px 要吃掉一整行数据（8 行 → 7 行），一行飞机比
 * 一次排序切换值钱。折中到 42 px ≈ 5 mm。
 *
 * 写在这里是为了下次有人照着"9 mm"那条规则来改它时，先看到这个取舍。
 */
#define HDR_HIT_TOP  (LST_TOP - 6)
#define HDR_HIT_BOT  (ROW0_Y + 6)

static uint32_t pk_adsb_row_icao(int row)
{
    if (row < 0 || row >= ROW_N) return 0;
    return s_screen_icao[row];
}

/*
 * 右对齐：先量宽再倒推起点。
 *
 * 宽度一律走 pk_aa_text_width，不用 strlen × cell_w。数据列确实全是 ASCII，
 * 但**列头也走这个函数**，而中文列头（「呼号」「距离」「更新」）一个字形三
 * 个字节、又比拉丁宽——按 strlen 算会把两字的标题当成六字符量，右对齐后整
 * 个标题被推到左边那条分隔线外。
 *

 * 这里曾经出过一次很难看出来的错：右对齐基准（COL_*_R 宏）和分隔线位置
 * （COLS[].hit_x0）是两份独立的数，我改列位时只改了其中一份，于是数据一律
 * 右对齐到旧位置、线画在新位置，字被线穿过去。缩略图上看不出来，罩哥放大
 * 才发现。
 *
 * 结论不是「下次仔细点」——是这两个数本来就不该分开写。见文件末尾的
 * layout_assert()：它在每帧开头核对一遍，对不上直接把红条画到屏幕上。
 */
static void puts_right(uint16_t *fb, int right_x, int y, const char *s,
                       uint16_t col, pk_aa_size_t sz)
{
    const int w = pk_aa_text_width(s, sz);
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

/* 标题的颜色不再由调用方传进来：它归全局层级管（PK_UI_TITLE_*），
 * 留一个参数只会让人以为这一页可以自己挑颜色。 */
static void draw_header(uint16_t *fb, int n, uint16_t col_dim)
{
    const int ty = PK_UI_TITLE_Y;
    LST_PUTS(fb, PAD_L, ty, pk_i18n_text(PK_TR_LIST_TITLE),
             PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);

    /* 目标数用 PFD 状态栏那枚 connecting_airports——同一台设备上「ADS-B 目标
     * 数」只该有一个符号。绿=有效在线，与状态栏、交通页一致。 */
    const uint16_t COL_ADSB = pk_rgb565(0, 220, 60);
    const uint8_t *ic = pk_icon_bitmap
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
    /* 缓冲按中文算：列名可能是 2 个汉字（6 字节），箭头再占 3 字节。 */
    char sbuf[40];
    snprintf(sbuf, sizeof(sbuf), "%s %s%s", pk_i18n_text(PK_TR_LIST_SORT),
             col_title(s_sort), s_sort_desc ? "↓" : "↑");
    /* 右对齐到 RESET 左边：这行的长度随排序列变（"SORT CALLSIGN↑" 比
     * "SORT DIST↑" 长 55 px），左对齐画就会在非默认排序时顶到 RESET 上。
     * 宽度必须问渲染器要——中文列名与箭头都不是一字节一格。 */
    /* 字号跟标题/计数同档（PK_UI_TITLE_SIZE）。原来排序说明是 S、RESET 是 XS，
     * 同一条 header 里三种字号——产品决策：header 内不分主次，视觉一致优先，
     * 主次交给颜色（标题白 / 排序暗灰 / RESET 琥珀）去表达。 */
    const int sw = pk_aa_text_width(sbuf, PK_UI_TITLE_SIZE);
    LST_PUTS(fb, RESET_X0 - 10 - sw, PK_UI_TITLE_Y, sbuf,
             col_dim, PK_UI_TITLE_SIZE);

    if (!sort_is_default()) {
        const uint16_t COL_RST = pk_rgb565(255, 210, 60);
        const char *rst = pk_i18n_text(PK_TR_LIST_RESET);
        /* 在按钮盒里水平居中：中英文宽度差 31 px，固定左内边距会让中文明显偏左。 */
        const int rw = pk_aa_text_width(rst, PK_UI_TITLE_SIZE);
        if (s_hdr_down == HDR_HIT_RESET)
            pk_pfd_darken_rect(fb, RESET_X0, 4, RESET_X1, PFD_BAR_BOT - 4, 60);
        LST_PUTS(fb, RESET_X0 + (RESET_W - rw) / 2, PK_UI_TITLE_Y, rst,
                 COL_RST, PK_UI_TITLE_SIZE);
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

        const char *t = col_title((sort_key_t)k);
        if (c->right_align) {
            puts_right(fb, c->x - arw, ty, t, cc, PK_AA_XS);
            if (ar) LST_PUTS(fb, c->x - arw, ty, ar, cc, PK_AA_XS);
        } else {
            /* 中文列头是多字节，strlen × cell_w 会算出好几倍的宽度，
             * 箭头就会飞到隔壁列去。宽度只能问渲染器要。 */
            const int w = pk_aa_text_width(t, PK_AA_XS);
            LST_PUTS(fb, c->x, ty, t, cc, PK_AA_XS);
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
    pk_pfd_fill_rect(fb, SEP_FLAG - 1, y0, SEP_FLAG, y1, COL_SEP);
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

/*
 * 详情抽屉：把主表放不下的字段补齐（spec §5.3 点名的机型 / SQK / ICAO /
 * 航司 / 国籍）。
 *
 * 为什么这些字段进抽屉而不是主表：它们是**身份**信息，不随时间变，看一次
 * 就够；主表那八列全是**状态**，每秒都在变，得能一眼扫完一屏。把机型塞进
 * 主表只会挤掉一列真正需要盯着的数。
 *
 * 排成两列键值：左列身份（呼号/ICAO/注册号/机型），右列归属与状态
 * （航司/国籍/应答机/上次报文）。值缺就写 ---，不留空白——空白让人以为
 * 这一格没画出来，而这里"查不到"本身就是有用的信息（多半是军机或未收录）。
 */
/*
 * 抽屉里的一个值：先逐级降字号（M→S→XS），仍放不下才截断并加省略号。
 *
 * 只降一档是不够的——"China Southern Airlines" 23 个字符，S 档仍要 253 px，
 * 而右列只有 200 px，结果溢出去被 FAB 盖掉了尾巴。截断放在最后一步：机型和
 * 航司截短了还能认，字号小一点则是无损的，所以先榨字号。
 *
 * 不同字号的字高不同，按各自 cell 高在行内居中，否则几行值会一高一低。
 */
static void put_val(uint16_t *fb, int x, int y_top, int avail,
                    const char *val, uint16_t col)
{
    /* 宽度一律问渲染器：抽屉里的值有中文（「秒前」），也有航司/国家名这类
     * 纯 ASCII 的库数据。strlen 会把一个汉字数成 3 格，于是中文界面下每一行
     * 都被误判为放不下而降到 XS，甚至被截成「秒..」。 */
    pk_aa_size_t sz = PK_AA_M;
    if (pk_aa_text_width(val, PK_AA_M) > avail)
        sz = (pk_aa_text_width(val, PK_AA_S) <= avail) ? PK_AA_S : PK_AA_XS;

    const int y = y_top + (PK_AA_M_H - pk_aa_cell_h(sz)) / 2;

    if (pk_aa_text_width(val, sz) <= avail) {
        LST_PUTS(fb, x, y, val, col, sz);
        return;
    }

    /* 还是放不下：截断 + ".."。按**码点**走而不是按字节切——UTF-8 从中间
     * 切开会留下半个汉字，渲染成一个空白格（CJK 字库是 catalog 子集，
     * 无效码点不落任何字形），看上去像丢了字而不像被截断。 */
    const int dots = pk_aa_text_width("..", sz);
    char tmp[64];
    size_t out = 0;
    int w = 0;
    /* 余量按最坏情况留：一个码点最多 4 字节，后面还要放 ".." 和结尾符。 */
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
    if (out == 0) { LST_PUTS(fb, x, y, "..", col, sz); return; }
    tmp[out++] = '.';
    tmp[out++] = '.';
    tmp[out]   = '\0';
    LST_PUTS(fb, x, y, tmp, col, sz);
}

static void draw_drawer(uint16_t *fb, const row_t *r,
                        uint16_t col_key, uint16_t col_val, uint16_t col_dim)
{
    const aircraft_t *a = r->ac;

    pk_pfd_fill_rect(fb, 0, DRAWER_TOP, PK_DISPLAY_W, PK_DISPLAY_H,
                     pk_rgb565(14, 20, 30));
    /* 顶边一条亮线，把抽屉和它盖住的表格分开——没有这条线，抽屉看起来像是
     * 表格的又一行，而它其实是浮在上面的。 */
    pk_pfd_fill_rect(fb, 0, DRAWER_TOP, PK_DISPLAY_W, DRAWER_TOP + 2,
                     pk_rgb565(70, 130, 190));

    char cs[AIRCRAFT_CALLSIGN_LEN];
    callsign_of(a, cs, sizeof(cs));

    /* 标题行：呼号用 L 档——抽屉一次只讲一架飞机，它是这块区域的主语。 */
    LST_PUTS(fb, PAD_L + 8, DRAWER_TOP + 14, cs, col_val, PK_AA_L);

    const char *tag = emergency_tag(a);
    if (tag) {
        const int tx = PAD_L + 8 + pk_aa_text_width(cs, PK_AA_L) + 12;
        const int tw = pk_aa_text_width(tag, PK_AA_S);
        pk_pfd_fill_rect(fb, tx - 5, DRAWER_TOP + 16, tx + tw + 5,
                         DRAWER_TOP + 16 + PK_AA_S_H + 6, pk_rgb565(220, 40, 40));
        LST_PUTS(fb, tx, DRAWER_TOP + 19, tag, pk_rgb565(255, 255, 255), PK_AA_S);
    }

    /* 两列键值。键用 XS 暗色、值用 M——扫的时候眼睛只需要抓值。
     *
     * 右列起点从 420 挪到 404：两列的值不是一回事，左列最长是机型全称
     * （"Boeing 737-8 MAX" 一类，本就短），右列是航司全称
     * （"China Southern Airlines"），后者一路要靠截断才塞得进去。把 16 px
     * 从宽裕的一侧还给紧张的一侧，两边同时变好。 */
    const int col_x[2]  = { PAD_L + 8, 404 };
    const int y0        = DRAWER_TOP + 62;
    const int line_h    = 34;

    /* 键存词条 id 而不是字符串：语言能在运行时切，static 的字符串表会停在
     * 旧语言上（列头 COLS[] 已经栽过这一次）。 */
    static const pk_tr_id_t keys[2][4] = {
        { PK_TR_LIST_D_ICAO,    PK_TR_LIST_D_REG,     PK_TR_LIST_D_TYPE,
          PK_TR_LIST_D_MODEL },
        { PK_TR_LIST_D_AIRLINE, PK_TR_LIST_D_COUNTRY, PK_TR_LIST_D_SQUAWK,
          PK_TR_LIST_D_LAST_SEEN },
    };
    /*
     * 键列宽按**本列最长的那条键**实测算，不写死。
     *
     * 原来两列共用一个 96：英文 "LAST SEEN" 在 XS 档实测 90 px，键与值之间只
     * 剩 6 px，低于本文件自己给列间距定的 SEP_GAP=7；而中文「上次报文」只有
     * 48 px，右边白白空出一大片。一个常量同时伺候两种语言、两列不同的词条，
     * 必然在某一头翻车——加宽到能装下 "LAST SEEN" 又会连累左列（那边最长的
     * "MODEL" 只有 50 px，凭什么也让出 96 px 的值宽）。
     *
     * 按列各算各的之后，左列省下来的宽度直接还给值，机型/型号那种长串少截
     * 一截；右列则拿到刚好够用的键宽，间距不再低于标准。
     *
     * 宽度一律问 pk_aa_text_width：中文一个字三字节，strlen 会把「上次报文」
     * 数成 12 格。
     */
    /* 键与值的最小间隙。取 10 而不是贴着 SEP_GAP=7：列与列之间隔的是一条
     * 分隔线，键与值之间什么都没有，靠留白分组就得多给一点。 */
    #define KEY_GAP  10
    _Static_assert(KEY_GAP >= SEP_GAP, "键值间距不得低于本页列间距标准");
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

    snprintf(v[0], sizeof(v[0]), "%06lX", (unsigned long)a->icao24);
    const char *reg = pk_aircraft_registration(a->icao24);
    snprintf(v[1], sizeof(v[1]), "%s", (reg && reg[0]) ? reg : "---");
    const char *tc = pk_aircraft_type_code(a->icao24);
    snprintf(v[2], sizeof(v[2]), "%s", (tc && tc[0]) ? tc : "---");
    const char *tm = pk_aircraft_type_model(a->icao24);
    snprintf(v[3], sizeof(v[3]), "%s", (tm && tm[0]) ? tm : "---");

    /* 航司按呼号前三字母查。查不到不是错误：尾号呼号（N12345）、军用呼号
     * 本来就不带航司前缀。 */
    const char *fnum = NULL;
    const pk_airline_t *al = a->have_callsign
                           ? pk_airline_from_callsign(a->callsign, &fnum) : NULL;
    snprintf(v[4], sizeof(v[4]), "%s", al ? al->name : "---");

    const pk_country_t *cn = pk_country_from_icao24(a->icao24);
    snprintf(v[5], sizeof(v[5]), "%s", cn ? cn->name : "---");

    if (a->have_squawk) snprintf(v[6], sizeof(v[6]), "%04d", a->squawk);
    else                snprintf(v[6], sizeof(v[6]), "---");

    snprintf(v[7], sizeof(v[7]), "%d %s", r->age_s,
             pk_i18n_text(PK_TR_LIST_D_AGO));

    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 4; ++i) {
            const int y = y0 + i * line_h;
            LST_PUTS(fb, col_x[c], y + (PK_AA_M_H - PK_AA_XS_H) / 2,
                     pk_i18n_text(keys[c][i]), col_key, PK_AA_XS);
            const char *val = v[c * 4 + i];
            const int avail = (c == 0 ? col_x[1] - 16 : CONTENT_R)
                            - col_x[c] - key_w[c];
            put_val(fb, col_x[c] + key_w[c], y, avail, val, col_val);
            (void)col_dim;
        }
    #undef KEY_GAP
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
    /* PK_SIM_DRAWER=<行号> 打开该行的抽屉，用来截图。 */
    { const char *e = getenv("PK_SIM_DRAWER");
      if (e) s_sim_drawer_row = atoi(e); }
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

    draw_header(fb, nr, COL_DIM);
    draw_col_titles(fb, COL_TITL, COL_HDR);
    pk_pfd_fill_rect(fb, 0, ROW0_Y - 1, PK_DISPLAY_W, ROW0_Y, COL_LINE);

    if (nr == 0) {
        /* 空列表也是一种状态，不能留白屏——留白让人以为页面没画出来。 */
        const char *msg = pk_i18n_text(PK_TR_LIST_NO_CONTACTS);
        /* 居中要的是显示宽度：「无目标」3 个汉字 9 字节，按 strlen 算会把
         * 它推到屏幕外边去。 */
        const int w = pk_aa_text_width(msg, PK_AA_L);
        LST_PUTS(fb, (PK_DISPLAY_W - w) / 2, ROW0_Y + 120, msg, COL_DIM, PK_AA_L);
        /* 清掉行映射与抽屉：否则触摸还会命中上一帧留下的飞机。 */
        for (int i = 0; i < ROW_N; ++i) s_screen_icao[i] = 0;
        s_drawer_icao = 0;
        return;
    }

    /*
     * 滚动窗口：让选中行始终留在屏内，且尽量居中。
     *
     * 不做「选中行走到底才滚一行」那种最小移动——表格里视线是往下扫的，
     * 把选中行钉在中间能同时看到它前后各三四架，比贴边好用。
     */
    /* 抽屉开着时可见行数减少。抽屉认的是 ICAO：排序一变、目标一进一出，
     * 行号就指向另一架飞机了。找不到（目标已过期）就自动收起抽屉——继续显示
     * 一架已经不在表里的飞机，比不显示更糟。 */
    int drawer_row = -1;
#ifdef PK_SIM_BUILD
    if (s_sim_drawer_row >= 0 && s_sim_drawer_row < nr)
        s_drawer_icao = s_rows[s_sim_drawer_row].ac->icao24;
#endif
    if (s_drawer_icao) {
        for (int i = 0; i < nr; ++i)
            if (s_rows[i].ac->icao24 == s_drawer_icao) { drawer_row = i; break; }
        if (drawer_row < 0) s_drawer_icao = 0;
    }
    const int rows_vis = (drawer_row >= 0) ? DRAWER_ROWS : ROW_N;

    /* 抽屉开着时，把被选中的那一行钉在可见区里——抽屉讲的就是它，
     * 让它滚出屏幕等于把上下文丢了。 */
    int first;
    if (s_scroll_first >= 0) {
        first = s_scroll_first;          /* 手指滚过：听手指的 */
    } else {
        const int anchor = (drawer_row >= 0) ? drawer_row : sel;
        first = anchor - rows_vis / 2;
    }
    if (first > nr - rows_vis) first = nr - rows_vis;
    if (first < 0)             first = 0;
    s_first_row = first;
    /* 收窄后的值要写回去：否则手指拖过了底部，s_scroll_first 会停在一个越界
     * 的大数上，往回拖时得先把那段虚位移抵消完列表才开始动——手感像卡住。 */
    if (s_scroll_first >= 0) s_scroll_first = first;

    int drawn = 0;
    for (int i = 0; i < ROW_N; ++i) s_screen_icao[i] = 0;
    for (int i = 0; i < rows_vis && first + i < nr; ++i, ++drawn) {
        draw_row(fb, &s_rows[first + i], ROW0_Y + i * ROW_H,
                 first + i == (drawer_row >= 0 ? drawer_row : sel));
        s_screen_icao[i] = s_rows[first + i].ac->icao24;
    }

    /* 线画在行**之后**：行底（斑马纹 / 威胁红底 / 选中亮底）是整条铺过去的，
     * 先画线会被行底盖掉。 */
    draw_col_seps(fb, drawn);

    if (drawer_row >= 0) {
        draw_drawer(fb, &s_rows[drawer_row], COL_TITL, COL_HDR, COL_DIM);
        return;   /* 抽屉盖住了底部的滚动条，不必再画 */
    }

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
/* 按下点是否落在列表要消费的区域内（表头 / 数据区 / 抽屉）。
 * FAB 那一块必须放行，否则列表页就切不走了。 */
static bool in_list_area(int x, int y)
{
    if (s_drawer_icao && y >= DRAWER_TOP) return true;
    if (y >= HDR_HIT_TOP && y < LIST_BOT && x >= PAD_L - 8 && x < CONTENT_R + 8)
        return true;
    if (y >= 0 && y < PFD_BAR_BOT && x >= RESET_X0 && x < RESET_X1 &&
        !sort_is_default())
        return true;
    return false;
}

/*
 * 按下。只记起点，不做动作——按下的那一刻还分不出这是点击还是滑动，
 * 现在就开抽屉的话，手指滑一下列表松手时抽屉也开了。
 */
bool pk_adsb_list_touch(int x, int y)
{
    s_press_valid = in_list_area(x, y);
    if (!s_press_valid) return false;

    s_press_x     = x;
    s_press_y     = y;
    s_press_first = s_first_row;
    s_moved       = false;

    /* 表头的按下高亮要立刻给，否则 10 fps 下点上去像没反应。真正切排序仍
     * 留到松手——高亮只是"我收到了"，不是"我做了"。 */
    if (y >= HDR_HIT_TOP && y < HDR_HIT_BOT) {
        for (int k = 0; k < SORT_COUNT; ++k)
            if (x >= COLS[k].hit_x0 && x < COLS[k].hit_x1) { s_hdr_down = k; break; }
    } else if (y < PFD_BAR_BOT) {
        s_hdr_down = HDR_HIT_RESET;
    }
    return true;
}

/*
 * 按住不放的后续帧：竖直位移换算成滚动。
 *
 * 方向与手指一致（手指往上滑 = 列表往上走 = 顶行变大），跟所有触摸列表
 * 一样；反过来会立刻觉得"这屏坏了"。
 */
bool pk_adsb_list_drag(int x, int y)
{
    if (!s_press_valid) return false;
    (void)x;

    const int dy = y - s_press_y;
    if (!s_moved && (dy > DRAG_SLOP || dy < -DRAG_SLOP)) s_moved = true;
    if (!s_moved) return true;

    s_hdr_down = -1;                       /* 已判定为拖动，撤掉按下高亮 */
    int first = s_press_first - dy / ROW_H;
    if (first < 0) first = 0;
    s_scroll_first = first;                /* 上界由 render 按行数收窄 */
    return true;
}

/*
 * 取消本次触摸：只丢状态，**不执行**动作。
 *
 * 与 touch_up 的区别很要紧：dock 展开时要让路，如果那时调 touch_up，
 * 之前落在列表上的那次按下会被当成一次完整点击提交出去——手指还没松，
 * 抽屉就自己开了。
 */
void pk_adsb_list_touch_cancel(void)
{
    s_hdr_down    = -1;
    s_press_valid = false;
    s_moved       = false;
}

/*
 * 松手：没移动过才算点击，按**按下时**的坐标分派动作。
 *
 * 用按下坐标而不是松手坐标：手指在 12 px 内挪一点仍算点击，但那点位移可能
 * 已经跨到相邻行/相邻列了，按松手位置分派会点到隔壁。
 */
void pk_adsb_list_touch_up(void)
{
    const bool click = s_press_valid && !s_moved;
    const int  x = s_press_x, y = s_press_y;

    s_hdr_down    = -1;
    s_press_valid = false;
    if (!click) { s_moved = false; return; }
    s_moved = false;

    /* 顶栏 RESET：回到默认排序。 */
    if (y >= 0 && y < PFD_BAR_BOT &&
        x >= RESET_X0 && x < RESET_X1 && !sort_is_default()) {
        s_sort         = SORT_DEFAULT_KEY;
        s_sort_desc    = false;
        s_scroll_first = -1;      /* 排序变了，回到自动锚定 */
        return;
    }

    /* 抽屉自身：点在上面不做事，但要吃掉（in_list_area 已保证走到这里）。 */
    if (s_drawer_icao && y >= DRAWER_TOP) return;

    /* 表头：切排序列 / 翻方向。 */
    if (y >= HDR_HIT_TOP && y < HDR_HIT_BOT) {
        for (int k = 0; k < SORT_COUNT; ++k) {
            if (x < COLS[k].hit_x0 || x >= COLS[k].hit_x1) continue;
            if (s_sort == (sort_key_t)k) {
                s_sort_desc = !s_sort_desc;
            } else {
                s_sort      = (sort_key_t)k;
                /* 换列时不继承上一列的方向，一律回到升序：降序的距离
                 * （最远的排最前）几乎没人想要。 */
                s_sort_desc = false;
            }
            s_scroll_first = -1;
            return;
        }
        return;
    }

    /* 数据区。 */
    if (y >= ROW0_Y && y < LIST_BOT) {
        const int rows_vis = s_drawer_icao ? DRAWER_ROWS : ROW_N;
        const int row = (y - ROW0_Y) / ROW_H;
        const uint32_t icao = (row < rows_vis) ? pk_adsb_row_icao(row) : 0;

        if (icao) {
            /* 点别的行直接换一架讲，不必先关再开；点同一行才收起。 */
            s_drawer_icao = (s_drawer_icao == icao) ? 0 : icao;
        } else {
            /* 空白处（列表不满一屏留下的空行，或抽屉盖住的那段）：关抽屉。
             * 这是最符合直觉的退出方式——不必回头去找刚才点的是哪一行。 */
            s_drawer_icao = 0;
        }
    }
}
