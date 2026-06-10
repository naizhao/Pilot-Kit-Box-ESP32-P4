/*
 * pfd_hsi_traffic.h — 在 PFD 底部 HSI 半圆罗盘外圈叠加前方交通目标。
 *
 * 只显示前方 ±95°（与 pfd_hsi.c 半圆同口径），后方目标在屏幕下角计数。
 * 恒 heading-up（不读地图朝向配置）。每帧由 pfd_task 在 PFD 模式、
 * pk_pfd_hsi_render 之后调用。
 */
#pragma once

#include <stdint.h>

void pk_pfd_hsi_traffic_render(uint16_t *fb);
