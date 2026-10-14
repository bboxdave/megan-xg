#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Codegen: data/parameters.json -> codec/MU100AddressMapData.h.

Bakes the device address map into a static C++ table so MU100AddressMap needs
no runtime JSON (works in the standalone test and drops into the plugin clean).
Re-run after editing parameters.json:  python tools/gen_address_map.py

Each address byte is emitted as {base, mask}: a fixed byte has mask 0; a
templated byte (nn/0n/3n/pp/tt/mm) has mask over its placeholder nibble(s) and
base over its fixed nibble(s), so resolve(instance) = base | (instance & mask).
Encoding: size1=SINGLE, size4=NIBBLE (MASTER TUNE), size2=BYTEPAIR (effect
type/param MSB+LSB are independent 7-bit bytes, confirmed empirically)."""
import json, os, re
from spdx_header import CPP_LINES

ROOT = os.path.dirname(os.path.dirname(__file__))

TABLE_MODEL = {
    'XG SYSTEM': 0, 'SYSTEM INFORMATION': 0, 'EFFECT 1': 0, 'MULTI EQ': 0,
    'EFFECT 2': 0, 'DISPLAY DATA': 0, 'MULTI PART': 0, 'A/D PART': 0,
    'A/D System': 0, 'DRUM SETUP': 0, 'PLUGIN BOARD': 0,
    'MU100 SYSTEM': 1, 'REMOTE SWITCH': 1, 'MU80, MU50 INTERNAL PERFORMANCE': 1,
    'CURRENT PERFORMANCE': 2, 'INTERNAL PERFORMANCE': 2,
}  # values are ModelIx (0=XG,1=Native1,2=Native2)

HEXD = '0123456789abcdefABCDEF'

# --- value display -----------------------------------------------------------
# The manual's Description cell ("display_raw") already states how each parameter
# READS on screen: "0...127", "-64...0...+63 [cent]", "OFF, ON", "L63...C...R63".
# Those strings are regular enough to compile into a display rule, which is worth
# doing rather than hand-listing parameters in C++: the rule then comes from the
# same document as the address, and re-running this pipeline keeps them in step.
#
# Anything not confidently recognised emits Disp::Raw and shows the raw byte. That
# is the honest fallback — a WRONG unit is worse than a bare number.
UNIT = {'cent': ' cent', 'semitones': ' st', '%': '%', 'dB': ' dB', 'Hz': ' Hz'}

# Display kinds whose C++ formatter reads p.rangeLo/rangeHi. Those two fields hold
# the manual's PER-BYTE range, which for a multi-byte parameter is one nibble of
# several ("00...0F" for DETUNE) and not the span the display law talks about — so a
# multi-byte parameter may only use a kind that ignores them.
RANGE_DEPENDENT = ('Scaled', 'PanRnd', 'EqFreq')

def classify_display(d, rlo, rhi, size, enc):
    """display_raw -> (kind, argA, argB, unit, enums) or None (= Disp::Raw)."""
    r = _classify_display(d, rlo, rhi, size, enc)
    if r is not None and size != 1 and r[0] in RANGE_DEPENDENT:
        return None
    return r

def _classify_display(d, rlo, rhi, size, enc):
    s = (d or '').strip().replace('…', '...')
    if not s: return None
    s = re.sub(r'(?<!\.)\.\.(?!\.)', '...', s)              # "-24..0..+24"
    s = re.sub(r'\s*\((?:[^()]*)\)\s*$', '', s).strip()     # trailing restatement
    # The multi-byte cells append their bit-packing footnote to the display law:
    # "-12.8...0...+12.7 [Hz] 1st bit3-0->bit7-4 2nd bit3-0->bit3-0". That is how the
    # value is ENCODED, which MU100AddressMap already handles; strip it so what is
    # left is the law itself.
    s = re.sub(r'\s*\d+(?:st|nd|rd|th)\s+bit.*$', '', s, flags=re.I).strip()
    if s.startswith('refer to'): return None                # effect lists: see xgfmt
    if 'receive only' in s or 'ASCII' in s: return None
    if size != 1:
        # The law describes the COMBINED value, so the raw span to check it against is
        # the combined one. Only NIBBLE packing makes that derivable (4 bits a byte);
        # a BYTEPAIR's usable span is parameter-specific, so those stay Raw.
        if enc != 'NIBBLE': return None
        rlo, rhi = 0, (1 << (4 * size)) - 1

    unit = ''
    m = re.search(r'\[\s*([^\]]+?)\s*\]\s*$', s)
    if m:
        unit = UNIT.get(m.group(1).strip(), '')
        if not unit: return None
        s = s[:m.start()].strip()

    # pan, which the manual writes either bare or with its raw span restated
    if re.fullmatch(r'RND\s*,\s*L63\s*\.\.\.\s*C\s*\.\.\.\s*R63', s): return ('PanRnd', 64, 0, '', None)
    if re.fullmatch(r'L63\s*\.\.\.\s*C\s*\.\.\.\s*R63', s):           return ('Pan',    64, 0, '', None)

    if re.fullmatch(r'C\s*-\s*2\s*\.\.\.\s*G\s*8', s) and rhi - rlo == 127:
        return ('Note', rlo, 0, '', None)

    # "-inf dB...0dB...+6dB" is a lookup table, not an affine map
    if '∞' in s or 'dB...' in s: return None

    s = re.sub(r'^(-\d+)\s*-\s*(\+\d+)$', r'\1...0...\2', s)   # "-12 - +12"

    m = re.fullmatch(r'([+-]?\d+)\s*\.\.\.\s*0\s*\.\.\.\s*([+-]?\d+)', s)
    if m:
        dlo, dhi = int(m.group(1)), int(m.group(2))
        if (dhi - dlo) == (rhi - rlo):
            return ('Centred', rlo - dlo, 0, unit, None)
        # A symmetric display span over a symmetric raw span, at a different
        # resolution (-12...+12 dB or -100...+100 % across raw 0..127). Carry the
        # centre and the display half-span; the formatter scales.
        #
        # The centre is the count's midpoint, NOT the span's: 0..127 is 128 values
        # and XG centres it at 64, which is also what the Part module's own EQ
        # formula ((raw-64)*12/64) assumes. rlo + (rhi-rlo)//2 gives 63 and would
        # put every reading a step out.
        half = (rhi - rlo + 1) // 2
        if dhi == -dlo and half > 0:
            return ('Scaled', rlo + half, dhi, unit, None)
        # The same law, but ASYMMETRIC because the count is even: the FILTER-control
        # depths read "-9600...0...+9450 [cent]" over raw 0..127, i.e. a flat 150
        # cent a step centred at 64, with the low rail reaching one step further than
        # the high one. That is not a manual typo and not a range worth checking
        # against hardware — 9600/64 and 9450/63 are the same step, which is what
        # makes the reading self-consistent. (XGEdit's manual rounds the top rail to
        # +9600; XGEdit itself shows +9450, as does the device.) Scaled divides by
        # `half`, so carrying -dlo as the display half-span reproduces the step.
        if half > 0 and dlo < 0 < dhi and (-dlo) % half == 0 \
           and dhi == ((-dlo) // half) * (half - 1):
            return ('Scaled', rlo + half, -dlo, unit, None)
        return None

    # The decimal form of the centred law: "-12.8...0...+12.7 [Hz]" (part DETUNE,
    # a nibble pair whose combined value runs 0..255). Signed, hence dispB = 1.
    m = re.fullmatch(r'([+-]?\d+\.\d)\s*\.\.\.\s*0\s*\.\.\.\s*([+-]?\d+\.\d)', s)
    if m:
        dlo, dhi = float(m.group(1)), float(m.group(2))
        if abs((dhi - dlo) * 10 - (rhi - rlo)) > 0.01: return None
        return ('Tenths', rlo - int(round(dlo * 10)), 1, unit, None)

    m = re.fullmatch(r'(\d+)\s*\.\.\.\s*(\d+)', s)
    if m:
        dlo, dhi = int(m.group(1)), int(m.group(2))
        if (dhi - dlo) != (rhi - rlo): return None
        return ('Offset', dlo - rlo, 0, unit, None)

    m = re.fullmatch(r'(\d+\.\d)\s*\.\.\.\s*(\d+\.\d)', s)
    if m:
        dlo, dhi = float(m.group(1)), float(m.group(2))
        if abs((dhi - dlo) * 10 - (rhi - rlo)) > 0.01: return None
        return ('Tenths', rlo - int(round(dlo * 10)), 0, unit, None)

    # the XG EQ frequency ladder — a table, resolved in C++ against kConvTables[3]
    if unit == ' Hz' and re.fullmatch(r'[\d.]+k?\s*\.\.\.\s*[\d.]+k?', s):
        return ('EqFreq', 0, 0, '', None)

    # a literal comma list, only when it accounts for the whole raw range
    if ',' in s and '...' not in s:
        toks = [t.strip() for t in s.split(',')]
        if all(t and len(t) <= 14 and not t[0].isdigit() for t in toks) \
           and len(toks) == (rhi - rlo + 1):
            return ('Enum', rlo, 0, '', '|'.join(toks))
    return None

def parse_byte(field):
    """'08'->(0x08,0,fixed)  'nn'->(0,0xFF,tmpl)  '0n'->(0,0x0F,tmpl)
       '3n'->(0x30,0x0F,tmpl). Returns (base, mask, templated)."""
    if field is None:
        return None
    base = mask = 0
    templated = False
    hi, lo = field[0], field[1]
    if hi in HEXD: base |= int(hi, 16) << 4
    else:          mask |= 0xF0; templated = True
    if lo in HEXD: base |= int(lo, 16)
    else:          mask |= 0x0F; templated = True
    return (base, mask, templated)

def main():
    params = json.load(open(os.path.join(ROOT, 'data', 'parameters.json'), encoding='utf-8'))

    pool = {}          # string -> index
    pool_list = []
    def intern(s):
        if s not in pool:
            pool[s] = len(pool_list)
            pool_list.append(s)
        return pool[s]
    intern('')         # index 0 is the empty string, so it doubles as "no enum list"

    rows = []
    skipped = 0
    dispCount = {}
    for t in params['tables']:
        tbl = t['table']
        model = TABLE_MODEL.get(tbl)
        if model is None:
            skipped += len(t['params']); continue
        for p in t['params']:
            hi = parse_byte(p['addr']['high'])
            mid = parse_byte(p['addr']['mid'])
            lo = parse_byte(p['addr']['low'])
            if hi is None or lo is None:
                skipped += 1; continue          # address incomplete (e.g. drum setup)
            if mid is None:
                mid = (0, 0, False)
            size = p['size']
            # Encoding defaults from size, but a param may override it (e.g.
            # DETUNE is size 2 yet nibble-packed, not a 7-bit byte pair).
            enc = p.get('enc') or {1: 'SINGLE', 2: 'BYTEPAIR', 4: 'NIBBLE'}.get(size, 'SINGLE')
            multi = sum(1 for b in (hi, mid, lo) if b[2]) > 1
            rng = p.get('range') or {}
            def h2i(x):
                try: return int(x, 16)
                except: return 0
            rlo, rhi = h2i(rng.get('lo')), h2i(rng.get('hi', '7f'))
            disp = classify_display(p.get('display_raw'), rlo, rhi, size, enc)
            kind, dA, dB, unit, enums = disp or ('Raw', 0, 0, '', None)
            dispCount[kind] = dispCount.get(kind, 0) + 1
            rows.append(dict(
                model=model, hi=hi, mid=mid, lo=lo, size=size, enc=enc,
                multi=multi, name=p['name'], table=tbl,
                rlo=rlo, rhi=rhi,
                nameOff=intern(p['name']), tblOff=intern(tbl),
                disp=kind, dA=dA, dB=dB,
                unitOff=intern(unit), enumOff=intern(enums) if enums else 0))

    # ---- emit ----
    out = []
    w = out.append
    out.extend(CPP_LINES)
    w('// AUTO-GENERATED by tools/gen_address_map.py — do not edit by hand.')
    w('// Source: data/parameters.json. Regenerate after editing that file.')
    w('#pragma once')
    w('#include <cstdint>')
    w('namespace mu100 {')
    w('')
    w('enum class Enc : uint8_t { SINGLE = 0, BYTEPAIR = 1, NIBBLE = 2 };')
    w('')
    w('// How a raw byte READS on screen, compiled from the manual\'s own Description')
    w('// column (parameters.json "display_raw"). Raw = show the byte; it is the')
    w('// fallback wherever the description was blank, non-affine (the -inf..+6 dB')
    w('// volume curve), or an effect-list reference (those go through xgfmt instead).')
    w('// A wrong unit would be worse than a bare number, so nothing is guessed.')
    w('enum class Disp : uint8_t {')
    w('    Raw = 0,    // show the byte as-is')
    w('    Offset,     // dispA added to the byte            ("1...128")')
    w('    Centred,    // byte - dispA, signed               ("-64...0...+63")')
    w('    Scaled,     // (byte - dispA) * dispB / halfSpan  ("-100...0...+100 [%]")')
    w('    Tenths,     // (byte - dispA) / 10, one decimal   ("0.1...12.0")')
    w('    Enum,       // enumOff list, indexed from dispA   ("OFF, ON")')
    w('    Pan,        // L63 / C / R63 about dispA')
    w('    PanRnd,     // as Pan, but the low rail reads RND')
    w('    Note,       // C-2...G8, note 0 at dispA')
    w('    EqFreq      // the XG EQ frequency ladder (mu100fx::kConvTables[3])')
    w('};')
    w('')
    w('struct AddrByte { uint8_t base; uint8_t mask; }; // resolved = base|(inst&mask)')
    w('struct ParamDef {')
    w('    uint8_t  model;          // ModelIx 0=XG 1=Native1 2=Native2')
    w('    AddrByte hi, mid, lo;')
    w('    uint8_t  size;')
    w('    Enc      enc;')
    w('    bool     multiTemplate;  // >1 templated byte (perf tables); resolve(inst) fills all')
    w('    uint8_t  rangeLo, rangeHi;')
    w('    uint16_t nameOff;        // index into kStr')
    w('    uint16_t tableOff;       // index into kStr')
    w('    Disp     disp;           // how the value reads on screen')
    w('    int16_t  dispA, dispB;   // meaning per Disp, above')
    w('    uint16_t unitOff;        // index into kStr: " dB", " cent", "%", ... ("" = none)')
    w('    uint16_t enumOff;        // index into kStr: \'|\'-joined option list (0 = none)')
    w('};')
    w('')
    def cstr(s):
        return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
    w('static const char* const kStr[] = {')
    line = '   '
    for s in pool_list:
        tok = cstr(s) + ','
        if len(line) + len(tok) + 1 > 116:
            w(line); line = '   '
        line += ' ' + tok
    if line.strip(): w(line)
    w('};')
    w('')
    w('static const ParamDef kParams[] = {')
    for r in rows:
        def ab(b): return '{0x%02X,0x%02X}' % (b[0], b[1])
        w('  {%d,%s,%s,%s,%d,Enc::%s,%s,0x%02X,0x%02X,%d,%d,Disp::%s,%d,%d,%d,%d}, // %s'
          % (r['model'], ab(r['hi']), ab(r['mid']), ab(r['lo']), r['size'],
             r['enc'], 'true' if r['multi'] else 'false', r['rlo'], r['rhi'],
             r['nameOff'], r['tblOff'], r['disp'], r['dA'], r['dB'],
             r['unitOff'], r['enumOff'], r['name'][:38]))
    w('};')
    w('static const int kParamCount = %d;' % len(rows))
    w('')
    w('} // namespace mu100')

    dst = os.path.join(ROOT, 'codec', 'MU100AddressMapData.h')
    # Explicit, not the platform default: on Windows that is cp1252, which wrote the
    # em dash below as 0x97 -- a byte that differs from what any other OS produces.
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('emitted', len(rows), 'params,', len(pool_list), 'strings ->', dst)
    print('skipped', skipped, '(incomplete address / non-editable tables)')
    raw = dispCount.get('Raw', 0)
    print('display rules:', len(rows) - raw, 'of', len(rows), 'formatted;', raw, 'fall back to raw')
    for k in sorted(dispCount, key=lambda k: -dispCount[k]):
        print('   %-8s %d' % (k, dispCount[k]))

if __name__ == '__main__':
    main()
