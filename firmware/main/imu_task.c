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
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "imu";

/* --- Hardware wiring (mirrors docs/hardware/board_pinout.md) --------- */
#define IMU_I2C_PORT             I2C_NUM_0
#define IMU_I2C_SDA              7
#define IMU_I2C_SCL              8
#define IMU_I2C_HZ               400000
#define IMU_I2C_ADDR             0x4A
#define IMU_PIN_INT              34    /* J3 Pin 28 = GPIO34 (JLC PCB net IMU_INT); current driver polls, INT unused */
#define IMU_PIN_RST              28    /* moved to J3 Pin 16 (upper) for PCB routing */

/* --- SH-2 constants -------------------------------------------------- */
#define SHTP_CH_COMMAND          0
#define SHTP_CH_EXECUTABLE       1
#define SHTP_CH_CONTROL          2
#define SHTP_CH_SENSORHUB        3
#define SHTP_HEADER_LEN          4
#define SHTP_MAX_PAYLOAD         128   /* plenty for Rotation Vector + slack */

#define SH2_CMD_SET_FEATURE      0xFD
#define SH2_CMD_REQUEST          0xF2
#define SH2_REPORT_ROTATION_VECTOR  0x05

/* SH-2 Command Request codes (sent inside a 0xF2 report on the
 * control channel). See SH-2 Reference Manual §6.4. */
#define SH2_COMMAND_TARE         0x03
#define SH2_COMMAND_SAVE_DCD     0x06
#define SH2_COMMAND_CLEAR_DCD    0x0B   /* "Clear Persistent DCD" —
                                           wipes mag/gyro/accel
                                           calibration data from
                                           BNO085 internal flash so
                                           the fusion engine starts
                                           fresh on the next
                                           initialisation. */

/* Tare command (0x03) p0 subcommand values. */
#define SH2_TARE_SUB_NOW         0x00
#define SH2_TARE_SUB_PERSIST     0x01

/* Tare Now (subcommand 0x00) p1 axes mask — OR these together. */
#define SH2_TARE_AXIS_X          0x01
#define SH2_TARE_AXIS_Y          0x02
#define SH2_TARE_AXIS_Z          0x04
#define SH2_TARE_AXIS_ALL        (SH2_TARE_AXIS_X | SH2_TARE_AXIS_Y | SH2_TARE_AXIS_Z)

/* Tare Now (subcommand 0x00) p2 basis rotation vector to tare against. */
#define SH2_TARE_BASIS_ROT_VEC   0x00

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
static uint8_t                    s_cmd_seq;     /* SH-2 Command Request sequence */
static bool                       s_imu_ready;   /* true after pk_imu_init() succeeds */

/* Last raw quaternion from the BNO — stashed for the 1 Hz diagnostic
 * log only. Volatile because the 1 Hz logger reads it from the imu
 * task itself; torn reads only cause a one-frame skew in the printout,
 * not a correctness issue. */
static volatile float             s_last_raw_qw, s_last_raw_qi, s_last_raw_qj, s_last_raw_qk;

/* Last post-sandwich, pre-tare-offset aircraft quaternion. Captured
 * every frame so pk_imu_tare_now() can snapshot "current attitude"
 * without rerunning the sandwich math. Protected by s_sample_lock. */
static float                      s_last_aircraft_qw = 1.0f,
                                  s_last_aircraft_qi = 0.0f,
                                  s_last_aircraft_qj = 0.0f,
                                  s_last_aircraft_qk = 0.0f;

/* Software tare offset — LEFT-multiplied onto the post-sandwich
 * quaternion every frame (q_displayed = s_tare · q_aircraft).
 * Identity = no offset. Set by pk_imu_tare_now() /
 * pk_imu_tare_persist() to the conjugate (= inverse, for a unit
 * quaternion) of the current post-sandwich attitude, so the
 * displayed quaternion is identity at the moment of tare. Left-mul
 * decomposes the differential in the tare-time aircraft body frame,
 * matching the aerospace convention for ZYX roll/pitch/yaw — see the
 * detailed rationale in parse_rotation_vector() at the multiplication
 * call site. Protected by s_sample_lock. */
