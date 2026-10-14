# MU100 Editor — data extraction (Step 1)

A modern, resizable replacement for XGEdit that drives the MU100 emulator (the
MEGAN XG plugin in this repository) over SysEx. **Step 1** is the static data layer: turn
the reference PDFs into structured JSON the editor will load at runtime, and
measure how much manual cleanup each table needs before any GUI work begins.

This directory holds the extraction pipeline and the JUCE-free codec (`codec/`);
the plugin's GUI lives in the repository's `Source/`.

## Layout
```
reference/   source docs (MU100 sound list + MIDI data PDF, XGEdit manual, screenshots)
tools/       Python extractors (PyMuPDF) + run_all.py
data/        generated JSON (parameters / effects / voices / tables / drums)
```

## Run
```
python -m pip install -r tools/requirements.txt   # PyMuPDF
python tools/run_all.py
```

## How it works
The Sound List PDF lays most tables out in **two side-by-side columns**, which
`pdftotext -layout` interleaves into unusable text. The extractors use PyMuPDF to
read word coordinates and split each page by x-position (`tools/pdf_lib.py`),
de-interleaving the columns. Fields within a row are then assigned by their x
against each table's own header positions, so the parsers self-calibrate.

## Generated data + extraction quality

### `data/parameters.json` — the device address map (the editor's spine)
16 `MIDI Parameter Change` tables → **711 parameters**, each with SysEx
Address (High/Mid/Low), Size, raw value Range, name, description and default.
Maps onto XGEdit sections: XG SYSTEM + MU100 SYSTEM → Global; MULTI EQ;
EFFECT 1/2; MULTI PART → Part; A/D PART; DRUM SETUP.

> **Never hand-edit this file.** `tools/extract_parameters.py` regenerates it in
> full, so a direct edit is destroyed on the next run, silently. Device facts the
> manual does not state go in that tool's `OVERRIDES` dict, keyed by address.
> Pipeline order: `extract_parameters.py` → `backfill_defaults.py` →
> `gen_address_map.py`.

- **`review: true`: 0 rows** (was ~38%). The extractor reads the PDF with
  positioned words and slices each row into columns by x offset, so names can no
  longer drift off their addresses.
- **Rewritten 2026-07-30.** The previous version parsed `reference/sound_list.txt`
  (`pdftotext -layout`), whose interleaved columns drift apart wherever a
  Description cell wraps. That put wrong names on a whole region of EFFECT 1 /
  EFFECT 2, promoted stray Default tokens (`40`, `7F`) to parameter names, and
  split every 2-byte parameter's MSB/LSB doc-lines into two entries — doubling the
  block stride, so `VARIATION PARAMETER 1` landed at 0x44 instead of 0x42. The
  flagged rows were NOT "good apart from the description": their addresses were
  wrong too.
- **Verified** against `reference/syx examples/all-factory-default.syx`: 303 of 304
  comparable addresses match the default the manual prints beside them (the one
  exception is glossed "depends on chorus type"). The independent hand-written
  `fxaddr` in the plugin (`Source/EffectAddressing.h`) agrees on the variation
  block. 6228/6228 codec tests pass.

### `data/effects.json` — effect types + per-effect parameters
- **`types`**: Effect Type Lists (No / MSB / LSB / name) — reverb 13, chorus 15,
  variation 72, insertion 44. Clean; these are the authoritative effect names.
- **`param_lists`**: 36 effect blocks, **241 parameters** (name / display range /
  raw value range / conversion-table ref). Only **15 rows** flagged for review.
  - Remaining manual touch-up: ~3 blocks come out `(unlabeled)` and a few block
    names merge two adjacent effects (e.g. `STAGE1, 2 PLATE`); fix by matching
    each block to `types`. The per-row parameter data itself is clean.
  - Each param's `table` ref (`table#1`..`table#11`) resolves into
    `data/tables.json` (below) for raw→display value conversion.

