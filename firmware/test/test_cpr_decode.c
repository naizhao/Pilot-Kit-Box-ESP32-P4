/*
 * test_cpr_decode.c — host proof for cpr_decode.c（阶段 4b：地面 CPR 局部
 * 解码 + 全局配对表 surface/airborne parity 混配防护）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main -o /tmp/test_cpr \
 *      firmware/test/test_cpr_decode.c -lm && /tmp/test_cpr
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_cpr_asan \
 *      firmware/test/test_cpr_decode.c -lm && /tmp/test_cpr_asan
 *
 * 同 test_pk_rec_format.c / test_mode_s_surface.c 的翻译单元惯例：把被测
 * .c 直接 #include 进同一 TU，不单独编译链接——这样才能直接调用
 * cpr_decode.c 里的 static helper（cpr_nl / cpr_mod）来实现下面的"独立
 * 编码器"，不必对着被测代码的输出编造期望值。
 *
 * 期望值怎么来的：不用任何抄来的"标准测试报文"十六进制串（找不到就是编的），
 * 而是自己写一个按 DO-260B 公式的**编码器**（decode 的逆运算），把一个
 * 明确写死的经纬度编码成 17 位 CPR 原始值，再喂给被测的 decode 函数，
 * 断言解出来的经纬度跟编码前一致（round-trip）。这比对着 decode 的输出
 * "看起来差不多"更严格——编码器和解码器是各自独立实现的两段代码，只有
 * 数学关系一致才能通过。
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../main/cpr_decode.c"

static int g_fail = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  [FAIL] " __VA_ARGS__);                                 \
            printf("        at %s:%d\n", __FILE__, __LINE__);                \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(got, want, tol, label)                                    \
    CHECK(fabs((double)(got) - (double)(want)) <= (tol),                     \
          "%s got=%.6f want=%.6f (tol=%.6f)\n", (label), (double)(got),      \
          (double)(want), (double)(tol))

/* ---- 独立编码器（decode 的逆运算，公式取自本文件顶部注释里的 DO-260B
 * A.1.7.3），airborne 用 360 度制，surface 用 90 度制。两个都只依赖被测
 * .c 里已有的 static cpr_nl()/cpr_mod()，不碰任何"待测"的新增函数。 */

static void airborne_cpr_encode(double lat, double lon, int fflag,
                                int *out_lat_cpr, int *out_lon_cpr)
{
    double dlat = (fflag == 0) ? DLAT_EVEN : DLAT_ODD;
    double yz   = cpr_mod(lat, dlat) / dlat;
    *out_lat_cpr = (int)(floor(CPR_MAX * yz + 0.5)) % (int)CPR_MAX;

    int nl = cpr_nl(lat);
    int ni = nl - fflag;
    if (ni < 1) ni = 1;
    double dlon = 360.0 / ni;
    double xz   = cpr_mod(lon, dlon) / dlon;
    *out_lon_cpr = (int)(floor(CPR_MAX * xz + 0.5)) % (int)CPR_MAX;
}

static void surface_cpr_encode(double lat, double lon, int fflag,
                               int *out_lat_cpr, int *out_lon_cpr)
{
    double dlat = (fflag == 0) ? SURFACE_DLAT_EVEN : SURFACE_DLAT_ODD;
    double yz   = cpr_mod(lat, dlat) / dlat;
    *out_lat_cpr = (int)(floor(CPR_MAX * yz + 0.5)) % (int)CPR_MAX;

    int nl = cpr_nl(lat);
    int ni = nl - fflag;
    if (ni < 1) ni = 1;
    double dlon = 90.0 / ni;
    double xz   = cpr_mod(lon, dlon) / dlon;
    *out_lon_cpr = (int)(floor(CPR_MAX * xz + 0.5)) % (int)CPR_MAX;
}

/* ================================================================
 * 1) 局部（单帧）解码：round-trip 正确性。
 * ================================================================ */
