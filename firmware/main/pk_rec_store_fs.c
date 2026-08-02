/*
 * pk_rec_store_fs.c — pk_rec_store 的真机文件系统 / FreeRTOS 胶水层。
 *
 * 纯逻辑（序号分配/回绕、保留策略选谁删、降级档位判定）在 pk_rec_store.c，
 * 那部分不摸文件系统、进 host 单测。本文件反过来——只做「摸文件系统」
 * 这一件事：session 目录扫描/创建/清理、文件按需打开与滚动、
 * pk_sdcard 热插拔的 pre-unmount 静默。依赖 FreeRTOS 信号量 + IDF VFS，
 * host 侧编不过，所以单独成一个 TU，不进 test_pk_rec_store.c 的
 * #include 列表。
 *
 * 目录/文件布局（设计文档「落盘布局」节）：
 *   <mount>/rec/<NNNN>/
 *       adsb-NNN.tsl    原始报文 ts-line（**无文件头**——要跟客户端既有
 *                       解码器认的 "<ts_ms> *<HEX>;" 纯文本格式保持字节
 *                       兼容，这也是 pk_rec_format.h 的 pk_rec_file_kind_t
 *                       只有 3 个二进制文件枚举值、不含它的原因），满
 *                       16 MiB 滚下一卷
 *       traffic.trk     32 B 定长记录，带 32 B 二进制文件头
 *       own.trk         48 B 定长记录，带 32 B 二进制文件头
 *       own_adsb.tsl    绑定机专属原始报文，同 adsb-NNN.tsl 格式，无头
 *       session.json    元数据（开机 partial，pre-unmount 时覆盖 final）
 *
 * traffic.idx 按机摘要**不在本阶段（3a）范围**：它的内容来自
 * traffic.trk 里逐条记录聚合出的按机统计，3a 没有任何生产者往
 * traffic.trk 写数据，这里先不建。3b 接上 dsp_task 之后再补。
 */
#include "pk_rec_store.h"
#include "pk_rec_format.h"
#include "pk_rec_idx.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ble_gatt.h"     /* pk_ble_device_name() —— session.json 的 device_name 字段 */
#include "pk_clock.h"     /* pk_clock_is_synced() —— session.json 的 start_clock_synced 字段 */
#include "pk_sdcard.h"
#include "ui_state.h"     /* pk_ui_toast_show_blink() —— SD 写失败/降级告警（阶段 5b） */

static const char *TAG = "rec_store";

#define REC_ROOT_NAME       "rec"
#define REC_STORE_TASK_STACK 3072
#define REC_STORE_POLL_MS    2000
#define ADSB_ROTATE_BYTES    (16u * 1024u * 1024u)
#define REC_STORE_MAX_SCAN   512   /* 一次扫描/保留最多处理这么多个 session 目录 */

/* 所有静态缓冲走 EXT_RAM——内部 RAM 早期堆余量只有一百多字节，一个字节
 * 都不能进内部 .bss（firmware/scripts/check_early_heap.py 的红线）。 */
EXT_RAM_BSS_ATTR static bool     s_seq_used[PK_REC_STORE_SEQ_SPACE];
EXT_RAM_BSS_ATTR static uint16_t s_seq_scan[REC_STORE_MAX_SCAN];
EXT_RAM_BSS_ATTR static uint16_t s_seq_victims[REC_STORE_MAX_SCAN];

static SemaphoreHandle_t s_lock;

static char     s_rec_root[32];       /* "<mount>/rec" */
static char     s_session_dir[48];    /* "<mount>/rec/NNNN" */
static bool     s_session_open;
static uint16_t s_session_seq;
static int64_t  s_session_start_ts_ms;

static FILE  *s_fp_adsb;
static size_t s_adsb_bytes;
static int    s_adsb_vol;

static FILE  *s_fp_traffic_trk;
static FILE  *s_fp_own_trk;

static bool    s_own_bound;
static uint8_t s_own_icao[3];
static FILE   *s_fp_own_adsb;

/* 按机摘要，运行期在线维护（每次 traffic.trk 追加成功就更新），session
 * 关闭时整表编码写出 traffic.idx。20 KB 量级（512 条 × 40 B），走
 * EXT_RAM——内存红线：所有静态缓冲一律 EXT_RAM_BSS_ATTR。 */
static EXT_RAM_BSS_ATTR pk_rec_idx_table_t s_idx;

