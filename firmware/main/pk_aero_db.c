/*
 * pk_aero_db.c — SD 卡航空数据库懒加载状态机（ABSENT/LOADING/READY/ERROR）。
 *
 * 加载链路（照抄 tmp/pk_aero_bench/p4_bench/bench_main.c 验证过的路径，
 * 差别只在"分块流水线"）：
 *   stat → heap_caps_malloc(SPIRAM) → 64 KB 分块 fread + PSA 流式
 *   AES-128-CTR 就地解密（读一块解一块，不等整读完；块间 vTaskDelay(1)
 *   让渡）→ SHA-256 分块校验 → pk_aero_init（校验 magic/version==2/段表）
 *   → 发布 READY。任何失败释放缓冲，按卡还在不在分流 ERROR / ABSENT。
 *
 * SD 驱动完全复用 pk_sdcard.c（唯一一套挂载/探活/热插拔状态机）：本模块
 * 只看 pk_sdcard_is_mounted()，自己绝不碰 sdmmc。挂载重试（IDF #10531
 * 首挂偶发超时 + 拆 LDO 断电冷启）已由 pk_sdcard 的 3 s 探测任务无限重试
 * 覆盖，这里不再重复实现——本模块的重试是"文件层"的：加载失败/无文件时
 * 周期性再试。
 *
 * 并发方案（照 aircraft_state.c 的单 mutex 风格，选了任务书里"查询期间
 * 持锁"这条最简单的路）：
 *   - s_lock 串行化「查询」与「发布/卸载」。查询 wrapper 全程持锁，卸载
 *     也要先拿锁再 free——查询进行中缓冲绝不会被释放。
 *   - 不用引用计数：查询最长 ~16 ms（nearest 东京最坏），拔卡路径多等
 *     这点时间毫无影响；计数方案要多一个"等归零"的握手，换不来收益。
 *   - s_io_lock 单独罩住 fopen→fread→fclose 的 I/O 会话（与 s_lock 无嵌套，
 *     锁序天然无环）：pk_sdcard 卸载序列的 pre-unmount 回调拿它当栅栏，
 *     保证 esp_vfs_fat_sdcard_unmount() 跑的时候本模块没有打开的 SD fd
 *     ——IDF 的 unmount 会无条件 free 含 FIL 数组的 fat_ctx，开着文件卸载
 *     就是 use-after-free（2026-08-01 热插拔回归的根因之一）。栅栏等待是
 *     毫秒级：状态先翻 NO_CARD，加载在一个 64 KB 块内就会自行中止。
 *   - 诊断快照（状态/周期/计数）另存无锁字段，渲染路径读它不进锁，
 *     免得被后台 nearest 的持锁窗口卡一帧。
 *   - reader 的查询会写 db->stats 计数器——也被同一把锁串行化，无竞态。
 */

#include "pk_aero_db.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "psa/crypto.h"   /* IDF v6 mbedtls 4.x：传统 aes.h/sha256.h 已从
                           * 公共头移除，AES/SHA 只能走 PSA API（硬件加速
                           * 均生效，p4_bench 实测解密 189 ms / SHA 139 ms） */

#include "pk_sdcard.h"

static const char *TAG = "pk_aero";

#define AERO_BIN_PATH        "/sdcard/aero/pk_aero.bin"

/* 分块尺寸：64 KB 是 p4_bench 实测的甜点（5.99 MB/s，4 KB 慢 29%）。
 * SHA 复核块放大到 512 KB——PSRAM 内存读不经 SD，块大只为少让渡几次。 */
#define AERO_READ_CHUNK      (64 * 1024)
#define AERO_SHA_CHUNK       (512 * 1024)

/* 无文件/加载失败后的重试间隔。与 pk_sdcard 的探测周期同数量级：卡插入
 * 后最迟一个周期就跟进，又不至于在"永远没这张卡"的机器上空转。 */
#define AERO_RETRY_MS        3000
/* READY 后监视拔卡的周期（与 pk_sdcard 探活同款 2 s）。 */
#define AERO_WATCH_MS        2000
/* 开机首次尝试前的静默期：让 splash→PFD 渲染、file sink、tile loader
 * 都先起来（main.c 里 PFD 最迟 ~3 s 起）。加载本来就是懒的，不差这几秒。 */
