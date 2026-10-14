// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// MU100State.cpp — see MU100State.h.
#include "MU100State.h"

namespace mu100 {

DeviceState::LoadResult DeviceState::load(const uint8_t* data, size_t len) {
    LoadResult r;
    auto msgs = parseStream(data, len);
    r.messages = static_cast<int>(msgs.size());

    for (const auto& m : msgs) {
        switch (m.kind) {
        case Kind::XGSystemOn:
            sawXGSystemOn_ = true;
            r.sawXGSystemOn = true;
            ++r.skipped;            // carries no editable bytes itself
            break;

        case Kind::ParamChange:
        case Kind::BulkDump: {
            ModelIx ix;
            if (!modelToIx(m.model, ix)) { ++r.skipped; break; }
            if (m.kind == Kind::BulkDump && !m.checksumOk) ++r.badChecksum;
            const uint32_t base = m.address();
            int wrote = 0;
            for (size_t k = 0; k < m.data.size(); ++k) {
                setByte(ix, base + static_cast<uint32_t>(k), m.data[k]);
                ++wrote;
            }
            if (wrote) { ++r.applied; r.bytes += wrote; } else ++r.skipped;
            break;
        }

        default:
            ++r.skipped;            // requests, universal, master volume, unknown
            break;
        }
    }
    return r;
}

// Split a model's stored bytes into maximal contiguous runs and hand each
// (startAddr, data) to `emit`.
template <typename Fn>
static void forEachRun(const std::map<uint32_t, uint8_t>& bytes, Fn&& emit) {
    auto runStart = bytes.end();
    uint32_t prevKey = 0;
    Bytes acc;
    auto flush = [&](uint32_t startKey) {
        if (acc.empty()) return;
        emit(startKey, acc);
        acc.clear();
    };
    for (auto it = bytes.begin(); it != bytes.end(); ++it) {
        if (acc.empty()) {
            runStart = it; prevKey = it->first; acc.push_back(it->second);
        } else if (it->first == prevKey + 1) {
            // same model (top nibble) and address contiguous
            if ((it->first & 0xFF000000u) == (prevKey & 0xFF000000u)) {
                prevKey = it->first; acc.push_back(it->second);
            } else {
                flush(runStart->first); runStart = it; prevKey = it->first; acc.push_back(it->second);
            }
        } else {
            flush(runStart->first); runStart = it; prevKey = it->first; acc.push_back(it->second);
        }
    }
    flush(runStart == bytes.end() ? 0 : runStart->first);
}

Bytes DeviceState::toBulkDumps(int device, bool leadWithXGSystemOn) const {
    Bytes out;
    if (leadWithXGSystemOn) { Bytes x = encodeXGSystemOn(device); out.insert(out.end(), x.begin(), x.end()); }
    forEachRun(bytes_, [&](uint32_t startKey, const Bytes& acc) {
        ModelIx ix = static_cast<ModelIx>((startKey >> 24) & 0x0F);
        uint32_t a = startKey & 0x1FFFFF;
        Bytes msg = encodeBulkDump(ixToModel(ix), device,
                                   static_cast<uint8_t>((a >> 14) & 0x7F),
                                   static_cast<uint8_t>((a >> 7) & 0x7F),
                                   static_cast<uint8_t>(a & 0x7F), acc);
        out.insert(out.end(), msg.begin(), msg.end());
    });
    return out;
}

Bytes DeviceState::toParamChanges(int device, bool leadWithXGSystemOn) const {
    Bytes out;
    if (leadWithXGSystemOn) { Bytes x = encodeXGSystemOn(device); out.insert(out.end(), x.begin(), x.end()); }
    forEachRun(bytes_, [&](uint32_t startKey, const Bytes& acc) {
        ModelIx ix = static_cast<ModelIx>((startKey >> 24) & 0x0F);
        uint32_t a = startKey & 0x1FFFFF;
        Bytes msg = encodeParamChange(ixToModel(ix), device,
                                      static_cast<uint8_t>((a >> 14) & 0x7F),
                                      static_cast<uint8_t>((a >> 7) & 0x7F),
                                      static_cast<uint8_t>(a & 0x7F), acc);
        out.insert(out.end(), msg.begin(), msg.end());
    });
    return out;
}

} // namespace mu100