/* --- 阶段 5b：session.json 的 counts/bounds/own_icao_changes ---------------
 * 三者都是「本 session 见过什么」，随 ensure_session_open_locked() 建新
 * session 一并 reset；纯逻辑（bounds 外包框、own_icao_changes 定长追加）
 * 在 pk_rec_store.h/.c，这里只管调用点与 JSON 序列化。体量都很小，不需要
 * 单独走 EXT_RAM（几十字节量级，跟 s_session_dir 这类小静态变量同档）。 */
static pk_rec_counts_t             s_counts;
static pk_rec_bounds_t             s_bounds;
static pk_rec_own_icao_changes_t   s_icao_changes;

/* --- 阶段 5b：SD 写失败/降级告警状态 ---------------------------------------
 * 四路 sink 各自的连续失败计数与"已判定失效"标记；降级档位则是模块级的
 * （物理 SD 容量，不随 session 边界复位，只随插卡换卡这类真实事件变化——
 * 见 ensure_session_open_locked 里的复位注释）。 */
typedef enum {
    REC_SINK_ADSB = 0,
    REC_SINK_OWN_ADSB,
    REC_SINK_TRAFFIC_TRK,
    REC_SINK_OWN_TRK,
    REC_SINK_COUNT,
} rec_sink_id_t;

static uint32_t         s_sink_fail[REC_SINK_COUNT];
static bool              s_sink_disabled[REC_SINK_COUNT];
static pk_rec_degrade_t  s_last_tier = PK_REC_DEGRADE_FULL;

/* --- 小 helper ---------------------------------------------------------- */

static bool ensure_dir(const char *path)
{
    if (mkdir(path, 0775) == 0) return true;
    if (errno == EEXIST) return true;
    ESP_LOGW(TAG, "mkdir(%s) failed: %s", path, strerror(errno));
    return false;
}

static bool is_all_digits(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

/* "0000".."9999" 目录名 → 序号；不匹配返回 -1。 */
static int parse_session_dirname(const char *name)
{
    size_t len = strlen(name);
    if (len != 4 || !is_all_digits(name, len)) return -1;
    return (name[0] - '0') * 1000 + (name[1] - '0') * 100 +
           (name[2] - '0') * 10 + (name[3] - '0');
}

static bool dirent_is_dir(const char *parent, const struct dirent *ent)
{
    if (ent->d_type == DT_DIR) return true;
    if (ent->d_type != DT_UNKNOWN) return false;
    /* 某些 VFS 实现可能不填 d_type；退回 stat 兜底判断。
     *
     * 缓冲区按最坏情况配：d_name 上限 255（NAME_MAX），parent 是 s_rec_root
     * 或 remove_dir_recursive 的 path（≤128）。拼接结果仍要判截断——不判的话
     * -O2 下 -Wformat-truncation 会直接报错，而且截断出来的路径 stat 到的是
     * 另一个东西，比"当成非目录跳过"危险得多。 */
    char path[128 + 1 + 256];
    int n = snprintf(path, sizeof(path), "%s/%s", parent, ent->d_name);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* 扫 s_rec_root 下的四位数字目录，填 s_seq_used 位图与 s_seq_scan 列表。
 * 返回列表长度（可能小于实际目录数——超过 REC_STORE_MAX_SCAN 的部分只
 * 影响保留策略清理的精确度，不影响正确性：位图不受这个上限限制，序号
 * 分配永远准）。调用方须持 s_lock。 */
static size_t scan_sessions_locked(void)
{
    memset(s_seq_used, 0, sizeof(s_seq_used));
    size_t n = 0;

    DIR *d = opendir(s_rec_root);
    if (d == NULL) return 0;   /* 目录还不存在（ENOENT）是正常情况 */

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (!dirent_is_dir(s_rec_root, ent)) continue;
        int seq = parse_session_dirname(ent->d_name);
        if (seq < 0) continue;
        s_seq_used[seq] = true;
        if (n < REC_STORE_MAX_SCAN) s_seq_scan[n++] = (uint16_t)seq;
    }
    closedir(d);
    return n;
}

/* 递归删除一个 session 目录（保留策略淘汰用）。FATFS 不支持非空目录
 * rmdir，必须先清空。 */
static void remove_dir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL) {
        unlink(path);   /* 万一传进来的其实是个文件，兜底 */
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[128 + 1 + 256];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            /* 截断后的路径指向别的文件，删它比留下这一条更糟——跳过并留痕。 */
            ESP_LOGW(TAG, "路径过长跳过: %s/%s", path, ent->d_name);
            continue;
        }
        if (dirent_is_dir(path, ent)) {
            remove_dir_recursive(child);
        } else if (unlink(child) != 0) {
            ESP_LOGW(TAG, "unlink(%s) failed: %s", child, strerror(errno));
        }
    }
    closedir(d);
    if (rmdir(path) != 0) {
        ESP_LOGW(TAG, "rmdir(%s) failed: %s", path, strerror(errno));
    }
}

