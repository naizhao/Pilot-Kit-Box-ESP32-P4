/* PCM1808 packed-24 stereo → ES8311 signed-16 stereo 纯转换层。 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PK_AIRBAND_AUDIO_PACKED_FRAME_BYTES 6u
#define PK_AIRBAND_AUDIO_GATE_FULL_Q15      32767u
#define PK_AIRBAND_AUDIO_GATE_STEP_Q15      256u
/* PCM1808 实机航空话音/噪声底典型只占 16-bit 输出的低十几位。
 * 固定 +12 dB（4倍）提高可懂度；当前实测 24-bit 峰值约 1.4M，换算后乘
 * 4 仍在 signed-16 内。异常强峰值继续使用饱和保护，不发生回绕。 */
#define PK_AIRBAND_AUDIO_PROGRAM_GAIN       4

typedef struct {
    uint16_t level_q15;
} pk_airband_audio_gate_t;

/* 把门控状态明确落在全开或全关；初始化/队列丢弃后调用。 */
void pk_airband_audio_gate_reset(pk_airband_audio_gate_t *gate, bool open);

/* 采集断流或队列丢弃会使旧包络失效：立即归零，下一次恢复从攻击斜坡开始。 */
void pk_airband_audio_gate_stream_break(pk_airband_audio_gate_t *gate);

/*
 * 解码 PCM1808 的 packed signed-24 little-endian stereo 帧，只取左声道，
 * 转成 signed-16 后复制到 ES8311 的 L/R 两个时隙。
 *
 * audible 是本块的目标门控；变化时每帧推进固定 Q15 步长，避免爆音。
 * dst_frames 是 dst 能容纳的 stereo 帧数，不是 int16_t 元素数。
 * 成功返回转换帧数。畸形长度、容量不足或 NULL 返回 0，且不改 dst/gate。
 */
size_t pk_airband_audio_render(const uint8_t *src, size_t src_len,
                               int16_t *dst, size_t dst_frames,
                               bool audible, pk_airband_audio_gate_t *gate);
