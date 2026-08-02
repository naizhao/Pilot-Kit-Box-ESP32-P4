/*
 * settings_draw.c — 设置页 800×480 的**纯绘制**层（spec §5.4）。
 *
 * 为什么单独一个文件：settings_page.c 里那套格式化两步确认状态机要开
 * FreeRTOS 任务，于是整个文件在模拟器里编不过——而版面恰恰是最需要反复
 * 看效果的部分。把绘制拆出来之后它只依赖各配置模块的 getter，模拟器用桩
 * 喂数据就能一键出图，不必为了挪一个控件去烧一次固件。
 *
 * 分工：本文件只读状态、只画像素；所有写操作（改 QNH、切量程、触发格式化）
 * 仍在 settings_page.c。
 */
#include "settings_page.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ble_gatt.h"          /* pk_ble_device_name —— 设备名那行显示完整广播名 */
#include "config_ble.h"
#include "config_demo.h"
#include "config_devname.h"    /* PK_DEVNAME_MAX_LEN —— 值框宽度按它定 */
#include "config_qnh.h"
#include "config_storage.h"
#include "config_traffic.h"
#include "display.h"
#include "i18n.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_layout.h"
#include "pk_sdcard.h"
#include "record_sink.h"

/* ═══════════════════════════════════════════════════════════════════════
 * 设置页 800×480（spec §5.4）
 *
 * 8 行 × 64 px，控件高 38 px。左半是项名，右半是控件——控件右对齐到同一条
 * 竖线上，扫一眼就知道每项当前选的是哪个，不必逐行找控件在哪。
 *
 * 分段控件（segmented）而不是下拉或滑块：选项都是 2~4 个的离散值，分段把
 * 全部选项和当前选择同时摆出来，一次触摸直达目标；下拉要两次交互，滑块在
 * 离散值上又不好停准。
 * ═════════════════════════════════════════════════════════════════════ */


#define SET_ROW_H      64
#define SET_CTL_H      38
/* 行标签、分隔线、页面标题共用整页左边距（pfd_layout.h）。本页原来自留一份
 * 20，夹在 diag/list 的 16 与 about 的 24 之间——三个页面三个数，切页时左边界
 * 每次都挪一点。 */
#define SET_PAD        PK_UI_PAD_L
#define SET_CTL_R      (PK_DISPLAY_W - 16 - 56 - 12)   /* 避开 FAB，同列表页 */

#define SET_ROWS      11
#define SET_VIEW_H    (PK_DISPLAY_H - PFD_BAR_BOT)
#define SET_MAX_SCROLL  (SET_ROWS * SET_ROW_H > SET_VIEW_H \
                         ? SET_ROWS * SET_ROW_H - SET_VIEW_H : 0)

/* 滚动偏移(px)。11 行 × 64 = 704 > 可视的 432，最后两行"演示模式 / 格式化 SD"
 * 必须滚才看得到——spec §5.4 就是这么写的（"危险按钮（需滚动可见）"），把最
 * 危险的操作放在需要多一个动作才能够到的地方。演示模式同样落在这一档：假数据
 * 在航空设备上和误格式化是同一量级的风险。 */
static int s_set_scroll;

/* 触摸手势：与列表页/诊断页同一套——按下只记起点，位移超阈值才算拖动，
 * 松手时没拖过才当点击。三页共用同一套判定，手感才一致。 */
static int  s_press_x, s_press_y, s_press_scroll;
static bool s_press_valid, s_moved;
#define SET_DRAG_SLOP  12

/* 触摸目标下限。屏 800 px ≈ 95 mm → 8.4 px/mm；通行下限 9 mm ≈ 76 px
 * （iOS 44 pt / Material 48 dp 同量级）。座舱里戴手套、有颠簸，取 80。 */
#define SEG_MIN_TOUCH_W  80

/* 设备名那行的值框宽度。与 PK_DEVNAME_MAX_LEN 绑死并加断言：名字上限是在
 * config_devname.h 里定的，改那个数的人不会想到来这里量框宽，而屏上「多出来
 * 的两个字符被裁掉了」肉眼很难发现——键盘页那两条触摸下限断言同一个道理。
 * 20 = 左右内边距各 10。 */
