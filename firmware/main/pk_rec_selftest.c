/* pk_rec_selftest.c — 注入式自检实现。设计说明见 pk_rec_selftest.h。
 *
 * 风格照 apt_detail_page.c 的真机自检（smoke_task）：只在
 * PK_REC_SELFTEST=1 时编译进去，boot 后延时几秒（等 SD/session 就绪）
 * 起一个任务跑完就删除自己。跟 apt_detail 的"只打日志不断言真值"不同
 * ——这里喂进去的是我们自己造的确定性数据（不是随 AIRAC 周期变化的真实
 * 数据），所以可以也应该做硬断言：CHECK() 失败会计入 g_fail 并打
 * ESP_LOGE，跑完汇总 PASS/FAIL。
 *
 * 覆盖的链路：
 *   1) record_dispatch() → record_sink_rec_store → pk_rec_store_
 *      append_adsb_line()：原始报文落 adsb-NNN.tsl；
 *   2) pk_rec_ingest_position() / pk_rec_ingest_identity()（不依赖 CPR，
 *      直接喂已解好的字段）→ traffic.trk 的位置/身份记录 + 在线索引；
 *   3) pk_rec_store_rebuild_index()：从 traffic.trk 全扫独立重建一份索引
 *      （不碰运行中的那份），核对点数/呼号/IS_OWN 标记——这是「掉电重建」
 *      逻辑在真机文件系统上的端到端验证（纯逻辑那一半已经在
 *      test_pk_rec_idx.c 里 host 测过）。
 *
 * 不覆盖 own.trk：那条链路是 1 Hz 周期性的，没有可注入的"喂一条就出结果"
 * 入口，真机上跑几秒自然就有数据，靠人工核对诊断页/pk_own_sampler_stats()
 * 更直接。
 */
#include "pk_rec_selftest.h"

#if PK_REC_SELFTEST

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "record_sink.h"
#include "pk_rec_store.h"
#include "pk_rec_format.h"
#include "pk_rec_idx.h"
#include "pk_rec_ingest.h"

static const char *TAG = "rec_selftest";

/* 两个假 ICAO，刻意避开常见真实分配段；A 在自检里同时扮演"绑定本机"。 */
#define SELFTEST_ICAO_A 0xABCDEFu
#define SELFTEST_ICAO_B 0x123456u

static int g_fail;

