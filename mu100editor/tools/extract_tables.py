#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""
Extract the Effect Data Assign Table (the raw-value -> displayed-value conversion
tables, "Table#1".."Table#11") from the Sound List PDF into data/tables.json.

These tables are what the effect-parameter `table` refs in data/effects.json
point at: e.g. an effect's "LFO Frequency" parameter stores a raw 0-127 value, and
Table#1 maps each raw value to its displayed value (0.00 .. 39.7 Hz). The editor
needs them to show real units next to each effect knob.

Layout (pages 23-25, printed 25-27): each page holds several tables in a grid.
A table = a `Table#N` marker, a name line just below it (e.g. "EQ Frequency",
"Compressor Attack Time"), a `Data Value` header repeated for each column-pair,
then rows. The data is column-major: pair 1 holds Data 0..k, pair 2 the next
run, etc. Wide tables have 4 pairs (Data 0-127); narrow ones a single pair (e.g.
Compressor Ratio, Data 0-7). Data numbers are left-aligned at a fixed x; Values
are right-aligned and can be wider than the column ("THRU(20)", "1.0k"), so we
split each pair by an x-boundary just right of the Data number rather than by
nearest column centre.

Run:  python tools/extract_tables.py
"""
import json
import re
import sys
from pathlib import Path

import fitz

ROOT = Path(__file__).resolve().parent.parent
PDF = ROOT / "reference" / "MU100_sound_list_midi_data.pdf"
OUT = ROOT / "data" / "tables.json"

PAGES = (23, 24, 25)          # the "Effect Data Assign Table" pages
MARKER_RE = re.compile(r"Table#(\d+)$")
HDR_BELOW = 30                # header sits within this many pt below the marker
DATA_HALFWIDTH = 19           # a Data number sits within +/- this of its header x
ROW_TOL = 2                   # words within this y are the same row


def markers(doc):
    """[(page, x, y, 'table#N')] for every Table#N definition marker."""
    out = []
    for pno in PAGES:
        for w in doc[pno].get_text("words"):
            m = MARKER_RE.match(w[4])
            if m:
                out.append((pno, w[0], w[1], f"table#{m.group(1)}"))
    return out


def table_columns(words, mx, my, right_limit):
    """Data-column header x's for the table at (mx, my): the 'Data' header words
    of the header row just below the marker, left of the neighbouring table."""
    hdr = sorted((w for w in words
                  if w[4] in ("Data", "Value") and my < w[1] < my + HDR_BELOW
                  and mx - 8 < w[0] < right_limit), key=lambda w: w[0])
    cols = [w[0] for w in hdr if w[4] == "Data"]
    header_y = hdr[0][1] if hdr else None
    return cols, header_y


def extract_table(page, mx, my, page_h, all_markers):
    words = page.get_text("words")

    # right edge: the next table to the right on (roughly) the same header row.
    right_limit = min((x for p, x, y, _ in all_markers
                       if p == page.number and x > mx + 20 and my - 10 < y < my + 25),
                      default=page.rect.width)

    data_x, header_y = table_columns(words, mx, my, right_limit)
    if not data_x or header_y is None:
        return None

    # name: words between the marker and the header row, left of the next table
    name = " ".join(w[4] for w in sorted(
        (w for w in words
         if my + 3 < w[1] < header_y - 2 and mx - 4 < w[0] < right_limit),
        key=lambda w: w[0]))

    span_right = min(right_limit, data_x[-1] + 120)
    # bottom edge: the next marker below that overlaps this table's x-span
    bottom = min((y for p, x, y, _ in all_markers
                  if p == page.number and y > header_y
                  and data_x[0] - 15 < x < data_x[-1] + 15), default=page_h)

    # pair i owns x in [data_x[i], data_x[i+1]); within it the Data number sits
    # near data_x[i] and the Value is everything further right.
    bounds = list(data_x) + [span_right]
    pairs = {i: {} for i in range(len(data_x))}      # pair -> {data:int -> value}
    rows = {}
    for w in words:
        if header_y + 2 < w[1] < bottom and data_x[0] - 8 < w[0] < span_right:
            rows.setdefault(round(w[1]), []).append(w)
    for _, rw in rows.items():
        for i, dx in enumerate(data_x):
            lo, hi = bounds[i], bounds[i + 1]
            cell = [w for w in rw if lo - 8 <= w[0] < hi]
            datum = [w for w in cell if w[0] < dx + DATA_HALFWIDTH and w[4].isdigit()]
            value = [w for w in cell if w[0] >= dx + DATA_HALFWIDTH]
            if not datum or not value:
                continue
            d = int(min(datum, key=lambda w: w[0])[4])
            v = " ".join(w[4] for w in sorted(value, key=lambda w: w[0]))
            pairs[i][d] = clean_value(v)

    merged = {}
    for i in sorted(pairs):
        merged.update(pairs[i])
    if not merged:
        return None
    n = max(merged) + 1
    values = [merged.get(d, "") for d in range(n)]
    gaps = [d for d in range(n) if d not in merged]
    return {"name": name or "(unnamed)", "min": min(merged), "max": max(merged),
            "count": len(merged), "values": values,
            **({"gaps": gaps} if gaps else {})}


def clean_value(s):
    s = re.sub(r"[^\x20-\x7e]", "-", s)          # non-ASCII (en-dash etc) -> '-'
    return re.sub(r"\s+", " ", s).strip()


def main():
    doc = fitz.open(str(PDF))
    mks = markers(doc)
    tables = {}
    for pno, mx, my, key in mks:
        t = extract_table(doc[pno], mx, my, doc[pno].rect.height, mks)
        if t:
            tables[key] = t

    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(json.dumps({
        "count": len(tables),
        "note": ("Effect Data Assign Table: raw value -> displayed value. "
                 "'values' is indexed by the raw Data value (0-based)."),
        "tables": tables,
    }, indent=2), encoding="utf-8")

    print(f"wrote {OUT}", file=sys.stderr)
    print(f"tables: {len(tables)}", file=sys.stderr)
    for k in sorted(tables, key=lambda k: int(k.split("#")[1])):
        t = tables[k]
        flag = f"  GAPS={t['gaps']}" if t.get("gaps") else ""
        print(f"  {k:9} {t['name']:24} Data {t['min']}-{t['max']} "
              f"({t['count']} entries){flag}", file=sys.stderr)


if __name__ == "__main__":
    main()
