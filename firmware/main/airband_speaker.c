#include "airband_speaker.h"

#include <stdatomic.h>
#include <string.h>

#include "audio_codec_if.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "airband_bk4819.h"
#include "airband_speaker_policy.h"
#include "pk_codec_ctrl.h"
#include "pk_i2c0_bus.h"

#define SPK_MCLK_GPIO GPIO_NUM_13
#define SPK_BCLK_GPIO GPIO_NUM_12
#define SPK_LRCK_GPIO GPIO_NUM_10
#define SPK_DOUT_GPIO GPIO_NUM_9
#define SPK_PA_GPIO   GPIO_NUM_53
#define SPK_RATE_HZ   48000u
#define SPK_QUEUE_DEPTH 4u
#define SPK_DEFAULT_VOLUME 35u
#define ES8311_ADDR7 0x18u
#define SPK_RETRY_MS 2000u

typedef struct {
    uint16_t frames;
    int16_t pcm[PK_AIRBAND_I2S_DMA_FRAME_NUM * 2u];
} speaker_block_t;

static const char *TAG = "airband_spk";
static QueueHandle_t s_queue;
static i2s_chan_handle_t s_tx;
static const audio_codec_if_t *s_codec;
/* GPIO interface has process lifetime. Reusing it prevents a heap leak every
 * time I2C recovery replays the ES8311 codec object. */
static const audio_codec_gpio_if_t *s_gpio_if;

static atomic_bool s_online;
static atomic_bool s_muted;
static atomic_bool s_tone_request;
static atomic_bool s_tone_cancel;
static atomic_bool s_tone_active;
static atomic_bool s_pa_enabled;
static atomic_bool s_program_playable;
static atomic_uchar s_volume = SPK_DEFAULT_VOLUME;
static atomic_uint s_control_generation;
static atomic_uint s_output_frames;
static atomic_uint s_queue_drops;
static atomic_uint s_write_errors;
static atomic_uint s_codec_restarts;
static atomic_uint s_test_result;
static atomic_uint s_test_expected_frames;
static atomic_uint s_test_written_frames;

static void queue_drain(void)
{
    speaker_block_t drop;
    while (s_queue != NULL && xQueueReceive(s_queue, &drop, 0) == pdTRUE) { }
}

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                        I2S_ROLE_MASTER);
    chan.dma_desc_num = 8;
    chan.dma_frame_num = PK_AIRBAND_I2S_DMA_FRAME_NUM;
    esp_err_t err = i2s_new_channel(&chan, &s_tx, NULL);
    if (err != ESP_OK) return err;
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = SPK_MCLK_GPIO,
            .bclk = SPK_BCLK_GPIO,
            .ws = SPK_LRCK_GPIO,
            .dout = SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if ((err = i2s_channel_init_std_mode(s_tx, &cfg)) != ESP_OK) {
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
        return err;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    return err;
}

static void i2s_cleanup(void)
{
    if (s_tx == NULL) return;
    (void)i2s_channel_disable(s_tx);
    (void)i2s_del_channel(s_tx);
    s_tx = NULL;
}

