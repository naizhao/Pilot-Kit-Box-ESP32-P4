/*
 * settings_page.h — user settings screen renderer.
 */
#pragma once

#include <stdint.h>

void pk_settings_page_render(uint16_t *fb);

/* 多行光标控制 (Task 9: Language / QNH 两行) */
void pk_settings_cursor_next(void);   /* 切换选中行(Language <-> QNH 循环) */
int  pk_settings_cursor_row(void);    /* 返回当前行: 0=Language, 1=QNH */
