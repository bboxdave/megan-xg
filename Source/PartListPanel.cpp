// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "PartListPanel.h"
#include "EditorTextScale.h"
#include "EditorColours.h"
#include "ValueBox.h"
#include "VoiceIndicators.h"
#include "MidiIndicator.h"
#include "DrumBuffer.h"
#include "PluginProcessor.h"
#include "codec/MU100AddressMap.h"
#include "codec/MU100Voices.h"
#include "VoiceNaming.h"          // full vs 8-character voice names
#include "codec/MU100Tooltips.h"
#include <cmath>

namespace
{
    // Per-part "MIDI in" indicator geometry. The tab's width, colour and fade
    // envelope come from midiind (shared with the Keyboard module's SYS/FX/BULK
    // lamps, so the two can't drift apart).
    // Right-to-left within the row: [border-clearance kIndInset][orange tab kIndW]
    // [gap kSepGap][separator kSepW][gap kSepGap][voice-name column …]. kIndReserve
    // is the whole strip the voice column (name text, selection highlight, drum
    // outline) must stay clear of; the separator sits at its left edge.
    constexpr int   kIndW       = midiind::kW;
    constexpr int   kIndInset   = 2;          // gap kept from the module's right border
    constexpr int   kSepGap     = 2;          // gap on each side of the column separator
    constexpr int   kSepW       = 1;          // separator thickness (matches the module border)
    constexpr int   kIndReserve = kIndInset + kIndW + kSepGap + kSepW + kSepGap;

    // The two ROW tabs' width. Derived from the value boxes' changed-tab token, so
    // the marks in this list and the marks on a control stay one family; rounded up
    // because a column is laid out in whole pixels.
    constexpr int   kRowOvrW    = (int) (edcol::kChangedTabW + 0.999f);
    constexpr int   kIndTickMs  = midiind::kTickMs;
    constexpr double kHoldMs    = midiind::kHoldMs;
    const juce::Colour& kSepColour = edcol::kModuleBorder;   // same as the Voice module border
}

//==============================================================================
// Resolve a part's bank/program to its display name + model + element count. The
// NAME follows Options -> Voice names (full or the hardware's 8-character form);
// model and elements are the same either way.
void PartListPanel::lookupVoice(int msb, int lsb, int prg0,
                                juce::String& name, int& model, int& elements)
{
    const int pgm = prg0 + 1;   // model stores 0-based program
    for (int i = 0; i < mu100::kVoiceCount; ++i)
    {
        const auto& v = mu100::kVoices[i];
        if (v.msb == msb && v.lsb == lsb && v.pgm == pgm)
        { name = voicenames::displayName(v); model = v.model; elements = v.elements; return; }
    }
    name = "--"; model = 0; elements = 1;
}

// Options -> Voice names changed: the row names are CACHED (partName), so they have
// to be re-resolved from the bank/program each row already holds. Nothing is sent
// and no other row state is touched -- this is a relabel, not a refresh.
void PartListPanel::refreshVoiceNames()
{
    for (int p = 0; p < 32; ++p)
        lookupVoice(partMsb[(size_t) p], partLsb[(size_t) p], partPrg[(size_t) p],
                    partName[(size_t) p], partModel[(size_t) p], partElem[(size_t) p]);
    repaint();
}

PartListPanel::PartListPanel(YamahaMU100AudioProcessor& proc) : audioProcessor(proc)
{
    using mu100::AddressMap;
    chParam      = AddressMap::find("Rcv CHANNEL",     "MULTI PART");
    reserveParam = AddressMap::find("ELEMENT RESERVE", "MULTI PART");
    bankMsbParam = AddressMap::find("BANK SELECT MSB", "MULTI PART");
    bankLsbParam = AddressMap::find("BANK SELECT LSB", "MULTI PART");
    programParam = AddressMap::find("PROGRAM NUMBER",  "MULTI PART");
    partModeParam = AddressMap::find("PART MODE",      "MULTI PART");

    for (int p = 0; p < 32; ++p)
    {
        partMsb[(size_t) p] = 0;
        partLsb[(size_t) p] = 0;
        partPrg[(size_t) p] = 0;                          // Grand Piano
        partBuffer[(size_t) p] = -1;
        lookupVoice(0, 0, 0, partName[(size_t) p], partModel[(size_t) p], partElem[(size_t) p]);

        // MIDI receive channel. The hardware raw values are discontiguous
        // (spec "00-1F, 7F"): 0..15 = A1..A16, 16..31 = B1..B16, and 0x7F = OFF
        // (part receives on no channel). The box uses a contiguous 0..32 domain
        // where the sentinel 32 stands for OFF (drawn just past +16, the highest
        // value) and maps to/from raw 0x7F at the device boundary below.
        auto ch = std::make_unique<ValueBox>();
        ch->setRange(0.0, 32.0, 1.0);
        ch->setDefaultValue(p);                           // part n -> channel n
        ch->setValueQuiet(p);
        ch->setShowChangedTab(false);                     // row-level marker instead
        ch->textFromValue = [](double dv) -> juce::String
        {
            const int v = (int) std::lround(dv);
            if (v >= 32) return "OFF";
            return v < 16 ? juce::String(v + 1) : "+" + juce::String(v - 15);
        };
        ch->valueFromText = [](const juce::String& t) -> double
        {
            const auto s = t.trim();
            if (s.containsIgnoreCase("off")) return 32.0;
            const int n = s.retainCharacters("0123456789").getIntValue();
            const int base = s.startsWithChar('+') ? 15 : -1;   // +1->16, 1->0
            return juce::jlimit(0.0, 32.0, (double) (base + n));
        };
        // OFF (sentinel 32) reads as an inactive part: black on grey, distinct
        // from the normal green-on-black MIDI-channel values.
        ch->valueColour = [](double v, juce::Colour& bg, juce::Colour& text)
        {
            if (v >= 32.0) { bg = edcol::kMeterFill; text = edcol::kMeterText; }
        };
        if (chParam >= 0)
            ch->onChange = [this, p](double v)
            {
                // Box sentinel 32 -> hardware OFF (0x7F); 0..31 pass through as-is.
                const uint32_t raw = (v >= 32.0) ? 0x7Fu : (uint32_t) v;
                audioProcessor.sendParamChange(mu100::AddressMap::makeParamChange(
                    mu100::AddressMap::param(chParam), p, raw));
            };
        ch->setTooltip(mu100::tooltips::forKey("VOICE|CH"));
        addAndMakeVisible(*ch);
        chBox[(size_t) p] = std::move(ch);

        // Element reserve: 0..64 (footer EL mirrors the selected part's value). The
        // power-on default is 2 for melodic parts but 0 for the rhythm parts (10/26);
        // take it from the factory-default state so the row-changed marker matches the
        // device (parts 10/26 no longer show a false "changed" tab).
        int elDef = 2;
        if (reserveParam >= 0)
        {
            const auto v = AddressMap::get(audioProcessor.factoryDefaults(),
                                           AddressMap::param(reserveParam), p);
            if (v.present) elDef = v.combined;
        }
        auto el = std::make_unique<ValueBox>();
        el->setRange(0.0, 64.0, 1.0);
        el->setDefaultValue((double) elDef);
        el->setValueQuiet((double) elDef);
        el->setShowChangedTab(false);                     // row-level marker instead
        if (reserveParam >= 0)
            el->onChange = [this, p](double v)
            {
                audioProcessor.sendParamChange(mu100::AddressMap::makeParamChange(
                    mu100::AddressMap::param(reserveParam), p, (uint32_t) v));
                updateElementBudget();      // recompute remaining + per-box caps
            };
        el->setTooltip(mu100::tooltips::forKey("VOICE|EL"));
        addAndMakeVisible(*el);
        elBox[(size_t) p] = std::move(el);
    }

    // MU100 defaults: part 10 is the rhythm channel (Standard Kit on DRUMS1) and
    // part 26 (port B's part 10) is Standard Kit on DRUMS3. Display-only — these
    // mirror the device's power-on state, so nothing is transmitted here.
    auto defaultDrumPart = [this](int part, int buffer)
    {
        partMsb[(size_t) part] = 127;   // Drum Kit bank
        partLsb[(size_t) part] = 0;
        partPrg[(size_t) part] = 0;     // Standard Kit
        partBuffer[(size_t) part] = buffer;
        lookupVoice(127, 0, 0, partName[(size_t) part],
                    partModel[(size_t) part], partElem[(size_t) part]);
    };
    defaultDrumPart(9,  0);   // part 10 -> DRUMS1
    defaultDrumPart(25, 2);   // part 26 -> DRUMS3

    // Snapshot the factory-default voice per part so a reassigned voice name can
    // be drawn in the changed-value yellow (Option A).
    partDefMsb = partMsb;
    partDefLsb = partLsb;
    partDefPrg = partPrg;

    updateElementBudget();
    buildCellProxies();

    refreshMidiIndicators();  // starts the fade timer if the option is on
}

