// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "EditorColours.h"
#include <cmath>

//==============================================================================
// Shared look + animation for the editor's incoming-MIDI indicators, so the
// Voice module's per-part tabs (PartListPanel) and the Keyboard module's
// SYS / FX / BULK lamps (KeyboardPanel) cannot drift apart in size, colour or
// timing. Both drain a processor activity accumulator on a ~33 fps timer,
// (re)arm a hold window for whatever received data this tick, then advance a
// fade envelope toward 1 while held / 0 when idle. The hold keeps a single
// event visible AND, because dense input keeps re-arming it, prevents flicker.
//==============================================================================
namespace midiind
{
    constexpr int   kW    = 5;      // indicator width (px) — the orange tab / lamp
    // Widened 4 -> 5 (2026-08-09): a 4 px tab was thin enough to be easy to miss,
    // and it alpha-fades on top of that. Geometry, so BOTH themes get it — the two
    // palettes differ in colour only. Every neighbour derives its own size from this
    // (PartListPanel's kIndReserve shortens the voice-name column, KeyboardPanel's
    // frameW grows the lamp frame), so nothing else needs touching.
    constexpr double kHoldMs = 90.0;   // min lit time for a single event
    constexpr float kRise = 0.45f;  // per-tick approach up   (slight fade-in)
    constexpr float kFall = 0.18f;  // per-tick approach down (smooth fade-out)
    constexpr int   kTickMs = 30;   // timer period (~33 fps)

    inline const juce::Colour& colour() { return edcol::kOrange; }  // graphic-display "0" orange

    // Advance one envelope toward its target. Returns true if the level moved
    // (i.e. the owner needs a repaint).
    inline bool advance(float& level, bool held)
    {
        const float target = held ? 1.0f : 0.0f;
        if (level == target) return false;
        const float rate = (target > level) ? kRise : kFall;
        float nv = level + (target - level) * rate;
        if (std::abs(target - nv) < 0.004f) nv = target;   // snap to the end
        level = nv;
        return true;
    }

    inline bool visible(float level) { return level > 0.004f; }
}
