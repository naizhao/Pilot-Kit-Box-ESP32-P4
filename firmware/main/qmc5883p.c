/*
 * qmc5883p.c — QMC5883P 三轴磁力计驱动，一阶段（WP-B Task 4）。
 *
 * 结构同 rf_safety：纯解码函数 host 可测（QMC5883P_HOST_TEST 隔离），
 * 目标端 I²C 胶水关在 #ifndef 里靠两板系构建验证。
 *
 * ── 寄存器事实取证（QMC5883P.pdf Rev A, Document #13-52-19）──────────
 * 页码按 PDF 页脚 N/18（= 阅读器物理页 + 1）：
 *   I²C 地址 0x2C ......... §5.4 p.11/18（"default I2C address ... is 2CH"）
 *   00H  CHIPID，默认 0x80 . §9.1 Table 14 + §9.2.1，p.15/18
 *   01H~06H 数据寄存器 .... §9.1 Table 14 + §9.2.1 + Table 15，p.15/18
 *                           （X/Y/Z 各 16 位二进制补码，LSB 寄存器在低地址，
 *                           输出饱和 ±32768）
 *   09H  状态 ............. §9.2.2 + Table 16，p.15/18（bit0 DRDY、
 *                           bit1 OVFL，只读，读后自清）；OVFL 门限
 *                           [-30000, 30000] LSB 在 §9.2.2 p.16/18
 *   0AH  控制寄存器 1 ..... §9.1 Table 14 + Table 17，p.15-16/18
 *                           （OSR2[7:6] OSR1[5:4] ODR[3:2] MODE[1:0]；
 *                           MODE: 00 挂起/01 正常/10 单次/11 连续；
 *                           ODR: 00 10Hz/01 50Hz/10 100Hz/11 200Hz）
 *   0BH  控制寄存器 2 ..... §9.1 Table 14 + Table 18，p.15-17/18
 *                           （SOFT_RST[7] SELF_TEST[6] RNG[3:2]
 *                           SET/RESET[1:0]；RNG: 00 ±30G/01 ±12G/
 *                           10 ±8G/11 ±2G；SET/RESET: 00 = set+reset on）
 *   连写示例 0AH=0x03 ..... §7.3 p.12/18（"set continuous mode"）
 *   29H  符号定义寄存器 ... §7.1/§7.2/§7.3 p.12/18：三个官方设置示例
 *                           （Normal/Continuous/Self-test）无一例外以
 *                           「Write Register 29H by 0x06」开头（"Define
 *                           the sign for X Y and Z axis"）。**29H 不在
 *                           §9.1 Table 14 的寄存器表里**——它只通过应用
 *                           示例成文（这正是 2026-09 审计漏掉它的原因）；
 *                           别因为「表里没有」把它当废步清理掉。
 *   Suspend 状态 .......... §5.2/§6.2.4：POR/软复位后器件停在 Suspend，
 *                           I²C 仍应答、CHIPID 仍可读，但不再测量——
 *                           轮询只看到 DRDY=0；重写配置序列即可唤醒。
 *   灵敏度 ±2G=15000 LSB/G §2.1 Table 2，p.5/18
 *   测量流程 .............. §7.5 p.12/18（先查 09H[0]，再读 01H~06H）
 *   温度输出寄存器 ........ **无**：Table 14 寄存器表没有温度寄存器，
 *                           p.1 的 "Temperature Compensated Data Output"
 *                           指片内补偿（§5.6 p.11/18）。见 qmc5883p.h 头注。
 */

#include "qmc5883p.h"

#include <stddef.h>

/* ── 寄存器与器件常量（取证页码见文件头表）───────────────────────────── */

#define QMC5883P_I2C_ADDR   0x2C  /* §5.4 p.11/18 */
#define QMC5883P_REG_CHIPID 0x00  /* Table 14 p.15/18 */
#define QMC5883P_CHIPID_VAL 0x80  /* §9.2.1 p.15/18 */
#define QMC5883P_REG_DATA   0x01  /* X LSB 起 6 字节，Table 14 p.15/18 */
#define QMC5883P_REG_STATUS 0x09  /* Table 14 p.15/18 */
#define QMC5883P_REG_CTRL1  0x0A  /* Table 17 p.16/18 */
#define QMC5883P_REG_CTRL2  0x0B  /* Table 18 p.16/18 */

