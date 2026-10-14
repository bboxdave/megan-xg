// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "ControllersPanel.h"
#include "EditorTextScale.h"
#include "EditorColours.h"
#include "XgKnob.h"
#include "BracketLabel.h"
#include "PluginProcessor.h"
#include "codec/MU100AddressMap.h"
#include "codec/MU100Tooltips.h"

namespace
{
    // Controller sources (the "Controller Name" list-box), each a 6-byte depth
    // block in the MULTI PART table. AC1/AC2 add a CONTROLLER NUMBER assignment.
    // `prefix` builds the MULTI PART param names (e.g. "MW PITCH CONTROL").
    struct Source { const char* name; const char* prefix; bool hasNumber; const char* numberName; };
    const Source kSources[] = {
        { "MODULATION",       "MW",   false, nullptr },
        { "PITCH BEND",       "BEND", false, nullptr },
        { "CH AFTER TOUCH",   "CAT",  false, nullptr },
        { "POLY AFTER TOUCH", "PAT",  false, nullptr },
        { "CONTROLLER 1",     "AC1",  true,  "AC1 CONTROLLER NUMBER" },
        { "CONTROLLER 2",     "AC2",  true,  "AC2 CONTROLLER NUMBER" },
    };
    constexpr int kNumSources = (int) (sizeof(kSources) / sizeof(kSources[0]));

    // Knobs in XGEdit display order vs the depth-block byte order. The block is
    // PITCH, LPF, AMP, PMOD, FMOD, AMOD; XGEdit shows PITCH, AMP, FILT, PMOD,
    // AMOD, FMOD — so each knob maps to a block offset.
    const char* const kKnobLabel[6]  = { "PITCH", "AMP", "FILT", "PMOD", "AMOD", "FMOD" };
    const int         kKnobOffset[6] = {   0,       2,     1,      3,      5,      4    };

    // The depth-param name suffix for each block offset.
    const char* const kSuffix[6] = {
        "PITCH CONTROL", "LOW PASS FILTER CONTROL", "AMPLITUDE CONTROL",
        "LFO PMOD DEPTH", "LFO FMOD DEPTH", "LFO AMOD DEPTH"
    };

    int depthParam(int sourceIx, int blockOffset)
    {
        const juce::String n = juce::String(kSources[sourceIx].prefix) + " " + kSuffix[blockOffset];
        return mu100::AddressMap::find(n.toRawUTF8(), "MULTI PART");
    }

    // Factory default for a depth (used before any read-back so a knob never shows
    // the previous source's value). Centre 64 for PITCH/FILT/AMP, 0 for the LFO
    // depths — except the two non-zero factory cases.
    int depthDefault(int sourceIx, int blockOffset)
    {
        if (sourceIx == 1 && blockOffset == 0) return 0x42;   // BEND PITCH = +2 semitones
        if (sourceIx == 0 && blockOffset == 3) return 0x0A;   // MW LFO PMOD = 10
        return blockOffset <= 2 ? 64 : 0;
    }

    // The MIDI CC numbers (0..95) that have a named "built-in" effect, per the XG
    // implementation chart; all others are shown as a bare number. Column-major in
    // the 3-column picker => 0-31 / 32-63 / 64-95, matching XGEdit.
    juce::String ccName(int cc)
    {
        switch (cc)
        {
            case 1:  return "MOD";       case 2:  return "BREATH";    case 4:  return "FOOT";
            case 5:  return "PORT TIME"; case 6:  return "DATA MSB";  case 7:  return "VOLUME";
            case 10: return "PAN";       case 11: return "EXPRESSION";
            case 64: return "SUS";       case 65: return "PORTA";     case 66: return "SOS";
            case 67: return "SOFT";      case 69: return "HOLD2";     case 71: return "HARMONIC";
            case 72: return "RELEASE";   case 73: return "ATTACK";    case 74: return "BRIGHT";
            case 84: return "PORTACTRL"; case 91: return "REVERB";    case 92: return "TREMOLO";
            case 93: return "CHORUS";    case 94: return "VARIATION"; case 95: return "PHASER";
            default: return {};
        }
    }

