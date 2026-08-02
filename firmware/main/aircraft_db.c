/*
 * aircraft_db.c — SD 卡机型库（/sdcard/aero/pk_actdb.bin）懒加载 + 查询。
 *
 * 格式解析与二分查找在 aircraft_db_reader.c（纯 C，可进 host 单测）；
 * 本文件负责：懒加载状态机、并发、单条 last-lookup 缓存、公共入口。
 *
 * ── 为什么从固件搬到 SD ────────────────────────────────────────────
 * 原先 8.16 MB 的 aircraft_db.bin 走 EMBED_FILES 进 .rodata，占了 factory
 * 分区的三分之二。搬走的理由不是省 flash（那只是顺带），而是**更新节奏**：
 * 机型库来自周更的 tar1090-db，SD 上的航空数据是 28 天的 AIRAC 周期，
 * 绑在一起就得让其中一个等另一个，或者为换机型库重刷整个固件。
 * 产品形态上 SD 是必插的（地图、航空数据都在卡上），所以"无卡降级"不是
 * 真实场景——真降级了也只是机型/注册号显示不出来，ICAO24 照常显示。
 *
 * ── 加载链路（照抄 pk_aero_db.c，去掉解密）────────────────────────
 *   stat → heap_caps_malloc(SPIRAM) → 64 KB 分块 fread（块间 vTaskDelay(1)
 *   让渡 + 查拔卡）→ SHA-256 分块校验 → pk_actdb_init（容器头 + PKADB1
 *   段边界）→ 发布 READY。失败释放缓冲，按卡还在不在分流 ERROR / ABSENT。
 *
 *   不解密：机型库是公开数据（tar1090-db），容器头里 enc_algo == 0，
 *   省掉整块 8 MB 的 AES-CTR（pk_aero 的 10.9 MB 实测 189 ms）。
 *
 * ── 并发（照 pk_aero_db.c 的两把锁）───────────────────────────────
 *   - s_lock：串行化「查询」与「发布/卸载」。查询全程持锁，卸载先拿锁再
 *     free——查询进行中缓冲绝不会被释放。查询是一次二分（~19 次 PSRAM 读，
 *     µs 级），持锁窗口比 pk_aero 的 nearest 短两个数量级。
 *   - s_io_lock：单独罩住 fopen→fread→fclose，给 pk_sdcard 的 pre-unmount
 *     回调当栅栏（IDF 的 unmount 会 free 掉含 FIL 数组的 fat_ctx，开着文件
 *     卸载就是 use-after-free）。与 s_lock 从不嵌套，锁序天然无环。
 *
 * ── 单条 last-lookup 缓存 ──────────────────────────────────────────
 * 调用模式是"逐架连续查同一个 icao24"：adsb_list.c:902-906 一帧内对同一
 * 架飞机连查 reg/code/model 三次，traffic_page.c:516-518 连查 code/desc/reg
 * 三次；且抽屉展开期间每帧都在重查同一架。单条缓存把这两种模式都吃满：
 *   - 一帧内的 3~4 连查 → 1 次二分 + 2~3 次命中；
 *   - 跨帧重查同一架   → 0 次二分（缓存跨帧存活）。
 * 做 LRU 只有在"一帧内交替查多架"时才有额外收益，而现实里没有这种调用点
 * （列表行只画呼号，不查库）——多写一个淘汰策略换不来一次命中。
 * 真机实测（8.16 MB 库，flash 版）：四字段各二分 234 µs → 缓存版 21 µs。
 *
 * 缓存里存的是**拷贝**不是指针：数据源是 SD 载入 PSRAM 的缓冲，拔卡即释放，
 * 往外发指进缓冲的指针就是 use-after-free。发布/卸载都会 bump s_generation，
 * 缓存记着自己那份属于哪一代，换库自动失效。
 */

#include "aircraft_db.h"
#include "aircraft_db_reader.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"    /* EXT_RAM_BSS_ATTR */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "psa/crypto.h"  /* IDF v6 mbedtls 4.x：SHA-256 只能走 PSA（硬件加速）*/

#include "pk_sdcard.h"

static const char *TAG = "acdb";

#define ACTDB_PATH           "/sdcard/aero/pk_actdb.bin"

