/* test_mag_var.c — host proof for pk_mag_var_lookup.
 *   cc -std=c11 -O2 -I firmware/main -o /tmp/test_mv firmware/test/test_mag_var.c -lm && /tmp/test_mv
 *
 * want 值取自 gen_mag_var.py 打印的网格点 geomag 真值；网格点上查表只受
 * int16 量化(0.005°)影响，故 tol 收紧到 0.02°。 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include "../main/mag_var.c"

static int g_fail = 0;
static void chk(const char *w, float got, float want, float tol){
    bool ok = fabsf(got - want) <= tol;
    printf("  [%s] %-16s got=%7.2f want=%7.2f\n", ok?"PASS":"FAIL", w, got, want);
    if(!ok) g_fail++;
}
int main(void){
    /* 网格点（lat/lon 均 5 的倍数）→ 精确命中 */
    chk("(40,115)",   pk_mag_var_lookup(40, 115),  -6.96, 0.02);
    chk("(30,120)",   pk_mag_var_lookup(30, 120),  -6.05, 0.02);
    chk("(35,-120)",  pk_mag_var_lookup(35, -120), 11.75, 0.02);
    chk("(0,0)",      pk_mag_var_lookup(0, 0),     -4.04, 0.02);
    chk("(50,10)",    pk_mag_var_lookup(50, 10),    3.83, 0.02);
    chk("(-35,150)",  pk_mag_var_lookup(-35, 150), 12.68, 0.02);
    /* 插值点：(30,120)=-6.05 与 (35,120)=? 之间，结果应有限且在邻域内 */
    float mid = pk_mag_var_lookup(32.5, 120);
    bool mid_ok = (mid > -10.0f && mid < 0.0f);
    printf("  [%s] interp(32.5,120)     got=%7.2f (邻域内)\n", mid_ok?"PASS":"FAIL", mid);
    if(!mid_ok) g_fail++;
    /* 经度 wrap：185° 应等价 -175° */
    chk("lon wrap 185", pk_mag_var_lookup(40, 185), pk_mag_var_lookup(40, -175), 0.01);
    /* 纬度越界 clamp：95° → 90° */
    chk("lat clamp 95", pk_mag_var_lookup(95, 0), pk_mag_var_lookup(90, 0), 0.01);
    printf("%s (%d fail)\n", g_fail?"FAILED":"PASSED", g_fail);
    return g_fail?1:0;
}
