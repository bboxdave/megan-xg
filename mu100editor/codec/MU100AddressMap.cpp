// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// MU100AddressMap.cpp — see MU100AddressMap.h.
#include "MU100AddressMap.h"
#include <cstring>

namespace mu100 {

int AddressMap::find(const char* nm, const char* tbl) {
    for (int i = 0; i < kParamCount; ++i) {
        if (std::strcmp(name(i), nm) != 0) continue;
        if (tbl && std::strcmp(table(i), tbl) != 0) continue;
        return i;
    }
    return -1;
}

// Does message address byte `mb` fit address spec `b`? If templated, the fixed
// part must match and the placeholder nibbles yield part of the instance.
static bool byteFits(const AddrByte& b, uint8_t mb, int& instContrib) {
    if (b.mask == 0) { instContrib = -1; return mb == b.base; }
    if ((mb & ~b.mask & 0x7F) != (b.base & ~b.mask & 0x7F)) return false;
    instContrib = mb & b.mask;
    return true;
}

int AddressMap::identify(const Message& m, int& instanceOut, int& instance2Out) {
    instanceOut = 0; instance2Out = 0;
    ModelIx mix;
    if (!modelToIx(m.model, mix)) return -1;
    const uint8_t aB[3] = { m.addr[0], m.addr[1], m.addr[2] };

    for (int i = 0; i < kParamCount; ++i) {
        const ParamDef& p = kParams[i];
        if (p.model != static_cast<uint8_t>(mix)) continue;
        const AddrByte* spec[3] = { &p.hi, &p.mid, &p.lo };
        int contrib[3]; bool ok = true;
        for (int k = 0; k < 3; ++k) if (!byteFits(*spec[k], aB[k], contrib[k])) { ok = false; break; }
        if (!ok) continue;
        // Mirror resolve(): least-significant templated byte -> instance,
        // next-more-significant -> instance2.
        int tmpl[3], nt = 0;
        for (int k = 0; k < 3; ++k) if (spec[k]->mask) tmpl[nt++] = k;
        if (nt >= 1) instanceOut  = contrib[tmpl[nt - 1]];
        if (nt >= 2) instance2Out = contrib[tmpl[nt - 2]];
        return i;
    }
    return -1;
}

AddressMap::Value AddressMap::get(const DeviceState& st, const ParamDef& p, int instance, int instance2) {
    Value v;
    ModelIx model; uint32_t addr;
    resolve(p, instance, instance2, model, addr);
    v.bytes.resize(p.size);
    for (int k = 0; k < p.size; ++k) {
        uint8_t b;
        if (!st.getByte(model, addr + static_cast<uint32_t>(k), b)) return v; // present stays false
        v.bytes[static_cast<size_t>(k)] = b;
    }
    v.present = true;
    switch (p.enc) {
    case Enc::NIBBLE:   v.combined = static_cast<int>(denibblize(v.bytes)); break;
    case Enc::BYTEPAIR: v.combined = (v.bytes[0] << 7) | (p.size > 1 ? v.bytes[1] : 0); break;
    case Enc::SINGLE:
    default:            v.combined = v.bytes[0]; break;
    }
    return v;
}

static Bytes encodeValue(Enc enc, int size, uint32_t value) {
    switch (enc) {
    case Enc::NIBBLE:   return nibblize(value, size);
    case Enc::BYTEPAIR: {
        Bytes b(static_cast<size_t>(size), 0);
        if (size >= 2) { b[0] = (value >> 7) & 0x7F; b[1] = value & 0x7F; }
        else            b[0] = value & 0x7F;
        return b;
    }
    case Enc::SINGLE:
    default: {
        Bytes b(static_cast<size_t>(size), 0);
        b[0] = value & 0x7F;
        return b;
    }
    }
}

Bytes AddressMap::set(DeviceState& st, const ParamDef& p, int instance, uint32_t value, int instance2) {
    ModelIx model; uint32_t addr;
    resolve(p, instance, instance2, model, addr);
    Bytes b = encodeValue(p.enc, p.size, value);
    for (size_t k = 0; k < b.size(); ++k)
        st.setByte(model, addr + static_cast<uint32_t>(k), b[k]);
    return b;
}

Bytes AddressMap::makeParamChange(const ParamDef& p, int instance, uint32_t value, int instance2, int device) {
    ModelIx model; uint32_t addr;
    resolve(p, instance, instance2, model, addr);
    Bytes b = encodeValue(p.enc, p.size, value);
    return encodeParamChange(ixToModel(model), device,
                             static_cast<uint8_t>((addr >> 14) & 0x7F),
                             static_cast<uint8_t>((addr >> 7) & 0x7F),
                             static_cast<uint8_t>(addr & 0x7F), b);
}

} // namespace mu100
