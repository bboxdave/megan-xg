// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "GraphicDisplayPanel.h"
#include "EditorColours.h"
#include "EditorTextScale.h"
#include "FreqLadder.h"
#include "PluginProcessor.h"
#include "PartPanel.h"
#include "codec/MU100AddressMap.h"
#include "codec/MU100Tooltips.h"
#include "AutomationParams.h"   // which screens drive a DAW parameter (the markers)
#include "XgKnob.h"             // the marker painter, shared with the knobs
#include "MidiIndicator.h"
#include <complex>
#include <cmath>
#include <vector>

namespace
{
    // Per-mode wiring. partParam = MULTI PART name (per-part bar). sParam/sTable =
    // the leftmost "S" system value (nullptr = inert: no XG system-level value).
    struct ModeSpec
    {
        const char* label;
        const char* partParam;   // MULTI PART, or nullptr for EG modes
        int         partDefault;
        const char* sParam;      // system master/return, or nullptr
        const char* sTable;
        int         sDefault;
    };

    // Indexed by Mode (Volume, Pan, AmpEG, PitchEG, Reverb, Chorus, Var).
    const ModeSpec kModes[] = {
        { "VOLUME",   "VOLUME",         100, "MASTER VOLUME", "XG SYSTEM", 127 },
        { "PAN",      "PAN",             64, nullptr,         nullptr,      64 },
        { "AMP EG",   nullptr,           64, nullptr,         nullptr,       0 },
        { "PITCH EG", nullptr,           64, nullptr,         nullptr,       0 },
        { "REVERB",   "REVERB SEND",     40, "REVERB RETURN", "EFFECT 1",   64 },
        { "CHORUS",   "CHORUS SEND",      0, "CHORUS RETURN", "EFFECT 1",   64 },
        { "VAR",      "VARIATION SEND",   0, nullptr,         nullptr,      64 },
        // EQ: the composite-response node editor over the Part module's HPF2 + 5 EQ
        // bands. No AddressMap param (values live in the Part boxes / processor,
        // reached via the eqReadRaw/eqWriteRaw bridge); no system ("S") value.
        { "EQ",       nullptr,            0, nullptr,         nullptr,       0 },
        // HPF2: plugin-only per-part clean HPF. No AddressMap param -- the parts come
        // from the processor's cleanHpfHz and the "S" bar from its systemHpfHz, so both
        // sParam and partParam stay null and sBarActive() special-cases this mode.
        { "HPF2",     nullptr,            0, nullptr,         nullptr,       0 },
    };

    // Colours. Every one of these is a REFERENCE into the themed palette, never a
    // copy: these are namespace-scope objects built at static-init time, long before
    // a theme is chosen, so a copy would freeze whichever theme happened to be the
    // declared default and the light theme would never reach this panel.
    const juce::Colour& kLcdBg      = edcol::kLcdBg;
    const juce::Colour& kLcdBorder  = edcol::kLcdBorder;
    const juce::Colour& kBar        = edcol::kBar;
    const juce::Colour& kBarInert   = edcol::kBarInert;
    const juce::Colour& kHi         = edcol::kChanged;  // selected / changed: the shared editor yellow (single source)
    const juce::Colour& kDash       = edcol::kDash;
    const juce::Colour& kCorner     = edcol::kCorner;
    const juce::Colour& kAxis       = edcol::kAxis;
    // Mode-button text, also used by the EQ screen's RANGE options so "what is
    // selected" looks the same everywhere on this panel.
    const juce::Colour& kBtnTextOff = edcol::kBtnTextOff;    // unselected
    const juce::Colour& kBtnTextOn  = edcol::kSelectedText;  // selected (green), shared with the History window
    const juce::Colour& kAxisZero   = edcol::kOrange;        // orange 10/20/30 markers
    const juce::Colour& kEgLine     = edcol::kEgLine;
    const juce::Colour& kEgFixed    = edcol::kEgFixed;
    const juce::Colour& kEgEdit     = edcol::kEgEdit;
    const juce::Colour& kReadout    = edcol::kReadout;
    // EQ analyser trace: dimmer than the EG/response lines so it reads as background
    // information under the curve rather than competing with it.
    const juce::Colour& kSpecLine   = edcol::kSpecLine;
    // VOLUME screen meters — the fixed green/yellow/orange/red ramp up the bar.
    const juce::Colour& kMeterLow   = edcol::kMeterLow;
    const juce::Colour& kMeterMid   = edcol::kMeterMid;
    const juce::Colour& kMeterHigh  = edcol::kMeterHigh;
    const juce::Colour& kMeterPeak  = edcol::kMeterPeak;

    void drawDashedH(juce::Graphics& g, float x0, float x1, float y, juce::Colour c)
    {
        g.setColour(c);
        const float dash[] = { 4.0f, 4.0f };
        juce::Line<float> line(x0, y, x1, y);
        g.drawDashedLine(line, dash, 2, 1.0f);
    }

    //==========================================================================
    // Value readouts (the numbers beside the EG points, EQ nodes and bars — both
    // the always-on EG ones and the hover/drag popups). They share the GrDisValue
    // edtext category, so the LCD's numbers size independently of the value boxes
    // in the surrounding modules, and sit a point above a box's text by default.
    //
    // Their boxes are derived from the FONT, never fixed design pixels: edtext
    // scales fonts but NOT cells, so a hardcoded height clips the text as soon as
    // editorTextGrDisValuePct grows (the trap the Part module's band labels dodge
    // by measuring). +2 keeps descenders off the box edge.
    //==========================================================================
    juce::Font  grValueFont()                  { return juce::Font(edtext::font(edtext::GrDisValue, 11.0f)); }
    float       grValueH(const juce::Font& f)  { return std::ceil(f.getHeight()) + 2.0f; }
    float       grValueW(const juce::Font& f, const juce::String& t, float minW)
    {
        return juce::jmax(minW, (float) juce::GlyphArrangement::getStringWidthInt(f, t) + 8.0f);
    }

    //==========================================================================
    // EQ screen support — a faithful composite frequency-response curve.
    //==========================================================================

    // Display frequency span (log x-axis). 20 Hz..16 kHz: the labelled decades
    // 20/100/1k/10k land where the Bitwig reference puts them, leaving the 10-16 kHz
    // strip in the unmarked space past the "10k" tick.
    const double kEqFsHz    = 44100.0;   // MUST match swp30.cpp's filter sample rate
    const double kEqFMin    = 20.0;
    const double kEqFMax    = 16000.0;
    // The ± dB a band's GAIN raw 0..127 spans. This is the PARAMETER's range — NOT the
    // display's vertical range, which the RANGE selector zooms between ±12 and ±24
    // (see eqRangeDb). They share the default 12, so keep the two uses distinct: this
    // one converts raw <-> dB, eqRangeDb only decides how the axis is drawn.
    const double kEqBandGainMaxDb = 12.0;

    // A FREQ cell's value -> Hz. The shelves hold an XG table index and the plugin-only
    // mid bands hold Hz outright, so PartPanel (which owns the boxes) does the telling.
    double eqRawToHz(int band, int raw) { return PartPanel::eqFreqToHz(band, raw); }

    // Frequency drag for the ladder-backed nodes (HPF2 + the peaking mid bands).
    //   plain : log sweep — `dx` px moves `startHz` by that fraction of the range's
    //           octave span, so half the plot covers the whole range (the feel the old
    //           linear raw-index drag had).
    //   Shift : steps the ladder one entry per kPixelsPerFineStep, exactly like the Part
    //           module's frequency boxes. A proportional fine multiplier can't do this —
    //           being relative to the value, it overshoots the 1 Hz steps in the middle
    //           of the range while crawling below 100 Hz — so this takes the Shift state
    //           itself rather than the 0.22x factor the gain/Q axes use.
    // The result is snapped to the ladder and clamped to [lo, hi]; anything under the
    // 20 Hz floor collapses to `lo`, which for HPF2 (lo = 0) means Off.
    int eqDragFreq(double startHz, float dx, float perX, bool fine, int lo, int hi)
    {
        double hz;
        if (fine)
        {
            hz = freqladder::drag(startHz, (double) dx, true);
        }
        else
        {
            const double floorHz = juce::jmax((double) lo, (double) freqladder::kMin);
            const double octSpan = std::log2((double) hi / floorHz);
            hz = startHz * std::pow(2.0, (double) (dx / perX) * octSpan);
        }
        if (hz < (double) freqladder::kMin) return lo;
        return juce::jlimit(lo, hi, freqladder::snap(hz));
    }

    // A normalised biquad (a0 folded in), matching swp30.cpp's coefficient layout:
    //   H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
    struct Biquad { double b0, b1, b2, a1, a2; bool active; };

    double biquadMagDb(const Biquad& c, double f)
    {
        if (! c.active) return 0.0;
        const double w = 2.0 * juce::MathConstants<double>::pi * f / kEqFsHz;
        const std::complex<double> z1 = std::polar(1.0, -w);
        const std::complex<double> z2 = z1 * z1;
        const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
        const std::complex<double> den = 1.0  + c.a1 * z1 + c.a2 * z2;
        const double mag = std::abs(num) / std::abs(den);
        return 20.0 * std::log10(juce::jmax(1.0e-9, mag));
    }

    // RBJ peaking EQ — identical to swp30_device::eq_make_coef (the plugin mid bands).
    Biquad peakingCoef(double gdb, double f, double q)
    {
        Biquad c {};
        if (std::abs(gdb) < 0.05 || f < 20.0 || q <= 0.0) { c.active = false; return c; }
        if (f > kEqFsHz * 0.45) f = kEqFsHz * 0.45;
        const double A = std::pow(10.0, gdb / 40.0);
        const double w0 = 2.0 * juce::MathConstants<double>::pi * f / kEqFsHz;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1.0 + alpha / A;
        c.b0 = (1.0 + alpha * A) / a0;
        c.b1 = (-2.0 * cw)       / a0;
        c.b2 = (1.0 - alpha * A) / a0;
        c.a1 = (-2.0 * cw)       / a0;
        c.a2 = (1.0 - alpha / A) / a0;
        c.active = true;
        return c;
    }

    // 2-pole Butterworth high-pass — identical to swp30_device::hpf_make_coef (HPF2).
    Biquad hpfCoef(double hz)
    {
        Biquad c {};
        if (hz < 20.0) { c.active = false; return c; }
        if (hz > kEqFsHz * 0.45) hz = kEqFsHz * 0.45;
        const double w0 = 2.0 * juce::MathConstants<double>::pi * hz / kEqFsHz;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * 0.70710678118);
        const double a0 = 1.0 + alpha;
        c.b0 = ((1.0 + cw) / 2.0) / a0;
        c.b1 = (-(1.0 + cw))      / a0;
        c.b2 = ((1.0 + cw) / 2.0) / a0;
        c.a1 = (-2.0 * cw)        / a0;
        c.a2 = (1.0 - alpha)      / a0;
        c.active = true;
        return c;
    }

    // RBJ low/high shelf (Q ~= 0.707) — a visual approximation of the XG BASS/TREBLE
    // shelves (those run in SWP30 hardware, so there is no plugin coefficient set to
    // mirror exactly; this matches their gain/corner and reads correctly on the graph).
    Biquad shelfCoef(double gdb, double f, bool high)
    {
        Biquad c {};
        if (std::abs(gdb) < 0.05 || f < 20.0) { c.active = false; return c; }
        if (f > kEqFsHz * 0.45) f = kEqFsHz * 0.45;
        const double A = std::pow(10.0, gdb / 40.0);
        const double w0 = 2.0 * juce::MathConstants<double>::pi * f / kEqFsHz;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * 0.70710678118);
        const double tsa = 2.0 * std::sqrt(A) * alpha;
        double b0, b1, b2, a0, a1, a2;
        if (! high)
        {
            b0 =      A * ((A + 1) - (A - 1) * cw + tsa);
            b1 =  2 * A * ((A - 1) - (A + 1) * cw);
            b2 =      A * ((A + 1) - (A - 1) * cw - tsa);
            a0 =          (A + 1) + (A - 1) * cw + tsa;
            a1 =     -2 * ((A - 1) + (A + 1) * cw);
            a2 =          (A + 1) + (A - 1) * cw - tsa;
        }
        else
        {
            b0 =      A * ((A + 1) + (A - 1) * cw + tsa);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 =      A * ((A + 1) + (A - 1) * cw - tsa);
            a0 =          (A + 1) - (A - 1) * cw + tsa;
            a1 =      2 * ((A - 1) - (A + 1) * cw);
            a2 =          (A + 1) - (A - 1) * cw - tsa;
        }
        c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0;
        c.a1 = a1 / a0; c.a2 = a2 / a0;
        c.active = true;
        return c;
    }
}

//==============================================================================
GraphicDisplayPanel::GraphicDisplayPanel(YamahaMU100AudioProcessor& proc)
    : audioProcessor(proc)
{
    using mu100::AddressMap;

    for (auto& band : eqOverride) { band[0] = -1; band[1] = -1; }

    for (int m = 0; m < NumModes; ++m)
    {
        const auto& spec = kModes[m];

        partParamIdx[m] = spec.partParam ? AddressMap::find(spec.partParam, "MULTI PART") : -1;
        sParamIdx[m]    = spec.sParam    ? AddressMap::find(spec.sParam, spec.sTable)      : -1;

        sVal[m] = spec.sDefault;
        for (int p = 0; p < 32; ++p) partVal[m][p] = spec.partDefault;
        // -1 = "the device agrees", which is what an editor with nothing on its input
        // must start from. Zero-initialising would put a tick at 0 on every column.
        for (int i = 0; i < kBarStops; ++i) barOverride[m][i] = -1;

        auto b = std::make_unique<ScreenButton>(spec.label);
        {
            // Plus the controller the screen's PART values record as in MIDI CC mode
            // (the same table the output path uses), after " - " so the head stays the
            // accessible name. AMP EG's two come from its part attack and release.
            juce::String tip = mu100::tooltips::forKey(std::string("DISP|") + spec.label);
            const int cc = YamahaMU100AudioProcessor::midiCcForMultiPartParam(spec.partParam);
            if (cc >= 0)
                tip << " - records as MIDI CC " << cc << " in MIDI CC mode";
            else if (m == AmpEG)
                tip << " - a part's attack and release record as MIDI CC "
                    << YamahaMU100AudioProcessor::midiCcForMultiPartParam("EG ATTACK TIME")
                    << " and "
                    << YamahaMU100AudioProcessor::midiCcForMultiPartParam("EG RELEASE TIME")
                    << " in MIDI CC mode";
            b->setTooltip(tip);
        }
        b->setClickingTogglesState(true);
        // These ARE keyboard stops (keynav walks the screen buttons), so unlike the
        // rest of the plugin's buttons they must hold focus -- a screen reader reads
        // what is focused. FocusableButton is what makes that safe: it declines every
        // key, so Alt+Return still reaches the shortcut handler. See its comment.
        b->setRadioGroupId(7001);
        b->setColour(juce::TextButton::buttonColourId,   edcol::kButtonBg);
        b->setColour(juce::TextButton::buttonOnColourId, edcol::kButtonBg);
        b->setColour(juce::TextButton::textColourOffId,  kBtnTextOff);
        b->setColour(juce::TextButton::textColourOnId,   kBtnTextOn);   // active = green
        const int mm = m;
        b->onClick = [this, mm] { setMode(mm); };
        addAndMakeVisible(*b);
        buttons[(size_t) m] = std::move(b);
    }
    buttons[(size_t) Volume]->setToggleState(true, juce::dontSendNotification);

    meterDb.fill(kMeterFloorDb);

    ampParam[0] = AddressMap::find("EG ATTACK TIME",  "MULTI PART");
    ampParam[1] = AddressMap::find("EG DECAY TIME",   "MULTI PART");
    ampParam[2] = AddressMap::find("EG RELEASE TIME", "MULTI PART");

    // Drum-edit AMP EG -> DRUM SETUP attack / decay1 / decay2 rates.
    drumAmpParam[0] = AddressMap::find("EG ATTACK RATE", "DRUM SETUP");
    drumAmpParam[1] = AddressMap::find("EG DECAY1 RATE", "DRUM SETUP");
    drumAmpParam[2] = AddressMap::find("EG DECAY2 RATE", "DRUM SETUP");

    pitchParam[0] = AddressMap::find("PITCH EG INITIAL LEVEL", "MULTI PART");
    pitchParam[1] = AddressMap::find("PITCH EG ATTACK TIME",   "MULTI PART");
    pitchParam[2] = AddressMap::find("PITCH EG RELEASE LEVEL", "MULTI PART");
    pitchParam[3] = AddressMap::find("PITCH EG RELEASE TIME",  "MULTI PART");

    for (int p = 0; p < 32; ++p)
    {
        ampVal[p][0] = ampVal[p][1] = ampVal[p][2] = 64;
        pitchVal[p][0] = pitchVal[p][1] = pitchVal[p][2] = pitchVal[p][3] = 64;
    }
}

GraphicDisplayPanel::~GraphicDisplayPanel()
{
    // The probe is NOT self-disabling: without this the sound thread keeps summing
    // voices into a ring nobody will ever read again. The processor's
    // releaseResources() is only a backstop for teardown paths that skip this one.
    spectrumTimer.stopTimer();
    audioProcessor.setSpectrumTarget(false, 0, -1);
    // Same for the VOLUME meters: the mixer keeps metering, and processBlock keeps
    // scanning the output buffer, until somebody says stop.
    meterTimer.stopTimer();
    audioProcessor.setPartLevelMeter(false);
    // The gesture lives in the PROCESSOR, which outlives this panel. Closing the
    // editor mid-drag would otherwise leave its depth stuck above zero and fold every
    // later edit, for the rest of the session, into one undo step.
    endDragUndo();
}

// The editor swaps between the Hardware photo and the Editor view, and the EQ
// screen is only one mode of one panel inside the latter — so becoming hidden is
// just as much a reason to stop analysing as switching mode is.
void GraphicDisplayPanel::visibilityChanged()
{
    updateSpectrumTarget();
    updateMeterTarget();
}

void GraphicDisplayPanel::refreshSpectrumOptions()
{
    updateSpectrumTarget();
    updateMeterTarget();
    // The VOLUME screen draws its bars differently with meters on, so it needs the
    // repaint whether or not a frame has landed yet.
    if (mode == EQ || mode == Volume) repaint();
}

void GraphicDisplayPanel::parentHierarchyChanged()
{
    // isShowing() also depends on every ancestor's visibility, which changes without
    // this component's own visibility flag ever moving (the view toggle does exactly
    // that). Without this hook the analyser would keep running behind the Hardware view.
    updateSpectrumTarget();
}

int GraphicDisplayPanel::modeForChannel(Channel ch)
{
    switch (ch)
    {
        case Channel::Volume: return Volume;
        case Channel::Pan:    return Pan;
        case Channel::Reverb: return Reverb;
        case Channel::Chorus: return Chorus;
        case Channel::Var:    return Var;
    }
    return Volume;
}

GraphicDisplayPanel::Channel GraphicDisplayPanel::channelForMode(int m)
{
    switch (m)
    {
        case Pan:    return Channel::Pan;
        case Reverb: return Channel::Reverb;
        case Chorus: return Channel::Chorus;
        case Var:    return Channel::Var;
        default:     return Channel::Volume;   // Volume (and any non-synced fallthrough)
    }
}

void GraphicDisplayPanel::selectMode(int m)
{
    if (m < 0 || m >= NumModes) return;
    setMode(m);
}

void GraphicDisplayPanel::setMode(int m)
{
    if (m == mode) { repaint(); return; }
    endDragUndo();
    dragging = false; dragIdx = -1;
    hoverIdx = -1; hoverShown = false; transientIdx = -1;
    eqHoverNode = -1;
    if (timerJob != TimerNone) { stopTimer(); timerJob = TimerNone; }
    mode = m;
    buttons[(size_t) m]->setToggleState(true, juce::dontSendNotification);
    updateSpectrumTarget();             // the analyser runs on the EQ screen only
    updateMeterTarget();                // ...and the meters on the VOLUME screen only
    repaint();
}

// "Disable plugin-only parameters" — see the header. The HPF2 screen has nothing on
// it but a plugin-only parameter, so the whole mode button goes; the EQ screen keeps
// working for the hardware shelves with its plugin-only handles greyed out.
void GraphicDisplayPanel::setPluginOnlyLocked(bool locked)
{
    if (locked == pluginOnlyLocked) return;
    pluginOnlyLocked = locked;

    if (auto* b = buttons[(size_t) HPF2].get())
        b->setEnabled(! locked);

    // Don't strand the user on a screen whose button they can no longer press.
    // setMode also drops any in-flight drag/hover on the way out.
    if (locked && mode == HPF2) setMode(Volume);
    else                        repaint();

    updateScreenMarkers();   // a disabled HPF2 button must not advertise an edit
}

