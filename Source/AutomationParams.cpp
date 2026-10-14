// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "AutomationParams.h"
#include "PluginProcessor.h"
#include "FreqLadder.h"
#include <cmath>
#include <cstring>
#include "codec/MU100AddressMap.h"
#include "codec/MU100Drums.h"          // kDrumFirstNote / kDrumLastNote
#include "codec/MU100DrumDefaults.h"   // per-key factory defaults (Standard Kit)
#include "codec/MU100Display.h"        // noteName, so a lane name cannot drift
                                       // from the keyboard's own octave labels

namespace
{
    // The DAW sees TWO names for every parameter. VST3 asks for a long one (the
    // automation list) and, separately, an <= 8-character one for narrow slots like
    // Cubase's mixer strip; JUCE answers both from AudioProcessorParameter::getName,
    // which plainly TRUNCATES — so a long name would reach those slots as "01 CUTOF"
    // / "MASTER V". Carry the short form explicitly instead. It is the parameter ID
    // as well (`VOL01`, `CUTOFF32`, `VOLMST`), which is the one string that must
    // never change: the VST3 ID is hash(id), and changing it orphans recorded lanes.
    struct MU100Parameter final : public juce::AudioParameterInt
    {
        // shortName is BOTH the parameter ID and the short display form. One string,
        // deliberately: keeping them the same is what makes "never change the ID"
        // checkable by eye against the tables below.
        MU100Parameter (const juce::String& shortName, const juce::String& longName,
                        int lo, int hi, int defaultValue,
                        juce::AudioParameterIntAttributes attrs)
            : juce::AudioParameterInt (juce::ParameterID (shortName, 1), longName,
                                       lo, hi, defaultValue, std::move (attrs)),
              shortForm (shortName), longForm (longName)
        {
            jassert (shortName.length() <= 8);
        }

        // 8 is not a guess: it is VST3's own shortTitle budget, and the only length
        // JUCE's wrapper asks for besides the full 128. Switching forms AT that
        // budget rather than "whenever the long one happens to fit" is what keeps a
        // host's narrow column consistent — otherwise 01 VOL (6 chars) would keep the
        // long form while 01 CUTOFF (9) fell back, and the strip would read half one
        // way and half the other.
        juce::String getName (int maximumStringLength) const override
        {
            return maximumStringLength <= 8
                       ? shortForm.substring (0, maximumStringLength)
                       : longForm .substring (0, maximumStringLength);
        }

        const juce::String shortForm, longForm;
    };

    // ---- value text, matching what the editor's own knobs show ----
    //
    // The lo/hi the lambdas clamp to are the parameter's OWN range, captured at
    // construction from the address map — not a hardcoded 0..127. NOTE SHIFT is
    // stored 0x28..0x58, so a shared 0..127 clamp would let a typed-in value out
    // of range and hand the device a byte it rejects.

    // Bipolar, stored with 64 = centre: HPF / RES / CUTOFF (shown -64..+63) and
    // KSHIFT (0x28..0x58, so -24..+24 semitones). Same formula, different span —
    // exactly as PartPanel's Kind::Signed and Kind::KShift do it.
    // `centre` is what reads as 0. It is 64 for everything with an XG centre-detent
    // range -- and 63 for the three COMPLEMENTED drum EG rates, because complementing
    // a byte about 127 moves the centre to 127 - 64 = 63. That is not a fudge: the
    // editor's own AMP EG readout is relative to the value the key LOADS with
    // (GraphicDisplayPanel::ampBaseline), and all 266 distinct kit rows in the
    // harvested table hold 0x40 for all three rates -- so 63 is where the node reads 0
    // on every kit and every key. Checked against the table, not assumed.
    juce::AudioParameterIntAttributes signedAttrs (int lo, int hi, int centre = 64)
    {
        return juce::AudioParameterIntAttributes()
                   .withStringFromValueFunction ([centre] (int v, int)
                   {
                       const int s = v - centre;
                       return (s > 0 ? "+" : "") + juce::String (s);
                   })
                   .withValueFromStringFunction ([lo, hi, centre] (const juce::String& t)
                   {
                       return juce::jlimit (lo, hi, centre + t.trim().getIntValue());
                   });
    }

    // XG PAN: 0 = Random, 1..63 = L63..L1, 64 = Centre, 65..127 = R1..R63.
    juce::AudioParameterIntAttributes panAttrs (int lo, int hi)
    {
        return juce::AudioParameterIntAttributes()
                   .withStringFromValueFunction ([] (int v, int)
                   {
                       if (v <= 0)  return juce::String ("RND");
                       if (v < 64)  return "L" + juce::String (64 - v);
                       if (v == 64) return juce::String ("C");
                       return "R" + juce::String (v - 64);
                   })
                   .withValueFromStringFunction ([lo, hi] (const juce::String& t)
                   {
                       const auto s = t.trim().toUpperCase();
                       if (s.startsWith ("RND")) return lo;
                       if (s == "C")             return 64;
                       if (s.startsWith ("L"))   return juce::jlimit (lo, 63,  64 - s.substring (1).getIntValue());
                       if (s.startsWith ("R"))   return juce::jlimit (65, hi,  64 + s.substring (1).getIntValue());
                       return juce::jlimit (lo, hi, s.getIntValue());
                   });
    }

