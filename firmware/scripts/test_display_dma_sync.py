#!/usr/bin/env python3
"""Compile the real DPI DMA callback to check the frame ownership handoff."""

import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("IDF_PATH", Path.home() / ".espressif/v6.0.1/esp-idf"))
DRIVER = SDK / "components/esp_lcd/dsi/esp_lcd_panel_dpi.c"


def without_comments(source: str) -> str:
    # Keep string literals intact: comments cannot satisfy extraction anchors.
    return re.sub(r'"(?:\\.|[^"\\])*"|/\*.*?\*/|//[^\n]*',
                  lambda match: "" if match[0].startswith(("/*", "//")) else match[0],
                  source, flags=re.DOTALL)


def callback(source: str) -> str:
    start = source.index("bool mipi_dsi_dma_trans_done_cb(")
    end = source.index("\nvoid mipi_dsi_bridge_isr_handler", start)
    return source[start:end]


def bridge_isr(source: str) -> str:
    source = without_comments(source)
    start = source.index("void mipi_dsi_bridge_isr_handler(")
    end = source.index("\nstatic esp_err_t dpi_panel_create_dma_link", start)
    return source[start:end]


BRIDGE_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct { void *bridge; } mipi_dsi_hal_context_t;
typedef struct { mipi_dsi_hal_context_t hal; } bus_t;
typedef struct { int ignored; } esp_lcd_panel_t;
typedef struct {
    esp_lcd_panel_t base;
    bus_t *bus;
    bool (*on_refresh_done)(esp_lcd_panel_t *, void *, void *);
    void *user_ctx;
} esp_lcd_dpi_panel_t;
static bus_t bus;
static esp_lcd_dpi_panel_t panel;
static uint32_t status;
static int cleared, logged, refreshed, yielded, underrun_bumps;
static bool refresh_yield;
static const char *dram_strings[8];
static int dram_count;
static const char *register_dram(const char *string) {
    assert(dram_count < 8);
    dram_strings[dram_count++] = string;
    return string;
}
/* Model the SDK's DRAM_STR address domain, not physical cache hardware.
 * The final P4 ELF must independently confirm its .dram placement. */
#define DRAM_STR(str) (__extension__({static const char value[] = (str); register_dram(value);}))
static bool in_dram(const char *string) {
    for (int i = 0; i < dram_count; i++) {
        if (dram_strings[i] == string) return true;
    }
    return false;
}
static void cache_disabled_log(const char *tag, const char *format) {
    /* A plain TAG literal must fail before the simulated ROM logger reads it. */
    assert(in_dram(tag) && in_dram(format));
    assert(strcmp(tag, "lcd.dsi") == 0);
    assert(strstr(format, "underrun happens") != NULL);
    logged++;
}
#define ESP_DRAM_LOGE(tag, format) cache_disabled_log(tag, DRAM_STR(format))
/* The real bump lives in display.c (IRAM_ATTR, DRAM atomic). unused-safe so
 * that mutation cases deleting the injected call still compile and fail on
 * the behavioral count assertion below. */
