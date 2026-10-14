// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "KeyNav.h"
#include <memory>
#include "XgSelector.h"

class YamahaMU100AudioProcessor;
class XgKnob;
namespace mu100 { class DeviceState; }

//==============================================================================
// MultiEqPanel — the XGEdit "Multi EQ" (Equaliser) module.
//
// The MU100's built-in 5-band master equaliser. A program list-box picks one of
// the five XG EQ macros (FLAT / JAZZ / POPS / ROCK / CLASSIC), and a compact
// per-band editor (BAND selector + GAIN / FREQ / Q / SHAPE knobs) edits the band
// chosen by the BAND knob — matching XGEdit, which shows one band at a time
// rather than all five at once.
//
// SHAPE (shelving / peaking) exists only on the low band (1) and high band (5);
// the three mid bands are always peaking, so SHAPE goes bare for them.
//
// These are GLOBAL parameters (MULTI EQ table, model XG, address 02 40 0n), not
// per-part, so there is no current-part to track.
//==============================================================================
class MultiEqPanel : public juce::Component,
                     public keynav::Group
{
public:

    // --- keyboard navigation (KeyNav.h) ---
    // Just a bag of standard controls: the generic reading-order collector
    // is exactly right, and it cannot go stale when this panel gains one.
    std::vector<keynav::Item> navItems() override { return keynav::collect(*this); }
    juce::String navGroupName() const override { return "Multi EQ"; }

    explicit MultiEqPanel(YamahaMU100AudioProcessor& proc);
    ~MultiEqPanel() override;

    // Phase 5 read-back: refresh the program box and the current band's controls
    // from device state. Sets quietly (no SysEx is sent back).
    void applyState(const mu100::DeviceState& st);

    // MIDI-override marks (MidiOverrideMarks.h). The band editor re-points at one band
    // at a time, so the marks follow the same switch the values do.
    void refreshMidiOverrides();
    void refreshMidiMarkStyle();

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void selectBand(int band1to5);   // re-point GAIN/FREQ/Q/SHAPE at the band
    void loadBandValues();           // pull the current band's values from the model
    void applyMacro(int macroIdx);   // push a type's factory band values to device + knobs

    YamahaMU100AudioProcessor& audioProcessor;
    int currentBand { 0 };           // 0..4

    juce::Label title;               // "Multi EQ"
    XgSelector  programBox;          // FLAT / JAZZ / POPS / ROCK / CLASSIC (EQ TYPE)

    std::unique_ptr<XgKnob> bandKnob;   // 1..5 (local selection, not a SysEx param)
    std::unique_ptr<XgKnob> gainKnob;   // EQ GAINn
    std::unique_ptr<XgKnob> freqKnob;   // EQ FREQUENCYn
    std::unique_ptr<XgKnob> qKnob;      // EQ Qn
    std::unique_ptr<XgKnob> shapeKnob;  // EQ SHAPEn (bands 1 & 5 only)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MultiEqPanel)
};
