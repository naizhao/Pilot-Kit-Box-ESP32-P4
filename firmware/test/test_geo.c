/* test_geo.c — host proof for geo_dist_brg.
 * Build & run:
 *   cc -std=c11 -O2 -I firmware/main -o /tmp/test_geo firmware/test/test_geo.c -lm && /tmp/test_geo
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include "../main/geo.c"          /* 纯函数，直接包含 */

static int g_fail = 0;
static void chk(const char *w, double got, double want, double tol){
    bool ok = fabs(got - want) <= tol;
    printf("  [%s] %-28s got=%9.3f want=%9.3f\n", ok?"PASS":"FAIL", w, got, want);
    if(!ok) g_fail++;
}
int main(void){
    double d, b;
    /* 正北 1°≈60nm，方位 0 */
    geo_dist_brg(0,0, 1,0, &d,&b);  chk("north dist",d,60.0,0.5); chk("north brg",b,0.0,0.5);
    /* 正东（赤道）方位 90 */
    geo_dist_brg(0,0, 0,1, &d,&b);  chk("east brg",b,90.0,0.5);
    /* 正南方位 180 */
    geo_dist_brg(0,0,-1,0, &d,&b);  chk("south brg",b,180.0,0.5);
    /* 正西方位 270 */
    geo_dist_brg(0,0, 0,-1,&d,&b);  chk("west brg",b,270.0,0.5);
    printf("%s (%d fail)\n", g_fail?"FAILED":"PASSED", g_fail);
    return g_fail?1:0;
}
