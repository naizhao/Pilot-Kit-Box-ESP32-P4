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
    printf("%s (%d fail)\n", g_fail?"FAILED":"PASSED", g_fail);
    return g_fail?1:0;
}
