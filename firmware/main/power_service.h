/*
 * power_service.h — 公共电源状态模型 + backend 注册（WP-D Task 1）。
 *
 * 背景：v3 载板上有 ETA6098（GPIO20 BAT_ADC + GPIO21 STAT，见 battery.c）；
 * v4 powered 版在此基础上加了 I²C 0x6A 的 SY6970 充电芯片。两代板子的
 * 电源信息不能再靠"battery.c 是唯一电源源"这种隐式假设，这里立一个
 * 公共快照模型 + 最多 2 个 backend 的注册表，SY6970（T4）与 ETA6098
 * （T2，从 battery.c 迁移）各占一槽。
 *
 * 结构照 qmc5883p / rf_safety 的既有模式：纯模型 host 可测
 * （POWER_SERVICE_HOST_TEST 隔离，test_power_service.c 直编）；
 * FreeRTOS 胶水（1 Hz 轮询任务）关在 #ifndef 里，靠两板系构建验证。
 *
 * ── 运行期选择语义 ──────────────────────────────────────────────────
 * 注册次序 = 优先级。SY6970 探测到 ACK → 它的 bring-up 先注册（I²C 充
 * 电寄存器是 v4 powered 上的权威数据源）；ETA6098 在载板上必然存在 →
 * 后注册兜底。snapshot_at() 返回第一个未 stale 的 backend 快照；首选
 * 数据过期（如 SY6970 掉线）自动回落到 ETA6098；全部 stale 时返回最近
 * 更新的一份并如实置 stale=true（UI 据此自行决定显示方式）；一个
 * backend 都没有 → UNKNOWN / stale，绝不编造数据。
 *
 * ── 时序合同 ────────────────────────────────────────────────────────
 * now_us 一律是"开机以来的微秒"，与 esp_timer_get_time() 同源同单位；
 * backend 的 poll(now_us) 用传进来的时间戳填 updated_us。stale 判定在
 * 服务端统一重算（updated_us 距今 >5 s），不信任 backend 自报。
 *
 * ── 线程合同 ────────────────────────────────────────────────────────
 * 槽位只有一个写者（poll 任务，见 power_service.c 的 1 Hz 任务）；
 * 读者（UI 任务）自由拷贝快照。不上锁：撕裂最坏混到相邻两拍（1 s）的
 * 字段，与今天 battery.c 无锁读的容忍口径一致，状态栏场景无害。
 * register() 在 bring-up 早期（任务起跑前后都允许）调用——"前后都允许"
 * 依赖 poll_tick 的空槽免疫兜底（power_service.c：调 poll 前先判槽位
 * NULL）；晚注册的 backend 下一拍自然进入轮询。
 *
 * ── battery.h 的迁移映射（Task 2 的合同）────────────────────────────
 *   pk_batt_t.valid    → pct_valid（batt_mv 量程 2500..4500 的判定留在
 *                        ETA6098 backend 内，别丢）
 *   pk_batt_t.batt_mv  → batt_mv
 *   pk_batt_t.pct      → pct_est（mv_to_pct 天然 0..100；服务端钳位只是
 *                        防 backend bug 的保险丝）
 *   pk_batt_t.charging → charging
 *   pk_batt_t.raw_mv   → 无对应字段：分压比标定辅助量留在 ETA6098
 *                        backend 本地，诊断页如仍需要找它直接要
 *   pk_batt_get()      → Task 2 改为 power_service_snapshot() 的薄包装
 *
 *   source 档位语义：POWER_SRC_BATTERY = 电池放电（两 backend 的常态）；
 *   POWER_SRC_EXTERNAL = 载板有外部电但无/未知电池；POWER_SRC_SY6970_VBUS
 *   = SY6970 报 VBUS 在位供电；POWER_SRC_UNKNOWN = 无 backend 或从未报数。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    POWER_SRC_UNKNOWN = 0,
    POWER_SRC_EXTERNAL,
    POWER_SRC_BATTERY,
    POWER_SRC_SY6970_VBUS,
} power_src_t;

/* 一份电源快照（字段最小集，消费方：状态栏 / 诊断页 / 将来的续航估计）。 */
typedef struct {
    power_src_t source;      /* 电从哪来（档位语义见文件头）        */
    bool        charging;    /* 正在充电                            */
    bool        vbus_present;/* 外部电在位（比 charging 宽：充满维持
                              * 时 charging=false 但 vbus 仍在）    */
    uint16_t    batt_mv;     /* 电池端电压 mV（无电池时 0）         */
    uint8_t     pct_est;     /* 电量估算 0..100（服务端钳位）       */
    bool        pct_valid;   /* pct_est/batt_mv 是否可信            */
    bool        time_degraded_na; /* 剩余时间估计是否不可用         */
    int64_t     updated_us;  /* 数据采集时刻（µs，0=从未报数哨兵）  */
    bool        stale;       /* 服务端重算：updated_us 距今 >5 s    */
} power_snapshot_t;

