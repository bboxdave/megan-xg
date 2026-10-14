// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "EffectParametersPanel.h"
#include "EditorTextScale.h"
#include "EditorColours.h"
#include "XgKnob.h"
#include "EffectAddressing.h"
#include "PluginProcessor.h"
#include "XgEffectFormat.h"
#include "TempoSync.h"
#include "codec/MU100Effects.h"
#include "codec/MU100State.h"
#include "codec/MU100Tooltips.h"
#include <cmath>
#include <utility>

namespace
{
    // Power-on default effect-program index per unit (mirrors the typeForUnit
    // member initialiser): Reverb HALL 1, Chorus CHORUS 1, Variation DELAY LCR,
    // Insertion 1/2 DISTORTION. Drives the changed-from-default gutter tab.
    constexpr int kDefaultProgram[5] { 1, 1, 13, 33, 33 };

    // Tempo-sync spec keyed by effect LAYOUT id (codec/MU100Effects.h). Keying by
    // layout (not effect name) covers a syncable effect's Variation and Insertion
    // variants at once, and sidesteps a stray "RDEL3" label typo in the insertion
    // ECHO layout. synced[] = the slots that switch to note values (-1 ends). The
    // SYNC toggle itself is drawn in the top strip over the 5th grid column for
    // every syncable effect, so it needs no per-effect slot.
    struct FxSyncSpec { int layout; bool isHz; int synced[4]; };

    static const FxSyncSpec kFxSync[] = {
        // Delay / echo — delay-time slots in ms.
        {  8, false, {  0,  1,  2,  3 } },  // DELAY LCR  (Variation)
        {  9, false, {  0,  1,  2,  3 } },  // DELAY LR   (Variation)
        { 10, false, {  0,  2,  5,  6 } },  // ECHO       (Variation)
        { 11, false, {  0,  1, -1, -1 } },  // CROSS DELAY(Variation)
        { 42, false, {  0,  1,  2,  3 } },  // DELAY LCR  (Insertion)
        { 43, false, {  0,  1,  2,  3 } },  // DELAY LR   (Insertion)
        { 44, false, {  0,  2,  5,  6 } },  // ECHO       (Insertion)
        { 45, false, {  0,  1, -1, -1 } },  // CROSS DELAY(Insertion)
        { 38, false, {  0,  1,  2, -1 } },  // DIST+DELAY / ODRV+DELAY (L.DEL,R.DEL,FBD)
        { 39, false, {  0, -1, -1, -1 } },  // COMP+DIST+DLY / COMP+ODRV+DLY (DELAY)
        { 40, false, {  0, -1, -1, -1 } },  // WAH+DIST+DLY / WAH+ODRV+DLY (DELAY)
        // Pitch change — the I.DEL (initial delay) slot in ms. Layout 31 is used by
        // BOTH pitch-change programs (PITCH CHANGE1 + PITCH CHANGE2, Variation-only),
        // so this single entry covers them both. I.DEL spans 0.1..400.0ms (conv
        // table#7), so at ordinary tempi only the shorter notes are reachable — the
        // generic dim/skip-over rules handle the rest.
        { 31, false, {  1, -1, -1, -1 } },  // PITCH CHANGE1/2 (Variation)
        // Modulation — the LFO FREQ/SPEED slot in Hz.
        { 17, true,  {  0, -1, -1, -1 } },  // ROTARY SPKR   (Variation)
        { 18, true,  {  0, -1, -1, -1 } },  // 2WAYROT SPKR  (Variation)
        { 19, true,  {  0, -1, -1, -1 } },  // TREMELO       (Variation)
        { 20, true,  {  0, -1, -1, -1 } },  // AUTO PAN      (Variation)
        { 27, true,  {  0, -1, -1, -1 } },  // AUTO WAH      (Var + Ins)
        { 28, true,  {  0, -1, -1, -1 } },  // AUTOWAH+DIST/+ODRV
        { 49, true,  {  0, -1, -1, -1 } },  // ROTARY SPKR   (Insertion)
        { 50, true,  {  0, -1, -1, -1 } },  // TREMELO       (Insertion)
        { 51, true,  {  0, -1, -1, -1 } },  // AUTO PAN      (Insertion)
        // Flanger / phaser — the LFO FREQ slot in Hz (disp 22, same param as the
        // modulation effects above). Each of these layouts is used ONLY by the
        // named flanger/phaser programs, so keying by layout is exact.
        {  4, true,  {  0, -1, -1, -1 } },  // FLANGER 1/2/3 (Chorus + Variation)
        { 47, true,  {  0, -1, -1, -1 } },  // FLANGER 1/2/3 (Insertion)
        {  7, true,  {  0, -1, -1, -1 } },  // PHASER1       (Chorus + Var + Ins)
        { 21, true,  {  0, -1, -1, -1 } },  // PHASER2       (Variation)
    };

    const FxSyncSpec* fxSyncForLayout(int layout)
    {
        for (const auto& s : kFxSync) if (s.layout == layout) return &s;
        return nullptr;
    }
}

