/* traffic_geom.c — 纯几何核心。只依赖 geo.h + <math.h>，host 可测。 */
#include "traffic_geom.h"

#include "geo.h"
#include <math.h>
#include <stdbool.h>

static float norm360(float a){ a = fmodf(a, 360.0f); if(a < 0) a += 360.0f; return a; }
static float norm180(float a){ a = norm360(a); return a > 180.0f ? a - 360.0f : a; }

pk_traffic_rel_t pk_traffic_rel_calc(
    bool own_has_pos, double own_lat, double own_lon,
    float own_heading_mag_deg, float mag_var_deg, int own_press_alt_ft,
    bool tgt_has_pos, double tgt_lat, double tgt_lon,
    bool tgt_has_alt, int tgt_alt_ft, int tgt_vs_fpm)
{
    pk_traffic_rel_t r = (pk_traffic_rel_t){0};
    r.vs_fpm = tgt_vs_fpm;
    if(!own_has_pos || !tgt_has_pos){ r.valid = false; return r; }

    double d, brg_true;
    geo_dist_brg(own_lat, own_lon, tgt_lat, tgt_lon, &d, &brg_true);

    /* 真北方位减磁偏角 → 磁方位（与本机磁航向同系）。东偏 mag_var 为正。 */
    float brg_mag = norm360((float)brg_true - mag_var_deg);

    r.valid       = true;
    r.dist_nm     = (float)d;
    r.abs_bearing = brg_mag;
    r.rel_bearing = norm180(brg_mag - own_heading_mag_deg);

    if(own_press_alt_ft != PK_ALT_UNAVAIL && tgt_has_alt){
        r.rel_alt_valid = true;
        r.rel_alt_ft    = tgt_alt_ft - own_press_alt_ft;
    }
    return r;
}

float pk_traffic_symbol_rot_deg(bool heading_up, float tgt_track_true_deg,
                                float mag_var_deg, float own_heading_deg)
{
    /* 真北 → 地图参考北：与 abs_bearing 减的是同一个 mag_var。 */
    float rot = tgt_track_true_deg - mag_var_deg;
    /* 机头朝上：图已经转过 own_heading，符号得跟着一起转回来。 */
    if(heading_up) rot -= own_heading_deg;
    return norm360(rot);
}

const char *pk_bearing_arrow(float rel_deg)
{
    /* 顺序 = 从正前开始顺时针每 45° 一格，右边注掉方位名是为了改表时能一眼
     * 核对——这张表错一格，屏上所有方向就整体偏 45°，而画面本身看不出毛病。 */
    static const char *kArrow[8] = {
        "↑", "↗", "→", "↘",   /* 正前 右前 正右 右后 */
        "↓", "↙", "←", "↖",   /* 正后 左后 正左 左前 */
    };
    /* 22.5° 是半个扇区：加上它再整除 45，等价于「四舍五入到最近的八方位」。 */
    return kArrow[((int)((norm360(rel_deg) + 22.5f) / 45.0f)) & 7];
}
