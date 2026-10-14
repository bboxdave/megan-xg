// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "SystemPanel.h"
#include "EditorTextScale.h"
#include "EditorColours.h"
#include "XgKnob.h"
#include "ValueBox.h"
#include "FreqLadder.h"
#include "PluginProcessor.h"
#include "AutomationParams.h"
#include "codec/MU100AddressMap.h"
#include "codec/MU100Tooltips.h"

namespace
{
    // Factory defaults (from the factory dump / parameters.json):
    //   MASTER VOLUME     = 127      MASTER ATTENUATOR = 0
    //   MASTER TUNE       = 0x0400 (0 cent, the 4-nibble centre)
    //   TRANSPOSE         = 0x40   (0 semitones)
    constexpr int kVolDefault    = 127;
    constexpr int kAttDefault    = 0;
    constexpr int kTuneDefault   = 0x0400;   // combined nibble value = 0 cent
    constexpr int kKshiftDefault = 0x40;     // raw 64 = 0 semitones

    // MASTER TUNE: combined 0..2047, 0x0400 = 0 cent, 0.1 cent / step.
    constexpr int kTuneMin    = 0x0000;
    constexpr int kTuneMax    = 0x07FF;
    constexpr int kTuneCentre = 0x0400;

    // Lazily resolve an XG SYSTEM param's name -> AddressMap index.
    int sysParam(const juce::String& name)
    {
        return mu100::AddressMap::find(name.toRawUTF8(), "XG SYSTEM");
    }
}