// "MIDI input indicators" was switched, or this panel was just built. The timer
// exists only to fade these tabs, so with the option off there is nothing for it to
// do -- and unlike KeyboardPanel's timer, which also drives the played-note display
// and drum-key selection, this one has no second job to keep it honest. It cannot
// notice the option coming back on by itself once stopped, so the editor's own tick
// mirrors the flag and calls this. See PluginEditor::applyMidiIndicators().
void PartListPanel::refreshMidiIndicators()
{
    if (audioProcessor.midiInputIndicators)
    {
        if (! isTimerRunning())
            startTimer(kIndTickMs);
    }
    else if (isTimerRunning())
    {
        stopTimer();
    }
}

//==============================================================================
// Accessibility pillar 3. Mute, Solo and the voice name are PAINTED, so there is
// nothing here a screen reader could focus or read; each gets an invisible
// mouse-transparent stand-in (mu100a11y::CellProxy) that navItems() moves onto the
// cell it computes for the selected row.
//
// Names come from tooltips.csv like every other accessible name in the editor, with
// the same {n} part-number substitution getTooltip() does -- so "Part 12 Mute" is
// authored in one place and reaches the hover text, the accessible name and the
// manual together.
void PartListPanel::buildCellProxies()
{
    for (int solo = 0; solo < 2; ++solo)
    {
        auto& px = msProxy[solo];
        const bool isSolo = (solo != 0);
        px.setRole(juce::AccessibilityRole::toggleButton);
        px.title = [this, isSolo]
        {
            return juce::String(mu100::tooltips::forKey(isSolo ? "VOICE|SOLO" : "VOICE|MUTE"))
                       .replace("{n}", juce::String(selectedPart + 1));
        };
        // Checkable, so a screen reader says "on"/"off" rather than reading a number
        // -- the same choice DotToggle makes. There is no value interface for the
        // same reason: checked IS the value.
        px.checked = [this, isSolo]
        {
            const auto m = isSolo ? audioProcessor.getPartSoloMask()
                                  : audioProcessor.getPartMuteMask();
            return selectedPart >= 0 && ((m >> selectedPart) & 1) != 0;
        };
        px.onActivate = [this, isSolo]
        {
            if (selectedPart >= 0) handleMsClick(selectedPart, isSolo, false);
        };
        addChildComponent(px);   // shown only while the columns are (see navItems)
    }

    nameProxy.setRole(juce::AccessibilityRole::button);
    nameProxy.title = [this]
    {
        return juce::String(mu100::tooltips::forKey("VOICE|VOICE NAME"))
                   + " " + juce::String(selectedPart + 1);
    };
    // The voice name IS the value here, and it is what a user actually wants read
    // out; pressing opens the Voice List, exactly as clicking the name does.
    //
    // THE OVERRIDE RIDES THE VALUE, as it does on every other control (see
    // ValueBox::spokenText): the row's cyan tab says the DEVICE is playing something
    // else, and a tab is exactly the kind of mark that says nothing without sight.
    // Read live, so FocusValueWatcher announces it arriving under the user.
    nameProxy.value = [this]
    {
        if (selectedPart < 0) return juce::String();
        auto v = voiceName(selectedPart);
        if (((voiceOverrideMask >> selectedPart) & 1u) != 0)
            v += ", MIDI has the device on " + voiceOverrideName[(size_t) selectedPart];
        return v;
    };
    // The row's two summary tabs, in words -- they are the whole point of the row
    // markers and neither has any other spoken form. Description rather than value:
    // a screen reader reads it when asked, so it cannot crowd out the voice name on
    // every move, and neither tab is a thing that changes under the user.
    nameProxy.description = [this]
    {
        juce::StringArray parts;
        if (selectedPart >= 0)
        {
            if (((rowOverrideMask >> selectedPart) & 1u) != 0)
                parts.add("incoming MIDI has moved other settings on this part");
            if (((rowChangedMask >> selectedPart) & 1u) != 0)
                parts.add("this part has settings changed from the factory default "
                          "that this row does not show");
        }
        return parts.joinIntoString("; ");
    };
    nameProxy.onActivate = [this]
    {
        if (selectedPart >= 0 && onRequestVoiceBrowse)
            onRequestVoiceBrowse(selectedPart, partMsb[(size_t) selectedPart],
                                 partLsb[(size_t) selectedPart], partPrg[(size_t) selectedPart]);
    };
    addChildComponent(nameProxy);
}