    // HPF2 is a plugin-only per-part high-pass in absolute Hz, 0 = Off. It is the
    // one automated control with no XG address (see Kind::CleanHpf), so it needs its
    // own display law rather than one of the Style values below.
    //
    // Its useful range is the ladder's fine segment, 20..1000 Hz, plus Off below the
    // floor — but the parameter is a plain 0..1000 integer and is deliberately NOT
    // snapped to the editor's `freqladder::hpf2::snap` law on the way in. Snapping
    // would mean writing a value back at the host that it did not ask for, and a lane
    // ramping through the 1..19 dead zone would fight that correction every tick.
    // Those values are harmless — a 6 Hz high-pass is inaudible — and everything that
    // displays one, here and in the editor, reads anything under 20 Hz as "Off".
    constexpr int kHpf2Max = 1000;

    juce::AudioParameterIntAttributes hpf2Attrs()
    {
        return juce::AudioParameterIntAttributes()
                   .withStringFromValueFunction ([] (int v, int)
                   {
                       return v < freqladder::kMin ? juce::String ("Off")
                                                   : freqladder::text ((double) v);
                   })
                   .withValueFromStringFunction ([] (const juce::String& t)
                   {
                       const auto s = t.trim();
                       if (s.isEmpty() || s.equalsIgnoreCase ("off")) return 0;
                       return juce::jlimit (0, kHpf2Max,
                                            (int) std::lround (freqladder::parse (s)));
                   });
    }

    // The five EQ band GAINs, hardware and plugin alike: raw 0..127 with 64 = 0 dB
    // and 12/64 dB a step, i.e. ±12 dB, shown to one decimal with a "dB" suffix.
    // Exactly what PartPanel::buildEqGrid puts in the boxes — the mid bands were
    // deliberately given the hardware shelves' step rather than the Multi EQ's 1 dB,
    // so one law covers all five.
    juce::AudioParameterIntAttributes gainDbAttrs (int lo, int hi)
    {
        return juce::AudioParameterIntAttributes()
                   .withStringFromValueFunction ([] (int v, int)
                   {
                       return juce::String ((v - 64) * (12.0 / 64.0), 1) + "dB";
                   })
                   .withValueFromStringFunction ([lo, hi] (const juce::String& t)
                   {
                       const double db = t.upToFirstOccurrenceOf ("d", false, true).getDoubleValue();
                       return juce::jlimit (lo, hi, (int) std::lround (64.0 + db * (64.0 / 12.0)));
                   });
    }

    enum class Style { Plain, Signed64, Pan, GainDb };

    // Where a per-part parameter's value lives. Most shadow an XG address; the two
    // plugin-only kinds have none, and are read/written through the processor.
    enum class Src { Xg, CleanHpf, PartEqGain, DrumHpf };

    // The per-part controls, in the Part module's own order — the knob row first
    // (VOL..DRY), then HPF2 from the EQ grid beneath it. That is the order the DAW
    // lists them in, so a flat parameter list reads as one channel strip per part.
    //
    // `prefix` + the two-digit part number is the parameter ID and its <= 8-character
    // SHORT name (CUTOFF32, CHORUS32, KSHIFT32, HPF232, EQ3GN12). Elide only as far as
    // legibility allows: EQ1GN01 spends its seventh character buying "GN" over a bare
    // "G", because EQ1G01 is three numerals in six characters and reads as ambiguously
    // to a person as it hashes unambiguously to a machine. The name the DAW LISTS is
    // built the other way round — "01 CUTOFF" — so the part number reads and sorts
    // first; see MU100Parameter.
    //
    // No range here for the Xg rows: it comes from the address map (rangeLo/rangeHi),
    // so it cannot drift from the parameter it describes. The plugin-only rows carry
    // no paramName (there is no address to name) and get their range in the add
    // helper instead — 0..kHpf2Max for HPF2, 0..127 for an EQ gain. `pluginBand` is
    // the peaking-band index 0..2 that PartEqGain addresses, and is ignored otherwise.
    //
    // The five EQ gains are a MIXED row: bands 1 and 5 are the hardware XG shelves,
    // bands 2-4 are the plugin's own peaking bands. Same range, same display, same
    // marker — different owners, which is exactly why Src is per-row and not per-table.
    struct PartSpec
    {
        const char* prefix;
        const char* name;           // what the DAW lists, after the part number
        Src         src;
        const char* paramName;      // MULTI PART parameter name (Src::Xg only)
        int         pluginBand;     // peaking band 0..2 (Src::PartEqGain only)
        int         defaultRaw;
        Style       style;
    };

