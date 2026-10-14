// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// AutomationParams.h — DAW-automatable parameters for the most-used mixer controls.
//
// The MU100 is driven entirely by XG SysEx over an emulated 31250-baud MIDI IN,
// so a DAW parameter can NOT be a direct write into the sound engine. Each one is
// a shadow of an XG address; the bridge below reconciles the two sides at
// kTickHz on the MESSAGE thread and routes every device write through the
// processor's existing coalescing throttle. Consequences that shaped the design:
//
//  * NOTHING happens on the audio thread. A host automation write is a plain
//    juce::AudioParameterInt store (one atomic); the audio thread never reads
//    editorState, never builds a SysEx message and never touches the MIDI queue.
//    That is the whole reason this is a poll rather than a parameter Listener —
//    a Listener callback lands on whichever thread the host chose, and from the
//    audio thread neither editorState (a std::map) nor sendParamChangeThrottled
//    is safe to touch.
//
//  * Automation is NOT sample-accurate. A value reaches the device on the next
//    tick (<= 1/kTickHz) plus its wire time, and intermediate values inside a
//    tick are dropped. That is inherent: the hardware's own control surface is a
//    MIDI port, and these are idempotent absolute writes, so the value the user
//    stopped on always lands.
//
//  * The wire is the scarce resource, not the CPU. 3125 bytes/s carries ~310
//    nine-byte parameter changes a second, i.e. ~10 continuously-moving
//    parameters at kTickHz. Past that the bridge backs off (see kSendLeadCeiling)
//    rather than letting the send queue run into reserveSendWindow's lead cap.
//
// Both directions are live: the host moves a parameter -> SysEx to the device;
// the editor knob / a file load / a device read-back moves the model -> the
// parameter follows, wrapped in a change gesture so a DAW in touch/latch mode
// records the move.

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <cstdint>

#include "codec/MU100State.h"

class YamahaMU100AudioProcessor;

class AutomationParams : private juce::Timer
{
public:
    explicit AutomationParams (YamahaMU100AudioProcessor& p) : owner (p) {}
    ~AutomationParams() override { stopTimer(); }

    // Number of XG parts exposed. The MU100 has 32 (two 16-channel MIDI ports).
    static constexpr int kNumParts = 32;

    // The four DRUM SETUP buffers (XGEdit's DRUMS1..DRUMS4), and the key span the
    // device stores per buffer. The span is the hardware's, taken from the drum
    // tables rather than restated here -- see createParameters.
    static constexpr int kNumDrumBuffers = 4;

    // Create and register every parameter. Call ONCE, from the processor's
    // constructor — VST3 parameter IDs and their order are part of the plugin's
    // public contract, so they must not depend on run-time state.
    void createParameters();

    // Start/stop reconciliation. Started from prepareToPlay (the parameters exist
    // from construction, but there is nothing to talk to before then).
    void start();
    void stop();

    // "Enable DAW automation" (project state; see the processor's flag). Off stops the
    // tick outright -- the parameters remain registered and visible to the host, since
    // VST3 fixes the list at construction, they just stop reconciling in either
    // direction. Re-enabling goes through start(), so the model is resynced to the
    // host BEFORE the first tick: without that, entries whose host value moved while
    // we were stopped would read as "the host moved it" and fire a burst of device
    // writes from a stale lastSeen.
    void setEnabled (bool on);
    bool isEnabled() const noexcept { return enabled; }

    // Re-baseline every parameter FROM whatever owns its value (the editor model,
    // or the processor for the plugin-only ones) without transmitting anything. Used
    // after a wholesale model replacement (project restore, file load, factory
    // seed): the model is the complete patch and the parameters are
    // a 355-value subset of it, so the model wins and the host is told the new
    // values. Without this a project reload would push all 355 parameters at the
    // device — a second of wire time — racing the patch push that is already
    // under way. Message thread.
    void resyncFromModel();

    // Does `paramName` in `table` have a host-automatable parameter behind it?
    // Answered from the SAME spec tables createParameters() builds from, so the
    // editor's "Mark DAW-automatable params" indicators cannot drift from the set
    // of parameters the DAW actually sees. Static: the editor asks about a control,
    // not about an instance, and the answer is the same for every part.
    //
    // The plugin-only controls have no address-map table, so they are asked for
    // under the pseudo-table "PLUGIN" — currently only ("PLUGIN", "HPF2"), which
    // answers for the part's HPF2 box AND for the drum-key box the same control
    // retargets to in drum-edit mode, because both now have a parameter.
    //
    // "DRUM SETUP" answers for a drum KEY parameter. It is the same question for
    // every key and every buffer, because the drum lanes are a full cross product
    // (kNumDrumBuffers x the whole key span x kDrumSpecs) with no gaps -- which is
    // exactly why a caller never has to name one.
    static bool isAutomated (const char* table, const char* paramName);

    // Are ALL FIVE of the EQ grid's band GAINs host-automatable? The Part module
    // marks the grid's GAIN ROW LABEL once rather than each of the five boxes, so it
    // needs one answer for the row; asking here keeps that answer derived from the
    // spec table, like every other marker.
    static bool isEqGainRowAutomated();

private:
    void timerCallback() override;

    // 30 Hz, matching the processor's own param-change flush: a released knob or
    // a stopped automation ramp reaches the device within ~33 ms, and a single
    // moving parameter costs ~300 bytes/s of the 3125 available.
    static constexpr int kTickHz = 30;

