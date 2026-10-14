#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""build_effects.py — extract XGEdit's authoritative effect definitions.

Source of truth: the original XGEdit95 binary (xgedit95.exe), which embeds, for
every effect type, an array of up-to-16 parameter "slots" in display order with
NULL entries for the knob positions XGEdit leaves blank.  This is exactly the
layout XGEdit draws in its Effect Parameters module, so re-using it gives a
pixel-faithful editor without guessing labels from the PDF.

Binary layout (reverse-engineered):
  * A flat array of 21-byte effect-type records, grouped by effect unit:
        +0x00  char*  name
        +0x04  u8     type MSB (bank select MSB for the effect program)
        +0x08  u8     type LSB
        +0x0c  void*  pointer to that effect's parameter-label array
    Units appear back-to-back: reverb, chorus, variation, insertion, then the
    PLG-harmony list and the unit-selector list.  Boundaries are detected from
    the "NO EFFECT" / "THRU" markers.
  * Each parameter-label array is an 8-byte header followed by sixteen 18-byte
    param structs (slot 1..16).  Struct +0x00 = char* label (NULL => unused
    knob); +0x04 = u8 display-type id (kept for later value formatting).

Output: codec/MU100Effects.h  (JUCE-free; copied into the plugin like the other
codec headers).  Run from the mu100editor root:  python tools/build_effects.py
"""
import struct, os, sys
from spdx_header import CPP_LINES

EXE = r"C:\Program Files (x86)\XGedit95\xgedit95.exe"
OUT = os.path.join(os.path.dirname(__file__), "..", "codec", "MU100Effects.h")

REC_STRIDE   = 21
PARAM_STRIDE = 18
PARAM_HDR    = 8
N_SLOTS      = 16


class PE:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3c)[0]
        self.imgbase = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = struct.unpack_from("<H", d, pe + 20)[0]
        sh = pe + 24 + opt
        self.secs = []
        for i in range(nsec):
            o = sh + i * 40
            va = struct.unpack_from("<I", d, o + 12)[0]
            vs = struct.unpack_from("<I", d, o + 8)[0]
            rp = struct.unpack_from("<I", d, o + 20)[0]
            rs = struct.unpack_from("<I", d, o + 16)[0]
            self.secs.append((va, vs, rp, rs))

    def va2off(self, v):
        r = v - self.imgbase
        for va, vs, rp, rs in self.secs:
            if va <= r < va + max(vs, rs):
                fo = rp + (r - va)
                return fo if 0 <= fo < len(self.data) else None
        return None

    def u32(self, off):
        return struct.unpack_from("<I", self.data, off)[0]

    def str_at(self, va):
        o = self.va2off(va)
        if o is None:
            return None
        e = self.data.find(b"\x00", o)
        if e < 0 or e - o > 40:
            return None
        s = self.data[o:e].decode("latin1")
        return s if all(32 <= ord(c) < 127 for c in s) else None


def find_records_base(pe):
    """Find the start of the reverb record table via the first 'HALL 1'."""
    import re
    for m in re.finditer(rb"HALL 1\x00", pe.data):
        va = None
        off = m.start()
        for va_, vs, rp, rs in pe.secs:
            if rp <= off < rp + rs:
                va = pe.imgbase + va_ + (off - rp)
                break
        if va is None:
            continue
        refs = [mm.start() for mm in re.finditer(re.escape(struct.pack("<I", va)), pe.data)]
        if refs:
            # the reverb HALL 1 record is the first; its name ptr is at +0,
            # NO EFFECT precedes it by one stride.
            return refs[0] - REC_STRIDE
    raise RuntimeError("could not locate record table")


def read_records(pe, base, limit=300):
    recs = []
    off = base
    for _ in range(limit):
        name = pe.str_at(pe.u32(off))
        if name is None:
            break
        msb = pe.u32(off + 4) & 0xff
        lsb = pe.u32(off + 8) & 0xff
        lap = pe.u32(off + 0xc)
        defp = pe.u32(off + 0x10)   # -> per-program default-value table
        recs.append({"name": name, "msb": msb, "lsb": lsb,
                     "lap": lap, "defp": defp, "off": off})
        off += REC_STRIDE
    return recs


# Per-program default values. Each record's +0x10 pointer targets a 192-byte
# block = THREE 64-byte sub-blocks (one per hardware model: MU80, MU90, MU100),
# each holding 16 little-endian u32 parameter defaults in display-slot order.
# Block 2 = MU100 (validated: its reverb BAL default 50 matches the MU100 factory
# dump, where MU80/90 hold 52). Returns 16 values (low 16 bits, so 14-bit
# variation params survive).
DEF_BLOCK   = 2
DEF_STRIDE  = 0x40

def read_defaults(pe, defp):
    base = pe.va2off(defp)
    if base is None:
        return [0] * N_SLOTS
    base += DEF_BLOCK * DEF_STRIDE
    return [struct.unpack_from("<I", pe.data, base + 4 * k)[0] & 0xffff
            for k in range(N_SLOTS)]


def read_layout(pe, lap):
    base = pe.va2off(lap)
    slots = []
    for k in range(N_SLOTS):
        so = base + PARAM_HDR + PARAM_STRIDE * k
        label = pe.str_at(pe.u32(so))
        disp = pe.data[so + 4]
        slots.append({"label": label, "disp": disp})
    return slots


def segment_units(recs):
    """Split the flat record list into the 4 effect units.

    Boundaries: reverb starts at 0; each following unit begins at the next
    'NO EFFECT'(msb 0) marker; insertion begins at the first 'THRU'(msb 0x40)
    and ends at the trailing 'THRU'(msb 0) / 'VOCODER'.
    """
    no_eff = [i for i, r in enumerate(recs) if r["name"] == "NO EFFECT" and r["msb"] == 0]
    ins_start = next(i for i, r in enumerate(recs) if r["name"] == "THRU" and r["msb"] == 0x40)
    # end of insertion: first record at/after ins_start that is a non-effect
    ins_end = next((i for i in range(ins_start + 1, len(recs))
                    if recs[i]["name"] in ("THRU", "VOCODER") and recs[i]["msb"] == 0), len(recs))
    rev = (no_eff[0], no_eff[1])
    cho = (no_eff[1], no_eff[2])
    var = (no_eff[2], ins_start)
    ins = (ins_start, ins_end)
    return {"reverb": rev, "chorus": cho, "variation": var, "insertion": ins}


def dedup(recs, lo, hi):
    """Keep first record per (msb,lsb) — the unit's clean dropdown list."""
    seen, out = set(), []
    for r in recs[lo:hi]:
        key = (r["msb"], r["lsb"])
        if key in seen:
            continue
        seen.add(key)
        out.append(r)
    return out


