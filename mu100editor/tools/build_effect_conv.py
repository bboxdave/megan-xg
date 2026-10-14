#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""
Extract the per-effect parameter conversion data (raw min-max range + the
display unit / "See Table" reference for each of the 16 knob slots) from the
MU100 Sound List PDF's "Effect Parameter List" pages, so the editor can show
real units (ms, Hz, dB, %, s, signed, ...) on every Effect-Parameters knob
instead of raw 0-127.

Pipeline stage 1 (this pass): parse the PDF tables into a structured
intermediate (data/effect_conv_raw.json) for validation. Stage 2 maps these to
the layout IDs in codec/MU100Effects.h and derives the conversion kinds; stage
3 emits the codec header.

Geometry (validated against pages 9/10): unlike the pdftotext dump (which floats
the Value/See-Table columns), the real PDF rows are cleanly y-aligned. Each page
holds two effect tables side by side, split at the gutter (~257pt). Within a
table:
  No. | Parameter | Display | Value | See Table | Control
A param row is anchored by its number (1-16) in the No. column. A param may have
a primary line plus sub-lines ~5pt below (block variants, e.g. "(variation
block)" 1-14860 vs "(insertion1,2 block)" 1-7429). Each line carries its own
Display text, a Value token (raw range "lo-hi" or single int), and an optional
"table#N" ref.