//==============================================================================
EffectParametersPanel::EffectParametersPanel(YamahaMU100AudioProcessor& proc)
    : audioProcessor(proc)
{
    title.setText("Effect Parameters", juce::dontSendNotification);
    title.setFont(edtext::font(edtext::Title, 15.f, juce::Font::bold));
    title.setJustificationType(juce::Justification::centredRight);
    title.setColour(juce::Label::textColourId, edcol::kTextTitle);
    addAndMakeVisible(title);

    nameBox.onSelect = [this](int idx) { selectType(idx, true); };
    nameBox.setPopupColumnWidth(140);   // fit long effect-program names at the larger text size
    nameBox.setShowChangedTab(true);    // default program per unit set in rebuildNameList()
    nameBox.setTooltip(mu100::tooltips::forKey("SEL|EFFECT PROGRAM"));
    addAndMakeVisible(nameBox);

    programLabel.setText("EFFECT PROGRAM", juce::dontSendNotification);
    programLabel.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
    programLabel.setJustificationType(juce::Justification::centredLeft);
    programLabel.setColour(juce::Label::textColourId, edcol::kTextLabel);
    addAndMakeVisible(programLabel);

    for (auto& k : knobs)
    {
        k = std::make_unique<XgKnob>();
        // Room for a horizontal override bar, granted to the whole 16-slot grid. The
        // slots a given effect leaves bare draw nothing into theirs, and the grid is
        // fixed-position, so the row cannot change height as effects are switched.
        k->setOverrideMarkable(true);
        addAndMakeVisible(*k);
    }

    for (int i = 0; i < 16; ++i) syncNote[i] = -1;

    // SYNC toggle — styled like the Part module's PORTA switch; only shown for a
    // tempo-syncable effect. Sits in the top strip, over the 5th grid column, with
    // the "SYNC" label to its right (placed in resized()).
    syncLabel.setText("SYNC", juce::dontSendNotification);
    syncLabel.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
    syncLabel.setJustificationType(juce::Justification::centredLeft);
    syncLabel.setColour(juce::Label::textColourId, edcol::kTextLabel);
    addChildComponent(syncLabel);
    syncToggle.onChange = [this](bool on) { onSyncToggled(on); };
    syncToggle.setTooltip(mu100::tooltips::forKey("EFFECT|SYNC"));
    syncToggle.setFillCell(true);   // larger circle (~value-box height), matches the PORTA toggle
    addChildComponent(syncToggle);

    seedDefaults();                  // model now holds each unit's power-on program
    setUnit(mu100fx::FX_REVERB);     // grid shows the reverb defaults immediately

    startTimerHz(10);                // poll the effective tempo (see timerCallback)
}

// Does the model already hold this unit's effect TYPE? A restored project, a loaded
// file and a device read-back all carry it; an empty model (a fresh instance, before
// seedEditorDefaults has run) does not. Used to decide whether seedDefaults may write.
bool EffectParametersPanel::unitTypeInModel(int u) const
{
    const auto     b  = fxaddr::block(u);
    const uint8_t  lo = fxaddr::typeLowMsb(u);
    const uint32_t a  = ((uint32_t) b.aH << 14) | ((uint32_t) b.aM << 7) | (uint32_t) lo;
    uint8_t msb, lsb;
    const auto& st = audioProcessor.getEditorState();
    return st.getByte(mu100::ModelIx::XG, a, msb)
        && st.getByte(mu100::ModelIx::XG, a + 1, lsb);
}

// Seed the editor model with every unit's power-on default program + that
// program's parameter defaults (from the XGEdit-mined tables, which match the
// MU100 factory state). Recorded into the model only — NOT transmitted: the
// device already holds these on power-up, and a later device read-back overrides
// them if a real patch is loaded. This makes the Effect Parameters grid show the
// correct defaults on first open without depending on read-back timing, and lets
// switching units restore the right values.
//
// ONLY seeds units the model does not already hold. This runs from the panel's
// CONSTRUCTOR, and on a DAW project reopen the panel is constructed AFTER
// setStateInformation has restored the saved patch — so seeding unconditionally
// overwrote the restored reverb / chorus / variation / insertion blocks with power-on
// defaults. Because it is a model-only write, nothing was transmitted: the device
// (booted from restored work RAM) went on playing the SAVED effects while every panel
// showed defaults, and only a File -> Read from Device recovered them. Multi EQ and the
// per-part sends were untouched because they live outside these five blocks, which is
// what made the loss look effect-specific rather than ordering-specific.
void EffectParametersPanel::seedDefaults()
{
    for (int u = 0; u <= 4; ++u)
    {
        int count = 0;
        const auto* types = mu100fx::unitTypes(u, count);
        if (count == 0) continue;
        if (unitTypeInModel(u)) continue;      // real state present — never seed over it
        const auto& t = types[juce::jlimit(0, count - 1, typeForUnit[u])];

        audioProcessor.recordParamChange(fxaddr::setType(u, t.msb, t.lsb));
        const auto& ly = mu100fx::kLayouts[t.layout];
        const bool isVar = (u == mu100fx::FX_VARIATION);
        const bool insMsb = fxaddr::insertionRequiresMsb(u, t);
        for (int slot = 0; slot < 16; ++slot)
            if (ly.slots[slot].label != nullptr && xgfmt::slotActive(t.layout, slot, isVar))
                audioProcessor.recordParamChange(fxaddr::setParam(u, slot, t.deflt[slot], insMsb));
    }
}