    const PartSpec kPartSpecs[] =
    {
        { "VOL",    "VOL",     Src::Xg,         "VOLUME",                            -1, 100, Style::Plain    },
        { "KSHIFT", "KSHIFT",  Src::Xg,         "NOTE SHIFT",                        -1,  64, Style::Signed64 },
        { "PAN",    "PAN",     Src::Xg,         "PAN",                               -1,  64, Style::Pan      },
        { "HPF",    "HPF",     Src::Xg,         "HIGH PASS FILTER CUTOFF FREQUENCY", -1,  64, Style::Signed64 },
        { "RES",    "RES",     Src::Xg,         "LOW PASS FILTER RESONANCE",         -1,  64, Style::Signed64 },
        { "CUTOFF", "CUTOFF",  Src::Xg,         "LOW PASS FILTER CUTOFF FREQUENCY",  -1,  64, Style::Signed64 },
        { "REV",    "REV",     Src::Xg,         "REVERB SEND",                       -1,  40, Style::Plain    },
        { "CHORUS", "CHORUS",  Src::Xg,         "CHORUS SEND",                       -1,   0, Style::Plain    },
        { "VAR",    "VAR",     Src::Xg,         "VARIATION SEND",                    -1,   0, Style::Plain    },
        { "DRY",    "DRY",     Src::Xg,         "DRY LEVEL",                         -1, 127, Style::Plain    },
        { "HPF2",   "HPF2",    Src::CleanHpf,   nullptr,                             -1,   0, Style::Plain    },
        { "EQ1GN",  "EQ1GAIN", Src::Xg,         "EQ BASS GAIN",                      -1,  64, Style::GainDb   },
        { "EQ2GN",  "EQ2GAIN", Src::PartEqGain, nullptr,                              0,  64, Style::GainDb   },
        { "EQ3GN",  "EQ3GAIN", Src::PartEqGain, nullptr,                              1,  64, Style::GainDb   },
        { "EQ4GN",  "EQ4GAIN", Src::PartEqGain, nullptr,                              2,  64, Style::GainDb   },
        { "EQ5GN",  "EQ5GAIN", Src::Xg,         "EQ TREBLE GAIN",                    -1,  64, Style::GainDb   },
    };

    // The system-wide trio, in the System module's own knob order — a table for the
    // same reason kPartSpecs is one: it is also what AutomationParams::isAutomated
    // answers from, so the editor's markers cannot name a parameter the DAW does
    // not have. The ID scheme puts "MST" in the last three characters (part
    // parameters take a two-digit part number instead) — hence KSHIFMST, elided to
    // stay inside the 8-character budget. The listed NAME instead begins with the
    // word MASTER, matching the leading part number the part parameters get.
    //
    // The System module's KSHIFT knob drives XG SYSTEM TRANSPOSE, which shares NOTE
    // SHIFT's 0x28..0x58 / -24..+24 semitone range rather than the usual 0..127.
    struct SystemSpec { const char* id; const char* name; const char* paramName; int defaultRaw; Style style; };

    const SystemSpec kSystemSpecs[] =
    {
        { "VOLMST",   "VOL",    "MASTER VOLUME",     127, Style::Plain    },
        { "ATTMST",   "ATT",    "MASTER ATTENUATOR",   0, Style::Plain    },
        { "KSHIFMST", "KSHIFT", "TRANSPOSE",          64, Style::Signed64 },
    };

    // ---- the DRUM SETUP key parameters -------------------------------------------
    //
    // The same idea as kPartSpecs, for one key of one DRUMS buffer. Registered as a
    // full cross product: kNumDrumBuffers x every key the device stores x this table.
    // The whole span is deliberate -- it is what keeps this from ever needing to be
    // widened, and an unused lane costs no wire time (see the header's note on what
    // the scarce resource actually is).
    //
    // ORDER mirrors the Part module: the knob row it rebinds in drum-edit mode
    // (VOL..VAR, minus DRY which has no drum equivalent, and minus the two Rcv
    // switches, which are state rather than something you ride), then HPF2 from the EQ grid
    // beneath it, then the three AMP EG rates -- which are NOT on the knob row at all
    // (they are edited from the Graphic Display's AMP EG screen), so they go last
    // rather than pretending to a position in a row they do not appear in.
    //
    // `code` + the buffer digit + the two-digit KEY is the parameter ID and its
    // <= 8-character short name: D158VOL, D113DC1. The key is a MIDI NOTE NUMBER and
    // not a note name for a reason that is not budget -- a note name is 2 to 4
    // characters wide (G5 .. C#-1, since the span starts at note 13), so names would
    // give IDs a ragged width and push the longest past eight. The number also cannot
    // be wrong: it is the byte the SysEx address is literally built from, whereas a
    // note NAME depends on an octave convention that differs between DAWs.
    //
    // The name the DAW LISTS is "DR1 A#2 58 VOL" -- buffer, note name, note number,
    // then the control. Both forms of the key, because the letter is what the user
    // reads off the plugin's keyboard and the number is the only form every host
    // agrees on. Longest is "DR1 C#−1 13 DECAY1" at 18 characters -- that minus is
    // U+2212, not ASCII, because Cubase's lane label cuts names at an ASCII '-' (see
    // the note name below).
    //
    // No range here, for the same reason kPartSpecs carries none: it comes from the
    // address map. No DEFAULT either -- that is per-key, and drumStandardDefault
    // reads it from the harvested factory table instead of restating it.
    // `complement` inverts the byte about 127 in BOTH directions, for the three AMP EG
    // entries and nothing else. DRUM SETUP stores those as RATES (higher = faster =
    // shorter) while every surface in this plugin presents them as TIMES -- the AMP EG
    // screen is drawn time-wise, and GraphicDisplayPanel's loadDrumAmp / sendAmp have
    // always complemented for exactly that reason. A lane that did not complement moved
    // the node the wrong way under a DAW modulator (USER-REPORTED 2026-09-18).
    // MULTI PART's envelope parameters are TIMES already, so this is a DRUM SETUP
    // quirk and not a general one -- which is why it is a per-ROW flag.
    struct DrumSpec
    {
        const char* code;           // ID fragment, <= 4 chars
        const char* name;           // what the DAW lists, after the key
        Src         src;
        const char* paramName;      // DRUM SETUP parameter name (Src::Xg only)
        Style       style;
        bool        complement;     // rate <-> time (the AMP EG trio only)
    };