//==============================================================================
// "Mark DAW-automatable parameters" on the screen buttons.
//
// ONE mark per screen, on its button -- not on each bar or node. The same call the
// Part module makes for its EQ GAIN row (one mark on the row label, not five on the
// boxes): the button is what names the screen's contents collectively, and 33 marks
// across a bar screen would bury the drawing the screen exists to show.
//
// Asked of AutomationParams rather than listed, like PartPanel's knob row, so a
// parameter added or dropped there moves its screen's mark with no edit here.
bool GraphicDisplayPanel::screenIsAutomated(int m) const
{
    switch (m)
    {
        // The drum key's ATTACK / DECAY1 / DECAY2 lanes. A PART's amp EG has no
        // host parameter, so this screen is marked only while it edits a drum key --
        // an actual KEY, not merely the mode: armed with no key picked (or an empty
        // one), drumKey is -1, sendAmp drops every write and egStops offers no node,
        // so a mark would advertise an edit that cannot be made. USER-REPORTED
        // 2026-10-04 (the mark appeared the moment the DRUM KEY box was armed).
        case AmpEG:
            // (The names are the ones drumAmpParam[] is found by, in the ctor.)
            if (! drumEditActive || drumKey < 0) return false;
            for (const char* name : { "EG ATTACK RATE", "EG DECAY1 RATE", "EG DECAY2 RATE" })
                if (AutomationParams::isAutomated("DRUM SETUP", name)) return true;
            return false;

        // Not in drum-edit mode, although the drum key's HPF2 node IS automatable
        // there (USER'S CALL, 2026-09-25): one live node out of six would read as
        // "this screen is automatable" and send the user dragging the five that are
        // not. That HPF2 stays marked where it has a control of its own -- the Part
        // module's HPF2 box, which retargets to the key in the same mode.
        case EQ:
            return ! drumEditActive
                && (AutomationParams::isEqGainRowAutomated()
                    || AutomationParams::isAutomated("PLUGIN", "HPF2"));

        // Per-part plugin-only cutoffs (HPF201..HPF232). The button is disabled
        // under the plugin-only lock, and a greyed button must not advertise an edit.
        case HPF2:
            return AutomationParams::isAutomated("PLUGIN", "HPF2") && ! pluginOnlyLocked;

        // The bar screens: marked when the PART bars are lanes (VOLUME, PAN, the three
        // sends). They stay part bars in drum-edit mode, so the mode does not enter.
        // PITCH EG has no partParam and no lanes, and falls out here.
        default:
            return kModes[m].partParam != nullptr
                && AutomationParams::isAutomated("MULTI PART", kModes[m].partParam);
    }
}

// The MIDI CC mark (underline) on a screen button: the screen's PART values go out
// as a controller in MIDI CC mode. Same one-mark-per-screen reasoning as above.
//   * the bar screens whose part parameter has a controller (VOLUME, PAN, REVERB,
//     CHORUS, VAR). They stay PART bars in drum-edit mode, so the mode does not enter.
//   * AMP EG in PART mode only: a part's attack and release have controllers (CC73 /
//     CC72; its decay does not -- CC75 was measured dead). In drum-edit mode the
//     screen edits a DRUM SETUP key, which has none. This is exactly the opposite
//     condition to AMP EG's DAW triangle, so the two never share that button.
//   * not VAR while the variation is an insertion (channelInert).
bool GraphicDisplayPanel::screenSendsMidiCc(int m) const
{
    if (m == AmpEG)
        return ! drumEditActive;
    // An inert channel (VAR with the variation routed as an insertion) has no
    // per-part send to record -- the same gate the Part module's VAR knob gets.
    if (channelInert(m))
        return false;
    return kModes[m].partParam != nullptr
        && YamahaMU100AudioProcessor::midiCcForMultiPartParam(kModes[m].partParam) >= 0;
}

void GraphicDisplayPanel::updateScreenMarkers()
{
    for (int m = 0; m < NumModes; ++m)
        if (auto* b = buttons[(size_t) m].get())
        {
            b->setAutomationMarker(screenIsAutomated(m) ? automationMarker
                                                        : XgKnob::MarkerNone);
            b->setCcMarker(ccMarks && screenSendsMidiCc(m));
        }
}

void GraphicDisplayPanel::setAutomationMarkers(int marker, bool cc)
{
    automationMarker = marker;
    ccMarks = cc;
    updateScreenMarkers();
}

void GraphicDisplayPanel::ScreenButton::setAutomationMarker(int marker)
{
    if (autoMarker == marker) return;
    autoMarker = marker;
    repaint();
}

void GraphicDisplayPanel::ScreenButton::setCcMarker(bool on)
{
    if (ccMarker == on) return;
    ccMarker = on;
    repaint();
}

// Unmarked buttons are drawn by the LookAndFeel, untouched. A TRIANGLE needs room
// the LookAndFeel does not leave: its text is centred across the whole button and
// these cells are narrow, so a triangle drawn before it would land on the border.
// So a marked button lays its own text out (an underline needs no room, and takes
// the same path with none reserved, so the two styles share one layout) -- the same
// font, indents, colour and fitting as LookAndFeel_V2::drawButtonText (which V4
// inherits), but centring triangle + text as ONE group, the way XgKnob shifts its
// label. `drawFittedText` still squeezes the text if the group cannot fit.
void GraphicDisplayPanel::ScreenButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    if (autoMarker == XgKnob::MarkerNone && ! ccMarker)
    {
        FocusableButton::paintButton(g, highlighted, down);
        return;
    }

    auto& lf = getLookAndFeel();
    lf.drawButtonBackground(g, *this,
                            findColour(getToggleState() ? buttonOnColourId : buttonColourId),
                            highlighted, down);

    const auto font   = lf.getTextButtonFont(*this, getHeight());
    const auto colour = findColour(getToggleState() ? textColourOnId : textColourOffId)
                            .withMultipliedAlpha(isEnabled() ? 1.0f : 0.5f);

    // LookAndFeel_V2::drawButtonText's box, verbatim.
    const int yIndent     = juce::jmin(4, proportionOfHeight(0.3f));
    const int cornerSize  = juce::jmin(getHeight(), getWidth()) / 2;
    const int fontHeight  = juce::roundToInt(font.getHeight() * 0.6f);
    const int leftIndent  = juce::jmin(fontHeight, 2 + cornerSize / (isConnectedOnLeft()  ? 4 : 2));
    const int rightIndent = juce::jmin(fontHeight, 2 + cornerSize / (isConnectedOnRight() ? 4 : 2));
    const float boxW      = (float) (getWidth() - leftIndent - rightIndent);
    if (boxW <= 0.0f) return;

    // The triangle's footprint, from the same font-derived sizes XgKnob uses.
    const float triH = juce::jmax(4.0f, font.getHeight() * 0.62f);
    const float lead = (autoMarker == XgKnob::MarkerTriangle)
                           ? triH * 0.866f + juce::jmax(2.0f, triH * 0.28f) : 0.0f;

    const float textW    = juce::jmin(boxW - lead,
                                      juce::GlyphArrangement::getStringWidth(font, getButtonText()));
    const float textLeft = (float) leftIndent + lead + (boxW - lead - textW) * 0.5f;

    g.setFont(font);
    g.setColour(colour);
    g.drawFittedText(getButtonText(),
                     juce::roundToInt(textLeft), yIndent,
                     juce::roundToInt(textW) + 1, getHeight() - yIndent * 2,
                     juce::Justification::centred, 2);

    // The underline drops 1.5 design px further here than under a knob's label
    // (USER-REPORTED 2026-09-25: it sat too close to the letters). The button font
    // is larger than a label's and the rule's offset below the baseline is scaled
    // from the font's descent, which this font keeps small -- so the knob spacing,
    // reused as is, crowded the glyphs. The triangle is unaffected.
    //
    // Two independent marks since 2026-10-04: the DAW triangle (autoMarker) and the
    // MIDI CC underline (ccMarker). Either, both or neither; the layout above already
    // reserved the triangle's lead only when there is one.
    if (autoMarker == XgKnob::MarkerTriangle)
        XgKnob::paintAutomationMarkerAt(g, font, textLeft, textW, getHeight() * 0.5f,
                                        colour, XgKnob::MarkerTriangle);
    if (ccMarker)
        XgKnob::paintAutomationMarkerAt(g, font, textLeft, textW, getHeight() * 0.5f + 1.5f,
                                        colour, XgKnob::MarkerUnderline);
}

void GraphicDisplayPanel::setCurrentPart(int part0to31)
{
    const int p = juce::jlimit(0, 31, part0to31);
    if (p == currentPart) return;
    currentPart = p;
    updateSpectrumTarget();   // follow the part the screen now describes
    // Every screen follows the part now: the EG screens because they DESCRIBE it,
    // the bar screens because their x-axis ruler carries the selected-part marker.
    // This used to skip the bar modes, which was correct only while nothing on a
    // bar screen depended on which part was selected.
    repaint();
}

//==============================================================================
// Geometry
//==============================================================================
juce::Rectangle<float> GraphicDisplayPanel::displayRect() const
{
    auto b = getLocalBounds().toFloat();
    const float buttonH = juce::jlimit(16.0f, 40.0f, b.getHeight() * 0.12f);
    b.removeFromTop(buttonH + 4.0f);
    return b;
}

// Widest label any mode puts in the left gutter: the EQ rails at either RANGE, a
// bar mode's full scale, HPF2's ladder. Measured rather than reasoned about, so
// whichever wins at a given font/scale is the one that sizes the gutter.
float GraphicDisplayPanel::axisGutterW() const
{
    const juce::Font f (edtext::font(edtext::Label, 11.0f));
    static const char* const kLabels[] = { "127", "+12", "-12", "+24", "-24", "250", "1k" };
    int w = 0;
    for (auto* t : kLabels)
        w = juce::jmax(w, juce::GlyphArrangement::getStringWidthInt(f, t));
    return juce::jmax(28.0f, (float) w + kAxisLabelGap);
}

float GraphicDisplayPanel::axisLabelH() const
{
    const juce::Font f (edtext::font(edtext::Label, 11.0f));
    return juce::jmax(14.0f, std::ceil(f.getHeight()) + 2.0f);
}

juce::Rectangle<float> GraphicDisplayPanel::barPlot(juce::Rectangle<float> plot) const
{
    return plot.withTrimmedTop(axisLabelH());
}

juce::Rectangle<float> GraphicDisplayPanel::axisRowRect() const
{
    if (! isBarMode(mode)) return {};
    const auto plot = plotRect();
    const float top = plot.getBottom() + 1.0f;
    return { plot.getX(), top, plot.getWidth(),
             juce::jmax(kAxisRowH, (float) getHeight() - top) };
}

int GraphicDisplayPanel::partAtAxisX(juce::Point<float> pos) const
{
    // Same column division the bars use, so a number always sits under its own bar.
    const int col = columnAtX(pos, plotRect());
    return col > 0 ? col - 1 : -1;          // col 0 is "S": a system value, not a part
}

juce::Rectangle<float> GraphicDisplayPanel::plotRect() const
{
    auto inner = displayRect().reduced(6.0f);
    inner.removeFromLeft(axisGutterW());               // corner / axis labels
    inner.removeFromRight(10.0f);
    inner.removeFromTop(isEqMode(mode) ? 16.0f : 6.0f);      // EQ: frequency labels along the top
    inner.removeFromBottom(isBarMode(mode) ? 20.0f : 6.0f);  // x-axis part numbers (taller font)
    return inner;
}

std::array<GraphicDisplayPanel::EgPoint, 5>
GraphicDisplayPanel::ampPoints(juce::Rectangle<float> plot) const
{
    const float yTop = plot.getY() + plot.getHeight() * 0.12f;
    const float yMid = plot.getCentreY();
    const float yBot = plot.getBottom() - plot.getHeight() * 0.12f;
    const float x0   = plot.getX() + plot.getWidth() * 0.05f;
    const float segMax   = plot.getWidth() * 0.20f;
    const float segFixed = plot.getWidth() * 0.10f;

    auto seg = [segMax](int raw) { return segMax * (float) juce::jlimit(0, 127, raw) / 127.0f; };

    const int atk = ampSlot(0);
    const int dcy = ampSlot(1);
    const int rel = ampSlot(2);

    std::array<EgPoint, 5> pts;
    pts[0] = { { x0, yBot }, false };
    pts[1] = { { pts[0].pos.x + seg(atk), yTop }, true };
    pts[2] = { { pts[1].pos.x + seg(dcy), yMid }, true };
    pts[3] = { { pts[2].pos.x + segFixed, yMid }, false };
    pts[4] = { { pts[3].pos.x + seg(rel), yBot }, true };
    return pts;
}

// The three editable nodes as the DEVICE currently has them. Mirrors ampPoints'
// arithmetic exactly -- if that layout ever changes, this changes with it.
std::array<juce::Point<float>, 3>
GraphicDisplayPanel::ampGhostPoints(juce::Rectangle<float> plot) const
{
    const float yTop = plot.getY() + plot.getHeight() * 0.12f;
    const float yMid = plot.getCentreY();
    const float yBot = plot.getBottom() - plot.getHeight() * 0.12f;
    const float x0   = plot.getX() + plot.getWidth() * 0.05f;
    const float segMax   = plot.getWidth() * 0.20f;
    const float segFixed = plot.getWidth() * 0.10f;
    auto seg = [segMax](int raw) { return segMax * (float) juce::jlimit(0, 127, raw) / 127.0f; };

    auto dev = [this](int which)
    {
        return ampOverride[which] >= 0 ? ampOverride[which] : ampSlot(which);
    };

    std::array<juce::Point<float>, 3> out;
    const float x1 = x0 + seg(dev(0));
    const float x2 = x1 + seg(dev(1));
    out[0] = { x1, yTop };
    out[1] = { x2, yMid };
    out[2] = { x2 + segFixed + seg(dev(2)), yBot };
    return out;
}

GraphicDisplayPanel::PitchGeom
GraphicDisplayPanel::pitchGeom(juce::Rectangle<float> plot) const
{
    const float yMid = plot.getCentreY();
    const float xA = plot.getX() + plot.getWidth() * 0.32f;
    const float xB = plot.getX() + plot.getWidth() * 0.62f;
    const float segMax    = plot.getWidth() * 0.24f;
    const float levelSpan = plot.getHeight() * 0.40f;

    auto seg = [segMax](int raw) { return segMax * (float) juce::jlimit(0, 127, raw) / 127.0f; };
    auto lvl = [yMid, levelSpan](int raw)
    {
        return yMid - (float) (juce::jlimit(0, 127, raw) - 64) / 64.0f * levelSpan;
    };

    const int init = pitchVal[currentPart][0];
    const int atkT = pitchVal[currentPart][1];
    const int relL = pitchVal[currentPart][2];
    const int relT = pitchVal[currentPart][3];

    PitchGeom geo;
    geo.a  = { xA, yMid };
    geo.b  = { xB, yMid };
    geo.p1 = { xA - seg(atkT), lvl(init) };   // attack extends left of A
    geo.p2 = { xB + seg(relT), lvl(relL) };   // release extends right of B
    return geo;
}

//==============================================================================
// Painting
//==============================================================================
void GraphicDisplayPanel::paint(juce::Graphics& g)
{
    g.setColour(edcol::kModuleBg);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    auto disp = displayRect();
    g.setColour(kLcdBg);
    g.fillRoundedRectangle(disp, 3.0f);
    g.setColour(kLcdBorder);
    g.drawRoundedRectangle(disp.reduced(0.5f), 3.0f, 1.0f);

    auto plot = plotRect();

    // Centre guide line (all modes show it). In the bar modes it marks the centre of
    // the BAR range (PAN's centre detent rides on it), so it drops by half a label
    // height along with the range's top; the EG / EQ screens centre on the plot.
    drawDashedH(g, plot.getX(), plot.getRight(),
                isBarMode(mode) ? barPlot(plot).getCentreY() : plot.getCentreY(), kDash);

    paintCornerLabels(g, plot);

    if (isBarMode(mode))
    {
        paintBars(g, plot);
        paintAxisLabels(g, plot);
    }
    else if (mode == AmpEG)
    {
        paintAmpEG(g, plot);
    }
    else if (mode == EQ)
    {
        paintEQ(g, plot);
    }
    else
    {
        paintPitchEG(g, plot);
    }

    // EG / EQ screens: a small corner hint (the 1..32 axis is intentionally hidden).
    // In drum-edit mode AMP EG / EQ name the drum tone instead of the part, while
    // PITCH EG (which a drum part ignores outright) explains its own inertness.
    if (! isBarMode(mode))
    {
        const juce::String corner =
            (mode == PitchEG && pitchEgInert())
                ? juce::String("Pitch EG inert for drum kits")
          : (isEqMode(mode) && eqBypassed())
                // Takes the corner OVER the part name, deliberately: while the EQ is
                // out of circuit that is the more important thing this screen has to
                // say, and the part number is on the Part module a few inches away.
                // It also wins over the shelf note below: bypass is the whole screen,
                // temporary, and the thing a user is likeliest to have forgotten.
                ? juce::String("EQ BYPASSED")
          : (isEqMode(mode) && eqShelvesDead())
                // Four live handles and two grey ones: say why those two are grey,
                // rather than leaving the user to wonder which rule they hit.
                ? juce::String("SHELVES PER DRUM KEY")
          : drumEditActive
                ? (drumKeyName.isNotEmpty() ? drumKeyName : juce::String("NO DRUM KEY"))
                : "PART " + juce::String(currentPart + 1);
        g.setColour((isEqMode(mode) && eqBypassed()) ? edcol::kAlert : kAxis);
        g.setFont(edtext::font(edtext::Label, 11.0f));
        g.drawText(corner, disp.reduced(8.0f).removeFromBottom(14.0f),
                   juce::Justification::centredRight);
    }

    // ...and the EQ screen fills the other end of that footer row with the RANGE
    // selector for the vertical axis.
    if (isEqMode(mode))
        paintEqRange(g);
}

void GraphicDisplayPanel::setPartDrumBuffer(int buffer)
{
    const int b = (buffer >= 0 && buffer < 4) ? buffer : -1;
    if (b == partBuffer) return;
    partBuffer = b;
    if (isEqMode(mode)) repaint();   // the handles and the corner both change with it
}

void GraphicDisplayPanel::setDrumKeyName(const juce::String& name)
{
    if (name == drumKeyName) return;
    drumKeyName = name;
    if (! isBarMode(mode)) repaint();   // AMP EG / PITCH EG / EQ all show it
}

void GraphicDisplayPanel::setDrumKitPart(bool isKit)
{
    if (isKit == drumKitPart) return;
    drumKitPart = isKit;
    if (dragging && mode == PitchEG) { endDragUndo(); dragging = false; }   // a kit swap mid-drag ends it
    if (mode == PitchEG) repaint();
}

// "RANGE:  ±12  ±24" along the EQ display's bottom-left margin — the same band the
// part / drum-key hint occupies at the right, so the two read as one footer row.
void GraphicDisplayPanel::eqRangeLayout(juce::Rectangle<float>& label,
                                        juce::Rectangle<float>& opt12,
                                        juce::Rectangle<float>& opt24) const
{
    const juce::Font f (edtext::font(edtext::Label, 11.0f));
    const float margin = 8.0f;   // the inset the hint opposite keeps from the display edge
    auto  band = displayRect().reduced(margin).removeFromBottom(14.0f);

    // Start level with the graph rather than the display edge: just inside the 20 Hz
    // gridline (which freqToX puts exactly on the plot's left edge, kEqFMin being 20),
    // clear of it by the same margin the hint keeps from the right edge — so both ends
    // of the footer sit the same distance inside the plot.
    band.setLeft(plotRect().getX() + margin);

    const float gap = 8.0f;      // label -> options, and between the options

    auto take = [&band, &f](const juce::String& s, float pad) -> juce::Rectangle<float>
    {
        const float w = (float) juce::GlyphArrangement::getStringWidthInt(f, s) + pad;
        return band.removeFromLeft(w);
    };

    label = take("RANGE:", 0.0f);
    band.removeFromLeft(gap);
    // A little padding round the options so they are comfortable to click.
    opt12 = take(pm() + "12", 8.0f);
    band.removeFromLeft(gap * 0.5f);
    opt24 = take(pm() + "24", 8.0f);
}

void GraphicDisplayPanel::paintEqRange(juce::Graphics& g)
{
    juce::Rectangle<float> label, opt12, opt24;
    eqRangeLayout(label, opt12, opt24);

    g.setFont(edtext::font(edtext::Label, 11.0f));

    // Label reads as an annotation (same grey as the part / drum-key hint opposite);
    // the options borrow the mode buttons' selected-green / unselected-grey so the
    // whole screen speaks one language about what is active.
    g.setColour(kAxis);
    g.drawText("RANGE:", label, juce::Justification::centredLeft);

    auto option = [&g](juce::Rectangle<float> r, const juce::String& t, bool on)
    {
        g.setColour(on ? kBtnTextOn : kBtnTextOff);
        g.drawText(t, r, juce::Justification::centred);
    };
    option(opt12, pm() + "12", eqRangeDb == 12);
    option(opt24, pm() + "24", eqRangeDb == 24);
}

