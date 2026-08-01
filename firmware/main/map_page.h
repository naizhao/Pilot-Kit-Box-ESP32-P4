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
