/*
 * pk_jpeg_dec.h — P4 硬件 JPEG 解码（瓦片专用）。
 *
 * lodepng 软件解 PNG 要 450-560ms/张，是地图瓦片加载的头号瓶颈。P4 有硬件
 * JPEG 编解码器（CONFIG_SOC_JPEG_CODEC_SUPPORTED），ESP-IDF esp_driver_jpeg
 * 组件提供 jpeg_decoder_process 接口。瓦片离线转 JPEG 后，这里用硬件解，
 * decode 预期 <20ms。
 *
 * 用法：loader 启动时调 pk_jpeg_dec_init() 创建引擎（一次）；每张瓦片调
 * pk_jpeg_decode_tile() 解码。引擎模块级复用，两个 worker（tile_ld0/1）
 * 共享——jpeg_decoder_process 内部 codec_mutex 互斥，无需额外加锁。
 *
 * 输出 RGB565 swapped（与 pk_rgb565 / canvas 的字节序约定一致，见 display.h）。
 * JPEG 硬解输出 RGB888（BGR order），本模块做 RGB888→RGB565 软转换。
 */
#ifndef PK_JPEG_DEC_H
#define PK_JPEG_DEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* 初始化硬件 JPEG 解码引擎。loader 启动时调一次，幂等（重复调跳过）。 */
bool pk_jpeg_dec_init(void);

/* 引擎是否就绪（诊断用）。 */
bool pk_jpeg_dec_inited(void);

/*
 * 解一张 JPEG → RGB565 swapped buffer。
 *   jpeg_data/jpeg_len : PMTiles 读出的 JPEG 压缩字节流
 *   out_rgb565         : 调用方提供的 256×256×2 buffer（通常是瓦片缓存槽）
 * 返回 true=成功，false=失败（解码错误/引擎未初始化/buffer 不够）。
 * 假设输入是 256×256 baseline JPEG（瓦片规格，见 tileserver 渲染配置）。
 */
bool pk_jpeg_decode_tile(const uint8_t *jpeg_data, size_t jpeg_len,
                         uint16_t *out_rgb565);

#endif /* PK_JPEG_DEC_H */