void GraphicDisplayPanel::paintCornerLabels(juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float gutter = axisGutterW(), labelH = axisLabelH();
    const float lx = plot.getX() - gutter, lw = gutter - kAxisLabelGap;

    // HPF2: five Hz reference labels down the left axis (linear 0..1000 scale, so
    // 250/500/750/1000 land at 25/50/75/100 % and 20 sits just above the floor).
    // Each label sits just ABOVE its value's line — the convention the bar modes
    // already use at both ends ("127" above the top of the range, "0" above the
    // bottom). Being centred on the line instead used to hang "1k" half outside the
    // plot, which is why HPF2's top margin read tighter than the other screens'.
    if (isHpf2(mode))
    {
        g.setColour(kCorner);
        g.setFont(edtext::font(edtext::Label, 11.0f));
        const int labels[5] = { 20, 250, 500, 750, 1000 };
        const auto bp = barPlot(plot);
        for (int i = 0; i < 5; ++i)
        {
            const float frac = (float) labels[i] / (float) kHpf2Max;
            const float y = bp.getBottom() - frac * bp.getHeight();
            // "1k" instead of "1000" so it isn't ellipsized in the narrow margin.
            const juce::String txt = (labels[i] == 1000) ? juce::String("1k")
                                                         : juce::String(labels[i]);
            g.drawText(txt, juce::Rectangle<float>(lx, y - labelH, lw, labelH),
                       juce::Justification::centredLeft);
        }
        return;
    }

    // EQ: gain axis down the left edge, aligned to the inset gridlines. The rails
    // follow the RANGE selector (±12 or ±24).
    if (isEqMode(mode))
    {
        g.setColour(kCorner);
        g.setFont(edtext::font(edtext::Label, 11.0f));
        auto row = [&](float cy, const juce::String& t)
        {
            g.drawText(t, juce::Rectangle<float>(lx, cy - labelH * 0.5f, lw, labelH),
                       juce::Justification::centredLeft);
        };
        row(gainToY((float) eqRangeDb, plot),  "+" + juce::String(eqRangeDb));
        row(plot.getCentreY(),                 "0");
        row(gainToY((float) -eqRangeDb, plot), "-" + juce::String(eqRangeDb));
        return;
    }

    const char* top = nullptr;
    const char* bot = nullptr;
    if (mode == Volume || mode == Reverb || mode == Chorus || mode == Var) { top = "127"; bot = "0"; }
    else if (mode == Pan)                                                  { top = "R";   bot = "L"; }
    // AMP EG / PITCH EG: no corner labels.

    if (top == nullptr) return;

    // The labels stay pinned to the plot's top and bottom; it is the BAR RANGE that
    // insets to meet them (barPlot), so max lands at the bottom of this top label.
    g.setColour(kCorner);
    g.setFont(edtext::font(edtext::Label, 11.0f));
    auto lhs = juce::Rectangle<float>(lx, plot.getY(), lw, plot.getHeight());
    g.drawText(top, lhs.removeFromTop(labelH),    juce::Justification::centredLeft);
    g.drawText(bot, lhs.removeFromBottom(labelH), juce::Justification::centredLeft);
}

// See the header. The ONE place a bar value becomes a y.
float GraphicDisplayPanel::barValueY(int m, int val, juce::Rectangle<float> bp) const
{
    if (m == Pan)
    {
        // Centre-anchored, and NOT symmetric: 64 is the centre, so the up half has 63
        // steps and the down half 64. A single signed expression would put every pan
        // value a fraction of a step out on one side.
        const float cy = bp.getCentreY(), half = bp.getHeight() * 0.5f;
        if (val == 64) return cy;
        return val > 64 ? cy - (float) (val - 64) / 63.0f * half
                        : cy + (float) (64 - val) / 64.0f * half;
    }
    const int mx = isHpf2(m) ? kHpf2Max : 127;
    return bp.getBottom() - (float) juce::jlimit(0, mx, val) / (float) mx * bp.getHeight();
}

void GraphicDisplayPanel::paintBars(juce::Graphics& g, juce::Rectangle<float> plot)
{
    const int   n    = 33;   // S + 32 parts
    const float colW = plot.getWidth() / (float) n;
    const float barW = juce::jmax(2.0f, colW * 0.62f);
    const bool  pan  = (mode == Pan);
    const float clampTop = displayRect().getY() + 1.0f;
    // Animated level meters: VOLUME only, and only while they are actually running.
    // Every other bar screen (and PAN, which is centre-anchored and has no "level"
    // to speak of) keeps the solid bars.
    const bool  meters = (mode == Volume) && metersActive();

    // Bars span barPlot, not the whole plot: full scale stops at the bottom of the
    // "127" label, leaving the readout somewhere to sit at 125..127. Columns (x) are
    // still the plot's.
    const auto bp = barPlot(plot);

    for (int i = 0; i < n; ++i)
    {
        const int val = barValue(mode, i);
        const float cx = plot.getX() + ((float) i + 0.5f) * colW;
        const float x  = cx - barW * 0.5f;

        const bool inert = channelInert(mode) || (i == 0 && ! sBarActive(mode));
        const bool active = (dragging && dragIdx == i);
        // "Differs from its default" is a CHANGED MARKER, in the one place the editor
        // states it with colour instead of a tab -- so Options -> "Show changed markers"
        // has to reach it too, or half the class goes dark and half does not. The
        // DRAG highlight is untouched: `active` says what the mouse is on, which is a
        // different sentence and not the one being switched off.
        const bool changed = ! barIsDefault(mode, i) && changedmarks::show();
        juce::Colour c = inert ? kBarInert : ((active || changed) ? kHi : kBar);
        g.setColour(c);

        // The value's y, from the shared helper the override ticks also use.
        const float vy = barValueY(mode, val, bp);

        float readoutY;   // y of the bar top the readout sits above
        if (pan)
        {
            const float cy = bp.getCentreY();
            if (val == 64)
            {
                g.fillRect(x, cy - 1.0f, barW, 2.0f);
                readoutY = cy;
            }
            else if (val > 64)
            {
                g.fillRect(x, vy, barW, cy - vy);
                readoutY = vy;
            }
            else
            {
                g.fillRect(x, cy, barW, vy - cy);
                readoutY = cy;        // left of centre: keep value just above the axis
            }
        }
        else
        {
            float top = vy;

            if (meters)
            {
                // The setting becomes a thin dash where the solid bar's top edge was,
                // and the level animates below it. At settings near 0 the dash would
                // hang off the bottom of the plot, so it is floored — the indicator
                // stays visible at every value, which a bar of height 0 would not.
                const float dashH = juce::jmin(meterDashH(), bp.getHeight());
                top = juce::jmin(top, bp.getBottom() - dashH);
                paintMeterBar(g, bp, x, barW, top + dashH, i);
                g.setColour(c);                     // the meter left a gradient fill set
                g.fillRect(x, top, barW, dashH);
            }
            else
            {
                g.fillRect(x, top, barW, bp.getBottom() - top);
            }
            readoutY = top;
        }

        if (shouldShowReadout(i))
        {
            const juce::Font vf = grValueFont();
            const juce::String txt = barReadoutText(mode, val);
            const float rw = grValueW(vf, txt, 48.0f), rh = grValueH(vf);
            auto tr = juce::Rectangle<float>(cx - rw * 0.5f,
                                             juce::jmax(clampTop, readoutY - rh - 1.0f),
                                             rw, rh);
            g.setColour(kReadout);
            g.setFont(vf);
            g.drawText(txt, tr, juce::Justification::centred);
        }
    }

    // THE SYSTEM HPF, drawn as a rule across every column rather than only as the S
    // bar. It is a FLOOR -- a part is filtered at max(its own cutoff, this) -- so a line
    // over the whole plot is what the rule actually IS: every bar below it is being
    // lifted to it, every bar above it is unaffected, and both are legible at a glance
    // in a way a 33rd bar at the left edge cannot be.
    //
    // The part bars keep showing their STORED cutoffs, not the raised ones, on the same
    // principle as the EQ bypass: a value the user set never moves on screen because
    // something else is overriding it. The line is what explains the difference.
    //
    // Dashed and in the same yellow as the S bar (kHi = "not at its default"), so the
    // eye ties the line to the column that sets it; dashed also keeps it from reading
    // as one of the axis gridlines.
    if (isHpf2(mode))
    {
        const int sys = barValue(mode, 0);
        if (sys >= freqladder::kMin)
        {
            const float y = bp.getBottom()
                          - (float) juce::jlimit(0, kHpf2Max, sys) / (float) kHpf2Max * bp.getHeight();
            juce::Path line;
            line.startNewSubPath(plot.getX(), y);
            line.lineTo(plot.getRight(), y);
            const float dashes[] = { 4.0f, 3.0f };
            juce::Path dashed;
            juce::PathStrokeType(1.0f).createDashedStroke(dashed, line, dashes, 2);
            g.setColour(kHi);
            g.fillPath(dashed);
        }
    }

    paintBarOverrides(g, plot);
}

// MIDI OVERRIDE ticks: one thin rule per column at the value the DEVICE holds, drawn
// after every bar so nothing can bury one. The bars do not move -- what is on screen
// is still what gets saved; the tick says where the song has taken the device.
//
// WHY A TICK AND NOT A SECOND BAR. On this screen a value IS a vertical position, so
// a line at that position states it exactly, and it states it the same way whether the
// column is a solid bar or the VOLUME screen's setting dash over an animated meter. A
// thin bar inside the wide one would have had to become something else as soon as the
// meters were switched on, and it would read as FILL on the one screen where fill
// already means the live level.
//
// VISIBILITY COMES FROM WIDTH, NEVER FROM HEIGHT. An override can sit one step from
// the user's value, so a tick regularly lands on the bar's own top edge; it is drawn
// WIDER than the bar, into the column gutter that is empty on both sides, so its ends
// stay visible. Growing it vertically instead would be a mark that misstates the very
// number it exists to report.
void GraphicDisplayPanel::paintBarOverrides(juce::Graphics& g, juce::Rectangle<float> plot)
{
    const auto  bp   = barPlot(plot);
    const float colW = plot.getWidth() / (float) kBarStops;
    const float barW = juce::jmax(2.0f, colW * 0.62f);
    // Into the gutter, but never as far as the neighbouring column: at the design
    // canvas that is ~4 px a side, of which this takes 3.
    const float over = juce::jlimit(0.0f, 3.0f, (colW - barW) * 0.5f - 0.5f);
    const float w    = barW + 2.0f * over;
    const float th   = juce::jmax(2.0f, kOverrideTickH);

    g.setColour(edcol::kMidiOverride);
    for (int i = 0; i < kBarStops; ++i)
    {
        const int v = barOverride[mode][i];
        if (v < 0) continue;

        // Clamped so a device sitting at 0 or at full scale still shows a whole tick
        // rather than a hairline half outside the plot -- the same reason the meter
        // screen floors its setting dash.
        const float y = juce::jlimit(bp.getY() + th * 0.5f, bp.getBottom() - th * 0.5f,
                                     barValueY(mode, v, bp));
        const float cx = plot.getX() + ((float) i + 0.5f) * colW;
        g.fillRect(cx - w * 0.5f, y - th * 0.5f, w, th);
    }
}

void GraphicDisplayPanel::paintAxisLabels(juce::Graphics& g, juce::Rectangle<float> plot)
{
    const int   n    = 33;
    const float colW = plot.getWidth() / (float) n;
    // Labels occupy the top of the click zone; the zone itself reaches further down
    // (see axisRowRect) so no sliver below the numbers falls back to the bars.
    auto row = axisRowRect().withHeight(kAxisRowH);
    // The part ruler. The 10/20/30 markers are drawn BOLD as well as orange: they are
    // the only labels on this row meant to be *found* rather than read, and weight is
    // the one way to make a single glyph carry further without darkening its colour.
    // Both themes -- this is typography, not palette, and the two differ in colour only.
    const juce::Font axisFont     (edtext::font(edtext::Label, 12.0f));
    const juce::Font axisZeroFont (edtext::font(edtext::Label, 12.0f, juce::Font::bold));

    for (int i = 0; i < n; ++i)
    {
        const float cx = plot.getX() + ((float) i + 0.5f) * colW;
        auto cell = juce::Rectangle<float>(cx - colW * 0.5f, row.getY(), colW, row.getHeight());

        juce::String txt;
        juce::Colour col = kAxis;
        bool zero = false;
        if (i == 0) { txt = "S"; col = kCorner; }
        else
        {
            const int part = i;                 // 1..32
            if (part % 10 == 0) { txt = "0"; col = kAxisZero; zero = true; }   // 10/20/30
            else                  txt = juce::String(part % 10);
        }

        // Selected-part marker: this column's number gets the Voice module's OWN
        // selected-row treatment — a kListSelBg block behind it, the glyph bold in
        // kListSelText — so the axis and the part list say "this is the selected
        // part" with one visual sentence instead of two. The tokens are used
        // directly, not aliased: matching the list EXACTLY is the whole point, so a
        // separate token could only ever drift away from it.
        const bool selected = (i > 0 && i - 1 == currentPart);
        if (selected)
        {
            // A square a shade larger than the glyph, centred on the label cell.
            // Sized from the cell rather than fixed, so it cannot outgrow its column
            // when the plot narrows or overrun the row when edtext scales the font.
            const float side = juce::jmin(colW - 1.0f, cell.getHeight() - 1.0f);
            g.setColour(edcol::kListSelBg);
            g.fillRect(juce::Rectangle<float>(side, side).withCentre(cell.getCentre()));

            // Bold in BOTH cases, including a 10/20/30 "0": the zero markers are bold
            // already, so selection only takes their colour — the orange says "count
            // from here", and on the one column that is also the selected part the
            // stronger statement wins.
            col  = edcol::kListSelText;
            zero = true;
        }

        g.setFont(zero ? axisZeroFont : axisFont);
        g.setColour(col);
        g.drawText(txt, cell, juce::Justification::centred);
    }
}

void GraphicDisplayPanel::paintAmpEG(juce::Graphics& g, juce::Rectangle<float> plot)
{
    const auto pts = ampPoints(plot);

    // Envelope polyline.
    g.setColour(kEgLine);
    juce::Path path;
    path.startNewSubPath(pts[0].pos);
    for (int i = 1; i < (int) pts.size(); ++i) path.lineTo(pts[i].pos);
    g.strokePath(path, juce::PathStrokeType(1.4f));

    const float sq = juce::jlimit(6.0f, 12.0f, plot.getHeight() * 0.07f);

    // Editable point ids 0,1,2 map to amp params atk,dcy,rel at pts[1],[2],[4].
    const int editPt[3]  = { 1, 2, 4 };
    const int editWhich[3] = { 0, 1, 2 };

    auto drawSquare = [&](juce::Point<float> p, bool editable, bool hi)
    {
        auto r = juce::Rectangle<float>(sq, sq).withCentre(p);
        if (! editable)
        {
            g.setColour(kEgFixed);
            g.fillRect(r);
        }
        else if (hi)
        {
            g.setColour(kHi);  g.fillRect(r);
            g.setColour(edcol::kChangedEdge); g.drawRect(r, 1.0f);
        }
        else
        {
            g.setColour(kLcdBg);  g.fillRect(r);
            g.setColour(kEgEdit); g.drawRect(r, 1.5f);
        }
    };

    drawSquare(pts[0].pos, false, false);
    drawSquare(pts[3].pos, false, false);

    const juce::Font vf = grValueFont();
    g.setFont(vf);
    for (int e = 0; e < 3; ++e)
    {
        const int pi = editPt[e];
        const int raw = ampSlot(editWhich[e]);
        const int base = ampBaseline(editWhich[e]);
        const bool active = (dragging && dragIdx == editWhich[e]);
        const bool changed = (raw != base) && changedmarks::show();   // see paintBars
        drawSquare(pts[(size_t) pi].pos, true, active || changed);

        // Value readout, signed and relative to what this control LOADS WITH: 64 for a
        // MULTI PART envelope, the kit's own (inverted) default for a drum key.
        const int disp = raw - base;
        juce::String txt = (disp > 0 ? "+" : "") + juce::String(disp);
        auto p = pts[(size_t) pi].pos;
        const bool below = (pi == 2);
        const float rw = grValueW(vf, txt, 44.0f), rh = grValueH(vf);
        // Clear of the KEYBOARD FOCUS RING, not just of the node square -- the ring is
        // the bigger of the two and the text used to sit under it.
        auto tr = juce::Rectangle<float>(p.x - rw * 0.5f, below ? p.y + kEgReadoutGap
                                                                : p.y - kEgReadoutGap - rh, rw, rh);
        g.setColour(kReadout);
        g.drawText(txt, tr, juce::Justification::centred);
    }

    // MIDI OVERRIDE ghosts: a hollow square where the DEVICE has this node, drawn
    // LAST so it is never buried by the real one it may sit close to. The real
    // nodes do not move -- the envelope on screen is still the one that gets saved.
    if (ampOverride[0] >= 0 || ampOverride[1] >= 0 || ampOverride[2] >= 0)
    {
        const auto ghost = ampGhostPoints(plot);
        g.setColour(edcol::kMidiOverride);
        for (int e = 0; e < 3; ++e)
        {
            if (ampOverride[editWhich[e]] < 0) continue;
            g.drawRect(juce::Rectangle<float>(sq, sq).withCentre(ghost[(size_t) e]), 1.5f);
        }
    }
}

void GraphicDisplayPanel::paintPitchEG(juce::Graphics& g, juce::Rectangle<float> plot)
{
    const auto geo = pitchGeom(plot);

    // A drum part ignores PITCH EG entirely, so the envelope is drawn in the
    // inert grey the bar screens use and the corner says why (see paint()).
    const bool inert = pitchEgInert();

    g.setColour(inert ? kBarInert : kEgLine);
    juce::Path path;
    path.startNewSubPath(geo.p1);
    path.lineTo(geo.a);
    path.lineTo(geo.b);
    path.lineTo(geo.p2);
    g.strokePath(path, juce::PathStrokeType(1.4f));

    const float sq = juce::jlimit(6.0f, 12.0f, plot.getHeight() * 0.07f);

    auto drawSquare = [&](juce::Point<float> p, bool editable, bool hi)
    {
        auto r = juce::Rectangle<float>(sq, sq).withCentre(p);
        if (inert)                   { g.setColour(kBarInert); g.fillRect(r); }
        else if (! editable)         { g.setColour(kEgFixed); g.fillRect(r); }
        else if (hi)                 { g.setColour(kHi); g.fillRect(r);
                                       g.setColour(edcol::kChangedEdge); g.drawRect(r, 1.0f); }
        else                         { g.setColour(kLcdBg); g.fillRect(r);
                                       g.setColour(kEgEdit); g.drawRect(r, 1.5f); }
    };

    drawSquare(geo.a, false, false);
    drawSquare(geo.b, false, false);

    auto pairText = [](int rawX, int rawY)
    {
        const int x = rawX - 64, y = rawY - 64;
        return (x > 0 ? "+" : "") + juce::String(x) + ", " + (y > 0 ? "+" : "") + juce::String(y);
    };

    const juce::Font vf = grValueFont();
    g.setFont(vf);
    const float rh = grValueH(vf);
    // Clearance the readout keeps from its node -- enough to clear the keyboard focus
    // ring, which reaches further than the node square does.
    const float kNodeGap = kEgReadoutGap;

    // ABOVE the node and centred on it, exactly as the AMP EG screen places its own
    // readouts. It used to sit BESIDE the node, which fails at the left end of the
    // travel: a long attack takes P1 to ~8% of the plot width, where a "+63, +63" pair
    // does not fit to its left. Clamping that inside the plot only traded the text
    // being cut in half for the NODE sitting on top of it, which is what the user saw.
    // Centred-above needs only half the text width of room, and clamping THAT can never
    // collide with the node, because the collision axis is now vertical and free.
    auto readoutFor = [&](juce::Point<float> p, const juce::String& txt, float rw, float rh)
    {
        float y = p.y - kNodeGap - rh;
        // Near the top of the plot, flip below. It cannot be needed at the level range
        // this screen uses (a node stops 10% short of the edge) but a smaller window
        // makes every margin smaller with it.
        if (y < plot.getY() - rh) y = p.y + kNodeGap;
        return juce::Rectangle<float>(juce::jlimit(plot.getX(), plot.getRight() - rw,
                                                   p.x - rw * 0.5f), y, rw, rh);
    };

    // Point 1 (left): readout to its LEFT. values: time = pitchVal[1], level = pitchVal[0].
    {
        const bool active = (dragging && dragIdx == 0);
        const bool changed = (pitchVal[currentPart][1] != 64 || pitchVal[currentPart][0] != 64)
                             && changedmarks::show();
        drawSquare(geo.p1, true, active || changed);
        const juce::String txt = pairText(pitchVal[currentPart][1], pitchVal[currentPart][0]);
        const float rw = grValueW(vf, txt, 72.0f);
        const auto tr = readoutFor(geo.p1, txt, rw, rh);
        g.setColour(kReadout);
        if (! inert)   // stored values are meaningless for a drum part
            g.drawText(txt, tr, juce::Justification::centred);
    }
    // Point 2 (right): readout to its RIGHT. values: time = pitchVal[3], level = pitchVal[2].
    {
        const bool active = (dragging && dragIdx == 1);
        const bool changed = (pitchVal[currentPart][3] != 64 || pitchVal[currentPart][2] != 64)
                             && changedmarks::show();
        drawSquare(geo.p2, true, active || changed);
        const juce::String txt = pairText(pitchVal[currentPart][3], pitchVal[currentPart][2]);
        const float rw = grValueW(vf, txt, 72.0f);
        const auto tr = readoutFor(geo.p2, txt, rw, rh);
        g.setColour(kReadout);
        if (! inert)   // stored values are meaningless for a drum part
            g.drawText(txt, tr, juce::Justification::centred);
    }
}