EffectParametersPanel::~EffectParametersPanel() { stopTimer(); }

//==============================================================================
void EffectParametersPanel::setUnit(int newUnit)
{
    unit = juce::jlimit(0, 4, newUnit);
    rebuildNameList();
}

void EffectParametersPanel::rebuildNameList()
{
    int count = 0;
    const auto* types = mu100fx::unitTypes(unit, count);

    juce::StringArray names;
    for (int i = 0; i < count; ++i) names.add(types[i].name);

    // Reverb (13) and Chorus (15) are short enough for a single column; the
    // Variation (71) and Insertion (44) lists get three columns so every option
    // is visible without scrolling.
    const bool wide = (unit == mu100fx::FX_VARIATION
                       || unit == mu100fx::FX_INSERTION1
                       || unit == mu100fx::FX_INSERTION2);
    nameBox.setItems(names, wide ? 3 : 1);

    // Gray out effect programs that act only on the A/D audio input and do nothing
    // on MIDI-triggered voices. VOICE CANCEL nulls a centre-panned vocal by L-R
    // subtraction of an external stereo source, so it has no meaning as a Variation
    // effect on a synth part. (TALKING MODULATOR is audio-oriented too but is a
    // self-contained formant filter that DOES process any routed signal, so it stays
    // selectable.) Disabling only blocks new user picks — a patch already on the
    // effect still displays it (setSelectedIndex is not blocked).
    juce::SortedSet<int> disabledPrograms;
    if (unit == mu100fx::FX_VARIATION)
        for (int i = 0; i < count; ++i)
            if (names[i] == "VOICE CANCEL")
                disabledPrograms.add(i);
    nameBox.setDisabledItems(disabledPrograms);

    const int sel = juce::jlimit(0, juce::jmax(0, count - 1), typeForUnit[unit]);
    nameBox.setSelectedIndex(sel, false);
    nameBox.setDefaultIndex(kDefaultProgram[juce::jlimit(0, 4, unit)]);
    layoutForCurrentType();
}

// transmitType=false on unit switch (just reflect the device's current effect);
// true when the user actually picks a new effect name.
void EffectParametersPanel::selectType(int typeIndex, bool transmitType)
{
    int count = 0;
    const auto* types = mu100fx::unitTypes(unit, count);
    if (count == 0) return;
    typeIndex = juce::jlimit(0, count - 1, typeIndex);
    typeForUnit[unit] = typeIndex;

    if (transmitType)
    {
        const auto& t = types[typeIndex];

        // ONE undo step, not eighteen. Picking a program is a single user action, but
        // it writes the type plus every used parameter slot's factory default plus the
        // SYNC state — so without a scope, undoing it would take seventeen clicks and
        // pass through sixteen incoherent half-states on the way.
        //
        // The gesture key additionally folds a RUN of picks on this same unit into
        // one step: scrolling the wheel over the program box steps through programs
        // one detent at a time, and each detent is a full type change, so without it
        // a flick of the wheel buries the stack. Picking from the pop-up list instead
        // seals the step (XgSelector), so deliberate clicks stay one step each.
        static const char* const kUnit[] = { "Reverb", "Chorus", "Variation",
                                             "Insertion 1", "Insertion 2" };
        YamahaMU100AudioProcessor::UndoScope undoStep(
            audioProcessor, juce::String("Effect type ")
                                + kUnit[juce::jlimit(0, 4, unit)] + " " + t.name,
            mu100undo::gestureMergeKey(mu100undo::GestureEffectType, unit));

        audioProcessor.sendParamChange(fxaddr::setType(unit, t.msb, t.lsb));

        // Picking an effect program loads its factory parameter defaults (the
        // per-program table mined from XGEdit95.exe). Push each used slot's
        // default to the device + editor model, so the grid shows the new
        // program's parameters immediately — matching XGEdit, no read-back.
        const auto& ly = mu100fx::kLayouts[t.layout];
        const bool isVar = (unit == mu100fx::FX_VARIATION);
        const bool insMsb = fxaddr::insertionRequiresMsb(unit, t);
        for (int slot = 0; slot < 16; ++slot)
            if (ly.slots[slot].label != nullptr && xgfmt::slotActive(t.layout, slot, isVar))
                audioProcessor.sendParamChange(fxaddr::setParam(unit, slot, t.deflt[slot], insMsb));

        // Picking a program loads its factory defaults; the tempo-SYNC toggle is a
        // per-program editor state whose default is OFF, so reset it too. Otherwise
        // a program previously synced would come back synced while its parameters
        // reset — inconsistent with every other edited value. (on=false clears the
        // stored entry; loadSyncState in layoutForCurrentType then reads back off.)
        audioProcessor.setEffectSync(unit, typeIndex, YamahaMU100AudioProcessor::SyncEntry{});
    }

    layoutForCurrentType();   // restores the grid from the (now-defaulted) model
}

