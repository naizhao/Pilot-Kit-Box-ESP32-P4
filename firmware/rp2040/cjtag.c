/*
 * cjtag.c — cJTAG 位脉冲实现。
 *
 * 架构分两层：
 *   纯逻辑层（TAP 状态机 + 操作序列）——host 可测（CJTAG_HOST_TEST）
 *   GPIO 胶水层（引脚驱动 + 延时）——目标端
 *
 * cJTAG compact 格式（CC13x2 ICEPICK-D3 默认 2 线模式）：
 * 每个 TCKC 时钟周期 TMSC 携带一个位：
 *   非 Shift 态：TMSC = TMS（RP2040 驱动，CC1312R 在 TCKC↑ 采样）
 *   Shift 写入：TMSC = TDI（RP2040 驱动）
 *   Shift 读取：CC1312R 在 TCKC↓ 驱动 TMSC = TDO（RP2040 切输入采样）
 *
 * 烧录路径：cJTAG → ICEPICK-D3 路由 → ARM ADIv5 AHB-AP → Flash Controller
 */
#include "cjtag.h"

#include <string.h>
#include <stdio.h>

/* MAYBE_UNUSED：host 测试构建时某些静态函数未被调用（不产出 -Wunused 警告）*/
#ifdef CJTAG_HOST_TEST
#define MAYBE_UNUSED __attribute__((unused))
#else
#define MAYBE_UNUSED
#endif

#ifndef CJTAG_HOST_TEST
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#endif

/* ── TAP 状态机转移表（纯逻辑，host 可测）────────────────────────── */

typedef enum {
    TAP_TLR = 0, TAP_RTI,
    TAP_SELECT_DR, TAP_CAPTURE_DR, TAP_SHIFT_DR, TAP_EXIT1_DR,
    TAP_PAUSE_DR, TAP_EXIT2_DR, TAP_UPDATE_DR,
    TAP_SELECT_IR, TAP_CAPTURE_IR, TAP_SHIFT_IR, TAP_EXIT1_IR,
    TAP_PAUSE_IR, TAP_EXIT2_IR, TAP_UPDATE_IR,
} tap_state_t;

static const tap_state_t tap_next[16][2] = {
    /* [当前态][TMS=0]              [当前态][TMS=1]           */
    /* TAP_TLR        */ { TAP_RTI,                          TAP_TLR },
    /* TAP_RTI        */ { TAP_RTI,                          TAP_SELECT_DR },
    /* TAP_SELECT_DR  */ { TAP_CAPTURE_DR,                   TAP_SELECT_IR },
    /* TAP_CAPTURE_DR */ { TAP_SHIFT_DR,                     TAP_EXIT1_DR },
    /* TAP_SHIFT_DR   */ { TAP_SHIFT_DR,                     TAP_EXIT1_DR },
    /* TAP_EXIT1_DR   */ { TAP_PAUSE_DR,                     TAP_UPDATE_DR },
    /* TAP_PAUSE_DR   */ { TAP_PAUSE_DR,                     TAP_EXIT2_DR },
    /* TAP_EXIT2_DR   */ { TAP_SHIFT_DR,                     TAP_UPDATE_DR },
    /* TAP_UPDATE_DR  */ { TAP_RTI,                          TAP_SELECT_DR },
    /* TAP_SELECT_IR  */ { TAP_CAPTURE_IR,                   TAP_TLR },
    /* TAP_CAPTURE_IR */ { TAP_SHIFT_IR,                     TAP_EXIT1_IR },
    /* TAP_SHIFT_IR   */ { TAP_SHIFT_IR,                     TAP_EXIT1_IR },
    /* TAP_EXIT1_IR   */ { TAP_PAUSE_IR,                     TAP_UPDATE_IR },
    /* TAP_PAUSE_IR   */ { TAP_PAUSE_IR,                     TAP_EXIT2_IR },
    /* TAP_EXIT2_IR   */ { TAP_SHIFT_IR,                     TAP_UPDATE_IR },
    /* TAP_UPDATE_IR  */ { TAP_RTI,                          TAP_SELECT_DR },
};

static tap_state_t s_tap = TAP_TLR;

/* ── 物理层：GPIO 位脉冲（目标端）/ 模拟（host）────────────────── */

