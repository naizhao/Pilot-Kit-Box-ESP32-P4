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

Or update the firmware source directly:

    scripts/gen_airline_codes.py \\
        --letters all \\
        --update-source main/airline_codes.c

The script also writes a debug summary to stderr with the number of
entries pulled per letter, so you can spot pages that failed.

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
    of the current cell.

    一个 `|` 在列 0 不一定是新单元格——它也可能是**跨行模板**的参数分隔。
    Lucky Air（祥鹏航空，ICAO=LKE）那一行的 IATA 单元格长这样：

        |8L<ref>{{cite web|url=https://www.iata.org/...|title=Airline and Airport Code Search
        |website=www.iata.org}}</ref>
        |LKE

    第二行的 `|website=` 是 cite 模板的参数，却顶在列 0。按"列 0 的 |
    就是新单元格"切，整行的列全部右移一格：ICAO 位取到的是
    "website=www.iata.org"，`isalpha()` 不过，**整条被丢掉**。屏上表现
    就是祥鹏航空的航班查不到航司——不是数据没更新，是这一条从来没进过表。

    所以按 `{{`/`}}` 与 `<ref>`/`</ref>` 记嵌套深度，深度不为 0 时列 0 的
    `|` 一律当作当前单元格的续行。
    """
    cells: list[str] = []
    cur: list[str] = []
    depth = 0
    for line in row_text.split("\n"):
        if line.startswith("|") and depth == 0:
            if cur:
                cells.append("\n".join(cur))
                cur = []
            # Drop the leading `|` and any leading whitespace.
            cur.append(line[1:].lstrip())
        else:
            # Continuation of the current cell.
            if cur:
                cur.append(line)
        # 深度在处理完本行之后再更新：本行**开头**的 `|` 该不该算新单元格，
        # 取决于走到这一行之前的深度，而不是本行结束时的深度。
        depth += line.count("{{") - line.count("}}")
        # 自闭合 <ref ... /> 不开新层级，先从 `<ref` 计数里扣掉。
        depth += (line.count("<ref") - line.count("</ref>")
                  - len(re.findall(r"<ref[^>]*/\s*>", line)))
        if depth < 0:
            depth = 0
    if cur:
        cells.append("\n".join(cur))
    return cells


# 备注栏里"这家公司本身已经没了"的**硬证据**。命中即永久丢弃。
#
# 注意这里**不含** "no longer allocated" / "withdrawn" 这类说"代码停用"的话。
# 试过，代价是 65 条被删，其中包括 AXM——维基备注写着 "ICAO code no longer
# allocated"，可 AirAsia 今天就在用 AXM / 呼号 RED CAP 飞。维基对代码停用的
# 记载明显有陈旧的，按它删等于自己制造"查不到航司"。同类被误删的还有
# CJG 浙江航空、CYN 中原航空。代码停不停用不影响我们查表——没在用的码本来
# 就不会出现在空中，留着零成本；删错了却直接让屏上显示 "---"。
DEFUNCT_HARD_RE = re.compile(
    r"\b(defunct|ceased|out of business|bankrupt|dissolved|liquidat\w*)\b",
    re.IGNORECASE,
)

# 只表示"改过名 / 并过谁 / 有过旧代码"的**软证据**。命中的行降级为备选：
# 同一个 ICAO 若别处有干净行就用干净行，没有才拿它兜底。
#
# 为什么不能像以前那样直接丢：备注里写 "Former IATA code: ..."、
# "Renamed from ..."、"merged into X" 的行里，有相当一批是**至今在飞**的公司，
# 历史只是背景说明。实测被误杀的有 ABX Air（美国货运，备注写的是它 2003 年
# 接手 "former Airborne Express" 的业务）、AirBridgeCargo(ABW)、Scoot(TGW)、
# Rossiya(SDM)、ASL Airlines Ireland(ABR)、African Express(AXK) 等 43 个码。
#
# 但这个过滤当初是有正当理由的，不能简单删掉：Wikipedia 按**航司名**分页，
# D 页的 "Deutsche Luft Hansa | Became Lufthansa" 与 L 页的 "Lufthansa"
# 共用 ICAO=DLH，而 D 排在 L 前面，先到先得的话历史名会赢。
#
# 降级（而不是丢弃）同时满足两头：DLH 有 L 页的干净行，干净行优先，
# 仍然显示 "Lufthansa"；ABX 没有任何干净行，于是软行兜底把它救回来。
DEFUNCT_SOFT_RE = re.compile(
    r"\b(became|closed|merged|absorbed|former|formerly|renamed|"
    r"no longer allocated|no longer used|withdrawn)\b",
    re.IGNORECASE,
)


def parse_page(letter: str, wikitext: str) -> list[tuple[str, str, str, int]]:
    """Return list of (icao3, iata2, name, tier) tuples.

    tier 0 = 备注干净，首选；tier 1 = 备注只有改名/合并/旧代码一类软证据，
    仅在该 ICAO 没有 tier 0 行时兜底。硬性停业的行直接不返回。
    """
    out: list[tuple[str, str, str, int]] = []
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
        # 硬性停业：永久丢弃。软证据：降级到 tier 1 兜底。详见两条正则的注释。
        if DEFUNCT_HARD_RE.search(comments):
            continue
        tier = 1 if DEFUNCT_SOFT_RE.search(comments) else 0
        # Trim parenthetical disambiguators like "Air China (cargo)" — keep
        # the lead but drop the (...) tail to keep entries short.
        name = re.sub(r"\s*\([^)]+\)\s*$", "", name).strip()
        out.append((icao, iata, name, tier))
    return out


# Manually-curated entries for carriers Wikipedia hasn't (yet) added to
# the per-letter tables — typically post-2020 new starts or small
# regional cargo / charter operators. Sourced from each airline's own
# Wikipedia page (which exists, just isn't linked from the per-letter
# code lists) or from public flight-tracker airline DBs.
#
# 现状核对（重跑生成器时顺手复核）：HGO / JDL / URC 维基至今没有，必须留；
# TBA / UEA / OKA / RLH 维基有条目但 IATA 栏是空的，留着是为了补 IATA；
# DKH / CKK / CSS / CYZ / YZR / EPA / AHK / HKC 这 8 条维基已收录且 IATA
# 一致，严格说可以删——留着只是因为这里的名字更常用（"Juneyao Airlines"
# vs 维基的 "Juneyao Air"、"AHK Air Hong Kong" vs "Air Hong Kong"）。
# 删它们不会丢数据，只会让屏上换个叫法。
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
    ap.add_argument("--update-source",
                    help="replace s_airlines[] inside this airline_codes.c")
    ap.add_argument("--check", action="store_true",
                    help="with --update-source, fail if source would change")
    args = ap.parse_args()

    if args.letters == "all":
        letters = LETTERS
    else:
        letters = [l.strip().upper() for l in args.letters.split(",")]

    os.makedirs(args.cache_dir, exist_ok=True)

    # ICAO → (icao, iata, name)。tier 0 与 tier 1 分开收，最后再合并，
    # 保证"L 页的干净 Lufthansa"永远压过"D 页的 Became Lufthansa"，
    # 与字母页的先后顺序无关。
    all_rows: dict[str, tuple[str, str, str]] = {}
    soft_rows: dict[str, tuple[str, str, str]] = {}
    per_letter: dict[str, tuple[int, int]] = {}
    for L in letters:
        cache_path = os.path.join(args.cache_dir, f"{L}.wikitext")
        if os.path.exists(cache_path) and os.path.getsize(cache_path) > 0:
            with open(cache_path, encoding="utf-8") as f:
                wt = f.read()
        else:
            sys.stderr.write(f"fetching {L} ...\n")
            wt = fetch_letter(L)
            with open(cache_path, "w", encoding="utf-8") as f:
                f.write(wt)
            time.sleep(1.0)   # be polite

        rows = parse_page(L, wt)
        n0 = sum(1 for r in rows if r[3] == 0)
        n1 = len(rows) - n0
        per_letter[L] = (n0, n1)
        sys.stderr.write(f"  {L}: {len(rows)} entries ({n0} primary, {n1} soft)\n")
        for icao, iata, name, tier in rows:
            # First occurrence wins (alphabetical letter pages are in the
            # right order, and duplicates across pages are rare).
            (all_rows if tier == 0 else soft_rows).setdefault(
                icao, (icao, iata, name))

    # 软行只填 tier 0 没覆盖到的空缺。
    n_soft_used = 0
    for icao, row in soft_rows.items():
        if icao not in all_rows:
            all_rows[icao] = row
            n_soft_used += 1
    sys.stderr.write(f"  +{n_soft_used} soft-tier fallbacks used\n")

    # Splice in the manual fixup entries. Manual wins on conflict — these
    # were added precisely because Wikipedia's entry was wrong / missing.
    for icao, iata, name in MANUAL_ADDITIONS:
        all_rows[icao] = (icao, iata, name)
    sys.stderr.write(f"  +{len(MANUAL_ADDITIONS)} manual additions\n")

    sorted_rows = sorted(all_rows.values(), key=lambda r: r[0])
    sys.stderr.write(f"\nTotal unique ICAO codes: {len(sorted_rows)}\n")

    block_lines: list[str] = []
    for icao, iata, name in sorted_rows:
        # Escape any " or \ in the name.
        n = name.replace("\\", "\\\\").replace("\"", "\\\"")
        block_lines.append(f'    {{ "{icao}", "{iata}", "{n}" }},\n')
    block = "".join(block_lines)

    with open(args.out_block, "w", encoding="utf-8") as f:
        f.write(block)
    sys.stderr.write(f"wrote {args.out_block} ({len(sorted_rows)} rows)\n")

    if args.update_source:
        src_path = args.update_source
        with open(src_path, encoding="utf-8") as f:
            old = f.read()
        pattern = re.compile(
            r"(static const pk_airline_t s_airlines\[\] = \{\n)"
            r".*?"
            r"(\};\n)",
            re.DOTALL,
        )
        new, nsubs = pattern.subn(
            lambda m: m.group(1) + block + m.group(2),
            old,
            count=1,
        )
        if nsubs != 1:
            raise RuntimeError(f"could not locate s_airlines[] in {src_path}")
        if args.check:
            if new != old:
                sys.stderr.write(f"{src_path} is out of date\n")
                return 1
            sys.stderr.write(f"{src_path} is up to date\n")
        elif new != old:
            with open(src_path, "w", encoding="utf-8") as f:
                f.write(new)
            sys.stderr.write(f"updated {src_path}\n")
        else:
            sys.stderr.write(f"{src_path} already up to date\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
