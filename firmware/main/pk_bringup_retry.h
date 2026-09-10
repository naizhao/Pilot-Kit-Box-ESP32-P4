/*
 * pk_bringup_retry.h — **必装器件**开机 bring-up 的重试策略。
 *
 * 要修的病
 * --------
 * 2026-09-09 审计：BNO085 与 BMP388 的初始瞬态失败会让整次开机永久失能。
 *   - imu_task.c：pk_imu_init() 在**创建长期任务之前**同步跑 bno_bring_up()，
 *     失败就 return —— 任务从来没被创建，于是那条 5 s stall watchdog、
 *     总线 generation 重放、RST 自愈全都不存在，只能重启整机；
 *   - baro_task.c：长期任务开头只探两轮 CHIP_ID，仍失败即 vTaskDelete()，
 *     任务本体消失，后面的 generation / EVENT.por 自愈同样无人消费。
 * 两处的共同形态是「开机那一瞬间总线/器件没准备好 = 这次开机没有姿态/高度」。
 * 而这一瞬间恰恰最不可靠：2026-08-03 真机日志里的总线塌陷就发生在开机阶段
 * （GT911 只 found 没 ready），imu 与 baro 谁排在它后面谁中招。
 *
 * 判据：长期任务必须**先存在**，bring-up 是它循环里的一步，失败就退避重试。
 *
 * 为什么单独成一个模块而不是各写一遍
 * ----------------------------------
 * 退避这种「看起来三行、错了也照样跑」的代码已经错过一次：qmc5883p.c 里
 * `if (cur < MAX) cur *= 2` 在 32 s 处翻到 64 s，越过了它自己声明的 60 s
 * 封顶（2026-09 审计）。写成一处、由 host 单测钉死，比在三个器件里各抄一遍
 * 划算。QMC（可选器件）后续可以并过来，本轮不动它，避免把范围扩到必装器件
 * 之外。
 *
 * 契约（host 单测 test_baro_bringup.c / test_imu_bringup.c 逐条验证）
 * -----------------------------------------------------------------
 *   1. **永不放弃**：pk_bringup_retry_run() 只在 attempt() 成功时返回，
 *      不返回失败码、不删任务、不 abort；
 *   2. **永不忙等**：每一次失败尝试之后必定 vTaskDelay 一段 >0 的时间；
 *   3. **退避有界**：时长从 backoff_start_ms 翻倍到 backoff_max_ms 封顶，
 *      任何一次等待都不超过封顶（封顶本身也是一次真实等待，不是无限等）；
 *   4. **失败可观测**：每次失败调一次 on_failed_attempt（器件在那里把自己
 *      的对外状态置为无效、并按自己的判据决定要不要升级到总线级恢复）。
 *      升级节流是**调用方的判据**，本模块只把 backoff_ms 交给它——器件不同，
 *      升级代价不同（baro 请求整板总线复位会打扰 imu/touch）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 默认退避档位：1 s 起步、60 s 封顶。
 * 起步 1 s：器件上电/复位后本来就要几百毫秒，更密的重试只是在总线上添堵；
 * 封顶 60 s：器件真的缺焊/坏了时，每分钟一轮探测的代价可以忽略，而插回去
 * 之后最迟一分钟上线（照抄 QMC5883P_UP_BACKOFF_MAX_MS 的论证）。 */
#define PK_BRINGUP_RETRY_START_MS  1000
#define PK_BRINGUP_RETRY_MAX_MS    60000

typedef struct {
    const char *name;                  /* 只进日志 */
    /* 一次完整的 bring-up 尝试。true = 器件已就绪。 */
    bool (*attempt)(void *ctx);
    /* 每次失败调用一次。attempt_no 从 1 开始；backoff_ms 是**接下来**要睡的
     * 时长（调用方用它做升级节流：退避涨到多大才值得惊动总线级恢复）。 */
    void (*on_failed_attempt)(void *ctx, uint32_t attempt_no, uint32_t backoff_ms);
    void *ctx;
    uint32_t backoff_start_ms;         /* 0 = 用 PK_BRINGUP_RETRY_START_MS */
    uint32_t backoff_max_ms;           /* 0 = 用 PK_BRINGUP_RETRY_MAX_MS */
} pk_bringup_retry_t;

/* 下一档退避：翻倍并 clamp 到 max_ms（cur 已经 ≥ max 时原样返回 max）。 */
uint32_t pk_bringup_retry_next_ms(uint32_t cur_ms, uint32_t max_ms);

/* 阻塞重试直到 attempt() 成功，返回成功时的尝试序号（1 = 第一次就成功）。
 * 只能在器件自己的长期任务里调（内部 vTaskDelay 会让渡）。 */
uint32_t pk_bringup_retry_run(const pk_bringup_retry_t *cfg);
