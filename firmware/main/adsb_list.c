/*
 * adsb_list.c — ADS-B aircraft list + selected-aircraft detail view.
 *
 * Pure pixel pushing — no I/O, no allocation, no state of its own.
 * Pulls the live aircraft snapshot from aircraft_state, the selection
 * cursor from ui_state, and renders into the caller's framebuffer.
 *
 * The list uses scale-1 (5×7) font so we can fit ICAO + callsign +
 * altitude + speed + heading + vertical-rate on one 320-px landscape
 * row (≈ 52 columns wide). The detail pane below uses scale-2 (10×14)
 * for the "ICAO:" / "Callsign:" key columns and scale-1 for the
 * values that need more width (lat/lon).
 */

#include "adsb_list.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

#include "aircraft_db.h"
#include "aircraft_state.h"
#include "airline_codes.h"
#include "display.h"
#include "icao_country.h"
#include "pfd_font.h"
#include "ui_state.h"

/* --- Layout constants ------------------------------------------------
 *
 * The list / detail split is ADAPTIVE: each frame we count how many
 * detail lines the selected aircraft needs (some fields are conditional —
 * Reg, Op, Sqwk, Cat, Type, Dist) and grow / shrink the list area
 * accordingly.
 *
 *   - When the selected aircraft has lots of fields populated, the
 *     detail pane is tall and the list shows fewer rows (down to
 *     MIN_LIST_ROWS_VISIBLE).
 *   - When the selection is sparse (just an ICAO/Call/Pos/Alt/Vel/Seen
 *     skeleton), the list expands toward the bottom of the screen,
 *     showing more aircraft.
 *   - When no aircraft are tracked at all, the list area gets the
 *     entire screen and the detail pane disappears.
 *
 * Header / col-titles / row0 stay fixed at the top; only LIST_BOTTOM_Y
 * and DETAIL_TOP_Y are computed per frame.
 */
#define LIST_HEADER_Y       0
#define LIST_COLTITLES_Y    14
#define LIST_ROW0_Y         24
#define LIST_ROW_H          10        /* scale-1 cell (8 px) + 2 px gap */
#define DETAIL_LINE_H       9         /* 8 px font + 1 px gap */
#define DETAIL_BOTTOM_PAD   2         /* leave 2 px below the last detail line */
#define DIVIDER_GAP_PX      4         /* 2 px divider + 2 px margin */
#define MIN_LIST_ROWS_VISIBLE 6       /* floor: don't squeeze list below this */

#define LIST_LEFT_PAD       4
#define LIST_RIGHT_LIMIT    (PK_DISPLAY_W - 2)

/* Column x-positions (scale-1 cell = 6 px) — laid out for 320 wide.
 * Total content ends ~ x=280, leaving ~40 px right slack.
 *
 *   ICAO     CALL      CT ALT   SPD HDG  VS    SQK   W
 *   780B1A   CZ1234    CN 16700 371 ^011 ^1500 1234  H
 *   6 chars  8 chars   2  5     3   4    5     4     1
 */
#define COL_X_ICAO    (LIST_LEFT_PAD)              /*   4 */
#define COL_X_CALL    (LIST_LEFT_PAD +  42)        /*  46  — 6c ICAO + gap */
#define COL_X_CT      (LIST_LEFT_PAD +  96)        /* 100  — 8c CALL + gap */
#define COL_X_ALT     (LIST_LEFT_PAD + 114)        /* 118  — 2c CT + gap */
#define COL_X_SPD     (LIST_LEFT_PAD + 150)        /* 154  — 5c ALT + gap */
#define COL_X_HDG     (LIST_LEFT_PAD + 174)        /* 178  — 3c SPD + gap */
#define COL_X_VS      (LIST_LEFT_PAD + 204)        /* 208  — 4c HDG + gap */
#define COL_X_SQK     (LIST_LEFT_PAD + 240)        /* 244  — 5c VS + gap */
#define COL_X_TYPE    (LIST_LEFT_PAD + 270)        /* 274  — 4c SQK + gap */

