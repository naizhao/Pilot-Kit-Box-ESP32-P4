/*
 * V4.9 AIRBAND 最小闭环：BK4819 RX-only + PCM1808 I2S 采集诊断。
 *
 * 实物网表（V4.9 导出 XML）：
 *   BK4819 U21: SCN=GPIO29, SCK=GPIO30, SDATA=GPIO47，EARO→PCM1808 VINL；
 *   PCM1808 U22: MCLK=GPIO5, BCK=GPIO22, LRCK=GPIO52, DOUT=GPIO48；
 *   MD0/MD1/FMT 全接 GND，因此为 slave / I2S 格式；VCC=VHF_AVCC(5V)，
 *   VDD=3V3_DIG。PAOUT 没有网络，固件还在 REG36/REG30 双重禁用发射路径。
 *
 * BK 三线时序与寄存器初始化的来源/边界见 airband_bk4819.c。PCM1808 时钟
 * 依据 TI PCM1808 Rev.B：48 kHz、SCKI=384fs、24-bit I2S。ESP-IDF 6.0.1
 * 在 ESP32-P4 上把 24-bit RX DMA 暴露为紧凑 3 byte/sample。
 */
#include "airband.h"
#include "airband_bk4819.h"
#include "airband_audio.h"
#include "airband_speaker.h"
#include "airband_frequency_input.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define VHF_SCN_GPIO   GPIO_NUM_29
#define VHF_SCK_GPIO   GPIO_NUM_30
#define VHF_SDATA_GPIO GPIO_NUM_47
#define VHF_MCLK_GPIO  GPIO_NUM_5
#define VHF_BCK_GPIO   GPIO_NUM_22
#define VHF_LRCK_GPIO  GPIO_NUM_52
#define VHF_DOUT_GPIO  GPIO_NUM_48

#define AIRBAND_HZ 121500000u
#define AUDIO_RATE 48000u
#define DEFAULT_SQUELCH_DBM_X2 (-180)
#define DEFAULT_SQUELCH_HYST_X2 12u
#define BK_RETRY_INTERVAL_US 1000000LL

static const char *TAG = "airband";
static i2s_chan_handle_t s_rx;
static portMUX_TYPE s_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static pk_airband_state_t s_diag;

typedef struct {
    uint32_t requested_hz;
    uint32_t requested_channel_khz;
    bool scanning;
    pk_airband_scan_plan_t scan_plan;
    int64_t scan_advance_at_us;
    uint32_t scan_wraps;
    bool squelch_enabled;
    int16_t squelch_open_dbm_x2;
    uint16_t squelch_hysteresis_db_x2;
} airband_control_t;

static airband_control_t s_control = {
    .requested_hz = AIRBAND_HZ,
    .requested_channel_khz = 121500u,
    .squelch_enabled = true,
    .squelch_open_dbm_x2 = DEFAULT_SQUELCH_DBM_X2,
    .squelch_hysteresis_db_x2 = DEFAULT_SQUELCH_HYST_X2,
};

/* 与上游 BK4819 参考驱动一致：SDA 建立、SCK 高、SCK 低各等待 1 us。 */
static inline void bus_delay(void) { esp_rom_delay_us(1); }

static void write_bits(uint32_t value, unsigned bits)
{
    for (unsigned i = 0; i < bits; i++) {
        gpio_set_level(VHF_SDATA_GPIO, (value & (1u << (bits - 1u))) != 0);
        bus_delay();
        gpio_set_level(VHF_SCK_GPIO, 1);
        bus_delay();
        value <<= 1;
        gpio_set_level(VHF_SCK_GPIO, 0);
        bus_delay();
    }
}

