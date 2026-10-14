// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "PartPanel.h"
#include "EditorColours.h"
#include "EditorTextScale.h"
#include "FreqLadder.h"
#include "XgKnob.h"
#include "ValueBox.h"
#include "BracketLabel.h"
#include "DrumBuffer.h"   // drumbuf::name, for the EQ BYP tooltip
#include "PluginProcessor.h"
#include "AutomationParams.h"
#include "codec/MU100AddressMap.h"
#include "codec/MU100Tooltips.h"

namespace
{
    // En dash for the title's "Drum Key <dash> reason". Built from its code point,
    // never written as a source literal, so it cannot hinge on the compiler's source
    // charset — same reasoning as GraphicDisplayPanel's pm().
    juce::String dash() { return juce::String::charToString((juce::juce_wchar) 0x2013); }


    // Main 16-knob channel-strip row, in XGEdit order. `param` is the MULTI PART
    // parameter name (nullptr = not yet mapped). HPF lives in the separate per-part
    // "0A nn 20" block but is registered under the MULTI PART group, so it resolves
    // and reads back like the others.
    //
    // `signed64` marks a bipolar param stored 0..127 with 64 = centre; XGEdit
    // shows it as -64..0..+63 rather than the raw value (vibrato, filter).
    struct KnobSpec { const char* label; const char* param; int dflt; bool signed64; };

    const KnobSpec kMainRow[] = {
        { "VOL",    "VOLUME",                            100, false },
        { "DETUNE", "DETUNE",                            128, false },   // 8-bit, 128 = 0.0 Hz
        { "KSHIFT", "NOTE SHIFT",                         64, false },
        { "PAN",    "PAN",                                64, false },
        { "V.SEN",  "VELOCITY SENSE DEPTH",              64, false },
        { "V.OFF",  "VELOCITY SENSE OFFSET",             64, false },
        { "RATE",   "VIBRATO RATE",                      64, true  },
        { "DEPTH",  "VIBRATO DEPTH",                     64, true  },
        { "DELAY",  "VIBRATO DELAY",                     64, true  },
        { "HPF",    "HIGH PASS FILTER CUTOFF FREQUENCY", 64, true  },
        { "RES",    "LOW PASS FILTER RESONANCE",         64, true  },
        { "CUTOFF", "LOW PASS FILTER CUTOFF FREQUENCY",  64, true  },
        { "REV",    "REVERB SEND",                       40, false },
        { "CHORUS", "CHORUS SEND",                        0, false },
        { "VAR",    "VARIATION SEND",                     0, false },
        { "DRY",    "DRY LEVEL",                        127, false },
    };

    // Part-mode value-display Kind for main-row knob i (mirrors the ctor logic).
    PartPanel::Kind partKindOf(int i)
    {
        const juce::String lbl(kMainRow[i].label);
        return (lbl == "PAN")    ? PartPanel::Kind::Pan
             : (lbl == "KSHIFT") ? PartPanel::Kind::KShift
             : (lbl == "DETUNE") ? PartPanel::Kind::DetuneHz
             : kMainRow[i].signed64 ? PartPanel::Kind::Signed
                                    : PartPanel::Kind::Plain;
    }

    // Drum-edit mode rebinding of the main row, by the same index as kMainRow.
    // `drumParam` is the DRUM SETUP parameter (nullptr = no drum equivalent, so
    // the knob goes inert in drum mode). EG ATTACK/DECAY are not here — they are
    // edited from the Graphic Display's AMP EG screen instead.
    struct DrumKnobSpec { const char* label; const char* drumParam; int dflt; PartPanel::Kind kind; };
    const DrumKnobSpec kDrumRow[] = {
        { "VOL",    "LEVEL",                            127, PartPanel::Kind::Plain  },
        { "DETUNE", "PITCH FINE",                        64, PartPanel::Kind::Signed },
        { "KSHIFT", "PITCH COARSE",                      64, PartPanel::Kind::Signed },
        { "PAN",    "PAN",                               64, PartPanel::Kind::Pan    },
        { "K.ON",   "Rcv NOTE ON",                        1, PartPanel::Kind::OnOff  },
        { "K.OFF",  "Rcv NOTE OFF",                       1, PartPanel::Kind::OnOff  },
        { "RATE",   nullptr,                             64, PartPanel::Kind::Signed },   // inactive
        { "DEPTH",  nullptr,                             64, PartPanel::Kind::Signed },   // inactive
        { "DELAY",  nullptr,                             64, PartPanel::Kind::Signed },   // inactive
        { "HPF",    "HIGH PASS FILTER CUTOFF FREQUENCY", 64, PartPanel::Kind::Signed },
        { "RES",    "LOW PASS FILTER RESONANCE",         64, PartPanel::Kind::Signed },
        { "CUTOFF", "LOW PASS FILTER CUTOFF FREQUENCY",  64, PartPanel::Kind::Signed },
        { "REV",    "REVERB SEND",                       64, PartPanel::Kind::Plain  },
        { "CHORUS", "CHORUS SEND",                       64, PartPanel::Kind::Plain  },
        { "VAR",    "VARIATION SEND",                    64, PartPanel::Kind::Plain  },
        { "DRY",    nullptr,                            127, PartPanel::Kind::Plain  },   // inactive
    };

    // Main-row indices of the two connection-gated controls (see updateVarDryEnabled).
    constexpr int kVarKnob = 14;
    constexpr int kDryKnob = 15;

    // Receive-enable LED row (the module's bottom strip), left to right exactly as
    // XGEdit orders it — which is also MULTI PART address order, 08 nn 30..40. Every
    // one is a plain OFF/ON byte defaulting to ON.
    //
    // NONE of these grays out on a drum/SFX-kit part, deliberately. XGEdit grays PP,
    // SOF and BS, but the spec's drum-part exclusion list (sound_list.txt p46) names
    // PARAMETERS, and every one of them resolves to an entry that is NOT a receive
    // switch: "BANK SELECT LSB" is 08 nn 02, distinct from Rcv BANK SELECT (08 nn 40);
    // "PORTAMENTO" is PORTAMENTO SWITCH/TIME (08 nn 67/68), distinct from Rcv
    // PORTAMENTO (08 nn 3D). BS is the clearest case against XGEdit — bank MSB is what
    // selects the kit, so the switch is functional on a drum part. PP and SOF the spec
    // never mentions at all. See updateDrumKitGating for what IS grayed.
    struct RcvSpec { const char* label; const char* param; };
    const RcvSpec kRcvRow[] = {
        { "PB",  "Rcv PITCH BEND"             },
        { "CHP", "Rcv CH AFTER TOUCH (CAT)"   },
        { "PRO", "Rcv PROGRAM CHANGE"         },
        { "CC",  "Rcv CONTROL CHANGE"         },
        { "PP",  "Rcv POLY AFTER TOUCH (PAT)" },
        { "NO",  "Rcv NOTE MESSAGE"           },
        { "RPN", "Rcv RPN"                    },
        { "NRP", "Rcv NRPN"                   },
        { "MO",  "Rcv MODURATION"             },   // sic — the spec's spelling
        { "VOL", "Rcv VOLUME"                 },
        { "PAN", "Rcv PAN"                    },
        { "EXP", "Rcv EXPRESSION"             },
        { "SUS", "Rcv HOLD1"                  },
        { "POR", "Rcv PORTAMENTO"             },
        { "SOS", "Rcv SOSTENUTO"              },
        { "SOF", "Rcv SOFT PEDAL"             },
        { "BS",  "Rcv BANK SELECT"            },
    };
    constexpr int kNumRcvLeds = (int) (sizeof(kRcvRow) / sizeof(kRcvRow[0]));

    // LED row geometry. kRcvAreaH is the strip removed from the bottom of the
    // module; the separator line sits kRcvSepTop into it and the LEDs occupy the
    // bottom kRcvLedD pixels (kRcvLedD == the value-box height, so the circles are
    // the same size as the PORTA toggle's).
    constexpr int kRcvAreaH   = 25;
    constexpr int kRcvSepTop  = 5;
    constexpr int kRcvLedD    = 16;
    constexpr int kRcvLabelGap = 3;   // LED -> its label

    const char* const kEqParamNames[4] = {
        "EQ BASS GAIN", "EQ BASS FREQUENCY", "EQ TREBLE GAIN", "EQ TREBLE FREQUENCY"
    };
    const int kEqDfltVals[4] = { 64, 12, 64, 54 };   // BASS G/F, TREBLE G/F defaults

    // Per-band FREQ range + default as raw indices into the XG EQ-frequency table.
    // Band 1 low-shelf 4..40 and band 5 high-shelf 28..58 keep the hardware ranges;
    // the mid bands span the full 32 Hz..16 kHz (raw 4..58). Shared by buildEqGrid
    // and PartPanel::eqFreqRange / resetEqBand (and mirrored by the EQ screen's
    // x-scaling via eqFreqRange). Mid defaults 500/1.0k/4.0k.
    //
    // The mid bands' *boxes* hold Hz, not these indices (see eqFreqIsHz) — they are
    // plugin-only, so they aren't restricted to the values the firmware accepts and
    // run on the finer freqladder instead. The raw endpoints still define where their
    // Hz range and default come from (via eqTableHz), keeping one source of truth,
    // and the wheel still walks this table entry by entry.
    const int kEqBandFreqLo[5]   = {  4,  4,  4,  4, 28 };
    const int kEqBandFreqHi[5]   = { 40, 58, 58, 58, 58 };
    const int kEqBandFreqDflt[5] = { 12, 28, 34, 46, 54 };

    // Clearance the EQ grid leaves between one column's widest band label and the
    // next column's — what "spread the columns out a little" comes to in pixels.
    const int kBandLabelGap = 14;

    // Air a MEASURED label cell keeps around its text, on top of whatever inset the
    // cell itself applies. edtext scales fonts but NOT cells, so any cell holding a
    // label has to be measured from the real font or the text ellipsizes as the user
    // raises editorTextLabelsPct — see the measured widths in resized().
    const int kRowLabelGap  = 10;   // EQ row labels (GAIN/FREQ/Q/BAND) vs the boxes
    const int kCellLabelGap = 6;    // left-cluster cells (PORTA, SCALE, TUNE, enums)

    // The widest value string each EQ-grid row format can produce: GAIN "-12.0dB"
    // (raw 0), FREQ "16.0k" (top of both the XG table and the ladder), Q "12.0",
    // HPF2 "1.00k". The boxes are sized off these rather than a fixed width for the
    // same reason the band labels set the column pitch — edtext scales fonts but not
    // cells, so a fixed box squeezes its text as the user raises editorTextValuesPct.
    const char* const kEqWidestValues[] = { "-12.0dB", "16.0k", "12.0", "1.00k" };

    // Per-side air an EQ value box keeps around its (centred) value text: the width
    // of the changed-from-default tab that sits on the box's left inside edge, plus
    // the same again as clearance, so a 3-digit gain never reads as if it were
    // touching the tab.
    const int kEqValuePad = 2 * (int) std::ceil(edcol::kChangedTabW);

    // XG "EQ Frequency" conversion table (parameters table#3): raw data value ->
    // displayed centre frequency. Indexed by the raw 0..60 value; the Part-EQ
    // LFREQ knob uses raw 4..40 (32Hz..2.0k) and HFREQ uses raw 28..58
    // (500..16.0k). Kept here so the Hz display matches the hardware exactly.
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
        const int i = juce::jlimit(0, kEqFreqCount - 1, (int) std::lround(dv));
        return kEqFreqTable[i];
    }

    // Numeric twin of kEqFreqTable: raw index -> Hz, parsed from the display strings
    // so the two can't drift apart. The THRU sentinels (0 / 60) give 0.
    double eqTableHz(int raw)
    {
        const juce::String e = kEqFreqTable[juce::jlimit(0, kEqFreqCount - 1, raw)];
        const double v = e.upToFirstOccurrenceOf("k", false, true).getDoubleValue();
        return e.contains("k") ? v * 1000.0 : v;
    }

    // Parse a typed EQ-frequency back to the nearest raw value in [lo,hi].
    double eqFreqValue(const juce::String& t, int lo, int hi)
    {
        auto s = t.trim().toLowerCase();
        double hz = s.upToFirstOccurrenceOf("k", false, true)
                     .retainCharacters("0123456789.").getDoubleValue();
        if (s.contains("k")) hz *= 1000.0;
        if (hz <= 0.0) return (double) lo;

        int    best   = lo;
        double bestErr = 1e18;
        for (int i = juce::jmax(1, lo); i <= juce::jmin(kEqFreqCount - 2, hi); ++i)
        {
            const double err = std::abs(eqTableHz(i) - hz);
            if (err < bestErr) { bestErr = err; best = i; }
        }
        return (double) best;
    }

    //==========================================================================
    // Ladder-backed frequency controls (HPF2 + EQ mid bands). See FreqLadder.h.
    //==========================================================================

    // Wheel: one XG EQ-table entry per detent, exactly as before the mid bands moved
    // to Hz — step to the nearest table entry strictly above/below the current value.
    // Every in-range table entry is also a ladder value, so this never lands off-ladder.
    double eqTableStep(double hz, int dir, int loRaw, int hiRaw)
    {
        if (dir > 0)
        {
            for (int r = loRaw; r <= hiRaw; ++r)
                if (eqTableHz(r) > hz + 0.5) return eqTableHz(r);
            return eqTableHz(hiRaw);
        }
        for (int r = hiRaw; r >= loRaw; --r)
            if (eqTableHz(r) < hz - 0.5) return eqTableHz(r);
        return eqTableHz(loRaw);
    }
}

