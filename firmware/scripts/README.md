# firmware/scripts — i18n Catalog and Font Glyph Set Generation

Chinese version: [`README-zh_CN.md`](README-zh_CN.md)

This directory holds the firmware's code/asset generation scripts. This document describes the **multilingual i18n
catalog** pipeline: catalog source → batch translation → font glyph set regeneration → compiled into the firmware.

```
i18n_catalog.py            single source of truth for the catalog (KEY + translations per language)
      │
      ├── translate_catalog.py    ① batch-fill missing-language translations (calls an LLM, writes back into this file)
      │
      ├── gen_i18n_assets.py      ② generates the C lookup table (**only** that)
      │                              → firmware/main/i18n_catalog.{h,c}
      │
      ├── gen_pfd_aa_font.py      ③ generates the glyphs actually used on screen (including the CJK subset)
      │                              → firmware/main/pfd_aa_font.c
      │
      └── gen_lv_font.py          ④ generates the font for LVGL widgets (toast, etc.)
                                     → firmware/main/lv_font_zh.c
```

> **The font pipeline is three pipelines, not one.** ②③④ each cover their own territory, and the symptoms of skipping
> any one of them differ: skipping ③ leaves Chinese characters blank across **directly drawn pages** (PFD/settings/
> about…; a failed lookup only advances the width without drawing), while skipping ④ turns LVGL widgets (toast) into
> tofu boxes. ② produces no glyphs at all —
> before 2026-08-03 it also generated `text_font_cjk*.{h,c}`, and those two tiers were deleted along with their last
> caller `text.c` (see the file header of `gen_i18n_assets.py`).

> **Note**: as required, `translate_catalog.py` is listed in `.gitignore` and kept locally only; it is not distributed
> with the repo (the script itself contains no secrets — see that section in `.gitignore`).
> If you cannot find this file after cloning the repo, ask a maintainer for a copy, or rewrite it following the
> interface described here: it only reads `i18n_catalog.py`, calls one OpenAI-compatible endpoint, and inserts the
> translations back into the original file.

---

## 1. Set Up .env

The translation script **contains no secrets**; keys are read only from environment variables or the repo-root `.env`.
**The variable names and the 5-level provider fallback follow the existing convention of similar translation scripts in
our other projects** (not something invented here), so the same `.env` works across repos and maintainers do not have to
learn a second scheme.

```bash
cp .env.example .env      # at the repo root
# Edit .env, fill in ZHIPU_API_KEY and/or GEMINI_API_KEY
```

The 5-level provider chain (**automatically falls through to the next level** when the previous one is exhausted/rate
limited; levels without a key are skipped):

| Level | Provider | Model (overridable) | Key |
|---|---|---|---|
| 1 | Zhipu GLM Coding Plan (paid, primary) | `ZHIPU_CODING_MODEL` = `glm-5.2` | `ZHIPU_API_KEY` |
| 2 | Google Gemma (free, generous RPD) | `GEMMA_MODEL` = `gemma-4-31b-it` | `GEMINI_API_KEY` |
| 3 | Google Gemini flash (free, top quality) | `GEMINI_TIER2_MODEL` = `gemini-3.5-flash` | `GEMINI_API_KEY` |
| 4 | Google Gemini flash (free, faster) | `GEMINI_TIER3_MODEL` = `gemini-3.6-flash` | `GEMINI_API_KEY` |
| 5 | Zhipu free fallback | `ZHIPU_FREE_MODEL` = `glm-4.5-flash` | `ZHIPU_API_KEY` |

It also runs with only one key filled in: with only `ZHIPU_API_KEY`, levels 1 and 5 are used; with only `GEMINI_API_KEY`,
levels 2, 3, and 4 are used. **With neither key, `--dry-run` still works** (the provider chain is constructed lazily).

Other options: `LLM_TIMEOUT` (default 300), `LLM_MAX_TOKENS` (default 8192),
per-level `*_BASE_URL` (point `GEMINI_BASE_URL` at `http://127.0.0.1:11580/v1` to route everything through the local
ServBay AI Gateway). See `.env.example` for the full list.