    const DrumSpec kDrumSpecs[] =
    {
        { "VOL", "VOL",    Src::Xg,      "LEVEL",                             Style::Plain,    false },
        { "DTN", "DETUNE", Src::Xg,      "PITCH FINE",                        Style::Signed64, false },
        { "KSH", "KSHIFT", Src::Xg,      "PITCH COARSE",                      Style::Signed64, false },
        { "PAN", "PAN",    Src::Xg,      "PAN",                               Style::Pan,      false },
        { "HPF", "HPF",    Src::Xg,      "HIGH PASS FILTER CUTOFF FREQUENCY", Style::Signed64, false },
        { "RES", "RES",    Src::Xg,      "LOW PASS FILTER RESONANCE",         Style::Signed64, false },
        { "CUT", "CUTOFF", Src::Xg,      "LOW PASS FILTER CUTOFF FREQUENCY",  Style::Signed64, false },
        { "REV", "REV",    Src::Xg,      "REVERB SEND",                       Style::Plain,    false },
        { "CHO", "CHORUS", Src::Xg,      "CHORUS SEND",                       Style::Plain,    false },
        { "VAR", "VAR",    Src::Xg,      "VARIATION SEND",                    Style::Plain,    false },
        { "HP2", "HPF2",   Src::DrumHpf, nullptr,                             Style::Plain,    false },
        { "ATK", "ATTACK", Src::Xg,      "EG ATTACK RATE",                    Style::Signed64, true  },
        { "DC1", "DECAY1", Src::Xg,      "EG DECAY1 RATE",                    Style::Signed64, true  },
        { "DC2", "DECAY2", Src::Xg,      "EG DECAY2 RATE",                    Style::Signed64, true  },
    };

    // What a drum-key parameter's host DEFAULT should be.
    //
    // A part parameter's default is one number for all 32 parts; a drum key's is not
    // -- it is per key AND per kit, and the kit is not known at construction (VST3
    // fixes the parameter list before anything has booted or loaded). So this answers
    // with the STANDARD KIT's value for that key, read out of the harvested factory
    // table rather than transcribed: it is the kit a drum part comes up on, and it is
    // per-key exact, which a single neutral constant could not be.
    //
    // This is cosmetic in normal use -- start() resyncs every parameter from the model
    // before the first tick, so the host sees real values as soon as there are any.
    // It is what the host's "reset to default" lands on, and what it shows in the
    // window between construction and boot.
    int drumStandardDefault (const mu100::ParamDef& def, int note, int fallback)
    {
        // 127/0/1 = Standard Kit. `def.lo.base` is the DRUM SETUP low address: the
        // low byte is NOT templated in this table (only 3n and rr are), so the base
        // IS the address, and taking it from the ParamDef keeps this from carrying a
        // second copy of the address map.
        const uint8_t* row = mu100::drumDefaultsFor (127, 0, 1, note);
        const int      off = mu100::drumDefaultOffsetForLow (def.lo.base);

        if (row == nullptr || off < 0)
            return fallback;   // a gap in the stored block, or a kit table that moved

        return (int) row[off];
    }
}

bool AutomationParams::isAutomated (const char* table, const char* paramName)
{
    if (table == nullptr || paramName == nullptr) return false;

    auto same = [] (const char* a, const char* b) { return std::strcmp (a, b) == 0; };

    if (same (table, "MULTI PART"))
        for (const auto& s : kPartSpecs)
            if (s.paramName != nullptr && same (paramName, s.paramName)) return true;

    if (same (table, "XG SYSTEM"))
        for (const auto& s : kSystemSpecs)
            if (same (paramName, s.paramName)) return true;

    // A drum KEY parameter. No key or buffer in the question: the lanes are a
    // complete cross product, so the answer cannot depend on which one is being
    // edited -- and a marker that had to be told the key would be one more thing
    // that could disagree with the parameter set.
    if (same (table, "DRUM SETUP"))
        for (const auto& s : kDrumSpecs)
            if (s.paramName != nullptr && same (paramName, s.paramName)) return true;

    // The plugin-only controls, which have no address-map table. Matched against the
    // LISTED NAME of the kPartSpecs row the parameter is built from — the name is the
    // human-facing label, which is what a panel asks with; the prefix is an internal
    // ID that has to be elided to fit eight characters (EQ2GN). Still the same table,
    // so a marker cannot outlive its parameter.
    // HPF2 matches here from kPartSpecs and would match from kDrumSpecs too: the
    // part box and the drum-key box the same control retargets to are both backed
    // by a parameter now, and they share the control's NAME. Checking both tables
    // keeps that true if either side ever drops it.
    if (same (table, "PLUGIN"))
    {
        for (const auto& s : kPartSpecs)
            if (s.paramName == nullptr && same (paramName, s.name)) return true;

        for (const auto& s : kDrumSpecs)
            if (s.paramName == nullptr && same (paramName, s.name)) return true;
    }

    return false;
}