#define SET_DEVNAME_PAD  10
#define SET_DEVNAME_W    (PK_DEVNAME_MAX_LEN * PK_AA_S_W + 2 * SET_DEVNAME_PAD)
/* 左边那半是行标签的地盘。最宽的标签是英文 "DEVICE NAME"：M 档 15 px × 11
 * = 165 px，从 SET_PAD 起到 181 结束；预算按 240 留，够将来换更长的译名。 */
#define SET_LABEL_BUDGET 240
_Static_assert(SET_CTL_R - SET_DEVNAME_W >= SET_PAD + SET_LABEL_BUDGET,
               "设备名值框宽到压住行标签了");

/*
 * 每行控件的几何，渲染时写、触摸时读。
 *
 * 与列表页的 s_screen_icao[] 同一个套路：几何只有绘制那一刻才知道（段宽依
 * 赖文字宽度、位置依赖滚动偏移），触摸回调拿不到，所以渲染每帧留一份。
 * 好处是命中区与画出来的形状**天然一致**——分开各算一次迟早会飘，列表页
 * 的分隔线就是这么飘过一次的。
 *
 * kind: 0=无控件 1=分段 2=步进器 3=按钮
 */
typedef struct {
    uint8_t kind;
    int     x0, w, n;      /* 分段：起点/段宽/段数；步进器与按钮：起点/总宽 */
    int     y0, y1;        /* 命中区的上下沿 —— 用整行高，不是控件高 */
} row_hit_t;

static row_hit_t s_hit[SET_ROWS];

/* draw_seg 算出的段宽，紧接着由 hit_set 取走。 */
static int s_last_seg_w;

/* 一个分段控件：n 个选项，sel 为当前项。返回控件左缘，供命中判定复用。 */
static int draw_seg(uint16_t *fb, int y_mid, const char *const *opts, int n,
                    int sel, bool dim)
{
    const uint16_t SEG_OFF = pk_rgb565(28, 36, 48);
    const uint16_t SEG_ON  = pk_rgb565(0, 110, 200);
    const uint16_t SEG_TXT_ON = pk_rgb565(255, 255, 255);
    const uint16_t SEG_TXT_OFF= pk_rgb565(170, 182, 200);
    const uint16_t SEG_DIM    = pk_rgb565(90, 96, 108);

    /* 每段宽度按最长选项算，所有段等宽——不等宽的分段控件在余光里像是
     * "当前项被放大了"，会误以为那是可拖动的滑块。 */
    int maxw = 1;
    for (int i = 0; i < n; ++i) {
        const int lw = pk_aa_text_width(opts[i], PK_AA_S);
        if (lw > maxw) maxw = lw;
    }
    /*
     * 段宽有下限，不能只按文字宽度算。
     *
     * 这块屏 800 px 对应约 95 mm，即 8.4 px/mm。触摸目标的通行下限是 9 mm
     * （iOS 44 pt / Material 48 dp 都在这个量级），换算过来 ≈ 76 px。
     * 初版写的 maxw + 20，量程那行每段只有 42 px ≈ 5 mm——比指尖还小，
     * 屏上看着也像一排装饰性的小标签。
     *
     * 座舱里还要再打个折扣：戴手套、有颠簸、注意力在窗外。所以取 80 px
     * （≈9.5 mm）而不是刚好卡在 76。
     *
     * 高度那侧靠命中区补：视觉仍按 spec 的 38 px（版面不能被撑散），但
     * 点击判定用整行 64 px——这是控件类 UI 的通行做法，视觉紧凑、手感宽松。
     */
    int seg_w = maxw + 32;
    if (seg_w < SEG_MIN_TOUCH_W) seg_w = SEG_MIN_TOUCH_W;
    const int total = seg_w * n;
    const int x0    = SET_CTL_R - total;
    const int y0    = y_mid - SET_CTL_H / 2;

    /* 整组先铺一个圆角底槽，再把选中段画成圆角胶囊叠上去——这是分段控件的
     * 标准形态（iOS/Material 都是），比"每段各画一个方块"干净得多：段之间
     * 没有缝，视觉上就是一条被分成几格的轨道，而不是几个独立按钮。 */
    const int radius = SET_CTL_H / 2;      /* 全圆角，扁控件用半高最自然 */
    pk_pfd_fill_round_rect(fb, x0, y0, SET_CTL_R, y0 + SET_CTL_H, radius, SEG_OFF);

    for (int i = 0; i < n; ++i) {
        const int sx = x0 + i * seg_w;
        const bool on = (i == sel);
        if (on && !dim)
            pk_pfd_fill_round_rect(fb, sx + 2, y0 + 2, sx + seg_w - 2,
                                   y0 + SET_CTL_H - 2, radius - 2, SEG_ON);
        const int tw = pk_aa_text_width(opts[i], PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   sx + (seg_w - 2 - tw) / 2, y0 + (SET_CTL_H - PK_AA_S_H) / 2,
                   opts[i], dim ? SEG_DIM : (on ? SEG_TXT_ON : SEG_TXT_OFF),
                   PK_AA_S);
    }
    s_last_seg_w = seg_w;
    return x0;
}