//==============================================================================
SystemPanel::SystemPanel(YamahaMU100AudioProcessor& proc) : audioProcessor(proc)
{
    title.setText("System", juce::dontSendNotification);
    title.setFont(edtext::font(edtext::Title, 15.f, juce::Font::bold));
    title.setJustificationType(juce::Justification::centredRight);
    title.setColour(juce::Label::textColourId, edcol::kTextTitle);
    addAndMakeVisible(title);

    // System HPF -- plugin-only, no XG address, never in a .syx. Same ladder, format
    // and Off rule as the Part module's HPF2 box, deliberately: it is the same filter
    // one level up, and two spellings of "Off at 20 Hz" would be one too many.
    hpfBox = std::make_unique<ValueBox>();
    hpfBox->setTooltip(mu100::tooltips::forKey("PLUGIN|SYSTEM HPF"));
    hpfBox->setRange(0.0, 1000.0, 1.0);
    hpfBox->snapValue  = [](double v)                       { return freqladder::hpf2::snap(v); };
    hpfBox->dragValue  = [](double s, double dy, bool fine) { return freqladder::hpf2::drag(s, dy, fine); };
    hpfBox->wheelValue = [](double v, int dir, bool fine)   { return freqladder::hpf2::wheel(v, dir, fine); };
    hpfBox->setDefaultValue(0);
    hpfBox->setValueQuiet((double) audioProcessor.getSystemHpfHz());
    hpfBox->textFromValue = [](double dv) -> juce::String
    {
        return dv < (double) freqladder::kMin ? juce::String("Off") : freqladder::text(dv);
    };
    hpfBox->valueFromText = [](const juce::String& t) -> double
    {
        const juce::String s = t.trim();
        if (s.isEmpty() || s.equalsIgnoreCase("off")) return 0.0;
        return juce::jlimit(0.0, 1000.0, freqladder::parse(s));
    };
    hpfBox->onChange = [this](double v)
    {
        audioProcessor.setSystemHpfHz((float) v);
        if (onSystemHpfChanged) onSystemHpfChanged((int) v);   // the HPF2 screen's S bar
    };
    addAndMakeVisible(*hpfBox);

    hpfLabel.setText("HPF", juce::dontSendNotification);
    hpfLabel.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
    hpfLabel.setJustificationType(juce::Justification::centredLeft);
    hpfLabel.setColour(juce::Label::textColourId, edcol::kTextLabel);
    addAndMakeVisible(hpfLabel);

    volKnob    = std::make_unique<XgKnob>();
    attKnob    = std::make_unique<XgKnob>();
    tuneKnob   = std::make_unique<XgKnob>();
    kshiftKnob = std::make_unique<XgKnob>();
    tempoKnob  = std::make_unique<XgKnob>();

    // Wire a knob to transmit an XG SYSTEM param-change for the given name.
    auto wire = [this](XgKnob& k, const juce::String& name)
    {
        const int ix = sysParam(name);
        if (ix < 0) { k.onChange = nullptr; return; }
        // Throttled — a knob drag; see sendParamChangeThrottled.
        k.onChange = [this, ix](double v)
        {
            audioProcessor.sendParamChangeThrottled(
                mu100::AddressMap::makeParamChange(mu100::AddressMap::param(ix), 0, (uint32_t) v));
        };
    };

    // VOL — MASTER VOLUME, raw 0..127. Wired specially (not via `wire`) so it can
    // also notify the Graphic Display's Volume "S" bar of user edits.
    volKnob->setLabel("VOL");
    volKnob->setTooltip(mu100::tooltips::forParam("XG SYSTEM", "MASTER VOLUME"));
    volKnob->setRange(0.0, 127.0, 1.0);
    volKnob->setDefaultValue((double) kVolDefault);
    {
        const int volIx = sysParam("MASTER VOLUME");
        volKnob->onChange = [this, volIx](double v)
        {
            if (volIx >= 0)
                audioProcessor.sendParamChangeThrottled(mu100::AddressMap::makeParamChange(
                    mu100::AddressMap::param(volIx), 0, (uint32_t) v));
            if (onMasterVolumeChanged) onMasterVolumeChanged((int) v);
        };
    }

    // ATT — MASTER ATTENUATOR, raw 0..127.
    attKnob->setLabel("ATT");
    attKnob->setTooltip(mu100::tooltips::forParam("XG SYSTEM", "MASTER ATTENUATOR"));
    attKnob->setRange(0.0, 127.0, 1.0);
    attKnob->setDefaultValue((double) kAttDefault);
    wire(*attKnob, "MASTER ATTENUATOR");

    // TUNE — MASTER TUNE, shown in cents (-102.4 .. +102.3).
    tuneKnob->setLabel("TUNE");
    tuneKnob->setTooltip(mu100::tooltips::forParam("XG SYSTEM", "MASTER TUNE"));
    tuneKnob->setRange((double) kTuneMin, (double) kTuneMax, 1.0);
    tuneKnob->setDefaultValue((double) kTuneDefault);
    tuneKnob->setTextFromValue([](double dv)
    {
        const double cents = (std::lround(dv) - kTuneCentre) * 0.1;
        return (cents > 0 ? "+" : "") + juce::String(cents, 1);
    });
    tuneKnob->setValueFromText([](const juce::String& t)
    {
        return juce::jlimit((double) kTuneMin, (double) kTuneMax,
                            kTuneCentre + std::round(t.getDoubleValue() * 10.0));
    });
    // 2048 steps of 0.1 cent: on the generic value-box law (floored at 4 px/step)
    // crossing the range would take ~8200 px of drag, against 250 px on the dial.
    // Make the box proportional so both cover the range in one drag. Shift-fine and
    // the wheel are unchanged.
    tuneKnob->setValueBoxFullRangeDrag();
    // Wheel over the BOX moves a whole cent (10 raw steps) per detent — 0.1 cent a
    // detent is too slow to be useful across ±102 cents. Shift keeps the 0.1 grid.
    // The DIAL's wheel is deliberately untouched; it is JUCE's proportional rotary
    // wheel (~7 cents/detent here), not an interval step, so the two differ by design.
    tuneKnob->setValueBoxWheelStep(10.0, 1.0);
    wire(*tuneKnob, "MASTER TUNE");

    // KSHIFT — TRANSPOSE, raw 0x28..0x58 (40..88), 64 = 0, shown in semitones.
    kshiftKnob->setLabel("KSHIFT");
    kshiftKnob->setTooltip(mu100::tooltips::forParam("XG SYSTEM", "TRANSPOSE"));
    kshiftKnob->setRange(0x28, 0x58, 1.0);
    kshiftKnob->setDefaultValue((double) kKshiftDefault);
    kshiftKnob->setTextFromValue([](double dv)
    {
        const int semi = (int) std::lround(dv) - 0x40;
        return (semi > 0 ? "+" : "") + juce::String(semi);
    });
    kshiftKnob->setValueFromText([](const juce::String& t)
    {
        return juce::jlimit(40.0, 88.0, 0x40 + std::round(t.getDoubleValue()));
    });
    wire(*kshiftKnob, "TRANSPOSE");

    // TEMPO — 10.00..250.00 BPM, shown to 2 decimals. Standalone: user-set and
    // pushed to the processor (setSyncTempo); the effect tempo-sync knobs read it.
    // Hosted: read-only, showing the live DAW tempo (driven via setDisplayedTempo).
    tempoEditable = juce::JUCEApplicationBase::isStandaloneApp();
    tempoKnob->setLabel("TEMPO");
    tempoKnob->setTooltip(mu100::tooltips::forKey("PLUGIN|TEMPO"));   // plugin-only; author in tooltips.csv
    tempoKnob->setRange(10.0, 250.0, 0.1);   // 0.1 grid so shift-fine steps are representable
    tempoKnob->setDefaultValue(120.0);
    tempoKnob->setTextFromValue([](double v) { return juce::String(v, 2); });
    tempoKnob->setValueFromText([](const juce::String& t)
    {
        return juce::jlimit(10.0, 250.0, t.getDoubleValue());
    });
    // Drag / scroll (dial or value box) by whole BPM; Shift = fine 0.1 BPM steps.
    tempoKnob->setCoarseFineStep(1.0, 0.1);
    tempoKnob->setValueQuiet(audioProcessor.effectiveBpm());
    if (tempoEditable)
    {
        tempoKnob->onChange = [this](double v) { audioProcessor.setSyncTempo(v); };
    }
    else
    {
        tempoKnob->setInteractive(false);   // hosted: DAW tempo wins, no user edit
        startTimerHz(10);                   // mirror the live DAW tempo into the display
    }

    // An override bar/arc on TUNE and KSHIFT grows out of ZERO, not out of the raw
    // minimum: both are signed parameters whose rest point sits mid-range. VOL and
    // ATT really do start at nothing, so they keep the default (the minimum).
    tuneKnob->setBarOrigin((double) kTuneCentre);
    kshiftKnob->setBarOrigin((double) kKshiftDefault);

    for (auto* k : { volKnob.get(), attKnob.get(), tuneKnob.get(), kshiftKnob.get(), tempoKnob.get() })
    {
        k->setMaxDialSize(54);   // compact dials to suit the shortened System module
        // Room for a horizontal override bar, granted to the WHOLE row rather than
        // only to the four knobs a message can reach: a row where TEMPO sits four
        // pixels higher than its neighbours is worse than one reserving four pixels
        // it never fills. TEMPO simply leaves its strip empty.
        k->setOverrideMarkable(true);
        addAndMakeVisible(*k);
    }

    loadValues();
}

