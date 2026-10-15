// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// EditUndo.h — the record types behind the editor's Undo/Redo.
//
// An undo step is a DELTA, never a snapshot. The editor model (mu100::DeviceState)
// is a map of 7-bit bytes keyed by (model, 21-bit address), so "what an edit did" is
// fully described by the handful of bytes it touched and what they held before. That
// keeps a step at ~60 bytes (worst case ~400, an effect-type change and its 16
// defaults) and the whole 20-deep stack under 10 KB.
//
// Deliberately JUCE-free and header-only, for two reasons: nothing here needs the
// framework, and the merge policy — the rule that decides whether a knob drag is one
// step or two hundred — is exactly the kind of thing that wants an off-tree unit test
// (the cl-through-vcvars64 idiom already used for TempoSync.h and FreqLadder.h).
//
// NOT persisted. The history is session-lifetime and in-memory only: project state is
// already ~250-290 KB (128 KB of work RAM plus the editor model, both base64), and a
// partially-restored history would be worse than none.

#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mu100undo {

// How deep the stack goes. Cheap; the number is about how far back a user can
// usefully reason, not about memory.
inline constexpr int kDepth = 20;

// Successive writes to the SAME address within this window collapse into one step,
// so a drag undoes to where it started rather than one mouse-pixel back.
inline constexpr double kCoalesceMs = 700.0;

// One byte of the device model. `stateKey` is mu100::DeviceState::key(ix, addr21),
// so the model index travels with the address and nothing downstream re-derives it.
// -1 means "the model held no byte at this address", which is distinct from 0.
struct ByteDelta
{
    uint32_t stateKey = 0;
    int      oldVal   = -1;
    int      newVal   = 0;
};

// The plugin-only parameters — HPF2 and the three peaking EQ mid bands, at part level
// and per drum key. These have no XG address at all (they run inside swp30 and are
// excluded from .syx), so they cannot be expressed as a ByteDelta.
enum class PluginField
{
    CleanHpf,                                   // a = part
    SystemHpf,                                  // no index: one system-wide value
    PartEqGain, PartEqFreq, PartEqQ,            // a = part,   b = band
    DrumHpf,                                    // a = buffer, b = key
    DrumEqGain, DrumEqFreq, DrumEqQ,            // a = buffer, b = key, c = band
    // Effect tempo-SYNC: a = unit, b = effect-type index, c = slot 0..15 (value = the
    // note index, -1 = none) or kEffectSyncOnSlot (value = SYNC on 0/1). Recorded
    // beside the raw ms/Hz the same action sends, so an undo puts back the NOTE and not
    // only the time -- the time alone is meaningless once the tempo has moved.
    EffectSync
};
constexpr int kEffectSyncOnSlot = 16;

// An explicit tuple rather than a std::function pair: no capture-lifetime question
// (the panel that made the edit may be long destroyed by the time undo runs), and it
// prints in a debugger.
struct PluginDelta
{
    PluginField field {};
    int    a = 0, b = 0, c = 0;
    double oldVal = 0.0, newVal = 0.0;
};

// One user action. `msgs` holds the NEW messages in send order; the OLD messages are
// reconstructed at apply time by cloning these and substituting the data bytes, which
// guarantees identical framing and sidesteps re-deriving the NIBBLE/BYTEPAIR encoding.
struct Step
{
    std::vector<ByteDelta>            bytes;
    std::vector<PluginDelta>          plugin;
    std::vector<std::vector<uint8_t>> msgs;

    // DRUM SETUP buffers whose whole 3n block must be RE-DERIVED after this step is
    // applied, in either direction. A kit change rewrites all 79 keys x 34 bytes of a
    // buffer, and recording that as ByteDeltas would be 2686 of them -- 32 KB for one
    // step, against a design budget of 10 KB for the entire 20-deep stack.
    //
    // It does not need recording, because it is DERIVABLE: undo restores the part's
    // bank and program bytes anyway, so the kit on the buffer is known afterwards and
    // the defaults come from MU100DrumDefaults.h. Storing the buffer number alone is
    // both cheaper and MORE correct than storing the bytes -- the device re-inits to
    // the restored kit's defaults when undo re-sends the program change, so a model
    // rebuilt from the same table matches it exactly, where restoring the user's
    // pre-change per-key edits would NOT (the device cannot bring those back; the
    // hardware destroys them on a kit change, which is the behaviour being modelled).
    // Undoing further back still replays those edits one ByteDelta at a time, onto
    // both halves, so nothing is lost by not duplicating them here.
    std::vector<int>                  drumReseed;

