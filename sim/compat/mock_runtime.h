/*
 * mock_runtime.h — 模拟器侧的运行时状态桩。
 *
 * pfd_hsi_traffic.c 要画交通目标，就得问 IMU 要航向、问 own_ship 要本机
 * 位置、问 aircraft_state 要目标表。这些在固件里是任务 + 硬件，PC 上没有。
 *
 * 与其把渲染逻辑复制一份到 sim（那就成了两套实现，迟早走偏），不如把它
 * 依赖的**数据接口**桩掉，让 pfd_hsi_traffic.c 原样编译进来。sim 从一开始
 * 就是这个原则：编译固件源码，不 fork。
 *
 * main.c 每帧调用 pk_mock_update() 把动画状态推进来，桩再据此合成本机与
 * 目标。目标是绕本机分布的一圈虚拟飞机，方位/高度各异，用来验证罗盘外圈
 * 的投影、相对高度标签、以及后方计数这三条路径。
 */
#pragma once

#include <stdbool.h>

/* 把当前帧的姿态与位置推给桩。yaw 单位度，alt 单位 ft。 */
void pk_mock_update(float yaw_deg, int own_alt_ft);

/* 是否生成本机与交通目标。关掉可验证「无数据」时各渲染器的降级表现。 */
void pk_mock_set_enabled(bool own_valid, bool traffic_valid);