// The Part module marks the EQ grid's GAIN ROW rather than each of its five boxes,
// so it needs one answer for the row. Derived from the same table as everything
// else: if any of the five stops being a host parameter, the row stops being marked.
bool AutomationParams::isEqGainRowAutomated()
{
    static const char* const kRow[] = { "EQ1GAIN", "EQ2GAIN", "EQ3GAIN", "EQ4GAIN", "EQ5GAIN" };

    for (const char* wanted : kRow)
    {
        bool found = false;
        for (const auto& s : kPartSpecs)
            if (std::strcmp (s.name, wanted) == 0) { found = true; break; }

        if (! found) return false;
    }

    return true;
}

//==============================================================================
void AutomationParams::add (juce::AudioProcessorParameterGroup& group,
                            const juce::String& shortName, const juce::String& name,
                            const char* table, const char* paramName, int instance,
                            int defaultRaw, int style, int instance2, bool complement)
{
    const int mapIndex = mu100::AddressMap::find (paramName, table);
    if (mapIndex < 0)
    {
        // A renamed parameter in the regenerated address map would silently drop
        // the automation parameter; say so rather than shipping a hole.
        jassertfalse;
        return;
    }

    const auto& def = mu100::AddressMap::param (mapIndex);

    Entry e;
    e.mapIndex  = mapIndex;
    e.instance  = instance;
    e.instance2 = instance2;
    mu100::AddressMap::resolve (def, instance, instance2, e.model, e.addr21);

    // Range comes from the address map, so it cannot drift from the parameter.
    // Most of these are 0..127, but NOTE SHIFT is 0x28..0x58 — a shared 0..127
    // would offer the host values the device rejects.
    e.lo = (int) def.rangeLo;
    e.hi = (int) def.rangeHi;
    jassert (e.lo < e.hi && defaultRaw >= e.lo && defaultRaw <= e.hi);

    // Every parameter here is a single 7-bit byte. If one ever isn't, the raw
    // <-> parameter mapping below would be wrong, so assert rather than guess.
    jassert (def.size == 1);

    e.complement = complement;

    // A complemented entry's centre moves with it -- see signedAttrs.
    auto attrs = style == (int) Style::Pan      ? panAttrs (e.lo, e.hi)
               : style == (int) Style::Signed64 ? signedAttrs (e.lo, e.hi,
                                                              complement ? 127 - 64 : 64)
               : style == (int) Style::GainDb   ? gainDbAttrs (e.lo, e.hi)
                                                : juce::AudioParameterIntAttributes();

    auto owned = std::make_unique<MU100Parameter> (shortName, name,
                                                   e.lo, e.hi, defaultRaw, std::move (attrs));
    e.param    = owned.get();
    e.lastSeen = defaultRaw;
    group.addChild (std::move (owned));
    entries.push_back (e);
}

// The plugin-only per-part HPF2. Everything the XG path resolves from the address
// map is fixed here instead — there is no address to resolve — and the value is
// read from / written to the processor rather than the editor model.
void AutomationParams::addCleanHpf (juce::AudioProcessorParameterGroup& group,
                                    const juce::String& shortName,
                                    const juce::String& name, int part)
{
    Entry e;
    e.kind     = Kind::CleanHpf;
    e.instance = part;
    e.lo       = 0;
    e.hi       = kHpf2Max;

    auto owned = std::make_unique<MU100Parameter> (shortName, name,
                                                   e.lo, e.hi, 0, hpf2Attrs());
    e.param    = owned.get();
    e.lastSeen = 0;
    group.addChild (std::move (owned));
    entries.push_back (e);
}

// The plugin-only per-(buffer, key) HPF2 -- the drum-edit counterpart of
// addCleanHpf above, and the same shape: no address to resolve, no wire time, and
// live before the engine has booted (setDrumHpf is sticky through its own dirty
// flag, exactly as the part one is). The only difference is where the value lives:
// (buffer, key) rather than part, so it is SHARED by every part on that buffer --
// which is the hardware's own DRUM SETUP semantics, not a simplification here.
void AutomationParams::addDrumHpf (juce::AudioProcessorParameterGroup& group,
                                   const juce::String& shortName,
                                   const juce::String& name, int buffer, int key)
{
    Entry e;
    e.kind      = Kind::DrumHpf;
    e.instance  = key;
    e.instance2 = buffer;
    e.lo        = 0;
    e.hi        = kHpf2Max;

    auto owned = std::make_unique<MU100Parameter> (shortName, name,
                                                   e.lo, e.hi, 0, hpf2Attrs());
    e.param    = owned.get();
    e.lastSeen = 0;
    group.addChild (std::move (owned));
    entries.push_back (e);
}

