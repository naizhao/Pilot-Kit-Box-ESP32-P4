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
#include <stdatomic.h>

#ifndef POWER_SERVICE_HOST_TEST
#include "esp_log.h"
static const char *TAG = "pwr";
#endif

/* 注册表：指针数组 + 平行快照槽位。槽位只有一个写者（poll 任务），
 * 读者（UI 任务）经 seqlock 协议拷贝（合同见头文件线程合同一节）：
 * 序号偶数 = 槽位稳定，奇数 = 写到一半。 */
static const power_backend_t *s_backends[POWER_SERVICE_MAX_BACKENDS];
static power_snapshot_t s_slots[POWER_SERVICE_MAX_BACKENDS];
static _Atomic uint32_t s_slot_seq[POWER_SERVICE_MAX_BACKENDS];
static size_t s_count;

/* "无数据"槽位初值 / 无 backend 时的返回值。time_degraded_na=true：
 * 没有任何数据源时剩余时间必然不可估，不许给默认假象。 */
static power_snapshot_t unknown_snapshot(void)
{
    power_snapshot_t s;
    s.source           = POWER_SRC_UNKNOWN;
    s.backend          = POWER_BACKEND_NONE;
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

        /* seqlock 写协议：进临界区先把序号打成奇数（release，读者见之
         * 即重试），槽位提交后再打回偶数（release，读者的一致性闸）。
         * 从「seq|1」起算保证收尾必是偶数——就算序号曾被外部掰成奇数
         * （仅 host 测试 seam 干得出来），写者一拍就恢复不变式。 */
        const uint32_t seq = atomic_load_explicit(&s_slot_seq[i],
                                                  memory_order_relaxed);
        atomic_store_explicit(&s_slot_seq[i], seq | 1u,
                              memory_order_release);
        s.backend = b->id;         /* 身份戳：赢家是谁由注册项自带 */
        s_slots[i] = s;
        atomic_store_explicit(&s_slot_seq[i], (seq | 1u) + 1u,
                              memory_order_release);
    }
}

/*
 * 槽位一致性拷贝（seqlock 读协议）：取序号（acquire）→ 拷贝 → 复核，
 * 奇数或复核不符即重试。上限 4 次：写临界区只有一次结构体赋值，1 Hz
 * 写者下读者连撞 4 次意味着系统已经病了——此时宁可按「本拍没读到」
 * 处理（调用方拿 UNKNOWN/stale），也绝不交出可能撕裂的副本（RV32 上
 * int64_t 的 updated_us 撕了就是垃圾时间戳， 见头文件线程合同）。
 */
static bool slot_copy(size_t i, power_snapshot_t *out)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        const uint32_t s1 = atomic_load_explicit(&s_slot_seq[i],
                                                 memory_order_acquire);
        if (s1 & 1u) continue;                    /* 写到一半：重试 */
        *out = s_slots[i];
        const uint32_t s2 = atomic_load_explicit(&s_slot_seq[i],
                                                 memory_order_acquire);
        if (s1 == s2) return true;                /* 一致副本 */
    }
    return false;
}

power_snapshot_t power_service_snapshot_at(int64_t now_us)
{
    power_snapshot_t freshest;
    bool have_freshest = false;

    for (size_t i = 0; i < s_count; i++) {
        power_snapshot_t s;
        if (!slot_copy(i, &s)) continue;   /* 重试耗尽：本拍跳过该槽 */
        if (!snapshot_is_stale(&s, now_us)) {
            s.stale = false;
            return s;                      /* 首个新鲜的赢 */
        }
        if (!have_freshest || s.updated_us > freshest.updated_us) {
            freshest = s;                  /* 记录最近更新的 */
            have_freshest = true;
        }
    }

    if (have_freshest) {
        freshest.stale = true;             /* 全 stale：给最近的，如实标 */
        return freshest;
    }
    return unknown_snapshot();             /* 无 backend（或全没读到） */
}

#ifdef POWER_SERVICE_HOST_TEST
void power_service_reset(void)
{
    s_count = 0;
}

void power_service_test_seq_break(size_t slot)
{
    if (slot >= POWER_SERVICE_MAX_BACKENDS) return;
    atomic_store_explicit(&s_slot_seq[slot], 1u, memory_order_relaxed);
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
