/* traffic_geom.h — 纯几何：本机/目标标量 → 相对方位/距离/相对高度。
 *
 * 无 I/O、无 aircraft_t 依赖（标量入参），便于 plain-`cc` host 单测。
 * aircraft_t → 标量的拆解由调用方(traffic_page / pfd_hsi_traffic)就地完成。
 * 全程磁北系：目标真北方位先减磁偏角降到磁系，再减本机磁航向得相对方位。
 */
#pragma once

#include <stdbool.h>
#include <limits.h>

#define PK_ALT_UNAVAIL INT_MIN

typedef struct {
    bool  valid;         /* own_has_pos && tgt_has_pos */
    float rel_bearing;   /* -180..180, 右为正 (heading-up 投影用) */
    float abs_bearing;   /* 0..360 磁方位 (north-up 投影用, N=磁北) */
    float dist_nm;
    bool  rel_alt_valid; /* own_press_alt != UNAVAIL && tgt_has_alt */
    int   rel_alt_ft;    /* 负=低于本机 */
    int   vs_fpm;        /* 目标升降率 */
} pk_traffic_rel_t;

/* own_heading_mag_deg: 本机磁航向(IMU yaw); mag_var_deg: 磁偏角(东+);
 * own_press_alt_ft: 本机标准气压高度(1013.25 参考)，PK_ALT_UNAVAIL=不可用 */
pk_traffic_rel_t pk_traffic_rel_calc(
    bool own_has_pos, double own_lat, double own_lon,
    float own_heading_mag_deg, float mag_var_deg, int own_press_alt_ft,
    bool tgt_has_pos, double tgt_lat, double tgt_lon,
    bool tgt_has_alt, int tgt_alt_ft, int tgt_vs_fpm);
