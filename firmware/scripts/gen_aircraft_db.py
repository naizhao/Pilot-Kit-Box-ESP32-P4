#!/usr/bin/env python3
"""
gen_aircraft_db.py — Build the SD-card aircraft database pk_actdb.bin from tar1090-db.

The mictronics-maintained tar1090-db (github.com/wiedehopf/tar1090-db) is a
sharded archive of every aircraft ever observed by the network, keyed by the
24-bit ICAO transponder address. Each shard is a gzipped JSON dict whose keys
are the last 3 hex chars of the ICAO24 and whose values are
[registration, type_code, flags, model_name] arrays.

Per-type technical descriptors (e.g. "L2J" = Land 2-Jet, "H2T" = Helicopter
2-Turboprop) come from the companion ICAO Doc 8643 designator table. Upstream
moved it: it used to be icao_aircraft_types.json at the repo root, and is now
db/icao_aircraft_types.js — same JSON body, but gzip-compressed. The --types
argument still wants plain JSON, so the fetch recipe below gunzips it.

We DELIBERATELY drop the per-aircraft `flags` field (military / VIP /
PIA / LADD bits). Identifying military aircraft from a civilian receiver
is legally fraught in some jurisdictions (specifically PRC — surveillance
of military aircraft is treated as espionage under domestic law). The
firmware therefore never surfaces those bits.

Output binary format (little-endian, ESP32-P4 native).

The file the box reads is `/sdcard/aero/pk_actdb.bin` = a 64-byte container
header + the PKADB1 payload described below. The container is byte-for-byte
the same shape as pk_aero.bin's (see firmware/main/pk_aero_reader.c), so the
firmware reuses one header-parse + SHA-256-verify code path for both:

  Container header (64 B) — written by write_container()
    off 0   char   magic[6]      "PKACT1"
    off 6   uint16 version       = 1
    off 8   char   cycle[8]      build date "YYYYMMDD" (NUL-padded)
    off 16  uint16 n_sections    = 0   (single blob, no section table)
    off 18  uint32 sections_off  = 64
    off 22  uint8  enc_algo      = 0   (NOT encrypted — see below)
    off 23  uint8  _reserved     = 0
    off 24  uint8  nonce[8]      = 0   (field kept for layout parity)
    off 32  uint8  sha256[32]    SHA-256 of the payload
    off 64  payload              = align16(sections_off + n_sections*32)

Not encrypted on purpose: tar1090-db is public data, and skipping AES-CTR
saves decrypting 8 MiB on every boot. pk_aero.bin keeps its encryption; the
two files are deliberately separate because tar1090-db refreshes weekly while
the aeronautical data follows the 28-day AIRAC cycle — merging them would
force one to wait on the other.

  Payload (PKADB1) — unchanged from the EMBED_FILES era:

  Header (32 B)
    char     magic[6]     = "PKADB1"
    uint16   version      = 2
    uint32   n_records              -- count of aircraft entries
    uint32   records_off            -- file offset of records[]
    uint16   n_types                -- count of type table entries
    uint16   _reserved
    uint32   types_off              -- file offset of types[]
    uint32   strings_off            -- file offset of strings pool
    uint32   strings_size           -- bytes in strings pool

  records[] @ records_off           -- sorted ASCENDING by icao24, packed
    struct {
      uint8  icao24_be[3]           -- 24-bit ICAO address, big-endian
      uint16 type_idx               -- index into types[]; 0xFFFF = unknown
      uint8  reg_off_be[3]          -- 24-bit BE offset into strings pool
                                       for the registration tail, or 0 if
                                       the source had no reg field
    } __packed                      -- 8 bytes each

  types[] @ types_off               -- indexed by record.type_idx
    struct {
      uint32 code_off               -- → "B738\0"
      uint32 model_off              -- → "BOEING 737-800\0"
      uint32 desc_off               -- → "L2J\0" (ICAO Doc 8643 designator),
                                       or 0 if no desc available
    }                               -- 12 bytes each

  strings @ strings_off             -- packed NUL-terminated ASCII strings.
                                     Byte 0 is reserved as the "no string"
                                     sentinel — actual strings start at
                                     offset 1.

Runtime lookup is a binary search over records[], O(log n_records) ≈ 19 cmps
for ~600k entries. Strings are referenced by offset so the loader doesn't need
to fix up pointers — the firmware mmaps the blob and uses it in place.

Usage:
    firmware/scripts/gen_aircraft_db.py \\
        --db-dir   /tmp/tar1090-db \\
        --types    /tmp/types.json
    # --out defaults to <repo>/datafiles/data/pk_actdb.bin

Then copy the result to the SD card as /sdcard/aero/pk_actdb.bin
(see datafiles/README.md for the full SD-card layout).
Pass --no-container to emit the bare PKADB1 payload instead (only useful for
diffing against an old EMBED_FILES-era aircraft_db.bin).

If --db-dir is missing or empty, the script prints the curl recipe.
"""

