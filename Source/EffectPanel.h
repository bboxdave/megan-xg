// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "KeyNav.h"
#include <memory>
#include "EffectRoutingStrip.h"
#include "XgSelector.h"

class YamahaMU100AudioProcessor;
class XgKnob;
class ValueBox;
namespace mu100 { class DeviceState; }

//==============================================================================
// EffectPanel — the XGEdit "Effect" module.
//
// An "Effect Type" picker selects which effect UNIT is being edited
// (Reverb / Chorus / Variation / Insertion 1 / Insertion 2; wheel-scroll or a
// 1-column popup). Below it sit the unit-level controls — LEVEL (return), PAN,
// ToREV, ToCHR, CON (connection), PART, CTRL (control depth) — only those that
// the selected unit actually has are active (e.g. reverb has just LEVEL + PAN).
//
// Changing the unit fires onUnitChanged, which the EditorView routes to the
// Effect Parameters module so it shows that unit's effect + parameters.
//==============================================================================
class EffectPanel : public juce::Component,
                    public keynav::Group,
                    private juce::Timer
{
public:

    // --- keyboard navigation (KeyNav.h) ---
    // Just a bag of standard controls: the generic reading-order collector
    // is exactly right, and it cannot go stale when this panel gains one.
    std::vector<keynav::Item> navItems() override { return keynav::collect(*this); }
    juce::String navGroupName() const override { return "Effect"; }

    explicit EffectPanel(YamahaMU100AudioProcessor& proc);
    ~EffectPanel() override;

    std::function<void(int)> onUnitChanged;   // fired with the EffectUnit index
    std::function<void()>    onConnectionChanged;  // fired when Variation CON (INS/SYS) toggles
    int getUnit() const { return unit; }

    // ---- Two-way sync with the Graphic Display's "S" (system-return) bars ----
    // The return LEVEL knob (Reverb / Chorus, and Variation when CON = SYS) edits
    // the same parameter as the matching Graphic Display "S" bar.
    //   * onReturnLevelChanged  — fired (unit, level) when the LEVEL knob is edited.
    //   * mirrorReturnLevel      — push a bar edit back into the LEVEL knob quietly.
    //   * setVariationSystemLevel — transmit the Variation system return (the
    //        Variation "S" bar has no AddressMap path of its own, so the bar
    //        delegates its send here).
    //   * onVariationSystemStateChanged — fired (sysActive, level) when the
    //        Variation CON routing changes (or after a read-back), so the Graphic
    //        Display can enable/disable + seed its Variation "S" bar.
    std::function<void(int unit, int level)>        onReturnLevelChanged;
    std::function<void(bool sysActive, int level)>  onVariationSystemStateChanged;
    void mirrorReturnLevel(int unit, int value);
    void setVariationSystemLevel(int value);
    bool variationIsSystem() const;
    int  variationSystemLevel() const;

    // Re-read the current unit's controls from device state (after a read-back).
    void applyState(const mu100::DeviceState& st);

    // MIDI-override marks (MidiOverrideMarks.h). This module addresses the device
    // POSITIONALLY (EffectAddressing.h), so the marks are read out of the shadow by
    // raw address rather than by AddressMap index.
    void refreshMidiOverrides();
    void refreshMidiMarkStyle();

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void applyUnit();         // enable/wire the controls the current unit supports

    // ---- The routing strip (EffectRoutingStrip.h) ----
    // Rebuilt from the editor model on a POLL rather than from the edit paths.
    // The routing it shows moves from six places -- this panel's own PART knob and
    // CON box, an undo or redo, a preset/.mss load, a device read-back, and the
    // Effect Parameters module when a type change rewrites the block -- and a
    // readout wired to five of the six would be wrong in a way nobody would notice
    // until they trusted it. Polling four bytes at 10 Hz cannot go stale (the same
    // argument keynav::collect makes for rebuilding its item list every keystroke,
    // and EffectParametersPanel already runs a 10 Hz timer of its own).
    void timerCallback() override { refreshRouting(); }
    void refreshRouting();
    juce::String unitTypeName(int u) const;   // effect name from the model's type bytes
    void refreshConnState();  // (re)wire the CON-gated controls for the current routing
    void wireKnob(XgKnob& k, int low, int dflt);
    void wireLevelKnob(int low);   // wireKnob(level) + the onReturnLevelChanged hook
    void wirePartKnob(int low);
    void applyTooltips();     // hover text for the control cells, gated like the wiring
    bool modelByte(int low, int& out) const;   // read a control's stored value
    bool overrideByte(int low, int& out) const;   // ...and where incoming MIDI has moved it
    bool varByte(int low, int& out) const;     // read a Variation-block byte (any current unit)
    bool unitByte(int u, int low, int& out) const;   // read ANY unit's byte, whichever is shown

    YamahaMU100AudioProcessor& audioProcessor;
    int unit { 0 };

    juce::Label title;       // "Effect"
    XgSelector  typeBox;     // REVERB / CHORUS / VARIATION / INSERTION1 / INSERTION2
    juce::Label effectLabel; // "EFFECT" — sits just right of the type picker

    // Controls in XGEdit order: LEVEL, PAN, ToREV, ToCHR, [CON], PART, CTRL.
    std::unique_ptr<XgKnob> levelKnob, panKnob, toRevKnob, toChrKnob, partKnob, ctrlKnob;
    juce::Label conLabel;
    std::unique_ptr<ValueBox> conBox;    // INS / SYS  (variation only)

    EffectRoutingStrip routing;   // top strip: INS1/INS2/VAR part ownership

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EffectPanel)
};
