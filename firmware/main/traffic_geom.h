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
    bool  vs_valid;      /* 目标报过垂速吗——0 fpm(平飞) 与"没数据"是两回事 */
    int   vs_fpm;        /* 目标升降率；vs_valid=false 时恒 0，不得据此画箭头 */
} pk_traffic_rel_t;

/*
 * 相对高度的本机基准 —— 降级链，判据全部收在这里（三页共用，谁也别再就地
 * 写一遍 if-else）：
 *
 *   1. 绑定 ADS-B 本机且它报过气压高度 → 用它。目标高度是 Mode-C/DF17 的
 *      气压高度（1013.25 基准），绑定机自报的高度与之同源同基准，最优。
 *   2. 舱内 BMP388 与 GNSS **互相印证**（QNH 修正高度 vs GNSS 正高，差异
 *      ≤ PK_OWN_ALT_XCHECK_MAX_DIFF_FT）→ 用 BMP388 静压换算的标准气压
 *      高度（1013.25 基准，与目标严格同基准、零残差）。
 *   3. 印证失败 / BMP388 缺失 → **GNSS MSL 兜底**。
 *
 * 两个关键设计：
 *   - **印证量是 QNH 修正高度（baro.alt_ft），不是静压换算的标准气压高度**。
 *     后者与 GNSS 之差是「当地 QNH 偏差 + 非标准温度累积」，随高度增长
 *     （ISA+15 暖柱巡航即差 1700+ ft），拿它印证会把巡航恒判失准、转 GNSS
 *     兜底后残留同量级偏差，±1000 ft 威胁分档整体翻转。而 QNH 修正高度与
 *     GNSS 同为「相对海平面」，差值只反映传感器健康度，与飞行高度无关：
 *     正常 <150 ft，增压座舱（舱压 8000 ft vs 真实 35000 ft）/传感器故障
 *     ≥数千 ft。差异巨大正是增压环境的指纹——盲信舱压曾把同高度迎头目标
 *     算成 +27000 ft、告警被抑制（spec §4.2「气压高度在增压环境不可信」）。
 *   - **印证通过后输出的仍是静压换算的 1013.25 气压高度**，与目标 Mode-C
 *     同基准相减零残差。兜底路径的基准偏差只有气压高度误差的量级（数百到
 *     两千 ft），已是没有任何气压高度可用时最好的近似，威胁判断不会被带偏
 *     一个数量级。
 *
 * 显示 N/A 让人去看别的信息源；显示一个错的数不会。而「有印证就用气压计、
 * 印证不上就用 GNSS」既不盲信也不空转，本机高度在绝大多数场景下都有数。
 *
 * 纯函数，无 I/O：三页（交通页 / ADS-B 列表 / PFD HSI 叠加层）共用。
 */
#define PK_OWN_ALT_XCHECK_MAX_DIFF_FT 500   /* 印证窗口：正常差异 <150 ft，
                                             * 增压舱/故障 ≥ 数千 ft，500 居中 */
int pk_traffic_own_press_alt(bool own_bound_adsb,
                             bool own_has_press_alt,
                             int  own_press_alt_ft,
                             bool baro_valid, float baro_pressure_pa,
                             int  baro_qnh_alt_ft,
                             bool gnss_has_msl, int gnss_msl_ft);

/* own_heading_mag_deg: 本机磁航向(IMU yaw); mag_var_deg: 磁偏角(东+);
 * own_press_alt_ft: 本机标准气压高度(1013.25 参考)，PK_ALT_UNAVAIL=不可用。
 * 该实参一律取自 pk_traffic_own_press_alt()，不要就地拼。 */
pk_traffic_rel_t pk_traffic_rel_calc(
    bool own_has_pos, double own_lat, double own_lon,
    float own_heading_mag_deg, float mag_var_deg, int own_press_alt_ft,
    bool tgt_has_pos, double tgt_lat, double tgt_lon,
    bool tgt_has_alt, int tgt_alt_ft,
    bool tgt_has_vs,  int tgt_vs_fpm);

/*
 * 目标剪影该朝哪儿——屏幕系方向角，0=正上、顺时针为正。
 *
 * 为什么必须和 pk_traffic_rel_calc 放在一起：两者共用同一个「地图参考北」的
 * 约定。rel_calc 把目标真方位减去 mag_var 得到 abs_bearing，也就是说这幅图的
 * 正上方（north-up 下 abs_bearing=0）指的是**真北旋转 -mag_var 之后**的方向；
 * 目标航迹是真北参考的，不减同一个 mag_var 就会和自己所在的那张图差一个磁偏角。
 * 分散在两个渲染文件里各推一遍的结果已经见过了：交通页与 PFD 罗盘上的同一架
 * 飞机机头差了 74.6°。
 *
 * heading_up=true 时整幅图已经跟着本机转过了，还要再减本机航向；这一步正是
 * 防撞语义本身——迎面来的机头朝下，同向飞的机头朝上。
 *
 * 单位口径：tgt_track_true_deg 取 ADS-B 地面航迹（真北）；own_heading_deg 与
 * mag_var_deg 必须与调用 rel_calc 时传的那一对完全相同（IMU 磁航向配查表磁
 * 偏角，或 ADS-B/GPS 真航迹配 mag_var=0），否则符号与落点分属两套参考系。
 */
float pk_traffic_symbol_rot_deg(bool heading_up, float tgt_track_true_deg,
                                float mag_var_deg, float own_heading_deg);

/*
 * 相对方位 → 八向箭头（UTF-8 字面量，↑ 为正前）。
 *
 * 为什么放在这里而不是各页面自己写一份：交通页、ADS-B 列表页、搜索页在屏上
 * 画的是同一个语义——「那个东西在本机的哪个方向」。它曾经是两份一模一样的
 * static 表（traffic_page.c / adsb_list.c），只要有人改了其中一份，同一个方位
 * 在两页就会长得不一样，而用户会把"长得不一样"读成"含义不一样"。
 *
 * 字形不是随手挑的 Unicode：这 8 个码位在 gen_pfd_aa_font.py 的 ARROW_CODES
 * 里显式烘进了 pk_aa 字库的 xs/s/m/l 四档（见 pfd_aa_font.h 的
 * pk_aa_cjk_codes），不走 i18n catalog 子集，所以直接写字面量是安全的。
 *
 * 返回的是静态常量串，调用方不得修改；宽度按 CJK 全角算（PK_AA_*_CJK_W）。
 */
const char *pk_bearing_arrow(float rel_deg);
