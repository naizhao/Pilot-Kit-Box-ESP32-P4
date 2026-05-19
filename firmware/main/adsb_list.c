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
#include <string.h>

#include "esp_timer.h"

#include "aircraft_state.h"
#include "display.h"
#include "pfd_font.h"
#include "ui_state.h"

/* --- Layout constants ------------------------------------------------ */
#define LIST_HEADER_Y       0
#define LIST_COLTITLES_Y    14
#define LIST_ROW0_Y         24
#define LIST_ROW_H          10        /* scale-1 cell (8 px) + 2 px gap */
#define LIST_BOTTOM_Y       154       /* end of list area (landscape: 240H - 86 px detail) */
#define DETAIL_TOP_Y        158
#define DETAIL_LINE_H       13        /* tightened from 14 so 6 rows fit */

#define LIST_LEFT_PAD       4
#define LIST_RIGHT_LIMIT    (PK_DISPLAY_W - 2)

/* Column x-positions (scale-1 cell = 6 px) — laid out for 320 wide */
#define COL_X_ICAO    (LIST_LEFT_PAD)
#define COL_X_CALL    (LIST_LEFT_PAD +  48)
#define COL_X_ALT     (LIST_LEFT_PAD + 116)
#define COL_X_SPD     (LIST_LEFT_PAD + 166)
#define COL_X_HDG     (LIST_LEFT_PAD + 206)
#define COL_X_VS      (LIST_LEFT_PAD + 246)

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

static void fmt_callsign(char *buf, size_t cap, const aircraft_t *a)
{
    if (a->have_callsign) {
        /* Trim trailing spaces from the 8-char ATC field. */
        char tmp[AIRCRAFT_CALLSIGN_LEN];
        memcpy(tmp, a->callsign, sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';
        for (int i = (int)strlen(tmp) - 1; i >= 0 && tmp[i] == ' '; --i) {
            tmp[i] = '\0';
        }
        snprintf(buf, cap, "%-8s", tmp);
    } else {
        snprintf(buf, cap, "--------");
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
    /* Column positions (scale-1 char width = 6 px).
     * Total budget: PK_DISPLAY_W - LIST_LEFT_PAD - 2 = 314 px.
     * Layout: ICAO (6 chars), CALL (8), ALT (5), SPD (3), HDG (3),
     * VS (5 signed). */
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_ICAO, LIST_COLTITLES_Y, "ICAO", COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_CALL, LIST_COLTITLES_Y, "CALL", COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_ALT,  LIST_COLTITLES_Y, "ALT",  COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_SPD,  LIST_COLTITLES_Y, "SPD",  COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_HDG,  LIST_COLTITLES_Y, "HDG",  COL_COL_TITLE, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 COL_X_VS,   LIST_COLTITLES_Y, "VS",   COL_COL_TITLE, 1);
}

static void render_row(uint16_t *fb, int row_idx, int y, bool selected,
                       const aircraft_t *a)
{
    uint16_t fg = selected ? COL_SELECTED_FG : COL_ROW_FG;
    uint16_t bg = selected ? COL_SELECTED_BG : COL_ROW_BG;

    /* Row background spans the full width. */
    fill_rect(fb, 0, y - 1, PK_DISPLAY_W, y + 8, bg);

    char buf[16];

    fmt_icao(buf, sizeof(buf), a->icao24);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_ICAO, y, buf, fg, 1);

    fmt_callsign(buf, sizeof(buf), a);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_CALL, y, buf, fg, 1);

    if (a->have_altitude) {
        snprintf(buf, sizeof(buf), "%5d", a->altitude_ft);
    } else {
        snprintf(buf, sizeof(buf), "%5s", "-----");
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_ALT, y, buf, fg, 1);

    if (a->have_velocity) {
        snprintf(buf, sizeof(buf), "%3d", a->ground_speed_kt);
    } else {
        snprintf(buf, sizeof(buf), "%3s", "---");
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_SPD, y, buf, fg, 1);

    if (a->have_velocity) {
        snprintf(buf, sizeof(buf), "%3d", a->heading_deg);
    } else {
        snprintf(buf, sizeof(buf), "%3s", "---");
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_HDG, y, buf, fg, 1);

    if (a->have_velocity) {
        snprintf(buf, sizeof(buf), "%+5d", a->vert_rate_fpm);
    } else {
        snprintf(buf, sizeof(buf), "%5s", "-----");
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COL_X_VS, y, buf, fg, 1);

    (void)row_idx;
}

static void render_divider(uint16_t *fb)
{
    fill_rect(fb, 0, LIST_BOTTOM_Y, PK_DISPLAY_W, LIST_BOTTOM_Y + 2,
              COL_DIVIDER);
}

