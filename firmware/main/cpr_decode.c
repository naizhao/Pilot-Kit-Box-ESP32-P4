/*
 * cpr_decode.c — implementation of the CPR global decoder.
 *
 * Reference: ICAO Annex 10 Vol IV / RTCA DO-260B Appendix A.1.7.
 *   Even / odd grid sizes:
 *     Nz = 15
 *     Dlat_even = 360 / (4*Nz)     = 6.000000
 *     Dlat_odd  = 360 / (4*Nz - 1) = 6.101694915...
 *
 *   Latitude index:
 *     j     = floor(59*YZ_even/2^17 - 60*YZ_odd/2^17 + 0.5)
 *     R_lat_even = Dlat_even * (mod(j, 60) + YZ_even/2^17)
 *     R_lat_odd  = Dlat_odd  * (mod(j, 59) + YZ_odd /2^17)
 *
 *   Longitude index, given which sample is the more recent (T):
 *     m       = floor(XZ_even*(NL(R_lat_T)-1)/2^17
 *                     - XZ_odd*NL(R_lat_T)/2^17 + 0.5)
 *     ni      = max(NL(R_lat_T) - T, 1)     ; T = fflag of the recent
 *     Dlon    = 360 / ni
 *     R_lon   = Dlon * (mod(m, ni) + XZ_T/2^17)
 *
 *   The latitudes used to seed NL must agree on the same zone
 *   (NL(R_lat_even) == NL(R_lat_odd)), otherwise the decode is
 *   ambiguous; we drop those frames per the standard.
 *
 * Surface (local/relative) decoding — DO-260B Appendix A.1.7.3:
 *   Surface CPR uses the SAME 17-bit encoding as airborne but scaled by
 *   1/4: the encoded value spans 90 deg of latitude/longitude instead
 *   of 360 deg (a ground target can't be more than ~1/4 turn of the
 *   globe away from any receiver that could possibly hear it, so the
 *   spec buys 2 extra bits of resolution by shrinking the coded range).
 *   That gives:
 *     Dlat_even = 90 / (4*Nz)     = 1.500000
 *     Dlat_odd  = 90 / (4*Nz - 1) = 1.525423729...
 *
 *   Given a reference position (rlat0, rlon0) already known to be
 *   within Dlat/2 of the true position, a SINGLE frame decodes
 *   unambiguously:
 *     j    = floor(rlat0/Dlat) + floor(0.5 + mod(rlat0,Dlat)/Dlat - YZ)
 *     rlat = Dlat * (j + YZ)
 *     ni   = max(NL(rlat) - T, 1)          ; T = fflag of this frame
 *     Dlon = 90 / ni
 *     m    = floor(rlon0/Dlon) + floor(0.5 + mod(rlon0,Dlon)/Dlon - XZ)
 *     rlon = Dlon * (m + XZ)
 *   This is the textbook "local CPR decode" (dump1090's
 *   decodeCPRrelative()) with the surface 90-degree scale substituted
 *   for the airborne 360-degree one.
 */

#include "cpr_decode.h"

#include <math.h>
#include <string.h>

#define NZ          15
#define DLAT_EVEN   (360.0 / (4.0 * NZ))
#define DLAT_ODD    (360.0 / (4.0 * NZ - 1.0))
#define SURFACE_DLAT_EVEN (DLAT_EVEN / 4.0)   /* 90 / (4*Nz)     */
#define SURFACE_DLAT_ODD  (DLAT_ODD  / 4.0)   /* 90 / (4*Nz - 1) */
#define CPR_MAX     131072.0   /* 2^17 */
#define EARTH_RADIUS_NM 3440.065

typedef struct {
    uint32_t icao24;        /* 0 marks an empty slot */

    bool     have_even;
    bool     is_surface_even;  /* parity type of the even sample below */
    int      lat_cpr_even;
    int      lon_cpr_even;
    int64_t  t_even_us;

    bool     have_odd;
    bool     is_surface_odd;   /* parity type of the odd sample below */
    int      lat_cpr_odd;
    int      lon_cpr_odd;
    int64_t  t_odd_us;

    cpr_position_t pos;
    int64_t  last_seen_us;   /* used as poor-man's LRU on collision */
} cpr_slot_t;

static cpr_slot_t s_table[CPR_TABLE_CAPACITY];