    // One CC's label, exactly as the picker lists it. Shared with the override mark
    // so the name the tooltip reports is the name the box would have shown.
    juce::String ccItemText(int cc)
    {
        if (cc < 0 || cc > 95) return juce::String(cc);   // out of range: the bare number
        const auto nm = ccName(cc);
        return nm.isEmpty() ? juce::String(cc) : juce::String(cc) + " " + nm;
    }

    juce::StringArray buildCcItems()
    {
        juce::StringArray a;
        for (int cc = 0; cc <= 95; ++cc) a.add(ccItemText(cc));
        return a;
    }

    void initLabel(juce::Label& l, const char* txt, juce::Justification j = juce::Justification::centredLeft)
    {
        l.setText(txt, juce::dontSendNotification);
        l.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
        l.setJustificationType(j);
        l.setColour(juce::Label::textColourId, edcol::kTextLabel);
    }
}

//==============================================================================
ControllersPanel::ControllersPanel(YamahaMU100AudioProcessor& proc) : audioProcessor(proc)
{
    modTitle.setText("Controller", juce::dontSendNotification);
    modTitle.setFont(edtext::font(edtext::Title, 15.f, juce::Font::bold));
    modTitle.setJustificationType(juce::Justification::centredRight);
    modTitle.setColour(juce::Label::textColourId, edcol::kTextTitle);
    addAndMakeVisible(modTitle);

    // Controller Name list-box (the source selector).
    {
        juce::StringArray names;
        for (const auto& s : kSources) names.add(s.name);
        nameBox.setItems(names, 1);
        nameBox.setTooltip(mu100::tooltips::forKey("SEL|CONTROLLER"));
        nameBox.setSelectedIndex(0, false);
        nameBox.setPopupColumnWidth(140);   // fit "POLY AFTER TOUCH" at the larger text size
        nameBox.onSelect = [this](int idx) { selectSource(idx); };
        addAndMakeVisible(nameBox);
    }

    // Six depth knobs with fixed per-knob display formatting (the byte each knob
    // edits changes with the source, but its meaning — pitch/amp/filter/LFO — does
    // not). Only PITCH is bipolar in semitones; AMP/FILT show % / cents like XGEdit.
    for (int i = 0; i < 6; ++i)
    {
        auto k = std::make_unique<XgKnob>();
        k->setLabel(kKnobLabel[i]);
        // Tooltip is the destination's meaning (source-independent). kKnobOffset maps
        // the display knob to its depth-block slot, whose suffix is the CTRL| key.
        k->setTooltip(mu100::tooltips::forKey(std::string("CTRL|") + kSuffix[kKnobOffset[i]]));

        switch (i)
        {
            case 0:  // PITCH — raw 0x28..0x58, 64 = 0, shown -24..+24 semitones
                k->setRange(40.0, 88.0, 1.0);
                k->setTextFromValue([](double dv)
                {
                    const int s = (int) std::lround(dv) - 64;
                    return (s > 0 ? "+" : "") + juce::String(s);
                });
                k->setValueFromText([](const juce::String& t)
                {
                    return juce::jlimit(40.0, 88.0, 64.0 + t.trim().getIntValue());
                });
                break;

            case 1:  // AMP — raw 0..127, 64 = 0%, bipolar percent
                k->setRange(0.0, 127.0, 1.0);
                k->setTextFromValue([](double dv)
                {
                    const int p = (int) std::lround(((int) std::lround(dv) - 64) * 100.0 / 64.0);
                    return (p > 0 ? "+" : "") + juce::String(p) + "%";
                });
                k->setValueFromText([](const juce::String& t)
                {
                    return juce::jlimit(0.0, 127.0, 64.0 + t.trim().getDoubleValue() * 64.0 / 100.0);
                });
                break;

            case 2:  // FILT (TVF) — raw 0..127, 64 = 0, shown in cents (150c/step)
                k->setRange(0.0, 127.0, 1.0);
                k->setTextFromValue([](double dv)
                {
                    const int c = ((int) std::lround(dv) - 64) * 150;
                    return (c > 0 ? "+" : "") + juce::String(c) + "c";
                });
                k->setValueFromText([](const juce::String& t)
                {
                    return juce::jlimit(0.0, 127.0, 64.0 + t.trim().getDoubleValue() / 150.0);
                });
                break;

            default: // PMOD / AMOD / FMOD — raw 0..127
                k->setRange(0.0, 127.0, 1.0);
                break;
        }

        // PITCH, AMP and FILT rest at 64, so an override bar or arc grows out of
        // centre; the three LFO depths rest at nothing and keep the default origin.
        if (i <= 2) k->setBarOrigin(64.0);

        // Room for a horizontal override bar. All six can be reached by a SysEx
        // parameter change, so the whole row earns it rather than merely sharing it.
        k->setOverrideMarkable(true);

        addAndMakeVisible(*k);
        depthKnobs[(size_t) i] = std::move(k);
    }

    // Controller 1 / Controller 2 — AC1/AC2 CC-number pickers (3-column popup).
    ctrlTitle.setText("Controller", juce::dontSendNotification);
    ctrlTitle.setFont(edtext::font(edtext::Title, 15.f, juce::Font::bold));   // match the other module titles
    ctrlTitle.setJustificationType(juce::Justification::centred);
    ctrlTitle.setColour(juce::Label::textColourId, edcol::kTextTitle);
    addAndMakeVisible(ctrlTitle);

    initLabel(ctrl1Label, "CONTROLLER 1", juce::Justification::centred);
    initLabel(ctrl2Label, "CONTROLLER 2", juce::Justification::centred);
    addAndMakeVisible(ctrl1Label);
    addAndMakeVisible(ctrl2Label);

    const auto ccItems = buildCcItems();
    auto wireAssign = [this](XgSelector& box, const char* paramName)
    {
        box.onSelect = [this, paramName](int cc)
        {
            const int ix = mu100::AddressMap::find(paramName, "MULTI PART");
            if (ix >= 0)
                audioProcessor.sendParamChange(
                    mu100::AddressMap::makeParamChange(mu100::AddressMap::param(ix), currentPart, (uint32_t) cc));
            afterEdit();
        };
    };
    ctrl1Box.setItems(ccItems, 3);
    ctrl2Box.setItems(ccItems, 3);
    ctrl1Box.setTooltip(mu100::tooltips::forKey("CTRL|AC1 CONTROLLER NUMBER"));
    ctrl2Box.setTooltip(mu100::tooltips::forKey("CTRL|AC2 CONTROLLER NUMBER"));
    ctrl1Box.setSelectedIndex(0x10, false);   // factory default AC1 = 16
    ctrl2Box.setSelectedIndex(0x11, false);   // factory default AC2 = 17
    ctrl1Box.setDefaultIndex(0x10);   ctrl1Box.setShowChangedTab(true);
    ctrl2Box.setDefaultIndex(0x11);   ctrl2Box.setShowChangedTab(true);
    wireAssign(ctrl1Box, "AC1 CONTROLLER NUMBER");
    wireAssign(ctrl2Box, "AC2 CONTROLLER NUMBER");
    addAndMakeVisible(ctrl1Box);
    addAndMakeVisible(ctrl2Box);

    // Controller wheel (manual §8.4.2) + its small "resend on edit" LED toggle.
    // Pitch-bend (source 1) rests centred; every other source rests at the bottom.
    for (auto& part : wheelPos)
        for (size_t s = 0; s < part.size(); ++s)
            part[s] = (s == 1) ? 0.5 : 0.0;

    wheel.onMove = [this](double v) { wheelPos[(size_t) currentPart][(size_t) currentSource] = v; sendWheel(); };
    wheel.setTooltip(mu100::tooltips::forKey("CTRL|WHEEL"));
    addAndMakeVisible(wheel);

    // The re-transmit LED defaults ON, matching XGEdit, so edits are audible
    // immediately without the user first having to move the wheel.
    resendToggle.setState(true, false);
    resendToggle.setTooltip(mu100::tooltips::forKey("CTRL|RESEND LED"));
    addAndMakeVisible(resendToggle);

    selectSource(0);
}