static void __attribute__((unused)) pk_display_underrun_bump(void) {
    underrun_bumps++;
}
TAG_DEFINITION
#define MIPI_DSI_BRG_LL_EVENT_UNDERRUN 4
static uint32_t mipi_dsi_brg_ll_get_interrupt_status(void *bridge) {
    assert(bridge == &bus); return status;
}
static void mipi_dsi_brg_ll_clear_interrupt_status(void *bridge, uint32_t bits) {
    assert(bridge == &bus && bits == status); cleared++;
}
static bool refresh(esp_lcd_panel_t *base, void *event, void *ctx) {
    assert(base == &panel.base && event == NULL && ctx == &panel);
    refreshed++; return refresh_yield;
}
#define portYIELD_FROM_ISR() (yielded++)
BRIDGE_ISR
int main(void) {
    bus.hal.bridge = &bus; panel.bus = &bus;
    panel.on_refresh_done = refresh; panel.user_ctx = &panel;
    for (int underrun = 0; underrun < 2; underrun++) {
        for (int vsync = 0; vsync < 2; vsync++) {
            for (int wake = 0; wake < 2; wake++) {
                status = (underrun ? MIPI_DSI_BRG_LL_EVENT_UNDERRUN : 0) |
                         (vsync ? MIPI_DSI_BRG_LL_EVENT_VSYNC : 0);
                cleared = logged = refreshed = yielded = underrun_bumps = dram_count = 0;
                refresh_yield = wake;
                mipi_dsi_bridge_isr_handler(&panel);
                assert(cleared == 1 && logged == underrun);
                assert(underrun_bumps == underrun);
                assert(refreshed == (bool)(status & MIPI_DSI_BRG_LL_EVENT_VSYNC));
                assert(yielded == (refreshed && wake));
            }
        }
    }
    return 0;
}
'''


def run_bridge_isr(source: str, vsync: int) -> subprocess.CompletedProcess:
    private_header = without_comments((DRIVER.parent / "mipi_dsi_priv.h").read_text())
    tag = re.search(r'^#define TAG\s+"[^"\n]+"$', private_header, re.MULTILINE)
    if tag is None:
        raise AssertionError("Real SDK DSI TAG definition not found")
    harness = BRIDGE_HARNESS.replace("TAG_DEFINITION", tag[0]).replace("BRIDGE_ISR", bridge_isr(source))
    with tempfile.TemporaryDirectory(prefix="pk-display-cacheoff-test-") as directory:
        path = Path(directory)
        c_file, executable = path / "bridge.c", path / "bridge"
        c_file.write_text(harness)
        built = subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             f"-DMIPI_DSI_BRG_LL_EVENT_VSYNC={vsync}", str(c_file), "-o", str(executable)],
            capture_output=True, text=True,
        )
        if built.returncode:
            raise AssertionError(built.stderr)
        return subprocess.run([str(executable)], capture_output=True, text=True)


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef void *dw_gdma_channel_handle_t;
typedef void *dw_gdma_link_list_handle_t;
typedef struct { int ignored; } dw_gdma_trans_done_event_data_t;
typedef struct { bool is_valid; bool is_last; } dw_gdma_block_markers_t;
typedef struct { int ignored; } esp_lcd_panel_t;
typedef struct {
    esp_lcd_panel_t base;
    uint8_t cur_fb_index;
    uint8_t *fbs[2];
    void *link_lists[2];
    bool (*on_refresh_done)(esp_lcd_panel_t *, void *, void *);
    void *user_ctx;
} esp_lcd_dpi_panel_t;
static esp_lcd_dpi_panel_t panel;
static int step, notified, refreshed;
static bool notification_yield, refresh_yield;
static unsigned char frames[2], links[2];
static void *dw_gdma_link_list_get_item(void *list, int index) {
    assert(step++ == 0 && index == 0 && list == panel.link_lists[1]);
    return list;
}
static void dw_gdma_lli_set_block_markers(void *item, dw_gdma_block_markers_t markers) {
    assert(step++ == 1 && item == panel.link_lists[1]);
    assert(markers.is_valid && markers.is_last);
}
static void dw_gdma_channel_use_link_list(void *chan, void *list) {
    assert(step++ == 2 && chan == &panel && list == panel.link_lists[1]);
}
static void dw_gdma_channel_enable_ctrl(void *chan, bool enable) {
    assert(step++ == 3 && chan == &panel && enable);
    /* A new request after sampling cannot change the selected frame. */
    panel.cur_fb_index = 0;
}
static bool pk_lcd_dma_frame_selected(esp_lcd_panel_t *base, const void *frame) {
    assert(step++ == 4 && base == &panel.base && frame == panel.fbs[1]);
    notified++;
    return notification_yield;
}
static bool refresh(esp_lcd_panel_t *base, void *event, void *ctx) {
    assert(base == &panel.base && event == NULL && ctx == &panel);
    refreshed++;
    return refresh_yield;
}
CALLBACK
int main(void) {
    panel.fbs[0] = &frames[0]; panel.fbs[1] = &frames[1];
    panel.link_lists[0] = &links[0]; panel.link_lists[1] = &links[1];
    panel.on_refresh_done = refresh; panel.user_ctx = &panel;
    for (int hook = 0; hook < 2; hook++) {
        for (int old = 0; old < 2; old++) {
            panel.cur_fb_index = 1; step = notified = refreshed = 0;
            notification_yield = hook; refresh_yield = old;
            bool yielded = mipi_dsi_dma_trans_done_cb(&panel, NULL, &panel);
            assert(step == 5 && notified == 1);
#if MIPI_DSI_BRG_LL_EVENT_VSYNC
            assert(refreshed == 0 && yielded == (bool)hook);
#else
            assert(refreshed == 1 && yielded == (bool)(hook || old));
#endif
        }
    }
    return 0;
}
'''


def run_callback(source: str, vsync: int) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory(prefix="pk-display-dma-test-") as directory:
        path = Path(directory)
        c_file = path / "callback.c"
        executable = path / "callback"
        c_file.write_text(HARNESS.replace("CALLBACK", callback(source)))
        built = subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", "-Wno-unused-function",
             f"-DMIPI_DSI_BRG_LL_EVENT_VSYNC={vsync}", str(c_file), "-o", str(executable)],
            capture_output=True, text=True,
        )
        if built.returncode:
            raise AssertionError(built.stderr)
        return subprocess.run([str(executable)], capture_output=True, text=True)


