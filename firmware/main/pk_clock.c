#include "pk_clock.h"

#include <string.h>
#include <sys/time.h>

#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "clock";

/* 下限：早于 2024-01-01 UTC 的 epoch 一律拒绝（防止 zero-init / 错误数据回退时钟）。 */
#define PK_CLOCK_MIN_EPOCH_MS     1704067200000LL

static volatile bool  s_synced;
static const char    *s_source = "none";
static int            s_tier;          /* 当前时钟来源的可信度档位；0 = 从未校时 */
static int64_t        s_last_apply_us; /* 当前这个值写入的单调时刻；0 = 从未 */
static pk_clock_sync_cb_t s_sync_cb;

/*
 * 校时来源的可信度阶梯。
 *
 *   gps        4  卫星 fix，授时源本身
 *   ble/cts    3  手机（它自己多半也是网络/卫星同步的，但多一跳）
 *   gps-coarse 2  GNSS 模块自己的 RTC（没有 fix，只是模块的钟还在走）
 *   nvs        1  上次关机时刻，从 flash 读回
 *   未知       0  拼错的来源字符串不得冒充高档位
 *
 * nvs 排在最低是有具体理由的，不是排资论辈：那个值**必然偏早**——关机期间
 * 时间照常流逝，而我们无从知道流逝了多久。它的用处只是"开机别显示 1970"，
 * 一旦让它覆盖已经校准的时钟，就是拿不准的换掉准的。
 *
 * ⚠️ 新增来源必须在这里登记，否则它会落到 0 档、被任何已登记来源压住。
 */
static int source_tier(const char *src)
{
    if (strcmp(src, "gps")        == 0) return 4;
    if (strcmp(src, "ble-write")  == 0) return 3;
    if (strcmp(src, "ios-cts")    == 0) return 3;
    if (strcmp(src, "gps-coarse") == 0) return 2;
    if (strcmp(src, "nvs")        == 0) return 1;
    return 0;
}

void pk_clock_reset_for_test(void)
{
    s_synced        = false;
    s_source        = "none";
    s_tier          = 0;
    s_last_apply_us = 0;
    s_sync_cb       = NULL;
}

void pk_clock_register_sync_cb(pk_clock_sync_cb_t cb) { s_sync_cb = cb; }

bool pk_clock_apply_epoch_ms(int64_t epoch_ms, const char *source)
{
    if (epoch_ms < PK_CLOCK_MIN_EPOCH_MS) {
        ESP_LOGW(TAG, "reject %s: epoch_ms=%lld older than 2024 — ignored",
                 source, (long long)epoch_ms);
        return false;
    }

    /* 可信度阶梯：更可信的来源写入之后，较低档位在窗口内不许覆盖。
     *
     * 这条取代了原来的「GPS 优先」特例。对 gps↔ble 这一对，行为与旧规则完全
     * 一致（gps 档位更高、窗口同为 5 分钟）；扩展之处在于它同样挡得住
     * nvs 覆盖 gps-coarse 这类新组合，不必每加一个来源就补一条特例。
     *
     * 窗口过了就放行：GPS 校过之后进隧道／室内几小时没有新的卫星授时，这时
     * 手机连上来报的时间更值得采信，抱着几小时前的旧值不放才是错的。 */
    const int tier = source_tier(source);
    if (tier < s_tier && s_last_apply_us != 0) {
        int64_t age = esp_timer_get_time() - s_last_apply_us;
        if (age < PK_CLOCK_PRIORITY_US) {
            ESP_LOGD(TAG, "ignore %s(tier %d): %s(tier %d) still fresh (%llds ago)",
                     source, tier, s_source, s_tier, (long long)(age / 1000000));
            return false;
        }
    }

    struct timeval before, target;
    gettimeofday(&before, NULL);
    int64_t before_ms = (int64_t)before.tv_sec * 1000LL + before.tv_usec / 1000LL;

    target.tv_sec  = (time_t)(epoch_ms / 1000);
    target.tv_usec = (suseconds_t)((epoch_ms % 1000) * 1000);
    settimeofday(&target, NULL);

    s_tier          = tier;
    s_last_apply_us = esp_timer_get_time();
    s_source        = source;
    s_synced        = true;

    ESP_LOGI(TAG, "clock set via %s(tier %d): epoch_ms=%lld (delta vs prior=%+lld ms)",
             source, tier, (long long)epoch_ms, (long long)(epoch_ms - before_ms));

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