//==============================================================================
// EQ screen — a Bitwig-style node editor whose curve is the true composite
// magnitude response (net dB at each frequency) of the active HPF2 + EQ bands.
//==============================================================================
float GraphicDisplayPanel::freqToX(float hz, juce::Rectangle<float> plot) const
{
    const double lo = std::log10(kEqFMin), hi = std::log10(kEqFMax);
    const double f = juce::jlimit(kEqFMin, kEqFMax, (double) hz);
    const double frac = (std::log10(f) - lo) / (hi - lo);
    return plot.getX() + (float) frac * plot.getWidth();
}

float GraphicDisplayPanel::xToFreq(float x, juce::Rectangle<float> plot) const
{
    const double lo = std::log10(kEqFMin), hi = std::log10(kEqFMax);
    const double frac = juce::jlimit(0.0, 1.0, (double) ((x - plot.getX()) / plot.getWidth()));
    return (float) std::pow(10.0, lo + frac * (hi - lo));
}

// Inset the ±12 dB gridlines from the plot edges so nodes/curve at the extremes
// don't collide with the top frequency labels or the bottom part/drum-key text.
float GraphicDisplayPanel::eqGainPad(juce::Rectangle<float> plot) const
{
    return juce::jlimit(10.0f, 20.0f, plot.getHeight() * 0.10f);
}

float GraphicDisplayPanel::gainToY(float db, juce::Rectangle<float> plot) const
{
    const float pad = eqGainPad(plot);
    const float top = plot.getY() + pad;            // +12 dB
    const float bot = plot.getBottom() - pad;       // -12 dB (0 dB stays at the centre)
    const double r = (double) eqRangeDb;                         // the RANGE selector's zoom
    const double d = juce::jlimit(-r, r, (double) db);
    const double frac = (d + r) / (2.0 * r);                     // 0 = bottom, 1 = top
    return bot - (float) frac * (bot - top);
}

// Node 0 = HPF2 ("F"), drawn at the 20 Hz floor when Off so it stays grabbable.
float GraphicDisplayPanel::eqFreqHzOf(int node) const
{
    if (node == 0)
    {
        const int hz = eqReadRaw ? eqReadRaw(-1, 0) : 0;
        return (float) juce::jmax(20, hz);
    }
    const int band = node - 1;
    const int raw  = eqReadRaw ? eqReadRaw(band, 1) : 28;
    return (float) eqRawToHz(band, raw);
}

float GraphicDisplayPanel::eqGainDbOf(int node) const
{
    if (node == 0) return 0.0f;                                   // HPF2 sits on 0 dB
    const int raw = eqReadRaw ? eqReadRaw(node - 1, 0) : 64;
    return (float) ((raw - 64) * (kEqBandGainMaxDb / 64.0));
}

bool GraphicDisplayPanel::eqBypassed() const
{
    return audioProcessor.isPartEqBypassed(currentPart);
}

// What the F node's filter is really doing, which is not always what its handle says:
// swp30 filters each voice at max(the part's HPF2, the system floor), so a part sitting
// below the floor -- or with no HPF2 at all -- is still being high-passed.
//
// PART MODE ONLY. In drum-edit mode this screen describes the KEY layer, and the part
// layer (its HPF2 and its three bands alike) is already absent from the curve; folding
// in the floor alone would be half an answer, which is worse than a consistent one.
//
// The HANDLE is deliberately left at the part's own value: it is the thing you can drag,
// and moving it to a frequency the box does not show would make the two disagree. The
// curve has never passed through the handles anyway -- it is the composite response, and
// this is one more term in it.
int GraphicDisplayPanel::eqEffectiveHpfHz() const
{
    const int own = eqReadRaw ? eqReadRaw(-1, 0) : 0;
    if (drumEditActive) return own;
    return juce::jmax(own, (int) audioProcessor.getSystemHpfHz());
}

bool GraphicDisplayPanel::eqNodeOn(int node) const
{
    if (eqInert()) return false;
    // A dead shelf is not merely uneditable, it is not IN the sound -- so it must not
    // be drawn into the response curve either. (The plugin-only LOCK is the opposite
    // case: those bands still run, so eqNodeLocked alone must not switch a node off.)
    if (eqShelvesDead() && ! eqNodeIsPluginOnly(node)) return false;
    if (node == 0) { const int hz = eqReadRaw ? eqReadRaw(-1, 0) : 0; return hz >= 20; }
    const int g = eqReadRaw ? eqReadRaw(node - 1, 0) : 64;
    return g != 64;                                              // any non-0 dB gain
}

float GraphicDisplayPanel::eqSquareSize(juce::Rectangle<float> plot) const
{
    return juce::jlimit(14.0f, 20.0f, plot.getHeight() * 0.11f); // slightly larger than EG squares
}

juce::Point<float> GraphicDisplayPanel::eqNodePos(int node, juce::Rectangle<float> plot) const
{
    return { freqToX(eqFreqHzOf(node), plot), gainToY(eqGainDbOf(node), plot) };
}

// A shelf's ghost node. GAIN and FREQUENCY are two separate parameters and a message
// can move either one alone, so each axis is taken from the device where it was
// overridden and from the user's own value where it was not. Taking both from the
// override state would put a gain-only override at whatever frequency the shadow
// happened to hold, which is a plausible-looking wrong answer rather than a visible
// one.
juce::Point<float> GraphicDisplayPanel::eqGhostPos(int band, juce::Rectangle<float> plot) const
{
    const int node = band + 1;
    const int og   = eqOverride[band][0];
    const int of   = eqOverride[band][1];

    const float hz = (of >= 0) ? (float) eqRawToHz(band, of) : eqFreqHzOf(node);
    const float db = (og >= 0) ? (float) ((og - 64) * (kEqBandGainMaxDb / 64.0))
                               : eqGainDbOf(node);
    return { freqToX(hz, plot), gainToY(db, plot) };
}

int GraphicDisplayPanel::eqNodeAt(juce::Point<float> pos, juce::Rectangle<float> plot) const
{
    const float grab = eqSquareSize(plot) * 1.2f;
    int best = -1; float bestD = grab;
    for (int n = 0; n < kEqNodes; ++n)
    {
        if (eqNodeLocked(n)) continue;          // plugin-only handle, option is on
        const float d = eqNodePos(n, plot).getDistanceFrom(pos);
        if (d <= bestD) { bestD = d; best = n; }
    }
    return best;
}

// Three significant digits, matching the Part module's frequency boxes. A free
// function rather than a lambda inside eqReadout: eqOverrideText formats the same
// axis, and a readout whose two halves rounded differently would read as two
// different measurements of one thing.
static juce::String eqHzText(double hz)
{
    const int v = (int) std::lround(hz);
    if (v < 1000)  return juce::String(v) + " Hz";
    if (v < 10000) return juce::String(v / 1000.0, 2) + " kHz";
    return juce::String(v / 1000.0, 1) + " kHz";
}

juce::String GraphicDisplayPanel::eqReadout(int node) const
{
    auto hzText = [](double hz) { return eqHzText(hz); };
    if (node == 0)
    {
        const int hz = eqReadRaw ? eqReadRaw(-1, 0) : 0;
        return hz < 20 ? juce::String("HPF2  Off") : "HPF2  " + hzText(hz);
    }
    const int band = node - 1;
    const int gr = eqReadRaw ? eqReadRaw(band, 0) : 64;
    const double db = (gr - 64) * (kEqBandGainMaxDb / 64.0);
    const int fr = eqReadRaw ? eqReadRaw(band, 1) : 28;
    juce::String s = (db > 0 ? "+" : "") + juce::String(db, 1) + " dB   " + hzText(eqRawToHz(band, fr));
    const int q = eqReadRaw ? eqReadRaw(band, 2) : -1;
    if (q >= 0) s += "   Q " + juce::String(q / 10.0, 1);
    return s;
}

//==============================================================================
// The DEVICE's reading for a painted stop, or "" when nothing has moved it -- the
// spoken half of the ghost node, the bar tick and the envelope ghost. See the header.

// AMP EG only. Its three segment times are MULTI PART parameters a controller can
// move; a PITCH EG node has no XG controller behind it and ampOverride has no slot
// for one. Signed -64..+63, exactly as egText prints the user's own value.
juce::String GraphicDisplayPanel::egOverrideText(const EgStop& st) const
{
    if (st.pitch || st.time < 0 || st.time > 2) return {};
    const int v = ampOverride[st.time];
    if (v < 0) return {};
    const int d = v - 64;
    return (d > 0 ? "+" : "") + juce::String(d);
}

// A bar column, read through the SAME formatter the bar's own readout uses, so "L23"
// and "Off" mean what they do everywhere else on the screen.
juce::String GraphicDisplayPanel::barOverrideText(int idx) const
{
    if (! isBarMode(mode) || idx < 0 || idx >= kBarStops) return {};
    const int v = barOverride[mode][idx];
    return v < 0 ? juce::String() : barReadoutText(mode, v);
}

// An EQ node. GAIN and FREQUENCY are separate parameters and a message can move
// either alone, so only the axis that actually moved is spoken -- the same rule
// eqGhostPos follows when it places the ghost. Only the two hardware shelves can
// ever answer: the mid bands and HPF2 are plugin-only, so no XG message reaches
// them and eqOverride holds -1 for them for ever.
juce::String GraphicDisplayPanel::eqOverrideText(int node) const
{
    const int band = node - 1;
    if (band < 0 || band >= 5) return {};
    const int og = eqOverride[band][0];
    const int of = eqOverride[band][1];

    juce::StringArray out;
    if (og >= 0)
    {
        const double db = (og - 64) * (kEqBandGainMaxDb / 64.0);
        out.add((db > 0 ? "+" : "") + juce::String(db, 1) + " dB");
    }
    if (of >= 0) out.add(eqHzText(eqRawToHz(band, of)));
    return out.joinIntoString("   ");
}

void GraphicDisplayPanel::captureEqDragStart(int node)
{
    if (node == 0)
    {
        dragStartVal  = eqReadRaw ? eqReadRaw(-1, 0) : 0;   // HPF2 Hz
        dragStartVal2 = 0; dragStartVal3 = 7;
        return;
    }
    const int band = node - 1;
    dragStartVal  = eqReadRaw ? eqReadRaw(band, 1) : 28;    // freq raw
    dragStartVal2 = eqReadRaw ? eqReadRaw(band, 0) : 64;    // gain raw
    const int q   = eqReadRaw ? eqReadRaw(band, 2) : 7;     // Q raw (-1 for shelves)
    dragStartVal3 = (q < 0 ? 7 : q);
}

//==============================================================================
// EQ animated spectrum — the thin live analyser line under the response curve.
//
// The signal comes from the swp30's per-part probe, which taps the END of the
// per-voice chain, so it is already post hardware-shelf / post plugin peaking
// bands / post HPF2 / post drum-key layer. That is what makes the line move when
// the curve above it does.
//==============================================================================
//==============================================================================
// VOLUME screen level meters — see the header for the contract.
//==============================================================================
float GraphicDisplayPanel::meterDashH()
{
    // The MIDI input indicators' width, so the two animated cues on this canvas carry
    // the same weight — and stay that way if either is ever retuned.
    return (float) midiind::kW;
}

bool GraphicDisplayPanel::metersActive() const
{
    return audioProcessor.volumeMetersEnabled
        && mode == Volume
        && isShowing();
}

void GraphicDisplayPanel::updateMeterTarget()
{
    const bool active = metersActive();
    audioProcessor.setPartLevelMeter(active);

    if (active)
    {
        if (! meterTimer.isTimerRunning())
        {
            meterLastMs = juce::Time::getMillisecondCounter();
            meterTimer.fn = [this] { meterFrame(); };
            meterTimer.startTimerHz(kMeterFps);
        }
    }
    else if (meterTimer.isTimerRunning())
    {
        meterTimer.stopTimer();
        // Drop the stale levels, so re-entering the screen doesn't flash whatever was
        // playing when it was last left.
        meterHasData = false;
        meterDb.fill(kMeterFloorDb);
    }
}

void GraphicDisplayPanel::meterFrame()
{
    // Re-assert every frame, for the two reasons spectrumFrame() documents at length:
    // JUCE never tells a descendant it stopped showing (the Hardware/Editor swap), and
    // setPartLevelMeter declines to push into swp30 while MAME is still booting, so
    // without a retry the meter would stay dead until some unrelated event re-pushed it.
    updateMeterTarget();
    if (! metersActive())
        return;

    float lin[32] = {};
    const int n = audioProcessor.readPartLevels(lin, 32);
    const float master = audioProcessor.takeMasterPeak();

    // Frame-rate-independent decay, exactly as the analyser derives it: changing the
    // update rate changes how often the bars move, never how fast they fall. Attack is
    // instant — a peak appears on the frame it happens.
    const juce::uint32 now = juce::Time::getMillisecondCounter();
    const double dtMs = juce::jlimit(1.0, 500.0, (double) (now - meterLastMs));
    meterLastMs = now;
    const double tau   = juce::jmax(1.0, audioProcessor.volumeMeterDecayMs());
    const float  decay = (float) std::exp(-dtMs / tau);

    // NB a 0 here genuinely means SILENCE, unlike readSpectrum's "reuse the previous
    // frame": these peaks are cleared as they are read, so nothing arriving means
    // nothing sounded, and decaying toward the floor is the correct response. That
    // also covers MAME being down (readPartLevels returns 0) — the bars fall away
    // rather than freezing at whatever was playing when the engine stopped.
    auto fold = [this, decay](int idx, float amp)
    {
        const float db = (amp > 1.0e-6f)
                             ? juce::jmax(kMeterFloorDb, 20.0f * std::log10(amp))
                             : kMeterFloorDb;
        meterDb[(size_t) idx] = meterHasData
            ? juce::jmax(db, kMeterFloorDb + (meterDb[(size_t) idx] - kMeterFloorDb) * decay)
            : db;
    };

    fold(0, master);                                  // the "S" bar = the main output
    for (int p = 0; p < 32; ++p)
        fold(p + 1, (p < n) ? lin[p] : 0.0f);

    meterHasData = true;
    repaint(plotRect().getSmallestIntegerContainer());   // the plot only
}

float GraphicDisplayPanel::meterFrac(int idx) const
{
    if (! meterHasData || idx < 0 || idx > 32)
        return 0.0f;
    const float db = meterDb[(size_t) idx];
    return juce::jlimit(0.0f, 1.0f,
                        (db - kMeterFloorDb) / (kMeterTopDb - kMeterFloorDb));
}

// One column's animated level, drawn from the floor up to `settingTopY` at the most —
// the underside of that part's setting dash. The clamp is what keeps the two readable
// as separate things: the level can never claim to be more than the setting allows,
// and it can never paint over the indicator that says what the setting IS.
void GraphicDisplayPanel::paintMeterBar(juce::Graphics& g, juce::Rectangle<float> bp,
                                        float x, float barW, float settingTopY, int idx)
{
    const float frac = meterFrac(idx);
    if (frac <= 0.0f)
        return;

    const float top = juce::jmax(settingTopY, bp.getBottom() - frac * bp.getHeight());
    const float h   = bp.getBottom() - top;
    if (h < 0.75f)
        return;

    // The ramp is anchored to the WHOLE bar span, not to the filled part, so a colour
    // always means the same level: a part sitting at -20 dB reads the same whether its
    // neighbours are silent or clipping. Where the stops land is the user's "Colour
    // spread" choice (edcol::meterSpreads) — dB, mapped through the same floor..0
    // scale meterFrac uses. Read live; no apply step, since the meter repaints itself
    // 30 times a second while it is running.
    auto at = [](float db)
    {
        return juce::jlimit(0.01f, 0.99f,
                            (db - kMeterFloorDb) / (kMeterTopDb - kMeterFloorDb));
    };
    const auto& spread = edcol::meterSpreads()[
        juce::jlimit(0, edcol::kMeterSpreadCount - 1, audioProcessor.volumeMeterSpreadIdx)];

    juce::ColourGradient grad (kMeterLow, x, bp.getBottom(), kMeterPeak, x, bp.getY(), false);
    grad.addColour (at(spread.knee[0]), kMeterLow);
    grad.addColour (at(spread.knee[1]), kMeterMid);
    grad.addColour (at(spread.knee[2]), kMeterHigh);

    // Meter fill (edcol::meterFills): Solid is one column, LED is the same
    // column cut into segments the size of the setting dash. The LED grid is
    // anchored to the BOTTOM of the bar span, not to the top of the fill, so the
    // segments hold still and only their number changes — anchoring it to the fill
    // would slide every block down the bar as the level fell, which reads as noise.
    const bool led = audioProcessor.volumeMeterFillIdx == 1;
    if (! led)
    {
        g.setGradientFill (grad);
        g.fillRect (x, top, barW, h);
        return;
    }

    const float segH  = juce::jmax (1.0f, meterDashH());
    const float gap   = juce::jmax (1.0f, segH * edcol::kMeterLedGapFrac);
    const float pitch = segH + gap;

    // Whole segments only: the fill's top is quantised DOWN to the grid, so a segment
    // lights when the level has genuinely reached it. Always at least one, since we
    // are here only because frac > 0 — a level that is audible must show something.
    const int  segs   = juce::jmax (1, (int) std::floor ((h + gap) / pitch));
    const float ledTop = juce::jmax (top, bp.getBottom() - ((float) segs * pitch - gap));

    g.setGradientFill (grad);
    g.fillRect (x, ledTop, barW, bp.getBottom() - ledTop);

    // Then cut the gaps back out in the LCD background. Painted rather than left
    // unpainted because the centre guide line runs BEHIND the bars: skipping the
    // paint would let it show through every gap on the bars that cross it.
    g.setColour (kLcdBg);
    for (int k = 1; k < segs; ++k)
    {
        const float gy = bp.getBottom() - (float) k * pitch;
        g.fillRect (x, gy, barW, gap);
    }
}

bool GraphicDisplayPanel::spectrumActive() const
{
    return audioProcessor.eqSpectrumEnabled
        && mode == EQ
        && isShowing()
        && ! eqInert();
}

void GraphicDisplayPanel::updateSpectrumTarget()
{
    const bool active = spectrumActive();

    // In drum-edit mode the screen edits ONE key, so the probe must narrow to it —
    // otherwise the line shows the whole kit while the curve describes one drum.
    const int key = (active && drumEditActive) ? drumKey : -1;
    audioProcessor.setSpectrumTarget(active, currentPart, key);

    if (active)
    {
        if (! spectrumTimer.isTimerRunning())
        {
            specLastMs = juce::Time::getMillisecondCounter();
            spectrumTimer.fn = [this] { spectrumFrame(); };
            spectrumTimer.startTimerHz(kSpecFps);
        }
    }
    else if (spectrumTimer.isTimerRunning())
    {
        spectrumTimer.stopTimer();
        // Drop the stale trace so re-entering the screen doesn't flash the previous
        // part's spectrum before the first frame lands.
        specHasData = false;
        std::fill(specDb.begin(), specDb.end(), kSpecFloorDb);
    }
}