/* --- Detail pane ---------------------------------------------------- */

static void render_detail(uint16_t *fb, const aircraft_t *a, int64_t now_us)
{
    int y = DETAIL_TOP_Y;
    char buf[48];

    /* ICAO */
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD, y, "ICAO   :", COL_DETAIL_KEY, 1);
    fmt_icao(buf, sizeof(buf), a->icao24);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD + 60, y, buf, COL_DETAIL_VAL, 1);
    y += DETAIL_LINE_H;

    /* Callsign */
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD, y, "Call   :", COL_DETAIL_KEY, 1);
    fmt_callsign(buf, sizeof(buf), a);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD + 60, y, buf, COL_DETAIL_VAL, 1);
    y += DETAIL_LINE_H;

    /* Position */
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD, y, "Pos    :", COL_DETAIL_KEY, 1);
    if (a->have_position) {
        char age_buf[16];
        fmt_age_s(age_buf, sizeof(age_buf), now_us, a->position_us);
        snprintf(buf, sizeof(buf), "%+8.4f %+9.4f (%s)",
                 a->lat, a->lon, age_buf);
    } else {
        snprintf(buf, sizeof(buf), "(no fix yet)");
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD + 60, y, buf, COL_DETAIL_VAL, 1);
    y += DETAIL_LINE_H;

    /* Altitude */
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD, y, "Alt    :", COL_DETAIL_KEY, 1);
    if (a->have_altitude) {
        snprintf(buf, sizeof(buf), "%d ft", a->altitude_ft);
    } else {
        snprintf(buf, sizeof(buf), "---");
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD + 60, y, buf, COL_DETAIL_VAL, 1);
    y += DETAIL_LINE_H;

    /* Velocity (speed @ heading) + vrate */
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD, y, "Vel    :", COL_DETAIL_KEY, 1);
    if (a->have_velocity) {
        /* "~" is the degree symbol per pk_font's mapping. */
        snprintf(buf, sizeof(buf), "%d kt @ %d~  v%+d fpm",
                 a->ground_speed_kt, a->heading_deg, a->vert_rate_fpm);
    } else {
        snprintf(buf, sizeof(buf), "---");
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD + 60, y, buf, COL_DETAIL_VAL, 1);
    y += DETAIL_LINE_H;

    /* Last seen */
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD, y, "Seen   :", COL_DETAIL_KEY, 1);
    fmt_age_s(buf, sizeof(buf), now_us, a->last_seen_us);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 LIST_LEFT_PAD + 60, y, buf, COL_DETAIL_VAL, 1);
    y += DETAIL_LINE_H;

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
     * pointer; aircraft_state's lock-protected copy is cheap. */
    static aircraft_t scratch[AIRCRAFT_TABLE_CAPACITY];
    size_t n = aircraft_state_snapshot(scratch,
                                       sizeof(scratch) / sizeof(scratch[0]),
                                       now_us,
                                       AIRCRAFT_STALE_AGE_US);

    render_header(fb, n);

    if (n == 0) {
        render_empty_hint(fb);
        render_divider(fb);
        /* Detail pane: leave intentionally blank when nothing to detail. */
        return;
    }

    /* Clamp selection cursor against the live count, then publish back
     * so future scrolls anchor at the live ceiling. */
    int sel = pk_ui_list_get_index();
    if (sel >= (int)n) sel = (int)(n - 1);
    if (sel < 0)       sel = 0;
    pk_ui_list_set_index(sel);

    render_col_titles(fb);

    /* Auto-scroll the list so the selected row is visible. With
     * LIST_ROW_H = 10 and (LIST_BOTTOM_Y − LIST_ROW0_Y) = 176 we can
     * fit 17 rows on screen — plenty for the 64-aircraft worst case.
     * Compute a window [first, first+n_visible) that contains `sel`. */
    int n_visible = (LIST_BOTTOM_Y - LIST_ROW0_Y) / LIST_ROW_H;
    int first = 0;
    if ((int)n > n_visible) {
        first = sel - n_visible / 2;
        if (first < 0) first = 0;
        if (first + n_visible > (int)n) first = (int)n - n_visible;
    }

    for (int row = 0; row < n_visible && (first + row) < (int)n; ++row) {
        int y = LIST_ROW0_Y + row * LIST_ROW_H;
        bool is_selected = ((first + row) == sel);
        render_row(fb, first + row, y, is_selected, &scratch[first + row]);
    }

    render_divider(fb);
    render_detail(fb, &scratch[sel], now_us);
}
