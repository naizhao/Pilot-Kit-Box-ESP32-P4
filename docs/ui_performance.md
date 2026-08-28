# UI Frame Rate Analysis (4.3″ 800×480)

Measured 2026-08-02 on real hardware: Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3.

中文版：[`ui_performance-zh_CN.md`](ui_performance-zh_CN.md)

## Conclusion first

**The PFD page currently runs at 9 FPS — 116 ms per frame. The bottleneck is
not the display. It is that every pixel operation lands in PSRAM, and the two
most expensive of them are done by the CPU one pixel at a time.**

The panel itself can run at 60 Hz and the DSI link has 2.7× headroom; neither
is a limit (arithmetic below). Of the 116 ms, 70 ms (60%) goes into just two
things: filling the attitude indicator across the full screen, and the
canvas → framebuffer full-screen copy. Neither scales with how complex the
picture is, which is why even the sparse list page also renders at 9 FPS.

## Reproducing these measurements

The instrumentation lives permanently in `pfd.c` behind `ESP_LOGD`, which the
default INFO level suppresses. `CONFIG_LOG_MAXIMUM_LEVEL=4` already compiles
it into the firmware, so **no build configuration change is needed**.

Add one line before the loop in `pfd_task()`, and remove it when you are done
(do not commit it — three lines per second will flood the serial console):

```c
esp_log_level_set(TAG, ESP_LOG_DEBUG);
```

Flash, then capture serial output. Once per second you get:

```
PERF2: att=<attitude> bar=<status bar> tape=<altitude+speed tapes> hsi=<compass+traffic> us/frame
PERF3: ppa=<PPA rotation> vsync_wait=<VSYNC wait> us per flush
PERF:  draw=<page drawing> lvgl=<LVGL compose+flush> (flush=<of which flush> x<per second>) per frame
```

Note that `lvgl` **includes** `flush`, and `draw` is the sum of page drawing
(the `PERF2` items add up to a few milliseconds less — the remainder is
infobox/leftbox/toast synchronization and other odds and ends).

## Measured data (PFD page, demo mode on)

Stable across many consecutive seconds; one representative sample:

```
PERF2: att=33959 bar=1981 tape=8030 hsi=8155 us/frame
PERF3: ppa=15542us vsync_wait=6018us per flush
PERF:  draw=58610us lvgl=57745us (flush=21605us x9) per frame
```

`draw + lvgl = 58.6 + 57.7 = 116.3 ms` → 8.6 FPS, matching the `PFD 9 FPS`
reported in the log.

| Item | Time | Share | Nature |
|---|---:|---:|---|
| Attitude indicator `att` | 34.0 ms | 29% | CPU, per-pixel, full screen |
| LVGL compose blit | 36.1 ms | 31% | CPU, per-pixel (`lvgl − flush`) |
| PPA rotation | 15.5 ms | 13% | Hardware DMA |
| Altitude/speed tapes `tape` | 8.0 ms | 7% | CPU, local rectangles |
| Compass + traffic `hsi` | 8.2 ms | 7% | CPU, local rectangles |
| VSYNC wait | 6.0 ms | 5% | Idle wait |
| Status bar + infobox etc. | ~8 ms | 7% | CPU, local |
| **Total** | **116 ms** | | **8.6 FPS** |

## Why it is slow: convert it to bandwidth

All three buffers live in PSRAM:

| Buffer | Size | Allocated in |
|---|---|---|
| canvas (PFD drawing surface) | 768 KB | `lv_port.c`, `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` |
| Logical framebuffer | 768 KB | `display.c`, `heap_caps_aligned_alloc(..., MALLOC_CAP_SPIRAM)` |
| DPI scan-out double buffer ×2 | 768 KB ×2 | `num_fbs = 2` |

Expressed as effective bandwidth, CPU and hardware differ by 4×:

| Operation | Data moved | Time | Effective bandwidth |
|---|---|---:|---:|
| Attitude fill (CPU per-pixel write) | 768 KB written | 34.0 ms | **23 MB/s** |
| LVGL blit (CPU software copy) | 768 KB read + 768 KB written | 36.1 ms | **42 MB/s** |
| PPA rotation (hardware DMA) | 768 KB read + 768 KB written | 15.5 ms | **97 MB/s** |

The attitude indicator is the single largest drawing item not because it is
visually complex, but because **it is the only one that covers the entire
screen every frame** (sky and ground, two large fills totalling 384,000
pixels). `tape` and `hsi` only paint their own local rectangles, hence 8 ms.

## The display is ruled out

From `video_timing` in `display.c` and the constants in `display.h`:

