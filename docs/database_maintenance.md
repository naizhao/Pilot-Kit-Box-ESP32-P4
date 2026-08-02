# Aviation Identity Database Maintenance

Simplified Chinese version: [`database_maintenance-zh_CN.md`](database_maintenance-zh_CN.md)

This document covers the local aviation identity databases used by the
ESP32-P4 firmware — the ones compiled into the firmware image and the
aircraft database shipped on the microSD card. These databases improve
ADS-B / Mode-S display quality, but they are not flight-plan, route,
timetable, dispatch, or live network data.

## Safety Boundary

Pilot Kit Box is an open-source situational-awareness and development
device. These databases must not be documented or used as certified
avionics data, a navigation source, or a collision-avoidance source.

The firmware deliberately does not expose per-aircraft military, VIP,
PIA, or LADD flags from the upstream aircraft database. Keep that
boundary intact when refreshing or replacing data sources.

## Where Each Database Lives

| Resource | Location | Source | Used by |
|---|---|---|---|
| Aircraft ICAO24 database | microSD `/sdcard/aero/pk_actdb.bin`; built into `datafiles/data/pk_actdb.bin`; reader `firmware/main/aircraft_db.c`, `firmware/main/aircraft_db_reader.c`, `firmware/main/aircraft_db.h` | `wiedehopf/tar1090-db` aircraft shards plus the Doc 8643 type table `db/icao_aircraft_types.js` | ADS-B LIST type, model, registration, and detail pane |
| Airline code table | Compiled into the firmware: `firmware/main/airline_codes.c`, `firmware/main/airline_codes.h` | Wikipedia per-letter airline code pages, with maintainer-reviewed manual additions in the generator | Callsign display and operator name lookup |
| ICAO24 country ranges | Compiled into the firmware: `firmware/main/icao_country.c`, `firmware/main/icao_country.h` | tar1090 `flags.js`, derived from ICAO 24-bit address allocations | ADS-B LIST country column and detail pane |

Only the aircraft ICAO24 database moved to the card. The airline code
table and the ICAO24 country ranges are generated C sources and are
still linked into the firmware image, as is the magnetic-variation
table `firmware/main/mag_var_table.h`.

The aircraft database file is a 64-byte `PKACT1` container header (magic,
cycle stamp, SHA-256 of the payload) wrapping a `PKADB1` version 2
payload. It is **not encrypted** (`enc_algo = 0`) because tar1090-db is
public data; the separate aeronautical database `/sdcard/aero/pk_aero.bin`
keeps its AES-128-CTR encryption. The current build is cycle `20260802`,
574,053 aircraft records, 1,886 type rows, 8,604,583 bytes (8.21 MB).
Treat these figures as a snapshot, not a fixed protocol limit.

The two files are deliberately separate: tar1090-db refreshes weekly
while the aeronautical data follows the 28-day AIRAC cycle, so merging
them would force one to wait on the other.

## User Update Model

Database refresh is a maintainer workflow, but the two classes of data
reach the device differently.

- **Aircraft ICAO24 database** — regenerate `pk_actdb.bin` and copy it to
  the card at `/aero/pk_actdb.bin`. No firmware reflash is involved. The
  firmware loads it lazily into PSRAM after boot; with no card or no file
  the box keeps working and ADS-B type/model/registration fields simply
  render as `---`.
- **Airline code table and ICAO24 country ranges** — regenerate the C
  sources, rebuild, and ship them inside the firmware image. Users receive
  newer data by installing a newer firmware build through the web flasher
  or the documented firmware release flow.

Card layout and the other data files (`pk_aero.bin`, `/maps/*.pmtiles`)
are documented in `datafiles/README.md`.

## Update Aircraft ICAO24 Database

Fetch the upstream tar1090-db shards and the ICAO Doc 8643 type table.
Upstream moved the type table: the old repo-root `icao_aircraft_types.json`
now 404s, and the current `db/icao_aircraft_types.js` is gzip-compressed,
so it has to be gunzipped into plain JSON for `--types`.

```bash
mkdir -p /tmp/tar1090-db
cd /tmp/tar1090-db

curl -sL https://api.github.com/repos/wiedehopf/tar1090-db/contents/db \
  | python3 -c "import sys,json;[print(i['name']) for i in json.load(sys.stdin) if i['name'].endswith('.js')]" \
  | xargs -I {} -P 16 curl -sLO https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/{}

curl -sL https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/icao_aircraft_types.js \
  | gunzip > /tmp/types.json
```

Regenerate the card file. `--out` defaults to
`<repo>/datafiles/data/pk_actdb.bin`, so it can be omitted:

```bash
cd /path/to/Pilot-Kit-Box-ESP32-P4
firmware/scripts/gen_aircraft_db.py \
  --db-dir /tmp/tar1090-db \
  --types /tmp/types.json
```

Then copy the result onto the card as `/aero/pk_actdb.bin`. Keep the
file name exactly — the path is a compile-time constant (`ACTDB_PATH` in
`firmware/main/aircraft_db.c`). No firmware rebuild or reflash is needed
for an aircraft-database refresh.

The generator drops upstream `flags` values by design. Do not add those
fields to the output.

## Update Airline Code Table

The airline generator fetches Wikipedia wikitext through the MediaWiki
API, caches pages under `/tmp/pkb_airlines_cache`, applies the
maintainer-reviewed `MANUAL_ADDITIONS`, and can replace the checked-in
C table directly.

```bash
cd firmware
scripts/gen_airline_codes.py \
  --letters all \
  --update-source main/airline_codes.c
```

For CI or maintainer verification:

```bash
cd firmware
scripts/gen_airline_codes.py \
  --letters all \
  --update-source main/airline_codes.c \
  --check
```

The script rate-limits requests to one fetch per second. Review large
diffs manually, especially airline renames, historic/defunct operators,
and the manual additions block.

## Update ICAO24 Country Ranges

The country generator fetches tar1090 `flags.js` by default and emits
`main/icao_country.c`:

```bash
cd firmware
scripts/gen_icao_country.py --out main/icao_country.c
```

For audited or offline updates, download the source file first:

```bash
cd firmware
scripts/gen_icao_country.py \
  --source-file /tmp/flags.js \
  --out main/icao_country.c
```

For CI or maintainer verification:

```bash
cd firmware
scripts/gen_icao_country.py --check --out main/icao_country.c
```

The generated lookup returns the most specific matching address range,
so small territory assignments inside larger country blocks override the
parent range. The generator also keeps explicit ISO code fixups when the
upstream flag code is not a valid ISO 3166-1 alpha-2 country code.

## Build And Release Checks

After any database refresh:

```bash
cd firmware
python3 -m py_compile scripts/gen_aircraft_db.py scripts/gen_airline_codes.py scripts/gen_icao_country.py
./build.sh build
```

A rebuild is only required for the airline and country tables; the
aircraft database is a card file and does not affect the image.

Also review:

- `firmware/main/CMakeLists.txt` if source names change.
- `datafiles/README.md` if the card layout, file names, or generator
  defaults change.
- `README.md` and `docs/README.md` if database capabilities, paths, or
  user-facing update behavior change.
- `docs/firmware_update.md` / `docs/firmware_update-zh_CN.md` if the
  release process changes.