static float                      s_tare_qw = 1.0f,
                                  s_tare_qi = 0.0f,
                                  s_tare_qj = 0.0f,
                                  s_tare_qk = 0.0f;

/* NVS keys — namespace + blob key for the persisted software tare.
 * Blob layout: 4 little-endian IEEE 754 floats in (w, x, y, z) order. */
#define IMU_NVS_NAMESPACE   "pk_imu"
#define IMU_NVS_KEY_TARE    "tare_quat"

/* Forward declarations for NVS helpers used by pk_imu_init() before
 * the definitions further down. */
static esp_err_t imu_nvs_load_tare(float *w, float *x, float *y, float *z);
static esp_err_t imu_nvs_save_tare(float w, float x, float y, float z);
static esp_err_t imu_nvs_erase_tare(void);

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

/* --- SH-2 Command Request helper ------------------------------------- *
 *
 * A Command Request is a 12-byte cargo on the control channel:
 *
 *   byte 0:  0xF2  (report ID)
 *   byte 1:  command-request sequence (incrementing, separate from SHTP)
 *   byte 2:  command code (0x03 Tare, 0x06 Save DCD, ...)
 *   byte 3-11: P0..P8 (9 command-specific parameter bytes)
 *
 * BNO085 replies with a 0xF1 Command Response on the same channel,
 * carrying the same sequence number. We don't read it — the user-visible
 * confirmation is the change in attitude reports a moment later. */
