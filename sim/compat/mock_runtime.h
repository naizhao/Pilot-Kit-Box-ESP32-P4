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

/*
 * 「这一项该不该缺数据」的统一判定。
 *
 * 为什么要有总开关：此前 mock 只压「极端大数据」（最长呼号、目标扎堆、数值
 * 极值），可用户第一次开机看到的是**另一个极端**——什么都没有。产品负责人
 * 手上那台盒子 IMU/气压/GPS/SDR 全没接，交通页那句「无本机位置」被本机符号
 * 压住，就是这一侧从没被截过图的直接后果。
 *
 * 用法：PK_SIM_EMPTY=1 一次把所有数据源掐掉（=出厂开机 / 外设全没接）；
 * 或用单项开关只掐一样，定位「只缺这一个」时该页降级成什么样。
 */
bool pk_sim_flag(const char *key);