#define AERO_STARTUP_DELAY_MS 5000

/* 任务参数。优先级 2 = 与 sd_detect 同级，低于 rec_file/tile_loader(3)、
 * pfd/dsp(4)、sdr(6)——加载/解密/SHA 全程可被渲染与写日志抢占。核 0 与
 * 其余 SD 消费方（sd_detect/rec_file/tile_loader）同侧。栈 16 KB：照
 * p4_bench 同款 16 KB 跑完全套加载+查询的余量记录留的（nearest 栈上
 * 工作区 ~1.2 KB + PSA + 浮点日志）。 */
#define AERO_TASK_STACK      (16 * 1024)
#define AERO_TASK_PRIO       2

/* READY 后跑一次的真机自检（学 aircraft_db 的 init 校验风格：只打日志、
 * 不影响状态）。验收稳定后可关，不碍事就留着——它是"数据源对不对"的
 * 第一手证据。 */
#define PK_AERO_DB_SMOKE     1

/* ---- 状态（发布/卸载都在 s_lock 内完成） ------------------------------ */

static SemaphoreHandle_t s_lock;
/* I/O 会话锁：只罩 fopen→分块 fread→fclose 区间，和 s_lock 从不嵌套持有。
 * 唯二使用者：aero_task 的加载路径 + pk_sdcard 的 pre-unmount 回调（纯栅栏）。 */
static SemaphoreHandle_t s_io_lock;
static volatile pk_aero_db_state_t s_state = PK_AERO_DB_ABSENT;
static uint8_t      *s_buf;        /* PSRAM 整文件缓冲（含明文 header） */
static pk_aero_db_t  s_db;         /* reader 解析态（指进 s_buf） */

/* 诊断快照：只在发布/卸载瞬间写，诊断页无锁读。 */
static char              s_cycle[9];
static volatile uint32_t s_n_airports, s_n_navaids, s_n_fixes;
static volatile uint8_t  s_load_pct;
static const char *volatile s_err;   /* ERROR 原因（静态串） */
/* 进 ERROR 时的 SD 挂载代数；与当前代数不等即说明卡被重插过，该重试了。 */
static volatile uint32_t s_err_generation;

/* ---- dev key -----------------------------------------------------------
 * AES-128 开发密钥（devkey，与导出脚本 --key-env PK_AERO_KEY 一致，
 * 非量产密钥）。拆成两半异或存放，不让整串出现在 .rodata 喂 `strings`；
 * 这只是防轻易提取的混淆——真正的保护是量产前打开 P4 flash encryption +
 * secure boot（届时镜像不可读，密钥随之受保护），见设计文档
 * 「加密的诚实边界」。 */
static void aero_key_assemble(uint8_t out[16])
{
    static const uint8_t a[16] = {
        0x22, 0x8B, 0x75, 0x90, 0xBA, 0x42, 0x72, 0x79,
        0xAA, 0x97, 0x51, 0x3E, 0x42, 0xAC, 0xFB, 0x5D,
    };
    static const uint8_t b[16] = {
        0x5A, 0xC3, 0x3C, 0xA5, 0x96, 0x69, 0xF0, 0x0F,
        0x1E, 0xE1, 0x2D, 0xD2, 0x4B, 0xB4, 0x87, 0x78,
    };
    for (int i = 0; i < 16; i++) out[i] = a[i] ^ b[i];
}

/* ---- 加载 -------------------------------------------------------------- */

/* 读入 + 流式解密。成功返回 true；失败把原因写进 *why（静态串）。
 * 全程不持 s_lock——缓冲尚未发布，没人看得见。调用方（aero_load_once）
 * 持着 s_io_lock：拔卡时 pk_sdcard 的卸载序列以这把锁为栅栏等本函数退出
 * （状态已先翻 NO_CARD，下面的分块检查会让我们在一个 64 KB 块内中止）。 */