void GraphicDisplayPanel::spectrumFrame()
{
    // Re-assert EVERY frame, active or not. Two distinct jobs, both load-bearing:
    //
    //  1. HIDE detection. JUCE calls visibilityChanged() only on the component whose
    //     own flag moved, NOT on its descendants — so the editor's Hardware/Editor
    //     swap (editorView->setVisible) never reaches this panel and isShowing() goes
    //     false with no notification at all. Re-testing here shuts the probe down
    //     within one frame on EVERY hide path, including ones nobody enumerated.
    //
    //  2. RETRY of the target push. setSpectrumTarget is a no-op once it has landed
    //     (it caches), but it silently declines to push while MAME is still booting.
    //     Entering the EQ screen shortly after launch does exactly that — and without
    //     a retry the probe stays disabled in swp30, readSpectrum returns 0 forever,
    //     and the analyser is dead until some unrelated event happens to re-push.
    //     Calling it here is what makes "the analyser re-pushes its own target" true.
    updateSpectrumTarget();
    if (! spectrumActive())
        return;

    auto plot = plotRect();
    const int cols = (int) std::floor(plot.getWidth());
    if (cols < 8) return;

    const int order = juce::jlimit(YamahaMU100AudioProcessor::kSpectrumFftOrderMin,
                                   YamahaMU100AudioProcessor::kSpectrumFftOrderMax,
                                   audioProcessor.eqSpectrumFftOrder);
    const int fftSize = 1 << order;

    // Rebuild only when the user changes resolution (not per frame).
    if (specFft == nullptr || order != specOrder)
    {
        specFft    = std::make_unique<juce::dsp::FFT>(order);
        specWindow = std::make_unique<juce::dsp::WindowingFunction<float>>(
                         (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false);
        specOrder  = order;
        specFifo.assign((size_t) fftSize * 2, 0.0f);
    }

    // A 0 return is normal (probe idle, MAME not up, or a dropped frame) and means
    // "keep the previous trace" — NOT "silence". Decaying to the floor on a dropped
    // frame would make the line stutter.
    std::fill(specFifo.begin(), specFifo.end(), 0.0f);
    if (audioProcessor.readSpectrum(specFifo.data(), fftSize) != fftSize)
        return;

    specWindow->multiplyWithWindowingTable(specFifo.data(), (size_t) fftSize);
    specFft->performFrequencyOnlyForwardTransform(specFifo.data());

    if ((int) specDb.size() != cols)
    {
        specDb.assign((size_t) cols, kSpecFloorDb);
        specHasData = false;
    }

    // Frame-rate-independent decay: derive the coefficient from the REAL elapsed
    // time, so changing the update rate changes how often the line moves, never how
    // fast it falls. Attack is instant (a peak appears the frame it happens).
    const juce::uint32 now = juce::Time::getMillisecondCounter();
    const double dtMs = juce::jlimit(1.0, 500.0, (double) (now - specLastMs));
    specLastMs = now;
    const double tau   = juce::jmax(1.0, audioProcessor.spectrumDecayMs());
    const float  decay = (float) std::exp(-dtMs / tau);

    // Hann coherent gain is 0.5, so a full-scale sine reads -6 dB without this.
    const double norm = 2.0 / (double) fftSize;
    const double binHz = YamahaMU100AudioProcessor::kSpectrumSampleRate / (double) fftSize;
    const int    maxBin = fftSize / 2;

    auto binDb = [&](int bin) -> float
    {
        const double mag = (double) specFifo[(size_t) juce::jlimit(0, maxBin, bin)] * norm;
        return (float) juce::jmax((double) kSpecFloorDb, 20.0 * std::log10(mag + 1.0e-12));
    };

    for (int i = 0; i < cols; ++i)
    {
        const float xL = plot.getX() + (float) i;
        const double fLo = xToFreq(xL,          plot);
        const double fHi = xToFreq(xL + 1.0f,   plot);

        // Bins per pixel varies hugely across a LOG axis: dozens up at 10 kHz, far
        // less than one down at 20 Hz.
        const int bLo = (int) std::ceil (fLo / binHz);
        const int bHi = (int) std::floor(fHi / binHz);

        float db;
        if (bHi >= bLo && bLo <= maxBin)
        {
            // Several bins land in this column: take the PEAK, so a narrow tone is
            // never averaged away against its quiet neighbours.
            db = kSpecFloorDb;
            for (int b = juce::jmax(0, bLo); b <= juce::jmin(maxBin, bHi); ++b)
                db = juce::jmax(db, binDb(b));
        }
        else
        {
            // No bin falls inside this column — the norm below a few hundred Hz,
            // where bins are sparser than pixels. Interpolating in dB between the
            // neighbours is what keeps the low end a curve instead of a staircase,
            // which matters because that is exactly where HPF2 works.
            const double fc  = std::sqrt(fLo * fHi);       // geometric centre
            const double pos = fc / binHz;
            const int    b0  = juce::jlimit(0, maxBin, (int) std::floor(pos));
            const int    b1  = juce::jlimit(0, maxBin, b0 + 1);
            const float  t   = (float) juce::jlimit(0.0, 1.0, pos - (double) b0);
            db = binDb(b0) + (binDb(b1) - binDb(b0)) * t;
        }

        specDb[(size_t) i] = specHasData
            ? juce::jmax(db, kSpecFloorDb + (specDb[(size_t) i] - kSpecFloorDb) * decay)
            : db;
    }

    specHasData = true;
    repaint(plot.getSmallestIntegerContainer());   // the plot only, never the whole panel
}

float GraphicDisplayPanel::specToY(float db, juce::Rectangle<float> plot) const
{
    // The BOTTOM is inset by the same eqGainPad the gain rails use — that pad exists
    // to keep extremes off the footer row (the RANGE selector and the PART / drum-key
    // hint). It matters more here than it does for the response curve: every silent
    // column pins the trace to the floor, so an un-inset floor draws a solid line
    // straight through that text on any quiet part of the spectrum.
    // The TOP stays at plot.getY(): the frequency labels are drawn ABOVE the plot,
    // so nothing there to clear, and the trace gets the full remaining height.
    const float bot = plot.getBottom() - eqGainPad(plot);
    const float t = juce::jlimit(0.0f, 1.0f,
                                 (db - kSpecFloorDb) / (kSpecTopDb - kSpecFloorDb));
    return bot - t * (bot - plot.getY());
}

void GraphicDisplayPanel::paintEQ(juce::Graphics& g, juce::Rectangle<float> plot)
{
    // Frequency grid lines + top decade labels (20 / 100 / 1k / 10k).
    struct { double hz; const char* txt; } ticks[] = {
        { 20, "20" }, { 100, "100" }, { 1000, "1k" }, { 10000, "10k" } };
    g.setFont(edtext::font(edtext::Label, 11.0f));
    for (auto& t : ticks)
    {
        const float x = freqToX((float) t.hz, plot);
        g.setColour(kDash);
        g.drawVerticalLine((int) std::round(x), plot.getY(), plot.getBottom());
        g.setColour(kAxis);
        const bool leftEdge = (t.hz == 20);
        auto lab = juce::Rectangle<float>(leftEdge ? x - 2.0f : x - 20.0f, plot.getY() - 15.0f, 40.0f, 13.0f);
        g.drawText(t.txt, lab, leftEdge ? juce::Justification::centredLeft : juce::Justification::centred);
    }

    const bool inert = eqInert();

    // Live spectrum, drawn FIRST so the thick response curve and the node handles
    // stay on top of it (as in the Bitwig reference).
    if (specHasData && (int) specDb.size() >= 2 && audioProcessor.eqSpectrumEnabled && ! inert)
    {
        juce::Path sp;
        bool started = false;
        // Clamp to the CURRENT plot width: a resize lands before the next analyser
        // frame rebuilds specDb, and specDb is indexed as one entry per pixel, so a
        // stale (wider) trace would draw past the plot's right edge.
        const int n = juce::jmin((int) specDb.size(), (int) std::floor(plot.getWidth()));
        for (int i = 0; i < n; ++i)
        {
            const float x = plot.getX() + (float) i;
            const float y = specToY(specDb[(size_t) i], plot);
            if (! started) { sp.startNewSubPath(x, y); started = true; }
            else           { sp.lineTo(x, y); }
        }
        // Unlike the response curve this is NOT cut where it leaves the view: it is
        // clamped by specToY, because a level pinned to the floor is a true reading
        // ("nothing here"), whereas a response cut off the graph would be a lie.
        g.setColour(kSpecLine);
        g.strokePath(sp, juce::PathStrokeType(1.0f));
    }

    // Composite response curve: sum every active band's magnitude (dB) per pixel.
    if (! inert && eqReadRaw)
    {
        std::vector<Biquad> bands;
        for (int n = 0; n < kEqNodes; ++n)
        {
            // The F node is asked about the EFFECTIVE cutoff rather than eqNodeOn's
            // view of the stored one: with the system floor engaged the filter runs
            // even when this part's own HPF2 is Off, and a curve that ignored that
            // would be a lie about the sound while the analyser trace beside it showed
            // the truth. Every other node is its own stored value, as before.
            if (n == 0)
            {
                const int hz = eqEffectiveHpfHz();
                if (hz >= (int) freqladder::kMin) bands.push_back(hpfCoef(hz));
                continue;
            }
            if (! eqNodeOn(n)) continue;
            const int band = n - 1;
            const double db = (eqReadRaw(band, 0) - 64) * (kEqBandGainMaxDb / 64.0);
            const double f  = eqRawToHz(band, eqReadRaw(band, 1));
            if      (band == 0) bands.push_back(shelfCoef(db, f, false));   // low shelf
            else if (band == 4) bands.push_back(shelfCoef(db, f, true));    // high shelf
            else                { const int qr = eqReadRaw(band, 2);
                                  bands.push_back(peakingCoef(db, f, (qr > 0 ? qr : 7) / 10.0)); }
        }
        if (! bands.empty())
        {
            // Draw only where the composite response is inside the displayed range.
            // When it leaves the graphable band the line is cut at the exact crossing
            // and resumes at re-entry — so an off-graph excursion never reads as a flat
            // line pinned to the edge (which would misrepresent the true, larger
            // boost/cut). Switching RANGE to ±24 is how you see such an excursion.
            juce::Path path;
            const int N = juce::jmax(64, (int) plot.getWidth());
            const double lim = (double) eqRangeDb;
            bool  havePrev = false;
            double pdb = 0.0; float px = 0.0f;
            for (int i = 0; i <= N; ++i)
            {
                const float x = plot.getX() + (float) i / (float) N * plot.getWidth();
                const double f = xToFreq(x, plot);
                double db = 0.0;
                for (auto& b : bands) db += biquadMagDb(b, f);
                const bool in = (db <= lim && db >= -lim);
                if (havePrev)
                {
                    const bool pin = (pdb <= lim && pdb >= -lim);
                    if (pin && in)                        // fully inside: extend
                        path.lineTo(x, gainToY((float) db, plot));
                    else if (pin && ! in)                 // leaving: cut at the crossing
                    {
                        const double b = (db > lim) ? lim : -lim;
                        const float xc = px + (float) ((b - pdb) / (db - pdb)) * (x - px);
                        path.lineTo(xc, gainToY((float) b, plot));
                    }
                    else if (! pin && in)                 // re-entering: start a fresh run
                    {
                        const double b = (pdb > lim) ? lim : -lim;
                        const float xc = px + (float) ((b - pdb) / (db - pdb)) * (x - px);
                        path.startNewSubPath(xc, gainToY((float) b, plot));
                        path.lineTo(x, gainToY((float) db, plot));
                    }
                    // else both out of range: draw nothing (the line is off-graph)
                }
                else if (in)
                    path.startNewSubPath(x, gainToY((float) db, plot));
                pdb = db; px = x; havePrev = true;
            }
            // EQ BYPASS: the curve is still the truth about the VALUES, and drawing
            // it normally would make it a lie about the SOUND. Grey and dashed says
            // "this shape is set but not in circuit" -- a line style rather than a
            // second meaning loaded onto a colour, and it survives both themes and a
            // screenshot in a way a colour change would not.
            const bool byp = eqBypassed();
            g.setColour(byp ? kBarInert : kHi);
            if (byp)
            {
                const float dashes[] = { 5.0f, 4.0f };
                juce::Path dashed;
                juce::PathStrokeType(1.6f).createDashedStroke(dashed, path, dashes, 2);
                g.fillPath(dashed);
            }
            else
                g.strokePath(path, juce::PathStrokeType(1.6f));
        }
    }

    // The six handle squares (F, 1..5), same palette as the EG points.
    const float sq = eqSquareSize(plot);
    g.setFont(edtext::font(edtext::Label, 12.0f));   // matches the part-number axis font
    static const char* const kLabels[kEqNodes] = { "F", "1", "2", "3", "4", "5" };
    for (int n = 0; n < kEqNodes; ++n)
    {
        const auto p = eqNodePos(n, plot);
        auto r = juce::Rectangle<float>(sq, sq).withCentre(p);
        const bool active = dragging && dragIdx == n;
        // A locked (plugin-only) handle wears the same grey as an inert screen — it
        // is uneditable for the same reason a user cares about: it isn't there to
        // touch. It reads grey even when its band is ON, which is the honest cue:
        // the curve still shows the band's contribution, the handle just can't move.
        const bool dim    = inert || eqNodeLocked(n);
        const bool filled = (eqNodeOn(n) || active) && ! dim;
        if (filled) { g.setColour(kHi);   g.fillRect(r); g.setColour(edcol::kChangedEdge); g.drawRect(r, 1.0f); }
        else        { g.setColour(kLcdBg); g.fillRect(r); g.setColour(dim ? kBarInert : kEgEdit); g.drawRect(r, 1.5f); }
        g.setColour(filled ? edcol::kOnChanged : (dim ? kBarInert : kCorner));
        g.drawText(kLabels[n], r, juce::Justification::centred);
    }

    // MIDI OVERRIDE ghosts: a hollow square, carrying the band's own number, where the
    // DEVICE has that shelf. Drawn after the handles so a ghost sitting close to its
    // real node is never buried by it, and before the hover readout so the readout
    // panel still wins.
    //
    // ONLY BANDS 1 AND 5 CAN EVER APPEAR. They are the hardware shelves (MULTI PART,
    // or DRUM SETUP while a drum key is being edited); the F handle and bands 2-4 are
    // plugin-only, so no message reaches them and no ghost is possible. The number is
    // drawn rather than left off precisely because two ghosts can be up at once and
    // they can sit anywhere along the frequency axis.
    //
    // No ghost CURVE. The curve on this screen is the composite of six bands, and a
    // second one would be unreadable rather than informative.
    if (! inert)
    {
        g.setColour(edcol::kMidiOverride);
        g.setFont(edtext::font(edtext::Label, 12.0f));
        for (const int band : { 0, 4 })
        {
            if (eqOverride[band][0] < 0 && eqOverride[band][1] < 0) continue;
            auto r = juce::Rectangle<float>(sq, sq).withCentre(eqGhostPos(band, plot));
            g.drawRect(r, 1.5f);
            g.drawText(juce::String(band + 1), r, juce::Justification::centred);
        }
    }

    // Value readout for the dragged handle (or the hovered one after the delay).
    // Sits on a translucent panel (legible over the curve), placed clear of the node
    // — above it by default, flipping below when it would hit the top labels — and
    // clamped inside the LCD so it is never masked at an edge.
    const int show = dragging ? dragIdx : ((hoverShown && eqHoverNode >= 0) ? eqHoverNode : -1);
    if (show >= 0 && ! inert)
    {
        const auto p = eqNodePos(show, plot);
        const juce::String txt = eqReadout(show);
        const juce::Font rf = grValueFont();
        juce::GlyphArrangement ga; ga.addLineOfText(rf, txt, 0.0f, 0.0f);
        const float tw = ga.getBoundingBox(0, -1, true).getWidth() + 12.0f;
        const float th = grValueH(rf) + 3.0f;   // panel: a little more air than a bare readout
        const auto disp = displayRect();

        // Placed clear of the KEYBOARD FOCUS RING, not merely of the handle square: the
        // ring reaches further than the square does, and at the old 4 px clearance the
        // panel's top edge ran under it. See eqReadoutGap (and kEgReadoutGap, which is
        // the same derivation on the envelope screens).
        const float gap = eqReadoutGap(plot);
        float ty = p.y - gap - th;                                    // above the node
        if (ty < plot.getY() + 1.0f) ty = p.y + gap;                  // flip below if it hits the top labels
        const float tx = juce::jlimit(disp.getX() + 3.0f,
                                      juce::jmax(disp.getX() + 3.0f, disp.getRight() - tw - 3.0f),
                                      p.x - tw * 0.5f);
        const auto box = juce::Rectangle<float>(tx, ty, tw, th);
        g.setColour(edcol::kTipBg.withAlpha(0.72f));
        g.fillRoundedRectangle(box, 3.0f);
        g.setColour(edcol::kTipBorder.withAlpha(0.8f));
        g.drawRoundedRectangle(box, 3.0f, 1.0f);
        g.setColour(edcol::kTipText);   // on the translucent tip panel, not the plot
        g.setFont(rf);
        g.drawText(txt, box, juce::Justification::centred);
    }
}

//==============================================================================
// Bar columns: the shared bodies for one nudge and one reset. The wheel and the
// keyboard both come here, so the two cannot grow different rules -- the HPF2 ladder
// and the "Off below 20 Hz" floor are stated ONCE.
//==============================================================================
void GraphicDisplayPanel::barNudge(int idx, int dir, bool fine)
{
    if (dir == 0 || ! isBarMode(mode) || channelInert(mode)) return;
    if (idx < 0 || idx >= kBarStops) return;
    if (idx == 0 && ! sBarActive(mode)) return;

    // HPF2 nudges by 10 Hz per notch (Shift = 1 Hz fine) and lifts off Off at the
    // 20 Hz floor -- the same rule as the Part module's HPF2 box. Every other bar is
    // a 0..127 raw value, where one step is one unit and there is nothing finer.
    const int cur = barValue(mode, idx);
    const int nv  = isHpf2(mode) ? (int) freqladder::hpf2::wheel(cur, dir, fine)
                                 : cur + dir;
    setBarValue(mode, idx, nv, true);
}

void GraphicDisplayPanel::barReset(int idx)
{
    if (! isBarMode(mode) || channelInert(mode)) return;
    if (idx < 0 || idx >= kBarStops) return;
    if (idx == 0 && ! sBarActive(mode)) return;
    const auto& spec = kModes[mode];
    setBarValue(mode, idx, idx == 0 ? spec.sDefault : spec.partDefault, true);
}

// Enter on a bar column. The overlay is configured for the CURRENT screen every time
// it opens -- a bar is 0..127 on five screens, absolute Hz on HPF2, and L/C/R text on
// PAN -- so there is one box rather than six, and its text hooks are the same functions
// the readout and the wheel already use.
// The one overlay, opened over whatever the cursor is on. Everything that differs
// between a bar column and an EQ handle is an argument: where it goes, what it may
// hold, how the value reads and parses, and what to do with it.
void GraphicDisplayPanel::openTypeIn(juce::Rectangle<int> near, juce::Component* focusBack,
                                     double lo, double hi, double cur,
                                     std::function<juce::String(double)> fmt,
                                     std::function<double(const juce::String&)> parse,
                                     std::function<double(double)> snap,
                                     std::function<void(double)> apply)
{
    if (typeIn == nullptr)
    {
        typeIn = std::make_unique<ValueBox>();
        addChildComponent(*typeIn);
        typeIn->onChange = [this](double v) { if (typeInApply) typeInApply(v); repaint(); };
        // Put itself away and hand the keyboard back. FOCUS FIRST, THEN HIDE: hiding a
        // component that holds the focus makes JUCE give that focus to the parent's
        // first focusable child -- some other column's or handle's proxy -- which is
        // the trap the accessibility work already paid for once.
        typeIn->onTypeInEnd = [this]
        {
            // ASYNC, and that is not a nicety: ValueBox::commitEdit defers the
            // TextEditor's destruction to the message queue, and deleting a focused
            // editor moves the focus AGAIN -- onto the (about to be hidden) box, where
            // the keyboard is then lost entirely and the arrow keys do nothing. Handing
            // the focus back from BEHIND that deletion in the queue is what makes the
            // cursor survive an Escape. Measured: without it the cursor vanished after
            // the first type-in and only Tab brought it back.
            juce::Component::SafePointer<juce::Component> back (typeInFocusBack);
            juce::Component::SafePointer<GraphicDisplayPanel> self (this);
            typeInFocusBack = nullptr;
            juce::MessageManager::callAsync([self, back]
            {
                if (self == nullptr) return;
                if (self->typeIn != nullptr) self->typeIn->setVisible(false);
                if (back != nullptr) back->grabKeyboardFocus();
                self->repaint();
            });
        };
    }

    typeInApply     = std::move(apply);
    typeInFocusBack = focusBack;

    typeIn->setRange(lo, hi, 1.0);
    typeIn->snapValue     = std::move(snap);
    typeIn->textFromValue = std::move(fmt);
    typeIn->valueFromText = std::move(parse);
    typeIn->setValueQuiet(cur);

    // Wide enough for the longest thing any of these formats produces ("-12.0dB",
    // "1.00k", "RND"), centred on `near`, and clamped inside the plot so an edge
    // column or a handle at 20 Hz is not left half off the screen.
    const auto plot = plotRect().getSmallestIntegerContainer();
    const int  w = juce::jmax(56, near.getWidth());
    juce::Rectangle<int> r (near.getCentreX() - w / 2, near.getY(), w, 16);
    if (r.getX() < plot.getX())          r.setX(plot.getX());
    if (r.getRight() > plot.getRight())  r.setX(plot.getRight() - w);
    if (r.getY() < plot.getY())          r.setY(plot.getY());
    if (r.getBottom() > plot.getBottom()) r.setY(plot.getBottom() - r.getHeight());

    typeIn->setBounds(r);
    typeIn->setVisible(true);
    typeIn->toFront(false);
    typeIn->beginTypeIn();
}

void GraphicDisplayPanel::beginBarTypeIn(int idx)
{
    if (! isBarMode(mode) || channelInert(mode)) return;
    if (idx < 0 || idx >= kBarStops) return;
    if (idx == 0 && ! sBarActive(mode)) return;

    const int cur = barValue(mode, idx);

    // Which proxy takes the keyboard back when the editor closes.
    juce::Component* back = nullptr;
    {
        const auto stops = barStops();
        for (int i = 0; i < (int) stops.size(); ++i)
            if (stops[(size_t) i].idx == idx) { back = &barProxy[i]; break; }
    }

    // The two text hooks, per screen. textFromValue is barReadoutText, so the editor
    // OPENS showing exactly what the bar was showing.
    const bool hpf = isHpf2(mode);
    const bool pan = (mode == Pan);
    auto parse = [hpf, pan](const juce::String& t) -> double
    {
        const juce::String s = t.trim();
        if (hpf)
        {
            if (s.isEmpty() || s.equalsIgnoreCase("off")) return 0.0;
            return juce::jlimit(0.0, (double) kHpf2Max, freqladder::parse(s));
        }
        if (! pan) return (double) juce::jlimit(0, 127, s.getIntValue());
        // PAN reads back the way it displays -- RND / L63..L1 / C / R1..R63 -- and also
        // takes the raw number, because that is what the other five screens take and a
        // user should not have to remember which screen they are on.
        if (s.equalsIgnoreCase("rnd")) return 0.0;
        if (s.equalsIgnoreCase("c"))   return 64.0;
        if (s.startsWithIgnoreCase("l")) return (double) juce::jlimit(1, 64,  64 - s.substring(1).getIntValue());
        if (s.startsWithIgnoreCase("r")) return (double) juce::jlimit(64, 127, 64 + s.substring(1).getIntValue());
        return (double) juce::jlimit(0, 127, s.getIntValue());
    };

    // At the TOP of the column, not on the bar's own top edge: a low value would put the
    // editor at the floor of the plot, and an editor that moves with the value is one
    // the eye has to hunt for.
    const auto  plot = plotRect();
    const float colW = plot.getWidth() / (float) kBarStops;
    const int   cx   = (int) (plot.getX() + ((float) idx + 0.5f) * colW);
    const juce::Rectangle<int> near (cx - (int) colW / 2, (int) plot.getY() + 2, (int) colW, 16);

    openTypeIn(near, back, 0.0, hpf ? (double) kHpf2Max : 127.0, (double) cur,
               [this](double v) { return barReadoutText(mode, (int) v); },
               std::move(parse),
               hpf ? std::function<double(double)>([](double v) { return freqladder::hpf2::snap(v); })
                   : nullptr,
               [this, idx](double v) { setBarValue(mode, idx, (int) v, true); });
}

// A node stop carries two or three values, so Enter needs an axis: the one you last
// moved (see egTypeAxis / eqTypeAxis). An AMP EG point has only one, and answers
// whatever the memory says.
void GraphicDisplayPanel::beginEgTypeIn(const EgStop& st, juce::Component* focusBack)
{
    if (eqInert()) return;
    // level < 0 is an AMP EG point: one value, the segment length, and both arrow pairs
    // already move it -- so there is nothing to choose.
    const int which = (st.level < 0) ? st.time
                                     : (egTypeAxis == 1 ? st.time : st.level);
    const int cur = egRaw(which, st.pitch);

    // Signed offsets from the SAME baseline the plot prints beside the node -- so
    // typing 0 lands on what the node reads as 0, which on a drum key is the kit's
    // loaded rate (63), not 64. Same defect as the right-click reset above.
    const int base = st.pitch ? 64 : ampBaseline(which);
    openTypeIn(st.cell, focusBack, 0.0, 127.0, (double) cur,
               [base](double v) { const int d = (int) v - base; return (d > 0 ? "+" : "") + juce::String(d); },
               [base](const juce::String& t) { return (double) juce::jlimit(0, 127, base + t.trim().getIntValue()); },
               nullptr,
               [this, which, st](double v) { egSet(which, st.pitch, (int) v); });
}

void GraphicDisplayPanel::beginEqTypeIn(const EqStop& st, juce::Component* focusBack)
{
    if (eqInert() || eqNodeLocked(st.node) || ! eqReadRaw) return;
    const int band = eqBand(st.node);          // -1 = the F handle (HPF2)

    // The F handle is a frequency and nothing else; a shelf has no Q. Both are clamped
    // HERE rather than by rewriting eqTypeAxis, so the Q you were editing is still
    // waiting when the cursor reaches the next mid band.
    int axis = eqTypeAxis;
    if (band < 0)            axis = 1;
    else if (! st.hasQ && axis == 2) axis = 0;

    const int row = (axis == 1) ? 1 : (axis == 2 ? 2 : 0);
    const int cur = eqReadRaw(band, row);
    if (cur < 0) return;                       // no such cell

    double lo = 0.0, hi = 127.0;
    if (row == 2)      { lo = 1.0;  hi = 120.0; }
    else if (row == 1)
    {
        if (band < 0) { lo = 0.0; hi = (double) kHpf2Max; }
        else          { int a, b; PartPanel::eqFreqRange(band, a, b); lo = (double) a; hi = (double) b; }
    }

    // The format and the parse are the Part module BOX's for this very cell, so what is
    // typed at a handle behaves exactly as it would in the box -- including that a
    // shelf's FREQ is an XG table index while a mid band's is Hz.
    auto fmt   = [this, band, row](double v) { return eqTextOf ? eqTextOf(band, row, v) : juce::String((int) v); };
    auto parse = [this, band, row](const juce::String& t) { return eqParse ? eqParse(band, row, t) : t.getDoubleValue(); };

    openTypeIn(st.cell, focusBack, lo, hi, (double) cur, std::move(fmt), std::move(parse),
               (row == 1 && (band < 0 || PartPanel::eqFreqIsHz(band)))
                   ? std::function<double(double)>([](double v) { return (double) freqladder::snap(v); })
                   : nullptr,
               [this, band, row](double v) { if (eqWriteRaw) eqWriteRaw(band, row, (int) v); });

    // Keep the node's own readout up while the box is open: it shows all of the node's
    // values, so you can see the two you are NOT editing.
    eqHoverNode = st.node;
    hoverShown  = true;
    if (timerJob != TimerNone) { stopTimer(); timerJob = TimerNone; }
    repaint();
}

// One stop per column, S first: reading order and part order are the same thing here.
// Cells span the BAR PLOT's height rather than the whole plot, so the mark sits over
// the bars and clear of the frequency labels above and the part ruler below.
std::vector<GraphicDisplayPanel::BarStop> GraphicDisplayPanel::barStops() const
{
    std::vector<BarStop> out;
    // channelInert covers Variation with CON=INS: the whole channel is grayed and
    // uneditable, so it offers nothing rather than 33 stops that decline every key.
    if (! isBarMode(mode) || channelInert(mode)) return out;

    const auto plot = plotRect();
    const auto bp   = barPlot(plot);
    const float colW = plot.getWidth() / (float) kBarStops;

    for (int i = 0; i < kBarStops; ++i)
    {
        if (i == 0 && ! sBarActive(mode)) continue;   // PAN has no system value
        const juce::Rectangle<float> cell (plot.getX() + (float) i * colW, bp.getY(),
                                           colW, bp.getHeight());
        out.push_back({ i, cell.getSmallestIntegerContainer() });
    }
    return out;
}

// A screen reader reads the NAME and then the value, so the name has to say which
// column and which parameter. The per-part names come from the same tooltip table the
// Part module's controls use, so the two say the same words about the same value.
juce::String GraphicDisplayPanel::barA11yName(int idx) const
{
    const auto& spec = kModes[mode];
    if (idx == 0)
    {
        // The system column. HPF2's is the plugin's own floor and has no XG param.
        if (isHpf2(mode)) return "System HPF";
        if (spec.sParam != nullptr && spec.sTable != nullptr)
            return juce::String(mu100::tooltips::forParam(spec.sTable, spec.sParam));
        return juce::String(spec.label) + " system";
    }

    const juce::String part = "Part " + juce::String(idx);
    if (isHpf2(mode)) return part + " HPF2";
    if (spec.partParam != nullptr)
        return part + " " + juce::String(mu100::tooltips::forParam("MULTI PART", spec.partParam));
    return part + " " + juce::String(spec.label);
}

//==============================================================================
// Value model
//==============================================================================
int GraphicDisplayPanel::barValue(int m, int idx) const
{
    if (isHpf2(m))   // S = the system floor, parts = their own cutoffs (both Hz)
        return (idx == 0) ? (int) audioProcessor.getSystemHpfHz()
                          : (int) audioProcessor.getCleanHpfHz(idx - 1);
    return (idx == 0) ? sVal[m] : partVal[m][idx - 1];
}

void GraphicDisplayPanel::setBarValue(int m, int idx, int v, bool send)
{
    if (isHpf2(m))
    {
        // Same rule as the Part module's HPF2 box: any integer up to 1 kHz, Off below
        // the ladder's 20 Hz floor. (This screen writes the processor directly, so it
        // has to apply the rule itself rather than inheriting the box's snapValue.)
        v = (int) freqladder::hpf2::snap(juce::jlimit(0, kHpf2Max, v));
        if (idx == 0)
        {
            audioProcessor.setSystemHpfHz((float) v);          // engine + project state
            if (send && onSystemHpfEdited) onSystemHpfEdited(v);  // the System module box
            return;
        }
        audioProcessor.setCleanHpfHz(idx - 1, (float) v);   // engine + project state
        if (send && onHpf2Edited) onHpf2Edited(idx - 1, v); // let the Part box recall
        return;                                     // no synced-knob channel to mirror
    }

    v = juce::jlimit(0, 127, v);
    if (idx == 0) { if (! sBarActive(m)) return; sVal[m] = v; }
    else            partVal[m][idx - 1] = v;
    if (send)
    {
        sendBar(m, idx, v);
        // Mirror this user edit into the matching knob (Part / System / Effect).
        if (onBarEdited) onBarEdited(channelForMode(m), idx - 1, v);   // idx 0 -> part -1 (S)
    }
}

// External mirror from a knob edit: update the stored value + repaint only; never
// re-send (the knob already did) and never fire onBarEdited (would loop).
void GraphicDisplayPanel::setExternalBar(Channel ch, int part, int value)
{
    const int m   = modeForChannel(ch);
    const int idx = part + 1;                 // part -1 -> idx 0 (S bar)
    setBarValue(m, idx, value, false);
    if (mode == m) repaint();
}

void GraphicDisplayPanel::mirrorHpf2(int /*part*/, int /*hz*/)
{
    // The processor already holds the new value (the Part box set it); the HPF2
    // bars read it live, so a repaint is all that's needed to reflect the change.
    if (mode == HPF2) repaint();
}

void GraphicDisplayPanel::mirrorSystemHpf(int /*hz*/)
{
    // As above: the floor is read live from the processor by both the S bar and the
    // rule drawn across the plot, so the repaint IS the mirror.
    if (mode == HPF2) repaint();
}

void GraphicDisplayPanel::setVariationSystemActive(bool active, int level)
{
    const bool changed = (varSysActive != active);
    varSysActive = active;
    if (active) sVal[Var] = juce::jlimit(0, 127, level);
    if (mode == Var) repaint();
    if (changed) updateScreenMarkers();   // VAR's CC mark follows the routing
}

bool GraphicDisplayPanel::barIsDefault(int m, int idx) const
{
    if (isHpf2(m)) return barValue(m, idx) == 0;               // default = Off (S too)
    const auto& spec = kModes[m];
    return barValue(m, idx) == (idx == 0 ? spec.sDefault : spec.partDefault);
}

juce::String GraphicDisplayPanel::barReadoutText(int m, int val) const
{
    if (isHpf2(m)) return val < 20 ? juce::String("Off") : juce::String(val);
    if (m == Pan)
    {
        if (val <= 0)  return "RND";
        if (val < 64)  return "L" + juce::String(64 - val);
        if (val == 64) return "C";
        return "R" + juce::String(val - 64);
    }
    return juce::String(juce::jlimit(0, 127, val));
}

//==============================================================================
// Bar-value readout: hover detection + show/hide bookkeeping
//==============================================================================
int GraphicDisplayPanel::columnAtX(juce::Point<float> pos, juce::Rectangle<float> plot) const
{
    if (pos.x < plot.getX() || pos.x > plot.getRight()) return -1;
    const int   n    = 33;
    const float colW = plot.getWidth() / (float) n;
    return juce::jlimit(0, n - 1, (int) ((pos.x - plot.getX()) / colW));
}

// Vertical extent [topY, botY] of the filled bar for column idx (mirrors paintBars).
void GraphicDisplayPanel::barVSpan(int idx, juce::Rectangle<float> plot, float& topY, float& botY) const
{
    const int val = barValue(mode, idx);
    const auto bp = barPlot(plot);   // must match paintBars or hover mis-hits
    if (mode == Pan)
    {
        const float cy = bp.getCentreY();
        const float half = bp.getHeight() * 0.5f;
        if (val == 64)     { topY = cy - 1.0f; botY = cy + 1.0f; }
        else if (val > 64) { const float h = (float) (val - 64) / 63.0f * half; topY = cy - h; botY = cy; }
        else               { const float h = (float) (64 - val) / 64.0f * half; topY = cy; botY = cy + h; }
    }
    else
    {
        const int   mx = isHpf2(mode) ? kHpf2Max : 127;
        const float h  = (float) juce::jlimit(0, mx, val) / (float) mx * bp.getHeight();
        topY = bp.getBottom() - h;
        botY = bp.getBottom();
    }
}

// The column whose FILLED bar the cursor is over (interactive bars only), or -1.
// A cursor in the column but above/below the filled bar returns -1 (off-bar).
int GraphicDisplayPanel::barHoverIdx(juce::Point<float> pos, juce::Rectangle<float> plot) const
{
    if (! isBarMode(mode) || channelInert(mode)) return -1;
    if (axisRowRect().contains(pos)) return -1;      // part ruler, not the graph
    const int idx = columnAtX(pos, plot);
    if (idx < 0) return -1;
    if (idx == 0 && ! sBarActive(mode)) return -1;   // inert S bar has no readout
    float t, b; barVSpan(idx, plot, t, b);
    const float tol = 2.5f;                          // a little slack at the ends
    return (pos.y >= t - tol && pos.y <= b + tol) ? idx : -1;
}

bool GraphicDisplayPanel::shouldShowReadout(int i) const
{
    if (dragging && dragIdx == i) return true;   // active drag
    if (hoverShown && hoverIdx == i) return true;// cursor resting on the bar
    if (transientIdx == i) return true;          // brief off-bar adjust readout
    return false;
}

void GraphicDisplayPanel::startHoverDelay(int idx)
{
    hoverIdx = idx; hoverShown = false; transientIdx = -1;
    timerJob = TimerHoverDelay;
    startTimer(kHoverDelayMs);
}

void GraphicDisplayPanel::startTransient(int idx)
{
    transientIdx = idx; hoverIdx = -1; hoverShown = false;
    timerJob = TimerTransientHide;
    startTimer(kTransientMs);
}

void GraphicDisplayPanel::timerCallback()
{
    stopTimer();
    const TimerJob job = timerJob;
    timerJob = TimerNone;
    if (job == TimerHoverDelay)          hoverShown = true;   // delay elapsed -> show
    else if (job == TimerTransientHide)  transientIdx = -1;   // linger elapsed -> hide
    repaint();
}

void GraphicDisplayPanel::sendBar(int m, int idx, int v)
{
    using mu100::AddressMap;
    // sendParamChange (not sendSysEx) so the edit is also recorded into the shared
    // editor model — that keeps the Part/Effect modules consistent when the user
    // later switches part or effect unit and those panels re-read the model.
    if (idx == 0)
    {
        // The Variation "S" bar has no AddressMap address (effect-block addressing
        // + CON gating), so it is transmitted by the Effect module instead: the
        // EditorView routes onBarEdited(Var, -1, v) to EffectPanel::setVariationSystemLevel.
        if (m == Var) return;
        if (sParamIdx[m] < 0) return;
        audioProcessor.sendParamChangeThrottled(AddressMap::makeParamChange(
            AddressMap::param(sParamIdx[m]), 0, (uint32_t) v));
    }
    else
    {
        if (partParamIdx[m] < 0) return;
        audioProcessor.sendParamChangeThrottled(AddressMap::makeParamChange(
            AddressMap::param(partParamIdx[m]), idx - 1, (uint32_t) v));
    }
}

void GraphicDisplayPanel::sendAmp(int which, int v)
{
    using mu100::AddressMap;
    if (drumEditActive)
    {
        // DRUM SETUP amp EG params are RATES (higher = faster = shorter segment),
        // but the envelope graphic is drawn time-wise (longer segment = higher
        // value, as for the part EG TIME params). Send the complement so the
        // audible envelope matches the on-screen shape and displayed value.
        if (drumAmpParam[which] >= 0 && drumKey >= 0)
            audioProcessor.sendParamChangeThrottled(AddressMap::makeParamChange(
                AddressMap::param(drumAmpParam[which]), drumKey, (uint32_t) (127 - v), drumSetup));
        return;
    }
    if (ampParam[which] < 0) return;
    audioProcessor.sendParamChangeThrottled(AddressMap::makeParamChange(
        AddressMap::param(ampParam[which]), currentPart, (uint32_t) v));
}

void GraphicDisplayPanel::sendPitch(int which, int v)
{
    using mu100::AddressMap;
    if (pitchEgInert()) return;   // a drum part ignores PITCH EG; DRUM SETUP has none
    if (pitchParam[which] < 0) return;
    audioProcessor.sendParamChangeThrottled(AddressMap::makeParamChange(
        AddressMap::param(pitchParam[which]), currentPart, (uint32_t) v));
}

void GraphicDisplayPanel::setDrumEdit(bool active, int note, int setup)
{
    drumSetup = juce::jlimit(0, 3, setup);
    const bool modeChanged = (active != drumEditActive);
    const bool keyChanged  = (note  != drumKey);
    drumKey = note;
    drumEditActive = active;

    if (drumEditActive && drumKey >= 0)
    {
        if (modeChanged || keyChanged) loadDrumAmp();   // show the key's real rates
    }
    else if (modeChanged && active)
        drumAmpVal[0] = drumAmpVal[1] = drumAmpVal[2] = 64;   // entered, no key yet

    // The probe narrows to the selected drum key, and eqInert() (no key chosen)
    // turns it off entirely, so both transitions have to re-decide.
    if (modeChanged || keyChanged) updateSpectrumTarget();

    // The ghosts are per (key, buffer) in this mode, so either moving makes them
    // stale -- and a drum-key click reaches no other refresh path.
    if (modeChanged || keyChanged) refreshMidiOverrides();

    if (modeChanged || keyChanged) repaint();

    // AMP EG is automatable only while it edits a drum KEY, and EQ only while it does
    // not, so the marks follow the mode -- and the key, since picking or clearing one
    // changes AMP EG's answer with the mode unchanged.
    if (modeChanged || keyChanged) updateScreenMarkers();
}

// Read the selected drum key's DRUM SETUP amp-EG rates from the model into the
// working values. The rates are stored inverted (see sendAmp): the device holds
// the complement of the displayed value, so undo it here for a matching graphic.
void GraphicDisplayPanel::loadDrumAmp()
{
    using mu100::AddressMap;
    const auto& st = audioProcessor.getEditorState();
    for (int i = 0; i < 3; ++i)
    {
        int v = 64;
        drumAmpDflt[i] = 64;
        if (drumAmpParam[i] >= 0 && drumKey >= 0)
        {
            const auto& pd = AddressMap::param(drumAmpParam[i]);
            const auto r = AddressMap::get(st, pd, drumKey, drumSetup);
            if (r.present) v = 127 - r.combined;

            // The baseline for the readout and the yellow "changed" marker. It is NOT
            // 64 here: the rates are stored inverted, so the kit's own default (0x40
            // on every kit and key measured) comes back as 127 - 64 = 63, and a
            // hardcoded 64 made every freshly loaded kit read "-1" on all three nodes
            // and paint them edited. USER-REPORTED 2026-09-08. Taken from the table
            // rather than assumed, so a kit whose EG defaults differ stays right.
            mu100::ModelIx ix {};
            uint32_t addr = 0;
            AddressMap::resolve(pd, drumKey, drumSetup, ix, addr);
            const int d = audioProcessor.drumDefaultByte(drumSetup, drumKey,
                                                         (uint8_t) (addr & 0x7F));
            if (d >= 0) drumAmpDflt[i] = 127 - d;
        }
        drumAmpVal[i] = v;
    }
}

// Read every bar mode's system ("S") and per-part value from the device-state
// model. Only values actually present in the dump overwrite the local model, so a
// partial dump leaves the rest at their prior (default) values.
void GraphicDisplayPanel::loadBars(const mu100::DeviceState& st)
{
    using mu100::AddressMap;
    for (int m = 0; m < NumModes; ++m)
    {
        if (! isBarMode(m)) continue;
        if (sParamIdx[m] >= 0)
        {
            const auto r = AddressMap::get(st, sParamIdx[m], 0);
            if (r.present) sVal[m] = juce::jlimit(0, 127, r.combined);
        }
        if (partParamIdx[m] >= 0)
            for (int p = 0; p < 32; ++p)
            {
                const auto r = AddressMap::get(st, partParamIdx[m], p);
                if (r.present) partVal[m][p] = juce::jlimit(0, 127, r.combined);
            }
    }
}

// Read every part's amp-EG (attack/decay/release) and pitch-EG points from the
// model. Same present-only guard as loadBars.
void GraphicDisplayPanel::loadPartEG(const mu100::DeviceState& st)
{
    using mu100::AddressMap;
    for (int p = 0; p < 32; ++p)
    {
        for (int i = 0; i < 3; ++i)
            if (ampParam[i] >= 0)
            {
                const auto r = AddressMap::get(st, ampParam[i], p);
                if (r.present) ampVal[p][i] = juce::jlimit(0, 127, r.combined);
            }
        for (int i = 0; i < 4; ++i)
            if (pitchParam[i] >= 0)
            {
                const auto r = AddressMap::get(st, pitchParam[i], p);
                if (r.present) pitchVal[p][i] = juce::jlimit(0, 127, r.combined);
            }
    }
}

//==============================================================================
// The AMP EG ghost nodes, in whichever mode the screen is in. In drum-edit mode the
// envelope addresses DRUM SETUP by (key, buffer) instead of MULTI PART by part, and
// the ghost has to follow that switch or it would draw the part's device value on a
// screen showing the drum key's.
//
// THE DRUM VALUE IS COMPLEMENTED, exactly as loadDrumAmp and sendAmp complement it:
// DRUM SETUP holds these as RATES (higher = faster = shorter) while the screen is
// drawn time-wise. A ghost taken raw would sit mirrored about the middle of the plot,
// which is not obviously wrong to look at -- it is just wrong.
void GraphicDisplayPanel::refreshMidiOverrides()
{
    using mu100::AddressMap;
    const bool show  = audioProcessor.showMidiOverrides;
    const bool drums = drumEditActive;
    const bool live  = show && (! drums || drumKey >= 0);

    bool moved = false;
    for (int i = 0; i < 3; ++i)
    {
        const int ix = drums ? drumAmpParam[i] : ampParam[i];
        int v = -1;
        if (live && ix >= 0)
        {
            v = drums ? audioProcessor.midiOverrideValue(ix, drumKey, drumSetup)
                      : audioProcessor.midiOverrideValue(ix, currentPart);
            if (drums && v >= 0) v = 127 - v;
        }
        if (v != ampOverride[i]) { ampOverride[i] = v; moved = true; }
    }
    const bool ampMoved = moved;

    // The BAR screens, every one of them rather than only the one on show: the table
    // is small, and keeping it current means a mode switch draws the right ticks
    // immediately instead of waiting for the next override message. Column 0 is the
    // S value (a SYSTEM parameter, reachable by SysEx alone -- no XG controller
    // addresses one), columns 1..32 are the parts.
    //
    // Not gated on drum-edit mode, unlike the envelope ghosts above: these channels
    // are MULTI PART whichever mode the panel is in, so the marks mean the same thing
    // either way.
    bool barMoved = false;
    for (int m = 0; m < NumModes; ++m)
    {
        if (! isBarMode(m)) continue;
        for (int i = 0; i < kBarStops; ++i)
        {
            const int ix = (i == 0) ? sParamIdx[m] : partParamIdx[m];
            int v = -1;
            if (show && ix >= 0)
                v = audioProcessor.midiOverrideValue(ix, i == 0 ? 0 : i - 1);
            if (v != barOverride[m][i])
            {
                barOverride[m][i] = v;
                if (m == mode) barMoved = true;
            }
        }
    }

    // ...and the EQ screen's two hardware shelves, through the Part module's bridge so
    // the grid's gutter bars and these ghosts read the same registers. The mid bands
    // and HPF2 are plugin-only and always answer -1.
    bool eqMoved = false;
    for (int band = 0; band < 5; ++band)
        for (int row = 0; row < 2; ++row)
        {
            const int v = (show && eqReadOverride) ? eqReadOverride(band, row) : -1;
            if (v != eqOverride[band][row]) { eqOverride[band][row] = v; eqMoved = true; }
        }

    if ((ampMoved && mode == Mode::AmpEG) || barMoved || (eqMoved && mode == Mode::EQ))
        repaint();
}

void GraphicDisplayPanel::applyState(const mu100::DeviceState& st)
{
    loadBars(st);
    loadPartEG(st);
    // While drum-editing, the AMP EG screen shows the drum key's rates (which
    // override the part amp values loaded above).
    if (drumEditActive && drumKey >= 0) loadDrumAmp();
    repaint();
}

//==============================================================================
// Mouse
//==============================================================================
// Open / close the undo gesture that makes a two-axis handle drag ONE step. See the
// dragUndoOpen comment in the header for why this is explicit rather than an
// UndoScope object, and why endDragUndo has to tolerate being called with none open.
void GraphicDisplayPanel::beginDragUndo(const juce::String& label)
{
    endDragUndo();                       // a drag left open by anything else
    audioProcessor.undoGestureBegin(label);
    dragUndoOpen = true;
}

void GraphicDisplayPanel::endDragUndo()
{
    if (! dragUndoOpen) return;
    dragUndoOpen = false;
    audioProcessor.undoGestureEnd();
}

// The part ruler carries the SAME tooltip as the Voice module's Pt column, taken
// from the same key: the two controls do the same job, and giving them one string
// keeps them saying so. Position-dependent (the row is painted, not a child
// component), exactly as PartListPanel resolves its own painted columns.
juce::String GraphicDisplayPanel::getTooltip()
{
    const auto pos = getMouseXYRelative().toFloat();
    if (! axisRowRect().contains(pos)) return {};
    const int p = partAtAxisX(pos);
    if (p < 0) return {};
    return juce::String(mu100::tooltips::forKey("VOICE|PART SELECT"))
               .replace("{n}", juce::String(p + 1));
}

void GraphicDisplayPanel::mouseDown(const juce::MouseEvent& e)
{
    auto plot = plotRect();
    const auto pos = e.position;

    if (isEqMode(mode))
    {
        // RANGE selector first. It only zooms the axis, so unlike the nodes it stays
        // live even when the screen is inert (drum-edit with no key selected) — you
        // may well want to set the view up before picking a key.
        {
            juce::Rectangle<float> label, opt12, opt24;
            eqRangeLayout(label, opt12, opt24);
            const int r = opt12.contains(pos) ? 12 : (opt24.contains(pos) ? 24 : 0);
            if (r != 0)
            {
                if (r != eqRangeDb) { eqRangeDb = r; repaint(); }
                return;
            }
        }

        if (eqInert()) return;                        // drum-edit with no key selected
        const int node = eqNodeAt(pos, plot);
        if (node < 0) return;
        if (e.mods.isRightButtonDown())
        {
            if (eqResetBand) eqResetBand(eqBand(node));   // band -1 = HPF2
            repaint();
            return;
        }
        eqHoverNode = -1; hoverShown = false;
        if (timerJob != TimerNone) { stopTimer(); timerJob = TimerNone; }
        // Nodes 1..5 move on two axes at once (frequency + gain, or + Q under Alt),
        // so the drag is one gesture writing two addresses. Node 0 (HPF2) moves in
        // frequency only, and is better left to the address merge — that keeps its
        // generated "Part 5 HPF2 250 Hz -> 400 Hz" label instead of a fixed one.
        if (node >= 1)
            beginDragUndo((drumEditActive ? "Drum " + drumKeyName
                                          : "Part " + juce::String(currentPart + 1))
                          + " EQ " + juce::String(node) + " node");
        dragging = true;
        dragIdx  = node;
        captureEqDragStart(node);
        dragStartPos = e.getPosition();
        repaint();
        return;
    }

    if (isBarMode(mode))
    {
        // The x-axis part ruler selects, it does not edit — and it answers BEFORE the
        // inert-channel test, since picking a part is worth doing on a screen whose
        // bars are disabled (Variation with CON=INS) just as much as on a live one.
        //
        // Right-clicking a BAR is its reset gesture; on the ruler that meant nothing and
        // was swallowed, which is exactly what left the button free for the part menu.
        // It selects first, for the reason the Voice module's rows do.
        if (axisRowRect().contains(pos))
        {
            const int p = partAtAxisX(pos);
            if (p >= 0 && onSelectPart) onSelectPart(p);
            if (p >= 0 && e.mods.isRightButtonDown() && onPartContextMenu) onPartContextMenu(p);
            return;
        }

        if (channelInert(mode)) return;   // whole channel disabled (Variation when CON=INS)
        const int n = 33;
        const float colW = plot.getWidth() / (float) n;
        if (pos.x < plot.getX() || pos.x > plot.getRight()) return;
        const int idx = juce::jlimit(0, n - 1, (int) ((pos.x - plot.getX()) / colW));
        if (idx == 0 && ! sBarActive(mode)) return;   // inert S

        if (e.mods.isRightButtonDown())
        {
            barReset(idx);   // the same body Backspace calls from the keyboard
            repaint();
            return;
        }
        // Starting a drag: the drag itself shows the readout; drop any hover/
        // transient bookkeeping so it doesn't linger on the wrong bar.
        hoverIdx = -1; hoverShown = false; transientIdx = -1;
        if (timerJob != TimerNone) { stopTimer(); timerJob = TimerNone; }
        dragging = true;
        dragIdx = idx;
        dragStartVal = barValue(mode, idx);
        dragStartPos = e.getPosition();
        repaint();
        return;
    }

    // EG modes: pick the nearest editable point.
    if (mode == AmpEG)
    {
        const auto pts = ampPoints(plot);
        const int editPt[3] = { 1, 2, 4 };
        int best = -1; float bestD = 16.0f;
        for (int e2 = 0; e2 < 3; ++e2)
        {
            const float d = pts[(size_t) editPt[e2]].pos.getDistanceFrom(pos);
            if (d < bestD) { bestD = d; best = e2; }   // best = which (0=atk,1=dcy,2=rel)
        }
        if (best < 0) return;
        if (e.mods.isRightButtonDown())
        {
            // The same function Backspace calls, so the two cannot drift.
            egReset({ -1, best, false, {}, nullptr, nullptr });
            return;
        }
        dragging = true; dragIdx = best;
        dragStartVal = ampSlot(best);
        dragStartPos = e.getPosition();
        repaint();
    }
    else // PitchEG
    {
        if (pitchEgInert()) return;   // drum part: nothing here reaches the device
        const auto geo = pitchGeom(plot);
        const float d1 = geo.p1.getDistanceFrom(pos);
        const float d2 = geo.p2.getDistanceFrom(pos);
        int pt = -1;
        if (d1 <= d2 && d1 < 16.0f) pt = 0;
        else if (d2 < 16.0f)        pt = 1;
        if (pt < 0) return;

        const int xWhich = (pt == 0) ? 1 : 3;   // attack/release TIME
        const int yWhich = (pt == 0) ? 0 : 2;   // initial/release LEVEL
        if (e.mods.isRightButtonDown())
        {
            // The same function Backspace calls (it opens the one undo step this needs
            // -- two addresses, and an address-keyed merge would otherwise make two).
            egReset({ yWhich, xWhich, true, {}, nullptr, nullptr });
            return;
        }
        // Both axes move together here too: TIME horizontally, LEVEL vertically. Which
        // of them Enter will type into is decided by the drag's own direction, in
        // mouseDrag, and only when it was decisive.
        beginDragUndo("Part " + juce::String(currentPart + 1) + " PITCH EG P"
                      + juce::String(pt + 1));
        dragging = true; dragIdx = pt;
        dragStartVal  = pitchVal[currentPart][xWhich];
        dragStartVal2 = pitchVal[currentPart][yWhich];
        dragStartPos = e.getPosition();
        repaint();
    }
}

void GraphicDisplayPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (! dragging) return;
    auto plot = plotRect();
    const bool  fineMod = e.mods.isShiftDown();
    const float fine    = fineMod ? 0.22f : 1.0f;

    if (isEqMode(mode))
    {
        const int   node = dragIdx;
        const bool  alt  = e.mods.isAltDown();
        const float dx   = (float) (e.getPosition().x - dragStartPos.x);
        const float dy   = (float) (e.getPosition().y - dragStartPos.y);
        const float perX = plot.getWidth()  * 0.50f;   // half the plot spans the full range
        const float perY = plot.getHeight() * 0.85f;

        if (node == 0)   // HPF2 "F": frequency only (no gain / no Q)
        {
            // Log sweep of 20..1000 Hz (Shift = 1 Hz ladder steps), so the node behaves
            // exactly like the HPF2 box — including where Off begins.
            const int nv = (dragStartVal < freqladder::kMin && dx <= 0.0f)
                             ? 0
                             : eqDragFreq(juce::jmax(dragStartVal, freqladder::kMin), dx, perX, fineMod,
                                          0, GraphicDisplayPanel::kHpf2Max);
            if (eqWriteRaw) eqWriteRaw(-1, 0, nv);
        }
        else
        {
            const int band = node - 1;
            // WHICH AXIS was this drag? It moves two values at once, so it only names
            // one when it was decisively one-directional; a diagonal leaves the memory
            // alone rather than guessing. An Alt-drag says Q outright.
            if (alt)                                          eqTypeAxis = 2;
            else if (std::abs(dy) > kAxisDominance * std::abs(dx)) eqTypeAxis = 0;
            else if (std::abs(dx) > kAxisDominance * std::abs(dy)) eqTypeAxis = 1;

            int lo, hi; PartPanel::eqFreqRange(band, lo, hi);
            // Mid bands are Hz on the ladder -> log sweep, Shift = ladder steps. The
            // shelves are XG table indices, which are already quasi-log and only have
            // ~30 entries, so they keep stepping linearly with the proportional fine.
            const int nf = PartPanel::eqFreqIsHz(band)
                             ? eqDragFreq(dragStartVal, dx, perX, fineMod, lo, hi)
                             : juce::jlimit(lo, hi, dragStartVal + juce::roundToInt(dx / perX * (float) (hi - lo) * fine));
            if (eqWriteRaw) eqWriteRaw(band, 1, nf);

            const bool hasQ = (band >= 1 && band <= 3);
            if (! alt)   // vertical = gain
            {
                // Scale with the RANGE zoom so the node keeps tracking the cursor: at
                // ±24 a dB is half as many pixels, so the same drag must cover twice
                // the gain. (Precision follows the zoom — that's what Shift is for.)
                const float span  = 128.0f * (float) (eqRangeDb / kEqBandGainMaxDb);
                const float dRawG = -dy / perY * span * fine;
                const int   ng = juce::jlimit(0, 127, dragStartVal2 + juce::roundToInt(dRawG));
                if (eqWriteRaw) eqWriteRaw(band, 0, ng);
            }
            else if (hasQ)   // Alt: vertical = Q (gain held), horizontal still = freq
            {
                const float dRawQ = -dy / perY * 120.0f * fine;
                const int   nq = juce::jlimit(1, 120, dragStartVal3 + juce::roundToInt(dRawQ));
                if (eqWriteRaw) eqWriteRaw(band, 2, nq);
            }
        }
        repaint();
        return;
    }

    if (isBarMode(mode))
    {
        const int   rng     = isHpf2(mode) ? kHpf2Max : 127;
        const float perFull = plot.getHeight() * 0.85f;   // full vertical range = whole span
        const float dv = (float) (dragStartPos.y - e.getPosition().y) * ((float) rng / perFull) * fine;
        const int nv = dragStartVal + juce::roundToInt(dv);
        // No 10 Hz snap on an HPF2 plain drag any more: every integer is reachable, and
        // this axis is linear Hz, so the bar simply tracks the mouse (Shift = fine).
        setBarValue(mode, dragIdx, nv, true);
        repaint();
    }
    else if (mode == AmpEG)
    {
        const float perFull = plot.getWidth() * 0.45f;
        const float dv = (float) (e.getPosition().x - dragStartPos.x) * (127.0f / perFull) * fine;
        const int v = juce::jlimit(0, 127, dragStartVal + juce::roundToInt(dv));
        if (v != ampSlot(dragIdx)) { ampSlot(dragIdx) = v; sendAmp(dragIdx, v); }
        repaint();
    }
    else // PitchEG
    {
        const int xWhich = (dragIdx == 0) ? 1 : 3;
        const int yWhich = (dragIdx == 0) ? 0 : 2;
        const float perFullX = plot.getWidth() * 0.45f;
        const float perFullY = plot.getHeight() * 0.80f;
        // Both points follow the mouse: the left point (P1) extends leftward, so
        // its time value increases as the mouse moves LEFT; the right point (P2)
        // extends rightward, so its time increases as the mouse moves right.
        // Level (y) increases as the mouse moves up for both. (mouse-up = +level)
        const float dx = (dragIdx == 0) ? (float) (dragStartPos.x - e.getPosition().x)
                                        : (float) (e.getPosition().x - dragStartPos.x);
        const float dvx = dx * (127.0f / perFullX) * fine;
        const float dyPix = (float) (dragStartPos.y - e.getPosition().y);
        const float dvy = dyPix * (127.0f / perFullY) * fine;

        // Which axis Enter will type into, if this drag was decisively one-directional.
        // Measured in PIXELS, not in the two values -- the axes have different scales,
        // so comparing the value changes would call a mostly-vertical drag horizontal.
        // A diagonal leaves the memory where it was rather than guessing.
        if (std::abs(dyPix) > kAxisDominance * std::abs(dx))      egTypeAxis = 0;
        else if (std::abs(dx) > kAxisDominance * std::abs(dyPix)) egTypeAxis = 1;
        const int vx = juce::jlimit(0, 127, dragStartVal  + juce::roundToInt(dvx));
        const int vy = juce::jlimit(0, 127, dragStartVal2 + juce::roundToInt(dvy));
        if (vx != pitchVal[currentPart][xWhich]) { pitchVal[currentPart][xWhich] = vx; sendPitch(xWhich, vx); }
        if (vy != pitchVal[currentPart][yWhich]) { pitchVal[currentPart][yWhich] = vy; sendPitch(yWhich, vy); }
        repaint();
    }
}

