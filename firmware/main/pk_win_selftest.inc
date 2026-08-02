/*
 * pk_win_selftest.inc — 窗口机制的**真机自检**，由 pk_win.c 在
 * `#if PK_WIN_SELFTEST` 下 include。默认关闭，只在验收时打开。
 *
 * 不是单元测试，是**取证**：它回答三个问题，每个都用日志给出可复核的数字。
 *
 *   Q1 区间读 + AES-CTR 随机解密对不对？
 *      → verify_against_full_db()：把窗口里读出来的机场记录逐条与
 *        pk_aero_db（全量加载路径）的 pk_aero_db_airport_get(idx) 对拍
 *        lat_e7/lon_e7/icao/grid_cell。两条路径的**唯一共同点是同一个
 *        文件**，一条整读一条按格区间读+随机解密，逐字段相同就说明
 *        块号算术、偏移、段表解析全对。这比任何 host 单测都硬。
 *
 *   Q2 三档密度的内存账是多少？对得上文档 §2.1 的预测吗？
 *      → 依次把窗口中心喂到内蒙稀疏 / 北京典型 / 珠三角密集，
 *        每档跑够时间让窗口填满，打印驻留字节 / 条数 / 读盘量 / PSRAM 余量。
 *
 *   Q3 让路规则生效了吗？
 *      → 每档打印 yields / forced 计数；拖地图时 yields 应当涨。
 *
 * 自检会强行改写窗口中心（pk_win_debug_override），与真实本机位置无关，
 * 所以**绝不能默认开着出厂**。
 */

/* 三档采样点。经纬度取文档 §2.1 实测表里的同一批坐标，好直接对账。 */
typedef struct {
    const char *name;
    double lat, lon, track;
    uint32_t doc_bytes;     /* 文档 §2.1 的预测值（v4 全量口径，含空域/航路） */
} win_probe_t;

static const win_probe_t s_probes[] = {
    { "稀疏/内蒙锡林浩特", 43.9,  115.9, 90.0,  5102 },
    { "典型/华北北京",     40.1,  116.6, 90.0, 23400 },
    { "密集/珠三角广州",   23.2,  113.5, 90.0, 44184 },
};
#define WIN_PROBE_N ((int)(sizeof(s_probes) / sizeof(s_probes[0])))

/* 每档给多少个 tick 让窗口填满：boot_fill 只在窗口全空时成立，之后是
 * 每 tick 一个格，所以要 ≥ 最大格数（15）+ 余量。 */
#define WIN_PROBE_TICKS   24

static int      s_probe_i = -1;
static int      s_probe_tick;
static uint32_t s_probe_bytes_read0;
static uint32_t s_probe_yields0, s_probe_forced0;

/* 把窗口里的机场记录与全量路径逐字段对拍。返回 (对拍条数, 不一致条数)。 */
static void verify_against_full_db(int *out_checked, int *out_bad)
{
    int checked = 0, bad = 0;

    if (pk_aero_db_state() != PK_AERO_DB_READY) {
        ESP_LOGW(TAG, "selftest: pk_aero_db 未 READY，跳过对拍");
        *out_checked = *out_bad = -1;
        return;
    }

    pk_win_cellset_t res;
    pk_win_lock();
    pk_win_resident_cells(&res);
    pk_win_unlock();

    for (int i = 0; i < (int)res.n && checked < 200; i++) {
        /* 记录拷进栈上小缓冲再放锁：pk_aero_db 的查询自己也要拿它那把锁，
         * 两把锁嵌套持有是自找死锁。 */
        uint8_t  rec[PK_AERO_AIRPORT_SIZE];
        uint32_t first = 0, n = 0;
        for (uint32_t k = 0; k < 8 && checked < 200; k++) {
            const uint8_t *recs = NULL;
            pk_win_lock();
            bool ok = pk_win_cell_records(res.cell[i], PK_AERO_SEC_AIRPORTS,
                                          &recs, &n, &first);
            if (ok && k < n) memcpy(rec, recs + k * PK_AERO_AIRPORT_SIZE,
                                    sizeof(rec));
            else ok = false;
            pk_win_unlock();
            if (!ok) break;

            pk_aero_airport_t a;
            if (!pk_aero_db_airport_get(first + k, &a)) { bad++; continue; }

            const int32_t lat_e7 = (int32_t)rd_u32(rec);
            const int32_t lon_e7 = (int32_t)rd_u32(rec + 4);
            const uint16_t gc    = rd_u16(rec + 37);
            char icao[5] = {0};
            memcpy(icao, rec + 8, 4);
            for (int z = 3; z >= 0 && (icao[z] == ' ' || icao[z] == '\0'); z--)
                icao[z] = '\0';

            checked++;
            if (lat_e7 != a.lat_e7 || lon_e7 != a.lon_e7 ||
                gc != a.grid_cell || strcmp(icao, a.icao) != 0) {
                bad++;
                if (bad <= 3) {
                    ESP_LOGE(TAG, "selftest MISMATCH idx=%lu win(%ld,%ld,%s,c%u) "
                                  "db(%ld,%ld,%s,c%u)",
                             (unsigned long)(first + k),
                             (long)lat_e7, (long)lon_e7, icao, (unsigned)gc,
                             (long)a.lat_e7, (long)a.lon_e7, a.icao,
                             (unsigned)a.grid_cell);
                }
            }
        }
    }
    *out_checked = checked;
    *out_bad     = bad;
}