def cident(s):
    return s


# --- Device-verified slot corrections ---------------------------------------
# {effect name: [(from_slot, to_slot), ...]}, 0-based (slot 12 = "Parameter 13").
#
# XGEdit's own table is the layout reference (it is what the editor draws), but it
# is not the reference for WHICH XG parameter number a knob writes to. PHASER2 is
# the one place found so far where the two disagree, and the device settled it:
#
#   MU100_DIAG_P2CENSUS wrote 0 / 20 / 100 / 127 to all sixteen Variation
#   parameters with PHASER2 selected and dumped the block after each.
#     param 12 -> clamps 4..124   (nothing in our layout claims this slot)
#     param 13 -> holds 0 through every write, from a default of 4
#   4..124 is exactly the printed range of "LFO Phase Difference", and every other
#   parameter's firmware range matched its PDF row exactly (p11 Stage 3..6, p4 FB
#   1..127, p6 EQ LF 4..40, ...). PHASER 2 documents eleven parameters and the
#   firmware implements eleven; the only unaccounted-for one has the only
#   unaccounted-for range. The MU100 data list prints the row one line low, and
#   XGEdit carries the same offset.
#
# Recorded here rather than in the generated header because the header is
# regenerated from the exe and a hand-edit would die silently.
SLOT_MOVES = {
    "PHASER2": [(12, 11)],
}


def apply_slot_moves(units, layouts):
    for unit_recs in units.values():
        for r in unit_recs:
            moves = SLOT_MOVES.get(r["name"])
            if not moves:
                continue
            # A layout pool entry is shared by every effect pointing at the same
            # label array. Moving a slot in a SHARED layout would silently rewrite
            # the other programs too, so refuse rather than guess.
            sharers = [o["name"] for u in units.values() for o in u
                       if o["layout"] == r["layout"]]
            if len(sharers) != 1:
                sys.exit(f"SLOT_MOVES: layout {r['layout']} is shared by {sharers}; "
                         f"cannot move a slot for {r['name']} alone")
            slots = layouts[r["layout"]]
            for src, dst in moves:
                if slots[dst]["label"]:
                    sys.exit(f"SLOT_MOVES: {r['name']} slot {dst} is not empty "
                             f"({slots[dst]['label']}) — the table has changed")
                slots[dst] = slots[src]
                slots[src] = {"label": None, "disp": 0}
                print(f"SLOT_MOVE {r['name']}: param {src + 1} -> param {dst + 1} "
                      f"({slots[dst]['label']})")