// D/W applies only to insertion-routed effects (see header). Insertion 1/2 are
// always insertions; the Variation block is an insertion only when CON = INS
// (model byte 0; SYS = non-zero). Reverb/Chorus are never insertions.
bool EffectParametersPanel::dryWetActive() const
{
    if (unit == mu100fx::FX_INSERTION1 || unit == mu100fx::FX_INSERTION2) return true;
    if (unit != mu100fx::FX_VARIATION) return false;

    const auto c = fxaddr::controls(mu100fx::FX_VARIATION);
    if (c.connection < 0) return false;
    const auto b = fxaddr::block(mu100fx::FX_VARIATION);
    const uint32_t a = ((uint32_t) b.aH << 14) | ((uint32_t) b.aM << 7) | (uint32_t) c.connection;
    uint8_t con = 0;   // default = INS (insertion)
    audioProcessor.getEditorState().getByte(mu100::ModelIx::XG, a, con);
    return con == 0;   // INS only
}

void EffectParametersPanel::refreshActive()
{
    layoutForCurrentType();
}

void EffectParametersPanel::layoutForCurrentType()
{
    int count = 0;
    const auto* types = mu100fx::unitTypes(unit, count);
    if (count == 0) return;
    const auto& t  = types[juce::jlimit(0, count - 1, typeForUnit[unit])];
    const auto& ly = mu100fx::kLayouts[t.layout];
    const bool insMsb = fxaddr::insertionRequiresMsb(unit, t);

    // Resolve the tempo-sync spec + this (unit,type)'s stored SYNC state before the
    // grid loop, so synced slots and the SYNC toggle slot can be handled specially.
    curLayout = t.layout;
    curInsMsb = insMsb;
    lastTxRaw.clear();                 // dedup is per display session (slot meaning can change)
    resolveSyncSpec();
    loadSyncState();

    for (int slot = 0; slot < 16; ++slot)
    {
        auto& k = *knobs[(size_t) slot];
        k.setVisible(true);   // the SYNC toggle now lives in the top strip, not a grid slot

        const char* label = ly.slots[slot].label;
        const bool isVar = (unit == mu100fx::FX_VARIATION);

        // Tooltip by the slot-label abbreviation, disambiguated by effect program +
        // display-type where an abbreviation is overloaded (e.g. R.DEL = reverb
        // delay vs right-channel delay; DELAY = echo spacing in the COMP+*+DLY
        // effects). Cleared for blank slots; refreshed every rebuild (knobs reused).
        k.setTooltip(label != nullptr
                         ? juce::String(mu100::tooltips::forEffectSlot(t.name, label,
                                                                       ly.slots[slot].disp))
                         : juce::String());

        // Bare (blank disc, like XGEdit) when the layout has no label, the MU100
        // spec defines no parameter at this slot (XGEdit-exe layouts carry stray
        // labels in spec-blank slots), or the param is Variation-block-only and
        // this isn't the Variation unit (e.g. chorus mid-EQ in the Chorus block).
        if (label == nullptr || ! xgfmt::slotActive(t.layout, slot, isVar))
        {
            k.setLabel({});
            k.setBare(true);
            k.setValueQuiet(0);
            continue;
        }

        // D/W (dry/wet balance) only applies to insertion-routed effects; for the
        // system (send/return) units it bypasses the dry path, so show its label
        // but render the knob inert/grayed (matching XGEdit).
        if (juce::String(label) == "D/W" && ! dryWetActive())
        {
            k.setLabel(label);
            k.setBare(true);
            continue;
        }

        // Tempo-sync note mode: this delay/LFO slot shows note values, not ms/Hz.
        if (curSyncable && syncOn && slotIsSynced(slot))
        {
            configureSyncKnob(slot);
            continue;
        }

        k.setBare(false);
        k.setValueDimmed(false);
        k.setLabel(label);

        const bool pair = fxaddr::slotIsPair(unit, slot, insMsb);

        // Range + value formatter (Hz / dB / ms / % / signed / enum / pan) for
        // this (layout, slot) from the MU100 spec conversion data. Right-click
        // reset returns to this program's factory default. Falls back to a raw
        // 0..127 (or 0..16383 for byte-pair slots) range where the spec has no
        // conversion for the slot.
        xgfmt::applyEffectFormat(k, t.layout, slot, pair, t.deflt[slot], isVar);

        // Where an override bar or arc grows from: the raw value at which this slot
        // DISPLAYS zero, if it has one (pan's centre, a signed depth's rest point).
        // Derived from the spec conversion, so it cannot drift from the readout.
        double origin = 0.0;
        if (xgfmt::barOriginFor(t.layout, slot, isVar, origin)) k.setBarOrigin(origin);
        else                                                    k.clearBarOrigin();

        // Restore this slot's value from the editor model (an edit the user made,
        // or a device read-back), so it survives unit/effect switches.
        k.setValueQuiet((double) readSlotRaw(slot));

        // Throttled, not immediate: this fires on EVERY value change, and a dial is
        // ~2 px per step, so a drag can emit far more parameter changes per second
        // than 31250 baud carries. The model still updates on every change — only the
        // transmission is coalesced, and the value you stop on is always the one sent.
        // (The effect-TYPE change in selectType() stays immediate: it carries its
        // program's defaults and is order-sensitive against these.)
        const int u = unit, s = slot;
        k.onChange = [this, u, s, insMsb](double v)
        {
            audioProcessor.sendParamChangeThrottled(fxaddr::setParam(u, s, (int) v, insMsb));
        };
    }

    // SYNC toggle: shown only for a tempo-syncable effect, reflecting its state.
    syncToggle.setVisible(curSyncable);
    syncLabel.setVisible(curSyncable);
    if (curSyncable) syncToggle.setState(syncOn, false);

    resized();
    // The grid has just been re-pointed at an effect type; a type change moves no
    // override, so nothing else would clear a mark belonging to the outgoing layout.
    refreshMidiOverrides();
}