/* --- Palette --------------------------------------------------------- */
#define COL_BG              pk_rgb565( 12,  12,  16)   /* very dark grey */
#define COL_HEADER          pk_rgb565( 80, 220, 240)   /* cyan          */
#define COL_COL_TITLE       pk_rgb565(140, 140, 140)   /* dim grey      */
#define COL_ROW_FG          pk_rgb565(230, 230, 230)
#define COL_ROW_BG          COL_BG
#define COL_SELECTED_FG     pk_rgb565( 16,  16,  16)
#define COL_SELECTED_BG     pk_rgb565(255, 215,   0)   /* yellow        */
#define COL_DIVIDER         pk_rgb565( 60,  60,  70)
#define COL_DETAIL_KEY      pk_rgb565( 80, 220, 240)   /* cyan          */
#define COL_DETAIL_VAL      pk_rgb565(240, 240, 240)
#define COL_EMPTY_HINT      pk_rgb565(160, 160, 160)
#define COL_OWN_BIND        pk_rgb565(255,   0, 255)   /* magenta — runtime
                                                          own-ship binding */
#define COL_EMERGENCY       pk_rgb565(255,  72,  72)   /* bright red — Mode-A
                                                          7500/7600/7700 squawk */

/* --- Primitives ----------------------------------------------------- */
static void fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

/* --- Helpers -------------------------------------------------------- */

/* Right-justified scale-1 string. Returns x of the first glyph. */
static int puts_right(uint16_t *fb, int right_x, int y,
                      const char *s, uint16_t color)
{
    int w = (int)strlen(s) * PK_FONT_CELL_W(1);
    int x = right_x - w;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x, y, s, color, 1);
    return x;
}

/* Format helpers — keep these tight to fit in the row. */
static void fmt_icao(char *buf, size_t cap, uint32_t icao)
{
    snprintf(buf, cap, "%06lX", (unsigned long)(icao & 0xFFFFFF));
}

/* Copy raw ADS-B callsign with trailing spaces trimmed. */
static void trim_callsign(char *dst, size_t cap, const aircraft_t *a)
{
    if (cap == 0) return;
    if (!a->have_callsign) {
        dst[0] = '\0';
        return;
    }
    size_t n = AIRCRAFT_CALLSIGN_LEN < cap ? AIRCRAFT_CALLSIGN_LEN : cap;
    memcpy(dst, a->callsign, n);
    dst[n - 1] = '\0';
    for (int i = (int)strlen(dst) - 1; i >= 0 && dst[i] == ' '; --i) {
        dst[i] = '\0';
    }
}

/*
 * Format the list-view CALL column. Preference:
 *   1) "<IATA><digits>" if the ICAO prefix is in airline_codes.c
 *      (e.g. "CSN1234" → "CZ1234")
 *   2) Raw ADS-B callsign as-is (e.g. "N12345", "CSC1234" if no IATA)
 *   3) "--------" placeholder when no callsign decoded yet
 */
static void fmt_call_column(char *buf, size_t cap, const aircraft_t *a)
{
    char raw[AIRCRAFT_CALLSIGN_LEN];
    trim_callsign(raw, sizeof(raw), a);
    if (raw[0] == '\0') {
        snprintf(buf, cap, "--------");
        return;
    }
    const char *flight_no = NULL;
    const pk_airline_t *air = pk_airline_from_callsign(raw, &flight_no);
    if (air && air->iata2 && air->iata2[0] && flight_no && flight_no[0]) {
        snprintf(buf, cap, "%s%s", air->iata2, flight_no);
    } else {
        snprintf(buf, cap, "%s", raw);
    }
}

/* True if the given decoded squawk is one of the three ICAO-reserved
 * emergency codes — values are decimal-display of the 4 octal digits
 * (see mode-s.c:412-428 conversion). */
static bool squawk_is_emergency(int squawk)
{
    return squawk == 7500 || squawk == 7600 || squawk == 7700;
}

