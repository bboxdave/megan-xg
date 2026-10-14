// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>

//==============================================================================
// Drum edit buffers (XGEdit "DRUMS1..DRUMS4").
//
// The MU100 has four editable Drum Setups (SysEx high byte 30/31/32/33 = setup
// 0..3). When a drum kit is allocated to one of these buffers it can be edited
// per-key in drum-edit mode; an unallocated kit is a plain (non-editable)
// program change. XGEdit marks each buffer with a colour and outlines the voice
// name of any part using that buffer in the matching colour. These helpers carry
// that shared colour scheme so the browser swatches and the Voice-list outlines
// agree.
//==============================================================================
namespace drumbuf
{
    static constexpr int kNumBuffers = 4;

    inline bool validBuffer(int buffer) { return buffer >= 0 && buffer < kNumBuffers; }

    // DRUMS1 red, DRUMS2 green, DRUMS3 blue, DRUMS4 magenta (per the XGEdit
    // Voice List screenshot).
    inline juce::Colour colour(int buffer)
    {
        static const juce::Colour c[kNumBuffers] = {
            juce::Colour(0xffe23b3b),   // DRUMS1
            juce::Colour(0xff3cc84a),   // DRUMS2
            juce::Colour(0xff3c7ce2),   // DRUMS3
            juce::Colour(0xffd23bd2)    // DRUMS4
        };
        return (buffer >= 0 && buffer < kNumBuffers) ? c[buffer]
                                                     : juce::Colours::transparentBlack;
    }

    inline const char* name(int buffer)
    {
        static const char* n[kNumBuffers] = { "DRUMS1", "DRUMS2", "DRUMS3", "DRUMS4" };
        return (buffer >= 0 && buffer < kNumBuffers) ? n[buffer] : "";
    }
}
