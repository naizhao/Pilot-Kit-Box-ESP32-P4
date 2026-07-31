/*
 * adsb_list.h — ADS-B aircraft list view for the logical 800×480 UI.
 *
 * The current landscape layout provides sortable columns, touch scrolling,
 * and a per-aircraft detail drawer. Navigation is handled by the shared
 * FAB/dock layer.
 * Refresh model: re-rendered every PFD-task frame when the UI mode is
 * PK_UI_MODE_ADSB_LIST. Cheap to redraw — same fill_rect + bitmap font
 * primitives the PFD already uses. No internal caching.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Renders the full ADS-B list view into an 800 × 480 RGB565 framebuffer.
 * Assumes the caller hands ownership of the framebuffer for the
 * duration of the call (i.e. flushes after). Pulls the aircraft
 * snapshot internally via aircraft_state_snapshot(). */
void pk_adsb_list_render(uint16_t *fb);

/*
 * 表头点击 → 切换排序列 / 翻转排序方向。
 *
 * 返回 true 表示这一下被表头消费了，调用方不应再转给别的控件（同
 * pk_traffic_page_touch 的约定）。坐标是逻辑屏坐标，与 framebuffer 同一套。
 */
bool pk_adsb_list_touch(int x, int y);

/*
 * 按住不放的后续帧：竖直位移换算成列表滚动。返回 true 表示本次触摸仍由
 * 列表消费。按下时 pk_adsb_list_touch() 返回过 true 才需要继续调用。
 */
bool pk_adsb_list_drag(int x, int y);

/* 取消本次触摸：只丢状态，不执行动作。dock 展开等需要让路的场合用它，
 * 不能用 touch_up——那会把之前的按下当成一次完整点击提交出去。 */
void pk_adsb_list_touch_cancel(void);

/* 松手：清掉按下高亮，并在「没拖动过」时才把这一下当作点击执行。 */
void pk_adsb_list_touch_up(void);
