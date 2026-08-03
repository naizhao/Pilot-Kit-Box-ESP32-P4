/*
 * boot_splash.h — pre-PFD boot logo screen.
 *
 * Renders the Pilot Kit logo (128×128 RGB565, embedded as a
 * .rodata blob via the build system's EMBED_FILES mechanism) into
 * the centre of the framebuffer, with "PILOT KIT BOX" and a short
 * version line below it. Called from main.c once `pk_display_init()`
 * returns, *before* the rest of the firmware finishes coming up —
 * gives the user something to look at during the ~1 second between
 * LCD ready and PFD task starting (USB host, ESP-Hosted, IMU init,
 * BLE all happen in that window).
 *
 * Once the PFD task starts spinning at 30 FPS it will overwrite the
 * splash on the next frame — no explicit dismiss needed.
 */
#pragma once

#include <stdint.h>

/* Renders the boot splash into the logical 800×480 RGB565 framebuffer. */
void pk_boot_splash_render(uint16_t *fb);

/*
 * 报一步开机进度：logo 下方那条进度条 + 阶段名。
 *
 * 为什么要有它
 * ------------
 * 实测（2026-08-04，4.3″ 一体板）：上电 5.3 s 出 logo，之后 IMU/气压计 ~0.9 s、
 * pk_map_store_scan() 扫 4 个 pmtiles **同步** 3.9 s，8.4 s 才起 PFD。也就是说
 * logo 出来以后还有三秒多，屏上是一个一动不动的 logo，用户不知道在等什么，
 * 反馈原话是「白等」。这条进度条就是把那三秒讲清楚。
 *
 * 语义与用法
 * ----------
 *   label  阶段名，直接喂 pk_i18n_text(PK_TR_BOOT_STAGE_*) 的返回值。
 *          描述**接下来要做的事**，所以在耗时动作之前调，不是之后。
 *          NULL 或空串 = 只画条不画字。文案会被拷进模块内的定长缓冲，
 *          调用方不必保证生命周期。
 *   done   已完成的步数，[0, total]，越界自动钳位。
 *   total  总步数，<1 按 1 处理。
 *
 * 内部自己取 framebuffer（pk_display_framebuffer）、重画整屏、同步推屏
 * （pk_display_flush_full），调用方一行即可。点屏失败（framebuffer 为 NULL）
 * 时静默返回——app_main 那边已经记过一条日志了。
 *
 * 只能在 **pk_pfd_start() 之前**调用
 * ---------------------------------
 * PFD 任务一起来就按 30 FPS 独占 framebuffer 与 flush，app_main 再往里画就是
 * 两个写者抢同一块内存 + 抢同一个 flush，会撕帧。所以 SDR 那一步（sdr_task 排
 * 在 pk_pfd_start() 之后，理由见 main.c 那段注释）**不在**这条进度条的覆盖范围
 * 内，别为它补一次调用。
 *
 * 建议的调用次序（app_main，与实测耗时对应）：
 *
 *   pk_boot_splash_progress(pk_i18n_text(PK_TR_BOOT_STAGE_START),   0, 3);
 *       ← 取代 pk_display_init() 成功分支里那两行 render + flush_full
 *   pk_boot_splash_progress(pk_i18n_text(PK_TR_BOOT_STAGE_SENSORS), 1, 3);
 *       ← 紧接在 pk_imu_init() 之前（它 + pk_baro_start() 约 0.9 s）
 *   pk_boot_splash_progress(pk_i18n_text(PK_TR_BOOT_STAGE_MAP),     2, 3);
 *       ← 紧接在 pk_tile_loader_init() 之前（最慢的一步，3.9 s）
 *   pk_boot_splash_progress(pk_i18n_text(PK_TR_BOOT_STAGE_READY),   3, 3);
 *       ← splash 最短驻留那段 vTaskDelay 之前、pk_pfd_start() 之前
 *
 * 每次调用的代价 = 重画一屏 + 一次 full flush，只在上面这几个点调，别放进循环。
 */
void pk_boot_splash_progress(const char *label, int done, int total);