/* Vertical-rate state icon: ↑ any climb / ↓ any descent / '-' exactly
 * level (vs == 0). Re-uses the 0x80 / 0x84 (N/S) compass glyphs from
 * pfd_font so the iconography matches the HDG column.
 *
 * No magnitude threshold — using ±100 fpm as a "level" band caused
 * cases like vs=-64 to render as "-64 fpm" (dash icon + magnitude 64)
 * which reads as negative 64. With strict sign-based mapping, vs=-64
 * → ↓64 fpm and vs=+1500 → ↑1500 fpm, unambiguous. */
static char vs_state_icon(int vs_fpm, bool have)
{
    if (!have)         return ' ';
    if (vs_fpm > 0)    return PK_FONT_ARROW_N;
    if (vs_fpm < 0)    return PK_FONT_ARROW_S;
    return '-';
}

/* HDG direction icon — dual-mode:
 *   - Own-ship bound + has velocity: render the target's heading
 *     RELATIVE to own-ship's track. ↑ = same direction, ↘ = ~135°
 *     right of own's nose, etc. Operationally useful — shows
 *     convergent / divergent traffic at a glance.
 *   - Otherwise: render the target's ABSOLUTE compass heading.
 *     0° → ↑, 045° → ↗, 090° → →, ..., 315° → ↖. Every row gets a
 *     directional cue even without an own-ship reference.
 * Returns ' ' only when the target itself has no velocity. */
static char hdg_dir_icon(const aircraft_t *target,
                         bool have_own_velocity,
                         int own_heading_deg)
{
    if (!target->have_velocity) return ' ';
    int ref = have_own_velocity ? (target->heading_deg - own_heading_deg)
                                : target->heading_deg;
    return pk_font_arrow_for_delta_deg(ref);
}

/* Great-circle distance in nautical miles + initial bearing in degrees
 * (0..360, where 0 = North, 90 = East). Caller owns the math.h linkage. */
#define EARTH_RADIUS_NM   3440.065
#define DEG2RAD(x)        ((x) * (M_PI / 180.0))
#define RAD2DEG(x)        ((x) * (180.0 / M_PI))

static void geo_dist_brg(double lat1_deg, double lon1_deg,
                         double lat2_deg, double lon2_deg,
                         double *out_dist_nm, double *out_brg_deg)
{
    double phi1   = DEG2RAD(lat1_deg);
    double phi2   = DEG2RAD(lat2_deg);
    double dphi   = DEG2RAD(lat2_deg - lat1_deg);
    double dlam   = DEG2RAD(lon2_deg - lon1_deg);

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

static void fmt_age_s(char *buf, size_t cap, int64_t now_us, int64_t then_us)
{
    int sec = (int)((now_us - then_us) / 1000000LL);
    if (sec < 0) sec = 0;
    if (sec < 60) {
        snprintf(buf, cap, "%2ds ago", sec);
    } else {
        int min = sec / 60;
        snprintf(buf, cap, "%dm ago", min);
    }
}

/* --- List render ---------------------------------------------------- */

static void render_header(uint16_t *fb, size_t n_aircraft)
{
    char buf[40];
    snprintf(buf, sizeof(buf), "ADS-B AIRCRAFT (%u)", (unsigned)n_aircraft);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD, LIST_HEADER_Y, buf, COL_HEADER, 1);
}

static void render_col_titles(uint16_t *fb)
{
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_ICAO, LIST_COLTITLES_Y, "ICAO",   COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_CALL, LIST_COLTITLES_Y, "CALL",   COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_CT,   LIST_COLTITLES_Y, "CT",     COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_ALT,  LIST_COLTITLES_Y, "ALT",    COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_SPD,  LIST_COLTITLES_Y, "SPD",    COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_HDG,  LIST_COLTITLES_Y, "HDG",    COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_VS,   LIST_COLTITLES_Y, "VS",     COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_SQK,  LIST_COLTITLES_Y, "SQK",    COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_TYPE, LIST_COLTITLES_Y, "TYPE",   COL_COL_TITLE, 1);
}