// One of the three PLUGIN-only peaking bands' GAIN (EQ grid columns 2-4). Bands 1
// and 5 in the same row are hardware XG shelves and go through add() above; only
// the middle three land here. Same range, default and display as the shelves — see
// gainDbAttrs.
void AutomationParams::addPartEqGain (juce::AudioProcessorParameterGroup& group,
                                      const juce::String& shortName,
                                      const juce::String& name, int part, int band)
{
    jassert (band >= 0 && band < 3);

    Entry e;
    e.kind     = Kind::PartEqGain;
    e.instance = part;
    e.band     = band;
    e.lo       = 0;
    e.hi       = 127;

    auto owned = std::make_unique<MU100Parameter> (shortName, name,
                                                   e.lo, e.hi, 64, gainDbAttrs (e.lo, e.hi));
    e.param    = owned.get();
    e.lastSeen = 64;
    group.addChild (std::move (owned));
    entries.push_back (e);
}

void AutomationParams::createParameters()
{
    jassert (entries.empty());
    constexpr int numPartSpecs   = (int) (sizeof (kPartSpecs)   / sizeof (kPartSpecs[0]));
    constexpr int numSystemSpecs = (int) (sizeof (kSystemSpecs) / sizeof (kSystemSpecs[0]));
    constexpr int numDrumSpecs   = (int) (sizeof (kDrumSpecs)   / sizeof (kDrumSpecs[0]));
    constexpr int numDrumKeys    = mu100::kDrumLastNote - mu100::kDrumFirstNote + 1;

    entries.reserve ((size_t) (numSystemSpecs + kNumParts * numPartSpecs
                               + kNumDrumBuffers * numDrumKeys * numDrumSpecs));

    // One AudioProcessorParameterGroup per part, plus MASTER. JUCE turns each into a
    // VST3 UNIT, which is what gives a host with a two-level parameter picker (Cubase,
    // Studio One) a folder to open instead of one flat list 355 entries long. Hosts
    // that ignore units are not left worse off: the part number is in the parameter
    // NAME too, so the flat list still sorts and reads part-first.
    //
    // Grouping does NOT touch parameter identity — the VST3 ID is hash(paramID), and
    // the IDs (VOL01 …) are unchanged — so automation recorded before this change
    // still lands on the same parameters.
    //
    // Order is preserved from the flat version (MASTER, then part 1..32, each in the
    // Part module's own control order), because a host that stores automation by
    // parameter INDEX rather than by ID would otherwise shift every lane.
    auto master = std::make_unique<juce::AudioProcessorParameterGroup> ("MASTER", "MASTER", " ");

    for (const auto& s : kSystemSpecs)
        add (*master, s.id, juce::String ("MASTER ") + s.name,
             "XG SYSTEM", s.paramName, 0, s.defaultRaw, (int) s.style);

    owner.addParameterGroup (std::move (master));

    for (int part = 0; part < kNumParts; ++part)
    {
        const juce::String number = juce::String (part + 1).paddedLeft ('0', 2);
        auto group = std::make_unique<juce::AudioProcessorParameterGroup> (
                         "PART" + number, "PART " + number, " ");

        for (const auto& s : kPartSpecs)
        {
            const juce::String shortName = s.prefix + number;
            const juce::String name      = number + " " + s.name;

            switch (s.src)
            {
                case Src::Xg:
                    add (*group, shortName, name, "MULTI PART", s.paramName, part,
                         s.defaultRaw, (int) s.style);
                    break;

                case Src::CleanHpf:
                    addCleanHpf (*group, shortName, name, part);
                    break;

                case Src::PartEqGain:
                    addPartEqGain (*group, shortName, name, part, s.pluginBand);
                    break;
            }
        }

        owner.addParameterGroup (std::move (group));
    }

    // ---- the DRUMS buffers -------------------------------------------------------
    //
    // APPENDED, after every part. Identity is hash(paramID) so position does not
    // affect it, but a host that stores automation by parameter INDEX would have
    // every existing lane shifted by an insertion -- so new parameters go on the end,
    // and the 515 that were here before keep indices 0..514.
    //
    // THREE levels of group, which JUCE turns into nested VST3 units (it fills
    // ParameterInfo::parentUnitId from the group's parent): DRUMS1..4, then one per
    // KEY, then the controls. That is what keeps a host's parameter picker to four
    // folders at the top rather than one list 4939 entries long. Hosts that ignore
    // units are not left stranded: buffer, note name and note number are all in the
    // parameter NAME, so the flat list still sorts buffer-first and key-in-order.
    //
    // The key group is named with BOTH forms of the key for the same reason the
    // parameter is -- browsing a picker in a host whose own octave convention is not
    // this one, the number is the part that cannot be off by an octave.
    for (int buffer = 0; buffer < kNumDrumBuffers; ++buffer)
    {
        const juce::String bufNum (buffer + 1);
        auto bufGroup = std::make_unique<juce::AudioProcessorParameterGroup> (
                            "DRUMS" + bufNum, "DRUMS" + bufNum, " ");

        for (int note = mu100::kDrumFirstNote; note <= mu100::kDrumLastNote; ++note)
        {
            // One source for the note name, shared with the editor's keyboard octave
            // labels and the drum key strip, so a lane cannot come to disagree with
            // what the plugin shows on screen.
            //
            // EXCEPT the octave-minus, which becomes U+2212 MINUS SIGN here and only
            // here. Cubase's automation LANE label rewrites an ASCII '-' as " - " (its
            // own "Plugin - Parameter" separator) and, when the name is too long, cuts
            // at it and keeps only the tail: "DR1 A#-1 22 CUTOFF" showed as
            // "1 22 CUTOFF" (USER-REPORTED 2026-09-25; the Add Parameter dialog is
            // unaffected). Only keys 13..23 (C#-1..B-1) carry one. Safe to change:
            // lanes bind to the parameter ID, never to this name.
            const juce::String noteText = juce::String (mu100::display::noteName (note).c_str())
                                              .replace ("-", juce::String::charToString (0x2212));
            const juce::String keyText  = noteText + " " + juce::String (note);

            auto keyGroup = std::make_unique<juce::AudioProcessorParameterGroup> (
                                "DR" + bufNum + "K" + juce::String (note), keyText, " ");

            for (const auto& d : kDrumSpecs)
            {
                const juce::String shortName = "D" + bufNum + juce::String (note) + d.code;
                const juce::String name      = "DR" + bufNum + " " + keyText + " " + d.name;

                if (d.src == Src::DrumHpf)
                {
                    addDrumHpf (*keyGroup, shortName, name, buffer, note);
                    continue;
                }

                // instance = the KEY, instance2 = the BUFFER: AddressMap resolves the
                // least-significant templated byte (rr) from the first and the next
                // (3n) from the second.
                const int mapIndex = mu100::AddressMap::find (d.paramName, "DRUM SETUP");
                int       dflt     = mapIndex < 0
                                         ? 64
                                         : drumStandardDefault (mu100::AddressMap::param (mapIndex),
                                                                note, 64);

                // The table holds a RATE and the parameter holds a TIME, so the default
                // crosses the same boundary every other value does.
                if (d.complement) dflt = 127 - dflt;

                add (*keyGroup, shortName, name, "DRUM SETUP", d.paramName, note,
                     dflt, (int) d.style, buffer, d.complement);
            }

            bufGroup->addChild (std::move (keyGroup));
        }

        owner.addParameterGroup (std::move (bufGroup));
    }
}

