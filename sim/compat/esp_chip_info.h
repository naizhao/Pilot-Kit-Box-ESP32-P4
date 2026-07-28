/* esp_chip_info.h — 模拟器桩，见 esp_app_desc.h 的说明。 */
#pragma once
typedef struct { int model; int features; int revision; int cores; } esp_chip_info_t;
void esp_chip_info(esp_chip_info_t *out);