/*
 * NL(lat): number of longitude zones at the given latitude.
 *
 * The closed-form expression below comes straight out of DO-260B and
 * agrees with the canonical 59-entry table for every input we care
 * about. Using the formula keeps us free of a ~1 KiB lookup table and,
 * since the P4 has hardware double-precision FPU, the cost is roughly
 * a single cos + acos per ADS-B position frame — well under a
 * microsecond on a 400 MHz core.
 */
static int cpr_nl(double lat)
{
    if (lat < 0.0) lat = -lat;
    if (lat < 10.47047130) return 59;
    if (lat >= 87.0)       return 1;

    /* Closed-form NL. The argument to acos is bounded in [-1, 1] for
     * any |lat| <= 87, so no domain clamping is required. */
    double cos_lat = cos(lat * M_PI / 180.0);
    double tmp     = 1.0 - (1.0 - cos(M_PI / (2.0 * NZ))) / (cos_lat * cos_lat);
    return (int)floor(2.0 * M_PI / acos(tmp));
}

/* Mathematical (Euclidean) modulus, always returning a non-negative value. */
static double cpr_mod(double a, double b)
{
    double r = fmod(a, b);
    return (r < 0.0) ? r + b : r;
}

void cpr_init(void)
{
    memset(s_table, 0, sizeof(s_table));
}

/* Locate an existing slot for icao or claim a free slot via linear probing.
 * On full-table collision, evicts the LRU slot in the probe sequence. */
static cpr_slot_t *cpr_lookup_or_claim(uint32_t icao24)
{
    const uint32_t base = icao24 % CPR_TABLE_CAPACITY;

    cpr_slot_t *empty = NULL;
    cpr_slot_t *lru   = &s_table[base];

    for (uint32_t step = 0; step < CPR_TABLE_CAPACITY; ++step) {
        cpr_slot_t *s = &s_table[(base + step) % CPR_TABLE_CAPACITY];
        if (s->icao24 == icao24) return s;
        if (!empty && s->icao24 == 0) empty = s;
        if (s->last_seen_us < lru->last_seen_us) lru = s;
    }

    cpr_slot_t *chosen = empty ? empty : lru;
    memset(chosen, 0, sizeof(*chosen));
    chosen->icao24 = icao24;
    return chosen;
}

bool cpr_decode_position(uint32_t icao24,
                         int      fflag,
                         bool     is_surface,
                         int      lat_cpr,
                         int      lon_cpr,
                         int64_t  now_us,
                         cpr_position_t *out_pos)
{
    cpr_slot_t *s = cpr_lookup_or_claim(icao24);
    s->last_seen_us = now_us;

    if (fflag == 0) {
        s->have_even       = true;
        s->is_surface_even = is_surface;
        s->lat_cpr_even    = lat_cpr;
        s->lon_cpr_even    = lon_cpr;
        s->t_even_us       = now_us;
    } else {
        s->have_odd       = true;
        s->is_surface_odd = is_surface;
        s->lat_cpr_odd    = lat_cpr;
        s->lon_cpr_odd    = lon_cpr;
        s->t_odd_us       = now_us;
    }

    /* Surface the cached position by default; we'll overwrite it if we
     * manage to decode a fresh one below. */
    *out_pos = s->pos;

    if (!s->have_even || !s->have_odd) return false;

    /* Never pair an airborne sample with a surface sample for the same
     * ICAO: airborne CPR spans 360 deg, surface CPR spans 90 deg — a
     * mixed pair decodes a plausible-looking but wrong position with no
     * way to detect the error downstream. */
    if (s->is_surface_even != s->is_surface_odd) return false;

    int64_t age_us = (s->t_even_us > s->t_odd_us)
                         ? (s->t_even_us - s->t_odd_us)
                         : (s->t_odd_us  - s->t_even_us);
    if (age_us > (int64_t)CPR_PAIR_MAX_AGE_US) return false;

    double yz_even = (double)s->lat_cpr_even / CPR_MAX;
    double yz_odd  = (double)s->lat_cpr_odd  / CPR_MAX;
    double xz_even = (double)s->lon_cpr_even / CPR_MAX;
    double xz_odd  = (double)s->lon_cpr_odd  / CPR_MAX;

    double j         = floor(59.0 * yz_even - 60.0 * yz_odd + 0.5);
    double lat_even  = DLAT_EVEN * (cpr_mod(j, 60.0) + yz_even);
    double lat_odd   = DLAT_ODD  * (cpr_mod(j, 59.0) + yz_odd);

    /* Latitudes >= 270 are encoded in the southern hemisphere as (lat-360). */
    if (lat_even >= 270.0) lat_even -= 360.0;
    if (lat_odd  >= 270.0) lat_odd  -= 360.0;

    int nl_even = cpr_nl(lat_even);
    int nl_odd  = cpr_nl(lat_odd);
    if (nl_even != nl_odd) {
        /* Receiver straddles a zone boundary; need a fresher pair. */
        return false;
    }

    double lat, lon;
    if (s->t_even_us >= s->t_odd_us) {
        /* Even frame is the more recent one (T = 0). */
        int    nl = nl_even;
        int    ni = (nl > 1) ? nl : 1;
        double m  = floor(xz_even * (nl - 1) - xz_odd * nl + 0.5);
        double dlon = 360.0 / ni;
        lat = lat_even;
        lon = dlon * (cpr_mod(m, (double)ni) + xz_even);
    } else {
        /* Odd frame is the more recent one (T = 1). */
        int    nl = nl_odd;
        int    ni = ((nl - 1) > 1) ? (nl - 1) : 1;
        double m  = floor(xz_even * (nl - 1) - xz_odd * nl + 0.5);
        double dlon = 360.0 / ni;
        lat = lat_odd;
        lon = dlon * (cpr_mod(m, (double)ni) + xz_odd);
    }

    /* Wrap longitude into [-180, 180). */
    if (lon >= 180.0) lon -= 360.0;

    s->pos.valid = true;
    s->pos.lat   = lat;
    s->pos.lon   = lon;
    *out_pos     = s->pos;
    return true;
}