static void test_surface_local_roundtrip(void)
{
    /* 参考位置：随手挑一个机场附近的点（旧金山湾区），目标就在附近滑行，
     * 明显落在 ±0.75°/45NM 有效半径内。even/odd 各测一次。 */
    const double ref_lat = 37.6188, ref_lon = -122.3750;
    const double true_lat = 37.6200, true_lon = -122.3730;

    int lat_cpr, lon_cpr;

    surface_cpr_encode(true_lat, true_lon, /*fflag=*/0, &lat_cpr, &lon_cpr);
    cpr_position_t pos = { .valid = false };
    bool ok = cpr_decode_surface_local(0, lat_cpr, lon_cpr, ref_lat, ref_lon, &pos);
    CHECK(ok, "surface local decode (even) should succeed\n");
    CHECK(pos.valid, "surface local decode (even) out_pos.valid\n");
    CHECK_NEAR(pos.lat, true_lat, 1e-4, "surface local even lat");
    CHECK_NEAR(pos.lon, true_lon, 1e-4, "surface local even lon");

    surface_cpr_encode(true_lat, true_lon, /*fflag=*/1, &lat_cpr, &lon_cpr);
    pos = (cpr_position_t){ .valid = false };
    ok = cpr_decode_surface_local(1, lat_cpr, lon_cpr, ref_lat, ref_lon, &pos);
    CHECK(ok, "surface local decode (odd) should succeed\n");
    CHECK(pos.valid, "surface local decode (odd) out_pos.valid\n");
    CHECK_NEAR(pos.lat, true_lat, 1e-4, "surface local odd lat");
    CHECK_NEAR(pos.lon, true_lon, 1e-4, "surface local odd lon");
}

/* ================================================================
 * 2) 超出 ±45 NM 有效半径必须拒绝，不能硬解出一个错的。
 *
 * 构造方式：局部解码算法按定义总是把纬度落在参考位置的 ±Dlat/2 内——
 * 这是算法本身的数学性质，不是拿来测的对象。真正会失守的是经度：Dlon
 * 由 NL(lat) 决定，纬度越靠近极点 NL 越小、Dlon 越大，Dlon/2 换算成实际
 * 距离可能远超 45 NM（NL=1 时 Dlon/2=45°经度，纬度 88° 处约 94 NM）。
 * 这正是「有效半径只是近似」的边界情况，也是这条拒绝判据真正要拦住的：
 * 参考位置在极区附近、经度差恰好落在该 CPR 分区边缘时，解码结果在数值上
 * "合法"但物理距离早已超出可信范围。
 *
 * 期望值手算：ref=(88.0, 0.0)，fflag=0，SURFACE_DLAT_EVEN=1.5。
 *   lat_cpr=0 (yz=0)   → j=floor(88/1.5)+floor(0.5+mod(88,1.5)/1.5-0)
 *                       = 58 + floor(0.5+0.6667) = 58+1 = 59
 *                       → rlat = 1.5*59 = 88.5（在 ref 的 ±0.75° 内）
 *   NL(88.5)=1 → ni=max(1-0,1)=1 → dlon=90
 *   lon_cpr=65536 (xz=0.5) → m=floor(0/90)+floor(0.5+0-0.5)=0
 *                       → rlon = 90*0.5 = 45.0
 *   great-circle(88.5,45.0 ; 88.0,0.0) ≈ 94 NM > 45 NM → 必须拒绝。
 * ================================================================ */
static void test_surface_local_rejects_out_of_range(void)
{
    cpr_position_t pos = { .valid = true, .lat = 999.0, .lon = 999.0 };
    bool ok = cpr_decode_surface_local(0, /*lat_cpr=*/0, /*lon_cpr=*/65536,
                                       /*ref_lat=*/88.0, /*ref_lon=*/0.0, &pos);
    CHECK(!ok, "out-of-range surface decode must fail\n");
    CHECK(!pos.valid, "out-of-range surface decode must clear out_pos.valid\n");

    /* Sanity check on the hand-derived math above, independent of the
     * function under test, so a future refactor that breaks the example
     * gets caught here too rather than only failing mysteriously. */
    double dist = great_circle_nm(88.5, 45.0, 88.0, 0.0);
    CHECK(dist > CPR_SURFACE_VALID_RADIUS_NM,
          "hand-derived example must itself exceed the 45 NM radius (got %.2f)\n",
          dist);
}

/* ================================================================
 * 3) 全局配对表：airborne round-trip 回归（确认没有把老路径改坏）。
 * ================================================================ */
