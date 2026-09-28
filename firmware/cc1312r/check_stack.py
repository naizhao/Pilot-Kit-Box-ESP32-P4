#!/usr/bin/env python3
"""Fail the CC1312R build when application stack frames exceed NoRTOS budget."""

from __future__ import annotations

import re
import sys
from pathlib import Path


build = Path(sys.argv[1] if len(sys.argv) > 1 else "build")
linker = Path(sys.argv[2]) if len(sys.argv) > 2 else None
usage: dict[str, int] = {}
for report in sorted(build.glob("*.su")):
    for line in report.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) < 3:
            continue
        function = fields[0].rsplit(":", 1)[-1]
        try:
            size = int(fields[1])
        except ValueError:
            continue
        usage[function] = max(size, usage.get(function, 0))

required = {
    "main": 128,
    "cc13s_txn_done": 256,
    "rf_frame_received": 128,
    "rf_callback": 160,
    "uat_rf_poll": 160,
    "rp_cc13xx_encode_rx_chunk": 640,
}
for function, limit in required.items():
    assert function in usage, f"stack gate: missing report for {function}"
    assert usage[function] <= limit, (
        f"stack gate: {function} uses {usage[function]} B > {limit} B"
    )

# Worst application-owned synchronous route from main into a maximum-size
# chunk encoder.  The TI library's own frames are outside compiler-visible
# reports, so retain at least 256 B of the configured 1 KiB stack for them.
chain_names = (
    "main",
    "cc13s_txn_done",
    "rp_cc13xx_encode_rx_chunk",
    "rp_cc13xx_encode.part.0",
)
chain = sum(usage.get(name, 0) for name in chain_names)
assert chain <= 768, f"stack gate: max SPI encode chain {chain} B > 768 B"

if linker:
    lds = linker.read_text(encoding="utf-8")
    match = re.search(r"\bSTACKSIZE\s*=\s*(\d+)\s*;", lds)
    assert match and int(match.group(1)) >= 1024, "stack gate: linker stack < 1 KiB"

print(f"PASS: CC1312R stack frames; max SPI encode chain={chain} B")