/* Great-circle distance in nautical miles (haversine). Used only for the
 * sanity check in cpr_decode_surface_local(); at the scale we care about
 * (<=45 NM) a flat-earth approximation would be fine too, but haversine
 * costs nothing extra here (one decode per surface frame, not per pixel)
 * and stays correct near the poles / date line where a naive
 * lat/lon-degree distance would not. */
static double great_circle_nm(double lat1, double lon1, double lat2, double lon2)
{
    const double rad = M_PI / 180.0;
    double dlat = (lat2 - lat1) * rad;
    double dlon = (lon2 - lon1) * rad;
    double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
               cos(lat1 * rad) * cos(lat2 * rad) *
               sin(dlon / 2.0) * sin(dlon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return EARTH_RADIUS_NM * c;
}

bool cpr_decode_surface_local(int     fflag,
                              int     lat_cpr,
                              int     lon_cpr,
                              double  ref_lat,
                              double  ref_lon,
                              cpr_position_t *out_pos)
{
    out_pos->valid = false;
    out_pos->lat   = 0.0;
    out_pos->lon   = 0.0;

    double dlat = (fflag == 0) ? SURFACE_DLAT_EVEN : SURFACE_DLAT_ODD;
    double yz   = (double)lat_cpr / CPR_MAX;

    double j    = floor(ref_lat / dlat) +
                  floor(0.5 + cpr_mod(ref_lat, dlat) / dlat - yz);
    double rlat = dlat * (j + yz);

    int nl = cpr_nl(rlat);
    int ni = nl - fflag;
    if (ni < 1) ni = 1;
    double dlon = 90.0 / ni;

    double xz   = (double)lon_cpr / CPR_MAX;
    double m    = floor(ref_lon / dlon) +
                  floor(0.5 + cpr_mod(ref_lon, dlon) / dlon - xz);
    double rlon = dlon * (m + xz);

    /* Wrap longitude into [-180, 180) before the distance check and
     * before handing it back — same convention as the global decoder. */
    while (rlon >= 180.0)  rlon -= 360.0;
    while (rlon < -180.0)  rlon += 360.0;

    /* Reject anything outside the algorithm's guaranteed-unambiguous
     * radius rather than silently returning a possibly-wrong position.
     * By construction the local decode always snaps to the zone nearest
     * the reference, so this should only trip on a degenerate/garbage
     * reference (e.g. no GPS fix and no prior known position for this
     * aircraft) — that's exactly the case we must not paper over. */
    double dist_nm = great_circle_nm(rlat, rlon, ref_lat, ref_lon);
    if (dist_nm > CPR_SURFACE_VALID_RADIUS_NM) return false;

    out_pos->valid = true;
    out_pos->lat   = rlat;
    out_pos->lon   = rlon;
    return true;
}