/* 一阶段配置值（都是「字段值拼装」，位段定义见表 17/18）：
 *
 * CTRL2 (0BH) = 0x0C = 0b0000_1100
 *   RNG[3:2]=11 → ±2 Gauss（Table 18 p.17/18）
 *   SET/RESET[1:0]=00 → set+reset on（Table 18 p.17/18；§7.2 p.12/18 的
 *   连续模式示例写 0x08 同法，仅量程换 2G）
 *
 * CTRL1 (0AH) = 0x03 = 0b0000_0011
 *   OSR2[7:6]=00 → 下采样 1；OSR1[5:4]=00 → 过采样 8（Table 17 p.16/18，
 *   最低噪声档）
 *   ODR[3:2]=00 → 10 Hz（轮询 1 Hz 的 10 倍余量，DRDY 恒有新数据可读）
 *   MODE[1:0]=11 → 连续模式（Table 17 p.16/18；§7.3 p.12/18 用同一值
 *   0x03 进连续模式）
 */
#define QMC5883P_CTRL2_CFG   0x0C
#define QMC5883P_CTRL1_CFG   0x03

#define QMC5883P_STATUS_DRDY 0x01 /* Table 16 p.15/18, bit0 */
#define QMC5883P_STATUS_OVFL 0x02 /* Table 16 p.15/18, bit1 */

/* 29H 符号定义（§7.1/§7.2/§7.3 示例统一值 0x06；见文件头告警——
 * Table 14 里没有这个寄存器，判据只存在于应用示例）。 */
#define QMC5883P_REG_SIGN    0x29
#define QMC5883P_SIGN_CFG    0x06

/* ── 初始化序列（数据表，host 可测）────────────────────────────────────
 * 顺序与取值 = PDF §7.2 Continuous Mode Setup Example 逐条（§7.1 Normal /
 * §7.3 Self-test 两个示例同样以 29H=0x06 开头，p.12/18）。0BH 先于 0AH：
 * 模式位最后落笔，器件带着定好的量程进入连续测量。
 *
 * ⚠ 首步 29H 不在 §9.1 Table 14 的寄存器表里（p.15/18 只列
 * 00H~06H/09H/0AH/0BH），它只出现在 §7 的应用示例中——**不要**因为
 * 「表里没有」就把它当废步清理掉（2026-09 审计 P1 即因此漏配）。 */
static const qmc5883p_init_step_t s_init_seq[] = {
    { QMC5883P_REG_SIGN, QMC5883P_SIGN_CFG,
      "sign for X/Y/Z (§7.2; NOT in Table 14 — example-only register)" },
    { QMC5883P_REG_CTRL2, QMC5883P_CTRL2_CFG,
      "RNG=±2G + set/reset on (Table 18 p.17/18)" },
    { QMC5883P_REG_CTRL1, QMC5883P_CTRL1_CFG,
      "OSR2=1/OSR1=8/ODR=10Hz/cont mode (Table 17 p.16/18)" },
};

const qmc5883p_init_step_t *qmc5883p_init_seq(size_t *n)
{
    if (n != NULL) *n = sizeof(s_init_seq) / sizeof(s_init_seq[0]);
    return s_init_seq;
}

/* ── 纯解码（host 可测）─────────────────────────────────────────────── */

void qmc5883p_decode_raw(const uint8_t *reg6, int16_t *x, int16_t *y, int16_t *z)
{
    if (reg6 == NULL || x == NULL || y == NULL || z == NULL) return;
    /* 小端：01H/03H/05H 是低字节，02H/04H/06H 是高字节（Table 14/15）。
     * 中间用无符号拼装再转补码，避免有符号移位的实现定义行为。 */
    *x = (int16_t)((uint16_t)reg6[0] | ((uint16_t)reg6[1] << 8));
    *y = (int16_t)((uint16_t)reg6[2] | ((uint16_t)reg6[3] << 8));
    *z = (int16_t)((uint16_t)reg6[4] | ((uint16_t)reg6[5] << 8));
}

void qmc5883p_decode_status(uint8_t s, qmc5883p_status_t *out)
{
    if (out == NULL) return;
    out->drdy = (s & QMC5883P_STATUS_DRDY) != 0;
    out->ovfl = (s & QMC5883P_STATUS_OVFL) != 0;
}

