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

/* ── 快照槽位的锁（2026-09 审计二轮：seqlock → portMUX）───────────────
 * 首轮的 C11 原子 seqlock 复审被否：教训同 gps_task.c:48-53 记录的那次
 * ——RVWMO（ESP32-P4 双核 RV32，弱内存序）下"读计数→读负载→复核计数"
 * 的计数与负载没有真正的全序，且 C11 口径下裸负载/存储的数据竞争本身
 * 就是 UB，事后复核救不回来。portMUX 自旋锁临界区是 ESP-IDF 的标准
 * 做法（GPS PPS 的 (计数,时间戳) 对即此方案）：写者 = 1 Hz poll 任务、
 * 读者 = UI 任务，同一把 spinlock 里整体拷贝，跨核正确性由构造保证。
 * 临界区只有一次结构体拷贝（快照 ~40-100 B，1 Hz 写），纳秒级。
 * 宿主单测（POWER_SERVICE_HOST_TEST）单线程无并发，锁宏退化为空操作
 * ——纯模型的注册/选择/stale 判定照常覆盖；并发正确性靠两板系构建的
 * 真锁（合同见 power_service.h 线程合同一节）。 */
#ifndef POWER_SERVICE_HOST_TEST
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
static const char *TAG = "pwr";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define PWR_SLOTS_LOCK()   portENTER_CRITICAL(&s_mux)
#define PWR_SLOTS_UNLOCK() portEXIT_CRITICAL(&s_mux)
#else
#define PWR_SLOTS_LOCK()   ((void)0)
#define PWR_SLOTS_UNLOCK() ((void)0)
#endif

/* 注册表：指针数组 + 平行快照槽位。槽位有一个写者（poll 任务）与多个
 * 读者（UI 任务），全部经 s_mux 临界区访问（见上）。 */
static const power_backend_t *s_backends[POWER_SERVICE_MAX_BACKENDS];
static power_snapshot_t s_slots[POWER_SERVICE_MAX_BACKENDS];
static size_t s_count;

/* 注册闸：注册表没有并发写者，靠「init 期单线程注册」的时序合同保证；
 * init 之后再来的注册一律拒收（合同见 power_service.h）。 */
static bool s_init_done;

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

    if (s_init_done) {
        /* 注册合同：所有 backend 必须在 power_service_init() 之前注册。
         * 注册表没有并发写者，靠这条时序保证；晚注册没有保护，拒收
         * 而不是碰运气（宿主测试钉死）。 */
#ifndef POWER_SERVICE_HOST_TEST
        ESP_LOGW(TAG, "backend '%s' rejected: registered after init",
                 b->name != NULL ? b->name : "?");
#endif
        return;
    }

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
        if (b == NULL || b->poll == NULL) continue; /* 空槽/异常槽免疫：
                                    * 便宜的健壮性保留（注册合同收紧后
                                    * 不再承担"晚注册下一拍进轮询"）。 */

        power_snapshot_t s = b->poll(now_us);

        /* 服务端保险丝：backend bug 不得把百分比吹到 100 以上
         * （uint8_t 无负值，下界 0 天然成立）。 */
        if (s.pct_est > 100) s.pct_est = 100;

        /* stale 由服务端按统一时序重算，backend 的自报不作数。 */
        s.stale = snapshot_is_stale(&s, now_us);

        /* 锁内整体提交：身份戳 + 槽位对读者原子（一次结构体拷贝，
         * 无重试路径，见文件头锁注释）。 */
        PWR_SLOTS_LOCK();
        s.backend = b->id;         /* 身份戳：赢家是谁由注册项自带 */
        s_slots[i] = s;
        PWR_SLOTS_UNLOCK();
    }
}

/* 槽位一致性读：锁内一次拷贝，无重试/作废路径（原 seqlock 的 4 次
 * 上限随旧方案一并删除）。 */
static power_snapshot_t slot_read(size_t i)
{
    power_snapshot_t out;
    PWR_SLOTS_LOCK();
    out = s_slots[i];
    PWR_SLOTS_UNLOCK();
    return out;
}

power_snapshot_t power_service_snapshot_at(int64_t now_us)
{
    power_snapshot_t freshest;
    bool have_freshest = false;

    for (size_t i = 0; i < s_count; i++) {
        const power_snapshot_t s = slot_read(i);
        if (!snapshot_is_stale(&s, now_us)) {
            power_snapshot_t out = s;      /* 首个新鲜的赢 */
            out.stale = false;
            return out;
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
    return unknown_snapshot();             /* 无 backend */
}

#ifdef POWER_SERVICE_HOST_TEST
void power_service_reset(void)
{
    s_count = 0;
    s_init_done = false;               /* 复位成「init 未跑」，隔离各用例 */
}

void power_service_init(void)
{
    /* 宿主没有任务可起：只翻转「服务已启动」标志，让注册合同的
     * 「init 之后拒收」分支在单线程单测里可达。 */
    s_init_done = true;
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
    /* 每 10 拍打一行状态。
     *
     * 这不是调试残留：在此之前**串口上没有任何途径能看到电源状态**——
     * SY6970 的 bring-up 日志只在启动时打一次，之后电池电压/电量/充电与否
     * 全都只活在诊断页的像素里。对一个电池供电的航空设备，"电量显示准不准"
     * 是要能在台架上核对的，而核对的前提是它得能被读出来。
     * 10 s 一行：电池是慢变量，够用且不淹没 1090 的帧流。 */
    unsigned tick = 0;
    for (;;) {
        power_service_poll_tick(esp_timer_get_time());
        if ((tick++ % 10) == 0) {
            const power_snapshot_t s = power_service_snapshot();
            ESP_LOGI(TAG,
                     "src=%d backend=%d chg=%d vbus=%d batt=%umV pct=%u%s%s",
                     (int)s.source, (int)s.backend, s.charging, s.vbus_present,
                     (unsigned)s.batt_mv, (unsigned)s.pct_est,
                     s.pct_valid ? "" : " (pct 不可信)",
                     s.stale ? " STALE" : "");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void power_service_init(void)
{
    static bool s_started;
    if (s_started) return;                         /* 幂等：只起一个任务 */
    /* 注册闸在这里落下：此后 register() 一律拒收（见 register 处注释）。
     * 即使任务创建失败也照落——服务缺席 = 没有任何 backend，晚注册
     * 同样改变不了什么。 */
    s_init_done = true;
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