static int seg_last_w(void) { return s_last_seg_w; }

/* 记一行的命中区。y 用**整行**而不是控件高：视觉 38 px 是 spec 定的，
 * 但 38 px ≈ 4.5 mm 手指够不着，命中放宽到 64 px 是控件类 UI 的通行做法。 */
static void hit_set(int row, uint8_t kind, int x0, int w, int n, int y_mid)
{
    if (row < 0 || row >= SET_ROWS) return;
    s_hit[row] = (row_hit_t){ kind, x0, w, n,
                              y_mid - SET_ROW_H / 2, y_mid + SET_ROW_H / 2 };
}

/* 步进器：− 值 +。值居中，两枚按钮等宽，与分段控件右缘对齐。 */
static void draw_stepper(uint16_t *fb, int y_mid, const char *val)
{
    const uint16_t STP_BTN = pk_rgb565(28, 36, 48);
    const uint16_t STP_TXT = pk_rgb565(235, 240, 248);
    /* 与分段同一个下限：44 px 只有 5.2 mm，比指尖还小。QNH 是要连点好几下
     * 才调到位的控件（0.01 hPa 一步），点不准的代价比别处更大。 */
    const int btn_w = SEG_MIN_TOUCH_W;
    const int val_w = 140;
    const int y0    = y_mid - SET_CTL_H / 2;
    const int x0    = SET_CTL_R - (btn_w * 2 + val_w);

    const int r = SET_CTL_H / 2;
    pk_pfd_fill_round_rect(fb, x0, y0, x0 + btn_w, y0 + SET_CTL_H, r, STP_BTN);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x0 + btn_w / 2 - 5,
               y0 + (SET_CTL_H - PK_AA_M_H) / 2, "-", STP_TXT, PK_AA_M);

    const int vw = pk_aa_text_width(val, PK_AA_S);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               x0 + btn_w + (val_w - vw) / 2,
               y0 + (SET_CTL_H - PK_AA_S_H) / 2, val, STP_TXT, PK_AA_S);

    const int bx = x0 + btn_w + val_w;
    pk_pfd_fill_round_rect(fb, bx, y0, bx + btn_w, y0 + SET_CTL_H, r, STP_BTN);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, bx + btn_w / 2 - 5,
               y0 + (SET_CTL_H - PK_AA_M_H) / 2, "+", STP_TXT, PK_AA_M);
}