    // Back off from sending when this many MIDI bytes are already queued for MAME
    // and unconsumed. 128 bytes is ~41 ms of wire at 3125 bytes/s: comfortably more
    // than one tick's worth of automation plus a dense chord, so ordinary playing
    // never trips it, and low enough that a runaway automation sweep stops adding to
    // a queue the device is not keeping up with. Deliberately measured in BYTES on
    // the ring rather than as a time cursor — automation is stamped at the clock and
    // has no cursor, and this way the host's own note traffic counts too.
    static constexpr uint32_t kMaxBacklogBytes = 128;

    // Ticks a host change-gesture stays open after the last model movement, so a
    // knob drag reads as ONE gesture rather than one per tick. ~100 ms.
    static constexpr int kGestureHoldTicks = 3;

    // How often a host-driven model change is announced to the editor, in ticks.
    // The editor's response is applyDeviceState() — a full re-read of the model
    // into every panel — so this is DELIBERATELY not every tick: running an
    // automation ramp would otherwise rebuild the whole editor 30 times a second.
    // At 4 ticks the panels track automation at ~7.5 Hz, which looks live, and the
    // work only happens while automation is actually moving something AND the GUI
    // is open. The parameter itself still reaches the device at the full tick rate
    // — this throttles the DISPLAY only.
    static constexpr int kEditorNotifyTicks = 4;

    // What sits behind a parameter. Nearly all of them shadow an XG address and are
    // reconciled over the emulated MIDI wire; HPF2 is a PLUGIN-ONLY control (the
    // clean per-part high-pass, `setCleanHpfHz`) with no XG address at all, so it
    // has no address to resolve, costs no wire time, and is neither gated on the
    // wire backlog nor on the engine having booted.
    // DrumHpf is the drum-key counterpart of CleanHpf: the same plugin-only
    // high-pass, but keyed by (DRUMS buffer, key) through setDrumHpf/getDrumHpf
    // rather than by part. Like CleanHpf it has no XG address, so it costs no wire
    // time and works before the engine has booted.
    enum class Kind { XgAddress, CleanHpf, PartEqGain, DrumHpf };

    struct Entry
    {
        juce::AudioParameterInt* param { nullptr };
        Kind           kind     { Kind::XgAddress };
        int            mapIndex { -1 };                    // AddressMap param index (XgAddress only)
        int            instance { 0 };                     // XG part 0..31, or drum KEY 13..91
        int            instance2 { 0 };                    // drum BUFFER 0..3; 0 otherwise
        int            band     { -1 };                    // peaking band 0..2 (PartEqGain only)
        bool           complement { false };               // 127 - raw both ways (drum AMP EG)
        mu100::ModelIx model    { mu100::ModelIx::XG };    // resolved once
        uint32_t       addr21   { 0 };                     // resolved once
        int            lo { 0 }, hi { 127 };               // the param's OWN raw range
        int            lastSeen { -1 };                    // value both sides last agreed on
        int            gestureIdle { -1 };                 // >= 0 while a gesture is open
    };

    void endGestureIfIdle (Entry& e);

    // Read the CURRENT value of `e` from whatever owns it — the editor model for an
    // XG address, the processor for a plugin-only one. Returns false when the model
    // has no byte at that address (so the caller keeps what it has).
    bool readCurrent (const Entry& e, int& valueOut) const;

    // `style` is a Style enum value, passed as int so the enum can stay private to
    // the .cpp alongside the spec table it belongs to. `name` is what the DAW lists
    // (part number / MASTER first); `shortName` is the <= 8-character form VST3 asks
    // for separately and is also the parameter ID, which must never change.
    // `instance2` is the COARSER templated address byte, which only DRUM SETUP uses
    // (instance = the key 13..91, instance2 = the buffer 0..3). The split and the
    // order are AddressMap::resolve's own, not a second convention.
    void add (juce::AudioProcessorParameterGroup& group,
              const juce::String& shortName, const juce::String& name,
              const char* table, const char* paramName,
              int instance, int defaultRaw, int style, int instance2 = 0,
              bool complement = false);

    // The plugin-only per-part HPF2 (see Kind::CleanHpf). No table, no address.
    void addCleanHpf (juce::AudioProcessorParameterGroup& group,
                      const juce::String& shortName, const juce::String& name, int part);

    // The plugin-only per-(buffer, key) HPF2 (see Kind::DrumHpf).
    void addDrumHpf (juce::AudioProcessorParameterGroup& group,
                     const juce::String& shortName, const juce::String& name,
                     int buffer, int key);

    // One plugin-only peaking-band GAIN, EQ grid columns 2-4 (see Kind::PartEqGain).
    void addPartEqGain (juce::AudioProcessorParameterGroup& group,
                        const juce::String& shortName, const juce::String& name,
                        int part, int band);

    std::vector<Entry> entries;

    // "Enable DAW automation". Default ON, and the compiled default is what a project
    // without the attribute reads as -- see the processor's dawAutomationEnabled.
    bool enabled { true };
    int  editorNotifyCountdown { 0 };   // ticks left before announcing host-driven edits
    bool editorNotifyPending   { false };
    YamahaMU100AudioProcessor& owner;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationParams)
};
