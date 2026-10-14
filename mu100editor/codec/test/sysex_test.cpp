// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// sysex_test.cpp — standalone tests for mu100::MU100SysEx.
//
// Build: codec/test/build.ps1  (drives cl.exe via vcvars).  Run: sysex_test.exe
//
// The strongest test here is the byte-exact round-trip of the real .syx
// fixtures in reference/syx examples/: parse -> re-encode -> compare to the
// original file bytes.  Passing it proves our encoder emits exactly what
// XGEdit and the MU100 hardware emit, which is the file-compatibility goal.

#define _CRT_SECURE_NO_WARNINGS
#include "../MU100SysEx.h"
#include "../MU100State.h"
#include "../MU100AddressMap.h"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <map>

using namespace mu100;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { if (cond) { ++g_pass; } else { \
    ++g_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while(0)

static bool readFile(const std::string& path, Bytes& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(n));
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

static std::string hex(const Bytes& b) {
    std::string s; char t[4];
    for (uint8_t x : b) { std::snprintf(t, sizeof t, "%02X ", x); s += t; }
    return s;
}

// ---- unit tests ----------------------------------------------------------

static void test_nibblize() {
    std::printf("[nibblize]\n");
    CHECK(nibblize(0x1234, 4) == (Bytes{0x01,0x02,0x03,0x04}), "nibblize 0x1234/4");
    CHECK(nibblize(0, 4) == (Bytes{0,0,0,0}), "nibblize 0/4");
    CHECK(denibblize(Bytes{0x01,0x02,0x03,0x04}) == 0x1234u, "denibblize roundtrip");
    // MASTER TUNE example: 0x0400 -> {0,4,0,0}; center 0x0400 == "0 cent" per map default 00 04 00 00
    CHECK(nibblize(0x0400, 4) == (Bytes{0,4,0,0}), "nibblize master-tune center");
    for (uint32_t v = 0; v <= 0xFFFF; v += 7)
        if (denibblize(nibblize(v, 4)) != v) { CHECK(false, "nibblize roundtrip sweep"); break; }
}

static void test_checksum() {
    std::printf("[checksum]\n");
    // (sum(body) + checksum) & 0x7F == 0
    Bytes body{0x00,0x07, 0x00,0x00,0x00, 1,2,3,4,5,6,7};
    uint8_t ck = bulkChecksum(body);
    unsigned s = 0; for (uint8_t x : body) s += x; s += ck;
    CHECK((s & 0x7F) == 0, "checksum makes low7 == 0");
}

static void test_encoders_match_spec() {
    std::printf("[encoders vs spec]\n");
    CHECK(encodeXGSystemOn(0) == (Bytes{0xF0,0x43,0x10,0x4C,0x00,0x00,0x7E,0x00,0xF7}),
          "XG System On bytes");
    CHECK(encodeGMSystemOn() == (Bytes{0xF0,0x7E,0x7F,0x09,0x01,0xF7}), "GM System On bytes");
    CHECK(encodeIdentityRequest() == (Bytes{0xF0,0x7E,0x7F,0x06,0x01,0xF7}), "Identity Request bytes");
    // XG dump request for the XG System block (00 00 00):
    CHECK(encodeDumpRequest(Model::XG, 0, 0,0,0) == (Bytes{0xF0,0x43,0x20,0x4C,0,0,0,0xF7}),
          "XG dump request bytes");
    // Parameter change for MASTER VOLUME (00 00 04) = 7F:
    CHECK(encodeParamChange(Model::XG, 0, 0,0,0x04, Bytes{0x7F})
          == (Bytes{0xF0,0x43,0x10,0x4C,0,0,0x04,0x7F,0xF7}), "param change bytes");
}

static void test_xgsystemon_classified() {
    std::printf("[XG System On classification]\n");
    auto ms = parseStream(encodeXGSystemOn(0));
    CHECK(ms.size() == 1 && ms[0].kind == Kind::XGSystemOn, "XG System On round classifies");
}

// ---- fixture tests -------------------------------------------------------

struct Expect { const char* file; int messages; int paramChg; int bulk; int xgOn; bool allCksumOk; };

static void test_fixture(const std::string& dir, const Expect& e) {
    std::string path = dir + "/" + e.file;
    Bytes raw;
    if (!readFile(path, raw)) { std::printf("[%s] SKIP (not found)\n", e.file); return; }
    std::printf("[%s] %zu bytes\n", e.file, raw.size());

    auto ms = parseStream(raw);
    CHECK((int)ms.size() == e.messages, "message count");

    int pc = 0, bd = 0, xg = 0; bool ck = true;
    std::map<std::string,int> kinds;
    for (auto& m : ms) {
        kinds[kindName(m.kind)]++;
        if (m.kind == Kind::ParamChange) ++pc;
        if (m.kind == Kind::BulkDump) { ++bd; if (!m.checksumOk) ck = false; }
        if (m.kind == Kind::XGSystemOn) ++xg;
        CHECK(m.kind != Kind::Unknown, "no Unknown messages");
    }
    CHECK(pc == e.paramChg, "param-change count");
    CHECK(bd == e.bulk, "bulk-dump count");
    CHECK(xg == e.xgOn, "XG-System-On count");
    CHECK(ck == e.allCksumOk, "all bulk checksums valid");

    // Per-message byte-exact re-encode.
    bool perMsgOk = true;
    for (size_t i = 0; i < ms.size(); ++i) {
        Bytes re = encode(ms[i]);
        if (re != ms[i].raw) {
            perMsgOk = false;
            std::printf("    msg %zu re-encode mismatch:\n      orig: %s\n      enc:  %s\n",
                        i, hex(ms[i].raw).c_str(), hex(re).c_str());
            break;
        }
    }
    CHECK(perMsgOk, "every message re-encodes byte-exact");

    // Whole-file byte-exact reconstruction (concatenate re-encoded messages).
    // Valid only when the file is purely back-to-back F0..F7 with no padding,
    // which all our fixtures are.
    Bytes rebuilt;
    for (auto& m : ms) { Bytes re = encode(m); rebuilt.insert(rebuilt.end(), re.begin(), re.end()); }
    CHECK(rebuilt == raw, "whole-file reconstruction byte-exact");

    std::printf("    kinds:");
    for (auto& kv : kinds) std::printf(" %s=%d", kv.first.c_str(), kv.second);
    std::printf("\n");
}

// ---- state-model + address-map tests -------------------------------------

static void test_state_roundtrip(const std::string& dir) {
    std::printf("[DeviceState round-trip]\n");
    Bytes raw;
    if (!readFile(dir + "/all-factory-default.syx", raw)) {
        std::printf("  SKIP (factory dump not found)\n"); return;
    }
    DeviceState st;
    auto lr = st.load(raw);
    CHECK(lr.badChecksum == 0, "factory dump: all checksums valid on load");
    CHECK(st.size() > 1000, "factory dump populated many addresses");
    std::printf("  loaded %d msgs, %d bytes into %zu addresses\n", lr.messages, lr.bytes, st.size());

    // Every data byte of every original bulk message must be in the state.
    bool allPresent = true;
    auto msgs = parseStream(raw);
    for (auto& m : msgs) {
        if (m.kind != Kind::BulkDump) continue;
        ModelIx ix; if (!modelToIx(m.model, ix)) continue;
        for (size_t k = 0; k < m.data.size(); ++k) {
            uint8_t b;
            if (!st.getByte(ix, m.address() + (uint32_t)k, b) || b != m.data[k]) { allPresent = false; break; }
        }
        if (!allPresent) break;
    }
    CHECK(allPresent, "every original bulk byte is recoverable from state");

    // Re-serialise to bulk dumps, reload, and require an identical state map.
    Bytes reser = st.toBulkDumps(0);
    DeviceState st2; st2.load(reser);
    CHECK(st2.raw() == st.raw(), "state survives toBulkDumps -> reload");

    // Same via param-change stream (XGEdit-style export).
    Bytes pcs = st.toParamChanges(0);
    DeviceState st3; st3.load(pcs);
    CHECK(st3.raw() == st.raw(), "state survives toParamChanges -> reload");
}

static void test_xgedit_into_state(const std::string& dir) {
    std::printf("[XGEdit stream -> state -> known params]\n");
    Bytes raw;
    if (!readFile(dir + "/echotech_2026_v2.syx", raw)) { std::printf("  SKIP\n"); return; }
    DeviceState st; auto lr = st.load(raw);
    CHECK(lr.sawXGSystemOn, "XGEdit file begins with XG System On");
    CHECK(st.size() > 50, "XGEdit stream populated state");
    // Reverb type was written to 02 01 00 in the file (data 01 01).
    // One 2-byte parameter, not two: the manual prints its MSB and LSB on
    // separate lines but they share the single address 02 01 00.
    int idx = AddressMap::find("REVERB TYPE", "EFFECT 1");
    CHECK(idx >= 0, "found REVERB TYPE in map");
    if (idx >= 0) {
        auto v = AddressMap::get(st, idx, 0);
        CHECK(v.present && v.bytes.size() == 2, "reverb type present, 2 bytes");
    }
}

static void test_addressmap_factory(const std::string& dir) {
    std::printf("[AddressMap vs factory defaults]\n");
    Bytes raw;
    if (!readFile(dir + "/all-factory-default.syx", raw)) { std::printf("  SKIP\n"); return; }
    DeviceState st; st.load(raw);

    struct Known { const char* table; const char* name; int inst; int expect; };
    const Known ks[] = {
        {"XG SYSTEM", "MASTER VOLUME",   0, 0x7F},
        {"XG SYSTEM", "MASTER TUNE",     0, 0x0400},   // nibblized center
        {"MULTI EQ",  "EQ GAIN1",        0, 0x40},
        {"MULTI PART","PAN",             0, 0x40},     // part 1 pan center
        {"MULTI PART","PAN",             4, 0x40},     // part 5 pan center (template nn)
        {"MULTI PART","DETUNE",          0, 0x80},     // nibble-packed (08 00) -> 0x80 = 0.0 Hz centre
    };
    for (auto& k : ks) {
        int idx = AddressMap::find(k.name, k.table);
        CHECK(idx >= 0, k.name);
        if (idx < 0) continue;
        auto v = AddressMap::get(st, idx, k.inst);
        bool ok = v.present && v.combined == k.expect;
        if (!ok) std::printf("    %s[%d] = present:%d combined:0x%X (want 0x%X)\n",
                             k.name, k.inst, v.present, v.combined, k.expect);
        CHECK(ok, k.name);
    }

    // identify() should map a param-change message back to its param + instance.
    int inst = -1;
    int pidx = AddressMap::find("PAN", "MULTI PART");
    if (pidx >= 0) {
        Bytes pc = AddressMap::makeParamChange(AddressMap::param(pidx), 6, 0x40, 0);
        auto ms = parseStream(pc);
        CHECK(ms.size() == 1, "makeParamChange emits one message");
        int id = AddressMap::identify(ms[0], inst);
        CHECK(id == pidx && inst == 6, "identify recovers param + instance (part 7)");
    }

    // set/get round-trip through state.
    int vol = AddressMap::find("VOLUME", "MULTI PART");
    if (vol >= 0) {
        AddressMap::set(st, AddressMap::param(vol), 3, 100);
        auto v = AddressMap::get(st, vol, 3);
        CHECK(v.present && v.combined == 100, "set/get round-trip (part 4 volume=100)");
    }

    // DETUNE is nibble-packed (2 bytes, low nibble each): 200 -> {0x0C,0x08} -> 200.
    int det = AddressMap::find("DETUNE", "MULTI PART");
    if (det >= 0) {
        Bytes b = AddressMap::set(st, AddressMap::param(det), 2, 200);
        auto v = AddressMap::get(st, det, 2);
        CHECK(b == (Bytes{0x0C, 0x08}), "DETUNE encodes to nibble pair");
        CHECK(v.present && v.combined == 200, "DETUNE set/get round-trip (part 3)");
    }
}

// Pin the (name, table) -> address pairs that the PLUGIN looks up by name, and
// the effect-routing addresses that were wrong before 2026-07-30.
//
// data/parameters.json is regenerated from the PDF by tools/extract_parameters.py.
// A regeneration that shifts an address or re-words a name breaks the plugin
// SILENTLY — AddressMap::find just returns -1 and the control goes inert. These
// checks are the tripwire for that. If one fails after a re-extract, confirm the
// new value against the manual before changing the expectation here.
static void test_known_addresses() {
    std::printf("[known name -> address pins]\n");
    struct Pin { const char* table; const char* name; int hi, mid, lo; };
    const Pin pins[] = {
        // Renamed by the 2026-07-30 re-extract: the manual wraps this name onto a
        // second line, so it is "SAME NOTE NUMBER **KEY ON ASSIGN**". PartPanel's
        // ASSIGN box looks it up by this exact string.
        {"MULTI PART", "SAME NOTE NUMBER KEY ON ASSIGN", 0x08, 0x00, 0x06},

        // Effect routing — absent or duplicated in the pre-2026-07-30 data.
        // These are what a part-scoped effect indicator would have to parse.
        {"EFFECT 1", "VARIATION CONNECTION",          0x02, 0x01, 0x5A},
        {"EFFECT 1", "VARIATION PART NUMBER",         0x02, 0x01, 0x5B},
        {"EFFECT 2", "INSERTION EFFECT1 PART NUMBER", 0x03, 0x00, 0x0C},
        {"EFFECT 2", "INSERTION EFFECT2 PART NUMBER", 0x03, 0x01, 0x0C},
        // 0x0D is NOT the part number — the old data listed it twice.
        {"EFFECT 2", "MW INSERTION CONTROL DEPTH",    0x03, 0x00, 0x0D},

        // Variation block stride: params are 2 bytes apart from 0x42, matching the
        // plugin's independent fxaddr (Source/EffectAddressing.h). The old data had
        // these at 0x44+ because it split each MSB/LSB pair into two entries.
        {"EFFECT 1", "VARIATION PARAMETER 1",  0x02, 0x01, 0x42},
        {"EFFECT 1", "VARIATION PARAMETER 10", 0x02, 0x01, 0x54},

        // A few names the plugin looks up, spread across the tables it uses.
        {"MULTI PART", "ELEMENT RESERVE",  0x08, 0x00, 0x00},
        {"MULTI PART", "PART MODE",        0x08, 0x00, 0x07},
        {"MULTI PART", "BANK SELECT MSB",  0x08, 0x00, 0x01},
        {"MULTI PART", "BANK SELECT LSB",  0x08, 0x00, 0x02},
        {"DRUM SETUP", "ALTERNATE GROUP",  0x30, 0x00, 0x03},
    };
    for (auto& p : pins) {
        int idx = AddressMap::find(p.name, p.table);
        CHECK(idx >= 0, p.name);
        if (idx < 0) continue;
        ModelIx m; uint32_t addr = 0;
        AddressMap::resolve(AddressMap::param(idx), 0, m, addr);
        const int hi  = (int) ((addr >> 14) & 0x7F);
        const int mid = (int) ((addr >> 7)  & 0x7F);
        const int lo  = (int) (addr & 0x7F);
        const bool ok = hi == p.hi && mid == p.mid && lo == p.lo;
        if (!ok)
            std::printf("    %-32s got %02X %02X %02X, want %02X %02X %02X\n",
                        p.name, hi, mid, lo, p.hi, p.mid, p.lo);
        CHECK(ok, p.name);
    }
}

static void test_drum_setup() {
    std::printf("[Drum Setup 2-D addressing]\n");
    int idx = AddressMap::find("PAN", "DRUM SETUP");
    CHECK(idx >= 0, "found DRUM SETUP PAN");
    if (idx < 0) return;
    const ParamDef& p = AddressMap::param(idx);

    // resolve(note=0x24 kick, setup=0): expect high 0x30, mid 0x24, low 0x04.
    ModelIx model; uint32_t addr;
    AddressMap::resolve(p, 0x24, 0, model, addr);
    CHECK(model == ModelIx::XG, "drum setup is XG model");
    CHECK(addr == ((0x30u << 14) | (0x24u << 7) | 0x04u), "resolve setup0 note0x24 PAN -> 30/24/04");

    // setup number lands in the high nibble (3n): setup=2 -> high 0x32.
    AddressMap::resolve(p, 0x24, 2, model, addr);
    CHECK(((addr >> 14) & 0x7F) == 0x32, "setup#2 -> high 0x32");

    // makeParamChange + identify round-trip recovers param + note + setup.
    Bytes pc = AddressMap::makeParamChange(p, 0x24 /*note*/, 0x40 /*pan center*/, 3 /*setup*/, 0);
    auto ms = parseStream(pc);
    CHECK(ms.size() == 1, "drum-setup param change emits one message");
    int inst = -1, inst2 = -1;
    int id = AddressMap::identify(ms[0], inst, inst2);
    CHECK(id == idx && inst == 0x24 && inst2 == 3,
          "identify recovers DRUM SETUP PAN, note 0x24, setup 3");

    // set/get round-trip through state for a specific note+setup.
    DeviceState st;
    AddressMap::set(st, p, 0x30 /*note*/, 100, 1 /*setup*/);
    auto v = AddressMap::get(st, idx, 0x30, 1);
    CHECK(v.present && v.combined == 100, "drum-setup set/get round-trip (note0x30 setup1)");
    // a different note must be independent (not written).
    auto v2 = AddressMap::get(st, idx, 0x31, 1);
    CHECK(!v2.present, "neighbouring drum note is independent");
}

int main(int argc, char** argv) {
    std::string dir = (argc > 1) ? argv[1] : "../../reference/syx examples";
    std::printf("=== MU100 SysEx codec tests ===\nfixture dir: %s\n\n", dir.c_str());

    test_nibblize();
    test_checksum();
    test_encoders_match_spec();
    test_xgsystemon_classified();

    // Expected shapes from tools/analyze_syx.py (2026-06-22 captures).
    const Expect fixtures[] = {
        // file,                              msgs, paramChg, bulk, xgOn, allCksumOk
        {"echotech_2026_v2.syx",               243,      242,    0,    1,  true},
        {"perf-001-2026-06-22-1606.syx",        15,        0,   15,    0,  true},
        {"mult-32-2026-06-22-1604.syx",       1441,        0, 1441,    0,  true},
        {"perf-all-2026-06-22-1605.syx",      1504,        0, 1504,    0,  true},
        {"all-2026-06-22-1603.syx",           2945,        0, 2945,    0,  true},
    };
    for (auto& f : fixtures) test_fixture(dir, f);

    test_state_roundtrip(dir);
    test_xgedit_into_state(dir);
    test_addressmap_factory(dir);
    test_known_addresses();
    test_drum_setup();

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
