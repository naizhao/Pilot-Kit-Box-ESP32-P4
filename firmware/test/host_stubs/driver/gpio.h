#pragma once
/*
 * host_stubs/driver/gpio.h — GPIO/中断服务的空壳。
 *
 * PPS 中断在 host 上不会触发；依赖 PPS 的判定（time_locked）由测试直接构造
 * 状态来验证，而不是靠伪造一个中断。
 */
#include <stdint.h>

#include "esp_err.h"

#define GPIO_MODE_INPUT        1
#define GPIO_MODE_OUTPUT       2
#define GPIO_PULLUP_DISABLE    0
#define GPIO_PULLUP_ENABLE     1
#define GPIO_PULLDOWN_DISABLE  0
#define GPIO_PULLDOWN_ENABLE   1
#define GPIO_INTR_DISABLE      0
#define GPIO_INTR_POSEDGE      1

typedef struct {
    uint64_t pin_bit_mask;
    int      mode;
    int      pull_up_en;
    int      pull_down_en;
    int      intr_type;
} gpio_config_t;

static inline esp_err_t gpio_config(const gpio_config_t *cfg)
{ (void)cfg; return ESP_OK; }

/* 器件复位脉冲（BNO085 的 RST）在 host 上没有可观察后果：判据是"复位之后
 * 器件回不回话"，那由测试里的假器件模型决定，不由电平决定。 */
static inline esp_err_t gpio_set_level(int pin, int level)
{ (void)pin; (void)level; return ESP_OK; }

static inline esp_err_t gpio_install_isr_service(int flags)
{ (void)flags; return ESP_OK; }

static inline esp_err_t gpio_isr_handler_add(int pin, void (*fn)(void *), void *arg)
{ (void)pin; (void)fn; (void)arg; return ESP_OK; }