/* 每个 tick 调一次（在 advance() 之前）。 */
static void win_selftest_step(void)
{
    if (s_probe_i < 0) {
        s_probe_i = 0;
        s_probe_tick = 0;
        ESP_LOGW(TAG, "==== pk_win SELFTEST 开始（窗口中心被强制覆盖）====");
    }
    if (s_probe_i >= WIN_PROBE_N) return;

    const win_probe_t *p = &s_probes[s_probe_i];

    if (s_probe_tick == 0) {
        /* 切档：清空窗口，从零开始计这一档的读盘量与让路次数 */
        pk_win_lock();
        slots_clear_all();
        pk_win_unlock();
        pk_aero_span_stats_reset();
        s_probe_bytes_read0 = 0;
        s_probe_yields0 = s_st.yields;
        s_probe_forced0 = s_st.forced;
        pk_win_debug_override(true, p->lat, p->lon, p->track);
        ESP_LOGW(TAG, "---- probe[%d] %s (%.2f, %.2f) trk=%.0f ----",
                 s_probe_i, p->name, p->lat, p->lon, p->track);
    }

    s_probe_tick++;

    if (s_probe_tick >= WIN_PROBE_TICKS) {
        int checked = 0, bad = 0;
        verify_against_full_db(&checked, &bad);

        ESP_LOGW(TAG,
                 "probe[%d] %s: 窗口格=%u 驻留格=%u 字节=%lu "
                 "(apt=%lu nav=%lu fix=%lu) 读盘=%llu B 最近整格耗时=%lu us "
                 "让路=%lu 强读=%lu 名称片段放弃=%lu PSRAM free=%lu 最大块=%lu "
                 "| 文档预测(v4口径)=%lu B | 对拍 %d 条 %d 处不一致 [%s]",
                 s_probe_i, p->name,
                 (unsigned)s_st.win_cells, (unsigned)s_st.n_ready,
                 (unsigned long)s_st.bytes,
                 (unsigned long)s_st.n_apt, (unsigned long)s_st.n_nav,
                 (unsigned long)s_st.n_fix,
                 (unsigned long long)pk_aero_span_bytes_read(),
                 (unsigned long)s_st.last_load_us,
                 (unsigned long)(s_st.yields - s_probe_yields0),
                 (unsigned long)(s_st.forced - s_probe_forced0),
                 (unsigned long)s_st.str_skipped,
                 (unsigned long)s_st.psram_free,
                 (unsigned long)s_st.psram_largest,
                 (unsigned long)p->doc_bytes,
                 checked, bad,
                 (checked > 0 && bad == 0) ? "PASS" : "CHECK");

        s_probe_i++;
        s_probe_tick = 0;
        if (s_probe_i >= WIN_PROBE_N) {
            pk_win_debug_override(false, 0, 0, 0);
            ESP_LOGW(TAG, "==== pk_win SELFTEST 结束，中心交还给本机位置 ====");
        }
    }
    (void)s_probe_bytes_read0;
}