/* --- session.json -------------------------------------------------------- */

static void write_session_json_locked(bool final)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/session.json", s_session_dir);
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "fopen(%s) failed: %s", path, strerror(errno));
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    char devname[40];
    snprintf(devname, sizeof(devname), "%s", pk_ble_device_name());

    int64_t end_ts_ms = final ? (int64_t)(esp_timer_get_time() / 1000) : -1;

    /* own_icao：3a 曾用 "pending" 占位（3a 不解 ICAO24→字符串）；3b 接上
     * own_ship 绑定后这里就是真值了。 */
    char own_icao_json[16] = "null";
    if (s_own_bound) {
        snprintf(own_icao_json, sizeof(own_icao_json), "\"%02X%02X%02X\"",
                 s_own_icao[0], s_own_icao[1], s_own_icao[2]);
    }

    fprintf(fp,
            "{\n"
            "  \"seq\": %u,\n"
            "  \"fw_version\": \"%s\",\n"
            "  \"device_name\": \"%s\",\n"
            "  \"start_ts_ms\": %lld,\n"
            "  \"start_clock_synced\": %s,\n"
            "  \"end_ts_ms\": %lld,\n"
            "  \"files\": [\"adsb-%03d.tsl\", \"traffic.trk\", \"own.trk\"%s],\n",
            (unsigned)s_session_seq,
            app != NULL ? app->version : "unknown",
            devname,
            (long long)s_session_start_ts_ms,
            pk_clock_is_synced() ? "true" : "false",
            (long long)end_ts_ms,
            s_adsb_vol,
            s_own_bound ? ", \"own_adsb.tsl\"" : "");

    /* counts：各文件已写记录数（阶段 5b，设计文档「session.json」节）。 */
    fprintf(fp,
            "  \"counts\": {\"traffic_pos\": %lu, \"traffic_id\": %lu, "
            "\"own\": %lu, \"adsb_lines\": %lu, \"own_adsb_lines\": %lu},\n",
            (unsigned long)s_counts.traffic_pos, (unsigned long)s_counts.traffic_id,
            (unsigned long)s_counts.own, (unsigned long)s_counts.adsb_lines,
            (unsigned long)s_counts.own_adsb_lines);

    /* bounds：本 session 见过的目标位置外包框，键名照抄客户端
     * coverage_bounds 约定（设计文档「session.json」节）。has_any=false
     * （这个 session 一条位置记录都没写过）序列化成 null，不编造 0,0 框。
     * e7 定点 → 浮点度数，%.7f 保住 1e7 定点的全部有效数字。 */
    if (s_bounds.has_any) {
        fprintf(fp,
                "  \"bounds\": {\"minLat\": %.7f, \"maxLat\": %.7f, "
                "\"minLon\": %.7f, \"maxLon\": %.7f},\n",
                (double)s_bounds.min_lat_e7 / 1e7, (double)s_bounds.max_lat_e7 / 1e7,
                (double)s_bounds.min_lon_e7 / 1e7, (double)s_bounds.max_lon_e7 / 1e7);
    } else {
        fprintf(fp, "  \"bounds\": null,\n");
    }

    fprintf(fp, "  \"own_icao\": %s,\n", own_icao_json);

    /* own_icao_changes[]：绑定/改绑/解绑历史（设计文档「绑定机独立文件」
     * 节）。unbind 事件 icao 写 null 而不是 "000000"——那看起来像一个真实
     * ICAO24，会被回放端误读成"绑定到了全零地址"。 */
    fprintf(fp, "  \"own_icao_changes\": [");
    for (size_t i = 0; i < s_icao_changes.count; i++) {
        const pk_rec_own_icao_change_t *e = &s_icao_changes.entries[i];
        if (e->bound) {
            fprintf(fp, "%s{\"ts_ms\": %lld, \"bound\": true, \"icao\": \"%02X%02X%02X\"}",
                    i ? ", " : "", (long long)e->ts_ms,
                    e->icao24[0], e->icao24[1], e->icao24[2]);
        } else {
            fprintf(fp, "%s{\"ts_ms\": %lld, \"bound\": false, \"icao\": null}",
                    i ? ", " : "", (long long)e->ts_ms);
        }
    }
    fprintf(fp, "],\n");

    fprintf(fp, "  \"complete\": %s\n}\n", final ? "true" : "false");

    fclose(fp);
}

/* --- session 生命周期 ------------------------------------------------------ */