static void render_row(uint16_t *fb, int row_idx, int y, bool selected,
                       bool is_own_bound,
                       bool have_own_velocity, int own_heading_deg,
                       const aircraft_t *a)
{
    uint16_t fg = selected ? COL_SELECTED_FG : COL_ROW_FG;
    uint16_t bg = selected ? COL_SELECTED_BG : COL_ROW_BG;

    /* Row background spans the full width. */
    fill_rect(fb, 0, y - 1, PK_DISPLAY_W, y + 8, bg);

    char buf[16];

    /* ICAO column — magenta if this aircraft is the runtime own-ship
     * binding (set via TARE short-press), unless this row is also the
     * selection highlight (yellow takes precedence so the cursor
     * stays unambiguous). */
    uint16_t icao_fg = (is_own_bound && !selected) ? COL_OWN_BIND : fg;
    fmt_icao(buf, sizeof(buf), a->icao24);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_ICAO, y, buf, icao_fg, 1);

    /* CALL — prefer IATA "CZ1234" form when the airline is in our table;
     * else fall back to raw ADS-B callsign / "--------". */
    fmt_call_column(buf, sizeof(buf), a);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_CALL, y, buf, fg, 1);

    /* CT — 2-letter ISO country code from ICAO 24-bit address. */
    const pk_country_t *country = pk_country_from_icao24(a->icao24);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_CT, y,
                 country ? country->iso2 : "--", fg, 1);

    /* ALT */
    if (a->have_altitude) {
        snprintf(buf, sizeof(buf), "%5d", a->altitude_ft);
    } else {
        snprintf(buf, sizeof(buf), "%5s", "-----");
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_ALT, y, buf, fg, 1);

    /* SPD */
    if (a->have_velocity) {
        snprintf(buf, sizeof(buf), "%3d", a->ground_speed_kt);
    } else {
        snprintf(buf, sizeof(buf), "%3s", "---");
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_SPD, y, buf, fg, 1);

    /* HDG — 4 chars: icon (rel-track marker, ^=same v=opp ' '=other) +
     * 3-digit heading. Icon is blank if own-ship has no velocity yet. */
    if (a->have_velocity) {
        char icon = hdg_dir_icon(a, have_own_velocity, own_heading_deg);
        snprintf(buf, sizeof(buf), "%c%03d", icon, a->heading_deg);
    } else {
        snprintf(buf, sizeof(buf), " ---");
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_HDG, y, buf, fg, 1);

    /* VS — 5 chars: ^v- icon + unsigned 4-digit |fpm| left-aligned. */
    if (a->have_velocity) {
        char icon = vs_state_icon(a->vert_rate_fpm, true);
        int  v    = a->vert_rate_fpm < 0 ? -a->vert_rate_fpm : a->vert_rate_fpm;
        if (v > 9999) v = 9999;
        snprintf(buf, sizeof(buf), "%c%-4d", icon, v);
    } else {
        snprintf(buf, sizeof(buf), "%5s", "-----");
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_VS, y, buf, fg, 1);

    /* SQK — 4 octal digits, rendered red for 7500/7600/7700 even when
     * the row is selected (emergency wins over selection visual). */
    if (a->have_squawk) {
        snprintf(buf, sizeof(buf), "%04d", a->squawk);
    } else {
        snprintf(buf, sizeof(buf), "----");
    }
    uint16_t sqk_fg = (a->have_squawk && squawk_is_emergency(a->squawk))
                          ? COL_EMERGENCY : fg;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_SQK, y, buf, sqk_fg, 1);

    /* TYPE — 4-char ICAO type designator from the embedded
     * aircraft_db (B738, A320, EC35, ...). Falls back to "----" when
     * the ICAO24 isn't in the DB (rare aircraft, military, brand-new
     * tail not yet harvested). Wake category went to detail-only. */
    const char *type_code = pk_aircraft_type_code(a->icao24);
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%-4s", type_code ? type_code : "----");
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_TYPE, y, tbuf, fg, 1);

    (void)row_idx;
}

static void render_divider(uint16_t *fb, int list_bottom_y)
{
    fill_rect(fb, 0, list_bottom_y, PK_DISPLAY_W, list_bottom_y + 2,
              COL_DIVIDER);
}

/* Count how many lines render_detail() would draw for this aircraft.
 * Must mirror the conditionals in render_detail() exactly — keep the
 * two in sync. Used by the layout pass to size the detail pane up
 * front, before the divider position is known. */
