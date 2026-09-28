#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pfd_icon_font.h"

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

typedef struct {
    pk_airband_station_t *rows;
    size_t count, capacity;
} pk_airband_station_list_t;
typedef struct {
    pk_airband_station_list_t stations;
    bool complete, failed;
} pk_airband_page_list_t;
/* Open preserves the last good result. True only when presets are needed. */
bool pk_airband_page_list_open(pk_airband_page_list_t *page);
/* Transfers ownership; caller frees the returned old list outside its lock. */
pk_airband_station_list_t pk_airband_page_list_publish(pk_airband_page_list_t *page,
    pk_airband_station_list_t *next, bool complete);
void pk_airband_page_list_failed(pk_airband_page_list_t *page);
void pk_airband_page_list_task_created(pk_airband_page_list_t *page, bool created);
/* Allocation failure leaves the complete previous list unchanged. */
bool pk_airband_station_list_append(pk_airband_station_list_t *list,
                                     const pk_airband_station_candidate_t *row);
void pk_airband_station_list_free(pk_airband_station_list_t *list);
int pk_airband_scroll_clamp(int offset, size_t count);
int pk_airband_scroll_drag(int initial_offset, int delta_y, size_t count);

typedef enum {
    PK_AIRBAND_PAGE_ACT_NONE = 0,
    PK_AIRBAND_PAGE_ACT_MANUAL_FREQUENCY,
    PK_AIRBAND_PAGE_ACT_AUTHORIZE_PHONE,
    PK_AIRBAND_PAGE_ACT_TUNE,
    PK_AIRBAND_PAGE_ACT_MUTE,
    PK_AIRBAND_PAGE_ACT_VOLUME_DOWN,
    PK_AIRBAND_PAGE_ACT_VOLUME_UP,
    PK_AIRBAND_PAGE_ACT_RECORD,
} pk_airband_page_action_kind_t;

typedef struct {
    pk_airband_page_action_kind_t kind;
    int station_index;
} pk_airband_page_action_t;
pk_airband_page_action_t pk_airband_page_action_scrolled(int x, int y,
                                                        size_t count, int offset);
typedef struct {
    int x, y, initial_offset;
    bool active, moved;
} pk_airband_page_gesture_t;
void pk_airband_page_gesture_press(pk_airband_page_gesture_t *g, int x, int y, int offset);
int pk_airband_page_gesture_drag(pk_airband_page_gesture_t *g, int x, int y,
                                 size_t count, int offset);
pk_airband_page_action_t pk_airband_page_gesture_release(pk_airband_page_gesture_t *g,
                                                       size_t count, int offset);
/* 列表异步换代只会使列表行索引失效；固定控件手势必须继续完成。 */
void pk_airband_page_gesture_cancel_for_list_publish(
    pk_airband_page_gesture_t *gesture);

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

/* Legacy fixed-output helper: SIZE_MAX means insufficient capacity, never
 * treat its partial output as a valid list. Production uses the growing list. */
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

/* FAB 只参与触摸仲裁，不能让底下的列表行视觉缩进。 */
int pk_airband_page_station_row_right(void);

/* uint32 单调 tick 的短时弹层判定；用 elapsed 而不是 deadline 排序，避免
 * 未激活/过期时间戳在半个 tick 周期后“复活”。 */
bool pk_airband_volume_popup_visible(bool active, uint32_t now,
                                     uint32_t started, uint32_t duration);

/* 按住音量键时立即返回对应动作；拖动超过手势阈值后取消。有效点击松手后
 * 可用 latched_* 保持短暂反馈，确保低帧率页面也至少显示一帧按键状态。 */
pk_airband_page_action_kind_t pk_airband_volume_feedback_action(
    const pk_airband_page_gesture_t *gesture,
    pk_airband_page_action_kind_t latched_action, bool latched_active,
    uint32_t now, uint32_t latched_started, uint32_t duration);
/* 任意新手势都会接管视觉状态，防止旧的松手反馈在新手势取消后复活。 */
void pk_airband_volume_feedback_cancel_latch(
    pk_airband_page_action_kind_t *latched_action, bool *latched_active);

typedef enum {
    PK_AIRBAND_REC_VIS_IDLE = 0,
    PK_AIRBAND_REC_VIS_ARMED,
    PK_AIRBAND_REC_VIS_ACTIVE,
    PK_AIRBAND_REC_VIS_WRITING,
    PK_AIRBAND_REC_VIS_ERROR,
    PK_AIRBAND_REC_VIS_SD_MISSING,
    PK_AIRBAND_REC_VIS_DROPPED,
} pk_airband_record_visual_label_t;

typedef struct {
    pk_airband_record_visual_label_t label;
    bool red;
    bool warning;
} pk_airband_record_visual_t;

/* active 必须优先于粘滞 interrupted：恢复写入后仍显示红色录制态，同时用
 * warning 标记保留“本次会话曾丢帧”的信息。 */
pk_airband_record_visual_t pk_airband_record_visual_resolve(
    bool stopping, bool error, bool sd_missing, bool interrupted,
    bool active, bool enabled);

/* 录制按钮把图标、间距和多语言文案视为一个整体居中。 */
int pk_airband_record_content_start(int label_width);

typedef enum {
    PK_AIRBAND_BUTTON_COLOR_PANEL = 0,
    PK_AIRBAND_BUTTON_COLOR_BLUE = 1,
    PK_AIRBAND_BUTTON_COLOR_ORANGE = 2,
} pk_airband_button_color_role_t;

/* 可播放是蓝色主状态；静音是橙色提醒状态。 */
pk_airband_button_color_role_t pk_airband_mute_button_color_role(bool muted);

typedef enum {
    PK_AIRBAND_CONTROL_ICON_NONE = 0,
    PK_AIRBAND_CONTROL_ICON_VOLUME_DOWN,
    PK_AIRBAND_CONTROL_ICON_BRAND_AWARENESS,
    PK_AIRBAND_CONTROL_ICON_VOLUME_OFF,
    PK_AIRBAND_CONTROL_ICON_VOLUME_UP,
} pk_airband_control_icon_role_t;

/* 语义角色独立于生成图标字体，host 测试无需引入 pfd_icon_font.h。 */
pk_airband_control_icon_role_t pk_airband_control_icon_role(
    pk_airband_page_action_kind_t action, bool muted);

/* 语义角色到实际生成字形枚举的最终映射；保持在 host 可测区域，避免页面
 * 选择正确角色、渲染层却画出另一枚图标。 */
pk_icon_id_t pk_airband_control_icon_id(pk_airband_control_icon_role_t role);

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
