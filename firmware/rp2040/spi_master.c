/*
 * spi_master.c — RP2040 侧 CC1312R SPI master：纯逻辑状态机（host 可测）
 * + 目标端胶水（SPI1 DMA / GPIO / RESET 脉冲）。
 *
 * 规范：firmware/PROTOCOL_RP2040_CC1312R_SPI.md v1.0。本实现是 §6.7 所称
 * "master 侧行为合同（WP-E 实现目标）"的第一个消费者——凡与规范冲突，
 * 以规范为准；发现规范缺陷上报，不得在代码里私自变通。
 *
 * 纯逻辑/胶水分离：状态机只依赖 codec 与时基参数；SPI1 事务执行、
 * GPIO14（IRQ，电平高有效）采样、GPIO18（RESET，≥1 ms 低脉冲，§1）与
 * UAT 帧下行（adsb_link UAT_UPLINK → p4_link 队列）全部在目标区。
 * host 测试以 mock slave 驱动（test_spi_master.c，B30/B34/B35 等走查
 * 场景全部可执行化——十轮审计"文档走查不可执行"的正式回应）。
 */
#include "spi_master.h"

#include <string.h>

#ifndef SPI_MASTER_HOST_TEST
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#endif

/* 本端固件版本（HELLO §4.1 携带）。装配集成点（1090 主循环接线）可
 * 换成构建注入的真实版本；host 测试用占位值。 */
#ifndef SPIM_FW_VER_MAJOR
#define SPIM_FW_VER_MAJOR 0
#endif
#ifndef SPIM_FW_VER_MINOR
#define SPIM_FW_VER_MINOR 1
#endif

/* ── 内部工具 ─────────────────────────────────────────────────────── */

static uint32_t elapsed_us(uint32_t now, uint32_t since)
{
    return now - since;            /* 模 2^32 差值，回绕安全 */
}

static void pad_frame(uint8_t *buf, size_t n)
{
    /* 定长事务（§2.1）：帧尾补 0 至 512 B，接收方丢弃填充。 */
    if (n < 512) memset(buf + n, 0, 512u - n);
}

/* 事件类帧判定（§3.5）由 digest 的分发处逐类型显式标注（ev_txn），
 * 不再经集中判定函数——保留逐类型注释以便对照规范。 */

/* ── 状态机 ───────────────────────────────────────────────────────── */

void spi_master_init(spi_master_t *m, spim_uat_fn uat_cb, void *user)
{
    memset(m, 0, sizeof(*m));
    m->state = SPIM_WAIT_HELLO;
    /* §6.2：进入 WAIT_HELLO 首个 HELLO 立即发出（t_last_hello 置为
     * "已过期"），此后每 500 ms 重试。10 次窗口（§6.2）以首个 HELLO
     * 起算 ≈ 5 s。 */
    m->t_last_hello = (uint32_t)(0u - SPIM_HELLO_RETRY_US);
    rp_cc13xx_reasm_init(&m->reasm);
    m->uat_cb  = uat_cb;
    m->uat_user = user;
}

/* LINKED → RECOVERY（§6.6）：请求 RESET 脉冲（胶水执行）。 */
static void enter_recovery(spi_master_t *m)
{
    m->state       = SPIM_RECOVERY;
    m->reset_req   = true;
    m->t_reset_req = m->t_now;
    m->stats.recoveries++;
}

void spi_master_reset_done(spi_master_t *m, uint32_t now_us)
{
    /* §6.1：RESET 脉冲 + ≥100 ms 启动等待由胶水完成后调用。
     * 会话全清（slave 随复位清零其全部会话态，§6.5/§6.6）。 */
    m->reset_req    = false;
    m->state        = SPIM_WAIT_HELLO;
    m->hello_tries  = 0;
    m->t_last_hello = now_us - SPIM_HELLO_RETRY_US;  /* 立即发首个 HELLO */
    m->t_last_legal = now_us;
    m->ev_seq_valid = false;
    m->rf_pending   = false;
    m->stall_cnt    = 0;      /* 无事件事务串（drain 停滞，§7.2） */
    m->t_irq_high   = 0;
    m->t_now        = now_us;
    rp_cc13xx_reasm_init(&m->reasm);
}

