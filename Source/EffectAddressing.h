// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <cstdint>
#include "codec/MU100SysEx.h"
#include "codec/MU100Effects.h"

//==============================================================================
// EffectAddressing — builds XG Parameter-Change messages for the four effect
// units (Reverb / Chorus / Variation / Insertion 1 & 2).
//
// Effect parameters are POSITIONAL: slot s (0..15 = "Parameter 1..16") maps to a
// fixed address offset within the unit's block, so we compute the address from
// the unit + slot rather than going through the parameters.json address map
// (whose variation/insertion blocks still carry the phantom-row extraction bug).
// Layout follows the XG / MU100 MIDI spec:
//
//   Reverb     02 01 00/01 type, 02..0B param1-10, 0C return, 0D pan, 10-15 p11-16
//   Chorus     02 01 20/21 type, 22..2B param1-10, 2C return, 2D pan, 2E to-rev,
//                                  30-35 p11-16
//   Variation  02 01 40/41 type, 42..55 param1-10 (size 2!), 56 return, 57 pan,
//                                  58 to-rev, 59 to-chr, 5A connection, 5B part,
//                                  5C control, 70-75 p11-16 (size 1)
//   Insertion1 03 00 00/01 type, 02..0B param1-10, 0C part, 0D control,
//                                  20-25 p11-16
//   Insertion2 03 01 …  (same offsets as Insertion1)
//
// Variation params 1-10 are 14-bit byte pairs; the rest are single 7-bit bytes.
//==============================================================================
namespace fxaddr
{
    using Bytes = std::vector<uint8_t>;

    struct Block { uint8_t aH, aM; };

    inline Block block(int unit)
    {
        switch (unit)
        {
            case mu100fx::FX_REVERB:     return { 0x02, 0x01 };
            case mu100fx::FX_CHORUS:     return { 0x02, 0x01 };
            case mu100fx::FX_VARIATION:  return { 0x02, 0x01 };
            case mu100fx::FX_INSERTION1: return { 0x03, 0x00 };
            case mu100fx::FX_INSERTION2: return { 0x03, 0x01 };
        }
        return { 0x02, 0x01 };
    }

    inline uint8_t typeLowMsb(int unit)
    {
        switch (unit)
        {
            case mu100fx::FX_REVERB:    return 0x00;
            case mu100fx::FX_CHORUS:    return 0x20;
            case mu100fx::FX_VARIATION: return 0x40;
            default:                    return 0x00;   // insertion
        }
    }

    // An insertion effect "requires the MSB" — its params 1-10 live in the 2-byte
    // region at 0x30 instead of the 1-byte region at 0x02 — iff any of those
    // params exceeds 7 bits. Per the MU100 MIDI spec this is exactly the delay
    // family (DELAY L,C,R / DELAY L,R / ECHO / CROSS DELAY), which is precisely the
    // set whose factory defaults exceed 127, so derive it from the defaults rather
    // than hard-coding type codes. Reverb/Chorus/Variation never use this region.
    inline bool insertionRequiresMsb(int unit, const mu100fx::EffectType& t)
    {
        if (unit != mu100fx::FX_INSERTION1 && unit != mu100fx::FX_INSERTION2)
            return false;
        for (int s = 0; s < 10; ++s)
            if (t.deflt[s] > 127) return true;
        return false;
    }

    // Address-low for parameter slot s (0..15 = Parameter 1..16). Returns -1 if
    // the unit doesn't expose that slot. For insertion units `insMsb` selects the
    // 2-byte param-1..10 region (0x30) used by MSB-required (delay) effects.
    inline int paramLow(int unit, int slot, bool insMsb = false)
    {
        switch (unit)
        {
            case mu100fx::FX_REVERB:
                return slot < 10 ? 0x02 + slot : 0x10 + (slot - 10);
            case mu100fx::FX_CHORUS:
                return slot < 10 ? 0x22 + slot : 0x30 + (slot - 10);
            case mu100fx::FX_VARIATION:
                return slot < 10 ? 0x42 + slot * 2 : 0x70 + (slot - 10);
            case mu100fx::FX_INSERTION1:
            case mu100fx::FX_INSERTION2:
                // Params 1-10: MSB-required effects use the 2-byte region 0x30-0x43,
                // others the 1-byte region 0x02-0x0B. Params 11-16 are always 1-byte.
                if (slot < 10) return insMsb ? 0x30 + slot * 2 : 0x02 + slot;
                return 0x20 + (slot - 10);
        }
        return -1;
    }

    inline bool slotIsPair(int unit, int slot, bool insMsb = false)
    {
        if (unit == mu100fx::FX_VARIATION) return slot < 10;
        if (unit == mu100fx::FX_INSERTION1 || unit == mu100fx::FX_INSERTION2)
            return insMsb && slot < 10;
        return false;
    }

    inline Bytes paramChange(uint8_t aH, uint8_t aM, uint8_t aL, const Bytes& data)
    {
        return mu100::encodeParamChange(mu100::Model::XG, 0, aH, aM, aL, data);
    }

    // A single effect parameter value (slot 0..15). `insMsb` routes insertion
    // params 1-10 to the 2-byte region (see paramLow/slotIsPair).
    inline Bytes setParam(int unit, int slot, int value, bool insMsb = false)
    {
        const auto b = block(unit);
        const int lo = paramLow(unit, slot, insMsb);
        if (lo < 0) return {};
        if (slotIsPair(unit, slot, insMsb))
            return paramChange(b.aH, b.aM, (uint8_t) lo,
                               { (uint8_t) ((value >> 7) & 0x7F), (uint8_t) (value & 0x7F) });
        return paramChange(b.aH, b.aM, (uint8_t) lo, { (uint8_t) (value & 0x7F) });
    }

    // Select the effect TYPE (program) by its bank MSB/LSB.
    inline Bytes setType(int unit, uint8_t msb, uint8_t lsb)
    {
        const auto b = block(unit);
        const uint8_t lo = typeLowMsb(unit);
        return paramChange(b.aH, b.aM, lo, { msb, lsb });   // MSB then LSB (consecutive)
    }

    // ---- Effect-module controls. low<0 means the unit has no such control. ----
    struct Controls { int level, pan, toRev, toChr, connection, part, control; };

    inline Controls controls(int unit)
    {
        switch (unit)
        {
            case mu100fx::FX_REVERB:     return { 0x0C, 0x0D, -1,  -1,  -1,  -1,  -1  };
            case mu100fx::FX_CHORUS:     return { 0x2C, 0x2D, 0x2E,-1,  -1,  -1,  -1  };
            case mu100fx::FX_VARIATION:  return { 0x56, 0x57, 0x58,0x59,0x5A,0x5B,0x5C };
            case mu100fx::FX_INSERTION1:
            case mu100fx::FX_INSERTION2: return { -1,  -1,   -1,  -1,  -1,  0x0C,0x0D };
        }
        return { -1, -1, -1, -1, -1, -1, -1 };
    }

    inline Bytes setControl(int unit, int low, int value)
    {
        const auto b = block(unit);
        if (low < 0) return {};
        return paramChange(b.aH, b.aM, (uint8_t) low, { (uint8_t) (value & 0x7F) });
    }
}