ControllersPanel::~ControllersPanel() = default;

//==============================================================================
void ControllersPanel::wireDepthKnob(int knobIx, int low)
{
    const int blockOffset = kKnobOffset[knobIx];
    const int ix = depthParam(currentSource, blockOffset);
    auto& k = *depthKnobs[(size_t) knobIx];

    k.setDefaultValue((double) depthDefault(currentSource, blockOffset));
    if (ix < 0) { k.onChange = nullptr; k.setEnabled(false); return; }
    k.setEnabled(true);
    // Throttled — a controller-depth knob drag; see sendParamChangeThrottled.
    k.onChange = [this, ix](double v)
    {
        audioProcessor.sendParamChangeThrottled(
            mu100::AddressMap::makeParamChange(mu100::AddressMap::param(ix), currentPart, (uint32_t) v));
        afterEdit();
    };
    juce::ignoreUnused(low);
}

void ControllersPanel::selectSource(int sourceIndex)
{
    currentSource = juce::jlimit(0, kNumSources - 1, sourceIndex);
    for (int i = 0; i < 6; ++i) wireDepthKnob(i, 0);
    loadDepths();

    // Pitch bend is a spring-loaded, self-centring (bipolar) wheel; everything
    // else rests at the bottom. Restore this source's cached wheel position.
    const bool isBend = (currentSource == 1);
    wheel.setSpring(isBend);
    wheel.setBipolar(isBend);
    wheel.setValueQuiet(wheelPos[(size_t) currentPart][(size_t) currentSource]);

    // Re-assert after wireDepthKnob, which enables every knob it can resolve.
    updateDrumKitGating();
}

