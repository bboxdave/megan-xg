// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// MU100AddressMap.h — semantic layer over DeviceState.
//
// Maps named parameters <-> resolved (model, address), using the codegen'd
// table in MU100AddressMapData.h (from data/parameters.json).  Resolves the
// address templates (part nn / a-d 0n / perf pp / 3n) for a given instance,
// reads/writes parameter VALUES from a DeviceState, and turns a parameter
// change message into the param it targets.
//
// Value encoding handled here (per empirical findings):
//   SINGLE   — one 7-bit byte (the vast majority).
//   NIBBLE   — XG big-endian 4-bit packing (MASTER TUNE, size 4).
//   BYTEPAIR — two independent 7-bit bytes (effect TYPE/PARAMETER MSB+LSB).
//              Combined value = (b0<<7)|b1; raw bytes also available.
// Effect-type-DEPENDENT display scaling of byte-pair params is NOT done here —
// that lives with the effect screens (Phase 4) backed by effects.json.

#pragma once
#include "MU100State.h"
#include "MU100AddressMapData.h"
#include <string>
#include <vector>
#include <cstdint>

namespace mu100 {

class AddressMap {
public:
    static int count() { return kParamCount; }
    static const ParamDef& param(int i) { return kParams[i]; }
    static const char* name(const ParamDef& p)  { return kStr[p.nameOff]; }
    static const char* table(const ParamDef& p) { return kStr[p.tableOff]; }
    static const char* name(int i)  { return name(param(i)); }
    static const char* table(int i) { return table(param(i)); }

    // Resolve a param's address for the given template instance(s).
    //
    // Most params have one templated address byte (part 0..31, A/D 0..1, perf
    // number); pass it as `instance`. A few have TWO (Drum Setup high=3n setup
    // number + mid=rr note; Internal Performance 3n+pp) — pass the finer index
    // as `instance` and the coarser one as `instance2`. Assignment is by address
    // significance: the least-significant templated byte takes `instance`, the
    // next-more-significant takes `instance2`.
    //   Drum Setup:  resolve(p, note, setupNumber)   note=0x0D..0x5B, setup=0..3
    static void resolve(const ParamDef& p, int instance, int instance2,
                        ModelIx& model, uint32_t& addr21) {
        model = static_cast<ModelIx>(p.model);
        const AddrByte* b[3] = { &p.hi, &p.mid, &p.lo };
        uint8_t v[3] = { p.hi.base, p.mid.base, p.lo.base };
        int tmpl[3], nt = 0;
        for (int i = 0; i < 3; ++i) if (b[i]->mask) tmpl[nt++] = i;
        if (nt >= 1) { int i = tmpl[nt - 1]; v[i] = static_cast<uint8_t>((b[i]->base | (instance  & b[i]->mask)) & 0x7F); }
        if (nt >= 2) { int i = tmpl[nt - 2]; v[i] = static_cast<uint8_t>((b[i]->base | (instance2 & b[i]->mask)) & 0x7F); }
        addr21 = (static_cast<uint32_t>(v[0]) << 14) | (static_cast<uint32_t>(v[1]) << 7) | v[2];
    }
    static void resolve(const ParamDef& p, int instance, ModelIx& model, uint32_t& addr21) {
        resolve(p, instance, 0, model, addr21);
    }

    // First param whose name (and optionally table) matches exactly. -1 if none.
    static int find(const char* name, const char* table = nullptr);

    // Identify which param (if any) a parsed message targets; reports the
    // template instance(s) it decodes to (instance = finer/LSB templated byte,
    // instance2 = coarser). Returns param index or -1.
    static int identify(const Message& m, int& instanceOut, int& instance2Out);
    static int identify(const Message& m, int& instanceOut) {
        int dummy; return identify(m, instanceOut, dummy);
    }

    // ---- value get/set on a DeviceState ----

    struct Value {
        bool    present = false;  // were all the param's bytes found in state?
        int     combined = 0;     // decoded scalar (single / nibble / byte-pair)
        Bytes   bytes;            // raw bytes as stored (size = p.size)
    };

    static Value get(const DeviceState& st, const ParamDef& p, int instance, int instance2 = 0);
    static Value get(const DeviceState& st, int paramIndex, int instance, int instance2 = 0) {
        return get(st, param(paramIndex), instance, instance2);
    }

    // Write a scalar value into state (encodes per p.enc). Returns the bytes written.
    static Bytes set(DeviceState& st, const ParamDef& p, int instance, uint32_t value, int instance2 = 0);

    // Build the parameter-change message that transmits this value to the device.
    static Bytes makeParamChange(const ParamDef& p, int instance, uint32_t value,
                                 int instance2 = 0, int device = 0);

private:
    static uint8_t resolveByte(const AddrByte& b, int instance) {
        return b.mask == 0 ? b.base
                           : static_cast<uint8_t>((b.base | (instance & b.mask)) & 0x7F);
    }
};

} // namespace mu100
