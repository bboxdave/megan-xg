#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Build data/voices.json from XGEdit's voice database (reference/XGVoice.ini),
with the 37 kit names taken from data/drums.json (Yamaha's Sound List PDF).

This SUPERSEDES the PDF extractor (extract_voices.py): XGVoice.ini is XGEdit's
authoritative voice list, with full display names, every bank (incl. the LSB 127
"MU100 addition" twins the PDF lacked), and per-voice model + element data.

INI structure (decoded):
  [vce_list]  type=1  -> top categories: `idx=bankMSB,Name`
                        (we keep Melody=0, SFX=64, MU100 Exclusive=48,
                         SFX Kit=126, Drum Kit=127; skip VL*/DX*/A-D).
  navigation  type=0  -> `idx=a,b,ChildSection` (recurse into [ChildSection]).
  leaf        type=2  -> `idx=program0,lsb,name` (an actual voice/kit).

Per-voice markers inside the name field:
  * leading byte 0xB2 ("2")  -> 2 elements   (else 1).
  * trailing  '*4'           -> MU100 Addition
              '***'          -> MU90 Addition
              '**'           -> MU80 Addition
              (none)         -> XG Standard
Both markers are stripped to leave the clean display name.

Run:  python tools/build_voices.py
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INI = os.path.join(ROOT, "reference", "XGVoice.ini")
DRUMS = os.path.join(ROOT, "data", "drums.json")
SHORT = os.path.join(ROOT, "data", "short_names.json")
OUT = os.path.join(ROOT, "data", "voices.json")

# Top categories we support -> bank MSB. (VL/DX plug-in boards and A-D excluded.)
TOP = {"Melody": 0, "SFX": 64, "MU100 Exclusive": 48, "SFX Kit": 126, "Drum Kit": 127}
MODEL_MARKERS = [("*4", "MU100"), ("***", "MU90"), ("**", "MU80")]

# Full display names we spell out where XGVoice.ini abbreviates. XGEdit squeezed
# its names into a narrow list box; ours is wide enough for the word, and every
# replacement here stays inside the 26-character budget the Voice module's list
# already carries ("DX + Analog Electric Piano"). Keyed by (msb, pgm, lsb) with
# the INI's spelling alongside, so a future XGVoice.ini change trips the
# assertion in main() instead of silently reverting the name. The 8-character
# LCD name is NOT touched -- that one is Yamaha's, out of the program ROM.
NAME_OVERRIDES = {
    (0, 2, 65):  ("Brght Cncert GrndKSP",     "Bright Concert Grand KSP"),
    (0, 5, 45):  ("Velo Crossfade E.Pno 1",   "Velo Crossfade E.Piano 1"),
    (0, 6, 45):  ("Velo Crossfade E.Pno 2",   "Velo Crossfade E.Piano 2"),
    (0, 17, 3):  ("Stereo Drawbar Org",       "Stereo Drawbar Organ"),
    (0, 17, 73): ("Dim Chorus",               "Dim Chorus Organ"),
    (0, 17, 75): ("Mellorgan",                "Mellow Organ"),
    (0, 17, 76): ("Fuzzorgan",                "Fuzz Organ"),
    (0, 18, 32): ("Detuned Percussive Org",   "Detuned Percussive Organ"),
    (0, 18, 69): ("Dim Click",                "Dim Click Organ"),
    (0, 18, 71): ("FM Click",                 "FM Click Organ"),
    (0, 18, 73): ("Super Rotary",             "Super Rotary Organ"),
    (0, 19, 65): ("Slow Rotary",              "Slow Rotary Organ"),
    (0, 19, 66): ("Fast Rotary",              "Fast Rotary Organ"),
    (0, 19, 67): ("Glacial Rotary",           "Glacial Rotary Organ"),
    (0, 24, 66): ("Tight Acrdion Detuned",    "Tight Accordion Detuned"),
    (0, 26, 41): ("SteelGtr with BodySound",  "Steel Guitar w Body Sound"),
    (0, 27, 69): ("DX Jazz Gtr Detuned",      "DX Jazz Guitar Detuned"),
    (0, 28, 66): ("Mid Tone Gtr Stereo",      "Mid Tone Guitar Stereo"),
    (0, 30, 32): ("OverdriveGtr Detuned",     "Overdriven Guitar Detuned"),
    (0, 31, 12): ("Distorted Rhythm Gtr",     "Distorted Rhythm Guitar"),
    (0, 33, 45): ("Velo Crsfade UprightBass", "Velo Crsfade Upright Bass"),
    (0, 34, 40): ("Bass & Distorted E.Gtr",   "Bass & Distorted E.Guitar"),
    (0, 35, 40): ("PickBass & MutedGtr",      "Pick Bass & Muted Guitar"),
    (0, 62, 36): ("Trumpet & Trombone2",      "Trumpet & Trombone 2"),
    (0, 62, 68): ("Power Sfrznd Bright",      "Powered Sforzando Bright"),
    (0, 63, 40): ("SynBrass with Osc",        "Synth Brass w Subosc"),
    (0, 88, 69): ("Sync B&L",                 "Sync Bass & Lead"),
    # The INI's raw name carries the INI's own doubled-quote escaping; the full
    # name should read like the 8-character one does.
    (0, 17, 40): ('"16+2""2/3"',              '16+2"2/3'),
    # Decades: the INI writes 60's/70's, i.e. an apostrophe standing for nothing.
    # The elided digits are the leading ones, so the apostrophe belongs in front.
    (0, 5, 64):  ("60's Electric Piano 1",    "'60s Electric Piano 1"),
    (0, 17, 33): ("60's Drawbar Organ 1",     "'60s Drawbar Organ 1"),
    (0, 17, 34): ("60's Drawbar Organ 2",     "'60s Drawbar Organ 2"),
    (0, 17, 35): ("70's Drawbar Organ 1",     "'70s Drawbar Organ 1"),
    (0, 17, 37): ("60's Drawbar Organ 3",     "'60s Drawbar Organ 3"),
    (0, 17, 65): ("70's Drawbar Organ 2",     "'70s Drawbar Organ 2"),
    (0, 18, 24): ("70's Percussive Organ 1",  "'70s Percussive Organ 1"),
    (0, 49, 35): ("60's Strings",             "'60s Strings"),
    (0, 50, 64): ("70's Strings",             "'70s Strings"),
}


def parse_ini():
    sections = {}
    cur = None
    with open(INI, "rb") as f:
        text = f.read().decode("latin-1")
    for raw in text.splitlines():
        if raw.startswith("[") and raw.endswith("]"):
            cur = raw[1:-1]
            sections[cur] = {"meta": {}, "entries": []}
        elif cur and "=" in raw:
            k, v = raw.split("=", 1)
            if k.isdigit():
                sections[cur]["entries"].append(v)
            else:
                sections[cur]["meta"][k] = v
    return sections


def parse_voice(value):
    """`program0,lsb,name[markers]` -> (pgm, lsb, clean_name, model, elements)."""
    prog, lsb, name = value.split(",", 2)
    elements = 1
    if name and ord(name[0]) == 0xB2:        # leading "2" superscript glyph
        elements = 2
        name = name[1:]
    model = "XG"
    s = name.rstrip()
    for mark, mdl in MODEL_MARKERS:
        if s.endswith(mark):
            model = mdl
            s = s[: -len(mark)]
            break
    return int(prog) + 1, int(lsb), s.strip(), model, elements


def walk(sections, name, msb, topcat, out):
    sec = sections.get(name)
    if sec is None:
        return
    is_nav = sec["meta"].get("type") == "0"
    for val in sec["entries"]:
        if is_nav:                            # `a,b,ChildSection`
            walk(sections, val.split(",", 2)[2], msb, topcat, out)
        else:                                 # leaf voice line
            pgm, lsb, nm, model, elements = parse_voice(val)
            cat = name if topcat == "Melody" else topcat
            v = {"pgm": pgm, "msb": msb, "lsb": lsb, "category": cat,
                 "name": nm, "model": model, "elements": elements}
            if msb == 0 and lsb in (126, 127):
                v["native"] = True
            out.append(v)


def main():
    sections = parse_ini()
    out = []
    for val in sections["vce_list"]["entries"]:
        msb, name = val.split(",", 1)
        if name in TOP:
            walk(sections, name, int(msb), name, out)

    # de-dup by (msb, pgm, lsb), keep first; sort for stable output.
    seen, uniq = set(), []
    for v in out:
        key = (v["msb"], v["pgm"], v["lsb"])
        if key in seen:
            continue
        seen.add(key)
        uniq.append(v)
    uniq.sort(key=lambda v: (v["msb"], v["pgm"], v["lsb"]))

    # Kit names (bank MSB 126/127) come from data/drums.json, which
    # extract_drums.py reads straight out of Yamaha's own Sound List PDF, not
    # from XGEdit. The two sources carry the same 37 kits at the same
    # (msb, pgm, lsb) keys and 34 of the 37 names already agree; taking
    # Yamaha's spelling for all of them makes the whole kit block
    # Yamaha-sourced for three cosmetic string changes ("HipHop Kit" ->
    # "Hip Hop Kit", and dropping XGEdit's "StndKit " prefix on MU100 Native /
    # MU Basic). "Brush Kit 2" USED to be a fourth: the PDF prints it unspaced
    # and extract_drums.py's KIT_NAME_OVERRIDES now normalises it there, which
    # happens to land on XGEdit's spelling. Nothing keys off these
    # strings -- they are display-only -- so this is a naming change, not a
    # data change. Everything else about a kit (category, model, elements)
    # still comes from the INI, which is why this is not a full de-coupling.
    with open(DRUMS, encoding="utf-8") as f:
        kit_name = {(k["msb"], k["pgm"], k["lsb"]): k["name"]
                    for k in json.load(f)["kits"]}
    renamed = 0
    for v in uniq:
        nm = kit_name.get((v["msb"], v["pgm"], v["lsb"]))
        if nm and nm != v["name"]:
            print("  kit name from drums.json: %r -> %r" % (v["name"], nm))
            v["name"] = nm
            renamed += 1
    missing = [v for v in uniq if v["msb"] in (126, 127)
               and (v["msb"], v["pgm"], v["lsb"]) not in kit_name]
    if missing:
        raise SystemExit("kits absent from drums.json: %s" % [v["name"] for v in missing])
    print("  kits re-sourced from drums.json: %d of %d renamed"
          % (renamed, len(kit_name)))

    # Spelled-out melodic names (see NAME_OVERRIDES). Applied after the kit
    # block so the two rename passes cannot fight over the same voice.
    by_key = {(v["msb"], v["pgm"], v["lsb"]): v for v in uniq}
    for key, (was, now) in NAME_OVERRIDES.items():
        v = by_key.get(key)
        if v is None or v["name"] != was:
            raise SystemExit("NAME_OVERRIDES %s expects %r, found %r -- "
                             "XGVoice.ini changed, re-check the override"
                             % (str(key), was, v and v["name"]))
        v["name"] = now
    print("  full names spelled out: %d" % len(NAME_OVERRIDES))

    # The 8-character LCD name beside the full one. Both have to be present for
    # every voice: the plugin's "Voice names" option swaps between them at will,
    # so a hole here would show up as a blank name, not as a fallback.
    with open(SHORT, encoding="utf-8") as f:
        short = json.load(f)["names"]
    for v in uniq:
        key = "%d/%d/%d" % (v["msb"], v["lsb"], v["pgm"])
        if key not in short:
            raise SystemExit("no short name for %s (%s) -- rerun "
                             "tools/extract_short_names.py" % (key, v["name"]))
        v["short"] = short[key]
    print("  short (8-char LCD) names merged: %d" % len(uniq))

    native = sum(1 for v in uniq if v.get("native"))
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump({
            "count": len(uniq),
            "native_count": native,
            "note": ("MU100 XG-mode voices + SFX/MU100-Exclusive voices + SFX/Drum "
                     "kits. Melodic/SFX display names come from XGEdit's "
                     "XGVoice.ini; the 37 kit names (MSB 126/127) come from "
                     "data/drums.json, i.e. Yamaha's own Sound List PDF. "
                     "'model' = XG/MU80/MU90/MU100 addition; 'elements' = 1 or 2; "
                     "'native':true marks MSB0 LSB126/127; 'short' is the "
                     "8-character LCD name from data/short_names.json."),
            "voices": uniq,
        }, f, indent=1, ensure_ascii=False)

    from collections import Counter
    print("wrote %s: %d voices (%d native)" % (OUT, len(uniq), native))
    print("  by MSB:", dict(Counter(v["msb"] for v in uniq)))
    print("  by model:", dict(Counter(v["model"] for v in uniq)))
    print("  elements:", dict(Counter(v["elements"] for v in uniq)))
    print("  lsb127 (msb0):", sum(1 for v in uniq if v["msb"] == 0 and v["lsb"] == 127))


if __name__ == "__main__":
    main()
