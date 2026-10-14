// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "EffectPanel.h"
#include "EditorTextScale.h"
#include "EditorColours.h"
#include "XgKnob.h"
#include "ValueBox.h"
#include "EffectAddressing.h"
#include "PluginProcessor.h"
#include "codec/MU100Effects.h"
#include "codec/MU100State.h"
#include "codec/MU100Tooltips.h"

namespace
{
    std::unique_ptr<XgKnob> makeCtl(const char* label)
    {
        auto k = std::make_unique<XgKnob>();
        k->setLabel(label);
        k->setRange(0.0, 127.0, 1.0);
        return k;
    }

    void initLabel(juce::Label& l, const char* txt)
    {
        l.setText(txt, juce::dontSendNotification);
        l.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
        l.setJustificationType(juce::Justification::centred);
        l.setColour(juce::Label::textColourId, edcol::kTextLabel);
    }
}

//==============================================================================
EffectPanel::EffectPanel(YamahaMU100AudioProcessor& proc) : audioProcessor(proc)
{
    title.setText("Effect", juce::dontSendNotification);
    title.setFont(edtext::font(edtext::Title, 15.f, juce::Font::bold));
    title.setJustificationType(juce::Justification::centredRight);
    title.setColour(juce::Label::textColourId, edcol::kTextTitle);
    addAndMakeVisible(title);

    typeBox.setItems({ "REVERB", "CHORUS", "VARIATION", "INSERTION1", "INSERTION2" }, 1);
    typeBox.setTooltip(mu100::tooltips::forKey("SEL|EFFECT TYPE"));
    typeBox.setSelectedIndex(0, false);
    typeBox.onSelect = [this](int idx)
    {
        unit = juce::jlimit(0, 4, idx);
        applyUnit();
        if (onUnitChanged) onUnitChanged(unit);
    };
    addAndMakeVisible(typeBox);

    initLabel(effectLabel, "EFFECT");
    effectLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(effectLabel);

    levelKnob = makeCtl("LEVEL");
    panKnob   = makeCtl("PAN");
    toRevKnob = makeCtl("ToREV");
    toChrKnob = makeCtl("ToCHR");
    partKnob  = makeCtl("PART");
    ctrlKnob  = makeCtl("CTRL");

    // PART is a knob: OFF, then 1..32 (raw 0x7F = off, else part-1).
    partKnob->setRange(0.0, 32.0, 1.0);
    partKnob->setTextFromValue([](double dv)
    {
        const int n = (int) std::lround(dv);
        return n <= 0 ? juce::String("OFF") : juce::String(n);
    });
    partKnob->setValueFromText([](const juce::String& t)
    {
        const auto s = t.trim().toUpperCase();
        if (s.startsWith("OFF")) return 0.0;
        return (double) juce::jlimit(0, 32, s.getIntValue());
    });

    // PAN: raw 0..127, 64 = centre, shown L64..C..R63 (no RND — unlike the
    // per-part PAN, the effect return pan has no random option).
    panKnob->setTextFromValue([](double dv) -> juce::String
    {
        const int v = (int) std::lround(dv) - 64;
        if (v == 0) return "C";
        return (v < 0 ? "L" + juce::String(-v) : "R" + juce::String(v));
    });
    panKnob->setValueFromText([](const juce::String& t) -> double
    {
        const auto s = t.trim().toUpperCase();
        if (s == "C")          return 64.0;
        if (s.startsWith("L")) return juce::jlimit(0.0,  64.0, 64.0 - s.substring(1).getDoubleValue());
        if (s.startsWith("R")) return juce::jlimit(64.0, 127.0, 64.0 + s.substring(1).getDoubleValue());
        return juce::jlimit(0.0, 127.0, 64.0 + t.trim().getDoubleValue());
    });

    // CTRL (control depth) is bipolar: raw 0..127, 64 = centre, shown -64..+63.
    ctrlKnob->setTextFromValue([](double dv)
    {
        const int s = (int) std::lround(dv) - 64;
        return (s > 0 ? "+" : "") + juce::String(s);
    });
    ctrlKnob->setValueFromText([](const juce::String& t)
    {
        return juce::jlimit(0.0, 127.0, 64.0 + t.trim().getIntValue());
    });
    // PAN and CTRL rest at 64; the sends and the return level rest at nothing, and
    // PART is an OFF-then-1..32 list with no centre.
    panKnob->setBarOrigin(64.0);
    ctrlKnob->setBarOrigin(64.0);

    for (auto* k : { levelKnob.get(), panKnob.get(), toRevKnob.get(),
                     toChrKnob.get(), partKnob.get(), ctrlKnob.get() })
    {
        // Room for a horizontal override bar. Granted to the whole row, including the
        // cells a given unit leaves bare -- a control row that changes height with the
        // selected unit would be worse than one reserving a few pixels it may not use.
        k->setOverrideMarkable(true);
        addAndMakeVisible(*k);
    }

    initLabel(conLabel, "CON");
    addAndMakeVisible(conLabel);

    conBox = std::make_unique<ValueBox>();
    conBox->setOptions({ "INS", "SYS" });
    conBox->setOverrideMarkable(true);   // it sits in the same row as the six knobs
    addAndMakeVisible(*conBox);

    addAndMakeVisible(routing);   // must exist before applyUnit() -> resized() places it

    applyUnit();
    refreshRouting();
    startTimerHz(10);             // see the note on timerCallback in the header
}

EffectPanel::~EffectPanel() = default;

//==============================================================================
// Read a control's stored raw value from the editor model (XG, current unit's
// block + the given address-low). Returns false if absent.
bool EffectPanel::modelByte(int low, int& out) const
{
    if (low < 0) return false;
    const auto b = fxaddr::block(unit);
    const uint32_t addr21 = ((uint32_t) b.aH << 14) | ((uint32_t) b.aM << 7) | (uint32_t) low;
    uint8_t v;
    if (! audioProcessor.getEditorState().getByte(mu100::ModelIx::XG, addr21, v))
        return false;
    out = v;
    return true;
}

// The override shadow's byte for one of this unit's controls, by raw address. The
// twin of modelByte above: same address arithmetic, the other of the two states.
bool EffectPanel::overrideByte(int low, int& out) const
{
    if (low < 0) return false;
    const auto b = fxaddr::block(unit);
    const uint32_t addr21 = ((uint32_t) b.aH << 14) | ((uint32_t) b.aM << 7) | (uint32_t) low;
    uint8_t v;
    if (! audioProcessor.midiOverrideByteAt(mu100::ModelIx::XG, addr21, v))
        return false;
    out = v;
    return true;
}

//==============================================================================
// The MIDI-override marks for the unit currently on screen. Every control here is a
// single byte in the unit's block, so the shadow is read by address -- there is no
// parameter index to ask midiOverrideValue with (see EffectAddressing.h).
//
// A control the current unit does not have takes low < 0 and so clears its mark,
// which is what keeps a mark from surviving a switch to a unit that cannot raise it.
void EffectPanel::refreshMidiOverrides()
{
    const bool show = audioProcessor.showMidiOverrides;
    const auto c = fxaddr::controls(unit);

    auto mark = [&](XgKnob* k, int low)
    {
        if (k == nullptr) return;
        int v = 0;
        const bool on = show && overrideByte(low, v);
        k->setMidiOverride(on ? (double) v : 0.0, on);
    };

    mark(levelKnob.get(), show ? c.level : -1);
    mark(panKnob.get(),   show ? c.pan   : -1);
    mark(toRevKnob.get(), show ? c.toRev : -1);
    mark(toChrKnob.get(), show ? c.toChr : -1);
    mark(ctrlKnob.get(),  show ? c.control : -1);

    // PART is shown as OFF, 1..32 while the device stores 0x7F for off and part-1
    // otherwise, so the raw byte has to be mapped the way wirePartKnob maps it or the
    // mark would land 1 step low -- or, for OFF, off the end of the dial entirely.
    if (partKnob != nullptr)
    {
        int raw = 0;
        const bool on = show && overrideByte(c.part, raw);
        const double v = (raw >= 0x7F) ? 0.0 : (double) (raw + 1);
        partKnob->setMidiOverride(on ? v : 0.0, on);
    }

    if (conBox != nullptr)
    {
        int v = 0;
        const bool on = show && overrideByte(c.connection, v);
        conBox->setMidiOverride(on ? (double) v : 0.0, on);
    }
}

// See SystemPanel::refreshMidiMarkStyle. The CON box needs the panel's own resized()
// (its strip comes out of the cell this panel lays out), the knobs need telling
// directly because their bounds do not change.
void EffectPanel::refreshMidiMarkStyle()
{
    for (auto* k : { levelKnob.get(), panKnob.get(), toRevKnob.get(),
                     toChrKnob.get(), partKnob.get(), ctrlKnob.get() })
        if (k != nullptr) k->refreshMidiMarkStyle();
    resized();
    repaint();
}

void EffectPanel::wireKnob(XgKnob& k, int low, int dflt)
{
    if (low < 0) { k.setBare(true); return; }
    k.setBare(false);
    k.setDefaultValue(dflt);
    int val = dflt;
    modelByte(low, val);            // restore the edited/device value if we have one
    k.setValueQuiet(val);
    const int u = unit, lo = low;
    k.onChange = [this, u, lo](double v)
    {
        audioProcessor.sendParamChange(fxaddr::setControl(u, lo, (int) v));
    };
}

// Wire the return LEVEL knob like any other control, then chain its onChange so a
// user edit also notifies the Graphic Display's "S" bar (only when the knob is
// actually active: reverb/chorus always, variation only when CON = SYS).
void EffectPanel::wireLevelKnob(int low)
{
    wireKnob(*levelKnob, low, 64);
    if (low < 0) return;
    auto base = levelKnob->onChange;
    const int u = unit;
    levelKnob->onChange = [this, u, base](double v)
    {
        if (base) base(v);
        if (onReturnLevelChanged) onReturnLevelChanged(u, (int) std::lround(v));
    };
}

void EffectPanel::wirePartKnob(int low)
{
    partKnob->setBare(false);
    partKnob->setDefaultValue(0);   // OFF
    int raw = 0x7F;                 // model stores raw (0x7F = off, else part-1)
    modelByte(low, raw);
    partKnob->setValueQuiet(raw == 0x7F ? 0 : raw + 1);   // display: OFF / part number
    const int u = unit, lo = low;
    partKnob->onChange = [this, u, lo](double v)
    {
        const int sel = (int) std::lround(v);        // 0 = OFF, else part number
        const int rawOut = (sel <= 0) ? 0x7F : (sel - 1);
        audioProcessor.sendParamChange(fxaddr::setControl(u, lo, rawOut));
    };
}

// Wire the controls whose activity depends on the Variation routing (CON):
//   * CON = SYS (system): the variation is a system effect, so the return
//     controls LEVEL / PAN / ToREV / ToCHR apply, but PART / CTRL do not.
//   * CON = INS (insertion): the variation behaves like an insertion effect, so
//     PART / CTRL apply and the return controls do not (matching the Insertion
//     units, which never expose the return controls).
// Reverb / Chorus / Insertion are not CON-gated — they wire straight from the map.
void EffectPanel::refreshConnState()
{
    const auto c = fxaddr::controls(unit);

    if (unit == mu100fx::FX_VARIATION && c.connection >= 0)
    {
        const bool sys = conBox->getValue() >= 0.5;   // SYS routing
        wireLevelKnob(sys ? c.level : -1);
        wireKnob(*panKnob,   sys ? c.pan   : -1, 64);
        wireKnob(*toRevKnob, sys ? c.toRev : -1, 0);
        wireKnob(*toChrKnob, sys ? c.toChr : -1, 0);

        const bool ins = ! sys;                       // INS routing
        if (ins && c.part >= 0) wirePartKnob(c.part); else partKnob->setBare(true);
        wireKnob(*ctrlKnob, ins ? c.control : -1, 64);
        applyTooltips();
        return;
    }

    wireLevelKnob(c.level);
    wireKnob(*panKnob,   c.pan,   64);
    wireKnob(*toRevKnob, c.toRev, 0);
    wireKnob(*toChrKnob, c.toChr, 0);
    if (c.part >= 0) wirePartKnob(c.part); else partKnob->setBare(true);
    wireKnob(*ctrlKnob, c.control, 64);
    applyTooltips();
}

// Hover text follows the same activity rules as the wiring above: a control that
// is bare for this unit/routing has no tooltip, so an inert slot stays silent.
void EffectPanel::applyTooltips()
{
    const auto c = fxaddr::controls(unit);
    const bool sys = c.connection < 0 || conBox->getValue() >= 0.5;
    const bool returns = unit != mu100fx::FX_VARIATION || sys;
    const bool ins     = unit != mu100fx::FX_VARIATION || ! sys;

    auto tip = [](XgKnob& k, bool active, int low, const char* key)
    {
        k.setTooltip(active && low >= 0 ? juce::String(mu100::tooltips::forKey(key))
                                        : juce::String());
    };
    tip(*levelKnob, returns, c.level,   "EFFECT|LEVEL");
    tip(*panKnob,   returns, c.pan,     "EFFECT|PAN");
    tip(*toRevKnob, returns, c.toRev,   "EFFECT|TOREV");
    tip(*toChrKnob, returns, c.toChr,   "EFFECT|TOCHR");
    tip(*partKnob,  ins,     c.part,    "EFFECT|PART");
    tip(*ctrlKnob,  ins,     c.control, "EFFECT|CTRL");

    // CON exists on the Variation unit alone (it IS the routing choice), so it is
    // silent wherever it is not shown — same rule as a bare knob above.
    conBox->setTooltip(c.connection >= 0 ? juce::String(mu100::tooltips::forKey("EFFECT|CON"))
                                         : juce::String());
}

void EffectPanel::applyUnit()
{
    const auto c = fxaddr::controls(unit);

    // CON (connection: INS/SYS) — variation only; toggling it re-evaluates which
    // controls are active for the new routing.
    const bool hasCon = c.connection >= 0;
    conLabel.setVisible(hasCon);
    conBox->setVisible(hasCon);
    if (hasCon)
    {
        int con = 0;                // INS = insertion (default)
        modelByte(c.connection, con);
        conBox->setValueQuiet(con != 0 ? 1 : 0);
        const int u = unit, lo = c.connection;
        conBox->onChange = [this, u, lo](double v)
        {
            audioProcessor.sendParamChange(fxaddr::setControl(u, lo, (int) v));  // 0 INS, 1 SYS
            refreshConnState();
            if (onConnectionChanged) onConnectionChanged();   // D/W gating in Effect Parameters
            // The Variation "S" bar is active only when CON = SYS — tell the Graphic Display.
            if (onVariationSystemStateChanged)
                onVariationSystemStateChanged((int) v != 0, variationSystemLevel());
        };
    }

    refreshConnState();
    resized();
    // The controls have just been re-pointed at a unit, and a unit change moves no
    // override -- so nothing else would clear a mark belonging to the unit we left.
    refreshMidiOverrides();
}

// After a device-state read-back, re-wire the current unit so every control
// reflects the freshly-loaded model (wireKnob/wirePartKnob/CON all restore from it).
void EffectPanel::applyState(const mu100::DeviceState&)
{
    applyUnit();
    refreshRouting();   // the poll would catch this within 100 ms, but a read-back is
                        // the one moment the whole panel visibly reloads -- do not
                        // leave the strip a tick behind the knobs beside it.
    // The read-back may have changed the Variation routing/return — refresh the
    // Graphic Display's Variation "S" bar gate + value.
    if (onVariationSystemStateChanged)
        onVariationSystemStateChanged(variationIsSystem(), variationSystemLevel());
}

//==============================================================================
// Variation system-return sync (Graphic Display "S" bar). These read/write the
// Variation block directly (block 02 01) regardless of which unit is currently
// shown, because the Variation "S" bar must reflect the variation routing even
// while the Effect module is displaying a different unit.
bool EffectPanel::unitByte(int u, int low, int& out) const
{
    if (low < 0) return false;
    const auto b = fxaddr::block(u);
    const uint32_t addr21 = ((uint32_t) b.aH << 14) | ((uint32_t) b.aM << 7) | (uint32_t) low;
    uint8_t v;
    if (! audioProcessor.getEditorState().getByte(mu100::ModelIx::XG, addr21, v))
        return false;
    out = v;
    return true;
}

bool EffectPanel::varByte(int low, int& out) const
{
    return unitByte(mu100fx::FX_VARIATION, low, out);
}

bool EffectPanel::variationIsSystem() const
{
    const auto c = fxaddr::controls(mu100fx::FX_VARIATION);
    int con = 0;                       // default INS (insertion)
    varByte(c.connection, con);
    return con != 0;                   // 0 = INS, 1 = SYS
}

int EffectPanel::variationSystemLevel() const
{
    const auto c = fxaddr::controls(mu100fx::FX_VARIATION);
    int lvl = 64;
    varByte(c.level, lvl);
    return lvl;
}

void EffectPanel::mirrorReturnLevel(int unitArg, int value)
{
    if (unit == unitArg && levelKnob) levelKnob->setValueQuiet((double) value);
}

void EffectPanel::setVariationSystemLevel(int value)
{
    const auto c = fxaddr::controls(mu100fx::FX_VARIATION);
    if (c.level < 0) return;
    audioProcessor.sendParamChange(fxaddr::setControl(mu100fx::FX_VARIATION, c.level, value));
    if (unit == mu100fx::FX_VARIATION && levelKnob) levelKnob->setValueQuiet((double) value);
}

//==============================================================================
// The routing strip's data. Every value is read from the EDITOR MODEL for the unit
// it describes, not from this panel's controls -- the strip has to be right about
// Insertion 2 while the panel is showing Reverb, which is the whole point of it.
//
// The fallbacks match what the controls beside it would show for the same missing
// byte: wirePartKnob defaults a part to 0x7F (OFF) and applyUnit defaults CON to 0
// (INS). Before the first read-back or seed that is a GUESS in both places, but it
// is the SAME guess, so the strip can never contradict the PART knob under it.
juce::String EffectPanel::unitTypeName(int u) const
{
    int msb = -1, lsb = -1;
    const int lo = (int) fxaddr::typeLowMsb(u);
    if (! unitByte(u, lo, msb) || ! unitByte(u, lo + 1, lsb)) return {};

    int count = 0;
    const auto* types = mu100fx::unitTypes(u, count);
    for (int i = 0; i < count; ++i)
        if ((int) types[i].msb == msb && (int) types[i].lsb == lsb)
            return juce::String(types[i].name);
    return {};
}

void EffectPanel::refreshRouting()
{
    // Raw part byte -> the PART knob's display number: 0x7F = OFF, else part-1.
    auto partOf = [this](int u)
    {
        const auto c = fxaddr::controls(u);
        int raw = 0x7F;
        unitByte(u, c.part, raw);
        return raw == 0x7F ? 0 : juce::jlimit(1, 32, raw + 1);
    };

    EffectRoutingStrip::Model m;
    m.varPart  = partOf(mu100fx::FX_VARIATION);
    m.ins1Part = partOf(mu100fx::FX_INSERTION1);
    m.ins2Part = partOf(mu100fx::FX_INSERTION2);

    int con = 0;                                   // 0 = INS, 1 = SYS (applyUnit's default)
    varByte(fxaddr::controls(mu100fx::FX_VARIATION).connection, con);
    m.varIsSystem = con != 0;

    // Names for the tooltip. Only the three units the strip shows -- naming Reverb
    // and Chorus there would describe something that is not on screen.
    m.varName  = unitTypeName(mu100fx::FX_VARIATION);
    m.ins1Name = unitTypeName(mu100fx::FX_INSERTION1);
    m.ins2Name = unitTypeName(mu100fx::FX_INSERTION2);

    routing.setModel(m);   // early-outs unless something actually moved
}

//==============================================================================
void EffectPanel::paint(juce::Graphics& g)
{
    g.setColour(edcol::kModuleBg);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.f);
    g.setColour(edcol::kModuleBorder);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.f, 1.f);
}

void EffectPanel::resized()
{
    auto b = getLocalBounds().reduced(6);

    auto top = b.removeFromTop(20);
    typeBox.setBounds(top.removeFromLeft(110).reduced(0, 1));

    // The routing strip is paid for out of the top strip's SLACK: "Effect" and
    // "EFFECT" were each given a fixed box far wider than their text, and both are
    // justified AWAY from the space they waste (title centredRight, effectLabel
    // centredLeft), so trimming each to its own glyph width moves neither one pixel.
    // The old widths stay as the CEILING, so a large text scale clips exactly where
    // it always did rather than eating into the strip. The strip takes whatever is
    // left; its labels are fixed (see EffectRoutingStrip.h), so this is headroom
    // against a large text scale rather than something it grows to fill.
    const auto titleFont = edtext::font(edtext::Title, 15.f, juce::Font::bold);
    const auto labelFont = edtext::font(edtext::Label, 11.f, juce::Font::bold);
    const int titleW = juce::jmin(80, juce::GlyphArrangement::getStringWidthInt(titleFont, title.getText()) + 4);
    const int labelW = juce::jmin(70, juce::GlyphArrangement::getStringWidthInt(labelFont, effectLabel.getText()) + 12);

    title.setBounds(top.removeFromRight(titleW));
    effectLabel.setBounds(top.removeFromLeft(labelW).reduced(4, 0));
    top.removeFromLeft(8);         // breathing room after the EFFECT caption
    top.removeFromRight(8);        // ...and before the module title
    routing.setBounds(top);

    b.removeFromTop(4);

    // Seven control cells: LEVEL PAN ToREV ToCHR CON PART CTRL.
    const int n = 7;
    const int cellW = b.getWidth() / n;
    auto cellAt = [&](int i) { return juce::Rectangle<int>(b.getX() + i * cellW, b.getY(), cellW, b.getHeight()); };

    levelKnob->setBounds(cellAt(0));
    panKnob  ->setBounds(cellAt(1));
    toRevKnob->setBounds(cellAt(2));
    toChrKnob->setBounds(cellAt(3));

    // CON cell: value box on top (aligned with the knob value boxes), label below.
    {
        auto cell = cellAt(4).reduced(3, 0);
        // 16 px as it has always been, plus the override bar's strip where the style
        // draws one -- the box itself is the one place that decides (see ValueBox).
        conBox->setBounds(cell.removeFromTop(16 + conBox->overrideStripHeight()));
        conLabel.setBounds(cell.removeFromTop(14));
    }
    partKnob->setBounds(cellAt(5));
    ctrlKnob->setBounds(cellAt(6));
}
