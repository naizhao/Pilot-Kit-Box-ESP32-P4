/*
 * edge_cap.c — edge_cap.pio 的宿主：PIO RX FIFO --DREQ--> DMA 环写 s_ring，
 * CPU 轮询 edge_cap_drain 取数。
 *
 * DMA 连续搬运采用 RP2040 数据手册 §2.5.2.2 的 ping-pong 双通道互链
 * （s_dma[0] 完成→立即触发 s_dma[1]，反之亦然）。原方案的自链不可行：
 * 手册明文 "A channel can not chain to itself. Setting CHAIN_TO to a
 * channel's own index means no chaining will take place."（CHAIN_TO=自己
 * 即“无链”，一环写完通道就停）。互链的连续性由两条寄存器语义保证：
 *   · TRANS_COUNT：通道每次被触发都把最近写入值重载进活动计数器
 *     （§2.5.1.2），两通道都写 EDGE_CAP_RING_ITEMS，被触发即整环重载；
 *   · READ/WRITE_ADDR：不重编程则沿用当前值作下一轮起点（§2.5.1.1），
 *     配合写环（RING_SEL=write, RING_SIZE=13，仅低 13 位变化、2^13 字节
 *     边界回卷）写完一整环后地址恰好回到 s_ring[0]，两通道交替无缝。
 * 通道交接（硬件即时触发）只隔几个周期；未开 FIFO join 时 RX FIFO
 * 深 4 字，4 字 ≫ 交接窗口，足以兜住间隔样本。
 *
 * 写环用 channel_config_set_ring(&c, true, 13)（pico-sdk dma.h 的正规
 * 写法，等价于 CTRL.RING_SEL/RING_SIZE 字段）；不用手工 al1_write_addr_trig
 * 掩码——那只是改一次起始地址再触发，并不构成硬件回卷。
 */
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "edge_cap.h"
#include "edge_cap.pio.h"
#include "board_pins.h"

static PIO  s_pio = pio0;
static uint s_sm;
static uint s_dma[2];       /* ping-pong 互链的两个通道 */
static bool s_started;
static uint32_t s_ring[EDGE_CAP_RING_ITEMS] __attribute__((aligned(4 * EDGE_CAP_RING_ITEMS)));
static volatile size_t s_read;
static uint32_t s_overruns;

static void ring_channel_setup(uint ch, uint other, bool trigger)
{
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);        /* FIFO 定读 */
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, false));
    channel_config_set_ring(&c, true, 13);               /* 写环 2^13=8KB（s_ring 已 8KB 对齐）*/
    channel_config_set_chain_to(&c, other);              /* 完成→触发对方（手册 §2.5.2.2）*/
    dma_channel_configure(ch, &c,
                          s_ring,
                          &s_pio->rxf[s_sm],
                          EDGE_CAP_RING_ITEMS,            /* 每轮一整环；触发即重载 */
                          trigger);
}

void edge_cap_start(void)
{
    if (s_started) return;

    uint offset = pio_add_program(s_pio, &edgecap_program);
    s_sm = pio_claim_unused_sm(s_pio, true);
    edge_cap_program_init(s_pio, s_sm, offset, PIN_PULSES);

    s_dma[0] = dma_claim_unused_channel(true);
    s_dma[1] = dma_claim_unused_channel(true);
    ring_channel_setup(s_dma[1], s_dma[0], false);       /* 先备好待触发 */
    ring_channel_setup(s_dma[0], s_dma[1], true);        /* 再开 0；整环后自动交给 1 */

    s_read = 0;
    s_overruns = 0;
    s_started = true;
}

/*
 * 环内写头 = ITEMS - 活动通道剩余计数。先采样计数、再查 BUSY：采样窗内
 * 计数只会变小（头只会前移），故 BUSY=1 时算出的头只会偏旧（少报），
 * 绝不会虚报未写数据。两通道都 idle（启动前或交接瞬间）按 0 处理——
 * 交接时写头恰好也回卷到 ring[0]。
 */
static size_t ring_head(void)
{
    for (int i = 0; i < 2; i++) {
        dma_channel_hw_t *ch = dma_channel_hw_addr(s_dma[i]);
        uint32_t remaining = ch->transfer_count;
        if (ch->ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS)
            return (EDGE_CAP_RING_ITEMS - remaining) % EDGE_CAP_RING_ITEMS;
    }
    return 0;
}

size_t edge_cap_drain(uint32_t *out, size_t cap)
{
    if (!s_started) return 0;

    /* PIO 侧丢沿：push noblock 撞满 RX FIFO 会置本 SM 的 RXSTALL（写 1 清除）*/
    uint32_t stall_bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + s_sm);
    if (s_pio->fdebug & stall_bit) {
        s_pio->fdebug = stall_bit;
        s_overruns++;
    }

    size_t head = ring_head();
    size_t avail = (head + EDGE_CAP_RING_ITEMS - s_read) % EDGE_CAP_RING_ITEMS;
    if (avail > EDGE_CAP_RING_ITEMS / 2) {               /* 被写穿：跳到 head 重同步 */
        s_overruns++;
        s_read = head;
        avail = 0;
    }
    size_t n = avail < cap ? avail : cap;
    for (size_t i = 0; i < n; i++)
        out[i] = edgecap_raw_to_ticks(          /* 原值 → 真实间隔 tick（edge_cap.h） */
            s_ring[(s_read + i) % EDGE_CAP_RING_ITEMS]);
    s_read = (s_read + n) % EDGE_CAP_RING_ITEMS;
    return n;
}

uint32_t edge_cap_overruns(void) { return s_overruns; }