void ControllersPanel::setDrumKitPart(bool isKit)
{
    if (isKit == drumKitPart) return;
    drumKitPart = isKit;
    updateDrumKitGating();
}

// Gray the two depth knobs the device ignores on a DRUM/SFX-kit part: FILTER
// MODURATION DEPTH and AMPLITUDE MODURATION DEPTH (sound_list.txt p46's MULTI PART
// note). In this panel's XGEdit display order those are AMOD = 4 and FMOD = 5 (see
// kKnobLabel) — note the block order is different, so index by the display array.
// PMOD (3) is NOT excluded and stays live.
//
// Values are untouched, as in PartPanel::updateDrumKitGating: they are MULTI PART
// bytes that survive a program change on the device too.
void ControllersPanel::updateDrumKitGating()
{
    for (int i : { 4, 5 })
    {
        auto* k = depthKnobs[(size_t) i].get();
        if (k == nullptr) continue;
        k->setInteractive(! drumKitPart);
        k->setLabelDimmed(drumKitPart);
    }
}

// Build and transmit the continuous-controller message for the current source at
// the wheel's current position, on the current part's channel.
void ControllersPanel::sendWheel()
{
    const double norm = wheel.getValue();
    const int    v7   = (int) std::lround(norm * 127.0);

    switch (currentSource)
    {
        case 0:  // Modulation -> CC 1
            audioProcessor.sendChannelMessage(currentPart, 0xB0, 1, v7);
            break;
        case 1:  // Pitch Bend -> 14-bit bend (centre = 0.5)
        {
            const int bend = (int) std::lround(norm * 16383.0);
            audioProcessor.sendChannelMessage(currentPart, 0xE0, bend & 0x7F, (bend >> 7) & 0x7F);
            break;
        }
        case 2:  // Channel After-Touch
            audioProcessor.sendChannelMessage(currentPart, 0xD0, v7);
            break;
        case 3:  // Poly After-Touch (sent on a representative note)
            audioProcessor.sendChannelMessage(currentPart, 0xA0, 60, v7);
            break;
        case 4:  // Controller 1 -> the AC1-assigned CC number
            audioProcessor.sendChannelMessage(currentPart, 0xB0, ctrl1Box.getSelectedIndex(), v7);
            break;
        case 5:  // Controller 2 -> the AC2-assigned CC number
            audioProcessor.sendChannelMessage(currentPart, 0xB0, ctrl2Box.getSelectedIndex(), v7);
            break;
        default: break;
    }
}

