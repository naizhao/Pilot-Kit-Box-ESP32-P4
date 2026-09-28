#!/usr/bin/env python3
"""Static safety gate for the CC1312R application-owned RF command graph."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CC = ROOT / "firmware" / "cc1312r"


def c_tokens(text: str) -> str:
    """Drop comments and literal contents before matching executable tokens."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    text = re.sub(r"'(?:\\.|[^'\\])*'", "''", text)
    return text


sources = "\n".join(
    c_tokens(path.read_text(encoding="utf-8"))
    for path in sorted(CC.glob("*.c"))
)
main_source = c_tokens((CC / "main.c").read_text(encoding="utf-8"))
platform_source = c_tokens((CC / "ti_platform.c").read_text(encoding="utf-8"))

for forbidden in (
    r"\bCMD_PROP_TX(?:_ADV)?\b",
    r"\bCMD_TX_TEST\b",
    r"\bRF_runDirectCmd\b",
    r"\.bTxMode\s*=\s*1\b",
):
    assert not re.search(forbidden, sources), f"RX-only gate: found {forbidden}"

for required in (
    r"\bCMD_PROP_RX_ADV\b",
    r"\bCMD_PROP_SET_LEN\b",
    r"\bRF_postCmd\b",
    r"\bRF_runImmediateCmd\b",
    r"\bRF_cancelCmd\s*\(",
    r"0xEACDDA4Eu",
    r"0x153225B1u",
    r"\.maxPktLen\s*=\s*0\b",
    r"\bNoRTOS_start\s*\(",
    r"\bWatchdogEnable\s*\(",
    r"\bcc13s_push_rx\s*\(",
    r"\birq_set\s*\(\s*cc13s_irq\s*\(",
    # SSI slave must remain reliable while the RF driver runs HWI/SWI work.
    # A CPU-polled eight-byte FIFO overruns/underruns on real hardware as soon
    # as continuous RX starts, so both directions must be a 512-byte uDMA
    # transaction armed before FSS falls.
    r"\buDMAControlBaseSet\s*\(",
    r"\buDMAChannelTransferSet\s*\(",
    r"\bSSIDMAEnable\s*\(",
):
    assert re.search(required, sources), f"RX-only gate: missing {required}"

assert not re.search(r"\bSSIDataGet\s*\(", main_source), (
    "SSI slave transport must not drain 512-byte transactions by CPU polling"
)
assert "uDMAChannelModeGet" not in main_source, (
    "SSI slave completion must follow CSN deassert; waiting for an exact DMA "
    "STOP can deadlock with tail bytes still in the RX FIFO"
)
for required in (
    r"\bRF_getRssi\s*\(",
    r"\buat_phy_on_liveness_probe\s*\(",
    r"\buat_phy_recovery_fault\s*\(",
    r"\buat_phy_recovery_step\s*\(",
    r"\buat_phy_recovery_cancel_result\s*\(",
    r"\bUAT_RF_PROBE_INTERVAL_SECONDS\b",
    r"\bRF_StatBusyError\b",
    r"\buat_rf_reset_required\s*\(",
    r"\bSysCtrlSystemReset\s*\(",
):
    assert re.search(required, sources), (
        "continuous RX must wire rate-limited RF liveness/RSSI and the "
        f"non-blocking recovery state machine: missing {required}"
    )
assert re.search(r"\.swiPriority\s*=\s*0\b", platform_source), (
    "RF Driver SWI must use the SDK/SysConfig lowest-priority default (0); "
    "a higher non-preemptive NoRTOS SWI can starve priority-0 system work"
)
assert re.search(r"\.hwiPriority\s*=\s*INT_PRI_LEVEL6\b", platform_source), (
    "NoRTOS reserves INT_PRI_LEVEL7 for its SwiP scheduler; RF HWI must use "
    "LEVEL6 or HwiP_construct rejects the interrupt handler"
)

print("PASS: CC1312R command graph is RX-only and wires RF→SPI")
