/*
 * pfd_infobox.h — PFD 底部左右两块三行信息框。
 *
 * 左：地速的 km/h 换算 + 本机数据来源
 * 右：气压高度 + 米制高度 + 升降率
 *
 * 两块同宽、**同底边**，坐在半圆罗盘的两侧，读起来是一对。样式完全一致：
 * 半透明底、左侧标签、右对齐数值——差别只在内容，所以共用同一个行渲染器。
 * 左块比右块少一行（原 mph 那行已删，见 pfd_infobox.c），空出来的一行留在
 * 顶上，底边仍然齐平。
 *
 * 为什么从 pfd.c / pfd_speed_tape.c 里抽出来
 * ------------------------------------------
 * 右块原先内联在 pfd_task 主循环里，坐标（256 / 170 / 318 …）全是照 320×240
 * 手算的绝对值；左块的换算区则长在速度带下方，只有 40 px 高、100 px 宽，被迫
 * 用 6 px 位图字。两处都既不随分辨率走，也进不了模拟器——而它们恰恰是布局
 * 评审时最需要看到的部分。
 *
 * 抽出来的边界刻意划在「数据 vs 画法」上：**选哪个字符串**（呼号 → squawk
 * → ICAO hex 的降级链）依赖 aircraft_t，留在 pfd.c；本模块只吃已经定好的
 * 字符串与颜色语义，因此不依赖任何运行时状态，可直接编进模拟器。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ── 本机数据来源 ── */
typedef enum {
    PK_PFD_SRC_NONE = 0,    /* 无本机数据      → 灰 */
    PK_PFD_SRC_ADSB,        /* 绑定的 ADS-B 机 → 青 */
    PK_PFD_SRC_GPS,         /* GPS 兜底        → 白 */
} pk_pfd_src_t;

/* ── 左块：速度换算 + 本机来源 ── */
typedef struct {
    bool         speed_valid;
    int          kmh;
    /* 没有 mph：英里/小时在中国空域零使用场景，那一整行已删。字段一并去掉，
     * 留着一个没人读的换算值只会让下一个人以为它还在屏上。 */

    pk_pfd_src_t src;
    char         label[12]; /* 呼号 / squawk / ICAO hex / "GPS" / "--" */
    /* 绑定丢失后的 5 秒告警。判定（含跳变检测与计时）留在 pfd.c，
     * 这里只负责把它画成闪烁红字，并让它占满整行——那是要抢注意力的。 */
    bool         adsb_lost_alert;
    bool         alert_blink_on;
} pk_pfd_leftbox_t;

void pk_pfd_leftbox_render(uint16_t *fb, const pk_pfd_leftbox_t *d);

/* ── 右块：气压高度 / 米制高度 / 升降率 ── */
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