static void test_global_airborne_roundtrip(void)
{
    const uint32_t icao = 0x100001;
    const double true_lat = 52.2572, true_lon = 3.91937;
    const int64_t t0 = 1000000;

    int lat_e, lon_e, lat_o, lon_o;
    airborne_cpr_encode(true_lat, true_lon, 0, &lat_e, &lon_e);
    airborne_cpr_encode(true_lat, true_lon, 1, &lat_o, &lon_o);

    cpr_init();
    cpr_position_t pos = { .valid = false };
    bool fresh = cpr_decode_position(icao, 0, /*is_surface=*/false, lat_e, lon_e, t0, &pos);
    CHECK(!fresh, "single even sample must not decode yet\n");
    CHECK(!pos.valid, "single even sample: out_pos.valid must stay false\n");

    fresh = cpr_decode_position(icao, 1, /*is_surface=*/false, lat_o, lon_o, t0 + 500000, &pos);
    CHECK(fresh, "matching odd sample must decode\n");
    CHECK(pos.valid, "matching odd sample: out_pos.valid must be true\n");
    CHECK_NEAR(pos.lat, true_lat, 1e-3, "global airborne lat");
    CHECK_NEAR(pos.lon, true_lon, 1e-3, "global airborne lon");
}

/* ================================================================
 * 4) 最危险的回归点：surface/airborne parity 绝不能混配。
 *
 *   even (surface) + odd (airborne) 的同一 ICAO 必须解码失败——即使两者
 *   时间差在 CPR_PAIR_MAX_AGE_US 之内、NL 恰好也agree，本该可以配对。
 *   随后送一个 is_surface 匹配的 odd 帧，证明表没有被永久卡死，只是那
 *   一次跨类型配对被正确拒绝了。
 * ================================================================ */
static void test_global_rejects_surface_airborne_mix(void)
{
    const uint32_t icao = 0x100002;
    const int64_t t0 = 2000000;

    int lat_e_surf, lon_e_surf, lat_o_surf, lon_o_surf, lat_o_air, lon_o_air;
    surface_cpr_encode(37.6200, -122.3730, 0, &lat_e_surf, &lon_e_surf);
    surface_cpr_encode(37.6200, -122.3730, 1, &lat_o_surf, &lon_o_surf);
    airborne_cpr_encode(37.6200, -122.3730, 1, &lat_o_air, &lon_o_air);

    cpr_init();
    cpr_position_t pos = { .valid = false };

    /* even, surface — stored, no pair yet. */
    bool fresh = cpr_decode_position(icao, 0, /*is_surface=*/true,
                                     lat_e_surf, lon_e_surf, t0, &pos);
    CHECK(!fresh, "single surface-even sample must not decode\n");

    /* odd, airborne — same ICAO, fresh within the pairing window, would
     * decode fine if parity-type checking were absent. Must be rejected. */
    fresh = cpr_decode_position(icao, 1, /*is_surface=*/false,
                                lat_o_air, lon_o_air, t0 + 1000000, &pos);
    CHECK(!fresh, "mismatched surface/airborne pair must NOT decode\n");
    CHECK(!pos.valid, "mismatched pair: out_pos.valid must stay false\n");

    /* odd, surface — now types match the cached even-surface sample.
     * Must succeed, proving the earlier rejection was specifically about
     * the type mismatch and didn't corrupt/wedge the slot.
     *
     * Note: cpr_decode_position()'s pairing math is airborne-scale
     * (DLAT_EVEN/DLAT_ODD, 360 deg range) unconditionally — it was never
     * meant to numerically decode surface-scale (90 deg range) samples;
     * production code always routes surface frames through
     * cpr_decode_surface_local() instead (see dsp_task.c). So this only
     * asserts that pairing SUCCEEDS once the types match again, not that
     * the resulting lat/lon is meaningful — feeding 90-degree-scale raw
     * values through 360-degree-scale math necessarily yields a
     * different (bogus) number, which is expected and out of scope
     * here; is_surface's only job in cpr_decode_position is to gate
     * *whether* a pair is attempted, not to pick the right math. */
    fresh = cpr_decode_position(icao, 1, /*is_surface=*/true,
                                lat_o_surf, lon_o_surf, t0 + 1500000, &pos);
    CHECK(fresh, "matching surface/surface pair must decode after the rejected attempt\n");
    CHECK(pos.valid, "matching surface pair: out_pos.valid must be true\n");
}

int main(void)
{
    test_surface_local_roundtrip();
    test_surface_local_rejects_out_of_range();
    test_global_airborne_roundtrip();
    test_global_rejects_surface_airborne_mix();

    if (g_fail) {
        printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    printf("OK: all cpr_decode checks passed\n");
    return 0;
}
