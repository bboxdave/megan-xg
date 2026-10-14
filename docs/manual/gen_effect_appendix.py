#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Generate A1-effect-reference.md -- the manual's effect appendix.

Run from docs/manual/:
    python gen_effect_appendix.py

Reads, all under mu100editor/:
    codec/MU100Effects.h   effect type -> name, MSB/LSB, layout id, 16 defaults
    data/effect_conv.json  layout id  -> per-slot label, range, unit, enum list
    data/tables.json       the 11 raw->display conversion tables

The type lists and defaults live in the generated header rather than in the
JSON because that header is what the plugin itself compiles, so the appendix and
the running program cannot disagree about which effects exist or what their
factory values are.

Re-run this after re-running the mu100editor data pipeline.  Never hand-edit the
output.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
EDITOR = HERE.parent.parent / "mu100editor"
OUT = HERE / "A1-effect-reference.md"

UNITS = ["reverb", "chorus", "variation", "insertion"]
UNIT_TITLE = {
    "reverb": "Reverb types",
    "chorus": "Chorus types",
    "variation": "Variation types",
    "insertion": "Insertion types",
}

# Which slots the editor can lock to the tempo, keyed by LAYOUT id exactly as
# kFxSync in Source/EffectParametersPanel.cpp is -- keying by name would not
# survive the abbreviated program names, and several effects share a layout.
# Parsed from that file so it cannot drift; the fallback is "mark nothing".
SYNC_SRC = HERE.parent.parent / "Source" / "EffectParametersPanel.cpp"


def read_sync() -> dict[int, list[int]]:
    try:
        text = SYNC_SRC.read_text(encoding="utf-8")
        body = text[text.index("kFxSync[]") : text.index("};", text.index("kFxSync[]"))]
    except (OSError, ValueError):
        return {}
    out: dict[int, list[int]] = {}
    for m in re.finditer(r"\{\s*(\d+)\s*,\s*(?:true|false)\s*,\s*\{([-\d,\s]*)\}\s*\}", body):
        slots = [int(v) for v in m.group(2).split(",") if int(v) >= 0]
        out[int(m.group(1))] = slots
    return out


# --------------------------------------------------------------------------
# Sources
# --------------------------------------------------------------------------

_TYPE_ROW = re.compile(
    r'\{\s*"([^"]*)"\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,'
    r'\s*(\d+)\s*,\s*\{([^}]*)\}\s*\}'
)


def read_types() -> dict[str, list[dict]]:
    """Parse the kXxxTypes[] arrays out of the generated effects header."""
    text = (EDITOR / "codec" / "MU100Effects.h").read_text(encoding="utf-8")
    out: dict[str, list[dict]] = {}
    for unit in UNITS:
        array = f"k{unit.capitalize()}Types[]"
        start = text.index(array)
        body = text[start : text.index("\n};", start)]
        rows = []
        for m in _TYPE_ROW.finditer(body):
            rows.append(
                {
                    "name": m.group(1),
                    "msb": int(m.group(2), 16),
                    "lsb": int(m.group(3), 16),
                    "layout": int(m.group(4)),
                    "deflt": [int(v) for v in m.group(5).split(",")],
                }
            )
        if not rows:
            raise SystemExit(f"no {array} entries parsed -- header format changed?")
        out[unit] = rows
    return out


_TABLE_VALS = re.compile(
    r"kConvTable(\d+)_vals\[\]\s*=\s*\{(.*?)\};", re.S
)
_TABLE_NAME = re.compile(r'\{\s*"([^"]*)"\s*,\s*\d+\s*,\s*kConvTable(\d+)_vals\s*\}')