//==============================================================================
// Element reserves draw from a fixed pool (64 on the MU100). The footer EL field
// shows the total remaining (pool - sum of all 32 reserves); each part's reserve
// box is capped so it can never push the remaining below zero.
void PartListPanel::updateElementBudget()
{
    int total = 0;
    for (auto& b : elBox) total += (int) b->getValue();
    for (auto& b : elBox)
    {
        const int v    = (int) b->getValue();
        const int maxP = juce::jmax(v, kTotalElements - (total - v));
        b->setRange(0.0, (double) maxP, 1.0);
    }
    repaint();   // footer "EL" = remaining
}

PartListPanel::~PartListPanel() = default;

//==============================================================================
// MIDI-in indicator animation. Runs ~33 fps: drain the processor's activity
// accumulators, (re)arm the hold window for any part that received data this
// tick, advance each part's fade envelope toward its target, and repaint just
// the indicator strip if anything moved. The hold window keeps a single event
// visible and, because dense input keeps re-arming it, prevents flicker.
void PartListPanel::timerCallback()
{
    const uint32_t chanMask = audioProcessor.takeMidiChanActivity();
    const uint32_t partMask = audioProcessor.takeMidiPartActivity();

    if (! audioProcessor.midiInputIndicators)
    {
        // Feature off: darken the column, then STOP -- there is nothing else this
        // timer does. (The drain above keeps stale bits from flashing when the option
        // is later re-enabled; refreshMidiIndicators() is what restarts us.) Reached
        // when the option is switched while we are mid-fade; the ordinary off path is
        // refreshMidiIndicators() itself, called from the editor's tick.
        bool any = false;
        for (int p = 0; p < 32; ++p)
        {
            if (midiLevel[(size_t) p] != 0.f) { midiLevel[(size_t) p] = 0.f; any = true; }
            midiHoldUntil[(size_t) p] = 0.0;
        }
        if (any) repaintIndicatorColumn();
        stopTimer();
        return;
    }

    const double now = juce::Time::getMillisecondCounterHiRes();

    // Arm the hold window for any part receiving data: channel-voice messages by
    // the part's Rcv channel (raw 0..31), SysEx by direct part index.
    for (int p = 0; p < 32; ++p)
    {
        const int  raw = (int) std::lround(chBox[(size_t) p]->getValue());
        const bool hit = (raw >= 0 && raw < 32 && (chanMask & (1u << raw)) != 0)
                      || ((partMask & (1u << p)) != 0);
        if (hit) midiHoldUntil[(size_t) p] = now + kHoldMs;
    }

    // Advance envelopes toward their targets (1 while held, else 0).
    bool changed = false;
    for (int p = 0; p < 32; ++p)
        changed |= midiind::advance(midiLevel[(size_t) p], now < midiHoldUntil[(size_t) p]);
    if (changed) repaintIndicatorColumn();
}

juce::Rectangle<int> PartListPanel::indicatorColumn() const
{
    // Full-height right strip covering every row's tab (and the selected-row
    // highlight beneath it), so a partial repaint stays correct.
    return { getWidth() - kIndReserve, 0, kIndReserve, getHeight() };
}

void PartListPanel::repaintIndicatorColumn()
{
    repaint(indicatorColumn());
}

//==============================================================================
juce::String PartListPanel::voiceName(int part) const
{
    return partName[(size_t) part];
}

void PartListPanel::getPartVoice(int part, int& msb, int& lsb, int& prg0) const
{
    const int p = juce::jlimit(0, 31, part);
    msb  = partMsb[(size_t) p];
    lsb  = partLsb[(size_t) p];
    prg0 = partPrg[(size_t) p];
}

void PartListPanel::setSelectedPart(int part0to31)
{
    const int p = juce::jlimit(0, 31, part0to31);
    if (p == selectedPart) return;
    selectedPart = p;
    refreshCellProxies();   // the three painted stops belong to the SELECTED row
    repaint();
}

void PartListPanel::setPartBuffer(int part, int buffer)
{
    if (part < 0 || part >= 32) return;
    partBuffer[(size_t) part] = buffer;
    repaint();
}

int PartListPanel::getPartBuffer(int part) const
{
    return (part >= 0 && part < 32) ? partBuffer[(size_t) part] : -1;
}

void PartListPanel::sendPartMode(int part, int mode)
{
    if (part < 0 || part >= 32 || partModeParam < 0) return;
    audioProcessor.sendParamChange(mu100::AddressMap::makeParamChange(
        mu100::AddressMap::param(partModeParam), part, (uint32_t) juce::jlimit(0, 5, mode)));
}

