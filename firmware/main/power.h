/*
 * power.h — Soft power management for Pilot Kit Box.
 *
 * Single entry point that puts the device into ESP32-P4 deep sleep
 * with GPIO5 (MODE button, LP_IO) configured as the ext1 wake source.
 * Wake = full cold boot from app_main(), so there is no "resume" path
 * to design — boot splash, every pk_*_init(), the lot, runs fresh.
 *
 * Triggered from main.c's MODE LONG_PRESS handler. Safe to call from
 * the button task context: the only pre-sleep operations are
 * pk_display_set_brightness(0) and a ~50 ms vTaskDelay to let the UART
 * FIFO drain. The button-task callback "no long sleeps" rule is
 * intentionally bent here because esp_deep_sleep_start() never returns
 * — the OS scheduler stops, every task is torn down by the clock cut,
 * and the next thing to happen is a cold boot. There is nothing left
 * for the button task to poll.
 */
#pragma once

void pk_power_enter_sleep(void);
