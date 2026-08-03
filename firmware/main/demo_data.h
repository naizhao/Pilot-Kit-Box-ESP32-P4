/*
 * demo_data.h — 演示模式的合成飞行数据（**固件与模拟器共用同一份**）。
 *
 * 为什么放在 firmware/main 而不是 sim/compat
 * ------------------------------------------
 * 这批目标集是长期积累出来的：迎头 / 方位扎堆 / 同向尾随 / 边界与降级 / 后方
 * 五组，每一组都是为了压住一类曾经出过问题的渲染路径（见下面 SET[] 的注释）。
 * 它原先只活在 sim/compat/mock_runtime.c 里、不进固件编译，于是真机上的"演示
 * 模式"要么另写一份（两份数据必然走偏，模拟器上验过的版面真机上未必压得到），
 * 要么放弃这批场景。把它提到共用模块是唯一不产生第二份真源的做法。
 *
 * 代价很小：整张表 17 行 × 40 B ≈ 700 B rodata，加上合成代码不到 2 KB——
 * 相对 app 分区 1.4 MB 的余量可以忽略。
 *
 * 本模块的硬约束
 * --------------
 *   - **不依赖 ESP-IDF**：只用 stdint / string / math 和项目内部的数据结构头。
 *     模拟器要原样编译它，一旦引入 esp_timer / FreeRTOS 就编不过。
 *   - **无状态**：所有输出都是 now_us 的纯函数。演示模式可以随时开关，没有
 *     需要重置的内部积分状态；模拟器的 `--shot <秒>` 也才能定格到任意一帧。
 *
 * 谁在用
 * ------
 *   固件   各数据源 getter（pk_imu_sample_get / pk_gps_get / pk_baro_get /
 *          aircraft_state_snapshot）在演示模式开启时改调这里。
 *   模拟器 sim/compat/mock_runtime.c 与 sim/main.c 的动画。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aircraft_state.h"
#include "baro.h"
#include "gps.h"
#include "imu_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 本机状态：来自一条真实飞行轨迹 ───────────────────────────────
 *
 * 早先这几个量是三条周期互质的正弦曲线（0.37 / 0.23 / 0.11），本机位置则
 * **完全固定**。2026-08 全部换成 demo_track 的真实 GPX 回放，两条理由：
 *
 *   产品：飞行员的原话是「演示模式就是要移动才行」。合成曲线下高度带在
 *         ±1200 ft 里来回荡、罗盘匀速转圈，展台上一眼假。
 *   工程：以本机为中心的滚动窗口（pk_win.c）与它的让路规则 R1–R4，本机不动
 *         就**永远走不到让路分支**——窗口填满后再没有新格要加载。实测静置
 *         180 s，`pk_win: status` 的 yields 恒为 0。
 *
 * 姿态也一并取自轨迹：坡度由转弯率反算（协调转弯 tan φ = Vω/g），俯仰取
 * 航迹倾角。真实空域里这两个量与航向、高度是**相关**的，正弦曲线那种互相
 * 独立的抖动反而会掩盖"姿态和航向对不上"这类真 bug。
 *
 * 全部仍是 now_us 的纯函数，无状态约束不变（见文件头）。 */
float pk_demo_roll_deg(int64_t now_us);
float pk_demo_pitch_deg(int64_t now_us);
/* 真航迹角。轨迹里包含 SID / 航路转弯 / STAR / 进近，回放倍速下罗盘转得
 * 足够勤，HSI 与交通叠加"有没有跟着航向转"照样压得到。 */
float pk_demo_yaw_deg(int64_t now_us);
int   pk_demo_own_alt_ft(int64_t now_us);
int   pk_demo_own_gs_kt(int64_t now_us);
int   pk_demo_own_vs_fpm(int64_t now_us);

/*
 * 本机地理位置——**会动**。
 *
 * 加 t_us 形参而不是留个无参版本读全局：本模块的无状态约束不允许有"当前
 * 时刻"这种隐藏状态，而模拟器的 --shot 要按任意时刻定格，位置必须跟着
 * 动画时钟走而不是墙钟。
 *
 * 目标位置是按「本机位置 + 相对方位 + 距离」算出来的，所以本机一动，
 * pk_demo_traffic() 合成的假目标自动跟着走，不会被甩在后面（见该函数）。
 */
void pk_demo_own_pos(int64_t t_us, double *lat, double *lon);

/* ── 数据源快照（签名与真实 getter 一一对应）───────────────────── */
bool pk_demo_imu_sample(int64_t now_us, pk_imu_sample_t *out);
bool pk_demo_baro(int64_t now_us, pk_baro_state_t *out);
bool pk_demo_gps(int64_t now_us, pk_gps_state_t *out);

/*
 * 目标表快照。
 *
 * own_yaw_deg / own_alt_ft 由调用方给，而不是在这里直接调 pk_demo_yaw_deg()：
 * 模拟器有 PK_SIM_HDG 这类"把航向钉死"的开关，钉住的值必须同时作用到目标的
 * 方位换算上，否则屏上的航向和目标的分布对不上。固件那边直接传本模块自己的
 * 曲线值。
 *
 * now_us  写进 last_seen_us 的基准，**必须是调用方自己那个时钟**——消费方拿
 *         esp_timer_get_time() 减它算 SEEN 列，两个时钟不同源，age 会算成负数
 *         或几万秒。
 * anim_us 动画时间，决定目标漂移到哪儿了**以及本机此刻飞到哪儿了**——目标
 *         的绝对经纬度按本机位置加相对方位算，两者必须用同一个时钟，否则
 *         本机一起飞，目标就整片留在广州。固件里它等于 now_us；模拟器里这
 *         两个**不是**一回事：那边 esp_timer 桩返回的是进程启动至今的真实墙钟，
 *         而动画时间由 `--shot <秒>` 指定，用墙钟的话同一条命令每次跑出来的图
 *         都不一样，回归基线也就没了。
 * extra_dist_nm  统一叠加到每个目标的距离上。模拟器的 PK_SIM_TFC_FAR 用它把
 *                目标整体推到量程外（"有数据但一架都画不出"那一态）；固件传 0。
 * bare           只保留位置，呼号 / 高度 / 速度全部置无——真实空域里只发
 *                DF17 位置报文的老应答机相当常见，各列要退化成 ---。固件传 false。
 */
size_t pk_demo_traffic(aircraft_t *out, size_t cap,
                       int64_t now_us, int64_t anim_us,
                       float own_yaw_deg, int own_alt_ft,
                       float extra_dist_nm, bool bare);

#ifdef __cplusplus
}
#endif