static int count_detail_lines(const aircraft_t *a,
                              bool is_own_bound,
                              bool have_own, const aircraft_t *own)
{
    int n = 1;                          /* ICAO (always) */

    if (pk_aircraft_registration(a->icao24)) n++;     /* Reg */

    n++;                                /* Call (always) */

    {                                   /* Op (only when airline known) */
        char raw[AIRCRAFT_CALLSIGN_LEN];
        trim_callsign(raw, sizeof(raw), a);
        if (raw[0] != '\0') {
            const char *fn = NULL;
            const pk_airline_t *air = pk_airline_from_callsign(raw, &fn);
            if (air && air->name && air->name[0]) n++;
        }
    }

    n++;                                /* Pos (always — placeholder if no fix) */
    n++;                                /* Alt (always) */
    n++;                                /* Vel (always) */

    if (a->have_squawk) n++;            /* Sqwk */

    {                                   /* Cat (only when wake known) */
        const char *wn = pk_wake_name(a->wake);
        if (wn && wn[0]) n++;
    }

    if (pk_aircraft_type_code(a->icao24)) n++;        /* Type */

    if (have_own && !is_own_bound &&
        own && own->have_position && a->have_position) n++;   /* Dist */

    n++;                                /* Seen (always) */
    return n;
}

/* --- Detail pane ---------------------------------------------------- */

#define DETAIL_KEY_X      LIST_LEFT_PAD
#define DETAIL_VAL_X      (LIST_LEFT_PAD + 48)   /* "Key   :" = 7 chars × 6 + gap */

/* Helper — render a "Key  :" / "value" pair on one line.
 * Returns next y. Skips rendering entirely when value is NULL. */
static int detail_line(uint16_t *fb, int y, const char *key, const char *value,
                       uint16_t value_color)
{
    if (value == NULL) return y;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 DETAIL_KEY_X, y, key, COL_DETAIL_KEY, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 DETAIL_VAL_X, y, value, value_color, 1);
    return y + DETAIL_LINE_H;
}