void PartListPanel::applyVoice(int part, int msb, int lsb, int program0, const juce::String& name)
{
    if (part < 0 || part >= 32) return;
    partMsb[(size_t) part]  = msb;
    partLsb[(size_t) part]  = lsb;
    partPrg[(size_t) part]  = program0;
    lookupVoice(msb, lsb, program0, partName[(size_t) part],
                partModel[(size_t) part], partElem[(size_t) part]);
    if (name.isNotEmpty()) partName[(size_t) part] = name;

    using mu100::AddressMap;
    auto send = [this, part](int paramIx, uint32_t value)
    {
        if (paramIx >= 0)
            audioProcessor.sendParamChange(AddressMap::makeParamChange(
                AddressMap::param(paramIx), part, value));
    };
    send(bankMsbParam, (uint32_t) msb);
    send(bankLsbParam, (uint32_t) lsb);
    send(programParam, (uint32_t) program0);
    repaint();
}


//==============================================================================
// Which rows are overridden, and what voice the DEVICE is on. Recomputed when the
// override shadow moves rather than per repaint: this list already repaints on
// every MIDI-activity tick, and the lookup walks 32 parts x 3 parameters.
//
// A row counts as overridden if ANY of bank MSB, bank LSB or program has been moved
// -- they arrive together as one program change but are three parameters, and one
// of them moving is already a different voice.
void PartListPanel::refreshMidiOverrides()
{
    const uint32_t was = voiceOverrideMask;
    voiceOverrideMask = 0;

    if (audioProcessor.showMidiOverrides && audioProcessor.midiOverrideTotal() > 0)
        for (int p = 0; p < 32; ++p)
        {
            const int msb = audioProcessor.midiOverrideValue(bankMsbParam, p);
            const int lsb = audioProcessor.midiOverrideValue(bankLsbParam, p);
            const int prg = audioProcessor.midiOverrideValue(programParam, p);
            if (msb < 0 && lsb < 0 && prg < 0) continue;

            voiceOverrideMask |= 1u << p;

            // An override may cover only part of the triple, so the halves it does
            // NOT cover come from the model -- that is where the device still agrees
            // with us, so it is the right source for them.
            juce::String nm; int model = 0, elem = 0;
            lookupVoice((uint8_t) (msb >= 0 ? msb : partMsb[(size_t) p]),
                        (uint8_t) (lsb >= 0 ? lsb : partLsb[(size_t) p]),
                        (uint8_t) (prg >= 0 ? prg : partPrg[(size_t) p]),
                        nm, model, elem);
            voiceOverrideName[(size_t) p] = nm;
        }

    // The per-part roll-up rides the same pass: it asks the same shadow, and two
    // refreshes on one trigger would only be two chances to forget one.
    const uint32_t wasRoll = rowOverrideMask;
    rowOverrideMask = 0;
    if (audioProcessor.showMidiOverrides)
        for (int p = 0; p < 32; ++p)
            if (audioProcessor.partOverriddenOutsideVoiceList(p))
                rowOverrideMask |= 1u << p;

    if (voiceOverrideMask != was || rowOverrideMask != wasRoll) repaint();
}

//==============================================================================
// Which rows have something changed from the factory default that the row itself
// does not show. Recomputed when the MODEL moves rather than per repaint -- the
// answer walks two address ranges per part, which is cheap once and not 32 times a
// second. See partChangedOutsideVoiceList for what counts.
void PartListPanel::refreshRowChanged()
{
    const uint32_t was = rowChangedMask;
    rowChangedMask = 0;
    for (int p = 0; p < 32; ++p)
        if (audioProcessor.partChangedOutsideVoiceList(p))
            rowChangedMask |= 1u << p;
    if (rowChangedMask != was) repaint();

}

void PartListPanel::applyState(const mu100::DeviceState& st)
{
    using mu100::AddressMap;

    auto getVal = [&st](int paramIx, int part, int& out) -> bool
    {
        if (paramIx < 0) return false;
        const auto v = AddressMap::get(st, AddressMap::param(paramIx), part);
        if (! v.present) return false;
        out = v.combined;
        return true;
    };

    for (int p = 0; p < 32; ++p)
    {
        int val = 0;
        if (getVal(chParam, p, val))
        {
            // Raw 0x7F = OFF -> box sentinel 32; 0..31 map straight through.
            const double boxv = (val == 0x7F) ? 32.0 : (double) juce::jlimit(0, 31, val);
            chBox[(size_t) p]->setValueQuiet(boxv);
        }
        if (getVal(reserveParam, p, val)) elBox[(size_t) p]->setValueQuiet((double) val);

        // Voice: rebuild from bank MSB/LSB + program if any of them are present.
        int msb = partMsb[(size_t) p], lsb = partLsb[(size_t) p], prg = partPrg[(size_t) p];
        bool gotVoice = false;
        gotVoice |= getVal(bankMsbParam, p, msb);
        gotVoice |= getVal(bankLsbParam, p, lsb);
        gotVoice |= getVal(programParam, p, prg);
        if (gotVoice)
        {
            partMsb[(size_t) p] = msb;
            partLsb[(size_t) p] = lsb;
            partPrg[(size_t) p] = prg;
            lookupVoice(msb, lsb, prg, partName[(size_t) p],
                        partModel[(size_t) p], partElem[(size_t) p]);
        }

        // Drum-buffer assignment: PART MODE 2..5 = DRUMS1..4 (edit buffer 0..3),
        // 0 = Normal and 1 = generic Drum (no editable DRUMS1..4 buffer) = none.
        // This is how a part's drum-setup buffer is recovered from a read-back; it
        // was previously never decoded, so any drum part other than the two hardware
        // defaults (10->DRUMS1, 26->DRUMS3) lost its buffer assignment on read-back.
        int mode = 0;
        if (getVal(partModeParam, p, mode))
            partBuffer[(size_t) p] = (mode >= 2 && mode <= 5) ? (mode - 2) : -1;
    }

    refreshRowChanged();     // the yellow row tabs summarise the MODEL
    updateElementBudget();   // recompute remaining + per-box caps, then repaint
}

//==============================================================================
bool PartListPanel::msVisible() const
{
    return audioProcessor.showMuteSoloColumns;
}