//==============================================================================
// Tempo-sync helpers.
bool EffectParametersPanel::slotIsSynced(int slot) const
{
    for (int i = 0; i < 4; ++i) if (curSyncedSlots[i] == slot) return true;
    return false;
}

void EffectParametersPanel::resolveSyncSpec()
{
    const FxSyncSpec* spec = fxSyncForLayout(curLayout);
    curSyncable = (spec != nullptr);
    for (int i = 0; i < 4; ++i) curSyncedSlots[i] = -1;
    if (! curSyncable) return;
    curSyncIsHz = spec->isHz;
    for (int i = 0; i < 4; ++i) curSyncedSlots[i] = spec->synced[i];
}

void EffectParametersPanel::loadSyncState()
{
    for (int i = 0; i < 16; ++i) syncNote[i] = -1;
    syncOn = false;
    if (! curSyncable) return;
    YamahaMU100AudioProcessor::SyncEntry e;
    if (audioProcessor.getEffectSync(unit, typeForUnit[unit], e))
    {
        syncOn = e.on;
        for (int i = 0; i < 16; ++i) syncNote[i] = e.slotNote[i];
    }
}

void EffectParametersPanel::persistSync()
{
    YamahaMU100AudioProcessor::SyncEntry e;
    e.on = syncOn;
    for (int i = 0; i < 16; ++i)
        e.slotNote[i] = (syncOn && slotIsSynced(i)) ? syncNote[i] : (int8_t) -1;
    audioProcessor.setEffectSync(unit, typeForUnit[unit], e);
}

int EffectParametersPanel::readSlotRaw(int slot) const
{
    const auto b  = fxaddr::block(unit);
    const int  lo = fxaddr::paramLow(unit, slot, curInsMsb);
    if (lo < 0) return 0;
    const bool pair = fxaddr::slotIsPair(unit, slot, curInsMsb);
    const auto& st = audioProcessor.getEditorState();
    const uint32_t a0 = ((uint32_t) b.aH << 14) | ((uint32_t) b.aM << 7) | (uint32_t) lo;
    uint8_t b0, b1;
    if (pair)
    {
        if (st.getByte(mu100::ModelIx::XG, a0, b0) && st.getByte(mu100::ModelIx::XG, a0 + 1, b1))
            return (b0 << 7) | b1;
        return 0;
    }
    if (st.getByte(mu100::ModelIx::XG, a0, b0)) return b0;
    return 0;
}

// The same slot read out of the MIDI-override shadow instead of the model: -1 where
// incoming MIDI has not moved it. The twin of readSlotRaw, address arithmetic and
// byte-pair handling included -- a 14-bit slot is only overridden when BOTH of its
// bytes are, or the combined value would mix a device byte with one of ours.
int EffectParametersPanel::readSlotOverride(int slot) const
{
    const auto b  = fxaddr::block(unit);
    const int  lo = fxaddr::paramLow(unit, slot, curInsMsb);
    if (lo < 0) return -1;
    const bool pair = fxaddr::slotIsPair(unit, slot, curInsMsb);
    const uint32_t a0 = ((uint32_t) b.aH << 14) | ((uint32_t) b.aM << 7) | (uint32_t) lo;
    uint8_t b0, b1;
    if (! audioProcessor.midiOverrideByteAt(mu100::ModelIx::XG, a0, b0)) return -1;
    if (! pair) return b0;
    if (! audioProcessor.midiOverrideByteAt(mu100::ModelIx::XG, a0 + 1, b1)) return -1;
    return (b0 << 7) | b1;
}