def emit(units, layouts):
    L = []
    L.extend(CPP_LINES)
    L.append("// MU100Effects.h — generated by tools/build_effects.py from XGEdit95.exe.")
    L.append("// Authoritative effect-type lists + per-effect 16-slot parameter labels")
    L.append("// (display order; null label = blank knob, exactly as XGEdit draws it).")
    L.append("// DO NOT EDIT BY HAND. Regenerate, then rebuild the plugin.")
    L.append("#pragma once")
    L.append("#include <cstdint>")
    L.append("")
    L.append("namespace mu100fx {")
    L.append("")
    L.append("enum EffectUnit { FX_REVERB = 0, FX_CHORUS, FX_VARIATION, FX_INSERTION1, FX_INSERTION2, FX_UNIT_COUNT };")
    L.append("")
    L.append("struct ParamSlot { const char* label; uint8_t disp; }; // label==nullptr => unused knob")
    L.append("struct EffectLayout { ParamSlot slots[16]; };")
    L.append("// deflt[16] = the program's default value per display slot (MU100), so picking")
    L.append("// an effect shows/loads its factory parameters without a device read-back.")
    L.append("struct EffectType { const char* name; uint8_t msb, lsb; uint16_t layout; uint16_t deflt[16]; };")
    L.append("")

    # layouts table
    L.append(f"static const EffectLayout kLayouts[] = {{")
    for li, slots in enumerate(layouts):
        parts = []
        for s in slots:
            if s["label"]:
                parts.append('{"%s",%d}' % (s["label"].replace('"', '\\"'), s["disp"]))
            else:
                parts.append("{nullptr,0}")
        L.append(f"  {{{{ {', '.join(parts)} }}}}, // %d" % li)
    L.append("};")
    L.append("")

    for unit, recs in units.items():
        arr = f"k{unit.capitalize()}Types"
        L.append(f"static const EffectType {arr}[] = {{")
        for r in recs:
            dl = ",".join(str(x) for x in r["defaults"])
            L.append('  {"%s", 0x%02x, 0x%02x, %d, {%s}},'
                     % (r["name"], r["msb"], r["lsb"], r["layout"], dl))
        L.append("};")
        L.append(f"static const int {arr}Count = {len(recs)};")
        L.append("")

    # accessor
    L.append("inline const EffectType* unitTypes(int unit, int& count) {")
    L.append("  switch (unit) {")
    L.append("    case FX_REVERB:     count = kReverbTypesCount;    return kReverbTypes;")
    L.append("    case FX_CHORUS:     count = kChorusTypesCount;    return kChorusTypes;")
    L.append("    case FX_VARIATION:  count = kVariationTypesCount; return kVariationTypes;")
    L.append("    case FX_INSERTION1:")
    L.append("    case FX_INSERTION2: count = kInsertionTypesCount; return kInsertionTypes;")
    L.append("  }")
    L.append("  count = 0; return nullptr;")
    L.append("}")
    L.append("")
    L.append("} // namespace mu100fx")
    L.append("")
    return "\n".join(L)


def main():
    pe = PE(EXE)
    base = find_records_base(pe)
    recs = read_records(pe, base)
    seg = segment_units(recs)

    # build a deduped layout pool keyed by lap
    lap_to_idx = {}
    layouts = []

    def layout_index(lap):
        if lap not in lap_to_idx:
            lap_to_idx[lap] = len(layouts)
            layouts.append(read_layout(pe, lap))
        return lap_to_idx[lap]

    units = {}
    for unit, (lo, hi) in seg.items():
        clean = dedup(recs, lo, hi)
        for r in clean:
            r["layout"] = layout_index(r["lap"])
            r["defaults"] = read_defaults(pe, r["defp"])
        units[unit] = clean

    apply_slot_moves(units, layouts)

    hdr = emit(units, layouts)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(hdr)

    # report
    for unit, recs2 in units.items():
        print(f"{unit:10} {len(recs2):3} types")
    print(f"layouts: {len(layouts)}")
    print(f"wrote {os.path.normpath(OUT)}")


if __name__ == "__main__":
    main()