PartListPanel::Cols PartListPanel::columns() const
{
    Cols c;
    c.pad   = 2;   // left content inset so the row content clears the gutter tab
    // 24 for the digits + the override tab's own width and a gap either side of it.
    // The tab is INSIDE this column rather than in a column of its own so it reads
    // as belonging to the part number, which is what it summarises.
    c.num   = 24 + kRowOvrW + 4;
    c.ovrX  = c.pad + c.num - kRowOvrW - 1;
    c.ch    = 36;
    c.el    = 30;
    // Mute / solo sit immediately right of the element reserve, mute first: they are
    // per-part monitoring, so they belong with the other per-part cells rather than
    // out at the row's edge next to the MIDI-activity tab, which is not a control.
    c.ms    = msVisible() ? kMsW : 0;
    c.msX   = msVisible() ? (c.pad + c.num + c.ch + c.el) : -1;
    c.nameX = c.pad + c.num + c.ch + c.el + 2 * c.ms;
    c.nameW = juce::jmax(40, getWidth() - c.nameX);
    // Clearance for the VOICE override tab at the right end of the name column. Kept
    // UNCONDITIONALLY, so a name does not reflow the moment a program change arrives
    // -- the same rule ValueBox follows for its own changed-tab gutter.
    c.nameGutter = kRowOvrW + 3;
    return c;
}

// One M or S cell, inset from its column so the two read as separate buttons and a
// lit fill does not touch the row rule above or below it.
juce::Rectangle<int> PartListPanel::msCell(const Cols& c, int x0, int y, int rh) const
{
    const int h = juce::jmin(rh - 4, 14);
    return { x0 + 2, y + (rh - h) / 2, c.ms - 4, h };
}

int PartListPanel::rowHeight() const
{
    return juce::jmax(13, listHeight() / 32);
}

int PartListPanel::rowAtY(int y) const
{
    if (y < headerH() || y >= headerH() + listHeight()) return -1;
    const int r = (y - headerH()) / rowHeight();
    return (r >= 0 && r < 32) ? r : -1;
}

void PartListPanel::resized()
{
    const auto c = columns();
    const int rh = rowHeight();
    const int boxH = juce::jmin(rh - 2, 16);
    for (int p = 0; p < 32; ++p)
    {
        const int y = headerH() + p * rh + (rh - boxH) / 2;
        chBox[(size_t) p]->setBounds(c.pad + c.num + 1,        y, c.ch - 3, boxH);
        elBox[(size_t) p]->setBounds(c.pad + c.num + c.ch + 1, y, c.el - 3, boxH);
    }
}

void PartListPanel::paintReadoutField(juce::Graphics& g, juce::Rectangle<int> r,
                                      const juce::String& text)
{
    g.setColour(edcol::kFieldBg);
    g.fillRect(r);
    g.setColour(edcol::kFieldBorder);
    g.drawRect(r, 1);
    g.setColour(edcol::kTextStrong);
    g.setFont(edtext::font(edtext::Label, 11.f));
    g.drawText(text, r, juce::Justification::centred);
    refreshCellProxies();   // the painted stops move with the layout
}

