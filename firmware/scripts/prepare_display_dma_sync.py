#!/usr/bin/env python3
"""Generate a project-local DPI driver with a DMA frame selection notification."""

import argparse
import hashlib
from pathlib import Path


EXPECTED_SHA256 = "7a5acacd2be4f4560b2ae85e2414aa3c5c36d313f58e62208e24539ac33e607a"
INCLUDE = '#include "hal/color_hal.h"\n'
DMA_ENABLE = "    dw_gdma_channel_enable_ctrl(chan, true);\n"
UNDERRUN_LOG = '        ESP_DRAM_LOGE(TAG, "can\'t fetch data from external memory fast enough, underrun happens");\n'
UNDERRUN_BUMP = "        pk_display_underrun_bump();\n"
NOTIFICATION = """    // Notify ownership only after DMA adopts the sampled frame buffer.
    if (pk_lcd_dma_frame_selected(&dpi_panel->base, dpi_panel->fbs[fb_index])) {
        yield_needed = true;
    }
"""


def prepare_driver(source: Path, destination: Path) -> None:
    source = Path(source)
    destination = Path(destination)
    if source.resolve() == destination.resolve():
        raise ValueError("DPI overlay destination must not overwrite the SDK driver")
    raw = source.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != EXPECTED_SHA256:
        raise ValueError(
            f"Unsupported ESP-IDF DPI driver SHA256 {digest}; expected {EXPECTED_SHA256}. "
            "Review its DMA callback before updating the overlay."
        )
    text = raw.decode("utf-8")
    for anchor in (INCLUDE, DMA_ENABLE, UNDERRUN_LOG):
        if text.count(anchor) != 1:
            raise ValueError(f"DPI overlay anchor must occur exactly once: {anchor.strip()}")
    # display_dma_sync.h: frame-ownership callback; display.h: underrun counter.
    text = text.replace(
        INCLUDE, INCLUDE + '#include "display_dma_sync.h"\n#include "display.h"\n', 1
    )
    text = text.replace(DMA_ENABLE, DMA_ENABLE + NOTIFICATION, 1)
    # Count the underrun before logging it: the counter is a DRAM atomic in
    # display.c (IRAM_ATTR), so the event stays observable through the
    # periodic status log even when the cache-off console line is lost.
    # ESP_DRAM_LOGE puts the format in DRAM, but its TAG argument remains
    # caller-owned. The cache-safe ISR must not read the Flash TAG literal.
    text = text.replace(
        UNDERRUN_LOG,
        UNDERRUN_BUMP + UNDERRUN_LOG.replace("(TAG,", "(DRAM_STR(TAG),", 1),
        1,
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    generated = text.encode("utf-8")
    if not destination.exists() or destination.read_bytes() != generated:
        destination.write_bytes(generated)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare_driver(args.source, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"DPI DMA overlay: {error}\n")


if __name__ == "__main__":
    main()
