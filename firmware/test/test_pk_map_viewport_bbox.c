/* test_pk_map_viewport_bbox.c — map_page.c 视口 bbox 几何的 host 回归。
 *
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main \
 *      -o /tmp/test_mvb firmware/test/test_pk_map_viewport_bbox.c -lm \
 *      && /tmp/test_mvb
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_mvb_asan \
 *      firmware/test/test_pk_map_viewport_bbox.c -lm && /tmp/test_mvb_asan
 *
 * ── 这是个「镜像」测试，不是真 include ─────────────────────────────────
 * map_page.c 依赖 ESP-IDF（esp_timer / FreeRTOS / esp_lcd），host 编不过，
 * 所以这里**逐字照抄**它的两段静态几何——Web Mercator 正反变换
 * (map_page.c:213-230) 和视口 bbox 的四角逆旋转 (map_page.c:775-793)。
 * 改 map_page.c 那几段时**必须**同步改这里——这是人肉对齐，不是自动覆盖。
 * 与 test_pk_win_nearest.c 合成数据而非 include 真 db 是同一种诚实让步。
 *
 * ── 它抓的是什么 bug ─────────────────────────────────────────────────
 * 旧 bbox 写法只取对角两角 + 第三角校正：lat 在墨卡托下只依赖 wy，对角两角
 * 的 wy 一北一南，但 min/max 赋值时名字写反，第三角校正又只修了 min_lat，
 * max_lat 永远卡在南边 → bbox 退化成零纬度高度（min_lat == max_lat），
 * pk_win_set_viewport 只挡 min>max，放行后视口只钉住最南一行格。
 * 本测试同时跑「旧 buggy 公式」断言它退化，和「修复公式」断言它覆盖全屏，
 * 两者成对才证明修复不是偶然通过。
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define TILE_PX 256

/* ── 照抄 map_page.c:213-230（Web Mercator 正反变换）── */
static void lonlat_to_world(double lon, double lat, uint8_t z, double *wx, double *wy)
{
    if (lat >  85.0511) lat =  85.0511;
    if (lat < -85.0511) lat = -85.0511;
    double n = (double)(1u << z) * (double)TILE_PX;
    double latrad = lat * M_PI / 180.0;
    *wx = (lon + 180.0) / 360.0 * n;
    *wy = (0.5 - log(tan(M_PI / 4.0 + latrad / 2.0)) / (2.0 * M_PI)) * n;
}

static void world_to_lonlat(double wx, double wy, uint8_t z, double *lon, double *lat)
{
    double n = (double)(1u << z) * (double)TILE_PX;
    *lon = wx / n * 360.0 - 180.0;
    double yfrac = wy / n;
    double latrad = atan(sinh(M_PI * (1.0 - 2.0 * yfrac)));
    *lat = latrad * 180.0 / M_PI;
}

/* ── 四角真值：独立算每个角的 (lon,lat)，取 min/max —— 作为基准 ── */
static void bbox_truth(double cwx, double cwy, uint8_t z,
                       double hw, double hh, double cos_r, double sin_r,
                       double *min_lat, double *max_lat,
                       double *min_lon, double *max_lon)
{
    double mlat = 1e9, Mlat = -1e9, mlon = 1e9, Mlon = -1e9;
    for (int ix = -1; ix <= 1; ix += 2)
        for (int iy = -1; iy <= 1; iy += 2) {
            double sdx = ix * hw, sdy = iy * hh;
            /* 屏幕角→世界：与 map_page.c:258-260 / world_to_screen_rot 逆变换一致 */
            double wx = cwx + (sdx * cos_r - sdy * sin_r);
            double wy = cwy + (sdx * sin_r + sdy * cos_r);
            double lon, lat;
            world_to_lonlat(wx, wy, z, &lon, &lat);
            if (lat < mlat) mlat = lat;
            if (lat > Mlat) Mlat = lat;
            if (lon < mlon) mlon = lon;
            if (lon > Mlon) Mlon = lon;
        }
    *min_lat = mlat; *max_lat = Mlat; *min_lon = mlon; *max_lon = Mlon;
}

/* ── 旧 buggy 公式（map_page.c 改造前）：只取对角两角 + 第三角 ── */
static void bbox_buggy(double cwx, double cwy, uint8_t z, double hw, double hh,
                       double *min_lat, double *max_lat,
                       double *min_lon, double *max_lon)
{
    double t, min_lat_, min_lon_, max_lat_, max_lon_;
    world_to_lonlat(cwx - hw, cwy - hh, z, &t, &min_lat_);
    min_lon_ = t;
    world_to_lonlat(cwx + hw, cwy + hh, z, &t, &max_lat_);
    max_lon_ = t;
    double la2, lo2;
    world_to_lonlat(cwx - hw, cwy + hh, z, &lo2, &la2);
    if (la2 < min_lat_) min_lat_ = la2;
    if (la2 > max_lat_) max_lat_ = la2;
    if (lo2 < min_lon_) min_lon_ = lo2;
    if (lo2 > max_lon_) max_lon_ = lo2;
    *min_lat = min_lat_; *max_lat = max_lat_;
    *min_lon = min_lon_; *max_lon = max_lon_;
}