/* ── 目标端（I²C 胶水）：host 单测不编译，靠两板系构建 + 启动日志验证 ── */

#ifndef QMC5883P_HOST_TEST

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "pk_i2c0_bus.h"   /* pk_i2c0_bus_get/generation —— 总线是板级的 */
#include "pk_board.h"      /* pk_board_mag_pkg_vec_to_body —— 封装系→机体 NED */

static const char *TAG = "qmc";

/* 与 BNO085/BMP388/GT911 同在 I²C0（400 kHz，见 pk_i2c0_bus.h）。 */
#define QMC5883P_I2C_HZ      400000

/* ±2G 满量程的灵敏度（§2.1 Table 2 p.5/18）：15000 LSB/G = 15 LSB/mG。
 * 一阶段选 ±2G 是为了诊断分辨率最大；地磁场 ~0.25-0.65 G，远在量程内。 */
#define QMC5883P_LSB_PER_G   15000.0f

/* 0BH/0AH 配置值的位段拆解注释在文件顶部的常量区（host 单测也要编译
 * 那段——初始化数据表引用这两个值）。 */

static i2c_master_dev_handle_t s_dev;

/* 诊断计数：单写者（qmc 任务 / init 前无并发），32 位对齐读原子，
 * volatile 防读者缓存——并发约定同 pk_i2c0_bus.h 的 s_gen。 */
static volatile qmc5883p_stats_t s_stats;

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100);
}

/*
 * 探测 + 配置（可选器件，不触发总线恢复——那是 baro 的职责，见下）：
 *   1. 读 00H，必须等于 0x80（§9.2.1 p.15/18）。10 次 × 100 ms；
 *   2. 按 s_init_seq 数据表逐条 reg_write（29H → 0BH → 0AH，见表定义处的
 *      判据注释）。
 * 任一步失败返回 false。失败时 probe_failures 计一次（整轮失败算一次，
 * 不按重试次数膨胀）。
 */
