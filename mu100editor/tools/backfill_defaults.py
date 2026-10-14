#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Back-fill `default_raw` in data/parameters.json from a factory-default
bulk dump (reference/syx examples/all-factory-default.syx). Far more reliable
than transcribing the PDF's Default column. Writes default_raw as a
space-separated hex string of `size` bytes read at each param's factory address.

Each MIDI Parameter Change table maps to a Model ID; templated address fields
(nn/0n/3n/pp/tt/mm) are resolved to the first instance present in the dump."""
import json, os, importlib.util

ROOT = os.path.dirname(os.path.dirname(__file__))
spec = importlib.util.spec_from_file_location('a', os.path.join(ROOT, 'tools', 'analyze_syx.py'))
a = importlib.util.module_from_spec(spec); spec.loader.exec_module(a)

FACTORY = os.path.join(ROOT, 'reference', 'syx examples', 'all-factory-default.syx')

# Table name -> Model ID (per MU100 MIDI spec 2.1.3 / 2.1.4 / 2.1.5).
TABLE_MODEL = {
    'XG SYSTEM': 0x4C, 'SYSTEM INFORMATION': 0x4C, 'EFFECT 1': 0x4C,
    'MULTI EQ': 0x4C, 'EFFECT 2': 0x4C, 'DISPLAY DATA': 0x4C, 'MULTI PART': 0x4C,
    'A/D PART': 0x4C, 'A/D System': 0x4C, 'DRUM SETUP': 0x4C, 'PLUGIN BOARD': 0x4C,
    'MU100 SYSTEM': 0x49, 'REMOTE SWITCH': 0x49, 'MU80, MU50 INTERNAL PERFORMANCE': 0x49,
    'CURRENT PERFORMANCE': 0x59, 'INTERNAL PERFORMANCE': 0x59,
}

def expand(path):
    st = {}
    for m in a.msgs(open(path, 'rb').read()):
        b = list(m)
        if len(b) < 5 or b[1] != 0x43: continue
        cmd, model = b[2] & 0xF0, b[3]
        if cmd == 0x10:
            base = (b[4] << 14) | (b[5] << 7) | b[6]
            for k, v in enumerate(b[7:-1]): st[(model, base + k)] = v
        elif cmd == 0x00:
            base = (b[6] << 14) | (b[7] << 7) | b[8]
            for k, v in enumerate(b[9:-2]): st[(model, base + k)] = v
    return st

def tmpl_match(tmpl, val):
    try: return int(tmpl, 16) == val
    except ValueError: pass
    hi, lo = tmpl[0], tmpl[1]; ok = True
    if hi in '0123456789abcdefABCDEF': ok = ok and ((val >> 4) & 0xF) == int(hi, 16)
    if lo in '0123456789abcdefABCDEF': ok = ok and (val & 0xF) == int(lo, 16)
    return ok

def find_default(obs, model, ah, am, al, size):
    """Return list of `size` bytes at the first (model,addr) matching the
    (possibly templated) high/mid and exact low, with size consecutive bytes
    all present; else None."""
    if al is None: return None
    try: low = int(al, 16)
    except ValueError: return None        # templated low byte (nn/mm) — skip
    cands = []
    for (mdl, A), v in obs.items():
        if mdl != model: continue
        a0, a1, a2 = (A >> 14) & 0x7F, (A >> 7) & 0x7F, A & 0x7F
        if a2 != low: continue
        if ah is not None and not tmpl_match(ah, a0): continue
        if am is not None and not tmpl_match(am, a1): continue
        if all((mdl, A + i) in obs for i in range(size)):
            cands.append(A)
    if not cands: return None
    A = min(cands)                       # first instance
    return [obs[(model, A + i)] for i in range(size)]

def main():
    obs = expand(FACTORY)
    params = json.load(open(os.path.join(ROOT, 'data', 'parameters.json'), encoding='utf-8'))
    filled = before = total = 0
    for t in params['tables']:
        model = TABLE_MODEL.get(t['table'])
        for p in t['params']:
            total += 1
            if p.get('default_raw'): before += 1
            if model is None: continue
            bs = find_default(obs, model, p['addr']['high'], p['addr']['mid'],
                              p['addr']['low'], p['size'])
            if bs is not None:
                p['default_raw'] = ' '.join('%02X' % x for x in bs)
                p['default_src'] = 'factory-dump'
                filled += 1
    out = os.path.join(ROOT, 'data', 'parameters.json')
    # utf-8 explicitly: descriptions carry real non-ASCII ("-∞dB...", the
    # "bit3-0→bit7-4" nibble maps), and the default locale encoding on Windows
    # (cp1252) cannot represent them.
    json.dump(params, open(out, 'w', encoding='utf-8'), ensure_ascii=False, indent=2)
    print('params total           :', total)
    print('had default before     :', before)
    print('defaults from factory  :', filled)
    print('still without default  :', total - filled)
    print('wrote', out)

if __name__ == '__main__':
    main()