### `data/tables.json` — Effect Data Assign Table (raw → display conversion)
The 11 conversion tables (`Table#1`..`Table#11`, PDF pp25-27) that the effect
params point at: each maps a raw SysEx value to its displayed value + unit, e.g.
`table#1` LFO Frequency 0→`0.00` … 127→`39.7` (Hz), `table#3` EQ Frequency
0→`THRU(20)` … (kHz), `table#10` Compressor Ratio 0→`1.0` … 7→`20.0`. Stored as
`values[]` indexed by the raw Data value (0-based); tables vary in length
(8–128 entries) and have no gaps. **All 11 refs in `effects.json` resolve and
every referenced value range fits its table.** Clean; no review needed. (Note:
`table#11`'s name is `Reverb Width; Depth; Height` — three params legitimately
share one table.) Extractor splits each Data/Value column-pair by an x-boundary
just right of the Data number, since Values are right-aligned and can be wider
than the column (`THRU(20)`, `1.0k`).

### `data/voices.json` — XG voice list for the patch browser

> **This section describes the SUPERSEDED PDF extractor** (`extract_voices.py`),
> which is kept for its account of the sound-list grid geometry. `voices.json` is
> now built by **`build_voices.py`** from XGEdit's `XGVoice.ini` (full display
> names, 1260 voices) + `drums.json` (kit names) + `short_names.json` (the
> 8-character LCD names, in each voice's `short` field). `extract_voices.py` is
> deliberately no longer in `run_all.py`: it WRITES `voices.json`, so running it
> would replace the full names with the abbreviations.

The MU100 has **1267 total voices**; the manual headlines **XG mode = 1074
voices** (the remaining ~193 are the separate **TG300B** and **C/M** lists, PDF
p90+, plus 46 drum kits). The extractor now reads **1099 grid cells** =
**1067 XG-bank voices + 32 "MU100 native" voices** (the MSB0/LSB126 bank, tagged
`"native": true`). 1067 is within 7 of the manual's 1074 headline; the small
residual is manual rounding / a couple of `+`-fragment edge cells.

Each voice has program number (1-128), Bank MSB, Bank LSB, GM family `category`,
and the MU100 8-char abbreviated `name`. **Key layout insight (corrected
2026-06-21):** the sound list is printed as two-page **spreads** — a titled
LEFT page (even: 60, 62, …, 72, 74, …, 86) carrying the program column + first
bank columns, and an untitled facing RIGHT page (odd: 61, 63, …) that continues
the *same* program rows with *more* bank columns but no program column of its
own. Programs 1-64 are the spreads from page 60, 65-128 from page 74. Most
voices are Bank MSB 0; the "MODEL EXCLUSIVE VOICE" / SFX spreads (left pages 72,
86) use MSB 48 (+ one MSB-64 group) with their header rows lower on the page.
The reader is geometry-driven and uniform across all spreads: it finds the Bank
Select MSB/LSB header rows by their label words on the left page, reads the
program rows from the left page, then reads name cells from *both* pages against
those rows (the facing page shares the spread's row grid). The LSB-0 Basic
column and its "MU100 Native" re-spelling repeat on every spread and are
de-duped; the far-left GM family-label column is rejected by a max-distance
guard.

- The previous **679** figure was wrong on two counts: (1) it was actually 657
  real melodic voices + ~22 SFX rows misparsed as MSB0 (the old fixed-y header
  reader landed page 72's MSB-48 header row in the melodic LSB band), and
  (2) it missed *every* odd continuation page — roughly a third of the voices
  (extra banks LSB 14/16-21/28-29/32-36/42-43/45/52-54/69-75/81-87/100-101/126,
  etc.). Reading the continuation pages, not the SFX bank, is what closed the
  gap.
- Spot-checked against known XG variations: e.g. pgm 1 → GrandPno / GrndPnoK /
  MelloGrP (LSB18, a continuation-page voice) / PianoStr / Dream / ConGrnd / …
  plus its MSB48 SFX bank (MtdClavi/Beeline/Rage/…) and MSB64 (CuttngNz). Names
  are abbreviations by design (full display names deferred — chosen 2026-06-21).
- **SFX bank** (MSB 48 + MSB 64) is genuinely sparse — only **157 voices**, not
  the ~395 once estimated. It does *not* extend to programs 65-128 in MSB48
  (page 86 carries only the basic column there); page 87 carries only the MSB64
  group. All four SFX pages (72/73/86/87) are fully read.

### `data/short_names.json` — the 8-character LCD voice names
The names the MU100's own LCD prints — `GrandPno` for `Grand Piano` — keyed
`"MSB/LSB/PGM"` to match `voices.json`, which `build_voices.py` merges in as each
voice's `short`. The plugin's Options ▸ Voice names swaps the two forms.
`tools/extract_short_names.py` builds it; two sources, with distinct jobs:

- **The Sound List PDF's XG Voice List grid gives the MAPPING** (same spreads the
  section above describes, read by word coordinates).
- **The program ROM gives the STRINGS.** Every voice record in `xu50720.ic11`
  starts with a space-padded 8-byte name field, so an exact 8-byte match verifies a
  cell was read correctly — and where the printed list and the shipped firmware
  disagree, **the ROM wins**, because the ROM is what the plugin's Hardware view
  renders. Exactly one name does: `0/40/74` prints `Brthy Fl`, a string that occurs
  nowhere in the ROM, against the ROM's `NeatBrth`. It is an `OVERRIDES` entry.
  The ROM dump is byte-swapped relative to CPU order (MAME loads it with
  `ROM_LOAD16_WORD_SWAP`), so it has to be unswapped before any string search.
  Pass `--rom <path>` to run the check; the checked-in JSON is what the build
  consumes, so the ROM is only needed to REGENERATE.

Two groups are not in the grid and are filled in by the tool: **MSB0/LSB127**
("MU Basic Map") prints an EMPTY column, which is how the list says "same as bank
0"; and the **37 drum/SFX kits** come from a contiguous 8-byte-per-entry table in
the ROM, checked verbatim. Coverage is asserted against `voices.json` — all 1260
keys present, nothing spare — and every name is checked to be ≤ 8 characters.

One grid trap worth knowing: the **"MU100 Native" column on each left page prints
its Bank Select LSB as 0**, because in native *mode* the LSB is 0 — but it is the
LSB **126** map (a later page prints the same names under 126). Left as 0 it
collides with the "MU100 Basic" LSB 0 column beside it, which is what the older
extractor's "de-duped" note is really describing.

### `data/drums.json` — XG Drum Map (kits + per-note drum voices)
The **37 XG drum kits** (30 drum kits at Bank MSB 127 + 7 SFX kits at MSB 126,
PDF pp101-105) with each kit's **per-note drum-voice names** (1243 note entries
over notes 13-91). Each kit has `msb`/`lsb`/`pgm`/`name` and a `keys` map of MIDI
note → drum-voice name. Note names are derived arithmetically (Yamaha C-2..G8),
not scraped. Validated: the Standard Kit (127/0/1) matches the canonical XG drum
map exactly (35 Kick Tight, 36 Kick, 38 Snare, 42 Hi-Hat Closed, 46 Hi-Hat Open,
49 Crash, 45/47/50 Low/Mid/High Tom, …).
- **Inheritance:** a variation kit prints only the notes whose voice *differs*
  from its base, so a note absent from a kit inherits the base kit (Standard Kit
  127/0/1 for MSB 127, SFX Kit 1 126/0/1 for MSB 126). Cells are stored **as
  printed** (lossless); the editor merges with the base for a full map. Hence the
  varied key counts (Standard 72, Standard Kit 2 only 7 = its differences, Bright
  72, MU Basic 0 = identical to base).
- Layout gotchas handled: kit-column right edges are anchored on the per-kit `E`
  (element-count) headers; the Note# column x **shifts between pages** (some carry
  an extra left column) so it's found as the x-band forming the note run, not a
  fixed x; alternate-assign codes (lone `V`/`B` before a footnote) and `***`/`****`
  MU-extension footnotes are stripped, while name-internal letters (size codes
  H/L/M, "Hi Q") are kept.