static bool aero_read_decrypt(FILE *f, uint8_t *buf, size_t len,
                              const char **why)
{
    psa_status_t st = psa_crypto_init();   /* 幂等（BLE 侧可能已 init 过） */
    if (st != PSA_SUCCESS) { *why = "psa init"; return false; }

    psa_key_id_t key_id = 0;
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    bool cipher_on = false;      /* enc_algo==1 且已 setup */
    uint32_t poff = 0;           /* payload 起点；首块读完后才知道 */
    size_t in_pos = 0;           /* 已喂给解密器的输入游标（文件内偏移） */
    size_t out_pos = 0;          /* 解密输出游标（CTR 流式，恒 ≤ in_pos，
                                  * 就地前向写不会踩到未读输入） */
    size_t off = 0;
    bool ok = false;

    while (off < len) {
        size_t want = len - off;
        if (want > AERO_READ_CHUNK) want = AERO_READ_CHUNK;
        if (fread(buf + off, 1, want, f) != want) {
            /* 短读多半是读到一半卡被拔了，也可能是文件被截断 */
            *why = "short read";
            goto out;
        }
        off += want;

        if (poff == 0) {
            /* 首块（64 KB ≫ header 64 B + 段表）：定位 payload 并起解密器 */
            poff = pk_aero_payload_off(buf, off);
            if (poff == 0 || poff > len) { *why = "bad header"; goto out; }
            uint8_t enc = buf[22];               /* header 偏移 22 = enc_algo */
            if (enc == PK_AERO_ENC_AES128_CTR) {
                uint8_t iv[16], key[16];
                memcpy(iv, buf + 24, 8);         /* header 偏移 24 = nonce */
                memset(iv + 8, 0, 8);            /* 大端块计数从 0 起 */
                aero_key_assemble(key);
                psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
                psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
                psa_set_key_bits(&attr, 128);
                psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
                psa_set_key_algorithm(&attr, PSA_ALG_CTR);
                st = psa_import_key(&attr, key, sizeof(key), &key_id);
                memset(key, 0, sizeof(key));     /* 栈上拼好的整串即用即毁 */
                if (st != PSA_SUCCESS) { *why = "psa key"; goto out; }
                st = psa_cipher_decrypt_setup(&op, key_id, PSA_ALG_CTR);
                if (st == PSA_SUCCESS) st = psa_cipher_set_iv(&op, iv, 16);
                if (st != PSA_SUCCESS) { *why = "psa setup"; goto out; }
                cipher_on = true;
            } else if (enc != PK_AERO_ENC_NONE) {
                *why = "enc algo";
                goto out;
            }
            in_pos = out_pos = poff;
        }

        /* 流水线：这一块落地就解掉（poff 之前的明文头跳过） */
        if (cipher_on && off > in_pos) {
            size_t olen = 0;
            st = psa_cipher_update(&op, buf + in_pos, off - in_pos,
                                   buf + out_pos, len - out_pos, &olen);
            if (st != PSA_SUCCESS) { *why = "psa update"; goto out; }
            in_pos = off;
            out_pos += olen;
        }

        /* 让渡 + 快速拔卡止损：不等整文件读完才发现卡没了 */
        s_load_pct = (uint8_t)((uint64_t)off * 100 / len);
        vTaskDelay(1);
        if (!pk_sdcard_is_mounted()) { *why = "card removed"; goto out; }
    }

    if (cipher_on) {
        size_t olen = 0;
        st = psa_cipher_finish(&op, buf + out_pos, len - out_pos, &olen);
        if (st != PSA_SUCCESS) { *why = "psa finish"; goto out; }
        out_pos += olen;
        if (out_pos != len) { *why = "decrypt len"; goto out; }
    }
    ok = true;

out:
    if (!ok && cipher_on) psa_cipher_abort(&op);
    if (key_id != 0) psa_destroy_key(key_id);
    return ok;
}