static esp_err_t sh2_send_command(uint8_t command, const uint8_t params[9])
{
    uint8_t cargo[12];
    cargo[0] = SH2_CMD_REQUEST;
    cargo[1] = s_cmd_seq++;
    cargo[2] = command;
    if (params) memcpy(&cargo[3], params, 9);
    else        memset(&cargo[3], 0, 9);
    return shtp_send(SHTP_CH_CONTROL, cargo, sizeof(cargo));
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

/* --- Hamilton quaternion product: out = a · b ----------------------- *
 *
 * Used to apply a constant mounting rotation to the BNO085's Rotation
 * Vector before Euler extraction — see PK_IMU_MOUNT_QUAT_* in
 * imu_task.h for the rationale and the diagnostic recipe.
 *
 * When the mounting quaternion is identity (1,0,0,0), this function
 * is invoked with W=1, X=Y=Z=0 and the compiler's constant
 * propagation collapses the body to `*out = a`, so the no-remap
 * build path pays nothing for this hook. */
static inline void quat_mul(float aw, float ax, float ay, float az,
                            float bw, float bx, float by, float bz,
                            float *ow, float *ox, float *oy, float *oz)
{
    *ow = aw * bw - ax * bx - ay * by - az * bz;
    *ox = aw * bx + ax * bw + ay * bz - az * by;
    *oy = aw * by - ax * bz + ay * bw + az * bx;
    *oz = aw * bz + ax * by - ay * bx + az * bw;
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

    /* TEMPORARY: stash raw quaternion for the 1 Hz diagnostic log. */
    s_last_raw_qw = qw;
    s_last_raw_qi = qi;
    s_last_raw_qj = qj;
    s_last_raw_qk = qk;

    /* Sandwich transform: q_aircraft = q_world_fix · q_bno · q_body_fix.
     * Left mul re-expresses the world reference from BNO's ENU to the
     * aerospace NED that quat_to_euler() expects; right mul re-expresses
     * the body frame from chip axes to aircraft axes (the mounting
     * remap). See imu_task.h for the geometric derivation. */
    float tw, ti, tj, tk;
    quat_mul(PK_IMU_WORLD_FIX_W, PK_IMU_WORLD_FIX_X,
             PK_IMU_WORLD_FIX_Y, PK_IMU_WORLD_FIX_Z,
             qw, qi, qj, qk,
             &tw, &ti, &tj, &tk);
    float aqw, aqi, aqj, aqk;
    quat_mul(tw, ti, tj, tk,
             PK_IMU_MOUNT_QUAT_W, PK_IMU_MOUNT_QUAT_X,
             PK_IMU_MOUNT_QUAT_Y, PK_IMU_MOUNT_QUAT_Z,
             &aqw, &aqi, &aqj, &aqk);

    /* Snapshot the post-sandwich attitude under the lock so
     * pk_imu_tare_now() can read a torn-free copy when the user
     * triggers a tare; then apply the software tare offset by
     * LEFT-multiplying s_tare_* onto the current aircraft attitude.
     *
     * Left-mul (not right-mul) matters: with s_tare_* = conjugate of
     * the tare-time aircraft attitude, s_tare · q_now decomposes the
     * differential rotation in the TARE-TIME aircraft body frame, so
     * Euler extraction yields roll-around-body-X, pitch-around-body-Y,
     * yaw-around-body-Z — the aerospace convention. (We consume only
     * roll/pitch from this tared quaternion; yaw is taken from the raw
     * attitude below so the compass survives a tare.) Right-mul instead
     * expresses the differential in world NED, which produces correct
     * results only when the tare-time aircraft pose happens to be
     * level/facing-N (q_at_tare ≈ identity); for any tilted mounting
     * the world-frame decomposition tangles roll/pitch/yaw, especially
     * near pitch = ±90° gimbal lock. */
    float dqw, dqi, dqj, dqk;
    xSemaphoreTake(s_sample_lock, portMAX_DELAY);
    s_last_aircraft_qw = aqw;
    s_last_aircraft_qi = aqi;
    s_last_aircraft_qj = aqj;
    s_last_aircraft_qk = aqk;
    float tare_w = s_tare_qw, tare_x = s_tare_qi,
          tare_y = s_tare_qj, tare_z = s_tare_qk;
    xSemaphoreGive(s_sample_lock);
    quat_mul(tare_w, tare_x, tare_y, tare_z,
             aqw, aqi, aqj, aqk,
             &dqw, &dqi, &dqj, &dqk);

    /* roll/pitch come from the TARED quaternion — the software tare cages
     * the artificial horizon to the tare-time reference. yaw (heading),
     * however, is sourced from the RAW post-sandwich aircraft attitude,
     * NOT the tared one: a TARE press must cage the horizon WITHOUT
     * touching the compass. If yaw came from the tared quaternion it
     * would collapse to 0 at the tare pose (conj(q)·q = identity) and the
     * HSI/HDG would thereafter show only heading *relative* to the tare
     * reference instead of the true magnetic heading. Decoupling yaw also
     * makes the compass immune to any NVS-persisted tare offset restored
     * at boot. The tared-quaternion yaw is intentionally discarded. */
    float roll, pitch, yaw;
    float discard_yaw, discard_roll, discard_pitch;
    quat_to_euler(dqi, dqj, dqk, dqw, &roll, &pitch, &discard_yaw);
    quat_to_euler(aqi, aqj, aqk, aqw, &discard_roll, &discard_pitch, &yaw);
    (void)discard_yaw; (void)discard_roll; (void)discard_pitch;

    /* Mounting-orientation corrections — see imu_task.h for the
     * diagnostic recipe and the rationale for each knob. These
     * compile away when the corresponding define is 0. */
#if PK_IMU_MOUNT_INVERT_ROLL
    roll = -roll;
#endif
#if PK_IMU_MOUNT_INVERT_PITCH
    pitch = -pitch;
#endif
#if PK_IMU_MOUNT_INVERT_YAW
    yaw = 360.0f - yaw;
#endif
#if PK_IMU_MOUNT_YAW_OFFSET_DEG != 0
    yaw += (float)PK_IMU_MOUNT_YAW_OFFSET_DEG;
    while (yaw >= 360.0f) yaw -= 360.0f;
    while (yaw <    0.0f) yaw += 360.0f;
#endif

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

/* --- Bring-up sequence (used both at boot and by the watchdog) ------- *
 *
 * Pulses RST, drains the boot-time SHTP advertisement, and re-enables
 * the Rotation Vector report. Safe to call from any task. Resets the
 * SHTP per-channel sequence counters too — BNO085 starts back at seq=0
 * after a reset and would reject our frames if our side kept counting. */
static esp_err_t bno_bring_up(void)
{
    bno_reset_pulse();

    /* SHTP and command-request sequence numbers restart at 0 on the
     * BNO085 side after a hard reset. Mirror that on our side. */
    memset(s_tx_seq, 0, sizeof(s_tx_seq));
    s_cmd_seq = 0;

    /* Drain whatever SH-2 sent us during boot (advertisement + Reset
     * Complete + a couple of internal acks). 500 ms is plenty even on
     * slow firmware revisions. */
    uint8_t  scratch[SHTP_MAX_PAYLOAD];
    uint8_t  ch;
    size_t   clen;
    int64_t  deadline = esp_timer_get_time() + 500000;
    while (esp_timer_get_time() < deadline) {
        if (shtp_recv(scratch, sizeof(scratch), &ch, &clen) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    return bno_enable_rotation_vector();
}

/* --- IMU polling task ------------------------------------------------ *
 *
 * Polls SHTP at 200 Hz, parses Rotation Vector reports, logs a 1 Hz
 * summary with classified recv counters, and runs a watchdog that
 * re-inits the BNO085 if no valid report has arrived in IMU_STALL_TIMEOUT_US.
 *
 * Why a watchdog is needed: the BNO085 occasionally hangs mid-stream
 * (chip firmware bug or I²C bus glitch). Once that happens the chip
 * stops emitting reports entirely and there's no spontaneous recovery
 * without a hard reset on the RST line. We catch it by tracking the
 * last successful parse and pulsing RST + replaying init when the gap
 * crosses a threshold. */

#define IMU_STALL_TIMEOUT_US      (5 * 1000000LL)   /* 5 s with no valid RV report → re-init */
#define IMU_REINIT_MIN_GAP_US     (3 * 1000000LL)   /* don't retry sooner than this after a re-init attempt */

static void imu_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "imu_task running (polling @ 200 Hz, stall watchdog @ 5 s)");

    uint8_t cargo[SHTP_MAX_PAYLOAD];
    int64_t last_log_us    = 0;
    int64_t last_valid_us  = esp_timer_get_time();
    int64_t last_reinit_us = 0;

    /* Per-second counters (zeroed in the 1 Hz dump). */
    uint32_t valid_count        = 0;   /* successfully parsed RV reports */
    uint32_t parse_fail         = 0;   /* SHTP frame on CH3 but not an RV report */
    uint32_t recv_not_found     = 0;   /* shtp_recv returned ESP_ERR_NOT_FOUND (no data this tick) */
    uint32_t recv_i2c_err       = 0;   /* shtp_recv returned a real I²C bus error */
    uint32_t recv_wrong_channel = 0;   /* SHTP frame received but channel != 3 (CH 0/1/2/4 etc.) */

    while (1) {
        uint8_t channel;
        size_t  cargo_len = 0;
        esp_err_t err = shtp_recv(cargo, sizeof(cargo), &channel, &cargo_len);
        if (err == ESP_OK) {
            if (channel == SHTP_CH_SENSORHUB) {
                if (parse_rotation_vector(cargo, cargo_len)) {
                    valid_count++;
                    last_valid_us = esp_timer_get_time();
                } else {
                    parse_fail++;
                }
            } else {
                recv_wrong_channel++;
                ESP_LOGD(TAG, "shtp ch=%u cargo=%u (ignored)",
                         channel, (unsigned)cargo_len);
            }
        } else if (err == ESP_ERR_NOT_FOUND) {
            recv_not_found++;
        } else {
            recv_i2c_err++;
        }

        int64_t now = esp_timer_get_time();

        /* --- Watchdog: re-init if no valid RV report in N seconds --- */
        if (now - last_valid_us > IMU_STALL_TIMEOUT_US &&
            now - last_reinit_us > IMU_REINIT_MIN_GAP_US) {
            ESP_LOGW(TAG, "no valid RV report for %.1fs — re-init BNO085",
                     (double)(now - last_valid_us) / 1e6);

            /* Mark sample invalid so PFD knows we lost the stream. */
            xSemaphoreTake(s_sample_lock, portMAX_DELAY);
            s_sample.valid = false;
            xSemaphoreGive(s_sample_lock);

            esp_err_t bu = bno_bring_up();
            last_reinit_us = esp_timer_get_time();
            /* Reset the stall clock so we give the chip time to start
             * producing reports again before retrying. */
            last_valid_us = last_reinit_us;
            if (bu != ESP_OK) {
                ESP_LOGW(TAG, "bring-up after stall failed: %s",
                         esp_err_to_name(bu));
            } else {
                ESP_LOGI(TAG, "BNO085 re-init complete; waiting for reports");
            }
        }

        /* --- 1 Hz summary log --- */
        if (now - last_log_us >= 1000000) {
            pk_imu_sample_t s;
            pk_imu_sample_get(&s);
            ESP_LOGI(TAG, "rpy = %+7.2f / %+7.2f / %7.2f  "
                          "raw_q(w,i,j,k) = %+0.4f %+0.4f %+0.4f %+0.4f  "
                          "(acc=%u valid=%lu parse_fail=%lu "
                          "nf=%lu i2c_err=%lu wrong_ch=%lu)",
                     s.roll_deg, s.pitch_deg, s.yaw_deg,
                     s_last_raw_qw, s_last_raw_qi, s_last_raw_qj, s_last_raw_qk,
                     s.accuracy,
                     (unsigned long)valid_count,
                     (unsigned long)parse_fail,
                     (unsigned long)recv_not_found,
                     (unsigned long)recv_i2c_err,
                     (unsigned long)recv_wrong_channel);
            valid_count        = 0;
            parse_fail         = 0;
            recv_not_found     = 0;
            recv_i2c_err       = 0;
            recv_wrong_channel = 0;
            last_log_us        = now;
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

    /* NVS bring-up. Idempotent: returns ESP_ERR_INVALID_STATE if some
     * other subsystem (BLE / esp_hosted) already initialised it, which
     * is fine — we just want to make sure it's up before opening our
     * namespace below. ESP_ERR_NVS_NO_FREE_PAGES / NEW_VERSION_FOUND
     * is the standard "first boot or partition layout changed" path:
     * erase and try again. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (%s) — wiping and retrying",
                 esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK && nvs_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "nvs_flash_init: %s — tare persistence disabled",
                 esp_err_to_name(nvs_err));
        /* Non-fatal: software tare in RAM still works, just no
         * cross-reboot persistence. */
    } else {
        /* Try to restore a previously-persisted software tare. Missing
         * key on a fresh install is the normal path, not an error. */
        float w, x, y, z;
        esp_err_t load_err = imu_nvs_load_tare(&w, &x, &y, &z);
        if (load_err == ESP_OK) {
            s_tare_qw = w; s_tare_qi = x; s_tare_qj = y; s_tare_qk = z;
            ESP_LOGI(TAG, "loaded persisted software tare from NVS: "
                          "(w,i,j,k) = %+0.4f %+0.4f %+0.4f %+0.4f",
                     w, x, y, z);
        } else if (load_err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "no persisted software tare in NVS — "
                          "starting with identity (TARE long-press to save)");
        } else {
            ESP_LOGW(TAG, "load persisted software tare failed (%s) — "
                          "starting with identity",
                     esp_err_to_name(load_err));
        }
    }

    esp_err_t err = i2c_bring_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_bring_up: %s", esp_err_to_name(err));
        return err;
    }

    err = bno_bring_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bno_bring_up: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "BNO085 rotation vector enabled @ 100 Hz");

    BaseType_t ok = xTaskCreatePinnedToCore(
        imu_task, "imu", 4096, NULL, 5, NULL, 0);
    if (ok != pdTRUE) return ESP_ERR_NO_MEM;
    s_imu_ready = true;
    return ESP_OK;
}

