/*
 * adsb_drawer.h — 每机详情抽屉（ADS-B 列表页与地图页共用）。
 *
 * 2026-10-03 从 adsb_list.c 平移而来：此前"地图上点飞机"跳到列表页去看抽屉
 * （并因此踩出导航卡死，见 map_page.c 的 touch_up 注释），产品形态改为**两页
 * 各自原地开抽屉**。抽屉的版面、绑定动作、开关语义只有这一份实现，两页共用
 * ——列表改了地图跟着变，不存在"两份实现走偏"的缝隙。
 *
 * 本模块只依赖：标准库 + 项目内头 + ui_state（绑定动作）。像素渲染依赖
 * AA 字体与 i18n，host 单测经 PK_ADSB_DRAWER_HOST_TEST 切掉（同
 * apt_detail_page.c 的惯例），可移植面（几何 / 徽章 / toggle / 绑定）照常编译。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "aircraft_state.h"   /* aircraft_t —— render 的数据参数 */
#include "display.h"          /* PK_DISPLAY_W/H —— 版面宏的唯一来源 */

#ifdef __cplusplus
extern "C" {
#endif

/* ── 版面（自列表 spec §5.3 平移，一个数都没动） ───────────────────
 * 抽屉贴底，高 230；开着时列表数据区只画得下 DRAWER_ROWS 行，地图页则
 * 整块盖住底部（含 footer 与左右两列地图按钮——那两列 y≥266，全在
 * 抽屉底下，抽屉开着时既不画也不可点）。 */
#define PK_ADSB_DRAWER_H        230
#define PK_ADSB_DRAWER_TOP      (PK_DISPLAY_H - PK_ADSB_DRAWER_H)   /* 250 */

/* 底部「绑定为本机」按钮。命中区在按钮之外放宽（x ±16、y 上 12 下到屏底）
 * ——手指比按钮胖，apt_detail_page.c 的 HIT 范式。半开区间。 */
#define PK_ADSB_DRAWER_BIND_W     240
#define PK_ADSB_DRAWER_BIND_H      30
#define PK_ADSB_DRAWER_BIND_Y0     (PK_ADSB_DRAWER_TOP + 198)       /* 448 */
#define PK_ADSB_DRAWER_BIND_X0     ((PK_DISPLAY_W - PK_ADSB_DRAWER_BIND_W) / 2)

/* 画出整个抽屉：呼号标题、紧急徽章、2×4 键值矩阵、绑定按钮。
 * age_s 由调用方按自己的快照算（列表来自行数据，地图来自 last_seen_us），
 * 两边口径一致：距上次报文的整秒数，钳到 0..99。
 * 绑定按钮的蓝/绿态按"当前 own == ac->icao24"现查，与本模块自身状态无关。 */
void pk_adsb_drawer_render(uint16_t *fb, const aircraft_t *ac, int age_s);

/* (x,y) 是否落在绑定按钮的（放宽后）命中区。 */
bool pk_adsb_drawer_bind_hit(int x, int y);

/*
 * 抽屉目标的 toggle 语义，两页共用同一判定：
 *   cur=0（关着）        → 开 tapped
 *   cur==tapped（讲的就是它）→ 收起（0）
 *   cur!=tapped（讲的是别架）→ 换讲 tapped
 *   tapped=0（点空处）   → 收起
 */
uint32_t pk_adsb_drawer_tap_target(uint32_t cur, uint32_t tapped);

/*
 * 绑定按钮的动作（从列表 touch_up 原样平移，一个字没改）：
 *   目标已是本机 → 解绑（own 显式清 0 + toast，不跳页）
 *   否则          → 绑定（落 own + toast + 跳 PFD 看效果）
 * 跳 PFD 是列表时代的产品决策（绑定后最想看的是本机姿态），地图页复用
 * 同一动作，行为不分叉。
 */
void pk_adsb_drawer_bind_activate(uint32_t icao24);

/* 紧急码徽章：7500→HJK / 7600→RDO / 7700→EMG，其余（含未广播）NULL。
 * 列表行内徽章与抽屉标题徽章同源。 */
const char *pk_adsb_emergency_tag(const aircraft_t *a);

#ifdef __cplusplus
}
#endif
