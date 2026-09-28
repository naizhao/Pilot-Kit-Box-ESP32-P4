/* Public program routing and 16k mono -> ES8311 48k stereo conversion. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PK_AIRBAND_AUDIO_PACKED_FRAME_BYTES 6u
#define PK_AIRBAND_AUDIO_GATE_FULL_Q15      32767u
#define PK_AIRBAND_AUDIO_GATE_STEP_Q15      256u
/* Program amplification is applied after the anti-alias FIR; clipping is
 * measured and cannot be described as a substitute for RF gain control. */
#define PK_AIRBAND_AUDIO_PROGRAM_GAIN       4

typedef struct {
    uint16_t level_q15;
} pk_airband_audio_gate_t;

/* 把门控状态明确落在全开或全关；初始化/队列丢弃后调用。 */
void pk_airband_audio_gate_reset(pk_airband_audio_gate_t *gate, bool open);

/* 采集断流或队列丢弃会使旧包络失效：立即归零，下一次恢复从攻击斜坡开始。 */
void pk_airband_audio_gate_stream_break(pk_airband_audio_gate_t *gate);

/*
 * Legacy single-rate converter retained for existing host regression vectors.
 * Live capture uses airband_pcm16 + pk_airband_audio_route16 below.
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

typedef struct {
    pk_airband_audio_gate_t program_gate;
    pk_airband_audio_gate_t speaker_gate;
    int16_t previous16;
} pk_airband_audio_route_t;
void pk_airband_audio_route_reset(pk_airband_audio_route_t *s);
/* In-place 16k program squelch ramp, then speaker-only mute and 3x linear
 * interpolation into 48k stereo. Program is the record/BLE tap; local test
 * tone never enters here. Capacity is stereo frames, not int16 elements. */
size_t pk_airband_audio_route16(pk_airband_audio_route_t *s,
                                int16_t *program, size_t frames,
                                bool program_open, bool speaker_muted,
                                int16_t *stereo48, size_t capacity);
