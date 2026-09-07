/*
 * power_service.c — 公共电源状态模型 + backend 注册表（WP-D Task 1）。
 *
 * 结构照 qmc5883p / rf_safety 的既有模式：纯模型 host 可测
 * （POWER_SERVICE_HOST_TEST 隔离，test_power_service.c 直编本文件），
 * FreeRTOS 胶水（1 Hz 轮询任务）关在 #ifndef 里靠两板系构建验证。
 *
 * 线程 / 时序 / 选择语义的合同见 power_service.h 文件头，此处不赘。
 */

#include "power_service.h"

#include <stddef.h>

#ifndef POWER_SERVICE_HOST_TEST
#include "esp_log.h"
static const char *TAG = "pwr";
#endif

/* 注册表：指针数组 + 平行快照槽位。槽位只有一个写者（poll 任务），
 * 读者自由拷贝（合同见头文件）。 */
static const power_backend_t *s_backends[POWER_SERVICE_MAX_BACKENDS];
static power_snapshot_t s_slots[POWER_SERVICE_MAX_BACKENDS];
static size_t s_count;

/* "无数据"槽位初值 / 无 backend 时的返回值。time_degraded_na=true：
 * 没有任何数据源时剩余时间必然不可估，不许给默认假象。 */
static power_snapshot_t unknown_snapshot(void)
{
    power_snapshot_t s;
    s.source           = POWER_SRC_UNKNOWN;
    s.charging         = false;
    s.vbus_present     = false;
    s.batt_mv          = 0;
    s.pct_est          = 0;
    s.pct_valid        = false;
    s.time_degraded_na = true;
    s.updated_us       = 0;
    s.stale            = true;
    return s;
}

/* stale 判定（服务端权威口径，不信任 backend 自报）：
 * updated_us<=0 是"从未报数"哨兵，恒 stale；否则按距今 >5 s 判。 */
static bool snapshot_is_stale(const power_snapshot_t *s, int64_t now_us)
{
    if (s == NULL || s->updated_us <= 0) return true;
    return (now_us - s->updated_us) > POWER_SERVICE_STALE_US;
}

void power_service_register(const power_backend_t *b)
{
    if (b == NULL || b->poll == NULL) return;      /* 非法注册整体拒收 */

    for (size_t i = 0; i < s_count; i++) {
        if (s_backends[i] == b) return;            /* 同指针幂等 */
    }

    if (s_count >= POWER_SERVICE_MAX_BACKENDS) {
        /* 静默忽略，不挤掉已注册者。WP-D 只有 SY6970/ETA6098 两个
         * backend，走到这里说明注册方写错了，WARN 定位足够。 */
#ifndef POWER_SERVICE_HOST_TEST
        ESP_LOGW(TAG, "backend '%s' rejected: registry full (%u)",
                 b->name != NULL ? b->name : "?", (unsigned)s_count);
#endif
        return;
    }

    s_backends[s_count] = b;
    s_slots[s_count]    = unknown_snapshot();      /* 从未报数 → stale */
    s_count++;
}

size_t power_service_backend_count(void)
{
    return s_count;
}

void power_service_poll_tick(int64_t now_us)
{
    for (size_t i = 0; i < s_count; i++) {
        const power_backend_t *b = s_backends[i];
        if (b == NULL || b->poll == NULL) continue; /* 晚注册/异常槽免疫 */

        power_snapshot_t s = b->poll(now_us);

        /* 服务端保险丝：backend bug 不得把百分比吹到 100 以上
         * （uint8_t 无负值，下界 0 天然成立）。 */
        if (s.pct_est > 100) s.pct_est = 100;

        /* stale 由服务端按统一时序重算，backend 的自报不作数。 */
        s.stale = snapshot_is_stale(&s, now_us);

        s_slots[i] = s;
    }
}

power_snapshot_t power_service_snapshot_at(int64_t now_us)
{
    const power_snapshot_t *freshest = NULL;

    for (size_t i = 0; i < s_count; i++) {
        const power_snapshot_t *s = &s_slots[i];
        if (!snapshot_is_stale(s, now_us)) {
            power_snapshot_t out = *s;             /* 首个新鲜的赢 */
            out.stale = false;
            return out;
        }
        if (freshest == NULL || s->updated_us > freshest->updated_us) {
            freshest = s;                          /* 记录最近更新的 */
        }
    }

    if (freshest != NULL) {
        power_snapshot_t out = *freshest;          /* 全 stale：给最近的 */
        out.stale = true;                          /* 但必须如实标过期 */
        return out;
    }
    return unknown_snapshot();                     /* 无 backend：UNKNOWN */
}

#ifdef POWER_SERVICE_HOST_TEST
void power_service_reset(void)
{
    s_count = 0;
}
#endif

/* ── 目标端（FreeRTOS 胶水）：host 单测不编译，靠两板系构建验证 ────── */

#ifndef POWER_SERVICE_HOST_TEST

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

/* 1 Hz 轮询任务：电池是慢变量，1 Hz 足够（沿用 battery.c 的节拍）。
 * 单写者合同：整个系统只有本任务写槽位。栈/优先级对齐 qmc5883p 的
 * 诊断级任务（4096/3/core0——backend 会打 ESP_LOG，栈不能太抠）。 */
static void power_poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        power_service_poll_tick(esp_timer_get_time());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void power_service_init(void)
{
    static bool s_started;
    if (s_started) return;                         /* 幂等：只起一个任务 */
    if (xTaskCreatePinnedToCore(power_poll_task, "pwr", 4096,
                                NULL, 3, NULL, 0) != pdTRUE) {
        /* 不算致命：服务缺席 = 没有任何 backend，snapshot() 如实报
         * UNKNOWN/stale，UI 按无数据显示。 */
        ESP_LOGE(TAG, "poll task create failed");
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "power service up, %u backend(s) registered",
             (unsigned)s_count);
}

power_snapshot_t power_service_snapshot(void)
{
    return power_service_snapshot_at(esp_timer_get_time());
}

#endif /* POWER_SERVICE_HOST_TEST */
