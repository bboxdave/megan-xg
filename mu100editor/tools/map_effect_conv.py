#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""
Stage 2 of the effect-conversion pipeline. Reads:
  - data/effect_conv_raw.json   (PDF tables, from build_effect_conv.py)
  - codec/MU100Effects.h        (layouts + which units use each layout)
  - data/tables.json            (the 11 raw->display conversion tables)
and produces data/effect_conv.json: per layout id, a 16-slot conversion list
{rawMin, rawMax, kind, table, dispMin, dispMax, unit, decimals, signed, enums}.

KEY FACT (validated): XGEdit's slot order == the PDF's param-number order, so PDF
param N maps to layout slot N-1 with no reordering, even for the scrambled 3-band
EQ. Matching a layout to its PDF table is therefore done by occupancy signature
(which of the 16 slots are filled) plus a label/param-name token check, NOT by
parsing the noisy effect-name headings.

Block-dependent ranges (a row that lists both a "(variation block)" and an
"(insertion1,2 block)" value): the value used per layout is chosen from whether
that layout is referenced by an Insertion unit (insertion value) or not
(variation/reverb/chorus value).

Run:  python tools/map_effect_conv.py
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RAW = ROOT / "data" / "effect_conv_raw.json"
EFFECTS_H = ROOT / "codec" / "MU100Effects.h"
TABLES = ROOT / "data" / "tables.json"
OUT = ROOT / "data" / "effect_conv.json"

DASH = "�"   # how the PDF en-dash extracts; also accept ~ and unicode dashes
KNOWN_UNITS = ["ms", "dB", "Hz", "kHz", "deg", "%", "s", "m"]

# Authoritative layout (MU100Effects.h kLayouts id) -> PDF table index
# (data/effect_conv_raw.json order). Derived from MU100Effects.h's effect-name
# -> layout map cross-referenced with each PDF table's heading/contents (the
# noisy headings make auto name-matching fragile; param occupancy collides
# across effects, so this mapping is authored once and validated by occupancy
# jaccard below). Block (variation vs insertion value line) is chosen per layout
# from whether the layout is referenced only by Insertion units.
# {layout id: [(from_slot, to_slot), ...]}, 0-based (slot 12 = "Parameter 13").
#
# The PDF's row number IS the XG parameter number everywhere except here. Layout 21
# is PHASER 2, whose "LFO Phase Difference" the MU100 data list prints in row 13
# with row 12 left blank; the firmware has it at Parameter 12 and holds 0 at 13.
# Established by MU100_DIAG_P2CENSUS writing 0/20/100/127 to all sixteen Variation
# parameters and dumping after each: param 12 clamps 4..124 — exactly the printed
# range of that row — while param 13 never leaves 0. See the matching SLOT_MOVES in
# build_effects.py, which moves the LABEL for the same reason; both must agree or
# the mapper pairs a label with no row and drops the conversion.
TABLE_ROW_MOVES = {
    21: [(12, 11)],
}

# {(layout id, slot): (rawMin, rawMax)} measured from the device, for slots where a
# factory default falls OUTSIDE the range the firmware will actually accept and the
# defaults-widening safety net below would therefore stretch the knob past it. The
# range must still match the PDF exactly (asserted) — this only says "believe the
# printed range here, not the default".
#
# (21, 11) = PHASER 2 P.DIFF. MU100_DIAG_P2CENSUS: writing 0 reads back 4 and
# writing 127 reads back 124, yet selecting the program loads 1. The firmware does
# not clamp its own default table.
DEVICE_RANGES = {
    (21, 11): (4, 124),
}

# {(layout id, slot): reason} — slots the firmware ACCEPTS but never acts on. Forced
# to kind "none" so the panel draws a bare disc (see slotActive), because a knob that
# stores and saves a value the device will never use is worse than no knob.
#
# Deliberately applied on top of TABLE_ROW_MOVES/DEVICE_RANGES rather than by deleting
# them: the row and its range stay recorded here, so re-enabling is a one-line change
# if the parameter ever turns out to matter (e.g. for an MU128 target, where the user
# confirmed on hardware 2026-08-01 that it IS audible).
INERT_SLOTS = {
    (21, 11): "PHASER 2 P.DIFF: MU100 firmware validates and stores it (clamps "
              "4..124) but never programs the MEG. MU100_DIAG_P2SLOT=11 with "
              "SWP30_PARMDIAG shows ZERO effect-DSP writes, against 12 for STAGE "
              "and 16 for PHASE, both audible. Inert on MU100.",
}

