/*
 * board_pins.h — RP2040 唯一 pin 清单。
 * 来源：hardware/expansion-board-v4/PINMAP.md §2（v3/v4 同源）。
 * 改任何一个脚都要先改 PINMAP，再改这里——顺序不能反。
 */
#pragma once

#define PIN_P4_UART_TX    0   /* UART0 TX → P4 GPIO46（J3-35）*/
#define PIN_P4_UART_RX    1   /* UART0 RX ← P4 GPIO32（J3-31）*/
#define PIN_BIAS_EN_1090  2   /* bias tee PMOS gate：低有效，10k 上拉默认关 */
#define PIN_BIAS_EN_978   3   /* 同上（978 支路）*/
#define PIN_GNSS_SEL_A    6   /* U17 V1 / Q4 gate（ECO 后配对）*/
#define PIN_GNSS_SEL_B    7   /* U17 V2 / Q5 gate */
#define PIN_ANT_SEL_1090_A 4  /* U16 SPDT V1：A=1/B=0 → J1-J3（板载 IFA）；
                               * A=0/B=1 → J1-J2（外接 J6）。默认装配，
                               * 真值表与 GNSS 开关一致（netlist U8.6/U8.7）*/
#define PIN_ANT_SEL_1090_B 5  /* U16 SPDT V2（netlist U8.7）*/
#define PIN_SUBG_SCK      10  /* CC1312R SPI1 —— WP-E 启用前不动 */
#define PIN_SUBG_MOSI     11
#define PIN_SUBG_MISO     12
#define PIN_SUBG_CSN      13
#define PIN_SUBG_IRQ      14
#define PIN_SUBG_SYNC     15
#define PIN_PULSES        19  /* TLV3501 → PIO 上升沿捕获 */
#define PIN_SELFTEST_OUT  24  /* 自检脉冲输出；台架跳线 24→19（≥1k 串联电阻，
                               * 19 上 TLV3501 是推挽驱动，直接对接会打架）。
                               * audit P1-4：原 18 是 SUBG_RESET（CC1312R），
                               * PINMAP §2 严禁挪用；24 = RECOVERED_CLK，
                               * v4 无落点（v3 仅 TP7），空置可安全借用 */
#define PIN_TL_PWM        25  /* 门限 PWM → R34/C49（RC≈1ms，F5）→ LEVEL_BIAS */
#define PIN_ADC_LEVEL     26  /* ADC0：门限直流回读 */
#define PIN_ADC_RSSI      27  /* ADC1：AD8313 RSSI */

/* F2 默认 GNSS 天线选择：A=0/B=1 → J1-J2（外接）导通且其 bias 路径
 * 被选中（ECO 后选择线即馈电使能，PLAN.md §3.1 F2）。台架默认外接。 */
#define RF_SAFETY_DEFAULT_GNSS_A 0
#define RF_SAFETY_DEFAULT_GNSS_B 1
