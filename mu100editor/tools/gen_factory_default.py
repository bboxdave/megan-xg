#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""gen_factory_default.py — generate codec/MU100FactoryDefault.h.

Reads the MU100 power-on factory dump ("reference/syx examples/all-factory-default.syx")
and emits it as a raw SysEx byte array the plugin folds into a default DeviceState.
The native Performance blocks (model 0x59) are stripped so the embedded default stays
Multi-32 scope; the XG (0x4C) and native-system (0x49) messages are kept.

Run from the mu100editor root:  python tools/gen_factory_default.py
(In the monorepo the build compiles this header directly from codec/, so no copy step.)
"""
import os
from spdx_header import CPP_LINES

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYX = os.path.join(ROOT, "reference", "syx examples", "all-factory-default.syx")
OUT = os.path.join(ROOT, "codec", "MU100FactoryDefault.h")

STRIP_MODELS = {0x59}   # native Performance — kept out of the Multi-32 default scope
PER_LINE = 20

def main():
    data = open(SYX, "rb").read()

    # Split into complete F0..F7 messages, dropping the stripped models.
    kept = bytearray()
    i = 0
    while i < len(data):
        if data[i] != 0xF0:
            i += 1
            continue
        j = data.find(0xF7, i)
        if j < 0:
            break
        msg = data[i:j + 1]
        if not (len(msg) >= 4 and msg[3] in STRIP_MODELS):
            kept += msg
        i = j + 1

    L = []
    L.extend(CPP_LINES)
    L.append("#pragma once")
    L.append("#include <cstddef>")
    L.append("")
    L.append("// AUTO-GENERATED from reference/syx examples/all-factory-default.syx")
    L.append("// (native Performance model 0x59 stripped so editorState stays Multi-32 scope).")
    L.append("// MU100 power-on factory state as a raw SysEx byte array; DeviceState::load")
    L.append("// folds it into a complete default model. Regenerate if the factory dump changes.")
    L.append("")
    L.append("namespace mu100 {")
    L.append("inline const unsigned char kFactoryDefault[] = {")
    for k in range(0, len(kept), PER_LINE):
        row = kept[k:k + PER_LINE]
        L.append("    " + "".join(f"0x{b:02X}," for b in row))
    L.append("};")
    L.append("inline const std::size_t kFactoryDefaultLen = sizeof(kFactoryDefault);")
    L.append("} // namespace mu100")
    L.append("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))
    print(f"wrote {OUT}  ({len(kept)} bytes)")

if __name__ == "__main__":
    main()
