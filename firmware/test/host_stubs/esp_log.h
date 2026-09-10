#pragma once
/*
 * host_stubs/esp_log.h — 日志丢弃，但**仍然参与编译期检查**。
 *
 * 直接展开成空会让"只被日志用到的变量"变成 -Wunused-but-set-variable，
 * 逼着生产代码为了 host 测试加无谓的 (void) 转换。这里改成 `if (0)` 里的一次
 * 真实调用：参数照常做类型检查、照常算作"被使用过"，运行时一条都不执行。
 * 顺带还能挡住"日志引用了已经改名/删掉的变量"这类漂移。
 */
static inline void pk_host_log_sink(const char *fmt, ...) { (void)fmt; }

#define PK_HOST_LOG(tag, ...) \
    do { (void)(tag); if (0) pk_host_log_sink(__VA_ARGS__); } while (0)

#define ESP_LOGE(tag, ...) PK_HOST_LOG(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) PK_HOST_LOG(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) PK_HOST_LOG(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) PK_HOST_LOG(tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) PK_HOST_LOG(tag, __VA_ARGS__)

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

static inline esp_log_level_t esp_log_level_get(const char *tag)
{ (void)tag; return ESP_LOG_INFO; }