To temporarily pin one model and bypass the fallback: `--model` / `--base-url` (escape hatch; once specified, only that
one provider is used).

`.env` / `.env.local` are already in `.gitignore`. **Never put real keys into
`.env.example`, scripts, documentation, or commit messages.**

---

## 2. Adding a New Language

Using Japanese `ja` as an example, four steps:

### ① Dry-run first to see what will be translated

```bash
python3 firmware/scripts/translate_catalog.py --lang ja --dry-run
```

Sends no requests and writes no files; prints three lists:

- **Existing translations**: not a single character is touched;
- **To translate**: the entries to be sent to the LLM this run, plus the protected terms hit by each entry;
- **Verbatim terms**: copied as English, not sent for translation (see Section 3).

### ② The real run

```bash
python3 firmware/scripts/translate_catalog.py --lang ja
```

- **Batching**: by default ≤25 entries and ≤4000 characters per batch (`--batch-size` / `--batch-max-chars`).
- **Rate limiting**: a default 1-second pause between batches (`--sleep`, 0 = no wait). Free-tier Gemini RPM is tight;
  rapid-fire requests will eat a 429 and trip the whole level's breaker.
- **Retry / fallback**: a failed batch is retried 2 times with exponential backoff (`--retries`);
  quota exhaustion or rate limiting does **not** count as failure — the current provider trips its breaker in place and
  translation continues automatically at the next level.
  Only when all five levels are tripped does it persist state and exit (exit code 2).
- **Resume from checkpoint**: after each successful batch, the result is immediately written to
  `firmware/scripts/.i18n_translate_cache/<lang>.json`. After quota exhaustion, a network drop, or Ctrl-C, **re-run the
  same command** to continue; already-translated entries will not be paid for twice. The checkpoint deletes itself once
  everything is written back successfully.
- **Per-entry validation before acceptance** (see Section 4): entries with mismatched placeholders, translated protected
  terms, or line breaks introduced into single-line entries are rejected outright, not written into the catalog, and
  left for the next re-run.
- **Write-back**: inserted line by line into the corresponding entries in `i18n_catalog.py`; comments, indentation, and
  layout are left untouched; the new language is also added to `LANGS` (use `--no-update-langs` to skip that).
- **Self-check before writing**: the file is re-parsed and asserted entry by entry — the entry count is unchanged, other
  languages' original values are untouched, and the target language's value equals this run's translation. Any failed
  assertion aborts the write with an error.

To try it on a small scale first: `--limit 10` sends only the first 10 entries for translation (verbatim-term entries
are exempt — they cost no quota and are all written back as usual).
To fill in only the verbatim-term entries (English as-is) without calling the API at all: `--verbatim-only`.

Multiple languages in one run: `--lang ja --lang ko` or `--lang ja,ko`.

### ③ Regenerate the catalog table and font glyph sets (**none of the three may be skipped**)

```bash
python3 firmware/scripts/gen_i18n_assets.py    # catalog table
python3 firmware/scripts/gen_pfd_aa_font.py    # glyphs for directly drawn pages (including CJK)
python3 firmware/scripts/gen_lv_font.py        # font for LVGL widgets
```

Glyph subsets are built **from the characters that actually appear in the catalog**
(`collect_cjk_codes()` in `gen_pfd_aa_font.py`, plus the eight directional arrows merged in).
A new language's glyphs are not in the subset, and on screen they would be blank. The latter two need ImageMagick's `magick`
command and `fontTools`.

### ④ Visual inspection

Use `sim/` or go straight to the device and check:

- Any characters rendered as blank. When a lookup misses, `pfd_aa_text.c` **only advances the width without drawing**
  (better to leave a blank than draw a misaligned glyph), so the symptom is "a few characters missing" rather than tofu
  boxes;
- Any column widths being blown out. All non-ASCII code points are laid out using that tier's wide `PK_AA_*_CJK_W`
  format —
  Cyrillic letters and accented Latin letters (`é` / `ü` / `ã`) also take this wide-format path, not just Chinese
  characters. The list page's column widths are pinned by a set of `_Static_assert`s (see the top of `adsb_list.c`),
  so blowing one out fails the build.

