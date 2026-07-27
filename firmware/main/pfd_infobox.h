/*
 * pfd_infobox.h — PFD 底部两块附加信息：右下三个数值框 + 左下本机来源徽标。
 *
 * 为什么从 pfd.c 里抽出来
 * ----------------------
 * 这两块原先是内联在 pfd_task 主循环里的一大段绘制代码，坐标（256 / 170 /
 * 318 / 88 …）全是照 320×240 手算的绝对值。既没法随分辨率走，也没法进
 * 模拟器 —— 而它们恰恰是布局评审时最需要看到的部分。
 *
 * 抽出来的边界刻意划在「数据 vs 画法」上：**选哪个字符串**（呼号 → squawk
 * → ICAO hex 的降级链）依赖 aircraft_t，留在 pfd.c；本模块只吃已经定好的
 * 字符串与颜色语义，因此不依赖任何运行时状态，可直接编进模拟器。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ── 右下三框：气压高度 / 权威高度的米值 / 升降率 ── */
typedef struct {
    bool baro_valid;
    int  baro_alt_ft;       /* 气压高度，ft */

    bool alt_valid;
    int  alt_ft;            /* 权威高度（ADS-B），模块内换算成米显示 */

    bool vs_valid;
    int  vs_fpm;            /* 升降率 */
    /* VS 有两个来源：ADS-B 自报（权威，白色）与 baro 微分（参考，琥珀）。
     * 颜色差异是给飞行员的可信度提示，不能合并。 */
    bool vs_from_adsb;
} pk_pfd_infobox_t;

void pk_pfd_infobox_render(uint16_t *fb, const pk_pfd_infobox_t *d);

/* ── 左下徽标：本机数据从哪来 ── */
typedef enum {
    PK_PFD_SRC_NONE = 0,    /* 无本机数据      → 灰 */
    PK_PFD_SRC_ADSB,        /* 绑定的 ADS-B 机 → 青 */
    PK_PFD_SRC_GPS,         /* GPS 兜底        → 白 */
} pk_pfd_src_t;

typedef struct {
    pk_pfd_src_t src;
    char         label[12]; /* 呼号 / squawk / ICAO hex / "GPS" / "--" */
    /* 绑定丢失后的 5 秒告警。判定（含跳变检测与计时）留在 pfd.c，
     * 这里只负责把它画成闪烁红字。 */
    bool         adsb_lost_alert;
    bool         alert_blink_on;
} pk_pfd_srcbadge_t;

void pk_pfd_srcbadge_render(uint16_t *fb, const pk_pfd_srcbadge_t *d);