size_t spi_master_next_txn(spi_master_t *m, uint32_t now_us,
                           uint8_t mosi[512], bool irq_high)
{
    size_t n = 0;
    m->t_now = now_us;

    /* drain 停滞与假粘触发（§7.2/§6.6）——在决策点判定，进入 IRQ_ACK
     * 流水前先看是否该 RECOVERY。 */
    if (m->state == SPIM_LINKED) {
        const bool stalled =
            irq_high && m->stall_cnt >= SPIM_STALL_MAX;
        /* §6.6 irq_spurious：IRQ 持续高 >1 s 且**期间**事务均无事件。
         * 实现取舍（较字面的宽松化，方向保守——只在病态下更早进
         * RECOVERY，结局与 §7.2 stall 一致）：以"最近事件事务时刻"
         * 滑动回看近似窗口内无事件（窗口中途有过事件、随后静默 >1s
         * 的混合情形会提前触发）；drain 态不另设门——stall（8 事务）
         * 通常先到，本条在慢线速下兜底。 */
        const bool spur =
            irq_high && m->t_irq_high != 0 &&
            elapsed_us(now_us, m->t_irq_high) > SPIM_IRQ_SPURIOUS_US &&
            elapsed_us(now_us, m->t_last_event) > SPIM_IRQ_SPURIOUS_US;
        const bool silent =
            m->t_irq_high == 0 &&   /* drain 挂起时事件流即活性，不计时 */
            elapsed_us(now_us, m->t_last_legal) > SPIM_WATCHDOG_US;
        if (stalled || spur || silent) {
            enter_recovery(m);
            return 0;
        }
    }

    /* IRQ 高电平沿记录（假粘窗口起点；读低即清） */
    if (irq_high) {
        if (m->t_irq_high == 0) m->t_irq_high = now_us;
    } else {
        m->t_irq_high = 0;
        m->stall_cnt  = 0;   /* IRQ 低：停滞串失效（§7.2 出口语境） */
    }

    switch (m->state) {
    case SPIM_WAIT_HELLO:
        if (m->hello_tries >= SPIM_HELLO_MAX_TRIES) {
            /* §6.2：连续 10 次 HELLO 无合法应答 → RESET 重来。 */
            enter_recovery(m);
            return 0;
        }
        if (elapsed_us(now_us, m->t_last_hello) >= SPIM_HELLO_RETRY_US) {
            /* §4.1：HELLO 携带本端固件版本（encode_empty 只收空载荷
             * 命令——HELLO 不在其列，用 encode_hello）。 */
            rp_cc13xx_hello_t h = {
                .fw_ver_major = SPIM_FW_VER_MAJOR,
                .fw_ver_minor = SPIM_FW_VER_MINOR,
                .reset_reason = 0,
            };
            n = rp_cc13xx_encode_hello(mosi, 512, m->cmd_seq++, &h);
            m->t_last_hello = now_us;
            m->hello_tries++;
            m->stats.txns++;
        }
        break;

    case SPIM_LINKED:
        if (m->rf_pending) {
            /* §6.4：LINKED 后 1 s 内 RF_CONFIG 查询（0 = 只读，§4.6）。 */
            rp_cc13xx_rf_config_t q = { .config_version = 0 };
            n = rp_cc13xx_encode_rf_config(mosi, 512,
                                           RP_CC13XX_MSG_RF_CONFIG,
                                           m->cmd_seq++, &q);
            m->rf_pending = false;
        } else if (irq_high) {
            /* §6.7 调度合同：IRQ 高 → 连发 IRQ_ACK 至读低（drain）。 */
            n = rp_cc13xx_encode_empty(mosi, 512, RP_CC13XX_MSG_IRQ_ACK,
                                       m->cmd_seq++);
        } else if (elapsed_us(now_us, m->t_last_ping) >= SPIM_LINKED_PING_US) {
            /* §6.6：LINKED 1 Hz PING，不看 IRQ。 */
            n = rp_cc13xx_encode_empty(mosi, 512, RP_CC13XX_MSG_PING,
                                       m->cmd_seq++);
            m->t_last_ping = now_us;
        }
        if (n) m->stats.txns++;
        break;

    case SPIM_RECOVERY:
    default:
        /* 等待胶水执行 RESET 脉冲（spi_master_reset_done 后回 WAIT_HELLO）。 */
        return 0;
    }

    if (n) pad_frame(mosi, n);
    return n;
}

/* 事件类 seq 链判定（§3.5/§5.5；§6.5 基线重置跳过一次）。 */
static void track_event_seq(spi_master_t *m, uint16_t seq)
{
    if (m->ev_seq_valid) {
        if (rp_cc13xx_seq_gap(m->ev_seq, seq)) m->stats.seq_gaps++;
    }
    m->ev_seq      = seq;
    m->ev_seq_valid = true;
}

