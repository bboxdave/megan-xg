// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// MU100SysEx.cpp — see MU100SysEx.h.
#include "MU100SysEx.h"

namespace mu100 {

// ---- small helpers -------------------------------------------------------

static bool isModel(uint8_t b) {
    return b == 0x4C || b == 0x49 || b == 0x59 || b == 0x27;
}

uint8_t bulkChecksum(const uint8_t* body, size_t len) {
    unsigned sum = 0;
    for (size_t i = 0; i < len; ++i) sum += body[i];
    return static_cast<uint8_t>((0x80 - (sum & 0x7F)) & 0x7F);
}
uint8_t bulkChecksum(const Bytes& body) {
    return bulkChecksum(body.data(), body.size());
}

Bytes nibblize(uint32_t value, int numBytes) {
    Bytes out(static_cast<size_t>(numBytes));
    for (int i = numBytes - 1; i >= 0; --i) {
        out[static_cast<size_t>(i)] = static_cast<uint8_t>(value & 0x0F);
        value >>= 4;
    }
    return out;
}
uint32_t denibblize(const uint8_t* p, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 4) | (p[i] & 0x0F);
    return v;
}
uint32_t denibblize(const Bytes& b) { return denibblize(b.data(), b.size()); }

// ---- classification ------------------------------------------------------

// Fill kind/model/device/addr/data/checksum for a single F0..F7 span.
static void classify(Message& m) {
    const Bytes& b = m.raw;
    const size_t n = b.size();
    if (n < 2 || b.front() != 0xF0) { m.kind = Kind::Unknown; return; }

    // Universal (non-)realtime
    if (b[1] == 0x7E) { // non-realtime
        if (n >= 5 && b[3] == 0x09 && b[4] == 0x01) { m.kind = Kind::GMSystemOn; m.device = b[2]; return; }
        if (n >= 5 && b[3] == 0x06 && b[4] == 0x01) { m.kind = Kind::IdentityRequest; m.device = b[2]; return; }
        m.kind = Kind::Universal; return;
    }
    if (b[1] == 0x7F) { // realtime
        if (n >= 5 && b[3] == 0x04 && b[4] == 0x01) {
            m.kind = Kind::MasterVolume; m.device = b[2] & 0x0F;
            if (n >= 7) m.data.assign(b.begin() + 5, b.begin() + 7);
            return;
        }
        m.kind = Kind::Universal; return;
    }

    if (b[1] != 0x43) { m.kind = Kind::Unknown; return; } // not Yamaha

    if (n < 4 || !isModel(b[3])) { m.kind = Kind::Unknown; return; }
    const uint8_t cmd = b[2] & 0xF0;
    m.device = b[2] & 0x0F;
    m.model  = static_cast<Model>(b[3]);

    switch (cmd) {
    case 0x10: { // parameter change: 1n MM aH aM aL data.. F7
        if (n < 8) { m.kind = Kind::Unknown; return; }
        m.addr[0] = b[4]; m.addr[1] = b[5]; m.addr[2] = b[6];
        m.data.assign(b.begin() + 7, b.end() - 1); // strip F7
        // XG System On is a 4C param change to 00 00 7E with data 00.
        if (m.model == Model::XG && b[4] == 0 && b[5] == 0 && b[6] == 0x7E)
            m.kind = Kind::XGSystemOn;
        else
            m.kind = Kind::ParamChange;
        return;
    }
    case 0x00: { // bulk dump: 0n MM bcH bcL aH aM aL data.. ck F7
        if (n < 11) { m.kind = Kind::Unknown; return; }
        m.kind = Kind::BulkDump;
        m.addr[0] = b[6]; m.addr[1] = b[7]; m.addr[2] = b[8];
        m.data.assign(b.begin() + 9, b.end() - 2);   // between addr and checksum
        const uint8_t ck = b[n - 2];
        // checksum covers byte-count(2)+addr(3)+data, i.e. b[4 .. n-3]
        const uint8_t want = bulkChecksum(&b[4], (n - 2) - 4);
        m.checksumOk = (ck == want);
        return;
    }
    case 0x30: // parameter request: 3n MM aH aM aL F7
        if (n < 7) { m.kind = Kind::Unknown; return; }
        m.kind = Kind::ParamRequest;
        m.addr[0] = b[4]; m.addr[1] = b[5]; m.addr[2] = b[6];
        return;
    case 0x20: // dump request: 2n MM aH aM aL F7
        if (n < 7) { m.kind = Kind::Unknown; return; }
        m.kind = Kind::DumpRequest;
        m.addr[0] = b[4]; m.addr[1] = b[5]; m.addr[2] = b[6];
        return;
    default:
        m.kind = Kind::Unknown;
        return;
    }
}

