/* cc13_bsl.c — 见 cc13_bsl.h。 */
#include "cc13_bsl.h"
#include "cc13_bsl_proto.h"

#include <stdio.h>

#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include "board_pins.h"

/* ROM bootloader 的固定角色 ↔ 本板网络（推导见头文件）。
 * 注意 TO_DEV / FROM_DEV 是**按 ROM 的角色**命名的，与板上 MOSI/MISO 的名字
 * 正好相反——这正是本模块存在的理由，不要"顺手改回来"。 */
#define BSL_CLK       PIN_SUBG_SCK    /* GPIO10 → DIO10 = SSI0_CLK  */
#define BSL_FSS       PIN_SUBG_CSN    /* GPIO13 → DIO11 = SSI0_FSS  */
#define BSL_TO_DEV    PIN_SUBG_MISO   /* GPIO12 → DIO9  = SSI0_RX   */
#define BSL_FROM_DEV  PIN_SUBG_MOSI   /* GPIO11 → DIO8  = SSI0_TX   */

static unsigned s_half_us = 5;        /* ~100 kHz，远低于 4 MHz 上限 */

/*
 * SPI 模式。TRM §10.2.2.2 白纸黑字写「Motorola format with SPH set to 1 and
 * SPO set to 1」= mode 3，但 **实机不是这样**：
 *
 * PantsForBirds/adsbee（同样是 RP2040 + CC1312 的开源项目，GPLv3——这里只取
 * 事实、不抄代码）在 cc1312.cc 里把 SPI 初始化成 CPOL_0/CPHA_0 = **mode 0**，
 * 而那段「进 bootloader 后切成 CPOL_1/CPHA_1」的代码是**被注释掉的**。也就是
 * 说他们试过手册写的 mode 3，最后跑通的是 mode 0。
 *
 * 手册和实机冲突时以实机为准，但两个都留着当变体试——这正是变体矩阵的用途。
 */
static bool s_mode3 = false;

static void bsl_pins_take(void)
{
    /* 从 SPI 外设手里把四根线要回来：外设的引脚角色是固定的，换不了方向。 */
    gpio_init(BSL_CLK);
    gpio_init(BSL_FSS);
    gpio_init(BSL_TO_DEV);
    gpio_init(BSL_FROM_DEV);

    gpio_put(BSL_CLK, 1);             /* SPO=1：时钟空闲高 */
    gpio_set_dir(BSL_CLK, GPIO_OUT);
    gpio_put(BSL_FSS, 1);             /* 片选空闲高 */
    gpio_set_dir(BSL_FSS, GPIO_OUT);
    gpio_put(BSL_TO_DEV, 0);
    gpio_set_dir(BSL_TO_DEV, GPIO_OUT);

    gpio_set_dir(BSL_FROM_DEV, GPIO_IN);
    gpio_pull_up(BSL_FROM_DEV);

    gpio_init(PIN_SUBG_RESET);
    gpio_put(PIN_SUBG_RESET, 1);
    gpio_set_dir(PIN_SUBG_RESET, GPIO_OUT);
}

/*
 * SPI mode 3（SPO=1 / SPH=1，TRM §10.2.2.2）：时钟空闲高；数据在**下降沿**
 * 换、在**上升沿**采。MSB 先出。
 */
static uint8_t bsl_xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        if (s_mode3) {
            /* mode 3：时钟空闲高，下降沿换数据、上升沿采样 */
            gpio_put(BSL_CLK, 0);
            gpio_put(BSL_TO_DEV, (out >> i) & 1);
            busy_wait_us(s_half_us);
            gpio_put(BSL_CLK, 1);
            in = (uint8_t)((in << 1) | (gpio_get(BSL_FROM_DEV) ? 1u : 0u));
            busy_wait_us(s_half_us);
        } else {
            /* mode 0：时钟空闲低，数据在上升沿之前摆好、上升沿采样 */
            gpio_put(BSL_TO_DEV, (out >> i) & 1);
            busy_wait_us(s_half_us);
            gpio_put(BSL_CLK, 1);
            in = (uint8_t)((in << 1) | (gpio_get(BSL_FROM_DEV) ? 1u : 0u));
            busy_wait_us(s_half_us);
            gpio_put(BSL_CLK, 0);
        }
    }
    return in;
}

