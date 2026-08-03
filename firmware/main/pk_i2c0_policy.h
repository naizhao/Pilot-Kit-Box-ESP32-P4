/*
 * pk_i2c0_policy.h — I²C0 总线级恢复的**纯策略**：什么时候该恢复、
 * 允不允许现在恢复。不碰硬件、不碰 FreeRTOS，好让 host 单测把
 * 「去抖 / 冷却 / 退避 / 不重入」这四条整条跑一遍
 * （firmware/test/test_pk_i2c0_policy.c）。
 *
 * 为什么要有这一层
 * ----------------
 * I²C0（GPIO7/GPIO8，400 kHz）上挂着 BNO085(0x4A)、BMP388(0x76)、
 * GT911(0x5D) 和板载 codec，总线 handle 全局唯一（imu_task.c 建，
 * pk_i2c0_bus_get() 是唯一入口）。2026-08-03 真机抓到一次偶发的
 * **总线**塌陷（不是某个器件挂了）：
 *
 *     I (13403) touch: GT911 found at 0x5D      ← 只有 found，没有 ready
 *     W (13619) baro:  BMP388 data read failed  ← 215 ms 后气压计就挂
 *     I (14271) imu:   ... valid=25 ... i2c_err=8
 *     I (15323) imu:   ... valid=0  ... i2c_err=10
 *     W (18480) imu:   no valid RV report for 5.0s — re-init BNO085
 *     W (19440) imu:   bring-up after stall failed: ESP_ERR_TIMEOUT
 *
 * 最后那行是判据：imu_task 的 5 秒 stall watchdog 只重新初始化 BNO085
 * （拉 RST + 重放 SH-2 init），坏的既然是总线，它必然救不回来。姿态和
 * 高度同时失效，飞行中就是 PFD 和高度表一起没了——这是安全件。
 *
 * 两个独立的小状态机
 * ------------------
 *   pk_i2c0_detector_t —— **每个器件各持一份**。回答「我这边连续失败到
 *     什么程度，才算总线塌了而不是偶尔一次 NACK」。
 *   pk_i2c0_gate_t     —— **全局唯一一份**。回答「现在准不准恢复」：
 *     串行化（同一时刻只许一轮）、冷却、失败退避。
 *
 * 分成两个而不是揉成一个，是因为触发方有两个（imu / baro）、而恢复动作
 * 只能有一个：总线是共享的，两个任务同时 reset 会互相打断。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ─────────────────────────────────────────────────────────────────────
 * 探测器：连续失败到多深才喊
 * ───────────────────────────────────────────────────────────────────── */

typedef struct {
    /* 调参（init 时填死，之后只读） */
    uint32_t min_fail_count;    /* 连续失败至少这么多次 */
    int64_t  min_fail_span_us;  /* 且这串失败至少已经持续这么久 */

    /* 状态 */
    uint32_t fail_count;        /* 当前这串连续失败的次数 */
    int64_t  first_fail_us;     /* 这串失败的第一次发生在什么时候 */
} pk_i2c0_detector_t;

/* 两个门槛**同时**满足才喊。
 *
 * 为什么要"次数 + 时长"两条而不是只看次数：同一个器件在不同状态下轮询
 * 周期不同（baro 正常读数是 100 ms 一轮，配置失败重试是 1 s 一轮），
 * 只看次数会让两条路径的实际去抖时间差 10 倍；只看时长又挡不住"每 5 秒
 * 才轮询一次"这种低频调用者用两次失败就把总线 reset 了。 */
void pk_i2c0_detector_init(pk_i2c0_detector_t *d,
                           uint32_t min_fail_count,
                           int64_t  min_fail_span_us);

/* 把失败计数清零，重新开始去抖。总线刚复位完、或器件刚重新初始化完时调。 */
void pk_i2c0_detector_reset(pk_i2c0_detector_t *d);

/* 报一次总线访问结果。返回 true = 「该请求总线恢复了」。
 *
 * ok=true  → 失败串清零（总线是活的）。
 * ok=false → 计数累加；两个门槛都过了就返回一次 true，**并立刻重新开始
 *            计数**。后一半很重要：不重新计数的话，一串永不结束的失败
 *            会在每 100 ms 的轮询里一直返回 true，把请求打成风暴；靠
 *            latch 布尔挡住又会导致"这轮恢复失败了就再也不喊"。重新
 *            计数天然做到"每过一个完整的去抖窗口最多喊一次"。 */
bool pk_i2c0_detector_report(pk_i2c0_detector_t *d, bool ok, int64_t now_us);

/* ─────────────────────────────────────────────────────────────────────
 * 闸门：串行化 + 冷却 + 退避
 * ───────────────────────────────────────────────────────────────────── */

typedef enum {
    /* 放行。调用方去做真正的恢复，**必须**配对调用 pk_i2c0_gate_finish()。 */
    PK_I2C0_GATE_GO = 0,
    /* 已经有一轮恢复在跑（另一个任务拿到了）。什么都别做。 */
    PK_I2C0_GATE_BUSY,
    /* 冷却/退避中。什么都别做。 */
    PK_I2C0_GATE_COOLDOWN,
} pk_i2c0_gate_decision_t;

typedef struct {
    /* 调参 */
    int64_t  cooldown_base_us;  /* 一轮恢复之后至少隔这么久才准下一轮 */
    int64_t  cooldown_max_us;   /* 指数退避的上限 */

    /* 状态 */
    bool     busy;              /* 不重入：GO 之后到 finish 之前恒为 true */
    uint32_t consec_fail;       /* 连续失败的恢复轮数，退避指数就是它 */
    int64_t  next_allowed_us;   /* 早于这个时刻的请求一律 COOLDOWN */
    bool     armed;             /* next_allowed_us 是否有效（开机第一次必放行） */
    uint32_t generation;        /* **成功**复位过的轮数。各器件靠它发现"总线换过一轮了" */
} pk_i2c0_gate_t;

void pk_i2c0_gate_init(pk_i2c0_gate_t *g, int64_t base_us, int64_t max_us);

/* 请求开始一轮恢复。返回 GO 时闸门已被本调用方独占，直到 finish()。 */
pk_i2c0_gate_decision_t pk_i2c0_gate_begin(pk_i2c0_gate_t *g, int64_t now_us);

/* 一轮恢复结束。recovered=true 表示复位后器件确实回应了：
 *   成功 → 退避指数清零、generation++、按 base 冷却一小段（防总线抖动时反复 reset）
 *   失败 → 退避指数 +1，下次最早允许时间按 base<<n 推后，封顶 max
 * 不管成败都会解除 busy——否则一次异常路径就把闸门永久锁死。 */
void pk_i2c0_gate_finish(pk_i2c0_gate_t *g, bool recovered, int64_t now_us);

/* 当前这一档退避时长（日志用："下次最早 %.1fs 后"）。 */
int64_t pk_i2c0_gate_cooldown_us(const pk_i2c0_gate_t *g);