/* 分块尺寸沿用 pk_aero_db 的实测甜点（64 KB = 5.99 MB/s；SHA 走内存，
 * 块放大到 512 KB 只为少让渡几次）。 */
#define ACTDB_READ_CHUNK     (64 * 1024)
#define ACTDB_SHA_CHUNK      (512 * 1024)

#define ACTDB_RETRY_MS       3000
#define ACTDB_WATCH_MS       2000

/* 开机首次尝试前的静默期。比 pk_aero 的 5 s 晚一截是故意的：两个库都在
 * SD 上，pk_aero 先加载（实测 2.5 s 出头），错开就不用抢同一条 SD 带宽。
 * 机型库只在用户点开某一架飞机的详情时才用得到，晚几秒完全无感。 */
#define ACTDB_STARTUP_DELAY_MS 12000

/* 任务参数照 pk_aero_db：prio 2（低于 pfd/dsp/sdr，全程可被渲染抢占）、
 * 核 1（那边只有更高优先级的 sdr/dsp/tile_ld1，本任务只吃空闲片，抢不到
 * ADS-B 解调头上）。栈比 pk_aero 的 16 KB 小一半：本任务没有 nearest 的
 * 栈上工作区，也没有 AES 上下文，只有 fread + PSA hash + 日志。 */
#define ACTDB_TASK_STACK     (8 * 1024)
#define ACTDB_TASK_PRIO      2
#define ACTDB_TASK_CORE      1

/* READY 后跑一次的真机自检（照 pk_aero_db.c 的 PK_AERO_DB_SMOKE 风格：
 * 只打日志、不改状态）。它同时是「一次二分 vs 四次二分」的真机对拍——
 * UI 上那条路径要人手点开抽屉才走得到，而这条在加载日志里就能看到。
 * 真值随 tar1090-db 周更变化，所以只报告不断言。 */
#define PK_ACDB_SMOKE 1

/* ---- 状态（发布/卸载都在 s_lock 内完成） ------------------------------ */

typedef enum {
    ACTDB_ABSENT = 0,   /* 无卡 / 无文件：周期性重试 */
    ACTDB_LOADING,      /* 只存在于 load_once() 内部 */
    ACTDB_READY,
    ACTDB_ERROR,        /* 文件坏：等卡被重插过再试 */
} actdb_state_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_io_lock;
static volatile actdb_state_t s_state = ACTDB_ABSENT;
static uint8_t   *s_buf;        /* PSRAM 整文件缓冲（含明文容器头） */
static pk_actdb_t s_db;         /* reader 解析态（指进 s_buf） */
static volatile uint32_t s_generation;
static volatile uint32_t s_err_generation;
static const char *volatile s_err;

/* 累计二分次数。留着是因为它是"缓存到底生效没有"的唯一客观证据（开机
 * smoke 读它），一个 uint32 的代价可以忽略。 */
static uint32_t s_n_search;

/* ---- 单条 last-lookup 缓存 ---------------------------------------------
 *
 * 放 PSRAM（EXT_RAM_BSS_ATTR）而不是内部 .bss：check_early_heap.py 的
 * 66000 B 底线只剩 ~100 B 余量，这条缓存 ~96 B 直接把它压穿（实测
 * 65984 B，构建失败）。它只在渲染路径上读写，PSRAM 延迟无所谓——真正
 * 省下的那几十次 PSRAM 随机读比这点访问开销大两个数量级。 */
static EXT_RAM_BSS_ATTR struct {
    bool               valid;
    uint32_t           icao24;
    uint32_t           generation;   /* 这份拷贝属于哪一代库 */
    bool               found;        /* 负结果也缓存：查不到的飞机同样每帧重查 */
    pk_aircraft_info_t info;
} s_cache;

/* ---- 加载 -------------------------------------------------------------- */

/* 分块读入。成功返回 true；失败把原因写进 *why（静态串）。
 * 调用方持着 s_io_lock（见文件头）。 */
static bool actdb_read(FILE *f, uint8_t *buf, size_t len, const char **why)
{
    for (size_t off = 0; off < len; ) {
        size_t want = len - off;
        if (want > ACTDB_READ_CHUNK) want = ACTDB_READ_CHUNK;
        if (fread(buf + off, 1, want, f) != want) {
            /* 短读多半是读到一半卡被拔了，也可能是文件被截断 */
            *why = "short read";
            return false;
        }
        off += want;
        /* 让渡 + 快速拔卡止损：不等整文件读完才发现卡没了 */
        vTaskDelay(1);
        if (!pk_sdcard_is_mounted()) { *why = "card removed"; return false; }
    }
    return true;
}