static void reg_write(uint8_t reg, uint16_t value)
{
    gpio_set_direction(VHF_SDATA_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(VHF_SCN_GPIO, 1);
    gpio_set_level(VHF_SCK_GPIO, 0);
    bus_delay();
    gpio_set_level(VHF_SCN_GPIO, 0);
    write_bits(reg, 8);
    bus_delay();
    write_bits(value, 16);
    bus_delay();
    gpio_set_level(VHF_SCN_GPIO, 1);
    bus_delay();
    gpio_set_level(VHF_SCK_GPIO, 1);
    gpio_set_level(VHF_SDATA_GPIO, 1);
}

static uint16_t reg_read(uint8_t reg)
{
    uint16_t value = 0;
    gpio_set_direction(VHF_SDATA_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(VHF_SCN_GPIO, 1);
    gpio_set_level(VHF_SCK_GPIO, 0);
    bus_delay();
    gpio_set_level(VHF_SCN_GPIO, 0);
    write_bits((uint8_t)(reg | 0x80u), 8);
    gpio_set_direction(VHF_SDATA_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(VHF_SDATA_GPIO, GPIO_PULLUP_ONLY);
    bus_delay();
    for (unsigned i = 0; i < 16; i++) {
        value = (uint16_t)((value << 1) | gpio_get_level(VHF_SDATA_GPIO));
        gpio_set_level(VHF_SCK_GPIO, 1);
        bus_delay();
        gpio_set_level(VHF_SCK_GPIO, 0);
        bus_delay();
    }
    gpio_set_direction(VHF_SDATA_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(VHF_SCN_GPIO, 1);
    bus_delay();
    gpio_set_level(VHF_SCK_GPIO, 1);
    gpio_set_level(VHF_SDATA_GPIO, 1);
    return value;
}

static bool bk_configure(uint32_t frequency_hz)
{
    size_t n = 0;
    const pk_airband_reg_step_t *seq = pk_airband_rx_init_seq(&n);
    pk_airband_reg_step_t freq[2];
    if (!pk_airband_frequency_steps(frequency_hz, freq)) return false;

    /* 表尾三步是 RX 上电转换；频率必须先落定。 */
    for (size_t i = 0; i < n - 3; i++) {
        reg_write(seq[i].reg, seq[i].val);
        if (i == 1) vTaskDelay(pdMS_TO_TICKS(1));
    }
    reg_write(freq[0].reg, freq[0].val);
    reg_write(freq[1].reg, freq[1].val);
    for (size_t i = n - 3; i < n; i++) reg_write(seq[i].reg, seq[i].val);
    vTaskDelay(pdMS_TO_TICKS(10));

    const uint16_t r38 = reg_read(0x38), r39 = reg_read(0x39);
    const uint16_t r47 = reg_read(0x47), r30 = reg_read(0x30);
    const bool ok = r38 == freq[0].val && r39 == freq[1].val &&
                    r47 == 0x6740u && r30 == 0xBFF1u;
    if (ok) {
        ESP_LOGI(TAG,
                 "BK4819 readback: r38=%04X r39=%04X r47=%04X r30=%04X OK",
                 r38, r39, r47, r30);
    } else {
        ESP_LOGW(TAG,
                 "BK4819 readback: r38=%04X r39=%04X r47=%04X r30=%04X FAIL",
                 r38, r39, r47, r30);
    }
    return ok;
}

static bool bk_tune(uint32_t frequency_hz)
{
    pk_airband_reg_step_t tune[4];
    if (!pk_airband_rx_retune_steps(frequency_hz, tune)) return false;

    /* 只关闭/恢复 RX 链，REG36 的 PA-CTL 与 REG30 的 TX/MIC 位始终为 0。 */
    for (size_t i = 0; i < 4; i++) reg_write(tune[i].reg, tune[i].val);
    vTaskDelay(pdMS_TO_TICKS(10));

    const uint16_t r38 = reg_read(0x38), r39 = reg_read(0x39);
    const uint16_t r30 = reg_read(0x30);
    const bool ok = r38 == tune[1].val && r39 == tune[2].val &&
                    r30 == tune[3].val;
    ESP_LOGI(TAG, "tune freq=%" PRIu32 ".%03" PRIu32
             "MHz r38=%04X r39=%04X r30=%04X %s",
             frequency_hz / 1000000u, (frequency_hz % 1000000u) / 1000u,
             r38, r39, r30, ok ? "OK" : "FAIL");
    return ok;
}

static airband_control_t control_snapshot(void)
{
    airband_control_t out;
    portENTER_CRITICAL(&s_diag_mux);
    out = s_control;
    portEXIT_CRITICAL(&s_diag_mux);
    return out;
}

static void scan_advance_if_due(int64_t now, bool rf_online)
{
    portENTER_CRITICAL(&s_diag_mux);
    if (rf_online && s_control.scanning &&
        now >= s_control.scan_advance_at_us) {
        uint32_t next;
        bool wrapped;
        if (pk_airband_scan_next(&s_control.scan_plan,
                                 s_control.requested_hz,
                                 &next, &wrapped)) {
            s_control.requested_hz = next;
            s_control.requested_channel_khz = 0;
            if (wrapped) s_control.scan_wraps++;
            s_control.scan_advance_at_us =
                now + (int64_t)s_control.scan_plan.dwell_ms * 1000;
        } else {
            s_control.scanning = false;
        }
    }
    portEXIT_CRITICAL(&s_diag_mux);
}

static void scan_rearm_after_tune(uint32_t frequency_hz, int64_t now)
{
    portENTER_CRITICAL(&s_diag_mux);
    if (s_control.scanning && s_control.requested_hz == frequency_hz) {
        s_control.scan_advance_at_us =
            now + (int64_t)s_control.scan_plan.dwell_ms * 1000;
    }
    portEXIT_CRITICAL(&s_diag_mux);
}

static esp_err_t audio_init(void)
{
    /* 载板 ES8311 明确占 I2S0；PCM1808 固定 I2S1，初始化顺序不再决定端口。 */
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,
                                                        I2S_ROLE_MASTER);
    chan.dma_desc_num = 8;
    chan.dma_frame_num = PK_AIRBAND_I2S_DMA_FRAME_NUM;
    esp_err_t err = i2s_new_channel(&chan, NULL, &s_rx);
    if (err != ESP_OK) return err;

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_24BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = VHF_MCLK_GPIO,
            .bclk = VHF_BCK_GPIO,
            .ws = VHF_LRCK_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = VHF_DOUT_GPIO,
            .invert_flags = { false, false, false },
        },
    };
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_24BIT;
    if ((err = i2s_channel_init_std_mode(s_rx, &cfg)) != ESP_OK) return err;
    return i2s_channel_enable(s_rx);
}

static void task(void *arg)
{
    (void)arg;
    bool online = false;
    bool squelch_open = false;
    uint32_t active_hz = 0;
    uint8_t samples[PK_AIRBAND_I2S_DMA_FRAME_NUM *
                    PK_AIRBAND_PACKED_STEREO_FRAME_BYTES];
    int16_t speaker_pcm[PK_AIRBAND_I2S_DMA_FRAME_NUM * 2u];
    pk_airband_audio_gate_t speaker_gate;
    pk_airband_audio_gate_reset(&speaker_gate, false);
    pk_airband_audio_stats_t audio;
    pk_airband_audio_stats_reset(&audio);
    uint32_t read_errors = 0;
    int64_t report_at = esp_timer_get_time() + 1000000;
    int64_t next_configure_at_us = 0;

    for (;;) {
        const int64_t before_read = esp_timer_get_time();
        scan_advance_if_due(before_read, online);
        airband_control_t ctl = control_snapshot();
        if (!online) {
            if (before_read >= next_configure_at_us) {
                online = bk_configure(ctl.requested_hz);
                if (online) {
                    active_hz = ctl.requested_hz;
                    scan_rearm_after_tune(active_hz, esp_timer_get_time());
                } else {
                    next_configure_at_us =
                        esp_timer_get_time() + BK_RETRY_INTERVAL_US;
                }
            }
        } else if (ctl.requested_hz != active_hz) {
            online = bk_tune(ctl.requested_hz);
            if (online) {
                active_hz = ctl.requested_hz;
                scan_rearm_after_tune(active_hz, esp_timer_get_time());
            } else {
                next_configure_at_us =
                    esp_timer_get_time() + BK_RETRY_INTERVAL_US;
            }
        }

        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, samples, sizeof(samples), &got,
                                         pdMS_TO_TICKS(250));
        bool pcm_valid = false;
        bool requested_playable = false;
        size_t rendered_frames = 0;
        if (err == ESP_OK && got != 0) {
            if (!pk_airband_audio_accumulate_packed24(samples, got, &audio)) {
                read_errors++;
            } else {
                pcm_valid = true;
                const bool audible = online &&
                    (!ctl.squelch_enabled || squelch_open) &&
                    !pk_airband_speaker_is_muted();
                const bool had_audio = speaker_gate.level_q15 != 0;
                rendered_frames = pk_airband_audio_render(
                    samples, got, speaker_pcm,
                    PK_AIRBAND_I2S_DMA_FRAME_NUM, audible, &speaker_gate);
                requested_playable = audible || had_audio;
            }
        } else if (err != ESP_ERR_TIMEOUT) {
            read_errors++;
        }
        const bool read_ok = err == ESP_OK && got != 0;
        const bool capture_continuous =
            pk_airband_speaker_capture_continuous(read_ok, pcm_valid,
                                                   rendered_frames);
        if (!capture_continuous)
            pk_airband_audio_gate_stream_break(&speaker_gate);
        const bool capture_playable = pk_airband_speaker_capture_playable(
            read_ok, pcm_valid, rendered_frames, requested_playable);
        pk_airband_speaker_set_program_playable(capture_playable);
        if (rendered_frames != 0) {
            /* 先发布节目门，再入队；关门斜坡结束后的全零块只负责清陈旧队列。 */
            (void)pk_airband_speaker_submit(
                speaker_pcm, rendered_frames, capture_playable);
        }

        const int64_t now = esp_timer_get_time();
        if (now < report_at) continue;
        report_at = now + 1000000;

        const uint16_t r30_before = reg_read(0x30);
        uint16_t r67 = 0, r65 = 0, r63 = 0;
        uint16_t r30_after = r30_before;
        if (online && r30_before == 0xBFF1u) {
            r67 = reg_read(0x67);
            r65 = reg_read(0x65);
            r63 = reg_read(0x63);
            r30_after = reg_read(0x30);
        }
        pk_airband_diag_t d;
        const bool rf_valid = pk_airband_decode_valid_rf(
            online, r30_before, r30_after, r67, r65, r63, &d);
        online = rf_valid;
        ctl = control_snapshot();
        if (rf_valid) {
            squelch_open = pk_airband_squelch_gate(
                ctl.squelch_enabled, squelch_open, d.rssi_dbm_x2,
                ctl.squelch_open_dbm_x2, ctl.squelch_hysteresis_db_x2);
        } else {
            squelch_open = false;
        }
        const uint32_t left_rms =
            pk_airband_audio_rms(audio.left_sum_sq, audio.frames);
        const uint32_t right_rms =
            pk_airband_audio_rms(audio.right_sum_sq, audio.frames);
        pk_airband_speaker_status_t speaker = {0};
        (void)pk_airband_speaker_get(&speaker);
        const pk_airband_state_t snap = {
            .online = online,
            .frequency_hz = online ? active_hz : 0,
            .rf = d,
            .health = pk_airband_assess(online, audio.frames, read_errors),
            .audio_frames = audio.frames,
            .audio_nonzero = audio.left_nonzero,
            .audio_min = audio.frames ? audio.left_min : 0,
            .audio_max = audio.frames ? audio.left_max : 0,
            .audio_rms = left_rms,
            .audio_right_nonzero = audio.right_nonzero,
            .audio_right_min = audio.frames ? audio.right_min : 0,
            .audio_right_max = audio.frames ? audio.right_max : 0,
            .audio_right_rms = right_rms,
            .read_errors = read_errors,
            .speaker_online = speaker.online,
            .speaker_muted = speaker.muted,
            .speaker_tone_active = speaker.tone_active,
            .speaker_pa_enabled = speaker.pa_enabled,
            .speaker_volume = speaker.volume,
            .speaker_test_result = speaker.test_result,
            .speaker_test_expected_frames = speaker.test_expected_frames,
            .speaker_test_written_frames = speaker.test_written_frames,
            .speaker_output_frames = speaker.output_frames,
            .speaker_queue_drops = speaker.queue_drops,
            .speaker_write_errors = speaker.write_errors,
            .speaker_codec_restarts = speaker.codec_restarts,
            .speaker_queued_blocks = speaker.queued_blocks,
            .squelch_enabled = ctl.squelch_enabled,
            .squelch_open = squelch_open,
            .squelch_open_dbm_x2 = ctl.squelch_open_dbm_x2,
            .squelch_hysteresis_db_x2 = ctl.squelch_hysteresis_db_x2,
            .scanning = online && ctl.scanning,
            .scan_plan = ctl.scan_plan,
            .scan_wraps = ctl.scan_wraps,
            .updated_us = now,
        };
        portENTER_CRITICAL(&s_diag_mux);
        s_diag = snap;
        portEXIT_CRITICAL(&s_diag_mux);
        if (rf_valid) {
            ESP_LOGI(TAG,
                     "online=1 actual=%" PRIu32 ".%03" PRIu32
                     "MHz scan=%d sq_enabled=%d sq_open=%d r30=%04X "
                     "rssi=%u(%d.%sdBm) noise=%u glitch=%u frames=%" PRIu32
                     " L[nz=%" PRIu32 " min=%" PRId32 " max=%" PRId32
                     " rms=%" PRIu32 "] R[nz=%" PRIu32 " min=%" PRId32
                     " max=%" PRId32 " rms=%" PRIu32 "] rd_err=%" PRIu32
                     " spk=%d mute=%d pa=%d vol=%u%% out=%" PRIu32
                     " q=%" PRIu32 " drop=%" PRIu32 " wr_err=%" PRIu32,
                     snap.frequency_hz / 1000000u,
                     (snap.frequency_hz % 1000000u) / 1000u, snap.scanning,
                     ctl.squelch_enabled, squelch_open, r30_after,
                     d.rssi_raw, d.rssi_dbm_x2 / 2,
                     (d.rssi_dbm_x2 & 1) ? "5" : "0", d.noise_raw,
                     d.glitch_raw, audio.frames, audio.left_nonzero,
                     audio.frames ? audio.left_min : 0,
                     audio.frames ? audio.left_max : 0, left_rms,
                     audio.right_nonzero,
                     audio.frames ? audio.right_min : 0,
                     audio.frames ? audio.right_max : 0, right_rms,
                     read_errors, speaker.online, speaker.muted,
                     speaker.pa_enabled, speaker.volume, speaker.output_frames,
                     speaker.queued_blocks, speaker.queue_drops,
                     speaker.write_errors);
        } else {
            ESP_LOGI(TAG,
                     "online=0 target=%" PRIu32 ".%03" PRIu32
                     "MHz scan_requested=%d rf=UNAVAILABLE "
                     "sq_enabled=%d sq_open=0 r30=%04X/%04X frames=%" PRIu32
                     " L_rms=%" PRIu32 " R_rms=%" PRIu32
                     " rd_err=%" PRIu32 " spk=%d mute=%d pa=%d vol=%u%%"
                     " drop=%" PRIu32 " wr_err=%" PRIu32,
                     ctl.requested_hz / 1000000u,
                     (ctl.requested_hz % 1000000u) / 1000u, ctl.scanning,
                     ctl.squelch_enabled, r30_before, r30_after,
                     audio.frames, left_rms, right_rms, read_errors,
                     speaker.online, speaker.muted, speaker.pa_enabled,
                     speaker.volume,
                     speaker.queue_drops, speaker.write_errors);
        }
        pk_airband_audio_stats_reset(&audio);
        read_errors = 0;
    }
}