/*
 * 复位进 ROM bootloader。
 *
 * sync 高低两种都试：TRM §10.1.2 的 backdoor 要在**复位那一刻**就把
 * BL_PIN_NUMBER 指定的脚摆在 BL_LEVEL 上。本板 SUBG_SYNC = DIO13 ←
 * RP2040 GPIO15，PINMAP 里本来就标着「bootloader trigger」。
 *
 * ⚠ 空片上 backdoor 其实用不了（CCFG 擦除态 BL_PIN_NUMBER=0xFF、
 * BL_ENABLE=0xFF），指望的是「无有效镜像 → 自动进 bootloader」那一条。
 * sync 当变体试只是为了把「摆错电平反而挡住了自动进入」这种可能排掉。
 */
static void bsl_reset_into_rom(bool sync_high)
{
    gpio_put(BSL_FSS, 1);
    gpio_put(BSL_CLK, s_mode3 ? 1 : 0);

    gpio_init(PIN_SUBG_SYNC);
    gpio_put(PIN_SUBG_SYNC, sync_high ? 1 : 0);
    gpio_set_dir(PIN_SUBG_SYNC, GPIO_OUT);

    gpio_put(PIN_SUBG_RESET, 0);
    sleep_ms(5);
    gpio_put(PIN_SUBG_RESET, 1);
    sleep_ms(80);                     /* 等 ROM 起来并采样 backdoor 脚 */
}

/*
 * 发一个 PING 包并把回读的原始字节记下来。
 *
 * 两处 TRM 明写的坑：
 * 1. §10.2.2.2 Note：**第一包的第一个字节期间器件不回数据**——ROM 要先在
 *    SSI0_RX 上收满 1 个字节才会去配置 SSI0_TX 输出脚。所以主机在第一个字节
 *    之后必须插一小段延时，等它配完。
 * 2. §10.2.1：发送方可以一直发 0 直到收到非 0 回应；接收方在准备好 ACK/NAK
 *    之前也可以一直回 0。所以"读到一串 0"是正常的，要继续клок。
 */
static int bsl_ping(uint8_t *log, int cap, bool fss_per_byte, bool first_gap)
{
    const uint8_t pkt[3] = { 0x03, CC13_BSL_CMD_PING, CC13_BSL_CMD_PING };  /* size/校验和/命令 */
    int n = 0;

    if (!fss_per_byte) gpio_put(BSL_FSS, 0);
    for (int i = 0; i < 3; i++) {
        if (fss_per_byte) gpio_put(BSL_FSS, 0);
        uint8_t r = bsl_xfer(pkt[i]);
        if (fss_per_byte) gpio_put(BSL_FSS, 1);
        if (n < cap) log[n++] = r;
        if (i == 0 && first_gap) sleep_us(500);   /* 坑 1 */
    }
    /* 继续空转读 ACK：接收方准备好之前可以一直回 0（坑 2）。 */
    for (int i = 0; i < 12 && n < cap; i++) {
        if (fss_per_byte) gpio_put(BSL_FSS, 0);
        uint8_t r = bsl_xfer(0x00);
        if (fss_per_byte) gpio_put(BSL_FSS, 1);
        log[n++] = r;
    }
    if (!fss_per_byte) gpio_put(BSL_FSS, 1);
    return n;
}

