/*
 * settings_page.h — user settings screen renderer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

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
void pk_settings_page_touch_cancel(void);

/* 执行一次设置变更。row = 设置页行号，v 随控件而定：分段=段序号、
 * 步进器=±1、按钮=0。由 settings_draw.c 的命中判定调用。 */
void pk_settings_apply(int row, int v);