void pk_settings_page_render(uint16_t *fb)
{
    const uint16_t V2_BG   = pk_rgb565(7, 10, 16);
    /* 标题色不在这里另立一份，走 PK_UI_TITLE_COL —— 本页原来的 (235,235,235)
     * 正是被选为全局标题色的那个值，抄一份只会让下次改动漏掉这里。 */
    const uint16_t V2_KEY  = pk_rgb565(215, 222, 232);
    const uint16_t V2_LINE = pk_rgb565(26, 33, 44);

#ifdef PK_SIM_BUILD
    { void pk_settings_sim_scroll(void); pk_settings_sim_scroll(); }
#endif
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, V2_BG);

    int row = 0;
    #define ROW_Y(i)  (PFD_BAR_BOT + (i) * SET_ROW_H - s_set_scroll + SET_ROW_H / 2)
    #define ROW_LABEL(i, text) do {                                            \
        const int _y = ROW_Y(i);                                               \
        if (_y > PFD_BAR_BOT - SET_ROW_H && _y < PK_DISPLAY_H + SET_ROW_H) {    \
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SET_PAD,                \
                       _y - PK_UI_ITEM_H / 2, (text), V2_KEY,                  \
                       PK_UI_ITEM_SIZE);                                       \
            pk_pfd_fill_rect(fb, SET_PAD, _y + SET_ROW_H / 2 - 1,              \
                             SET_CTL_R, _y + SET_ROW_H / 2, V2_LINE);         \
        }                                                                      \
    } while (0)

    /* 1 语言 */
    { /* 两个选项不过 catalog：语言选择器要用各语言自己的写法（endonym），中文
       * 永远是「中文」、英文永远是「EN」。翻译它就成了「当前看不懂的语言里
       * 写着另一种看不懂的语言」——切过去就切不回来。 */
      const char *o[] = { "\u4e2d\u6587", "EN" };
      ROW_LABEL(row, pk_i18n_text(PK_TR_SETTINGS_LANGUAGE));
      const int _x = draw_seg(fb, ROW_Y(row), o, 2,
                             pk_i18n_get_lang() == PK_LANG_ZH ? 0 : 1, false);
      hit_set(row, 1, _x, seg_last_w(), 2, ROW_Y(row));
      row++; }

    /* 2 QNH —— 步进器：它是连续量，分段摆不下。 */
    { char v[16]; snprintf(v, sizeof(v), "%.2f hPa", (double)pk_qnh_get());
      ROW_LABEL(row, pk_i18n_text(PK_TR_SETTINGS_QNH));
      draw_stepper(fb, ROW_Y(row), v);
      hit_set(row, 2, SET_CTL_R - (SEG_MIN_TOUCH_W * 2 + 140),
              SEG_MIN_TOUCH_W * 2 + 140, 0, ROW_Y(row));
      row++; }

    /* 3 地图朝向 */
    { const char *o[] = { pk_i18n_text(PK_TR_MAP_ORIENT_HDG_UP),
                          pk_i18n_text(PK_TR_MAP_ORIENT_NORTH_UP) };
      ROW_LABEL(row, pk_i18n_text(PK_TR_SETTINGS_MAP_ORIENT));
      const int _x = draw_seg(fb, ROW_Y(row), o, 2,
               pk_map_orient_get() == PK_MAP_HEADING_UP ? 0 : 1, false);
      hit_set(row, 1, _x, seg_last_w(), 2, ROW_Y(row));
      row++; }

    /* 4 雷达量程 —— 选项取自 pk_traffic_range_nm，不另抄一份数字。 */
    { static const char *o[] = { "2", "5", "10", "20" };
      ROW_LABEL(row, pk_i18n_text(PK_TR_SETTINGS_RADAR_RANGE));
      const int _x = draw_seg(fb, ROW_Y(row), o, 4, pk_traffic_range_idx_get(), false);
      hit_set(row, 1, _x, seg_last_w(), 4, ROW_Y(row));
      row++; }

    /* 5 屏幕亮度 */
    /* 段数与 display.h 的 PK_BL_STEP_COUNT 一致：曾经多摆一个 AUTO 档，
     * 但板上没有环境光传感器，那一格永远点不动。灰着的格子照样占触摸宽度，
     * 还让人反复怀疑是不是坏了——干脆不出现。 */
    { const char *o[] = { pk_i18n_text(PK_TR_BRIGHT_LOW),
                          pk_i18n_text(PK_TR_BRIGHT_MID),
                          pk_i18n_text(PK_TR_BRIGHT_HIGH) };
      ROW_LABEL(row, pk_i18n_text(PK_TR_SETTINGS_BRIGHTNESS));
      const int _x = draw_seg(fb, ROW_Y(row), o, PK_BL_STEP_COUNT,
                              pk_backlight_step_get(), false);
      hit_set(row, 1, _x, seg_last_w(), PK_BL_STEP_COUNT, ROW_Y(row));
      row++; }

    /* 6 日间/夜间配色 */
    { const char *o[] = { pk_i18n_text(PK_TR_THEME_DAY),
                          pk_i18n_text(PK_TR_THEME_NIGHT) };
      ROW_LABEL(row, pk_i18n_text(PK_TR_SETTINGS_THEME));
      draw_seg(fb, ROW_Y(row), o, 2, 0, true);   /* 尚未接入，整行置灰 */
      row++; }

    /* 7 记录存储 */
    { const char *o[] = { pk_i18n_text(PK_TR_LOG_STORE_FLASH),
                          pk_i18n_text(PK_TR_LOG_STORE_SD) };
      ROW_LABEL(row, pk_i18n_text(PK_TR_SETTINGS_LOG_STORE));
      const int _x = draw_seg(fb, ROW_Y(row), o, 2,
               pk_log_store_get() == PK_LOG_STORE_SD ? 1 : 0,
               !pk_sdcard_is_mounted());
      hit_set(row, 1, _x, seg_last_w(), 2, ROW_Y(row));
      row++; }

    /* 8 蓝牙开关（P2-4）——放在格式化之前，让危险按钮独占最底下那一行。 */
    { const char *o[] = { pk_i18n_text(PK_TR_SWITCH_OFF),
                          pk_i18n_text(PK_TR_SWITCH_ON) };
      const char *label = pk_i18n_text(PK_TR_SETTINGS_BLUETOOTH);
      ROW_LABEL(row, label);
      const int _x = draw_seg(fb, ROW_Y(row), o, 2, pk_ble_enabled_get() ? 1 : 0, false);
      hit_set(row, 1, _x, seg_last_w(), 2, ROW_Y(row));
      /* 「重启后生效」必须写出来：BLE 起停牵扯 NimBLE 的卸载路径，更牵扯
       * hosted 握手要排在点屏之前那条硬约束，运行时切会打乱开机顺序。
       * 不写的话，点了没反应会被当成 bug。 */
      { const int _y = ROW_Y(row);
        if (_y > PFD_BAR_BOT - SET_ROW_H && _y < PK_DISPLAY_H + SET_ROW_H)
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                       SET_PAD + pk_aa_text_width(label,
                                                  PK_UI_ITEM_SIZE) + 16,
                       _y - PK_AA_XS_H / 2,
                       pk_i18n_text(PK_TR_SETTINGS_RESTART_HINT),
                       pk_rgb565(120, 130, 145), PK_AA_XS); }
      row++; }

    /* 9 设备名（P2-5）——紧跟蓝牙那行：它改的就是 BLE 广播出去的名字，
     * 隔开放会让人以为是另一码事。控件是个可点的值框，点了进键盘编辑器。 */
    { ROW_LABEL(row, pk_i18n_text(PK_TR_SETTINGS_DEVNAME));
      const int y_mid = ROW_Y(row);
      const int y0 = y_mid - SET_CTL_H / 2;
      /* 框宽按**最长的名字**定，不是按当前值。最长的是自定义名：26 字符
       * （= adv 预算，config_devname.h），S 档 11 px/字符 = 286 px，左右各
       * 留 10 → 306。出厂默认 "Pilot Kit Box-AABBCC" 只有 20 字符（220 px），
       * 比它短。
       *
       * 选择加宽而不是截断显示：这一行的用处就是让用户核对「手机上会扫到
       * 什么」，显示成 "N123AB-HANGAR-0…" 等于把这个用处废掉。右缘 716
       * （SET_CTL_R，避开 FAB）− 306 = 410，而最宽的行标签 "DEVICE NAME"
       * 是 M 档 15 px × 11 字符 = 165 px，从 16 起到 181 就结束，中间还空着
       * 229 px。 */
      const int w  = SET_DEVNAME_W;
      const int x0 = SET_CTL_R - w;
      pk_pfd_fill_round_rect(fb, x0, y0, x0 + w, y0 + SET_CTL_H,
                             SET_CTL_H / 2, pk_rgb565(28, 36, 48));
      /* 显示的是**完整广播名**（含 MAC 后缀），不是用户存的那半截：用户要
       * 核对的是「手机上会扫到什么」。未设置时前半段就是出厂前缀。 */
      const char *nm = pk_ble_device_name();
      /* 名字全是 ASCII（字符集限死在 A-Z 0-9 - _），但宽度仍走
       * pk_aa_text_width：这一页别处都这么算，留一处 strlen×cell 迟早被抄走。 */
      pk_aa_size_t sz = PK_AA_S;
      int tw = pk_aa_text_width(nm, sz);
      /* 装不下就降档而不是截断——名字被切一半比小一号更难辨认。 */
      if (tw > w - 20) { sz = PK_AA_XS; tw = pk_aa_text_width(nm, sz); }
      pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x0 + (w - tw) / 2,
                 y0 + (SET_CTL_H - pk_aa_cell_h(sz)) / 2, nm,
                 pk_rgb565(235, 240, 248), sz);
      hit_set(row, 3, x0, w, 0, y_mid);
      row++; }

    /* 10 演示模式 —— 排在倒数第二行，仅次于格式化 SD 的位置。
     *
     * 为什么放这么靠后：它和格式化一样属于「不该被顺手点到」的一类。假数据在
     * 航空设备上是会害人的，所以入口必须要多滚一屏才够得到——与 spec §5.4 把
     * 危险按钮放在需要滚动才可见处是同一条理由。同时它**必须**留在设置页里、
     * 由用户显式打开，不设任何快捷手势或组合键。 */
    { const char *o[] = { pk_i18n_text(PK_TR_SWITCH_OFF),
                          pk_i18n_text(PK_TR_SWITCH_ON) };
      const char *label = pk_i18n_text(PK_TR_SETTINGS_DEMO);
      ROW_LABEL(row, label);
      const int _x = draw_seg(fb, ROW_Y(row), o, 2, pk_demo_enabled() ? 1 : 0, false);
      hit_set(row, 1, _x, seg_last_w(), 2, ROW_Y(row));
      /* 「数据为模拟」这句小字与蓝牙那行的「重启生效」同一个位置、同一个档，
       * 但配色不同：那句是中性说明，这句是警告，开关旁边就得让人看见代价。 */
      { const int _y = ROW_Y(row);
        if (_y > PFD_BAR_BOT - SET_ROW_H && _y < PK_DISPLAY_H + SET_ROW_H)
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                       SET_PAD + pk_aa_text_width(label, PK_UI_ITEM_SIZE) + 16,
                       _y - PK_AA_XS_H / 2,
                       pk_i18n_text(PK_TR_SETTINGS_DEMO_HINT),
                       pk_demo_enabled() ? pk_rgb565(255, 90, 80)
                                         : pk_rgb565(120, 130, 145),
                       PK_AA_XS); }
      row++; }

    /* 11 格式化 SD —— 危险按钮，红底。文案跟着两步确认状态机走。 */
    { ROW_LABEL(row, pk_i18n_text(PK_TR_SETTINGS_FORMAT_SD));
      const int y_mid = ROW_Y(row);
      const int y0 = y_mid - SET_CTL_H / 2;
      const int w  = 200;
      const int x0 = SET_CTL_R - w;
      const bool armed = (pk_settings_format_state() == 1);
      const bool avail = pk_sdcard_is_mounted() && !record_sink_file_uses_sd();
      pk_pfd_fill_round_rect(fb, x0, y0, x0 + w, y0 + SET_CTL_H, SET_CTL_H / 2,
                       !avail  ? pk_rgb565(45, 45, 50)
                       : armed ? pk_rgb565(200, 40, 40)
                               : pk_rgb565(90, 30, 30));
      const char *txt =
            !pk_sdcard_is_mounted()      ? pk_i18n_text(PK_TR_FORMAT_BTN_NO_CARD)
          : record_sink_file_uses_sd()   ? pk_i18n_text(PK_TR_FORMAT_BTN_IN_USE)
          : armed                        ? pk_i18n_text(PK_TR_FORMAT_BTN_ARMED)
                                         : pk_i18n_text(PK_TR_FORMAT_BTN_FORMAT);
      const int tw = pk_aa_text_width(txt, PK_AA_S);
      pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x0 + (w - tw) / 2,
                 y0 + (SET_CTL_H - PK_AA_S_H) / 2, txt,
                 avail ? pk_rgb565(255, 255, 255) : pk_rgb565(120, 124, 132),
                 PK_AA_S);
      hit_set(row, 3, x0, w, 0, y_mid);
      row++; }

    /* 顶栏最后画，行从底下滑过。 */
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PFD_BAR_BOT, V2_BG);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SET_PAD,
               PK_UI_TITLE_Y, pk_i18n_text(PK_TR_SETTINGS_TITLE),
               PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);
    #undef ROW_Y
    #undef ROW_LABEL
}