void GraphicDisplayPanel::mouseUp(const juce::MouseEvent& e)
{
    endDragUndo();                   // before the early return: a gesture may be open
    if (! dragging) return;
    const int idx = dragIdx;
    dragging = false;
    dragIdx = -1;

    // End the undo step here rather than kCoalesceMs later, like every other control
    // that can be dragged (XgKnob, ValueBox). Matters most for the single-parameter
    // drags that open no gesture at all — the bars and the AMP EG.
    mu100undo::requestSeal();

    if (isEqMode(mode))
    {
        // Keep the readout up if the cursor is still resting on the handle.
        const int over = eqNodeAt(e.position, plotRect());
        eqHoverNode = (over == idx) ? idx : -1;
        hoverShown  = (over == idx);
        repaint();
        return;
    }

    // After a bar drag: if the cursor is still on the filled bar keep the readout
    // up (hover); if it ended above/below the bar, let it linger briefly then hide.
    if (isBarMode(mode) && idx >= 0)
    {
        if (barHoverIdx(e.position, plotRect()) == idx)
        {
            hoverIdx = idx; hoverShown = true; transientIdx = -1;
            if (timerJob != TimerNone) { stopTimer(); timerJob = TimerNone; }
        }
        else
            startTransient(idx);
    }
    repaint();   // drop the "active" highlight (keeps yellow only if changed)
}

