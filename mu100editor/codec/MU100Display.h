// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// MU100Display.h — raw byte -> the text the user actually sees.
//
// Turns a ParamDef's compiled Disp rule (see MU100AddressMapData.h) into a string:
// raw 40 on PAN reads "L24", raw 76 on an EQ gain reads "+2.3 dB", raw 1 on a Rcv
// switch reads "ON". The rules come from the manual's own Description column via
// tools/gen_address_map.py, so this file holds the arithmetic and none of the data.
//
// WHY THIS EXISTS SEPARATELY from the panels: every panel already formats its own
// controls, but always from inside the class that built them (PartPanel::applyKind,
// MultiEqPanel's lambdas, GraphicDisplayPanel::eqReadout). None of that is reachable
// from the processor, which is where the undo history lives and where a tooltip has
// to be composed with the GUI possibly closed. This is the panel-independent path.
//
// Disp::Raw falls through to the bare number. That is deliberate: the manual leaves
// ~250 Description cells blank and describes some parameters with non-affine curves,
// and a plausible-looking WRONG unit is worse than an honest raw byte.
//
// JUCE-free, like the rest of codec/, so it can be unit-tested off-tree.

#pragma once
#include "MU100AddressMap.h"
#include "MU100EffectConv.h"     // the XG EQ frequency ladder (kConvTables[3])
#include <string>
#include <cstdio>
#include <cmath>

namespace mu100 {
namespace display {

inline std::string trimNum(double v, int decimals)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

inline std::string withSign(long v)
{
    return (v > 0 ? "+" : "") + std::to_string(v);
}

// Pull option `idx` out of a '|'-joined list. Returns false when the list is too short,
// so an out-of-range byte reads as a number rather than as the wrong option name.
inline bool pipeOption(const char* list, int idx, std::string& out)
{
    if (list == nullptr || idx < 0) return false;
    const char* p = list;
    for (int i = 0; i < idx; ++i)
    {
        while (*p && *p != '|') ++p;
        if (*p != '|') return false;
        ++p;
    }
    const char* e = p;
    while (*e && *e != '|') ++e;
    if (e == p) return false;
    out.assign(p, (size_t) (e - p));
    return true;
}

inline std::string noteName(int n)
{
    static const char* const kName[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    if (n < 0 || n > 127) return std::to_string(n);
    return std::string(kName[n % 12]) + std::to_string(n / 12 - 2);   // 60 = C3, MU100 convention
}

// The one entry point. `raw` is the decoded scalar (SINGLE byte, or the combined
// value for multi-byte params — those are all Disp::Raw today, so they print plainly).
inline std::string format(const ParamDef& p, int raw)
{
    const char* unit = kStr[p.unitOff];

    switch (p.disp)
    {
        case Disp::Offset:
            return std::to_string((long) raw + p.dispA) + unit;

        case Disp::Centred:
            return withSign((long) raw - p.dispA) + unit;

        case Disp::Scaled:
        {
            // A centred display span over a raw span at a different resolution, e.g.
            // -12..+12 dB or -100..+100 % across raw 0..127 — or the FILTER-control
            // depths' -9600..+9450 cent, where the span is ASYMMETRIC because 128
            // values cannot be symmetric about one of them. dispB is the LOW half's
            // magnitude in display units, so the arithmetic is the same either way.
            // `half` must be derived exactly as the generator did (count midpoint,
            // not span midpoint) or every reading lands a step out.
            const int half = ((int) p.rangeHi - (int) p.rangeLo + 1) / 2;
            if (half <= 0) break;
            const double v = (double) (raw - p.dispA) * p.dispB / half;
            // A narrow display span means one raw step is well under one unit, so
            // integers would throw the reading away: ±12 dB over 128 steps needs a
            // decimal, ±100 % over the same 128 does not.
            if (p.dispB > 0 && p.dispB <= 12)
                return (v > 0 ? "+" : "") + trimNum(v, 1) + unit;
            return withSign(std::lround(v)) + unit;
        }

        case Disp::Tenths:
        {
            const double v = (raw - p.dispA) / 10.0;
            // dispB = 1 marks the CENTRED form ("-12.8...0...+12.7"), where the sign
            // carries meaning and the control shows it. The plain "0.0...12.7" form
            // leaves dispB 0 and prints bare, as its own control does.
            return ((p.dispB != 0 && v > 0.0) ? "+" : "") + trimNum(v, 1) + unit;
        }

        case Disp::Enum:
        {
            std::string s;
            if (pipeOption(kStr[p.enumOff], raw - p.dispA, s)) return s;
            break;
        }

        case Disp::PanRnd:
            // The low rail is RANDOM, not hard left — the one place where the pan
            // formula and the pan DISPLAY disagree, so it cannot be folded into Pan.
            if (raw <= (int) p.rangeLo) return "RND";
            // fall through
        case Disp::Pan:
        {
            const int v = raw - p.dispA;
            if (v == 0) return "C";
            return (v < 0 ? "L" + std::to_string(-v) : "R" + std::to_string(v));
        }

        case Disp::Note:
            return noteName(raw - p.dispA);

        case Disp::EqFreq:
        {
            const auto& t = mu100fx::kConvTables[3];      // "EQ Frequency"
            if (raw >= 0 && raw < (int) t.count && t.values[raw] != nullptr)
                return std::string(t.values[raw]) + " Hz";
            break;
        }

        case Disp::Raw:
        default:
            break;
    }
    return std::to_string((long) raw);
}

} // namespace display
} // namespace mu100
