/*
 * imu_task.c — BNO085 minimal SH-2 client + 100 Hz attitude polling.
 *
 * SH-2 / SHTP framing summary (for the I²C transport):
 *
 *   ┌──────────┬──────────┬─────────┬──────┬────────────┐
 *   │ len LSB  │ len MSB  │ channel │ seq# │   cargo …  │
 *   └──────────┴──────────┴─────────┴──────┴────────────┘
 *      0           1           2        3        4..
 *
 *   - length is total frame size including the 4 header bytes
 *   - bit 15 of len MSB is the "continuation" flag (we don't see it
 *     for any reports we use, so it's safe to mask off)
 *   - channels we care about:
 *       0: command (advertisement / reset on boot)
 *       1: executable (reset complete, etc.)
 *       2: control   (Set/Get Feature Command, Feature Response)
 *       3: sensorhub (input reports — the actual sensor data)
 *
 * We send exactly one Set Feature Command on channel 2 (enable
 * Rotation Vector at 100 Hz) and then drain reports off channel 3.
 * Everything else BNO085 sends is logged at DEBUG and dropped.
 *
 * References:
 *   - Hillcrest / CEVA SH-2 Reference Manual (Sensor Hub Protocol)
 *   - SparkFun BNO080_Arduino_Library (MIT licensed reference)
 *   - Bosch / Hillcrest BNO080 datasheet 1000-3251 rev 1.10
 */

#include "imu_task.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

static const char *TAG = "imu";

/* --- Hardware wiring (mirrors docs/hardware/board_pinout.md) --------- */
#define IMU_I2C_PORT             I2C_NUM_0
#define IMU_I2C_SDA              7
#define IMU_I2C_SCL              8
#define IMU_I2C_HZ               400000
#define IMU_I2C_ADDR             0x4A
#define IMU_PIN_INT              20    /* unused in polled mode (Phase 4b first cut) */
#define IMU_PIN_RST              21

/* --- SH-2 constants -------------------------------------------------- */
#define SHTP_CH_COMMAND          0
#define SHTP_CH_EXECUTABLE       1
#define SHTP_CH_CONTROL          2
#define SHTP_CH_SENSORHUB        3
#define SHTP_HEADER_LEN          4
#define SHTP_MAX_PAYLOAD         128   /* plenty for Rotation Vector + slack */

#define SH2_CMD_SET_FEATURE      0xFD
#define SH2_REPORT_ROTATION_VECTOR  0x05

/* Set Feature Command payload layout — 17 bytes (see SH-2 ref §6.5.4). */
typedef struct __attribute__((packed)) {
    uint8_t  report_id;          /* 0xFD */
    uint8_t  feature_id;         /* 0x05 = Rotation Vector */
    uint8_t  feature_flags;      /* 0 */
    int16_t  change_sensitivity; /* 0 */
    uint32_t report_interval_us; /* 10000 = 100 Hz */
    uint32_t batch_interval_us;  /* 0 */
    uint32_t sensor_specific;    /* 0 */
} sh2_set_feature_t;

/* --- Module state ---------------------------------------------------- */
static i2c_master_bus_handle_t    s_bus;
static i2c_master_dev_handle_t    s_dev;
static SemaphoreHandle_t          s_sample_lock;
static pk_imu_sample_t            s_sample;
static uint8_t                    s_tx_seq[6];   /* per-channel SHTP sequence */

/* --- I²C bring-up ---------------------------------------------------- */
static esp_err_t i2c_bring_up(void)
{
    /* Bus is shared with the ES8311 codec. esp_driver_i2c happily lets
     * us re-init the bus if no one else has — both code paths in the
     * firmware end up here exactly once, so a fresh init is fine. */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = IMU_I2C_PORT,
        .sda_io_num = IMU_I2C_SDA,
        .scl_io_num = IMU_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMU_I2C_ADDR,
        .scl_speed_hz = IMU_I2C_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
}

