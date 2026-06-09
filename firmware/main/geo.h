/* geo.h — great-circle distance (nm) + initial bearing (deg, 0=N,90=E).
 *
 * Extracted from adsb_list.c so the radar page / HSI traffic overlay /
 * list view can all share one Haversine implementation. */
#pragma once

void geo_dist_brg(double lat1_deg, double lon1_deg,
                  double lat2_deg, double lon2_deg,
                  double *out_dist_nm, double *out_brg_deg);
