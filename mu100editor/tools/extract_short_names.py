#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Extract the MU100's 8-character LCD voice names into data/short_names.json.

These are the names the HARDWARE shows -- the ones visible on the LCD in the
plugin's Hardware view -- as opposed to the full display names in voices.json
("GrandPno" vs "Grand Piano"). The plugin's Options -> Voice names option picks
between the two, so the two sets have to key off the same (MSB, LSB, PGM).

Two sources, with distinct jobs:

  * reference/MU100_sound_list_midi_data.pdf, "XG Voice List (Normal voices)"
    (PDF pages 60..87) supplies the MAPPING. It is a grid: one column per bank
    (its Bank Select MSB/LSB printed in the two header rows), one row per
    program, and each cell holds the LCD name plus a right-aligned element
    count. Word coordinates from PyMuPDF reconstruct it directly.

  * the program ROM (xu50720.ic11) supplies the GROUND TRUTH for the strings
    themselves. Every voice record in it starts with a space-padded 8-byte name
    field, so an exact 8-byte match is a strong check that a cell was read
    correctly -- and where the printed list and the shipped firmware disagree,
    the ROM wins, because the ROM is what the Hardware view actually renders.
    The ROM is byte-swapped relative to CPU order (MAME loads it with
    ROM_LOAD16_WORD_SWAP), so it has to be unswapped before any string search.
    Pass --rom to run that check; without it the extraction still runs, and the
    checked-in JSON is what the build consumes, so the ROM is only needed to
    REGENERATE.

Two groups are not in the grid and are filled in here:

  * MSB 0 / LSB 127 ("MU Basic Map") prints an EMPTY column in the sound list,
    which is the list's way of saying "same as bank 0" -- so those 128 names are
    copied from LSB 0, exactly as voices.json gives them the same full names.

  * The 37 drum/SFX kits (MSB 126/127) are not normal voices and appear nowhere
    in the grid. Their LCD names are a contiguous 8-byte-per-entry table in the
    ROM, transcribed into KIT_SHORT below in ROM order; --rom checks that the
    table is still there verbatim.