/* --- BNO085 hard reset ----------------------------------------------- */
static void bno_reset_pulse(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << IMU_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(IMU_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(IMU_PIN_RST, 1);
    /* BNO085 boot takes ~200 ms; SH-2 advertisement follows. */
    vTaskDelay(pdMS_TO_TICKS(250));
}

/* --- SHTP I/O -------------------------------------------------------- */
static esp_err_t shtp_send(uint8_t channel, const uint8_t *cargo, size_t cargo_len)
{
    if (cargo_len > SHTP_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    uint8_t  frame[SHTP_HEADER_LEN + SHTP_MAX_PAYLOAD];
    uint16_t total = (uint16_t)(SHTP_HEADER_LEN + cargo_len);
    frame[0] = total & 0xFF;
    frame[1] = (total >> 8) & 0x7F;   /* clear continuation bit */
    frame[2] = channel;
    frame[3] = s_tx_seq[channel]++;
    if (cargo_len > 0) memcpy(frame + SHTP_HEADER_LEN, cargo, cargo_len);
    return i2c_master_transmit(s_dev, frame, total, 100);
}

/* Read one SHTP frame. *out_channel and *out_cargo_len are populated;
 * cargo bytes are copied into `out_cargo`. Returns ESP_ERR_NOT_FOUND if
 * the sensor reported no data (length == 0 in the header). */
static esp_err_t shtp_recv(uint8_t *out_cargo, size_t out_cap,
                           uint8_t *out_channel, size_t *out_cargo_len)
{
    uint8_t hdr[SHTP_HEADER_LEN];
    esp_err_t err = i2c_master_receive(s_dev, hdr, sizeof(hdr), 100);
    if (err != ESP_OK) return err;

    uint16_t length = ((uint16_t)hdr[1] & 0x7F) << 8 | hdr[0];
    if (length == 0 || length == 0xFFFF) return ESP_ERR_NOT_FOUND;
    if (length < SHTP_HEADER_LEN) return ESP_ERR_INVALID_RESPONSE;

    size_t cargo_len = (size_t)(length - SHTP_HEADER_LEN);
    if (cargo_len > out_cap) cargo_len = out_cap;

    /* Re-issue an I²C read of the full frame; BNO085 wants the whole
     * thing in one transaction, header included, so we read length
     * bytes and skip the header in-place. */
    uint8_t scratch[SHTP_MAX_PAYLOAD + SHTP_HEADER_LEN];
    if (length > sizeof(scratch)) length = sizeof(scratch);
    err = i2c_master_receive(s_dev, scratch, length, 100);
    if (err != ESP_OK) return err;

    *out_channel = scratch[2];
    *out_cargo_len = (size_t)(length - SHTP_HEADER_LEN);
    if (*out_cargo_len > out_cap) *out_cargo_len = out_cap;
    memcpy(out_cargo, scratch + SHTP_HEADER_LEN, *out_cargo_len);
    return ESP_OK;
}

/* --- Set Feature: Rotation Vector at 100 Hz -------------------------- */
static esp_err_t bno_enable_rotation_vector(void)
{
    sh2_set_feature_t cmd = {
        .report_id           = SH2_CMD_SET_FEATURE,
        .feature_id          = SH2_REPORT_ROTATION_VECTOR,
        .feature_flags       = 0,
        .change_sensitivity  = 0,
        .report_interval_us  = 10000,    /* 100 Hz */
        .batch_interval_us   = 0,
        .sensor_specific     = 0,
    };
    return shtp_send(SHTP_CH_CONTROL, (const uint8_t *)&cmd, sizeof(cmd));
}

/* --- Quaternion → Euler (ZYX Tait-Bryan, aerospace convention) ------ */
static void quat_to_euler(float qi, float qj, float qk, float qw,
                          float *roll_deg, float *pitch_deg, float *yaw_deg)
{
    /* All formulas below assume a right-handed coordinate frame where
     * +X is forward, +Y is right, +Z is down (aerospace NED). The
     * BNO085 in default orientation reports a quaternion in NED, so
     * this is direct. */
    float sinr_cosp = 2.0f * (qw * qi + qj * qk);
    float cosr_cosp = 1.0f - 2.0f * (qi * qi + qj * qj);
    *roll_deg = atan2f(sinr_cosp, cosr_cosp) * (180.0f / (float)M_PI);

    float sinp = 2.0f * (qw * qj - qk * qi);
    if (sinp >  1.0f) sinp =  1.0f;
    if (sinp < -1.0f) sinp = -1.0f;
    *pitch_deg = asinf(sinp) * (180.0f / (float)M_PI);

    float siny_cosp = 2.0f * (qw * qk + qi * qj);
    float cosy_cosp = 1.0f - 2.0f * (qj * qj + qk * qk);
    float y = atan2f(siny_cosp, cosy_cosp) * (180.0f / (float)M_PI);
    if (y < 0) y += 360.0f;
    *yaw_deg = y;
}

/* --- Rotation Vector report parser ---------------------------------- *
 *
 * BNO085 prefixes every channel-3 frame with a 5-byte "Sensor Hub
 * Timestamp Reference" (report ID 0xFB) header, then the actual report
 * follows. For Rotation Vector (0x05) the layout is:
 *
 *   Byte 0:  reportID = 0x05
 *   Byte 1:  sequence
 *   Byte 2:  status (bits 0-1 = accuracy)
 *   Byte 3:  delay
 *   Byte 4-5:  i quaternion (Q14 LE)
 *   Byte 6-7:  j quaternion (Q14 LE)
 *   Byte 8-9:  k quaternion (Q14 LE)
 *   Byte 10-11: real quaternion (Q14 LE)
 *   Byte 12-13: accuracy estimate (Q12 LE radians) — optional
 *
 * If the report we get doesn't have the timestamp ref preamble (some
 * SH-2 firmwares omit it for stream reports), we'll find the report ID
 * at offset 0; otherwise it's at offset 5. We tolerate both.
 */
static bool parse_rotation_vector(const uint8_t *cargo, size_t cargo_len)
{
    const uint8_t *p = cargo;
    size_t remaining = cargo_len;

    /* Strip the 5-byte timestamp ref if present (reportID 0xFB). */
    if (remaining >= 5 && p[0] == 0xFB) {
        p += 5;
        remaining -= 5;
    }
    if (remaining < 12) return false;
    if (p[0] != SH2_REPORT_ROTATION_VECTOR) return false;

    int16_t qi_raw = (int16_t)((uint16_t)p[5] << 8 | p[4]);
    int16_t qj_raw = (int16_t)((uint16_t)p[7] << 8 | p[6]);
    int16_t qk_raw = (int16_t)((uint16_t)p[9] << 8 | p[8]);
    int16_t qw_raw = (int16_t)((uint16_t)p[11] << 8 | p[10]);
    uint8_t status = p[2];

    const float Q14 = 1.0f / (float)(1 << 14);
    float qi = (float)qi_raw * Q14;
    float qj = (float)qj_raw * Q14;
    float qk = (float)qk_raw * Q14;
    float qw = (float)qw_raw * Q14;

    float roll, pitch, yaw;
    quat_to_euler(qi, qj, qk, qw, &roll, &pitch, &yaw);

    xSemaphoreTake(s_sample_lock, portMAX_DELAY);
    s_sample.ts_us     = esp_timer_get_time();
    s_sample.roll_deg  = roll;
    s_sample.pitch_deg = pitch;
    s_sample.yaw_deg   = yaw;
    s_sample.accuracy  = status & 0x03;
    s_sample.valid     = true;
    xSemaphoreGive(s_sample_lock);
    return true;
}

/* --- IMU polling task ------------------------------------------------ */
static void imu_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "imu_task running (polling @ 200 Hz)");

    uint8_t cargo[SHTP_MAX_PAYLOAD];
    int64_t last_log_us = 0;
    uint32_t valid_count = 0;
    uint32_t parse_fail  = 0;

    while (1) {
        uint8_t channel;
        size_t  cargo_len = 0;
        esp_err_t err = shtp_recv(cargo, sizeof(cargo), &channel, &cargo_len);
        if (err == ESP_OK && channel == SHTP_CH_SENSORHUB) {
            if (parse_rotation_vector(cargo, cargo_len)) valid_count++;
            else parse_fail++;
        } else if (err == ESP_OK) {
            ESP_LOGD(TAG, "shtp ch=%u cargo=%u (ignored)",
                     channel, (unsigned)cargo_len);
        }

        int64_t now = esp_timer_get_time();
        if (now - last_log_us >= 1000000) {
            pk_imu_sample_t s;
            pk_imu_sample_get(&s);
            ESP_LOGI(TAG, "rpy = %+7.2f / %+7.2f / %7.2f  "
                          "(acc=%u valid=%lu parse_fail=%lu)",
                     s.roll_deg, s.pitch_deg, s.yaw_deg,
                     s.accuracy,
                     (unsigned long)valid_count,
                     (unsigned long)parse_fail);
            valid_count = 0;
            parse_fail = 0;
            last_log_us = now;
        }
        vTaskDelay(pdMS_TO_TICKS(5));   /* 200 Hz polling */
    }
}