/* 调用方须持 s_lock。无卡返回 false；已开则直接返回 true（幂等）。 */
static bool ensure_session_open_locked(void)
{
    if (s_session_open) return true;
    if (!pk_sdcard_is_mounted()) return false;

    snprintf(s_rec_root, sizeof(s_rec_root), "%s/%s",
             pk_sdcard_mount_point(), REC_ROOT_NAME);
    if (!ensure_dir(s_rec_root)) return false;

    size_t n = scan_sessions_locked();
    uint16_t new_seq = pk_rec_store_alloc_seq(s_seq_used);

    char dir[48];
    snprintf(dir, sizeof(dir), "%s/%04u", s_rec_root, new_seq);
    if (!ensure_dir(dir)) return false;

    snprintf(s_session_dir, sizeof(s_session_dir), "%s", dir);
    s_session_seq       = new_seq;
    s_session_open       = true;
    s_session_start_ts_ms = (int64_t)(esp_timer_get_time() / 1000);
    s_adsb_vol   = 0;
    s_adsb_bytes = 0;
    pk_rec_idx_reset(&s_idx);

    /* 阶段 5b：新 session = 新的「见过什么」统计窗口。sink 失效/连续失败
     * 计数也一并复位——新 session 常常对应插了张新卡或问题已解决，不该背
     * 着上个 session 的失效状态不放（"停止重试"是 session 内的约定，不是
     * 永久熔断）。s_last_tier（降级档位）不在这里复位：它跟着物理 SD 容量
     * 走，跨 session 边界仍然成立，复位反而会在同一张快满的卡上重复告警。 */
    memset(&s_counts, 0, sizeof(s_counts));
    pk_rec_bounds_reset(&s_bounds);
    pk_rec_own_icao_changes_reset(&s_icao_changes);
    memset(s_sink_fail, 0, sizeof(s_sink_fail));
    memset(s_sink_disabled, 0, sizeof(s_sink_disabled));

    /* 保留策略：连新建的这个一起算，超过 KEEP 个就删最旧的。 */
    if (n < REC_STORE_MAX_SCAN) s_seq_scan[n++] = new_seq;
    size_t nv = pk_rec_store_select_prune(s_seq_scan, n, PK_REC_STORE_KEEP_SESSIONS,
                                           s_seq_victims);
    for (size_t i = 0; i < nv; i++) {
        char victim[48];
        snprintf(victim, sizeof(victim), "%s/%04u", s_rec_root, s_seq_victims[i]);
        ESP_LOGI(TAG, "保留策略：清理旧 session %s", victim);
        remove_dir_recursive(victim);
    }

    write_session_json_locked(false);
    ESP_LOGI(TAG, "session 开始：%s", s_session_dir);
    return true;
}

/* --- 降级档位 -------------------------------------------------------------- */

static pk_rec_degrade_t current_degrade_tier_locked(void)
{
    uint64_t total = 0, free_b = 0;
    if (!pk_sdcard_info(&total, &free_b)) {
        return PK_REC_DEGRADE_OWN_ONLY;   /* 拿不到容量信息，按最保守档位处理 */
    }
    return pk_rec_store_degrade_tier(free_b);
}

/* 降级档位跃迁 → 弹一次闪烁告警（不阻断飞行，见 ui_state.h
 * pk_ui_toast_show_blink）。只在真的变化时触发，同一档位反复写不会重复
 * 弹（设计文档「同一故障只在状态跳变时弹一次」）。变差（tier 数值变大，
 * 枚举本身按严重度递增定义）才告警；变好（腾出空间/换了张更大的卡）只记
 * 日志，不需要用户操作、不必打扰。 */
static void degrade_alert_check_locked(pk_rec_degrade_t tier)
{
    if (tier == s_last_tier) return;
    pk_rec_degrade_t old = s_last_tier;
    s_last_tier = tier;
    if (tier > old) {
        pk_tr_id_t id = (tier == PK_REC_DEGRADE_OWN_ONLY)
                       ? PK_TR_TOAST_REC_SD_OWNONLY
                       : PK_TR_TOAST_REC_SD_NORAW;
        ESP_LOGW(TAG, "SD 剩余空间降级：档位 %d → %d", (int)old, (int)tier);
        pk_ui_toast_show_blink(id, true, 3);
    } else {
        ESP_LOGI(TAG, "SD 剩余空间恢复：档位 %d → %d", (int)old, (int)tier);
    }
}

/* current_degrade_tier_locked() 的告警版：无卡时不算"降级事件"（开机没
 * 插卡不该被当成"SD 空间告急"），只在卡确实挂载时才拿真实档位去跟上次比。
 * append 系列函数改用这个而不是裸的 current_degrade_tier_locked()。 */
