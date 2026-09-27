#include "pk_codec_ctrl.h"

#include <stdbool.h>
#include <stddef.h>

#include "driver/i2c_master.h"
#include "esp_codec_dev_types.h"
#include "esp_log.h"

#include "pk_i2c0_bus.h"

#define CODEC_I2C_HZ 100000u
#define CODEC_I2C_TIMEOUT_MS 100

typedef struct {
    audio_codec_ctrl_if_t base;
    i2c_master_dev_handle_t dev;
    bool open;
} pk_codec_ctrl_t;

static const char *TAG = "codec_i2c";
static pk_codec_ctrl_t s_ctrl;

static int ctrl_open(const audio_codec_ctrl_if_t *ctrl, void *cfg, int cfg_size)
{
    (void)cfg;
    (void)cfg_size;
    return ctrl != NULL && ((const pk_codec_ctrl_t *)ctrl)->open
               ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRONG_STATE;
}

static bool ctrl_is_open(const audio_codec_ctrl_if_t *ctrl)
{
    return ctrl != NULL && ((const pk_codec_ctrl_t *)ctrl)->open;
}

static int ctrl_read(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len,
                     void *data, int data_len)
{
    const pk_codec_ctrl_t *self = (const pk_codec_ctrl_t *)ctrl;
    if (self == NULL || !self->open || data == NULL ||
        reg_len != 1 || data_len != 1 || reg < 0 || reg > 0xFF) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    const uint8_t addr = (uint8_t)reg;
    const esp_err_t err = pk_i2c0_bus_transmit_receive(
        self->dev, &addr, 1, data, 1, CODEC_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read reg 0x%02X: %s", reg, esp_err_to_name(err));
        return ESP_CODEC_DEV_READ_FAIL;
    }
    return ESP_CODEC_DEV_OK;
}

static int ctrl_write(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len,
                      void *data, int data_len)
{
    const pk_codec_ctrl_t *self = (const pk_codec_ctrl_t *)ctrl;
    if (self == NULL || !self->open || data == NULL ||
        reg_len != 1 || data_len != 1 || reg < 0 || reg > 0xFF) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    const uint8_t frame[2] = { (uint8_t)reg, *(const uint8_t *)data };
    const esp_err_t err = pk_i2c0_bus_transmit(
        self->dev, frame, sizeof(frame), CODEC_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write reg 0x%02X: %s", reg, esp_err_to_name(err));
        return ESP_CODEC_DEV_WRITE_FAIL;
    }
    return ESP_CODEC_DEV_OK;
}

static int ctrl_close(const audio_codec_ctrl_if_t *ctrl)
{
    (void)ctrl;
    /* 板级总线和 device handle 均为整机生命周期资源。ES8311 重放初始化时只
     * 重建 codec 对象，不能 rm_device 后让恢复代数仍引用旧 handle。 */
    return ESP_CODEC_DEV_OK;
}

const audio_codec_ctrl_if_t *pk_codec_ctrl_init(uint8_t addr7)
{
    if (s_ctrl.open) return &s_ctrl.base;
    i2c_master_bus_handle_t bus = pk_i2c0_bus_get();
    if (bus == NULL || addr7 > 0x7F) return NULL;

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr7,
        .scl_speed_hz = CODEC_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_ctrl.dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add ES8311 0x%02X: %s", addr7, esp_err_to_name(err));
        return NULL;
    }
    s_ctrl.base = (audio_codec_ctrl_if_t){
        .open = ctrl_open,
        .is_open = ctrl_is_open,
        .read_reg = ctrl_read,
        .write_reg = ctrl_write,
        .close = ctrl_close,
    };
    s_ctrl.open = true;
    ESP_LOGI(TAG, "ES8311 control online at 7-bit 0x%02X via locked I2C0", addr7);
    return &s_ctrl.base;
}