void PartListPanel::paint(juce::Graphics& g)
{
    g.setColour(edcol::kModuleBg);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.f);
    g.setColour(edcol::kModuleBorder);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.f, 1.f);

    const auto c = columns();
    const int rh = rowHeight();

    // MIDI-in indicator column geometry (right-to-left): the voice column ends at
    // voiceColRight, then a gap, the 1px separator at sepX, another gap, the tab.
    const int voiceColRight = getWidth() - kIndReserve;               // name / highlight / outline stop here
    const int sepX          = getWidth() - kIndInset - kIndW - kSepGap - kSepW;   // separator left edge
    const int tabX          = getWidth() - kIndInset - kIndW;         // orange tab left edge

    // Column header.
    g.setColour(edcol::kTextDim);
    g.setFont(edtext::font(edtext::Label, 10.f, juce::Font::bold));
    g.drawText("Pt",    c.pad + 2,            0, c.num, headerH(), juce::Justification::centred);
    g.drawText("CH",    c.pad + c.num,        0, c.ch,  headerH(), juce::Justification::centred);
    g.drawText("EL",    c.pad + c.num + c.ch, 0, c.el,  headerH(), juce::Justification::centred);
    if (c.ms > 0)
    {
        g.drawText("M", c.msX,        0, c.ms, headerH(), juce::Justification::centred);
        g.drawText("S", c.msX + c.ms, 0, c.ms, headerH(), juce::Justification::centred);
    }
    g.drawText("Voice", c.nameX + 2,          0, c.nameW, headerH(), juce::Justification::centredLeft);

    for (int p = 0; p < 32; ++p)
    {
        const int y = headerH() + p * rh;
        const bool selected = (p == selectedPart);
        if (selected)
        {
            // Stop the highlight at the separator so it doesn't reach the indicator column.
            g.setColour(edcol::kListSelBg);
            g.fillRect(0, y, sepX, rh);
        }

        // IDE-style row-changed marker: one tab at the far-left row edge when ANY
        // element of this part's row differs from its default — the receive channel
        // (CH), element reserve (EL), or the assigned voice.
        //
        // The field handed to drawChangedTab is sized FROM kChangedTabW rather than
        // being a literal (it was a hard-coded 6). drawChangedTab takes its width off
        // the left of whatever field it is given, so a literal narrower than the tab
        // would silently CLAMP it — the marker would just stop tracking the token, with
        // no error and nothing to see. Every other neighbour of these markers already
        // derives itself (kIndReserve, KeyboardPanel's frameW, PartPanel's kEqValuePad);
        // this was the last one that did not.
        // THE ROW TAB SUMMARISES WHAT THE ROW DOES NOT SHOW. It used to mean "the
        // channel, the element reserve or the voice differs from default" -- all three
        // of which are readable on the row itself, one cell to its right. Since the
        // Voice module can show all 32 parts at once, the useful question is the
        // opposite one: has anything happened to this part that I would have to click
        // through to find? So the roll-up EXCLUDES those five values (see
        // partChangedOutsideVoiceList) and the individual cells carry no tabs of their
        // own -- they never did, and now there is nothing missing when they do not.
        if (((rowChangedMask >> p) & 1u) != 0)
            edcol::drawChangedTab(g, juce::Rectangle<float>(0.0f, (float) y,
                                                            edcol::kChangedTabW, (float) rh));

        g.setColour(selected ? edcol::kListSelText : edcol::kTextDim);
        g.setFont(edtext::font(edtext::Label, 11.f, selected ? juce::Font::bold : juce::Font::plain));
        // The digits keep the LEFT of the column; the override tab has the right of
        // it, so the number stays where the eye expects it whether or not a tab is up.
        g.drawText(juce::String(p + 1), c.pad + 2, y, c.num - kRowOvrW - 5, rh,
                   juce::Justification::centred);

        // The MIDI-override roll-up, the cyan counterpart of the yellow tab at the
        // other edge and answering the same question about the same set of values.
        // The VOICE has its own mark at the far end of the row and is excluded here:
        // a program change is a big enough event to be worth its own place, and it is
        // the one override this list can name rather than merely count.
        // `pos` 1.0 with origin 0 = a FULL-HEIGHT tab: this is a roll-up, not a
        // level, so there is no value for a height to report. The 0.0f is the bar's
        // ORIGIN and must be passed explicitly -- the width is the argument after it.
        if (((rowOverrideMask >> p) & 1u) != 0)
            edcol::drawMidiOverrideBar(
                g, juce::Rectangle<float>((float) c.ovrX, (float) y,
                                          (float) kRowOvrW, (float) rh),
                1.0f, 0.0f, (float) kRowOvrW, 1.5f);

        // Mute / solo cells. Painted, not child components: 64 more Components in a
        // list that already repaints per MIDI-activity tick costs more than two
        // rectangles, and the row already resolves its own click targets by column.
        if (c.ms > 0)
        {
            const bool muted  = audioProcessor.isPartMuted(p);
            const bool soloed = audioProcessor.isPartSoloed(p);
            const struct { int x; bool on; juce::Colour bg; const char* t; } cells[2] = {
                { c.msX,         muted,  edcol::kMuteOnBg, "M" },
                { c.msX + c.ms,  soloed, edcol::kSoloOnBg, "S" },
            };
            for (const auto& cell : cells)
            {
                const auto r = msCell(c, cell.x, y, rh);
                g.setColour(cell.on ? cell.bg : edcol::kMsOffBg);
                g.fillRect(r);
                g.setColour(edcol::kMsBorder);
                g.drawRect(r, 1);
                g.setColour(cell.on ? edcol::kMsOnText : edcol::kMsOffText);
                g.setFont(edtext::font(edtext::Label, 10.f, juce::Font::bold));
                g.drawText(cell.t, r, juce::Justification::centred);
            }
        }

        voiceind::draw(g, juce::Rectangle<int>(c.nameX + 2, y, voiceind::kGutter, rh),
                       partModel[(size_t) p], partElem[(size_t) p]);
        const int nx = c.nameX + 2 + voiceind::kGutter + 1;
        g.setColour(selected ? edcol::kListSelText : edcol::kTextLabel);
        g.setFont(edtext::font(edtext::VoiceList, 11.f));
        // Stops short of the voice override tab's gutter, so a long name ellipsizes
        // rather than running under the mark. Unconditional -- see Cols::nameGutter.
        g.drawText(voiceName(p), nx, y, voiceColRight - c.nameGutter - nx, rh,
                   juce::Justification::centredLeft);

        // Drum edit buffer: outline the voice name in the buffer's colour. The
        // outline stops at voiceColRight so it clears the separator + indicator tab.
        if (partBuffer[(size_t) p] >= 0)
        {
            g.setColour(drumbuf::colour(partBuffer[(size_t) p]));
            g.drawRect(juce::Rectangle<int>(c.nameX + 1, y + 1,
                                            voiceColRight - (c.nameX + 1), rh - 2), 2);
        }

        // The MIDI-override mark, at the OTHER end of the same row -- the same
        // left-versus-right pairing the value boxes use, so one habit reads both.
        //
        // A full-height TAB, not a level bar: a voice is a bank and a program, not a
        // position in a range, so there is no height that would mean anything. The
        // row's own name still reads as YOUR voice; the device's is in the tooltip,
        // because it is the one thing here that will not fit on the row.
        //
        // This is the case with teeth. A program change re-initialises the DRUM
        // SETUP buffer the part's PART MODE names -- measured, and it happens even
        // when the program is the one already loaded, which is what a sequencer
        // sends at the top of every take.
        //
        // DRAWN AFTER THE DRUM-BUFFER OUTLINE, and that is not tidiness: the outline
        // ends at voiceColRight, which is exactly where this tab sits, so painted
        // earlier the tab is overdrawn by a 2 px line in the buffer's colour and all
        // but disappears -- on a DRUM part, which is the case the mark matters most
        // for. The outline is an identity code and cannot move, so the mark goes on
        // top of it instead.
        //
        // `pos` 1.0 with origin 0 = a FULL-HEIGHT tab, and the four trailing arguments
        // are POSITIONAL: pos, origin, width, vInset. This call passed the width into
        // the ORIGIN slot and left the width at 0, so `removeFromRight(0)` yielded an
        // empty gutter and the tab painted NOTHING -- invisible rather than merely
        // wrong, which is why it read as a missing feature. Same defect the roll-up
        // tab a few lines up had; that one was fixed and this one was not.
        if (((voiceOverrideMask >> p) & 1u) != 0)
            edcol::drawMidiOverrideBar(
                g, juce::Rectangle<float>((float) (voiceColRight - edcol::kChangedTabW),
                                          (float) y, edcol::kChangedTabW, (float) rh),
                1.0f, 0.0f, edcol::kChangedTabW, 1.5f);

        // Per-part "MIDI in" indicator: an orange tab just inside the right edge,
        // its opacity the part's current fade level (see timerCallback).
        if (audioProcessor.midiInputIndicators && midiind::visible(midiLevel[(size_t) p]))
        {
            g.setColour(midiind::colour().withAlpha(juce::jlimit(0.f, 1.f, midiLevel[(size_t) p])));
            g.fillRect(tabX, y + 1, kIndW, rh - 2);
        }

        g.setColour(edcol::kRowRule);
        g.drawHorizontalLine(y + rh - 1, 0.f, (float) getWidth());
    }

    // Vertical separator dividing the voice-name column from the MIDI-in indicator
    // column, matching the module border (colour + 1px). Spans the 32 list rows.
    g.setColour(kSepColour);
    g.fillRect(sepX, headerH(), kSepW, 32 * rh);

    // Footer readout for the selected part: EL / MSB / LSB / PRG (read-only).
    // Value fields on top, labels just below them. Anchored a small fixed gap
    // below the actual last part row (row 32), rather than at the footer top —
    // the list's row-division leftover otherwise left a larger gap there.
    const int rowsBottom = headerH() + 32 * rh;   // bottom of the part-32 row
    const int valY = rowsBottom + 8;
    const int fieldH = 16;
    const int lblY = valY + fieldH + 1;
    const int lblH = 12;
    int reserved = 0;
    for (auto& b : elBox) reserved += (int) b->getValue();
    const int el  = kTotalElements - reserved;             // total elements remaining
    const int msb = partMsb[(size_t) selectedPart];
    const int lsb = partLsb[(size_t) selectedPart];
    const int prg = partPrg[(size_t) selectedPart] + 1;   // show 1-based

    // The EL field is left/right-aligned with the per-row EL boxes above it (same
    // x + width). The four fields are then evenly spaced across to the right edge.
    const int elX = c.pad + c.num + c.ch + 1;
    const int elW = c.el - 3;
    const int c0  = elX + elW / 2;                         // EL centre (fixed)
    // PRG centre, pulled a further box-width in from the right so there's a
    // ~one-box margin between PRG and the module's right edge.
    const int c3  = getWidth() - 4 - elW - elW / 2;        // rightmost (PRG) centre
    const char* labels[4] = { "EL", "MSB", "LSB", "PRG" };
    const int   vals[4]   = { el, msb, lsb, prg };
    for (int i = 0; i < 4; ++i)
    {
        const int cx = c0 + (c3 - c0) * i / 3;             // evenly spaced centres
        const int x  = cx - elW / 2;
        paintReadoutField(g, juce::Rectangle<int>(x, valY, elW, fieldH),
                          juce::String(vals[i]));
        g.setColour(edcol::kTextDim);
        g.setFont(edtext::font(edtext::Label, 10.f, juce::Font::bold));
        g.drawText(labels[i], x - 4, lblY, elW + 8, lblH, juce::Justification::centred);
    }
}

