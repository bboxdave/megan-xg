// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include <cmath>

//==============================================================================
// freqladder — the cutoff resolution shared by the plugin-only filters: HPF2 and
// the Part-EQ peaking bands 2-4. Three significant digits across the board:
//
//     every 1 Hz    20 .. 1000        ("20", "437", "1.00k")
//     every 10 Hz   1.01k .. 10.0k    ("1.01k", "4.00k")
//     every 100 Hz  10.1k .. 16.0k    ("10.1k", "16.0k")
//
// Each control clamps the ladder to its own range — HPF2 to 20..1000 (with 0 =
// Off below the floor), the EQ mid bands to 32..16000. The hardware BASS/TREBLE
// shelves are NOT on this ladder: they stay on the coarse XG EQ-frequency table,
// which is all the firmware accepts.
//
// Every entry of that XG table between 20 Hz and 16 kHz is itself a ladder value,
// so the mouse wheel (which still steps the table, entry by entry) always lands
// on the ladder and snapping never nudges it.
//==============================================================================
namespace freqladder
{
    constexpr int kMin = 20, kMax = 16000;

    // Ladder index layout: [0,980] = 20..1000, [981,1880] = 1010..10000,
    // [1881,1940] = 10100..16000.
    constexpr int kSegB  = 981;
    constexpr int kSegC  = 1881;
    constexpr int kCount = 1941;

    // A plain drag is logarithmic, so every octave gets the same travel: the full
    // 20 Hz..16 kHz span (~9.6 octaves) fits in a ~270 px drag.
    constexpr double kPixelsPerOctave  = 28.0;
    // Shift-drag steps the ladder one entry at a time instead.
    constexpr double kPixelsPerFineStep = 4.0;

    // Nearest ladder value to `hz`. Each segment proposes its own rounding and the
    // closest wins, so the segment joins behave (1004 -> 1000, 1006 -> 1010).
    inline int snap(double hz)
    {
        auto seg = [hz](int lo, int hi, int step)
        {
            return juce::jlimit(lo, hi, (int) std::lround(hz / step) * step);
        };
        const int cand[3] = { seg(kMin, 1000, 1), seg(1010, 10000, 10), seg(10100, kMax, 100) };
        int best = cand[0];
        for (int i = 1; i < 3; ++i)
            if (std::abs(cand[i] - hz) < std::abs(best - hz)) best = cand[i];
        return best;
    }

    inline int valueAt(int index)
    {
        const int i = juce::jlimit(0, kCount - 1, index);
        if (i < kSegB) return kMin + i;
        if (i < kSegC) return 1010 + (i - kSegB) * 10;
        return 10100 + (i - kSegC) * 100;
    }

    inline int indexOf(double hz)
    {
        const int v = snap(hz);
        if (v <= 1000)  return v - kMin;
        if (v <= 10000) return kSegB + (v - 1010) / 10;
        return kSegC + (v - 10100) / 100;
    }

    // Three significant digits, with or without the "k" (see the header comment).
    inline juce::String text(double hz)
    {
        const int v = snap(hz);
        if (v < 1000)  return juce::String(v);
        if (v < 10000) return juce::String(v / 1000.0, 2) + "k";
        return juce::String(v / 1000.0, 1) + "k";
    }

    // Accepts "437", "1.01k", "1010", "10.1k" — the "k" is optional. A bare decimal
    // below the 20 Hz floor ("4.5", "1.01") can only sensibly mean kHz, so it is read
    // that way; whole numbers are never reinterpreted, so "20" stays 20 Hz.
    inline double parse(const juce::String& t)
    {
        const juce::String s = t.trim().toLowerCase();
        const bool hasK = s.contains("k");
        double hz = s.upToFirstOccurrenceOf("k", false, true)
                     .retainCharacters("0123456789.").getDoubleValue();
        if (hasK) hz *= 1000.0;
        else if (s.containsChar('.') && hz > 0.0 && hz < (double) kMin) hz *= 1000.0;
        return hz;
    }

    // Drag law for a ValueBox::dragValue hook. `dy` = pixels dragged (up = positive)
    // from `startHz`, the value the drag began on. Plain = log sweep; Shift = one
    // ladder entry per kPixelsPerFineStep. The caller clamps to its own range, and
    // the plain result is snapped by the box's snapValue hook.
    //
    // Returns < kMin when a Shift-drag walks off the bottom of the ladder, so a
    // control with an "off" position below the floor (HPF2) can reach it.
    inline double drag(double startHz, double dy, bool fine)
    {
        if (! fine) return startHz * std::pow(2.0, dy / kPixelsPerOctave);
        const int i = indexOf(startHz) + (int) std::lround(dy / kPixelsPerFineStep);
        return i < 0 ? 0.0 : (double) valueAt(i);
    }

    //==========================================================================
    // HPF2's rule set: the ladder's fine segment (any integer 20..1000) plus an Off
    // position (0) one notch below the floor. Lives here because three surfaces
    // share it — the Part module's HPF2 box and the Graphic Display's HPF2 bar and
    // EQ screens — and they must agree on where Off begins.
    //==========================================================================
    namespace hpf2
    {
        inline double snap(double v)
        {
            return v < (double) kMin ? 0.0 : (double) freqladder::snap(v);
        }

        // A plain drag lifts off at 20 Hz and sweeps up from there; dragging back under
        // the floor turns it off again (via snap). A drag that starts from Off stays off
        // on the way down.
        inline double drag(double start, double dy, bool fine)
        {
            if (start < (double) kMin && dy <= 0.0) return 0.0;
            return freqladder::drag(juce::jmax(start, (double) kMin), dy, fine);
        }

        // Wheel increments are unchanged from before the ladder: 10 Hz, Shift 1 Hz.
        // Wheeling up from Off lands on the 20 Hz floor rather than the old 10 Hz
        // (which displayed as "Off" anyway); wheeling below the floor turns it off.
        inline double wheel(double v, int dir, bool fine)
        {
            if (v < (double) kMin) return dir > 0 ? (double) kMin : 0.0;
            return v + dir * (fine ? 1.0 : 10.0);
        }
    }
}
