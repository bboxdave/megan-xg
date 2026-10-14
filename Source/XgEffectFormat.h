// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "codec/MU100EffectConv.h"
#include <cmath>

//==============================================================================
// XgEffectFormat — value-display formatters + ranges for XG effect parameters,
// so the Effect Parameters knobs read like XGEdit (e.g. "1.0k" Hz, "+3dB",
// "248.0ms", "mono") and span the correct raw min..max instead of raw 0..127.
//
// Fully data-driven from codec/MU100EffectConv.h, which is generated from the
// MU100/XG MIDI spec's per-effect parameter tables (mu100editor pipeline:
// build_effect_conv.py -> map_effect_conv.py -> gen_effect_conv.py). Each
// (layout id, slot) carries a conversion: a raw->display table (table#N), a
// linear mapping with a unit, an enum list, a pan, or plain raw. This replaces
// the earlier hand-coded frequency/EQ-gain-only subset.
//==============================================================================
namespace xgfmt
{
    inline const mu100fx::SlotConv& slotConv (int layoutId, int slot)
    {
        layoutId = juce::jlimit (0, mu100fx::kLayoutConvCount - 1, layoutId);
        slot     = juce::jlimit (0, 15, slot);
        return mu100fx::kLayoutConv[layoutId].slots[(size_t) slot];
    }

    // True when the MU100 MIDI spec actually defines a parameter at this
    // (layout, slot). Some XGEdit-exe-derived layouts (MU100Effects.h) carry a
    // label in slots the spec leaves blank (e.g. DELAY HPF/LPF, CHORUS AMDEP,
    // PHASER P.DIFF, NOISE GATE RATIO, PITCH-CHANGE EQ, 2-BAND mid-EQ); XGEdit
    // renders those as blank discs. Used to hide them and skip their defaults.
    inline bool hasSpecParam (int layoutId, int slot)
    {
        return slotConv (layoutId, slot).kind != mu100fx::CK_NONE;
    }

    // Whether this knob slot should be shown/sent for the current block. False
    // for spec-blank slots (CK_NONE) and for Variation-block-only params (e.g.
    // chorus/flanger/symphonic mid-EQ) when the effect is NOT in the Variation
    // block — those addresses don't exist in the Chorus/Insertion block, so the
    // knob is hidden there (matching XGEdit) and not seeded with a default.
    inline bool slotActive (int layoutId, int slot, bool isVariationUnit)
    {
        const auto& sc = slotConv (layoutId, slot);
        if (sc.kind == mu100fx::CK_NONE)  return false;
        if (sc.varOnly && ! isVariationUnit) return false;
        return true;
    }

    // The raw + display range for a slot IN A GIVEN BLOCK. A few params are
    // wider as a Variation than in the Chorus/Insertion blocks -- PHASER1's
    // STAGE is 4..12 vs 4..6 -- and the device silently clamps rather than
    // rejecting, so a knob built from the unconditional rawMin/rawMax lets you
    // dial values that never arrive. Every range consumer must come through
    // here; reading sc.rawMin/rawMax directly is what this replaces.
    struct SlotRange { int lo, hi; double dispLo, dispHi; };

    inline SlotRange rangeFor (const mu100fx::SlotConv& sc, bool isVariationUnit)
    {
        if (! isVariationUnit && sc.altRawMax > sc.altRawMin)
            return { sc.altRawMin, sc.altRawMax, (double) sc.altDispMin, (double) sc.altDispMax };
        return { sc.rawMin, sc.rawMax, (double) sc.dispMin, (double) sc.dispMax };
    }

    inline SlotRange rangeFor (int layoutId, int slot, bool isVariationUnit)
    {
        return rangeFor (slotConv (layoutId, slot), isVariationUnit);
    }

    // Parse a numeric token that may carry a 'k'/'M' suffix ("1.0k" -> 1000).
    inline double parseScaled (const juce::String& s)
    {
        auto x = s.trim().toLowerCase();
        double v = x.retainCharacters ("0123456789.-+").getDoubleValue();
        if (x.contains ("k")) v *= 1000.0;
        if (x.contains ("m") && ! x.contains ("ms")) v *= 1.0; // 'm' = metres, no scale
        return v;
    }

