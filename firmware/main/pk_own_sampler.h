/*
 * pk_own_sampler.h — 本机 1 Hz 航迹采样，own.trk 的生产者。
 *
 * 设计依据 docs/internal/2026-08-02-adsb-data-persistence-design-zh_CN.md
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

#ifdef __cplusplus
}
#endif