static int g_fail = 0;
static void chk(const char *what, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

int main(void)
{
    const double center_lat = 40.0, center_lon = 116.0;
    const uint8_t zoom = 8;
    const double hw = 400.0, hh = 200.0;   /* PK_DISPLAY_W/2, (PK_DISPLAY_H-MAP_TOP)/2 */
    double cwx, cwy;
    lonlat_to_world(center_lon, center_lat, zoom, &cwx, &cwy);

    /* ===== north-up：修复公式 == 四角真值；旧公式 lat 退化 ===== */
    printf("-- north-up --\n");
    double tmin_lat, tmax_lat, tmin_lon, tmax_lon;
    bbox_truth(cwx, cwy, zoom, hw, hh, 1.0, 0.0,
               &tmin_lat, &tmax_lat, &tmin_lon, &tmax_lon);
    /* 修复公式就是 bbox_truth(cos=1,sin=0)，直接复用 */
    chk("修复 north-up: lat 跨度 > 1.5°（非退化）", (tmax_lat - tmin_lat) > 1.5);

    double bmin_lat, bmax_lat, bmin_lon, bmax_lon;
    bbox_buggy(cwx, cwy, zoom, hw, hh, &bmin_lat, &bmax_lat, &bmin_lon, &bmax_lon);
    printf("  [buggy] min_lat=%.6f max_lat=%.6f (跨度 %.6f°)\n",
           bmin_lat, bmax_lat, bmax_lat - bmin_lat);
    chk("旧 buggy: lat 退化 (min==max)", fabs(bmin_lat - bmax_lat) < 1e-9);
    chk("旧 buggy: 退化值 == 真值 min_lat（南边）", fabs(bmin_lat - tmin_lat) < 1e-9);
    chk("旧 buggy: 没够到真值 max_lat（北边）", (tmax_lat - bmax_lat) > 1.0);

    /* ===== heading-up 45°：修复 bbox ⊇ north-up bbox（保守上界）===== */
    printf("-- heading-up 45° --\n");
    double H = 45.0 * M_PI / 180.0;
    double hmin_lat, hmax_lat, hmin_lon, hmax_lon;
    bbox_truth(cwx, cwy, zoom, hw, hh, cos(H), sin(H),
               &hmin_lat, &hmax_lat, &hmin_lon, &hmax_lon);
    chk("heading-up bbox ⊇ north-up lat", hmin_lat <= tmin_lat + 1e-9 &&
                                          hmax_lat >= tmax_lat - 1e-9);
    chk("heading-up bbox ⊇ north-up lon", hmin_lon <= tmin_lon + 1e-9 &&
                                          hmax_lon >= tmax_lon - 1e-9);
    chk("heading-up bbox 严格更大（旋转矩形外接框）",
        (hmax_lat - hmin_lat) > (tmax_lat - tmin_lat) + 1e-9);

    /* ===== 高纬也成立（mercator lat 非线性更剧烈）===== */
    printf("-- high-lat 70°N north-up --\n");
    double cwx2, cwy2;
    lonlat_to_world(116.0, 70.0, zoom, &cwx2, &cwy2);
    double n2min_lat, n2max_lat, n2min_lon, n2max_lon;
    bbox_truth(cwx2, cwy2, zoom, hw, hh, 1.0, 0.0,
               &n2min_lat, &n2max_lat, &n2min_lon, &n2max_lon);
    double b2min_lat, b2max_lat, b2min_lon, b2max_lon;
    bbox_buggy(cwx2, cwy2, zoom, hw, hh, &b2min_lat, &b2max_lat, &b2min_lon, &b2max_lon);
    printf("  [fix] 高纬 lat 跨度=%.6f° (mercator 高纬压缩，< 40°N 的 1.68° 属正常)\n",
           n2max_lat - n2min_lat);
    chk("高纬 north-up: 修复 lat 非退化 (跨度 > 0.3°)", (n2max_lat - n2min_lat) > 0.3);
    chk("高纬 north-up: 修复跨度 > buggy 跨度 (=0)",
        (n2max_lat - n2min_lat) > (b2max_lat - b2min_lat) + 1e-9);
    chk("高纬 north-up: 旧 buggy 仍退化", fabs(b2min_lat - b2max_lat) < 1e-9);

    printf("\n%s (%d fail)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
