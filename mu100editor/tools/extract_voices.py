#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""
Extract the FULL XG Voice List (Normal voices) from the Sound List PDF into
data/voices.json -- the MU100's 1074 XG-mode voices.

Grid structure (decoded from page geometry):
  * The voice list is laid out as two-page **spreads**: a LEFT page (even, e.g.
    60) carries the section title, the program-number column (1-64 or 65-128), a
    family label, the Basic anchor, and the first set of Bank-Select columns; its
    facing RIGHT page (odd, e.g. 61) carries NO title and NO program column -- it
    just continues the same program rows with MORE Bank-Select columns. (XGEdit's
    original sound-list booklet is printed the same way.) Earlier versions only
    read the titled left pages and so missed roughly a third of the voices.
  * Programs 1-64 live on the spreads from page 60, programs 65-128 from page 74.
  * Most "Normal" voices use Bank MSB 0 and differ only by Bank-Select LSB. The
    "MODEL EXCLUSIVE VOICE" / SFX spreads (left pages 72 and 86) instead use Bank
    MSB 48 (plus one MSB-64 group) and put their header rows lower on the page.
  * A voice = (Program 1-128) x (Bank MSB, Bank LSB); a non-empty cell is a real
    voice. The LSB-0 Basic column (and the MU100-Native re-spelling of it) repeat
    on every spread and are de-duped here. Names are the MU100 8-char
    abbreviations (full display names are a separate later task -- see README).

The reader is geometry-driven and uniform across melodic and SFX spreads:
locate the Bank Select MSB/LSB header rows by their label words on the left page,
read the program rows from the left page's program column, then read name cells
from BOTH pages of the spread against those same row y's (the facing page shares
the spread's row grid, so its cells map onto the left page's program numbers).

Run:  python tools/extract_voices.py
"""
import json
import re
import sys
from pathlib import Path

import fitz

ROOT = Path(__file__).resolve().parent.parent
PDF = ROOT / "reference" / "MU100_sound_list_midi_data.pdf"
OUT = ROOT / "data" / "voices.json"

PGM_X = (90, 106)        # program-number column (always on the left page)
HDR_TOL = 2.0            # y tolerance when reading a header value row
ROW_TOL = 4              # name word <-> program row y tolerance
COL_OFFSET = 16          # name text sits ~16pt left of its header value
COL_MAX_DIST = 30        # reject words too far from any bank column (e.g. the
                         # far-left GM family-label column on left pages)
HDR_X_MIN = 40           # ignore page-margin digits left of the first column

GM_FAMILIES = [
    "Piano", "Chromatic Percussion", "Organ", "Guitar", "Bass", "Strings",
    "Ensemble", "Brass", "Reed", "Pipe", "Synth Lead", "Synth Pad",
    "Synth Effects", "Ethnic", "Percussive", "Sound Effects",
]


def gm_family(pgm):
    return GM_FAMILIES[(pgm - 1) // 8]


def voice_pages(doc):
    # The titled LEFT page of every spread. NB: some pages spell it "(normal
    # voices)" lowercase -- match case-insensitively.
    return [p for p in range(len(doc))
            if "xg voice list" in doc[p].get_text("text").lower()
            and "normal" in doc[p].get_text("text").lower()]


def clean_name(s):
    # strip the leading element-count glyph(s) and stray punctuation/spaces
    s = re.sub(r"[^\x20-\x7e]", "", s)          # drop non-ASCII (▣ 2-element mark)
    s = re.sub(r"\s+", " ", s).strip(" .")
    # "#" is the MU100-Native column's suffix marker (e.g. "GrandP #"); when it
    # bleeds into a neighbouring cell it appears as a leading/standalone token.
    s = re.sub(r"^#\s*", "", s).strip()
    return s


def label_row_y(page, label):
    """y of the 'Bank Select MSB'/'LSB' label word in the header band, or None."""
    ys = [w[1] for w in page.get_text("words") if w[4] == label and w[1] < 80]
    return min(ys) if ys else None


def header_columns(page, msb_y, lsb_y):
    """Return [(x, msb, lsb)] for each bank column at the given header-row y's.

    The MSB/LSB labels live only on the left page, but both pages of a spread
    share the same header-row y's, so the right page is read at the left page's
    y's. Pairs each LSB value with the nearest MSB value by x (default MSB 0)."""
    if msb_y is None or lsb_y is None:
        return []

    def values(y):
        return sorted((w[0], int(w[4])) for w in page.get_text("words")
                      if abs(w[1] - y) < HDR_TOL and w[4].isdigit() and w[0] > HDR_X_MIN)

    msb, lsb = values(msb_y), values(lsb_y)
    return [(x, (min(msb, key=lambda t: abs(t[0] - x))[1] if msb else 0), l)
            for x, l in lsb]


