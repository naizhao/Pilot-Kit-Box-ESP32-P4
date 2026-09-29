/* traffic_geom.c — 纯几何核心。只依赖 geo.h + <math.h>，host 可测。 */
#include "traffic_geom.h"

#include "geo.h"
#include <math.h>
#include <stdbool.h>

static float norm360(float a){ a = fmodf(a, 360.0f); if(a < 0) a += 360.0f; return a; }
static float norm180(float a){ a = norm360(a); return a > 180.0f ? a - 360.0f : a; }

int pk_traffic_own_press_alt(bool own_bound_adsb,
                             bool own_has_press_alt,
                             int  own_press_alt_ft,
                             bool baro_valid, float baro_pressure_pa,
                             int  baro_qnh_alt_ft,
                             bool gnss_has_msl, int gnss_msl_ft)
{
    /* 1. 绑定机自报的气压高度与目标 Mode-C 同源同基准，永远优先。 */
    if(own_bound_adsb && own_has_press_alt) return own_press_alt_ft;

    /* 守卫 p<=0：powf 底数为负会产生 NaN。 */
    if(baro_valid && baro_pressure_pa > 0.0f && gnss_has_msl){
        /* 印证量：QNH 修正高度（≈MSL 口径）vs GNSS 正高。同为「相对海平面」，
         * 差值与「静压换算的标准气压高度 vs GNSS」之差（QNH+温度累积，巡航
         * 1~2 kft 级）相比小得多。窗口语义随 QNH 模式而变（AUTO=压力源粗判、
         * MANUAL=温度+QNH 误差之和），但增压舱指纹（≥12 kft）在两种模式下
         * 都远超窗口——拦的都是同一种事，见 traffic_geom.h。 */
        const int diff = baro_qnh_alt_ft - gnss_msl_ft;
        if(diff <= PK_OWN_ALT_XCHECK_MAX_DIFF_FT &&
           diff >= -PK_OWN_ALT_XCHECK_MAX_DIFF_FT){
            /* 2. 印证通过：输出静压换算的 1013.25 标准气压高度——与目标
             * Mode-C 同基准相减零残差，这是兜底链里精度最高的一条。 */
            const float alt_m = 44330.0f *
                (1.0f - powf(baro_pressure_pa / 101325.0f, 0.190295f));
            return (int)(alt_m * 3.28084f);
        }
        /* 3. 差异巨大 = 座舱指纹/传感器故障：GNSS 兜底（真实高度，基准偏差
         * 只剩气压高度误差的量级）。 */
        return gnss_msl_ft;
    }

    /* 3. BMP388 缺失但 GNSS 在：GNSS 兜底（用户口径：GNSS 高度永远是兜底）。 */
    if(gnss_has_msl) return gnss_msl_ft;

    /* 4. 只剩一个无法自证的气压计（或全无）：N/A。
     * 独立气压计盲用正是增压舱老缺陷的成因——没有 GNSS 印证就分不清它读的
     * 是外界静压还是座舱高度。 */
    return PK_ALT_UNAVAIL;
}

pk_traffic_rel_t pk_traffic_rel_calc(
    bool own_has_pos, double own_lat, double own_lon,
    float own_heading_mag_deg, float mag_var_deg, int own_press_alt_ft,
    bool tgt_has_pos, double tgt_lat, double tgt_lon,
    bool tgt_has_alt, int tgt_alt_ft,
    bool tgt_has_vs,  int tgt_vs_fpm)
{
    pk_traffic_rel_t r = (pk_traffic_rel_t){0};
    /* 没报过垂速就是没报过：把 0 fpm 交出去，渲染层分不出"平飞"和"不知道"。 */
    r.vs_valid = tgt_has_vs;
    r.vs_fpm   = tgt_has_vs ? tgt_vs_fpm : 0;
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
