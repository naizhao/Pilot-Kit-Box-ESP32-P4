/* sdkconfig.h — 模拟器桩：只提供绘制模块实际读到的那几个 Kconfig 值。
 * 数值与 firmware/sdkconfig.defaults 保持一致，不一致会让陈旧判定的
 * 行为和真机对不上。 */
#pragma once

#define CONFIG_PK_OWN_STALE_AGE_MS   5000   /* = Kconfig.projbuild default */