- Skipped (matching the rest of the pipeline): the **C/M drum maps** (pp104-105)
  and **TG300B** voice/drum lists (pp106-108). The once-quoted "46 drum kits"
  likely counted the C/M kits too.

### `data/drum_defaults.json` — every kit's per-key DEFAULTS (harvested, not read)
The only dataset here that does **not** come from a PDF. It is what the device
re-initialises a DRUM SETUP buffer to when a part's PROGRAM is written: 37 kits x
79 keys (notes 13..91) x 34 bytes, in the four separately-addressed sub-blocks a
Dump Request answers (`00` 16 bytes, `20` 14, `50` 2, `60` 2).

Harvested off the **emulated** device by the `MU100_DIAG_DRUMHARVEST=1` diag probe
in the plugin, which loads each kit in turn and reads all 316 sub-blocks straight
back — so it is the same device the plugin ships with, and there is no spec to
mis-read and no version skew. Re-harvest only if the MAME build's behaviour
changes; the JSON lands in `<Documents>/MEGAN XG/` and is copied here.

Pipeline: `python tools/gen_drum_defaults.py` → `codec/MU100DrumDefaults.h`
(compiled directly by the plugin). Rows are **pooled**: 266 of 2923 key rows are
distinct, so the table is ~9 KB of rows plus 2923 indices rather than 99 KB. The
generator refuses to bake a partial or all-identical harvest rather than emit
something subtly wrong. Why it exists: writing a part's PROGRAM makes the device re-initialise the
DRUM SETUP buffer to the new kit's defaults, so the editor has to know those
defaults to keep its model in step with the device.

