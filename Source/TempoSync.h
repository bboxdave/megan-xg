// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <array>
#include <cmath>
#include <cstddef>

//==============================================================================
// TempoSync — the note-value ↔ time/frequency math shared by the tempo-sync
// feature of the Effect Parameters editor (delay times, LFO rates locked to the
// project/standalone tempo). Pure, JUCE-free, off-tree testable like FreqLadder.h.
//
// The device only ever sees raw ms/Hz values (see EffectParametersPanel); this
// header just converts a chosen musical note at a given BPM to the target
// physical value, which the effect-format layer then rounds to the nearest raw.
//==============================================================================
namespace temposync
{
    // A tempo-sync note division. `fraction` = its length as a fraction of a
    // whole note. T (triplet) = ×2/3, D (dotted) = ×3/2. The 17 values are in
    // strictly ascending duration order, matching the editor's readout order.
    struct Note { const char* name; double fraction; };

    inline constexpr int kNoteCount = 17;

    inline const std::array<Note, kNoteCount>& notes()
    {
        static const std::array<Note, kNoteCount> t = { {
            { "1/64",  1.0 / 64.0                 },
            { "1/32",  1.0 / 32.0                 },
            { "1/16T", (1.0 / 16.0) * (2.0 / 3.0) },
            { "1/16",  1.0 / 16.0                 },
            { "1/8T",  (1.0 / 8.0)  * (2.0 / 3.0) },
            { "1/16D", (1.0 / 16.0) * 1.5         },
            { "1/8",   1.0 / 8.0                  },
            { "1/4T",  (1.0 / 4.0)  * (2.0 / 3.0) },
            { "1/8D",  (1.0 / 8.0)  * 1.5         },
            { "1/4",   1.0 / 4.0                  },
            { "1/2T",  (1.0 / 2.0)  * (2.0 / 3.0) },
            { "1/4D",  (1.0 / 4.0)  * 1.5         },
            { "1/2",   1.0 / 2.0                  },
            { "1/1T",  1.0 * (2.0 / 3.0)          },
            { "1/2D",  (1.0 / 2.0)  * 1.5         },
            { "1/1",   1.0                        },
            { "1/1D",  1.5                        },
        } };
        return t;
    }

    inline const char* noteName(int idx)
    {
        return (idx >= 0 && idx < kNoteCount) ? notes()[(size_t) idx].name : "";
    }

    // A whole note lasts 4 beats and one beat = 60000/bpm ms, so a note of
    // `fraction` whole-notes = fraction * 240000 / bpm milliseconds.
    inline double noteToMs(int idx, double bpm)
    {
        if (idx < 0 || idx >= kNoteCount || bpm <= 0.0) return 0.0;
        return notes()[(size_t) idx].fraction * 240000.0 / bpm;
    }

    // LFO sync = one full cycle per note, so rate Hz = 1000 / period-ms.
    inline double noteToHz(int idx, double bpm)
    {
        const double ms = noteToMs(idx, bpm);
        return ms > 0.0 ? 1000.0 / ms : 0.0;
    }

    // The target physical quantity for a note: Hz for LFO-rate params, ms for
    // delay params.
    inline double noteToValue(int idx, double bpm, bool isHz)
    {
        return isHz ? noteToHz(idx, bpm) : noteToMs(idx, bpm);
    }

    // Whether a note's target value lies within the parameter's real display
    // range [dispLo, dispHi] (inclusive, with a small relative epsilon).
    inline bool achievable(int idx, double bpm, bool isHz, double dispLo, double dispHi)
    {
        const double v   = noteToValue(idx, bpm, isHz);
        const double eps = 1e-6 * (std::abs(dispHi) + 1.0);
        return v >= dispLo - eps && v <= dispHi + eps;
    }

    // Nearest achievable note index to `idx` (searching outward, preferring the
    // shorter side on a tie). Returns `idx` if it is itself achievable, else the
    // closest that is, or -1 if none is achievable at this tempo.
    inline int nearestAchievable(int idx, double bpm, bool isHz, double dispLo, double dispHi)
    {
        if (achievable(idx, bpm, isHz, dispLo, dispHi)) return idx;
        for (int d = 1; d < kNoteCount; ++d)
        {
            const int lo = idx - d, hi = idx + d;
            if (lo >= 0 && achievable(lo, bpm, isHz, dispLo, dispHi)) return lo;
            if (hi < kNoteCount && achievable(hi, bpm, isHz, dispLo, dispHi)) return hi;
        }
        return -1;
    }

    // Reverse map: pick the note index whose target value is closest to a given
    // physical value at this tempo. Used to seed a synced knob's note from a
    // device read-back / model value (which is stored as raw ms/Hz).
    inline int nearestNoteToValue(double value, double bpm, bool isHz)
    {
        int best = 0; double bestErr = 1e30;
        for (int i = 0; i < kNoteCount; ++i)
        {
            const double e = std::abs(noteToValue(i, bpm, isHz) - value);
            if (e < bestErr) { bestErr = e; best = i; }
        }
        return best;
    }
}
