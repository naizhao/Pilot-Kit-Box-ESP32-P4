/*
 * pk_aero_span.c — 实现说明见 pk_aero_span.h。
 *
 * 密钥拼装、PSA 用法、pre-unmount 栅栏三处都是**照抄 pk_aero_db.c**
 * （同一份 devkey、同一套 psa_cipher_* 调用、同一种"回调里 take/give
 * 一次锁当栅栏"的做法）。照抄是有意的：这两个模块读的是同一个文件，
 * 任何一处口径不一致都会表现成"解出来是乱码"这种最难查的症状。
 */
#include "pk_aero_span.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "psa/crypto.h"

#include "pk_sdcard.h"

static const char *TAG = "aero_span";

#define SPAN_BIN_PATH   "/sdcard/aero/pk_aero.bin"

/* 解密工作缓冲。8 KB 是"够大到走顺序路径、又小到不值得心疼"的折中：
 * pk_aero_db 实测 64 KB 分块 5.99 MB/s、4 KB 慢 29%，8 KB 已在曲线的平
 * 缓段，而窗口的单次区间读中位数只有几 KB（文档 §2.1：典型窗口 23 KB / 10 格）。
 * 放 PSRAM .bss：只在本模块的 IO 路径里用，不做 DMA（fread 内部自己有
 * 对齐缓冲），内部 RAM 那 65 KB 的"调度器启动前窗口"一个字节都不能碰
 * （见 pk_tile_loader.c 顶部注释）。 */
#define SPAN_SCRATCH_BYTES  (8 * 1024)
EXT_RAM_BSS_ATTR static uint8_t s_scratch[SPAN_SCRATCH_BYTES];

/* 段表上限：v4 是 14 个段，给一点余量。超出的段直接忽略（前向兼容，
 * 同 pk_aero_reader 的 "未知类型跳过"）。 */
#define SPAN_MAX_SECTIONS   20

static SemaphoreHandle_t s_io_lock;
static FILE             *s_fp;              /* 常驻只读句柄 */
static uint32_t          s_payload_off;     /* payload 在文件内的起点 */
static uint32_t          s_payload_len;
static uint16_t          s_version;
static uint8_t           s_enc_algo;
static uint8_t           s_nonce[8];
static char              s_cycle[9];
/* 段表 20 × 32 B = 640 B：进 PSRAM .bss。内部 dram0 那 65 KB 的"调度器启动
 * 前窗口"余量只剩一千多字节（scripts/check_early_heap.py 会直接拦下），
 * 往里加几百字节静态数据就是开机 boot loop——理由见 pk_tile_loader.c 顶部。
 * 它是纯冷结构：只在本模块的 open/查询路径里读写，不做 DMA、不进 ISR。 */
EXT_RAM_BSS_ATTR static pk_aero_section_t s_sec[SPAN_MAX_SECTIONS];
static int               s_n_sec;
static volatile uint32_t s_generation;
static volatile uint64_t s_bytes_read;
static volatile uint32_t s_read_calls;

/* ---- 小端读取（逐字节，绝不做未对齐强转）---- */
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 与 pk_aero_db.c 用同一把密钥。两边都从 pk_aero_key.h 取——
 * 这里原本存着一份逐字节手抄的副本，是典型的"改一处漏一处"隐患。 */
#include "pk_aero_key.h"

static void span_key_assemble(uint8_t out[16])
{
    pk_aero_key_assemble(out);
}

/* ---- 打开 / 关闭 ---- */

static void span_reset_locked(void)
{
    if (s_fp != NULL) { fclose(s_fp); s_fp = NULL; }
    s_payload_off = s_payload_len = 0;
    s_version = 0;
    s_enc_algo = 0;
    s_n_sec = 0;
    s_cycle[0] = '\0';
    memset(s_nonce, 0, sizeof(s_nonce));
    memset(s_sec, 0, sizeof(s_sec));
}

