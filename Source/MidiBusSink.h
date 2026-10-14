// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

// MidiBusSink — narrow, dependency-free interface that lets the (patched) JUCE
// VST3 wrapper hand a second MIDI input bus's events to the AudioProcessor.
//
// JUCE's processBlock() only delivers one merged MidiBuffer and discards the
// VST3 per-event busIndex. To expose the MU100's two physical MIDI IN jacks
// (MU100-A / MU100-B) as two DAW-visible input ports, the wrapper is patched
// (guarded by JucePlugin_MU100_DualMidiIn) to split events by busIndex: bus 0
// stays in the normal MidiBuffer, bus 1 is delivered here.
//
// Forward-declares juce::MidiBuffer only (passed by reference), so this header
// can be included from inside the JUCE module translation unit without pulling
// in JUCE include-order assumptions.

namespace juce { class MidiBuffer; }

struct MidiBusSink
{
    virtual ~MidiBusSink() = default;

    // Called on the audio thread from within the VST3 wrapper's process(),
    // immediately before the wrapper invokes processBlock() on the same thread.
    virtual void receivePortBMidi (const juce::MidiBuffer& portB) = 0;
};