SystemPanel::~SystemPanel() { stopTimer(); }

void SystemPanel::timerCallback()
{
    // Hosted only (see ctor): keep the read-only TEMPO knob showing the DAW tempo.
    setDisplayedTempo(audioProcessor.effectiveBpm());
}

//==============================================================================
void SystemPanel::loadValues()
{
    using mu100::AddressMap;
    const auto& st = audioProcessor.getEditorState();

    auto load = [&](XgKnob& k, const juce::String& name, int dflt)
    {
        const int ix = sysParam(name);
        int v = dflt;
        if (ix >= 0)
        {
            const auto got = AddressMap::get(st, AddressMap::param(ix), 0);
            if (got.present) v = got.combined;
        }
        k.setValueQuiet((double) v);
    };

    load(*volKnob,    "MASTER VOLUME",     kVolDefault);
    load(*attKnob,    "MASTER ATTENUATOR", kAttDefault);
    load(*tuneKnob,   "MASTER TUNE",       kTuneDefault);
    load(*kshiftKnob, "TRANSPOSE",         kKshiftDefault);

    // The system HPF is PLUGIN-only, so it comes from the processor rather than from
    // the editor model the four knobs above read. Refreshing it here is what makes a
    // project restore, a file load and an undo all show the right number: every one of
    // those ends in applyState().
    if (hpfBox) hpfBox->setValueQuiet((double) audioProcessor.getSystemHpfHz());
}

void SystemPanel::applyState(const mu100::DeviceState&)
{
    loadValues();   // values live in the shared editor model; just re-read them
}

void SystemPanel::mirrorMasterVolume(int value)
{
    if (volKnob) volKnob->setValueQuiet((double) value);
}

void SystemPanel::setAutomationMarkers(int marker)
{
    auto mark = [marker](XgKnob* k, const char* paramName)
    {
        if (k == nullptr) return;
        k->setAutomationMarker(AutomationParams::isAutomated("XG SYSTEM", paramName)
                                   ? marker : XgKnob::MarkerNone);
    };

    mark(volKnob.get(),    "MASTER VOLUME");
    mark(attKnob.get(),    "MASTER ATTENUATOR");
    mark(kshiftKnob.get(), "TRANSPOSE");

    // TUNE / TEMPO have no host parameter. Cleared explicitly rather than
    // left alone, so turning a marker off actually clears every knob.
    if (tuneKnob)  tuneKnob->setAutomationMarker(XgKnob::MarkerNone);
    if (tempoKnob) tempoKnob->setAutomationMarker(XgKnob::MarkerNone);
}