//==============================================================================
// The MIDI-override marks for the 16-slot grid. A slot the current effect does not
// use is BARE, and a bare knob draws no mark -- so clearing those is not strictly
// needed, but it is done anyway: a slot that becomes bare keeps whatever it was last
// told until something says otherwise.
//
// A TEMPO-SYNCED slot is deliberately left UNMARKED. In that mode the knob's domain
// is a note index while the shadow holds the raw ms or Hz the device was sent, so a
// mark drawn from it would be a position on the wrong scale -- and a note is not
// something a parameter change can express in the first place.
// A SLOT IS ALSO LEFT UNMARKED WHILE THE PROGRAM ITSELF IS OVERRIDDEN TO AN EFFECT
// WITH A DIFFERENT LAYOUT, which is the same rule one step up. A knob's range and its
// readout come from OUR program's conversion, and the mark positions the device's raw
// byte in that range -- but slot 1 is TIME in a reverb layout and FREQ in a chorus
// one, so across layouts the byte would be drawn at a meaningless position and spoken
// with the wrong unit. Two effects sharing a layout keep their marks, because there
// the slots do mean the same things; the program tab carries the story in the other
// case. See refreshTypeOverride, which decides it.
void EffectParametersPanel::refreshMidiOverrides()
{
    const bool show = audioProcessor.showMidiOverrides;

    // FIRST: it sets typeOverrideOtherLayout, which the loop below consults.
    refreshTypeOverride();

    for (int slot = 0; slot < 16; ++slot)
    {
        auto& k = *knobs[(size_t) slot];
        const bool synced = curSyncable && syncOn && slotIsSynced(slot);
        const bool mute   = synced || typeOverrideOtherLayout;
        const int  v = (show && ! mute) ? readSlotOverride(slot) : -1;
        k.setMidiOverride(v < 0 ? 0.0 : (double) v, v >= 0);
    }
}

// The effect PROGRAM box. This is the override with the most consequence in the
// editor and the least to show for itself: the type word selects the whole effect,
// so while it is overridden every knob above is labelled for YOUR program while the
// device is running another one entirely. A tab plus the device's program name in
// the tooltip is what there is room for; the knobs keep reporting your effect,
// because that is what they edit and what will be saved.
void EffectParametersPanel::refreshTypeOverride()
{
    typeOverrideOtherLayout = false;

    const bool show = audioProcessor.showMidiOverrides;
    const auto b  = fxaddr::block(unit);
    const uint32_t a0 = ((uint32_t) b.aH << 14) | ((uint32_t) b.aM << 7)
                      | (uint32_t) fxaddr::typeLowMsb(unit);

    // The type is a two-byte word and a message may carry either half, so EITHER byte
    // being overridden is an override -- the same any-of rule the Voice module applies
    // to bank MSB / bank LSB / program.
    uint8_t om = 0, ol = 0;
    const bool hasM = show && audioProcessor.midiOverrideByteAt(mu100::ModelIx::XG, a0, om);
    const bool hasL = show && audioProcessor.midiOverrideByteAt(mu100::ModelIx::XG, a0 + 1, ol);
    if (! hasM && ! hasL) { nameBox.setMidiOverride(false, {}); return; }

    // The half NOT overridden comes from the model: that is where the device still
    // agrees with us, so it is the right source for it.
    uint8_t mm = 0, ml = 0;
    const auto& st = audioProcessor.getEditorState();
    st.getByte(mu100::ModelIx::XG, a0,     mm);
    st.getByte(mu100::ModelIx::XG, a0 + 1, ml);
    const uint8_t msb = hasM ? om : mm;
    const uint8_t lsb = hasL ? ol : ml;

    // An unrecognised pair names nothing, and the tab still goes up -- "something is
    // different" is honest where a guess would not be.
    //
    // It also decides whether the SLOT marks above can be trusted. An unrecognised
    // pair means we cannot know the device's layout, which counts as different: the
    // safe direction is to suppress, because a mark at a wrong position is worse than
    // no mark at all.
    int count = 0;
    const auto* types = mu100fx::unitTypes(unit, count);
    juce::String nm;
    bool sameLayout = false;
    for (int i = 0; i < count; ++i)
        if (types[i].msb == msb && types[i].lsb == lsb)
        {
            nm = types[i].name;
            sameLayout = ((int) types[i].layout == curLayout);
            break;
        }
    typeOverrideOtherLayout = ! sameLayout;

    nameBox.setMidiOverride(true, nm);
}

// See SystemPanel::refreshMidiMarkStyle -- our resized() hands each knob the grid
// cell it already had, so a style change has to reach them directly.
void EffectParametersPanel::refreshMidiMarkStyle()
{
    for (auto& k : knobs) if (k) k->refreshMidiMarkStyle();
    resized();
    repaint();
}

// `isVariationUnit` is a parameter rather than a read of the member `unit`
// because reapplyUnitForTempo() walks EVERY unit, not just the displayed one —
// and a slot's range can differ per block (see xgfmt::rangeFor).
void EffectParametersPanel::slotDisplayRange(int layout, int slot, double& lo, double& hi,
                                             bool isVariationUnit) const
{
    const auto& sc = xgfmt::slotConv(layout, slot);
    const auto  rg = xgfmt::rangeFor(sc, isVariationUnit);
    if (sc.kind == mu100fx::CK_TABLE)
    {
        const auto& tb = mu100fx::kConvTables[juce::jlimit(0, mu100fx::kConvTableCount, (int) sc.table)];
        if (tb.values != nullptr && tb.count > 0)
        {
            const int i0 = juce::jlimit(0, (int) tb.count - 1, rg.lo);
            const int i1 = juce::jlimit(0, (int) tb.count - 1, rg.hi);
            lo = xgfmt::parseScaled(tb.values[i0]);
            hi = xgfmt::parseScaled(tb.values[i1]);
        }
        else { lo = 0.0; hi = 0.0; }
    }
    else   // CK_LINEAR (delay ms)
    {
        lo = rg.dispLo; hi = rg.dispHi;
    }
    if (lo > hi) std::swap(lo, hi);
}

