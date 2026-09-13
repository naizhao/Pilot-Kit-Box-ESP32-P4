/*
 * ant_adv_page.h — 「1090 高级」页：解调门限调节 + 门限扫频。
 *
 * 为什么单独一页而不是往设置页里塞几行：这页是**调试工具**，不是日常项。
 * 它有一张 9 行的结果表、一个跑 90 秒的异步任务和一个会改射频行为的步进器，
 * 混在语言/QNH 那些每次都看的行里既挤又危险。走设置页第 12 行「罗盘校准」
 * 同款的跳转模式。
 *
 * 为什么必须做到屏上：门限的最优点不是常数——2026-09-13 同一块 V4.4 板、
 * 同一根天线，一天之内从 600 漂到 700，换天线换位置后又变。此前调门限只能
 * 通过 RP2040 的 CDC 命令，等于"必须接电脑"，而需要调的场合恰恰在外场。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void pk_ant_adv_page_render(uint16_t *fb);

/* 约定同 pk_diag_page_* / pk_settings_page_*：返回 true 表示被本页消费。
 * 本页没有滚动，只有按钮，所以不需要 drag/touch_up。 */
bool pk_ant_adv_page_touch(int x, int y);