//==============================================================================
// The MIDI-override marks for this module. The four device knobs are GLOBAL XG
// SYSTEM parameters, so instance 0 and no part to track -- which also means a mark
// here never goes stale on a part change the way the Part module's do.
//
// MASTER VOLUME is the one an incoming song is most likely to move: it is XG's
// master fader, every XG file's header tends to set it, and until now the knob went
// on showing the user's value while the device played the song's.
void SystemPanel::refreshMidiOverrides()
{
    const bool show = audioProcessor.showMidiOverrides;

    auto mark = [&](XgKnob* k, const juce::String& name)
    {
        if (k == nullptr) return;
        const int ix = show ? sysParam(name) : -1;
        const int v  = ix >= 0 ? audioProcessor.midiOverrideValue(ix, 0) : -1;
        k->setMidiOverride(v < 0 ? 0.0 : (double) v, v >= 0);
    };

    mark(volKnob.get(),    "MASTER VOLUME");
    mark(attKnob.get(),    "MASTER ATTENUATOR");
    mark(tuneKnob.get(),   "MASTER TUNE");
    mark(kshiftKnob.get(), "TRANSPOSE");
    // TEMPO is plugin-only and has no XG address, so nothing can override it.
}

// A style change moves LAYOUT, not just paint (see MidiOverrideMarks.h): the bar
// lives in a strip the value box is built taller for, and the dial gives up radius
// for the arc. Our resized() hands each knob the rectangle it already had, and
// setBounds with unchanged bounds never calls resized() -- so the knobs have to be
// told directly or they paint in the new style at the old split.
void SystemPanel::refreshMidiMarkStyle()
{
    for (auto* k : { volKnob.get(), attKnob.get(), tuneKnob.get(),
                     kshiftKnob.get(), tempoKnob.get() })
        if (k != nullptr) k->refreshMidiMarkStyle();
    resized();
    repaint();
}

void SystemPanel::setDisplayedTempo(double bpm)
{
    if (! tempoKnob) return;
    const double v = juce::jlimit(10.0, 250.0, bpm);
    if (std::abs(v - tempoKnob->getValue()) > 1e-6)
        tempoKnob->setValueQuiet(v);   // display only — never fires onChange
}

//==============================================================================
void SystemPanel::paint(juce::Graphics& g)
{
    g.setColour(edcol::kModuleBg);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.f);
    g.setColour(edcol::kModuleBorder);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.f, 1.f);
}

// Quiet mirror from the HPF2 screen's S bar. The processor already holds the value.
void SystemPanel::mirrorSystemHpf(int hz)
{
    if (hpfBox) hpfBox->setValueQuiet((double) hz);
}

// See the header: greys the box, never the value.
void SystemPanel::setPluginOnlyLocked(bool locked)
{
    if (pluginOnlyLocked == locked) return;
    pluginOnlyLocked = locked;
    if (hpfBox)
    {
        hpfBox->setEnabled(! locked);
        hpfLabel.setColour(juce::Label::textColourId,
                           locked ? edcol::kValueOff : edcol::kTextLabel);
    }
}

void SystemPanel::resized()
{
    auto b = getLocalBounds().reduced(6);

    // The title row carries the module name on the RIGHT (XGEdit style), which leaves
    // its left end free for the system HPF. Taken from the same strip the title uses,
    // so the two can never overlap however the module is scaled.
    auto titleRow = b.removeFromTop(18);
    {
        // Measured from the real font rather than fixed, like the Part module's cells:
        // edtext scales the fonts but not the boxes, so a hardcoded width ellipsizes
        // as the text-scale option grows.
        const juce::Font labFont (edtext::font(edtext::Label, 11.f, juce::Font::bold));
        const int labW = juce::GlyphArrangement::getStringWidthInt(labFont, "HPF") + 6;
        auto cell = titleRow.removeFromLeft(juce::jmin(titleRow.getWidth() / 2, 52 + labW));
        if (hpfBox) hpfBox->setBounds(cell.removeFromLeft(52).reduced(0, 1));
        cell.removeFromLeft(4);
        hpfLabel.setBounds(cell);
    }
    title.setBounds(titleRow);
    b.removeFromTop(2);

    // Keep the control row a fixed compact height and centre it vertically, so the
    // (now taller) module — its top raised to align with the Voice module — doesn't
    // spread each knob's value box / dial / label far apart.
    const int rowH = juce::jmin(b.getHeight(), 100);
    auto row = b.withSizeKeepingCentre(b.getWidth(), rowH);

    // Five knob cells: VOL, ATT, TUNE, KSHIFT, TEMPO.
    XgKnob* knobs[5] = { volKnob.get(), attKnob.get(), tuneKnob.get(), kshiftKnob.get(), tempoKnob.get() };
    const int cellW = row.getWidth() / 5;
    for (int i = 0; i < 5; ++i)
        knobs[i]->setBounds(row.getX() + i * cellW, row.getY(), cellW, row.getHeight());
}
