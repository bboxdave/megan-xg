// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// MU100SysEx.h — JUCE-independent codec for Yamaha MU100 / XG System Exclusive.
//
// Pure framing layer: turns byte streams into classified messages and back.
// No JUCE, no JSON, no I/O — just std::vector<uint8_t> in/out so it drops
// straight into the plugin (which compiles it directly) or any test harness.
//
// Reference: MU100 Owner's Manual "MIDI Data Format" (sound-list/MIDI PDF
// pp.34-39).  Message families implemented:
//   * XG / native Parameter Change      F0 43 1n MM aH aM aL dd.. F7
//   * XG / native Bulk Dump             F0 43 0n MM bcH bcL aH aM aL dd.. ck F7
//   * XG / native Parameter Request     F0 43 3n MM aH aM aL F7
//   * XG / native Dump Request          F0 43 2n MM aH aM aL F7
//   * XG System On / GM System On / Identity Request (universal)
//
// MM (Model ID): 4C = XG, 49 = MU100 native(1), 59 = MU100 native(2),
//                27 = Master Tune (legacy).
//
// Observed in the wild (reference/syx examples/, 2026-06-22):
//   * XGEdit saves a flat STREAM OF PARAMETER CHANGES (model 4C, device
//     nibble F = broadcast), led by an XG System On.
//   * The MU100's own "dumpout" saves BULK DUMP blocks (device 0).
//   Both decode through this one codec; both reduce to {model,address}->bytes.

#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace mu100 {

using Bytes = std::vector<uint8_t>;

enum class Model : uint8_t {
    XG          = 0x4C,
    Native1     = 0x49,  // MU100 native param change/bulk (1)  (MU50/80/90 fmt)
    Native2     = 0x59,  // MU100 native param change/bulk (2)  (MU90 fmt)
    MasterTune  = 0x27,  // legacy master tuning
    Unknown     = 0x00
};

enum class Kind {
    ParamChange,     // 1n  MM ...
    BulkDump,        // 0n  MM ...  (carries checksum)
    ParamRequest,    // 3n  MM addr
    DumpRequest,     // 2n  MM addr
    XGSystemOn,      // 1n 4C 00 00 7E 00  (special ParamChange)
    GMSystemOn,      // 7E 7F 09 01
    IdentityRequest, // 7E dd 06 01
    MasterVolume,    // 7F 7F 04 01 ll mm
    Universal,       // other 7E/7F universal we don't model
    Unknown
};

// One parsed System Exclusive message (the full F0..F7 span is preserved in raw).
struct Message {
    Kind    kind   = Kind::Unknown;
    Model   model  = Model::Unknown;
    int     device = -1;          // low nibble of the status/command byte (0..15)
    uint8_t addr[3] = {0,0,0};    // High/Mid/Low (param change, bulk, requests)
    Bytes   data;                 // payload bytes (param change data, or bulk body)
    bool    checksumOk = true;    // bulk dump only; true for non-bulk kinds
    Bytes   raw;                  // exact original bytes F0..F7 inclusive

    // 21-bit packed address, handy as a map key.
    uint32_t address() const { return (addr[0] << 14) | (addr[1] << 7) | addr[2]; }
};

// ---- Parsing -------------------------------------------------------------

// Split a raw byte buffer (a whole .syx file, or a live MIDI chunk) into the
// SysEx messages it contains and classify each. Bytes outside F0..F7 are
// skipped. Truncated trailing messages (no F7) are dropped.
std::vector<Message> parseStream(const uint8_t* data, size_t len);
inline std::vector<Message> parseStream(const Bytes& b) {
    return parseStream(b.data(), b.size());
}

// ---- Encoding ------------------------------------------------------------

// Parameter Change: F0 43 1n MM aH aM aL <data..> F7
Bytes encodeParamChange(Model model, int device,
                        uint8_t aHigh, uint8_t aMid, uint8_t aLow,
                        const Bytes& data);

// Bulk Dump: F0 43 0n MM bcH bcL aH aM aL <data..> ck F7  (checksum computed).
Bytes encodeBulkDump(Model model, int device,
                     uint8_t aHigh, uint8_t aMid, uint8_t aLow,
                     const Bytes& data);

// Parameter Request: F0 43 3n MM aH aM aL F7
Bytes encodeParamRequest(Model model, int device,
                         uint8_t aHigh, uint8_t aMid, uint8_t aLow);

// Dump Request: F0 43 2n MM aH aM aL F7  (addr must be a block start).
Bytes encodeDumpRequest(Model model, int device,
                        uint8_t aHigh, uint8_t aMid, uint8_t aLow);

// XG System On: F0 43 1n 4C 00 00 7E 00 F7  (switches module into XG mode).
Bytes encodeXGSystemOn(int device = 0);

// GM System On (universal non-realtime): F0 7E 7F 09 01 F7.
Bytes encodeGMSystemOn(int device = 0x7F);

// Identity Request (universal non-realtime): F0 7E dd 06 01 F7.
Bytes encodeIdentityRequest(int device = 0x7F);

// Re-emit a parsed message to bytes (used to prove byte-exact round-trips).
// Recomputes bulk-dump checksums; otherwise reconstructs from fields.
Bytes encode(const Message& m);

// ---- Value <-> data-byte helpers ----------------------------------------

// XG multi-byte numeric values are stored big-endian, 4 bits per data byte
// (the low nibble of each byte). nibblize(0x1234, 4) -> {01,02,03,04}.
Bytes     nibblize(uint32_t value, int numBytes);
uint32_t  denibblize(const Bytes& bytes);              // inverse of nibblize
uint32_t  denibblize(const uint8_t* p, size_t n);

// Checksum used by bulk dumps: lower 7 bits of (sum of bytes) negated, i.e.
// the value that makes (sum(payloadStart..end) + checksum) & 0x7F == 0.
// `body` here is byte-count(2) + address(3) + data, exactly as on the wire.
uint8_t   bulkChecksum(const uint8_t* body, size_t len);
uint8_t   bulkChecksum(const Bytes& body);

// ---- Misc ----------------------------------------------------------------

const char* modelName(Model m);
const char* kindName(Kind k);

} // namespace mu100
