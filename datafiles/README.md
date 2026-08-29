# datafiles/ — Box Offline Data Workspace

Chinese version: [`README-zh_CN.md`](README-zh_CN.md)

All offline data the box reads at runtime lives here, instead of being scattered across `tmp/`, `firmware/main/`, and elsewhere.

**These files are not committed to git** (the root `.gitignore` ignores `datafiles/**/*.bin` and
`datafiles/**/*.pmtiles`); only the directory structure and this document are tracked. The reason is size: the four
basemap packages total 3.4 GB, with the largest single package at 1.4 GB; each data bin is 8~17 MB. All of them are
artifacts regenerable by scripts — not a single one is hand-written source.

```
datafiles/
├── README.md      ← this file
├── data/          aviation/aircraft data bins, corresponding to /aero/ on the SD card
└── maps/          offline basemap pmtiles packages, corresponding to /maps/ on the SD card
```

---

## Directory Layout on the SD Card

The box mounts the card at `/sdcard` and only recognizes the following fixed paths (all hard-coded constants in the firmware):

| SD Card Path | Source in This Repo | Constant in Firmware |
|---|---|---|
| `/aero/pk_aero.bin`  | `datafiles/data/pk_aero.bin`  | `AERO_BIN_PATH` (`firmware/main/pk_aero_db.c:56`) |
| `/aero/pk_actdb.bin` | `datafiles/data/pk_actdb.bin` | `ACTDB_PATH` (`firmware/main/aircraft_db.c:67`) |
| `/maps/*.pmtiles`    | `datafiles/maps/*.pmtiles`    | `MAP_DIR` (`firmware/main/pk_tile_loader.c:30`) |

Flashing the card means copying the two active bins from `data/` into `/aero/` and the packages from `maps/` into `/maps/`,
keeping the file names unchanged (`/maps` is scanned as a directory, so file names are not part of routing, but the other
two are hard-coded paths).

The microSD must be inserted in Slot 0 — ESP-Hosted occupies the only SDMMC controller; in the wrong slot no card will
be detected at all.

---

## data/ — Aviation / Aircraft Data

| File | Size | Format | Status |
|---|---|---|---|
| `pk_aero.bin` | 11,450,416 B (10.92 MB) | `PKAER1` v3, cycle `2026-02`, 9 segments | **Active**, matches the card inside the box |
| `pk_actdb.bin` | 8,604,583 B (8.21 MB) | `PKACT1` container + `PKADB1` v2, cycle `20260802` | **Active** |
| `pk_aero_v4.bin` | 17,438,040 B (16.63 MB) | `PKAER1` v4, cycle `2026-08`, 14 segments | **Firmware does not support it yet; do not copy it to the SD card** |
| `aircraft_db.bin` | 8,560,452 B (8.16 MB) | Bare `PKADB1` v2 payload (no container), 570,141 records | **Legacy, no longer part of the build** |

### pk_aero.bin — Aviation Data (airports/runways/navaids/waypoints/airspace)

The generator does not live in this repo: it is
`scripts/aero_data_pipeline/export_box_bin.py` in the Pilot-Kit repo. The authoritative format definition is that script;
the parser on this repo's side is `firmware/main/pk_aero_reader.c/.h`, and the two sides stay aligned via file-header
comments. Updated on the 28-day AIRAC cycle.

**Do not use the v4 file yet**: `firmware/main/pk_aero_reader.h:26` states that init only accepts
`version ∈ {2, 3}`; the v4 header has version=4 and the segment count grows from 9 to 14. Copying it straight to the card
will make the firmware refuse to load it. It is kept here so that a real sample is at hand when adapting the firmware.

### pk_actdb.bin — ICAO24 Aircraft Database

Produced by this repo itself:

