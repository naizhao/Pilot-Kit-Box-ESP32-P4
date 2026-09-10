#pragma once
/*
 * host_stubs/driver/uart.h — UART 驱动的空壳。
 *
 * 测试不经过 UART 喂数据：它直接调用模块自己的整行入口（pk_gps_feed_line），
 * 与生产任务循环拼行之后调用的是同一个函数。uart_read_bytes 恒返回 0，
 * 保证任何误启动的任务循环不会拿到伪造字节。
 */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_2 2

#define UART_PIN_NO_CHANGE      (-1)
#define UART_DATA_8_BITS        3
#define UART_PARITY_DISABLE     0
#define UART_STOP_BITS_1        1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT       0

typedef struct {
    int baud_rate;
    int data_bits;
    int parity;
    int stop_bits;
    int flow_ctrl;
    int rx_flow_ctrl_thresh;
    int source_clk;
} uart_config_t;

static inline esp_err_t uart_driver_install(int port, int rx_buf, int tx_buf,
                                            int queue_size, void *queue, int flags)
{ (void)port; (void)rx_buf; (void)tx_buf; (void)queue_size; (void)queue; (void)flags;
  return ESP_OK; }

static inline esp_err_t uart_param_config(int port, const uart_config_t *cfg)
{ (void)port; (void)cfg; return ESP_OK; }

static inline esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts)
{ (void)port; (void)tx; (void)rx; (void)rts; (void)cts; return ESP_OK; }

static inline int uart_read_bytes(int port, void *buf, size_t len, unsigned ticks)
{ (void)port; (void)buf; (void)len; (void)ticks; return 0; }

static inline int uart_write_bytes(int port, const void *src, size_t len)
{ (void)port; (void)src; return (int)len; }