//==============================================================================
// Configure a knob's range + value display for `kind`. `paramIndexForRange` is
// used by the kinds whose range comes from the parameter table (EqFreq, and the
// byte-pair Plain case). Reusable so drum-edit mode can re-skin a knob in place.
void PartPanel::applyKind(XgKnob& kref, Kind kind, int paramIndex)
{
    using mu100::AddressMap;
    XgKnob* kp = &kref;
    auto& kobj = *kp;
    // Reset any formatter so a previous kind's display doesn't linger -- and the same
    // for the override bar's origin, or a knob re-skinned from a bipolar parameter to
    // a plain one (drum-edit mode swaps the whole main row) would keep growing its
    // mark out of the middle of a range that starts at nothing.
    kobj.setTextFromValue({});
    kobj.setValueFromText({});
    kobj.clearBarOrigin();

    auto k = kp;   // keep the original switch body (which uses `k->`) unchanged
    switch (kind)
    {
        case Kind::Pan:
            k->setRange(0.0, 127.0, 1.0);
            k->setBarOrigin(64.0);   // C: an override bar/arc grows out of centre
            k->setTextFromValue([](double dv) -> juce::String
            {
                const int v = (int) std::lround(dv);
                if (v <= 0)  return "RND";
                if (v < 64)  return "L" + juce::String(64 - v);
                if (v == 64) return "C";
                return "R" + juce::String(v - 64);
            });
            k->setValueFromText([](const juce::String& t) -> double
            {
                const auto s = t.trim().toUpperCase();
                if (s.startsWith("RND")) return 0.0;
                if (s == "C")            return 64.0;
                if (s.startsWith("L"))   return juce::jlimit(1, 63, 64 - s.substring(1).getIntValue());
                if (s.startsWith("R"))   return juce::jlimit(65, 127, 64 + s.substring(1).getIntValue());
                return (double) s.getIntValue();
            });
            break;

        case Kind::KShift:
            k->setRange(40.0, 88.0, 1.0);   // raw 0x28..0x58, 64 = 0
            k->setBarOrigin(64.0);
            k->setTextFromValue([](double dv) -> juce::String
            {
                const int s = (int) std::lround(dv) - 64;
                return (s > 0 ? "+" : "") + juce::String(s);
            });
            k->setValueFromText([](const juce::String& t) -> double
            {
                return juce::jlimit(40, 88, 64 + t.trim().getIntValue());
            });
            break;

        case Kind::GainDb:
            k->setRange(0.0, 127.0, 1.0);   // 0x00..0x7F, 64 = 0 dB, span ±12 dB
            k->setBarOrigin(64.0);          // 0 dB
            k->setTextFromValue([](double dv) -> juce::String
            {
                const double db = ((int) std::lround(dv) - 64) * (12.0 / 64.0);
                return juce::String(db, 1) + "dB";
            });
            k->setValueFromText([](const juce::String& t) -> double
            {
                const double db = t.upToFirstOccurrenceOf("d", false, true).getDoubleValue();
                return juce::jlimit(0.0, 127.0, 64.0 + db * (64.0 / 12.0));
            });
            break;

        case Kind::Signed:
            k->setRange(0.0, 127.0, 1.0);   // 0..127, 64 = centre, shown -64..+63
            k->setBarOrigin(64.0);
            k->setTextFromValue([](double dv) -> juce::String
            {
                const int s = (int) std::lround(dv) - 64;
                return (s > 0 ? "+" : "") + juce::String(s);
            });
            k->setValueFromText([](const juce::String& t) -> double
            {
                return juce::jlimit(0.0, 127.0, 64.0 + t.trim().getIntValue());
            });
            break;

        case Kind::DetuneHz:
            // 8-bit value 0..255 (nibble-packed into 2 bytes by the codec),
            // 128 = 0.0 Hz, 0.1 Hz/step, shown -12.8..+12.7 Hz like XGEdit.
            k->setRange(0.0, 255.0, 1.0);
            k->setBarOrigin(128.0);   // 0.0 Hz
            k->setTextFromValue([](double dv) -> juce::String
            {
                const double hz = (std::lround(dv) - 128) / 10.0;
                return (hz > 0.0 ? "+" : "") + juce::String(hz, 1);
            });
            k->setValueFromText([](const juce::String& t) -> double
            {
                const double hz = t.trim().retainCharacters("0123456789.+-").getDoubleValue();
                return juce::jlimit(0.0, 255.0, 128.0 + hz * 10.0);
            });
            break;

        case Kind::EqFreq:
        {
            // Range comes from the param (LFREQ 4..40, HFREQ 28..58); display Hz
            // via the XG EQ-frequency table.
            const int lo = (paramIndex >= 0) ? (int) AddressMap::param(paramIndex).rangeLo : 0;
            const int hi = (paramIndex >= 0) ? (int) AddressMap::param(paramIndex).rangeHi : 60;
            if (paramIndex >= 0) k->setRange((double) lo, (double) hi, 1.0);
            k->setTextFromValue([](double dv) { return eqFreqText(dv); });
            k->setValueFromText([lo, hi](const juce::String& t) { return eqFreqValue(t, lo, hi); });
            break;
        }

        case Kind::OnOff:
            k->setRange(0.0, 1.0, 1.0);     // drum Rcv NOTE ON/OFF
            k->setTextFromValue([](double dv) -> juce::String
            {
                return (int) std::lround(dv) != 0 ? "ON" : "OFF";
            });
            k->setValueFromText([](const juce::String& t) -> double
            {
                return t.trim().toUpperCase().startsWith("ON") ? 1.0 : 0.0;
            });
            break;

        case Kind::Plain:
        default:
        {
            // 2-byte (byte-pair) params (Detune) get the wide combined range.
            const bool isPair = (paramIndex >= 0 && AddressMap::param(paramIndex).size == 2);
            if (isPair) k->setRange(0.0, 16383.0, 1.0);
            else        k->setRange(0.0, 127.0, 1.0);
            break;
        }
    }
}

// A Part-module tooltip, plus the controller it records as in MIDI CC mode when it has
// one. Appended after " - " so the head stays the accessible NAME and the CC line
// becomes the spoken description (mu100a11y splits there). The number comes from the
// same table the output path uses, so the text cannot name a controller that is not
// sent.
static juce::String withCcNote(const juce::String& tip, const char* param)
{
    const int cc = YamahaMU100AudioProcessor::midiCcForMultiPartParam(param);
    if (cc < 0 || tip.isEmpty()) return tip;
    return tip + " - records as MIDI CC " + juce::String(cc) + " in MIDI CC mode";
}

std::unique_ptr<XgKnob> PartPanel::makeKnob(const char* label, const char* paramName,
                                            int dflt, Kind kind)
{
    using mu100::AddressMap;
    const int paramIndex = (paramName != nullptr) ? AddressMap::find(paramName, "MULTI PART") : -1;

    auto k = std::make_unique<XgKnob>();
    k->setLabel(label);
    if (paramName != nullptr)
        k->setTooltip(withCcNote(mu100::tooltips::forParam("MULTI PART", paramName), paramName));
    applyKind(*k, kind, paramIndex);

    k->setDefaultValue(dflt);
    k->setValueQuiet(dflt);

    if (paramIndex >= 0)
    {
        // Throttled: a knob drag fires this on every value change, far above what
        // 31250 baud carries. See sendParamChangeThrottled. (The enum boxes below
        // stay immediate — a handful of discrete options, no drag rate to speak of.)
        k->onChange = [this, paramIndex](double v)
        {
            const auto msg = mu100::AddressMap::makeParamChange(
                mu100::AddressMap::param(paramIndex), currentPart, (uint32_t) v);
            audioProcessor.sendParamChangeThrottled(msg);
        };
    }
    else
    {
        k->setEnabled(false);   // unmapped (HPF) — visible but inert
    }

    return k;
}

void PartPanel::addEnum(const char* label, const char* paramName,
                        const juce::StringArray& items, int dflt)
{
    using mu100::AddressMap;
    const int paramIndex = AddressMap::find(paramName, "MULTI PART");

    EnumControl ec;
    ec.label = std::make_unique<juce::Label>();
    ec.label->setText(label, juce::dontSendNotification);
    ec.label->setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
    ec.label->setJustificationType(juce::Justification::centred);
    ec.label->setColour(juce::Label::textColourId, edcol::kTextLabel);
    addAndMakeVisible(*ec.label);

    ec.box = std::make_unique<ValueBox>();
    ec.box->setTooltip(mu100::tooltips::forParam("MULTI PART", paramName));
    ec.box->setOptions(items);                      // drag/wheel through options, no arrow
    ec.box->setDefaultValue(dflt);
    ec.box->setValueQuiet(dflt);
    if (paramIndex >= 0)
    {
        ec.box->onChange = [this, paramIndex](double v)
        {
            const auto msg = mu100::AddressMap::makeParamChange(
                mu100::AddressMap::param(paramIndex), currentPart, (uint32_t) v);
            audioProcessor.sendParamChange(msg);
        };
    }
    addAndMakeVisible(*ec.box);

    enums.push_back(std::move(ec));
}

