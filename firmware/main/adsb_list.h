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

/* 取消本次触摸：只丢状态，不执行动作。不能用 touch_up 代替——那会把之前的
 * 按下当成一次完整点击提交出去。
 * 当前唯一调用方是模拟器 sim/main.c 的 FAB 放行自检（详见 .c 里的注释）。 */
void pk_adsb_list_touch_cancel(void);

/* 松手：清掉按下高亮，并在「没拖动过」时才把这一下当作点击执行。 */
void pk_adsb_list_touch_up(void);

/*
 * 展开指定目标的详情抽屉（地图页点飞机跳转过来的入口）。
 *
 * 只设抽屉状态不切页：目标在下一帧 render() 里被钉进可见区并画出详情
 * （render 的 drawer_row 逻辑本来就是"抽屉讲的就是它，让它滚出屏幕等于
 * 把上下文丢了"）。切页归导航宿主——与机场详情的弱符号分层一致，本页
 * 不知道是谁、从哪一页跳来的。目标不在快照里（过期/超出窗口）时抽屉
 * 会被 render 的常规清理路径自然收掉，调用方无需预检。
 */
void pk_adsb_list_focus_icao(uint32_t icao24);
