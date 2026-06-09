/* mag_var.c — 磁偏角网格表双线性插值。纯查表，host 可测。 */
#include "mag_var.h"

#include "mag_var_table.h"

static float cell(int i, int j){ return MAGVAR_TBL[i * MAGVAR_NLON + j] / 100.0f; }

float pk_mag_var_lookup(double lat_deg, double lon_deg)
{
    /* 纬度 clamp 到表范围 */
    double lat_hi = MAGVAR_LAT0 + (MAGVAR_NLAT - 1) * MAGVAR_STEP;
    if(lat_deg < MAGVAR_LAT0) lat_deg = MAGVAR_LAT0;
    if(lat_deg > lat_hi)      lat_deg = lat_hi;
    /* 经度 wrap 到 [LON0, LON0+360) */
    while(lon_deg <  MAGVAR_LON0)          lon_deg += 360.0;
    while(lon_deg >= MAGVAR_LON0 + 360.0)  lon_deg -= 360.0;

    double fi = (lat_deg - MAGVAR_LAT0) / MAGVAR_STEP;
    double fj = (lon_deg - MAGVAR_LON0) / MAGVAR_STEP;
    int i0 = (int)fi, j0 = (int)fj;
    int i1 = (i0 + 1 < MAGVAR_NLAT) ? i0 + 1 : i0;
    int j1 = (j0 + 1 < MAGVAR_NLON) ? j0 + 1 : j0;
    double ti = fi - i0, tj = fj - j0;

    float a = cell(i0, j0), b = cell(i0, j1);
    float c = cell(i1, j0), d = cell(i1, j1);
    float top = a + (b - a) * (float)tj;
    float bot = c + (d - c) * (float)tj;
    return top + (bot - top) * (float)ti;
}