/* ── 触摸 ──────────────────────────────────────────────────────────
 * 目前只做滚动。各控件的点击命中要等写操作那侧（改 QNH、切量程…）一起接，
 * 那些 setter 在 settings_page.c 里；渲染层先把滚动落地，否则第 8 行根本
 * 够不到。 */
bool pk_settings_page_touch(int x, int y)
{
    /* 右侧 FAB 那条必须放行，否则设置页就切不走了（列表页踩过这个坑）。 */
    s_press_valid = (y >= PFD_BAR_BOT && x < SET_CTL_R + 8);
    if (!s_press_valid) return false;
    s_press_x      = x;
    s_press_y      = y;
    s_press_scroll = s_set_scroll;
    s_moved        = false;
    return true;
}

bool pk_settings_page_drag(int x, int y)
{
    if (!s_press_valid) return false;
    (void)x;
    const int dy = y - s_press_y;
    if (!s_moved && (dy > SET_DRAG_SLOP || dy < -SET_DRAG_SLOP)) s_moved = true;
    if (!s_moved) return true;

    int sy = s_press_scroll - dy;      /* 方向与手指一致 */
    if (sy < 0) sy = 0;
    if (sy > SET_MAX_SCROLL) sy = SET_MAX_SCROLL;
    s_set_scroll = sy;
    return true;
}