//==============================================================================
PartPanel::PartPanel(YamahaMU100AudioProcessor& proc) : audioProcessor(proc)
{
    titleLabel.setText("Part", juce::dontSendNotification);
    titleLabel.setFont(edtext::font(edtext::Title, 15.f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centredLeft);   // see resized()
    titleLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
    addAndMakeVisible(titleLabel);

    // Enum controls (defaults match the factory dump: poly, same-note=multi).
    addEnum("M/P",    "MONO/POLY MODE",   { "Mono", "Poly" },            1);
    addEnum("ASSIGN", "SAME NOTE NUMBER KEY ON ASSIGN", { "Single", "Multi", "Inst" }, 1);

    // Portamento: a dot toggle (switch) sitting next to its Time knob.
    eqBypLabel.setText("EQ BYP", juce::dontSendNotification);
    eqBypLabel.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
    eqBypLabel.setJustificationType(juce::Justification::centred);
    eqBypLabel.setColour(juce::Label::textColourId, edcol::kTextLabel);
    addAndMakeVisible(eqBypLabel);

    eqBypToggle.setFillCell(true);
    eqBypToggle.setAlert(true);       // lit = bypassed = not the normal state
    eqBypToggle.setTooltip(eqBypTooltip());   // state-dependent; refreshEqBypass re-composes it
    eqBypToggle.onChange = [this](bool on)
    {
        // Straight to the processor's live mask -- nothing is transmitted from here,
        // nothing enters the model, and nothing enters undo. The mask's own callback
        // brings the lamp, the EQ screen and the bottom-bar badge back into step, so
        // this deliberately does not touch any of them itself.
        audioProcessor.setPartEqBypass(currentPart, on);
    };
    addAndMakeVisible(eqBypToggle);

    portaLabel.setText("PORTA", juce::dontSendNotification);
    portaLabel.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
    portaLabel.setJustificationType(juce::Justification::centred);
    portaLabel.setColour(juce::Label::textColourId, edcol::kTextLabel);
    addAndMakeVisible(portaLabel);

    {
        const int portaSwParam = mu100::AddressMap::find("PORTAMENTO SWITCH", "MULTI PART");
        portaToggle.onChange = [this, portaSwParam](bool on)
        {
            if (portaSwParam < 0) return;
            const auto msg = mu100::AddressMap::makeParamChange(
                mu100::AddressMap::param(portaSwParam), currentPart, on ? 1u : 0u);
            audioProcessor.sendParamChange(msg);
        };
        portaToggle.setTooltip(withCcNote(mu100::tooltips::forParam("MULTI PART", "PORTAMENTO SWITCH"), "PORTAMENTO SWITCH"));
        portaToggle.setFillCell(true);   // larger circle (~value-box height), matches the SYNC toggle
        addAndMakeVisible(portaToggle);
    }

    ptimeKnob = makeKnob("PTIME", "PORTAMENTO TIME", 0, Kind::Plain);
    ptimeKnob->setShowDial(false);   // value box + label only (aligns with the enum cells)
    addAndMakeVisible(*ptimeKnob);

    // Main channel-strip row.
    for (const auto& s : kMainRow)
    {
        const juce::String lbl(s.label);
        const Kind kind = (lbl == "PAN")    ? Kind::Pan
                        : (lbl == "KSHIFT") ? Kind::KShift
                        : (lbl == "DETUNE") ? Kind::DetuneHz
                        : s.signed64        ? Kind::Signed
                                            : Kind::Plain;
        mainKnobs.push_back(makeKnob(s.label, s.param, s.dflt, kind));
        addAndMakeVisible(*mainKnobs.back());
    }

    // Resolve each main knob's part / drum AddressMap index and route its change
    // through dispatchMain so it can target MULTI PART (part mode) or DRUM SETUP
    // (drum-edit mode). This overrides the per-knob onChange set above.
    for (int i = 0; i < (int) mainKnobs.size(); ++i)
    {
        mainPartParam[i] = (kMainRow[i].param  != nullptr)
                         ? mu100::AddressMap::find(kMainRow[i].param,  "MULTI PART") : -1;
        mainDrumParam[i] = (kDrumRow[i].drumParam != nullptr)
                         ? mu100::AddressMap::find(kDrumRow[i].drumParam, "DRUM SETUP") : -1;
        mainKnobs[(size_t) i]->onChange = [this, i](double v) { dispatchMain(i, v); };
    }

    // EQ grid: 3 rows (GAIN/FREQ/Q) x 5 bands. Bands 1 & 5 are the hardware XG
    // BASS/TREBLE shelves; bands 2-4 are the plugin-only peaking EQ.
    buildEqGrid();

    // ASSIGN enum: SAME NOTE NUMBER (part) / KEY ASSIGN (drum), mode-aware.
    if (enums.size() > 1)
        enums[1].box->onChange = [this](double v)
        {
            if (drumEditActive) { sendDrum("KEY ASSIGN", v); return; }
            const int pp = mu100::AddressMap::find("SAME NOTE NUMBER KEY ON ASSIGN", "MULTI PART");
            if (pp >= 0)
                audioProcessor.sendParamChange(mu100::AddressMap::makeParamChange(
                    mu100::AddressMap::param(pp), currentPart, (uint32_t) v));
        };

    buildScaleControls();
    buildRcvLeds();
    buildDrumControls();
    updateModeTooltips();   // initial (part-mode) EQ + HPF2 + ASSIGN tooltips
    allowOverrideMarks();   // this module is the only one that draws override marks
}

//==============================================================================
// Grant every control in this module room for a horizontal MIDI-override bar (see
// MidiOverrideMarks.h). Opt-in, and this is the ONLY place that opts in: the Part
// module is where every override mark in the editor is drawn, so no other module
// gives up layout for a bar it can never show.
//
// It is granted to the WHOLE of each cluster, not only to the controls a message can
// actually reach. The EQ grid and the left cluster each read across as one row of
// value boxes, and a row where two cells sit four pixels lower than the rest is worse
// than a row that reserves four pixels it does not always use. The controls that can
// never be marked simply leave their strip empty.
//
// Called once: the flag says the room EXISTS, while midimarks::style() decides whether
// a strip is taken and a bar drawn, live, at layout and paint time.
void PartPanel::allowOverrideMarks()
{
    for (auto& k : mainKnobs) if (k) k->setOverrideMarkable(true);
    if (ptimeKnob) ptimeKnob->setOverrideMarkable(true);

    for (auto& e : enums) if (e.box) e.box->setOverrideMarkable(true);
    for (auto* b : { scaleNoteBox.get(), tuneBox.get(), grpBox.get(),
                     vptchBox.get(), vlpfBox.get(), hpf2Box.get() })
        if (b != nullptr) b->setOverrideMarkable(true);

    for (int r = 0; r < 3; ++r)
        for (int band = 0; band < 5; ++band)
            if (eqBox[r][band]) eqBox[r][band]->setOverrideMarkable(true);
}

// A mark-style change moves LAYOUT, not just paint: the horizontal bar lives in a
// strip the value boxes are built taller for. So the panel re-lays itself out rather
// than merely repainting -- and the EQ grid's row pitch, the left cluster and the
// dials' diameter all follow from the one number resized() reads.
void PartPanel::refreshMidiMarkStyle()
{
    // THE KNOBS HAVE TO BE TOLD, and this is the trap the first cut fell into: our
    // resized() hands each XgKnob the SAME rectangle it already had (the knob row's
    // geometry does not depend on the style), and juce::Component::setBounds with
    // unchanged bounds does not call resized(). So the knob kept the outgoing style's
    // split between its value box and its dial while PAINTING in the new one -- a
    // squashed value box over a full-size dial, or the reverse.
    //
    // It showed up on VAR and DRY alone, which is what made it look like a per-knob
    // bug: those two are the only ones something else calls setBare() on (the
    // variation-routing refresh), and setBare ends in resized().
    for (auto& k : mainKnobs) if (k) k->refreshMidiMarkStyle();
    if (ptimeKnob) ptimeKnob->refreshMidiMarkStyle();

    resized();
    repaint();
}

//==============================================================================
// Receive-enable LED row (see kRcvRow). Each LED is the same DotToggle the PORTA
// switch uses, at the same size, with its abbreviation to the right.
//
// The switches are discrete on/off, so they send IMMEDIATELY (like the M/P and
// ASSIGN enums) rather than through sendParamChangeThrottled — there is no drag
// rate to coalesce, and NO (Rcv Note) doubles as a mute, which wants to take
// effect on the click.
void PartPanel::buildRcvLeds()
{
    rcvLeds.reserve((size_t) kNumRcvLeds);
    for (const auto& s : kRcvRow)
    {
        RcvLed led;
        led.param = mu100::AddressMap::find(s.param, "MULTI PART");

        led.dot = std::make_unique<DotToggle>();
        led.dot->setFillCell(true);          // circle tracks the cell — matches PORTA
        led.dot->setState(true, false);      // every Rcv switch defaults to ON
        led.dot->setTooltip(mu100::tooltips::forParam("MULTI PART", s.param));
        if (led.param >= 0)
        {
            const int p = led.param;
            led.dot->onChange = [this, p](bool on)
            {
                audioProcessor.sendParamChange(mu100::AddressMap::makeParamChange(
                    mu100::AddressMap::param(p), currentPart, on ? 1u : 0u));
            };
        }
        addAndMakeVisible(*led.dot);

        led.label = std::make_unique<juce::Label>();
        led.label->setText(s.label, juce::dontSendNotification);
        led.label->setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
        led.label->setJustificationType(juce::Justification::centredLeft);
        // No border: the row measures each label to its exact text width, and
        // juce::Label's default 5px-a-side inset would eat that and ellipsize.
        led.label->setBorderSize(juce::BorderSize<int>(0));
        led.label->setColour(juce::Label::textColourId, edcol::kTextLabel);
        addAndMakeVisible(*led.label);

        rcvLeds.push_back(std::move(led));
    }
}

// Lay the 17 [LED][gap][label] groups across the module's bottom strip, evenly
// spread, with the separator line along the top of the strip.
//
// Label widths are MEASURED from the real font, like the EQ columns and the
// Keyboard module's lamp row: edtext scales fonts but not cells, so a hardcoded
// group width would overlap its neighbour as editorTextLabelsPct rises.
void PartPanel::layOutRcvLeds(juce::Rectangle<int> area)
{
    rcvSepLine = { area.getX(), area.getY() + kRcvSepTop, area.getWidth(), 1 };

    auto row = area.withTrimmedTop(area.getHeight() - kRcvLedD);   // bottom kRcvLedD px
    const int n = (int) rcvLeds.size();
    if (n == 0) return;

    const juce::Font labFont (edtext::font(edtext::Label, 11.f, juce::Font::bold));

    std::vector<int> groupW((size_t) n);
    int sumW = 0;
    for (int i = 0; i < n; ++i)
    {
        // +2: getStringWidthInt rounds down, and a fitted juce::Label ellipsizes the
        // moment its box is a sub-pixel short of the glyph run.
        groupW[(size_t) i] = kRcvLedD + kRcvLabelGap + 2
            + juce::GlyphArrangement::getStringWidthInt(labFont, rcvLeds[(size_t) i].label->getText());
        sumW += groupW[(size_t) i];
    }

    // Spread the leftover between the groups; the odd remainder pixel goes to the
    // leftmost gaps so the last group still ends exactly on the strip's right edge.
    const int gaps    = n - 1;
    const int slack   = row.getWidth() - sumW;
    const int spacing = juce::jmax(2, gaps > 0 ? slack / gaps : 0);
    const int extra   = juce::jmax(0, slack - spacing * gaps);

    int x = row.getX();
    for (int i = 0; i < n; ++i)
    {
        auto& led = rcvLeds[(size_t) i];
        led.dot->setBounds(x, row.getY(), kRcvLedD, kRcvLedD);
        led.label->setBounds(x + kRcvLedD + kRcvLabelGap, row.getY(),
                             groupW[(size_t) i] - kRcvLedD - kRcvLabelGap, kRcvLedD);
        x += groupW[(size_t) i] + spacing + (i < extra ? 1 : 0);
    }
}

void PartPanel::setRcvRowVisible(bool shouldShow)
{
    if (shouldShow == rcvRowVisible) return;
    rcvRowVisible = shouldShow;
    for (auto& led : rcvLeds)
    {
        led.dot->setVisible(shouldShow);
        led.label->setVisible(shouldShow);
    }
    repaint();   // the separator line is painted, not a component
}

void PartPanel::setDrumKitPart(bool isKit)
{
    if (isKit == drumKitPart) return;
    drumKitPart = isKit;
    updateDrumKitGating();
}

// Gray the controls the DEVICE ignores on a DRUM/SFX-kit part. The set comes from
// the spec, not from XGEdit: sound_list.txt p46 (the MULTI PART table's own note)
// — "In the case of a DRUM PART, the following parameters will have no effect" —
// BANK SELECT LSB, MONO/POLY MODE, SCALE TUNING, PORTAMENTO, PITCH EG, FILTER
// MODURATION DEPTH, AMPLITUDE MODURATION DEPTH. Of those, this panel owns three:
//
//   MONO/POLY MODE  -> the M/P enum
//   SCALE TUNING    -> the SCALE note selector + TUNE cents box
//   PORTAMENTO      -> the PORTA switch + PTIME knob
//
// PITCH EG is the Graphic Display's screen (setDrumKitPart there), FMOD/AMOD DEPTH
// are the Controller module's two depth knobs, and BANK SELECT LSB has no editor
// control of its own (the voice browser writes it). The receive-LED row is
// deliberately untouched — see kRcvRow.
//
// The VALUES are left alone. These are MULTI PART bytes — part state, not voice
// state — so a program change does not clear them on the device either; the grayed
// control is an honest mirror of what the part still holds, and assigning a melodic
// voice back revives it. Same reasoning as the PITCH EG screen.
// A DRUMS buffer was assigned to (or taken from) the current part. That decides
// whether the part-level EQ row is live at all -- see updateEditGating.
void PartPanel::setPartDrumBuffer(int buffer)
{
    const int b = (buffer >= 0 && buffer < 4) ? buffer : -1;
    if (b == partBuffer) return;
    partBuffer = b;
    updateEditGating();
    // A buffer change can flip an inherited bypass to none (or to a different kit's)
    // without moving a single bit in the mask, so the lamp's tooltip is re-composed
    // here rather than only from the processor's change callback.
    refreshEqBypass();    // also repaints: the "EQ" bracket says which state the row is in
}

// The three title-naming setters. Each early-outs on no change and then re-runs
// updateEditGating, which is the single place the title string is composed — so the
// name and the mode word can never be assembled by two different rules.
//
// updateEditGating does a good deal more than the title, but it is cheap (no
// allocation beyond the string, no layout) and it runs on part selection already.
// Calling it is what keeps the title correct for free when the MODE changes under a
// name that was pushed earlier.
void PartPanel::setTitleNameMode(int mask)
{
    const int m = mask & (TitleNamePart | TitleNameDrum);
    if (m == titleNameMode) return;
    titleNameMode = m;
    updateEditGating();
}

void PartPanel::setPartVoiceName(const juce::String& name)
{
    if (name == partVoiceName) return;
    partVoiceName = name;
    updateEditGating();
}

void PartPanel::setDrumToneName(const juce::String& name)
{
    if (name == drumToneName) return;
    drumToneName = name;
    updateEditGating();
}

void PartPanel::updateDrumKitGating()
{
    const bool live = ! drumKitPart;
    const juce::Colour labCol = live ? edcol::kTextLabel : edcol::kValueOff;

    auto gateBox = [live](ValueBox* box, juce::Label* lab, const juce::Colour& c)
    {
        if (box != nullptr)
        {
            box->setEnabled(live);
            box->setInterceptsMouseClicks(live, live);
        }
        if (lab != nullptr) lab->setColour(juce::Label::textColourId, c);
    };

    if (enums.size() > 0) gateBox(enums[0].box.get(), enums[0].label.get(), labCol);  // M/P
    gateBox(scaleNoteBox.get(), &scaleLabel, labCol);                                 // SCALE
    gateBox(tuneBox.get(),      &tuneLabel,  labCol);                                 // TUNE

    portaToggle.setDimmed(! live);                                                    // PORTA
    portaLabel.setColour(juce::Label::textColourId, labCol);
    if (ptimeKnob) { ptimeKnob->setInteractive(live); ptimeKnob->setLabelDimmed(! live); }

    // PORTA / PTIME's MIDI CC marks follow `live`, and nothing else re-decides them
    // when a kit voice arrives or leaves.
    updateAutomationMarkers();
}

// Gray the WHOLE receive-LED row while drum-key-edit mode is on. In that mode every
// other control on the panel edits the selected drum key, so a live part-level
// switch would read as if it were part of the key's data. Graying rather than hiding
// keeps its state visible. Note the one LED with a per-key counterpart, NO (Rcv Note
// Message), already has it on screen in this mode: DRUM SETUP's Rcv NOTE ON / OFF are
// the K.ON / K.OFF knobs, and the two nest — NO gates the whole part regardless.
void PartPanel::updateRcvRowGating()
{
    const juce::Colour labCol = drumEditActive ? edcol::kValueOff : edcol::kTextLabel;
    for (auto& led : rcvLeds)
    {
        led.dot->setDimmed(drumEditActive);
        led.label->setColour(juce::Label::textColourId, labCol);
    }
}

//==============================================================================
// SCALE micro-tuning: one note selector (C..B) plus a TUNE box giving that
// note's detune in cents (-64..+63). The 12 SCALE TUNING bytes (0x41-0x4C) are
// edited one at a time; TUNE shows the selected note's value. Until device
// read-back exists, the selector recalls our last-sent value per note.
void PartPanel::buildScaleControls()
{
    static const char* const kNoteNames[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* const kScaleParamNames[12] = {
        "SCALE TUNING C",  "SCALE TUNING C#", "SCALE TUNING D",  "SCALE TUNING D#",
        "SCALE TUNING E",  "SCALE TUNING F",  "SCALE TUNING F#", "SCALE TUNING G",
        "SCALE TUNING G#", "SCALE TUNING A",  "SCALE TUNING A#", "SCALE TUNING B"
    };
    for (int i = 0; i < 12; ++i)
    {
        scaleParam[i] = mu100::AddressMap::find(kScaleParamNames[i], "MULTI PART");
        scaleCache[i] = 64;   // 0x40 = 0 cents (factory default)
    }

    auto initLabel = [this](juce::Label& l, const char* txt)
    {
        l.setText(txt, juce::dontSendNotification);
        l.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
        l.setJustificationType(juce::Justification::centred);
        l.setColour(juce::Label::textColourId, edcol::kTextLabel);
        addAndMakeVisible(l);
    };
    initLabel(scaleLabel, "SCALE");
    initLabel(tuneLabel,  "TUNE");

    // Note selector — a local pick of which note TUNE edits (no SysEx of its own).
    scaleNoteBox = std::make_unique<ValueBox>();
    scaleNoteBox->setOptions(juce::StringArray(kNoteNames, 12));
    scaleNoteBox->setDefaultValue(0);
    scaleNoteBox->setValueQuiet(0);
    scaleNoteBox->setTooltip(mu100::tooltips::forParam("MULTI PART", "SCALE"));
    scaleNoteBox->onChange = [this](double v)
    {
        const int note = juce::jlimit(0, 11, (int) v);
        tuneBox->setValueQuiet(scaleCache[note]);   // recall that note's value
        // TUNE now shows a DIFFERENT one of the twelve SCALE TUNING bytes, so its
        // override mark has to move with it -- picking a note moves no override, so
        // nothing else would.
        refreshMidiOverrides();
    };
    addAndMakeVisible(*scaleNoteBox);

    // TUNE — cents for the selected note (raw 0..127, 64 = 0 cents).
    tuneBox = std::make_unique<ValueBox>();
    tuneBox->setRange(0.0, 127.0, 1.0);
    tuneBox->setBarOrigin(64.0);        // 0 cents
    tuneBox->setDefaultValue(64);
    tuneBox->setValueQuiet(64);
    tuneBox->setTooltip(mu100::tooltips::forParam("MULTI PART", "TUNE"));
    tuneBox->textFromValue = [](double dv) -> juce::String
    {
        const int c = (int) std::lround(dv) - 64;
        return (c > 0 ? "+" : "") + juce::String(c);
    };
    tuneBox->valueFromText = [](const juce::String& t) -> double
    {
        return juce::jlimit(0.0, 127.0, 64.0 + t.trim().getIntValue());
    };
    tuneBox->onChange = [this](double v)
    {
        const int note = juce::jlimit(0, 11, (int) scaleNoteBox->getValue());
        scaleCache[note] = (int) v;
        if (scaleParam[note] >= 0)
            audioProcessor.sendParamChangeThrottled(mu100::AddressMap::makeParamChange(
                mu100::AddressMap::param(scaleParam[note]), currentPart, (uint32_t) v));
    };
    addAndMakeVisible(*tuneBox);

    // HPF2 — plugin-only clean per-part high-pass (absolute Hz, 0 = Off). Value box
    // only (no knob); drives proc.setCleanHpfHz, NOT a SysEx param, so it never
    // enters a dump. Laid out as the EQ grid's first column (FREQ row) in resized();
    // its "F / HPF2" label is part of the band-label row.
    hpf2Box = std::make_unique<ValueBox>();
    hpf2Box->setTooltip(mu100::tooltips::forKey("PLUGIN|HPF2"));   // plugin-only; author in tooltips.csv
    hpf2Box->setRange(0.0, 1000.0, 1.0);
    // Any integer 20..1000 (the ladder's fine segment), reachable by drag or type-in.
    // Plain drag = log sweep of the whole range; Shift-drag = 1 Hz; wheel = 10 Hz
    // (Shift 1 Hz) as before.
    hpf2Box->snapValue  = [](double v)                       { return freqladder::hpf2::snap(v); };
    hpf2Box->dragValue  = [](double s, double dy, bool fine) { return freqladder::hpf2::drag(s, dy, fine); };
    hpf2Box->wheelValue = [](double v, int dir, bool fine)   { return freqladder::hpf2::wheel(v, dir, fine); };
    hpf2Box->setDefaultValue(0);
    hpf2Box->setValueQuiet(0);
    hpf2Box->textFromValue = [](double dv) -> juce::String
    {
        return dv < (double) freqladder::kMin ? juce::String("Off") : freqladder::text(dv);
    };
    hpf2Box->valueFromText = [](const juce::String& t) -> double
    {
        const juce::String s = t.trim();
        if (s.isEmpty() || s.equalsIgnoreCase("off")) return 0.0;
        return juce::jlimit(0.0, 1000.0, freqladder::parse(s));
    };
    hpf2Box->onChange = [this](double v)
    {
        if (drumEditActive)
        {
            // Drum mode: HPF2 edits the current drum key (buffer+key), not the part.
            if (drumKey >= 0) audioProcessor.setDrumHpf(drumSetup, drumKey, (float) v);
        }
        else
        {
            audioProcessor.setCleanHpfHz(currentPart, (float) v);
            if (onHpf2Changed) onHpf2Changed(currentPart, (int) v);   // live-sync the graphic bar
        }
        notifyEqChanged();   // repaint the EQ screen's F node (part or drum mode)
    };
    addAndMakeVisible(*hpf2Box);
}

void PartPanel::recallHpf2()
{
    if (! hpf2Box) return;
    // Drum mode shows the current drum key's (buffer+key) HPF2; normal mode the part's.
    const float hz = drumEditActive ? audioProcessor.getDrumHpf(drumSetup, drumKey)
                                    : audioProcessor.getCleanHpfHz(currentPart);
    hpf2Box->setValueQuiet((double) hz);
}

//==============================================================================
// Build the 3-row (GAIN/FREQ/Q) x 5-band EQ grid of value boxes. Bands 1 & 5 (cols
// 0 & 4) are the hardware XG BASS/TREBLE shelves (mode-aware SysEx via dispatchEq);
// bands 2-4 (cols 1-3) are the plugin-only peaking EQ (proc.setPartEq*, mirroring
// the Multi EQ mid bands' ranges). Shelves have no Q. Row labels sit left of the
// grid and a two-line band label under each column; HPF2 is laid out as the grid's
// first column (see resized()). The "EQ" bracket is drawn in paint().
void PartPanel::buildEqGrid()
{
    using mu100::AddressMap;

    // Hardware shelf param indices, order [0]=BASS GAIN, [1]=BASS FREQ, [2]=TREBLE
    // GAIN, [3]=TREBLE FREQ — resolved for both part (MULTI PART) and drum modes.
    for (int i = 0; i < 4; ++i)
    {
        eqPartParam[i] = AddressMap::find(kEqParamNames[i], "MULTI PART");
        eqDrumParam[i] = AddressMap::find(kEqParamNames[i], "DRUM SETUP");
    }

    // Value-display helpers. ALL five gain boxes share the shelf format/range/step
    // (raw 0..127, 64 = 0 dB, 0.1875 dB/step, one decimal) — the mid bands match the
    // hardware bands 1 & 5 exactly (per user), not the Multi EQ's 1 dB steps.
    auto gainText = [](double dv) -> juce::String
    { const double db = ((int) std::lround(dv) - 64) * (12.0 / 64.0); return juce::String(db, 1) + "dB"; };
    auto gainVal  = [](const juce::String& t) -> double
    { const double db = t.upToFirstOccurrenceOf("d", false, true).getDoubleValue();
      return juce::jlimit(0.0, 127.0, 64.0 + db * (64.0 / 12.0)); };
    auto qText = [](double dv) -> juce::String { return juce::String(std::lround(dv) / 10.0, 1); };
    auto qVal  = [](const juce::String& t) -> double
    { return juce::jlimit(1.0, 120.0, std::round(t.getDoubleValue() * 10.0)); };

    auto makeBox = [this](double lo, double hi, int dflt) -> std::unique_ptr<ValueBox>
    {
        auto box = std::make_unique<ValueBox>();
        box->setRange(lo, hi, 1.0);
        box->setDefaultValue(dflt);
        box->setValueQuiet(dflt);
        addAndMakeVisible(*box);
        return box;
    };

    // Per-band FREQ range + default now live at file scope (kEqBandFreq*) so the
    // EQ screen and eqFreqRange/resetEqBand share them. All use the same
    // 1-table-entry drag/wheel step, so the mid bands' broadened segments step
    // exactly like bands 1 & 5.
    const int* const kFreqLo   = kEqBandFreqLo;
    const int* const kFreqHi   = kEqBandFreqHi;
    const int* const kFreqDflt = kEqBandFreqDflt;

    for (int band = 0; band < 5; ++band)
    {
        const bool hw    = (band == 0 || band == 4);
        const int  pband = band - 1;                    // plugin band index for cols 1-3

        // --- GAIN (all bands: raw 0..127, shelf format/range/step) ---
        {
            auto box = makeBox(0.0, 127.0, 64);
            box->textFromValue = gainText;
            box->valueFromText = gainVal;
            box->setBarOrigin(64.0);   // 0 dB is this cell's rest point, not raw 0
            if (hw)
            {
                const int hwIx = (band == 0) ? 0 : 2;   // BASS GAIN / TREBLE GAIN
                box->setTooltip(mu100::tooltips::forParam(
                    "MULTI PART", band == 0 ? "EQ BASS GAIN" : "EQ TREBLE GAIN"));
                box->onChange = [this, hwIx](double v) { dispatchEq(hwIx, v); };
            }
            else
                box->onChange = [this, pband](double v)
                {
                    if (drumEditActive) { if (drumKey >= 0) audioProcessor.setDrumEqGain(drumSetup, drumKey, pband, (int) v); }
                    else                audioProcessor.setPartEqGain(currentPart, pband, (int) v);
                    notifyEqChanged();
                };
            eqBox[EqGain][band] = std::move(box);
        }

        // --- FREQ ---
        // Shelves: the raw XG table index the firmware wants. Mid bands: plugin-only,
        // so absolute Hz on the finer freqladder instead (same 32 Hz..16 kHz span).
        {
            const int lo = kFreqLo[band], hi = kFreqHi[band];
            std::unique_ptr<ValueBox> box;
            if (hw)
            {
                box = makeBox((double) lo, (double) hi, kFreqDflt[band]);
                box->textFromValue = [](double dv) { return eqFreqText(dv); };
                box->valueFromText = [lo, hi](const juce::String& t) { return eqFreqValue(t, lo, hi); };
                const int hwIx = (band == 0) ? 1 : 3;   // BASS FREQ / TREBLE FREQ
                box->setTooltip(mu100::tooltips::forParam(
                    "MULTI PART", band == 0 ? "EQ BASS FREQUENCY" : "EQ TREBLE FREQUENCY"));
                box->onChange = [this, hwIx](double v) { dispatchEq(hwIx, v); };
            }
            else
            {
                box = makeBox(eqTableHz(lo), eqTableHz(hi), (int) eqTableHz(kFreqDflt[band]));
                box->snapValue  = [](double v)                       { return (double) freqladder::snap(v); };
                box->dragValue  = [](double s, double dy, bool fine) { return freqladder::drag(s, dy, fine); };
                box->wheelValue = [lo, hi](double v, int dir, bool)  { return eqTableStep(v, dir, lo, hi); };
                box->textFromValue = [](double dv) { return freqladder::text(dv); };
                box->valueFromText = [](const juce::String& t) { return freqladder::parse(t); };
                box->onChange = [this, pband](double v)
                {
                    if (drumEditActive) { if (drumKey >= 0) audioProcessor.setDrumEqFreq(drumSetup, drumKey, pband, (int) v); }
                    else                audioProcessor.setPartEqFreq(currentPart, pband, (int) v);
                    notifyEqChanged();
                };
            }
            eqBox[EqFreq][band] = std::move(box);
        }

        // --- Q (mid bands only) ---
        if (! hw)
        {
            auto box = makeBox(1.0, 120.0, 7);
            box->textFromValue = qText;
            box->valueFromText = qVal;
            box->onChange = [this, pband](double v)
            {
                if (drumEditActive) { if (drumKey >= 0) audioProcessor.setDrumEqQ(drumSetup, drumKey, pband, (int) v); }
                else                audioProcessor.setPartEqQ(currentPart, pband, (int) v);
                notifyEqChanged();
            };
            eqBox[EqQ][band] = std::move(box);
        }
    }

    // Row labels (left of the rows) + column labels (below), styled like the panel's
    // other small labels.
    auto initLabel = [this](juce::Label& l, const char* t, juce::Justification j)
    {
        l.setText(t, juce::dontSendNotification);
        l.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
        l.setJustificationType(j);
        l.setColour(juce::Label::textColourId, edcol::kTextLabel);
        addAndMakeVisible(l);
    };
    static const char* const kRowNames[4] = { "GAIN", "FREQ", "Q", "BAND" };
    for (int r = 0; r < 4; ++r) initLabel(eqRowLabel[r], kRowNames[r], juce::Justification::centredRight);

    // Band labels, two lines per column: position on top, what the band IS beneath.
    // Column 0 is HPF2 — the grid's first column, FREQ row only.
    static const char* const kColNames[6][2] = {
        { "F",    "HPF2"  },
        { "1 LO", "SHELF" },
        { "2",    "PEAK"  },
        { "3",    "PEAK"  },
        { "4",    "PEAK"  },
        { "5 HI", "SHELF" },
    };
    for (int c = 0; c < 6; ++c)
        for (int line = 0; line < 2; ++line)
            initLabel(eqColLabel[c][line], kColNames[c][line], juce::Justification::centred);
}

// Load the plugin mid-bands (2-4 = plugin bands 0-2) from processor state for the
// current part. Quiet (no send-back). HPF2/shelves are handled separately.
void PartPanel::recallPartEq()
{
    // Drum mode shows the current drum key's (buffer+key) bands; normal mode the part's.
    const bool d = drumEditActive;
    for (int band = 1; band <= 3; ++band)
    {
        const int pband = band - 1;
        const int g = d ? audioProcessor.getDrumEqGain(drumSetup, drumKey, pband) : audioProcessor.getPartEqGain(currentPart, pband);
        const int f = d ? audioProcessor.getDrumEqFreq(drumSetup, drumKey, pband) : audioProcessor.getPartEqFreq(currentPart, pband);
        const int q = d ? audioProcessor.getDrumEqQ   (drumSetup, drumKey, pband) : audioProcessor.getPartEqQ   (currentPart, pband);
        if (eqBox[EqGain][band]) eqBox[EqGain][band]->setValueQuiet((double) g);
        if (eqBox[EqFreq][band]) eqBox[EqFreq][band]->setValueQuiet((double) f);
        if (eqBox[EqQ][band])    eqBox[EqQ][band]->setValueQuiet   ((double) q);
    }
}

// Drum-only mid-row controls (ALTERNATE GROUP + the two velocity-sense params),
// shown only in drum-edit mode where they overlay the Porta-Time / SCALE / TUNE
// cells. Created hidden (addChildComponent).
void PartPanel::buildDrumControls()
{
    auto initLabel = [this](juce::Label& l, const char* t)
    {
        l.setText(t, juce::dontSendNotification);
        l.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
        l.setJustificationType(juce::Justification::centred);
        l.setColour(juce::Label::textColourId, edcol::kTextLabel);
        addChildComponent(l);
    };

    grpBox = std::make_unique<ValueBox>();
    grpBox->setRange(0.0, 127.0, 1.0);
    grpBox->setDefaultValue(0);
    grpBox->setValueQuiet(0);
    grpBox->setEnabled(false);
    grpBox->setInterceptsMouseClicks(false, false);
    grpBox->textFromValue = [](double dv) -> juce::String
    { const int g = (int) std::lround(dv); return g == 0 ? juce::String("OFF") : juce::String(g); };
    grpBox->valueFromText = [](const juce::String& t) -> double
    { const auto s = t.trim(); if (s.toUpperCase().startsWith("OFF")) return 0.0;
      return (double) juce::jlimit(0, 127, s.getIntValue()); };
    grpBox->onChange = [this](double v) { sendDrum("ALTERNATE GROUP", v); };
    grpBox->setTooltip(mu100::tooltips::forParam("DRUM SETUP", "ALTERNATE GROUP"));
    addChildComponent(*grpBox);
    initLabel(grpLabel, "GRP");

    auto makeVelSense = [this](std::unique_ptr<ValueBox>& box, const char* drumParam)
    {
        box = std::make_unique<ValueBox>();
        box->setRange(48.0, 80.0, 1.0);   // raw 0x30..0x50, 64 = 0, shown -16..+16
        box->setBarOrigin(64.0);
        box->setDefaultValue(64);
        box->setValueQuiet(64);
        box->setEnabled(false);
        box->setInterceptsMouseClicks(false, false);
        box->textFromValue = [](double dv) -> juce::String
        { const int s = (int) std::lround(dv) - 64; return (s > 0 ? "+" : "") + juce::String(s); };
        box->valueFromText = [](const juce::String& t) -> double
        { return (double) juce::jlimit(48, 80, 64 + t.trim().getIntValue()); };
        const juce::String pname(drumParam);
        box->onChange = [this, pname](double v) { sendDrum(pname.toRawUTF8(), v); };
        box->setTooltip(mu100::tooltips::forParam("DRUM SETUP", drumParam));
        addChildComponent(*box);
    };
    makeVelSense(vptchBox, "VELOCITY SENSE PITCH");
    makeVelSense(vlpfBox,  "VELOCITY SENSE LPF CUTOFF");
    initLabel(vptchLabel, "VPTCH");
    initLabel(vlpfLabel,  "VLPF");
}

void PartPanel::setDrumEdit(bool active, int note, int setup, bool noDrumOnKey)
{
    drumSetup = juce::jlimit(0, 3, setup);
    const bool modeChanged = (active != drumEditActive);
    // The title distinguishes the two note<0 cases, so a change of REASON has to
    // refresh it even though the key itself stayed at -1 (clicking from one
    // drumless key to another, or off a real key onto one).
    const bool keyChanged  = (note != drumKey) || (noDrumOnKey != drumNoDrumOnKey);
    drumKey = note;
    drumNoDrumOnKey = noDrumOnKey;
    if (modeChanged)
    {
        drumEditActive = active;
        applyDrumMode();                 // re-skins controls + seeds defaults (calls updateEditGating)
        repaint();                       // the "EQ" bracket changes with the mode
    }
    else if (keyChanged && drumEditActive)
        updateEditGating();              // first key selected / key cleared -> ungray / gray
    // Show the selected drum key's real values from the device-state model (a
    // bulk-dump read-back populates it). Runs on mode entry with a key, and on
    // every key change; absent params fall back to the drum defaults.
    if (drumEditActive && drumKey >= 0 && (modeChanged || keyChanged))
        loadDrumKey();

    // The override marks are per (key, buffer) in this mode, so they are stale the
    // moment either moves -- and a drum-key click reaches no other refresh path.
    if (modeChanged || keyChanged) refreshMidiOverrides();
}

// The drum key on screen is showing values that have moved underneath it -- a kit
// was assigned to this buffer, so every per-key value changed without the mode or the
// selected key changing. setDrumEdit reloads only on a mode or KEY change and
// setPartDrumBuffer early-outs when the buffer number is the same, so neither fires
// here: the controls kept displaying the previous kit's values while the model and the
// device had both moved on, and only navigating to another key and back corrected it.
// USER-REPORTED 2026-09-08.
//
// `setup` comes from the caller rather than being assumed, so the buffer the values
// are read from is the one the part is on RIGHT NOW -- drumSetup is otherwise only
// updated by setDrumEdit, and a refresh can arrive before it.
void PartPanel::refreshDrumKey(int setup)
{
    if (! drumEditActive || drumKey < 0) return;
    if (setup >= 0 && setup < 4) drumSetup = setup;
    loadDrumKey();
    refreshMidiOverrides();
}

// Read-back: set every drum-mode control to the current key+setup's value in the
// device-state model (or the spec default if the model has no entry). Quiet, so
// nothing is transmitted back.
void PartPanel::loadDrumKey()
{
    using mu100::AddressMap;
    if (! drumEditActive || drumKey < 0) return;
    const auto& st = audioProcessor.getEditorState();

    auto rd = [&st, this](int paramIx, int dflt) -> int
    {
        if (paramIx < 0) return dflt;
        const auto v = AddressMap::get(st, AddressMap::param(paramIx), drumKey, drumSetup);
        return v.present ? v.combined : dflt;
    };

    // The baseline the yellow "changed" marker measures against. For a MULTI PART
    // control that is one static per-parameter default and always right; for a DRUM
    // SETUP one it is whatever THIS KIT loads THIS KEY with, which is only knowable
    // since MU100DrumDefaults.h. Using the static default here marked a freshly loaded
    // kit as edited all over -- REVERB SEND alone has ten distinct defaults across the
    // kits and LEVEL has 57, and the static 64 is not even the common one.
    // USER-REPORTED 2026-09-08. Falls back to the static default when the kit is not
    // in the table, which is the old behaviour and no worse than it was.
    auto kitDflt = [this](int paramIx, int dflt) -> double
    {
        if (paramIx < 0 || drumKey < 0) return (double) dflt;
        mu100::ModelIx ix {};
        uint32_t addr = 0;
        AddressMap::resolve(AddressMap::param(paramIx), drumKey, drumSetup, ix, addr);
        const int d = audioProcessor.drumDefaultByte(drumSetup, drumKey, (uint8_t) (addr & 0x7F));
        return (double) (d >= 0 ? d : dflt);
    };

    for (int i = 0; i < (int) mainKnobs.size(); ++i)
        if (kDrumRow[i].drumParam != nullptr)
        {
            mainKnobs[(size_t) i]->setValueQuiet((double) rd(mainDrumParam[i], kDrumRow[i].dflt));
            mainKnobs[(size_t) i]->setDefaultValue(kitDflt(mainDrumParam[i], kDrumRow[i].dflt));
        }

    // Hardware shelf bands (cols 0 & 4) show the drum key's DRUM SETUP EQ; the mid
    // bands (2-4) stay the part's plugin EQ (recalled separately, part-level).
    if (eqBox[EqGain][0]) { eqBox[EqGain][0]->setValueQuiet((double) rd(eqDrumParam[0], kEqDfltVals[0]));
                            eqBox[EqGain][0]->setDefaultValue(kitDflt(eqDrumParam[0], kEqDfltVals[0])); }
    if (eqBox[EqFreq][0]) { eqBox[EqFreq][0]->setValueQuiet((double) rd(eqDrumParam[1], kEqDfltVals[1]));
                            eqBox[EqFreq][0]->setDefaultValue(kitDflt(eqDrumParam[1], kEqDfltVals[1])); }
    if (eqBox[EqGain][4]) { eqBox[EqGain][4]->setValueQuiet((double) rd(eqDrumParam[2], kEqDfltVals[2]));
                            eqBox[EqGain][4]->setDefaultValue(kitDflt(eqDrumParam[2], kEqDfltVals[2])); }
    if (eqBox[EqFreq][4]) { eqBox[EqFreq][4]->setValueQuiet((double) rd(eqDrumParam[3], kEqDfltVals[3]));
                            eqBox[EqFreq][4]->setDefaultValue(kitDflt(eqDrumParam[3], kEqDfltVals[3])); }

    const int grpP  = AddressMap::find("ALTERNATE GROUP",           "DRUM SETUP");
    const int vptchP = AddressMap::find("VELOCITY SENSE PITCH",      "DRUM SETUP");
    const int vlpfP  = AddressMap::find("VELOCITY SENSE LPF CUTOFF", "DRUM SETUP");
    const int kaP    = AddressMap::find("KEY ASSIGN",                "DRUM SETUP");
    if (grpBox)   { grpBox->setValueQuiet  ((double) rd(grpP,   0));  grpBox->setDefaultValue(kitDflt(grpP,   0)); }
    if (vptchBox) { vptchBox->setValueQuiet((double) rd(vptchP, 64)); vptchBox->setDefaultValue(kitDflt(vptchP, 64)); }
    if (vlpfBox)  { vlpfBox->setValueQuiet ((double) rd(vlpfP,  64)); vlpfBox->setDefaultValue(kitDflt(vlpfP,  64)); }
    if (enums.size() > 1)
    {
        enums[1].box->setValueQuiet((double) rd(kaP, 0));
        enums[1].box->setDefaultValue(kitDflt(kaP, 0));
    }

    // Plugin mid-band EQ + HPF2 are per-(buffer,key) in drum mode — refresh them for
    // the newly-selected drum key (they aren't in the DRUM SETUP device model above).
    recallPartEq();
    recallHpf2();
}

// The single place that decides whether each control on this panel accepts input.
// TWO independent conditions gate it, and they OVERLAP on HPF2 + the mid bands, so
// neither may call setEnabled on its own — they are resolved together here.
//
//  1. Drum-key-edit mode with NO key selected. Every drum-mode control addresses the
//     selected key, so at drumKey == -1 every send path drops the write: dispatchMain,
//     dispatchEq and sendDrum all test drumKey >= 0, as do the drum branches of the
//     HPF2 and mid-band EQ onChange handlers. Those paths are also the MODEL write, so
//     an edit made in that state reached neither the device nor the model and was NOT
//     queued for a key picked later — it was silently lost, while the control moved
//     under the mouse and drew its yellow changed-tab as if it had been accepted.
//     GRP/VPTCH/VLPF were gated this way from the start; this extends it to the rest
//     of the panel, which is what makes the gray-out mean "no key selected" rather
//     than "these three in particular".
//  2. setPluginOnlyLocked — HPF2 + the three plugin-only peaking mid bands only. The
//     shelves (bands 1 & 5) are hardware XG params and are never plugin-only-locked.
//
// Uses both setEnabled (visual) and setInterceptsMouseClicks (routing) so the block
// works even if JUCE routes an event before the enabled state is consulted.
void PartPanel::updateEditGating()
{
    const bool keyOk = ! drumEditActive || drumKey >= 0;      // condition (1)
    const juce::Colour labCol = keyOk ? edcol::kTextLabel : edcol::kValueOff;

    // The module title names what the panel edits. In drum-key-edit mode that is a
    // key of a DRUM SETUP buffer, not the part — the knobs, the ASSIGN enum and the
    // EQ shelves all retarget — so "Part" would be actively wrong for as long as the
    // mode is on. When there is no key it also says WHY everything below is grey:
    // that cue otherwise lives only in the Keyboard module's DRUM KEY box, at the
    // far corner of the canvas, so a uniformly grey panel reads as broken. The two
    // reasons are worth separating — "none selected" is an instruction, while
    // "empty key" says the click DID land and the kit simply maps nothing there.
    //
    // Options ▸ Part module title shows ▸ appends what is being edited — the part's
    // number and voice, or the drum tone. Both halves are OFF by default: the
    // name is a genuine convenience for some and a moving string in the corner of the
    // eye for others, and the panel is legible without it. The two are independent
    // bits because the modes are used by different people for different lengths of
    // time; see PartTitleNames in the processor.
    //
    // NAMES ARE PUSHED IN, never looked up here. PartListPanel owns the voice name
    // (it also honours Options ▸ Voice names) and KeyboardPanel owns the drum tone
    // name; a second lookup in this panel would be a second answer, which is the one
    // way this can be wrong rather than merely absent. Empty = not pushed yet, and
    // the title falls back to the bare mode word rather than showing a gap.
    juce::String title;
    if (! drumEditActive)
    {
        title = "Part";
        if ((titleNameMode & TitleNamePart) != 0)
        {
            title += " " + juce::String(currentPart + 1);
            if (partVoiceName.isNotEmpty())
                title += " " + dash() + " " + partVoiceName;
        }
    }
    else if (keyOk)
    {
        title = "Drum Key";
        // Only the keyOk branch takes a name: the other two already carry a dash
        // suffix saying why there is nothing to edit, and there is no tone to name.
        if ((titleNameMode & TitleNameDrum) != 0 && drumToneName.isNotEmpty())
            title += " " + dash() + " " + drumToneName;
    }
    else
    {
        title = drumNoDrumOnKey ? "Drum Key " + dash() + " empty key"
                                : "Drum Key " + dash() + " none selected";
    }
    titleLabel.setText(title, juce::dontSendNotification);

    auto gate = [](ValueBox* b, bool live)
    {
        if (b == nullptr) return;
        b->setEnabled(live);
        b->setInterceptsMouseClicks(live, live);
    };

    // Values shown with no key selected are the drum-row DEFAULTS seeded by
    // applyDrumMode; they belong to no key, so showing them invites reading them as
    // the key's data. Grey says "not editable"; blank says "there is nothing here".
    const bool blankVals = drumEditActive && drumKey < 0;

    // Main knob row. Bare slots (a part-mode VAR/DRY on an insertion, or a main-row
    // position with no DRUM SETUP equivalent) are already inert and setBare owns their
    // mouse state — don't half-revive them with setInteractive(true).
    for (auto& kp : mainKnobs)
    {
        if (kp->isBare()) continue;
        kp->setInteractive(keyOk);
        kp->setLabelDimmed(! keyOk);
        kp->setValueBlank(blankVals);
    }

    // ASSIGN — drum mode's only live enum (M/P is hidden there, and gated separately by
    // updateDrumKitGating in part mode). Label follows that function's convention.
    if (enums.size() > 1)
    {
        gate(enums[1].box.get(), keyOk);
        enums[1].box->setBlank(blankVals);
        enums[1].label->setColour(juce::Label::textColourId, labCol);
    }

    // EQ grid + HPF2. The EQ row/column labels are structural headers, not per-control
    // names, so they are left alone — as they already are under the plugin-only lock.
    //
    // Condition (3): on a part with a DRUMS buffer the two HARDWARE SHELF columns are
    // dead. The MU100 owner's manual, p.82: "When a Drum Part is selected, the EQ menu
    // is not available. However, the same EQ parameters can be adjusted for each
    // individual drum sound in the DRUM menu." MEASURED to match (2026-09-03): a
    // +11.8 dB part shelf on a kit part moves the part meter not at all, while the same
    // boost on the drum KEY moves it as much as it moves a melodic part's.
    //
    // HPF2 and the three PLUGIN mid bands stay LIVE there. They are our own per-voice
    // filters, they run on a drum voice perfectly well (measured: a part HPF2 at 460 Hz
    // does cut a kick), and on a drum part they stack on top of the per-key layer: the
    // part pair shapes the whole kit, the key layer one sound. They were grayed with the
    // shelves from 2026-09-03 to 2026-09-05, on the argument that four live columns
    // beside two dead ones is not a row anyone can read; reversed at the user's
    // direction, because a kit wants a part-wide HPF and mid dip as much as any other
    // part does. Two grayed shelf columns, labels and all, carry that on their own.
    const bool shelvesDead = partShelfEqInert();
    gate(hpf2Box.get(), keyOk && ! pluginOnlyLocked);
    if (hpf2Box) hpf2Box->setBlank(blankVals);
    for (int row = 0; row < 3; ++row)
        for (int band = 0; band < 5; ++band)
        {
            const bool pluginOnly = (band >= 1 && band <= 3);   // mid bands 2-4
            const bool dead = shelvesDead && ! pluginOnly;      // bands 1 and 5 only
            gate(eqBox[row][band].get(), keyOk && ! (pluginOnly && pluginOnlyLocked) && ! dead);
            if (eqBox[row][band]) eqBox[row][band]->setBlank(blankVals);
        }
    // The ROW labels head live columns again, so they stay normal. The two SHELF COLUMN
    // labels carry the greying instead -- the cue has to sit over the dead columns, not
    // across a row that is now mostly live. eqColLabel is offset by one: column 0 is
    // HPF2, so the shelves are columns 1 and 5.
    for (int r = 0; r < 4; ++r)
        eqRowLabel[r].setColour(juce::Label::textColourId, edcol::kTextLabel);
    for (const int c : { 1, 5 })
        for (int line = 0; line < 2; ++line)
            eqColLabel[c][line].setColour(juce::Label::textColourId,
                                          shelvesDead ? edcol::kValueOff : edcol::kTextLabel);

    // The drum-only mid-row boxes exist only in drum mode (hidden otherwise).
    const bool drumOnly = drumEditActive && drumKey >= 0;
    gate(grpBox.get(),   drumOnly);
    gate(vptchBox.get(), drumOnly);
    gate(vlpfBox.get(),  drumOnly);
    for (auto* b : { grpBox.get(), vptchBox.get(), vlpfBox.get() })
        if (b) b->setBlank(blankVals);

    // Every marker follows the gate that was just applied -- HPF2's box AND the knob
    // row. (This comment used to say the knob row needed no call because
    // setInteractive "calls back into the markers already". It does not: only
    // setBare's caller paths do. Re-deciding here is what keeps a grayed knob
    // unmarked.)
    updateAutomationMarkers();
}

// "Disable plugin-only parameters": grey + block HPF2 and the three peaking mid
// bands, the only controls here with no XG address (so the only ones a `.syx`
// export drops). Mode-independent — the same boxes serve part- and drum-edit mode,
// they merely retarget — and value-preserving: this blocks EDITING, it does not
// neutralise anything already dialled in. The greying itself is applied by
// updateEditGating, which owns every enabled state on this panel: it and the
// drum-key gate both cover HPF2 + the mid bands, so applying one here would
// clobber the other's decision on whichever ran last.
void PartPanel::setPluginOnlyLocked(bool locked)
{
    pluginOnlyLocked = locked;
    updateEditGating();
}

// Send a DRUM SETUP parameter change for the current drum key + setup buffer.
void PartPanel::sendDrum(const char* drumParamName, double v)
{
    using mu100::AddressMap;
    if (drumKey < 0) return;
    const int dp = AddressMap::find(drumParamName, "DRUM SETUP");
    if (dp >= 0)
        audioProcessor.sendParamChangeThrottled(AddressMap::makeParamChange(
            AddressMap::param(dp), drumKey, (uint32_t) v, drumSetup));
}

void PartPanel::dispatchMain(int i, double v)
{
    using mu100::AddressMap;
    if (drumEditActive)
    {
        const int dp = mainDrumParam[i];
        if (dp >= 0 && drumKey >= 0)
            audioProcessor.sendParamChangeThrottled(AddressMap::makeParamChange(
                AddressMap::param(dp), drumKey, (uint32_t) v, drumSetup));
        return;
    }
    const int pp = mainPartParam[i];
    if (pp >= 0)
        audioProcessor.sendParamChangeThrottled(AddressMap::makeParamChange(
            AddressMap::param(pp), currentPart, (uint32_t) v));
    // Mirror into the Graphic Display's bar for this part (EditorView filters to
    // the synced channels — VOL / PAN / REV / CHORUS / VAR).
    if (onChannelKnobChanged) onChannelKnobChanged(i, (int) v);
}

void PartPanel::mirrorMainKnob(int knobIndex, int part, int value)
{
    if (drumEditActive) return;                 // knobs show DRUM SETUP, not part values
    if (part != currentPart) return;            // only the part currently on screen
    if (knobIndex < 0 || knobIndex >= (int) mainKnobs.size()) return;
    mainKnobs[(size_t) knobIndex]->setValueQuiet((double) value);
}

void PartPanel::setVariationConnection(bool sys)
{
    varConnectionSys = sys;
    updateVarDryEnabled();   // no-op in drum mode (applyDrumMode owns VAR/DRY there)
}

// Gray (and disable) VAR + DRY when the variation is an INSERTION effect — their
// per-part VARIATION SEND / DRY LEVEL have no effect then. Drum-edit mode owns
// these controls itself (VAR active via DRUM SETUP, DRY bare), so leave them be.
void PartPanel::updateVarDryEnabled()
{
    if (drumEditActive) return;
    setMainKnobActive(kVarKnob, varConnectionSys);
    setMainKnobActive(kDryKnob, varConnectionSys);
}

// Make a main-row knob either a full part-edit knob (active) or a bare, grayed,
// mouse-inert slot. Active restores the part-mode skin + the current part's value;
// this mirrors the part-mode branch of applyDrumMode for a single knob.
void PartPanel::setMainKnobActive(int i, bool active)
{
    using mu100::AddressMap;
    if (i < 0 || i >= (int) mainKnobs.size()) return;
    auto& k = *mainKnobs[(size_t) i];

    if (! active)
    {
        k.setLabel(kMainRow[i].label);
        k.setBare(true);              // grayed disc, value box hidden, inert to mouse/wheel
        updateAutomationMarkers();
        return;
    }

    k.setBare(false);
    k.setLabel(kMainRow[i].label);
    k.setTooltip(withCcNote(mu100::tooltips::forParam("MULTI PART", kMainRow[i].param), kMainRow[i].param));
    applyKind(k, partKindOf(i), mainPartParam[i]);
    k.setDefaultValue(kMainRow[i].dflt);
    k.setEnabled(mainPartParam[i] >= 0);

    int v = kMainRow[i].dflt;
    if (mainPartParam[i] >= 0)
    {
        const auto got = AddressMap::get(audioProcessor.getEditorState(),
                                         AddressMap::param(mainPartParam[i]), currentPart);
        if (got.present) v = got.combined;
    }
    k.setValueQuiet((double) v);
    k.onChange = [this, i](double val) { dispatchMain(i, val); };

    // The bare/active swap decides whether this knob carries a marker.
    updateAutomationMarkers();
}

// Set every tooltip that RETARGETS with the mode (part vs drum-edit). Shelf bands
// 1/5 are hardware params (MULTI PART / DRUM SETUP); the mid bands and HPF2 are
// plugin-only (PLUGIN| keys). ASSIGN is here for the same reason the EQ boxes are:
// its onChange writes MULTI PART|SAME NOTE NUMBER KEY ON ASSIGN in part mode and
// the selected key's DRUM SETUP|KEY ASSIGN in drum mode, so a tooltip fixed at
// construction described the wrong register for half the panel's life.
// Called at init and on every mode switch.
void PartPanel::updateModeTooltips()
{
    // enums[1] == ASSIGN (enums[0], M/P, is part-only and hidden in drum mode).
    if (enums.size() > 1)
        enums[1].box->setTooltip(mu100::tooltips::forKey(
            drumEditActive ? "DRUM SETUP|KEY ASSIGN"
                           : "MULTI PART|SAME NOTE NUMBER KEY ON ASSIGN"));

    if (hpf2Box)
        hpf2Box->setTooltip(mu100::tooltips::forKey(drumEditActive ? "PLUGIN|DRUM HPF2"
                                                                   : "PLUGIN|HPF2"));

    const char* const shelfName[5] = { "EQ BASS", nullptr, nullptr, nullptr, "EQ TREBLE" };
    const char* const rowTok[3]    = { "GAIN", "FREQUENCY", "Q" };
    const juce::String hwTable = drumEditActive ? "DRUM SETUP" : "MULTI PART";
    const juce::String midPfx  = drumEditActive ? "PLUGIN|DRUM EQ B" : "PLUGIN|PART EQ B";

    for (int band = 0; band < 5; ++band)
    {
        const bool hw = (band == 0 || band == 4);
        for (int row = 0; row < 3; ++row)
        {
            auto& box = eqBox[row][band];
            if (! box) continue;
            const juce::String key = hw
                ? hwTable + "|" + shelfName[band] + " " + (row == EqGain ? "GAIN" : "FREQUENCY")
                : midPfx + juce::String(band + 1) + " " + rowTok[row];
            box->setTooltip(mu100::tooltips::forKey(key.toStdString()));
        }
    }
}

// Re-apply the "Mark DAW-automatable params" indicators to the main knob row.
//
// Which knobs qualify is asked of AutomationParams, not listed here, so adding a
// host parameter marks its knob with no edit to this file (and a knob whose
// parameter is dropped stops being marked).
//
// Drum-edit mode is NOT suppressed any more (2026-09-18): the row then edits DRUM
// SETUP, and those keys have host parameters now. It asks the same question of the
// same spec tables, just with the row's DRUM name instead of its MULTI PART one --
// so the knobs with no drum equivalent at all (the vibrato trio, DRY) and the drum
// parameters we chose not to automate (the two Rcv switches -- state, not something
// you ride) go unmarked without either table having to list them.
//
// Still suppressed on a bare slot (VAR/DRY when the variation is routed to an
// insertion): the knob is grayed and inert, so an indicator on it would invite an
// edit that cannot be made here.
void PartPanel::updateAutomationMarkers()
{
    for (int i = 0; i < (int) mainKnobs.size(); ++i)
    {
        const char* const table = drumEditActive ? "DRUM SETUP" : "MULTI PART";
        const char* const param = drumEditActive ? kDrumRow[i].drumParam
                                                 : kMainRow[i].param;

        // isInteractive(), NOT isEnabled(): drum-key gating grays a knob with
        // setInteractive(false), which disables the dial and value box INSIDE the
        // XgKnob and leaves the wrapper itself enabled -- so isEnabled() said yes on
        // every grayed knob, and drum-edit mode with no key picked showed a triangle
        // on each one. USER-REPORTED 2026-10-04.
        const bool live   = ! mainKnobs[(size_t) i]->isBare()
                         && mainKnobs[(size_t) i]->isInteractive();
        const bool marked = param != nullptr
                         && AutomationParams::isAutomated(table, param)
                         && live;

        mainKnobs[(size_t) i]->setAutomationMarker(marked ? automationMarker
                                                          : XgKnob::MarkerNone);

        // The MIDI CC underline: part mode only (no DRUM SETUP parameter has a
        // controller), and the same bare/enabled gate as the triangle, for the same
        // reason -- a grayed knob must not advertise an edit.
        const bool ccMarked = ccMarks && ! drumEditActive
                           && YamahaMU100AudioProcessor::midiCcForMultiPartParam(kMainRow[i].param) >= 0
                           && live;
        mainKnobs[(size_t) i]->setCcMarker(ccMarked);
    }

    // PTIME and PORTA, the left cluster's two CC controls. Hidden in drum-edit mode
    // and inert on a kit part (the device ignores portamento there), and either way
    // no mark. PORTA's name is a plain Label, so its underline is painted here, the
    // way HPF2's triangle is.
    const bool portaLive = ccMarks && ! drumEditActive && ! portaToggle.isDimmed();
    if (ptimeKnob != nullptr)
        ptimeKnob->setCcMarker(portaLive && ptimeKnob->isInteractive());
    if (portaLive != portaCcMarked)
    {
        portaCcMarked = portaLive;
        repaint();
    }

    // HPF2 — automatable per part (HPF201..HPF232) but NOT an XgKnob: it is a bare
    // ValueBox whose name lives in the EQ grid's band-label row, so its marker is
    // drawn by this panel (paint) against that label rather than by a knob. The
    // same rule as the row above, for the same reasons -- except that drum-edit
    // mode no longer suppresses it: the box then retargets at a (buffer, key) HPF2,
    // which has its own parameter now (D158HP2). A box the plugin-only lock has
    // greyed must still not advertise an edit it will refuse.
    const int wantHpf2 = (AutomationParams::isAutomated("PLUGIN", "HPF2")
                          && hpf2Box != nullptr && hpf2Box->isEnabled())
                             ? automationMarker : XgKnob::MarkerNone;

    // The EQ grid's five band GAINs are all automatable, so the marker goes ONCE
    // against the row label rather than five times across the row - the user's call,
    // and the right one: five identical triangles in a row would read as decoration,
    // and the row label is what names them collectively.
    //
    // Deliberately NOT gated on the boxes' enabled state, unlike HPF2 above. The
    // plugin-only lock greys bands 2-4 but leaves the two hardware shelves live, so
    // the row stays partly editable and the greying already says which parts are not.
    // Drum-edit mode does suppress it: there the whole row retargets to a drum key,
    // and no drum-key value has a host parameter.
    const int wantEqGain = (! drumEditActive && AutomationParams::isEqGainRowAutomated())
                               ? automationMarker : XgKnob::MarkerNone;

    if (wantHpf2 != hpf2Marker || wantEqGain != eqGainMarker)
    {
        hpf2Marker   = wantHpf2;
        eqGainMarker = wantEqGain;
        repaint();
    }
}

void PartPanel::setAutomationMarkers(int marker, bool cc)
{
    automationMarker = marker;
    ccMarks = cc;
    updateAutomationMarkers();
}

void PartPanel::dispatchEq(int i, double v)
{
    using mu100::AddressMap;
    const int p = drumEditActive ? eqDrumParam[i] : eqPartParam[i];
    if (p < 0) return;
    if (drumEditActive)
    {
        if (drumKey >= 0)
            audioProcessor.sendParamChangeThrottled(AddressMap::makeParamChange(
                AddressMap::param(p), drumKey, (uint32_t) v, drumSetup));
    }
    else
        audioProcessor.sendParamChangeThrottled(AddressMap::makeParamChange(
            AddressMap::param(p), currentPart, (uint32_t) v));
    notifyEqChanged();   // shelf edit -> repaint the EQ screen
}

//==============================================================================
// Graphic Display EQ-screen bridge (see the header). The value boxes are the
// single source of truth and are already part- / drum-key-aware, so the screen
// simply reads and writes them.
//==============================================================================
int PartPanel::eqRaw(int band, int row) const
{
    if (band < 0)                                        // HPF2 (Hz, 0 = Off)
        return hpf2Box ? (int) hpf2Box->getValue() : 0;
    if (band > 4 || row < 0 || row > 2) return -1;
    const auto& b = eqBox[row][band];
    return b ? (int) b->getValue() : -1;                 // shelf Q cells are null
}

// The ghost half of the same bridge: what the DEVICE holds for a shelf cell, where
// something arriving over MIDI has moved it away from the value above. The param
// arrays are the same ones refreshMidiOverrides marks the boxes from, so the grid's
// gutter bar and the EQ screen's ghost node can never disagree about which register
// they describe.
int PartPanel::eqOverrideRaw(int band, int row) const
{
    if (row < 0 || row > 1) return -1;               // GAIN / FREQ only; shelves have no Q
    const int i = (band == 0) ? row : (band == 4) ? 2 + row : -1;
    if (i < 0) return -1;                            // a mid band or HPF2: plugin-only
    if (drumEditActive)
    {
        if (drumKey < 0) return -1;                  // nothing is addressed in this state
        return audioProcessor.midiOverrideValue(eqDrumParam[i], drumKey, drumSetup);
    }
    return audioProcessor.midiOverrideValue(eqPartParam[i], currentPart);
}

// Both go through the very box the EQ screen is editing, so a value typed at a node is
// parsed by the same rule as one typed into the Part module's cell. A cell with no hook
// (or no box) falls back to plain integers, which is what ValueBox itself does.
juce::String PartPanel::eqTextFromValue(int band, int row, double v) const
{
    const ValueBox* b = (band < 0) ? hpf2Box.get()
                      : (band <= 4 && row >= 0 && row <= 2) ? eqBox[row][band].get()
                      : nullptr;
    if (b != nullptr && b->textFromValue) return b->textFromValue(v);
    return juce::String((int) v);
}

double PartPanel::eqValueFromText(int band, int row, const juce::String& t) const
{
    const ValueBox* b = (band < 0) ? hpf2Box.get()
                      : (band <= 4 && row >= 0 && row <= 2) ? eqBox[row][band].get()
                      : nullptr;
    if (b != nullptr && b->valueFromText) return b->valueFromText(t);
    return t.getDoubleValue();
}

void PartPanel::setEqFromDisplay(int band, int row, int raw)
{
    if (band < 0) { if (hpf2Box) hpf2Box->setValueNotify((double) raw); return; }
    if (band > 4 || row < 0 || row > 2) return;
    if (eqBox[row][band]) eqBox[row][band]->setValueNotify((double) raw);
}

void PartPanel::nudgeEqFromDisplay(int band, int row, int dir, bool fine)
{
    if (band < 0) { if (hpf2Box) hpf2Box->nudge(dir, fine); return; }
    if (band > 4 || row < 0 || row > 2) return;
    if (eqBox[row][band]) eqBox[row][band]->nudge(dir, fine);
}

void PartPanel::resetEqBand(int band)
{
    // ONE undo step for the whole reset. A band's default is GAIN + FREQ + Q — three
    // model writes with three different coalescing keys, so without a scope the EQ
    // screen's right-click reset lands as three separate steps and a single Undo
    // restores only the LAST of them (Q, which does not even move the node) — the
    // reset reads as "Undo does nothing". The node DRAG beside it is already one step
    // for exactly this reason; the reset has to match it.
    YamahaMU100AudioProcessor::UndoScope undoStep(
        audioProcessor,
        (drumEditActive ? "Drum key " + juce::String(drumKey)
                        : "Part " + juce::String(currentPart + 1))
        + (band < 0 ? juce::String(" HPF2 reset")
                    : " EQ " + juce::String(juce::jlimit(0, 4, band) + 1) + " reset"));

    if (band < 0) { if (hpf2Box) hpf2Box->setValueNotify(0.0); return; }   // HPF2 -> Off
    band = juce::jlimit(0, 4, band);
    if (eqBox[EqGain][band]) eqBox[EqGain][band]->setValueNotify(64.0);                       // 0 dB
    if (eqBox[EqFreq][band]) eqBox[EqFreq][band]->setValueNotify(eqFreqDefault(band));
    if (eqBox[EqQ][band])    eqBox[EqQ][band]->setValueNotify(7.0);                            // Q 0.7 (mids only)
}

bool PartPanel::eqFreqIsHz(int band)
{
    return band >= 1 && band <= 3;   // the plugin-only peaking bands; shelves are raw
}

double PartPanel::eqFreqToHz(int band, int v)
{
    if (band < 0)         return (double) v;    // HPF2 is already Hz
    if (eqFreqIsHz(band)) return (double) v;
    return eqTableHz(v);
}

double PartPanel::eqFreqDefault(int band)
{
    if (band < 0) return 0.0;                   // HPF2 -> Off
    band = juce::jlimit(0, 4, band);
    const int raw = kEqBandFreqDflt[band];
    return eqFreqIsHz(band) ? eqTableHz(raw) : (double) raw;
}

void PartPanel::eqFreqRange(int band, int& lo, int& hi)
{
    if (band < 0) { lo = 0; hi = 1000; return; }   // HPF2 Hz
    band = juce::jlimit(0, 4, band);
    if (eqFreqIsHz(band)) { lo = (int) eqTableHz(kEqBandFreqLo[band]); hi = (int) eqTableHz(kEqBandFreqHi[band]); return; }
    lo = kEqBandFreqLo[band];
    hi = kEqBandFreqHi[band];
}

// Reconfigure every control for the current mode (part vs drum-edit).
void PartPanel::applyDrumMode()
{
    const bool d = drumEditActive;

    for (int i = 0; i < (int) mainKnobs.size(); ++i)
    {
        auto& k = *mainKnobs[(size_t) i];
        if (d)
        {
            const auto& c = kDrumRow[i];
            k.setLabel(c.label);
            k.setTooltip(c.drumParam != nullptr
                             ? juce::String(mu100::tooltips::forParam("DRUM SETUP", c.drumParam))
                             : juce::String());
            if (c.drumParam != nullptr)
            {
                k.setBare(false);
                k.setEnabled(true);
                applyKind(k, c.kind, mainDrumParam[i]);
                k.setDefaultValue(c.dflt);
                k.setValueQuiet(c.dflt);
                k.onChange = [this, i](double v) { dispatchMain(i, v); };
            }
            else
            {
                k.setBare(true);   // no drum equivalent (RATE/DEPTH/DELAY/DRY)
            }
        }
        else
        {
            k.setBare(false);
            k.setLabel(kMainRow[i].label);
            k.setTooltip(withCcNote(mu100::tooltips::forParam("MULTI PART", kMainRow[i].param), kMainRow[i].param));
            applyKind(k, partKindOf(i), mainPartParam[i]);
            k.setDefaultValue(kMainRow[i].dflt);
            k.setEnabled(mainPartParam[i] >= 0);
            k.setValueQuiet((double) kMainRow[i].dflt);
            k.onChange = [this, i](double v) { dispatchMain(i, v); };
        }
    }

    // The EQ grid's shelf bands (cols 0 & 4) keep a fixed config; dispatchEq routes
    // them to MULTI PART or DRUM SETUP by mode, and their values reload via
    // loadDrumKey (drum) / applyState (part). The mid bands are part-level plugin EQ
    // in both modes. So nothing to reconfigure here.

    // Mid-row: show part-only controls in part mode, drum-only ones in drum mode.
    const bool sp = ! d;
    if (enums.size() > 0) { enums[0].label->setVisible(sp); enums[0].box->setVisible(sp); }
    portaLabel.setVisible(sp); portaToggle.setVisible(sp);
    if (ptimeKnob)    ptimeKnob->setVisible(sp);
    scaleLabel.setVisible(sp); if (scaleNoteBox) scaleNoteBox->setVisible(sp);
    tuneLabel.setVisible(sp);  if (tuneBox)      tuneBox->setVisible(sp);
    grpLabel.setVisible(d);    if (grpBox)   grpBox->setVisible(d);
    vptchLabel.setVisible(d);  if (vptchBox) vptchBox->setVisible(d);
    vlpfLabel.setVisible(d);   if (vlpfBox)  vlpfBox->setVisible(d);

    // Assign options: drum mode uses only Single/Multi (no Inst); part mode has all three.
    // Clamp the current value so it stays in range when shrinking from 3 to 2 options.
    if (enums.size() > 1)
    {
        enums[1].box->setOptions(d ? juce::StringArray { "Single", "Multi" }
                                   : juce::StringArray { "Single", "Multi", "Inst" });
        if (d && enums[1].box->getValue() > 1.0)
            enums[1].box->setValueQuiet(1.0);
    }

    // Nothing on the panel is editable in drum mode until a key is actually selected.
    updateEditGating();

    // Returning to part mode: reload the current part's real values, then re-apply
    // the CON gating (VAR/DRY gray out when the variation is an insertion effect).
    if (! d) { applyState(audioProcessor.getEditorState()); updateVarDryEnabled(); }
    updateModeTooltips();   // EQ/HPF2/ASSIGN tooltips switch Part<->Drum with the mode
    updateRcvRowGating(); // the LED row grays out for the duration of drum-edit mode
    updateDrumKitGating();// re-assert: the left cluster is re-shown on the way back
    updateAutomationMarkers();  // the row's markers switch tables with the mode
    repaint();
}

PartPanel::~PartPanel() = default;

void PartPanel::setCurrentPart(int part0to31)
{
    const int p = juce::jlimit(0, 31, part0to31);
    if (p == currentPart) return;
    currentPart = p;
    recallHpf2();     // HPF2 is plugin state (from the processor), not a SysEx param
    recallPartEq();   // plugin mid-band EQ is likewise per-part plugin state
    refreshEqBypass();// the lamp belongs to the PART, so it changes with the part
    // The title carries the part NUMBER, and updateEditGating is the only place it is
    // composed. The name pushes reach it too, but setPartVoiceName early-outs on an
    // unchanged string — so with both parts still on the default voice nothing
    // recomposed the title and the number stayed on the part last left. The number is
    // ours, not the name owner's, so recompose it here instead of waiting for a push.
    updateEditGating();
    if (onPartChanged) onPartChanged(currentPart);
    // (Per-part control read-back arrives with device-state sync, Phase 5.)
}

// Two lit lamps on one kit are identical to look at and behave differently: the part
// the user actually pressed keeps its bypass when it moves to another buffer, the one
// that inherited it from the kit does not. That is the one thing about this control the
// GUI knows and the user cannot see, so the tooltip says it -- in TEXT, on the control
// itself, rather than as a second meaning loaded onto the lamp's colour.
//
// Only on a drum-buffer part: on a melodic part there is nothing to inherit from and the
// distinction would be noise. Returned rather than set, because the ctor needs it too.
juce::String PartPanel::eqBypTooltip() const
{
    juce::String tip = mu100::tooltips::forKey("PLUGIN|EQ BYPASS");
    if (partBuffer < 0 || ! audioProcessor.isPartEqBypassed(currentPart))
        return tip;
    return tip + (audioProcessor.isPartEqBypassChosen(currentPart)
                    ? juce::String(" - bypassed from this part")
                    : " - bypassed with " + juce::String(drumbuf::name(partBuffer)));
}

// The lamp and the bracket both read the processor's live mask, so this is the one
// place either is refreshed -- from a click here, from the keyboard shortcut, from the
// badge's clear-all, from a part change, or from a buffer change (which moves no bit but
// can change which of the two states above the part is in). setState(..., false)
// deliberately does NOT notify: notifying would call straight back into setPartEqBypass.
void PartPanel::refreshEqBypass()
{
    eqBypToggle.setState(audioProcessor.isPartEqBypassed(currentPart), false);
    // Also the toggle's ACCESSIBLE description (DotToggle::setTooltip keeps rawTip in
    // step), so a screen reader gets the same distinction the tooltip gives the mouse.
    eqBypToggle.setTooltip(eqBypTooltip());
    repaint();   // the "EQ - BYPASSED" bracket
}


//==============================================================================
// The MIDI-override marks for this part. Every control keeps its own value; this
// only tells each one where the DEVICE currently is, so it can draw the second
// mark beside it.
//
// MODE-AWARE, and that is the whole of the drum half. The main row edits MULTI PART
// in part mode and DRUM SETUP in drum-edit mode, so the marks have to follow the same
// switch or they would point at a different parameter than the one under the knob --
// which is what suppressing them wholesale here used to avoid.
//
// The drum side is reachable ONLY by SysEx parameter change: no XG controller lands
// on a DRUM SETUP address, and no DAW automation parameter is bound to one either
// (AutomationParams covers the mixer controls, all MULTI PART). So a mark here always
// means "a SysEx parameter change moved this key", which is exactly what it says.
//
// The instance pair differs too: a MULTI PART parameter is addressed by PART, a DRUM
// SETUP one by (key, buffer) -- hence the two-argument form on the drum branch.
void PartPanel::refreshMidiOverrides()
{
    using mu100::AddressMap;
    const bool show  = audioProcessor.showMidiOverrides;
    const bool drums = drumEditActive;
    // In drum mode with no key selected there is nothing addressed at all, so every
    // mark comes down -- the same state in which every send path drops its write.
    const bool live  = show && (! drums || drumKey >= 0);

    auto value = [&](int paramIndex) -> int
    {
        if (! live || paramIndex < 0) return -1;
        return drums ? audioProcessor.midiOverrideValue(paramIndex, drumKey, drumSetup)
                     : audioProcessor.midiOverrideValue(paramIndex, currentPart);
    };
    auto markKnob = [&](XgKnob* k, int paramIndex)
    {
        if (k == nullptr) return;
        const int v = value(paramIndex);
        k->setMidiOverride(v < 0 ? 0.0 : (double) v, v >= 0);
    };
    auto markBox = [&](ValueBox* b, int paramIndex)
    {
        if (b == nullptr) return;
        const int v = value(paramIndex);
        b->setMidiOverride(v < 0 ? 0.0 : (double) v, v >= 0);
    };

    if (drums)
    {
        // The main row, rebound to its DRUM SETUP parameters (a null drumParam leaves
        // the knob inert in this mode, and mainDrumParam is -1 for it).
        for (size_t i = 0; i < mainKnobs.size(); ++i)
            markKnob(mainKnobs[i].get(), mainDrumParam[i]);

        // PORTAMENTO is a part parameter and is not shown in this mode; leaving its
        // ring lit from part mode would be the one stale mark on the panel.
        ptimeKnob->setMidiOverride(0.0, false);
        portaToggle.setMidiOverride(false);

        // The two hardware shelves -- per (key, buffer) here, not per part.
        markBox(eqBox[EqGain][0].get(), eqDrumParam[0]);
        markBox(eqBox[EqFreq][0].get(), eqDrumParam[1]);
        markBox(eqBox[EqGain][4].get(), eqDrumParam[2]);
        markBox(eqBox[EqFreq][4].get(), eqDrumParam[3]);

        // ...and the three drum-only boxes plus the ASSIGN enum, which in this mode
        // is KEY ASSIGN. Resolved here the same way loadDrumKey resolves them.
        markBox(grpBox.get(),   AddressMap::find("ALTERNATE GROUP",           "DRUM SETUP"));
        markBox(vptchBox.get(), AddressMap::find("VELOCITY SENSE PITCH",      "DRUM SETUP"));
        markBox(vlpfBox.get(),  AddressMap::find("VELOCITY SENSE LPF CUTOFF", "DRUM SETUP"));
        if (enums.size() > 1)
            markBox(enums[1].box.get(), AddressMap::find("KEY ASSIGN", "DRUM SETUP"));

        // M/P and TUNE are part parameters with no DRUM SETUP counterpart, and they
        // are not what this mode edits -- so their part-mode marks come down here,
        // for the same reason PORTAMENTO's does.
        if (enums.size() > 0) markBox(enums[0].box.get(), -1);
        markBox(tuneBox.get(), -1);
        return;
    }

    // The drum-only boxes carry no part parameter, so they must be cleared on the way
    // back out of drum mode rather than simply left alone.
    markBox(grpBox.get(),   -1);
    markBox(vptchBox.get(), -1);
    markBox(vlpfBox.get(),  -1);

    // The channel strip. Seven of the eleven controllers the device actually honours
    // land here (volume, pan, resonance, cutoff and the three sends), which is what
    // makes this row the one that had to be covered first.
    for (size_t i = 0; i < mainKnobs.size(); ++i)
        markKnob(mainKnobs[i].get(),
                 kMainRow[i].param != nullptr
                     ? AddressMap::find(kMainRow[i].param, "MULTI PART") : -1);

    markKnob(ptimeKnob.get(), AddressMap::find("PORTAMENTO TIME", "MULTI PART"));

    // The portamento SWITCH is a toggle, not a value box, so it takes the ring form
    // of the mark. It is reachable by a controller (CC65), which is why it is here
    // and the other LEDs on this panel are not -- nothing can move those but SysEx,
    // and a SysEx write to one is rare enough not to have earned its own cue yet.
    {
        const int ix = AddressMap::find("PORTAMENTO SWITCH", "MULTI PART");
        portaToggle.setMidiOverride(show && ix >= 0
                                    && audioProcessor.midiOverrideValue(ix, currentPart) >= 0);
    }

    // The two hardware EQ shelves. No controller reaches these -- only a SysEx
    // parameter change does -- but that is a door like any other, and leaving them
    // unmarked would make the grid the one place the feature quietly stops.
    markBox(eqBox[EqGain][0].get(), eqPartParam[0]);
    markBox(eqBox[EqFreq][0].get(), eqPartParam[1]);
    markBox(eqBox[EqGain][4].get(), eqPartParam[2]);
    markBox(eqBox[EqFreq][4].get(), eqPartParam[3]);

    // The ASSIGN enum is SAME NOTE NUMBER here and KEY ASSIGN in drum mode. It has to
    // be marked in BOTH modes or it keeps the other mode's mark on the way across.
    if (enums.size() > 1)
        markBox(enums[1].box.get(),
                AddressMap::find("SAME NOTE NUMBER KEY ON ASSIGN", "MULTI PART"));

    // ...and the M/P enum beside it, which is MONO/POLY MODE in this mode and has no
    // counterpart in the other.
    if (enums.size() > 0)
        markBox(enums[0].box.get(), AddressMap::find("MONO/POLY MODE", "MULTI PART"));

    // TUNE shows the SCALE TUNING byte for whichever note the SCALE box names, so the
    // mark follows that selector as well as the part -- the same resolution the value
    // itself goes through.
    {
        const int note = scaleNoteBox != nullptr
                             ? juce::jlimit(0, 11, (int) scaleNoteBox->getValue()) : 0;
        markBox(tuneBox.get(), scaleParam[note]);
    }
}

void PartPanel::applyState(const mu100::DeviceState& st)
{
    using mu100::AddressMap;

    // HPF2 and the plugin mid-band EQ are per-part plugin state (not in the device
    // dump) — refresh for the current part in both normal and drum-edit modes.
    recallHpf2();
    recallPartEq();

    // Receive-enable LED row — every switch defaults ON where the model is silent.
    // Refreshed BEFORE the drum-edit early return below: these stay part-level in
    // both modes (they have no DRUM SETUP counterpart to retarget to).
    for (auto& led : rcvLeds)
    {
        if (led.param < 0) continue;
        const auto rv = AddressMap::get(st, AddressMap::param(led.param), currentPart);
        led.dot->setState(rv.present ? rv.combined != 0 : true, false);
    }

    // In drum-edit mode the controls show DRUM SETUP values, not this part's
    // MULTI PART values — refresh the selected drum key from the new state.
    if (drumEditActive) { loadDrumKey(); return; }

    // Overwrite `inout` with the current part's value for a MULTI PART param IF
    // the model has it; otherwise leave `inout` as the caller's factory default.
    // Falling back to the default (rather than leaving the control alone) is what
    // stops a knob from showing the *previous* part's value when the model has no
    // entry for this part — the "sticking" seen before any read-back.
    auto getVal = [&st, this](const char* paramName, int& inout)
    {
        const int ix = AddressMap::find(paramName, "MULTI PART");
        if (ix < 0) return;
        const auto v = AddressMap::get(st, AddressMap::param(ix), currentPart);
        if (v.present) inout = v.combined;
    };

    // Main channel-strip row — mainKnobs[i] mirrors kMainRow[i] order (built 1:1).
    // HPF (index 9) is included: it resolves to the "0A nn 20" param and reads back
    // from the bulk dump like the rest.
    for (size_t i = 0; i < mainKnobs.size(); ++i)
    {
        if (kMainRow[i].param == nullptr) continue;
        int v = kMainRow[i].dflt;
        getVal(kMainRow[i].param, v);
        mainKnobs[i]->setValueQuiet((double) v);
    }

    // EQ grid shelf bands (cols 0 & 4) from MULTI PART EQ. Mid bands (recallPartEq,
    // above) are plugin state. Order matches kEqParamNames: BASS G/F, TREBLE G/F.
    auto setShelf = [&](int row, int band, const char* param, int dflt)
    {
        if (! eqBox[row][band]) return;
        int v = dflt; getVal(param, v);
        eqBox[row][band]->setValueQuiet((double) v);
    };
    setShelf(EqGain, 0, "EQ BASS GAIN",        64);
    setShelf(EqFreq, 0, "EQ BASS FREQUENCY",   12);
    setShelf(EqGain, 4, "EQ TREBLE GAIN",      64);
    setShelf(EqFreq, 4, "EQ TREBLE FREQUENCY", 54);

    // Portamento Time + switch.
    { int v = 0; getVal("PORTAMENTO TIME", v);   if (ptimeKnob) ptimeKnob->setValueQuiet((double) v); }
    { int v = 0; getVal("PORTAMENTO SWITCH", v); portaToggle.setState(v != 0, false); }

    // Enum controls — enums[0] = Mono/Poly (dflt poly), enums[1] = Assign (dflt multi).
    if (enums.size() > 0) { int v = 1; getVal("MONO/POLY MODE",   v); enums[0].box->setValueQuiet((double) v); }
    if (enums.size() > 1) { int v = 1; getVal("SAME NOTE NUMBER KEY ON ASSIGN", v); enums[1].box->setValueQuiet((double) v); }

    // SCALE micro-tuning: reload all 12 cents for this part (model or default 64),
    // then show the selected note. (scaleCache is shared across parts, so it must
    // be reloaded on every part switch — edits were recorded into the model.)
    static const char* const kScaleParamNames[12] = {
        "SCALE TUNING C",  "SCALE TUNING C#", "SCALE TUNING D",  "SCALE TUNING D#",
        "SCALE TUNING E",  "SCALE TUNING F",  "SCALE TUNING F#", "SCALE TUNING G",
        "SCALE TUNING G#", "SCALE TUNING A",  "SCALE TUNING A#", "SCALE TUNING B"
    };
    for (int i = 0; i < 12; ++i) { int v = 64; getVal(kScaleParamNames[i], v); scaleCache[i] = v; }
    if (tuneBox && scaleNoteBox)
    {
        const int note = juce::jlimit(0, 11, (int) scaleNoteBox->getValue());
        tuneBox->setValueQuiet((double) scaleCache[note]);
    }
}

void PartPanel::paint(juce::Graphics& g)
{
    g.setColour(edcol::kModuleBg);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.f);
    g.setColour(edcol::kModuleBorder);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.f, 1.f);

    // Separator above the receive-enable LED row, dividing it from the main row's
    // parameter labels. Same 1px / edcol::kRule as the module border and the Voice
    // module's column separator.
    if (rcvRowVisible && ! rcvSepLine.isEmpty())
    {
        g.setColour(edcol::kRule);
        g.fillRect(rcvSepLine);
    }

    // XGEdit-style group brackets over the parameter runs.
    auto bracketOver = [&g](juce::Component* firstBox, juce::Component* lastBox, const char* label)
    {
        xgbracket::draw(g, firstBox->getX(), lastBox->getRight(),
                        firstBox->getY() - 7, label);
    };

    // "EQ" bracket over the whole six-column grid: from the HPF2 column across to
    // band 5. Can't use bracketOver — HPF2 has no GAIN box to take a left edge and a
    // y from, so the span starts at the HPF2 box (FREQ row) while the line still
    // rides above the GAIN row.
    if (eqBox[EqGain][0] && eqBox[EqGain][4] && hpf2Box)
    {
        // Text, not a colour: the one cue that cannot be misread as meaning something
        // else. It names, over the very boxes it sits above, what those boxes are.
        //   bypassed                                 -> what you are not hearing
        //   else, shelves dead (drum buffer, part mode) -> where THOSE TWO live instead
        // BYPASS WINS when both are true: it is the whole grid, it is temporary, and it
        // is the state a user is most likely to have forgotten about. The shelf columns
        // say their own piece by being grayed. In DRUM-EDIT mode every column is live
        // (they edit the key), so only the bypass text can appear there.
        // Kept SHORT: the bracket centres its label in the grid's width and a label
        // wider than that span silently swallows the bracket's own lines.
        const char* label = audioProcessor.isPartEqBypassed(currentPart) ? "EQ - BYPASSED"
                          : partShelfEqInert() ? "EQ - SHELVES PER KEY"
                          : "EQ";
        xgbracket::draw(g, hpf2Box->getX(), eqBox[EqGain][4]->getRight(),
                        eqBox[EqGain][0]->getY() - 7, label);
    }

    // The EQ grid's two DAW-automatable markers, drawn here because neither control
    // is an XgKnob: HPF2's against the second line of its band label ("HPF2"), and
    // one for the whole GAIN row against that row's label. Same geometry and colour
    // as the knob row's — see XgKnob::paintAutomationMarker, which reads the label's
    // justification, so the right-justified "GAIN" lands correctly.
    XgKnob::paintAutomationMarker(g, eqColLabel[0][1], hpf2Marker);
    XgKnob::paintAutomationMarker(g, eqRowLabel[EqGain], eqGainMarker);
    if (portaCcMarked)
        XgKnob::paintAutomationMarker(g, portaLabel, XgKnob::MarkerUnderline);

    // The main-row groups only carry these meanings when the knobs under them are
    // live. Drum-edit mode re-skins the row (kDrumRow): VIBRATO's three knobs go
    // inert, so that bracket is dropped; FILTER keeps HPF/RES/CUTOFF unchanged; and
    // EFFECT loses DRY (no DRUM SETUP equivalent), so it spans REV/CHORUS/VAR only.
    if (mainKnobs.size() >= 16)
    {
        if (! drumEditActive)
            bracketOver(mainKnobs[6].get(), mainKnobs[8].get(), "VIBRATO"); // RATE/DEPTH/DELAY

        bracketOver(mainKnobs[9].get(), mainKnobs[11].get(), "FILTER");     // HPF/RES/CUTOFF
        bracketOver(mainKnobs[12].get(),
                    mainKnobs[drumEditActive ? 14 : 15].get(), "EFFECT");   // REV/CHORUS/VAR[/DRY]
    }
}