/*
 * 电源 backend：一个真实电源数据源（SY6970 / ETA6098）。
 * name 仅用于日志；poll 每拍被调用一次，返回一份带 updated_us 的快照，
 * 内部可以慢（I²C/ADC），但不得阻塞超过一个轮询周期量级。
 */
typedef struct {
    const char      *name;
    power_snapshot_t (*poll)(int64_t now_us);
} power_backend_t;

/* WP-D 的两代板子只有 SY6970 与 ETA6098 两个数据源，容量钉死为 2：
 * 第三个注册一律拒收（静默忽略，不挤掉已注册者）。 */
#define POWER_SERVICE_MAX_BACKENDS 2

/* stale 阈值：5 s。1 Hz 轮询下连续 5 拍无新数据才算过期——单拍 I²C
 * 抖动不应让状态栏闪"无数据"。 */
#define POWER_SERVICE_STALE_US 5000000

/* ── 纯模型（host 可测）────────────────────────────────────────────── */

/* 注册一个 backend。幂等（同指针重复注册只算一次）；poll 为空整体拒收；
 * 超容量静默忽略（目标端会打一条 WARN）。典型调用点：各 backend 的
 * bring-up（SY6970 探测 ACK 后 / ETA6098 init 成功后），先注册者优先。 */
void power_service_register(const power_backend_t *b);

/* 已注册的 backend 数（诊断 + host 测试断言用）。 */
size_t power_service_backend_count(void);

/*
 * 一个轮询拍：依次调每个 backend 的 poll() 并刷新其槽位。now_us 透传给
 * backend（时序合同见文件头）；pct_est 钳位 0..100、stale 在这里统一
 * 重算。只有 poll 任务该调它（单写者合同）。
 */
void power_service_poll_tick(int64_t now_us);

/*
 * 指定时刻的聚合快照：第一个未 stale 的 backend 赢；全 stale 时返回
 * 最近更新的一份（stale=true）；无 backend → UNKNOWN / stale。
 * snapshot() 就是本函数以"现在"取值的目标端封装。
 */
power_snapshot_t power_service_snapshot_at(int64_t now_us);

/* ── 目标端（FreeRTOS 胶水，host 单测不编译）───────────────────────── */

/*
 * 启动电源服务：创建 1 Hz 轮询任务（backend 注册由各 backend 自己的
 * bring-up 调 power_service_register() 完成——SY6970 要探测 ACK 才能定
 * 去留，注册时机天然在它自己的 init 里）。幂等：重复调用只起一个任务。
 * 不返回错误：任务创建失败只打 ERROR，服务缺席等价于"无 backend"，
 * snapshot() 会如实报 UNKNOWN/stale，UI 按无数据显示。
 */
void power_service_init(void);

/* 聚合"现在"的快照（esp_timer_get_time() 时刻的 snapshot_at）。 */
power_snapshot_t power_service_snapshot(void);

/* 仅 host 单测：清空注册表（POWER_SERVICE_HOST_TEST 才声明/定义）。 */
#ifdef POWER_SERVICE_HOST_TEST
void power_service_reset(void);
#endif