/* --- I²C0 bus handle export ----------------------------------------- *
 *
 * 暴露 I²C0 主总线 handle,供同总线的其它 device(BMP388)复用。
 * i2c_new_master_bus() 全局只能建一次,此 handle 是唯一入口。
 * baro_task 用它 i2c_master_bus_add_device() 挂自己的 BMP388 device。
 * 返回 NULL 表示 IMU 尚未初始化(总线未建)。 */
i2c_master_bus_handle_t pk_i2c0_bus_get(void)
{
    return s_bus;
}

/* --- Tare API (software-side) --------------------------------------- *
 *
 * The user-facing tare functions don't talk to the BNO at all (with
 * one exception: factory_reset, which has to clean up legacy persisted
 * state inside BNO flash). Instead, "tare" means "set s_tare_* to the
 * conjugate of the current post-sandwich attitude" so that next frame
 * the displayed quaternion is identity.
 *
 * Why not the BNO's own SH-2 Tare command: the world-frame fix
 * (ENU→NED) and the body-frame remap (chip→aircraft) are applied as
 * a sandwich around the BNO output in parse_rotation_vector(). When
 * BNO Tare zeroes its internal output to identity, the sandwich math
 * turns that identity into q_world_fix · q_body_fix ≠ identity — the
 * decoded Euler angles hit gimbal lock at pitch = -90°. A pure-software
 * tare sidesteps that entirely. */

