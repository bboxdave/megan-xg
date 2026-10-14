#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""
Extract the XG Drum Map (drum kits + per-note drum-voice assignments) from the
Sound List PDF into data/drums.json.

Source: pages 99-103 (printed 101-105), "XG Drum Map". Each page is a grid of up
to ~10 kit columns; each column is one drum kit (Bank MSB 127 = drum kits, MSB
126 = SFX kits; Program# selects the kit). Rows are MIDI notes (13-91). A cell is
that kit's drum-voice name for that note.

Layout notes that drive the parser:
  * Each kit column header is "<kit name> E" (E = element-count sub-column). The
    'E' header x's are stable per-kit anchors, so we use them as column right
    edges (cell i spans from the previous E to this E).
  * A cell holds: the drum-voice name, then optionally an alternate-assign code
    (a lone 'V'/'B' RIGHT BEFORE a '***'/'****' footnote), the MU-extension
    footnote itself, and the element count digit. We keep the name and drop the
    rest. (Single letters that are part of a name, e.g. "Hi Q", are kept because
    they are NOT immediately before a footnote.)
  * Blank cells are SHADED, and the shade is the whole meaning -- the legend on
    the last drum-map page spells out both: light grey (fill 0.9) = "Same as
    Standard Kit", dark grey (fill 0.5) = "No sound". So a blank is NOT always
    an inherit. We read the fill under each blank cell and emit the light ones
    as that kit's "inherits" list; everything else blank is silent.
    Measured over all five pages: every MSB127 kit's dark blanks are exactly
    notes 85-91 (the notes Standard Kit itself does not define), and EVERY blank
    on the SFX page (MSB126) is dark -- the SFX/Techno/Sakura/Latin kits have no
    base kit at all and inherit nothing.

The C/M drum maps (pp104-105) and TG300B voice lists (pp106-108) are intentionally
skipped, matching the rest of the pipeline.

A very small number of kit names are normalised on the way out; see
KIT_NAME_OVERRIDES below for the list and the reasoning.

Run:  python tools/extract_drums.py
"""
import json
import re
import sys
from pathlib import Path

import fitz

ROOT = Path(__file__).resolve().parent.parent
PDF = ROOT / "reference" / "MU100_sound_list_midi_data.pdf"
OUT = ROOT / "data" / "drums.json"

PAGES = (99, 100, 101, 102, 103)
ROW_TOL = 3
FOOTNOTE = re.compile(r"\*+")
PITCH = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# Kit-name overrides, keyed by (bank MSB, bank LSB, program) -- an ADDRESS, not a
# name, so a re-extraction cannot silently detach an override from its kit. Same
# discipline as extract_parameters.py's OVERRIDES dict, and for the same reason:
# data/drums.json is FULLY REGENERATED, so a hand-edit to the JSON dies on the
# next run without a word.
#
# The PDF really does print "Brush Kit2" (p101, verified in the raw text) while
# every other numbered kit in the same list is spaced -- Standard Kit 2, Rock
# Kit 2, Analog Kit 2, Jazz Kit 2, SFX Kit 2. That is Yamaha's own typo, and it
# is the one kit name out of 37 that reads as a mistake on screen. Normalised on
# the user's call, 2026-08-21. This is the ONLY place any kit name is edited:
# build_voices.py takes voices.json's kit names from this file, so the drum data
# and the voice list cannot disagree.
KIT_NAME_OVERRIDES = {
    (127, 0, 42): "Brush Kit 2",     # PDF prints "Brush Kit2"
}


def note_name(n):
    # Yamaha convention: note 0 = C-2 (so note 13 = C#-1, note 36 = C1).
    return f"{PITCH[n % 12]}{n // 12 - 2}"


def header_row_y(page):
    """y of the kit-name / column-header row (the row carrying 'Note#')."""
    ys = [w[1] for w in page.get_text("words") if w[4] == "Note#"]
    return min(ys) if ys else None


def kit_columns(page, hy):
    """[(msb, lsb, pgm, name, e_x)] for each kit column on the page, where e_x is
    the column's 'E' header x (its right edge)."""
    words = page.get_text("words")

    def hdr_vals(ylo, yhi):
        return sorted((w[0], w[4]) for w in words
                      if ylo <= w[1] < yhi and w[4].isdigit())

    msb = hdr_vals(53, 57)
    lsb = hdr_vals(60, 65)
    pgm = hdr_vals(68, 72)
    e_x = sorted(w[0] for w in words if w[4] == "E" and abs(w[1] - hy) < 1.5)
    if not (msb and lsb and pgm and e_x):
        return []

    # name-row tokens (drop the fixed left-column headers and the E markers)
    skip = {"E", "Note#", "Note", "Off", "Group", "Key", "Alternate", "assign"}
    toks = sorted((w[0], w[4]) for w in words
                  if abs(w[1] - hy) < 1.5 and w[4] not in skip)

    cols = []
    prev_e = pgm[0][0] - 40           # left edge of the first kit
    for i, (px, pg) in enumerate(pgm):
        ex = e_x[i] if i < len(e_x) else px + 22
        name = " ".join(t for x, t in toks
                        if prev_e < x <= ex + 2 and not FOOTNOTE.fullmatch(t))
        name = re.sub(r"\s*\*+\s*$", "", name).strip()   # drop trailing **** footnote
        cols.append((int(msb[i][1]), int(lsb[i][1]), int(pg), name, ex))
        prev_e = ex
    return cols


def note_rows(page):
    """[(y, note_number)] for each data row. The Note# column's x shifts between
    pages (some pages carry an extra left column), so find it as the x-band whose
    integers form the long note run, not a fixed x window."""
    ints = sorted(((w[0], w[1], int(w[4])) for w in page.get_text("words")
                   if w[1] > 88 and w[4].isdigit() and 0 <= int(w[4]) <= 127),
                  key=lambda t: t[0])
    bands = []
    for x, y, v in ints:
        if bands and x - bands[-1][-1][0] < 6:
            bands[-1].append((x, y, v))
        else:
            bands.append([(x, y, v)])
    if not bands:
        return []
    best = max(bands, key=lambda b: len({v for _, _, v in b}))
    return sorted((y, v) for _, y, v in best)


# Blank-cell shading. The legend (last drum-map page) gives both meanings:
#   fill 0.9 (light grey) = "Same as Standard Kit"  -> inherit from the base kit
#   fill 0.5 (dark grey)  = "No sound"              -> the note is silent
INHERIT_FILL = 0.9


def fills(page):
    """[(rect, grey level)] for every filled rectangle on the page."""
    return [(d["rect"], round(d["fill"][0], 2))
            for d in page.get_drawings() if d.get("fill")]


def blank_is_inherit(rects, x, y):
    """True if the blank cell at (x, y) is shaded 'Same as Standard Kit'.

    A cell can sit inside several nested fills (the page-wide banner plus a
    per-column block), so take the DARKEST -- dark grey is painted on top of
    light and it is the one that means 'No sound'."""
    sh = [g for r, g in rects if r.x0 <= x <= r.x1 and r.y0 <= y <= r.y1]
    return bool(sh) and min(sh) == INHERIT_FILL


def cell_name(words, lo, hi, y):
    """The drum-voice name in one kit cell (x in (lo, hi]) on row y.

    Drops the element-count digit, the MU-extension footnote, and the lone
    alternate-assign code ('V'/'B', which sits just before the footnote or at the
    cell's end). Single letters that are part of a name -- size codes H/L/M, "Hi
    Q" -- are kept; only V/B are treated as assign codes."""
    toks = sorted((w for w in words if lo < w[0] <= hi and abs(w[1] - y) < ROW_TOL),
                  key=lambda w: w[0])
    texts = [w[4] for w in toks if not w[4].isdigit()]    # drop element-count first
    keep = []
    for i, t in enumerate(texts):
        if FOOTNOTE.fullmatch(t):
            continue
        nxt = texts[i + 1] if i + 1 < len(texts) else ""
        if t in ("V", "B") and (nxt == "" or FOOTNOTE.fullmatch(nxt)):
            continue
        keep.append(t)
    return re.sub(r"\s*\*+$", "", " ".join(keep)).strip()  # strip an attached footnote


def main():
    doc = fitz.open(str(PDF))
    kits, notes_seen = {}, set()
    for pno in PAGES:
        page = doc[pno]
        hy = header_row_y(page)
        if hy is None:
            continue
        cols = kit_columns(page, hy)
        rows = note_rows(page)
        words = page.get_text("words")
        rects = fills(page)
        notes_seen.update(n for _, n in rows)
        prev_e = cols[0][4] - 48 if cols else 0
        for i, (msb, lsb, pgm, name, ex) in enumerate(cols):
            lo = prev_e
            hi = ex + 3
            prev_e = ex
            key = (msb, lsb, pgm)
            keys, inherits = {}, []
            for y, n in rows:
                v = cell_name(words, lo, hi, y)
                if v:
                    keys[n] = v
                elif blank_is_inherit(rects, (lo + hi) / 2, y):
                    inherits.append(n)
            # the repeated Standard/SFX anchor column appears on several pages;
            # keep the richest (most populated) copy.
            if key not in kits or len(keys) > len(kits[key]["keys"]):
                kits[key] = {"msb": msb, "lsb": lsb, "pgm": pgm,
                             "name": KIT_NAME_OVERRIDES.get(key, name or "(unnamed)"),
                             "inherits": inherits,
                             "keys": keys}

    kit_list = sorted(kits.values(), key=lambda k: (k["msb"], k["lsb"], k["pgm"]))
    note_names = {str(n): note_name(n) for n in sorted(notes_seen)}
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(json.dumps({
        "count": len(kit_list),
        "note": ("XG Drum Map. 'keys' maps MIDI note number -> drum-voice name as "
                 "printed. 'inherits' lists the blank notes the PDF shades 'Same "
                 "as Standard Kit' (base kit 127/0/1); every other blank note is "
                 "shaded 'No sound' and is silent. The MSB126 SFX/Techno/Sakura/"
                 "Latin kits have NO base kit -- all their blanks are 'No sound', "
                 "so 'inherits' is empty for every one of them. C/M and TG300B "
                 "drum data intentionally skipped."),
        "note_names": note_names,
        "kits": kit_list,
    }, indent=2), encoding="utf-8")

    print(f"wrote {OUT}", file=sys.stderr)
    print(f"kits: {len(kit_list)} "
          f"(MSB127={sum(1 for k in kit_list if k['msb']==127)}, "
          f"MSB126={sum(1 for k in kit_list if k['msb']==126)})", file=sys.stderr)
    print(f"note range: {min(note_names, key=int)}..{max(note_names, key=int)} "
          f"({len(note_names)} notes)", file=sys.stderr)
    for k in kit_list:
        print(f"  {k['msb']}/{k['lsb']}/{k['pgm']:>3}  {k['name']:20} "
              f"{len(k['keys']):>3} keys  {len(k['inherits']):>3} inherited",
              file=sys.stderr)


if __name__ == "__main__":
    main()
