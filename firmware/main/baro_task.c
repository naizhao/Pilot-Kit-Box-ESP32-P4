/*
 * baro_task.c — BMP388 气压计驱动。
 *
 * Task 1: 挂上 BMP388 device 并读到 CHIP_ID=0x50,证明 I²C0 复用通路打通。
 * Task 2: 读 calib + 配置 OSR/ODR/PWR_CTRL + 周期读温压 + 气压高度/VS 计算。
 *
 * BMP388 I²C 地址: 0x76
 * CHIP_ID 寄存器: 0x00,期望值: 0x50
 *
 * 与 BNO085 IMU 共享 I²C0 总线。scl_speed_hz 必须与 IMU 一致(400 kHz)。
 */

#include "baro.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "imu_task.h"   /* pk_i2c0_bus_get */
#include "config_qnh.h" /* pk_qnh_get() — 动态 QNH(修正海压) */

static const char *TAG = "baro";

#define BMP388_ADDR        0x76
#define BMP388_REG_CHIPID  0x00
#define BMP388_CHIPID      0x50

/* BMP388 寄存器地址 */
#define BMP388_REG_DATA    0x04   /* PRESS_XLSB..TEMP_MSB (6 bytes) */
#define BMP388_REG_PWR     0x1B   /* PWR_CTRL */
#define BMP388_REG_OSR     0x1C   /* OSR */
#define BMP388_REG_ODR     0x1D   /* ODR */
#define BMP388_REG_CALIB   0x31   /* 校准系数起始 (21 bytes) */

/* BMP388 与 BNO085 共享 I²C0 总线,scl_speed_hz 必须与 IMU 一致。
 * imu_task.c 中 IMU_I2C_HZ = 400000,故此处同样使用 400000。 */
#define BARO_I2C_HZ        400000

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t       s_mutex;
static pk_baro_state_t         s_state;   /* guarded by s_mutex */

/* ── 量化后的校准系数(Bosch BMP3_FLOAT 格式) ── */
static struct {
    float t1, t2, t3;
    float p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11;
    float t_lin;   /* compensate_temperature 的中间结果,供 compensate_pressure 复用 */
} s_cal;

/* 配置+校准成功 gate:配置写入或校准读取任一失败前禁止输出 valid 数据 */
static volatile bool s_ready = false;

/* ─────────────────────────────────────────────────────────────────────── */
/*  寄存器读写辅助                                                          */
/* ─────────────────────────────────────────────────────────────────────── */

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100);
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  Bosch BMP3_FLOAT 补偿公式(照原文一字不改)                               */
/* ─────────────────────────────────────────────────────────────────────── */

static float compensate_temperature(uint32_t uncomp_temp)
{
    float pd1 = (float)uncomp_temp - s_cal.t1;
    float pd2 = pd1 * s_cal.t2;
    s_cal.t_lin = pd2 + (pd1 * pd1) * s_cal.t3;
    return s_cal.t_lin;
}

static float compensate_pressure(uint32_t uncomp_press)
{
    float t   = s_cal.t_lin;
    float po1 = s_cal.p5 + s_cal.p6 * t + s_cal.p7 * (t * t) + s_cal.p8 * (t * t * t);
    float po2 = (float)uncomp_press * (s_cal.p1 + s_cal.p2 * t + s_cal.p3 * (t * t) + s_cal.p4 * (t * t * t));
    float up2 = (float)uncomp_press * (float)uncomp_press;
    float po3 = up2 * (s_cal.p9 + s_cal.p10 * t) + (up2 * (float)uncomp_press) * s_cal.p11;
    return po1 + po2 + po3;   /* Pa */
}

/* 前向声明:configure_and_calibrate 调用 load_calibration */
static esp_err_t load_calibration(void);

/* ─────────────────────────────────────────────────────────────────────── */
/*  配置 OSR/ODR/PWR_CTRL + 读校准系数。任一步失败返回非 ESP_OK。            */
/* ─────────────────────────────────────────────────────────────────────── */

