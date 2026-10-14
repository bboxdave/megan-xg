// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// MU100State.h — device state model for the MU100 editor.
//
// DeviceState is the byte-exact backbone: it stores ONE 7-bit value per
// (model, 21-bit address).  Both on-disk formats collapse into it —
//   * XGEdit param-change streams (each F0 43 1n 4C ... sets a few bytes)
//   * MU100 bulk dumps           (each block writes a contiguous byte run)
// so a file in either format loads into the same editable state, and the
// editor reads/writes individual addresses without caring how they were framed.
//
// Multi-byte parameter *interpretation* (nibble vs byte-pair vs effect-type-
// dependent) is deliberately NOT here — that's the MU100AddressMap semantic
// layer.  DeviceState only moves bytes, which makes it robust against any
// quirks in the parameter map.
//
// JUCE-independent; pairs with MU100SysEx. The plugin compiles it directly.

#pragma once
#include "MU100SysEx.h"
#include <cstdint>
#include <map>
#include <vector>
#include <string>

namespace mu100 {

// Compact model index for state keys (the three editable model IDs).
enum class ModelIx { XG = 0, Native1 = 1, Native2 = 2 };

inline bool modelToIx(Model m, ModelIx& out) {
    switch (m) {
    case Model::XG:      out = ModelIx::XG;      return true;
    case Model::Native1: out = ModelIx::Native1; return true;
    case Model::Native2: out = ModelIx::Native2; return true;
    default:             return false;
    }
}
inline Model ixToModel(ModelIx ix) {
    switch (ix) {
    case ModelIx::Native1: return Model::Native1;
    case ModelIx::Native2: return Model::Native2;
    default:               return Model::XG;
    }
}

class DeviceState {
public:
    // 28-bit key: model index (high 4 bits) | 21-bit address (a3<<14|a2<<7|a1).
    static uint32_t key(ModelIx ix, uint32_t addr21) {
        return (static_cast<uint32_t>(ix) << 24) | (addr21 & 0x1FFFFF);
    }

    // ---- mutation ----
    void setByte(ModelIx ix, uint32_t addr21, uint8_t value) {
        bytes_[key(ix, addr21)] = static_cast<uint8_t>(value & 0x7F);
    }
    bool getByte(ModelIx ix, uint32_t addr21, uint8_t& out) const {
        auto it = bytes_.find(key(ix, addr21));
        if (it == bytes_.end()) return false;
        out = it->second; return true;
    }
    // Remove one byte; true if there was one. The plugin's MIDI-override shadow
    // needs it: an entry RETIRES when the device comes back into line with the
    // model, and a state that could only grow would go on claiming a difference
    // that is no longer there.
    bool eraseByte(ModelIx ix, uint32_t addr21) {
        return bytes_.erase(key(ix, addr21)) != 0;
    }
    bool has(ModelIx ix, uint32_t addr21) const {
        return bytes_.count(key(ix, addr21)) != 0;
    }
    void clear() { bytes_.clear(); sawXGSystemOn_ = false; }
    size_t size() const { return bytes_.size(); }

    // ---- loading ----
    struct LoadResult {
        int messages = 0;       // SysEx messages parsed
        int applied  = 0;       // messages that wrote at least one byte
        int bytes    = 0;       // total bytes written
        int badChecksum = 0;    // bulk dumps with a bad checksum (still applied)
        int skipped  = 0;       // messages we didn't fold into state (requests, etc.)
        bool sawXGSystemOn = false;
    };

    // Fold a .syx (or live MIDI chunk) into the state. Param changes and bulk
    // dumps write bytes; XG System On is noted; requests/universal are skipped.
    // Returns stats; applying is additive (call clear() first for a fresh load).
    LoadResult load(const uint8_t* data, size_t len);
    LoadResult load(const Bytes& b) { return load(b.data(), b.size()); }

    bool sawXGSystemOn() const { return sawXGSystemOn_; }

    // ---- serialisation ----
    // Emit the state as bulk-dump messages, one per maximal contiguous address
    // run within each model (device number `device`). Reloading the output
    // reproduces this state exactly. Optionally prepend an XG System On.
    Bytes toBulkDumps(int device = 0, bool leadWithXGSystemOn = false) const;

    // Emit the state as a flat parameter-change stream (XGEdit-style): one
    // message per contiguous run. `model`-correct command per stored model.
    Bytes toParamChanges(int device = 0, bool leadWithXGSystemOn = false) const;

    // Direct access for the semantic layer / tests.
    const std::map<uint32_t, uint8_t>& raw() const { return bytes_; }

private:
    std::map<uint32_t, uint8_t> bytes_;
    bool sawXGSystemOn_ = false;
};

} // namespace mu100