LAYOUT_TO_TABLE = {
    1: 0, 2: 1, 3: 9, 4: 10, 5: 11, 6: 12, 7: 18, 8: 2, 9: 3, 10: 4,
    11: 5, 12: 6, 13: 7, 14: 8, 15: 12, 16: 13, 17: 14, 18: 15, 19: 16,
    20: 17, 21: 19, 22: 20, 23: 22, 24: 21, 25: 23, 26: 24, 27: 25, 28: 26,
    29: 27, 30: 28, 31: 29, 32: 31, 33: 32, 34: 33, 35: 34, 36: 35, 37: 36,
    38: 37, 39: 38, 40: 39, 41: 0, 42: 2, 43: 3, 44: 4, 45: 5, 46: 9,
    47: 10, 48: 11, 49: 14, 50: 16, 51: 17, 52: 24,
}


# ---- parse MU100Effects.h -----------------------------------------------------

def parse_effects_h():
    txt = EFFECTS_H.read_text(encoding="utf-8")

    # layouts: each "{{ {"X",d}, ... }}, // N"
    layouts = {}
    for m in re.finditer(r"\{\{(.*?)\}\},\s*//\s*(\d+)", txt):
        body, lid = m.group(1), int(m.group(2))
        slots = []
        for sm in re.finditer(r"\{\s*(nullptr|\"([^\"]*)\")\s*,\s*(\d+)\s*\}", body):
            label = None if sm.group(1) == "nullptr" else sm.group(2)
            slots.append(label)
        layouts[lid] = slots[:16]

    # type arrays -> which units reference which layout id
    # block name -> list of (unit-name, layout)
    unit_blocks = {
        "FX_REVERB": "kReverbTypes",
        "FX_CHORUS": "kChorusTypes",
        "FX_VARIATION": "kVariationTypes",
        "FX_INSERTION": "kInsertionTypes",
    }
    layout_units = {}   # layout id -> set of unit names
    layout_defaults = {}  # layout id -> {slot -> set(default raw values)}
    for unit, arr in unit_blocks.items():
        block = re.search(arr + r"\[\]\s*=\s*\{(.*?)\n\};", txt, re.S)
        if not block:
            continue
        for tm in re.finditer(
                r"\{\"[^\"]*\",\s*0x[0-9a-fA-F]+,\s*0x[0-9a-fA-F]+,\s*(\d+),"
                r"\s*\{([^}]*)\}", block.group(1)):
            lid = int(tm.group(1))
            layout_units.setdefault(lid, set()).add(unit)
            vals = [int(x) for x in tm.group(2).split(",")]
            d = layout_defaults.setdefault(lid, {})
            for slot, v in enumerate(vals[:16]):
                d.setdefault(slot, set()).add(v)
    return layouts, layout_units, layout_defaults


# ---- value/display parsing ----------------------------------------------------

def parse_range(val):
    """'0-69' -> (0,69); '1-14860' -> (1,14860); single int -> (n,n); else None."""
    if not val:
        return None
    m = re.fullmatch(r"([+-]?\d+)-([+-]?\d+)", val)
    if m:
        return int(m.group(1)), int(m.group(2))
    if re.fullmatch(r"\d+", val):
        return int(val), int(val)
    return None


def norm_dash(s):
    return s.replace(DASH, "~").replace("–", "~").replace("—", "~")


# A dry/wet or balance display: "D63>W ~ D=W ~ D<W63". The leading digits are what
# separate it from an enum item that merely contains ">" (AUTO PAN's "L->R").
BALANCE_RE = re.compile(r"\s*([A-Za-z])\s*\d+\s*>\s*([A-Za-z])")

