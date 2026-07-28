/* esp_app_desc.h — 模拟器桩。固件里由 ESP-IDF 在链接期填入编译信息，
 * PC 上给一组固定值即可：页面排版只关心字符串长度，不关心内容真伪。 */
#pragma once
typedef struct {
    char project_name[32];
    char version[32];
    char date[16];
    char time[16];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