/* SHA-256 分块校验（硬件 SHA；块间让渡）。 */
static bool aero_sha_verify(const pk_aero_db_t *db)
{
    psa_hash_operation_t h = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&h, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
    uint32_t plen = 0;
    const uint8_t *p = pk_aero_payload(db, &plen);
    for (uint32_t at = 0; at < plen; ) {
        uint32_t n = plen - at;
        if (n > AERO_SHA_CHUNK) n = AERO_SHA_CHUNK;
        if (psa_hash_update(&h, p + at, n) != PSA_SUCCESS) {
            psa_hash_abort(&h);
            return false;
        }
        at += n;
        vTaskDelay(1);
    }
    uint8_t digest[32];
    size_t dlen = 0;
    if (psa_hash_finish(&h, digest, sizeof(digest), &dlen) != PSA_SUCCESS)
        return false;
    return dlen == 32 && memcmp(digest, db->sha256, 32) == 0;
}

/* 单次加载尝试。返回 true = 已发布 READY。
 * 返回 false 时状态已按"卡还在不在"落到 ERROR（原因存 s_err，等拔卡
 * 重插再试——同一份坏文件每 3 s 重读一遍毫无意义）或 ABSENT（拔卡/
 * 无文件，探测周期自动跟进）。 */
static bool aero_load_once(void)
{
    struct stat fst;
    if (stat(AERO_BIN_PATH, &fst) != 0 || !S_ISREG(fst.st_mode)) {
        return false;   /* 没有文件：保持 ABSENT，周期性重试 */
    }
    size_t len = (size_t)fst.st_size;
    if (len < PK_AERO_HEADER_SIZE) {
        s_state = PK_AERO_DB_ERROR;
        s_err   = "file too small";
        ESP_LOGE(TAG, "%s: %u B — too small", AERO_BIN_PATH, (unsigned)len);
        return false;
    }

    s_load_pct = 0;
    s_state = PK_AERO_DB_LOADING;
    ESP_LOGI(TAG, "loading %s (%.2f MB) in background",
             AERO_BIN_PATH, len / 1048576.0);
    int64_t t0 = esp_timer_get_time();

    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        s_state = PK_AERO_DB_ERROR;
        s_err   = "psram alloc";
        ESP_LOGE(TAG, "PSRAM alloc %u B failed", (unsigned)len);
        return false;
    }

    const char *why = "?";
    /* I/O 会话整段持 s_io_lock（fopen→分块 fread→fclose）：卸载序列的
     * pre-unmount 回调 take 这把锁即等到"文件已关、无在途读"才放行
     * unmount。fclose 后立刻释放——后面的解析/SHA 全在 PSRAM，不碰 SD。 */
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    FILE *f = fopen(AERO_BIN_PATH, "rb");   /* SD 只读红线：只 "rb" */
    bool ok = false;
    if (f == NULL) {
        why = "fopen";
    } else {
        ok = aero_read_decrypt(f, buf, len, &why);
        fclose(f);
    }
    xSemaphoreGive(s_io_lock);

    pk_aero_db_t db;
    if (ok) {
        int rc = pk_aero_init(&db, buf, len, true);   /* magic/version==2/段表 */
        if (rc != PK_AERO_OK) {
            ok = false;
            why = rc == PK_AERO_ERR_MAGIC   ? "bad magic"
                : rc == PK_AERO_ERR_VERSION ? "bad version"
                                            : "bad layout";
        }
    }
    if (ok && !aero_sha_verify(&db)) {
        ok = false;
        why = "sha256 mismatch";
    }

    if (!ok) {
        free(buf);
        if (!pk_sdcard_is_mounted()) {
            /* 半路拔卡不算文件坏：回 ABSENT，重插自动重来 */
            s_state = PK_AERO_DB_ABSENT;
            ESP_LOGW(TAG, "load aborted (%s) — card gone, back to ABSENT",
                     why);
        } else {
            /* 记下进 ERROR 时的挂载代数：退出条件靠它判「卡被重插过」，
             * 而不是去采样 is_mounted 的电平（见下面 ERROR 分支）。 */
            s_err_generation = pk_sdcard_mount_generation();
            s_state = PK_AERO_DB_ERROR;
            s_err   = why;
            ESP_LOGE(TAG, "load failed: %s (re-insert card to retry)", why);
        }
        return false;
    }

    /* 发布：持锁换指针，查询方要么看到旧态（未 READY 返回空），要么看到
     * 完整的新库，没有中间态。 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_buf = buf;
    s_db  = db;
    memcpy(s_cycle, db.cycle, sizeof(s_cycle));
    s_n_airports = db.sec_airport.n;
    s_n_navaids  = db.sec_navaid.n;
    s_n_fixes    = db.sec_fix.n;
    s_err        = NULL;
    s_load_pct   = 100;
    s_state      = PK_AERO_DB_READY;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "aero DB ready (cycle %s): %lu airports, %lu rwy-dirs, "
                  "%lu freqs, %lu navaids, %lu fixes — %.2f s, "
                  "SPIRAM free %u B",
             s_cycle,
             (unsigned long)db.sec_airport.n, (unsigned long)db.sec_rwy.n,
             (unsigned long)db.sec_freq.n, (unsigned long)db.sec_navaid.n,
             (unsigned long)db.sec_fix.n,
             (esp_timer_get_time() - t0) / 1e6,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

/* 卸载（拔卡触发）。持锁 free：正在进行的查询会先跑完再轮到这里。 */
static void aero_unload(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_buf);
    s_buf = NULL;
    memset(&s_db, 0, sizeof(s_db));
    s_cycle[0]   = '\0';
    s_n_airports = s_n_navaids = s_n_fixes = 0;
    s_err        = NULL;
    s_state      = PK_AERO_DB_ABSENT;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "card removed — aero DB unloaded (auto-reload on insert)");
}