/*
 * 松手：没拖动过才算点击，按**按下时**的坐标分派。
 *
 * 分工：本文件只做命中判定（它拥有几何），真正的写操作在 settings_page.c
 * 的 pk_settings_apply()——那边才该碰 NVS、起格式化任务。
 */
void pk_settings_page_touch_up(void)
{
    const bool click = s_press_valid && !s_moved;
    const int  y = s_press_y, x = s_press_x;
    s_press_valid = false;
    s_moved       = false;
    if (!click) return;

    for (int r = 0; r < SET_ROWS; ++r) {
        const row_hit_t *h = &s_hit[r];
        if (h->kind == 0 || y < h->y0 || y >= h->y1) continue;
        if (x < h->x0 || x >= h->x0 + h->w * (h->kind == 1 ? h->n : 1)) continue;

        switch (h->kind) {
        case 1:   /* 分段：算落在第几段 */
            pk_settings_apply(r, (x - h->x0) / h->w);
            return;
        case 2: { /* 步进器：左 1/3 是 −，右 1/3 是 + ，中间的值区不响应 */
            const int third = h->w / 3;
            if (x < h->x0 + third)            pk_settings_apply(r, -1);
            else if (x >= h->x0 + h->w - third) pk_settings_apply(r, +1);
            return;
        }
        case 3:   /* 按钮 */
            pk_settings_apply(r, 0);
            return;
        default:
            return;
        }
    }
}

#ifdef PK_SIM_BUILD
/* 截图用：PK_SIM_SET_SCROLL=<px> 直接把版面滚到指定位置。 */
#include <stdlib.h>
void pk_settings_sim_scroll(void)
{
    const char *e = getenv("PK_SIM_SET_SCROLL");
    if (e) {
        int v = atoi(e);
        s_set_scroll = v < 0 ? 0 : (v > SET_MAX_SCROLL ? SET_MAX_SCROLL : v);
    }
}
#endif