void EffectParametersPanel::configureSyncKnob(int slot)
{
    auto& k = *knobs[(size_t) slot];
    k.setBare(false);
    k.setLabel(mu100fx::kLayouts[curLayout].slots[slot].label);
    k.setRange(0.0, (double) (temposync::kNoteCount - 1), 1.0);
    k.setDefaultValue(9.0);   // right-click reset -> 1/4
    k.setTextFromValue([](double v)
    {
        return juce::String(temposync::noteName((int) std::lround(v)));
    });
    k.setValueFromText([](const juce::String& tx) -> double
    {
        const auto s = tx.trim();
        for (int i = 0; i < temposync::kNoteCount; ++i)
            if (s.equalsIgnoreCase(temposync::noteName(i))) return (double) i;
        return 0.0;
    });

    // Seed the note: keep the stored selection, else derive from the model raw.
    const double bpm = audioProcessor.effectiveBpm();
    int idx = syncNote[slot];
    if (idx < 0)
    {
        const double phys = xgfmt::parseScaled(
            xgfmt::formatValue(curLayout, slot, (double) readSlotRaw(slot),
                               unit == mu100fx::FX_VARIATION));
        idx = temposync::nearestNoteToValue(phys, bpm, curSyncIsHz);
        syncNote[slot] = (int8_t) idx;
    }
    k.setValueQuiet((double) idx);

    double lo, hi; slotDisplayRange(curLayout, slot, lo, hi, unit == mu100fx::FX_VARIATION);
    k.setValueDimmed(! temposync::achievable(idx, bpm, curSyncIsHz, lo, hi));

    const int s = slot;
    k.onChange = [this, s](double v) { onSyncKnobTurned(s, (int) std::lround(v)); };
}

void EffectParametersPanel::onSyncKnobTurned(int slot, int idx)
{
    const double bpm = audioProcessor.effectiveBpm();
    double lo, hi; slotDisplayRange(curLayout, slot, lo, hi, unit == mu100fx::FX_VARIATION);

    // Skip-over: a manual turn can't rest on an unreachable note.
    int ach = temposync::nearestAchievable(idx, bpm, curSyncIsHz, lo, hi);
    if (ach < 0) ach = idx;
    if (ach != idx) { knobs[(size_t) slot]->setValueQuiet((double) ach); idx = ach; }

    syncNote[slot] = (int8_t) idx;
    knobs[(size_t) slot]->setValueDimmed(! temposync::achievable(idx, bpm, curSyncIsHz, lo, hi));

    const int rawI = (int) std::lround(xgfmt::parseValue(
        curLayout, slot, juce::String(temposync::noteToValue(idx, bpm, curSyncIsHz), 3),
        unit == mu100fx::FX_VARIATION));
    lastTxRaw[unit * 100 + slot] = rawI;
    audioProcessor.sendParamChange(fxaddr::setParam(unit, slot, rawI, curInsMsb));
    persistSync();
}

void EffectParametersPanel::onSyncToggled(bool on)
{
    if (! curSyncable) return;
    syncOn = on;
    const double bpm = audioProcessor.effectiveBpm();

    if (on)
    {
        // Seed each synced slot's note from its current model raw (nearest note).
        for (int i = 0; i < 4; ++i)
        {
            const int slot = curSyncedSlots[i];
            if (slot < 0) continue;
            const double phys = xgfmt::parseScaled(
                xgfmt::formatValue(curLayout, slot, (double) readSlotRaw(slot),
                               unit == mu100fx::FX_VARIATION));
            syncNote[slot] = (int8_t) temposync::nearestNoteToValue(phys, bpm, curSyncIsHz);
        }
    }

    persistSync();
    layoutForCurrentType();   // rebuild the grid into / out of note mode

    if (on)
    {
        // Snap the device to the seeded notes so it matches the displayed values.
        for (int i = 0; i < 4; ++i)
        {
            const int slot = curSyncedSlots[i];
            if (slot < 0) continue;
            const int idx = syncNote[slot];
            if (idx < 0) continue;
            const int rawI = (int) std::lround(xgfmt::parseValue(
                curLayout, slot, juce::String(temposync::noteToValue(idx, bpm, curSyncIsHz), 3),
                unit == mu100fx::FX_VARIATION));
            lastTxRaw[unit * 100 + slot] = rawI;
            audioProcessor.sendParamChange(fxaddr::setParam(unit, slot, rawI, curInsMsb));
        }
    }
}

