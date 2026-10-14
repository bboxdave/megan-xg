# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors

"""
Shared PyMuPDF helpers for the MU100 data extractors.

The Sound List PDF lays many tables out in TWO side-by-side columns. `pdftotext
-layout` interleaves them; here we split each page into a left/right half by x
position and reconstruct rows (words grouped by y, sorted by x) within each half.
This de-interleaves the columns so per-effect / per-voice tables come out intact.
"""
import itertools
import fitz  # PyMuPDF

GUTTER_X = 257.0  # page is ~515pt wide; the two-column gutter sits near here


def open_pdf(path):
    return fitz.open(str(path))


def half_rows(page, side):
    """Return [(y, [words])] for one half of the page.

    side: "left" or "right". Words are the PyMuPDF word tuples
    (x0, y0, x1, y1, text, block, line, word). Rows are grouped by rounded y
    and each row's words are sorted left-to-right.
    """
    if side == "left":
        lo, hi = 0.0, GUTTER_X
    else:
        lo, hi = GUTTER_X, page.rect.width
    words = [w for w in page.get_text("words") if lo <= w[0] < hi]
    words.sort(key=lambda w: (round(w[1]), w[0]))
    rows = []
    for y, grp in itertools.groupby(words, key=lambda w: round(w[1])):
        rows.append((y, list(grp)))
    return rows


def row_text(words):
    return " ".join(w[4] for w in words)