    //==========================================================================
    // raw value -> display string
    inline juce::String formatValue (int layoutId, int slot, double raw, bool isVariationUnit)
    {
        const auto& sc = slotConv (layoutId, slot);
        const auto  rg = rangeFor (sc, isVariationUnit);
        const int   r  = (int) std::lround (raw);

        switch (sc.kind)
        {
            case mu100fx::CK_TABLE:
            {
                const auto& t = mu100fx::kConvTables[juce::jlimit (0, mu100fx::kConvTableCount, (int) sc.table)];
                if (t.values == nullptr || t.count == 0) break;
                return t.values[juce::jlimit (0, (int) t.count - 1, r)];
            }
            case mu100fx::CK_LINEAR:
            {
                double d = rg.dispLo;
                if (rg.hi != rg.lo)
                    d = rg.dispLo + (raw - rg.lo) * (rg.dispHi - rg.dispLo)
                                      / (double) (rg.hi - rg.lo);
                // juce::String(double, n) treats n <= 0 as "no precision given"
                // and prints the ostream default (6 significant digits), so a
                // 0-decimal slot whose ramp doesn't land on integers -- e.g.
                // pitch-change FB, -99..+99% over raw 1..127 -- would read
                // "-41.5714%". Round those instead.
                const bool round = sc.decimals == 0;
                if (round) d = (double) std::lround (d);
                juce::String s = round ? juce::String ((int) d)
                                       : juce::String (d, (int) sc.decimals);
                if (sc.signedShow && d > 0.0) s = "+" + s;
                if (sc.unit != nullptr) s += sc.unit;
                return s;
            }
            case mu100fx::CK_ENUM:
            {
                if (sc.enums == nullptr) break;
                auto opts = juce::StringArray::fromTokens (juce::String (sc.enums), "|", "");
                const int idx = r - rg.lo;
                if (idx >= 0 && idx < opts.size()) return opts[idx];
                break;
            }
            case mu100fx::CK_PAN:
            {
                const int v = r - 64;            // raw 1..127, centre 64
                if (v == 0) return "C";
                return (v < 0 ? "L" + juce::String (-v) : "R" + juce::String (v));
            }
            case mu100fx::CK_SPECIAL:
            {
                // dry/wet or early-ref/reverb balance: "D32" / "W14" / "D=W",
                // "E14" / "R20" / "E=R" — offset from centre 64.
                if (sc.enums == nullptr) break;
                auto bd = juce::StringArray::fromTokens (juce::String (sc.enums), "|", "");
                if (bd.size() != 2) break;
                const int v = r - 64;
                if (v == 0) return bd[0] + "=" + bd[1];
                return (v < 0 ? bd[0] + juce::String (-v) : bd[1] + juce::String (v));
            }
            default: break;                       // PLAIN / NONE
        }
        return juce::String (r);
    }

    //==========================================================================
    // display text -> raw value
    inline double parseValue (int layoutId, int slot, const juce::String& text, bool isVariationUnit)
    {
        const auto& sc = slotConv (layoutId, slot);
        const auto  rg = rangeFor (sc, isVariationUnit);
        const int   lo = rg.lo, hi = rg.hi;

        switch (sc.kind)
        {
            case mu100fx::CK_TABLE:
            {
                const auto& t = mu100fx::kConvTables[juce::jlimit (0, mu100fx::kConvTableCount, (int) sc.table)];
                if (t.values == nullptr || t.count == 0) break;
                const double want = parseScaled (text);
                int best = lo; double bestErr = 1e18;
                for (int i = juce::jmax (0, lo); i <= juce::jmin ((int) t.count - 1, hi); ++i)
                {
                    const double e = std::abs (parseScaled (t.values[i]) - want);
                    if (e < bestErr) { bestErr = e; best = i; }
                }
                return (double) best;
            }
            case mu100fx::CK_LINEAR:
            {
                const double d = text.retainCharacters ("0123456789.-+").getDoubleValue();
                double raw = lo;
                if (rg.dispHi != rg.dispLo)
                    raw = rg.lo + (d - rg.dispLo) * (double) (rg.hi - rg.lo)
                                    / (rg.dispHi - rg.dispLo);
                return juce::jlimit ((double) lo, (double) hi, std::round (raw));
            }
            case mu100fx::CK_ENUM:
            {
                if (sc.enums == nullptr) break;
                auto opts = juce::StringArray::fromTokens (juce::String (sc.enums), "|", "");
                for (int i = 0; i < opts.size(); ++i)
                    if (opts[i].equalsIgnoreCase (text.trim())) return (double) (lo + i);
                break;
            }
            case mu100fx::CK_PAN:
            {
                auto s = text.trim().toUpperCase();
                if (s.startsWith ("C")) return 64.0;
                const int n = s.retainCharacters ("0123456789").getIntValue();
                if (s.startsWith ("L")) return juce::jlimit ((double) lo, (double) hi, 64.0 - n);
                if (s.startsWith ("R")) return juce::jlimit ((double) lo, (double) hi, 64.0 + n);
                break;
            }
            case mu100fx::CK_SPECIAL:
            {
                if (sc.enums == nullptr) break;
                auto bd = juce::StringArray::fromTokens (juce::String (sc.enums), "|", "");
                if (bd.size() != 2) break;
                auto s = text.trim().toUpperCase();
                if (s.contains ("=")) return 64.0;
                const int n = s.retainCharacters ("0123456789").getIntValue();
                if (s.startsWith (bd[0].toUpperCase())) return juce::jlimit ((double) lo, (double) hi, 64.0 - n);
                if (s.startsWith (bd[1].toUpperCase())) return juce::jlimit ((double) lo, (double) hi, 64.0 + n);
                break;
            }
            default: break;
        }
        return (double) text.getIntValue();
    }