static esp_err_t configure_and_calibrate(void)
{
    esp_err_t e;
    if ((e = reg_write(BMP388_REG_OSR, 0x02)) != ESP_OK) { ESP_LOGE(TAG, "OSR write: %s",  esp_err_to_name(e)); return e; }
    if ((e = reg_write(BMP388_REG_ODR, 0x04)) != ESP_OK) { ESP_LOGE(TAG, "ODR write: %s",  esp_err_to_name(e)); return e; }
    if ((e = reg_write(BMP388_REG_PWR, 0x33)) != ESP_OK) { ESP_LOGE(TAG, "PWR write: %s",  esp_err_to_name(e)); return e; }
    vTaskDelay(pdMS_TO_TICKS(100));   /* 等首次转换 (>= 80ms ODR 周期) */
    return load_calibration();        /* 读 21 字节校准 */
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  读并解析 21 字节校准系数(纯只读,不写配置)                               */
/* ─────────────────────────────────────────────────────────────────────── */

static esp_err_t load_calibration(void)
{
    uint8_t c[21];
    esp_err_t err = reg_read(BMP388_REG_CALIB, c, 21);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "calib read failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t T1 = (uint16_t)((c[1] << 8) | c[0]);
    uint16_t T2 = (uint16_t)((c[3] << 8) | c[2]);
    int8_t   T3 = (int8_t)c[4];
    int16_t  P1 = (int16_t)((c[6] << 8) | c[5]);
    int16_t  P2 = (int16_t)((c[8] << 8) | c[7]);
    int8_t   P3 = (int8_t)c[9];
    int8_t   P4 = (int8_t)c[10];
    uint16_t P5 = (uint16_t)((c[12] << 8) | c[11]);
    uint16_t P6 = (uint16_t)((c[14] << 8) | c[13]);
    int8_t   P7 = (int8_t)c[15];
    int8_t   P8 = (int8_t)c[16];
    int16_t  P9 = (int16_t)((c[18] << 8) | c[17]);
    int8_t   P10 = (int8_t)c[19];
    int8_t   P11 = (int8_t)c[20];

    s_cal.t1  = (float)T1  / 0.00390625f;              /* 2^-8  */
    s_cal.t2  = (float)T2  / 1073741824.0f;             /* 2^30  */
    s_cal.t3  = (float)T3  / 281474976710656.0f;         /* 2^48  */
    s_cal.p1  = ((float)P1  - 16384.0f) / 1048576.0f;   /* 2^20  */
    s_cal.p2  = ((float)P2  - 16384.0f) / 536870912.0f; /* 2^29  */
    s_cal.p3  = (float)P3  / 4294967296.0f;              /* 2^32  */
    s_cal.p4  = (float)P4  / 137438953472.0f;            /* 2^37  */
    s_cal.p5  = (float)P5  / 0.125f;                     /* 2^-3  */
    s_cal.p6  = (float)P6  / 64.0f;                      /* 2^6   */
    s_cal.p7  = (float)P7  / 256.0f;                     /* 2^8   */
    s_cal.p8  = (float)P8  / 32768.0f;                   /* 2^15  */
    s_cal.p9  = (float)P9  / 281474976710656.0f;          /* 2^48  */
    s_cal.p10 = (float)P10 / 281474976710656.0f;          /* 2^48  */
    s_cal.p11 = (float)P11 / 36893488147419103232.0f;    /* 2^65  */
    s_cal.t_lin = 0.0f;

    ESP_LOGI(TAG, "calib loaded T1=%.1f T2=%.3e P5=%.1f P6=%.3e",
             s_cal.t1, s_cal.t2, s_cal.p5, s_cal.p6);
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  baro_task                                                               */
/* ─────────────────────────────────────────────────────────────────────── */

static void baro_task(void *arg)
{
    (void)arg;

    /* ── 1. 验证 CHIP_ID ── */
    uint8_t id = 0;
    for (int retry = 0; retry < 10; retry++) {
        if (reg_read(BMP388_REG_CHIPID, &id, 1) == ESP_OK && id == BMP388_CHIPID) break;
        ESP_LOGW(TAG, "CHIP_ID retry %d (got 0x%02X)", retry, id);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (id != BMP388_CHIPID) {
        ESP_LOGE(TAG, "BMP388 not found (chip_id=0x%02X), task exit", id);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.valid = false;
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "BMP388 chip_id=0x%02X OK", id);

    /* ── 2+3. 配置 OSR/ODR/PWR_CTRL + 读校准系数(开机尝试一次;失败后进循环内每秒重试) ── */
    s_ready = (configure_and_calibrate() == ESP_OK);

    /* ── 4. 循环读温压 → 补偿 → 高度/VS → 填 s_state ── */
    /* QNH_PA 已改为每轮调 pk_qnh_get() * 100.0f(Task 9) */
    static const float VS_ALPHA = 0.2f;      /* EMA 平滑系数 */

    int     prev_alt_ft  = 0;
    int64_t prev_time_us = 0;
    float   vs_ema       = 0.0f;
    bool    has_prev     = false;
    int     log_tick     = 0;

    while (1) {
        /* 配置+校准 gate:开机失败则循环内每秒重试;成功前 valid 恒 false */
        if (!s_ready) {
            s_ready = (configure_and_calibrate() == ESP_OK);
            if (!s_ready) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_state.valid = false;
                xSemaphoreGive(s_mutex);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        uint8_t d[6];
        if (reg_read(BMP388_REG_DATA, d, 6) != ESP_OK) {
            ESP_LOGW(TAG, "BMP388 data read failed");
            has_prev = false;   /* 读失败后清除前次状态,防止恢复后 VS 出现尖峰 */
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_state.valid = false;
            xSemaphoreGive(s_mutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint32_t raw_press = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16);
        uint32_t raw_temp  = (uint32_t)d[3] | ((uint32_t)d[4] << 8) | ((uint32_t)d[5] << 16);

        /* 顺序重要:先温度(更新 t_lin),再气压(依赖 t_lin) */
        float temp_c   = compensate_temperature(raw_temp);
        float press_pa = compensate_pressure(raw_press);

        /* 守卫:press_pa <= 0 会使 powf 底数为负,产生 NaN → (int)NaN UB */
        if (!(press_pa > 0.0f)) {
            has_prev = false;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_state.valid = false;
            xSemaphoreGive(s_mutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 气压高度(国际民航标准大气公式);QNH 每轮读取,支持运行时调整 */
        float qnh_pa = pk_qnh_get() * 100.0f;   /* hPa → Pa */
        float alt_m  = 44330.0f * (1.0f - powf(press_pa / qnh_pa, 0.190295f));
        int   alt_ft = (int)roundf(alt_m * 3.28084f);

        /* VS: 高度对时间微分 + EMA 平滑 */
        int64_t now = esp_timer_get_time();
        int vs_fpm = 0;
        if (has_prev && (now - prev_time_us) > 0) {
            float dt_min    = (float)(now - prev_time_us) / 60000000.0f;   /* µs → min */
            float vs_inst   = (float)(alt_ft - prev_alt_ft) / dt_min;      /* ft/min   */
            vs_ema          = VS_ALPHA * vs_inst + (1.0f - VS_ALPHA) * vs_ema;
            vs_fpm          = (int)vs_ema;
        }
        prev_alt_ft  = alt_ft;
        prev_time_us = now;
        has_prev     = true;

        /* 诊断 log:降频到每秒约一次(100ms * 10 = 1s) */
        log_tick++;
        if (log_tick >= 10) {
            ESP_LOGI(TAG, "P=%.0fPa T=%.1fC alt=%dft vs=%d",
                     press_pa, temp_c, alt_ft, vs_fpm);
            log_tick = 0;
        }

        /* 加锁更新 s_state */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.valid       = true;
        s_state.pressure_pa = press_pa;
        s_state.temp_c      = temp_c;
        s_state.alt_ft      = alt_ft;
        s_state.vs_fpm      = vs_fpm;
        s_state.updated_us  = now;
        xSemaphoreGive(s_mutex);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  公共 API                                                                */
/* ─────────────────────────────────────────────────────────────────────── */

void pk_baro_start(void)
{
    i2c_master_bus_handle_t bus = pk_i2c0_bus_get();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C0 bus not ready (call after pk_imu_init)");
        return;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BMP388_ADDR,
        .scl_speed_hz    = BARO_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_mutex); s_mutex = NULL;
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(baro_task, "baro", 4096, NULL, 4, NULL, 0);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "baro task create failed");
        i2c_master_bus_rm_device(s_dev);
        vSemaphoreDelete(s_mutex); s_mutex = NULL;
        return;
    }
}

bool pk_baro_get(pk_baro_state_t *out)
{
    if (s_mutex == NULL) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_mutex);
    return out->valid;
}
