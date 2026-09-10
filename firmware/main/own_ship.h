#pragma once
#include <stdint.h>
#include "aircraft_state.h"

typedef enum {
    PK_OWN_SRC_NONE = 0,
    PK_OWN_SRC_BOUND_ADSB,   /* user manually bound an ADS-B aircraft */
    PK_OWN_SRC_GPS,          /* fallback: GPS fix */
} pk_own_src_t;

/* 本机航向(HDG)来源。pk_own_heading_resolve() 输出，消费方据此决定是否做
 * 磁偏修正：ADS-B / GPS track 是真北参考，IMU 是磁北参考。 */
typedef enum {
    PK_HDG_SRC_NONE = 0,
    PK_HDG_SRC_ADSB,         /* 绑定飞机 ADS-B 地面航迹(真北) */
    PK_HDG_SRC_IMU,          /* IMU 磁航向 yaw(磁北, 机头朝向) */
    PK_HDG_SRC_GPS,          /* GPS track 兜底(真北, 地速方向) */
} pk_hdg_src_t;

/*
 * 本机的三种高度——互不可换，所以各带各的有效位，不许挤进同一个字段。
 *
 *   press_alt_ft      气压高度（1013.25 标准基准）。ADS-B 目标的 Mode-C/DF17
 *                     高度、GDL90 0x0A/0x14 的高度字段用的都是它，也是唯一
 *                     能与目标相减得出相对高度的量。**只有绑定 ADS-B 本机时
 *                     才有**：盒子自己的 BMP388 装在座舱里，增压座舱内那个数
 *                     恒等于座舱高度（约 8000 ft），拿它当基准会把巡航
 *                     FL350 的同高度迎头目标算成 +27000 ft 并抑制告警。
 *   gnss_msl_ft       GNSS 正高（MSL），GGA 第 9 字段。与气压高度差着当地
 *                     气压偏差，非标准日下地面就能差上千英尺。
 *   gnss_ellipsoid_ft GNSS 椭球高（HAE），ADS-B TC20-22。与 MSL 差一个大地
 *                     水准面起伏。
 *
 * 三者都没有时全为 false —— 那是"不知道本机多高"，不是"本机在海平面"。
 */
typedef struct {
    bool have_press_alt;
    int  press_alt_ft;
    bool have_gnss_msl;
    int  gnss_msl_ft;
    bool have_gnss_ellipsoid;
    int  gnss_ellipsoid_ft;
} pk_own_alt_t;

/* Resolve the effective own-ship.
   Priority: manual ADS-B binding ALWAYS wins; else GPS fix; else none.
   Fills *out and (if non-NULL) *src. Returns true if a usable own-ship exists.

   注意 out->altitude_ft 的定义是**气压高度**：GPS 兜底那一路给不出气压高度，
   于是 out->have_altitude 恒为 false。需要 GPS 的 MSL 请用
   pk_own_ship_resolve_ex() 取 pk_own_alt_t。 */
bool pk_own_ship_resolve(int64_t now_us, int64_t max_age_us,
                         aircraft_t *out, pk_own_src_t *src);

/* 同上，另外把三种高度基准分开写进 *alt（可为 NULL）。
 * pk_own_ship_resolve() 就是它 alt=NULL 的薄封装，两者判据完全同源。 */
bool pk_own_ship_resolve_ex(int64_t now_us, int64_t max_age_us,
                            aircraft_t *out, pk_own_src_t *src,
                            pk_own_alt_t *alt);

/*
 * 该不该把这架本机编成 GDL90 Ownship Report(0x0A) 发出去。
 *
 * 判据是**空地状态已知**：GDL90 的 Misc bit3 "Airborne" 是正向断言，协议里
 * 没有"未知"编码——不知道而照发，线上表现就是 surface，多数 EFB 会据此抑制
 * 本机周边的交通告警。宁可不发（EFB 显示"无本机位置"，用户会去看别的信息
 * 源），也不能发一个宣称自己在地面的报文。
 *
 * 做成可测的生产函数而不是 BLE 大任务里的一个 if：那个任务依赖 NimBLE，
 * 没有 host 测试缝，规则写在里面就等于没有守卫。
 */
bool pk_own_gdl90_should_emit(bool own_valid, const aircraft_t *own);

/* 统一解析本机有效机头朝向(HDG)的来源 —— PFD / traffic / list 共用，
 * 取代各处重复的内联优先级。4 级优先级：
 *   1. 绑定 ADS-B(且有速度) → own->heading_deg(飞机自报航迹，最准)
 *   2. IMU 有效              → imu_yaw_deg(磁航向 = 机头朝向)
 *   3. GPS 兜底且地速≥2kt    → own->heading_deg(GPS track；静止时是噪声故设门槛)
 *   4. 都没有                → 无航向
 * own/own_valid/own_src 来自 pk_own_ship_resolve；imu_valid/imu_yaw_deg 来自
 * pk_imu_sample_get(无 IMU 传 false/0)。写 *out_deg(按来源基准：ADS-B/GPS 真北、
 * IMU 磁北) 与(可选)*out_src，返回 true 表示有有效航向。 */
bool pk_own_heading_resolve(bool own_valid, pk_own_src_t own_src,
                            const aircraft_t *own,
                            bool imu_valid, float imu_yaw_deg,
                            float *out_deg, pk_hdg_src_t *out_src);