static void render_detail(uint16_t *fb, int detail_top_y,
                          const aircraft_t *a, int64_t now_us)
{
    int y = detail_top_y;
    char buf[64];

    uint32_t own_icao = pk_ui_get_own_icao();
    bool is_own_bound = (own_icao != 0 && a->icao24 == own_icao);

    /* Own-ship snapshot — used for relative altitude + dist/bearing.
     * NULL out if not bound or stale. */
    aircraft_t own;
    bool have_own = false;
    if (own_icao != 0) {
        have_own = aircraft_state_get_own(own_icao, now_us,
                                          AIRCRAFT_STALE_AGE_US, &own);
    }
    (void)have_own;  /* used below for rel-alt / dist */

    /* --- ICAO + country + own-binding marker --- */
    {
        const pk_country_t *country = pk_country_from_icao24(a->icao24);
        if (country) {
            snprintf(buf, sizeof(buf), "%06lX  %s/%s%s",
                     (unsigned long)(a->icao24 & 0xFFFFFF),
                     country->iso2, country->name,
                     is_own_bound ? "  (OWN)" : "");
        } else {
            snprintf(buf, sizeof(buf), "%06lX%s",
                     (unsigned long)(a->icao24 & 0xFFFFFF),
                     is_own_bound ? "  (OWN)" : "");
        }
        y = detail_line(fb, y, "ICAO  :", buf,
                        is_own_bound ? COL_OWN_BIND : COL_DETAIL_VAL);
    }

    /* --- Registration tail (e.g. "B-5797", "N12345"). Skipped if the
     *     embedded DB has no reg for this ICAO24. */
    {
        const char *reg = pk_aircraft_registration(a->icao24);
        if (reg && reg[0]) {
            y = detail_line(fb, y, "Reg   :", reg, COL_DETAIL_VAL);
        }
    }

    /* --- Callsign (ICAO long form + IATA short form when known) --- */
    {
        char raw[AIRCRAFT_CALLSIGN_LEN];
        trim_callsign(raw, sizeof(raw), a);
        if (raw[0] == '\0') {
            y = detail_line(fb, y, "Call  :", "--------", COL_DETAIL_VAL);
        } else {
            const char *flight_no = NULL;
            const pk_airline_t *air = pk_airline_from_callsign(raw, &flight_no);
            if (air && air->iata2 && air->iata2[0] && flight_no && flight_no[0]) {
                snprintf(buf, sizeof(buf), "%s  ->  %s%s",
                         raw, air->iata2, flight_no);
            } else {
                snprintf(buf, sizeof(buf), "%s", raw);
            }
            y = detail_line(fb, y, "Call  :", buf, COL_DETAIL_VAL);
        }
    }

    /* --- Operator (full airline name; skip if unknown) --- */
    {
        char raw[AIRCRAFT_CALLSIGN_LEN];
        trim_callsign(raw, sizeof(raw), a);
        const char *flight_no = NULL;
        const pk_airline_t *air = (raw[0] != '\0')
            ? pk_airline_from_callsign(raw, &flight_no) : NULL;
        if (air && air->name) {
            y = detail_line(fb, y, "Op    :", air->name, COL_DETAIL_VAL);
        }
    }

    /* --- Position (or "no fix") --- */
    {
        if (a->have_position) {
            char age_buf[16];
            fmt_age_s(age_buf, sizeof(age_buf), now_us, a->position_us);
            snprintf(buf, sizeof(buf), "%+8.4f %+9.4f (%s)",
                     a->lat, a->lon, age_buf);
        } else {
            snprintf(buf, sizeof(buf), "(no fix yet)");
        }
        y = detail_line(fb, y, "Pos   :", buf, COL_DETAIL_VAL);
    }

    /* --- Altitude + relative-to-own --- */
    {
        if (a->have_altitude) {
            int n = snprintf(buf, sizeof(buf), "%d ft", a->altitude_ft);
            if (have_own && own.have_altitude && !is_own_bound &&
                n > 0 && (size_t)n < sizeof(buf)) {
                int rel = a->altitude_ft - own.altitude_ft;
                snprintf(buf + n, sizeof(buf) - n,
                         "   REL %+d ft", rel);
            }
        } else {
            snprintf(buf, sizeof(buf), "---");
        }
        y = detail_line(fb, y, "Alt   :", buf, COL_DETAIL_VAL);
    }

    /* --- Velocity — ground speed + heading (with compass arrow) +
     *     vertical rate (with climb/descent arrow). Two arrows so the
     *     line is parseable at a glance even before reading the digits.
     *     The HDG arrow uses the same dual-mode logic as the list view:
     *     relative to own-ship when bound, absolute compass direction
     *     otherwise. */
    {
        if (a->have_velocity) {
            char vsicon  = vs_state_icon(a->vert_rate_fpm, true);
            int  vs_abs  = a->vert_rate_fpm < 0
                              ? -a->vert_rate_fpm : a->vert_rate_fpm;
            bool have_own_vel = have_own && own.have_velocity;
            char hdgicon = hdg_dir_icon(a, have_own_vel,
                                        have_own_vel ? own.heading_deg : 0);
            /* "~" is the degree symbol per pk_font's mapping. */
            snprintf(buf, sizeof(buf), "%d kt @ %c%03d~  %c%d fpm",
                     a->ground_speed_kt, hdgicon, a->heading_deg,
                     vsicon, vs_abs);
        } else {
            snprintf(buf, sizeof(buf), "---");
        }
        y = detail_line(fb, y, "Vel   :", buf, COL_DETAIL_VAL);
    }

    /* --- Squawk (red if emergency) --- */
    {
        if (a->have_squawk) {
            const char *emerg_tag = "";
            if (a->squawk == 7500)      emerg_tag = "  HIJACK";
            else if (a->squawk == 7600) emerg_tag = "  RADIO FAIL";
            else if (a->squawk == 7700) emerg_tag = "  EMERGENCY";
            snprintf(buf, sizeof(buf), "%04d%s", a->squawk, emerg_tag);
            y = detail_line(fb, y, "Sqwk  :", buf,
                            squawk_is_emergency(a->squawk)
                                ? COL_EMERGENCY : COL_DETAIL_VAL);
        }
    }

    /* --- Wake / aircraft category --- */
    {
        const char *wname = pk_wake_name(a->wake);
        if (wname && wname[0]) {
            snprintf(buf, sizeof(buf), "%s (%c)", wname, pk_wake_letter(a->wake));
            y = detail_line(fb, y, "Cat   :", buf, COL_DETAIL_VAL);
        }
    }

    /* --- Type (ICAO 4-char designator + Doc 8643 tech descriptor +
     *     canonical model name). Format: "B738 L2J  BOEING 737-800"
     *     — code first for the 1-glance read, then desc in parens, then
     *     the model name when distinct from the code.
     *     "L2J" parses as Land 2-engine Jet, "H2T" as Helicopter 2-Turbo,
     *     etc. (ICAO Doc 8643 designator). */
    {
        const char *code  = pk_aircraft_type_code(a->icao24);
        const char *model = pk_aircraft_type_model(a->icao24);
        const char *desc  = pk_aircraft_type_desc(a->icao24);
        if (code && code[0]) {
            int n = snprintf(buf, sizeof(buf), "%s", code);
            if (desc && desc[0] && n > 0 && (size_t)n < sizeof(buf)) {
                n += snprintf(buf + n, sizeof(buf) - n, " %s", desc);
            }
            if (model && model[0] && strcmp(code, model) != 0 &&
                n > 0 && (size_t)n < sizeof(buf)) {
                snprintf(buf + n, sizeof(buf) - n, "  %s", model);
            }
            y = detail_line(fb, y, "Type  :", buf, COL_DETAIL_VAL);
        }
    }

    /* --- Distance / bearing to own-ship --- */
    if (have_own && !is_own_bound && own.have_position && a->have_position) {
        double dist_nm, brg_deg;
        geo_dist_brg(own.lat, own.lon, a->lat, a->lon, &dist_nm, &brg_deg);
        snprintf(buf, sizeof(buf), "%.1f NM  brg %03d~",
                 dist_nm, (int)(brg_deg + 0.5));
        y = detail_line(fb, y, "Dist  :", buf, COL_DETAIL_VAL);
    }

    /* --- Last seen (+ GND suffix when on the ground) --- */
    {
        char age_buf[16];
        fmt_age_s(age_buf, sizeof(age_buf), now_us, a->last_seen_us);
        if (a->on_ground) {
            snprintf(buf, sizeof(buf), "%s  GND", age_buf);
        } else {
            snprintf(buf, sizeof(buf), "%s", age_buf);
        }
        y = detail_line(fb, y, "Seen  :", buf, COL_DETAIL_VAL);
    }

    (void)puts_right; /* may be useful later for right-justified fields */
}

