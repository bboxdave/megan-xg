// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "KeyNav.h"
#include "ValueBox.h"
#include <memory>

class YamahaMU100AudioProcessor;
class XgKnob;
namespace mu100 { class DeviceState; }

//==============================================================================
// SystemPanel — the XGEdit "System" module, trimmed to the four parameters this
// project needs: VOL (Master Volume), ATT (Master Attenuator), TUNE (Master
// Tune) and KSHIFT (Transpose). All four are GLOBAL XG SYSTEM parameters (model
// XG, address 00 00 0n), so there is no per-part state to track.
//
// To their right sits a TEMPO knob (10.00–250.00 BPM) used by the effect
// tempo-sync feature: authoritative when standalone, and a read-only live
// display of the DAW tempo when hosted. (PANIC moved to the bottom strip.)
//==============================================================================
class SystemPanel : public juce::Component,
                    public  keynav::Group,
                    private juce::Timer
{
public:

    // --- keyboard navigation (KeyNav.h) ---
    // Just a bag of standard controls: the generic reading-order collector
    // is exactly right, and it cannot go stale when this panel gains one.
    std::vector<keynav::Item> navItems() override { return keynav::collect(*this); }
    juce::String navGroupName() const override { return "System"; }

    explicit SystemPanel(YamahaMU100AudioProcessor& proc);
    ~SystemPanel() override;

    // Phase 5 read-back: refresh the four knobs from device state (no SysEx sent).
    void applyState(const mu100::DeviceState& st);

    // Push the effective BPM into the TEMPO knob's display. In hosted mode the
    // knob is read-only and this shows the live DAW tempo; the editor calls it
    // from its poll. Fired only when the value actually changes.
    void setDisplayedTempo(double bpm);
    // True when the TEMPO knob is user-editable (standalone). The editor uses it
    // to decide whether a tempo change should be pushed to synced effect params.
    bool tempoIsUserEditable() const { return tempoEditable; }

    // Two-way sync with the Graphic Display's Volume "S" bar (same MASTER VOLUME
    // parameter). onMasterVolumeChanged fires when the VOL knob is edited;
    // mirrorMasterVolume pushes a bar edit back into the knob quietly (no SysEx).
    std::function<void(int value)> onMasterVolumeChanged;
    void mirrorMasterVolume(int value);

    // The same pair for the system HPF, whose other face is the HPF2 screen's "S" bar.
    // onSystemHpfChanged fires when the box is edited here; mirrorSystemHpf pushes a
    // bar edit back into the box quietly (the processor already has the value).
    std::function<void(int hz)> onSystemHpfChanged;
    void mirrorSystemHpf(int hz);

    // "Disable plugin-only parameters": greys the system HPF box, which is the only
    // control in this module with no XG address. Value untouched -- the option blocks
    // EDITING, it neutralises nothing (same contract as the Part module's).
    void setPluginOnlyLocked(bool locked);

    // "Mark DAW-automatable params" (the Options dropdown; XgKnob::AutoMarker
    // values). Of this module's five knobs VOL, ATT and KSHIFT have host
    // parameters (VOLMST / ATTMST / KSHIFMST); TUNE has none, and TEMPO is a
    // plugin-side convenience that never reaches the device at all. Which is
    // which is asked of AutomationParams, not listed here.
    void setAutomationMarkers(int marker);

    // MIDI-override marks (MidiOverrideMarks.h). refreshMidiOverrides re-reads the
    // shadow and moves each knob's mark; refreshMidiMarkStyle re-lays the module out
    // after the Display Animation style changed, which the knobs cannot be told by a
    // setBounds that hands them the rectangle they already had.
    void refreshMidiOverrides();
    void refreshMidiMarkStyle();

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void loadValues();   // pull the four params' values from the editor model
    void timerCallback() override;   // hosted: mirror the live DAW tempo into TEMPO

    YamahaMU100AudioProcessor& audioProcessor;

    juce::Label title;          // "System"
    // System HPF: a plugin-only FLOOR under every part's HPF2 cutoff. A ValueBox, not a
    // knob -- the five-knob row below is at its width limit, the module's title row is
    // empty on the left, and a box is what lets a floor be TYPED ("40") rather than
    // dialled. Label to its right, so the number sits at the module's corner where the
    // eye lands first.
    std::unique_ptr<ValueBox> hpfBox;
    juce::Label               hpfLabel;   // "HPF"
    bool pluginOnlyLocked { false };

    std::unique_ptr<XgKnob> volKnob;     // MASTER VOLUME
    std::unique_ptr<XgKnob> attKnob;     // MASTER ATTENUATOR
    std::unique_ptr<XgKnob> tuneKnob;    // MASTER TUNE
    std::unique_ptr<XgKnob> kshiftKnob;  // TRANSPOSE
    std::unique_ptr<XgKnob> tempoKnob;   // TEMPO (effect tempo-sync)

    bool tempoEditable { true };         // standalone = editable; hosted = read-only

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SystemPanel)
};