#define CHECK(cond) do { \
    if (!(cond)) { \
        ESP_LOGE(TAG, "FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } else { \
        ESP_LOGI(TAG, "ok   %s:%d: %s", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static bool file_contains(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return false;
    char buf[512];
    bool found = false;
    size_t n;
    /* 自检样本行很短，512 B 一块足够整行落在同一块里，不处理跨块拼接。 */
    while ((n = fread(buf, 1, sizeof(buf) - 1, fp)) > 0) {
        buf[n] = '\0';
        if (strstr(buf, needle) != NULL) { found = true; break; }
    }
    fclose(fp);
    return found;
}

static void selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));   /* 等 pk_sdcard / pk_rec_store session 就绪 */

    char session_dir[64];
    if (!pk_rec_store_session_dir(session_dir, sizeof(session_dir))) {
        ESP_LOGW(TAG, "self-test skipped: no SD session open");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "self-test start, session=%s", session_dir);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ts_ms = (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;

    /* --- 1) 原始报文：走 record_dispatch，驱动全部 4 个已注册 sink
     * （含 record_sink_rec_store）--- */
    record_t rec = {
        .ts_ms   = ts_ms,
        .icao24  = SELFTEST_ICAO_A,
        .df      = 17,
        .hex_len = 4,
    };
    strcpy(rec.hex, "DEAD");
    record_dispatch(&rec);

    /* --- 2) 目标位置 + 身份：直接走 pk_rec_ingest_*，不依赖 CPR/射频 ---
     *
     * pk_rec_ingest_position/identity 现在只做编码 + 非阻塞入队
     * （xQueueSend(..., 0)），真正的 fwrite 挪到了 pk_rec_ingest.c 自带的
     * 写任务上、异步完成——不能像改造前那样调用完就直接读文件校验，要先
     * 用 pk_rec_ingest_stats() 的 written 计数轮询等写任务把这 3 条排空，
     * 再 flush_all()。 */
    uint32_t written_before = 0, dropped_before = 0;
    pk_rec_ingest_stats(&written_before, &dropped_before);

    pk_rec_ingest_position(SELFTEST_ICAO_A, ts_ms, 31.230416, 121.473701,
                            /*have_alt=*/true, /*alt_ft=*/3500,
                            /*have_gs=*/true, /*gs_kt=*/120,
                            /*have_track=*/true, /*track_deg=*/90,
                            /*have_vs=*/true, /*vs_fpm=*/512,
                            /*on_ground=*/false, /*from_surface_cpr=*/false);
    pk_rec_ingest_identity(SELFTEST_ICAO_A, ts_ms, "SELFTST1", /*PK_WAKE_LARGE=*/3);
    pk_rec_ingest_position(SELFTEST_ICAO_B, ts_ms + 1000, 31.240000, 121.480000,
                            /*have_alt=*/false, 0,
                            /*have_gs=*/false, 0, /*have_track=*/false, 0,
                            /*have_vs=*/false, 0,
                            /*on_ground=*/false, /*from_surface_cpr=*/false);

    uint32_t written_after = written_before;
    for (int i = 0; i < 50; i++) {   /* 最多等 50*20 = 1000 ms */
        pk_rec_ingest_stats(&written_after, NULL);
        if (written_after - written_before >= 3) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    CHECK(written_after - written_before >= 3);

    pk_rec_store_flush_all();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* --- 3) 读回原始报文，校验样本行落盘（session 目录里第一卷固定叫
     * adsb-000.tsl，见 pk_rec_store_fs.c 的 ensure_adsb_volume_open_locked） --- */
    char adsb_path[80];
    snprintf(adsb_path, sizeof(adsb_path), "%s/adsb-000.tsl", session_dir);
    CHECK(file_contains(adsb_path, "*DEAD;"));

    /* --- 4) 读回 traffic.trk 最后三条记录，逐字段解码校验 --- */
    char trk_path[80];
    snprintf(trk_path, sizeof(trk_path), "%s/traffic.trk", session_dir);
    FILE *fp = fopen(trk_path, "rb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        fseek(fp, 0, SEEK_END);
        long total = ftell(fp);
        long records_total = (total - (long)PK_REC_HEADER_LEN) / (long)PK_TRK_RECORD_LEN;
        CHECK(records_total >= 3);
        if (records_total >= 3) {
            fseek(fp, (long)PK_REC_HEADER_LEN + (records_total - 3) * (long)PK_TRK_RECORD_LEN,
                  SEEK_SET);
            uint8_t buf[PK_TRK_RECORD_LEN];

            CHECK(fread(buf, 1, PK_TRK_RECORD_LEN, fp) == PK_TRK_RECORD_LEN);
            CHECK(pk_trk_rec_type_peek(buf) == PK_TRK_REC_POSITION);
            pk_trk_pos_t pos;
            pk_trk_pos_decode(buf, &pos);
            CHECK(pos.icao24[0] == 0xAB && pos.icao24[1] == 0xCD && pos.icao24[2] == 0xEF);
            CHECK(pos.alt_d25 == (int16_t)(3500 / 25));
            CHECK(pos.gs_kt == 120);
            CHECK(pos.vs_fpm_d64 == (int16_t)(512 / 64));

            CHECK(fread(buf, 1, PK_TRK_RECORD_LEN, fp) == PK_TRK_RECORD_LEN);
            CHECK(pk_trk_rec_type_peek(buf) == PK_TRK_REC_IDENTITY);
            pk_trk_id_t id;
            pk_trk_id_decode(buf, &id);
            CHECK(strncmp(id.callsign, "SELFTST1", 8) == 0);
            CHECK(id.emitter_category == 3);

            CHECK(fread(buf, 1, PK_TRK_RECORD_LEN, fp) == PK_TRK_RECORD_LEN);
            CHECK(pk_trk_rec_type_peek(buf) == PK_TRK_REC_POSITION);
            pk_trk_pos_decode(buf, &pos);
            CHECK(pos.icao24[0] == 0x12 && pos.icao24[1] == 0x34 && pos.icao24[2] == 0x56);
            CHECK(pos.alt_d25 == PK_REC_ALT_INVALID);
            CHECK(pos.gs_kt == PK_REC_GS_INVALID);
            CHECK(pos.track_deg10 == PK_REC_TRACK_INVALID);
            CHECK(pos.vs_fpm_d64 == PK_REC_VS_INVALID);
        }
        fclose(fp);
    }

    /* --- 5) 独立重建索引（不碰运行中那份），校验点数/呼号/IS_OWN --- */
    uint8_t own_icao_bytes[3] = {
        (uint8_t)((SELFTEST_ICAO_A >> 16) & 0xFFu),
        (uint8_t)((SELFTEST_ICAO_A >> 8) & 0xFFu),
        (uint8_t)(SELFTEST_ICAO_A & 0xFFu),
    };
    static pk_rec_idx_table_t s_rebuilt;   /* 20 KB 量级，别上栈 */
    bool rebuild_ok = pk_rec_store_rebuild_index(session_dir, own_icao_bytes, &s_rebuilt);
    CHECK(rebuild_ok);
    if (rebuild_ok) {
        const pk_idx_rec_t *e = pk_rec_idx_find(&s_rebuilt, own_icao_bytes);
        CHECK(e != NULL);
        if (e != NULL) {
            CHECK(e->point_count >= 1);
            CHECK(e->identity_count >= 1);
            CHECK((e->flags & PK_IDX_FLAG_HAD_POSITION) != 0);
            CHECK((e->flags & PK_IDX_FLAG_HAD_CALLSIGN) != 0);
            CHECK((e->flags & PK_IDX_FLAG_IS_OWN) != 0);
            CHECK(strncmp(e->callsign, "SELFTST1", 8) == 0);
        }

        uint8_t icao_b[3] = {0x12, 0x34, 0x56};
        const pk_idx_rec_t *eb = pk_rec_idx_find(&s_rebuilt, icao_b);
        CHECK(eb != NULL);
        if (eb != NULL) {
            CHECK((eb->flags & PK_IDX_FLAG_IS_OWN) == 0);   /* B 没绑定，不该带 IS_OWN */
        }
    }

    ESP_LOGI(TAG, "self-test done: %s (%d failed assertion%s)",
             g_fail == 0 ? "PASS" : "FAIL", g_fail, g_fail == 1 ? "" : "s");
    vTaskDelete(NULL);
}

void pk_rec_selftest_init(void)
{
    if (xTaskCreatePinnedToCore(selftest_task, "rec_selftest", 6144, NULL, 2,
                                NULL, 0) != pdTRUE) {
        ESP_LOGE(TAG, "selftest task create failed");
    }
}

#else

void pk_rec_selftest_init(void) { }

#endif /* PK_REC_SELFTEST */