void GraphicDisplayPanel::mouseMove(const juce::MouseEvent& e)
{
    if (isEqMode(mode))
    {
        if (eqInert()) return;
        const int n = eqNodeAt(e.position, plotRect());
        if (n >= 0)
        {
            if (n != eqHoverNode || (! hoverShown && timerJob != TimerHoverDelay))
            {
                eqHoverNode = n; hoverShown = false; transientIdx = -1;
                timerJob = TimerHoverDelay; startTimer(kHoverDelayMs);
                repaint();
            }
        }
        else if (eqHoverNode != -1 || hoverShown)
        {
            eqHoverNode = -1; hoverShown = false;
            if (timerJob == TimerHoverDelay) { stopTimer(); timerJob = TimerNone; }
            repaint();
        }
        return;
    }

    if (! isBarMode(mode)) return;   // EG screens draw their own always-on readouts
    const int idx = barHoverIdx(e.position, plotRect());
    if (idx >= 0)
    {
        // New bar, or resting on one with no hover pending/shown -> start the delay.
        if (idx != hoverIdx || (! hoverShown && timerJob != TimerHoverDelay))
        {
            startHoverDelay(idx);
            repaint();   // clear any readout still showing on a previous bar
        }
    }
    else if (hoverIdx != -1 || hoverShown)   // moved off the filled bar
    {
        hoverIdx = -1; hoverShown = false;
        if (timerJob == TimerHoverDelay) { stopTimer(); timerJob = TimerNone; }
        repaint();
    }
}

void GraphicDisplayPanel::mouseExit(const juce::MouseEvent&)
{
    if (hoverIdx != -1 || eqHoverNode != -1 || hoverShown)
    {
        hoverIdx = -1; eqHoverNode = -1; hoverShown = false;
        if (timerJob == TimerHoverDelay) { stopTimer(); timerJob = TimerNone; }
        repaint();
    }
}