#if PK_AERO_DB_SMOKE
/* READY 后跑一次：拿一个已知机场核对聚簇计数并打进日志。全部走公开
 * wrapper（顺带验证锁路径），只报告不改状态——数据抽查的真值随 cycle
 * 变化，硬断言只会把"换了数据"误报成"坏了"。2026-02 全量库真值：
 * ZGGG 跑道方向 10 / 频率 63。 */
static void aero_smoke_check(void)
{
    int64_t t0 = esp_timer_get_time();
    int32_t idx = pk_aero_db_airport_by_icao("ZGGG");
    int64_t dt = esp_timer_get_time() - t0;
    pk_aero_airport_t a;
    if (idx < 0 || !pk_aero_db_airport_get((uint32_t)idx, &a)) {
        ESP_LOGW(TAG, "smoke: ZGGG not found (regional data set?)");
        return;
    }
    uint32_t rf = 0, rn = 0, ff = 0, fn = 0;
    bool rok = pk_aero_db_airport_runways((uint32_t)idx, &rf, &rn);
    bool fok = pk_aero_db_airport_freqs((uint32_t)idx, &ff, &fn);
    ESP_LOGI(TAG, "smoke: ZGGG idx=%ld (%.4f,%.4f) \"%s\" rwy=%lu freq=%lu "
                  "(2026-02 truth 10/63) icao lookup %lld us [%s]",
             (long)idx, a.lat, a.lon, a.name,
             (unsigned long)rn, (unsigned long)fn, (long long)dt,
             (rok && fok && rn == 10 && fn == 63) ? "PASS" : "CHECK");
    /* 第一条跑道 + 第一条频率各解一条，覆盖聚簇段的解码路径 */
    pk_aero_rwy_dir_t r;
    pk_aero_freq_t fq;
    if (rn > 0 && pk_aero_db_rwy_dir_get(rf, &r))
        ESP_LOGI(TAG, "smoke: ZGGG rwy[0]=%s len=%u ft", r.designator,
                 (unsigned)r.length_ft);
    if (fn > 0 && pk_aero_db_freq_get(ff, &fq))
        ESP_LOGI(TAG, "smoke: ZGGG freq[0]=%lu kHz \"%s\"",
                 (unsigned long)fq.freq_khz, fq.callsign);
}
#endif /* PK_AERO_DB_SMOKE */

/* ---- 后台任务 ----------------------------------------------------------
 * 与 sd_detect_task 同构的两态轮询：未就绪 → 试加载；READY/ERROR →
 * 盯着拔卡。状态迁移只靠轮询，周期与 pk_sdcard 对齐；pre-unmount 回调
 * （sd_io_barrier_cb）只是卸载时序的 I/O 栅栏，不驱动状态机。 */