//==============================================================================
void AutomationParams::start()
{
    if (! enabled)
        return;

    resyncFromModel();
    startTimerHz (kTickHz);
}

void AutomationParams::setEnabled (bool on)
{
    if (on == enabled)
        return;

    enabled = on;

    if (on) start();   // resyncs first -- see the header
    else    stop();
}

void AutomationParams::stop()
{
    stopTimer();

    // Leave no gesture open — a host that never sees the matching end can latch
    // a parameter into "being touched" for the rest of the session.
    for (auto& e : entries)
        if (e.gestureIdle >= 0)
        {
            e.param->endChangeGesture();
            e.gestureIdle = -1;
        }
}

// The one place that knows where an entry's value actually lives. Clamped to the
// parameter's own range in BOTH cases: the model can legitimately hold a byte
// outside it (a .syx from other gear), and setValueNotifyingHost with an
// out-of-range value would normalise to a different number and read back as a
// change we would then transmit.
bool AutomationParams::readCurrent (const Entry& e, int& valueOut) const
{
    if (e.kind == Kind::CleanHpf)
    {
        valueOut = juce::jlimit (e.lo, e.hi,
                                 (int) std::lround (owner.getCleanHpfHz (e.instance)));
        return true;
    }

    if (e.kind == Kind::DrumHpf)
    {
        valueOut = juce::jlimit (e.lo, e.hi,
                                 (int) std::lround (owner.getDrumHpf (e.instance2, e.instance)));
        return true;
    }

    if (e.kind == Kind::PartEqGain)
    {
        valueOut = juce::jlimit (e.lo, e.hi, owner.getPartEqGain (e.instance, e.band));
        return true;
    }

    uint8_t raw = 0;
    if (! owner.getEditorState().getByte (e.model, e.addr21, raw))
        return false;                          // model has no value here

    // Rate -> time for the AMP EG trio, identity for everything else. Complemented
    // BEFORE the clamp, so the clamp still describes the value the host will see.
    const int v = e.complement ? 127 - (int) raw : (int) raw;

    valueOut = juce::jlimit (e.lo, e.hi, v);
    return true;
}

void AutomationParams::resyncFromModel()
{
    // Callers (a file load, a read-back, undo) do not know about the switch. While it
    // is off we must not write host parameters at all, and there is nothing to keep in
    // step: setEnabled(true) resyncs before the first tick.
    if (! enabled)
        return;

    for (auto& e : entries)
    {
        int v = 0;
        if (! readCurrent (e, v))
            continue;                          // nothing to read; keep ours

        if (v == e.lastSeen && v == e.param->get())
            continue;

        e.lastSeen = v;
        e.param->setValueNotifyingHost (e.param->convertTo0to1 ((float) v));
    }
}

void AutomationParams::endGestureIfIdle (Entry& e)
{
    if (e.gestureIdle >= 0 && --e.gestureIdle < 0)
        e.param->endChangeGesture();
}

