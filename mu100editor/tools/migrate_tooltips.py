#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""migrate_tooltips.py — one-time transform of data/tooltips.csv for the 2026-07-16
tooltip-expansion batch: spelling fixes, Part-EQ renames, HPF2 text, a `group`
column, and all the newly-wired controls (selectors, Voice/Keyboard/Display
regions, control wheel, SYNC, context-dependent effect params, drum-mode EQ/HPF2).

Reads + rewrites data/tooltips.csv in place (preserves existing hand edits except
where explicitly renamed). Idempotent-ish: re-running re-applies the same edits.
Run from the mu100editor root:  python tools/migrate_tooltips.py
Then: python tools/gen_tooltips.py  (regenerates the header).
"""
import csv, os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV = os.path.join(ROOT, "data", "tooltips.csv")

# --- spelling fixes applied to every tooltip ---
def fix_spelling(s):
    s = re.sub(r'[Cc]entre', lambda m: 'Center' if m.group(0)[0] == 'C' else 'center', s)
    s = re.sub(r'[Cc]uttoff', lambda m: 'Cutoff' if m.group(0)[0] == 'C' else 'cutoff', s)
    return s

# --- rename existing rows (key -> new tooltip) ---
RENAME = {
    "MULTI PART|EQ BASS GAIN":       "Part EQ Low Shelf Band 1 Gain",
    "MULTI PART|EQ BASS FREQUENCY":  "Part EQ Low Shelf Band 1 Frequency",
    "MULTI PART|EQ TREBLE GAIN":     "Part EQ High Shelf Band 5 Gain",
    "MULTI PART|EQ TREBLE FREQUENCY":"Part EQ High Shelf Band 5 Frequency",
    "PLUGIN|HPF2":                   "High-Pass Filter 2 Cutoff Frequency",
    # FX|R.DEL / FX|DELAY are now context-keyed (see NEW rows); blank the generics.
    "FX|R.DEL":                      "",
    "FX|DELAY":                      "",
}

# --- group assignment for a key (human organization only; gen ignores it) ---
def group_for(key):
    if key.startswith("MULTI PART|"): return "Part"
    if key.startswith("DRUM SETUP|"): return "Drum"
    if key.startswith("XG SYSTEM|"):  return "System"
    if key.startswith("MULTI EQ|"):   return "MultiEQ"
    if key.startswith("CTRL|"):       return "Controller"
    if key.startswith("FX|"):         return "EffectParam"
    if key.startswith("SEL|"):        return "Selector"
    if key.startswith("VOICE|"):      return "Voice"
    if key.startswith("KBD|"):        return "Keyboard"
    if key.startswith("DISP|"):       return "Display"
    if key.startswith("EFFECT|"):     return "EffectParam"
    if key == "PLUGIN|TEMPO":         return "System"
    if key.startswith("PLUGIN|DRUM"): return "Drum"
    if key.startswith("PLUGIN|"):     return "Part"   # HPF2 + PART EQ mid bands
    return "Misc"

# --- new rows: (key, context, tooltip, confidence) ---
NEW = [
    # Part EQ mid peaking bands (plugin-only), part + drum, bands 2..4 x gain/freq/q
    *[(f"PLUGIN|PART EQ B{b} {r.upper()}", f"Part EQ mid band {b} {r}",
       f"Part EQ Peak Band {b} {r}", "high")
      for b in (2, 3, 4) for r in ("Gain", "Frequency", "Q")],
    *[(f"PLUGIN|DRUM EQ B{b} {r.upper()}", f"Drum EQ mid band {b} {r}",
       f"Drum EQ Peak Band {b} {r}", "high")
      for b in (2, 3, 4) for r in ("Gain", "Frequency", "Q")],
    # Drum-mode shelf EQ counterparts
    ("DRUM SETUP|EQ BASS GAIN",      "Drum EQ", "Drum EQ Low Shelf Band 1 Gain", "high"),
    ("DRUM SETUP|EQ BASS FREQUENCY", "Drum EQ", "Drum EQ Low Shelf Band 1 Frequency", "high"),
    ("DRUM SETUP|EQ TREBLE GAIN",    "Drum EQ", "Drum EQ High Shelf Band 5 Gain", "high"),
    ("DRUM SETUP|EQ TREBLE FREQUENCY","Drum EQ","Drum EQ High Shelf Band 5 Frequency", "high"),
    # HPF2 drum-mode counterpart
    ("PLUGIN|DRUM HPF2", "Drum HPF2", "Drum High-Pass Filter 2 Cutoff Frequency", "high"),
    # Selectors
    ("SEL|EFFECT TYPE",    "Effect unit selector",     "Effect Type",         "high"),
    ("SEL|CONTROLLER",     "Controller source list",   "Controller Selection","high"),
    ("SEL|EFFECT PROGRAM", "Effect program list",      "Effect Program",      "high"),
    # Voice module
    ("VOICE|PART SELECT", "Voice Pt column",  "Part {n} Select",           "high"),
    ("VOICE|CH",          "Voice CH column",  "Part MIDI receive channel", "high"),
    ("VOICE|EL",          "Voice EL column",  "Part Element Reserve",      "high"),
    ("VOICE|VOICE NAME",  "Voice name column","Voice Program Selection",   "high"),
    # Keyboard module
    ("KBD|VEL LOW",       "Velocity limit low",  "Part Velocity Limit Low",  "high"),
    ("KBD|VEL HIGH",      "Velocity limit high", "Part Velocity Limit High", "high"),
    ("KBD|DRUM EDIT KEY", "Drum edit key box",   "Drum Edit Key",            "high"),
    ("KBD|KEYBOARD",      "Keyboard (normal)",
        "Keyboard: L. Mouse Sets Play Range, R. Mouse Plays Notes", "high"),
    ("KBD|KEYBOARD DRUM", "Keyboard (drum edit)",
        "L. Mouse Selects Drum Edit Key, R. Mouse Plays Notes", "high"),
    # Graphic display mode buttons
    ("DISP|VOLUME",   "Display button", "Part Volume Overview Mode",        "high"),
    ("DISP|PAN",      "Display button", "Part Pan Overview Mode",           "high"),
    ("DISP|AMP EG",   "Display button", "Amplitude Envelope Generator Mode","high"),
    ("DISP|PITCH EG", "Display button", "Pitch Envelope Generator Mode",    "high"),
    ("DISP|REVERB",   "Display button", "Part Reverb Overview Mode",        "high"),
    ("DISP|CHORUS",   "Display button", "Part Chorus Overview Mode",        "high"),
    ("DISP|VAR",      "Display button", "Part Variation Overview Mode",     "high"),
    ("DISP|EQ",       "Display button", "Part/Drum EQ Mode",                "high"),
    ("DISP|HPF2",     "Display button", "Part High-Pass Filter 2 Overview Mode", "high"),
    # Controller wheel + resend LED
    ("CTRL|WHEEL",      "Control wheel",       "Control Wheel",                  "high"),
    ("CTRL|RESEND LED", "Resend-on-edit LED",  "Resend Wheel Position on Edits", "high"),
    # Effect tempo-sync toggle
    ("EFFECT|SYNC", "Effect SYNC toggle", "Use note-based tempo-synced time values", "high"),
    # Context-dependent effect params (looked up before the generic FX|<abbr>)
    ("FX|R.DEL@2", "R.DEL in reverb effects (disp 2)",
        "Delay Between Early Reflections and Reverberation", "high"),
    ("FX|R.DEL@6", "R.DEL in delay effects (disp 6)", "Right Channel Delay (ms)", "high"),
    ("FX|COMP+DIST+DLY|DELAY", "DELAY in COMP+DIST+DLY", "Spacing of Echo Reflections", "high"),
    ("FX|COMP+ODRV+DLY|DELAY", "DELAY in COMP+ODRV+DLY", "Spacing of Echo Reflections", "high"),
]

GROUP_ORDER = ["System", "MultiEQ", "Part", "Drum", "Controller", "Selector",
               "EffectParam", "Voice", "Keyboard", "Display", "Misc"]

def main():
    rows = {}
    order = []
    with open(CSV, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            key = r["key"]
            rows[key] = {"key": key, "context": r.get("context", ""),
                         "tooltip": r.get("tooltip", ""), "confidence": r.get("confidence", ""),
                         "xg_id": r.get("xg_id", "")}
            order.append(key)

    # rename + spelling on existing
    for key, row in rows.items():
        if key in RENAME:
            row["tooltip"] = RENAME[key]
        row["tooltip"] = fix_spelling(row["tooltip"])

    # add new rows (don't clobber a hand-filled existing one)
    for key, ctx, tip, conf in NEW:
        if key in rows and rows[key]["tooltip"]:
            continue
        rows[key] = {"key": key, "context": ctx, "tooltip": fix_spelling(tip),
                     "confidence": conf, "xg_id": rows.get(key, {}).get("xg_id", "")}
        if key not in order:
            order.append(key)

    # assign group, sort by (group order, key)
    out = list(rows.values())
    for row in out:
        row["group"] = group_for(row["key"])
    out.sort(key=lambda r: (GROUP_ORDER.index(r["group"]) if r["group"] in GROUP_ORDER else 99,
                            r["key"]))

    with open(CSV, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["group", "key", "context", "tooltip", "confidence", "xg_id"])
        w.writeheader()
        for row in out:
            w.writerow(row)

    from collections import Counter
    print(f"rows: {len(out)}  ->  {CSV}")
    print("by group:", dict(Counter(r["group"] for r in out)))
    blank = [r["key"] for r in out if not r["tooltip"]]
    print("still blank:", blank if blank else "none")

if __name__ == "__main__":
    main()