/* 解析明文头 + 段表。失败返回 false（此时 s_fp 已被调用方处理）。 */
static bool span_parse_header_locked(size_t file_len)
{
    uint8_t hdr[PK_AERO_HEADER_SIZE];
    if (fseek(s_fp, 0, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, sizeof(hdr), s_fp) != sizeof(hdr)) return false;
    if (memcmp(hdr, PK_AERO_MAGIC, 6) != 0) return false;

    const uint16_t version = rd_u16(hdr + 6);
    if (version < PK_AERO_VERSION_MIN || version > PK_AERO_VERSION_MAX)
        return false;

    memcpy(s_cycle, hdr + 8, 8);
    s_cycle[8] = '\0';
    for (int i = 7; i >= 0 && (s_cycle[i] == ' ' || s_cycle[i] == '\0'); i--)
        s_cycle[i] = '\0';

    const uint16_t n_sections   = rd_u16(hdr + 16);
    const uint32_t sections_off = rd_u32(hdr + 18);
    s_enc_algo = hdr[22];
    memcpy(s_nonce, hdr + 24, 8);

    if (s_enc_algo != PK_AERO_ENC_NONE && s_enc_algo != PK_AERO_ENC_AES128_CTR)
        return false;

    const uint64_t table_end = (uint64_t)sections_off
                             + (uint64_t)n_sections * PK_AERO_SECTION_SIZE;
    if (table_end > file_len) return false;

    /* payload 起点：段表末尾向上 16 B 对齐（同 pk_aero_payload_off）。 */
    const uint32_t poff = (uint32_t)((table_end + 15u) / 16u * 16u);
    if (poff > file_len) return false;
    s_payload_off = poff;
    s_payload_len = (uint32_t)(file_len - poff);
    s_version     = version;

    /* 段表（明文）。逐条读，越界的段直接判文件坏——窗口机制的所有区间读
     * 都以段的 data_off/data_size 为界，这里不校验后面就是野读。 */
    if (fseek(s_fp, (long)sections_off, SEEK_SET) != 0) return false;
    s_n_sec = 0;
    for (uint16_t i = 0; i < n_sections; i++) {
        uint8_t e[PK_AERO_SECTION_SIZE];
        if (fread(e, 1, sizeof(e), s_fp) != sizeof(e)) return false;
        if (s_n_sec >= SPAN_MAX_SECTIONS) continue;   /* 前向兼容：多的忽略 */
        pk_aero_section_t s;
        s.type         = rd_u16(e);
        s.rec_size     = rd_u16(e + 2);
        s.n            = rd_u32(e + 4);
        s.data_off     = rd_u32(e + 8);
        s.data_size    = rd_u32(e + 12);
        s.index_off    = rd_u32(e + 16);
        s.index_size   = rd_u32(e + 20);
        s.strings_off  = rd_u32(e + 24);
        s.strings_size = (version >= 3) ? rd_u32(e + 28) : 0;
        if ((uint64_t)s.data_off + s.data_size > s_payload_len)     return false;
        if ((uint64_t)s.rec_size * s.n > s.data_size)               return false;
        if ((uint64_t)s.index_off + s.index_size > s_payload_len)   return false;
        if ((uint64_t)s.strings_off + s.strings_size > s_payload_len) return false;
        s_sec[s_n_sec++] = s;
    }
    return s_n_sec > 0;
}

bool pk_aero_span_open(void)
{
    if (s_io_lock == NULL) {
        s_io_lock = xSemaphoreCreateMutex();
        if (s_io_lock == NULL) return false;
    }
    if (s_fp != NULL) return true;          /* 幂等 */
    if (!pk_sdcard_is_mounted()) return false;

    struct stat fst;
    if (stat(SPAN_BIN_PATH, &fst) != 0 || !S_ISREG(fst.st_mode)) return false;
    if ((size_t)fst.st_size < PK_AERO_HEADER_SIZE) return false;

    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    bool ok = false;
    s_fp = fopen(SPAN_BIN_PATH, "rb");      /* SD 只读红线：只 "rb" */
    if (s_fp != NULL) {
        ok = span_parse_header_locked((size_t)fst.st_size);
        if (!ok) span_reset_locked();
    }
    if (ok) {
        s_bytes_read = 0;
        s_read_calls = 0;
        s_generation++;
    }
    xSemaphoreGive(s_io_lock);

    if (ok) {
        ESP_LOGI(TAG, "open %s: v%u cycle=%s payload=%lu B enc=%u sections=%d gen=%lu",
                 SPAN_BIN_PATH, (unsigned)s_version, s_cycle,
                 (unsigned long)s_payload_len, (unsigned)s_enc_algo, s_n_sec,
                 (unsigned long)s_generation);
    }
    return ok;
}

void pk_aero_span_close(void)
{
    if (s_io_lock == NULL) return;
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    span_reset_locked();
    xSemaphoreGive(s_io_lock);
}

bool pk_aero_span_is_open(void)   { return s_fp != NULL; }
uint16_t pk_aero_span_version(void) { return s_version; }
const char *pk_aero_span_cycle(void) { return s_cycle; }
uint32_t pk_aero_span_generation(void) { return s_generation; }
uint64_t pk_aero_span_bytes_read(void) { return s_bytes_read; }
uint32_t pk_aero_span_read_calls(void) { return s_read_calls; }

void pk_aero_span_stats_reset(void)
{
    s_bytes_read = 0;
    s_read_calls = 0;
}

const pk_aero_section_t *pk_aero_span_section(uint16_t type)
{
    for (int i = 0; i < s_n_sec; i++)
        if (s_sec[i].type == type) return &s_sec[i];
    return NULL;
}

/* ---- 区间读 + 随机解密 ---- */