/* 合法 HELLO（已过 codec ver 校验）→ 会话建立/重建（§6.2/§6.5）。
 *
 * 状态区分（握手回声语义）：WAIT_HELLO 中收到 = 握手完成 → LINKED +
 * 完整会话建立（seq 基线、RF_CONFIG 待发）。LINKED 中收到 = 握手回声
 * ——延迟应答（§2.2）下 master 在 WAIT_HELLO 发出的每个 HELLO 都会
 * 被应答，最后一个应答在 LINKED 之后到达；slave 从不主动发帧（无
 * 时钟），LINKED 中的 HELLO 不可能是重启宣告（真重启走 PING 静默 →
 * 3 s → RECOVERY 路径，规范 B30），故仅刷新对端版本诊断、不重建会话。 */
static void on_hello(spi_master_t *m, const rp_cc13xx_msg_t *msg)
{
    rp_cc13xx_hello_t h;
    if (rp_cc13xx_decode_hello(msg, &h) != RP_CC13XX_OK) {
        m->stats.len_errors++;
        return;
    }
    m->peer_hello  = h;
    m->hello_tries = 0;
    if (m->state == SPIM_WAIT_HELLO) {
        /* §6.5：基线重置不计 seq_gaps。事件帧取 slave 事件计数器
         * （§3.5），与 HELLO 回显的 master 命令 seq 不同域——"以该帧
         * seq 为新基线"对事件链的操作化 = 跳过下一事件帧的 gap 判定
         * （ev_seq_valid 清零），而非把命令 seq 当事件基线数值使用。 */
        m->ev_seq_valid = false;
        m->rf_pending   = true;      /* §6.4：新会话必须查询 RF 配置 */
        m->stall_cnt    = 0;
        m->t_last_ping  = m->t_now;  /* 1 Hz PING 从会话建立起算 */
        rp_cc13xx_reasm_on_hello(&m->reasm);   /* §6.5：清半交付态 */
        m->state = SPIM_LINKED;
    }
}

