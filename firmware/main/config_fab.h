/*
 * config_fab.h — FAB（悬浮操作球）落点的持久化。
 *
 * 存「哪一侧 + 垂直百分比」而不是 x/y 像素：换屏、改分辨率或旋转之后像素值
 * 全部失效，而「吸在右边、大约三分之二高」这件事一直成立。spec §3.2 也是
 * 按这两个量描述的。
 *
 * 为什么值得持久化：FAB 是这台设备唯一的常驻入口，惯用手不同的人会把它拖到
 * 相反的一侧。每次开机都弹回默认位置，等于每次飞行前都要重新摆一遍。
 */
#pragma once

#include <stdbool.h>

/* 开机时读一次，读进进程内缓存。须在 NVS 可用之后调用。 */
void pk_config_fab_load(void);

bool pk_fab_left(void);
int  pk_fab_y_pct(void);

/* 落点变化时写回。由 pk_ui_nav_on_fab_moved() 调用——只在拖动**松手**时发生，
 * 不是拖动过程中每帧，所以不存在把 flash 写穿的问题。 */
void pk_fab_pos_set(bool left, int y_pct);