static void aero_task(void *arg)
{
    (void)arg;
    /* 静默期：等 UI/PFD 起来再开始占 SD 带宽（懒加载定案：开机不加载） */
    vTaskDelay(pdMS_TO_TICKS(AERO_STARTUP_DELAY_MS));

    while (1) {
        switch (s_state) {
        case PK_AERO_DB_ABSENT:
            if (pk_sdcard_is_mounted()) {
                if (aero_load_once()) {
#if PK_AERO_DB_SMOKE
                    aero_smoke_check();
#endif
                    continue;   /* 直接进 READY 分支的监视节奏 */
                }
            }
            vTaskDelay(pdMS_TO_TICKS(AERO_RETRY_MS));
            break;

        case PK_AERO_DB_READY:
            vTaskDelay(pdMS_TO_TICKS(AERO_WATCH_MS));
            if (!pk_sdcard_is_mounted()) aero_unload();
            break;

        case PK_AERO_DB_ERROR:
            /*
             * 坏文件不反复重读；卡被重插过（= 用户来处理了）才回 ABSENT。
             *
             * 判据是挂载代数变了，不是采样到 !is_mounted。原先那样写会漏：
             * 2026-08-01 实测连续插拔，卡离开的窗口只有 3.5 s，而本分支的
             * 两个检查点一个还在 delay 里、一个落在卡已经回来之后，整段窗口
             * 被跳过，于是 aero 一直卡在 ERROR，要等下一次拔卡才恢复——正是
             * 「频繁插拔就不读 aero」的成因。
             *
             * 还有一层：short read 本来多半就是「读到一半被拔卡」，但 pk_sd
             * 的探活有 2 s 周期，比 aero 自己撞上读失败晚了几百 ms，于是上面
             * 那个 if 看到的 is_mounted 还是 true，把拔卡误判成文件损坏进了
             * 这里。代数比较对这种误判同样有效：卡一旦重挂就重试。
             */
            vTaskDelay(pdMS_TO_TICKS(AERO_WATCH_MS));
            if (!pk_sdcard_is_mounted() ||
                pk_sdcard_mount_generation() != s_err_generation) {
                s_state = PK_AERO_DB_ABSENT;
                s_err   = NULL;
            }
            break;

        default:   /* LOADING 只存在于 aero_load_once() 内部 */
            vTaskDelay(pdMS_TO_TICKS(AERO_RETRY_MS));
            break;
        }
    }
}

/* ---- 公共 API ----------------------------------------------------------- */

/* pk_sdcard 卸载前回调：纯栅栏。take 到手即说明加载路径的 fopen→fclose
 * 会话已退出（或根本没在跑）——状态已被卸载序列先翻成 NO_CARD，
 * aero_read_decrypt 的分块检查会在一个 64 KB 块内中止并 fclose，等待是
 * 毫秒级。本模块没有常开句柄，无需真正"关"什么。 */
static void sd_io_barrier_cb(void)
{
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    xSemaphoreGive(s_io_lock);
}

void pk_aero_db_init(void)
{
    if (s_lock != NULL) return;   /* 幂等 */
    s_lock    = xSemaphoreCreateMutex();
    s_io_lock = xSemaphoreCreateMutex();
    pk_sdcard_register_pre_unmount_cb(sd_io_barrier_cb);

    /* 只创建任务，不做任何 IO——开机路径零阻塞。 */
    BaseType_t ok = xTaskCreatePinnedToCore(aero_task, "aero_db",
                                            AERO_TASK_STACK, NULL,
                                            AERO_TASK_PRIO, NULL, 0);
    if (ok != pdTRUE) ESP_LOGE(TAG, "aero_db task create failed");
}

pk_aero_db_state_t pk_aero_db_state(void)
{
    return s_state;
}

const char *pk_aero_db_cycle(void)
{
    return s_cycle;   /* 未 READY 时为 ""（卸载时清空） */
}

