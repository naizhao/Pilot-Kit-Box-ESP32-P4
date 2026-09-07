/*
 * pk_i2c0_bus.h — I²C0 主总线的**板级**所有权 + 恢复代数。
 *
 * 为什么总线不归 imu_task（2026-09-06 之前它归 imu_task）
 * --------------------------------------------------------
 * I²C0（SDA GPIO7 / SCL GPIO8，400 kHz）上挂着 BNO085(0x4A)、BMP388(0x76)、
 * GT911(0x5D) 和板载 codec。过去总线在 pk_imu_init() 里创建（imu_task.c 的
 * i2c_bring_up），「IMU 没起来」和「总线没了」被绑成同一件事：BNO085 缺焊
 * 只该让 IMU 失败，但 baro / touch 也会跟着拿不到总线——optional 器件缺失
 * 被误当成总线故障，与 PLAN.md §6.1 的语义相反。现在总线由 app_main 最前面
 * 的 pk_i2c0_bus_init() 创建，先于一切 I²C 器件 init；imu/baro/touch 只
 * add_device 自己的器件，谁也不再「隐式拥有」总线。
 *
 * 恢复代数（generation）
 * ----------------------
 * pk_i2c0_recover.c 成功复位一轮总线后（与既有恢复判据同点、同锁）调
 * pk_i2c0_bus_generation_inc()；各器件任务在自己的循环里比对
 * pk_i2c0_bus_generation()，发现变了就重放自己既有的 bring-up——机制与
 * 「为什么不做成回调」的完整论述见 pk_i2c0_recover.h。
 *
 * 并发：单写者多读者，无锁。写者只有 pk_i2c0_recover.c，且它先拿到恢复
 * 闸门（pk_i2c0_gate_t 的 busy 位保证同一时刻只有一轮恢复在跑）才 inc；
 * 读者是 imu / baro / touch 各自的任务循环。generation 是 C11 原子
 * （atomic_uint，load/fetch_add）——2026-09 审计把 volatile 换成原子，
 * 不再依赖「对齐读写恰好原子」的平台论证。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef PK_I2C0_BUS_HOST_TEST
#include "esp_err.h"
#include "driver/i2c_master.h"
#else
/* host 单测（test_pk_i2c0_bus.c）没有 ESP-IDF：只影子化本头文件用到的
 * 名字。真类型见 IDF esp_err.h / driver/i2c_master.h，取值语义一致。 */
typedef int esp_err_t;
#define ESP_OK                0
#define ESP_ERR_INVALID_STATE 0x103
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
#endif

/*
 * 创建 I²C0 主总线。**幂等**：总线已存在时直接返回 ESP_OK。
 * 端口配置照抄 imu_task 旧日的 i2c_bring_up()（I2C_NUM_0、SDA GPIO7 /
 * SCL GPIO8、400 kHz、glitch_ignore_cnt=7、内部上拉）——先保持 400 kHz，
 * 待整板波形和上拉实测后再调整（PLAN.md §6.1）。
 *
 * 调用时机：app_main 里、先于一切 I²C 器件 init（imu / baro / touch）。
 * 失败时总线不存在、pk_i2c0_bus_get() 恒 NULL，各器件 init 会各自报错——
 * 那是可诊断的路径，不在这里 abort。**契约（2026-09-05 深审裁定）**：
 * 调用方 main.c 记 ERROR 后继续启动，降级为无 IMU/baro/touch 的
 * 1090 盒子——不是 ESP_ERROR_CHECK 收口，不会重启。
 */
esp_err_t pk_i2c0_bus_init(void);

/*
 * ═══════════════════════════════════════════════════════════════════
 * 总线级互斥（2026-09 审计 P1/遗留）：事务 vs 复位
 * ═══════════════════════════════════════════════════════════════════
 * IDF 只对 transmit/receive/probe 的事务执行做了线程安全，而
 * i2c_master_bus_reset() **不取**内部 ops 锁（i2c_master.c:1241）：
 * 复位与一笔在飞的事务并发时，控制器状态机可能被中途拆掉。
 *
 * 因此本模块持一把 FreeRTOS mutex，把「器件事务」和「总线复位」在
 * 总线层面串行起来。**所有** I²C0 事务与复位必须走下面这组
 * pk_i2c0_bus_* 包装（内部取锁再转发）——这正是 i2c_master_bus_reset()
 * 在本项目里安全的前提；谁绕开包装直呼 IDF API，保证对谁失效
 * （审计 P1/遗留的收口）。已知例外见 touch_gt911.c 的 esp_lcd panel_io
 * 注释：组件内部事务无法包进本锁，残余窗口以「一次 100 ms 超时」计。
 *
 * i2c_master_bus_add_device / rm_device 不在包装范围：只许 init 期
 * 单线程调用（先于各恢复/轮询任务启动），无并发窗口。
 */

/* 器件事务（参数与 i2c_master_transmit/receive/transmit_receive 一一对应，
 * 仅多了内部取锁）。总线未建好时返回 ESP_ERR_INVALID_STATE。 */
esp_err_t pk_i2c0_bus_transmit(i2c_master_dev_handle_t dev,
                               const uint8_t *wr, size_t wr_len, int timeout_ms);
esp_err_t pk_i2c0_bus_receive(i2c_master_dev_handle_t dev,
                              uint8_t *rd, size_t rd_len, int timeout_ms);
esp_err_t pk_i2c0_bus_transmit_receive(i2c_master_dev_handle_t dev,
                                       const uint8_t *wr, size_t wr_len,
                                       uint8_t *rd, size_t rd_len, int timeout_ms);

/* 地址探活（内部用本模块的总线 handle；参数同 i2c_master_probe）。 */
esp_err_t pk_i2c0_bus_probe(uint8_t dev_addr, int timeout_ms);

/* 总线复位（恢复路径专用：目前只有 pk_i2c0_recover.c 该调）。 */
esp_err_t pk_i2c0_bus_reset(void);

/*
 * 全局唯一的 I²C0 handle。未 init 或 init 失败时返回 NULL——语义同旧
 * imu_task 版本的 pk_i2c0_bus_get()。s_bus 由 app_main 单线程在 init 期
 * 写入、之后只读，无并发保护。
 */
i2c_master_bus_handle_t pk_i2c0_bus_get(void);

/*
 * 成功复位过的总线轮数。开机为 0，每成功恢复一轮 +1（只由
 * pk_i2c0_recover.c 在恢复判据判真处递增，别处不要调 inc）。
 * 各器件任务的用法（沿用原 pk_i2c0_recover_generation() 的约定）：
 *
 *     uint32_t bus_gen = pk_i2c0_bus_generation();   // 任务起来时取一次
 *     ...
 *     uint32_t gen = pk_i2c0_bus_generation();
 *     if (gen != bus_gen) { bus_gen = gen; 重放自己的 bring_up(); }
 *
 * 用「变了没有」而不是「等于几」，谁先谁后、中途漏看几轮都不影响正确性。
 */
uint32_t pk_i2c0_bus_generation(void);

/* 代数 +1。**只许 pk_i2c0_recover.c 调**，且必须在其恢复闸门的临界区内
 * （与 pk_i2c0_gate_finish() 同点、同锁），单写者保证见上面的并发说明。 */
void pk_i2c0_bus_generation_inc(void);
