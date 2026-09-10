#pragma once
#include <assert.h>

typedef int esp_err_t;

#ifndef ESP_OK
#define ESP_OK                  0
#define ESP_FAIL               -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#endif

/* 取值与 IDF 一致（esp_err.h）。器件驱动把它们当判据用（例如 shtp_recv 的
 * ESP_ERR_NOT_FOUND = "本拍没数据"，与真正的总线错误分属两条路径），所以
 * 这里必须影子化成不同的值，不能全塞成 ESP_FAIL。 */

/* 日志里常见的 esp_err_to_name()。host 上只要求可编译、返回非 NULL。 */
static inline const char *esp_err_to_name(esp_err_t e)
{ (void)e; return "ESP_ERR(host stub)"; }

/* 驱动装配失败在 host 上是测试自身的配置错误，直接 assert 而不是静默放过。 */
#define ESP_ERROR_CHECK(x) do { esp_err_t err_rc_ = (x); assert(err_rc_ == ESP_OK); } while (0)
