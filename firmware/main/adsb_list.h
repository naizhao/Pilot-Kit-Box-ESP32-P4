/*
 * adsb_list.h — ADS-B aircraft list view for the 240×320 ST7789 panel.
 *
 * Layout (portrait, 240 × 320 RGB565):
 *
 *   y = 0       ┌────────────────────────────────────────┐
 *               │ ADS-B AIRCRAFT (n)                     │  header (scale-1 text)
 *   y = 14      ├────────────────────────────────────────┤
 *               │ ICAO    CALL      ALT     SPD   HDG    │  column titles
 *   y = 24      ├────────────────────────────────────────┤
 *               │ 780B0D  CSN3825   30100   415   335    │  row 0   (highlighted if
 *               │ 7822BB  ----      15175   368    35    │   selected)
 *               │ ...                                    │
 *   y = 200     ├────────────────────────────────────────┤
 *               │ ICAO    : 780B0D                       │  detail pane for the
 *               │ Callsign: CSN3825                      │  highlighted row
 *               │ Position: 39.91°N  116.40°E (2 s ago)  │
 *               │ Altitude: 30100 ft                     │
 *               │ Speed   : 415 kt @ 335°                │
 *               │ V-rate  : +22 fpm                      │
 *               │ Seen    : 1 s ago                      │
 *   y = 320     └────────────────────────────────────────┘
 *
 * Selection is driven by pk_ui_list_scroll(±1) from button_task.
 * adsb_list resolves the highlight via pk_ui_list_resolve_row() each
 * frame — ui_state tracks the selection by ICAO under the hood so the
 * highlight stays anchored to the same aircraft even as the sorted
 * snapshot reshuffles when aircraft enter / leave the 60s window.
 *
 * Refresh model: re-rendered every PFD-task frame when the UI mode is
 * PK_UI_MODE_ADSB_LIST. Cheap to redraw — same fill_rect + bitmap font
 * primitives the PFD already uses; the bottleneck is the SPI flush
 * (same as PFD). No internal caching.
 */
#pragma once

#include <stdint.h>

/* Renders the full ADS-B list view into a 240 × 320 RGB565 framebuffer.
 * Assumes the caller hands ownership of the framebuffer for the
 * duration of the call (i.e. flushes after). Pulls the aircraft
 * snapshot internally via aircraft_state_snapshot(). */
void pk_adsb_list_render(uint16_t *fb);