esp_err_t pk_airband_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << VHF_SCN_GPIO) | (1ULL << VHF_SCK_GPIO) |
                        (1ULL << VHF_SDATA_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    gpio_set_level(VHF_SCN_GPIO, 1);
    gpio_set_level(VHF_SCK_GPIO, 1);
    gpio_set_level(VHF_SDATA_GPIO, 1);

    if ((err = audio_init()) != ESP_OK) {
        ESP_LOGW(TAG, "PCM1808 I2S init failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_err_t speaker_err = pk_airband_speaker_start();
    if (speaker_err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 speaker start failed: %s; RX diagnostics continue",
                 esp_err_to_name(speaker_err));
    }
    if (xTaskCreate(task, "airband", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "task started: RX-only 121.500MHz, PCM1808 48k/packed-24 stereo");
    return ESP_OK;
}

bool pk_airband_get(pk_airband_state_t *out)
{
    if (out == NULL) return false;
    portENTER_CRITICAL(&s_diag_mux);
    *out = s_diag;
    portEXIT_CRITICAL(&s_diag_mux);
    if (out->updated_us == 0) {
        *out = (pk_airband_state_t){0};
        return false;
    }
    return true;
}

bool pk_airband_set_frequency(uint32_t frequency_hz)
{
    uint32_t unused;
    if (!pk_airband_frequency_word(frequency_hz, &unused)) return false;
    portENTER_CRITICAL(&s_diag_mux);
    s_control.requested_hz = frequency_hz;
    s_control.requested_channel_khz = 0;
    s_control.scanning = false;
    portEXIT_CRITICAL(&s_diag_mux);
    return true;
}

bool pk_airband_set_channel(uint32_t channel_khz)
{
    uint32_t carrier_hz = 0;
    if (!pk_airband_channel_to_carrier_hz(channel_khz, &carrier_hz))
        return false;
    portENTER_CRITICAL(&s_diag_mux);
    s_control.requested_hz = carrier_hz;
    s_control.requested_channel_khz = channel_khz;
    s_control.scanning = false;
    portEXIT_CRITICAL(&s_diag_mux);
    return true;
}

bool pk_airband_scan_start(const pk_airband_scan_plan_t *plan)
{
    if (!pk_airband_scan_plan_valid(plan)) return false;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_diag_mux);
    s_control.scan_plan = *plan;
    s_control.requested_hz = plan->start_hz;
    s_control.requested_channel_khz = 0;
    s_control.scan_wraps = 0;
    s_control.scan_advance_at_us =
        now + (int64_t)plan->dwell_ms * 1000;
    s_control.scanning = true;
    portEXIT_CRITICAL(&s_diag_mux);
    return true;
}

void pk_airband_scan_stop(void)
{
    portENTER_CRITICAL(&s_diag_mux);
    s_control.scanning = false;
    portEXIT_CRITICAL(&s_diag_mux);
}

bool pk_airband_set_squelch(int16_t open_dbm_x2,
                            uint16_t hysteresis_db_x2)
{
    if (open_dbm_x2 < -280 || open_dbm_x2 > -40 ||
        hysteresis_db_x2 > 80) {
        return false;
    }
    portENTER_CRITICAL(&s_diag_mux);
    s_control.squelch_open_dbm_x2 = open_dbm_x2;
    s_control.squelch_hysteresis_db_x2 = hysteresis_db_x2;
    portEXIT_CRITICAL(&s_diag_mux);
    return true;
}

void pk_airband_set_squelch_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_diag_mux);
    s_control.squelch_enabled = enabled;
    portEXIT_CRITICAL(&s_diag_mux);
}

