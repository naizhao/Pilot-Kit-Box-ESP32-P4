/*
 * pk_jpeg_dec.c — P4 硬件 JPEG 解码实现。接口说明见 pk_jpeg_dec.h。
 *
 * 照抄 ESP-IDF examples/peripherals/jpeg/jpeg_decode 的用法，适配瓦片场景：
 *   - 引擎模块级单例，pk_jpeg_dec_init() 创建一次永久复用
 *   - 输入/输出 buffer 用 jpeg_alloc_decoder_mem（PSRAM + cache 对齐），
 *     模块级 static 复用，不每张 malloc/free（DMA 对齐分配有开销）
 *   - JPEG 硬解输出 RGB888(BGR)，转成 RGB565 swapped（pk_rgb565 约定）
 *
 * 线程模型：两个 worker（tile_ld0/1）共享这一个引擎。jpeg_decoder_process
 * 内部用 codec_mutex 互斥（见 esp_driver_jpeg/jpeg_decode.c），同一时刻
 * 只有一个 worker 真正在解，另一个阻塞等 mutex。硬件 JPEG 是单一外设，
 * 两个引擎 handle 最终也排队访问同一硬件，所以单引擎共享最省资源。
 */
#include "pk_jpeg_dec.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/jpeg_decode.h"

static const char *TAG = "pk_jpeg_dec";

/* 瓦片固定 256×256。RGB888 输出 = 256*256*3 = 192 KB。
 * JPEG 硬件的 MCU 对齐要求宽高是 8/16 的倍数，256 满足，无需 padding。 */
#define TILE_W           256
#define TILE_H           256
#define RGB888_OUT_BYTES ((size_t)TILE_W * TILE_H * 3)
/* JPEG 压缩输入上限：z10 瓦片实测最大 ~53KB，留足余量。 */
#define JPEG_IN_MAX_BYTES (128u * 1024u)

static jpeg_decoder_handle_t s_engine;
static bool s_inited;
/* 两个 worker(tile_ld0/1)共享同一套 buffer + 引擎，整个 decode 必须串行：
 * memcpy 到 s_in_buf、硬解到 s_out_rgb888、RGB565 转换这三步若有 worker 交叉，
 * 会互相覆盖输入/输出 → 拼图错乱。引擎内部 codec_mutex 只保护硬解那一拍，
 * 不覆盖前后的内存操作，这里补一把外层锁。 */
static SemaphoreHandle_t s_decode_lock;

/* 复用的对齐 buffer。jpeg_alloc_decoder_mem 要求输出 buffer 地址+大小都按
 * cache line 对齐（见 jpeg_decode.c 注释），所以不能普通 malloc。 */
static uint8_t *s_in_buf;       /* JPEG 压缩数据，PSRAM */
static uint8_t *s_out_rgb888;   /* 硬解输出 RGB888，PSRAM cache 对齐 */

bool pk_jpeg_dec_inited(void) { return s_inited; }

