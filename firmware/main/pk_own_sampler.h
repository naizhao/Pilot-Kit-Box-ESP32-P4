/*
 * pk_own_sampler.h — 本机 1 Hz 航迹采样，own.trk 的生产者。
 *
 * 设计依据 docs/internal/2026-08-02-adsb-data-persistence-design-zh_CN.md
 * 「own.trk」「相位标记」「写入管线」三节。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif
