// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "MultiEqPanel.h"
#include "EditorTextScale.h"
#include "EditorColours.h"
#include "XgKnob.h"
#include "PluginProcessor.h"
#include "codec/MU100AddressMap.h"
#include "codec/MU100Tooltips.h"

namespace
{
    // XG "EQ Frequency" conversion table (parameters table#3): raw data value ->
    // displayed centre frequency. Indexed by the raw 0..60 value. The Multi EQ
    // bands use sub-ranges of this (band 1 = raw 4..40 = 32Hz..2.0k, mid bands =
    // 14..54 = 100..10.0k, band 5 = 28..58 = 500..16.0k). Kept here so the Hz
    // readout matches the hardware exactly (same table the Part-EQ uses).
    const char* const kEqFreqTable[] = {
        "THRU", "22", "25", "28", "32", "36", "40", "45", "50", "56", "63", "70",
        "80", "90", "100", "110", "125", "140", "160", "180", "200", "225", "250",
        "280", "315", "355", "400", "450", "500", "560", "630", "700", "800", "900",
        "1.0k", "1.1k", "1.2k", "1.4k", "1.6k", "1.8k", "2.0k", "2.2k", "2.5k",
        "2.8k", "3.2k", "3.6k", "4.0k", "4.5k", "5.0k", "5.6k", "6.3k", "7.0k",
        "8.0k", "9.0k", "10.0k", "11.0k", "12.0k", "14.0k", "16.0k", "18.0k", "THRU"
    };
    constexpr int kEqFreqCount = (int) (sizeof(kEqFreqTable) / sizeof(kEqFreqTable[0]));

    juce::String eqFreqText(double dv)
    {
        return kEqFreqTable[juce::jlimit(0, kEqFreqCount - 1, (int) std::lround(dv))];
    }

    double eqFreqValue(const juce::String& t, int lo, int hi)
    {
        auto s = t.trim().toLowerCase();
        double hz = s.upToFirstOccurrenceOf("k", false, true)
                     .retainCharacters("0123456789.").getDoubleValue();
        if (s.contains("k")) hz *= 1000.0;
        if (hz <= 0.0) return (double) lo;

        int best = lo; double bestErr = 1e18;
        for (int i = juce::jmax(1, lo); i <= juce::jmin(kEqFreqCount - 2, hi); ++i)
        {
            const juce::String e = kEqFreqTable[i];
            double v = e.upToFirstOccurrenceOf("k", false, true).getDoubleValue();
            if (e.contains("k")) v *= 1000.0;
            const double err = std::abs(v - hz);
            if (err < bestErr) { bestErr = err; best = i; }
        }
        return (double) best;
    }

    // Per-band factory defaults (from the factory dump): GAIN = 0dB for all bands,
    // FREQ differs per band, Q = 0.7, SHAPE = shelving. (Matches the FLAT macro.)
    const int kFreqDefault[5] = { 0x0C, 0x1C, 0x22, 0x2E, 0x34 };

    // The five XG MULTI EQ macro tables, from XGEdit's factory settings
    // (mu100editor/reference/xgedit multi eq settings.csv). For each macro
    // (FLAT/JAZZ/POPS/ROCK/CLASSIC) the five bands give the raw device values:
    //   GAIN  — 0x40 (64) = 0 dB, 1 dB / step.
    //   FREQ  — index into kEqFreqTable (e.g. 80 Hz = 12, 1.0k = 34).
    //   Q     — raw = 10 x displayed (0.7 -> 7, 2.0 -> 20).
    //   SHAPE — 0 = shelving, 1 = peaking (only bands 1 & 5 carry it).
    // Selecting a macro loads these straight into the band editor with no device
    // read-back, mirroring XGEdit.
    struct EqBandDefault { int gain, freq, q, shape; };
    const EqBandDefault kEqMacros[5][5] = {
        // FLAT
        { {64,12,7,0}, {64,28,7,0}, {64,34,7,0}, {64,46,7,0},  {64,52,7,0} },
        // JAZZ
        { {58, 8,7,0}, {66,16,3,0}, {68,33,3,0}, {60,44,5,0},  {58,50,7,0} },
        // POPS
        { {68,16,7,0}, {60,24,20,0},{67,34,7,0}, {60,40,20,0}, {70,48,7,0} },
        // ROCK
        { {71,16,7,0}, {68,20,7,0}, {60,36,5,0}, {68,41,10,0}, {66,50,7,0} },
        // CLASSIC
        { {67,12,7,0}, {68,24,7,0}, {64,34,5,0}, {66,50,7,0},  {61,52,7,0} },
    };

    // The five XG MULTI EQ macros, named ONCE. The program box lists them, the undo
    // label quotes one, and the override mark has to name the one the DEVICE is on.
    const char* const kMacroNames[5] = { "FLAT", "JAZZ", "POPS", "ROCK", "CLASSIC" };

    juce::String macroName(int idx)
    {
        return (idx >= 0 && idx < 5) ? juce::String(kMacroNames[idx]) : juce::String();
    }

    // Lazily resolve a MULTI EQ param's name -> AddressMap index.
    int eqParam(const juce::String& name)
    {
        return mu100::AddressMap::find(name.toRawUTF8(), "MULTI EQ");
    }
}

//==============================================================================
MultiEqPanel::MultiEqPanel(YamahaMU100AudioProcessor& proc) : audioProcessor(proc)
{
    title.setText("Multi EQ", juce::dontSendNotification);
    title.setFont(edtext::font(edtext::Title, 15.f, juce::Font::bold));
    title.setJustificationType(juce::Justification::centredRight);
    title.setColour(juce::Label::textColourId, edcol::kTextTitle);
    addAndMakeVisible(title);

    // Program list-box — selecting a macro sets EQ TYPE on the device and then
    // loads that macro's factory per-band values (kEqMacros) into the band editor
    // and the device, so the GAIN/FREQ/Q/SHAPE knobs show the type's defaults
    // immediately with no device read-back required.
    programBox.setItems({ kMacroNames[0], kMacroNames[1], kMacroNames[2],
                          kMacroNames[3], kMacroNames[4] }, 1);
    programBox.setTooltip(mu100::tooltips::forKey("MULTI EQ|PROGRAM"));
    programBox.setSelectedIndex(0, false);
    programBox.setDefaultIndex(0);          // FLAT = factory default
    programBox.setShowChangedTab(true);
    programBox.onSelect = [this](int idx)
    {
        // ONE undo step. Picking a program writes EQ TYPE and then every band's
        // GAIN/FREQ/Q/SHAPE from the macro — 22 messages — so without a scope the
        // history filled with individual band writes ("EQ SHAPE1 SHELF -> SHELF")
        // and undoing a program pick meant twenty-two clicks through half-applied
        // curves. The gesture key folds a wheel-scroll run over the box into one
        // step, exactly as the effect program box does.
        YamahaMU100AudioProcessor::UndoScope undoStep(
            audioProcessor,
            "Multi EQ " + macroName(juce::jlimit(0, 4, idx)),
            mu100undo::gestureMergeKey(mu100undo::GestureMultiEqProgram, 0));

        const int ix = eqParam("EQ TYPE");
        if (ix >= 0)
            audioProcessor.sendParamChange(
                mu100::AddressMap::makeParamChange(mu100::AddressMap::param(ix), 0, (uint32_t) idx));
        applyMacro(idx);
    };
    addAndMakeVisible(programBox);

    bandKnob  = std::make_unique<XgKnob>();
    gainKnob  = std::make_unique<XgKnob>();
    freqKnob  = std::make_unique<XgKnob>();
    qKnob     = std::make_unique<XgKnob>();
    shapeKnob = std::make_unique<XgKnob>();

    bandKnob->setTooltip(mu100::tooltips::forKey("MULTI EQ|BAND"));
    gainKnob->setTooltip(mu100::tooltips::forKey("MULTI EQ|GAIN"));
    freqKnob->setTooltip(mu100::tooltips::forKey("MULTI EQ|FREQ"));
    qKnob->setTooltip(mu100::tooltips::forKey("MULTI EQ|Q"));
    shapeKnob->setTooltip(mu100::tooltips::forKey("MULTI EQ|SHAPE"));

    // BAND — local selection (1..5), drives which band the other knobs edit.
    bandKnob->setLabel("BAND");
    bandKnob->setRange(1.0, 5.0, 1.0);
    bandKnob->setDefaultValue(1.0);
    bandKnob->setValueQuiet(1.0);
    bandKnob->onChange = [this](double v) { selectBand((int) std::lround(v)); };

    // GAIN — raw 0x34..0x4C (52..76), 64 = 0 dB, 1 dB / step, span -12..+12 dB.
    gainKnob->setLabel("GAIN");
    gainKnob->setRange(52.0, 76.0, 1.0);
    gainKnob->setTextFromValue([](double dv)
    {
        const int db = (int) std::lround(dv) - 64;
        return (db > 0 ? "+" : "") + juce::String(db) + "dB";
    });
    gainKnob->setValueFromText([](const juce::String& t)
    {
        return juce::jlimit(52.0, 76.0, 64.0 + t.upToFirstOccurrenceOf("d", false, true).getDoubleValue());
    });

    // FREQ — Hz via the EQ-frequency table; range is set per band in selectBand().
    freqKnob->setLabel("FREQ");
    freqKnob->setTextFromValue([](double dv) { return eqFreqText(dv); });

    // Q — raw 1..120 shown 0.1..12.0.
    qKnob->setLabel("Q");
    qKnob->setRange(1.0, 120.0, 1.0);
    qKnob->setDefaultValue(7.0);
    qKnob->setTextFromValue([](double dv) { return juce::String(std::lround(dv) / 10.0, 1); });
    qKnob->setValueFromText([](const juce::String& t)
    {
        return juce::jlimit(1.0, 120.0, std::round(t.getDoubleValue() * 10.0));
    });

    // SHAPE — shelving / peaking (bands 1 & 5 only); factory default is SHELF (0).
    shapeKnob->setLabel("SHAPE");
    shapeKnob->setRange(0.0, 1.0, 1.0);
    shapeKnob->setDefaultValue(0.0);
    shapeKnob->setTextFromValue([](double dv) { return std::lround(dv) == 0 ? "SHELF" : "PEAK"; });
    shapeKnob->setValueFromText([](const juce::String& t)
    {
        return t.trim().toUpperCase().startsWith("PEAK") ? 1.0 : 0.0;
    });

    // GAIN is the one bipolar control here: 64 is 0 dB, so an override bar or arc grows
    // out of centre. The others really do start at nothing.
    gainKnob->setBarOrigin(64.0);

    for (auto* k : { bandKnob.get(), gainKnob.get(), freqKnob.get(), qKnob.get(), shapeKnob.get() })
    {
        k->setMaxDialSize(54);   // compact dials to suit the shortened Multi EQ module
        // Room for a horizontal override bar, granted to the whole row (see the note in
        // SystemPanel): BAND is a local selection and simply leaves its strip empty.
        k->setOverrideMarkable(true);
        addAndMakeVisible(*k);
    }

    selectBand(1);
}

MultiEqPanel::~MultiEqPanel() = default;

//==============================================================================
// Point GAIN/FREQ/Q/SHAPE at band `band1to5` (1..5): set each knob's onChange to
// transmit that band's MULTI EQ param, fix the band-specific FREQ range, mark
// SHAPE bare on the mid bands, then load the band's current values.
void MultiEqPanel::selectBand(int band1to5)
{
    using mu100::AddressMap;
    currentBand = juce::jlimit(0, 4, band1to5 - 1);
    const int n = currentBand + 1;   // 1..5

    auto wire = [this](XgKnob& k, int ix)
    {
        if (ix < 0) { k.onChange = nullptr; return; }
        // Throttled — a knob drag, see sendParamChangeThrottled. The EQ TYPE selector
        // and applyMacro() below stay immediate: the macro's band values must follow
        // the type change, and the immediate path drains pending edits first.
        k.onChange = [this, ix](double v)
        {
            audioProcessor.sendParamChangeThrottled(
                AddressMap::makeParamChange(AddressMap::param(ix), 0, (uint32_t) v));
        };
    };

    const int gainIx  = eqParam("EQ GAIN" + juce::String(n));
    const int freqIx  = eqParam("EQ FREQUENCY" + juce::String(n));
    const int qIx     = eqParam("EQ Q" + juce::String(n));
    const int shapeIx = (n == 1 || n == 5) ? eqParam("EQ SHAPE" + juce::String(n)) : -1;

    // FREQ range is band-dependent (low band starts at 32 Hz, high band at 500 Hz).
    int lo = 4, hi = 58;
    if (freqIx >= 0) { lo = AddressMap::param(freqIx).rangeLo; hi = AddressMap::param(freqIx).rangeHi; }
    freqKnob->setRange((double) lo, (double) hi, 1.0);
    freqKnob->setDefaultValue((double) kFreqDefault[currentBand]);
    freqKnob->setValueFromText([lo, hi](const juce::String& t) { return eqFreqValue(t, lo, hi); });

    gainKnob->setDefaultValue(64.0);

    wire(*gainKnob,  gainIx);
    wire(*freqKnob,  freqIx);
    wire(*qKnob,     qIx);
    wire(*shapeKnob, shapeIx);
    shapeKnob->setBare(shapeIx < 0);

    loadBandValues();
}

void MultiEqPanel::loadBandValues()
{
    using mu100::AddressMap;
    const auto& st = audioProcessor.getEditorState();
    const int n = currentBand + 1;

    auto load = [&](XgKnob& k, const juce::String& name, int dflt)
    {
        const int ix = eqParam(name);
        int v = dflt;
        if (ix >= 0)
        {
            const auto got = AddressMap::get(st, AddressMap::param(ix), 0);
            if (got.present) v = got.combined;
        }
        k.setValueQuiet((double) v);
    };

    load(*gainKnob, "EQ GAIN" + juce::String(n),      64);
    load(*freqKnob, "EQ FREQUENCY" + juce::String(n), kFreqDefault[currentBand]);
    load(*qKnob,    "EQ Q" + juce::String(n),          7);
    if (n == 1 || n == 5) load(*shapeKnob, "EQ SHAPE" + juce::String(n), 0);

    // The knobs have just been re-pointed (or re-read), so the marks must follow in
    // the same call -- a band change moves no override, so nothing else would fire.
    refreshMidiOverrides();
}

// Push the factory band values for macro `macroIdx` (0..4) to the device and fold
// them into the editor model, then refresh the currently-shown band's knobs. This
// is what lets a freshly-selected EQ type display its defaults without a read-back.
void MultiEqPanel::applyMacro(int macroIdx)
{
    using mu100::AddressMap;
    if (macroIdx < 0 || macroIdx > 4) return;
    const auto& bands = kEqMacros[macroIdx];

    auto send = [this](const juce::String& name, int v)
    {
        const int ix = eqParam(name);
        if (ix >= 0)
            audioProcessor.sendParamChange(
                AddressMap::makeParamChange(AddressMap::param(ix), 0, (uint32_t) v));
    };

    for (int n = 1; n <= 5; ++n)
    {
        const auto& bd = bands[n - 1];
        send("EQ GAIN" + juce::String(n),      bd.gain);
        send("EQ FREQUENCY" + juce::String(n), bd.freq);
        send("EQ Q" + juce::String(n),         bd.q);
        if (n == 1 || n == 5)
            send("EQ SHAPE" + juce::String(n), bd.shape);
    }

    loadBandValues();   // re-read the shown band from the now-updated model
}

//==============================================================================
// The MIDI-override marks. MULTI EQ is global, so instance 0 throughout -- but the
// four editable knobs are re-pointed at one band at a time, so the marks have to
// follow `currentBand` exactly as loadBandValues does, or they would report a band
// the user is not looking at.
//
// No XG controller reaches these; a mark here always means a SysEx parameter change.
void MultiEqPanel::refreshMidiOverrides()
{
    const bool show = audioProcessor.showMidiOverrides;
    const int  n    = currentBand + 1;

    auto mark = [&](XgKnob* k, const juce::String& name)
    {
        if (k == nullptr) return;
        const int ix = show ? eqParam(name) : -1;
        const int v  = ix >= 0 ? audioProcessor.midiOverrideValue(ix, 0) : -1;
        k->setMidiOverride(v < 0 ? 0.0 : (double) v, v >= 0);
    };

    mark(gainKnob.get(), "EQ GAIN" + juce::String(n));
    mark(freqKnob.get(), "EQ FREQUENCY" + juce::String(n));
    mark(qKnob.get(),    "EQ Q" + juce::String(n));
    // SHAPE exists on the two shelving bands only; on a mid band it is bare, and the
    // empty name resolves to -1, which clears the mark rather than leaving a stale one.
    mark(shapeKnob.get(), (n == 1 || n == 5) ? "EQ SHAPE" + juce::String(n)
                                             : juce::String());
    // BAND is a local selection with no address, so nothing can override it.

    // The PROGRAM box. EQ TYPE is one byte naming a macro, so the mark is a tab and
    // the macro's name goes to the tooltip -- the box can only show one name and that
    // name is the user's. Note picking a macro also writes all five bands, so a type
    // override leaves the band knobs describing YOUR macro while the device is on
    // another; those knobs mark individually, and this is the piece that says why.
    {
        const int ix = show ? eqParam("EQ TYPE") : -1;
        const int v  = ix >= 0 ? audioProcessor.midiOverrideValue(ix, 0) : -1;
        programBox.setMidiOverride(v >= 0, macroName(v));
    }
}

// See SystemPanel::refreshMidiMarkStyle -- a style change moves the knobs' internal
// split, and our resized() hands them bounds they already had.
void MultiEqPanel::refreshMidiMarkStyle()
{
    for (auto* k : { bandKnob.get(), gainKnob.get(), freqKnob.get(),
                     qKnob.get(), shapeKnob.get() })
        if (k != nullptr) k->refreshMidiMarkStyle();
    resized();
    repaint();
}

void MultiEqPanel::applyState(const mu100::DeviceState& st)
{
    using mu100::AddressMap;
    const int ix = eqParam("EQ TYPE");
    if (ix >= 0)
    {
        const auto got = AddressMap::get(st, AddressMap::param(ix), 0);
        if (got.present) programBox.setSelectedIndex(got.combined, false);
    }
    loadBandValues();
}

//==============================================================================
void MultiEqPanel::paint(juce::Graphics& g)
{
    g.setColour(edcol::kModuleBg);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.f);
    g.setColour(edcol::kModuleBorder);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.f, 1.f);
}

void MultiEqPanel::resized()
{
    auto b = getLocalBounds().reduced(6);

    auto top = b.removeFromTop(20);
    programBox.setBounds(top.removeFromLeft(130).reduced(0, 1));
    title.setBounds(top.removeFromRight(110));
    b.removeFromTop(4);

    // Five knob cells: BAND GAIN FREQ Q SHAPE.
    XgKnob* knobs[5] = { bandKnob.get(), gainKnob.get(), freqKnob.get(), qKnob.get(), shapeKnob.get() };
    const int cellW = b.getWidth() / 5;
    for (int i = 0; i < 5; ++i)
        knobs[i]->setBounds(b.getX() + i * cellW, b.getY(), cellW, b.getHeight());
}
