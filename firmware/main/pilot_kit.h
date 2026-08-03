/*
 * pilot_kit.h — shared declarations for the Pilot Kit Box firmware.
 *
 * Three tasks cooperate to deliver a stable IQ data pipeline:
 *
 *   usb_host_lib_task  pumps usb_host_lib_handle_events() forever
 *   sdr_task           registers a USB host client, opens the RTL-SDR,
 *                      configures 1090 MHz / 2 MSPS / AGC, runs
 *                      rtlsdr_read_async() (whose callback fires on
 *                      this same task and pushes bytes into g_iq_ringbuf)
 *   dsp_task           drains g_iq_ringbuf, accumulates byte counts,
 *                      logs throughput at 1 Hz
 *
 * Per Pilot Kit Box SPEC red line #3 the IQ callback never performs DSP;
 * computation is strictly downstream of the ring buffer.
 */
#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/* --- RF / hardware constants ------------------------------------------- */

#define PK_RTLSDR_FREQ_HZ        1090000000UL  /* ADS-B Mode-S carrier */
#define PK_RTLSDR_SAMPLERATE_HZ  2000000UL     /* 2 MSPS (= 2 MB/s of IQ8) */

/* --- USB 在飞缓冲（URB）------------------------------------------------ *
 *
 * 15 × 6144 B ≈ 92 KB，与 librtlsdr 的默认值一致（DEFAULT_BUF_NUMBER /
 * DEFAULT_BUF_LENGTH，见 components/esp32-rtl-sdr/main/librtlsdr.c:359,371）。
 * 这里写成显式常量而不是传 0 吃默认值，是为了让这段说明有地方落脚。
 *
 * 这 92 KB **落在 PSRAM，不占内部 RAM** —— 前提是
 * CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y（见 sdkconfig.defaults 里
 * 那段注释）。P4 的 USB-DWC DMA 能直接访问外部 RAM。
 *
 * ⚠ 那个 Kconfig 一旦被关掉，URB 会退回内部 DMA 堆，届时这个数字必须跟着
 * 砍到 4 以下，否则开机就是这一串：
 *
 *     E rtlsdr_async: usb_host_transfer_alloc[5] ERR=257 (buf_len=6144 B)
 *     E display: ST7701 panel create failed: ESP_ERR_NO_MEM
 *     E vhci_drv: Tx ble_transport_to_ll_cmd_impl: malloc failed
 *     E baro: baro task create failed
 *
 * 长度 6144 = 12×512 对齐 USB HS 的 MPS，**别改**：组件 README 记着换成非
 * 对齐值（原来的 6400）会在 HS 上静默失流。
 *
 * 调整后看 dsp_task 的 1 Hz 吞吐日志有没有掉字节（pk_iq_dropped_bytes_swap）。
 */
#define PK_SDR_URB_COUNT         15
#define PK_SDR_URB_LEN           (12 * 512)    /* 6144 B，勿改，见上 */
/*
 * ESP32-P4 有**两个** USB-OTG 外设（soc_caps.h: SOC_USB_OTG_PERIPH_NUM = 2），
 * peripheral_map 是按外设序号的位掩码。序号到硬件的映射在 IDF 的
 * esp_hal_usb/esp32p4/include/hal/usb_dwc_ll.h：
 *
 *     #define USB_DWC_LL_GET_HW(num) (((num) == 1) ? &USB_DWC_FS : &USB_DWC_HS)
 *
 * 即 BIT0 = 0 号 = **High-Speed** 控制器（UTMI PHY，走专用 USBD_P/N 引脚），
 * BIT1 = 1 号 = Full-Speed 控制器（走 GPIO24/25 的 USB1P1_P/N）。
 *
 * RTL-SDR 必须落在 HS 上（2 MSPS IQ 流 FS 带宽不够），所以这里是 BIT0。
 *
 * 注意 USBD_P/N 这一对信号**同时**引到板上 H2（丝印 USB）和 J3 排针
 * 25/27（丝印 DM/DP）——是同一组网络，不是两条路。载板从 J3 取 DP/DM
 * 接自己的 USB-A 座，与直接插 H2 在固件看来完全等价，**不需要改这里**；
 * 但两者同一时刻只能占用一个（见 docs/hardware/board_pinout.md §6）。
 */
