/*
 * pilot_kit.h — shared declarations for the Pilot Kit Box firmware.
 *
 * ADS-B 数据经 UART 自 RP2040 进入 adsb_link_task（UART2，见
 * adsb_link_task.c），报文后业务链在 modes_ingest 之后
 * （CPR/航迹/记录/看板）。
 */
#pragma once

/* Boot splash minimum on-screen time. Init work (IMU/UI/BLE/
 * SDR) overlaps with this hold, so we only sleep for the remainder
 * if init was faster than the target. 3 s is long enough to read the
 * version line and watch the panel stabilise; bump to 5000 if you
 * want to admire the logo. */
#define PK_BOOT_SPLASH_MIN_MS    3000