void GraphicDisplayPanel::mouseWheelMove(const juce::MouseEvent& e,
                                         const juce::MouseWheelDetails& wheel)
{
    const int step = (wheel.deltaY > 0.0f) ? 1 : (wheel.deltaY < 0.0f ? -1 : 0);
    if (step == 0) return;
    auto plot = plotRect();
    const auto pos = e.position;

    if (isEqMode(mode))
    {
        if (eqInert()) return;
        const int node = eqNodeAt(pos, plot);
        if (node < 1) return;                 // F (0) has no Q; miss = -1
        const int band = node - 1;
        if (band < 1 || band > 3) return;     // shelves have no Q
        const int q = eqReadRaw ? eqReadRaw(band, 2) : -1;
        if (q < 0) return;
        const int nq = juce::jlimit(1, 120, q + step);   // up = higher Q = narrower
        eqTypeAxis = 2;                                  // the wheel here means Q, and only Q
        if (eqWriteRaw) eqWriteRaw(band, 2, nq);
        eqHoverNode = node; hoverShown = true;           // show the Q readout
        if (timerJob != TimerNone) { stopTimer(); timerJob = TimerNone; }
        repaint();
        return;
    }

    if (isBarMode(mode))
    {
        if (channelInert(mode)) return;   // whole channel disabled (Variation when CON=INS)
        if (axisRowRect().contains(pos)) return;   // part ruler: selects, never adjusts
        const int n = 33;
        const float colW = plot.getWidth() / (float) n;
        if (pos.x < plot.getX() || pos.x > plot.getRight()) return;
        const int idx = juce::jlimit(0, n - 1, (int) ((pos.x - plot.getX()) / colW));
        if (idx == 0 && ! sBarActive(mode)) return;
        barNudge(idx, step, e.mods.isShiftDown());
        // On-bar wheel: readout persists until the cursor leaves the bar. Off-bar
        // (cursor above/below the bar): brief transient readout (hidden ~0.5s later).
        if (barHoverIdx(pos, plot) == idx)
        {
            hoverIdx = idx; hoverShown = true; transientIdx = -1;
            if (timerJob != TimerNone) { stopTimer(); timerJob = TimerNone; }
        }
        else
            startTransient(idx);
        repaint();
    }
    else if (mode == AmpEG)
    {
        const auto pts = ampPoints(plot);
        const int editPt[3] = { 1, 2, 4 };
        int best = -1; float bestD = 16.0f;
        for (int w = 0; w < 3; ++w)
        {
            const float d = pts[(size_t) editPt[w]].pos.getDistanceFrom(pos);
            if (d < bestD) { bestD = d; best = w; }
        }
        if (best < 0) return;
        // kEgStep per notch, 1 with Shift -- the SAME law the arrow keys use over the
        // same handle, which is the whole reason the wheel moved off 1 per notch: one
        // step in 127 is a tolerable wheel but an unusable keyboard, and the two must
        // not disagree about what a notch is worth.
        const int d = step * (e.mods.isShiftDown() ? 1 : kEgStep);
        const int v = juce::jlimit(0, 127, ampSlot(best) + d);
        ampSlot(best) = v; sendAmp(best, v); repaint();
    }
    else // PitchEG — wheel nudges the level (vertical) of the nearest point.
    {
        if (pitchEgInert()) return;   // drum part: nothing here reaches the device
        const auto geo = pitchGeom(plot);
        const int pt = (geo.p1.getDistanceFrom(pos) <= geo.p2.getDistanceFrom(pos)) ? 0 : 1;
        if ((pt == 0 ? geo.p1 : geo.p2).getDistanceFrom(pos) > 18.0f) return;
        const int yWhich = (pt == 0) ? 0 : 2;
        const int d = step * (e.mods.isShiftDown() ? 1 : kEgStep);
        const int v = juce::jlimit(0, 127, pitchVal[currentPart][yWhich] + d);
        pitchVal[currentPart][yWhich] = v; sendPitch(yWhich, v); repaint();
    }
}

//==============================================================================
// Keyboard + screen-reader access to the envelope nodes. See the header.

// One stop per NODE, marked on the node itself, so the ring is always where the
// handle is -- including after a value has moved it, and after a mouse click, which
// navPointAt matches against these same rects.
std::vector<GraphicDisplayPanel::EgStop> GraphicDisplayPanel::egStops() const
{
    std::vector<EgStop> out;
    const auto plot = plotRect();
    if (plot.isEmpty()) return out;

    // Small enough that keynav::cueFor draws a closed ring rather than corner
    // brackets, which is right for a handle with no text inside it. The readout
    // spacing is derived from the same constant -- see kEgReadoutGap.
    const int half = kEgMarkHalf;
    auto cellAt = [half](juce::Point<float> p)
    {
        return juce::Rectangle<int>((int) p.x - half, (int) p.y - half, half * 2, half * 2);
    };

    if (mode == AmpEG)
    {
        // In drum-edit mode the AMP EG retargets to the selected DRUM KEY's rates, and
        // with no key picked sendAmp drops the write -- so there is nothing here to put
        // a cursor on. Same judgement the Part module's drum-key gating makes, and the
        // same reason: an edit that reaches neither the device nor the model, with the
        // control moving as if it had, is worse than a control that declines.
        if (drumEditActive && drumKey < 0) return out;

        const auto pts = ampPoints(plot);
        const int  node[3] = { 1, 2, 4 };
        const char* names[3] = { "EG ATTACK TIME", "EG DECAY TIME", "EG RELEASE TIME" };
        const char* drumNames[3] = { "EG ATTACK RATE", "EG DECAY1 RATE", "EG DECAY2 RATE" };
        for (int i = 0; i < 3; ++i)
            out.push_back({ -1, i, false, cellAt(pts[(size_t) node[i]].pos),
                            drumEditActive ? "DRUM SETUP" : "MULTI PART",
                            drumEditActive ? drumNames[i] : names[i] });
    }
    else if (mode == PitchEG)
    {
        // A drum part ignores PITCH EG outright, and the screen greys itself to say so
        // -- the same test mouseDown, the wheel and sendPitch all make.
        if (pitchEgInert()) return out;

        const auto geo = pitchGeom(plot);
        // pitchVal: 0 = initial level, 1 = attack time, 2 = release level, 3 = release
        // time. P1 is (level 0, time 1); P2 is (level 2, time 3).
        out.push_back({ 0, 1, true, cellAt(geo.p1), "DISP", "PITCH EG P1" });
        out.push_back({ 2, 3, true, cellAt(geo.p2), "DISP", "PITCH EG P2" });
    }

    return out;
}

int GraphicDisplayPanel::egRaw(int which, bool pitch) const
{
    return pitch ? pitchVal[currentPart][which] : ampSlot(which);
}

void GraphicDisplayPanel::egSet(int which, bool pitch, int raw)
{
    raw = juce::jlimit(0, 127, raw);
    if (raw == egRaw(which, pitch)) return;   // a no-op send wastes a message and an undo record

    if (pitch) { pitchVal[currentPart][which] = raw; sendPitch(which, raw); }
    else       { ampSlot(which) = raw;               sendAmp  (which, raw); }
    repaint();
}

// Both of a node's parameters at once, which is what the mouse's right-click has
// always done here. ONE undo step: a PITCH EG node is two ADDRESSES and the undo merge
// is keyed on the address, so two bare sends would be two steps and one Undo would
// leave the node halfway back.
void GraphicDisplayPanel::egReset(const EgStop& st)
{
    if (st.level < 0)   // AMP EG: one address, so no scope to open
    {
        // ampBaseline, NOT 64. In drum-edit mode these are stored as RATES and the
        // screen complements them, so the value the key loads with is 127 - 0x40 = 63.
        // Resetting to 64 landed one step ABOVE the default -- the node read "+1" and
        // painted itself edited, which is the readout's own baseline disagreeing with
        // the reset. USER-REPORTED 2026-09-18. Part mode is unaffected: ampBaseline
        // answers 64 there, which is what this always meant to say.
        egSet(st.time, st.pitch, ampBaseline(st.time));
        return;
    }

    YamahaMU100AudioProcessor::UndoScope undoStep(
        audioProcessor, "Part " + juce::String(currentPart + 1) + " PITCH EG "
                        + juce::String(st.time == 1 ? "P1" : "P2") + " reset");
    egSet(st.time,  st.pitch, 64);
    egSet(st.level, st.pitch, 64);
}

// What the plot prints beside the node, so a screen reader says what a sighted user
// reads: signed -64..+63, and for a PITCH EG node the same "time, level" pair the
// plot draws.
juce::String GraphicDisplayPanel::egText(const EgStop& st) const
{
    // Relative to the same baseline the PLOT prints against (see paintAmpEG), or the
    // screen reader would say "+1" where a sighted user reads "0" on a freshly loaded
    // drum kit. A PITCH EG node is MULTI PART only, so 64 is right for both its axes.
    const int base = st.pitch ? 64 : ampBaseline(st.time);
    auto one = [](int raw, int b) { const int d = raw - b; return (d > 0 ? "+" : "") + juce::String(d); };
    if (st.level < 0) return one(egRaw(st.time, st.pitch), base);
    return one(egRaw(st.time, st.pitch), base) + ", " + one(egRaw(st.level, st.pitch), 64);
}

//==============================================================================
// Keyboard + screen-reader access to the EQ nodes. See the header.

int GraphicDisplayPanel::eqMarkHalf(juce::Rectangle<float> plot) const
{
    return (int) (eqSquareSize(plot) * 0.5f) + 1;
}

float GraphicDisplayPanel::eqReadoutGap(juce::Rectangle<float> plot) const
{
    return (float) eqMarkHalf(plot) + 4.0f + 3.0f;   // ring outer edge + a little air
}

std::vector<GraphicDisplayPanel::EqStop> GraphicDisplayPanel::eqStops() const
{
    std::vector<EqStop> out;
    if (mode != EQ) return out;
    const auto plot = plotRect();
    if (plot.isEmpty()) return out;

    // In drum-edit mode with no key picked there is nothing to edit -- the same test
    // eqNodeAt's callers make, and the same judgement the AMP EG stops make: an edit
    // that reaches neither the device nor the model is not somewhere to put a cursor.
    if (eqInert()) return out;

    const int half = eqMarkHalf(plot);
    for (int n = 0; n < kEqNodes; ++n)
    {
        // A locked (plugin-only) handle is dropped exactly as eqNodeAt drops it, so the
        // keyboard cannot reach what the mouse cannot -- and the grey square says why.
        if (eqNodeLocked(n)) continue;
        const auto p = eqNodePos(n, plot);
        out.push_back({ n, n >= 2 && n <= 4,
                        juce::Rectangle<int>((int) p.x - half, (int) p.y - half,
                                             half * 2, half * 2) });
    }
    return out;
}

// "HPF2 node" / "EQ band 3 node", with the shelves named as the XG spec names them so
// a spoken readout matches both the manual and the Part module's own boxes.
juce::String GraphicDisplayPanel::eqA11yName(int node) const
{
    if (node == 0) return "HPF2 node";
    const char* const kBandName[5] = { "BASS", "2", "3", "4", "TREBLE" };
    return "EQ " + juce::String(kBandName[juce::jlimit(0, 4, node - 1)]) + " node";
}

// One keypress on one axis. GAIN and Q are plain raw scales and take a coarse/fine
// delta; FREQUENCY goes through the box's own wheel law, because a FREQ cell is Hz on
// one band and a table index on the next (see eqNudgeRaw in the header).
void GraphicDisplayPanel::eqKeyAdjust(const EqStop& st, int axis, int dir, bool fine)
{
    if (dir == 0 || eqInert() || eqNodeLocked(st.node)) return;
    const int band = eqBand(st.node);          // -1 = HPF2
    const int step = dir * (fine ? 1 : kEqStep);

    // The F handle is frequency and nothing else, so its GAIN axis is its frequency
    // too -- both arrow pairs move it. Its Q axis stays unwired (no adjustQ), so `[`
    // and `]` go on reaching the host while the cursor is on it.
    if (band < 0) axis = 1;

    // Remember it: this is the axis Enter will type into, on this node and on the next.
    // NOT recorded for the F handle, whose axis was forced above rather than chosen --
    // stepping F must not silently retarget the mid bands' Enter to frequency.
    if (band >= 0) eqTypeAxis = axis;

    if (axis == 1)
    {
        if (eqNudgeRaw) eqNudgeRaw(band, 1, dir, fine);
    }
    else
    {
        const int row = (axis == 2) ? 2 : 0;
        const int cur = eqReadRaw ? eqReadRaw(band, row) : -1;
        if (cur < 0) return;                   // no such cell (a shelf has no Q)
        const int lim = (row == 2) ? 1 : 0;    // Q is 1..120, GAIN 0..127
        const int nv  = juce::jlimit(lim, (row == 2) ? 120 : 127, cur + step);
        if (nv != cur && eqWriteRaw) eqWriteRaw(band, row, nv);
    }

    // Show the node's numbers, exactly as the wheel over the same handle does: the
    // handle moves but carries no text, so without this a keyboard edit is a square
    // sliding with nothing to read. Persists until the mouse says otherwise.
    eqHoverNode = st.node;
    hoverShown  = true;
    transientIdx = -1;
    if (timerJob != TimerNone) { stopTimer(); timerJob = TimerNone; }
    repaint();
}

std::vector<keynav::Item> GraphicDisplayPanel::navItems()
{
    // The screen buttons, in the row across the top. The envelope nodes are appended
    // rather than sorted in with them, which is already reading order: the button row
    // is above the plot.
    auto items = keynav::collect(*this);

    const auto stops = egStops();

    // Visibility is set ONCE per proxy, never hidden-then-shown: hiding a component
    // that HAS keyboard focus makes JUCE give that focus away to the panel's first
    // focusable child, and this runs on every focus change.
    for (int i = (int) stops.size(); i < 3; ++i)
        egProxy[i].setVisible(false);

    for (int i = 0; i < (int) stops.size() && i < 3; ++i)
    {
        const auto st = stops[(size_t) i];
        auto& px = egProxy[i];

        // Re-wired every pass: which parameter a proxy stands for changes with the
        // screen, and in drum-edit mode the AMP EG retargets to the DRUM SETUP rates,
        // which are named differently.
        px.setRole(juce::AccessibilityRole::slider);
        px.title = [this, st] { return juce::String(mu100::tooltips::forParam(st.table, st.param)); };
        px.value = [this, st] { return mu100a11y::withOverride(egText(st), egOverrideText(st)); };
        px.onActivate = nullptr;   // a handle has nothing to "press"
        if (px.getParentComponent() == nullptr) addChildComponent(px);
        px.setBounds(st.cell);
        px.setVisible(true);
        px.invalidateAccessibilityHandler();   // the name it answers with has changed

        keynav::Item it;
        it.comp      = this;
        it.painted   = true;
        it.cell      = st.cell;
        it.a11yProxy = &px;

        // Up/down: the LEVEL where the node has one, and otherwise the single value
        // the node carries -- an AMP EG node is one segment length and answers to
        // either pair of keys.
        const int vert = (st.level >= 0) ? st.level : st.time;
        it.adjust  = [this, st, vert](int dir, bool fine)
        {
            if (dir == 0) return;
            // The axis you last moved is the one Enter will type into.
            if (st.level >= 0) egTypeAxis = 0;
            egSet(vert, st.pitch, egRaw(vert, st.pitch) + dir * (fine ? 1 : kEgStep));
        };

        // `.` / `,`: the TIME. MORE and LESS, never "right" and "left" -- PITCH EG's
        // first node extends LEFTWARD from its anchor, so a longer attack moves it the
        // other way from every other node. Keying off the value keeps this the same law
        // as "up increases", and makes `.` read as "longer" on both nodes, which is
        // what it does to the envelope.
        it.adjustX = [this, st](int dir, bool fine)
        {
            if (dir == 0) return;
            if (st.level >= 0) egTypeAxis = 1;
            egSet(st.time, st.pitch, egRaw(st.time, st.pitch) + dir * (fine ? 1 : kEgStep));
        };

        // Backspace resets the whole node, exactly as the mouse's right-click does --
        // the same function, so the two cannot drift.
        it.reset    = [this, st] { egReset(st); };
        // Enter TYPES here too -- into the axis last moved (a one-value AMP EG point has
        // no choice to make). The proxy's invoke does the same, so a screen reader and
        // the key cannot disagree.
        it.activate = [this, st, &px] { beginEgTypeIn(st, &px); };
        px.onActivate = [this, st, &px] { beginEgTypeIn(st, &px); };
        items.push_back(std::move(it));
    }

    // ---- the bar screens' 33 columns ----
    // Same shape as the node stops above: a proxy per stop, re-wired every pass, set
    // visible ONCE (never hidden-then-shown while it might hold focus).
    const auto bars = barStops();

    for (int i = (int) bars.size(); i < kBarStops; ++i)
        barProxy[i].setVisible(false);

    for (int i = 0; i < (int) bars.size(); ++i)
    {
        const auto st = bars[(size_t) i];
        auto& px = barProxy[i];

        // Re-wired rather than tied to a column: which column proxy i stands for moves
        // by one whenever the S column drops out (PAN), and the parameter it names
        // changes with every screen.
        px.setRole(juce::AccessibilityRole::slider);
        px.title = [this, st] { return barA11yName(st.idx); };
        px.value = [this, st]
        {
            return mu100a11y::withOverride(barReadoutText(mode, barValue(mode, st.idx)),
                                           barOverrideText(st.idx));
        };
        // A screen reader's "invoke" does what Enter does -- opens the type-in -- so
        // the two cannot mean different things on the same cell.
        px.onActivate = [this, st] { beginBarTypeIn(st.idx); };
        if (px.getParentComponent() == nullptr) addChildComponent(px);
        px.setBounds(st.cell);
        px.setVisible(true);
        px.invalidateAccessibilityHandler();

        keynav::Item it;
        it.comp      = this;
        it.painted   = true;
        it.cell      = st.cell;
        it.a11yProxy = &px;

        it.adjust = [this, st](int dir, bool fine)
        {
            barNudge(st.idx, dir, fine);
            // Show the number. A bar carries no text of its own, so without this a
            // keyboard edit is a rectangle changing height with nothing to read. The
            // TRANSIENT form (not the hover form) because the mouse is elsewhere:
            // it hides itself, and a further press restarts it.
            startTransient(st.idx);
            repaint();
        };
        it.reset    = [this, st] { barReset(st.idx); repaint(); };
        // ENTER TYPES, here as everywhere else in the editor -- that rule is the whole
        // reason this is on Enter and the part-select moved off it (2026-09-05, the day
        // after these stops landed: they shipped with Enter selecting, which made the
        // bars the one place the manual's one-line rule was untrue).
        it.activate = [this, st] { beginBarTypeIn(st.idx); };
        // Ctrl+Enter: select the part this column belongs to, the same call the ruler's
        // numbers make. Only on a PART column -- the S column has no part, so it leaves
        // the key to the host rather than swallowing it silently.
        if (st.idx > 0)
            it.activateAlt = [this, st] { if (onSelectPart) onSelectPart(st.idx - 1); };
        items.push_back(std::move(it));
    }

    // ---- the EQ screen's six nodes, same treatment ----
    const auto eqs = eqStops();

    // Set ONCE per proxy, never hidden-then-shown: hiding a component that HAS the
    // keyboard focus makes JUCE hand that focus to the panel's first focusable child,
    // and this runs on every focus change. eqStops() is empty off the EQ screen, so
    // leaving it hides all six.
    for (int i = (int) eqs.size(); i < kEqNodes; ++i)
        eqProxy[i].setVisible(false);

    for (int i = 0; i < (int) eqs.size(); ++i)
    {
        const auto st = eqs[(size_t) i];
        auto& px = eqProxy[i];

        // Re-wired every pass: which NODE a given proxy stands for changes as locked
        // handles drop in and out of the list (the plugin-only lock is an option), so
        // the proxy at index i is not tied to node i.
        px.setRole(juce::AccessibilityRole::slider);
        px.title = [this, st] { return eqA11yName(st.node); };
        // The same string the plot prints beside the handle -- gain, frequency and Q in
        // one readout -- so a screen reader says what a sighted user reads.
        px.value = [this, st]
        {
            return mu100a11y::withOverride(eqReadout(st.node), eqOverrideText(st.node));
        };
        px.onActivate = [this, st, &px] { beginEqTypeIn(st, &px); };
        if (px.getParentComponent() == nullptr) addChildComponent(px);
        px.setBounds(st.cell);
        px.setVisible(true);
        px.invalidateAccessibilityHandler();   // the name it answers with has changed

        keynav::Item it;
        it.comp      = this;
        it.painted   = true;
        it.cell      = st.cell;
        it.a11yProxy = &px;

        it.activate = [this, st, &px] { beginEqTypeIn(st, &px); };
        it.adjust  = [this, st](int dir, bool fine) { eqKeyAdjust(st, 0, dir, fine); };
        it.adjustX = [this, st](int dir, bool fine) { eqKeyAdjust(st, 1, dir, fine); };
        // Left null on the shelves and on F, which have no Q at all -- that is what
        // keeps `[` and `]` the host's keys everywhere but the three mid bands.
        if (st.hasQ)
            it.adjustQ = [this, st](int dir, bool fine) { eqKeyAdjust(st, 2, dir, fine); };

        // Backspace resets the whole band -- gain, frequency and Q -- which is what the
        // mouse's right-click on the same handle does, through the same function, in one
        // undo step (PartPanel::resetEqBand opens the scope).
        it.reset    = [this, st] { if (eqResetBand) { eqResetBand(eqBand(st.node)); repaint(); } };
        items.push_back(std::move(it));
    }

    return items;
}

//==============================================================================
void GraphicDisplayPanel::resized()
{
    auto b = getLocalBounds();
    const int buttonH = (int) juce::jlimit(16.0f, 40.0f, (float) b.getHeight() * 0.12f);
    auto row = b.removeFromTop(buttonH);

    const int n = NumModes;
    const int gap = 2;
    const int cellW = (row.getWidth() - gap * (n - 1)) / n;
    int x = row.getX();
    for (int m = 0; m < n; ++m)
    {
        buttons[(size_t) m]->setBounds(x, row.getY(), cellW, buttonH);
        x += cellW + gap;
    }
}