juce::String PartListPanel::getTooltip()
{
    // Position-dependent: the Pt-number and voice-name columns are painted (no child
    // component), so their tooltips are resolved here; the CH/EL ValueBox children
    // carry their own tooltips and are hit before this is reached.
    const auto pt = getMouseXYRelative();
    const int row = rowAtY(pt.y);
    if (row < 0) return {};
    const auto c = columns();
    if (pt.x >= c.pad && pt.x < c.pad + c.num)
        return juce::String(mu100::tooltips::forKey("VOICE|PART SELECT"))
                   .replace("{n}", juce::String(row + 1));
    // The MIDI-in indicator column (separator + tab strip) has its own tooltip; the
    // voice-name tooltip must stop at voiceColRight so it doesn't leak onto it.
    if (pt.x >= getWidth() - kIndReserve)
        return mu100::tooltips::forKey("VOICE|MIDI INDICATOR");
    // The M / S cells. These have no XGEdit counterpart to mine hover text from, so
    // their rows in tooltips.csv are marked `manual` — but they still live there
    // rather than as literals here, like every other tooltip in the editor.
    if (c.ms > 0 && pt.x >= c.msX && pt.x < c.nameX)
        return juce::String(mu100::tooltips::forKey(pt.x >= c.msX + c.ms ? "VOICE|SOLO"
                                                                        : "VOICE|MUTE"))
                   .replace("{n}", juce::String(row + 1));
    if (pt.x >= c.nameX)
    {
        // On an overridden row the device's voice is the thing the user cannot see
        // and cannot get any other way -- the row necessarily shows THEIRS. Led
        // with, ahead of the standing tooltip, because it is the transient half.
        if (((voiceOverrideMask >> row) & 1u) != 0)
            return "Incoming MIDI has this part playing "
                 + voiceOverrideName[(size_t) row]
                 + " on the device. The name shown is your setup, and is what will be saved.\n\n"
                 + juce::String(mu100::tooltips::forKey("VOICE|VOICE NAME"));
        return mu100::tooltips::forKey("VOICE|VOICE NAME");
    }
    return {};
}

// One click on an M or S cell. Cubase's semantics, which is what the user already
// has open in the other window: a plain click toggles that part additively, and
// Ctrl+click is EXCLUSIVE -- it takes the state away from every other part.
//
// Ctrl+clicking a part that is already the only one engaged clears everything
// instead of leaving it engaged. Without that, unsoloing after an exclusive solo
// needs a second, different click; with it, Ctrl+click twice is the whole
// "clear a screenful of solos" gesture.
void PartListPanel::handleMsClick(int part, bool solo, bool exclusive)
{
    const uint32_t bit  = 1u << part;
    const uint32_t cur  = solo ? audioProcessor.getPartSoloMask()
                               : audioProcessor.getPartMuteMask();
    const uint32_t next = exclusive ? ((cur == bit) ? 0u : bit)
                                    : (cur ^ bit);
    if (solo) audioProcessor.setPartSoloMask(next);
    else      audioProcessor.setPartMuteMask(next);
    repaint();
}

void PartListPanel::mouseDown(const juce::MouseEvent& e)
{
    const int p = rowAtY(e.y);
    if (p < 0) return;

    const auto cols = columns();

    // RIGHT-CLICK: the part menu, answered before anything else on the row so it
    // reaches the name column too -- a right-click there used to fall through and open
    // the Voice List, which would now be the browser AND the menu at once.
    //
    // It SELECTS FIRST. Every item on the menu acts on "the selected part", so opening
    // it on a row the rest of the editor is not showing would invite an edit aimed at
    // one part and applied to another.
    if (e.mods.isRightButtonDown())
    {
        if (p != selectedPart)
        {
            selectedPart = p;
            repaint();
            if (onSelectPart) onSelectPart(p);
        }
        if (onPartContextMenu) onPartContextMenu(p);
        return;
    }

    // Mute / solo, handled BEFORE the row selection below and returning without
    // touching it: the whole point of a per-row cell rather than a global button is
    // that you can silence part 7 while still editing part 3.
    if (cols.ms > 0 && e.mods.isLeftButtonDown()
        && e.x >= cols.msX && e.x < cols.nameX)
    {
        handleMsClick(p, e.x >= cols.msX + cols.ms, e.mods.isCommandDown());
        return;
    }

    if (p != selectedPart)
    {
        selectedPart = p;
        repaint();
        if (onSelectPart) onSelectPart(p);
    }

    // A click in the voice-name column opens the browser to reassign it.
    const auto& c = cols;
    if (e.x >= c.nameX && onRequestVoiceBrowse)
        onRequestVoiceBrowse(p, partMsb[(size_t) p], partLsb[(size_t) p], partPrg[(size_t) p]);
}

