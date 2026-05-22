#!/usr/bin/env python3
"""
gen_airline_codes.py — Build firmware/main/airline_codes.c from Wikipedia.

Wikipedia maintains a more current mirror of ICAO Doc 8585 (Designators
for Aircraft Operating Agencies) than the openflights / npow snapshots
the previous table was generated from. The data lives across 26
per-letter pages plus one numeric prefix page:

    https://en.wikipedia.org/wiki/List_of_airline_codes_(A)
    ...
    https://en.wikipedia.org/wiki/List_of_airline_codes_(Z)
    https://en.wikipedia.org/wiki/List_of_airline_codes_(0%E2%80%939)
    https://en.wikipedia.org/wiki/List_of_airline_codes  (intro letters)

Each page has a `{| class="wikitable sortable"` table whose rows follow
this exact wikitext shape:

    |-
    |<IATA or blank>
    |<ICAO 3-letter or blank>
    |[[<airline name>]]                (sometimes plain text, no [[]])
    |<callsign or blank>
    |<country>
    |<comments>

We pull the wikitext via the parse API, regex-split the rows, strip wiki
markup, dedupe on ICAO, sort ascending, and emit the C initialiser block
that airline_codes.c expects between `static const pk_airline_t
s_airlines[] = {` and `};`.

Usage:
    cd firmware
    scripts/gen_airline_codes.py \\
        --letters all \\
        --out-block /tmp/airlines_block.c

Then splice the block into airline_codes.c. The script also writes a
debug summary to stderr with the number of entries pulled per letter,
so you can spot pages that failed.

Wikipedia API etiquette: we pass a descriptive User-Agent including a
contact email (per their automated-access policy) and rate-limit at
1 fetch / second.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request


USER_AGENT = ("PilotKitBoxAirlineCodeGen/0.1 "
              "(pilotbra@icloud.com; https://github.com/naizhao)")

LETTERS = list("ABCDEFGHIJKLMNOPQRSTUVWXYZ")


def fetch_letter(letter: str) -> str:
    """Fetch wikitext for one airline-codes letter page."""
    page = f"List_of_airline_codes_({letter})"
    url = ("https://en.wikipedia.org/w/api.php"
           f"?action=parse&page={urllib.parse.quote(page)}"
           "&prop=wikitext&format=json&redirects=1")
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=30) as r:
        d = json.loads(r.read())
    if "parse" not in d:
        raise RuntimeError(f"no parse result for {page}: {d}")
    return d["parse"]["wikitext"]["*"]


# Row separator. We split the wikitext on lines that are exactly `|-`,
# then for each row chunk we extract cells line-by-line. This survives
# `|` characters that appear INSIDE a wikilink (e.g. `[[Speedbird|
# SPEEDBIRD]]` in the callsign cell of British Airways), which a flat
# regex over `[^|\n]` would otherwise truncate.
ROW_SEP_RE = re.compile(r"^\|-\s*$", re.MULTILINE)


def strip_wiki(s: str) -> str:
    """Strip wiki markup: [[A]] / [[A|B]] / refs / italics / bold."""
    # [[ ... | label ]] → label   (piped link)
    s = re.sub(r"\[\[[^\]|]*\|\s*([^\]]+)\s*\]\]", r"\1", s)
    # [[ name ]] → name
    s = re.sub(r"\[\[\s*([^\]|]+)\s*\]\]", r"\1", s)
    # <ref ... /> and <ref>...</ref>
    s = re.sub(r"<ref[^>]*/\s*>", "", s)
    s = re.sub(r"<ref[^>]*>.*?</ref>", "", s, flags=re.DOTALL)
    # {{tpl|...}} (drop entirely — usually citation templates)
    s = re.sub(r"\{\{[^}]*\}\}", "", s)
    # ''italic'' / '''bold'''
    s = re.sub(r"'''?", "", s)
    # html tags
    s = re.sub(r"<[^>]+>", "", s)
    # & escapes
    s = s.replace("&amp;", "&").replace("&nbsp;", " ")
    return s.strip()


def parse_row_cells(row_text: str) -> list[str]:
    """Split a wikitext row chunk into cells. Cells start with `|` at
    column 0; the cell value extends to the next cell-start line or to
    the end of the chunk. Lines that begin with a space (template
    continuations, indented references) are treated as continuation
    of the current cell."""
    cells: list[str] = []
    cur: list[str] = []
    for line in row_text.split("\n"):
        if line.startswith("|"):
            if cur:
                cells.append("\n".join(cur))
                cur = []
            # Drop the leading `|` and any leading whitespace.
            cur.append(line[1:].lstrip())
        else:
            # Continuation of the current cell.
            if cur:
                cur.append(line)
    if cur:
        cells.append("\n".join(cur))
    return cells


DEFUNCT_RE = re.compile(
    r"\b(became|defunct|ceased|out of business|closed|bankrupt|"
    r"merged|absorbed|dissolved|former|renamed)\b",
    re.IGNORECASE,
)


def parse_page(letter: str, wikitext: str) -> list[tuple[str, str, str]]:
    """Return list of (icao3, iata2, name) tuples. Skips entries whose
    comments cell looks defunct."""
    out: list[tuple[str, str, str]] = []
    chunks = ROW_SEP_RE.split(wikitext)
    for chunk in chunks:
        cells = parse_row_cells(chunk)
        if len(cells) < 5:
            continue
        iata     = strip_wiki(cells[0]).upper()
        icao     = strip_wiki(cells[1]).upper()
        name     = strip_wiki(cells[2])
        comments = strip_wiki(cells[5]) if len(cells) >= 6 else ""
        # ICAO must be exactly 3 alpha chars (the standard form).
        if len(icao) != 3 or not icao.isalpha():
            continue
        # NB: don't filter by `icao[0] == letter` — Wikipedia files
        # entries by AIRLINE NAME, not ICAO code, so e.g. "Air China"
        # (ICAO=CCA) lives on the A page despite having a C-prefix code.
        # IATA must be 1-3 chars and alphanumeric, else empty.
        if not iata or not re.match(r"^[A-Z0-9]{1,3}$", iata):
            iata = ""
        if not name:
            continue
        # Skip entries whose comments mark them defunct / merged / renamed.
        # The current operator usually has its own row elsewhere in the
        # table (e.g. "Deutsche Luft Hansa | Became Lufthansa" on the D
        # page is shadowed by "Lufthansa" on the L page with the same
        # ICAO DLH). Without this filter the historic name wins because
        # alphabetical ordering puts D before L.
        if DEFUNCT_RE.search(comments):
            continue
        # Trim parenthetical disambiguators like "Air China (cargo)" — keep
        # the lead but drop the (...) tail to keep entries short.
        name = re.sub(r"\s*\([^)]+\)\s*$", "", name).strip()
        out.append((icao, iata, name))
    return out


# Manually-curated entries for carriers Wikipedia hasn't (yet) added to
# the per-letter tables — typically post-2020 new starts or small
# regional cargo / charter operators. Sourced from each airline's own
# Wikipedia page (which exists, just isn't linked from the per-letter
# code lists) or from public flight-tracker airline DBs.
MANUAL_ADDITIONS: list[tuple[str, str, str]] = [
    ("HGO", "HE", "One Air"),                        # UK cargo charter (2021)
    ("JDL", "JD", "Jingdong Cargo Airlines"),        # 京东货运 (2019)
    ("URC", "UQ", "Urumqi Airlines"),                # 乌鲁木齐航空 (2014)
    ("TBA", "TV", "Tibet Airlines"),                 # 西藏航空
    ("UEA", "EU", "Chengdu Airlines"),               # 成都航空
    ("OKA", "BK", "Okay Airways"),                   # 奥凯航空
    ("DKH", "HO", "Juneyao Airlines"),               # 吉祥航空
    ("CKK", "CK", "China Cargo Airlines"),           # 中货航
    ("CSS", "O3", "SF Airlines"),                    # 顺丰航空
    ("CYZ", "8Y", "China Postal Airlines"),          # 中国邮政航空
    ("YZR", "Y8", "Suparna Airlines"),               # 金鹏航空
    ("EPA", "DZ", "Donghai Airlines"),               # 东海航空
    ("AHK", "LD", "AHK Air Hong Kong"),              # 香港华民航空 / DHL
    ("HKC", "RH", "Hongkong Air Cargo"),             # 香港货运航空
    # IATA fixups — Wikipedia lists these without an IATA code but the
    # carrier actually has one. Without an IATA the list-view fmt_call
    # path would print just the flight number (e.g. "RLH5301" → "5301").
    ("RLH", "DR", "Ruili Airlines"),                 # 瑞丽航空
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--letters", default="all",
                    help="comma-separated list of letters (default: all 26)")
    ap.add_argument("--cache-dir", default="/tmp/pkb_airlines_cache",
                    help="cache raw wikitext here to avoid refetching")
    ap.add_argument("--out-block", default="/tmp/airlines_block.c",
                    help="emit the s_airlines[] initialiser block here")
    args = ap.parse_args()

    if args.letters == "all":
        letters = LETTERS
    else:
        letters = [l.strip().upper() for l in args.letters.split(",")]

    os.makedirs(args.cache_dir, exist_ok=True)

    all_rows: dict[str, tuple[str, str, str]] = {}    # ICAO → (icao, iata, name)
    for L in letters:
        cache_path = os.path.join(args.cache_dir, f"{L}.wikitext")
        if os.path.exists(cache_path) and os.path.getsize(cache_path) > 0:
            wt = open(cache_path).read()
        else:
            sys.stderr.write(f"fetching {L} ...\n")
            wt = fetch_letter(L)
            open(cache_path, "w").write(wt)
            time.sleep(1.0)   # be polite

        rows = parse_page(L, wt)
        sys.stderr.write(f"  {L}: {len(rows)} entries\n")
        for icao, iata, name in rows:
            # First occurrence wins (alphabetical letter pages are in the
            # right order, and duplicates across pages are rare).
            all_rows.setdefault(icao, (icao, iata, name))

    # Splice in the manual fixup entries. Manual wins on conflict — these
    # were added precisely because Wikipedia's entry was wrong / missing.
    for icao, iata, name in MANUAL_ADDITIONS:
        all_rows[icao] = (icao, iata, name)
    sys.stderr.write(f"  +{len(MANUAL_ADDITIONS)} manual additions\n")

    sorted_rows = sorted(all_rows.values(), key=lambda r: r[0])
    sys.stderr.write(f"\nTotal unique ICAO codes: {len(sorted_rows)}\n")

    with open(args.out_block, "w") as f:
        for icao, iata, name in sorted_rows:
            # Escape any " or \ in the name
            n = name.replace("\\", "\\\\").replace("\"", "\\\"")
            f.write(f'    {{ "{icao}", "{iata}", "{n}" }},\n')
    sys.stderr.write(f"wrote {args.out_block} ({len(sorted_rows)} rows)\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
