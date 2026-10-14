// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "KeyNav.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include "XgSelector.h"
#include "DotToggle.h"

class YamahaMU100AudioProcessor;
class XgKnob;
namespace mu100 { class DeviceState; }

//==============================================================================
// EffectParametersPanel — the XGEdit "Effect Parameters" module.
//
// A 3-column "Effect Name" picker (the effect program within the current unit)
// over a fixed 16-knob grid. The knob layout follows XGEdit's per-effect label
// table (codec/MU100Effects.h, extracted from XGEdit95.exe): used slots show a
// labelled, wired knob; unused slots show a bare disc, exactly as XGEdit draws.
//
// The current effect UNIT is driven by the Effect module (setUnit); each unit
// remembers its last-selected effect name. Picking a name transmits the effect
// TYPE change; turning a knob transmits that parameter (see EffectAddressing.h).
//==============================================================================
class EffectParametersPanel : public juce::Component,
                              public  keynav::Group,
                              private juce::Timer
{
public:

    // --- keyboard navigation (KeyNav.h) ---
    // Just a bag of standard controls: the generic reading-order collector
    // is exactly right, and it cannot go stale when this panel gains one.
    std::vector<keynav::Item> navItems() override { return keynav::collect(*this); }
    juce::String navGroupName() const override { return "Effect Parameters"; }

    explicit EffectParametersPanel(YamahaMU100AudioProcessor& proc);
    ~EffectParametersPanel() override;

    // Switch the unit being edited (Reverb/Chorus/Variation/Insertion 1/2).
    void setUnit(int unit);

    // After a device-state read-back: update each unit's selected effect type from
    // the model and restore the current grid's parameter values.
    void applyState(const mu100::DeviceState& st);

    // Rebuild the current grid in place (e.g. after the Variation CON routing
    // changes, which switches the D/W knob between active and inactive).
    void refreshActive();

    // MIDI-override marks (MidiOverrideMarks.h). Like the Effect module, this one
    // addresses the device POSITIONALLY, so the shadow is read by raw address.
    void refreshMidiOverrides();
    void refreshMidiMarkStyle();

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void rebuildNameList();
    void selectType(int typeIndex, bool transmitType);
    void layoutForCurrentType();
    void seedDefaults();   // populate the model with power-on defaults (see .cpp)
    bool unitTypeInModel(int unit) const;   // guards seedDefaults — see .cpp

    // True when the D/W (dry/wet) knob applies to the current unit: only for
    // insertion-routed effects — Insertion 1/2, or the Variation block when it is
    // connected as an insertion (CON = INS). System (send/return) effects (Reverb,
    // Chorus, Variation in SYS mode) bypass the dry path, so XGEdit grays D/W out.
    bool dryWetActive() const;

    // --- Tempo sync (plugin-only editor feature) ---------------------------------
    // Certain delay/echo/modulation effects can lock a delay-time / LFO-rate
    // parameter to musical note values at the project (hosted) or TEMPO-knob
    // (standalone) tempo. A SYNC DotToggle (styled like PORTA) sits in the top strip
    // over the 5th grid column, the same place for every syncable effect; when on,
    // the synced knobs switch to a note-index domain and display note names, while
    // the device still receives the resulting raw ms/Hz. See TempoSync.h and the
    // sync-spec table in the .cpp.
    void timerCallback() override;                 // polls the effective tempo
    void resolveSyncSpec();                        // fill cur* fields for the active type
    void loadSyncState();                          // pull SYNC on/notes from the processor
    void persistSync();                            // push SYNC on/notes to the processor
    void onSyncToggled(bool on);
    void configureSyncKnob(int slot);              // put a slot into note-index mode
    void onSyncKnobTurned(int slot, int noteIdx);  // user turned a synced knob (skip-over)
    void reapplyUnitForTempo(int u, double bpm, bool force);  // retransmit a unit's synced raws
    int  readSlotRaw(int slot) const;              // current raw value from the model
    int  readSlotOverride(int slot) const;         // ...or -1 where MIDI has not moved it
    void refreshTypeOverride();                    // the PROGRAM box's own mark
    void slotDisplayRange(int layout, int slot, double& lo, double& hi,
                          bool isVariationUnit) const;   // [min,max] ms/Hz
    bool slotIsSynced(int slot) const;

    YamahaMU100AudioProcessor& audioProcessor;

    int unit { 0 };
    // Default effect-name index per unit = the MU100's power-on programs (from the
    // factory dump): Reverb HALL 1, Chorus CHORUS 1, Variation DELAY LCR,
    // Insertion 1/2 DISTORTION.
    int typeForUnit[5] { 1, 1, 13, 33, 33 };

    juce::Label title;        // "Effect Parameters"
    XgSelector  nameBox;      // effect-name picker (3 columns)
    juce::Label programLabel; // "EFFECT PROGRAM" — sits just right of the name picker
    std::array<std::unique_ptr<XgKnob>, 16> knobs;

    DotToggle   syncToggle;   // effect tempo-SYNC switch (occupies a blank slot)
    juce::Label syncLabel;    // "SYNC"

    // Resolved sync spec for the currently displayed effect type (set by
    // resolveSyncSpec). curSyncable=false when the effect can't tempo-sync.
    // Set by refreshTypeOverride: the program is overridden to an effect whose slot
    // LAYOUT is not the one on screen, so the sixteen slots do not mean the same
    // things and their marks are suppressed. See refreshMidiOverrides.
    bool typeOverrideOtherLayout { false };

    bool curSyncable   { false };
    bool curSyncIsHz   { false };       // true = LFO Hz, false = delay ms
    int  curLayout     { 0 };
    bool curInsMsb     { false };
    int  curSyncedSlots[4] { -1, -1, -1, -1 };

    bool   syncOn { false };            // SYNC state of the current (unit,type)
    int8_t syncNote[16];                // note index per synced slot (-1 = not synced)
    std::map<int, int> lastTxRaw;       // last raw transmitted per (unit*100+slot), for dedup
    double lastBpm { -1.0 };            // tempo-change detector for the timer

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EffectParametersPanel)
};