    // The bytes in a reseeded buffer that were NOT at the outgoing kit's default --
    // i.e. the user's own per-key edits, which the reseed above would otherwise
    // destroy. Applied AFTER the reseed in both directions, so they survive it.
    //
    // These DO have to be stored: they are the one thing in the block that is not
    // derivable from the table. They are also cheap, because there are only ever as
    // many as the user actually edited -- none at all on an untouched kit. Each is
    // re-transmitted as a single-byte parameter change when applied, which is what
    // keeps the DEVICE in step: undo re-sends the program change, so the device
    // re-inits to the restored kit's DEFAULTS and would otherwise sit one edit behind
    // the model.
    std::vector<ByteDelta>            drumEdits;

    // Identity for coalescing: the XG address key, or a synthesised key for a
    // plugin-only field. 0 means "never merge into this" — set by a multi-message
    // scope, so an effect-type change can't absorb the next unrelated edit.
    uint32_t mergeKey    = 0;
    bool     sealed      = false;
    double   lastTouchMs = 0.0;

    // Label INPUTS, not a label. Formatting is deferred to the moment a tooltip is
    // actually read (~4 Hz) because a merge happens on every onChange of a drag —
    // building a string 200 times a second would be the only real cost in the feature.
    int mapIndex  = -1;      // mu100::AddressMap index, -1 = unidentified
    int instance  = 0;       // part / drum key
    int instance2 = 0;       // drum buffer
    int labelOld  = 0;
    int labelNew  = 0;
    std::string explicitLabel;   // set by an UndoScope; wins over the lazy format
};

// A multi-message GESTURE's coalescing key. Successive gestures of the same kind on
// the same target fold together, so scrolling the wheel through effect programs is
// one undo step rather than one per detent — while a *different* edit arriving next
// still cannot fold in, because its key differs. Bit 30, so it collides with neither
// a paramChangeKey (28 bits) nor a pluginMergeKey (bit 31).
enum GestureKind { GestureEffectType = 1, GestureMultiEqProgram = 2, GestureVoicePick = 3,
                   GestureSyncNote = 4 };   // target = unit * 16 + slot

inline uint32_t gestureMergeKey (GestureKind kind, int target)
{
    return 0x40000000u
         | ((static_cast<uint32_t> (kind) & 0xFF) << 16)
         |  (static_cast<uint32_t> (target) & 0xFFFF);
}

// Plugin-only fields share the coalescing machinery with XG addresses, so they need a
// key in the same space. A paramChangeKey is msg[3]<<21 | ... — 28 bits, so bit 31 is
// always clear there and setting it here makes a collision impossible.
inline uint32_t pluginMergeKey (PluginField f, int a, int b, int c)
{
    uint32_t k = 0x80000000u;
    k |= (static_cast<uint32_t> (f) & 0xF) << 24;
    k |= (static_cast<uint32_t> (a) & 0xFF) << 16;
    k |= (static_cast<uint32_t> (b) & 0xFF) << 8;
    k |=  static_cast<uint32_t> (c) & 0xFF;
    return k;
}

// "The user released the mouse." Set by the shared controls (ValueBox, XgKnob's dial)
// and consumed by the next commit, which seals the open step first — so releasing the
// button ends a step crisply instead of kCoalesceMs later.
//
// A flag rather than a callback into the processor, because these controls are generic
// and hold no processor reference. Deliberately safe when a host runs several plugin
// instances in one process: the flag says only that a mouse was released, and sealing a
// stack with no drag in progress is a no-op. A shared *callback* would let one
// instance's control reach into another's history.
//
// Message thread only, like everything else in this feature.
inline bool gSealRequest = false;

inline void requestSeal() { gSealRequest = true; }

// Should an incoming edit with this key fold into `top` rather than open a new step?
//
// Note what is NOT consulted: which control sent it, or whether a mouse button is
// down. Drag, wheel burst, right-click reset and type-in all write the same address,
// so keying on the address alone makes every one of them coalesce with no per-control
// code — and a control added later inherits the behaviour for free.
inline bool shouldMerge (const Step& top, uint32_t key, double nowMs)
{
    return ! top.sealed
        && top.mergeKey != 0
        && top.mergeKey == key
        && (nowMs - top.lastTouchMs) < kCoalesceMs;
}

// Fold a new value for the same address into an existing step. `oldVal` is
// deliberately untouched: the step must always revert to the value that was there
// when the gesture STARTED, however many intermediate values it passed through.
inline void mergeBytes (Step& top, const std::vector<ByteDelta>& incoming)
{
    for (const auto& in : incoming)
        for (auto& have : top.bytes)
            if (have.stateKey == in.stateKey)
            {
                have.newVal = in.newVal;
                break;
            }
}

} // namespace mu100undo