std::vector<Message> parseStream(const uint8_t* data, size_t len) {
    std::vector<Message> out;
    size_t i = 0;
    while (i < len) {
        if (data[i] != 0xF0) { ++i; continue; }
        size_t j = i + 1;
        while (j < len && data[j] != 0xF7) ++j;
        if (j >= len) break;            // truncated final message: drop
        Message m;
        m.raw.assign(data + i, data + j + 1);
        classify(m);
        out.push_back(std::move(m));
        i = j + 1;
    }
    return out;
}

// ---- encoders ------------------------------------------------------------

static uint8_t cmdByte(uint8_t hi, int device) {
    return static_cast<uint8_t>(hi | (device & 0x0F));
}

Bytes encodeParamChange(Model model, int device,
                        uint8_t aH, uint8_t aM, uint8_t aL, const Bytes& data) {
    Bytes m{0xF0, 0x43, cmdByte(0x10, device), static_cast<uint8_t>(model), aH, aM, aL};
    m.insert(m.end(), data.begin(), data.end());
    m.push_back(0xF7);
    return m;
}

Bytes encodeBulkDump(Model model, int device,
                     uint8_t aH, uint8_t aM, uint8_t aL, const Bytes& data) {
    const uint32_t bc = static_cast<uint32_t>(data.size());
    Bytes body{ static_cast<uint8_t>((bc >> 7) & 0x7F), static_cast<uint8_t>(bc & 0x7F),
                aH, aM, aL };
    body.insert(body.end(), data.begin(), data.end());
    const uint8_t ck = bulkChecksum(body);

    Bytes m{0xF0, 0x43, cmdByte(0x00, device), static_cast<uint8_t>(model)};
    m.insert(m.end(), body.begin(), body.end());
    m.push_back(ck);
    m.push_back(0xF7);
    return m;
}

Bytes encodeParamRequest(Model model, int device, uint8_t aH, uint8_t aM, uint8_t aL) {
    return Bytes{0xF0, 0x43, cmdByte(0x30, device), static_cast<uint8_t>(model), aH, aM, aL, 0xF7};
}

Bytes encodeDumpRequest(Model model, int device, uint8_t aH, uint8_t aM, uint8_t aL) {
    return Bytes{0xF0, 0x43, cmdByte(0x20, device), static_cast<uint8_t>(model), aH, aM, aL, 0xF7};
}

Bytes encodeXGSystemOn(int device) {
    return Bytes{0xF0, 0x43, cmdByte(0x10, device), 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7};
}

Bytes encodeGMSystemOn(int device) {
    return Bytes{0xF0, 0x7E, static_cast<uint8_t>(device & 0x7F), 0x09, 0x01, 0xF7};
}

Bytes encodeIdentityRequest(int device) {
    return Bytes{0xF0, 0x7E, static_cast<uint8_t>(device & 0x7F), 0x06, 0x01, 0xF7};
}

Bytes encode(const Message& m) {
    switch (m.kind) {
    case Kind::XGSystemOn:      return encodeXGSystemOn(m.device < 0 ? 0 : m.device);
    case Kind::ParamChange:     return encodeParamChange(m.model, m.device, m.addr[0], m.addr[1], m.addr[2], m.data);
    case Kind::BulkDump:        return encodeBulkDump(m.model, m.device, m.addr[0], m.addr[1], m.addr[2], m.data);
    case Kind::ParamRequest:    return encodeParamRequest(m.model, m.device, m.addr[0], m.addr[1], m.addr[2]);
    case Kind::DumpRequest:     return encodeDumpRequest(m.model, m.device, m.addr[0], m.addr[1], m.addr[2]);
    case Kind::GMSystemOn:      return encodeGMSystemOn(m.device < 0 ? 0x7F : m.device);
    case Kind::IdentityRequest: return encodeIdentityRequest(m.device < 0 ? 0x7F : m.device);
    default:                    return m.raw; // Universal/MasterVolume/Unknown: re-emit verbatim
    }
}

// ---- names ---------------------------------------------------------------

const char* modelName(Model m) {
    switch (m) {
    case Model::XG:         return "XG";
    case Model::Native1:    return "Native1(49)";
    case Model::Native2:    return "Native2(59)";
    case Model::MasterTune: return "MasterTune(27)";
    default:                return "Unknown";
    }
}
const char* kindName(Kind k) {
    switch (k) {
    case Kind::ParamChange:     return "ParamChange";
    case Kind::BulkDump:        return "BulkDump";
    case Kind::ParamRequest:    return "ParamRequest";
    case Kind::DumpRequest:     return "DumpRequest";
    case Kind::XGSystemOn:      return "XGSystemOn";
    case Kind::GMSystemOn:      return "GMSystemOn";
    case Kind::IdentityRequest: return "IdentityRequest";
    case Kind::MasterVolume:    return "MasterVolume";
    case Kind::Universal:       return "Universal";
    default:                    return "Unknown";
    }
}

} // namespace mu100
