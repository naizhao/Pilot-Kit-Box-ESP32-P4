#pragma once
#include <stdbool.h>
#include <stdint.h>
void pk_diag_page_render(uint16_t *fb);

/* 触摸：拖动滚动卡片区。约定同 pk_adsb_list_*——返回 true 表示这一下被本页
 * 消费，调用方不应再转给别的控件。dock 展开时由 read_cb 统一让路。 */
bool pk_diag_page_touch(int x, int y);
bool pk_diag_page_drag(int x, int y);
void pk_diag_page_touch_up(void);
void pk_diag_page_touch_cancel(void);
