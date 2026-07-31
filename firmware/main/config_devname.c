/*
 * config_devname.c — 见 config_devname.h。
 *
 * 结构照 config_ble.c（volatile 缓冲 + portMUX + ensure_nvs + get/set/load），
 * 唯一的不同是它存**字符串**——全仓第一个。u8/blob 那几个模块（config_ble /
 * config_qnh / imu_task）都是定长，读法是「给个缓冲直接读」；字符串必须先问
 * 长度再读，见 load_locked() 里的两段式。
 */
#include "config_devname.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg_devname";

#define DEVNAME_NVS_NAMESPACE  "pk_dev"
#define DEVNAME_NVS_KEY        "devname"

/* 空串 = 未设置。开机 load 之前也是这个值，于是「NVS 没读到」与「还没读」
 * 表现一致——都退回出厂默认名，不会短暂广播出一个半截的名字。 */
static char s_name[PK_DEVNAME_BUF_SIZE];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* 幂等 NVS init，照 config_ble.c:ensure_nvs */
static void ensure_nvs(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

bool pk_devname_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '-' || c == '_';
}

void pk_devname_get(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return;
    portENTER_CRITICAL(&s_mux);
    /* strlcpy 在 IDF 上有，但这里长度是自己钉死的常量，snprintf/strncpy 都
     * 要额外补 NUL——直接按可写字节数手动截。 */
    size_t n = strlen(s_name);
    if (n > out_size - 1) n = out_size - 1;
    memcpy(out, s_name, n);
    out[n] = '\0';
    portEXIT_CRITICAL(&s_mux);
}

/* 过滤 + 截断，把用户输入规整成可存的样子。返回落进 dst 的字符数。
 *
 * 非法字符**丢弃**而不是替换成占位：名字是给人看的，塞一堆 '_' 进去只会
 * 让人以为设备把输入弄坏了。正常路径下键盘页根本敲不出非法字符，这里是
 * 给将来可能出现的其它入口（BLE 写特征、串口命令）兜底的。 */
static size_t sanitize(const char *src, char *dst, size_t dst_cap)
{
    size_t n = 0;
    if (src == NULL) { dst[0] = '\0'; return 0; }
    for (const char *p = src; *p && n < dst_cap - 1; ++p) {
        if (pk_devname_char_ok(*p)) dst[n++] = *p;
    }
    dst[n] = '\0';
    return n;
}

void pk_devname_set(const char *name)
{
    char clean[PK_DEVNAME_BUF_SIZE];
    const size_t n = sanitize(name, clean, sizeof(clean));

    portENTER_CRITICAL(&s_mux);
    memcpy(s_name, clean, n + 1);
    portEXIT_CRITICAL(&s_mux);

    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(DEVNAME_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    /* 空串不存空串，而是把键**删掉**：存一个空值之后，「用户清空了名字」和
     * 「用户从没设过」在 NVS 里长得一样，但前者会白占一条记录，而且下一版
     * 若给默认名换个前缀，残留的空键会让人误以为设置还在。 */
    err = (n == 0) ? nvs_erase_key(h, DEVNAME_NVS_KEY)
                   : nvs_set_str(h, DEVNAME_NVS_KEY, clean);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   /* 本来就没有，不算错 */
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "save failed (%s)", esp_err_to_name(err));

    ESP_LOGI(TAG, "device name -> \"%s\"%s", clean,
             n == 0 ? " (default)" : "");
}

void pk_config_devname_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(DEVNAME_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no device name stored, using default");
        return;                          /* s_name 保持空串 = 用默认 */
    }

    /*
     * 字符串必须两段式读：先传 NULL 问长度，再按长度读。
     *
     * 不能像 u8/blob 那样「给个缓冲直接读」——nvs_get_str 若发现缓冲装不下
     * 会返回 ESP_ERR_NVS_INVALID_LENGTH 且**什么都不写**。上限是自己钉的 26，
     * 但 NVS 里那条记录可能来自将来放宽了上限的固件版本（或降级回来的旧版），
     * 先问长度就能把这种情况识别成「存的名字比我认得的长」，而不是读出半截。
     */
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, DEVNAME_NVS_KEY, NULL, &len);
    if (err == ESP_OK && len > 0 && len <= sizeof(s_name)) {
        char buf[PK_DEVNAME_BUF_SIZE];
        err = nvs_get_str(h, DEVNAME_NVS_KEY, buf, &len);
        if (err == ESP_OK) {
            /* 存进去时已经过滤过，这里再过一遍：NVS 内容可能被别的固件版本
             * 或手工工具写过，广播名里混进控制字符会让扫描端显示成乱码。 */
            char clean[PK_DEVNAME_BUF_SIZE];
            sanitize(buf, clean, sizeof(clean));
            portENTER_CRITICAL(&s_mux);
            memcpy(s_name, clean, strlen(clean) + 1);
            portEXIT_CRITICAL(&s_mux);
        }
    } else if (err == ESP_OK && len > sizeof(s_name)) {
        ESP_LOGW(TAG, "stored name is %u bytes (> %u), ignoring",
                 (unsigned)len, (unsigned)sizeof(s_name));
        err = ESP_ERR_NVS_INVALID_LENGTH;
    }
    nvs_close(h);

    if (s_name[0] != '\0') ESP_LOGI(TAG, "device name: \"%s\"", s_name);
    else                   ESP_LOGI(TAG, "no device name stored, using default");
}