def overlay_module():
    script = ROOT / "scripts/prepare_display_dma_sync.py"
    spec = importlib.util.spec_from_file_location("display_dma_overlay", script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DisplayDmaSyncTest(unittest.TestCase):
    def overlay(self, output: Path) -> str:
        script = ROOT / "scripts/prepare_display_dma_sync.py"
        if not script.exists():
            # Before implementation, compile the original driver to expose the
            # missing ownership notification, rather than failing an import.
            return DRIVER.read_text()
        module = overlay_module()
        module.prepare_driver(DRIVER, output)
        return output.read_text()

    def test_selected_frame_notification_after_dma_enable_v1_and_v3(self):
        with tempfile.TemporaryDirectory(prefix="pk-display-overlay-test-") as directory:
            source = self.overlay(Path(directory) / "esp_lcd_panel_dpi.c")
            for vsync in (0, 2):
                with self.subTest(vsync=vsync):
                    result = run_callback(source, vsync)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_bridge_underrun_keeps_tag_and_format_cacheoff_safe_v1_and_v3(self):
        with tempfile.TemporaryDirectory(prefix="pk-display-overlay-test-") as directory:
            source = self.overlay(Path(directory) / "esp_lcd_panel_dpi.c")
            for vsync in (0, 2):
                with self.subTest(vsync=vsync):
                    result = run_bridge_isr(source, vsync)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_bridge_guard_rejects_flash_tag_and_lost_underrun_diagnostic(self):
        with tempfile.TemporaryDirectory(prefix="pk-display-mutation-test-") as directory:
            source = self.overlay(Path(directory) / "esp_lcd_panel_dpi.c")
            safe_log = "ESP_DRAM_LOGE(DRAM_STR(TAG),"
            self.assertEqual(without_comments(source).count(safe_log), 1)
            # Fake documentation cannot hide the removed safety or diagnostic.
            documentation = f"\n/* {safe_log} example only */\n"
            mutations = {
                "Flash tag": source.replace(safe_log, "ESP_DRAM_LOGE(TAG,", 1),
                "diagnostic omitted": source.replace(safe_log, "if (0) " + safe_log, 1),
            }
            for name, mutated in mutations.items():
                for vsync in (0, 2):
                    with self.subTest(mutation=name, vsync=vsync):
                        result = run_bridge_isr(mutated + documentation, vsync)
                        self.assertNotEqual(result.returncode, 0, "Mutation was not detected")

    def test_underrun_count_injected_once_and_removal_is_detected(self):
        module = overlay_module()
        with tempfile.TemporaryDirectory(prefix="pk-display-underrun-mutation-") as directory:
            source = self.overlay(Path(directory) / "esp_lcd_panel_dpi.c")
            bump = module.UNDERRUN_BUMP
            # The count call must be real code, not a promise in a comment.
            self.assertEqual(without_comments(source).count(bump), 1)
            mutated = source.replace(bump, "", 1)
            for vsync in (0, 2):
                with self.subTest(vsync=vsync):
                    result = run_bridge_isr(mutated, vsync)
                    self.assertNotEqual(result.returncode, 0, "Mutation was not detected")

    def test_unknown_sdk_fails_before_touching_existing_output(self):
        with tempfile.TemporaryDirectory(prefix="pk-display-unknown-test-") as directory:
            path = Path(directory)
            source, output = path / "sdk.c", path / "esp_lcd_panel_dpi.c"
            source.write_bytes(DRIVER.read_bytes() + b"\n/* unknown SDK */\n")
            output.write_bytes(b"previous valid overlay")
            with self.assertRaisesRegex(ValueError, "Unsupported ESP-IDF DPI driver SHA256"):
                overlay_module().prepare_driver(source, output)
            self.assertEqual(output.read_bytes(), b"previous valid overlay")

    def test_generator_cannot_overwrite_sdk(self):
        original = DRIVER.read_bytes()
        with self.assertRaisesRegex(ValueError, "must not overwrite the SDK"):
            overlay_module().prepare_driver(DRIVER, DRIVER)
        self.assertEqual(DRIVER.read_bytes(), original)

    def test_unchanged_generation_keeps_build_timestamp(self):
        with tempfile.TemporaryDirectory(prefix="pk-display-idempotent-test-") as directory:
            output = Path(directory) / "esp_lcd_panel_dpi.c"
            module = overlay_module()
            original = DRIVER.read_bytes()
            module.prepare_driver(DRIVER, output)
            before = output.stat().st_mtime_ns
            module.prepare_driver(DRIVER, output)
            self.assertEqual(output.stat().st_mtime_ns, before)
            self.assertEqual(DRIVER.read_bytes(), original)

    def test_behavior_guard_rejects_missing_early_or_wrong_frame_notification(self):
        module = overlay_module()
        with tempfile.TemporaryDirectory(prefix="pk-display-mutation-test-") as directory:
            source = self.overlay(Path(directory) / "esp_lcd_panel_dpi.c")
            self.assertEqual(source.count(module.NOTIFICATION), 1)
            self.assertEqual(source.count(module.DMA_ENABLE + module.NOTIFICATION), 1)
            selected = "dpi_panel->fbs[fb_index]"
            self.assertEqual(source.count(selected), 1)
            mutations = {
                "missing": source.replace(module.NOTIFICATION, "", 1),
                "before DMA enable": source.replace(
                    module.DMA_ENABLE + module.NOTIFICATION,
                    module.NOTIFICATION + module.DMA_ENABLE, 1,
                ),
                "new request instead of sampled frame": source.replace(
                    selected, "dpi_panel->fbs[dpi_panel->cur_fb_index]", 1,
                ),
            }
            for name, mutated in mutations.items():
                for vsync in (0, 2):
                    with self.subTest(mutation=name, vsync=vsync):
                        result = run_callback(mutated, vsync)
                        self.assertNotEqual(result.returncode, 0, "Mutation was not detected")

    def configure_overlay(self, directory: Path, source_entries: list[str]):
        fixture = r'''
cmake_minimum_required(VERSION 3.16)
project(display_overlay_test C)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/other.c" "int other(void) { return 0; }\n")
add_library(esp_lcd STATIC @SOURCE_ENTRIES@ "${CMAKE_CURRENT_BINARY_DIR}/other.c")
function(idf_component_get_property output component property)
    if(property STREQUAL "COMPONENT_LIB")
        set(${output} esp_lcd PARENT_SCOPE)
    elseif(property STREQUAL "COMPONENT_DIR")
        set(${output} "@LCD_DIRECTORY@" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "Unexpected component property ${property}")
    endif()
endfunction()
function(idf_build_get_property output property)
    if(property STREQUAL "BUILD_DIR")
        set(${output} "${CMAKE_CURRENT_BINARY_DIR}" PARENT_SCOPE)
    elseif(property STREQUAL "PYTHON")
        set(${output} "@PYTHON@" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "Unexpected build property ${property}")
    endif()
endfunction()
include("@OVERLAY_CMAKE@")
pk_enable_display_dma_sync()
get_target_property(actual esp_lcd SOURCES)
set(expected "${CMAKE_CURRENT_BINARY_DIR}/display_dma_sync/esp_lcd_panel_dpi.c;${CMAKE_CURRENT_BINARY_DIR}/other.c")
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Wrong source replacement: ${actual}")
endif()
'''
        replacements = {
            "@SOURCE_ENTRIES@": " ".join(f'"{entry}"' for entry in source_entries),
            "@LCD_DIRECTORY@": str(DRIVER.parent.parent),
            "@PYTHON@": sys.executable,
            "@OVERLAY_CMAKE@": str(ROOT / "cmake/display_dma_sync.cmake"),
        }
        for token, value in replacements.items():
            fixture = fixture.replace(token, value)
        directory.mkdir(parents=True)
        (directory / "CMakeLists.txt").write_text(fixture)
        return subprocess.run(
            ["cmake", "-S", str(directory), "-B", str(directory / "build")],
            capture_output=True, text=True,
        )

    def test_cmake_replaces_only_driver_for_relative_and_absolute_sources(self):
        with tempfile.TemporaryDirectory(prefix="pk-display-cmake-test-") as directory:
            for index, entry in enumerate(("dsi/esp_lcd_panel_dpi.c", str(DRIVER))):
                with self.subTest(entry=entry):
                    result = self.configure_overlay(Path(directory) / str(index), [entry])
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cmake_rejects_missing_or_duplicate_driver_sources(self):
        with tempfile.TemporaryDirectory(prefix="pk-display-cmake-invalid-test-") as directory:
            for index, entries in enumerate(([], [str(DRIVER), str(DRIVER)])):
                with self.subTest(entries=entries):
                    result = self.configure_overlay(Path(directory) / str(index), entries)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("Expected exactly one esp_lcd DPI translation unit", result.stderr)


if __name__ == "__main__":
    unittest.main()
