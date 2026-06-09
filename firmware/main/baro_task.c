/*
 * baro_task.c — BMP388 气压计驱动(骨架)。
 *
 * Task 1: 挂上 BMP388 device 并读到 CHIP_ID=0x50,证明 I²C0 复用通路打通。
 * TODO 后续 task: 读 calib + 配置 OSR/ODR + 周期读温压 + 气压高度/VS 计算。
 *
 * BMP388 I²C 地址: 0x76
 * CHIP_ID 寄存器: 0x00，期望值: 0x50
 *
 * 与 BNO085 IMU 共享 I²C0 总线。scl_speed_hz 必须与 IMU 一致(400 kHz)。
 */

#include "baro.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "imu_task.h"   /* pk_i2c0_bus_get */

static const char *TAG = "baro";

#define BMP388_ADDR        0x76
#define BMP388_REG_CHIPID  0x00
#define BMP388_CHIPID      0x50

/* BMP388 与 BNO085 共享 I²C0 总线,scl_speed_hz 必须与 IMU 一致。
 * imu_task.c 中 IMU_I2C_HZ = 400000,故此处同样使用 400000。 */
#define BARO_I2C_HZ        400000

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t       s_mutex;
static pk_baro_state_t         s_state;   /* guarded by s_mutex */

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t __attribute__((unused)) reg_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100);
}

static void baro_task(void *arg)
{
    (void)arg;
    /* TODO 后续 task: 读 calib + 配置 OSR/ODR + 周期读温压 */
    uint8_t id = 0;
    while (1) {
        if (reg_read(BMP388_REG_CHIPID, &id, 1) == ESP_OK) {
            bool ok_id = (id == BMP388_CHIPID);
            if (ok_id) ESP_LOGI(TAG, "BMP388 chip_id=0x%02X", id);
            else       ESP_LOGW(TAG, "BMP388 chip_id=0x%02X (expect 0x50!)", id);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_state.valid = ok_id;
            xSemaphoreGive(s_mutex);
        } else {
            ESP_LOGW(TAG, "BMP388 read failed");
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_state.valid = false;
            xSemaphoreGive(s_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

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
