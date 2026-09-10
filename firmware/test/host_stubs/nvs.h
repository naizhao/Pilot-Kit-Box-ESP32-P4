#pragma once
/*
 * host_stubs/nvs.h — NVS 键值存储的空壳。
 *
 * 被测的启动状态机不依赖 NVS 里有什么：tare 读不出来只影响初始四元数，
 * 不影响"任务在不在、器件恢复得了恢复不了"。所以这里一律返回
 * ESP_ERR_NVS_NOT_FOUND（= 全新设备的正常路径），不伪造存储语义。
 */
#include <stddef.h>

#include "esp_err.h"

#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND         0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES     0x110D
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#endif

typedef int nvs_handle_t;

typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;

/* 观察点：写/擦各发生过几次。「没取得有效姿态时不许写 NVS」这类判据只有
 * 数出来才算证明——返回码相同，区别全在有没有真的落一次盘。
 * weak：生产 TU 与测试 TU 看同一个变量（同 host_stubs/freertos/task.h）。 */
__attribute__((weak)) int pk_host_nvs_set_blob_calls;
__attribute__((weak)) int pk_host_nvs_erase_calls;

static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{ (void)ns; (void)mode; if (out) *out = 1; return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *buf, size_t *len)
{ (void)h; (void)key; (void)buf; (void)len; return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *key,
                                     const void *buf, size_t len)
{ (void)h; (void)key; (void)buf; (void)len; pk_host_nvs_set_blob_calls++; return ESP_OK; }
static inline esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{ (void)h; (void)key; pk_host_nvs_erase_calls++; return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