def program_rows(page):
    """Return sorted [(y, pgm)] from the left page's program-number column."""
    return sorted((w[1], int(w[4])) for w in page.get_text("words")
                  if PGM_X[0] <= w[0] < PGM_X[1] and w[1] > 78
                  and w[4].isdigit() and 1 <= int(w[4]) <= 128)


def name_cells(page, cols, rows):
    """Read voice cells from one page given its bank columns and the spread's
    program rows. Element-count digits and footnote marks (*, **) are dropped;
    box glyphs vanish in clean_name(); multi-word names are rejoined per cell."""
    if not cols or not rows:
        return []
    col_x = [x for x, _, _ in cols]
    cells = {}  # (pgm, col_index) -> list[(x, text)]
    for w in page.get_text("words"):
        if w[1] <= 78:
            continue
        txt = clean_name(w[4])
        if not txt or txt.isdigit() or re.fullmatch(r"\*+", txt):
            continue
        pgm = min(rows, key=lambda r: abs(r[0] - w[1]))
        if abs(pgm[0] - w[1]) > ROW_TOL:
            continue
        ci = min(range(len(col_x)), key=lambda i: abs(col_x[i] - (w[0] + COL_OFFSET)))
        if abs(col_x[ci] - (w[0] + COL_OFFSET)) > COL_MAX_DIST:
            continue                         # not under any bank column (family label)
        cells.setdefault((pgm[1], ci), []).append((w[0], txt))

    out = []
    for (pgm, ci), parts in cells.items():
        name = clean_name(" ".join(t for _, t in sorted(parts)))
        if not name:
            continue
        _, msb, lsb = cols[ci]
        v = {"pgm": pgm, "msb": msb, "lsb": lsb,
             "category": gm_family(pgm), "name": name}
        # MSB0/LSB126-127 is the "MU100 native" bank: it restates native voices
        # in XG bank space (marked '#' in the source). The manual's 1074-voice
        # headline "skips bank numbers that have the same Voice as bank 000",
        # i.e. excludes these. Tag them so the editor can filter and the count
        # reconciles to the manual.
        if msb == 0 and lsb in (126, 127):
            v["native"] = True
        out.append(v)
    return out


def extract_spread(doc, left_idx):
    """Extract every voice on the spread whose left page is `left_idx`, reading
    both the left page and its untitled facing (continuation) page."""
    left = doc[left_idx]
    msb_y, lsb_y = label_row_y(left, "MSB"), label_row_y(left, "LSB")
    rows = program_rows(left)
    voices = name_cells(left, header_columns(left, msb_y, lsb_y), rows)

    right_idx = left_idx + 1
    if right_idx < len(doc):
        right = doc[right_idx]
        # The facing page shares the spread's header-row and program-row y's.
        right_cols = header_columns(right, msb_y, lsb_y)
        if right_cols:
            voices += name_cells(right, right_cols, rows)
    return voices


def main():
    doc = fitz.open(str(PDF))
    left_pages = voice_pages(doc)

    voices = []
    for p in left_pages:
        voices.extend(extract_spread(doc, p))

    # de-dup by (pgm, msb, lsb); the Basic LSB-0 column (and its Native
    # re-spelling) repeats on every spread.
    uniq = {}
    for v in voices:
        uniq.setdefault((v["pgm"], v["msb"], v["lsb"]), v)
    voices = sorted(uniq.values(), key=lambda v: (v["pgm"], v["msb"], v["lsb"]))

    basic = [v for v in voices if v["lsb"] == 0 and v["msb"] == 0]
    native = [v for v in voices if v.get("native")]
    xg = len(voices) - len(native)
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(json.dumps({
        "count": len(voices),
        "xg_count": xg,
        "native_count": len(native),
        "note": ("MU100 XG-mode voices, abbreviated names (full display names are a "
                 "later task). 'native':true marks the MSB0/LSB126 native bank, "
                 "excluded from the manual's 1074 headline (xg_count)."),
        "voices": voices,
    }, indent=2), encoding="utf-8")

    lsbs = sorted({v["lsb"] for v in voices})
    print(f"wrote {OUT}", file=sys.stderr)
    print(f"total cells: {len(voices)}  = {xg} XG + {len(native)} native (MSB0/LSB126)",
          file=sys.stderr)
    print(f"  (manual headline: 1074 XG voices; basic MSB0/LSB0: {len(basic)})", file=sys.stderr)
    print(f"distinct bank LSBs ({len(lsbs)}): {lsbs}", file=sys.stderr)
    for msb in sorted({v["msb"] for v in voices}):
        n = sum(1 for v in voices if v["msb"] == msb)
        print(f"  MSB {msb}: {n} voices", file=sys.stderr)


if __name__ == "__main__":
    main()
