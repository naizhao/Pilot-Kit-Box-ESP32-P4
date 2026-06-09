/* geo.c — great-circle distance + initial bearing. Pure math, no ESP-IDF
 * dependency, so it is host-testable with plain `cc`. */
#include "geo.h"

#include <math.h>

#define EARTH_RADIUS_NM 3440.065
#define DEG2RAD(x) ((x) * (M_PI / 180.0))
#define RAD2DEG(x) ((x) * (180.0 / M_PI))

void geo_dist_brg(double lat1_deg, double lon1_deg,
                  double lat2_deg, double lon2_deg,
                  double *out_dist_nm, double *out_brg_deg)
{
    double phi1 = DEG2RAD(lat1_deg);
    double phi2 = DEG2RAD(lat2_deg);
    double dphi = DEG2RAD(lat2_deg - lat1_deg);
    double dlam = DEG2RAD(lon2_deg - lon1_deg);

    double a = sin(dphi * 0.5) * sin(dphi * 0.5)
             + cos(phi1) * cos(phi2) * sin(dlam * 0.5) * sin(dlam * 0.5);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    if (out_dist_nm) *out_dist_nm = EARTH_RADIUS_NM * c;

    if (out_brg_deg) {
        double y = sin(dlam) * cos(phi2);
        double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlam);
        double brg = RAD2DEG(atan2(y, x));
        if (brg < 0) brg += 360.0;
        *out_brg_deg = brg;
    }
}