void spi_master_digest(spi_master_t *m, uint32_t txn_us, const uint8_t miso[512])
{
    m->t_now += txn_us;

    /* 大对象静态化（审计 WP-E-2 P1 栈越界）：msg ~510 B / chunk
     * ~500 B 曾占本函数栈帧主体。单调用者合同：仅 core0 的
     * spim_poll → digest 路径调用（host 测试单线程同安全）。 */
    static rp_cc13xx_msg_t msg;
    const rp_cc13xx_status_t st =
        rp_cc13xx_decode_frame(miso, 512, &msg);
    bool ev_txn = false;             /* 事件事务（drain 活性证明，§7.2） */

    switch (st) {
    case RP_CC13XX_OK:
        m->stats.legal_frames++;
        m->t_last_legal = m->t_now;
        switch (msg.type) {
        case RP_CC13XX_MSG_HELLO:
            on_hello(m, &msg);
            break;

        case RP_CC13XX_MSG_RX_DESCRIPTOR: {
            rp_cc13xx_rx_desc_t d;
            if (rp_cc13xx_decode_rx_descriptor(&msg, &d) == RP_CC13XX_OK &&
                rp_cc13xx_reasm_start(&m->reasm, &d) == RP_CC13XX_OK) {
                m->desc     = d;     /* rssi/ts_us 随报文携带（§4.3） */
                m->uat_done = false;
                track_event_seq(m, msg.seq);
                ev_txn = true;
            } else {
                m->stats.len_errors++;
            }
            break;
        }

        case RP_CC13XX_MSG_RX_PAYLOAD_CHUNK: {
            static rp_cc13xx_chunk_t c;   /* ~500 B：静态化（栈审计） */
            if (rp_cc13xx_decode_rx_chunk(&msg, &c) == RP_CC13XX_OK &&
                rp_cc13xx_reasm_feed(&m->reasm, &c) == RP_CC13XX_OK) {
                track_event_seq(m, msg.seq);
                ev_txn = true;
                if (!m->uat_done && m->reasm.active &&
                    m->reasm.have == m->reasm.total_len) {
                    /* 报文取毕：UAT 帧一次性交付（552 B 合同见事实卡 §7，
                     * 长度裁别在回调方/胶水，master 不私设上限）。 */
                    m->uat_done = true;
                    m->stats.uat_complete++;
                    if (m->uat_cb) {
                        m->uat_cb(m->reasm.data, m->reasm.total_len,
                                  m->desc.rssi, m->desc.ts_us, m->uat_user);
                    }
                }
            } else {
                m->stats.len_errors++;
            }
            break;
        }

        case RP_CC13XX_MSG_QUEUE_FULL: {
            rp_cc13xx_queue_full_t q;
            if (rp_cc13xx_decode_queue_full(&msg, &q) == RP_CC13XX_OK) {
                track_event_seq(m, msg.seq);
                ev_txn = true;       /* §7.2：进入 drain（IRQ_ACK 流水） */
            } else {
                m->stats.len_errors++;
            }
            break;
        }

        case RP_CC13XX_MSG_ERROR: {
            rp_cc13xx_error_t e;
            if (rp_cc13xx_decode_error(&msg, &e) == RP_CC13XX_OK) {
                if (e.code >= 0x04) {
                    /* 异步 ERROR 是事件（§3.5）：计入事件事务（drain
                     * 活性证明）。其 seq 为 0x0000 哨兵——§3.5 明文
                     * "接收方不得对其做 seq 对账或 gap 判定"（§4.9），
                     * 故不调 track_event_seq、不触碰事件基线。 */
                    ev_txn = true;
                }
                /* 命令性 ERROR（0x01–0x03）：直接应答类，非事件。 */
            } else {
                m->stats.len_errors++;
            }
            break;
        }

        case RP_CC13XX_MSG_RF_CONFIG_STATUS: {
            rp_cc13xx_rf_config_t c;
            if (rp_cc13xx_decode_rf_config(&msg, &c) == RP_CC13XX_OK) {
                m->peer_rf = c;
            } else {
                m->stats.len_errors++;
            }
            break;
        }

        case RP_CC13XX_MSG_PONG:
        case RP_CC13XX_MSG_RESET_STATUS:
        case RP_CC13XX_MSG_UPGRADE_STATUS:
        default:
            /* 直接应答类/未知类型（UNKNOWN 容忍，§5.4）：合法帧、非事件。 */
            break;
        }
        break;

    case RP_CC13XX_NO_FRAME:
        m->stats.no_frame_txns++;
        break;

    case RP_CC13XX_ERR_MAGIC:    m->stats.resyncs++;        break;
    case RP_CC13XX_ERR_CRC:      m->stats.crc_errors++;     break;
    case RP_CC13XX_ERR_LEN:      m->stats.len_errors++;     break;
    case RP_CC13XX_ERR_VERSION:  m->stats.version_mismatch++; break;
    case RP_CC13XX_ERR_SHORT:    m->stats.len_errors++;     break;
    case RP_CC13XX_ERR_ARG:                              break;
    case RP_CC13XX_UNKNOWN_TYPE:
        /* CRC 合法但类型未知：容忍忽略（§5.4），合法帧、非事件。
         * §5.3 计数器集之 unknown_types（审计 round-WP-E-1 P2-3）。 */
        m->stats.unknown_types++;
        m->stats.legal_frames++;
        m->t_last_legal = m->t_now;
        break;
    }

    /* §7.2：事件事务复位停滞串；无事件事务累计（仅在 IRQ 高语境下
     * 有意义——判定点在 next_txn）。 */
    if (ev_txn) {
        m->stats.event_txns++;
        m->stall_cnt   = 0;
        m->t_last_event = m->t_now;
    } else {
        m->stall_cnt++;   /* 回绕安全（u8；阈值 8 远早于 255） */
    }
}

/* ── 目标端胶水（host 测试不编译）─────────────────────────────────── */

#ifndef SPI_MASTER_HOST_TEST

/* 引脚（docs/hardware/pinmap_978.md：SPI1 GPIO10-13、IRQ GPIO14、
 * RESET GPIO18）。时钟速率未冻结（协议 §1——台架实测决定）：
 * 8 MHz 为上电保守起点，WP-E 二期台架校准。 */
#define SPIM_PIN_SCK   10
#define SPIM_PIN_MOSI  11
#define SPIM_PIN_MISO  12
#define SPIM_PIN_CSN   13
#define SPIM_PIN_IRQ   14
#define SPIM_PIN_RESET 18
/* 4 MHz = CC1312R slave 模式硬上限（TI ssi.h：FSSI >= 12×bitrate，
 * 48 MHz/12；审计 round-WP-E-1 P1-2 从 8 MHz 下调）。时钟未冻结
 * （协议 §1），台架验证后可再压。 */
#define SPIM_HZ        4000000

