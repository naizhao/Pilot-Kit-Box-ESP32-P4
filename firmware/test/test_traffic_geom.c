/* test_traffic_geom.c — host proof for pk_traffic_rel_calc.
 *   cc -std=c11 -O2 -I firmware/main -o /tmp/test_tg firmware/test/test_traffic_geom.c -lm && /tmp/test_tg
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include "../main/geo.c"
#include "../main/traffic_geom.c"

static int g_fail = 0;
static void chk(const char *w, float got, float want, float tol){
    bool ok = fabsf(got - want) <= tol;
    printf("  [%s] %-32s got=%8.2f want=%8.2f\n", ok?"PASS":"FAIL", w, got, want);
    if(!ok) g_fail++;
}
int main(void){
    pk_traffic_rel_t r;
    /* 本机赤道原点朝北(hdg0,var0)，目标正北 → rel 0, abs 0, dist 60, relalt +1000 */
    r = pk_traffic_rel_calc(true,0,0, 0,0,1000, true,1,0, true,2000,0);
    chk("N rel",r.rel_bearing,0,0.5); chk("N abs",r.abs_bearing,0,0.5);
    chk("dist60",r.dist_nm,60,0.5);   chk("relalt+1000",r.rel_alt_ft,1000,0.5);
    /* 朝北，目标正东 → rel +90 */
    r = pk_traffic_rel_calc(true,0,0, 0,0,1000, true,0,1, true,0,0);
    chk("E rel+90",r.rel_bearing,90,0.5);
    /* 机头朝东(hdg90)，目标正北 → rel -90 (在我左) */
    r = pk_traffic_rel_calc(true,0,0, 90,0,1000, true,1,0, true,0,0);
    chk("hdg90 N rel-90",r.rel_bearing,-90,0.5);
    /* 磁偏角 +6(东偏)：目标真北方位0 → 磁方位 -6→354 */
    r = pk_traffic_rel_calc(true,0,0, 0,6,1000, true,1,0, true,0,0);
    chk("magvar abs354",r.abs_bearing,354,0.5);
    /* 本机无气压高度 → rel_alt_valid false */
    r = pk_traffic_rel_calc(true,0,0, 0,0,PK_ALT_UNAVAIL, true,1,0, true,5000,0);
    chk("relalt invalid",r.rel_alt_valid?1:0,0,0);
    /* 目标无位置 → valid false */
    r = pk_traffic_rel_calc(true,0,0, 0,0,1000, false,1,0, true,0,0);
    chk("tgt nopos invalid",r.valid?1:0,0,0);

    /* ── 目标剪影朝向 ───────────────────────────────────────────────
     *
     * 这两条断言就是防撞语义本身，不是几何练习：机头朝上模式下同向飞的必须
     * 朝上、迎面来的必须朝下。此前 traffic_page.c 写的是
     *   rot = heading_deg - (screen - rel_bearing)
     * 而那个分支里 screen 恒等于 rel_bearing，减法恒为 0，两种朝向都只用了
     * 目标航迹本身——迎面与同向画出来完全一样。
     *
     * 本机航向按调用方的口径给：IMU 磁航向 = 真航向 - 磁偏角，与传给
     * pk_traffic_rel_calc 的那一对参数保持同一参考系。 */
    {
        const float var = -7.4f;            /* 北京一带，西偏 */
        const float own_true = 82.0f;
        const float own_map  = own_true - var;   /* 本机在地图参考系里的航向 */

        /* 同向：目标真航迹 == 本机真航向 → 机头朝屏幕正上方 */
        chk("hdgup same-track rot0",
            pk_traffic_symbol_rot_deg(true, own_true, var, own_map), 0, 0.01f);
        /* 迎面：差 180° → 机头朝屏幕正下方。这一条画错就等于把「他冲我来」
         * 显示成「他背我去」。 */
        chk("hdgup head-on rot180",
            pk_traffic_symbol_rot_deg(true, own_true + 180.0f, var, own_map),
            180, 0.01f);
        /* 正北朝上：与本机航向无关，只把真航迹降到地图参考北。 */
        chk("northup track175",
            pk_traffic_symbol_rot_deg(false, 175.0f, var, own_map),
            175.0f - var, 0.01f);
        /* 归一化到 0..360，负角与超 360 都不许漏出去（旋转本身不在乎，但
         * 断言和日志在乎）。 */
        chk("northup wrap",
            pk_traffic_symbol_rot_deg(false, 5.0f, 10.0f, 0.0f), 355, 0.01f);
        /* 磁偏角为 0（ADS-B / GPS 真航迹口径）时退化成纯减本机航向。 */
        chk("hdgup magvar0",
            pk_traffic_symbol_rot_deg(true, 90.0f, 0.0f, 30.0f), 60, 0.01f);
    }

    printf("%s (%d fail)\n", g_fail?"FAILED":"PASSED", g_fail);
    return g_fail?1:0;
}