//==============================================================================
// Keyboard navigation (KeyNav.h) — the SELECTED row only, not all 32.
//
// The generic collector would offer 64 stops (a channel and an element-reserve box
// per part) and walking to part 20's channel would be a rout. It would also miss the
// three cells that matter most, because Mute, Solo and the voice name are PAINTED,
// not components — the collector can only see real children.
//
// Restricting to the selected row is not a compromise: it is the shape the Part,
// Controllers and Keyboard modules already have (they only ever show the selected
// part), and it composes with Ctrl+cursor left/right, which moves that selection.
// So the two together cover all 32 rows in two axes instead of one long one.
std::vector<keynav::Item> PartListPanel::navItems()
{
    std::vector<keynav::Item> items;

    const int p = selectedPart;
    if (p < 0 || p > 31)
    {
        // An invisible proxy is out of the accessibility tree and cannot take focus,
        // which is exactly what a stop that is not offered needs.
        for (auto& px : msProxy) px.setVisible(false);
        nameProxy.setVisible(false);
        return items;
    }

    const auto c  = columns();
    const int  rh = rowHeight();
    const int  y  = headerH() + p * rh;

    if (chBox[(size_t) p] != nullptr) items.push_back(keynav::Item { chBox[(size_t) p].get() });
    if (elBox[(size_t) p] != nullptr) items.push_back(keynav::Item { elBox[(size_t) p].get() });

    // Mute and Solo. Both route through handleMsClick, the same entry point a click
    // and the Alt+M / Alt+S shortcuts use, so there is no second behaviour to keep in
    // step. Up/down set the state outright rather than toggling, matching a DotToggle:
    // these are 0/1 values and "up increases" holds. Enter toggles, as a click does.
    // Neither has a default to reset to, so Backspace is deliberately a no-op.
    // NEVER hide a proxy and then re-show it in the same pass. Hiding a component that
    // HAS keyboard focus makes JUCE give that focus away -- to the panel's first
    // focusable child, part 1's CH box -- and the focus listener then walks the cursor
    // there. This function runs on every focus change (navFindStopFor asks every group
    // for its stops), so a blanket "hide all, then show the ones we offer" at the top
    // yanked the cursor off the very cell a click had just put it on. Set each proxy's
    // visibility ONCE, to its final value.
    if (c.ms <= 0)
        for (auto& px : msProxy) px.setVisible(false);

    if (c.ms > 0)
    {
        for (int solo = 0; solo < 2; ++solo)
        {
            keynav::Item it;
            it.comp    = this;
            it.painted = true;
            it.cell    = msCell(c, c.msX + solo * c.ms, y, rh);
            const bool isSolo = (solo != 0);
            // The keyboard focus for this stop. The CELL is authoritative -- the proxy
            // is moved onto it, never the other way round.
            auto& px = msProxy[solo];
            px.setBounds(it.cell);
            px.setVisible(true);   // never hidden-then-shown -- see the note below
            it.a11yProxy = &px;
            it.activate = [this, p, isSolo] { handleMsClick(p, isSolo, false); };
            it.adjust   = [this, p, isSolo](int dir, bool)
            {
                if (dir == 0) return;
                const bool on = isSolo ? ((audioProcessor.getPartSoloMask() >> p) & 1) != 0
                                       : ((audioProcessor.getPartMuteMask() >> p) & 1) != 0;
                if (on != (dir > 0)) handleMsClick(p, isSolo, false);
            };
            items.push_back(std::move(it));
        }
    }

    // The voice name: Enter opens the Voice List for this part, exactly as clicking
    // the name does. There is nothing to nudge — a voice is not a scalar you step
    // through here, the list is — so adjust is a no-op rather than a hidden fourth
    // way to change patches.
    {
        // INSET, because a part on a DRUMS buffer already has its name framed in that
        // buffer's colour (paint(), the same rect as below before the reduce). Those
        // colours are an identity code and cannot move, so the cursor gives way:
        // reduced(5) puts its own expanded(2) 1px clear of that line's inner edge.
        // The inset costs nothing, because a mark this wide is drawn as BRACKETS (see
        // keynav::cueFor) and so has no long edges to crowd the name with.
        const int voiceColRight = getWidth() - kIndReserve;
        // Stops at the name column's own gutter, not at voiceColRight: the voice
        // override tab lives in that gutter, and the focus ring is drawn OUTSIDE this
        // cell (expanded by 2), so a cell reaching the full width would put the ring
        // on top of the mark. Same rule the EG and EQ readouts follow -- what has to
        // be cleared is the CURSOR around a control, which is bigger than the control.
        const auto nameRect = juce::Rectangle<int>(c.nameX + 1, y + 1,
                                                   voiceColRight - c.nameGutter
                                                       - (c.nameX + 1), rh - 2);
        keynav::Item it;
        it.comp    = this;
        it.painted = true;
        it.cell    = nameRect.reduced(5);
        nameProxy.setBounds(it.cell);
        nameProxy.setVisible(true);
        it.a11yProxy = &nameProxy;
        it.adjust  = [](int, bool) {};
        it.activate = [this, p]
        {
            if (onRequestVoiceBrowse)
                onRequestVoiceBrowse(p, partMsb[(size_t) p], partLsb[(size_t) p],
                                     partPrg[(size_t) p]);
        };
        it.reset = [] {};
        items.push_back(std::move(it));
    }

    return items;
}
