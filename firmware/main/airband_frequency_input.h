#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PK_AIRBAND_FREQ_INPUT_TEXT_MAX 7
#define PK_AIRBAND_FREQ_KEY_DELETE     '\b'
#define PK_AIRBAND_FREQ_KEY_OK         '\n'

typedef struct {
    char text[PK_AIRBAND_FREQ_INPUT_TEXT_MAX + 1];
    bool replace_armed;
} pk_airband_frequency_input_t;

/* ICAO 频道标识（kHz）换算为 BK4819 实际载频。支持 25 kHz 与 8.33 kHz
 * 频道；例如 118005 -> 118000000，118010 -> 118008330。 */
bool pk_airband_channel_to_carrier_hz(uint32_t channel_khz,
                                      uint32_t *carrier_hz);

/* 解析人机界面的频道文本，同时保留频道标识与实际载频。 */
bool pk_airband_channel_text_parse(const char *text, uint32_t *channel_khz,
                                   uint32_t *carrier_hz);

/* 初始频率保留在屏上；第一个合法数字会整体覆盖它。 */
void pk_airband_frequency_input_init(pk_airband_frequency_input_t *state,
                                     const char *initial);

/* disabled 的按键同时用于绘制和触摸判定，避免“看似禁用但仍能输入”。 */
bool pk_airband_frequency_input_key_enabled(
    const pk_airband_frequency_input_t *state, char key);

/* 应用数字、小数点或删除；disabled 键返回 false 且状态完全不变。 */
bool pk_airband_frequency_input_apply(pk_airband_frequency_input_t *state,
                                      char key);

/* 把完整输入补齐成三位小数并换算为 Hz；不完整前缀返回 false。 */
bool pk_airband_frequency_input_commit(
    const pk_airband_frequency_input_t *state,
    char normalized[PK_AIRBAND_FREQ_INPUT_TEXT_MAX + 1], uint32_t *out_hz);