/* SHA-256 分块校验（硬件 SHA；块间让渡）。 */
static bool actdb_sha_verify(const pk_actdb_t *db)
{
    if (psa_crypto_init() != PSA_SUCCESS) return false;   /* 幂等 */
    psa_hash_operation_t h = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&h, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
    for (uint32_t at = 0; at < db->payload_len; ) {
        uint32_t n = db->payload_len - at;
        if (n > ACTDB_SHA_CHUNK) n = ACTDB_SHA_CHUNK;
        if (psa_hash_update(&h, db->payload + at, n) != PSA_SUCCESS) {
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

#if PK_ACDB_SMOKE
static void actdb_smoke_check(void);
#endif

/* 单次加载尝试。返回 true = 已发布 READY。
 * 返回 false 时状态已按"卡还在不在"落到 ERROR（原因存 s_err，等卡重插
 * 再试——同一份坏文件每 3 s 重读一遍毫无意义）或 ABSENT。 */
static bool actdb_load_once(void)
{
    struct stat fst;
    if (stat(ACTDB_PATH, &fst) != 0 || !S_ISREG(fst.st_mode)) {
        return false;   /* 没有文件：保持 ABSENT，周期性重试 */
    }
    size_t len = (size_t)fst.st_size;
    if (len < PK_ACTDB_HEADER_SIZE) {
        s_err_generation = pk_sdcard_mount_generation();
        s_state = ACTDB_ERROR;
        s_err   = "file too small";
        ESP_LOGE(TAG, "%s: %u B — too small", ACTDB_PATH, (unsigned)len);
        return false;
    }

    s_state = ACTDB_LOADING;
    ESP_LOGI(TAG, "loading %s (%.2f MB) in background",
             ACTDB_PATH, len / 1048576.0);
    int64_t t0 = esp_timer_get_time();

    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        s_err_generation = pk_sdcard_mount_generation();
        s_state = ACTDB_ERROR;
        s_err   = "psram alloc";
        ESP_LOGE(TAG, "PSRAM alloc %u B failed", (unsigned)len);
        return false;
    }

    const char *why = "?";
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    FILE *f = fopen(ACTDB_PATH, "rb");   /* SD 只读红线：只 "rb" */
    bool ok = false;
    if (f == NULL) {
        why = "fopen";
    } else {
        ok = actdb_read(f, buf, len, &why);
        fclose(f);
    }
    xSemaphoreGive(s_io_lock);

    pk_actdb_t db;
    if (ok) {
        int rc = pk_actdb_init(&db, buf, len, true);
        if (rc != PK_ACTDB_OK) {
            ok = false;
            why = rc == PK_ACTDB_ERR_MAGIC     ? "bad magic"
                : rc == PK_ACTDB_ERR_VERSION   ? "bad version"
                : rc == PK_ACTDB_ERR_ENCRYPTED ? "encrypted"
                                               : "bad layout";
        }
    }
    if (ok && !actdb_sha_verify(&db)) {
        ok = false;
        why = "sha256 mismatch";
    }

    if (!ok) {
        free(buf);
        if (!pk_sdcard_is_mounted()) {
            /* 半路拔卡不算文件坏：回 ABSENT，重插自动重来 */
            s_state = ACTDB_ABSENT;
            ESP_LOGW(TAG, "load aborted (%s) — card gone, back to ABSENT", why);
        } else {
            /* 记下进 ERROR 时的挂载代数：退出条件靠它判「卡被重插过」，
             * 而不是去采样 is_mounted 的电平（pk_aero_db.c 里那段血泪）。 */
            s_err_generation = pk_sdcard_mount_generation();
            s_state = ACTDB_ERROR;
            s_err   = why;
            ESP_LOGE(TAG, "load failed: %s (re-insert card to retry)", why);
        }
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_buf         = buf;
    s_db          = db;
    s_err         = NULL;
    s_cache.valid = false;      /* 换库 → 上一份拷贝作废 */
    s_generation++;
    s_state       = ACTDB_READY;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "aircraft DB ready (container v%u cycle %s, payload v%u): "
                  "%lu records, %u types, %.1f KB strings — %.2f MB, %.2f s, "
                  "SPIRAM free %u B",
             (unsigned)db.container_version, db.cycle, (unsigned)db.version,
             (unsigned long)db.n_records, (unsigned)db.n_types,
             db.strings_size / 1024.0, len / 1048576.0,
             (esp_timer_get_time() - t0) / 1e6,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

/* 卸载（拔卡触发）。持锁 free：正在进行的查询会先跑完再轮到这里。 */
static void actdb_unload(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_buf);
    s_buf = NULL;
    memset(&s_db, 0, sizeof(s_db));
    s_err         = NULL;
    s_cache.valid = false;
    s_generation++;
    s_state       = ACTDB_ABSENT;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "card removed — aircraft DB unloaded (auto-reload on insert)");
}

/* ---- 后台任务（与 pk_aero_db.c 的 aero_task 同构）---------------------- */

static void actdb_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(ACTDB_STARTUP_DELAY_MS));

    while (1) {
        switch (s_state) {
        case ACTDB_ABSENT:
            if (pk_sdcard_is_mounted() && actdb_load_once()) {
#if PK_ACDB_SMOKE
                actdb_smoke_check();
#endif
                continue;   /* 直接进 READY 分支的监视节奏 */
            }
            vTaskDelay(pdMS_TO_TICKS(ACTDB_RETRY_MS));
            break;

        case ACTDB_READY:
            vTaskDelay(pdMS_TO_TICKS(ACTDB_WATCH_MS));
            if (!pk_sdcard_is_mounted()) actdb_unload();
            break;

        case ACTDB_ERROR:
            /* 坏文件不反复重读；卡被重插过（= 用户来处理了）才回 ABSENT。
             * 判据是挂载代数变了而不是采样 !is_mounted——后者会漏掉短暂的
             * 插拔窗口（pk_aero_db.c 的 ERROR 分支里有实测记录）。 */
            vTaskDelay(pdMS_TO_TICKS(ACTDB_WATCH_MS));
            if (!pk_sdcard_is_mounted() ||
                pk_sdcard_mount_generation() != s_err_generation) {
                s_state = ACTDB_ABSENT;
                s_err   = NULL;
            }
            break;

        default:   /* LOADING 只存在于 actdb_load_once() 内部 */
            vTaskDelay(pdMS_TO_TICKS(ACTDB_RETRY_MS));
            break;
        }
    }
}

