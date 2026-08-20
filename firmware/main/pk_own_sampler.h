/*
 * pk_own_sampler.h — 本机 1 Hz 航迹采样，own.trk 的生产者。
 *
 * 设计依据 ADS-B 数据持久化设计（内部文档）
 * 「own.trk」「相位标记」「写入管线」三节。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pk_flight_phase.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 幂等；须晚于 pk_rec_store_init()（own.trk 走同一个 session 目录）。
 * 不要求 GPS/IMU/baro 已就绪——采样器每 tick 各自 best-effort 取值，取不到
 * 就在记录里写无效标记，不等它们。也顺带向 pk_clock 注册校时回调，校时
 * 事件发生时同步落一条 own.trk 时间修正记录（rec_type=1）。
 */
void pk_own_sampler_start(void);

/* 写入/丢弃计数（诊断页用）。丢弃只发生在写任务被 SD I/O 卡住导致队列
 * 3 s 都排不进去的极端情况——正常运行下应恒为 0。返回 false 表示采样器
 * 还没启动。 */
bool pk_own_sampler_stats(uint32_t *out_written, uint32_t *out_dropped);

/*
 * 当前本机飞行相位——渲染层（地图页/交通页）拿它做"显著性跟随本机相位"
 * （阶段 4d）：本机在地面时空中目标压暗，本机在空中时地面目标压暗。
 *
 * 线程安全：单一写者（own_sample_task，1 Hz）写一个 volatile 的
 * pk_flight_phase_t（枚举底层是 int，4 字节对齐访问在本项目的两种目标架构
 * ——Xtensa 与 RISC-V——上都是原子的），任意多个渲染任务只读，不需要互斥锁。
 * 这与本文件同一份 .c 里 s_dropped/s_written 的既有惯例一致。
 *
 * 开机瞬间、采样任务还没跑过第一拍时返回 PK_PHASE_UNKNOWN——调用方必须把
 * 这当成"不压暗任何一侧"处理：状态机猜错方向就把该看的目标压暗，比不做
 * 这个功能更危险（IMU 没接/GPS 丢失/刚开机时同样会长期停在 unknown）。
 */
pk_flight_phase_t pk_own_sampler_get_phase(void);

/* ring 容量（2 的幂）。渲染层按下标掩码遍历，见 pk_own_sampler_get_trail()。 */
#define PK_OWN_TRAIL_CAP 2048u

/* 一个航迹采样点。存相位是为了渲染降采样区分间隔（飞行 15s/地面 60s）。 */
typedef struct {
    uint32_t ts_1k;   /* 1kHz tick（ms），32 位够 49 天 */
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint8_t  phase;   /* pk_flight_phase_t，降采样间隔判据 */
} pk_own_trail_point_t;

/* 返回本机航迹 ring 的只读视图：*out_count 写入有效点数，*out_start 写入最老
 * 点的下标（时序起点）。返回值是长度 PK_OWN_TRAIL_CAP 的循环缓冲数组，渲染层
 * 必须按下标 (start + i) & (PK_OWN_TRAIL_CAP - 1) 遍历 count 个点——ring 满了
 * 回绕后 pt[0] 不再是最老点，线性遍历会在最新点与最老点之间画一条横穿地图的
 * 闭口直线。
 * 线程安全：单写者 own_sample_task（1Hz）写，任意渲染任务只读，与
 * pk_own_sampler_get_phase() 同一套惯例（volatile、枚举底层 int 原子）。 */
const pk_own_trail_point_t *pk_own_sampler_get_trail(uint32_t *out_count,
                                                     uint32_t *out_start);

#ifdef __cplusplus
}
#endif