#define PK_USB_PERIPHERAL_MAP    BIT0

/* --- Buffering --------------------------------------------------------- *
 * Producer is 2 MSPS × 2 B (IQ8) = 4 MB/s real-time. 128 KiB only gave
 * us ~32 ms of head-room and we observed occasional overflows
 * ("DROPPED ~37 KB") once BLE + SDIO + LCD landed on the CPU 1
 * scheduler. 512 KiB ≈ 128 ms — comfortable margin against any
 * realistic scheduling burst.
 *
 * The buffer is BYTEBUF (no per-item header) and lives in PSRAM via
 * the CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 threshold — internal
 * RAM is way too precious to spend on bulk IQ.
 */
#define PK_IQ_RINGBUF_SIZE_BYTES (512 * 1024)

/* Boot splash minimum on-screen time. Init work (IMU/UI/buttons/BLE/
 * SDR) overlaps with this hold, so we only sleep for the remainder
 * if init was faster than the target. 3 s is long enough to read the
 * version line and watch the panel stabilise; bump to 5000 if you
 * want to admire the logo. */
#define PK_BOOT_SPLASH_MIN_MS    3000

extern RingbufHandle_t g_iq_ringbuf;

/* --- Task entry points ------------------------------------------------- */

void usb_host_lib_task(void *arg);
void sdr_task(void *arg);
void dsp_task(void *arg);

/* --- Drop counter (sdr_task producer, dsp_task consumer) --------------- *
 * Returns the number of IQ bytes that were dropped on the floor since the
 * previous call (because xRingbufferSend() failed) and atomically resets
 * the counter. 32-bit reads/writes are atomic on RV32 so no extra
 * synchronisation is needed for this single-producer/single-consumer use.
 */
uint32_t pk_iq_dropped_bytes_swap(void);

/* --- SDR re-init signal (dsp_task → sdr_task) --------------------------- *
 *
 * Callable from any task. Asks sdr_task to tear down the current
 * librtlsdr session (close + re-open the same USB device address — no
 * USB unplug/replug needed) and rebuild the streaming pipeline. Used
 * by dsp_task's IQ-stall watchdog instead of esp_restart() so a hung
 * dongle no longer drags IMU / PFD / BLE down with it.
 *
 * `reason` is logged as the cause. After N consecutive re-inits that
 * fail to restore a live IQ stream, sdr_task escalates to esp_restart()
 * as a last-resort recovery.
 */
void pk_sdr_request_reinit(const char *reason);

/* --- Dongle 状态（诊断页）--------------------------------------------- *
 *
 * NO_DEVICE 与其余三态的区别是**接线问题 vs 运行问题**，诊断页要分开说。
 *
 * RTL-SDR 走的是原生 USB 2.0 HS 那一对 USBD_P/N，它有两个物理出口，
 * 二选一（同一组网络，不能同时占用）：
 *   - 装了载板：插载板上的 USB-A 座（DP/DM 由 J3 排针 27/25 引出），H2 空着；
 *   - 裸板调试：插板上 H2（丝印 USB）的 Type-C，配 OTG 转接。
 * H1 是 CH343P 调试串口，P1 是 C6 UART 下载排针，两者都不是 SDR 口。
 */
typedef enum {
    PK_SDR_NO_DEVICE = 0,   /* USB 上没枚举到任何设备 —— 没插或插错口 */
    PK_SDR_ATTACHED,        /* 枚举到了，尚未打开（初始化中或打开失败） */
    PK_SDR_STALLED,         /* 已打开但超过 1 s 没有 IQ —— 掉链/供电/过热 */
    PK_SDR_STREAMING,       /* 正常出数 */
} pk_sdr_state_t;

pk_sdr_state_t pk_sdr_state_get(uint32_t *dropped_kb);
