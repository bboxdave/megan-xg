#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Replace the garbled DRUM SETUP table in data/parameters.json with the correct
table transcribed from the MU100 MIDI spec (Table 1-11, PDF p50).

The PDF extractor lost the Drum Setup address form (High=3n drum-setup number
0-3, Mid=rr note 0D-5B) and misaligned the EQ rows. This rebuilds it cleanly:
high='3n', mid='rr', NOT USED offsets omitted (not editable)."""
import json, os

ROOT = os.path.dirname(os.path.dirname(__file__))

# (low, name, range_lo, range_hi, default_or_None, description)
ROWS = [
    ('00', 'PITCH COARSE',                     '00','7F','40', '-64...0...+63'),
    ('01', 'PITCH FINE',                       '00','7F','40', '-64...0...+63 [cent]'),
    ('02', 'LEVEL',                            '00','7F', None,'0...127 (depends on note)'),
    ('03', 'ALTERNATE GROUP',                  '00','7F', None,'OFF, 1...127 (depends on note)'),
    ('04', 'PAN',                              '00','7F', None,'RND, L63...C...R63 (depends on note)'),
    ('05', 'REVERB SEND',                      '00','7F', None,'0...127 (depends on note)'),
    ('06', 'CHORUS SEND',                      '00','7F', None,'0...127 (depends on note)'),
    ('07', 'VARIATION SEND',                   '00','7F','7F', '0...127'),
    ('08', 'KEY ASSIGN',                       '00','01','00', 'SINGLE, MULTI'),
    ('09', 'Rcv NOTE OFF',                     '00','01', None,'OFF, ON (depends on note)'),
    ('0A', 'Rcv NOTE ON',                      '00','01','01', 'OFF, ON'),
    ('0B', 'LOW PASS FILTER CUTOFF FREQUENCY', '00','7F','40', '-64...0...63'),
    ('0C', 'LOW PASS FILTER RESONANCE',        '00','7F','40', '-64...0...63'),
    ('0D', 'EG ATTACK RATE',                   '00','7F','40', '-64...0...63'),
    ('0E', 'EG DECAY1 RATE',                   '00','7F','40', '-64...0...63'),
    ('0F', 'EG DECAY2 RATE',                   '00','7F','40', '-64...0...63'),
    ('20', 'EQ BASS GAIN',                     '00','7F','40', '-12...+12 [dB]'),
    ('21', 'EQ TREBLE GAIN',                   '00','7F','40', '-12...+12 [dB]'),
    ('24', 'EQ BASS FREQUENCY',                '04','28','0C', '32...2.0k [Hz]'),
    ('25', 'EQ TREBLE FREQUENCY',              '1C','3A','36', '500...16.0k [Hz]'),
    ('50', 'HIGH PASS FILTER CUTOFF FREQUENCY','00','7F','40', '-64...0...63'),
    ('60', 'VELOCITY SENSE PITCH',             '30','50', None,'-16...0...16 (depends on note)'),
    ('61', 'VELOCITY SENSE LPF CUTOFF',        '30','50', None,'-16...0...16 (depends on note)'),
]

def main():
    path = os.path.join(ROOT, 'data', 'parameters.json')
    d = json.load(open(path, encoding='utf-8'))
    params = []
    for low, name, rlo, rhi, dflt, desc in ROWS:
        params.append({
            'name': name,
            'addr': {'high': '3n', 'mid': 'rr', 'low': low},  # n=drum setup 0-3, rr=note 0D-5B
            'size': 1,
            'range': {'lo': rlo, 'hi': rhi},
            'display_raw': desc,
            'default_raw': dflt,
            'default_src': ('spec' if dflt else None),
            'review': False,
        })
    for t in d['tables']:
        if t['table'] == 'DRUM SETUP':
            t['params'] = params
            t['param_count'] = len(params)
            t['note'] = 'Address High=3n (Drum Setup number 0-3), Mid=rr (note 0D-5B). NOT USED offsets omitted.'
            break
    json.dump(d, open(path, 'w', encoding='utf-8'), ensure_ascii=False, indent=2)
    print('rewrote DRUM SETUP:', len(params), 'params ->', path)

if __name__ == '__main__':
    main()