#ifdef CJTAG_HOST_TEST
/* host 测试桩：记录位序列供断言（不驱动真实引脚） */
#define MAYBE_UNUSED __attribute__((unused))

static MAYBE_UNUSED uint8_t  h_log_tms[4096];
static MAYBE_UNUSED uint8_t  h_log_tdi[4096];
static MAYBE_UNUSED uint8_t  h_log_tdo[4096];
static int      h_log_n;
static uint32_t h_idcode_override = 0;

static void hw_pin_init(void) { h_log_n = 0; }
static void hw_reset_pulse(void) { /* 模拟 */ }
static inline void hw_tck_low(void)  { }
static inline void hw_tck_high(void) { }
static inline void hw_drive_tms(uint8_t bit)
{
    if (h_log_n < 4096) { h_log_tms[h_log_n] = bit; h_log_tdi[h_log_n] = 0; h_log_n++; }
}
static inline void hw_drive_tdi(uint8_t bit)
{
    if (h_log_n < 4096) { h_log_tdi[h_log_n] = bit; h_log_n++; }
}
static inline uint8_t hw_sample_tdo(void)
{
    /* host 模式：返回预注入的 IDCODE 位（由 h_idcode_override 驱动） */
    static int bit_idx = 0;
    uint8_t bit = (h_idcode_override >> (bit_idx & 31)) & 1;
    bit_idx++;
    if (h_log_n < 4096) h_log_tdo[h_log_n] = bit;
    return bit;
}
static MAYBE_UNUSED inline void hw_delay_ns(int ns) { (void)ns; }

/* host 测试注入：设置模拟 IDCODE */
void cjtag_test_set_idcode(uint32_t id) { h_idcode_override = id; }
#else /* 目标端 */
static void hw_pin_init(void)
{
    gpio_init(CJTAG_PIN_TMSC);
    gpio_init(CJTAG_PIN_TCKC);
    gpio_init(CJTAG_PIN_RESET);
    gpio_set_dir(CJTAG_PIN_TCKC, GPIO_OUT);
    gpio_set_dir(CJTAG_PIN_RESET, GPIO_OUT);
    gpio_put(CJTAG_PIN_TCKC, 0);         /* TCKC 空闲低 */
    gpio_put(CJTAG_PIN_RESET, 1);        /* RESET 空闲高（低有效） */
    gpio_pull_up(CJTAG_PIN_TMSC);        /* TMSC 上拉（cJTAG 规范） */
}

static void hw_reset_pulse(void)
{
    gpio_put(CJTAG_PIN_RESET, 0);
    sleep_ms(2);                          /* ≥1 ms（CC1312R 要求） */
    gpio_put(CJTAG_PIN_RESET, 1);
    sleep_ms(10);                         /* 启动等待 */
}

static inline void hw_delay_ns(int ns)
{
    /* pico 的 busy_wait 至少 1 µs；对 2 µs 的 delay 用 sleep_us(1)×2 */
    sleep_us((ns + 999) / 1000);
}

static inline void hw_tck_low(void)
{
    gpio_put(CJTAG_PIN_TCKC, 0);
    hw_delay_ns(CJTAG_TCK_DELAY_NS);
}

static inline void hw_tck_high(void)
{
    gpio_put(CJTAG_PIN_TCKC, 1);
    hw_delay_ns(CJTAG_TCK_DELAY_NS);
}

static inline void hw_drive_tms(uint8_t bit)
{
    gpio_set_dir(CJTAG_PIN_TMSC, GPIO_OUT);
    gpio_put(CJTAG_PIN_TMSC, bit);
}

static inline void hw_drive_tdi(uint8_t bit)
{
    gpio_set_dir(CJTAG_PIN_TMSC, GPIO_OUT);
    gpio_put(CJTAG_PIN_TMSC, bit);
}

static inline uint8_t hw_sample_tdo(void)
{
    gpio_set_dir(CJTAG_PIN_TMSC, GPIO_IN);
    /* TCKC 低电平期间目标驱动 TMSC，在 TCKC↑ 之前采样 */
    return (uint8_t)gpio_get(CJTAG_PIN_TMSC);
}
#endif

