/*
 * esp_attr.h — PC 模拟器的空实现。
 *
 * 固件里的绘制模块会 include 这个头文件来取段放置属性，例如
 * pfd_attitude.c 用 EXT_RAM_BSS_ATTR 把预计算的天空/地面渐变查找表
 * （s_sky_grad / s_ground_grad，各 16×ATTITUDE_HEIGHT 个 uint16）
 * 放到外部 PSRAM，避免挤占内部 SRAM。
 *
 * 这些属性只对 ESP32 的内存布局有意义，PC 上没有对应概念，全部定义
 * 成空即可 —— 这也是这批绘制代码能原样在两个平台编译的唯一前提。
 *
 * 若后续纳入更多固件源文件时又冒出未定义的 *_ATTR，往这里补即可，
 * 不要去改固件源码。
 */
#pragma once

/* 代码段 */
#define IRAM_ATTR
#define DRAM_ATTR
#define FORCE_INLINE_ATTR       inline

/* 外部 PSRAM */
#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR

/* RTC 内存（深睡保留区）—— 本项目取消 deep sleep 后基本用不到，
 * 但历史代码里可能残留，一并兜住 */
#define RTC_DATA_ATTR
#define RTC_RODATA_ATTR
#define RTC_IRAM_ATTR
#define RTC_SLOW_ATTR
#define RTC_FAST_ATTR
#define RTC_NOINIT_ATTR

/* 其它 */
#define NOINIT_ATTR
#define __NOINIT_ATTR
#define WORD_ALIGNED_ATTR       __attribute__((aligned(4)))
#define DRAM_STR(str)           (str)
