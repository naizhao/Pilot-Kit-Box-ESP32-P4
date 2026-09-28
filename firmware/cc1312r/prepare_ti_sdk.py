#!/usr/bin/env python3
"""Materialize the small part of the locally-installed TI SDK needed to link.

The user's extracted SDK intentionally contains the device headers/libraries and
NoRTOS, but not ``source/ti/drivers``.  The matching official SDK archive is
already installed locally.  Keeping this extraction in ``build/`` avoids
checking hundreds of vendor headers and prebuilt archives into this repository.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from zipfile import ZipFile


SDK_DIR = "simplelink_cc13xx_cc26xx_sdk_8_33_00_16"
DRIVER_PREFIX = f"{SDK_DIR}/source/ti/drivers/"
EXAMPLE_LINKER = (
    f"{SDK_DIR}/examples/nortos/CC1312R1_LAUNCHXL/prop_rf/"
    "rfPacketRx/gcc/cc13x2_cc26x2_nortos.lds"
)
LIBRARIES = {
    f"{DRIVER_PREFIX}lib/gcc/m4f/drivers_cc13x2.a",
    f"{DRIVER_PREFIX}rf/lib/gcc/m4f/rf_multiMode_cc13x2.a",
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("archive", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    if not args.archive.is_file():
        raise SystemExit(f"TI SDK archive not found: {args.archive}")

    args.output.mkdir(parents=True, exist_ok=True)
    with ZipFile(args.archive) as zf:
        names = set(zf.namelist())
        required = LIBRARIES | {EXAMPLE_LINKER}
        missing = sorted(required - names)
        if missing:
            raise SystemExit("TI SDK archive is missing: " + ", ".join(missing))
        selected = [
            name for name in names
            if (name.startswith(DRIVER_PREFIX) and name.endswith(".h"))
            or name in required
        ]
        for name in selected:
            relative = Path(name).relative_to(SDK_DIR)
            target = args.output / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(zf.read(name))

    marker = args.output / ".prepared-8.33.00.16"
    marker.write_text(f"archive={args.archive}\nfiles={len(selected)}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
