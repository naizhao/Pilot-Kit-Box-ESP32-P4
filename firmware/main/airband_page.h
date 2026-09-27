#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PK_AIRBAND_STATION_LABEL_MAX 24

typedef struct {
    uint32_t freq_khz;
    uint32_t airport_idx;
    uint8_t service;
    const char *label;
} pk_airband_station_candidate_t;

typedef struct {
    uint32_t channel_khz;
    uint32_t frequency_hz;
    uint32_t airport_idx;
    uint8_t service;
    char label[PK_AIRBAND_STATION_LABEL_MAX];
} pk_airband_station_t;

typedef enum {
    PK_AIRBAND_PAGE_ACT_NONE = 0,
    PK_AIRBAND_PAGE_ACT_MANUAL_FREQUENCY,
    PK_AIRBAND_PAGE_ACT_TUNE,
    PK_AIRBAND_PAGE_ACT_SPEAKER_TEST,
    PK_AIRBAND_PAGE_ACT_MUTE,
    PK_AIRBAND_PAGE_ACT_VOLUME_DOWN,
    PK_AIRBAND_PAGE_ACT_VOLUME_UP,
} pk_airband_page_action_kind_t;

typedef struct {
    pk_airband_page_action_kind_t kind;
    int station_index;
} pk_airband_page_action_t;

typedef int (*pk_airband_nearest_query_fn)(double lat, double lon,
                                            void *out, int max);

/* 优先查已驻留的航空窗口；窗口未就绪/未覆盖时才回退整库。 */
int pk_airband_nearest_with_fallback(double lat, double lon, void *out, int max,
                                     pk_airband_nearest_query_fn window_query,
                                     pk_airband_nearest_query_fn full_query);

/* 页面已打开但首次加载早于 GPS fix / 数据库 ready 时，按间隔重试。
 * now/last 使用同一个 uint32 单调时基，减法按回绕语义计算。 */
bool pk_airband_page_should_retry_loader(bool loader_running,
                                         bool nearby_loaded,
                                         uint32_t now, uint32_t last,
                                         uint32_t interval);

/* 通过与最终列表相同的频道合法性和载频去重规则计算可用候选数。 */
size_t pk_airband_station_usable_candidate_count(
    const pk_airband_station_candidate_t *in, size_t in_count);

/* 窗口虽查到机场、但频率全部带外/离散频道无效/重复到零个可用台时，
 * 仍须查询“带频率机场”的整库路径。 */
bool pk_airband_page_should_query_full_db(
    int window_airport_count,
    const pk_airband_station_candidate_t *candidates,
    size_t candidate_count);

/* 候选入队前统一做频道合法性、实际载频去重和容量检查。这样窗口返回大量
 * 非法频率时不会占满数组，整库 fallback 仍有空间写入。 */
bool pk_airband_station_candidate_can_append(
    const pk_airband_station_candidate_t *existing, size_t count,
    size_t capacity, uint32_t channel_khz);

/* 过滤航空话音频段、按频率去重，并补上两条无需定位也能使用的预置频率。 */
size_t pk_airband_station_collect(const pk_airband_station_candidate_t *in,
                                  size_t in_count,
                                  pk_airband_station_t *out,
                                  size_t capacity);

pk_airband_page_action_t pk_airband_page_action_at(int x, int y,
                                                   size_t station_count);

/* ICAO 频道文本换算为实际载频，不经过浮点；拒绝非频道栅格值。 */
bool pk_airband_frequency_parse_mhz(const char *text, uint32_t *out_hz);

/* 页面只在可见 FAB 之外拥有按压；供触摸仲裁与 host 回归测试共用。 */
bool pk_airband_page_owns_press(int x, int y, bool fab_visible,
                               int fab_x, int fab_y, int fab_w, int fab_h);

#ifndef PK_AIRBAND_PAGE_HOST_TEST
void pk_airband_page_init(void);
void pk_airband_page_open(void);
void pk_airband_page_render(uint16_t *fb);
bool pk_airband_page_touch(int x, int y);
bool pk_airband_page_drag(int x, int y);
void pk_airband_page_touch_up(void);
#endif

#ifdef __cplusplus
}
#endif