Run:  python tools/build_effect_conv.py
"""
import json
import re
import sys
from pathlib import Path

import fitz

ROOT = Path(__file__).resolve().parent.parent
PDF = ROOT / "reference" / "MU100_sound_list_midi_data.pdf"
OUT_RAW = ROOT / "data" / "effect_conv_raw.json"

PAGES = range(9, 21)        # the "Effect Parameter List" tables (PDF 0-indexed)
GUTTER_X = 257.0
HALVES = (("left", 0.0, GUTTER_X), ("right", GUTTER_X, 9999.0))

RANGE_RE = re.compile(r"^-?\d+-[-+]?\d+$|^\d+$")   # "0-69", "1-14860", "-63-+63"? no
VALUE_RE = re.compile(r"^[+-]?\d+-[+-]?\d+$|^\d+$")  # Value column tokens
TABLE_RE = re.compile(r"^table#(\d+)$")


def find_header(words):
    """Locate the 'No. Parameter Display Value See Table Control' header row in a
    set of (already half-filtered) words. Return a dict of column-left x's, or
    None. There can be several such headers per half (one per stacked table)."""
    headers = []
    by_y = {}
    for w in words:
        by_y.setdefault(round(w[1]), []).append(w)
    for y, row in by_y.items():
        texts = {w[4]: w[0] for w in row}
        if "Parameter" in texts and "Display" in texts and "Value" in texts \
           and "Control" in texts:
            no_x = min((w[0] for w in row if w[4] in ("No.", "No")), default=None)
            see_x = texts.get("See", texts.get("Table"))
            headers.append({
                "y": y,
                "no": no_x if no_x is not None else texts["Parameter"] - 12,
                "param": texts["Parameter"],
                "disp": texts["Display"],
                "value": texts["Value"],
                "table": see_x if see_x is not None else texts["Control"] - 24,
                "control": texts["Control"],
            })
    headers.sort(key=lambda h: h["y"])
    return headers


def table_name(words, header, prev_bottom, lo_x, hi_x):
    """The effect-name heading lines above a header row (between prev table's
    bottom and this header). Strips the leading 'G'/'q' bullet glyph."""
    ws = [w for w in words
          if prev_bottom < w[1] < header["y"] - 2 and lo_x <= w[0] < hi_x]
    # group by line
    lines = {}
    for w in ws:
        lines.setdefault(round(w[1]), []).append(w)
    out = []
    for y in sorted(lines):
        toks = [w[4] for w in sorted(lines[y], key=lambda w: w[0])]
        # drop the bullet glyph that pdf renders as 'G'/'q'/box at far left
        if toks and toks[0] in ("G", "q", "") and len(toks[0]) == 1:
            toks = toks[1:]
        s = " ".join(toks).strip()
        if s:
            out.append(s)
    return out


def parse_table(words, header, next_header_y, page_bottom, lo_x, hi_x):
    """Extract param rows for one table given its header columns."""
    body_top = header["y"] + 3
    body_bot = next_header_y if next_header_y else page_bottom

    # column boundaries (x); value is right-aligned so give it slack to the left
    c_no, c_par, c_disp = header["no"], header["param"], header["disp"]
    c_val, c_tab, c_ctrl = header["value"], header["table"], header["control"]

    # param-number anchors: integer tokens 1..16 in the No. column
    nums = []
    for w in words:
        if body_top < w[1] < body_bot and c_no - 4 <= w[0] < c_par - 3:
            if re.fullmatch(r"\d+", w[4]) and 1 <= int(w[4]) <= 16:
                nums.append((int(w[4]), w[1]))
    nums.sort(key=lambda t: t[1])
    if not nums:
        return None

    rows = []
    for i, (num, ny) in enumerate(nums):
        y_lo = ny - 2
        y_hi = nums[i + 1][1] - 2 if i + 1 < len(nums) else body_bot
        band = [w for w in words if y_lo <= w[1] < y_hi and lo_x <= w[0] < hi_x]
        # name: param column tokens on the anchor line (within ~3pt of ny)
        name = " ".join(w[4] for w in sorted(
            (w for w in band if c_par - 4 <= w[0] < c_disp - 3
             and abs(w[1] - ny) < 3.0), key=lambda w: w[0]))
        # split band into sub-lines by y
        sublines = {}
        for w in band:
            sublines.setdefault(round(w[1]), []).append(w)
        lines = []
        for sy in sorted(sublines):
            sw = sublines[sy]
            disp = " ".join(w[4] for w in sorted(
                (w for w in sw if c_disp - 4 <= w[0] < c_val - 6),
                key=lambda w: w[0]))
            val = None
            for w in sw:
                if c_val - 12 <= w[0] < c_tab - 3 and VALUE_RE.match(w[4]):
                    val = w[4]
            tab = None
            for w in sw:
                m = TABLE_RE.match(w[4])
                if m and c_tab - 6 <= w[0] < c_ctrl + 4:
                    tab = f"table#{m.group(1)}"
            if disp or val or tab:
                lines.append({"display": disp, "value": val, "table": tab})
        rows.append({"num": num, "name": name.strip(), "lines": lines})
    return rows


def main():
    doc = fitz.open(str(PDF))
    tables = []
    for pno in PAGES:
        page = doc[pno]
        page_bottom = page.rect.height - 20
        for side, lo, hi in HALVES:
            words = [w for w in page.get_text("words") if lo <= w[0] < hi]
            words.sort(key=lambda w: (round(w[1]), w[0]))
            headers = find_header(words)
            for hi_i, hdr in enumerate(headers):
                nxt = headers[hi_i + 1]["y"] if hi_i + 1 < len(headers) else None
                prev_bottom = headers[hi_i - 1]["y"] + 200 if hi_i > 0 else 0
                # heading lines sit just above this header; cap search to ~60pt
                name = table_name(words, hdr, max(prev_bottom, hdr["y"] - 70),
                                  lo, hi)
                rows = parse_table(words, hdr, nxt, page_bottom, lo, hi)
                if rows:
                    tables.append({
                        "page": pno, "side": side,
                        "heading": name, "rows": rows,
                    })

    OUT_RAW.write_text(json.dumps(tables, indent=2, ensure_ascii=False),
                       encoding="utf-8")
    print(f"wrote {OUT_RAW}  ({len(tables)} tables)", file=sys.stderr)
    for t in tables:
        head = " / ".join(t["heading"])[:60]
        print(f"  p{t['page']:>2} {t['side']:<5} {len(t['rows']):>2} rows  {head}",
              file=sys.stderr)


if __name__ == "__main__":
    main()