from __future__ import annotations

import argparse
import collections
import datetime
import glob
import gzip
import hashlib
import json
import os
import struct
import sys
import time
from pathlib import Path


# 仓库根（firmware/scripts/ 往上两级）。默认产物落在仓库内 datafiles/data/，
# 不写死绝对路径，脚本从任何 cwd 调用都指向同一个地方。见 datafiles/README.md。
REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / "datafiles" / "data" / "pk_actdb.bin"

MAGIC = b"PKADB1"
VERSION = 2

# ---- container (see module docstring; mirrors pk_aero.bin's header) --------
CONTAINER_MAGIC   = b"PKACT1"
CONTAINER_VERSION = 1
CONTAINER_HEADER  = 64
ENC_NONE          = 0


def write_container(f, payload: bytes, cycle: str) -> None:
    """Write the 64 B container header followed by `payload`.

    Deliberately hand-rolled rather than imported from the aero pipeline's
    export_box_bin.py: that script lives in a different repo, and this header
    is 8 fields long. Keeping it here means gen_aircraft_db.py stays a
    single self-contained file, which is how every other script under
    firmware/scripts/ works.
    """
    hdr = bytearray(CONTAINER_HEADER)
    hdr[0:6]   = CONTAINER_MAGIC
    struct.pack_into("<H", hdr, 6, CONTAINER_VERSION)
    hdr[8:16]  = cycle.encode("ascii")[:8].ljust(8, b"\0")
    struct.pack_into("<H", hdr, 16, 0)                  # n_sections
    struct.pack_into("<I", hdr, 18, CONTAINER_HEADER)   # sections_off
    hdr[22] = ENC_NONE
    # hdr[23] _reserved, hdr[24:32] nonce — both stay zero (not encrypted).
    hdr[32:64] = hashlib.sha256(payload).digest()
    # payload_off = align16(sections_off + n_sections*32) == 64: the header is
    # already 16-aligned, so no padding is needed. Assert rather than compute —
    # a silent mismatch here would make the firmware read the payload at the
    # wrong offset and report "bad magic".
    assert (CONTAINER_HEADER + 0 * 32 + 15) // 16 * 16 == CONTAINER_HEADER
    f.write(hdr)
    f.write(payload)

# 32 bytes; little-endian (native ESP32-P4 byte order).
HEADER_FMT = "<6sH I I H H I I I"
# 8 bytes packed: 3 ICAO BE + 2 type_idx LE + 3 reg_off BE.
# Python's struct can't natively express the mixed-endian 3-byte field, so we
# pack each record byte-by-byte at write time.

assert struct.calcsize(HEADER_FMT) == 32, struct.calcsize(HEADER_FMT)

RECORD_SIZE  = 8
TYPE_NO_TYPE = 0xFFFF
STR_NONE     = 0          # offset 0 = "no string" sentinel; pool starts at 1


def load_all_shards(db_dir: str) -> dict[int, tuple[str | None, str | None, str | None]]:
    """Read every <prefix>.js[.gz] / <prefix>.json under db/. Returns
    {icao24_int: (type_code, model_name, registration)} where each field may
    be None. We keep records that have EITHER type or reg — both are
    individually useful for the detail pane."""
    out: dict[int, tuple[str | None, str | None, str | None]] = {}
    paths = sorted(glob.glob(os.path.join(db_dir, "*.js"))) \
        + sorted(glob.glob(os.path.join(db_dir, "*.json")))
    for path in paths:
        prefix = os.path.basename(path).split(".")[0]
        # Skip auxiliary files — only hex-prefix shards.
        if not all(c in "0123456789ABCDEFabcdef" for c in prefix):
            continue
        try:
            if path.endswith(".js"):
                with gzip.open(path, "rt") as f:
                    d = json.load(f)
            else:
                with open(path) as f:
                    d = json.load(f)
        except Exception as e:
            sys.stderr.write(f"  skip {path}: {e}\n")
            continue
        if not isinstance(d, dict):
            continue
        for suffix, rec in d.items():
            if not isinstance(rec, list) or len(rec) < 4:
                continue
            reg, tc, _flags, md = rec[:4]
            if not isinstance(tc, str) or not tc:
                tc = None
            else:
                tc = tc.upper()
            if not isinstance(md, str) or not md:
                md = None
            else:
                md = md.upper()
            if not isinstance(reg, str) or not reg:
                reg = None
            if tc is None and reg is None:
                continue
            try:
                icao24 = int(prefix + suffix, 16)
            except ValueError:
                continue
            if not (0 <= icao24 <= 0xFFFFFF):
                continue
            out[icao24] = (tc, md, reg)
    return out