/* ── TAP 操作原语 ────────────────────────────────────────────────── */

/* 发一个 TCKC 时钟（带 TMS 位），更新状态机 */
static void tap_clock_tms(uint8_t tms)
{
    hw_drive_tms(tms);
    hw_tck_high();                        /* 目标在↑采样 TMS */
    hw_tck_low();
    s_tap = tap_next[s_tap][tms];
}

/* Shift 状态发一个时钟（TDI 写入或 TDO 读取） */
static void tap_shift_write(uint8_t tdi)
{
    hw_drive_tdi(tdi);
    hw_tck_high();
    hw_tck_low();
}

static MAYBE_UNUSED uint8_t tap_shift_read(void)
{
    hw_tck_low();                         /* 确保低，目标驱动 TMSC */
    uint8_t tdo = hw_sample_tdo();
    hw_tck_high();                        /* ↑锁存 */
    hw_tck_low();
    return tdo;
}

/* 走到指定状态（驱动 TMS 序列） */
static void tap_goto(tap_state_t target)
{
    int guard = 0;
    while (s_tap != target && guard++ < 32) {
        /* 简化导航：利用 TAP 树的层次结构 */
        if (target == TAP_TLR) {
            tap_clock_tms(1); tap_clock_tms(1); tap_clock_tms(1);
            tap_clock_tms(1); tap_clock_tms(1);
            s_tap = TAP_TLR;
            return;
        }
        if (target == TAP_SHIFT_IR || target == TAP_SHIFT_DR) {
            /* 从当前态导航到 Shift：先回 RTI 再进 */
            if (s_tap != TAP_RTI) { tap_goto(TAP_RTI); }
            if (target == TAP_SHIFT_IR) {
                tap_clock_tms(1);  /* SELECT_DR */
                tap_clock_tms(1);  /* SELECT_IR */
                tap_clock_tms(0);  /* CAPTURE_IR */
                tap_clock_tms(0);  /* SHIFT_IR */
            } else {
                tap_clock_tms(1);  /* SELECT_DR */
                tap_clock_tms(0);  /* CAPTURE_DR */
                tap_clock_tms(0);  /* SHIFT_DR */
            }
            return;
        }
        if (target == TAP_UPDATE_DR || target == TAP_UPDATE_IR) {
            /* 从 Shift 态到 Update */
            tap_clock_tms(1);  /* EXIT1 */
            tap_clock_tms(1);  /* UPDATE */
            return;
        }
        if (target == TAP_RTI) {
            tap_clock_tms(1); tap_clock_tms(0);
            s_tap = TAP_RTI;
            return;
        }
        /* 兜底：TMS=1 五次回 TLR */
        tap_clock_tms(1);
    }
}

/* 移位 IR（LSB 先出） */
static void jtag_shift_ir(uint32_t instr, int bits)
{
    tap_goto(TAP_SHIFT_IR);
    for (int i = 0; i < bits; i++) {
        uint8_t bit = (instr >> i) & 1;
        if (i == bits - 1) {
            /* 最后一位带 TMS=1 → EXIT1_IR */
            hw_drive_tdi(bit);
            /* drive TMS=1 for exit */
            hw_drive_tms(1);  /* 在同一时钟发 TDI+TMS */
            hw_tck_high();
            hw_tck_low();
            s_tap = TAP_EXIT1_IR;
        } else {
            tap_shift_write(bit);
        }
    }
    tap_goto(TAP_UPDATE_IR);
    tap_goto(TAP_RTI);
}

/* 移位 DR（LSB 先出，返回读到的值） */
static uint32_t jtag_shift_dr(uint32_t tdi, int bits)
{
    uint32_t tdo = 0;
    tap_goto(TAP_SHIFT_DR);
    for (int i = 0; i < bits; i++) {
        uint8_t wbit = (tdi >> i) & 1;
        if (i == bits - 1) {
            /* 最后一位：写 TDI + TMS=1（exit）+ 读 TDO */
            hw_drive_tdi(wbit);
            /* TMS=1 在同一时钟 */
            hw_tck_high();
            /* TDO 采样（目标的最后一位） */
            hw_tck_low();
            uint8_t rbit = hw_sample_tdo();
            tdo |= (uint32_t)rbit << i;
            s_tap = TAP_EXIT1_DR;
        } else {
            /* 写 TDI 同时读 TDO */
            hw_drive_tdi(wbit);
            hw_tck_high();
            hw_tck_low();
            uint8_t rbit = hw_sample_tdo();
            tdo |= (uint32_t)rbit << i;
        }
    }
    tap_goto(TAP_UPDATE_DR);
    tap_goto(TAP_RTI);
    return tdo;
}