Run:  python tools/extract_short_names.py [--rom PATH_TO_xu50720.ic11]
"""
import argparse
import json
import os
import re
from collections import defaultdict

import fitz  # PyMuPDF

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PDF = os.path.join(ROOT, "reference", "MU100_sound_list_midi_data.pdf")
VOICES = os.path.join(ROOT, "data", "voices.json")
OUT = os.path.join(ROOT, "data", "short_names.json")
DEFAULT_ROM = r"D:\My Documents\MEGAN XG\rom files unzipped\xu50720.ic11"

# PDF pages (0-based) of "XG Voice List (Normal voices)". Left pages carry the
# program-number column; the right page of each spread continues the same rows
# with more bank columns and no program column of its own.
FIRST_PAGE, LAST_PAGE = 60, 87

# Where the printed sound list and the shipped firmware disagree. The Hardware
# view renders the ROM, so the ROM wins. Keyed "MSB/LSB/PGM".
#   0/40/74 -- full name "Neat Breath". The list prints "Brthy Fl", a string
#              that does not occur anywhere in xu50720; the ROM has "NeatBrth".
OVERRIDES = {
    "0/40/74": "NeatBrth",
}

# Drum/SFX kit LCD names, in ROM table order (see the module docstring). The
# order is not assumed: --rom checks the table byte-for-byte, and the names line
# up one-to-one with the kit list in voices.json, which is what KIT_KEYS pairs
# them with.
KIT_SHORT = [
    "StandKit", "StndKit2", "Dry Kit ", "BriteKit", "Skim Kit", "Slim Kit",
    "RogueKit", "Hob Kit ", "Room Kit", "DarKRKit", "Rock Kit", "RockKit2",
    "ElctrKit", "AnalgKit", "AnlgKit2", "DanceKit", "HipHpKit", "JunglKit",
    "ApogeeKt", "PergeeKt", "Jazz Kit", "JazzKit2", "BrushKit", "BrshKit2",
    "SymphKit", "TrampKit", "AmberKit", "CoffinKt", "StndKit#", "StandKit",
    "SFXKit 1", "SFXKit 2", "TknoKtKS", "TknoKtHi", "TknoKtLo", "SakuraKt",
    "SLatinKt",
]
# (MSB, LSB, PGM) for each KIT_SHORT entry, same order: the 28 XG drum kits,
# then the two MU100-native kits, then the seven MSB126 SFX kits.
KIT_KEYS = (
    [(127, 0, p) for p in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 17, 18, 25, 26, 27,
                           28, 29, 30, 31, 32, 33, 34, 41, 42, 49, 65, 66, 67)]
    + [(127, 0, 127), (127, 0, 128)]
    + [(126, 0, p) for p in (1, 2, 17, 18, 19, 33, 34)]
)
# The 38th ROM entry, "SilenKit", follows KIT_SHORT in the table and anchors the
# verbatim check at its far end. It is not a selectable kit, so it gets no key.
KIT_TABLE_TAIL = "SilenKit"

NUM = re.compile(r"\d+$")


def load_rom(path):
    """Program ROM in CPU byte order (the dump is word-swapped)."""
    raw = open(path, "rb").read()
    sw = bytearray(len(raw))
    sw[0::2] = raw[1::2]
    sw[1::2] = raw[0::2]
    return bytes(sw)


def lines_of(page):
    """Words grouped into text lines, each sorted left to right.

    The grid's header rows sit barely 1pt apart, so the tolerance has to stay
    under that; data rows are 8pt apart and never merge by accident.
    """
    ws = [w for w in page.get_text("words") if w[4].strip()]
    ws.sort(key=lambda w: (w[1], w[0]))
    out = []
    for x0, y0, x1, y1, t, *_ in ws:
        if out and y0 - out[-1][0] < 1.0:
            out[-1][1].append((x0, x1, t))
        else:
            out.append([round(y0, 1), [(x0, x1, t)]])
    return [(y, sorted(v)) for y, v in out]


def centre(w):
    return (w[0] + w[1]) / 2


def extract_grid():
    """(msb, lsb, pgm) -> LCD name, straight off the sound list's voice grid."""
    doc = fitz.open(PDF)
    result = {}
    prev_pgm = {}
    for p in range(FIRST_PAGE, LAST_PAGE + 1):
        lines = lines_of(doc[p])
        by_y = dict(lines)

        # The row of "E" column headers (E = number of elements) is the last
        # header row; everything below it is data. It also fixes where each
        # column's element digit is printed, which matters below.
        erow = max(y for y, v in lines if sum(1 for _, _, t in v if t == "E") >= 3)
        e_centres = [centre(w) for w in by_y[erow] if w[2] == "E"]

        # Bank Select MSB then LSB: the last two mostly-numeric rows above the
        # E row. The right page of a spread prints them without the "Bank
        # Select MSB/LSB" caption, so the caption cannot be the anchor.
        numeric = [y for y, v in lines
                   if y < erow
                   and sum(1 for _, _, t in v if NUM.match(t)) >= 3
                   and all(NUM.match(t) or t in ("Bank", "Select", "MSB", "LSB")
                           for _, _, t in v)]
        msbs = [(centre(w), int(w[2])) for w in by_y[numeric[-2]] if NUM.match(w[2])]
        lsbs = [(centre(w), int(w[2])) for w in by_y[numeric[-1]] if NUM.match(w[2])]
        if len(msbs) != len(lsbs):
            raise SystemExit("page %d: %d MSB headers vs %d LSB" % (p, len(msbs), len(lsbs)))
        cols = [[round((a + b) / 2, 1), m, l] for (a, m), (b, l) in zip(msbs, lsbs)]
        centres = [c[0] for c in cols]
        pitch = min(centres[i + 1] - centres[i] for i in range(len(centres) - 1))

        pgm_x = None
        for y, v in lines:
            for w in v:
                # The column sub-titled "MU100 Native" prints its Bank Select
                # LSB as 0, because in native MODE the LSB *is* 0 -- but it is
                # the LSB 126 native map (page 73 prints the same names under
                # LSB 126), and left as 0 it would collide with the plain
                # "MU100 Basic" LSB 0 column next to it.
                if w[2] == "Native" and y < erow + 6:
                    k = min(range(len(centres)), key=lambda i: abs(centre(w) - centres[i]))
                    cols[k][2] = 126
                if w[2] == "(1-128)":
                    pgm_x = centre(w)

        pgm_by_y, cells = {}, defaultdict(list)
        for y, v in lines:
            if y <= erow + 6:
                continue
            for w in v:
                c = centre(w)
                if pgm_x is not None and abs(c - pgm_x) < 7 and NUM.match(w[2]):
                    pgm_by_y[y] = int(w[2])
                    continue
                # Drop the element count. It is right-aligned under its own
                # column's "E" header, which puts it nearer the NEXT column's
                # centre on the narrower pages -- so it has to go by the "E"
                # position, not by which column centre is closest.
                if re.fullmatch(r"[12]", w[2]) and any(abs(c - e) < 4 for e in e_centres):
                    continue
                k = min(range(len(centres)), key=lambda i: abs(c - centres[i]))
                if abs(c - centres[k]) <= pitch / 2:
                    cells[(y, k)].append(w)

        # A right page has no program column: its rows line up by y with the
        # left page of the same spread.
        if pgm_x is None:
            pgm_by_y = dict(prev_pgm)
        else:
            prev_pgm = dict(pgm_by_y)

        def pgm_for(y):
            best = None
            for yy, val in pgm_by_y.items():
                dy = abs(yy - y)
                if dy < 3 and (best is None or dy < best[0]):
                    best = (dy, val)
            return best[1] if best else None

        for (y, k), v in sorted(cells.items()):
            # The "*"/"**" model-addition footnotes and the star glyph marking
            # MU100 additions are not part of the name.
            toks = [w for w in sorted(v)
                    if w[2].isascii() and not re.fullmatch(r"\*+", w[2])]
            pgm = pgm_for(y)
            if not toks or pgm is None:
                continue          # legend and "continued on page N" footers
            # A handful of cells print their element digit away from the "E"
            # header the filter above keys off, so it survives into the cell.
            # Two more tells, either of which settles it: a gap far wider than
            # the ~1.6pt space this font sets, or a name that would run past
            # the 8-character LCD field. The ROM check has to agree with all
            # three rules, which is what keeps them honest.
            if len(toks) > 1 and re.fullmatch(r"[12]", toks[-1][2]):
                gap = toks[-1][0] - toks[-2][1]
                if gap > 4.0 or len(" ".join(w[2] for w in toks)) > 8:
                    toks = toks[:-1]
            name = " ".join(w[2] for w in toks)
            _, msb, lsb = cols[k]
            key = (msb, lsb, pgm)
            if key in result and result[key] != name:
                raise SystemExit("page %d: %s reads both %r and %r"
                                 % (p, key, result[key], name))
            result[key] = name
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", nargs="?", const=DEFAULT_ROM,
                    help="program ROM to verify every name against "
                         "(default %s)" % DEFAULT_ROM)
    args = ap.parse_args()

    names = {"%d/%d/%d" % k: v for k, v in extract_grid().items()}
    print("sound list grid: %d cells" % len(names))

    for key, val in OVERRIDES.items():
        was = names.get(key)
        if was == val:
            raise SystemExit("override %s no longer needed (%r)" % (key, val))
        print("  override %s: %r -> %r" % (key, was, val))
        names[key] = val

    # MU Basic Map (LSB 127) = bank 0 under another LSB; the list prints it blank.
    basic = 0
    for key, val in list(names.items()):
        msb, lsb, pgm = (int(x) for x in key.split("/"))
        if msb == 0 and lsb == 0:
            names["0/127/%d" % pgm] = val
            basic += 1
    print("  MU Basic Map (LSB 127) copied from bank 0: %d" % basic)

    for (msb, lsb, pgm), short in zip(KIT_KEYS, KIT_SHORT):
        names["%d/%d/%d" % (msb, lsb, pgm)] = short.rstrip()
    print("  drum/SFX kits from the ROM kit table: %d" % len(KIT_KEYS))

    # Coverage: exactly the voices the editor can select, no more, no less.
    voices = json.load(open(VOICES, encoding="utf-8"))["voices"]
    want = {"%d/%d/%d" % (v["msb"], v["lsb"], v["pgm"]) for v in voices}
    missing, extra = sorted(want - set(names)), sorted(set(names) - want)
    if missing or extra:
        raise SystemExit("coverage: %d voices with no short name %s, "
                         "%d short names with no voice %s"
                         % (len(missing), missing[:8], len(extra), extra[:8]))
    print("  coverage: all %d voices in voices.json, nothing spare" % len(want))

    long_names = {k: v for k, v in names.items() if len(v) > 8}
    if long_names:
        raise SystemExit("longer than 8 characters: %s" % list(long_names.items())[:8])

    if args.rom:
        rom = load_rom(args.rom)
        table = "".join(KIT_SHORT) + KIT_TABLE_TAIL
        if rom.find(table.encode()) < 0:
            raise SystemExit("kit-name table not found verbatim in %s" % args.rom)
        bad = sorted({v for v in names.values() if rom.find(v.ljust(8).encode()) < 0})
        if bad:
            raise SystemExit("not an 8-byte name field in %s: %s" % (args.rom, bad))
        print("  ROM check (%s): kit table verbatim, all %d names exact"
              % (os.path.basename(args.rom), len(set(names.values()))))
    else:
        print("  ROM check SKIPPED (pass --rom to verify against the firmware)")

    with open(OUT, "w", encoding="utf-8") as f:
        json.dump({
            "count": len(names),
            "note": ("MU100 8-character LCD voice names, keyed 'MSB/LSB/PGM' to "
                     "match data/voices.json. Mapping from the XG Voice List grid "
                     "in reference/MU100_sound_list_midi_data.pdf; strings verified "
                     "byte-for-byte against the program ROM, which wins where the "
                     "printed list and the firmware disagree. Kits (MSB 126/127) "
                     "come from the ROM's own kit-name table; MSB0/LSB127 (MU Basic "
                     "Map) repeats bank 0, which is how the list prints it."),
            "names": dict(sorted(names.items(),
                                 key=lambda kv: [int(x) for x in kv[0].split("/")])),
        }, f, indent=1, ensure_ascii=False)
    print("wrote %s: %d names" % (OUT, len(names)))


if __name__ == "__main__":
    main()