```bash
# 1) Fetch the upstream tar1090-db (the script prints this recipe if --db-dir does not exist)
mkdir -p /tmp/tar1090-db && cd /tmp/tar1090-db
curl -sL https://api.github.com/repos/wiedehopf/tar1090-db/contents/db \
  | python3 -c "import sys,json;[print(i['name']) for i in json.load(sys.stdin) if i['name'].endswith('.js')]" \
  | xargs -I {} -P 16 curl -sLO https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/{}
curl -sL https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/icao_aircraft_types.js \
  | gunzip > /tmp/types.json

# 2) Build the package (--out defaults to datafiles/data/pk_actdb.bin, no need to specify)
firmware/scripts/gen_aircraft_db.py --db-dir /tmp/tar1090-db --types /tmp/types.json
```

The upstream Doc 8643 type table has moved: the old address (`icao_aircraft_types.json` at the repo root)
is now a 404; the new address is `db/icao_aircraft_types.js` and it is gzip-compressed, hence the `gunzip`
into plain JSON before feeding it to `--types`. tar1090-db updates weekly, out of sync with pk_aero.bin's 28-day AIRAC
cycle — the two files are deliberately kept separate so that neither one has to wait for the other.

### aircraft_db.bin — Retired

It used to be embedded into the firmware `.rodata` via `EMBED_FILES`, taking up a large chunk of the factory partition.
After the aircraft database moved to the SD card with lazy loading (see the file header of `firmware/main/aircraft_db.c`), the `EMBED_FILES` line was deleted,
no `_binary_aircraft_db_bin` reference remains anywhere in the repo, and partition usage dropped from 91% to 23%.

It is a **bare payload** without the 64-byte container header — not the same kind of file as `pk_actdb.bin`, and it must
not be copied to the SD card. There is exactly one reason to keep it: diffing a newly generated database against this
old snapshot for reconciliation (`gen_aircraft_db.py
--no-container` produces the comparable isomorphic file). Once reconciliation is no longer needed, it can be deleted.

---

## maps/ — Offline Basemap

Raster basemap packages in PMTiles format, dark theme. At startup the firmware scans `/sdcard/maps` and routes by (z,x,y)
across multiple packages: in overlapping areas the one with the deepest maxzoom wins; where nothing reaches, the parent
tile is zoomed in (overzoom) and a hint badge is shown.

| File | Size | Coverage |
|---|---|---|
| `pk_map_global.pmtiles` | 584,152,227 B (557 MB) | Global z0-9 |
| `pk_map_cn.pmtiles` | 1,459,408,344 B (1.36 GB) | China z10-12 |
| `pk_map_us_conus.pmtiles` | 1,365,682,229 B (1.27 GB) | CONUS z10-12 |
| `pk_map_prd_pilot.pmtiles` | 14,016,795 B (13.4 MB) | Pearl River Delta pilot z0-12, bounds `112.5,21.5,114.6,23.5` |

Package pipeline: tileserver-gl rendering → MBTiles → `pmtiles convert`. You can also copy the packages straight back
from a flashed SD card's `/maps/`.

---

## Who Reads This Directory

Before moving these files, take a look here — the default paths below all point here:

| Location | Default | How to Override |
|---|---|---|
| `firmware/test/test_pk_pmtiles.c` | `datafiles/maps` | Environment variable `PK_MAP_TEST_DATA_DIR` |
| `firmware/test/test_pk_map_store.c` | `datafiles/maps` | Environment variable `PK_MAP_TEST_DATA_DIR` |
| `sim/compat/pk_tile_loader_sim.c` | `datafiles/maps` | Environment variable `PK_SIM_MAPS_DIR` |
| `sim/capture.py` | `<repo>/datafiles/maps` | Edit `PK_SIM_MAPS_DIR` in the script |
| `firmware/scripts/gen_aircraft_db.py` | `<repo>/datafiles/data/pk_actdb.bin` | `--out` |

The defaults of the two host tests are paths **relative to the repo root**, so those one-line `cc` commands must be run
from the repo root. When the sample packages are missing, they SKIP the whole case without counting as a failure — so
after running them, check the output to confirm the cases actually ran; do not mistake a SKIP for a pass.