def read_tables() -> dict[str, dict]:
    """Parse the conversion tables out of the generated codec header.

    Deliberately NOT data/tables.json: the header carries a twelfth table
    (Lo-Fi sampling frequency) that the JSON does not, and the header is what
    the plugin compiles, so it is the copy that matches what you see on screen.
    """
    text = (EDITOR / "codec" / "MU100EffectConv.h").read_text(encoding="utf-8")
    names = {n: nm for nm, n in _TABLE_NAME.findall(text)}
    out: dict[str, dict] = {}
    for num, body in _TABLE_VALS.findall(text):
        vals = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
        out[f"table#{num}"] = {"name": names.get(num, ""), "values": vals}
    if not out:
        raise SystemExit("no conversion tables parsed -- header format changed?")
    return out


def read_conv() -> tuple[dict[int, list[dict]], dict[str, dict]]:
    conv = json.loads((EDITOR / "data" / "effect_conv.json").read_text(encoding="utf-8"))
    return {int(k): v for k, v in conv["layouts"].items()}, read_tables()


# --------------------------------------------------------------------------
# Value formatting -- mirrors codec/MU100EffectConv.h's documented rules
# --------------------------------------------------------------------------


def fmt_num(v: float, decimals: int, signed: bool, unit: str) -> str:
    s = f"{v:.{decimals}f}"
    if signed and v > 0:
        s = "+" + s
    return s + unit


def table_values(tables: dict, n: int) -> list[str]:
    return tables[f"table#{n}"]["values"]


def fmt_pan(raw: int) -> str:
    if raw == 64:
        return "C"
    return f"L{64 - raw}" if raw < 64 else f"R{raw - 64}"


def fmt_special(spec: str, raw: int) -> str:
    """A balance control: an offset either side of center 64.

    Mirrors Source/XgEffectFormat.h's CK_SPECIAL case exactly -- "DW" renders
    D63 .. D=W .. W63, "ER" early-reflection vs reverb, "LH" low vs high. These
    are NOT plain numbers on screen, so they must not be plain numbers here.
    """
    a, b = (spec + "??")[0], (spec + "??")[1]
    v = raw - 64
    if v == 0:
        return f"{a}={b}"
    return f"{a}{-v}" if v < 0 else f"{b}{v}"


def fmt_value(slot: dict, raw: int, tables: dict) -> str:
    kind = slot.get("kind")
    if kind == "table":
        vals = table_values(tables, slot["table"])
        return vals[max(0, min(raw, len(vals) - 1))]
    if kind == "linear":
        lo, hi = slot["rawMin"], slot["rawMax"]
        span = (hi - lo) or 1
        v = slot["dispMin"] + (raw - lo) * (slot["dispMax"] - slot["dispMin"]) / span
        return fmt_num(v, slot.get("decimals", 0), slot.get("signed", False), slot.get("unit", ""))
    if kind == "enum":
        opts = slot.get("enums") or []
        i = raw - slot.get("rawMin", 0)
        return opts[i] if 0 <= i < len(opts) else str(raw)
    if kind == "pan":
        return fmt_pan(raw)
    if kind == "special":
        return fmt_special(slot.get("special", "??"), raw)
    return str(raw)


def fmt_range(slot: dict, tables: dict) -> str:
    kind = slot.get("kind")
    if kind == "table":
        vals = table_values(tables, slot["table"])
        lo, hi = slot["rawMin"], slot["rawMax"]
        lo_s = vals[max(0, min(lo, len(vals) - 1))]
        hi_s = vals[max(0, min(hi, len(vals) - 1))]
        # The table's own name is the only statement of what the numbers MEAN
        # (the values themselves are unitless), so it is carried through rather
        # than a unit guessed per parameter.
        name = re.sub(r"\s*\(", " (", tables[f"table#{slot['table']}"].get("name", ""))
        return f"{lo_s} – {hi_s}" + (f" — {name.lower()}" if name else "")
    if kind == "linear":
        d, sg, u = slot.get("decimals", 0), slot.get("signed", False), slot.get("unit", "")
        return f"{fmt_num(slot['dispMin'], d, sg, u)} – {fmt_num(slot['dispMax'], d, sg, u)}"
    if kind == "enum":
        return " / ".join(slot.get("enums") or [])
    if kind == "pan":
        return "L63 – C – R63"
    if kind == "special":
        sp = slot.get("special", "??")
        lo, hi = slot.get("rawMin", 1), slot.get("rawMax", 127)
        return f"{fmt_special(sp, lo)} – {sp[0]}={sp[1]} – {fmt_special(sp, hi)}"
    return f"{slot.get('rawMin', 0)} – {slot.get('rawMax', 127)}"


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------

