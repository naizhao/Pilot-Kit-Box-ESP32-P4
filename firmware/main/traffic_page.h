/*
 * traffic_page.h — 360° 交通雷达页（PK_UI_MODE_TRAFFIC）。
 *
 * 本机居中，其它飞机按相对方位/距离落在距离环上；朝向/量程读
 * config_traffic。每帧由 pfd_task 在 TRAFFIC 模式下调用。
 */
#pragma once

#include <stdint.h>

void pk_traffic_page_render(uint16_t *fb);
