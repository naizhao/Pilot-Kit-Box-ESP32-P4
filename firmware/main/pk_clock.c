#include "pk_clock.h"

#include <string.h>
#include <sys/time.h>

#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "clock";

/* 下限：早于 2024-01-01 UTC 的 epoch 一律拒绝（防止 zero-init / 错误数据回退时钟）。 */
#define PK_CLOCK_MIN_EPOCH_MS     1704067200000LL
/* GPS 精校后，多久之内压过其它来源（蓝牙不许覆盖更准的卫星时间）。 */
#define PK_CLOCK_GPS_PRIORITY_US  (5LL * 60 * 1000000)   /* 5 分钟 */

static volatile bool  s_synced;
static const char    *s_source = "none";
static int64_t        s_last_gps_us;   /* 最近一次 "gps" 精校的单调时刻；0 = 从未 */
static pk_clock_sync_cb_t s_sync_cb;

void pk_clock_register_sync_cb(pk_clock_sync_cb_t cb) { s_sync_cb = cb; }

bool pk_clock_apply_epoch_ms(int64_t epoch_ms, const char *source)
{
    if (epoch_ms < PK_CLOCK_MIN_EPOCH_MS) {
        ESP_LOGW(TAG, "reject %s: epoch_ms=%lld older than 2024 — ignored",
                 source, (long long)epoch_ms);
        return false;
    }

    /* GPS 优先：GPS 精校仍新鲜时，非 GPS 来源不许覆盖。 */
    bool is_gps = (strcmp(source, "gps") == 0);
    if (!is_gps && s_last_gps_us != 0) {
        int64_t age = esp_timer_get_time() - s_last_gps_us;
        if (age < PK_CLOCK_GPS_PRIORITY_US) {
            ESP_LOGD(TAG, "ignore %s: GPS fix authoritative (%llds ago)",
                     source, (long long)(age / 1000000));
            return false;
        }
    }

    struct timeval before, target;
    gettimeofday(&before, NULL);
    int64_t before_ms = (int64_t)before.tv_sec * 1000LL + before.tv_usec / 1000LL;

    target.tv_sec  = (time_t)(epoch_ms / 1000);
    target.tv_usec = (suseconds_t)((epoch_ms % 1000) * 1000);
    settimeofday(&target, NULL);

    if (is_gps) s_last_gps_us = esp_timer_get_time();
    s_source = source;
    s_synced = true;

    ESP_LOGI(TAG, "clock set via %s: epoch_ms=%lld (delta vs prior=%+lld ms)",
             source, (long long)epoch_ms, (long long)(epoch_ms - before_ms));

    if (s_sync_cb != NULL) s_sync_cb(epoch_ms, before_ms, source);
    return true;
}

bool pk_clock_is_synced(void) { return s_synced; }

const char *pk_clock_source(void) { return s_source; }

int64_t pk_clock_civil_utc_to_epoch_ms(int yr, int mo, int dy,
                                       int hr, int mi, int se, int frac256)
{
    /* Howard Hinnant's days_from_civil — handles full Gregorian range. */
    int y = yr - (mo <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + dy - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days_since_epoch = (int64_t)era * 146097 + (int)doe - 719468;
    int64_t s = days_since_epoch * 86400 + hr * 3600 + mi * 60 + se;
    return s * 1000 + (int64_t)frac256 * 1000 / 256;
}