- Total line width = 480 + 42 (hbp) + 12 (hpw) + 42 (hfp) = **576**
- Total lines = 800 + 2 (vbp) + 8 (vpw) + 60 (vfp) = **870**
- Per frame: 576 × 870 = 501,120 clk, with `PK_LCD_DPI_CLOCK_MHZ = 30`
- Refresh = 30 MHz ÷ 501,120 = **59.9 Hz**

DSI link: 2 lanes × 500 Mbps = 125 MB/s; 60 FPS needs only 768 KB × 60 = 46 MB/s.

**Both the panel and the link have ample headroom. Fitting a smaller screen
will not raise the frame rate.**

## Why every page renders at 9 FPS

`lvgl + flush = 57.7 ms` is **completely independent** of what the page draws:

1. `pk_lv_port_invalidate()` in `lv_port.c` calls `lv_obj_invalidate(s_canvas)`
   every frame, marking the whole canvas dirty
2. LVGL therefore composes the full screen every frame → a 768 KB blit
3. PPA rotates the full screen
4. Wait for VSYNC

So the list page (a few dozen rectangles and text runs) and the PFD page
(attitude indicator + two tapes + compass + traffic) render at the same rate.
**Any effort that only optimizes _what_ is drawn is capped by these 58 ms.**

The `vTaskDelayUntil(pdMS_TO_TICKS(33))` at the end of the `pfd.c` loop (a
30 FPS target) has long been a no-op — a frame far exceeds 33 ms, so it
returns immediately every time.

## Optimization directions (by payoff; none implemented)

### 1. Eliminate the canvas → framebuffer blit — saves about 36 ms

`lv_port.c` already records a **failed** attempt at this; read it before
starting. It tried having the PFD draw straight into the display buffer, with
the screen background set to `LV_OPA_TRANSP` and only the FAB/dock marked
dirty each frame. On hardware the FAB gained a white halo and flickered
continuously against the dock. Two fundamental problems:

- RGB565 has no alpha channel. In DIRECT mode, LVGL needs an opaque backdrop
  to redraw a widget's dirty region; a transparent screen provides none, so
  rounded corners and semi-transparent areas fill with undefined colors.
- The PFD covers the whole screen every frame, but LVGL has its own refresh
  period (`LV_DEF_REFR_PERIOD`) and does not actually redraw on every
  `lv_timer_handler` call — so on some frames widgets are drawn, and on others
  the PFD paints over them.

That file's conclusion: **to remove this blit, widgets and the PFD must be
composed in a single pass** (e.g. drop LVGL layers and blend widget pixels
yourself). Adjusting the dirty region cannot solve it. This is the largest
single win and also the largest change.

### 2. Move the attitude fill to hardware — saves about 26 ms

Sky and ground are two regular regions, currently written to PSRAM by the CPU
one pixel at a time (23 MB/s). PPA measures at 97 MB/s, so the same 768 KB
takes roughly 8 ms as a hardware fill — **saving about 26 ms**.

Note that the horizon is slanted (it tilts with roll), so these are not two
upright rectangles. A workable approach is to hardware-fill the two blocks
along a horizontal split first, then have the CPU touch only the band
containing the slanted boundary, rather than the whole screen.

### 3. PPA rotation at 15.5 ms is slower than expected — possibly a few ms

97 MB/s is not fast for moving 1.5 MB. Worth investigating:
`PPA_TRANS_MODE_BLOCKING` (a synchronous wait during which the CPU spins),
and whether the color mode and memory alignment take the optimal path.
Switching to asynchronous operation plus double buffering would let the CPU
draw the next frame while PPA works.

### 4. VSYNC idle wait of 6 ms

After submitting a flush, `display.c` spins until the refresh counter changes.
This becomes more prominent once the frame rate rises (a frame is 16.7 ms at
60 Hz). It can likewise be overlapped with drawing by going asynchronous.

**Doing only 1 and 2 brings 116 ms down to roughly 54 ms (≈18 FPS); all four
should approach 30 FPS.**

## Ruled out — do not revisit

- **The screen is too large / fit a smaller one**: the panel does 60 Hz and
  the DSI link has 2.7× headroom. Neither is the limit.
- **Raise the `vTaskDelayUntil` target frame rate**: it is already a no-op;
  lowering the value changes nothing.
- **Optimize the drawing of one particular page**: the 58 ms of fixed overhead
  is independent of page content.

## Measurement conditions

- Firmware built 2026-08-02 from what is now the `v4` branch (then named
  `feat/lcd-4.3-touch`), including every change in the working tree at the time.
- Demo mode on (synthetic data). Demo data affects only the values displayed,
  not the drawing path or the pixel count.
- Page under test: the PFD home page. The list page was observed at the same
  9 FPS but was not broken down item by item — to get a breakdown for another
  page, repeat the procedure above.