def load_type_descs(path: str) -> dict[str, str]:
    """Load ICAO Doc 8643 type descriptors from icao_aircraft_types.json.
    Format: {"B738": {"desc": "L2J", "wtc": "M"}, ...}. We only keep `desc`;
    `wtc` is redundant with what the firmware already extracts from ADS-B
    DF17 metype 4. Returns {TYPE: desc}."""
    if not os.path.exists(path):
        sys.stderr.write(f"warning: --types {path!r} not found, skipping descs\n")
        return {}
    out: dict[str, str] = {}
    d = json.load(open(path))
    for k, v in d.items():
        if isinstance(v, dict):
            desc = v.get("desc")
            if isinstance(desc, str) and desc:
                out[k.upper()] = desc
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--db-dir", default="/tmp/tar1090-db",
                    help="path to a tar1090-db checkout / extract")
    ap.add_argument("--types", default="/tmp/types.json",
                    help="path to icao_aircraft_types.json")
    ap.add_argument("--out", default=str(DEFAULT_OUT),
                    help="output .bin path (default: <repo>/datafiles/data/pk_actdb.bin)")
    ap.add_argument("--no-container", action="store_true",
                    help="emit the bare PKADB1 payload, without the container header")
    ap.add_argument("--cycle", default=None,
                    help="8-char cycle/date stamp for the container header "
                         "(default: today, YYYYMMDD)")
    args = ap.parse_args()
    t_start = time.monotonic()

    if not os.path.isdir(args.db_dir):
        sys.stderr.write(
            f"db-dir {args.db_dir!r} not found.\n\n"
            "Fetch tar1090-db first:\n"
            "  mkdir -p /tmp/tar1090-db && cd /tmp/tar1090-db\n"
            "  curl -sL https://api.github.com/repos/wiedehopf/tar1090-db/contents/db \\\n"
            "    | python3 -c \"import sys,json;[print(i['name']) for i in json.load(sys.stdin) if i['name'].endswith('.js')]\" \\\n"
            "    | xargs -I {} -P 16 curl -sLO https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/{}\n"
            # 类型表上游已从仓库根的 icao_aircraft_types.json 挪到 db/ 下、
            # 且改成 gzip（旧地址现在是 404）。这里下下来直接解压成纯 JSON，
            # --types 那一侧不用动。
            "  curl -sL https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/icao_aircraft_types.js \\\n"
            "    | gunzip > /tmp/types.json\n"
            "Then re-run this script.\n")
        return 1

    print(f"loading shards from {args.db_dir} ...")
    records = load_all_shards(args.db_dir)
    print(f"  records (type or reg present): {len(records):,}")

    print(f"loading type descs from {args.types} ...")
    desc_for_type = load_type_descs(args.types)
    print(f"  type descs available: {len(desc_for_type):,}")

    # ---- Build type table ------------------------------------------------
    # Canonical model name per type = most common model_name across all
    # aircraft of that type. Falls back to the type code itself when no
    # aircraft carried a model string.
    per_type_models: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    type_seen: set[str] = set()
    for _icao, (tc, md, _reg) in records.items():
        if tc is None: continue
        type_seen.add(tc)
        if md: per_type_models[tc][md] += 1

    canonical_model: dict[str, str] = {}
    for tc in type_seen:
        ctr = per_type_models.get(tc)
        canonical_model[tc] = ctr.most_common(1)[0][0] if ctr else tc

    type_list = sorted(type_seen)
    type_idx_map: dict[str, int] = {tc: i for i, tc in enumerate(type_list)}
    print(f"  unique types: {len(type_list):,}")
    has_desc = sum(1 for tc in type_list if tc in desc_for_type)
    print(f"  types with desc: {has_desc:,} ({100*has_desc/len(type_list):.0f}%)")

    # ---- String pool -----------------------------------------------------
    # Byte 0 reserved as the "no string" sentinel — keeps offset==0 cleanly
    # distinguishable from a valid first-string offset.
    str_pool = bytearray(b"\0")
    str_offsets: dict[str, int] = {}

    def intern(s: str) -> int:
        if not s: return STR_NONE
        if s in str_offsets: return str_offsets[s]
        off = len(str_pool)
        str_offsets[s] = off
        str_pool.extend(s.encode("ascii", errors="replace"))
        str_pool.append(0)
        return off

    type_code_offs  = [intern(tc) for tc in type_list]
    type_model_offs = [intern(canonical_model[tc]) for tc in type_list]
    type_desc_offs  = [intern(desc_for_type.get(tc, "")) for tc in type_list]

    # Intern each registration. Per-aircraft strings dominate the pool size.
    reg_offs: dict[int, int] = {}
    for icao, (_tc, _md, reg) in records.items():
        if reg:
            reg_offs[icao] = intern(reg)

    print(f"  string pool: {len(str_pool):,} bytes "
          f"({len(str_offsets):,} unique strings)")
    # Sanity: 3-byte offset can address up to 16 MiB. Pool MUST fit.
    if len(str_pool) >= (1 << 24):
        sys.stderr.write(f"ERROR: string pool {len(str_pool):,} B exceeds "
                         f"3-byte offset range (16 MiB)\n")
        return 1

    # ---- Records ---------------------------------------------------------
    sorted_icaos = sorted(records.keys())
    records_size = len(sorted_icaos) * RECORD_SIZE
    types_size   = len(type_list) * 12
    strings_size = len(str_pool)
    header_size  = struct.calcsize(HEADER_FMT)

    records_off = header_size
    types_off   = records_off + records_size
    strings_off = types_off   + types_size

    # Assemble the payload in memory: it has to be hashed as a whole for the
    # container's SHA-256 anyway, and 8 MiB is nothing on a build host.
    payload = bytearray()
    payload += struct.pack(HEADER_FMT,
                           MAGIC, VERSION,
                           len(sorted_icaos), records_off,
                           len(type_list), 0,
                           types_off, strings_off, strings_size)

    # records — 8 bytes each, packed by hand to interleave BE icao,
    # LE type_idx and BE reg_off.
    for icao in sorted_icaos:
        tc, _md, _reg = records[icao]
        type_idx = type_idx_map[tc] if tc is not None else TYPE_NO_TYPE
        reg_off  = reg_offs.get(icao, STR_NONE)
        buf = bytearray(RECORD_SIZE)
        buf[0] = (icao >> 16) & 0xFF
        buf[1] = (icao >>  8) & 0xFF
        buf[2] =  icao        & 0xFF
        buf[3] =  type_idx       & 0xFF        # type_idx little-endian
        buf[4] = (type_idx >> 8) & 0xFF
        buf[5] = (reg_off >> 16) & 0xFF        # reg_off big-endian
        buf[6] = (reg_off >>  8) & 0xFF
        buf[7] =  reg_off        & 0xFF
        payload += buf

    # types — 3 little-endian uint32 offsets per entry.
    for code_off, model_off, desc_off in zip(type_code_offs,
                                             type_model_offs,
                                             type_desc_offs):
        payload += struct.pack("<III", code_off, model_off, desc_off)

    payload += str_pool

    # The firmware trusts these offsets after only a bounds check, so verify
    # the layout we just wrote actually matches the header we just packed.
    assert len(payload) == strings_off + strings_size, \
        (len(payload), strings_off + strings_size)

    cycle = args.cycle or datetime.date.today().strftime("%Y%m%d")
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        if args.no_container:
            f.write(payload)
        else:
            write_container(f, bytes(payload), cycle)

    sz = os.path.getsize(args.out)
    print(f"\nwrote {args.out}  ({sz:,} bytes ≈ {sz/1024/1024:.2f} MB)"
          f"  in {time.monotonic() - t_start:.1f} s")
    if not args.no_container:
        print(f"  container {CONTAINER_HEADER} B  (magic PKACT1 v{CONTAINER_VERSION}, "
              f"cycle {cycle}, enc_algo 0)")
        print(f"  sha256   {hashlib.sha256(bytes(payload)).hexdigest()}")
    print(f"  payload  {len(payload):,} B")
    print(f"    header   {header_size} B")
    print(f"    records  {records_size:,} B  ({len(sorted_icaos):,} × 8 B)")
    print(f"    types    {types_size:,} B    ({len(type_list):,} × 12 B)")
    print(f"    strings  {strings_size:,} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
