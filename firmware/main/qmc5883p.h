/*
 * qmc5883p.h — QMC5883P 三轴磁力计驱动，一阶段（WP-B Task 4）。
 *
 * 一阶段范围：探测 + 连续模式配置 + 1 Hz 轮询 + 原始三轴/溢出诊断计数。
 * **不出航向**（R1）：敏感轴未标定（pk_board_mag_axes_calibrated() 恒
 * false —— QMC5883P.pdf Rev A 通篇没有轴向图），输出只进诊断链路，
 * 不得进入航向融合或替换 BNO085 heading。
 *
 * 寄存器事实全部现场取自 hardware/datasheets/QMC5883P.pdf（Rev A,
 * Document #13-52-19）。引用页码一律按 PDF **页脚 N/18** 标注（页脚
 * 编号 = 阅读器物理页 + 1，首页封面不计）。
 *
 * 温度：任务简报要求「温度换算」，但 Rev A 全文没有温度输出寄存器——
 * §9.1 Table 14（p.15/18）的寄存器表只有 00H~06H/09H/0AH/0BH；第 1 页
 * 的 "Temperature Compensated Data Output" 指磁数据在片内完成温度补偿
 * （§5.6, p.11/18），不是可读的温度寄存器（QMC5883L 的 07H/08H TEMP 在
 * P 上无对应文档）。按「取不到的事实不写码」的硬约束，一阶段不含温度；
 * 等拿到 QST 的寄存器事实后再补。
 *
 * host 单测：test_qmc5883p.c 只编纯解码部分（QMC5883P_HOST_TEST 隔离，
 * 模式同 rf_safety / pk_i2c0_bus）；目标端专属部分靠两板系构建验证。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef QMC5883P_HOST_TEST
#include "esp_err.h"
#include "driver/i2c_master.h"
#else
/* host 单测（test_qmc5883p.c）没有 ESP-IDF：只影子化本头文件用到的
 * 名字。真类型见 IDF esp_err.h / driver/i2c_master.h，取值语义一致。 */
typedef int esp_err_t;
#define ESP_OK 0
typedef void *i2c_master_bus_handle_t;
#endif

/* 一次轮询的原始样本。诊断用途：芯片计数 + 封装系→机体 NED 变换后的
 * 三轴（R1：只是重映射，不含航向）。 */
typedef struct {
    bool    valid;                 /* 本次 poll 读到了新数据并解码成功 */
    int16_t x_raw, y_raw, z_raw;   /* 芯片输出计数（16 位二进制补码，LSB）*/
    float   body_mg[3];            /* 机体 NED 毫高斯（pk_board_mag_pkg_vec_to_body）*/
} qmc5883p_sample_t;

/* 状态寄存器 09H 的两个标志位（Table 16, p.15/18）。 */
typedef struct {
    bool drdy;   /* bit0：三轴新数据已就绪；读状态寄存器后自动清零 */
    bool ovfl;   /* bit1：任一轴输出超出 [-30000, 30000] LSB，读后清零 */
} qmc5883p_status_t;

/* 诊断计数（消费方：将来的诊断页）。 */
typedef struct {
    uint32_t polls;           /* 成功轮询次数（DRDY 且 6 字节解码成功）*/
    uint32_t overflows;       /* 状态 OVFL=1 的轮询次数 */
    uint32_t probe_failures;  /* 探测/配置整轮失败的次数 */
} qmc5883p_stats_t;

/* ---- 纯解码（host 可测）------------------------------------------- */

/*
 * 6 字节数据帧 → 三轴有符号值。帧布局 = 从 01H 连读 6 字节：
 * X LSB, X MSB, Y LSB, Y MSB, Z LSB, Z MSB（Table 14/15, p.15/18），
 * 小端拼装、16 位二进制补码（§9.2.1：MSB 寄存器是符号位，输出饱和在
 * ±32768）。全 0xFF 帧安全解码为 (-1, -1, -1)。
 */
void qmc5883p_decode_raw(const uint8_t *reg6, int16_t *x, int16_t *y, int16_t *z);

/*
 * 状态寄存器字节 → DRDY/OVFL。只看 bit0/bit1，其余位为厂测保留
 * （§9.2.2），一律忽略。
 */
void qmc5883p_decode_status(uint8_t s, qmc5883p_status_t *out);

/* 上电配置序列的单步：寄存器地址 → 写入值 + 判据说明。 */
typedef struct {
    uint8_t     reg;
    uint8_t     val;
    const char *why;   /* 判据出处（静态字面量），host 测试断言非空 */
} qmc5883p_init_step_t;

/*
 * 上电配置序列（数据表，host 可测）。返回表头；n 非空时写入步数。
 * 顺序与 29H=0x06 = QMC5883P.pdf Rev A §7.2 Continuous Mode Setup Example
 * 逐条；0BH/0AH 取值为 Table 17/18 字段级自选（±2G/10 Hz，出处见
 * qmc5883p.c 各常量行内引用）。首步必须是 29H=0x06 —— 见 qmc5883p.c
 * 表定义处的告警注释（29H 不在 Table 14 寄存器表里，是 example-only
 * 寄存器）。
 */
const qmc5883p_init_step_t *qmc5883p_init_seq(size_t *n);

/* ---- 目标端（I²C 胶水）-------------------------------------------- */

/*
 * 启动 QMC5883P：挂器件 + 起轮询任务（探测 00H CHIPID=0x80 → 按
 * qmc5883p_init_seq() 写配置 → 1 Hz 轮询）。设备是 optional 的：
 * bring-up 失败只在任务里记 WARN + 按 1 s 退避重试，永不删任务——
 * 器件 POR/软复位后会停在 Suspend（§5.2/§6.2.4），配置写得进去就能
 * 复活；缺焊是永久失败但同样无害（周期重试，不影响系统其余部分）。
 * 本函数只报器件挂载/任务创建失败。bus 为 NULL 时返回错误。
 */
esp_err_t qmc5883p_init(i2c_master_bus_handle_t bus);

/*
 * 同步轮询一次：读 09H 状态（DRDY=0 直接返回 false，§9.2.2）→ 连读
 * 01H~06H（§7.5 的测量流程）→ 解码 + 封装系→机体变换。返回 out->valid。
 * 一阶段只有驱动自己的任务调用它；计数器对所有调用方累计。
 */
bool qmc5883p_poll(qmc5883p_sample_t *out);

/* 诊断计数快照。单写者（驱动任务）多读者：内部计数为 C11 原子
 * （relaxed，同 modes_edge.h stats 口径——2026-09-05 pre-merge 批次
 * 升级，先于诊断页消费者就位），本函数把 relaxed load 逐字段拷入普通
 * 结构体；各计数单调、逐字段独立采样，不做跨字段一致性承诺。out 为
 * NULL 时忽略。 */
void qmc5883p_stats_get(qmc5883p_stats_t *out);
