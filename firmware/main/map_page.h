/*
 * map_page.h — SD 离线地图页（PK_UI_MODE_MAP）。
 *
 * north-up、本机居中跟随，PMTiles 栅格底图 + ADS-B 目标叠加。设计依据
 * docs/superpowers/specs/2026-08-01-sd-offline-map-design.md。三函数模式照
 * traffic_page.h：render(fb) 每帧画整页；touch(x,y) 处理按下（含单指拖动
 * 平移的起手/续行——map 页没有独立的 drag() 入口，拖动的每一帧都重复调
 * touch()，由内部状态区分"这是新按下"还是"接着上一次拖"）；touch_up() 松手。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void pk_map_page_render(uint16_t *fb);

/*
 * 触摸：按钮命中（+/− 缩放、回中）与拖动平移共用一个入口。
 *
 * 返回 true 表示这一下被地图页消费，调用方不应再转给别的控件。坐标是
 * 逻辑屏坐标，与 framebuffer 同一套。touch_gt911.c 需要在按下的**每一帧**
 * 都调用它（不只是按下的第一帧）才能连续拖动——这是与 pk_traffic_page_touch
 * 那种"只在按下瞬间触发一次"的按钮语义唯一的不同点，函数签名本身不变。
 */
bool pk_map_page_touch(int x, int y);

/* 松手。清掉按下高亮、结束本次拖动手势。 */
void pk_map_page_touch_up(void);

/*
 * 把视口挪到指定经纬度并定住（搜索页点结果的落点）。
 *
 * 副作用有三个，缺一不可：设 center/zoom、**关掉跟随**（s_follow=false，
 * 否则下一帧本机位置一到就把视口拽回去了）、通知瓦片加载器与航空叠加层
 * 视图变了（不通知的话，屏上会先干瞪几秒旧瓦片）。
 * zoom 会被夹到 [MAP_ZOOM_MIN, MAP_ZOOM_MAX]。
 */
void pk_map_page_goto(double lat, double lon, int zoom);

/*
 * 搜索结果的 PIN。画在最上层（ADS-B 目标与本机符号之上、UI 铬层之下），
 * 形状与配色都与既有符号刻意区分：航空叠加层的机场/导航台/FIX 是蓝/绿/紫，
 * ADS-B 是青，本机是白——PIN 取琥珀 + 白描边，且是唯一一个"带尖脚落地"的
 * 形状，余光扫过就知道那是刚查到的那个点。
 *
 * label 是要显示在 PIN 上方的短串（一般是 ICAO/ident），会被拷贝；传 NULL
 * 或空串就只画符号。clear 之后不再画。
 */
void pk_map_page_set_pin(double lat, double lon, const char *label);
void pk_map_page_clear_pin(void);

/* 打开搜索页的入口在本页右侧那一列按钮上；这个回调由 map_page 调出去，
 * 宿主接到就 pk_search_page_open()。做成弱符号（同 pk_ui_nav 的做法）是
 * 为了让模拟器与 host 单测不必把整个搜索页链进来。 */
void pk_map_page_on_search(void);

/* 点中了地图上的机场符号，宿主接到就 pk_apt_detail_page_open()。
 * apt_idx = 机场段内记录下标（由 pk_aero_layer 的命中测试给出）。
 * 弱符号，理由同上。 */
void pk_map_page_on_apt_detail(uint32_t apt_idx);

/*
 * 收起的搜索/详情 sheet —— 本页左上角那枚返回钮（2026-08-04）。
 *
 * 语义见 apt_detail_page.h 的 pk_sheet_state_t：点搜索结果跳到地图之后，
 * 搜索页是"收起"而不是"关掉"，用户看一眼发现不是要找的那个，点这枚钮就能
 * 原样回到结果列表接着挑下一条。
 *
 * 两个都是弱符号，理由同上面两个：模拟器与 host 单测不必把整个模态栈链进来。
 * 固件侧的强符号在 pk_ui_nav_host.c，转给 pk_ui_sheet_has_collapsed /
 * pk_ui_sheet_restore。
 *
 * has_collapsed() 每帧都会被 render 与 touch 各问一次，必须是廉价的纯查询
 * ——它只是读两个页面的可见性枚举。
 */
bool pk_map_page_sheet_collapsed(void);
void pk_map_page_on_sheet_restore(void);

#ifdef PK_SIM_BUILD
/* 截图用：按一下那枚返回钮，走**真机同一条** touch()+touch_up()。落点由
 * map_page.c 自己按当前版面（含 FAB 避让）算，不在 sim/main.c 那边照抄坐标
 * ——抄一份就会在 FAB 挪过去之后静静点空。 */
void pk_map_page_sim_tap_sheet_back(void);
#endif