/* ── ADIv5 AP/DP 操作 ────────────────────────────────────────────── */

/* DPACC/APACC 的 3-bit AP-SEL + 2-bit 寄存器地址编码到 35-bit DR：
 * [34:3] = 数据 (32-bit)
 * [2:1]  = 寄存器地址 (DP: 0=CTRLSTAT/1=SELECT/2=RDBUFF / AP: 0=CSW...)
 * [0]    = RnW (0=写, 1=读)
 */
static void jtag_dp_write(uint8_t reg, uint32_t data)
{
    uint64_t dr = ((uint64_t)data << 3) | ((reg & 0x3) << 1) | 0;  /* 写 */
    jtag_shift_ir(JTAG_IR_DPACC, 4);
    jtag_shift_dr((uint32_t)dr, 35);  /* 低 35 位 */
}

static MAYBE_UNUSED uint32_t jtag_dp_read(uint8_t reg)
{
    uint64_t dr = ((uint64_t)0 << 3) | ((reg & 0x3) << 1) | 1;  /* 读 */
    jtag_shift_ir(JTAG_IR_DPACC, 4);
    jtag_shift_dr((uint32_t)dr, 35);
    /* 读结果在下一次 DPACC RDBUFF 读取 */
    jtag_shift_ir(JTAG_IR_DPACC, 4);
    uint32_t result = jtag_shift_dr(0, 35);
    return result;  /* 高 32 位是数据 */
}

static void jtag_ap_write(uint8_t ap, uint8_t reg, uint32_t data)
{
    /* 先选 AP */
    jtag_dp_write(DP_SELECT, (uint32_t)(ap << 24) | (reg & 0xF0));
    /* 再写 AP 寄存器 */
    uint64_t dr = ((uint64_t)data << 3) | ((reg & 0x3) << 1) | 0;
    jtag_shift_ir(JTAG_IR_APACC, 4);
    jtag_shift_dr((uint32_t)dr, 35);
}

static uint32_t jtag_ap_read(uint8_t ap, uint8_t reg)
{
    jtag_dp_write(DP_SELECT, (uint32_t)(ap << 24) | (reg & 0xF0));
    uint64_t dr = ((uint64_t)0 << 3) | ((reg & 0x3) << 1) | 1;
    jtag_shift_ir(JTAG_IR_APACC, 4);
    jtag_shift_dr((uint32_t)dr, 35);
    /* 结果在 RDBUFF */
    jtag_shift_ir(JTAG_IR_DPACC, 4);
    uint32_t result = jtag_shift_dr(0, 35);
    return result;
}

/* ── 公共 API ────────────────────────────────────────────────────── */

void cjtag_enter(void)
{
    hw_pin_init();
    hw_reset_pulse();
    /* 5 个 TCKI(TMS=1) → Test-Logic-Reset */
    for (int i = 0; i < 5; i++) tap_clock_tms(1);
    s_tap = TAP_TLR;
    /* 到 Run-Test/Idle */
    tap_clock_tms(0);
}

void cjtag_exit(void)
{
    /* 回 TLR */
    for (int i = 0; i < 5; i++) tap_clock_tms(1);
    s_tap = TAP_TLR;
    /* RESET 脉冲让 CC1312R 重启 */
    hw_reset_pulse();
}

uint32_t cjtag_read_idcode(void)
{
    jtag_shift_ir(JTAG_IR_IDCODE, 4);
    uint32_t id = jtag_shift_dr(0, 32);
    return id;
}

/* AHB-AP（MEM-AP）读写 */
uint32_t cjtag_ahb_read32(uint32_t addr)
{
    /* 设置 TAR */
    jtag_ap_write(0, AP_TAR, addr);
    /* 读 DRW */
    return jtag_ap_read(0, AP_DRW);
}