/* Conjugate of a unit quaternion = (w, -x, -y, -z) — represents the
 * inverse rotation. */
static inline void quat_conj(float w, float x, float y, float z,
                             float *ow, float *ox, float *oy, float *oz)
{
    *ow =  w;
    *ox = -x;
    *oy = -y;
    *oz = -z;
}

/* --- NVS load/save for the persisted software tare ------------------ */
static esp_err_t imu_nvs_load_tare(float *w, float *x, float *y, float *z)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(IMU_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    float buf[4];
    size_t len = sizeof(buf);
    err = nvs_get_blob(h, IMU_NVS_KEY_TARE, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) return err;
    if (len != sizeof(buf)) return ESP_ERR_INVALID_SIZE;

    *w = buf[0]; *x = buf[1]; *y = buf[2]; *z = buf[3];
    return ESP_OK;
}

static esp_err_t imu_nvs_save_tare(float w, float x, float y, float z)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(IMU_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    const float buf[4] = { w, x, y, z };
    err = nvs_set_blob(h, IMU_NVS_KEY_TARE, buf, sizeof(buf));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t imu_nvs_erase_tare(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(IMU_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_erase_key(h, IMU_NVS_KEY_TARE);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK; /* already absent — fine */
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* --- Legacy BNO state cleanup (factory reset only) ------------------- *
 *
 * Old firmware revisions wrote a persistent Tare and Save-DCD into BNO
 * internal flash via SH-2 commands. Those writes can survive a
 * software upgrade, so on factory_reset we still need to wipe them or
 * they leak into the new software-tare regime. Neither of these is
 * exposed in the public API anymore — they're internal helpers for
 * pk_imu_factory_reset() only. */

static esp_err_t bno_clear_persisted_tare(void)
{
    ESP_LOGI(TAG, "BNO: clearing persisted reorientation (identity quat + persist)");

    /* Tare Set Reorientation (subcommand 2) with identity quaternion
     * (qi=qj=qk=0, qw=1.0). The four components are signed 16-bit
     * Q14, little-endian, packed into P1..P8:
     *   P1/P2 = qi (LSB/MSB)   → 0x00 0x00
     *   P3/P4 = qj             → 0x00 0x00
     *   P5/P6 = qk             → 0x00 0x00
     *   P7/P8 = qw             → 0x00 0x40   (16384 in Q14 = 1.0) */
    uint8_t p[9] = {0};
    p[0] = 0x02;       /* Tare Set Reorientation */
    p[7] = 0x00;       /* qw LSB */
    p[8] = 0x40;       /* qw MSB → 0x4000 = 16384 = 1.0 in Q14 */
    esp_err_t err = sh2_send_command(SH2_COMMAND_TARE, p);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(50));

    memset(p, 0, sizeof(p));
    p[0] = SH2_TARE_SUB_PERSIST;
    return sh2_send_command(SH2_COMMAND_TARE, p);
}

static esp_err_t bno_clear_persisted_dcd(void)
{
    ESP_LOGI(TAG, "BNO: clearing persisted DCD (mag/gyro/accel zero offsets)");

    /* SH-2 Command 0x0B "Clear Persistent DCD" takes no parameters.
     * BNO085 wipes its internal flash DCD region and continues running
     * with the in-memory calibration unchanged — the wipe only takes
     * effect after the next power-cycle or soft reset, which the
     * caller (pk_imu_factory_reset) performs via bno_bring_up(). */
    uint8_t p[9] = {0};
    return sh2_send_command(SH2_COMMAND_CLEAR_DCD, p);
}

/* --- Public Tare API ------------------------------------------------- */

esp_err_t pk_imu_tare_now(void)
{
    if (!s_imu_ready) return ESP_ERR_INVALID_STATE;

    /* Snapshot the latest post-sandwich attitude under the lock,
     * compute its conjugate, and stash that as the new offset. Next
     * frame, parse_rotation_vector() will multiply (aircraft attitude)
     * by (conjugate of attitude at tare moment), which collapses to
     * identity here and to the differential rotation as the chip moves
     * away from the tared pose. */
    xSemaphoreTake(s_sample_lock, portMAX_DELAY);
    float aw = s_last_aircraft_qw, ax = s_last_aircraft_qi,
          ay = s_last_aircraft_qj, az = s_last_aircraft_qk;
    quat_conj(aw, ax, ay, az,
              &s_tare_qw, &s_tare_qi, &s_tare_qj, &s_tare_qk);
    float ow = s_tare_qw, oi = s_tare_qi, oj = s_tare_qj, ok = s_tare_qk;
    xSemaphoreGive(s_sample_lock);

    ESP_LOGI(TAG, "software tare: captured (w,i,j,k) = "
                  "%+0.4f %+0.4f %+0.4f %+0.4f (PFD horizon now reads "
                  "roll/pitch 0/0; heading/HDG unchanged)",
             ow, oi, oj, ok);
    return ESP_OK;
}

esp_err_t pk_imu_tare_persist(void)
{
    esp_err_t err = pk_imu_tare_now();
    if (err != ESP_OK) return err;

    err = imu_nvs_save_tare(s_tare_qw, s_tare_qi, s_tare_qj, s_tare_qk);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "software tare persisted in RAM but NVS write "
                      "failed: %s — value lost on next reboot",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "software tare persisted to NVS (survives reboot)");
    }
    return err;
}