// If the LED is on, re-transmit the wheel position after an edit so the change is
// audible without the user having to wiggle the real controller (manual §8.4.2).
void ControllersPanel::afterEdit()
{
    if (resendToggle.getState()) sendWheel();
}

void ControllersPanel::loadDepths()
{
    using mu100::AddressMap;
    const auto& st = audioProcessor.getEditorState();
    for (int i = 0; i < 6; ++i)
    {
        const int off = kKnobOffset[i];
        const int ix  = depthParam(currentSource, off);
        int v = depthDefault(currentSource, off);
        if (ix >= 0)
        {
            const auto got = AddressMap::get(st, AddressMap::param(ix), currentPart);
            if (got.present) v = got.combined;
        }
        depthKnobs[(size_t) i]->setValueQuiet((double) v);
    }

    // The knobs have just been re-pointed at a source (or re-read for a part), so the
    // marks must move with them in the same call: neither switch touches the override
    // shadow, so nothing else would fire.
    refreshMidiOverrides();
}

//==============================================================================
// The MIDI-override marks for the six depth knobs. Which parameter each knob edits
// depends on the selected SOURCE as well as the selected part, so this resolves them
// exactly as loadDepths does -- display order through kKnobOffset into the depth
// block, never the block order.
//
// No XG controller lands on these addresses, so a mark here always means a SysEx
// parameter change moved the part's response to that controller.
void ControllersPanel::refreshMidiOverrides()
{
    const bool show = audioProcessor.showMidiOverrides;

    for (int i = 0; i < 6; ++i)
    {
        const int ix = show ? depthParam(currentSource, kKnobOffset[i]) : -1;
        const int v  = ix >= 0 ? audioProcessor.midiOverrideValue(ix, currentPart) : -1;
        depthKnobs[(size_t) i]->setMidiOverride(v < 0 ? 0.0 : (double) v, v >= 0);
    }

    // The two AC assignment boxes. These name a CC NUMBER rather than sit somewhere in
    // a range, so each takes a full-height tab and the device's choice goes to the
    // tooltip -- see XgSelector::setMidiOverride.
    auto markAssign = [&](XgSelector& box, const char* paramName)
    {
        const int ix = show ? mu100::AddressMap::find(paramName, "MULTI PART") : -1;
        const int v  = ix >= 0 ? audioProcessor.midiOverrideValue(ix, currentPart) : -1;
        box.setMidiOverride(v >= 0, v >= 0 ? ccItemText(v) : juce::String());
    };
    markAssign(ctrl1Box, "AC1 CONTROLLER NUMBER");
    markAssign(ctrl2Box, "AC2 CONTROLLER NUMBER");
}

// See SystemPanel::refreshMidiMarkStyle -- a style change moves each knob's internal
// split, and our resized() hands them the bounds they already had.
void ControllersPanel::refreshMidiMarkStyle()
{
    for (auto& k : depthKnobs) if (k) k->refreshMidiMarkStyle();
    resized();
    repaint();
}