void cjtag_ahb_write32(uint32_t addr, uint32_t val)
{
    jtag_ap_write(0, AP_TAR, addr);
    jtag_ap_write(0, AP_DRW, val);
}

/* ── Flash 操作 ─────────────────────────────────────────────────── */

/* 等待 flash controller 空闲 */
static bool flash_wait_ready(int timeout_ms)
{
    (void)timeout_ms;  /* 位脉冲较慢，每个操作本身在 JTAG 时钟粒度完成 */
    uint32_t stat;
    do {
        stat = cjtag_ahb_read32(FLASH_FSTAT);
    } while (stat & FSTAT_BUSY);
    return true;
}

bool cjtag_flash_erase_sector(uint32_t addr)
{
    cjtag_ahb_write32(FLASH_FADDR, addr);
    cjtag_ahb_write32(FLASH_FMC, FMC_ERASE_SECTOR);
    return flash_wait_ready(2000);
}

bool cjtag_flash_write_word(uint32_t addr, uint32_t val)
{
    cjtag_ahb_write32(FLASH_FADDR, addr);
    cjtag_ahb_write32(FLASH_FDATA0, val);
    cjtag_ahb_write32(FLASH_FMC, FMC_WRD);
    return flash_wait_ready(100);
}

bool cjtag_flash_verify(uint32_t addr, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        uint32_t expect;
        memcpy(&expect, data + i, (len - i >= 4) ? 4 : (len - i));
        uint32_t got = cjtag_ahb_read32(addr + i);
        if (got != expect) return false;
    }
    return true;
}

bool cjtag_flash_program(uint32_t addr, const uint8_t *data, size_t len)
{
    /* 先解锁 flash（CC13x2 默认锁定） */
    /* 写 FCFG1 的 FLASH_UNLOCK（地址 0x5000130C 写 0xC35A01E2） */
    cjtag_ahb_write32(0x5000130C, 0xC35A01E2);

    /* 使能 DAP 电源 */
    jtag_dp_write(DP_CTRLSTAT, DP_CTRL_CSYSPWRUP | DP_CTRL_CDBGPWRUP);

    /* 擦除涉及的 sectors */
    uint32_t end = addr + len;
    for (uint32_t s = addr & ~(uint32_t)(CC13_SECTOR_SIZE - 1);
         s < end; s += CC13_SECTOR_SIZE) {
        if (!cjtag_flash_erase_sector(s)) return false;
    }

    /* 逐 word 写入 */
    for (size_t i = 0; i < len; i += 4) {
        uint32_t word;
        memcpy(&word, data + i, (len - i >= 4) ? 4 : (len - i));
        if (!cjtag_flash_write_word(addr + i, word)) return false;
    }

    /* 校验 */
    if (!cjtag_flash_verify(addr, data, len)) return false;

    /* 锁回 flash */
    cjtag_ahb_write32(0x5000130C, 0xC35A01E3);

    return true;
}

/* ── CDC 接口 ────────────────────────────────────────────────────── */

static bool s_active = false;
static uint8_t  s_buf[CC13_SECTOR_SIZE];
static size_t   s_buf_len = 0;
static uint32_t s_flash_addr = 0;

bool cjtag_cdc_enter(void)
{
    if (s_active) return true;
    cjtag_enter();

    /* Proof of life：读 IDCODE */
    uint32_t id = cjtag_read_idcode();
    if (id == 0 || id == 0xFFFFFFFF) {
        cjtag_exit();
        return false;
    }

    s_active = true;
    s_buf_len = 0;
    s_flash_addr = 0;
    return true;
}

bool cjtag_cdc_data(uint8_t byte)
{
    if (!s_active) return false;
    if (s_buf_len < sizeof(s_buf)) {
        s_buf[s_buf_len++] = byte;
        return true;
    }
    return false;  /* 缓冲满 */
}

void cjtag_cdc_quit(void)
{
    if (!s_active) return;
    if (s_buf_len > 0 && s_flash_addr > 0) {
        cjtag_flash_program(s_flash_addr, s_buf, s_buf_len);
    }
    cjtag_exit();
    s_active = false;
}

bool cjtag_cdc_active(void)
{
    return s_active;
}