/* 持锁版本。返回 false 时 dst 内容未定义（调用方整体丢弃）。 */
static bool span_read_locked(uint32_t off, uint8_t *dst, uint32_t len)
{
    if (s_fp == NULL) return false;
    if ((uint64_t)off + len > s_payload_len) return false;

    /* CTR 只能从 16 B 块边界起流，所以向下对齐再把多读的前缀丢掉。
     * 多读的字节最多 15 个，相对 KB 级的区间读可以忽略。 */
    const uint32_t abase = off & ~15u;
    const uint32_t skip  = off - abase;
    const uint32_t total = skip + len;

    if (fseek(s_fp, (long)(s_payload_off + abase), SEEK_SET) != 0) return false;

    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    psa_key_id_t key_id = 0;
    bool cipher_on = false;
    bool ok = false;

    if (s_enc_algo == PK_AERO_ENC_AES128_CTR) {
        if (psa_crypto_init() != PSA_SUCCESS) goto out;   /* 幂等 */
        uint8_t iv[16], key[16];
        pk_aero_span_ctr_iv(s_nonce, abase, iv);
        span_key_assemble(key);
        psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attr, 128);
        psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&attr, PSA_ALG_CTR);
        psa_status_t st = psa_import_key(&attr, key, sizeof(key), &key_id);
        memset(key, 0, sizeof(key));      /* 栈上拼好的整串即用即毁 */
        if (st != PSA_SUCCESS) goto out;
        if (psa_cipher_decrypt_setup(&op, key_id, PSA_ALG_CTR) != PSA_SUCCESS)
            goto out;
        if (psa_cipher_set_iv(&op, iv, sizeof(iv)) != PSA_SUCCESS) goto out;
        cipher_on = true;
    }

    for (uint32_t pos = 0; pos < total; ) {
        uint32_t chunk = total - pos;
        if (chunk > SPAN_SCRATCH_BYTES) chunk = SPAN_SCRATCH_BYTES;
        if (fread(s_scratch, 1, chunk, s_fp) != chunk) goto out;
        if (cipher_on) {
            /* 就地解密：CTR 是异或流，输入输出同指针安全（pk_aero_db.c 的
             * 流水线用的是同一种就地写法，真机已验证）。 */
            size_t olen = 0;
            if (psa_cipher_update(&op, s_scratch, chunk,
                                  s_scratch, SPAN_SCRATCH_BYTES,
                                  &olen) != PSA_SUCCESS) goto out;
            if (olen != chunk) goto out;   /* CTR 不缓冲，进多少出多少 */
        }
        /* 本块覆盖 total 内的 [pos, pos+chunk)；目标区间是 [skip, skip+len) */
        uint32_t s0 = (pos > skip) ? pos : skip;
        uint32_t s1 = pos + chunk;
        if (s1 > skip + len) s1 = skip + len;
        if (s1 > s0) memcpy(dst + (s0 - skip), s_scratch + (s0 - pos), s1 - s0);
        pos += chunk;
    }
    s_bytes_read += total;
    s_read_calls++;
    ok = true;

out:
    if (cipher_on) psa_cipher_abort(&op);
    if (key_id != 0) psa_destroy_key(key_id);
    return ok;
}

bool pk_aero_span_read(uint32_t off, void *dst, uint32_t len)
{
    if (len == 0) return true;
    if (dst == NULL || s_io_lock == NULL) return false;
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    bool ok = span_read_locked(off, (uint8_t *)dst, len);
    xSemaphoreGive(s_io_lock);
    if (!ok && !pk_sdcard_is_mounted()) {
        /* 读到一半拔卡：句柄已经没意义了，主动关掉，等重插后 open 一次。 */
        pk_aero_span_close();
    }
    return ok;
}

bool pk_aero_span_index(uint16_t type, pk_aero_index_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    const pk_aero_section_t *s = pk_aero_span_section(type);
    if (s == NULL || s->index_off == 0 || s->index_size < 8) return false;

    /* 索引区自描述头：{u32 n_grid; u32 n_second}（在 payload 里，要解密） */
    uint8_t hdr[8];
    if (!pk_aero_span_read(s->index_off, hdr, sizeof(hdr))) return false;
    out->n_grid     = rd_u32(hdr);
    out->n_second   = rd_u32(hdr + 4);
    out->grid_off   = s->index_off + 8;
    out->second_off = out->grid_off + out->n_grid * 8u;
    /* 越界的索引头 = 文件坏（或解密口径错），宁可整段作废也不去读野偏移 */
    if ((uint64_t)out->grid_off + (uint64_t)out->n_grid * 8u
            > (uint64_t)s->index_off + s->index_size) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return out->n_grid > 0;
}

/* pk_sdcard 卸载前回调：关句柄 + 当栅栏。契约见 pk_sdcard.h——回调返回时
 * 本模块必须无打开的 SD fd、无在途 SD I/O。take 到手即说明没有在途读。 */
static void span_pre_unmount_cb(void)
{
    if (s_io_lock == NULL) return;
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    span_reset_locked();
    xSemaphoreGive(s_io_lock);
}

/* 由 pk_win_init() 调用一次（本模块没有自己的 init，句柄是懒开的）。 */
void pk_aero_span_register_unmount_cb(void)
{
    static bool done;
    if (done) return;
    done = true;
    if (s_io_lock == NULL) s_io_lock = xSemaphoreCreateMutex();
    pk_sdcard_register_pre_unmount_cb(span_pre_unmount_cb);
}