void spim_hw_init(void)
{
    spi_init(spi1, SPIM_HZ);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(SPIM_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SPIM_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SPIM_PIN_MISO, GPIO_FUNC_SPI);
    gpio_init(SPIM_PIN_CSN);
    gpio_put(SPIM_PIN_CSN, 1);
    gpio_set_dir(SPIM_PIN_CSN, GPIO_OUT);
    gpio_init(SPIM_PIN_IRQ);
    gpio_set_dir(SPIM_PIN_IRQ, GPIO_IN);
    gpio_init(SPIM_PIN_RESET);
    gpio_put(SPIM_PIN_RESET, 1);       /* 高 = 不复位（低有效，§1） */
    gpio_set_dir(SPIM_PIN_RESET, GPIO_OUT);

    /* §6.1 上电路径（审计 WP-E-2 P2-1）：RESET_N 低脉冲 ≥1 ms →
     * 回高 → 启动等待 ≥100 ms——此前只在 RECOVERY 才发脉冲，slave
     * 上电初态不受控（可能残留旧会话）。前向声明（定义在下方）。 */
    void spim_hw_reset_pulse(void);
    spim_hw_reset_pulse();
}

void spim_hw_reset_pulse(void)
{
    gpio_put(SPIM_PIN_RESET, 0);
    sleep_ms(2);                        /* ≥1 ms（§1/§6.1） */
    gpio_put(SPIM_PIN_RESET, 1);
    sleep_ms(100);                      /* 启动等待下限（§6.1） */
}

/* 单次 512 B 定长事务（阻塞收发；CSN 由本函数界定）。 */
static void spim_hw_transfer(const uint8_t *mosi, uint8_t *miso)
{
    gpio_put(SPIM_PIN_CSN, 0);
    /* 逐字节同步收发（spi_write_read_blocking 单调用即可全双工）。 */
    spi_write_read_blocking(spi1, mosi, miso, 512);
    gpio_put(SPIM_PIN_CSN, 1);
}

/* 单步轮询（与 host 测试同构的事务循环）。装配进 1090 主循环的集成点
 * 与 UAT 回调 → adsb_link UAT_UPLINK → p4_link 队列的接线属后续任务
 * （本任务只交付状态机与胶水原语，spec §6.7 合同的可执行证明在 host）。 */
/* 事务间隔（协议 R14 修订：master 行为合同）：CSN 高电平 ≥1 ms
 * ——slave 在 CSN↑ 后要完成帧校验（CRC ≈ 506 B）、512 B 装载缓冲
 * 复制与下一 pending 装载（48 MHz 下最坏数百 µs），1 µs 的线级
 * CSN 高不足以保证 MISO 头 8 字节在下一次 FSS↓ 前入 FIFO（审计
 * round-WP-E-1 P1-4 的连续事务竞态）。master 在此节流；线级
 * 硬件保证仍是 ≥1 µs，1 ms 是骨架的软件合同（台架实测后可压缩
 * 并回写协议）。 */
#define SPIM_INTER_TXN_US 1000u

void spim_poll(spi_master_t *m, uint32_t now_us)
{
    /* 大缓冲静态化（审计 WP-E-2 P1 栈越界）：mosi/miso 各 512 B
     * 曾是本函数的栈帧主体（链路最深 3448 B > core0 栈 2048 B）。
     * 单调用者合同：仅 core0 主循环调用本函数。 */
    static uint8_t mosi[512], miso[512];
    static uint32_t last_txn_end_us;

    if (spi_master_reset_requested(m)) {
        spim_hw_reset_pulse();
        spi_master_reset_done(m, now_us);
        last_txn_end_us = now_us;
        return;
    }
    /* 节流基准是**上一事务结束时刻**（含阻塞事务自身耗时 ≈1 ms，
     * 验证轮 P1-B：以入口时刻为基准会被事务耗时吞掉整个窗口）。 */
    if (now_us - last_txn_end_us < SPIM_INTER_TXN_US) return;

    const bool irq = gpio_get(SPIM_PIN_IRQ);
    if (spi_master_next_txn(m, now_us, mosi, irq) == 0) return;
    spim_hw_transfer(mosi, miso);
    /* 实测时基（审计 WP-E-3 P1）：CSN 拉高后取真实结束时刻——
     * 125 MHz 外设时钟下 4 MHz 请求实际得 3.90625 MHz（分频离散），
     * 512 B 事务 ≈1048.6 µs；从入口时间推算（+1024 常量）会把结束
     * 时刻记早 ≥24.6 µs，吞掉 R14 的 1 ms CSN 高电平合同。digest 的
     * 看护时基同样用实测 delta。 */
    const uint32_t end_us = time_us_32();
    spi_master_digest(m, end_us - now_us, miso);
    last_txn_end_us = end_us;
}

#endif /* SPI_MASTER_HOST_TEST */