static void render_empty_hint(uint16_t *fb)
{
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD, LIST_ROW0_Y + 20,
                 "No aircraft in the trailing 60s window.",
                 COL_EMPTY_HINT, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD, LIST_ROW0_Y + 36,
                 "Antenna pointed up? RTL-SDR streaming?",
                 COL_EMPTY_HINT, 1);
}

/* --- Public entry --------------------------------------------------- */

void pk_adsb_list_render(uint16_t *fb)
{
    int64_t now_us = esp_timer_get_time();

    /* Background — explicit fill avoids ghosting from the previous frame
     * (which may have been a PFD render with a horizon). */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* Snapshot — same 60 s "fresh contacts" window the PFD uses. We
     * stash this in static storage so the on-stack cost is one int
     * pointer; aircraft_state's lock-protected copy is cheap. Placed
     * in PSRAM (EXT_RAM_BSS) — the renderer is the only consumer and
     * doesn't need the snapshot in fast DRAM. Keeps internal DRAM
     * free for FreeRTOS / ESP-Hosted timer-task allocations at boot;
     * without this the daemon-task stack-malloc trips
     * vApplicationGetTimerTaskMemory's NULL assert. */
    static EXT_RAM_BSS_ATTR aircraft_t scratch[AIRCRAFT_TABLE_CAPACITY];
    size_t n = aircraft_state_snapshot(scratch,
                                       sizeof(scratch) / sizeof(scratch[0]),
                                       now_us,
                                       AIRCRAFT_STALE_AGE_US);

    render_header(fb, n);

    if (n == 0) {
        /* Empty list: no detail pane, no divider — give the empty hint
         * the whole screen below the header. */
        render_empty_hint(fb);
        return;
    }

    /* Resolve the selected row against the live (sorted-by-ICAO)
     * snapshot. The resolver tracks selection by ICAO under the hood,
     * so the highlight sticks to the same aircraft across snapshot
     * reshuffles (entries entering / leaving the trailing-60s window)
     * and consumes any buffered UP/DOWN intent in the process. */
    uint32_t sel_icaos[AIRCRAFT_TABLE_CAPACITY];
    for (size_t i = 0; i < n; ++i) sel_icaos[i] = scratch[i].icao24;
    int sel = pk_ui_list_resolve_row(sel_icaos, n);

    /* Snapshot the own-ship aircraft (if bound) so we can:
     *   - compute the HDG-relative arrow per row (render_row)
     *   - feed render_detail for REL alt + Dist + Vel relative HDG
     *   - feed count_detail_lines for the layout pass below
     * All three callees only need the velocity / position fields, so we
     * stash the whole record once and read out the relevant bits. */
    uint32_t   own_icao   = pk_ui_get_own_icao();
    aircraft_t own;
    bool       have_own   = false;
    if (own_icao != 0) {
        have_own = aircraft_state_get_own(own_icao, now_us,
                                          AIRCRAFT_STALE_AGE_US, &own);
    }
    bool have_own_velocity = have_own && own.have_velocity;
    int  own_heading_deg   = have_own_velocity ? own.heading_deg : 0;

    /* ----- Adaptive layout ---------------------------------------------
     * Count how many lines the detail pane will draw for the selected
     * aircraft, then compute where to put the divider so the detail
     * exactly fits at the bottom of the screen. If the detail is too
     * tall to leave room for MIN_LIST_ROWS_VISIBLE list rows, clamp
     * the divider up and let the detail overflow off-screen at the
     * bottom (rare with the 12-line worst case + 10-row floor). */
    bool sel_is_own = (own_icao != 0 && scratch[sel].icao24 == own_icao);
    int detail_lines = count_detail_lines(&scratch[sel], sel_is_own,
                                          have_own, &own);
    int detail_height = detail_lines * DETAIL_LINE_H + DETAIL_BOTTOM_PAD;
    int detail_top_y  = PK_DISPLAY_H - detail_height;
    int list_bottom_y = detail_top_y - DIVIDER_GAP_PX;

    const int min_list_bottom_y =
        LIST_ROW0_Y + MIN_LIST_ROWS_VISIBLE * LIST_ROW_H;
    if (list_bottom_y < min_list_bottom_y) {
        list_bottom_y = min_list_bottom_y;
        detail_top_y  = list_bottom_y + DIVIDER_GAP_PX;
    }

    render_col_titles(fb);

    /* Auto-scroll the list so the selected row is visible. Window
     * [first, first+n_visible) contains `sel`. n_visible depends on
     * the adaptive list_bottom_y above. */
    int n_visible = (list_bottom_y - LIST_ROW0_Y) / LIST_ROW_H;
    if (n_visible < 1) n_visible = 1;
    int first = 0;
    if ((int)n > n_visible) {
        first = sel - n_visible / 2;
        if (first < 0) first = 0;
        if (first + n_visible > (int)n) first = (int)n - n_visible;
    }

    for (int row = 0; row < n_visible && (first + row) < (int)n; ++row) {
        int y = LIST_ROW0_Y + row * LIST_ROW_H;
        bool is_selected   = ((first + row) == sel);
        bool is_own_bound  = (own_icao != 0 &&
                              scratch[first + row].icao24 == own_icao);
        render_row(fb, first + row, y, is_selected, is_own_bound,
                   have_own_velocity, own_heading_deg,
                   &scratch[first + row]);
    }

    render_divider(fb, list_bottom_y);
    render_detail(fb, detail_top_y, &scratch[sel], now_us);
}
