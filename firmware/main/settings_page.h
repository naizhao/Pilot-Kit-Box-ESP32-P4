/*
 * settings_page.h — user settings screen renderer.
 */
#pragma once

#include <stdint.h>

void pk_settings_page_render(uint16_t *fb);

/* 多行光标控制 (Language / QNH / MAP朝向 / RANGE量程 四行) */
void pk_settings_cursor_next(void);   /* 循环切换选中行 0→1→2→3→0 */
int  pk_settings_cursor_row(void);    /* 0=Language 1=QNH 2=MAP 3=RANGE */