void pk_aero_db_status_get(pk_aero_db_status_t *out)
{
    /* 无锁快照：字段只在发布/卸载瞬间变化，诊断页每帧读也不会被后台
     * nearest 的持锁窗口（最坏 ~16 ms）卡住。极端时序下读到卸载前后的
     * 混合值，也只是诊断页闪一帧旧数字，无解引用风险。 */
    out->state      = s_state;
    memcpy(out->cycle, s_cycle, sizeof(out->cycle));
    out->cycle[sizeof(out->cycle) - 1] = '\0';
    out->n_airports = s_n_airports;
    out->n_navaids  = s_n_navaids;
    out->n_fixes    = s_n_fixes;
    out->load_pct   = s_load_pct;
    out->err        = s_err;
}

/* 查询 wrapper 的公共骨架：先无锁看一眼状态（未就绪路径零开销），进锁后
 * 再核对一次——两次检查之间可能刚好拔了卡。 */
#define AERO_QUERY(not_ready_val, expr)                    \
    do {                                                   \
        if (s_state != PK_AERO_DB_READY) return (not_ready_val); \
        xSemaphoreTake(s_lock, portMAX_DELAY);             \
        __typeof__(not_ready_val) ret_ =                   \
            (s_state == PK_AERO_DB_READY) ? (expr) : (not_ready_val); \
        xSemaphoreGive(s_lock);                            \
        return ret_;                                       \
    } while (0)

int32_t pk_aero_db_airport_by_icao(const char *code)
{
    AERO_QUERY((int32_t)-1, pk_aero_airport_by_icao(&s_db, code));
}

bool pk_aero_db_airport_get(uint32_t idx, pk_aero_airport_t *out)
{
    AERO_QUERY(false, pk_aero_airport_get(&s_db, idx, out));
}

bool pk_aero_db_rwy_dir_get(uint32_t idx, pk_aero_rwy_dir_t *out)
{
    AERO_QUERY(false, pk_aero_rwy_dir_get(&s_db, idx, out));
}

bool pk_aero_db_freq_get(uint32_t idx, pk_aero_freq_t *out)
{
    AERO_QUERY(false, pk_aero_freq_get(&s_db, idx, out));
}

bool pk_aero_db_navaid_get(uint32_t idx, pk_aero_navaid_t *out)
{
    AERO_QUERY(false, pk_aero_navaid_get(&s_db, idx, out));
}

bool pk_aero_db_fix_get(uint32_t idx, pk_aero_fix_t *out)
{
    AERO_QUERY(false, pk_aero_fix_get(&s_db, idx, out));
}

bool pk_aero_db_airport_runways(uint32_t apt_idx,
                                uint32_t *first, uint32_t *count)
{
    AERO_QUERY(false, pk_aero_airport_runways(&s_db, apt_idx, first, count));
}

bool pk_aero_db_airport_freqs(uint32_t apt_idx,
                              uint32_t *first, uint32_t *count)
{
    AERO_QUERY(false, pk_aero_airport_freqs(&s_db, apt_idx, first, count));
}

/* nearest 固定 approx：P4 无 double FPU，exact 东京最坏 avg 46 ms 会卡帧
 * （p4_bench 定论），不给调用方选错的机会。 */
int pk_aero_db_nearest_airports(double lat, double lon,
                                pk_aero_near_t *out, int max)
{
    AERO_QUERY(0, pk_aero_nearest_airports(&s_db, lat, lon, out, max,
                                           PK_AERO_DIST_APPROX));
}

int pk_aero_db_nearest_navaids(double lat, double lon,
                               pk_aero_near_t *out, int max)
{
    AERO_QUERY(0, pk_aero_nearest_navaids(&s_db, lat, lon, out, max,
                                          PK_AERO_DIST_APPROX));
}

int pk_aero_db_nearest_fixes(double lat, double lon,
                             pk_aero_near_t *out, int max)
{
    AERO_QUERY(0, pk_aero_nearest_fixes(&s_db, lat, lon, out, max,
                                        PK_AERO_DIST_APPROX));
}

int pk_aero_db_fix_by_ident(const char *ident, uint32_t *out, int max)
{
    /* reader 参数非法返回负错误码，对外统一收敛成 0（"没有"） */
    if (s_state != PK_AERO_DB_READY) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = (s_state == PK_AERO_DB_READY)
          ? pk_aero_fix_by_ident(&s_db, ident, out, max) : 0;
    xSemaphoreGive(s_lock);
    return n > 0 ? n : 0;
}