void EffectParametersPanel::reapplyUnitForTempo(int u, double bpm, bool force)
{
    int count = 0;
    const auto* types = mu100fx::unitTypes(u, count);
    if (count == 0) return;
    const int type = juce::jlimit(0, count - 1, typeForUnit[u]);
    const auto& t = types[type];
    const FxSyncSpec* spec = fxSyncForLayout(t.layout);
    if (spec == nullptr) return;

    YamahaMU100AudioProcessor::SyncEntry e;
    if (! audioProcessor.getEffectSync(u, type, e) || ! e.on) return;
    const bool insMsb = fxaddr::insertionRequiresMsb(u, t);

    for (int i = 0; i < 4; ++i)
    {
        const int slot = spec->synced[i];
        if (slot < 0) continue;
        const int idx = e.slotNote[slot];
        if (idx < 0) continue;

        const int rawI = (int) std::lround(xgfmt::parseValue(
            t.layout, slot, juce::String(temposync::noteToValue(idx, bpm, spec->isHz), 3),
            u == mu100fx::FX_VARIATION));
        const int key = u * 100 + slot;
        const auto it = lastTxRaw.find(key);
        if (force || it == lastTxRaw.end() || it->second != rawI)
        {
            lastTxRaw[key] = rawI;
            audioProcessor.sendParamChange(fxaddr::setParam(u, slot, rawI, insMsb));
        }

        // Keep the on-screen knob's dim state current (displayed unit only).
        if (u == unit && slotIsSynced(slot))
        {
            double lo, hi; slotDisplayRange(t.layout, slot, lo, hi, u == mu100fx::FX_VARIATION);
            knobs[(size_t) slot]->setValueDimmed(! temposync::achievable(idx, bpm, spec->isHz, lo, hi));
        }
    }
}

void EffectParametersPanel::timerCallback()
{
    const double bpm = audioProcessor.effectiveBpm();
    if (std::abs(bpm - lastBpm) < 1e-6) return;
    lastBpm = bpm;
    // A tempo change re-imposes every unit's synced notes (keep note, clamp raw),
    // not just the displayed one.
    for (int u = 0; u <= 4; ++u) reapplyUnitForTempo(u, bpm, /*force*/ false);
}

//==============================================================================
// After a read-back: refresh every unit's selected effect type from the model
// (so switching units shows the device's current effect), then rebuild the
// current grid (which restores its parameter values from the model).
void EffectParametersPanel::applyState(const mu100::DeviceState& st)
{
    for (int u = 0; u <= 4; ++u)
    {
        const auto    b  = fxaddr::block(u);
        const uint8_t lo = fxaddr::typeLowMsb(u);
        const uint32_t a = ((uint32_t) b.aH << 14) | ((uint32_t) b.aM << 7) | (uint32_t) lo;
        uint8_t msb, lsb;
        if (! st.getByte(mu100::ModelIx::XG, a, msb)
            || ! st.getByte(mu100::ModelIx::XG, a + 1, lsb))
            continue;

        int count = 0;
        const auto* types = mu100fx::unitTypes(u, count);
        for (int i = 0; i < count; ++i)
            if (types[i].msb == msb && types[i].lsb == lsb) { typeForUnit[u] = i; break; }
    }
    rebuildNameList();   // re-selects the current unit's type + restores its params
}

//==============================================================================
void EffectParametersPanel::paint(juce::Graphics& g)
{
    g.setColour(edcol::kModuleBg);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.f);
    g.setColour(edcol::kModuleBorder);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.f, 1.f);
}

void EffectParametersPanel::resized()
{
    auto b = getLocalBounds().reduced(6);

    // Top strip: effect-name picker (left) + module title (right).
    auto top = b.removeFromTop(20);
    const int topY = top.getY(), topH = top.getHeight();   // keep for the SYNC toggle
    nameBox.setBounds(top.removeFromLeft(149).reduced(0, 1));   // +~1 char for the larger text
    title.setBounds(top.removeFromRight(170));
    programLabel.setBounds(top.removeFromLeft(120).reduced(4, 0));
    b.removeFromTop(4);

    // 16-knob grid spanning the full width.
    const int gridX = b.getX();
    const int cellW = b.getWidth() / 16;
    int x = gridX;
    for (auto& k : knobs)
    {
        k->setBounds(x, b.getY(), cellW, b.getHeight());
        x += cellW;
    }

    // SYNC lives in the top strip (not a grid slot) so it sits in the same place for
    // every syncable effect: the green-dot toggle centred over the 5th grid column,
    // with the "SYNC" label just to its right. Circle sized to ~the value-box height
    // (XgKnob valueH) — DotToggle::fillCell makes the drawn circle track that.
    if (curSyncable)
    {
        const int col5CentreX = gridX + 4 * cellW + cellW / 2;
        const int d = 16;   // ~value-box height
        syncToggle.setBounds(juce::Rectangle<int>(0, 0, d, d)
                                 .withCentre({ col5CentreX, topY + topH / 2 }));
        syncLabel.setBounds(syncToggle.getRight() + 3, topY, 46, topH);
    }
}