void cc13_bsl_diag(void)
{
    bsl_pins_take();

    printf("cc13-bsl: ROM 串行 bootloader (SSI0) —— TRM SWCU185G §10\n");
    printf("  接线: CLK=GPIO%d FSS=GPIO%d  送数据→GPIO%d(DIO9=SSI0_RX)  "
           "收数据←GPIO%d(DIO8=SSI0_TX)\n",
           BSL_CLK, BSL_FSS, BSL_TO_DEV, BSL_FROM_DEV);

    /* 先量一下"器件输出"那根线上有没有别人——和 cJTAG 那边同一套判据：
     * 我们下拉还读到 1，说明线上有强过内部 50k 的上拉（=接到了东西）。 */
    gpio_set_dir(BSL_FROM_DEV, GPIO_IN);
    gpio_pull_up(BSL_FROM_DEV);   sleep_ms(1); int up = gpio_get(BSL_FROM_DEV);
    gpio_pull_down(BSL_FROM_DEV); sleep_ms(1); int dn = gpio_get(BSL_FROM_DEV);
    gpio_pull_up(BSL_FROM_DEV);
    printf("  线探针 DIO8(器件输出): 上拉=%d 下拉=%d %s\n", up, dn,
           (up && dn) ? "← 线上有外部上拉" : "← 只有我们自己在拉");

    for (int v = 0; v < 16; v++) {
        s_mode3                 = (v & 1) != 0;
        const bool sync_high    = (v & 2) != 0;
        const bool fss_per_byte = (v & 4) != 0;
        const bool first_gap    = (v & 8) != 0;
        s_half_us = 5u;

        bsl_pins_take();
        bsl_reset_into_rom(sync_high);
        uint8_t log[16];
        int n = bsl_ping(log, (int)sizeof log, fss_per_byte, first_gap);

        bool acked = false, any = false;
        for (int i = 0; i < n; i++) {
            if (log[i] == CC13_BSL_ACK) acked = true;
            if (log[i] != 0xFF && log[i] != 0x00) any = true;
        }

        printf("  mode%d sync=%d FSS=%-4s 延时=%d 回读:",
               s_mode3 ? 3 : 0, sync_high,
               fss_per_byte ? "逐字节" : "整包", first_gap);
        for (int i = 0; i < n; i++) printf(" %02X", log[i]);
        printf("%s\n", acked ? "   ← ACK(0xCC)!" : (any ? "   ← 有非平凡回应" : ""));
    }

    /* 收工：把线交还给 SPI master（它会在下一次 spim_poll 里重新配好）。 */
    gpio_put(PIN_SUBG_RESET, 1);
}


/* ══ 包收发 ═══════════════════════════════════════════════════════
 *
 * SSI 是全双工：我们每送一个字节，同时收回一个字节。TRM §10.2.1 说
 * 「Neither device transfers a nonzero byte until it has received a response
 * after transmitting a packet」——所以"读到一串 0"是正常的等待态，必须继续
 * 打时钟，不能当成没人应答。
 *
 * 所有等待都有**明确上界**：本项目没开看门狗，死循环只能靠断电恢复。
 */

/* 空转直到收到一个非零字节。返回该字节，超时返回 -1。 */
static int bsl_wait_nonzero(int max_bytes)
{
    for (int i = 0; i < max_bytes; i++) {
        uint8_t r = bsl_xfer(0x00);
        if (r != 0x00 && r != 0xFF) return r;
    }
    return -1;
}

/* 发一个包，然后等 ACK。first=true 时在首字节后插延时（TRM §10.2.2.2：
 * 器件收到第一个字节后才去配置 SSI0_TX 输出脚，主机得给它这个时间）。 */
static cc13_bsl_result_t bsl_send_pkt(const uint8_t *pkt, size_t len, bool first)
{
    gpio_put(BSL_FSS, 0);
    for (size_t i = 0; i < len; i++) {
        (void)bsl_xfer(pkt[i]);
        if (i == 0 && first) sleep_us(500);
    }
    int r = bsl_wait_nonzero(64);
    gpio_put(BSL_FSS, 1);

    if (r < 0) return CC13_BSL_ERR_LINK;
    if ((uint8_t)r == CC13_BSL_NACK) return CC13_BSL_ERR_PROTOCOL;
    if ((uint8_t)r != CC13_BSL_ACK)  return CC13_BSL_ERR_PROTOCOL;
    return CC13_BSL_OK;
}

/* 收一个器件发来的包（size/checksum/data），校验后回 ACK。
 * 返回 data 长度（= size-2），失败返回负的 cc13_bsl_result_t。 */
static int bsl_recv_pkt(uint8_t *out, size_t cap)
{
    gpio_put(BSL_FSS, 0);
    int size = bsl_wait_nonzero(256);
    if (size < 3 || (size_t)size - 2u > cap) {
        gpio_put(BSL_FSS, 1);
        return -(int)CC13_BSL_ERR_PROTOCOL;
    }
    uint8_t pkt[CC13_BSL_MAX_PACKET];
    pkt[0] = (uint8_t)size;
    for (int i = 1; i < size; i++) pkt[i] = bsl_xfer(0x00);
    /* 收全了才回 ACK——先 ACK 再校验等于承认了还没看过的东西。 */
    bool ok = cc13_bsl_pkt_valid(pkt, (size_t)size);
    if (ok) (void)bsl_xfer(CC13_BSL_ACK);
    else    (void)bsl_xfer(CC13_BSL_NACK);
    gpio_put(BSL_FSS, 1);

    if (!ok) return -(int)CC13_BSL_ERR_PROTOCOL;
    for (int i = 2; i < size; i++) out[i - 2] = pkt[i];
    return size - 2;
}