void AutomationParams::timerCallback()
{
    // One check per tick, not per parameter: skipping the whole tick is what keeps a
    // 355-parameter sweep from piling messages onto a wire that is already behind.
    //
    // The signal is the RING BACKLOG, not the send-queue lead: automation is stamped
    // at the clock and never touches the pacing cursor, so that cursor says nothing
    // about it. The backlog also counts the host's own note bytes, which is what we
    // actually must not get in the way of.
    //
    // "Not booted yet" is folded into the same condition rather than returning
    // early, because the model -> host mirroring below must keep running while the
    // engine comes up (that is when the factory defaults are seeded, and the host
    // should see them). Holding lastSeen back means a host value set before the
    // engine exists is not lost — it goes out on the first tick after boot.
    const bool wireBusy = ! owner.mameIsFullyBooted.load(std::memory_order_acquire)
                       || owner.midiRingBacklog(0) > kMaxBacklogBytes;

    // How many entries may be mirrored OUT to the host in one tick. Nothing in
    // ordinary use comes close -- a knob drag moves one, a lane a handful -- so this
    // does not slow the normal path down; it exists for the one event that moves
    // hundreds at once. Assigning a kit to a DRUMS buffer rewrites that buffer's
    // whole 3n block, which is ~700 drum lanes differing in a single tick, and a DAW
    // with lanes armed in Write/Touch would record every one of them.
    //
    // Entries over the cap are simply left for the next tick: lastSeen is untouched,
    // so they still differ and are picked up ~33 ms later. A full 4623-entry sweep
    // drains in well under a second, and the direction that actually reaches the
    // device has its own, separate gate (wireBusy).
    constexpr int kMaxMirrorPerTick = 96;
    int mirrored = 0;


    for (auto& e : entries)
    {
        const int host = e.param->get();

        if (host != e.lastSeen)
        {
            // The host moved it (automation playback, a generic-editor slider, a
            // control surface). Under wire pressure, leave lastSeen alone and try
            // again next tick — these are idempotent absolute writes, so the only
            // cost of dropping an intermediate value is resolution, and the value
            // the automation settles on still lands.
            //
            // A plugin-only parameter is exempt: it never reaches the MIDI wire, so
            // it neither adds to the backlog nor has to wait for the engine to boot
            // (setCleanHpfHz is sticky — cleanHpfDirty re-pushes it after any boot).
            if (wireBusy && e.kind == Kind::XgAddress)
                continue;

            e.lastSeen = host;

            if (e.kind != Kind::XgAddress)
            {
                // Suppressed from the undo stack for the same reason the SysEx path
                // below is: automation playback is the host replaying its own data.
                YamahaMU100AudioProcessor::UndoSuppress noUndo (owner);

                if      (e.kind == Kind::CleanHpf) owner.setCleanHpfHz (e.instance, (float) host);
                else if (e.kind == Kind::DrumHpf)  owner.setDrumHpf    (e.instance2, e.instance, (float) host);
                else                               owner.setPartEqGain (e.instance, e.band, host);

                editorNotifyPending = true;
                continue;
            }

            // The ordinary editor path. It used to be bypassed because it funnelled
            // into sendSysEx's 50 ms-ahead pacing, which scattered note timing; now
            // every small send is clock-stamped (see smallSendStartTime), so automation
            // and the editor's own knobs can share one funnel again. Sharing it is
            // better than not: a knob and an automation lane on the SAME address
            // coalesce into one message per tick instead of both firing.
            // Suppressed from the undo stack. The editor's Undo covers edits the USER
            // made in the editor; automation playback is the host replaying its own
            // data, and letting a moving lane fill the stack would push every real
            // edit out of it within a second of pressing play.
            {
                YamahaMU100AudioProcessor::UndoSuppress noUndo (owner);
                // ...and time -> rate on the way back out, the mirror of readCurrent.
                const int wire = e.complement ? 127 - host : host;

                owner.sendParamChangeThrottled (
                    mu100::AddressMap::makeParamChange (mu100::AddressMap::param (e.mapIndex),
                                                        e.instance, (uint32_t) wire,
                                                        e.instance2));
            }

            // sendParamChangeThrottled updates the model but does NOT announce it —
            // it is the path the editor's own knobs use, and they already show what
            // they just sent. A host-driven change has no such originator, so
            // without this the Part panel would sit still while automation moved
            // the device underneath it. Announced on a divider; see kEditorNotifyTicks.
            editorNotifyPending = true;
            continue;
        }

        // Host side is unchanged, so anything new on our side came from the editor,
        // a file load, a read-back or received SysEx — mirror it out.
        int modelValue = 0;
        if (! readCurrent (e, modelValue))
        {
            endGestureIfIdle (e);
            continue;
        }

        if (modelValue == e.lastSeen)
        {
            endGestureIfIdle (e);
            continue;
        }

        if (++mirrored > kMaxMirrorPerTick)
            continue;          // over the cap; lastSeen untouched, so retried next tick

        if (e.gestureIdle < 0)
            e.param->beginChangeGesture();
        e.gestureIdle = kGestureHoldTicks;

        e.lastSeen = modelValue;
        e.param->setValueNotifyingHost (e.param->convertTo0to1 ((float) modelValue));
    }


    if (editorNotifyPending && --editorNotifyCountdown <= 0)
    {
        editorNotifyPending  = false;
        editorNotifyCountdown = kEditorNotifyTicks;
        owner.editorStateExternalSeq.fetch_add (1, std::memory_order_release);
    }
}