# Enum items the PDF spells out but XGEdit abbreviates, because a knob's value
# field is only a few characters wide. Keyed by the PDF's exact item text; any
# item not listed passes through unchanged.
ENUM_ABBREV = {
    "L<->R": "L<>R",
    "L->R":  "L>R",
    "L<-R":  "L<R",
    "Lturn": "LTRN",
    "Rturn": "RTRN",
}


def parse_display(disp):
    """Classify a Display string -> dict describing the conversion (sans table).
    Kinds: 'linear' (dispMin/dispMax/unit/decimals/signed), 'enum' (enums list),
    'special' (dry/wet & balance), or None."""
    if not disp:
        return None
    d = norm_dash(disp).strip()
    # strip a trailing "(... block)" annotation
    d = re.sub(r"\(.*?block\)", "", d).strip()
    if not d:
        return None

    # balance / dry-wet special: "D63>W = D=W = D<W63" / "E63>R = E=R = E<R63"
    # / "L63>H = L=H = L<H63". The two boundary letters are the regex's groups, so
    # the formatter can render "D32" / "E14" (offset from centre 64) like XGEdit.
    #
    # Match the FULL "<letter><digits>><letter>" head, not a bare ">": AUTO PAN's
    # direction list ("L<->R,L->R,...") also contains ">" and used to land here,
    # scanning out the first two letters as an L/R balance and displaying "L64".
    m = BALANCE_RE.match(d)
    if m:
        return {"kind": "special", "special": (m.group(1) + m.group(2)).upper()}

    # pan: "L63 ~ R63" (centre C) -> render L63..C..R63 like a part pan
    if re.search(r"\bL\s*\d+", d) and re.search(r"\bR\s*\d+", d):
        return {"kind": "pan"}

    # numeric linear range "A ~ B[unit]" (units may sit on either endpoint)
    nums = re.findall(r"[+-]?\d+(?:\.\d+)?", d)
    if "~" in d and len(nums) >= 2:
        a, b = nums[0], nums[-1]
        # unit = letters after the last number (or any known unit present)
        unit = ""
        for u in ("ms", "dB", "deg", "s", "m", "%"):
            if re.search(re.escape(b) + re.escape(u), d) or d.rstrip().endswith(u):
                unit = u
                break
        # Hz/kHz ranges are always table-driven (table#3/#1) -> not linear here
        if "Hz" in d:
            unit = ""    # let the table handle it; if no table, fall through raw
        decimals = max(len((x.split(".")[1]) if "." in x else "") for x in (a, b))
        dmin, dmax = float(a), float(b)
        return {"kind": "linear", "dispMin": dmin, "dispMax": dmax,
                "unit": unit, "decimals": decimals, "signed": dmin < 0}

    # enum lists: "mono/stereo", "TypeA,TypeB", "S-H, L-H, Rdm, Rvs, Plt, Spr",
    # "L,R,L&R", "L<->R,L->R,L<-R,Lturn,Rturn,L/R".
    #
    # A comma is the list separator WHEREVER one is present, and only then does a
    # slash belong to an item ("L/R" is one AUTO PAN direction, not two). Splitting
    # on both at once turned that list into 7 items and shifted every direction
    # above it. No PDF display string uses a comma AND a separating slash.
    if ("/" in d or "," in d) and not re.search(r"\d", d.replace(",", "")):
        parts = d.split(",") if "," in d else d.split("/")
        enums = [ENUM_ABBREV.get(p.strip(), p.strip()) for p in parts if p.strip()]
        if len(enums) >= 2:
            return {"kind": "enum", "enums": enums}
    return None


# ---- build per-table structured slots -----------------------------------------

def line_blocks(ln):
    """The set of block names a value line is annotated with, e.g.
    '4,5,6 (chorus, insertion1,2 block)' -> {'chorus','insertion'}. Empty when
    the line carries no '(... block)' annotation at all."""
    d = (ln.get("display") or "").lower()
    if "block" not in d:
        return set()
    return {w for w in ("variation", "chorus", "insertion", "reverb") if w in d}