static pk_rec_degrade_t degrade_tier_check_locked(void)
{
    pk_rec_degrade_t tier = current_degrade_tier_locked();
    if (pk_sdcard_is_mounted()) degrade_alert_check_locked(tier);
    return tier;
}

/* sink 写结果记账：ok=false 累加连续失败，达阈值（PK_REC_STORE_FAIL_
 * THRESHOLD）就标记该路失效、停止重试，并弹一次告警——只在 false→true
 * 这一次跳变弹，后续再失败不会重复刷屏。调用方只应在"确实尝试过写"时
 * 调用（降级跳过的那一路不算失败，也不该计进连续失败次数）。 */
static void sink_write_result_locked(rec_sink_id_t sink, bool ok)
{
    if (ok) { s_sink_fail[sink] = 0; return; }
    s_sink_fail[sink]++;
    if (!s_sink_disabled[sink] &&
        pk_rec_store_sink_should_disable(s_sink_fail[sink])) {
        s_sink_disabled[sink] = true;
        ESP_LOGE(TAG, "sink #%d 连续写失败 %u 次，已停用，停止重试",
                 (int)sink, (unsigned)s_sink_fail[sink]);
        pk_ui_toast_show_blink(PK_TR_TOAST_REC_SINK_FAIL, true, 3);
    }
}

/* --- 文件打开 --------------------------------------------------------------- */

static bool open_binary_with_header_locked(FILE **fp_slot, const char *filename,
                                            const char *magic, uint16_t record_size,
                                            uint8_t file_kind)
{
    if (*fp_slot != NULL) return true;
    if (!ensure_session_open_locked()) return false;

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", s_session_dir, filename);
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "fopen(%s) failed: %s", path, strerror(errno));
        return false;
    }

    pk_rec_header_t hdr = {0};
    memcpy(hdr.magic, magic, 8);
    hdr.format_version = 1;
    hdr.record_size    = record_size;
    hdr.endian_marker  = PK_REC_ENDIAN_MARKER;
    hdr.created_ts_ms  = (uint64_t)(esp_timer_get_time() / 1000);
    hdr.file_kind      = file_kind;

    uint8_t hdrbuf[PK_REC_HEADER_LEN];
    pk_rec_header_encode(&hdr, hdrbuf);
    if (fwrite(hdrbuf, 1, PK_REC_HEADER_LEN, fp) != PK_REC_HEADER_LEN) {
        ESP_LOGW(TAG, "写 %s 文件头失败: %s", path, strerror(errno));
        fclose(fp);
        return false;
    }

    *fp_slot = fp;
    return true;
}

static bool ensure_adsb_volume_open_locked(void)
{
    if (s_fp_adsb != NULL) return true;
    if (!ensure_session_open_locked()) return false;

    char path[64];
    snprintf(path, sizeof(path), "%s/adsb-%03d.tsl", s_session_dir, s_adsb_vol);
    FILE *fp = fopen(path, "ab");
    if (fp == NULL) {
        ESP_LOGW(TAG, "fopen(%s) failed: %s", path, strerror(errno));
        return false;
    }
    s_fp_adsb    = fp;
    s_adsb_bytes = 0;
    return true;
}

/* --- traffic.idx --------------------------------------------------------- */

/* 短开短关：只在 session 关闭时写一次整表（spec「traffic.idx」节）。
 * 调用方须持 s_lock 且 session 仍处于 open 状态（s_session_dir 有效）。 */
static void write_traffic_idx_locked(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/traffic.idx", s_session_dir);
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "fopen(%s) failed: %s", path, strerror(errno));
        return;
    }

    pk_rec_header_t hdr = {0};
    memcpy(hdr.magic, PK_REC_MAGIC_TRAFFIC_IDX, 8);
    hdr.format_version = 1;
    hdr.record_size    = PK_IDX_RECORD_LEN;
    hdr.endian_marker  = PK_REC_ENDIAN_MARKER;
    hdr.created_ts_ms  = (uint64_t)(esp_timer_get_time() / 1000);
    hdr.file_kind      = PK_REC_FILE_KIND_TRAFFIC_IDX;

    uint8_t hdrbuf[PK_REC_HEADER_LEN];
    pk_rec_header_encode(&hdr, hdrbuf);
    if (fwrite(hdrbuf, 1, PK_REC_HEADER_LEN, fp) != PK_REC_HEADER_LEN) {
        ESP_LOGW(TAG, "写 %s 文件头失败: %s", path, strerror(errno));
        fclose(fp);
        return;
    }

    uint8_t recbuf[PK_IDX_RECORD_LEN];
    for (size_t i = 0; i < s_idx.count; i++) {
        pk_idx_rec_encode(&s_idx.entries[i], recbuf);
        if (fwrite(recbuf, 1, PK_IDX_RECORD_LEN, fp) != PK_IDX_RECORD_LEN) {
            ESP_LOGW(TAG, "写 %s 第 %u 条摘要失败: %s", path, (unsigned)i, strerror(errno));
            break;
        }
    }
    fclose(fp);
    ESP_LOGI(TAG, "traffic.idx 写出：%u 架飞机", (unsigned)s_idx.count);
}

