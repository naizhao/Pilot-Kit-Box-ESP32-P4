/*
 * traffic_page.h — 360° 交通雷达页（PK_UI_MODE_TRAFFIC）。
 *
 * 本机居中，其它飞机按相对方位/距离落在距离环上；朝向/量程读
 * config_traffic。每帧由 pfd_task 在 TRAFFIC 模式下调用。
 */
#pragma once

#include <stdbool.h>

#include <stdint.h>

void pk_traffic_page_render(uint16_t *fb);

/*
 * 交通页上的按钮命中处理（朝向切换 / 量程 +-）。
 *
 * 返回 true 表示这一下被按钮消费了，调用方不应再把它转给别的控件——否则手指
 * 落在按钮上同时也会点到底下的 FAB。
 *
 * 坐标是**逻辑屏坐标**，与 framebuffer 同一套。
 */
bool pk_traffic_page_touch(int x, int y);

/* 松手。清掉按下高亮——按钮要给「我收到了」的即时反馈，否则在 10 fps 上
 * 点下去像是没反应。 */
void pk_traffic_page_touch_up(void);