---

## 3. Where the Do-Not-Translate Term Table Lives

Two layers, both in `translate_catalog.py`:

1. **Per-entry decision (primary)** — `is_verbatim_entry()`: if an entry's `zh` and `en` are **exactly identical**, a
   human has already decided "this entry is a term; do not translate", so English is kept as-is for **all** languages.
   The current catalog has 24 such entries (QNH / IMU / PFD / ALT / TRK / GS / V/S /
   ICAO / ESP-IDF / LVGL / microSD …).

   Why per entry rather than per surface form: the same surface form is handled differently in different contexts.
   `DIAG_CARD_IMU` is `en=IMU / zh=IMU` (diagnostics card title, not translated),
   while `ABOUT_IMU` is `en=IMU / zh=姿态` (the description line on the about page, translated).
   A blanket per-surface rule would turn the latter into `IMU` too, overriding the human decision.

2. **Surface-form table `KEEP_VERBATIM_TERMS` (auxiliary)** — does only two things:

   - Backstops **new entries that do not yet have a `zh` translation** (an entry that is exactly one term → not
     translated);
   - **In-sentence term protection**: terms hit within a sentence are passed per entry in the prompt's
     `keep_verbatim` field, instructing the model to keep them as-is. For example, `"QNH REF"` prompts keeping
     `QNH`, and `"int WDT"` prompts keeping `WDT`.

   Matching uses alphanumeric boundaries, so `SETTINGS` does not falsely match `GS`, and `left` does not falsely match
   `ft`. When an entry exactly equals a term, the hint is **not** sent — reaching the translation flow at all means a
   human has already decided it should be translated (e.g. `LIST_D_SQUAWK`'s `zh` is 「应答机」); hinting keep verbatim
   then would be self-contradictory.

The original source of the translation principles is the comment at `i18n_catalog.py:500-509` (avionics terms are not
translated / status descriptions are translated / lines with numbers are not whole-sentence format strings). Read that
section before changing term policy.

---

## 4. Translation Validation (after the model returns, before writing into the catalog)

Every translation must pass `validate_value()` to be accepted. Three hard rules; failing any one **rejects that entry**
(not the whole batch); rejected entries stay outside the checkpoint and are re-translated on the next run:

| Check | Why |
|---|---|
| printf placeholder set must match the source (`%d` / `%s` / `%.1f` / `%02d`) | The C-side `snprintf` arguments are hard-coded; one extra or missing placeholder means an **out-of-bounds stack read** |
| Terms in `keep_verbatim` must appear verbatim in the translation | The model occasionally paraphrases `QNH REF` wholesale; avionics abbreviations must not be translated |
| If the source is single-line, the translation must contain no line break | The screen uses fixed-height single-line widgets; `\n` renders as boxes |

Plus one soft rule: when the translation is longer than `--max-len-ratio` (default 1.6) times the source, it is
**warned about but not rejected**
— German/Russian are naturally longer, and a blanket cut would discard good translations, but the column widths are
worth a look on the device.

Before acceptance there is also a cleanup pass (`sanitize_value()`): it strips code fences ` ```…``` ` and matching
leading/trailing quotes — the two things models most love to add.

> This "response must pass validation, otherwise treat it as failed" approach comes from a script in another of our
> projects that translates release notes (its validation required translations to start with `##`, contain a version
> number, and have exactly one version block) — models occasionally return something structurally correct but off in
> content, and without validation it silently pollutes the artifacts.

---

## 5. When Adding Catalog Entries (No New Language Involved)

After editing `i18n_catalog.py` to add `("KEY", {"en": ..., "zh": ...})`, you **must** re-run the three commands from
Section 2 ③: skipping `gen_i18n_assets.py` means the new entries have no enum values at all (a compile-time error,
visible); skipping `gen_pfd_aa_font.py` means the new Chinese characters are not in the glyph subset, and those
characters simply vanish on screen (**silent** — only visible to the eye).

If the new Chinese characters are all already in the existing subset (e.g. reusing 「校」「准」, characters that already
appear elsewhere), the font files will not change after re-running, and `git diff` is empty — that does not mean the
run failed.
