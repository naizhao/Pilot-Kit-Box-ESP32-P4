#!/usr/bin/env python3
"""Prove the RF Core 32-bit correlator + header reconstructs UAT sync36."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "firmware" / "cc1312r" / "uat_radio_config.c"


def c_tokens(text: str) -> str:
    """Remove comments/strings so documentation cannot satisfy this gate."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    return text


source = c_tokens(CONFIG.read_text(encoding="utf-8"))


def numeric_initializer(field: str) -> int:
    match = re.search(rf"\.{re.escape(field)}\s*=\s*(0x[0-9A-Fa-f]+|\d+)u?\b",
                      source)
    assert match, f"missing numeric initializer for {field}"
    return int(match.group(1), 0)


assert numeric_initializer("nSwBits") == 32
assert numeric_initializer("numHdrBits") == 4

adsb_hw = numeric_initializer("syncWord0")
uplink_hw = numeric_initializer("syncWord1")
adsb_full = (adsb_hw << 4) | 0x2
uplink_full = (uplink_hw << 4) | 0xD

assert adsb_full == 0xEACDDA4E2, hex(adsb_full)
assert uplink_full == 0x153225B1D, hex(uplink_full)
assert re.search(r"sync_tail\s*==\s*0x0D[uU]?", c_tokens(
    (ROOT / "firmware" / "cc1312r" / "uat_rf.c").read_text(encoding="utf-8")
)), "uplink 4-bit sync tail is not used by SET_LEN classifier"

print("PASS: CC1312R 32+4 bit configuration rebuilds both UAT sync36 words")
