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
