/* esp_system.h 桩 —— 诊断页用 esp_reset_reason()，导航网格的电源 pop
 * 用 esp_restart()。
 * 模拟器恒报 POWERON：那是最常见的真实值，也不会把 SYS 卡染成琥珀。 */
#pragma once

#include <stdlib.h>

typedef enum {
    ESP_RST_UNKNOWN = 0, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
    ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT,
    ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB,
    ESP_RST_JTAG,
} esp_reset_reason_t;

static inline esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }

/* 模拟器里「重启」就是退出进程——没有更贴切的等价物，而静默 no-return
 * 会让点了重启之后的模拟器停在一个真机上不存在的状态里。
 * 标 noreturn 是为了与真身一致：调用方（nav_grid_page.c）在它后面没有
 * 收尾代码，编译器按 no-return 推断可达性。 */
_Noreturn static inline void esp_restart(void) { exit(0); }
