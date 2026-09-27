/*
 * settings_page.h — user settings screen renderer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 设置页行数——**两边共用这一个定义**。
 *
 * settings_draw.c 拥有版面（画的顺序就是行号），settings_page.c 按同一个
 * 行号分派写操作。这两个数此前是各自 #define 的，drift 过一次：4.3″ 上早已
 * 9 行而 settings_page.c 还写着 6，光标转到第 6 行就绕回去，后面几项按键根本
 * 选不到（那段历史见 settings_page.c 的注释）。
 *
 * drift 的代价随着行数增长只会更糟：行号错位意味着点 A 行执行 B 行的动作，
 * 而这一页的最后一行是**格式化 SD**。
 */
#define PK_SETTINGS_ROW_COUNT  17

/* ── 天线两行的段序约定（1090 与 GNSS 共用）──────────────────────────
 * 段 0 = 板载，段 1 = 外置。
 *
 * 这两行原来各自跟着自己的枚举值排：1090 是 [板载][外置]、GNSS 是
 * [外置][板载]。根源是两个枚举的 0 恰好是各自不同的上电默认（1090 默认板载、
 * GNSS 默认外接），跟着实现走就跟人的认知拧了。两行紧挨着、同一位置含义相反，
 * 手指按肌肉记忆落下去就会选反 —— 2026-09-13 实机上就误切过一次 GNSS。
 *
 * 做成共用函数而不是"两边记得写一致"：段序在 settings_draw.c 决定怎么显示、
 * 在 settings_page.c 决定怎么写入，分头各写一份迟早再次漂开。
 */
#define PK_ANT_SEG_ONBOARD   0
#define PK_ANT_SEG_EXTERNAL  1

static inline int pk_ant_seg_of(bool onboard)
{
    return onboard ? PK_ANT_SEG_ONBOARD : PK_ANT_SEG_EXTERNAL;
}

static inline bool pk_ant_seg_is_onboard(int seg)
{
    return seg == PK_ANT_SEG_ONBOARD;
}

typedef struct {
    uint8_t segment_count;
    uint8_t text_line_count;
} pk_settings_antenna_row_layout_t;

/* V4.10 的 GNSS 行只有标签与两段选择器，不再渲染旧板 ECO 警告。 */
static inline pk_settings_antenna_row_layout_t pk_settings_gnss_row_layout(void)
{
    return (pk_settings_antenna_row_layout_t){
        .segment_count = 2,
        .text_line_count = 1,
    };
}

void pk_settings_page_render(uint16_t *fb);

/* 多行光标控制 (Language / QNH / MAP朝向 / RANGE量程 / LOG存储 / FORMAT SD) */
void pk_settings_cursor_next(void);   /* 循环切换选中行 0→…→5→0 */
int  pk_settings_cursor_row(void);    /* 0=Language 1=QNH 2=MAP 3=RANGE
                                         4=LOG 5=FORMAT SD */

/* FORMAT SD 行的按键动作(UP/DOWN 短按触发):两步确认状态机。
 * 第一次按进入待确认,5 s 内再按启动后台格式化任务;无卡或日志
 * 正写 SD 时拒绝。状态由 settings 页渲染展示。 */
void pk_settings_format_action(void);

/* 两步确认状态机：1 = 已 ARM（等第二次点击确认），0 = 其余。 */
int pk_settings_format_state(void);

/* 触摸：拖动滚动（8 行放不下一屏，格式化按钮在最底下）。
 * 约定同 pk_adsb_list_* / pk_diag_page_*：返回 true 表示被本页消费。 */
bool pk_settings_page_touch(int x, int y);
bool pk_settings_page_drag(int x, int y);
void pk_settings_page_touch_up(void);

/* 执行一次设置变更。row = 设置页行号，v 随控件而定：分段=段序号、
 * 步进器=±1、按钮=0。由 settings_draw.c 的命中判定调用。 */
void pk_settings_apply(int row, int v);
