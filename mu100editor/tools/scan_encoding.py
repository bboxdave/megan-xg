#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Empirically classify multi-byte param encoding (nibble vs 7-bit byte-pair)
by expanding every bulk dump / param change into address->byte and inspecting
the byte ranges at each size-2/4 parameter address. If every byte at a param
stays <=0x0F it's (probably) nibblized; if any byte exceeds 0x0F it must be a
7-bit byte value."""
import json, glob, os, importlib.util
spec = importlib.util.spec_from_file_location('a', os.path.join(os.path.dirname(__file__), 'analyze_syx.py'))
a = importlib.util.module_from_spec(spec); spec.loader.exec_module(a)

ROOT = os.path.dirname(os.path.dirname(__file__))

def expand(path):
    """Return dict (model, addr24) -> byte from a .syx (bulk + param change)."""
    data = open(path, 'rb').read()
    st = {}
    for m in a.msgs(data):
        b = list(m)
        if len(b) < 5 or b[1] != 0x43:
            continue
        cmd = b[2] & 0xF0; model = b[3]
        if cmd == 0x10:                      # param change
            base = (b[4] << 14) | (b[5] << 7) | b[6]
            for k, v in enumerate(b[7:-1]):
                st[(model, base + k)] = v
        elif cmd == 0x00:                    # bulk dump
            base = (b[6] << 14) | (b[7] << 7) | b[8]
            for k, v in enumerate(b[9:-2]):
                st[(model, base + k)] = v
    return st

# Merge all example files into per-address observed byte sets.
from collections import defaultdict
obs = defaultdict(set)
for f in glob.glob(os.path.join(ROOT, 'reference', 'syx examples', '*.syx')):
    for key, v in expand(f).items():
        obs[key].add(v)

params = json.load(open(os.path.join(ROOT, 'data', 'parameters.json'), encoding='utf-8'))

def _tmpl_match(tmpl, val):
    """tmpl is a 2-char field like '08' (fixed) or 'nn'/'0n'/'3n'/'pp' (wildcard).
    Returns True if the observed byte `val` is compatible."""
    try:
        return int(tmpl, 16) == val          # pure-hex => exact match
    except ValueError:
        pass
    # has a placeholder letter; check any fixed nibble that is hex
    hi, lo = tmpl[0], tmpl[1]
    ok = True
    if hi in '0123456789abcdefABCDEF':
        ok = ok and ((val >> 4) & 0xF) == int(hi, 16)
    if lo in '0123456789abcdefABCDEF':
        ok = ok and (val & 0xF) == int(lo, 16)
    return ok

# For template params we scan all mid/high instances present in obs.
print('%-4s %-26s %-22s %s' % ('size', 'param', 'block', 'observed byte[i] ranges'))
for t in params['tables']:
    for p in t['params']:
        if p['size'] not in (2, 4):
            continue
        ah, am, al = p['addr']['high'], p['addr']['mid'], p['addr']['low']
        if al is None:
            continue
        low = int(al, 16)
        # collect, over every (model, addr) whose low byte run matches and whose
        # high/mid match the (possibly templated) addr, the bytes at offsets 0..size-1
        ranges = [set() for _ in range(p['size'])]
        for (model, A), vals in obs.items():
            a0 = (A >> 14) & 0x7F; a1 = (A >> 7) & 0x7F; a2 = A & 0x7F
            if a2 != low:
                continue
            if ah not in (None,) and not _tmpl_match(ah, a0):
                continue
            if am not in (None,) and not _tmpl_match(am, a1):
                continue
            for i in range(p['size']):
                k = (model, A + i)
                if k in obs:
                    ranges[i] |= obs[k]
        desc = ' '.join('b%d:[%02X..%02X]' % (i, min(r), max(r)) if r else 'b%d:--' % i
                        for i, r in enumerate(ranges))
        anyhi = any((r and max(r) > 0x0F) for r in ranges)
        tag = 'BYTEPAIR/7bit' if anyhi else 'maybe-nibble'
        print('%-4d %-26s %-22s %-12s %s' % (p['size'], p['name'][:26], t['table'][:22], tag, desc))