bool pk_rec_store_rebuild_index(const char *session_dir, const uint8_t own_icao[3],
                                pk_rec_idx_table_t *out)
{
    if (session_dir == NULL || out == NULL) return false;
    pk_rec_idx_reset(out);

    char path[80];
    int n = snprintf(path, sizeof(path), "%s/traffic.trk", session_dir);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return false;

    uint8_t hdrbuf[PK_REC_HEADER_LEN];
    if (fread(hdrbuf, 1, PK_REC_HEADER_LEN, fp) != PK_REC_HEADER_LEN) {
        fclose(fp);
        return false;
    }
    pk_rec_header_t hdr;
    if (!pk_rec_header_decode(hdrbuf, &hdr) || hdr.file_kind != PK_REC_FILE_KIND_TRAFFIC_TRK) {
        ESP_LOGW(TAG, "rebuild_index(%s): 文件头校验失败", path);
        fclose(fp);
        return false;
    }

    /* 分块读，不一次性把整个 traffic.trk 读进内存——32 条/块 = 1 KiB 栈上
     * 缓冲，够小，可以放心在调用方（自检任务）的栈上跑。 */
    #define REBUILD_CHUNK_RECORDS 32u
    uint8_t chunk[REBUILD_CHUNK_RECORDS * PK_TRK_RECORD_LEN];
    for (;;) {
        size_t got = fread(chunk, PK_TRK_RECORD_LEN, REBUILD_CHUNK_RECORDS, fp);
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) {
            const uint8_t *rec = &chunk[i * PK_TRK_RECORD_LEN];
            uint8_t rec_type = pk_trk_rec_type_peek(rec);
            bool is_own = (own_icao != NULL) && memcmp(&rec[8], own_icao, 3) == 0;
            if (rec_type == PK_TRK_REC_POSITION) {
                pk_trk_pos_t pos;
                pk_trk_pos_decode(rec, &pos);
                pk_rec_idx_ingest_pos(out, &pos, is_own);
            } else if (rec_type == PK_TRK_REC_IDENTITY) {
                pk_trk_id_t id;
                pk_trk_id_decode(rec, &id);
                pk_rec_idx_ingest_id(out, &id, is_own);
            }
        }
        if (got < REBUILD_CHUNK_RECORDS) break;   /* 读到文件尾 */
    }
    #undef REBUILD_CHUNK_RECORDS
    fclose(fp);
    return true;
}

void pk_rec_store_flush_all(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_fp_adsb)        fflush(s_fp_adsb);
    if (s_fp_traffic_trk) fflush(s_fp_traffic_trk);
    if (s_fp_own_trk)     fflush(s_fp_own_trk);
    if (s_fp_own_adsb)    fflush(s_fp_own_adsb);
    xSemaphoreGive(s_lock);
}

/* --- pre-unmount ------------------------------------------------------------ */

/* 注册进 pk_sdcard 的 pre-unmount 回调表（槽位已由 4 扩到 8，见 pk_sdcard.c）。
 *
 * 一度改成挂在 record_sink_file 的回调里转调以避开满槽，那是错的：那处注册是
 * `if (s_on_sdcard)` 有条件的，而日志后端默认是 flash——默认配置下转调根本不会
 * 发生，拔卡时本模块的 fd 一个都不关，正是 pk_sdcard.h 契约明令禁止的
 * use-after-free。
 *
 * 调用时机与锁语境见 pk_sdcard_register_pre_unmount_cb 的契约：sd_detect 任务
 * 上、卡已判定 NO_CARD、SD I/O 仍可用的窗口内。 */