static cc13_bsl_result_t bsl_cmd(uint8_t cmd, const uint8_t *params,
                                 size_t n, bool first)
{
    uint8_t pkt[CC13_BSL_MAX_PACKET];
    size_t len = cc13_bsl_pkt_build(pkt, sizeof pkt, cmd, params, n);
    if (!len) return CC13_BSL_ERR_SIZE;
    return bsl_send_pkt(pkt, len, first);
}

/* GET_STATUS：返回状态码，链路/协议出错返回 -1。 */
static int bsl_get_status(void)
{
    if (bsl_cmd(CC13_BSL_CMD_GET_STATUS, NULL, 0, false) != CC13_BSL_OK)
        return -1;
    uint8_t data[8];
    int n = bsl_recv_pkt(data, sizeof data);
    if (n != 1) return -1;
    return data[0];
}

/* 发命令并确认它真的成功了。手册反复强调：ACK 只代表"包收到了"，
 * 命令到底成没成功必须再问一次 GET_STATUS（§10.2.3.2/§10.2.3.3）。 */
static cc13_bsl_result_t bsl_cmd_checked(uint8_t cmd, const uint8_t *params,
                                         size_t n, cc13_bsl_result_t on_fail)
{
    cc13_bsl_result_t r = bsl_cmd(cmd, params, n, false);
    if (r != CC13_BSL_OK) return r;
    int st = bsl_get_status();
    if (st < 0) return CC13_BSL_ERR_LINK;
    return (st == (int)CC13_BSL_RET_SUCCESS) ? CC13_BSL_OK : on_fail;
}

/* ══ 流式烧录状态 ═════════════════════════════════════════════════ */

static bool     s_active;
static uint32_t s_total, s_sent;
static uint32_t s_crc;
static uint8_t  s_buf[CC13_BSL_MAX_DATA];
static size_t   s_buf_len;

const char *cc13_bsl_result_str(cc13_bsl_result_t r)
{
    switch (r) {
    case CC13_BSL_OK:           return "OK";
    case CC13_BSL_ERR_SIZE:     return "镜像长度非法";
    case CC13_BSL_ERR_LINK:     return "无应答(没进 bootloader/模式或接线不对)";
    case CC13_BSL_ERR_PROTOCOL: return "包不自洽(长度或校验和)";
    case CC13_BSL_ERR_ERASE:    return "擦除被拒";
    case CC13_BSL_ERR_PROGRAM:  return "写入失败(GET_STATUS 非 SUCCESS)";
    case CC13_BSL_ERR_CRC:      return "CRC32 不匹配(先查多项式假设，别急着重擦)";
    case CC13_BSL_ERR_STATE:    return "调用顺序不对";
    default:                    return "?";
    }
}

bool cc13_bsl_active(void) { return s_active; }

static void bsl_release(void)
{
    s_active = false;
    /* backdoor 脚交还高阻+下拉：留着高电平会让下次复位又进 bootloader。 */
    gpio_set_dir(PIN_SUBG_SYNC, GPIO_IN);
    gpio_pull_down(PIN_SUBG_SYNC);
    gpio_put(PIN_SUBG_RESET, 1);
}