### `data/tooltips.csv` — editor control hover text (mined from XGEdit95.exe)
Full-text descriptions for the editor's abbreviated controls, mostly extracted
from `xgedit95.exe`'s Windows `RT_STRING` resources. Human-editable
(`group,key,context,tooltip,confidence,xg_id`; blank tooltip = no popup).
Pipeline: edit `data/tooltips.csv` → `python tools/gen_tooltips.py` →
`codec/MU100Tooltips.h`, which the plugin compiles directly (no copy step).
`tools/build_tooltips.py` (bootstrap) and `tools/migrate_tooltips.py` (one-time
2026-07-16 expansion) **overwrite the CSV — don't re-run them**; the CSV is now
the hand-maintained source of truth. Key convention + the `mu100::tooltips`
header API (`forKey`/`forParam`/`forEffect`/`forEffectSlot`) are documented in
`tools/build_tooltips.py` (the keys) and in the generated header (the API).

## Known remaining gaps (the real cleanup cost)
1. **Voice list — DONE** (the full XG grid, melodic + SFX, 1099 cells). Open
   sub-items: (a) **Full (non-abbreviated) display names** — the PDF only has
   8-char abbreviations; cross-reference an XG voice-name list or log from XGEdit
   / the emulator. (b) A handful of `+`-fragment / 2-char names (e.g. pgm6/LSB80
   `'+'`) are split-cell artifacts worth a spot-fix. (c) **TG300B + C/M voice
   lists** (~193 voices, PDF p90+) are still not extracted — only needed if the
   editor supports those compatibility modes (recommended SKIP for now).
2. **Effect parameter Description text in `parameters.json`** — superseded by
   `effects.json` display ranges; the EFFECT tables there can lean on effects.json.
3. **Conversion tables — DONE** (`data/tables.json`, all 11, every effect ref
   resolves). See its section above.
4. **Drum kits / per-key drum voices — DONE** (`data/drums.json`, 37 kits, 1243
   note entries; Standard Kit validated vs canonical XG). The **bulk-dump request
   framing** and live Drum Setup edit round-trip are still runtime-behaviour items
   to verify against the emulator (not PDF data).

## Verdict
The address map, effect tables, the full XG voice grid (1099 cells, melodic +
SFX), the 11 raw→display conversion tables, and the 37-kit XG drum map — the bulk
of what the editor needs — all extract cleanly from the PDF. The remaining work is
**full display names** (the PDF only carries 8-char voice abbreviations);
everything else is spot-fixes against authoritative cross-references.