/* --- Public init ----------------------------------------------------- */
bool pk_imu_sample_get(pk_imu_sample_t *out)
{
    if (out == NULL || s_sample_lock == NULL) return false;
    xSemaphoreTake(s_sample_lock, portMAX_DELAY);
    *out = s_sample;
    bool valid = s_sample.valid;
    xSemaphoreGive(s_sample_lock);
    return valid;
}

esp_err_t pk_imu_init(void)
{
    s_sample_lock = xSemaphoreCreateMutex();
    if (s_sample_lock == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = i2c_bring_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_bring_up: %s", esp_err_to_name(err));
        return err;
    }

    bno_reset_pulse();

    /* Drain whatever SH-2 sent us during boot (advertisement +
     * Reset Complete + a couple of internal acks). 500 ms is plenty
     * even on slow firmware revisions. */
    uint8_t  scratch[SHTP_MAX_PAYLOAD];
    uint8_t  ch;
    size_t   clen;
    int64_t  deadline = esp_timer_get_time() + 500000;
    while (esp_timer_get_time() < deadline) {
        if (shtp_recv(scratch, sizeof(scratch), &ch, &clen) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    err = bno_enable_rotation_vector();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable_rotation_vector: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "BNO085 rotation vector enabled @ 100 Hz");

    BaseType_t ok = xTaskCreatePinnedToCore(
        imu_task, "imu", 4096, NULL, 5, NULL, 0);
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}