esp_err_t pk_imu_factory_reset(void)
{
    if (!s_imu_ready) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "factory reset: wipe SW tare + NVS + BNO persisted "
                  "state + reinit chip");

    /* Step 1: in-RAM software tare → identity. From this point the
     * PFD reverts to raw mounting-corrected attitude. */
    xSemaphoreTake(s_sample_lock, portMAX_DELAY);
    s_tare_qw = 1.0f;
    s_tare_qi = 0.0f;
    s_tare_qj = 0.0f;
    s_tare_qk = 0.0f;
    xSemaphoreGive(s_sample_lock);

    /* Step 2: erase the persisted tare key so the next boot doesn't
     * restore a stale one. */
    esp_err_t err = imu_nvs_erase_tare();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "factory reset: NVS tare erase failed (%s) — "
                      "continuing", esp_err_to_name(err));
    }

    /* Step 3: scrub BNO's own persisted reorientation matrix (legacy
     * firmware may have written one via SH-2 Tare Persist). Belt and
     * braces: even if we never write it again, leaving it stale could
     * silently bias the raw output. */
    err = bno_clear_persisted_tare();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "factory reset: BNO clear-reorient failed (%s) — "
                      "continuing", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Step 4: wipe the persisted DCD so mag/gyro/accel calibration
     * re-learns on next fusion start. */
    err = bno_clear_persisted_dcd();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "factory reset: BNO clear-DCD failed (%s) — "
                      "continuing", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Step 5: hard reset + replay init so the chip rebuilds fusion
     * state from now-clean flash. */
    err = bno_bring_up();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "factory reset: chip reinit failed (%s) — "
                      "watchdog will retry within 5 s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "factory reset complete — start figure-8 motion "
                      "to let BNO085 re-learn magnetometer calibration");
    }
    return err;
}