void pk_rec_store_pre_unmount(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_session_open) {
        write_session_json_locked(true);
        write_traffic_idx_locked();
        if (s_fp_adsb)        { fclose(s_fp_adsb);        s_fp_adsb = NULL; }
        if (s_fp_traffic_trk) { fclose(s_fp_traffic_trk); s_fp_traffic_trk = NULL; }
        if (s_fp_own_trk)     { fclose(s_fp_own_trk);     s_fp_own_trk = NULL; }
        if (s_fp_own_adsb)    { fclose(s_fp_own_adsb);    s_fp_own_adsb = NULL; }
        s_session_open = false;
        ESP_LOGI(TAG, "拔卡前收尾：session %04u 已关闭", (unsigned)s_session_seq);
    }
    xSemaphoreGive(s_lock);
}

/* --- 后台任务：探测热插入、补建 session ------------------------------------- */

static void rec_store_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(REC_STORE_POLL_MS));
        if (!pk_sdcard_is_mounted()) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_session_open) {
            ensure_session_open_locked();
        }
        xSemaphoreGive(s_lock);
    }
}

void pk_rec_store_init(void)
{
    if (s_lock != NULL) return;   /* 幂等 */

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (pk_sdcard_is_mounted()) {
        ensure_session_open_locked();
    }
    xSemaphoreGive(s_lock);

    pk_sdcard_register_pre_unmount_cb(pk_rec_store_pre_unmount);

    BaseType_t ok = xTaskCreatePinnedToCore(rec_store_task, "rec_store",
                                            REC_STORE_TASK_STACK, NULL, 2, NULL, 0);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(rec_store) failed");
    }
}

void pk_rec_store_get_health(pk_rec_store_health_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) { out->tier = PK_REC_DEGRADE_FULL; return; }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* 无卡时不报"降级"——那一格另有 microSD 卡片说"无卡"，这里报 FULL
     * 只是"没有降级要说"，不是撒谎说"空间充裕"。 */
    out->tier = pk_sdcard_is_mounted() ? current_degrade_tier_locked()
                                       : PK_REC_DEGRADE_FULL;
    for (int i = 0; i < REC_SINK_COUNT; i++) {
        if (s_sink_disabled[i]) out->disabled_count++;
    }
    xSemaphoreGive(s_lock);
}

bool pk_rec_store_session_dir(char *out, size_t cap)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_session_open;
    if (ok) snprintf(out, cap, "%s", s_session_dir);
    xSemaphoreGive(s_lock);
    return ok;
}

/* --- 绑定机专属文件 ----------------------------------------------------------- */

void pk_rec_store_set_own_icao(const uint8_t icao24[3])
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000);

    if (icao24 == NULL) {
        if (s_own_bound) {
            if (s_fp_own_adsb) { fclose(s_fp_own_adsb); s_fp_own_adsb = NULL; }
            s_own_bound = false;
            /* session.json 的 own_icao_changes[]（设计文档「绑定机独立文件」
             * 节：改绑要记变更时刻）。定长数组满了就丢，不影响绑定本身。 */
            pk_rec_own_icao_changes_append(&s_icao_changes, now_ms, false, NULL);
            ESP_LOGI(TAG, "own_adsb.tsl：解绑");
        }
    } else if (!s_own_bound || memcmp(s_own_icao, icao24, 3) != 0) {
        if (s_fp_own_adsb) { fclose(s_fp_own_adsb); s_fp_own_adsb = NULL; }
        memcpy(s_own_icao, icao24, 3);
        s_own_bound = true;
        pk_rec_own_icao_changes_append(&s_icao_changes, now_ms, true, icao24);
        ESP_LOGI(TAG, "own_adsb.tsl：改绑 %02X%02X%02X",
                 icao24[0], icao24[1], icao24[2]);
    }

    xSemaphoreGive(s_lock);
}

/* --- append 接口（3b 的生产者调用） -------------------------------------------- */

bool pk_rec_store_append_adsb_line(const char *line, size_t len)
{
    if (s_lock == NULL || line == NULL || len == 0) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool ok = false;
    if (!s_sink_disabled[REC_SINK_ADSB] &&
        degrade_tier_check_locked() == PK_REC_DEGRADE_FULL) {
        if (ensure_adsb_volume_open_locked() &&
            fwrite(line, 1, len, s_fp_adsb) == len) {
            s_adsb_bytes += len;
            ok = true;
            s_counts.adsb_lines++;
            if (s_adsb_bytes >= ADSB_ROTATE_BYTES) {
                fclose(s_fp_adsb);
                s_fp_adsb = NULL;
                s_adsb_vol++;
                ensure_adsb_volume_open_locked();
            }
        }
        sink_write_result_locked(REC_SINK_ADSB, ok);
    }

    xSemaphoreGive(s_lock);
    return ok;
}