static bool bring_up(void)
{
    uint8_t id = 0;
    for (int retry = 0; retry < 10; retry++) {
        if (reg_read(QMC5883P_REG_CHIPID, &id, 1) == ESP_OK &&
            id == QMC5883P_CHIPID_VAL) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (id != QMC5883P_CHIPID_VAL) {
        ESP_LOGW(TAG, "CHIPID probe failed (got 0x%02X, want 0x80 @0x2C)", id);
        s_stats.probe_failures++;
        return false;
    }
    esp_err_t e;
    size_t n = 0;
    const qmc5883p_init_step_t *seq = qmc5883p_init_seq(&n);
    for (size_t i = 0; i < n; i++) {
        if ((e = reg_write(seq[i].reg, seq[i].val)) != ESP_OK) {
            ESP_LOGW(TAG, "config write 0x%02X failed: %d (%s)",
                     seq[i].reg, e, seq[i].why);
            s_stats.probe_failures++;
            return false;
        }
    }
    return true;
}

/* 1 Hz 轮询任务。结构照抄 baro_task：代数比对重放 bring-up + 循环轮询。
 *
 * 与 baro 的两处有意差异（qmc 是 optional 诊断器件）：
 *   - 探测失败**不调** pk_i2c0_recover_request()：QMC 缺焊是合法状态，
 *     不能让它周期性触发整板总线复位去打扰 baro/touch；
 *   - bring-up / 重放失败**永不删任务**（2026-09 审计 P2）：WARN +
 *     1 s 退避后下一轮重试。POR/软复位会把器件打进 Suspend（§5.2/
 *     §6.2.4：I²C 仍应答、CHIPID 仍可读、不再测量），配置写得进去就能
 *     复活——旧代码失败即 vTaskDelete，一次故障就永久失明到重启。
 * 轮询侧同理：连续 10 轮（≈10 s）拿不到有效样本就重放一遍配置序列，
 * 自愈 Suspend；拿到任何有效样本即清零计数。 */
static void qmc5883p_task(void *arg)
{
    (void)arg;
    uint32_t bus_gen = pk_i2c0_bus_generation();
    int fail_streak = 0;   /* 连续无有效样本的轮数（1 轮 ≈ 1 s） */

    while (!bring_up()) {
        ESP_LOGW(TAG, "QMC5883P bring-up 失败，1 s 后重试（可选器件，不放弃）");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "QMC5883P ready @0x%02X (cont mode, ODR=10Hz, ±2G)",
             QMC5883P_I2C_ADDR);

    while (1) {
        /* 总线被救回来了 → 重放探测+配置（pk_i2c0_bus.h 的器件侧契约）。 */
        const uint32_t gen = pk_i2c0_bus_generation();
        if (gen != bus_gen) {
            bus_gen = gen;
            ESP_LOGW(TAG, "I²C0 总线已复位（第 %lu 轮）— 重放 QMC 配置",
                     (unsigned long)gen);
            if (!bring_up()) {
                ESP_LOGW(TAG, "QMC5883P 重放失败，1 s 后重试");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        qmc5883p_sample_t s;
        if (qmc5883p_poll(&s)) {
            fail_streak = 0;
            /* 1 Hz 诊断日志（同 baro 的频率惯例）。body_mg 已是机体 NED
             * 毫高斯；R1：只有原始轴/变换轴，永不换算航向。 */
            ESP_LOGI(TAG, "raw=(%d,%d,%d) body_mG=(%.0f,%.0f,%.0f)",
                     s.x_raw, s.y_raw, s.z_raw,
                     s.body_mg[0], s.body_mg[1], s.body_mg[2]);
        } else if (++fail_streak >= 10) {
            /* 10 s 无有效样本：ODR=10 Hz 下本应拍拍有数。最可疑的是器件
             * 停在 Suspend（§5.2/§6.2.4）——重放配置序列唤醒；总线真坏
             * 时这些写会失败、下轮照常走失败重试分支。 */
            ESP_LOGW(TAG, "连续 %d 轮无有效样本 — 重放 QMC 配置（Suspend 自愈）",
                     fail_streak);
            if (!bring_up()) {
                ESP_LOGW(TAG, "自愈重放失败，下轮再试");
            }
            fail_streak = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t qmc5883p_init(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C0 bus not ready");
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = QMC5883P_I2C_ADDR,
        .scl_speed_hz    = QMC5883P_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device: %d", err);
        return err;
    }

    /* 诊断器件：栈同 baro（4096），优先级压到 baro(4)/imu 之下。 */
    BaseType_t ok = xTaskCreatePinnedToCore(qmc5883p_task, "qmc", 4096,
                                            NULL, 3, NULL, 0);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "task create failed");
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool qmc5883p_poll(qmc5883p_sample_t *out)
{
    if (out == NULL || s_dev == NULL) return false;
    out->valid = false;

    /* §7.5 p.12/18 的测量流程：先查 09H[0]，就绪再读 01H~06H。
     * 读状态本身会把 DRDY 清零（§9.2.2 p.15/18）。 */
    uint8_t st = 0;
    if (reg_read(QMC5883P_REG_STATUS, &st, 1) != ESP_OK) return false;
    qmc5883p_status_t flags;
    qmc5883p_decode_status(st, &flags);
    if (flags.ovfl) s_stats.overflows++;   /* §9.2.2 p.16/18：OVFL=1；
                                            * 只要状态里见过就计，不等 DRDY */
    if (!flags.drdy) return false;         /* 无新数据，不算失败 */

    uint8_t d[6];
    if (reg_read(QMC5883P_REG_DATA, d, 6) != ESP_OK) return false;
    qmc5883p_decode_raw(d, &out->x_raw, &out->y_raw, &out->z_raw);

    /* 封装系计数 → 机体 NED，再按 ±2G 灵敏度换算 mG（Table 2 p.5/18）。
     * 只是坐标重映射 + 定标：R1 红线是「不算航向」，不在此处。 */
    const float pkg[3] = { (float)out->x_raw, (float)out->y_raw,
                           (float)out->z_raw };
    pk_board_mag_pkg_vec_to_body(pk_board_profile(), pkg, out->body_mg);
    for (int i = 0; i < 3; i++) {
        out->body_mg[i] /= (QMC5883P_LSB_PER_G / 1000.0f);
    }

    out->valid = true;
    s_stats.polls++;
    return true;
}

void qmc5883p_stats_get(qmc5883p_stats_t *out)
{
    if (out == NULL) return;
    out->polls          = s_stats.polls;
    out->overflows      = s_stats.overflows;
    out->probe_failures = s_stats.probe_failures;
}

#endif /* QMC5883P_HOST_TEST */
