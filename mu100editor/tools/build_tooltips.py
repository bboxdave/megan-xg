#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""build_tooltips.py — bootstrap the editor tooltip table from XGEdit95.exe.

XGEdit stores its control hover text as Windows RT_STRING resources (clean
UTF-16). This mines them and joins each to a MU100-editor control by a stable
`key`, writing data/tooltips.csv — the human-editable SOURCE OF TRUTH.

  key convention (what the plugin looks up):
    MULTI PART|<PARAM>   part channel-strip controls (by codec param name)
    DRUM SETUP|<PARAM>   drum-edit-mode rebindings
    XG SYSTEM|<PARAM>    System module knobs
    MULTI EQ|<CTRL>      Multi EQ module (PROGRAM/BAND/GAIN/FREQ/Q/SHAPE)
    CTRL|<SUFFIX>        Controller module depth knobs (source-independent)
    FX|<ABBR>            Effect Parameters module, by the slot-label abbreviation
    PLUGIN|<NAME>        plugin-only controls with no XG param (author by hand)

Run from the mu100editor root:  python tools/build_tooltips.py
Re-running OVERWRITES data/tooltips.csv from the curated data below, discarding
hand edits — so once you start editing the CSV, regenerate the header with
gen_tooltips.py instead of re-running this.
"""
import struct, os, csv, sys

EXE = r"C:\Program Files (x86)\XGedit95\xgedit95.exe"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV = os.path.join(ROOT, "data", "tooltips.csv")

# --------------------------------------------------------------- XGedit RT_STRING
def load_xg():
    data = open(EXE, "rb").read()
    e = struct.unpack_from("<I", data, 0x3c)[0]; coff = e + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    optsz = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (112 if magic == 0x20b else 96)
    res_rva = struct.unpack_from("<I", data, dd + 16)[0]
    secoff = opt + optsz
    secs = []
    for i in range(nsec):
        o = secoff + i * 40
        # section header @o+8: VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        secs.append((vaddr, vsize, rawptr, rawsize))
    def r2o(rva):
        for vaddr, vsize, rawptr, rawsize in secs:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
    rb = r2o(res_rva)
    def pdir(off):
        nn, ni = struct.unpack_from("<HH", data, off + 12)
        out = []; ee = off + 16
        for i in range(nn + ni):
            nm, of = struct.unpack_from("<II", data, ee); ee += 8
            out.append((nm, rb + (of & 0x7fffffff)))
        return out
    xg = {}
    for tid, tc in pdir(rb):
        if tid != 6:                       # RT_STRING
            continue
        for bid, bc in pdir(tc):
            block = bid & 0x7fffffff
            for lid, lc in pdir(bc):
                dr, sz, _, _ = struct.unpack_from("<IIII", data, lc)
                blk = data[r2o(dr):r2o(dr) + sz]; p = 0
                for k in range(16):
                    if p + 2 > len(blk):
                        break
                    cl = struct.unpack_from("<H", blk, p)[0]; p += 2
                    s = blk[p:p + cl * 2].decode("utf-16le", "replace"); p += cl * 2
                    if cl:
                        xg[(block - 1) * 16 + k] = s
    return xg

# ---------------------------------------------------------------- curated data
# (key, context, xg_id or None, confidence)  -- context is a human hint only
STATIC = [
    # Part channel strip (MULTI PART)
    ("MULTI PART|VOLUME",                            "Part VOL",    29,  "high"),
    ("MULTI PART|DETUNE",                            "Part DETUNE", 30,  "high"),
    ("MULTI PART|NOTE SHIFT",                        "Part KSHIFT", 31,  "high"),
    ("MULTI PART|PAN",                               "Part PAN",    32,  "high"),
    ("MULTI PART|VELOCITY SENSE DEPTH",              "Part V.SEN",  33,  "high"),
    ("MULTI PART|VELOCITY SENSE OFFSET",             "Part V.OFF",  34,  "high"),
    ("MULTI PART|VIBRATO RATE",                      "Part RATE",   35,  "high"),
    ("MULTI PART|VIBRATO DEPTH",                     "Part DEPTH",  36,  "high"),
    ("MULTI PART|VIBRATO DELAY",                     "Part DELAY",  37,  "high"),
    ("MULTI PART|HIGH PASS FILTER CUTOFF FREQUENCY", "Part HPF",    336, "high"),
    ("MULTI PART|LOW PASS FILTER RESONANCE",         "Part RES",    38,  "high"),
    ("MULTI PART|LOW PASS FILTER CUTOFF FREQUENCY",  "Part CUTOFF", 39,  "high"),
    ("MULTI PART|REVERB SEND",                       "Part REV",    40,  "high"),
    ("MULTI PART|CHORUS SEND",                       "Part CHORUS", 41,  "high"),
    ("MULTI PART|VARIATION SEND",                    "Part VAR",    42,  "high"),
    ("MULTI PART|DRY LEVEL",                         "Part DRY",    43,  "high"),
    ("MULTI PART|PORTAMENTO TIME",                   "Part PTIME",  47,  "high"),
    ("MULTI PART|MONO/POLY MODE",                    "Part M/P",    77,  "high"),
    ("MULTI PART|SAME NOTE NUMBER",                  "Part ASSIGN", 46,  "high"),
    ("MULTI PART|EQ BASS GAIN",                      "Part EQ",     331, "high"),
    ("MULTI PART|EQ BASS FREQUENCY",                 "Part EQ",     332, "high"),
    ("MULTI PART|EQ TREBLE GAIN",                    "Part EQ",     333, "high"),
    ("MULTI PART|EQ TREBLE FREQUENCY",               "Part EQ",     334, "high"),
    # Drum-edit-mode rebindings (DRUM SETUP)
    ("DRUM SETUP|LEVEL",                             "Drum VOL",    168, "high"),
    ("DRUM SETUP|PITCH FINE",                        "Drum DETUNE", 169, "high"),
    ("DRUM SETUP|PITCH COARSE",                      "Drum KSHIFT", 170, "high"),
    ("DRUM SETUP|PAN",                               "Drum PAN",    171, "high"),
    ("DRUM SETUP|Rcv NOTE ON",                       "Drum K.ON",   172, "high"),
    ("DRUM SETUP|Rcv NOTE OFF",                      "Drum K.OFF",  173, "high"),
    ("DRUM SETUP|HIGH PASS FILTER CUTOFF FREQUENCY", "Drum HPF",    346, "high"),
    ("DRUM SETUP|LOW PASS FILTER RESONANCE",         "Drum RES",    177, "high"),
    ("DRUM SETUP|LOW PASS FILTER CUTOFF FREQUENCY",  "Drum CUTOFF", 178, "high"),
    ("DRUM SETUP|REVERB SEND",                       "Drum REV",    179, "high"),
    ("DRUM SETUP|CHORUS SEND",                       "Drum CHORUS", 180, "high"),
    ("DRUM SETUP|VARIATION SEND",                    "Drum VAR",    181, "high"),
    # Controller module depth knobs (source-independent)
    ("CTRL|PITCH CONTROL",                           "Ctrl PITCH",  49,  "high"),
    ("CTRL|AMPLITUDE CONTROL",                       "Ctrl AMP",    50,  "high"),
    ("CTRL|LOW PASS FILTER CONTROL",                 "Ctrl FILT",   51,  "high"),
    ("CTRL|LFO PMOD DEPTH",                          "Ctrl PMOD",   52,  "high"),
    ("CTRL|LFO AMOD DEPTH",                          "Ctrl AMOD",   53,  "high"),
    ("CTRL|LFO FMOD DEPTH",                          "Ctrl FMOD",   54,  "high"),
    # System module (XG SYSTEM)
    ("XG SYSTEM|MASTER VOLUME",                      "Sys VOL",     601, "medium"),
    ("XG SYSTEM|MASTER ATTENUATOR",                  "Sys ATT",     1,   "high"),
    ("XG SYSTEM|MASTER TUNE",                        "Sys TUNE",    2,   "high"),
    ("XG SYSTEM|TRANSPOSE",                          "Sys KSHIFT",  3,   "high"),
    ("PLUGIN|TEMPO",                                 "Sys TEMPO",   None,"manual"),
    # Multi EQ module
    ("MULTI EQ|PROGRAM",                             "MEQ PROGRAM", 156, "high"),
    ("MULTI EQ|BAND",                                "MEQ BAND",    157, "high"),
    ("MULTI EQ|GAIN",                                "MEQ GAIN",    158, "high"),
    ("MULTI EQ|FREQ",                                "MEQ FREQ",    159, "high"),
    ("MULTI EQ|Q",                                   "MEQ Q",       160, "high"),
    ("MULTI EQ|SHAPE",                               "MEQ SHAPE",   161, "high"),
    # plugin-only (no XG param) -- author by hand later
    ("PLUGIN|HPF2",                                  "Part HPF2",   None,"manual"),
]

# Effect Parameters module: slot-label abbreviation -> XGedit id (None = author by hand).
FX = {
    "AM": (602, "high"), "AMDEP": (602, "high"), "ANGLE": (656, "high"),
    "ATT": (604, "high"), "BAL": (631, "high"), "BITS": (605, "medium"),
    "C.FREQ": (609, "medium"), "C.LEV": (607, "high"), "C.OFF": (654, "medium"),
    "CHDEL": (606, "high"), "CHLEV": (607, "high"), "D/W": (621, "high"),
    "D2LEV": (613, "high"), "DAMP": (639, "high"), "DELAY": (None, "manual"),
    "DENS": (614, "high"), "DENSE": (614, "high"), "DEPTH": (648, "medium"),
    "DEQHG": (625, "high"), "DEQLG": (627, "high"), "DIFF": (617, "high"),
    "DRIVE": (622, "high"), "EDGE": (622, "medium"), "EMPH": (None, "manual"),
    "EQ HF": (624, "high"), "EQ HG": (625, "high"), "EQ LF": (626, "high"),
    "EQ LG": (627, "high"), "EQ MF": (628, "high"), "EQ MG": (629, "high"),
    "EQ MW": (630, "high"), "FB": (634, "high"), "FBD": (633, "high"),
    "FBD1": (633, "high"), "FBD2": (633, "high"), "FILT": (635, "medium"),
    "FINE1": (636, "high"), "FINE2": (636, "high"), "FRDEP": (632, "high"),
    "FREQ": (649, "high"), "GAIN": (659, "medium"), "HEIGHT": (637, "high"),
    "HIGH": (619, "medium"), "HPF": (640, "high"), "I.DEL": (641, "high"),
    "INPUT": (643, "medium"), "L.DEL": (646, "high"), "L.FB": (647, "high"),
    "L/H": (653, "medium"), "LDEL1": (646, "high"), "LDEL2": (646, "high"),
    "LDLY": (646, "medium"), "LEN": (605, "medium"), "LEVEL": (660, "high"),
    "LIVE": (651, "high"), "LOW": (620, "medium"), "LPF": (654, "high"),
    "LRDEL": (645, "high"), "LRDEP": (644, "high"), "MIX": (657, "high"),
    "MODE": (682, "medium"), "OPL1": (660, "medium"), "OPL2": (660, "medium"),
    "P.DIFF": (650, "high"), "PAN1": (662, "high"), "PAN2": (663, "high"),
    "PAND": (664, "medium"), "PHASE": (665, "medium"), "PITCH": (666, "high"),
    "PM": (667, "medium"), "R.DEL": (None, "manual"), "R.FB": (671, "high"),
    "RATIO": (669, "high"), "RDEL1": (670, "medium"), "RDEL2": (670, "medium"),
    "RDEL3": (670, "medium"), "RDLY": (670, "medium"), "REL": (672, "high"),
    "RES": (673, "high"), "RLDEL": (668, "high"), "ROOM": (676, "high"),
    "SENS": (679, "high"), "SPEED": (677, "high"), "STAGE": (680, "medium"),
    "T.HOLD": (681, "high"), "TIME": (675, "high"), "TUNE": (666, "medium"),
    "TYPE": (682, "high"), "VOWEL": (683, "high"), "WALL": (685, "high"),
    "WIDTH": (686, "high"), "XFREQ": (608, "medium"),
}

def main():
    xg = load_xg()
    rows = []
    warn = []
    def add(key, ctx, xid, conf):
        tip = ""
        if xid is not None:
            tip = xg.get(xid, "")
            if not tip:
                warn.append(f"xg_id {xid} missing for {key}")
        rows.append([key, ctx, tip, conf, "" if xid is None else str(xid)])
    for key, ctx, xid, conf in STATIC:
        add(key, ctx, xid, conf)
    for abbr in sorted(FX):
        xid, conf = FX[abbr]
        add("FX|" + abbr, "Effect " + abbr, xid, conf)

    with open(CSV, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["key", "context", "tooltip", "confidence", "xg_id"])
        w.writerows(rows)

    from collections import Counter
    cc = Counter(r[3] for r in rows)
    print(f"tooltips: {len(rows)}  ->  {CSV}")
    print("confidence:", dict(cc))
    if warn:
        print("WARNINGS:", *warn, sep="\n  ")
    else:
        print("no validation warnings")

if __name__ == "__main__":
    main()