def table_slots(tbl, prefer_insertion):
    """Return {slot -> {rawMin,rawMax, table, display, alt*}} for one PDF table,
    picking the block-appropriate value line.

    `alt*` carries the OTHER block's value line when the row states two (see
    the module docstring). Most such rows are resolved by the layout split
    instead — DELAY/ECHO/reverb have separate variation and insertion layout
    ids — so the alt is only ever emitted by main() for a layout that really is
    shared across both block classes."""
    slots = {}
    for r in tbl["rows"]:
        slot = r["num"] - 1
        if not (0 <= slot < 16):
            continue
        lines = [ln for ln in r["lines"] if ln.get("value") or ln.get("table")]
        if not lines:
            continue
        # choose line: if multiple and any annotated block, pick by preference
        chosen = lines[0]
        other = None
        if len(lines) > 1:
            var_ln = ins_ln = None
            for ln in lines:
                b = line_blocks(ln)
                if "insertion" in b:
                    ins_ln = ln
                elif "variation" in b:
                    var_ln = ln
            if prefer_insertion and ins_ln:
                chosen = ins_ln
                other = var_ln
            elif var_ln:
                chosen = var_ln
                other = ins_ln
        rng = parse_range(chosen.get("value"))
        e = {
            "rawMin": rng[0] if rng else None,
            "rawMax": rng[1] if rng else None,
            "table": chosen.get("table"),
            "display": chosen.get("display") or "",
            "blocks": sorted(line_blocks(chosen)),
        }
        if other is not None:
            arng = parse_range(other.get("value"))
            if arng:
                e["altRawMin"], e["altRawMax"] = arng
                e["altDisplay"] = other.get("display") or ""
                e["altBlocks"] = sorted(line_blocks(other))
        slots[slot] = e
    return slots


def occupancy(slotmap):
    return frozenset(s for s, v in slotmap.items()
                     if v["rawMin"] is not None or v["table"])


# ---- label/name compatibility (light tiebreaker) ------------------------------

def name_tokens(s):
    return set(re.findall(r"[a-z0-9]+", (s or "").lower()))


def label_match_score(layout_slots, tbl_slots):
    score = 0
    for slot, label in enumerate(layout_slots):
        if label is None:
            continue
        tv = tbl_slots.get(slot)
        if not tv:
            continue
        lt = name_tokens(label)
        nt = name_tokens(tv["display"]) | name_tokens(tbl_slots[slot].get("name", ""))
        # cheap: shared starting letters of label vs param-name handled elsewhere;
        # here reward any token overlap
        if lt & nt:
            score += 1
    return score


def var_block_only(table):
    """{slot -> True} for params the MU100 spec marks as VARIATION-BLOCK-ONLY,
    i.e. a param whose block annotations mention 'variation' and never 'chorus'
    or 'insertion' (so it exists only when the effect runs as a Variation, not in
    the dedicated Chorus/Insertion block). Dual-block params like PHASER 'Stage'
    ('(chorus, insertion1,2 block)' + '(variation block)') are NOT flagged.

    The annotation may sit on the row's NAME rather than on a value line: AUTO
    WAH and TOUCH WAH 1 both print param 11 as "Drive (Variation block)" with a
    bare "0 - 127" value line. Scanning only the lines missed those two, so the
    editor offered a live DRIVE knob in the Insertion blocks, where the device
    ignores it and reads back 0 (confirmed by the 2026-07-31 round-trip sweep)."""
    out = {}
    for r in table["rows"]:
        slot = r["num"] - 1
        if not (0 <= slot < 16):
            continue
        blocks = set()
        texts = [ln.get("display") or "" for ln in r["lines"]]
        texts.append(r.get("name") or "")
        for d in texts:
            d = d.lower()
            if "block" not in d:
                continue
            for w in ("variation", "chorus", "insertion", "reverb"):
                if w in d:
                    blocks.add(w)
        out[slot] = bool(blocks) and blocks == {"variation"}
    return out


UNIT_BLOCK = {
    "FX_REVERB": "reverb", "FX_CHORUS": "chorus",
    "FX_VARIATION": "variation", "FX_INSERTION": "insertion",
}