bool pk_jpeg_dec_init(void)
{
    if (s_inited) return true;
    if (s_decode_lock == NULL) {
        s_decode_lock = xSemaphoreCreateMutex();
        if (s_decode_lock == NULL) return false;
    }
    jpeg_decode_engine_cfg_t eng_cfg = {
        .timeout_ms = 3000,   /* 单张瓦片硬解应 <50ms，3s 足够兜底 */
    };
    esp_err_t err = jpeg_new_decoder_engine(&eng_cfg, &s_engine);
    if (err != ESP_OK) {
        /* init 失败最常见原因：P4 rev<v3 的 rxlink DMA 描述符分不到带 DEFAULT cap
         * 的内部内存。需应用 firmware/patches/esp_driver_jpeg_rxlink_no_default.patch
         * （详见 memory: p4-jpeg-dma-no-default-cap）。失败不致命，PNG 包仍可用。 */
        ESP_LOGE(TAG, "jpeg_new_decoder_engine: %s", esp_err_to_name(err));
        return false;
    }

    /* 输入 buffer：PSRAM 即可，无对齐要求（jpeg_alloc_decoder_mem 注释）。 */
    jpeg_decode_memory_alloc_cfg_t tx_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t in_alloced = 0;
    s_in_buf = jpeg_alloc_decoder_mem(JPEG_IN_MAX_BYTES, &tx_cfg, &in_alloced);
    if (s_in_buf == NULL) {
        ESP_LOGE(TAG, "alloc input buffer failed");
        jpeg_del_decoder_engine(s_engine);
        s_engine = NULL;
        return false;
    }

    /* 输出 buffer：必须 cache 对齐（2DDMA 写 PSRAM）。 */
    jpeg_decode_memory_alloc_cfg_t rx_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t out_alloced = 0;
    s_out_rgb888 = jpeg_alloc_decoder_mem(RGB888_OUT_BYTES, &rx_cfg, &out_alloced);
    if (s_out_rgb888 == NULL) {
        ESP_LOGE(TAG, "alloc output buffer failed");
        free(s_in_buf);
        s_in_buf = NULL;
        jpeg_del_decoder_engine(s_engine);
        s_engine = NULL;
        return false;
    }

    s_inited = true;
    ESP_LOGI(TAG, "JPEG HW decoder ready (in=%uB out=%uB)",
             (unsigned)in_alloced, (unsigned)out_alloced);
    return true;
}

bool pk_jpeg_decode_tile(const uint8_t *jpeg_data, size_t jpeg_len,
                         uint16_t *out_rgb565)
{
    /* 懒初始化：pk_tile_loader_init 在启动早期调 init 会因内部 RAM 不足失败
     *（JPEG DMA 描述符 rxlink 要 MALLOC_CAP_INTERNAL，启动内存悬崖期分不到）。
     * 推迟到首次真正解码——此时启动已完成，内部 RAM 稳定。幂等。 */
    if (!s_inited && !pk_jpeg_dec_init()) return false;
    if (s_engine == NULL || out_rgb565 == NULL) return false;
    if (jpeg_len == 0 || jpeg_len > JPEG_IN_MAX_BYTES) return false;

    /* 串行化：两个 worker 共享 s_in_buf/s_out_rgb888/引擎，整段必须互斥。 */
    xSemaphoreTake(s_decode_lock, portMAX_DELAY);
    /* 拷到对齐输入 buffer。jpeg_data 来自 PMTiles fread（普通 PSRAM），
     * 硬件 2DDMA 读输入要求走 jpeg_alloc_decoder_mem 分配的 buffer。 */
    memcpy(s_in_buf, jpeg_data, jpeg_len);

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
    };

    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(s_engine, &dec_cfg,
                                         s_in_buf, jpeg_len,
                                         s_out_rgb888, RGB888_OUT_BYTES,
                                         &out_size);
    if (err != ESP_OK) {
        xSemaphoreGive(s_decode_lock);
        ESP_LOGW(TAG, "jpeg_decoder_process: %s (len=%u)", esp_err_to_name(err),
                 (unsigned)jpeg_len);
        return false;
    }

    /* RGB888 → RGB565 swapped（内联避免调用开销，65536 像素）。 */
    const uint8_t *src = s_out_rgb888;
    for (int i = 0; i < TILE_W * TILE_H; i++) {
        uint8_t r = src[i * 3 + 0];
        uint8_t g = src[i * 3 + 1];
        uint8_t b = src[i * 3 + 2];
        uint16_t v = ((uint16_t)(r & 0xF8) << 8) |
                     ((uint16_t)(g & 0xFC) << 3) |
                     ((uint16_t)b >> 3);
        /* swapped：高低字节交换（与 pk_rgb565 一致，见 display.h:43-47） */
        out_rgb565[i] = (uint16_t)((v >> 8) | (v << 8));
    }

    xSemaphoreGive(s_decode_lock);
    return true;
}