bool pk_rec_store_append_own_adsb_line(const char *line, size_t len)
{
    if (s_lock == NULL || line == NULL || len == 0) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool ok = false;
    if (s_own_bound && !s_sink_disabled[REC_SINK_OWN_ADSB] &&
        degrade_tier_check_locked() == PK_REC_DEGRADE_FULL &&
        ensure_session_open_locked()) {
        if (s_fp_own_adsb == NULL) {
            char path[64];
            snprintf(path, sizeof(path), "%s/own_adsb.tsl", s_session_dir);
            s_fp_own_adsb = fopen(path, "ab");
        }
        if (s_fp_own_adsb != NULL) {
            ok = fwrite(line, 1, len, s_fp_own_adsb) == len;
            if (ok) s_counts.own_adsb_lines++;
        }
        sink_write_result_locked(REC_SINK_OWN_ADSB, ok);
    }

    xSemaphoreGive(s_lock);
    return ok;
}

bool pk_rec_store_append_traffic_record(const uint8_t rec[32])
{
    if (s_lock == NULL || rec == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool ok = false;
    pk_rec_degrade_t tier = degrade_tier_check_locked();
    if (!s_sink_disabled[REC_SINK_TRAFFIC_TRK] && tier != PK_REC_DEGRADE_OWN_ONLY) {
        if (open_binary_with_header_locked(&s_fp_traffic_trk, "traffic.trk",
                                            PK_REC_MAGIC_TRAFFIC_TRK, PK_TRK_RECORD_LEN,
                                            PK_REC_FILE_KIND_TRAFFIC_TRK)) {
            ok = fwrite(rec, 1, PK_TRK_RECORD_LEN, s_fp_traffic_trk) == PK_TRK_RECORD_LEN;
            if (ok) {
                /* 按机摘要在线维护——traffic.idx 从这张内存表在 session 关闭时
                 * 整表写出（write_traffic_idx_locked），掉电场景靠
                 * pk_rec_store_rebuild_index() 从 traffic.trk 全扫恢复。 */
                uint8_t rec_type = pk_trk_rec_type_peek(rec);
                bool is_own = s_own_bound && memcmp(&rec[8], s_own_icao, 3) == 0;
                if (rec_type == PK_TRK_REC_POSITION) {
                    pk_trk_pos_t pos;
                    pk_trk_pos_decode(rec, &pos);
                    pk_rec_idx_ingest_pos(&s_idx, &pos, is_own);
                    s_counts.traffic_pos++;
                    /* session.json 的 bounds{}：本 session 见过的目标位置外
                     * 包框（设计文档「session.json」节，键名照抄客户端
                     * coverage_bounds）。地面目标也算——它照样是"见过的目
                     * 标"，外包框不区分空中/地面。 */
                    pk_rec_bounds_update(&s_bounds, pos.lat_e7, pos.lon_e7);
                } else if (rec_type == PK_TRK_REC_IDENTITY) {
                    pk_trk_id_t id;
                    pk_trk_id_decode(rec, &id);
                    pk_rec_idx_ingest_id(&s_idx, &id, is_own);
                    s_counts.traffic_id++;
                }
            }
        }
        sink_write_result_locked(REC_SINK_TRAFFIC_TRK, ok);
    }

    xSemaphoreGive(s_lock);
    return ok;
}

bool pk_rec_store_append_own_record(const uint8_t rec[48])
{
    if (s_lock == NULL || rec == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool ok = false;
    /* own.trk 是本机航迹本身，四档降级里唯一从不停写的一路
     * （设计文档「SD 满 / 写失败降级」表：<100MB 时"只保留 own.trk"）。
     * 正因为它从不因降级跳过，own.trk 的写入节奏（1 Hz）也是最稳定的
     * 降级/告警探测点——即使这一刻没有任何 ADS-B 报文在收，own.trk 的
     * 写入照样每秒跑一次，检测不会因为空域安静而停摆。 */
    if (!s_sink_disabled[REC_SINK_OWN_TRK]) {
        (void)degrade_tier_check_locked();   /* 只为触发降级告警，own.trk 本身不看这个档位 */
        if (open_binary_with_header_locked(&s_fp_own_trk, "own.trk",
                                            PK_REC_MAGIC_OWN_TRK, PK_OWN_RECORD_LEN,
                                            PK_REC_FILE_KIND_OWN_TRK)) {
            ok = fwrite(rec, 1, PK_OWN_RECORD_LEN, s_fp_own_trk) == PK_OWN_RECORD_LEN;
            if (ok) s_counts.own++;
        }
        sink_write_result_locked(REC_SINK_OWN_TRK, ok);
    }

    xSemaphoreGive(s_lock);
    return ok;
}