def _alt_range_for(lid, slot, tv, units):
    """(altMin, altMax) when this layout genuinely needs a second, block-
    dependent range, else None.

    Emitted ONLY when the primary line is the Variation one and the layout is
    also used by non-Variation units -- because that is the single shape the
    runtime can express: XgEffectFormat already knows `isVariationUnit` and
    nothing else about the block. Any other split (e.g. a row that is
    reverb+variation vs insertion on a layout used by all three) is refused
    loudly rather than silently mapped onto the wrong test."""
    if tv.get("altRawMin") is None:
        return None
    have = {UNIT_BLOCK[u] for u in units if u in UNIT_BLOCK}
    primary, alt = set(tv.get("blocks") or []), set(tv.get("altBlocks") or [])
    if not (have & alt):
        return None                      # layout never runs in the alt's blocks
    if primary != {"variation"}:
        print(f"  WARNING: layout {lid} slot {slot} has a block-dependent range "
              f"the runtime cannot express (primary blocks {sorted(primary)}, "
              f"alt {sorted(alt)}); left at the primary range", file=sys.stderr)
        return None
    return tv["altRawMin"], tv["altRawMax"]


def main():
    raw = json.load(open(RAW, encoding="utf-8"))
    layouts, layout_units, layout_defaults = parse_effects_h()
    tables_json = json.load(open(TABLES, encoding="utf-8"))

    result = {}
    report = []
    for lid, slots in sorted(layouts.items()):
        layout_occ = frozenset(i for i, l in enumerate(slots) if l is not None)
        if not layout_occ:
            continue   # NO EFFECT / THRU (layout 0)
        if lid not in LAYOUT_TO_TABLE:
            print(f"  WARNING: layout {lid} has no PDF table mapping",
                  file=sys.stderr)
            continue
        units = layout_units.get(lid, set())
        prefer_ins = units == {"FX_INSERTION"}   # insertion-only layout
        ti = LAYOUT_TO_TABLE[lid]
        sm = table_slots(raw[ti], prefer_ins)
        vonly = var_block_only(raw[ti])
        for s in sm:
            sm[s]["name"] = {r["num"] - 1: r["name"]
                             for r in raw[ti]["rows"]}.get(s, "")

        # Device-verified row corrections (see TABLE_ROW_MOVES). Applied AFTER the
        # name lookup above, which is keyed by the row's printed number, and before
        # the occupancy comparison below, so the jaccard score is measured against
        # the corrected layout rather than flagging the move as a mismatch.
        for src, dst in TABLE_ROW_MOVES.get(lid, []):
            if src not in sm:
                sys.exit(f"TABLE_ROW_MOVES: layout {lid} has no PDF row {src + 1}")
            if dst in sm:
                sys.exit(f"TABLE_ROW_MOVES: layout {lid} row {dst + 1} is occupied "
                         f"({sm[dst].get('name')}) — the table has changed")
            sm[dst] = sm.pop(src)
            if vonly.pop(src, None):
                vonly[dst] = True
            print(f"  ROW_MOVE layout {lid}: param {src + 1} -> param {dst + 1} "
                  f"({sm[dst].get('name')})")

        occ = occupancy(sm)
        jacc = (len(layout_occ & occ) / len(layout_occ | occ)
                if (layout_occ | occ) else 0)

        conv = []
        for slot in range(16):
            label = slots[slot]
            tv = sm.get(slot)
            c = {"label": label}
            if label is None or tv is None or tv["rawMin"] is None:
                c["kind"] = "none"
                conv.append(c)
                continue
            c["rawMin"], c["rawMax"] = tv["rawMin"], tv["rawMax"]
            if tv["table"]:
                c["kind"] = "table"
                c["table"] = int(tv["table"].split("#")[1])
            else:
                pd = parse_display(tv["display"])
                if pd and pd["kind"] in ("linear", "enum"):
                    c.update(pd)
                elif pd and pd["kind"] in ("special", "pan"):
                    c["kind"] = pd["kind"]
                    if pd.get("special"):
                        c["special"] = pd["special"]
                else:
                    c["kind"] = "plain"
                # Safety net: factory defaults are authoritative device values, so
                # the knob range must contain them. The PDF occasionally
                # under-specifies a block range (e.g. insertion reverb DENSE 0-2
                # vs the device's default 4). Widen non-table ranges to include
                # all defaults, preserving the linear law (slope/offset).
                defs = layout_defaults.get(lid, {}).get(slot, set())
                # ...but a MEASURED device range outranks a default. PHASER2's
                # P.DIFF is the case: the firmware clamps writes to 4..124 and yet
                # loads 1 into it on a type change, so the safety net would widen
                # the knob to 1..124 and extrapolate its display to -189deg for a
                # value the device refuses to accept. Trust the census.
                if (lid, slot) in DEVICE_RANGES:
                    want = DEVICE_RANGES[(lid, slot)]
                    if (c["rawMin"], c["rawMax"]) != want:
                        sys.exit(f"DEVICE_RANGES: layout {lid} slot {slot} is "
                                 f"({c['rawMin']},{c['rawMax']}) in the PDF, "
                                 f"measured {want} — reconcile before overriding")
                    defs = set()
                if defs:
                    lo, hi = min(defs | {c["rawMin"]}), max(defs | {c["rawMax"]})
                    if c["kind"] == "linear" and c["rawMax"] != c["rawMin"]:
                        slope = (c["dispMax"] - c["dispMin"]) / (c["rawMax"] - c["rawMin"])
                        c["dispMin"] = round(c["dispMin"] + slope * (lo - c["rawMin"]), 3)
                        c["dispMax"] = round(c["dispMin"] + slope * (hi - lo), 3)
                    c["rawMin"], c["rawMax"] = lo, hi
            if vonly.get(slot):
                c["varBlockOnly"] = True
            # Block-dependent range on a layout that is SHARED across block
            # classes. Nearly every such row is already resolved by the layout
            # split (DELAY LCR is layout 8 for Variation and 42 for Insertion,
            # each carrying its own range), so this fires only where one layout
            # id really does serve both -- in the whole effect set that is
            # PHASER1 (layout 7: Chorus + Insertion + Variation), whose Stage is
            # 4..12 as a Variation but 4..6 in the Chorus/Insertion blocks. The
            # device confirms it: writing 9 reads back 6 on units 1/3/4 and is
            # accepted on unit 2.
            alt = _alt_range_for(lid, slot, tv, units)
            if alt:
                c["altRawMin"], c["altRawMax"] = alt
                if c["kind"] == "linear" and c["rawMax"] != c["rawMin"]:
                    slope = (c["dispMax"] - c["dispMin"]) / (c["rawMax"] - c["rawMin"])
                    c["altDispMin"] = round(
                        c["dispMin"] + slope * (alt[0] - c["rawMin"]), 3)
                    c["altDispMax"] = round(
                        c["dispMin"] + slope * (alt[1] - c["rawMin"]), 3)
            conv.append(c)
        result[lid] = conv
        report.append((lid, ti, raw[ti]["page"], raw[ti]["side"], jacc,
                       sorted(units)))

    # --- Manual conversion overrides ----------------------------------------
    # PHASER1 (layout 7) slot 12 "P.DIFF" USED TO BE mirrored from PHASER2
    # (layout 21 slot 12) here, on the grounds that XGEdit shows it. Removed
    # 2026-07-31: the PDF leaves PHASER 1's param 13 blank, and the device
    # agrees -- the round-trip sweep wrote 44 to it in every block PHASER1 runs
    # in and read back 0 every time. XGEdit really does draw a live knob there
    # (verified in its Chorus block: P.DIFF green, value 0), so this is one of
    # the places XGEdit is wrong and we had copied it. Slot 12 now falls through
    # to "none" and renders as a bare disc, like every other spec-blank slot.
    # PHASER2's own P.DIFF is left alone -- it IS in the spec. (It also reads
    # back 0 on this firmware, which is a separate question and not this fix.)

    # TOUCH WAH 2 (layout 30) slots 10..14 -- DRIVE / DEQLG / DEQHG / LPF /
    # LEVEL, the overdrive section. Layout 30 is shared by "TOUCH WAH2" and
    # "WAH+OVERDRV", and the PDF prints both in ONE table (raw[28]) whose
    # heading marks TOUCH WAH+ODRV as "(Variation block)". So those five
    # columns exist only for the Variation-only +ODRV program; plain TOUCH WAH
    # 2 in an Insertion block has no overdrive stage. The row text alone does
    # not say so -- var_block_only cannot see it -- so it is asserted here, and
    # it is a DEVICE fact: the sweep wrote all five in Insertion 1 and 2 and
    # read back 0 from each, while slot 15 (REL), which is TOUCH WAH 2's own
    # row, round-tripped exactly.
    # Slots the firmware accepts but never acts on (see INERT_SLOTS).
    for (lid, slot), why in INERT_SLOTS.items():
        if lid not in result:
            sys.exit(f"INERT_SLOTS: layout {lid} not in the result set")
        if result[lid][slot].get("kind") == "none":
            sys.exit(f"INERT_SLOTS: layout {lid} slot {slot} is already none — "
                     f"the override is stale")
        label = result[lid][slot].get("label")
        result[lid][slot] = {"label": label, "kind": "none"}
        print(f"  INERT layout {lid} slot {slot} ({label}): {why.split(':')[0]}")

    if 30 in result:
        for slot in range(10, 15):
            if result[30][slot].get("kind") != "none":
                result[30][slot]["varBlockOnly"] = True

    # LO-FI (layout 37) slots 0 and 3. Both are table-valued controls that the PDF
    # writes as a bare range with no "table#N" reference, so the range parser fell
    # through to linear/plain. They are the only two such slots in the whole effect
    # set (audited 2026-07-28 against every layout's PDF row).
    #
    #   slot 0 FREQ "44.1kHz - 345Hz" raw 0-127 -- endpoints in DIFFERENT units, and
    #     the quantity DESCENDS, so the linear ramp 44.1 -> 345 was wrong in
    #     direction, curve and unit. It is 44100/(raw+1) Hz; see DERIVED_TABLES
    #     (table#12) in gen_effect_conv.py for the derivation and its verification.
    #   slot 3 LPF "63Hz - thru" raw 10-60 -- this is table#3 (EQ Frequency), whose
    #     entries 10..60 are exactly "63".."THRU(20k)". Corroborated by XGEdit's own
    #     LO-FI screenshot, which shows "10.0K" for the factory default raw 54, and
    #     table#3[54] == "10.0k".
    #
    # NOT changed: slot 6 BIT ASSIGN stays 0-6. XGEdit displays it as 20..40, but
    # that is XGEdit applying table#3 to it (table#3[0..6] == THRU(20),22,25,28,32,
    # 36,40 -- and XGEdit shows 22 for the factory default raw 1). The PDF says
    # plainly "0 - 6", and a bit-depth control is not a frequency. XGEdit's LO-FI
    # conversions are demonstrably unreliable: the same screenshot shows GAIN as
    # "-58dB" for raw 6, where the PDF's -6..+12dB over raw 0-18 gives 0dB.
    if 37 in result:
        result[37][0] = {"label": "FREQ", "rawMin": 0, "rawMax": 127,
                         "kind": "table", "table": 12}
        result[37][3] = {"label": "LPF", "rawMin": 10, "rawMax": 60,
                         "kind": "table", "table": 3}

    OUT.write_text(json.dumps({
        "note": "per-layout (MU100Effects.h kLayouts) 16-slot effect param "
                "conversions; raw value range + table#N or linear/enum mapping.",
        "tables_ref": "see data/tables.json for table#N value arrays",
        "layouts": result,
    }, indent=2, ensure_ascii=False), encoding="utf-8")

    print(f"wrote {OUT}  ({len(result)} layouts)", file=sys.stderr)
    print("layout -> table  (occupancy jaccard, units)  LOW jaccard = check",
          file=sys.stderr)
    for lid, ti, pg, side, jacc, units in report:
        head = " ".join(raw[ti]["heading"]).split("(")[0].strip()[:30]
        flag = "  <-- CHECK" if jacc < 0.85 else ""
        print(f"  L{lid:>2} -> t{ti:>2} p{pg}{side[0]} j={jacc:.2f} "
              f"{','.join(u[3:] for u in units):<22} {head}{flag}",
              file=sys.stderr)


if __name__ == "__main__":
    main()
