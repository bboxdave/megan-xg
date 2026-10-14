// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "KeyNav.h"
#include <memory>
#include <array>
#include "XgSelector.h"
#include "ControllerWheel.h"
#include "PartPanel.h"     // DotToggle

class YamahaMU100AudioProcessor;
class XgKnob;
namespace mu100 { class DeviceState; }

//==============================================================================
// ControllersPanel — XGEdit's "Controller section of the Effects Module".
//
// Edits how the currently-selected part responds to continuous-controller
// messages. A "Controller Name" list-box picks the controller source
// (Modulation / Pitch Bend / Channel After-Touch / Poly After-Touch /
// Controller 1 / Controller 2); the six knobs below it set that source's depth
// parameters for the part — PITCH, AMP, FILT (and the LFO PMOD / AMOD / FMOD
// depths). Two further list-boxes, Controller 1 and Controller 2, assign which
// MIDI CC number (0..95) drives the part's two assignable controllers (AC1/AC2).
//
// This single panel covers what the XGEdit layout draws as the "Modulation / LFO"
// group (the source selector + six knobs) and the "Controller" group
// (Controller 1 / Controller 2), since they are one logical module.
//
// All values live in the MULTI PART table, so the controls track the selected
// part (setCurrentPart) and reload on read-back (applyState).
//==============================================================================
class ControllersPanel : public juce::Component,
                         public keynav::Group
{
public:

    // --- keyboard navigation (KeyNav.h) ---
    // Just a bag of standard controls: the generic reading-order collector
    // is exactly right, and it cannot go stale when this panel gains one.
    std::vector<keynav::Item> navItems() override { return keynav::collect(*this); }
    juce::String navGroupName() const override { return "Controller"; }

    explicit ControllersPanel(YamahaMU100AudioProcessor& proc);
    ~ControllersPanel() override;

    void setCurrentPart(int part0to31);

    // Whether the part on screen holds a DRUM/SFX kit voice (bank MSB 126/127),
    // pushed by EditorView::refreshDrumContext. The device ignores FILTER MODURATION
    // DEPTH and AMPLITUDE MODURATION DEPTH on a drum part (sound_list.txt p46), so
    // the FMOD and AMOD knobs gray out — for every controller source, since the
    // exclusion is per part, not per source. Values are left alone (see PartPanel).
    void setDrumKitPart(bool isKit);

    // Phase 5 read-back: refresh every control for the current part from `st`.
    void applyState(const mu100::DeviceState& st);

    // MIDI-override marks (MidiOverrideMarks.h). The six depth knobs are MULTI PART
    // parameters, so the marks follow the selected part AND the selected controller
    // source -- the same two switches the values follow.
    void refreshMidiOverrides();
    void refreshMidiMarkStyle();

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void selectSource(int sourceIndex);   // re-point the six depth knobs
    void loadDepths();                     // pull the source's depths from the model
    void loadAssignments();                // pull AC1/AC2 CC numbers from the model
    void wireDepthKnob(int knobIx, int low);
    void updateDrumKitGating();            // gray FMOD/AMOD on a kit part
    void sendWheel();                      // transmit the wheel position for the source
    void afterEdit();                      // resend the wheel position if the LED is on

    YamahaMU100AudioProcessor& audioProcessor;
    int  currentPart   { 0 };
    int  currentSource { 0 };
    bool drumKitPart   { false };   // see setDrumKitPart

    // The controller wheel (manual §8.4.2): drag to send the source's continuous
    // controller; the small toggle (LED) under it re-transmits the wheel position
    // after any edit so the change is heard immediately. Position is cached per
    // part AND per source — live-only, not saved to disk or kept across reopen.
    ControllerWheel wheel;
    DotToggle       resendToggle;
    std::array<std::array<double, 6>, 32> wheelPos {};  // [part][source]

    juce::Label modTitle;     // "Controller" group title
    XgSelector  nameBox;      // Controller Name (the source list-box)

    // Six depth knobs, in XGEdit display order: PITCH AMP FILT PMOD AMOD FMOD.
    std::array<std::unique_ptr<XgKnob>, 6> depthKnobs;

    juce::Label ctrlTitle;    // "Controller"
    juce::Label ctrl1Label, ctrl2Label;
    XgSelector  ctrl1Box, ctrl2Box;   // AC1 / AC2 controller-number pickers (3-col)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ControllersPanel)
};