/* ---- 查询 -------------------------------------------------------------- */

/* 返回缓存里那条（命中）或 NULL（未命中/库未就绪）。
 * 全程持 s_lock：卸载路径同样先拿锁再 free(s_buf)，所以"锁在我手上"期间
 * payload 一定还活着。返回的指针指向 s_cache（拷贝），出锁后依然有效。 */
static const pk_aircraft_info_t *lookup_cached(uint32_t icao24)
{
    icao24 &= 0xFFFFFF;
    if (s_lock == NULL) return NULL;   /* init 之前（不该发生，兜底） */

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!(s_cache.valid && s_cache.generation == s_generation &&
          s_cache.icao24 == icao24)) {
        if (s_state != ACTDB_READY) {
            xSemaphoreGive(s_lock);
            return NULL;   /* 未就绪不写缓存：READY 之后第一次查就该真查 */
        }
        s_n_search++;
        s_cache.found      = pk_actdb_lookup(&s_db, icao24, &s_cache.info);
        s_cache.icao24     = icao24;
        s_cache.generation = s_generation;
        s_cache.valid      = true;
    }
    const bool found = s_cache.found;
    xSemaphoreGive(s_lock);
    return found ? &s_cache.info : NULL;
}

#if PK_ACDB_SMOKE
static void actdb_smoke_check(void)
{
    /* 2026-08 tar1090-db 真值：0x004002 = Z-WPA / B732 / L2J，
     *                          0xA526C7 = N4309X / P28A / L1P。
     * 真值随周更变化，所以只报告不断言——变了说明数据新了，不是坏了。
     *
     * 注意：本函数跑在 acdb 任务上，是查询路径的**第二个**调用方（正常
     * 只有 UI 渲染任务）。极端时序下 UI 正好在中间查了另一架，缓存被换掉，
     * 下面几个指针就会打印出那一架的字段。只影响这条日志，不会崩——
     * 指向的始终是 s_cache 这块合法内存。为一条自检日志上引用计数不值当。 */
    static const uint32_t kIcao[2] = { 0x004002, 0xA526C7 };

    for (int k = 0; k < 2; k++) {
        const uint32_t icao = kIcao[k];
        pk_aircraft_info_t tmp;

        /* 旧行为：四个字段各做一次完整二分（直接调 reader，不过缓存） */
        uint32_t s0 = s_n_search;
        int64_t t0 = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < 4; i++) pk_actdb_lookup(&s_db, icao, &tmp);
        xSemaphoreGive(s_lock);
        int64_t dt_old = esp_timer_get_time() - t0;

        /* 新行为：四个旧入口（1 次二分 + 3 次缓存命中） */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cache.valid = false;
        xSemaphoreGive(s_lock);
        t0 = esp_timer_get_time();
        const char *code  = pk_aircraft_type_code(icao);
        const char *model = pk_aircraft_type_model(icao);
        const char *desc  = pk_aircraft_type_desc(icao);
        const char *reg   = pk_aircraft_registration(icao);
        int64_t dt_new = esp_timer_get_time() - t0;
        /* 只数走缓存那条路径的二分次数：上面那 4 次直接调 reader，不计数。 */
        uint32_t searches = s_n_search - s0;

        ESP_LOGI(TAG, "smoke %06lX: reg=%s code=%s model=%s desc=%s | "
                      "4×lookup %lld us vs cached 4 calls %lld us, "
                      "searches=%lu (want 1) [%s]",
                 (unsigned long)icao,
                 reg ? reg : "-", code ? code : "-",
                 model ? model : "-", desc ? desc : "-",
                 (long long)dt_old, (long long)dt_new,
                 (unsigned long)searches,
                 (searches == 1 && dt_new < dt_old) ? "PASS" : "CHECK");
    }
}
#endif /* PK_ACDB_SMOKE */