HEADER = """<!-- GENERATED by gen_effect_appendix.py -- DO NOT EDIT BY HAND.
     Regenerate with:  python gen_effect_appendix.py  -->

# A1 Effect reference

Every effect the MU100 can load, and what its sixteen knobs do.

Use this when you have picked an effect in the **Effect Parameters** module and
want to know what an abbreviated knob label means, what range it covers, or what
the factory setting was before you started turning things.

**How to read an entry.** *Knob* is the position in the 4 × 4 grid, counting
across the top row first — the same position the control occupies on screen, so
knob 1 is top-left and knob 16 is bottom-right. Positions with nothing listed are
the blank discs you see in the grid; that effect simply has no parameter there.
*Default* is the value the effect loads with when you select it.

> **Watch out** — selecting an effect **type** loads that type's defaults over
> whatever was in the sixteen knobs. If you have dialed in something you want to
> keep, save it before you go browsing types.

**Balance controls** read as an offset either side of center, naming whichever
side is winning — so **D/W** (dry against wet) runs `D63` … `D=W` … `W63`, and
the further into `W` you go the more effect you hear. **BAL** does the same for
early reflections against reverb (`E` … `R`), and a few effects use `L`/`H` for
low against high. D/W is only active when the effect is routed as an insertion.

Effects marked **⟳** have a delay time or LFO rate the editor can lock to the
project tempo — see *Tempo-synced effects* in the Effect Parameters chapter.

"""


def main() -> int:
    types = read_types()
    layouts, tables = read_conv()
    sync = read_sync()

    out: list[str] = [HEADER]
    total = 0

    for unit in UNITS:
        rows = types[unit]
        out.append(f"## {UNIT_TITLE[unit]}\n")
        out.append(f"{len(rows)} types.\n")
        for t in rows:
            total += 1
            synced = sync.get(t["layout"], [])
            out.append(f"### {t['name']}{' ⟳' if synced else ''}\n")
            out.append(
                f"Bank MSB `{t['msb']:02X}` / LSB `{t['lsb']:02X}`"
                + (" — no effect; the unit is bypassed.\n" if t["layout"] == 0 else "\n")
            )
            slots = layouts.get(t["layout"])
            if not slots or t["layout"] == 0:
                continue
            body = []
            for i, slot in enumerate(slots):
                if slot.get("kind") in (None, "none") or not slot.get("label"):
                    continue
                note = ""
                if slot.get("varBlockOnly"):
                    note = " *(Variation block only)*"
                elif slot.get("altRawMax", 0) > slot.get("altRawMin", 0):
                    alt = dict(slot)
                    alt["rawMin"], alt["rawMax"] = slot["altRawMin"], slot["altRawMax"]
                    alt["dispMin"] = slot.get("altDispMin", slot.get("dispMin", 0))
                    alt["dispMax"] = slot.get("altDispMax", slot.get("dispMax", 0))
                    note = f" *(as an insertion: {fmt_range(alt, tables)})*"
                if i in synced:
                    note += " ⟳"
                default = fmt_value(slot, t["deflt"][i], tables) if i < len(t["deflt"]) else ""
                body.append(
                    f"| {i + 1} | {slot['label']} | {fmt_range(slot, tables)}{note} | {default} |"
                )
            if body:
                out.append("| Knob | Label | Range | Default |")
                out.append("|---:|---|---|---|")
                out.extend(body)
                out.append("")

    OUT.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{total} effect types -> {OUT.name} ({OUT.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