static esp_err_t write_pcm(const int16_t *pcm, size_t frames)
{
    size_t wrote = 0;
    const size_t bytes = frames * 2u * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(s_tx, pcm, bytes, &wrote, 100);
    if (err != ESP_OK || wrote != bytes) {
        atomic_fetch_add(&s_write_errors, 1);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    atomic_fetch_add(&s_output_frames, (unsigned)frames);
    return ESP_OK;
}

static int pa_set(bool enabled)
{
    const esp_err_t err = gpio_set_level(SPK_PA_GPIO, enabled ? 1 : 0);
    const bool observed = err == ESP_OK &&
                          gpio_get_level(SPK_PA_GPIO) == (enabled ? 1 : 0);
    atomic_store(&s_pa_enabled, observed && enabled);
    if (!observed) {
        ESP_LOGE(TAG, "PA GPIO53 set=%d failed: rc=%s read=%d", enabled,
                 esp_err_to_name(err), gpio_get_level(SPK_PA_GPIO));
        return ESP_CODEC_DEV_WRITE_FAIL;
    }
    return ESP_CODEC_DEV_OK;
}

static void codec_shutdown(void)
{
    atomic_store(&s_online, false);
    pk_airband_speaker_set_program_playable(false);
    (void)pa_set(false);
    if (s_codec != NULL) {
        if (s_codec->enable) (void)s_codec->enable(s_codec, false);
        audio_codec_delete_codec_if(s_codec);
        s_codec = NULL;
    }
    (void)pa_set(false);
}

/* ES8311 的 enable() 会主动解除 DAC 静音。PA 由本模块单独管理，因此先在
 * codec 内静音/解除静音，最后才切 GPIO53，避免 I2C 恢复或用户静音时出现
 * 短促爆音。 */
static int apply_mute_and_pa(bool tone_active)
{
    const bool audible = pk_airband_speaker_pa_enabled(
        atomic_load(&s_online), atomic_load(&s_muted),
        atomic_load(&s_program_playable), tone_active);
    int pa_rc = ESP_CODEC_DEV_OK;
    if (!audible) pa_rc = pa_set(false);
    const int mute_rc = s_codec->mute(s_codec, !audible);
    if (mute_rc == ESP_CODEC_DEV_OK && audible) pa_rc = pa_set(true);
    if (mute_rc != ESP_CODEC_DEV_OK) (void)pa_set(false);
    return mute_rc != ESP_CODEC_DEV_OK ? mute_rc : pa_rc;
}

static esp_err_t codec_bringup(void)
{
    (void)pa_set(false);
    const audio_codec_ctrl_if_t *ctrl = pk_codec_ctrl_init(ES8311_ADDR7);
    if (ctrl == NULL) return ESP_ERR_NOT_FOUND;
    if (s_gpio_if == NULL) s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == NULL) return ESP_ERR_NO_MEM;
    es8311_codec_cfg_t cfg = {
        .ctrl_if = ctrl,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        /* PA 必须在静音状态确定后才开；禁用驱动内建的 enable() 联动，
         * GPIO53 由 apply_mute_and_pa() 以明确顺序控制。 */
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
        .mclk_div = 256,
    };
    s_codec = es8311_codec_new(&cfg);
    if (s_codec == NULL) return ESP_FAIL;
    esp_codec_dev_sample_info_t sample = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = SPK_RATE_HZ,
        .mclk_multiple = 256,
    };
    int rc = s_codec->set_fs(s_codec, &sample);
    if (rc == ESP_CODEC_DEV_OK)
        rc = s_codec->set_vol(
            s_codec, pk_airband_speaker_volume_db(atomic_load(&s_volume)));
    if (rc == ESP_CODEC_DEV_OK)
        rc = s_codec->mute(s_codec, true);
    if (rc != ESP_CODEC_DEV_OK) {
        codec_shutdown();
        return ESP_FAIL;
    }

    static const int16_t silence[PK_AIRBAND_I2S_DMA_FRAME_NUM * 2u] = {0};
    if (write_pcm(silence, PK_AIRBAND_I2S_DMA_FRAME_NUM) != ESP_OK) {
        codec_shutdown();
        return ESP_FAIL;
    }
    rc = s_codec->enable(s_codec, true);
    if (rc == ESP_CODEC_DEV_OK)
        rc = s_codec->mute(s_codec, true);
    if (rc != ESP_CODEC_DEV_OK) {
        codec_shutdown();
        return ESP_FAIL;
    }
    int reg00 = -1;
    if (s_codec->get_reg(s_codec, 0x00, &reg00) != ESP_CODEC_DEV_OK) {
        codec_shutdown();
        return ESP_FAIL;
    }
    atomic_store(&s_online, true);
    if (apply_mute_and_pa(false) != ESP_CODEC_DEV_OK) {
        codec_shutdown();
        return ESP_FAIL;
    }
    queue_drain();
    ESP_LOGI(TAG, "ES8311 online: I2S0 48k/16-bit stereo, reg00=0x%02X, "
                  "PA GPIO53 follows playable gate, volume=%u%%",
             reg00 & 0xFF, atomic_load(&s_volume));
    return ESP_OK;
}

static void fill_tone(speaker_block_t *b, uint32_t *phase, uint32_t frames)
{
    b->frames = (uint16_t)frames;
    for (uint32_t i = 0; i < frames; i++) {
        const int16_t v = pk_airband_speaker_tone_sample(*phase);
        b->pcm[i * 2u] = v;
        b->pcm[i * 2u + 1u] = v;
        (*phase)++;
    }
}