/* ---- 公共 API ----------------------------------------------------------- */

/* pk_sdcard 卸载前回调：纯栅栏（同 pk_aero_db.c 的 sd_io_barrier_cb）。
 * take 到手即说明加载路径的 fopen→fclose 会话已退出（或根本没在跑）。 */
static void sd_io_barrier_cb(void)
{
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    xSemaphoreGive(s_io_lock);
}

void pk_aircraft_db_init(void)
{
    if (s_lock != NULL) return;   /* 幂等 */
    s_lock    = xSemaphoreCreateMutex();
    s_io_lock = xSemaphoreCreateMutex();
    pk_sdcard_register_pre_unmount_cb(sd_io_barrier_cb);

    /* 只创建任务，不做任何 IO——开机路径零阻塞。 */
    BaseType_t ok = xTaskCreatePinnedToCore(actdb_task, "acdb",
                                            ACTDB_TASK_STACK, NULL,
                                            ACTDB_TASK_PRIO, NULL,
                                            ACTDB_TASK_CORE);
    if (ok != pdTRUE) ESP_LOGE(TAG, "acdb task create failed");
}

bool pk_aircraft_lookup(uint32_t icao24, pk_aircraft_info_t *out)
{
    if (out == NULL) return false;
    const pk_aircraft_info_t *i = lookup_cached(icao24);
    if (i == NULL) { memset(out, 0, sizeof(*out)); return false; }
    *out = *i;
    return true;
}

const char *pk_aircraft_type_code(uint32_t icao24)
{
    const pk_aircraft_info_t *i = lookup_cached(icao24);
    return (i && i->code[0]) ? i->code : NULL;
}

const char *pk_aircraft_type_model(uint32_t icao24)
{
    const pk_aircraft_info_t *i = lookup_cached(icao24);
    return (i && i->model[0]) ? i->model : NULL;
}

const char *pk_aircraft_type_desc(uint32_t icao24)
{
    const pk_aircraft_info_t *i = lookup_cached(icao24);
    return (i && i->desc[0]) ? i->desc : NULL;
}

const char *pk_aircraft_registration(uint32_t icao24)
{
    const pk_aircraft_info_t *i = lookup_cached(icao24);
    return (i && i->reg[0]) ? i->reg : NULL;
}
