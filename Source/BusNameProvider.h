// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

// BusNameProvider — narrow, dependency-free interface that lets the (patched) JUCE
// VST3 wrapper ask the AudioProcessor for a *dynamic* output bus name, and tell the
// host when those names have changed.
//
// Why a patch is needed at all: JUCE fixes a bus name at construction
// (AudioProcessor::Bus::name has no public setter) and its ChangeDetails carries only
// latency/parameterInfo/program/nonParameterState — there is no bus-name flag, so
// audioProcessorChanged() can never emit Vst::kIoTitlesChanged, the VST3 flag meaning
// "bus title has changed, re-query getBusInfo". The constant exists in the SDK and
// Cubase honours it (Groove Agent SE renames an ALREADY-CREATED output track when a
// pad is assigned to an output), so this is purely a gap in JUCE's exposure of it.
//
// The wrapper reads the name live on every getBusInfo() call and caches nothing, so
// returning a different string here is all that is required to make a bus name change.
//
// Guarded by JucePlugin_MU100_DynamicBusNames, defined ONLY in the MU100 project, so
// SQ-1 / MU128 compile the stock wrapper unchanged.
//
// Deliberately takes a caller-provided char buffer rather than a juce::String, so this
// header stays free of JUCE include-order assumptions inside the module translation
// unit — the same discipline as MidiBusSink.h.

struct BusNameProvider
{
    virtual ~BusNameProvider() = default;

    // Fill `dest` with a UTF-8, NUL-terminated name for output bus `busIndex`
    // (0 = the main stereo out, 1..32 = the per-part aux buses). Write at most
    // destBytes-1 bytes plus the terminator. Return false to leave the bus's static
    // construction-time name in place.
    //
    // Called from getBusInfo(), which the host may invoke from a thread of its
    // choosing, so implementations must not touch message-thread-owned state
    // directly — serve it from a snapshot.
    virtual bool getDynamicOutputBusName (int busIndex, char* dest, int destBytes) const = 0;

    // Returns true (and clears the flag) if the names have changed since the last
    // call. Polled from audioProcessorChanged(); when true the wrapper ORs in
    // Vst::kIoTitlesChanged so the host re-queries getBusInfo().
    virtual bool consumeBusNamesDirty() = 0;
};
