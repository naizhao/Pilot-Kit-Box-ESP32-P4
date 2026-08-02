#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * pk_clock — 系统墙钟(settimeofday) 的单一校时入口。
 *
 * BLE(手机写入 / iOS CTS) 和 GPS 都通过这里校准本机时间，集中处理：
 *   - 下限保护：拒绝早于 2024-01-01 的 epoch（防止把时钟回退到错误的小值）。
 *   - "GPS 优先"：一旦 GPS 精校(source=="gps")成功，在
 *     PK_CLOCK_GPS_PRIORITY 窗口内忽略其它来源 —— GPS 是卫星授时，比手机更准。
 *
 * 线程安全：状态为简单标量，校时是低频事件；沿用本工程既有的无锁 volatile 风格。
 */

/* 把一个 UTC Unix epoch(毫秒) 应用到系统时钟。
 * source 仅用于日志/诊断显示，必须是静态字面量（内部只存指针，不拷贝）。
 * 返回 true=已写入系统时钟；false=被拒（早于下限，或被 GPS 优先挡下）。*/
bool pk_clock_apply_epoch_ms(int64_t epoch_ms, const char *source);

/* 系统时钟是否已被任何来源校准过。*/
bool pk_clock_is_synced(void);

/* 最近一次成功校时的来源字符串（"gps"/"gps-coarse"/"ble-write"/"ios-cts"）；
 * 从未校时返回 "none"。供诊断页显示时间来源。*/
const char *pk_clock_source(void);

/* 民用 UTC 年/月/日 时:分:秒 → Unix epoch 毫秒。
 * frac256 为 1/256 秒的小数部分（无小数传 0）。GPS 与 BLE-CTS 解析时间共用。*/
int64_t pk_clock_civil_utc_to_epoch_ms(int yr, int mo, int dy,
                                       int hr, int mi, int se, int frac256);

/*
 * 校时回调（ADS-B/本机数据落盘设计「时间可信度」节：校时瞬间要向 own.trk
 * 写一条时间修正记录）。epoch_ms 是本次写入的新时间；prev_ms 是写入前的
 * 时钟读数（gettimeofday 快照，用于回放端做相对时间平移）；source 与
 * pk_clock_apply_epoch_ms() 的入参同一个指针，不拷贝。
 *
 * 单槽（当前唯一消费者是 pk_own_sampler）；重复注册直接覆盖。传 NULL 清除。
 * 回调在触发校时的调用方任务上下文里同步执行（GPS/BLE 任务），必须快返回，
 * 不能做阻塞 I/O——pk_own_sampler 的实现里这条走的是 pk_rec_store 的直接
 * 落盘调用，校时本身是低频事件，可以接受。 */
typedef void (*pk_clock_sync_cb_t)(int64_t epoch_ms, int64_t prev_ms, const char *source);
void pk_clock_register_sync_cb(pk_clock_sync_cb_t cb);