bool pk_airband_control_get(pk_airband_control_status_t *out)
{
    if (out == NULL) return false;
    /* Queue inspection may enter the scheduler, so take it outside the
     * s_diag_mux critical section. */
    pk_airband_speaker_status_t speaker = {0};
    const bool speaker_started = pk_airband_speaker_get(&speaker);
    portENTER_CRITICAL(&s_diag_mux);
    *out = (pk_airband_control_status_t){
        .requested_hz = s_control.requested_hz,
        .requested_channel_khz = s_control.requested_channel_khz,
        .scanning = s_control.scanning,
        .scan_plan = s_control.scan_plan,
        .squelch_enabled = s_control.squelch_enabled,
        .squelch_open_dbm_x2 = s_control.squelch_open_dbm_x2,
        .squelch_hysteresis_db_x2 = s_control.squelch_hysteresis_db_x2,
        .speaker_muted = speaker_started ? speaker.muted :
                                           pk_airband_speaker_is_muted(),
        .speaker_volume = speaker_started ? speaker.volume : 0,
    };
    portEXIT_CRITICAL(&s_diag_mux);
    return true;
}

void pk_airband_set_speaker_muted(bool muted)
{
    pk_airband_speaker_set_muted(muted);
}

void pk_airband_set_speaker_volume(uint8_t volume)
{
    pk_airband_speaker_set_volume(volume);
}

bool pk_airband_speaker_test(void)
{
    return pk_airband_speaker_test_tone();
}

pk_airband_speaker_test_click_t pk_airband_speaker_test_action(void)
{
    return pk_airband_speaker_test_tone_action();
}

bool pk_airband_speaker_test_active(void)
{
    return pk_airband_speaker_test_tone_active();
}