cc13_bsl_result_t cc13_bsl_begin(uint32_t total_len)
{
    if (total_len == 0 || total_len > CC13_FLASH_BYTES) return CC13_BSL_ERR_SIZE;
    if (total_len & 3u) return CC13_BSL_ERR_SIZE;     /* flash 按字编程 */

    bsl_pins_take();
    s_active = true;
    bsl_reset_into_rom(true);            /* backdoor 拉高再复位 */

    /* PING 是唯一一条"只看 ACK"的命令——此时还没有状态可问。 */
    cc13_bsl_result_t r = bsl_cmd(CC13_BSL_CMD_PING, NULL, 0, true);
    if (r != CC13_BSL_OK) { bsl_release(); return r; }

    /* 整片擦除。BANK_ERASE 受 CCFG 的 BANK_ERASE_DIS 管；被拒时不要自作聪明
     * 退回逐扇区擦——那只会把"配置不允许"伪装成"擦了一半"。 */
    r = bsl_cmd_checked(CC13_BSL_CMD_BANK_ERASE, NULL, 0, CC13_BSL_ERR_ERASE);
    if (r != CC13_BSL_OK) { bsl_release(); return r; }

    uint8_t p[8];
    cc13_bsl_put_be32(p + 0, 0x00000000u);      /* flash 从 0 开始（§10.2.3.2）*/
    cc13_bsl_put_be32(p + 4, total_len);
    r = bsl_cmd_checked(CC13_BSL_CMD_DOWNLOAD, p, 8, CC13_BSL_ERR_PROGRAM);
    if (r != CC13_BSL_OK) { bsl_release(); return r; }

    s_total = total_len; s_sent = 0; s_buf_len = 0;
    s_crc = 0xFFFFFFFFu;
    return CC13_BSL_OK;
}

/* 把攒够的一片发出去。 */
static cc13_bsl_result_t bsl_flush(void)
{
    if (s_buf_len == 0) return CC13_BSL_OK;
    cc13_bsl_result_t r = bsl_cmd_checked(CC13_BSL_CMD_SEND_DATA,
                                          s_buf, s_buf_len,
                                          CC13_BSL_ERR_PROGRAM);
    if (r != CC13_BSL_OK) return r;
    s_sent += (uint32_t)s_buf_len;
    s_buf_len = 0;
    return CC13_BSL_OK;
}

cc13_bsl_result_t cc13_bsl_feed(const uint8_t *data, size_t n)
{
    if (!s_active) return CC13_BSL_ERR_STATE;
    if (s_sent + s_buf_len + n > s_total) return CC13_BSL_ERR_SIZE;

    s_crc = cc13_bsl_crc32_update(s_crc, data, n);
    while (n) {
        size_t room = CC13_BSL_MAX_DATA - s_buf_len;
        size_t take = (n < room) ? n : room;
        for (size_t i = 0; i < take; i++) s_buf[s_buf_len + i] = data[i];
        s_buf_len += take; data += take; n -= take;
        if (s_buf_len == CC13_BSL_MAX_DATA) {
            cc13_bsl_result_t r = bsl_flush();
            if (r != CC13_BSL_OK) { bsl_release(); return r; }
        }
    }
    return CC13_BSL_OK;
}

cc13_bsl_result_t cc13_bsl_finish(void)
{
    if (!s_active) return CC13_BSL_ERR_STATE;

    cc13_bsl_result_t r = bsl_flush();
    if (r != CC13_BSL_OK) { bsl_release(); return r; }
    if (s_sent != s_total) { bsl_release(); return CC13_BSL_ERR_SIZE; }

    /* CRC32 校验整段。§10.2.3.8：第三个参数是读重复次数，0 = 只读一遍；
     * 长度必须 > 8，否则器件直接回 0xFFFFFFFF。 */
    uint8_t p[12];
    cc13_bsl_put_be32(p + 0, 0x00000000u);
    cc13_bsl_put_be32(p + 4, s_total);
    cc13_bsl_put_be32(p + 8, 0x00000000u);
    r = bsl_cmd(CC13_BSL_CMD_CRC32, p, 12, false);
    if (r != CC13_BSL_OK) { bsl_release(); return r; }

    uint8_t rx[8];
    int n = bsl_recv_pkt(rx, sizeof rx);
    if (n != 4) { bsl_release(); return CC13_BSL_ERR_PROTOCOL; }
    /* 结果 4 字节 MSB first（§10.2.3.8） */
    const uint32_t dev = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
                         ((uint32_t)rx[2] << 8)  |  (uint32_t)rx[3];
    const uint32_t mine = cc13_bsl_crc32_final(s_crc);
    if (dev != mine) {
        printf("cc13-bsl: CRC32 器件=0x%08lX 本地=0x%08lX\n",
               (unsigned long)dev, (unsigned long)mine);
        bsl_release();
        return CC13_BSL_ERR_CRC;
    }

    /* 复位让新固件跑起来。RESET 之后器件不再应答，不要等 ACK。 */
    (void)bsl_cmd(CC13_BSL_CMD_RESET, NULL, 0, false);
    bsl_release();
    return CC13_BSL_OK;
}

