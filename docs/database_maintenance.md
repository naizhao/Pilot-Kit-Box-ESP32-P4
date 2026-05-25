# Embedded Database Maintenance

Simplified Chinese version: [`database_maintenance-zh_CN.md`](database_maintenance-zh_CN.md)

This document covers the local aviation identity databases embedded in
the ESP32-P4 firmware. These databases improve ADS-B / Mode-S display
quality, but they are not flight-plan, route, timetable, dispatch, or
live network data.

## Safety Boundary

Pilot Kit Box is an open-source situational-awareness and development
device. The embedded databases must not be documented or used as
certified avionics data, a navigation source, or a collision-avoidance
source.

The firmware deliberately does not expose per-aircraft military, VIP,
PIA, or LADD flags from the upstream aircraft database. Keep that
boundary intact when refreshing or replacing data sources.

## Embedded Resources

| Resource | Firmware path | Source | Used by |
|---|---|---|---|
| Aircraft ICAO24 database | `firmware/main/aircraft_db.bin`, `firmware/main/aircraft_db.c`, `firmware/main/aircraft_db.h` | `wiedehopf/tar1090-db` aircraft shards plus `icao_aircraft_types.json` | ADS-B LIST type, model, registration, and detail pane |
| Airline code table | `firmware/main/airline_codes.c`, `firmware/main/airline_codes.h` | Wikipedia per-letter airline code pages, with maintainer-reviewed manual additions in the generator | Callsign display and operator name lookup |
| ICAO24 country ranges | `firmware/main/icao_country.c`, `firmware/main/icao_country.h` | tar1090 `flags.js`, derived from ICAO 24-bit address allocations | ADS-B LIST country column and detail pane |

The current checked-in `aircraft_db.bin` snapshot uses format `PKADB1`
version 2 and contains 570,141 aircraft records, 1,889 type rows, and an
8.16 MiB embedded blob. Treat these figures as a snapshot, not a fixed
protocol limit.

## User Update Model

Database refresh is a maintainer workflow. The firmware embeds generated
C sources and a generated binary blob at build time, then ships them as
part of the normal firmware image.

End users do not update these databases separately on the device. To
receive newer data, they install a newer firmware build through the web
flasher or the documented firmware release flow.

## Update Aircraft ICAO24 Database

Fetch the upstream tar1090-db shards and the ICAO aircraft type table:

```bash
mkdir -p /tmp/tar1090-db
cd /tmp/tar1090-db

curl -sL https://api.github.com/repos/wiedehopf/tar1090-db/contents/db \
  | python3 -c "import sys,json;[print(i['name']) for i in json.load(sys.stdin) if i['name'].endswith('.js')]" \
  | xargs -I {} -P 16 curl -sLO https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/{}

curl -sL -o /tmp/types.json \
  https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/icao_aircraft_types.json
```

Regenerate the firmware blob:

```bash
cd /path/to/Pilot-Kit-Box-ESP32-P4/firmware
scripts/gen_aircraft_db.py \
  --db-dir /tmp/tar1090-db \
  --types /tmp/types.json \
  --out main/aircraft_db.bin
```

The generator drops upstream `flags` values by design. Do not add those
fields to the firmware output.

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

Also review:

- `firmware/main/CMakeLists.txt` if embedded files or source names
  change.
- `README.md` and `docs/README.md` if database capabilities, paths, or
  user-facing update behavior change.
- `docs/firmware_update.md` / `docs/firmware_update-zh_CN.md` if the
  release process changes.
