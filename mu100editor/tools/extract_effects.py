#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""
Extract MU100 effect data into data/effects.json:

  * "types": the Effect Type Lists (No / MSB / LSB / name) for the REVERB,
    CHORUS, VARIATION and INSERTION blocks -- from the layout text (clean).
  * "param_lists": the per-effect parameter tables (parameter name, display
    range, raw value range, conversion-table ref) -- from PDF coordinates,
    de-interleaving the two-column layout via tools/pdf_lib.

Run:  python tools/extract_effects.py
"""
import json
import re
import sys
from pathlib import Path

import pdf_lib

ROOT = Path(__file__).resolve().parent.parent
TXT = ROOT / "reference" / "sound_list.txt"
PDF = ROOT / "reference" / "MU100_sound_list_midi_data.pdf"
OUT = ROOT / "data" / "effects.json"

# ---- Effect Type Lists (from layout text) ---------------------------------
# Each block: "<No> <MSB>H <LSB>H <NAME>  [remarks]"
TYPE_ROW = re.compile(r"^\s*(\d+)\s+([0-9A-F]{2})H\s+([0-9A-F]{2})H\s+(\S.*?)\s*$")
TYPE_BLOCKS = [  # (json key, start marker line content, end marker)
    ("reverb", "      REVERB", "CHORUS"),
    ("chorus", "CHORUS                             Remarks", "VARIATION"),
    ("variation", "   VARIATION", "INSERTION1,2"),
    ("insertion", "INSERTION1,2", "REVERB TYPE"),
]


def extract_types():
    lines = TXT.read_text(encoding="utf-8", errors="replace").splitlines()
    # locate each block by first line that startswith the marker
    def find(marker, after=0):
        for i in range(after, len(lines)):
            if lines[i].startswith(marker) or lines[i].strip() == marker.strip():
                return i
        return None

    out = {}
    for key, start_m, end_m in TYPE_BLOCKS:
        s = find(start_m)
        e = find(end_m, s + 1 if s else 0) if s is not None else None
        if s is None:
            out[key] = []
            continue
        rows, seen = [], set()
        for ln in lines[s:e or len(lines)]:
            m = TYPE_ROW.match(ln)
            if not m:
                continue
            name = re.split(r"\s{2,}", m.group(4))[0].strip()  # drop remarks
            msb, lsb = m.group(2), m.group(3)
            key2 = (msb, lsb, name)
            if key2 in seen or not name:
                continue
            seen.add(key2)
            rows.append({"no": int(m.group(1)), "msb": msb, "lsb": lsb, "name": name})
        out[key] = rows
    return out


# ---- Effect Parameter Lists (from PDF coordinates) ------------------------
FIELDS = ["no", "parameter", "display", "value", "see_table", "control"]
HEADER_WORDS = {"No.": "no", "Parameter": "parameter", "Display": "display",
                "Value": "value", "Table": "see_table", "Control": "control"}
VALUE_RE = re.compile(r"^\d+\s*-\s*\d+$")
TABLE_RE = re.compile(r"table#\d+")


def find_param_pages(doc):
    pages = []
    for pno in range(len(doc)):
        t = doc[pno].get_text("text")
        if "See Table" in t and "Display" in t and "Parameter" in t:
            pages.append(pno)
    return pages


# Lines to ignore as title candidates: page furniture, the stray bullet glyph,
# block-diagram labels (dry / L / R / LSF / HSF / +-...), and data-row leakage.
TITLE_NOISE = re.compile(
    r"^(MU100 EFFECT|Effect Parameter List|G|\d.*|"
    r"[LR]|dry.*|[LR]?\s*LSF.*|HSF.*|[+\-().LR ]+|"
    r".*\d+\s*-\s*\d+.*)$")


def split_title(buffer):
    """From the last buffered text lines, derive (effect_name, scope).

    Titles look like "FLANGER 1, 2, 3" then "(Chorus, Variation, Insertion 1, 2
    block)". The name is everything before the first parenthesis across the last
    couple of non-noise lines.
    """
    cand = [b for b in buffer if not TITLE_NOISE.match(b)]
    text = " ".join(cand[-2:]).strip()
    text = re.sub(r"\s+", " ", text)
    name = text.split("(")[0].strip(" ,")
    scope = ""
    if "(" in text:
        scope = "(" + text.split("(", 1)[1]
    return name or "(unlabeled)", scope


def parse_block(title, header_anchors, data_rows):
    """Assign each data word to the nearest field anchor; build param dicts."""
    params = []
    for words in data_rows:
        buckets = {f: [] for f in FIELDS}
        for w in words:
            x = w[0]
            # nearest anchor
            f = min(header_anchors, key=lambda k: abs(header_anchors[k] - x))
            buckets[f].append(w[4])
        no = " ".join(buckets["no"]).strip()
        name = " ".join(buckets["parameter"]).strip()
        display = " ".join(buckets["display"]).strip()
        value = " ".join(buckets["value"]).strip()
        table = " ".join(buckets["see_table"]).strip()
        # fix common spillover: a non-range token in value belongs to display
        if value and not VALUE_RE.match(value):
            display = (display + " " + value).strip()
            value = ""
        tm = TABLE_RE.search(table) or TABLE_RE.search(display)
        if not no.isdigit() or not name:
            continue
        params.append({
            "no": int(no),
            "parameter": name,
            "display": display.replace("�", "-").strip(),
            "value": value,
            "table": tm.group(0) if tm else None,
            "review": value == "" or display == "",
        })
    name, scope = title
    return {"effect": name, "scope": scope,
            "param_count": len(params), "params": params}


def extract_param_lists(doc):
    blocks = []
    for pno in find_param_pages(doc):
        page = doc[pno]
        for side in ("left", "right"):
            rows = pdf_lib.half_rows(page, side)
            # segment by header rows
            i = 0
            title_buf = []
            while i < len(rows):
                y, words = rows[i]
                texts = [w[4] for w in words]
                is_header = sum(1 for t in texts if t in HEADER_WORDS) >= 3
                if is_header:
                    anchors = {}
                    for w in words:
                        if w[4] in HEADER_WORDS:
                            anchors[HEADER_WORDS[w[4]]] = w[0]
                    # collect data rows until next header / title-ish gap
                    data = []
                    j = i + 1
                    while j < len(rows):
                        yj, wj = rows[j]
                        tj = [w[4] for w in wj]
                        if sum(1 for t in tj if t in HEADER_WORDS) >= 3:
                            break
                        # a new effect title (no leading number, has letters) ends block
                        first = tj[0] if tj else ""
                        if data and not first[:1].isdigit() and any(c.isalpha() for c in " ".join(tj)) and len(tj) <= 8 and "table#" not in " ".join(tj):
                            # could be next title; stop and let outer loop re-handle
                            break
                        data.append(wj)
                        j += 1
                    blk = parse_block(split_title(title_buf), anchors, data)
                    if blk["param_count"]:
                        blocks.append(blk)
                    title_buf = []
                    i = j
                    continue
                # non-header line: buffer as a candidate title fragment
                line = re.sub(r"\s+", " ", pdf_lib.row_text(words).strip())
                if line:
                    title_buf.append(line)
                i += 1
    return blocks


def main():
    doc = pdf_lib.open_pdf(PDF)
    types = extract_types()
    param_lists = extract_param_lists(doc)

    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(json.dumps({"types": types, "param_lists": param_lists},
                              indent=2), encoding="utf-8")

    print(f"wrote {OUT}", file=sys.stderr)
    print("TYPES:", {k: len(v) for k, v in types.items()}, file=sys.stderr)
    tot = sum(b["param_count"] for b in param_lists)
    rev = sum(1 for b in param_lists for p in b["params"] if p["review"])
    print(f"PARAM LISTS: {len(param_lists)} effect blocks, {tot} params, "
          f"{rev} rows need review", file=sys.stderr)


if __name__ == "__main__":
    main()