void cc13_bsl_abort(void)
{
    if (!s_active) return;
    bsl_release();
}

/* ══ CDC 流式接口 ═════════════════════════════════════════════════
 * 与 cjtag 那条通道同款约定（docs/firmware_update.md）：先 4 字节小端长度，
 * 再是镜像本体。不用终止符——352 KB 镜像里必然出现任何单字节值，拿某个字节
 * 当结束符一定会被截断。
 */

static bool     s_cdc_on;
static uint8_t  s_hdr[4];
static uint8_t  s_hdr_len;
static bool     s_hdr_done;
static bool     s_failed;
static uint32_t s_recv;
/* 主机声明的镜像长度。**失败之后也要留着**：用来把剩下的镜像字节吞完。
 * s_total 是烧录状态机的，begin() 失败时根本没被赋值，不能拿它当吞字节的
 * 计数——那正是下面这个坑的来源。 */
static uint32_t s_cdc_expect;

void cc13_bsl_cdc_start(void)
{
    s_cdc_on = true; s_hdr_len = 0; s_hdr_done = false;
    s_failed = false; s_recv = 0; s_cdc_expect = 0;
    printf("BSL-MODE: 送 4 字节小端长度，再送镜像（最大 %u 字节）\n",
           (unsigned)CC13_FLASH_BYTES);
}

bool cc13_bsl_cdc_busy(void) { return s_cdc_on; }

static void cdc_end(cc13_bsl_result_t r)
{
    s_cdc_on = false;
    if (r == CC13_BSL_OK) printf("BSL-DONE %lu 字节\n", (unsigned long)s_total);
    else printf("BSL-FAIL: %s\n", cc13_bsl_result_str(r));
}

void cc13_bsl_cdc_byte(uint8_t b)
{
    if (!s_cdc_on) return;

    if (!s_hdr_done) {
        s_hdr[s_hdr_len++] = b;
        if (s_hdr_len < 4) return;
        uint32_t len = (uint32_t)s_hdr[0] | ((uint32_t)s_hdr[1] << 8) |
                       ((uint32_t)s_hdr[2] << 16) | ((uint32_t)s_hdr[3] << 24);
        s_hdr_done = true;
        s_cdc_expect = len;
        cc13_bsl_result_t r = cc13_bsl_begin(len);
        if (r != CC13_BSL_OK) {
            /* ⚠ 这里**不能**直接退出烧录模式。主机已经在往下灌镜像了，
             * 一旦回到命令解析，镜像里的字节就会被当命令执行——352 KB 里
             * 必然出现 'B'，那会让 RP2040 进 BOOTSEL（仓库里记过的坑）。
             * 所以转入"吞字节"态，收满声明长度再退出。 */
            printf("BSL-FAIL: %s\n", cc13_bsl_result_str(r));
            s_failed = true;
            if (s_cdc_expect == 0) { s_cdc_on = false; printf("BSL-ABORT\n"); }
            return;
        }
        printf("BSL-RECV %lu 字节...\n", (unsigned long)len);
        return;
    }

    s_recv++;
    if (s_failed) {
        /* 已失败：继续吞掉剩余字节，别让镜像内容被当成命令解析
         * （镜像里必然有 'B'，那会让 RP2040 进 BOOTSEL）。
         * 计数用 s_cdc_expect 而不是 s_total——begin() 失败时后者没被赋值。 */
        if (s_recv >= s_cdc_expect) { s_cdc_on = false; printf("BSL-ABORT\n"); }
        return;
    }

    cc13_bsl_result_t r = cc13_bsl_feed(&b, 1);
    if (r != CC13_BSL_OK) {
        printf("BSL-FAIL @%lu: %s\n", (unsigned long)s_recv,
               cc13_bsl_result_str(r));
        s_failed = true;
        return;
    }
    if (s_recv >= s_cdc_expect) cdc_end(cc13_bsl_finish());
}