void PartPanel::resized()
{
    auto b = getLocalBounds().reduced(6);

    // Header: the module title, at the LEFT — every other module titles itself on the
    // right, and this one is the exception because it is the only title that CHANGES.
    // See updateEditGating for what it can say; with the naming option on it also
    // carries a voice or drum-tone name, so its width is not knowable here at all.
    // Right-aligned, that growth ran back across the header toward the EQ grid, which
    // has its own longest-string problem directly below it; left-aligned, the title is
    // anchored and grows into empty space.
    //
    // It takes the WHOLE header row rather than a measured cell. Nothing else occupies
    // that row, so there is nothing for it to collide with, and a cell sized to a list
    // of known strings is exactly what a dynamic name would outgrow — silently, by
    // ellipsis, and only for the longest voice names. Part selection is driven by the
    // Voice list. (This retires the old measure-the-widest-of-four cell; the "Editor
    // text scaling" rule it served is satisfied more simply by not having a cell.)
    auto header = b.removeFromTop(22);
    titleLabel.setBounds(header);
    b.removeFromTop(4);

    // The value-box height, plus the strip a horizontal MIDI-override bar needs under
    // it in the two styles that draw one (zero in the third, so that layout is exactly
    // what it has always been). ONE number: the EQ grid's row pitch, the left cluster's
    // cells and the row labels all derive from it, and XgKnob works the same figure out
    // for itself from its own value box -- both through midimarks::stripHeight.
    const int valueH = 16 + midimarks::stripHeight(false);
    const int labelH = 14;

    // The 16-knob main strip sits along the bottom; the upper "mid area" is taller
    // than one row now because it holds the 3-row EQ grid (plus HPF2 + the left
    // cluster of enums / scale / porta).
    const int mainRowH = 74;
    const int rowGap   = 14;

    // Bottom strip: the receive-enable LED row, under a separator line. Taken off
    // first so the main knob row keeps its full height (the module grew by kRcvAreaH
    // to make room — see kPartRect in EditorView.cpp).
    layOutRcvLeds(b.removeFromBottom(kRcvAreaH));

    auto mainRow = b.removeFromBottom(mainRowH);
    b.removeFromBottom(rowGap);
    auto midArea = b;

    // EQ grid geometry the LEFT CLUSTER shares, hoisted so the two cannot disagree
    // about where a row is: eqBracketH is the headroom the grid leaves above its
    // GAIN row for the "EQ" bracket paint() draws there, and eqRowPitch is one row
    // of the grid. The cluster sits one pitch down from the bracket, i.e. level with
    // the FREQ row — see the cluster's own comment for why that row and not GAIN.
    const int eqBracketH = 12;
    const int eqRowSep   = 6;
    const int eqRowPitch = valueH + eqRowSep;

    // The band labels' font and line height, hoisted: the EQ block measures its
    // columns with the font, and the grid's own vertical position is derived from the
    // line height (see eqShiftUp).
    const juce::Font bandFont (edtext::font(edtext::Label, 11.f, juce::Font::bold));
    const int bandLineH = juce::jmax(labelH, (int) std::ceil(bandFont.getHeight()) + 1);

    // How far the whole EQ block sits ABOVE the mid area's top inset. One band-label
    // line: the block ends in TWO such lines, and without this the second of them
    // crowds the VIBRATO / FILTER / EFFECT brackets over the knob row below. Derived
    // from the label rather than fixed, so it keeps its clearance at any text scale --
    // the "edtext scales fonts but not cells" rule, applied to a gap instead of a cell.
    //
    // It spends the headroom the "EQ" bracket had over the module's title row, which
    // is empty on the right-hand side where the bracket sits (the title is anchored
    // LEFT precisely because it is the one title that grows).
    const int eqShiftUp = bandLineH;
    const int eqTop     = midArea.getY() + eqBracketH - eqShiftUp;

    // ---- Left cluster (one row of stacked value+label cells): Mono/Poly + Assign,
    // SCALE + TUNE, Porta switch, Porta Time. Drum-edit overlays (VPTCH/VLPF/GRP)
    // reuse the captured SCALE/TUNE/PORTA cells.
    //
    // Its boxes are top-aligned with the EQ grid's Q row -- the THIRD row down, since
    // the grid moved up by eqShiftUp and the cluster stayed where it was. Being on a
    // grid row at all is the point: both read across as one row of value boxes, and
    // asking for "the grid's Nth row" rather than for a number means the cluster
    // follows the grid's spacing instead of drifting off it.
    //
    // It must also stay clear of the module's top-left corner, which the title needs
    // (it can run to "Drum Key — none selected"), and of the "EQ" bracket, which can
    // run to "EQ — SHELVES PER KEY". Both of those are reasons it cannot go back UP.
    int leftClusterRight = midArea.getX();   // how far the EQ block may grow leftwards
    {
        // Cells are removed from the left and each hugs the top of this rect.
        auto row = midArea.withTrimmedTop(eqTop - midArea.getY() + 2 * eqRowPitch);

        // Cell widths are MEASURED from the real font, not fixed, for the same reason
        // the EQ columns are (edtext scales fonts but not cells, so a hardcoded width
        // ellipsizes as editorTextLabelsPct grows). Each cell is reduced(3, 0) before
        // its label is placed, so it must fit the widest label it will EVER show —
        // its part-mode one AND the drum-edit overlay that reuses the same rect —
        // plus that 6 px inset. Floored at the old fixed widths, so nothing tightens
        // and the default look is unchanged.
        const juce::Font labFont (edtext::font(edtext::Label, 11.f, juce::Font::bold));
        auto cellW = [&labFont](int minW, std::initializer_list<const juce::Label*> ls)
        {
            int w = 0;
            for (auto* l : ls)
                w = juce::jmax(w, juce::GlyphArrangement::getStringWidthInt(labFont, l->getText()));
            return juce::jmax(minW, w + 6 + kCellLabelGap);
        };

        int enumW = 84;
        for (auto& e : enums) enumW = cellW(enumW, { e.label.get() });
        for (auto& e : enums)
        {
            auto cell = row.removeFromLeft(enumW).reduced(3, 0);
            e.box->setBounds(cell.removeFromTop(valueH));
            e.label->setBounds(cell.removeFromTop(labelH));
        }

        auto labelledBox = [&row, valueH, labelH](juce::Label& lab, ValueBox& box, int w)
        {
            auto cell = row.removeFromLeft(w);
            auto inner = cell.reduced(3, 0);
            box.setBounds(inner.removeFromTop(valueH));
            lab.setBounds(inner.removeFromTop(labelH));
            return cell;
        };
        // SCALE / TUNE also host the drum overlays VPTCH / VLPF, so they size to both.
        scaleCell = labelledBox(scaleLabel, *scaleNoteBox, cellW(52, { &scaleLabel, &vptchLabel }));
        tuneCell  = labelledBox(tuneLabel,  *tuneBox,      cellW(52, { &tuneLabel,  &vlpfLabel  }));

        // PORTA hosts the GRP overlay. Its cell was the one fixed at 48 while the
        // neighbouring 52 px cells rendered the same-length label fine — hence the
        // ellipsis here first. Measuring it also retires the old grpBox
        // `portaCell.withWidth(52)` fudge, which placed GRP 4 px WIDER than the cell
        // it sits in (spilling into PTIME); GRP is now just another label this cell
        // is sized to hold.
        auto portaCell = row.removeFromLeft(cellW(48, { &portaLabel, &grpLabel }));
        {
            auto cell = portaCell.reduced(3, 0);
            auto valArea = cell.removeFromTop(valueH);
            const int s = juce::jmax(12, juce::jmin(valArea.getWidth(), valueH));
            portaToggle.setBounds(valArea.withSizeKeepingCentre(s, s));
            portaLabel.setBounds(cell.removeFromTop(labelH));
        }

        ptimeCell = row.removeFromLeft(72);
        ptimeKnob->setBounds(ptimeCell.reduced(2, 0));   // dial hidden: uses only its top 30 px

        auto placeBox = [valueH, labelH](juce::Label& lab, ValueBox* box, juce::Rectangle<int> cell)
        {
            if (box == nullptr) return;
            auto inner = cell.reduced(3, 0);
            box->setBounds(inner.removeFromTop(valueH));
            lab.setBounds(inner.removeFromTop(labelH));
        };
        placeBox(vptchLabel, vptchBox.get(), scaleCell);
        placeBox(vlpfLabel,  vlpfBox.get(),  tuneCell);
        placeBox(grpLabel,   grpBox.get(),   portaCell);

        leftClusterRight = row.getX();   // whatever the cells above consumed
    }

    // ---- EQ block, right-aligned in midArea: [row labels][HPF2 | bands 1..5].
    // Three value rows (GAIN/FREQ/Q), slightly separated, with a two-line band label
    // under each column and the GAIN/FREQ/Q/BAND row labels to their left. HPF2 is
    // the grid's first column and has a FREQ cell only, so the GAIN and Q rows are
    // empty there. The "EQ" bracket spans HPF2 + all five bands (drawn in paint()).
    {
        // The boxes are as wide as their widest value plus kEqValuePad a side, so the
        // value text always clears the changed-tab by at least the tab's own width
        // (a 3-digit gain used to sit right on it) and never gets squeezed to fit.
        // Measured from the real font — a fixed width tightens as the text scale
        // rises. Floored at the old fixed 48 so narrower text can't shrink the grid.
        const juce::Font valueFont (edtext::font(edtext::Value, 11.f));
        int widestValue = 0;
        for (const char* s : kEqWidestValues)
            widestValue = juce::jmax(widestValue,
                juce::GlyphArrangement::getStringWidthInt(valueFont, s));
        const int boxW = juce::jmax(48, widestValue + 2 * kEqValuePad);

        // The band labels, not the boxes, set the column pitch: widen the columns until
        // the longest lines ("SHELF") clear their neighbours by kBandLabelGap. Measured
        // from the real font so the gap survives any editor text scale (edtext scales
        // fonts but not cells, so a fixed pitch would collide as the user scales up).
        // Floored at the box width and capped short of the left cluster. `bandFont` is
        // hoisted above -- the grid's vertical position is derived from it too.

        // The row-label column is measured for the SAME reason (it was fixed at 42,
        // so "BAND" — the widest of the four — ellipsized first as the text scale
        // rose). The labels are laid out into rowLabelW - 4, that 4 being their gap
        // to the boxes; kRowLabelGap covers it plus air. Floored at the old 42.
        int widestRowLabel = 0;
        for (int r = 0; r < 4; ++r)
            widestRowLabel = juce::jmax(widestRowLabel,
                juce::GlyphArrangement::getStringWidthInt(bandFont, eqRowLabel[r].getText()));
        const int rowLabelW = juce::jmax(42, widestRowLabel + kRowLabelGap);

        int widestBandLine = 0;
        for (int c = 0; c < 6; ++c)
            for (int line = 0; line < 2; ++line)
                widestBandLine = juce::jmax(widestBandLine,
                    juce::GlyphArrangement::getStringWidthInt(bandFont, eqColLabel[c][line].getText()));

        const int minColW  = boxW + 8;
        const int maxColW  = juce::jmax(minColW, (midArea.getRight() - leftClusterRight - rowLabelW) / 6);
        const int bandColW = juce::jlimit(minColW, maxColW, widestBandLine + kBandLabelGap);

        const int gridLeft     = midArea.getRight() - 6 * bandColW;
        const int rowLabelLeft = gridLeft - rowLabelW;

        const int pitch = eqRowPitch;   // hoisted: the cluster shares it, as does eqTop
        const int rowY[3] = { eqTop, eqTop + pitch, eqTop + 2 * pitch };

        // Band labels: two stacked lines, pitched off the font (bandLineH, hoisted) so
        // they can't overlap as the text scale grows.
        const int bandLabelY = eqTop + 3 * pitch + 2;    // line 1; line 2 sits a bandLineH below

        // Row labels (right-justified so they sit against the boxes). "BAND" gets the
        // band labels' line height so it centres level with their FIRST line.
        eqRowLabel[EqGain].setBounds(rowLabelLeft, rowY[EqGain], rowLabelW - 4, valueH);
        eqRowLabel[EqFreq].setBounds(rowLabelLeft, rowY[EqFreq], rowLabelW - 4, valueH);
        eqRowLabel[EqQ]   .setBounds(rowLabelLeft, rowY[EqQ],    rowLabelW - 4, valueH);
        eqRowLabel[3]     .setBounds(rowLabelLeft, bandLabelY,   rowLabelW - 4, bandLineH);   // "BAND"

        // EQ BYPASS lamp, in the empty gap to the LEFT of the row labels: the dot on
        // the FREQ row (the grid's middle row, so it reads as belonging to the whole
        // block rather than to one row of it) with its caption under it on the Q row.
        // Sized to its own caption, and pushed left of the row labels by one label
        // width of air so it never crowds "FREQ".
        {
            const int capW = juce::jmax(
                36, juce::GlyphArrangement::getStringWidthInt(
                        edtext::font(edtext::Label, 11.f, juce::Font::bold),
                        eqBypLabel.getText()) + 6);
            const int lampX = rowLabelLeft - 14 - capW;
            const int s = juce::jmax(12, juce::jmin(capW, valueH));
            eqBypToggle.setBounds(juce::Rectangle<int>(lampX, rowY[EqFreq], capW, valueH)
                                      .withSizeKeepingCentre(s, s));
            eqBypLabel.setBounds(lampX, rowY[EqQ], capW, labelH);
        }

        // Grid column 0 = HPF2, columns 1..5 = bands 1..5. Boxes keep their width and
        // centre in the (wider) column, so spreading the columns adds air, not bulk.
        auto colX      = [gridLeft, bandColW](int col) { return gridLeft + col * bandColW; };
        auto centreBox = [bandColW, boxW, valueH](int cx, int y)
        { return juce::Rectangle<int>(cx + (bandColW - boxW) / 2, y, boxW, valueH); };

        if (hpf2Box) hpf2Box->setBounds(centreBox(colX(0), rowY[EqFreq]));

        for (int band = 0; band < 5; ++band)
            for (int row = 0; row < 3; ++row)
                if (eqBox[row][band])
                    eqBox[row][band]->setBounds(centreBox(colX(band + 1), rowY[row]));

        for (int c = 0; c < 6; ++c)
        {
            eqColLabel[c][0].setBounds(colX(c), bandLabelY,             bandColW, bandLineH);
            eqColLabel[c][1].setBounds(colX(c), bandLabelY + bandLineH, bandColW, bandLineH);
        }
    }

    // Main row: 16 knobs evenly divided.
    const int n = (int) mainKnobs.size();
    if (n > 0)
    {
        const int cellW = mainRow.getWidth() / n;
        int x = mainRow.getX();
        for (int i = 0; i < n; ++i)
        {
            mainKnobs[(size_t) i]->setBounds(x, mainRow.getY(), cellW, mainRow.getHeight());
            x += cellW;
        }
    }
}