static void task(void *arg)
{
    (void)arg;
    uint32_t bus_generation = pk_i2c0_bus_generation();
    uint32_t applied_control = atomic_load(&s_control_generation);
    uint32_t tone_remaining = 0;
    uint32_t tone_phase = 0;
    uint32_t tone_written = 0;
    uint32_t tone_error_base = 0;
    bool tone_setup_ok = false;
    int64_t retry_at_us = 0;
    speaker_block_t block;

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        const uint32_t gen = pk_i2c0_bus_generation();
        if (gen != bus_generation) {
            bus_generation = gen;
            if (tone_remaining != 0 || atomic_load(&s_tone_active)) {
                const uint32_t errors = atomic_load(&s_write_errors) -
                                        tone_error_base;
                atomic_store(&s_test_written_frames, tone_written);
                atomic_store(&s_test_result, pk_airband_speaker_tone_result(
                    false, SPK_RATE_HZ, tone_written, errors));
                tone_remaining = 0;
                atomic_store(&s_tone_request, false);
                atomic_store(&s_tone_cancel, false);
                atomic_store(&s_tone_active, false);
                ESP_LOGE(TAG, "speaker test FAIL: I2C recovery interrupted it, "
                              "expected=%u written=%u errors=%u",
                         SPK_RATE_HZ, tone_written, errors);
            }
            codec_shutdown();
            atomic_fetch_add(&s_codec_restarts, 1);
            retry_at_us = now_us;
            ESP_LOGW(TAG, "I2C0 generation changed to %lu; replaying ES8311 init",
                     (unsigned long)gen);
        }
        if (!atomic_load(&s_online)) {
            if (now_us >= retry_at_us && codec_bringup() != ESP_OK) {
                (void)pa_set(false);
                retry_at_us = now_us + (int64_t)SPK_RETRY_MS * 1000;
                ESP_LOGW(TAG, "ES8311 bring-up failed; PA held low, retry in %ums",
                         SPK_RETRY_MS);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (atomic_exchange(&s_tone_cancel, false)) {
            const bool was_active = atomic_exchange(&s_tone_active, false);
            const bool start_pending = atomic_exchange(&s_tone_request, false);
            tone_remaining = 0;
            queue_drain();
            if (was_active) {
                const uint32_t stopped_written =
                    pk_airband_speaker_cancel_written_frames(start_pending,
                                                              tone_written);
                atomic_store(&s_test_written_frames, stopped_written);
                atomic_store(&s_test_result,
                             PK_AIRBAND_SPEAKER_TEST_STOPPED);
                int restore_rc = s_codec->set_vol(
                    s_codec,
                    pk_airband_speaker_volume_db(atomic_load(&s_volume)));
                restore_rc |= apply_mute_and_pa(false);
                if (restore_rc != ESP_CODEC_DEV_OK)
                    atomic_fetch_add(&s_write_errors, 1);
                ESP_LOGI(TAG, "local speaker test STOPPED: written=%u PA=%d",
                         stopped_written, atomic_load(&s_pa_enabled));
            }
            continue;
        }

        const uint32_t control = atomic_load(&s_control_generation);
        if (control != applied_control && tone_remaining == 0) {
            applied_control = control;
            int rc = s_codec->set_vol(
                s_codec, pk_airband_speaker_volume_db(atomic_load(&s_volume)));
            rc |= apply_mute_and_pa(false);
            if (rc != ESP_CODEC_DEV_OK) atomic_fetch_add(&s_write_errors, 1);
        }

        if (atomic_exchange(&s_tone_request, false)) {
            queue_drain();
            tone_remaining = pk_airband_speaker_tone_begin(0, SPK_RATE_HZ);
            tone_phase = 0;
            tone_written = 0;
            tone_error_base = atomic_load(&s_write_errors);
            atomic_store(&s_test_expected_frames, tone_remaining);
            atomic_store(&s_test_written_frames, 0);
            atomic_store(&s_test_result, PK_AIRBAND_SPEAKER_TEST_RUNNING);
            const float user_db =
                pk_airband_speaker_volume_db(atomic_load(&s_volume));
            int tone_rc = s_codec->set_vol(
                s_codec, pk_airband_speaker_test_volume_db(user_db));
            tone_rc |= apply_mute_and_pa(true);
            tone_setup_ok = tone_rc == ESP_CODEC_DEV_OK &&
                            gpio_get_level(SPK_PA_GPIO) == 1;
            if (!tone_setup_ok) {
                atomic_fetch_add(&s_write_errors, 1);
                tone_remaining = 0;
            }
            ESP_LOGI(TAG, "local speaker test: 1kHz -3dBFS for 1s "
                          "codec_gain=%.1fdB set_vol_rc=%d PA=%d "
                          "setup=%s (RF untouched)",
                     pk_airband_speaker_test_volume_db(user_db),
                     tone_rc, gpio_get_level(SPK_PA_GPIO),
                     tone_setup_ok ? "OK" : "FAIL");
        }
        if (tone_remaining != 0) {
            const uint32_t n = tone_remaining > PK_AIRBAND_I2S_DMA_FRAME_NUM
                                 ? PK_AIRBAND_I2S_DMA_FRAME_NUM : tone_remaining;
            fill_tone(&block, &tone_phase, n);
            const esp_err_t write_err = write_pcm(block.pcm, n);
            if (write_err == ESP_OK) {
                tone_written += n;
                tone_remaining -= n;
                atomic_store(&s_test_written_frames, tone_written);
            } else {
                ESP_LOGE(TAG, "speaker test I2S write failed: %s",
                         esp_err_to_name(write_err));
                tone_remaining = 0;
            }
            if (tone_remaining == 0) {
                queue_drain();
                atomic_store(&s_tone_active, false);
                int restore_rc = s_codec->set_vol(
                    s_codec,
                    pk_airband_speaker_volume_db(atomic_load(&s_volume)));
                restore_rc |= apply_mute_and_pa(false);
                if (restore_rc != ESP_CODEC_DEV_OK)
                    atomic_fetch_add(&s_write_errors, 1);
                const uint32_t errors = atomic_load(&s_write_errors) -
                                        tone_error_base;
                const pk_airband_speaker_test_result_t result =
                    pk_airband_speaker_tone_result(tone_setup_ok, SPK_RATE_HZ,
                                                   tone_written, errors);
                atomic_store(&s_test_result, result);
                ESP_LOG_LEVEL(result == PK_AIRBAND_SPEAKER_TEST_PASS
                                  ? ESP_LOG_INFO : ESP_LOG_ERROR,
                              TAG,
                              "local speaker test %s: expected=%u written=%u "
                              "errors=%u PA_after=%d",
                              result == PK_AIRBAND_SPEAKER_TEST_PASS
                                  ? "PASS" : "FAIL",
                              SPK_RATE_HZ, tone_written, errors,
                              gpio_get_level(SPK_PA_GPIO));
            }
            continue;
        }

        /* setup 失败时没有进入上面的写循环，仍要收尾并发布 FAIL。 */
        if (atomic_load(&s_tone_active) &&
            atomic_load(&s_test_result) == PK_AIRBAND_SPEAKER_TEST_RUNNING) {
            atomic_store(&s_tone_active, false);
            atomic_store(&s_test_written_frames, tone_written);
            atomic_store(&s_test_result, PK_AIRBAND_SPEAKER_TEST_FAIL);
            (void)s_codec->set_vol(
                s_codec, pk_airband_speaker_volume_db(atomic_load(&s_volume)));
            (void)apply_mute_and_pa(false);
            ESP_LOGE(TAG, "local speaker test FAIL before PCM: expected=%u "
                          "written=%u PA=%d", SPK_RATE_HZ, tone_written,
                     gpio_get_level(SPK_PA_GPIO));
        }

        if (xQueueReceive(s_queue, &block, pdMS_TO_TICKS(20)) == pdTRUE)
            (void)write_pcm(block.pcm, block.frames);
    }
}

esp_err_t pk_airband_speaker_start(void)
{
    if (s_queue != NULL) return ESP_OK;
    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << SPK_PA_GPIO,
        /* gpio_get_level() 对纯输出 pad 按 IDF 契约恒返回 0；测试音必须读回
         * PA_CTRL，所以同时开启输入。这是 18:15 实机 setup=FAIL 的根因。 */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&pa);
    if (err != ESP_OK) return err;
    if (pa_set(false) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    if ((err = i2s_init()) != ESP_OK) {
        (void)pa_set(false);
        return err;
    }
    s_queue = xQueueCreate(SPK_QUEUE_DEPTH, sizeof(speaker_block_t));
    if (s_queue == NULL) {
        i2s_cleanup();
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_muted, false);
    atomic_store(&s_program_playable, false);
    atomic_store(&s_volume, SPK_DEFAULT_VOLUME);
    if (xTaskCreate(task, "airband_spk", 6144, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        i2s_cleanup();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "speaker task started: I2S0 GPIO13/12/10/9, PA held low");
    return ESP_OK;
}

bool pk_airband_speaker_submit(const int16_t *stereo, size_t frames,
                               bool playable)
{
    if (s_queue == NULL || stereo == NULL || frames == 0 ||
        frames > PK_AIRBAND_I2S_DMA_FRAME_NUM) return false;
    const size_t queued = uxQueueMessagesWaiting(s_queue);
    const pk_airband_speaker_queue_action_t action =
        pk_airband_speaker_queue_action(queued, SPK_QUEUE_DEPTH, playable);
    if (action == PK_AIRBAND_SPEAKER_QUEUE_FLUSH) {
        queue_drain();
        return true;
    }
    if (action == PK_AIRBAND_SPEAKER_QUEUE_REJECT) return false;
    if (action == PK_AIRBAND_SPEAKER_QUEUE_DROP_OLDEST_ENQUEUE) {
        speaker_block_t drop;
        if (xQueueReceive(s_queue, &drop, 0) == pdTRUE)
            atomic_fetch_add(&s_queue_drops, 1);
    }
    speaker_block_t b = { .frames = (uint16_t)frames };
    memcpy(b.pcm, stereo, frames * 2u * sizeof(int16_t));
    if (xQueueSend(s_queue, &b, 0) != pdTRUE) {
        atomic_fetch_add(&s_queue_drops, 1);
        return false;
    }
    return true;
}

void pk_airband_speaker_set_program_playable(bool playable)
{
    const bool was_playable = atomic_exchange(&s_program_playable, playable);
    if (!playable) queue_drain();
    if (was_playable != playable)
        atomic_fetch_add(&s_control_generation, 1);
}

bool pk_airband_speaker_get(pk_airband_speaker_status_t *out)
{
    if (out == NULL) return false;
    *out = (pk_airband_speaker_status_t){
        .online = atomic_load(&s_online),
        .muted = atomic_load(&s_muted),
        .tone_active = atomic_load(&s_tone_active),
        .pa_enabled = atomic_load(&s_pa_enabled),
        .volume = atomic_load(&s_volume),
        .test_result = atomic_load(&s_test_result),
        .test_expected_frames = atomic_load(&s_test_expected_frames),
        .test_written_frames = atomic_load(&s_test_written_frames),
        .output_frames = atomic_load(&s_output_frames),
        .queue_drops = atomic_load(&s_queue_drops),
        .write_errors = atomic_load(&s_write_errors),
        .codec_restarts = atomic_load(&s_codec_restarts),
        .queued_blocks = s_queue ? uxQueueMessagesWaiting(s_queue) : 0,
    };
    return s_queue != NULL;
}

void pk_airband_speaker_set_muted(bool muted)
{
    atomic_store(&s_muted, muted);
    atomic_fetch_add(&s_control_generation, 1);
    if (muted) queue_drain();
}

bool pk_airband_speaker_is_muted(void)
{
    return atomic_load(&s_muted);
}

void pk_airband_speaker_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    atomic_store(&s_volume, volume);
    atomic_fetch_add(&s_control_generation, 1);
}

pk_airband_speaker_test_click_t pk_airband_speaker_test_tone_action(void)
{
    const pk_airband_speaker_test_click_t click =
        pk_airband_speaker_test_click(atomic_load(&s_online),
                                      atomic_load(&s_tone_active));
    if (click == PK_AIRBAND_SPEAKER_TEST_CLICK_REJECT) {
        ESP_LOGW(TAG, "speaker test request rejected: codec offline");
        return click;
    }
    if (click == PK_AIRBAND_SPEAKER_TEST_CLICK_STOP) {
        atomic_store(&s_tone_cancel, true);
        ESP_LOGI(TAG, "speaker test stop requested");
        return click;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_tone_active, &expected, true)) {
        atomic_store(&s_tone_cancel, true);
        ESP_LOGI(TAG, "speaker test became active; stop requested");
        return PK_AIRBAND_SPEAKER_TEST_CLICK_STOP;
    }
    atomic_store(&s_tone_cancel, false);
    atomic_store(&s_test_expected_frames, SPK_RATE_HZ);
    atomic_store(&s_test_written_frames, 0);
    atomic_store(&s_test_result, PK_AIRBAND_SPEAKER_TEST_RUNNING);
    atomic_store(&s_tone_request, true);
    ESP_LOGI(TAG, "speaker test request accepted");
    return PK_AIRBAND_SPEAKER_TEST_CLICK_START;
}

bool pk_airband_speaker_test_tone_active(void)
{
    return atomic_load(&s_tone_active);
}

bool pk_airband_speaker_test_tone(void)
{
    return pk_airband_speaker_test_tone_action() !=
           PK_AIRBAND_SPEAKER_TEST_CLICK_REJECT;
}