void ControllersPanel::loadAssignments()
{
    using mu100::AddressMap;
    const auto& st = audioProcessor.getEditorState();
    auto load = [&](XgSelector& box, const char* name, int dflt)
    {
        const int ix = AddressMap::find(name, "MULTI PART");
        int v = dflt;
        if (ix >= 0)
        {
            const auto got = AddressMap::get(st, AddressMap::param(ix), currentPart);
            if (got.present) v = got.combined;
        }
        box.setSelectedIndex(v, false);
    };
    load(ctrl1Box, "AC1 CONTROLLER NUMBER", 0x10);
    load(ctrl2Box, "AC2 CONTROLLER NUMBER", 0x11);
}

void ControllersPanel::setCurrentPart(int part0to31)
{
    currentPart = juce::jlimit(0, 31, part0to31);
    loadDepths();
    loadAssignments();
    wheel.setValueQuiet(wheelPos[(size_t) currentPart][(size_t) currentSource]);   // per-part wheel
}

void ControllersPanel::applyState(const mu100::DeviceState&)
{
    loadDepths();
    loadAssignments();
}

//==============================================================================
void ControllersPanel::paint(juce::Graphics& g)
{
    g.setColour(edcol::kModuleBg);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.f);
    g.setColour(edcol::kModuleBorder);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.f, 1.f);

    // LFO group bracket over PMOD / AMOD / FMOD (depth knobs 3..5), indicating
    // those three are the LFO depths.
    if (depthKnobs[3] && depthKnobs[5])
        xgbracket::draw(g, depthKnobs[3]->getX(), depthKnobs[5]->getRight(),
                        depthKnobs[3]->getY() - 7, "LFO");
}

void ControllersPanel::resized()
{
    auto b = getLocalBounds().reduced(6);

    // Controller wheel on the leftmost side, with its small resend LED below it.
    {
        const int wheelW = juce::jmax(24, juce::roundToInt(b.getWidth() * 0.035f));
        auto col = b.removeFromLeft(wheelW);
        b.removeFromLeft(8);
        const int toggleH = juce::jmax(10, juce::roundToInt(col.getWidth() * 0.6f));
        auto toggleArea = col.removeFromBottom(toggleH);
        wheel.setBounds(col.reduced(0, 1));
        resendToggle.setBounds(toggleArea.withSizeKeepingCentre(toggleH, toggleH));
    }

    // The Controller (AC1/AC2) group sits on the right; the source selector + six
    // depth knobs fill the rest.
    const int rightW = juce::jmax(150, juce::roundToInt(b.getWidth() * 0.24f));
    auto right = b.removeFromRight(rightW);
    b.removeFromRight(8);   // gap between the two groups

    // Left group: Controller Name across the top, six depth knobs below. The name
    // box is shrunk so its right edge sits just left of the FILT value box (3
    // columns), leaving the top strip above PMOD/AMOD/FMOD free for the LFO group
    // bracket drawn in paint().
    const int cellW = b.getWidth() / 6;
    auto nameStrip = b.removeFromTop(20);
    nameBox.setBounds(nameStrip.withWidth(3 * cellW - 6).reduced(0, 1));
    b.removeFromTop(4);
    for (int i = 0; i < 6; ++i)
        depthKnobs[(size_t) i]->setBounds(b.getX() + i * cellW, b.getY(), cellW, b.getHeight());

    // Right group: title, then two labelled CC pickers. The title band is taller
    // than the label rows so the larger (15f) module-matching title fits; this
    // also nudges the CC pickers below down slightly.
    ctrlTitle.setBounds(right.removeFromTop(22));
    right.removeFromTop(2);
    auto field = [&right]()
    {
        auto box   = right.removeFromTop(18);
        auto label = right.removeFromTop(14);
        right.removeFromTop(4);
        return std::make_pair(box, label);
    };
    auto [b1, l1] = field();
    ctrl1Box.setBounds(b1.reduced(0, 1));
    ctrl1Label.setBounds(l1);
    auto [b2, l2] = field();
    ctrl2Box.setBounds(b2.reduced(0, 1));
    ctrl2Label.setBounds(l2);
}