    //==========================================================================
    // Apply range + value formatter + right-click-reset default to a knob for
    // (layoutId, slot). `pair` = the slot is a 14-bit byte-pair (variation
    //==========================================================================
    // The raw value a MIDI-override bar or arc should GROW OUT OF for this slot --
    // the parameter's REST point, which for an effect parameter is wherever its
    // display reads zero. Returns false where the slot has no such point, and the
    // mark then grows from the minimum (see ValueBox::setBarOrigin).
    //
    // Derived from the spec conversion rather than listed per effect, so a slot
    // added to the tables gets the right answer with no edit here.
    inline bool barOriginFor (int layoutId, int slot, bool isVariationUnit, double& out)
    {
        const auto& sc = slotConv (layoutId, slot);
        const auto  rg = rangeFor (sc, isVariationUnit);

        // Both of these are defined as an offset from raw 64: pan about centre, and
        // the dry/wet and early-ref/reverb balances about "the two are equal".
        if (sc.kind == mu100fx::CK_PAN || sc.kind == mu100fx::CK_SPECIAL)
        {
            out = 64.0;
            return true;
        }

        // A signed linear ramp rests where it displays zero -- which is not the
        // middle of the raw range in general, and for several slots is not even
        // inside it (a 0..+100% depth has no rest point but its minimum).
        if (sc.kind == mu100fx::CK_LINEAR && rg.hi > rg.lo
            && rg.dispLo < 0.0 && rg.dispHi > 0.0)
        {
            out = (double) rg.lo + (0.0 - rg.dispLo) * (double) (rg.hi - rg.lo)
                                     / (rg.dispHi - rg.dispLo);
            return true;
        }

        return false;
    }

    // slots 0-9); used only to widen the range when the spec has no conversion
    // for the slot. `defaultRaw` = this effect program's factory default for the
    // slot (right-click reset target), clamped into range.
    template <typename Knob>
    inline void applyEffectFormat (Knob& k, int layoutId, int slot, bool pair, int defaultRaw,
                                   bool isVariationUnit)
    {
        const auto& sc = slotConv (layoutId, slot);
        const auto  rg = rangeFor (sc, isVariationUnit);
        int lo = rg.lo, hi = rg.hi;
        if (sc.kind == mu100fx::CK_NONE || hi <= lo)   // no spec conversion -> raw
        {
            lo = 0;
            hi = pair ? 16383 : 127;
        }
        k.setRange ((double) lo, (double) hi, 1.0);
        k.setDefaultValue (juce::jlimit (lo, hi, defaultRaw));
        k.setTextFromValue ([layoutId, slot, isVariationUnit] (double v)
                            { return formatValue (layoutId, slot, v, isVariationUnit); });
        k.setValueFromText ([layoutId, slot, isVariationUnit] (const juce::String& t)
                            { return parseValue (layoutId, slot, t, isVariationUnit); });
    }
} // namespace xgfmt
