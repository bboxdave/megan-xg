// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

// MAME macros must be defined before any MAME includes
#ifndef PTR64
#define PTR64 1
#define LSB_FIRST 1
#define NDEBUG 1
#define __STDC_LIMIT_MACROS 1
#define __STDC_FORMAT_MACROS 1
#define __STDC_CONSTANT_MACROS 1
#endif

#include <JuceHeader.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <memory>
#include <vector>
#include <deque>
#include <set>
#include <cstdint>
#include <array>

#include "emu.h"
#include "mame.h"
#include "MidiBusSink.h"
#include "BusNameProvider.h"
#include "EditUndo.h"
#include "KeyboardShortcuts.h"
#include "MidiOverrideMarks.h"
#include "ChangedMarks.h"
#include "NoteLabel.h"

namespace mu100 {
    class DeviceState;
    // Declared here, not included: this header is reached from the MAME side of the
    // build and must not pull the codec in. A scoped enum's underlying type is int by
    // default, so naming it makes this a compatible redeclaration of the definition in
    // codec/MU100State.h rather than a second, different type.
    enum class ModelIx : int;
}
class AutomationParams;

//==============================================================================

class YamahaMU100AudioProcessor : public juce::AudioProcessor,
                                  public MidiBusSink,
                                  public BusNameProvider
{
public:
    YamahaMU100AudioProcessor();
    ~YamahaMU100AudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // --- Shared state serializer (DAW project state + the native "complete setup" file) ---
    // buildStateXml() produces the YamahaMU100State tree. `includeSession` adds the
    // per-app / per-session items (romPath, PDC/buffer, loaded-file marker, MIDI routing)
    // and `includeWorkram` the 128 KB work-RAM snapshot — both TRUE for DAW project state,
    // both FALSE for the portable .mss file, which carries only the patch + the plugin-only
    // extensions (HPF2, mid-band/drum EQ, effect tempo-SYNC + TEMPO). getStateInformation
    // is buildStateXml(true,true); saveSetupToFile() is buildStateXml(false,false).
    std::unique_ptr<juce::XmlElement> buildStateXml(bool includeSession, bool includeWorkram);
    // Apply the plugin-only extension attrs (HPF2 / part-EQ / drum-EQ / tempo-SYNC) from a
    // state tree and raise the re-push dirty flags. Shared by setStateInformation (project
    // restore) and the native-file load path.
    void applyPluginExtras(const juce::XmlElement& xml);
    // Write / classify the native "complete setup" file (.mss). saveSetupToFile serializes
    // buildStateXml(false,false) as text XML; makeSetupDefaultFileName is "meganxg-<stamp>.mss".
    bool saveSetupToFile(const juce::File& file);

    //==========================================================================
    // Native setup files — one COMPLETE setup (.mss) plus three PARTIAL ones.
    //
    // All four share the "YamahaMU100State" root tag (so the Open chooser's
    // 0xF0-vs-'<' format sniff and the hasTagName check are unchanged) and are
    // told apart by a "setupKind" attribute; a file without one is a .mss from
    // before the partial formats existed, hence System is the fallback.
    //
    //   System  .mss  the whole patch + every plugin-only extension (unchanged)
    //   Part    .mps  ONE part: MULTI PART 08 nn xx + the part HPF 0A nn 20 + that
    //                 part's HPF2 / mid-band EQ — AND, when the part sits on a DRUMS
    //                 buffer, that buffer's per-key data as well
    //   Drum    .mds  the per-key layer ALONE: 3n rr xx for every key + the
    //                 per-(buffer,key) HPF2 / mid-band EQ. No part settings.
    //   Effect  .mes  all five effect units + Multi EQ as one block (02 01, 02 40,
    //                 03 00, 03 01) plus the plugin-only tempo-SYNC selections
    //
    // WHY .mps SWALLOWS THE DRUM BUFFER. A part block holds the kit's bank/program, and
    // writing a PROGRAM re-initialises the Drum Setup assigned to that part. So "load
    // this part but leave its buffer alone" is not a state that exists — the buffer
    // changes either way, and the only question is whether the file says what it should
    // become. It does. That makes a Part setup self-sufficient.
    //
    // WHY .mds IS STILL A SEPARATE THING. Carrying no program means loading one fires no
    // program change, so it is the only file that rewrites per-key data WITHOUT the
    // buffer being re-initialised first. That is what lets a drum treatment (levels,
    // tuning, pans, sends, filters, per-key EQ) move onto a different kit or another
    // part, leaving the kit and the part's channel strip untouched.
    //
    // The Part/Drum blocks are stored RE-ADDRESSED to part 0 / buffer 0, so a file
    // is agnostic about where it was saved from and loads into whatever part or buffer
    // is chosen. The Effect blocks are not indexed by anything and keep their real
    // addresses. Loading a partial file replaces only its own settings and leaves
    // the loaded-file marker alone — the model no longer matches any one file, so
    // the unsaved-changes '*' appears, which is exactly right.
    enum class SetupKind { System, Part, Drum, Effect };
    static SetupKind   setupKindOfXml(const juce::XmlElement& xml);
    static const char* setupKindExtension(SetupKind k);   // "mss" / "mps" / "mds" / "mes"
    static const char* setupKindLabel(SetupKind k);       // "System Setup" / "Part Setup" / …
    static juce::String makeSetupDefaultFileName(SetupKind k = SetupKind::System);
    // Every native extension as a chooser filter, e.g. "*.mss;*.mps;*.mds;*.mes".
    static juce::String nativeSetupFilters();

    std::unique_ptr<juce::XmlElement> buildPartSetupXml  (int part);
    std::unique_ptr<juce::XmlElement> buildDrumSetupXml  (int buffer);
    std::unique_ptr<juce::XmlElement> buildEffectSetupXml();
    static bool writeSetupXml(const juce::File& file, const juce::XmlElement& xml);

    // Fold a partial file into the model, transmit the affected block to the device and
    // return the estimated streaming time in ms — or -1 when the file cannot be applied.
    // Message thread only, like every other editorState writer.
    //
    // `targetBuffer` (-1 = whatever the file was saved with) is the DRUMS buffer the user
    // picked. For a Part setup it REWRITES the PART MODE byte, so the part and its drum
    // data land together; for a Drum setup it simply redirects the per-key block.
    int applyPartSetup  (const juce::XmlElement& xml, int part, int targetBuffer = -1);
    int applyDrumSetup  (const juce::XmlElement& xml, int targetBuffer = -1);
    int applyEffectSetup(const juce::XmlElement& xml);

    // The DRUMS buffer a file targets by default: a .mds states it outright, a .mps says
    // it through the PART MODE byte in its own part block. -1 = no buffer involved.
    static int  drumBufferOfSetupXml(const juce::XmlElement& xml);
    // True when loading this file will write a DRUMS buffer — i.e. when the user should
    // be asked which one, because every part on that buffer is affected.
    static bool setupTargetsDrumBuffer(const juce::XmlElement& xml);
    // The DRUMS buffer a part sits on per the MODEL's PART MODE (2..5 = DRUMS1..4); -1
    // if melodic or a kit with no buffer. The processor-side twin of
    // PartListPanel::getPartBuffer, usable with the GUI closed.
    int  partDrumBufferFromModel(int part) const;
    // Shared tail of every apply*Setup: clears undo history (a record is an ABSOLUTE
    // address and would corrupt the loaded setup), re-baselines the DAW parameters and
    // announces the model move to the editor.
    void finishSetupApply();

    // --- Part copy / paste / reset --------------------------------------------------
    // The .mps trio without the file. What they carry is exactly what a .mps carries --
    // including the DRUMS buffer when the part sits on one, because the two are not
    // separable (see the .cpp). Unlike a setup LOAD these are EDITS: each is one undo
    // step, so none of them may go near finishSetupApply().
    //
    // The clipboard is per-INSTANCE and session-only: nothing is written to settings.xml
    // or to project state, and a second plugin instance has its own.
    struct PartPluginExtras
    {
        float hpf2 = 0.0f;                 // HPF2 in Hz, 0 = Off
        int   eqGain[3] { 64, 64, 64 };    // mid bands 2-4, raw
        int   eqFreq[3] { 0, 0, 0 };       // Hz; 0 = "use the band's own default"
        int   eqQ   [3] { 7, 7, 7 };
    };

    bool copyPart(int part);
    bool hasPartCopy() const { return partCopyXml != nullptr; }
    int  partCopySource() const { return partCopySrc; }   // 0-based source part, -1 = empty
    // Does pasting the clipboard write a DRUMS buffer? Same question (and the same
    // answer) setupTargetsDrumBuffer asks of a file, so the chooser can be reused.
    bool partCopyTargetsDrumBuffer() const;
    int  partCopySavedBuffer() const;                     // the copy's own buffer, or -1
    bool pastePart(int part, int targetBuffer = -1);
    bool resetPart(int part);
    // The buffer a RESET of this part would re-initialise, or -1. Non-empty means the
    // reset reaches every part on that buffer, which is what the warning is about: the
    // factory PART MODE decides it, so it is parts 10 and 26 on an untouched device.
    int  partResetDrumBuffer(int part) const;
    // 0-based parts whose PART MODE currently names `buffer`.
    std::vector<int> partsOnDrumBuffer(int buffer) const;
    // Write one DRUMS buffer into a setup tree (3n block + per-key plugin data); shared
    // by .mds and by .mps when its part sits on a buffer.
    bool writeDrumBufferToXml(juce::XmlElement& root, int buffer);
    // Fold a file's drum block into `buffer` (model + device) starting at `startAt` on
    // the MAME-time timeline; returns where the stream ended so a caller can chain.
    double applyDrumBufferFromXml(const juce::XmlElement& xml, int buffer, double startAt);
    // A kit on a DRUMS buffer belongs to the BUFFER: copy `part`'s bank/program to every
    // other part on the same buffer, in the MODEL only (the device re-points them itself).
    // Without this their Voice rows kept naming a kit they no longer play until a
    // Read from Device. EditorView's voice-browser pick applies the identical rule.
    void propagateDrumKitToBufferPeers(int part, int buffer);

    //==============================================================================
    // MAME bridge
    void bootMameIfNeeded();   // deferred MAME launch, gated to fire once
    void runMameEngine();
    void pushAudioFromMame(const int16_t* pcmBuffer, int numSamples);
    void pushMidiByte(uint8_t data, double targetMameTime);        // → port 0 (convenience)
    void pushMidiByteToPort(int port, uint8_t data, double targetMameTime);
    // Enqueue ONE whole MIDI message atomically: no other producer thread can place a
    // byte inside it (see the MidiRing comment). Byte i is stamped
    // startTime + i*timePerByte. Returns false only if the ring is full, in which case
    // the message is dropped whole rather than delivered truncated. Prefer this to
    // pushMidiByteToPort() for anything longer than a byte.
    bool pushMidiMessageToPort(int port, const uint8_t* data, int numBytes,
                               double startTime, double timePerByte);
    // Enqueue a byte stream that may contain several complete SysEx messages, each
    // reserved separately so none can be split. Bytes outside an F0…F7 message go one
    // at a time. `bulkGap` (seconds) is added after each Yamaha BULK DUMP message, whose
    // checksum-and-write the firmware must finish before the next arrives. Returns the
    // advanced timestamp.
    double pushSysExStream(int port, const uint8_t* data, size_t size,
                           double startTime, double timePerByte, double bulkGap = 0.0);
    // Stream a .syx file → MIDI IN A; returns est. ms until consumed. If
    // drumSetupsOut is non-null it receives the DRUM SETUPs (0..3) the file assigns
    // via PART MODE, so a follow-up read-back can capture their contents in one pass.
    int  loadSysExFile(const juce::File& syxFile, std::vector<int>* drumSetupsOut = nullptr);
    // Stream a SysEx message → MIDI IN A (editor param changes). Returns false when it
    // could NOT be transmitted (the engine is down and there is no lease to take) — a
    // caller that also records the edit somewhere is then knowingly out of step with
    // the device. See sendParamChange.
    bool sendSysEx(const std::vector<uint8_t>& msg);
    // MIDI bytes reserved but not yet consumed by MAME on `port`. The throttle signal
    // for clock-stamped senders, which have no queue-tail cursor to read.
    uint32_t midiRingBacklog(int port = 0) const;
    // Send an NRPN (CC99/98/06 + RPN-null) for a given XG part. Used for part
    // parameters that have no bulk-SysEx address (e.g. melodic-part HPF cutoff).
    // Routes to the part's port/channel under the default part→channel mapping:
    // parts 1-16 → MIDI IN A (ch 1-16), parts 17-32 → MIDI IN B (ch 1-16).
    void sendNRPN(int part, uint8_t nrpnMSB, uint8_t nrpnLSB, uint8_t value);
    // Send a channel-voice message (CC / pitch-bend / after-touch) to a part's
    // channel, routed by the same default part→port/channel mapping as sendNRPN.
    // `status` is the high nibble (0xB0 CC, 0xE0 bend, 0xD0 channel-AT, 0xA0
    // poly-AT); the channel nibble is OR'd in. Pass d2 = -1 for one-data-byte
    // messages (channel after-touch). Used by the Controller module's wheel.
    void sendChannelMessage(int part, uint8_t status, uint8_t d1, int d2 = -1);

    // Raw Rcv CHANNEL for a part: 0..15 = port A ch 1..16, 16..31 = port B,
    // -1 = Rcv OFF. The ONE place that answers "which channel is this part on",
    // asked by both directions -- the editor's own sends and the resolution of an
    // incoming channel message. Message thread only (it reads the editor model).
    int  partRcvChannelRaw(int part) const;
    // MIDI panic: All Sound Off + All Notes Off, plus Hold and Sostenuto released, on
    // every channel of both ports. Clears stuck notes and the stuck pedals that would
    // re-hang the next ones. Deliberately does NOT send Reset All Controllers — see the
    // implementation for why that costs more than it buys here.
    void sendPanic();

    // --- Tempo (for the effect tempo-sync editor feature) ---
    // Hosted: the DAW project tempo, sampled from the playhead each processBlock.
    // Standalone: the user's TEMPO knob value (there is no host transport). The
    // editor reads effectiveBpm() to convert note values to ms/Hz; the standalone
    // value is user-set and persists in project state. None of this reaches the
    // device — only the resulting raw ms/Hz effect parameters do.
    double effectiveBpm() const;
    void   setSyncTempo(double bpm);                     // message thread (standalone)
    double getSyncTempo() const { return syncTempo.load(std::memory_order_relaxed); }
    double getDawBpm()    const { return dawBpm.load(std::memory_order_relaxed); }
    // False only while HOSTED and the DAW has not yet reported a tempo, i.e. while
    // effectiveBpm() is answering with its 120 fallback. Anything that would ACT on
    // the tempo (re-imposing synced effect notes) waits for this rather than sending
    // the device values for a tempo the project does not have.
    bool   isEffectiveBpmKnown() const
    {
        return juce::JUCEApplicationBase::isStandaloneApp()
            || dawBpm.load(std::memory_order_relaxed) > 0.0;
    }

    // --- Effect tempo-sync state (plugin-only; excluded from .syx) ---
    // Per (effect unit, effect-type index): whether SYNC is on and the chosen note
    // index (0..16, see TempoSync.h) for each of the up-to-4 synced parameter
    // slots. Stored sparsely (only entries with sync on are persisted) and keyed
    // so switching effect programs and back restores the selection. slotNote uses
    // the effect grid's slot index (0..15); unused slots hold -1.
    struct SyncEntry { bool on { false }; int8_t slotNote[16]; SyncEntry() { for (auto& n : slotNote) n = -1; } };
    bool     getEffectSync(int unit, int typeIndex, SyncEntry& out) const;   // false if none stored
    void     setEffectSync(int unit, int typeIndex, const SyncEntry& e);     // e.on=false clears; undo-captured
    void     applyEffectSyncField(int unit, int typeIndex, int field, int v); // undo's write (see .cpp)
    // Phase 5 device-state read-back: ask the MU100 to transmit its current XG
    // state by sending a batch of XG Dump Requests (System, Effect 1, Multi EQ,
    // Insertion 1/2, all 32 Multi Parts, A/D). The device replies with bulk
    // dumps on MIDI OUT, captured via pushMidiOutByte() / pollCompletedDump().
    // `drumSetups` lists the DRUM SETUP numbers (0-3) to read back: each drum note
    // (0x0D-0x5B) is requested per sub-block, so this can be hundreds of requests
    // — pass only the setups actually in use. Empty = skip drum read-back.
    void requestDeviceDump(const std::vector<int>& drumSetups = {});

    // --- Editor working model (the single source of truth for control values) ---
    // sendParamChange() transmits an XG parameter-change message to the device AND
    // folds it into editorState, so every edit the user makes is recorded. Panels
    // restore control values from getEditorState() when they rebuild (switching
    // part / effect unit), so edits persist across navigation without a re-read.
    // loadEditorState() replaces the model wholesale from a captured bulk dump
    // (device-state read-back). All three are message-thread only.
    void sendParamChange(const std::vector<uint8_t>& paramChangeMsg);
    // As sendParamChange, but for a CONTINUOUS control (a knob/box being dragged).
    // The model is updated immediately — the UI stays live — while the transmission
    // is coalesced per XG address and flushed at kParamFlushHz, so a drag emits a
    // bounded message rate instead of one SysEx per value change.
    //
    // WHY: MIDI IN carries 31250 baud = 3125 bytes/s, i.e. ~310 ten-byte parameter
    // changes a second, and NOTHING upstream of the wire enforced that. A knob drag
    // fires onChange on every value change (a dial is ~2 px per step), so a fast
    // mouse can emit several times what the port can carry. What that overruns is
    // not our ring (512 K slots) but MAME's midiin_device::m_xmitring, which wraps
    // over unsent bytes with no overflow check — corruption mid-SysEx, which the
    // firmware then discards, so an edit silently never lands.
    //
    // Coalescing is safe here because these messages are IDEMPOTENT absolute writes:
    // dropping an intermediate drag value changes nothing, and the last value always
    // goes out on the next tick. Do NOT route anything order-sensitive through it
    // (effect-type changes, program changes, file loads) — see flushPendingParamChanges.
    void sendParamChangeThrottled(const std::vector<uint8_t>& paramChangeMsg);
    // Transmit every coalesced param-change now. Called by the flush timer, and by
    // sendParamChange/sendSysEx before an immediate send so a pending drag value can
    // never land AFTER (and stomp) a later type change or file load.
    void flushPendingParamChanges();
    // Record a param-change into the model WITHOUT transmitting it (used to seed
    // the editor's display with known power-on defaults the device already holds).
    void recordParamChange(const std::vector<uint8_t>& paramChangeMsg);

    // --- Undo / Redo (see EditUndo.h) ---------------------------------------------
    // Covers EDITOR PARAMETER EDITS ONLY: everything that reaches sendParamChange or
    // sendParamChangeThrottled, plus the plugin-only setters (HPF2, the peaking EQ
    // mid bands, their drum-key counterparts). All message thread.
    //
    // Capture is at the FUNNEL with an inverted default — everything is recorded
    // unless explicitly suppressed. That is fail-CLOSED: a control added to a panel
    // tomorrow is undoable automatically. Annotating the ~50 panel call sites instead
    // would be fail-open, where a forgotten site is silently non-undoable with no
    // compile error and no symptom until a user notices.
    //
    // Two things suppress: DAW automation playback (AutomationParams must not fill
    // the stack with the host's own writes) and the apply path itself.
    bool canUndo() const { return undoPos > 0; }
    bool canRedo() const { return undoPos < (int) undoStack.size(); }
    // "Part 5 VOLUME 100 -> 64". Empty when there is nothing to undo/redo. Formatted
    // on demand — see the note on Step::mapIndex about why it is not stored.
    juce::String undoLabel() const;
    juce::String redoLabel() const;
    void undo();
    void redo();
    // Counts undo()/redo() applications (message thread). The effect panel watches it
    // to put a restored tempo-synced time back on the CURRENT tempo.
    int  undoApplyCount() const { return undoApplies; }

    // --- History window (the hamburger between the arrows) -------------------------
    // The list the window shows is the stack itself, so there is no second copy to
    // keep in step: entry 0 is the state BEFORE the oldest retained step and entry i
    // the state just after step i, which makes `undoPosition()` the index of the entry
    // to mark as current.
    int  undoStepCount() const { return (int) undoStack.size(); }
    int  undoPosition()  const { return undoPos; }
    // Is entry 0 genuinely the state before ANY edit? False once the 20-step cap has
    // discarded the oldest steps — entry 0 is then just the oldest state still
    // reachable, and calling it "Initial State" would be a lie the user cannot check.
    bool undoHasInitialState() const { return undoDropped == 0; }
    // "Part 5 VOLUME 64 -> 100" — what step `index` (0-based) DID, i.e. old -> new,
    // the opposite reading from undoLabel()'s "what pressing Undo will do".
    juce::String undoStepLabel(int index) const;

    // Move to the state after step `pos` (0 = entry 0, the oldest reachable state) by
    // replaying undo()/redo() one step at a time. Deliberately not a shortcut: each
    // step's messages are the only description of what it changed, so the only way to
    // get from A to B is through every step between them.
    void undoGoTo(int pos);

    // Drop the history. Called by every path that replaces the patch WHOLESALE —
    // seedEditorDefaults / loadEditorState / appendEditorState, which between them
    // cover File > New, File > Open, Read from Device and setStateInformation.
    //
    // This is a correctness requirement, not tidiness. A record is a byte at an
    // ABSOLUTE XG address, meaningful only against the patch that produced it. Kept
    // across a load, undo would write the previous patch's value into the new one —
    // not an undo but a one-byte corruption, and an invisible one, since the value is
    // well-formed and in range. There is no way to detect that at apply time, so the
    // history has to die the moment its baseline does.
    //
    // sendPanic() deliberately does NOT clear: it sends CCs and never touches the
    // model, so no record is invalidated. Nor does sendPatchToDevice(), which
    // transmits the model outward and leaves it as it was.
    void undoClear();

    // RAII: collapse every model write inside the scope into ONE step. Needed where a
    // single user action fans out — an effect TYPE change carries its program's 16
    // defaults (17 messages), a voice change is bank MSB/LSB + program (3).
    //
    // `mergeKey` (mu100undo::gestureMergeKey) makes SUCCESSIVE scopes of the same kind
    // on the same target fold together as well, so a wheel-scroll burst through the
    // effect programs is one step rather than one per detent. Leave it 0 where each
    // invocation is a deliberate separate action; a scope with key 0 never merges and
    // never absorbs a later edit. A scope may also outlive a single call — hold one in
    // a unique_ptr across mouseDown..mouseUp to make a whole drag one step, which is
    // what the graphic display's two-axis EQ nodes do.
    struct UndoScope
    {
        UndoScope(YamahaMU100AudioProcessor& p, const juce::String& label,
                  uint32_t mergeKey = 0)
            : proc(p) { proc.undoGestureBegin(label, mergeKey); }
        ~UndoScope() { proc.undoGestureEnd(); }
        YamahaMU100AudioProcessor& proc;
    };

    // The same thing without the object, for a gesture that spans several EVENTS
    // rather than one call: a mouse drag starts in mouseDown and ends in mouseUp, so
    // there is no enclosing block to hold a scope. Must be balanced — but
    // undoGestureEnd() is a no-op when nothing is open, so a caller that also has to
    // cope with a drag being cancelled can simply call it.
    void undoGestureBegin(const juce::String& label, uint32_t mergeKey = 0);
    void undoGestureEnd();

    // RAII: record nothing at all inside the scope.
    struct UndoSuppress
    {
        explicit UndoSuppress(YamahaMU100AudioProcessor& p) : proc(p) { ++proc.undoSuppressDepth; }
        ~UndoSuppress() { --proc.undoSuppressDepth; }
        YamahaMU100AudioProcessor& proc;
    };

    // Seal the open step so the next edit starts a fresh one, even inside the coalesce
    // window. Called on mouse-up from the shared controls so releasing the button ends
    // the step crisply instead of 700 ms later. Safe to call when there is no step.
    void undoSealTop();

    void loadEditorState(const uint8_t* data, size_t len);
    // Fold a captured bulk dump into the model WITHOUT clearing it first. Used to
    // accumulate a read-back reply that arrives as several bursts (the first burst
    // clears via loadEditorState, later bursts append here).
    void appendEditorState(const uint8_t* data, size_t len);
    const mu100::DeviceState& getEditorState() const;

    // The complete MU100 power-on factory-default state (embedded blob). editorState
    // is seeded from this so it's always complete — independent of the lossy device
    // read-back — which makes save/recall exact.
    // STATIC: a process-wide constant built once from the embedded blob. Making it
    // static is what lets the const per-part roll-ups compare against it.
    static const mu100::DeviceState& factoryDefaults();

    // Replace the editor model with the complete factory defaults (fresh open / after
    // reset — the device is at defaults then too, so the two stay in sync with no
    // device read-back).
    void seedEditorDefaults();

    // Load a patch FILE into the editor model as factory-defaults OVERLAID with the
    // file: params the file omits reset to their default (a load = "the patch is
    // exactly this"), not left at a previous file's value. Reliable + complete
    // regardless of framing (XGEdit stream or bulk dump).
    void loadEditorFile(const uint8_t* data, size_t len);

    // Fold SysEx into the model the way the DEVICE folds it: a write that CHANGES an
    // effect unit's TYPE also loads that type's parameter defaults, before the rest of
    // the same message applies. A plain DeviceState::load keeps the previous type's
    // parameters under the new type -- a .syx carrying "VARIATION TYPE = CROSS DELAY,
    // P1, P2" left the factory DELAY LCR values in P3..P7, and on Cross Delay P3 is
    // FEEDBACK, so the model held 5000 against a 1..127 range. Used for file loads and
    // incoming bulk dumps, i.e. wherever SysEx that is not the device's own read-back
    // becomes the model.
    void loadModelLikeDevice(const uint8_t* data, size_t len);

    // Put every ACTIVE effect parameter whose value is outside its range for the unit's
    // current type back to that type's default. Heals files and projects saved while
    // the loader above did not exist; the default is what the device actually played,
    // since the out-of-range bytes never reached it until 2026-09-24's whole-unit push.
    // Returns how many parameters it changed (each is logged). Model only.
    int healEffectParamsOutOfRange();

    // Push the current editor model to the device so it AUDIBLY plays the patch:
    // XG System On (reset to defaults) followed by the model's non-default params as
    // a parameter-change stream. Param changes apply reliably (a large bulk stream
    // overflows the H8 and drops blocks) and the diff is sparse, so it's fast.
    // Returns an estimated wall-clock ms for the transfer (for the load overlay).
    int  sendPatchToDevice();

    // --- Loaded-file tracking (bottom-strip filename + unsaved-changes marker) ---
    // The editor's bottom strip shows loadedFilePath (the .syx the model was last
    // loaded-from or saved-to; empty = untitled) with a '*' when the live model no
    // longer matches the state captured at that sync point. editorStateSignature()
    // hashes the same canonical bulk-dump byte stream that getStateInformation
    // persists, so "matches file" means "byte-identical to the last-synced state".
    // loadedFilePath + savedStateSig persist in get/setStateInformation (DAW project
    // state), so the marker survives a project close/reopen.
    juce::String editorStateSignature() const;
    // An empty savedStateSig means no baseline has been established yet (the defaults
    // seed / a file load / save sets it) — with no reference there is nothing to be
    // dirty against, so report clean. This keeps a freshly opened, no-file-loaded
    // instance from showing a spurious '*' before the defaults baseline is seeded.
    bool isEditorStateDirty() const
    {
        return savedStateSig.isNotEmpty() && editorStateSignature() != savedStateSig;
    }
    // Record that the model is now in sync with `path` (a file load or save): store the
    // path and freeze the current signature as the saved baseline (clears the marker).
    void markStateSyncedToFile(const juce::String& path);

    // What kind of file the model is currently associated with. Native = a .mss complete
    // setup (Save Setup overwrites it in place); Syx = a loaded/exported raw SysEx dump
    // (Save Setup must NOT overwrite it — it routes to Save Setup As with a default native
    // name instead); None = untitled. Derived from loadedFilePath's extension, so it stays
    // correct across a DAW project reopen without a separate persisted field.
    enum class OpenFileType { None, Native, Syx };
    OpenFileType openFileType { OpenFileType::None };
    static OpenFileType openFileTypeForPath(const juce::String& path);

    // True once a device-state read-back has run to COMPLETION at least once this
    // plugin-instance lifetime. The editor uses it to avoid re-running the full
    // read-back (a heavy burst of dump requests + replies on MIDI IN A that starves
    // note handling for seconds) every time the GUI window is reopened — on reopen
    // it just re-applies this already-captured model to the panels. Set on
    // completion (EditorView::pollReadBackTimeout), NOT on the first reply burst, so
    // a read-back interrupted by an early GUI close leaves this false and restarts
    // cleanly on reopen instead of stranding a partial model. Survives editor
    // close/reopen (this object doesn't).
    bool editorStatePopulated { false };

    bool pollMidiData(int port = 0);
    int  readMidiByte(int port = 0);
    void shutdownMame();

    // --- Incoming note-on observation (editor Keyboard drum-key selection) ---
    // Every incoming MIDI note-on (from any port) updates a lock-free single slot.
    // The editor polls takeLatestNoteOn(): if a new note-on has arrived since the
    // caller's `seq`, it updates `seq` and returns the note number, else -1. Lets
    // a played note pick the drum key while the Keyboard module is in drum-edit
    // mode. Safe to call observeNoteOn() from the audio/MIDI threads.
    void observeNoteOn(int note);
    int  takeLatestNoteOn(uint32_t& seq) const;

    // --- Held incoming notes, for the Keyboard module's played-note display ---
    // A 128-bit "currently held" set per RAW receive channel (port*16 + ch-1) -- the
    // same 0..31 key the chan-activity mask below uses, so a panel resolves a part to
    // its notes exactly as PartListPanel resolves it to its indicator, and the Rcv
    // channel map stays in the panels rather than being duplicated here.
    // Maintained by observeMidiActivity() from the audio / MIDI-callback threads.
    // Unlike the activity masks this is STATE, not an event, so it is read without
    // draining. The four words are read one at a time: a chord landing across the
    // read can be seen half-applied, which at 30 Hz is invisible.
    void heldNotesFor(int rawChannel, uint32_t out[4]) const;
    void clearHeldNotes();               // panic, and engine shutdown

    // --- Incoming MIDI activity, for the Voice module's per-part indicators ---
    // Records genuinely-received MIDI (host bus / standalone device callback) so
    // the editor can flash a per-part "MIDI in" tab whenever ANY MIDI data reaches
    // that part. Two lock-free accumulators:
    //   * chan mask — bit (port*16 + channel-1) set on ANY channel-voice message
    //     (note on/off, poly/channel aftertouch, CC, program change, pitch bend),
    //     so the panel can light whichever part(s) receive on that channel (the Rcv
    //     channel map lives in the panel, not here).
    //   * part mask — bit p set for SysEx that RESOLVES to MULTI PART p (see
    //     classifySysEx). SysEx that targets no part sets NO part bits; only a bulk
    //     dump / performance load, which really does rewrite all 32, sets all bits.
    //   * sys mask — SYS / FX / BULK bits for the Keyboard module's lamps, i.e. the
    //     messages that are real input but belong to no part.
    // System-realtime/common messages (clock, active sensing, …) target no part and
    // are ignored. Editor edits sent via pushMidiByteToPort() bypass this path by
    // design, so only genuine external input flashes. take*() drain-and-clear (msg thread).
    enum SysActivity { SYSACT_SYS = 1, SYSACT_FX = 2, SYSACT_BULK = 4 };
    void     observeMidiActivity(int port, const juce::MidiMessage& msg);
    void     classifySysEx(const juce::MidiMessage& msg,
                           uint32_t& parts, uint32_t& sys) const;   // see the .cpp
    // --- Received SysEx → the editor's working model ---
    // editorState is written by the editor's own sends (sendParamChange), by file loads
    // and by read-backs, but nothing used to feed it the SysEx the plugin RECEIVES. So a
    // parameter change arriving from a DAW track or an external controller changed the
    // device while the panels went on showing whatever the editor last selected itself —
    // the reported symptom being the Effect module reading DELAY LCR while PITCH CHANGE
    // was audible.
    //
    // observeMidiActivity() runs on the audio thread (VST3) or a Windows MIDI callback
    // thread (standalone), and editorState is a std::map read from many message-thread
    // sites, so received messages are QUEUED (lock-free) and folded into the model by
    // drainIncomingSysEx() on the message thread. The model is never written from the
    // callback.
    //
    // The drain runs off the PROCESSOR's own timer rather than the editor's, because the
    // model backs project state and so must stay current with the GUI closed — and
    // because an undrained ring fills up and then drops the NEWEST messages, which would
    // leave the model holding stale values at the moment it is reopened or saved.
    // editorStateExternalSeq bumps whenever a drain changed the model; the editor watches
    // it to know when to re-apply the model to its panels.
    bool drainIncomingSysEx();                                  // message thread
    std::atomic<uint32_t> editorStateExternalSeq { 0 };

    // --- MIDI overrides: what INCOMING MIDI has done to the DEVICE -------------
    // MEASURED 2026-09-09 (MU100_DIAG_MIDIPC): a channel CC moves the MULTI PART
    // parameter it maps to, a channel PROGRAM CHANGE moves the part's program AND
    // re-initialises the DRUM SETUP buffer its PART MODE names -- even when the
    // program is the one already loaded, which is what a sequencer sends at the top
    // of every take. Bank select alone does nothing until a program change follows.
    //
    // None of that may reach editorState. The model is the USER's setup: it is what
    // the panels show, what a .mss stores and what the host silently captures in
    // getStateInformation with no dialog in front of it. So an externally-arrived
    // value is recorded HERE instead, in a second DeviceState that is never saved,
    // never sent and never undone -- a display-only shadow of the device.
    //
    //   editorState        = your setup. Saved. Only you and your DAW lanes move it.
    //   midiOverrideState  = what the device is actually playing, where that differs.
    //
    // Automation lanes are deliberately NOT shadowed: a lane is project data the user
    // authored, it replays identically, and every DAW expects it to move the control.
    // It goes through sendParamChangeThrottled like an editor knob and so CLEARS any
    // override at the addresses it writes (see clearMidiOverridesForMessage) -- the
    // device and the model agree again the instant either one of them writes.
    //
    // Threading is the same discipline as the SysEx queue above: channel messages are
    // queued lock-free from the audio / MIDI-callback thread and translated on the
    // MESSAGE thread, where the model may be read (channel -> part needs each part's
    // Rcv CHANNEL, which lives in editorState).
    //
    // -1 = no override at that address / for that parameter.
    int  midiOverrideValue(int paramIndex, int instance, int instance2 = 0) const;

    // The same shadow read by raw ADDRESS instead of by parameter index. The two
    // Effect modules address the device POSITIONALLY -- unit + slot -> address low
    // (EffectAddressing.h) -- so they hold no parameter index to ask
    // midiOverrideValue with. The shadow is stored BY ADDRESS either way, so this
    // asks it the way those panels can. Returns false where the byte carries no
    // override, exactly as DeviceState::getByte reports an unset byte.
    bool midiOverrideByteAt(mu100::ModelIx model, uint32_t addr21, uint8_t& out) const;

    int  midiOverrideTotal() const { return midiOverrideCount.load(std::memory_order_acquire); }

    // WHERE the overrides are, as one line for the badge's tooltip -- "3 on Pt 10,
    // 2 on DRUMS2 (C1, D1)". Empty when nothing is overridden.
    //
    // The badge is the only surface that holds the whole set, and for one case it is
    // the only surface that CAN: an override on a drum buffer no part on screen is
    // using has no knob and no key strip to appear on, so without this it was visible
    // ONLY as the count going up. Message thread, like the other roll-ups.
    juce::String midiOverrideWhere() const;
    // Bumps whenever the override set changes, so the editor can repaint only then
    // rather than walking every control on every timer tick.
    uint32_t midiOverrideSequence() const { return midiOverrideSeq.load(std::memory_order_acquire); }
    // "Clear MIDI overrides": drop the marks AND re-send our own values, so the device
    // is put back to what the panels have been showing all along.
    void clearMidiOverrides(bool resendToDevice);
    // "Keep MIDI overrides": fold the device's values into the model as one undoable
    // step, so what the song did becomes the setup you save.
    void keepMidiOverrides();

    // --- Per-part roll-ups for the Voice module's row tabs ---------------------
    // "Has anything happened to this part that I cannot already see on its row?"
    // Both deliberately EXCLUDE the five values the Voice module shows for itself
    // (element reserve, both bank bytes, the program, the receive channel) and the
    // per-key DRUM SETUP data, which belongs to a BUFFER shared between parts. See
    // the .cpp. Message thread.
    bool partChangedOutsideVoiceList(int part) const;
    bool partOverriddenOutsideVoiceList(int part) const;
    // Both are global settings (settings.xml), like every other Option except the
    // per-part output pair and the automation switch.
    bool showMidiOverrides   { true };    // Display Animation -> "Show MIDI changes to the device"
    // ...and the FORM those marks take: Display Animation -> "Mark them with".
    // midimarks::Style (0 horizontal bars, 1 bars + knob arcs, 2 vertical bars + knob
    // dots). There is deliberately no "none" here -- the toggle above is the off
    // switch, and two ways to turn one thing off is what that would be.
    int  midiOverrideMarkStyle { midimarks::BarsAndArcs };
    // Renamed 2026-09-10 with its menu item: the flag now reads the way the label
    // does (ticked = following), so nothing here is stored inverted any more.
    bool midiFollowEnabled   { true };    // Options -> "Follow MIDI changes to the device"

    uint32_t takeMidiChanActivity() { return midiChanActivity.exchange(0, std::memory_order_acquire); }
    uint32_t takeMidiPartActivity() { return midiPartActivity.exchange(0, std::memory_order_acquire); }
    uint32_t takeMidiSysActivity()  { return midiSysActivity .exchange(0, std::memory_order_acquire); }

    // --- Per-part clean (non-resonant) high-pass filter ---
    // A plugin-only DSP feature applied per-voice inside the SWP30 (parts are
    // pre-summed there, so it cannot live plugin-side). The cutoff is an absolute
    // Hz value per part; 0 (or <20) = off. It is NOT a hardware/XG parameter and
    // never enters the SysEx stream, so .syx load/save stays byte-compatible.
    // The engine tags each voice with its true part by reading the H8 firmware's
    // part pointer at keyon (see swp30 hpf_part_from_h8). Persisted in project state.
    void  setCleanHpfHz(int part, float hz);      // message thread
    float getCleanHpfHz(int part) const { return (part >= 0 && part < 32) ? cleanHpfHz[part] : 0.0f; }
    void  reassertCleanHpfIfDirty();               // MAME thread (re-push after (re)boot)

    // --- System HPF: a FLOOR under every part's cutoff ---
    // Same filter, one level up: a part is high-passed at max(its own HPF2, this), so
    // the value is the lowest corner any part may have. NOT a second filter in series --
    // swp30 runs one biquad at the higher of the two -- so it costs nothing on top of
    // the per-part HPF, whichever way round the two are set. Every one of these filters
    // is 12 dB/oct, so this sets a FREQUENCY floor, never a slope.
    //
    // Plugin-only, like HPF2 and the mid bands: no XG address, never in a .syx, greyed
    // by "Disable plugin-only parameters", and persisted in project state.
    void  setSystemHpfHz(float hz);               // message thread
    float getSystemHpfHz() const { return systemHpfHz; }

    // --- Per-part plugin-only 3-band peaking EQ (the mid bands that complement the
    // XG BASS/TREBLE shelves). Like HPF2 it is applied per voice inside swp30 and is
    // NOT an XG parameter, so it never enters the SysEx stream (.syx stays byte-
    // compatible). Values are the editor's raw values: gain 0..127 (64 = 0 dB) and
    // Q 1..120 (0.1..12.0), but freq is absolute Hz (32..16000) — being plugin-only,
    // these bands aren't confined to the coarse XG EQ-freq table the firmware takes,
    // so they use the finer freqladder (Source/FreqLadder.h) instead. band = 0..2.
    // Persisted in project state.
    // These bands and HPF2 run on EVERY part, drum parts included -- they are OUR
    // filters, applied per voice, and a drum voice is a voice like any other. On a part
    // with a DRUMS buffer they stack ON TOP OF the per-key drum layer: the part-level
    // pair shapes the whole kit, the key layer shapes one sound.
    //
    // What a DRUMS buffer DOES kill is the two XG BASS/TREBLE shelves, and that is the
    // DEVICE's doing, not ours: the MU100 ignores part-level EQ on a drum part (owner's
    // manual p.82, and measured here). Hence partShelfEqInert below, which is about the
    // FIRMWARE half only -- it must never be used to gate the plugin bands.
    //
    // (Until 2026-09-05 the plugin bands were pushed FLAT on a drum part too, so the
    // whole row could be grayed as one. Reversed at the user's direction: a kit wants a
    // part-wide HPF and a part-wide mid dip as much as any other part does, and the two
    // dead shelf columns are cue enough on their own.)
    bool partShelfEqInert(int part) const
    { return part >= 0 && part < 32 && partDrumBuffer[part] >= 0; }

    void setPartEqGain(int part, int band, int raw);   // message thread
    void setPartEqFreq(int part, int band, int hz);    // message thread
    void setPartEqQ   (int part, int band, int raw);   // message thread
    int  getPartEqGain(int part, int band) const { return partEqValid(part, band) ? partEqGain[part][band] : 64; }
    int  getPartEqFreq(int part, int band) const { return partEqValid(part, band) ? partEqFreq[part][band] : kPartEqFreqDefault[band]; }
    int  getPartEqQ   (int part, int band) const { return partEqValid(part, band) ? partEqQ[part][band]    : 7; }
    void reassertPartEqIfDirty();                  // MAME thread (re-push after (re)boot)

    // --- Per-part spectrum probe (drives the EQ screen's animated analyser) ---
    // The swp30 sums the voices the firmware keyed on one part (optionally one drum
    // key) AFTER the whole per-voice filter chain, so what comes back is post
    // hardware-shelf, post plugin peaking bands, post HPF2 and post drum-key layer —
    // i.e. exactly what the EQ screen's response curve claims to describe. It is
    // pre-mixer, so it excludes part volume/expression/pan, the insertion effects and
    // the system effects, which is the conventional tap point for an EQ analyser.
    // Fixed 44100 Hz (the SWP30's internal stream rate), NOT the host rate — the
    // analyser's frequency axis must be derived from this, not getSampleRate().
    static constexpr double kSpectrumSampleRate = 44100.0;
    // Message thread. `active` false disables the probe outright. NOTHING disables it
    // automatically: the caller MUST push false when the analyser stops (screen
    // hidden, editor closed) or the sound thread keeps accumulating for nobody.
    void setSpectrumTarget(bool active, int part, int drumKey);
    // Message thread. Copies the most recent `numSamples` into dst (chronological);
    // returns the count, or 0 when the probe is idle / MAME isn't up / the frame was
    // dropped. A 0 return is normal and means "reuse the previous frame".
    int  readSpectrum(float* dst, int numSamples);

    // --- Per-part level meter (drives the VOLUME screen's animated bars) ---
    // The same DRY signal the per-part output buses carry — pan and volume applied,
    // ahead of the MEG — reduced to one peak per part. So a part's meter follows the
    // very VOLUME bar it sits under, which is the whole point of putting it there,
    // and it excludes the system reverb/chorus/variation tails (they are not any one
    // part's signal once the MEG has summed them).
    //
    // Message thread, both. Like the spectrum probe, the meter re-pushes its own
    // target every frame, so a reboot self-heals with no dirty-flag plumbing — and
    // like it, NOTHING disables it automatically: the caller MUST push false when the
    // meter stops (screen left, editor closed) or the mixer keeps metering for nobody.
    void setPartLevelMeter(bool active);
    // Fills dst[0..maxParts) with each part's peak since the last call, as linear
    // amplitude where 1.0 is the main output's full scale, and CLEARS the peaks.
    // Returns the count, or 0 when the meter is idle / MAME is not up.
    int  readPartLevels(float* dst, int maxParts);

    // The "S" (master) bar's meter. Not from swp30: the master value is the plugin's
    // own post-everything stereo output, which processBlock already has in hand — so
    // this costs a peak pass over the output buffer, and ONLY while the meter is on
    // (the audio-thread cost is why it is armed rather than always sampled).
    float takeMasterPeak() { return masterPeak.exchange(0.0f, std::memory_order_relaxed); }

    // --- Optional per-part stereo outputs ---
    // 32 aux stereo output buses, one per part, carrying that part's DRY signal
    // (pan and volume applied, but ahead of the MEG — so no reverb, chorus,
    // variation or insertion effect). They exist so a DAW can process each part
    // with its own plugins.
    //
    // The buses are always DECLARED — VST3 fixes bus count at construction, so
    // they cannot be added later — but they are disabled by default, and the
    // capture inside swp30 only runs while this is on. A default instance is
    // therefore exactly the plain stereo plugin it has always been.
    //
    // NOTE the aux buses are NOT audible in the Standalone build: JUCE's
    // StandaloneFilterWindow only opens the main bus. Test them in a DAW.
    static constexpr int kNumPartBuses = 32;
    // Main stereo out + 32 stereo aux buses, the aux ones not enabled by default.
    // Static because it runs in the constructor's initialiser list, before there is
    // an object; a member because BusesProperties is protected in AudioProcessor.
    static BusesProperties buildBusLayout();
    // The ONLY two options that live in DAW project state instead of settings.xml.
    // How many tracks a song wants out of the synth belongs to the song, and the DAW
    // already stores its half of it (which aux buses are enabled), so a global flag
    // would let the two halves disagree — a mix built on the per-part outputs coming
    // back with the routing off, or with the main mix silent (or doubled) because
    // "remove from main" was last set in a different song.
    //
    // They are NOT in settings.xml at all, which is what keeps the rule simple: every
    // other Option is global, these two are the project's, and a NEW instance is always
    // off — exactly the plain stereo plugin described above. Someone who always works
    // this way keeps a DAW template, which is a saved project and therefore carries it.
    void setPartOutputsEnabled(bool enable);      // message thread
    bool getPartOutputsEnabled() const
    {
        return partOutputsEnabled.load(std::memory_order_relaxed);
    }
    void reassertPartOutputsIfDirty();            // MAME thread (re-push after (re)boot)

    // "Remove those parts from the main mix". Applies only to parts whose aux bus the
    // HOST has actually enabled, so routing four parts out does not silence the other
    // twenty-eight. The part's reverb/chorus tail stays in the main mix — the MEG
    // returns a single stereo pair, and its tails cannot be separated per part.
    void setPartOutputsRemoveFromMain(bool remove);   // message thread
    bool getPartOutputsRemoveFromMain() const
    {
        return partOutputsRemoveFromMain.load(std::memory_order_relaxed);
    }

    // --- Per-part MUTE / SOLO (the Voice list's M and S columns).
    //
    // A MIXER-layer feature, not a MIDI one: nothing is transmitted, so it cannot
    // dirty the edit model, be captured by undo or by an armed automation lane, or
    // need unwinding when a project is reopened. It is deliberately NOT persisted
    // (settings.xml or DAW project state) and NOT a DAW-automatable parameter — it
    // answers "what am I listening to right now", and a plugin that reopens silent
    // with no visible cause is the failure mode this feature would otherwise add.
    //
    // Both masks are bit-per-part, bit n = part n, and a part is NEVER in both:
    // engaging one clears the other on that part (see setMuteSoloMasks). So the two
    // resolve into ONE "audible" mask with no tie to break — while anything is
    // soloed the soloed parts are what you hear, otherwise everything unmuted is.
    void setPartMuted (int part, bool muted);    // message thread
    void setPartSoloed(int part, bool soloed);   // message thread
    void setPartMuteMask(uint32_t mask);         // message thread (exclusive click, clear-all)
    void setPartSoloMask(uint32_t mask);         // message thread
    uint32_t getPartMuteMask() const { return partMuteMask.load(std::memory_order_relaxed); }
    uint32_t getPartSoloMask() const { return partSoloMask.load(std::memory_order_relaxed); }
    bool isPartMuted (int part) const { return (getPartMuteMask() >> part) & 1u; }
    bool isPartSoloed(int part) const { return (getPartSoloMask() >> part) & 1u; }
    // What the part can actually be heard through: soloed if anything is soloed,
    // otherwise not muted. The single place that rule is written down.
    uint32_t partAudibleMask() const
    {
        const uint32_t solo = getPartSoloMask();
        return solo ? solo : ~getPartMuteMask();
    }
    bool isPartAudible(int part) const { return (partAudibleMask() >> part) & 1u; }
    // Called by the editor after any mute/solo change so the bottom-bar indicator and
    // the Voice list stay in step with a change made from either of them.
    std::function<void()> onPartMuteSoloChanged;

    // --- Per-part EQ BYPASS (the Part module's EQ BYP lamp).
    //
    // Takes a part's whole EQ chain out of circuit for an A/B, WITHOUT disturbing a
    // single value that describes it. That last part is the design: the model is
    // untouched, so saving a setup or exporting a .syx while bypassed still writes the
    // user's real EQ, and Undo has nothing to unwind. Like mute/solo (above) it is
    // live state -- not persisted, not automatable -- and for the same reason: a
    // project that reopened silently bypassed would be a bug the user cannot see.
    //
    // It reaches the audio in TWO ways, because the six bands are not one mechanism:
    //  * HPF2, the three peaking mid bands and the drum-key layer are PLUGIN-owned,
    //    applied per voice inside swp30. One mask (swp30_ext::set_part_eq_bypass)
    //    crossfades them out; nothing is re-pushed and no value moves.
    //  * The XG BASS/TREBLE shelves are the FIRMWARE's, programmed into the voice's
    //    own filter registers, so the only lever is the parameter itself. Engaging
    //    transmits gain 64 (0 dB, which IS "no shelf") and releasing transmits the
    //    model's value back -- through sendSysEx, which does NOT fold into
    //    editorState, so the model still holds what the user set.
    //
    // The consequence of that second half, and it is not optional: a device READ-BACK
    // while bypassed would read the zeroed shelves into the model and destroy the
    // settings, so requestDeviceDump releases the bypass first (see its callers).
    //
    // SCOPE: one part -- EXCEPT on a part with a DRUMS buffer, where it is the whole
    // KIT (every part on that buffer, each lighting its own lamp and counted in the
    // badge). The forcing term is the DRUM SETUP's per-key BASS/TREBLE shelves: they are
    // drum-key-level data the FIRMWARE owns, shared by every part on the buffer, so the
    // only lever reaches all of them. Bypassing them for one part alone is not on the
    // menu -- the choices are to leave them audible (an incomplete bypass) or to flatten
    // them for the buffer's other parts behind a dark lamp (a silent half-state). Doing
    // it openly, for the whole kit, is the one answer that stays visible.
    //
    // It DOES cost something, and knowingly: since 2026-09-05 a drum-buffer part has a
    // live part-level EQ again (its plugin HPF2 + mid bands, over the key layer), so the
    // widening takes those out on sibling parts the user never touched. Weighed and
    // accepted by the user that day -- everyone playing one kit hears it the same way,
    // and no affected part is left looking untouched.
    //
    // A MELODIC part is always alone: everything the mask governs there is keyed by
    // PART, and it shares no DRUM SETUP with anyone.
    //
    // THE SCOPE IS RE-DERIVED, NEVER STORED (2026-09-05). Who is affected depends on
    // buffer membership, and membership changes -- so only the parts the user actually
    // PRESSED are kept (partEqBypassChosen) and the affected set is recomputed on every
    // toggle and every buffer change. Assign a part to a bypassed kit and it joins the
    // bypass, lamp and badge included; move a part off, and it keeps the bypass only if
    // it was chosen in its own right, while the kit it left is restored once nobody on
    // it is bypassed. Before that, the affected set was computed once at press time and
    // went stale in both directions -- a joining part silently lost the kit's per-key
    // shelves behind a dark lamp, and a departing one left its old buffer flattened on
    // the device with nothing pointing at it any more.
    void setPartEqBypass(int part, bool on);      // message thread
    // The argument is the CHOSEN set, not the affected set: it is widened by the kit
    // rule exactly as a single toggle is. Both callers pass 0 (release everything) or
    // ~0 (every part), where the two coincide.
    void setPartEqBypassMask(uint32_t mask);      // message thread
    // The AFFECTED set -- what the lamps, the badge and swp30 all read.
    uint32_t getPartEqBypassMask() const { return partEqBypassMask.load(std::memory_order_relaxed); }
    // Did the user press the button on THIS part, or is it bypassed because it shares a
    // kit with a part they did? Identical in every other way -- same lamp, same badge,
    // same sound -- and they only diverge when the part changes DRUMS buffer, so the
    // Part module says which in the lamp's tooltip. Message thread.
    bool isPartEqBypassChosen(int part) const
    { return part >= 0 && part < 32 && ((partEqBypassChosen >> part) & 1u) != 0; }
    bool isPartEqBypassed(int part) const
    { return part >= 0 && part < 32 && ((getPartEqBypassMask() >> part) & 1u) != 0; }
    // Fired after any change, so the lamp, the EQ screen and the bottom-bar badge all
    // follow a toggle made from any of them.
    std::function<void()> onPartEqBypassChanged;

    // --- Per-drum-key EQ/HPF (the drum-mode counterpart of the plugin part EQ/HPF).
    // Mirrors the hardware DRUM SETUP semantics: keyed by (DRUMS buffer 0..3, key
    // 0..127), SHARED across parts on the same buffer, reset when a new kit is
    // assigned. Applied as a second per-voice layer after the part layer in swp30.
    // Raw values as the UI uses them (same ranges as the part EQ/HPF). Persisted.
    void setPartDrumBuffer(int part, int buffer);            // -1 = melodic; pushed to swp30
    void setDrumHpf   (int buffer, int key, float hz);       // message thread
    void setDrumEqGain(int buffer, int key, int band, int raw);
    void setDrumEqFreq(int buffer, int key, int band, int hz);
    void setDrumEqQ   (int buffer, int key, int band, int raw);
    float getDrumHpf   (int buffer, int key) const           { return drumIdx(buffer, key) ? drumHpfHz[buffer][key] : 0.0f; }
    int   getDrumEqGain(int buffer, int key, int band) const { return drumBandIdx(buffer, key, band) ? drumEqGain[buffer][key][band] : 64; }
    int   getDrumEqFreq(int buffer, int key, int band) const { return drumBandIdx(buffer, key, band) ? drumEqFreq[buffer][key][band] : kPartEqFreqDefault[band]; }
    int   getDrumEqQ   (int buffer, int key, int band) const { return drumBandIdx(buffer, key, band) ? drumEqQ[buffer][key][band]    : 7; }
    // A kit has been assigned to `buffer`: flatten that buffer's plugin-side EQ/HPF
    // AND repopulate the model's hardware-side 3n block with the new kit's defaults,
    // mirroring the re-init the device performs on the program write.
    // `old*` describe the kit LEAVING the buffer, so the per-key values the user
    // edited away from that kit's defaults can be preserved for undo. oldPgm <= 0
    // means "unknown" (no part named this buffer), and then there is nothing to
    // preserve: an unreferenced buffer cannot be heard or read back, so its bytes are
    // leftovers of a kit nothing names any more.
    void resetDrumBuffer(int buffer, int msb, int lsb, int pgm,
                         int oldMsb = 0, int oldLsb = 0, int oldPgm = -1);
    bool drumBufferKitFromModel(int buffer, int& msb, int& lsb, int& pgm) const;
    bool loadDrumBufferDefaults(int buffer, int msb, int lsb, int pgm);
    void reseedDrumBufferFromModel(int buffer);
    // The value a drum key's parameter loads with under the buffer's current kit --
    // the baseline for a "changed from default" marker. -1 if not known.
    int  drumDefaultByte(int buffer, int key, uint8_t lo) const;

    // --- Per-drum-key roll-ups for the Keyboard module's marker strip ---
    // "Which keys of this buffer's kit has anything happened to?", answered for all
    // 128 keys at once as two 128-bit masks (mask[note >> 5], bit note & 31):
    //
    //   changed    = a DRUM SETUP byte differs from what THIS KIT loads THIS KEY
    //                with (drumDefaultByte's baseline, not the static spec default),
    //                or one of the key's plugin-only HPF2 / mid-EQ values is off its
    //                own default.
    //   overridden = incoming MIDI has moved one of the key's parameters away from
    //                the model (the display-only shadow; see midiOverrideValue).
    //
    // Both are asked for the WHOLE BUFFER in one call rather than per key: the
    // changed side is a single ordered walk of the buffer's 3n block, and asking it
    // per key would re-do that walk 79 times.
    void drumKeyMarks(int buffer, std::array<uint32_t, 4>& changed,
                      std::array<uint32_t, 4>& overridden) const;
    // The same two answers for ONE key, as counts, for the strip's tooltip. Kept
    // apart from the masks because it runs on hover, not on the poll.
    void drumKeyMarkDetail(int buffer, int key, int& changedCount,
                           int& overrideCount) const;
    // A part is leaving `buffer` with no kit assigned to it -- called by the editor
    // BEFORE the pick writes anything, so the undo step carries that buffer's per-key
    // edits and the re-init the undo triggers cannot wipe them.
    void preserveDrumBufferOnLeave(int buffer, int msb, int lsb, int pgm);
    void reassertDrumFxIfDirty();                  // MAME thread (re-push map + targets after (re)boot)

    // --- MIDI OUT capture → bulk-dump-to-file ---
    // MAME's MIDI OUT (the driver's `mdout` port) is routed to VstMidiOutputPort,
    // which calls pushMidiOutByte() on the MAME thread for every transmitted byte.
    // We assemble Yamaha bulk-dump SysEx messages (F0 43 0N {4C|49|59} …) and, once
    // the transmit burst has been idle past kDumpFinalizeMs, hand the concatenated
    // bytes to the editor (pollCompletedDump) which pops a "Save As" dialog.
    // Only bulk-dump SysEx is captured, so MIDI thru / note echo can't trigger a save.
    void pushMidiOutByte(uint8_t data);                  // MAME thread
    // message thread (editor timer). `eager` (during a read-back) hands over the
    // accumulated complete messages periodically so a long multi-second reply stream
    // is consumed incrementally instead of all-at-once only after it goes idle — the
    // latter risks the read-back timing out mid-stream and orphaning the rest. When
    // not eager (a user dumpout) it still waits for the burst to go fully idle.
    bool pollCompletedDump(std::vector<uint8_t>& out, bool eager = false);
    void clearDumpCapture();                             // drop unconsumed capture residue

    // Milliseconds since the last captured bulk-dump byte (i.e. how long the device's
    // MIDI-OUT has been quiet). 0xFFFFFFFF if nothing has ever been captured. Used by
    // the editor to tell when a cancelled read-back's trailing reply stream has ended.
    uint32_t dumpStreamIdleMs() const
    {
        const uint32_t last = lastDumpByteMs.load(std::memory_order_relaxed);
        return last == 0 ? 0xFFFFFFFFu
                         : juce::Time::getMillisecondCounter() - last;
    }

    // Drop every queued (incl. future-scheduled) byte waiting to go into MAME MIDI
    // IN A (port 0). Used to abort a read-back's paced Dump-Request stream when the
    // user clicks "Stop reading" — without this the hundreds of requests already
    // queued seconds ahead keep the device replying (→ spurious Save dump) and would
    // delay any subsequent .syx load stuck behind them. The ring buffer's read index
    // is owned by the MAME thread, so this just raises a flag the MAME thread honours
    // on its next poll. Safe to call from the message thread.
    void flushMidiInputPort0() { midiRing.flushRequested.store(true, std::memory_order_release); }

    // Builds the default save name from the dump's detected subtype + current local
    // time, e.g. "mult-32ad-2026-06-18-1430.syx" or the generic "dumpout-…" fallback.
    static juce::String makeDumpDefaultFileName(const std::vector<uint8_t>& bytes);

    // MidiBusSink: VST3 wrapper delivers MIDI IN B (MU100-B) events here, on the
    // audio thread immediately before processBlock(). Stashed for processBlock to
    // drain into MAME port 1. See Source/MidiBusSink.h.
    void receivePortBMidi(const juce::MidiBuffer& portB) override;

    // --- Dynamic output bus names. See Source/BusNameProvider.h ---
    // Recompose every bus name from the current voice assignments and, if any changed,
    // raise the dirty flag and poke the host so it re-queries getBusInfo().
    // Message thread. Returns true if anything actually changed.
    bool refreshBusNames();

    // BusNameProvider. getDynamicOutputBusName may be called from any thread.
    bool getDynamicOutputBusName(int busIndex, char* dest, int destBytes) const override;
    bool consumeBusNamesDirty() override
    {
        return busNamesDirty.exchange(false, std::memory_order_acq_rel);
    }

    // --- Per-part EQ bypass (see setPartEqBypass) ---
    // Pushed to swp30 on the MAME thread by reassertPartOutputsIfDirty, level-triggered
    // against a shadow copy and invalidated (not cleared) on a reboot -- the same shape
    // as the mute/solo mask beside it, and for the same reason.
    // partEqBypassMask is DERIVED and is the affected set (what the lamps, the badge and
    // swp30 read). partEqBypassChosen is the SOURCE OF TRUTH: the parts the user actually
    // pressed the button on. Everything else is recomputed from it plus the CURRENT
    // buffer membership -- see applyEqBypassChosen for why storing the affected set
    // instead does not survive a part changing its DRUMS buffer.
    std::atomic<uint32_t> partEqBypassMask { 0 };
    uint32_t partEqBypassChosen { 0 };          // message thread only
    std::atomic<bool>     partEqBypassDirty { true };
    uint32_t partEqBypassPushed { 0 };
    bool     partEqBypassValid  { false };
    // What this plugin last WROTE to each DRUMS buffer's per-key shelves, so a
    // reconcile can tell "needs flattening" from "already flattened". Not a copy of the
    // mask: a buffer is flattened while any part on it is bypassed, and membership moves
    // under a mask that does not.
    bool bufferShelvesZeroed[4] { false, false, false, false };
    // chosen -> affected (adds every part sharing a buffer with a chosen one).
    uint32_t deriveEqBypassMask(uint32_t chosen) const;
    // The one entry point: store the choices, re-derive, and reconcile both halves of
    // the firmware-owned shelves against what is actually on the device.
    void applyEqBypassChosen(uint32_t chosen);
    // Transmit a PART's own XG shelves for every part whose bypass just changed.
    // `changed` is the bits that moved, `now` the new affected mask.
    void applyPartShelfBypass(uint32_t changed, uint32_t now);
    // Reconcile every DRUMS buffer's shared per-key shelves against `now`.
    void applyBufferShelfBypass(uint32_t now);

    // --- Per-part clean HPF (see setCleanHpfHz) ---
    float cleanHpfHz[32] {};                     // absolute Hz per part; 0 = off
    float systemHpfHz { 0.0f };                  // system-wide FLOOR; 0 = off
    std::atomic<bool> cleanHpfDirty { true };    // re-push all cutoffs to swp30

    // --- Per-part 3-band peaking EQ (see setPartEqGain/Freq/Q) ---
    // Stored as the editor's values; gain/Q are converted to dB/Q when pushed to swp30.
    // Must stay in step with the Part module's own box defaults, which PartPanel
    // derives from kEqBandFreqDflt[1..3] (raw 28/34/46 -> these same frequencies).
    static constexpr int kPartEqFreqDefault[3] = { 500, 1000, 4000 };   // Hz
    int partEqGain[32][3];   // raw 0..127, 64 = 0 dB (default 64 = flat)
    int partEqFreq[32][3];   // absolute Hz, 32..16000 (on the freqladder)
    int partEqQ   [32][3];   // raw 1..120 (0.1..12.0), default 7
    std::atomic<bool> partEqDirty { true };      // re-push all bands to swp30
    static bool partEqValid(int part, int band) { return part >= 0 && part < 32 && band >= 0 && band < 3; }
    void initPartEqDefaults();                   // seed the raw arrays to flat defaults

    // --- Per-drum-key EQ/HPF (see setDrumHpf/setDrumEq*), keyed by (buffer, key) ---
    int   partDrumBuffer[32];        // DRUMS buffer per part (-1 = melodic), pushed to swp30
    float drumHpfHz[4][128];         // [buffer][key] absolute Hz, 0 = off
    int   drumEqGain[4][128][3];     // raw 0..127 (64 = 0 dB)
    int   drumEqFreq[4][128][3];     // absolute Hz, 32..16000 (on the freqladder)
    int   drumEqQ   [4][128][3];     // raw 1..120 (0.1..12.0)
    std::atomic<bool> drumFxDirty { true };      // re-push map + targets to swp30
    static bool drumIdx(int buffer, int key) { return buffer >= 0 && buffer < 4 && key >= 0 && key < 128; }
    static bool drumBandIdx(int buffer, int key, int band) { return drumIdx(buffer, key) && band >= 0 && band < 3; }
    void initDrumFxDefaults();

    // Reset every plugin-only, non-SysEx feature (tempo-SYNC, HPF2, mid-band + drum-key
    // EQ) to its default. A .syx carries only device (XG) state, so loading one — which
    // replaces the whole patch — must also clear these, since the file can't address
    // them. Called from loadEditorFile (NOT from project restore, which keeps them).
    void resetPluginOnlyState();
private:
    void pushPartEqBand(int part, int band);     // convert raw->physical, send to swp30
    void pushDrumHpf (int buffer, int key);      // push a (buffer,key) HPF cutoff to swp30
    void pushDrumBand(int buffer, int key, int band);   // convert + push a (buffer,key) EQ band

    // Last target handed to swp30, so the analyser's per-frame setSpectrumTarget call
    // is a no-op unless something actually changed. Packed the same way swp30 packs it
    // (enable<<16 | part<<8 | key); -1 = "nothing pushed yet", which forces the first
    // push through even if it is the disable.
    int lastSpectrumTarget { -1 };

    // Same idea for the per-part level meter: -1 = "nothing pushed yet", so the first
    // push goes through even when it is the disable, and MAME being down resets it so
    // a reboot re-pushes. 0/1 otherwise.
    int lastPartLevelMeter { -1 };

public:
    // Armed by setPartLevelMeter, read by processBlock. Audio thread reads it once
    // per block; when it is false the output-peak pass does not run at all.
    std::atomic<bool>  masterMeterArmed { false };
    // Peak |sample| of the main output since the last takeMasterPeak(). Single
    // writer (audio thread), single reader (message thread), so the read-modify-write
    // is a plain load/store pair — losing one block's peak to a race with the
    // reader's exchange costs a meter frame nobody can see.
    std::atomic<float> masterPeak { 0.0f };
private:

    // Guards the mameMachine pointer's VALIDITY (not the machine's internals):
    // shared = "a lease is out, the machine may not be destroyed"; exclusive =
    // publishMachine() swapping it. See the MameLease note below.
    std::shared_mutex mameLifetimeLock;
public:

    // --- MAME engine lifetime ------------------------------------------------
    // `mameMachine` is NOT an ordinary pointer. MAME's running_machine is a STACK
    // object living in mame_machine_manager::execute()'s frame on the MAME thread,
    // so the instant that thread unwinds the pointer targets freed stack memory —
    // which Windows soon decommits or hands to another thread. Dereferencing it
    // then is an access violation, not merely stale data.
    //
    // So a bare `if (mameMachine)` test is NOT a safe guard, for two reasons:
    //   1. It is check-then-use: the machine can die between the test and the
    //      dereference, and the reader is on a different thread entirely.
    //   2. mameIsFullyBooted used to latch true forever, so after the first boot
    //      it stopped saying anything about the CURRENT machine.
    // That combination is what killed the standalone on ~11% of cold boots: the
    // editor's spectrum probe (message thread) and the anchor sync in
    // processBlock() (audio thread) both dereferenced a machine that was gone.
    //
    // Every access from a thread other than MAME's must therefore go through
    // MameLease, which holds the engine pinned for as long as the reference is
    // used. Code already running ON the MAME thread (the OSD update() callbacks,
    // the MIDI port polls) may use mameMachine directly — the machine cannot
    // disappear underneath its own thread.
    running_machine* mameMachine = nullptr;
    std::atomic<bool> mameIsFullyBooted { false };

    // MAME's emulated clock, published by the MAME thread for every other thread to
    // read. Written ONLY from VstOsdInterface::update() (MAME thread, past the phase
    // gate); everyone else reads it through senderMameTime(). Public for the same
    // reason mameIsFullyBooted is: the OSD writes it. See senderMameTime() for why
    // running_machine::time() must never be called off the MAME thread.
    std::atomic<double> publishedMameTime { 0.0 };
    std::atomic<bool> isRomMissing      { false };
    std::atomic<bool> isMameRunning     { false };

    // Non-zero once the MAME engine has been terminated by a swallowed SEH
    // exception (see the __except in runMameEngine). The emulator is dead and the
    // plugin is silent from that point; this is the only trace it leaves.
    std::atomic<unsigned long> mameFaultCode { 0 };

    // Publishes (or retracts, with nullptr) the live machine. Retracting also
    // clears mameIsFullyBooted so the flag tracks THIS machine, not "ever booted",
    // and blocks until every in-flight lease has been released.
    void publishMachine(running_machine* machine);

    // Pins the engine for the lifetime of the object. Falsy when MAME is down;
    // when truthy the machine is guaranteed to stay alive until it goes out of
    // scope. Keep the scope SHORT — teardown waits on it.
    class MameLease
    {
    public:
        explicit MameLease(YamahaMU100AudioProcessor& p)
            : lock(p.mameLifetimeLock),
              machine(p.mameIsFullyBooted.load(std::memory_order_acquire) ? p.mameMachine
                                                                          : nullptr) {}
        explicit operator bool()      const { return machine != nullptr; }
        running_machine& operator*()  const { return *machine; }
        running_machine* operator->() const { return machine; }

    private:
        std::shared_lock<std::shared_mutex> lock;
        running_machine* machine;
    };

    // True once the device is booted AND past the one-shot boot XG-reset (fired at
    // emulated t=3.0s). Loading a .syx or reading state back before this races the
    // boot reset (which wipes the load) or the still-flooding boot traffic, so the
    // editor defers a file load until this is true.
    bool isReadyForEditing() const
    {
        return mameIsFullyBooted.load(std::memory_order_acquire)
            && xgResetSent.load(std::memory_order_acquire);
    }

    // Gates the deferred MAME launch in prepareToPlay so it only fires once.
    // Plugin scanners / GUI-only / metadata-only host instantiations never call
    // prepareToPlay, so MAME never boots in those contexts. This is what stops
    // Bitwig (and similar hosts that re-instantiate the plugin on GUI open) from
    // cold-booting MAME and resetting to the default patch on every editor open.
    std::atomic<bool> mameHasStarted    { false };

    // Throttle event: MAME signals this when it's ready for the next batch
    juce::WaitableEvent mameThrottleEvent { false };

    // Buffer threshold (samples) — how far MAME may run ahead of processBlock.
    // Clamped to kMaxBufferThreshold everywhere it is set, including on read from
    // settings.xml: the ring sizes below are asserted against that bound, so a
    // hand-edited file must not be able to exceed it.
    std::atomic<int> mameBufferThreshold { 2048 };

    // The largest threshold the Options ▸ Audio dropdown offers, and the bound BOTH
    // ring buffers are sized against. MAME may legitimately run this far ahead of the
    // drain, so a ring smaller than this laps its own reader — silently, since the
    // stale frames are well-formed audio. Raising the dropdown means raising this,
    // and the static_asserts by the rings will refuse to build if a ring no longer fits.
    static constexpr int kMaxBufferThreshold = 32768;   // ~743 ms at 44100

    // PDC Trim (ms): added to the ring-buffer threshold and reported to the DAW via
    // setLatencySamples so already-recorded MIDI is triggered earlier to compensate for
    // both the buffer pre-fill and the plugin's residual pipeline lag.
    // Index into the option array {0,5,10,15,20,25,30,40,50}.
    std::atomic<int> pdcTrimIndex { 0 };
    static int trimMsForIndex(int idx);
    int  getTrimSamples() const;
    void setPdcTrimIndex(int idx);      // editor-side entry point (message thread)
    void setBufferThreshold(int samples); // editor-side; updates atomic + reports latency
    void reportLatencyToHost();         // recomputes setLatencySamples(threshold + trim)
    void saveSettings();                // persist settings.xml (romPath, lastSyxDir, MIDI routing, …)

    // MULTI-INSTANCE: settings.xml is ONE file and a host can hold several instances,
    // each with its own copy of every option in memory. A save that wrote that whole
    // copy would revert anything another instance had changed since this one loaded --
    // and the editor's destructor saves, so merely closing one instance's window was
    // enough to undo an option set in the other (measured 2026-09-11). So a save now
    // writes only the keys THIS instance changed and leaves the rest of the file as it
    // finds it. `settingsSnapshot` is what this instance believes is on disk: the file
    // as loaded at construction, and after that whatever it last wrote. The diff
    // against it is what "this instance changed it" means.
    //
    // It does NOT make a change propagate to an instance that is already open; that is
    // unchanged and documented. It only stops one instance's stale copy overwriting
    // another's deliberate change.
    std::unique_ptr<juce::XmlElement> buildSettingsXml() const;   // our whole view
    std::unique_ptr<juce::XmlElement> settingsSnapshot;           // ...as last seen on disk
    // settings.xml holds every Option EXCEPT the per-part output pair, which is project
    // state only (see setPartOutputsEnabled). In particular romPath, the PDC trim, the
    // buffer threshold and the MIDI input routing are deliberately NOT in project state:
    // each describes the MACHINE, not the song. The trim is stored in milliseconds and
    // the threshold counts MAME-rate samples (see reportLatencyToHost), so both are
    // already invariant across a project's sample rate; what varies is host block size
    // and CPU load, which is per-installation. MIDI input routing is standalone-only
    // (initMidiDevices returns in a plugin), and the standalone does not persist project
    // state at all.

    // Editor working model (see sendParamChange / loadEditorState). Owned via
    // unique_ptr so the codec header stays out of this MAME-macro-heavy header.
    std::unique_ptr<mu100::DeviceState> editorState;

    // ROM path (user-configurable via settings.xml or hardcoded default)
    juce::String romPath;

    // Directory of the most recently loaded SysEx file (persisted in settings.xml
    // so the file browser reopens where the user last picked a .syx file).
    juce::String lastSyxDir;

    // Directory of the most recently saved bulk dump (persisted in settings.xml).
    // Seeded to My Documents/MEGAN XG on first use, then follows the user.
    juce::String lastDumpDir;

    // Directory of the most recently saved native "complete setup" (.mss) file
    // (persisted in settings.xml, like lastSyxDir / lastDumpDir).
    juce::String lastSetupDir;

    // File > Open Recent: the most-recently opened-or-saved setup/SysEx files, newest
    // first, capped at kMaxRecentFiles. Lives in settings.xml (an APP-level list, like
    // the lastXxxDir fields above) and NOT in project state — a recent list belongs to
    // the installation, not to one DAW project, and putting it in the state blob would
    // also make two projects fight over it.
    static constexpr int kMaxRecentFiles = 5;
    juce::StringArray recentFiles;
    // Record a path at the head, removing any earlier occurrence (case-insensitively on
    // Windows) and trimming to the cap. Saves settings itself, so callers need not.
    void addRecentFile(const juce::String& fullPath);

    // Bottom-strip loaded-file display + unsaved-changes marker (see
    // editorStateSignature / markStateSyncedToFile). Persisted in get/setStateInformation
    // (the DAW project state), NOT settings.xml — they belong to the patch, not the app.
    juce::String loadedFilePath;
    juce::String savedStateSig;

    // Which view the editor opens in: "Hardware" (photo front panel) or "Editor"
    // (the XGEdit-style parameter editor). Persisted in settings.xml.
    juce::String defaultEditorView { "Hardware" };

    // Which PALETTE the editor view draws in: "Dark" (the original XGEdit-derived
    // set) or "Light". Persisted in settings.xml, and kept SEPARATE from
    // defaultEditorView on purpose: the theme is remembered across a trip through
    // the Hardware view, so switching Hardware -> editor comes back to the theme
    // last used rather than resetting to dark. The Hardware view is the front-panel
    // photo and is not themed at all — see applyEditorTheme().
    juce::String editorTheme { "Dark" };

    // True while the DAW transport is playing (sampled from the play head in
    // processBlock). The editor reads it to auto-suppress MIDI-triggered drum-key
    // selection during playback, so edits stay on the chosen key while a track
    // auditions them. Written on the audio thread; read on the message thread.
    std::atomic<bool> dawIsPlaying { false };

    // DAW project tempo, sampled from the play head in processBlock (hosted only;
    // stays at its last value / default when standalone). Read on the message
    // thread by effectiveBpm(). See the tempo-sync editor feature.
    std::atomic<double> dawBpm { 120.0 };

    // Standalone user tempo (the System-module TEMPO knob). Ignored when hosted
    // (the DAW tempo wins). Persisted in project state (get/setStateInformation).
    std::atomic<double> syncTempo { 120.0 };

    // Sparse per-(unit,type) effect tempo-sync state; key = unit*1000 + typeIndex.
    // Plugin-only, persisted in project state, never sent as SysEx.
    std::map<int, SyncEntry> effectSync;

    // When false (default), MIDI-triggered drum-key selection is suppressed while
    // dawIsPlaying is true; left-click selection on the on-screen keyboard is
    // never affected. When true, MIDI notes keep selecting the drum key even
    // during playback (useful when playback is sparse). Persisted in settings.xml.
    // Message-thread only (options checkbox writes it, keyboard timer reads it).
    bool midiDrumSelectDuringPlayback { false };

    // Where editor tooltips are shown (default: the bottom bar -- it never lands
    // under the cursor and so never covers the control being read). Persisted in
    // settings.xml as `tooltipMode`; the old boolean `showTooltips` is still read
    // for migration (on -> BottomBar, off -> None). The VST options dropdown /
    // standalone submenu write it; the editor's timer applies it to its
    // TooltipWindow (Popup) or drives the bottom-strip readout itself (BottomBar).
    enum TooltipMode { TooltipPopup = 0, TooltipBottomBar = 1, TooltipNone = 2 };
    int tooltipMode { TooltipBottomBar };

    // When the keyboard cursor's MARK is drawn (default: always). Persisted in
    // settings.xml as `focusMarkMode`.
    //
    // There is deliberately NO "never". The mark is the standing promise that the
    // unmodified arrow keys act on the one control it is around -- and in a DAW, where
    // the shortcut layer starts off, it is the ONLY warning that the accessibility key
    // floor has made those keys live on whatever was last clicked. An invisible cursor
    // plus bare arrows is a footgun; "only while using the computer keyboard" removes
    // the distraction for a mouse-only user without creating one.
    enum FocusMarkMode { FocusMarkAlways = 0, FocusMarkKeyboard = 1 };
    int focusMarkMode { FocusMarkAlways };

    // "Part module title shows" (default: BOTH halves on — the part's number and voice,
    // and the drum instrument). Persisted in settings.xml as `partTitleNames`.
    //
    // A BITMASK, and the menu is two CHECK items rather than four radio rows, because
    // the two halves are independent: they name different things, in modes used by
    // different people for different lengths of time. Someone who wants the drum tone
    // named while picking their way round a kit has no reason to want the voice named
    // in part mode, or the other way about.
    //
    // It shipped OFF, on the reasoning that a name in the corner of the eye is a
    // convenience to some and a distraction to others and the module is legible
    // without it. Turned ON by default 2026-09-04 at the user's direction: the name is
    // what connects this module to the part it is editing, and the title is the only
    // place that connection is stated. Both halves, because the drum half answers that
    // same question in the mode where it is least guessable. Turning it back off is two
    // menu rows, and settings.xml remembers that.
    enum PartTitleNames { PartTitleNamePart = 1, PartTitleNameDrum = 2 };
    int partTitleNames { PartTitleNamePart | PartTitleNameDrum };

    // "Label notes as" (default: note letters). Persisted in settings.xml as
    // `noteLabels`, and pushed into notelabel::gMode, which every surface that prints
    // a note as text reads -- see NoteLabel.h for the list and for why the option
    // exists at all (the octave DIGIT is not standard between DAWs; the letter is).
    //
    // A display option with no bearing on any value, so it lives in settings.xml with
    // the rest of the editor preferences rather than in project state: it describes
    // how this installation reads, not what the song contains.
    enum NoteLabelMode { NoteLabelLetters = 0, NoteLabelNumbers = 1 };
    int noteLabelMode { NoteLabelLetters };

    // "MIDI Out sends edits as" (default: XG SysEx, the behaviour this plugin has
    // always had). Persisted in settings.xml as `midiOutFormat`: it describes how this
    // installation records, not what a song contains.
    //
    // EXACTLY TWO MODES, AND NO FALLBACK BETWEEN THEM -- the user's decision
    // (2026-10-04). In CC mode an edit with no CC equivalent sends NOTHING rather than
    // its SysEx: a recording that is mostly CC with stray SysEx mixed in is harder to
    // read and edit than either format alone. Equally, a CC-capable edit is NEVER
    // withheld for being ambiguous on playback (a part on port B, two parts sharing a
    // Rcv channel): those limits are documented, not enforced. The one edit that
    // cannot be sent is a part on Rcv OFF, which has no channel to put a CC on.
    //
    // CC mode also sends the Controller module's wheel (mod wheel, pitch bend,
    // aftertouch, AC1/AC2), which SysEx mode never has: those are performance
    // gestures with no SysEx form, and recording a filter sweep through AC1 is the
    // case that wants them.
    enum MidiOutFormat { MidiOutSysEx = 0, MidiOutCC = 1 };
    int midiOutFormat { MidiOutSysEx };

    // "Mark DAW-automatable params" (default: triangles). Persisted in settings.xml as
    // `automationMarkers`. Draws an indicator by the NAME label of every editor control
    // that also exists as a host-automatable parameter (see AutomationParams) — the
    // System module's VOLUME + KSHIFT and the Part module's channel-strip row. It says
    // "your DAW can automate this one natively"; every other parameter is still
    // reachable as XG SysEx on a MIDI track, so the marker distinguishes the two kinds
    // of automation rather than marking what is automatable at all.
    //
    // VST-ONLY: the standalone has no host to automate from, so the editor forces
    // the mark off there and the option appears in the VST Options menu alone.
    //
    // A PLAIN ON/OFF since 2026-10-04 (was Triangles / Underlines / None). The
    // underline now belongs to the MIDI CC mark below, so the DAW mark is always the
    // triangle and the only choice left is whether to draw it. The settings key did
    // not move: `automationMarkers` still holds 0 = on and 2 = off, and the retired 1
    // (underlines) reads as on -- it was a style of "on", not a way of saying off.
    bool markDawAutomatable { true };

    // "Mark parameters that send MIDI CC" (default on, both hosts). Persisted as
    // `markMidiCcParams`. An UNDERLINE under the name of every editor control whose
    // edits go out as a MIDI controller in MIDI CC mode (see midiOutFormat). Drawn
    // ONLY while MIDI Out is in CC mode -- in SysEx mode every XG edit records, so
    // the mark would answer a question nobody is asking -- and the Options item grays
    // there rather than resetting, so the choice survives a trip through SysEx mode.
    bool markMidiCcParams { true };

    // Whether a MULTI PART parameter has a MIDI controller this plugin sends for it
    // (kCcParams, the device-measured table), and which. The ONE answer for both the
    // output path and the editor's marks, so a mark cannot promise a controller the
    // tee does not send.
    static int midiCcForMultiPartParam(const char* paramName);

    // "Invert knob/dial direction" (default off). Persisted in settings.xml. Reverses the
    // drag/wheel direction on the editor's knobs + value boxes only; the live flag
    // is the dialaction::gReverse global (see DialAction.h), which the VST options
    // checkbox / standalone menu item update in step with this member.
    // NOTE the member and its settings.xml key stay `reverseDialAction` although the
    // menu item was renamed from "Reverse dial action" (2026-08-17). Renaming the KEY
    // would silently drop the setting from every existing settings.xml, and the option
    // would come back off for anyone who had turned it on.
    bool reverseDialAction { false };

    // "Show changed markers" (default ON). Persisted in settings.xml. When OFF, every
    // mark that says "this value is not its default" is left unpainted -- the tab on a
    // value box or a selection box, the Voice module's row roll-up, the Graphic
    // Display's highlighted bars and envelope handles, and the Keyboard's drum marker
    // strip. The live flag is the changedmarks::gShow global (see ChangedMarks.h),
    // which the Options menu item updates in step with this member.
    //
    // DISPLAY ONLY, and it costs no layout: every gutter a tab lives in is reserved
    // unconditionally, so nothing reflows either way and the values themselves are
    // untouched. The MIDI-override marks are a different question with their own
    // switches and are deliberately not covered.
    bool showChangedMarks { true };

    // "Enable plugin-only parameters" (default ON). Persisted in settings.xml. When
    // OFF, the editor greys out and blocks every parameter the MU100/XG hardware does
    // NOT have — HPF2 and the Part EQ's three peaking mid bands (2-4), part- and
    // drum-key-level alike — so an edit session can't produce anything the `.syx`
    // export would silently drop. It is an EDITING guard only: existing values stay
    // in effect and stay in project state, they simply can't be changed while it's off.
    // (Effect tempo-SYNC is deliberately NOT covered — it only computes a raw ms/Hz
    // that IS written to the device and IS carried by the `.syx`.)
    //
    // Stated in the AFFIRMATIVE since 2026-09-07, so the menu reads like every other
    // toggle beside it and a tick means "this is available" rather than "this is taken
    // away". The old `disablePluginOnlyParams` attribute is still READ, once, to carry
    // an existing settings.xml across — see loadSettings.
    bool enablePluginOnlyParams { true };

    // "Enable DAW automation" (default ON). PROJECT STATE ONLY -- like the per-part
    // output pair, and for a related reason: the automation data itself already lives
    // in the song, so the switch that decides whether it does anything belongs beside
    // it rather than in a settings.xml shared with every other project. A project
    // saved before this existed has no attribute and reads as ON, which is also what
    // a new instance comes up as.
    //
    // What it actually does is stop AutomationParams' 30 Hz bridge. What it CANNOT do
    // is remove the parameters: VST3 registers them at construction, long before
    // setStateInformation runs (see the ctor), and their identity and order are part
    // of the plugin's public contract. So with this off the host still lists all 515 --
    // they simply stop moving anything and stop following the editor. That silence is
    // why refreshAutomationMarkers() also drops the markers when this is off: a
    // triangle that says "the DAW can automate this" would be a lie, and its absence
    // is the only cue the user gets in the place they are actually looking.
    bool dawAutomationEnabled { true };
    void setDawAutomationEnabled(bool on);   // message thread

    // "Show part MIDI receive switches" (default ON). Persisted in settings.xml. When
    // off, the Part module's bottom row of 17 Rcv LEDs (MULTI PART 08 nn 30..40) is
    // HIDDEN — purely a visibility preference, so the switches keep whatever values
    // they hold and the device is not touched either way. The module keeps its height:
    // the row's strip is simply left blank, because module rects are fixed design
    // pixels (see EditorView) and shrinking one would shift the Keyboard and Voice
    // columns every time the option is toggled.
    bool showPartRcvSwitches { true };

    // "Show Mute and Solo columns" (default ON). Persisted in settings.xml. When off,
    // the Voice list's M and S cells are hidden and the voice-name column reclaims
    // their width -- the module and the canvas keep their size, for the same reason
    // the Rcv row above keeps its strip.
    //
    // Hiding the columns does NOT clear mute or solo: the state is still real and
    // still audible, which is precisely why the bottom bar carries an indicator for
    // it that is independent of this option.
    bool showMuteSoloColumns { true };

    // "Voice names" (default OFF = full names). Persisted in settings.xml. When on,
    // every voice name the plugin shows is the 8-character one the MU100's own LCD
    // prints -- "GrandPno" rather than "Grand Piano" -- across the Voice module, the
    // Voice List browser, drum and SFX kits included. It is a DISPLAY choice over one
    // table (kVoices carries both strings), so nothing is stored or transmitted
    // differently and switching back is lossless.
    //
    // File -> Name All Output Tracks from Voices deliberately follows it, so the DAW's
    // track names match what is on screen. To keep full names in the editor but short
    // ones on the tracks: switch to 8-character, name the tracks, switch back.
    //
    // The live copy is voicenames::gShort (Source/VoiceNaming.h) -- read from the
    // message thread AND from whatever thread the host asks getBusInfo() on, which is
    // why that one is atomic and this one is only the persisted value.
    bool shortVoiceNames { false };

    // --- Keyboard shortcuts (Options -> "Use keyboard shortcuts" / "Edit keyboard
    // shortcuts...") ---------------------------------------------------------------
    // Both live in settings.xml and NOWHERE ELSE. A key map describes the
    // INSTALLATION, not the song; carried in DAW
    // project state as well, an old project would silently redefine the user's keys.
    //
    // The default is OFF in a plugin and ON in the standalone, decided in the ctor
    // rather than here: a hosted plugin that starts eating Ctrl+Z from the DAW the
    // first time its window is clicked is a surprise nobody asked for, while the
    // standalone has no host to take anything from. Both are one menu tick away.
    //
    // The editor is the only reader. Nothing here touches the audio thread: a key
    // event arrives on the message thread and runs the same handler a button click
    // would, so the feature costs a hosted session exactly nothing.
    bool           useKeyboardShortcuts { true };
    kbsc::Bindings keyBindings;

    // OFF hosted, ON standalone. Used to seed useKeyboardShortcuts when settings.xml
    // does not name it (a fresh install, or one predating the feature).
    bool defaultUseKeyboardShortcuts() const
    {
        return wrapperType == juce::AudioProcessor::wrapperType_Standalone;
    }

    // "MIDI input indicators" (default ON). Persisted in settings.xml. When on, the
    // Voice module flashes a per-part orange tab as MIDI data arrives for that part.
    // Read live by PartListPanel's animation timer.
    bool midiInputIndicators { true };

    // "Played notes on the keyboard" (default ON). Persisted in settings.xml. When on,
    // the Keyboard module lights every key currently held by incoming MIDI on the
    // selected part's Rcv channel, in the same green the right-drag audition uses.
    // Read live by KeyboardPanel's animation timer, which runs regardless (it also
    // drives the SYS/FX/BULK lamps), so this needs no editor nudge the way the EQ
    // analyser and the volume meters do.
    bool keyboardPlayedNotes { true };

    // --- Display Animation options (all persisted in settings.xml) ---
    // These are VIEW preferences, so they live in settings.xml like showTooltips and
    // midiInputIndicators — NOT in project state, where a saved song would then
    // dictate a machine-level display preference.
    // "EQ animated spectrum" (default ON): draws the live per-part spectrum under the
    // EQ screen's response curve.
    bool eqSpectrumEnabled { true };
    // FFT size as a power of two. 12 = 4096 (~11 Hz bins, ~93 ms window) — the default
    // because coarser bins can't resolve what HPF2 does down at 20-1000 Hz.
    int  eqSpectrumFftOrder { 12 };
    // Index into kSpectrumDecayMs: how fast a peak falls back. Attack is always instant.
    int  eqSpectrumDecayIdx { 2 };
    static constexpr int kSpectrumFftOrderMin = 10;   // 1024
    static constexpr int kSpectrumFftOrderMax = 13;   // 8192
    static constexpr double kSpectrumDecayMs[4] = { 120.0, 300.0, 700.0, 1500.0 };
    static constexpr int kSpectrumDecayCount = 4;
    double spectrumDecayMs() const
    {
        return kSpectrumDecayMs[juce::jlimit(0, kSpectrumDecayCount - 1, eqSpectrumDecayIdx)];
    }

    // "Animated volume level meters" (default ON): the VOLUME screen draws each
    // part's setting as a thin dash and fills the bar below it with that part's live
    // output level. It shipped OFF, unlike the EQ analyser, because it is not purely a
    // GUI cost: it turns on a capture inside the emulated mixer, the same reasoning
    // that keeps the per-part outputs off until asked for. Turned ON by default
    // 2026-09-04 at the user's direction — the capture is one peak per part per block
    // and measured too small to matter, and a level display nobody switched on tells
    // nobody anything.
    bool volumeMetersEnabled { true };
    // Index into kSpectrumDecayMs — the SAME four choices the analyser offers, on
    // purpose: they are the same question ("how fast does a peak fall back") asked of
    // two displays, and one table means the two cannot drift apart in wording or
    // timing. Attack is instant on both.
    int  volumeMeterDecayIdx { 1 };
    // Which colour ramp the meters draw with — an index into edcol::meterRamps().
    // The ONE user-choosable palette in the editor: the meters are eye candy, and
    // the ramp is the part of them a user has an opinion about. Each ramp carries
    // its own per-theme values, so choosing one cannot break either theme.
    int  volumeMeterRampIdx { 0 };
    // How far down the bar the ramp's stops sit — an index into edcol::meterSpreads().
    // 0 = Standard (the meter-bridge convention). Read LIVE by paintMeterBar, so a
    // change needs no apply step: the meter's own 30 fps tick repaints anyway.
    int  volumeMeterSpreadIdx { 0 };
    // Whether the animated level is one continuous column or a stack of lit segments
    // — an index into edcol::meterFills(). 0 = Solid, which is what the meters have
    // always been. Cosmetic only, and read LIVE by paintMeterBar like the spread.
    int  volumeMeterFillIdx { 0 };
    double volumeMeterDecayMs() const
    {
        return kSpectrumDecayMs[juce::jlimit(0, kSpectrumDecayCount - 1, volumeMeterDecayIdx)];
    }

    // Last Editor-view window size, persisted in settings.xml so the editor
    // window reopens at the size the user left it (the Hardware view recomputes
    // its size from the photo's fixed aspect ratio, so it isn't persisted).
    // 1200 -> 1229 with the canvas going 1480 -> 1516 (mute/solo columns), so a
    // new install opens at the same on-screen scale as before. A SAVED size is
    // deliberately left alone: it is the user's window, and it simply renders the
    // slightly wider canvas ~2.5% smaller until they resize.
    // The size a fresh install opens the Editor view at, and what the "reset window
    // size" shortcut returns to. Named so the editor and the settings default cannot
    // drift apart -- they were two literals in two files.
    // 1229 -> 1244 (2026-09-09), in the same proportion as the canvas widening that
    // paid for the Voice list's two override tabs, so a NEW install opens at the same
    // on-screen scale as before. A SAVED size is left alone and simply renders ~1%
    // smaller until the window is next resized.
    static constexpr int kDefaultEditorViewW = 1244;
    static constexpr int kDefaultEditorViewH = 862;
    int editorViewW { kDefaultEditorViewW }, editorViewH { kDefaultEditorViewH };

    // Editor-view TEXT size, as integer percentages, persisted in settings.xml.
    // 100 = the tuned default look (the ~30-35% baseline enlargement lives in
    // edtext::kBaseline, so a clean 100 here already renders larger than the raw
    // design-pixel sizes); higher values scale further. Categories: parameter
    // values / control labels / module titles / voice-list names / the Graphic
    // Display's value readouts. Applied to the edtext:: globals on load. See
    // EditorTextScale.h.
    int editorTextValuesPct    { 100 }, editorTextLabelsPct { 100 },
        editorTextTitlesPct    { 100 }, editorTextVoiceListPct { 100 },
        editorTextGrDisValuePct { 100 };

    // The OPTION WINDOWS' label text (Audio Settings, Display Animation, and the
    // standalone's MIDI Input / MIDI Output), same percentage scale. Those windows
    // are not on the editor canvas, so 100 here is exactly the sizes the dialogs
    // are laid out for -- and the layout follows it (edtext::optPx), so raising it
    // widens the label columns and the windows rather than clipping the text.
    int editorTextOptionsPct { 100 };

    // Per-instance MAME workspace (homepath / cfg / nvram / state). Unique temp
    // subdir so concurrent boots in a multi-instance project don't race on the
    // same .cfg / .nv files. Created in runMameEngine(), deleted in dtor.
    juce::File mameWorkDir;

    // --- MU100 work-RAM save/restore ---
    // getStateInformation grabs the 128 KB H8S work RAM live through the
    // memshare("workram") tag defined in ymmu100.cpp, base64-encodes it, and
    // writes it into the project state block. setStateInformation stages the
    // bytes; bootMameIfNeeded writes them as mu100/workram in the per-instance
    // nvram dir before launching MAME, and ymmu100's nvram_device picks them
    // up in device_start — before the H8 executes its first instruction.
    static constexpr size_t WORKRAM_BYTES = 0x20000;  // 128 KB
    juce::MemoryBlock pendingWorkRam;

    // --- Restored-project fallback: push the patch when work RAM did NOT carry it ---
    // A reopened project normally needs no device write at all: the work-RAM snapshot
    // restores the device wholesale, which is why nothing else on this path transmits.
    // But that snapshot is not guaranteed to exist. getStateInformation can only take it
    // if MAME was already running (see its `mameHasStarted` guard), so a project saved in
    // the first seconds after the plugin is inserted — or after an engine fault — carries
    // NO workram attribute. And even a valid blob is only applied if it is staged BEFORE
    // the engine launches; state arriving after prepareToPlay has already booted MAME
    // misses its window.
    //
    // Either way the device comes up at factory defaults while the editor model holds the
    // saved patch: panels right, sound wrong, with no visible cue. That state is worse
    // than it sounds, because the intuitive recovery makes it permanent — Read from Device
    // would overwrite the good model with the defaulted device.
    //
    // So: if a project restored a model and work RAM did not actually reach the boot, push
    // the whole patch once the device is ready. sendPatchToDevice() is exactly right here
    // — the device really is at factory defaults, so its step-1 diff against the defaults
    // is accurate rather than an assumption.
    //
    // `workRamAppliedAtBoot` is the honest signal (set where the blob is written to the
    // nvram dir), NOT "was the attribute present" — that would miss the boot-order case
    // above. The decision is deferred to ready-time because at setStateInformation the
    // boot has usually not happened yet.
    // Atomic because setStateInformation's calling thread is the HOST's choice, while the
    // poll below runs on the message thread.
    std::atomic<bool> workRamAppliedAtBoot   { false };
    std::atomic<bool> restoredModelNeedsPush { false };
    // The restored model was HEALED (healEffectParamsOutOfRange changed something), so
    // the work-RAM snapshot is known to hold values the model no longer does -- push
    // even when the snapshot applied. Set by setStateInformation only.
    std::atomic<bool> restoredModelHealed { false };
    juce::uint32 restorePushDeadlineMs { 0 };   // set on the first poll; message thread only

    // Polled from the SysExInDrain tick rather than given a timer of its own: that timer
    // already runs at 20 Hz for the whole life of the processor, started from the
    // constructor where the message thread IS guaranteed. Starting a timer from
    // setStateInformation would not be — the host picks that thread. Boot takes ~3 s, so
    // 20 Hz is ample, and the poll is one atomic read while disarmed.
    void serviceRestorePatchPush();

    // MU100_FORCE_PATCH_PUSH=1 takes the fallback even when work RAM was applied, so the
    // path can be exercised deliberately. It is a rare path by construction, and a rare
    // path nothing ever runs is how `pendingGlobalRestore` came to carry a comment
    // describing behaviour it never had.
    static bool forceRestorePatchPush();

    // --- Global effect/EQ/system blocks: NO restore push, and none is needed ---
    // There used to be a `pendingGlobalRestore` here: getStateInformation snapshots the
    // global XG effect/EQ/system blocks (aH 00/02/03 — reverb/chorus/variation incl. its
    // CONNECTION, Multi EQ, insertion effects) into the `globalState` attribute, and
    // setStateInformation staged those bytes for requestDeviceDump to re-send, on the
    // premise that the work-RAM snapshot did not carry them. Removed 2026-08-07:
    //
    //  * The premise is WRONG. Measured: a project reopen whose editor model had been
    //    clobbered back to defaults (the seedDefaults bug fixed in b3fcae0) still PLAYED
    //    the saved reverb and variation. Nothing was transmitted on that path, so the
    //    device could only have got them from restored work RAM. Work RAM carries these
    //    blocks.
    //  * It never ran anyway. A project reopen SKIPS the read-back (editorStatePopulated
    //    is true), so requestDeviceDump — the only sender — was never reached, and the
    //    staged bytes sat indefinitely. The next thing to consume them was whatever manual
    //    File -> Read from Device came later, which pushed the project-LOAD-time blocks
    //    over whatever the user had since edited and then read that back: silent loss of
    //    live edits, triggered by an action that reads as "sync FROM the device".
    //  * It could not have saved the case it looks like insurance for. If work RAM were
    //    missing or rejected, the device would be wrong EVERYWHERE — parts, voices, drum
    //    setups — and nothing pushes those either. That case needs a whole-patch
    //    sendPatchToDevice() after boot, not a partial block push.
    //
    // The `globalState` attribute itself is GONE as of 2026-08-07 — neither written nor
    // read. It briefly outlived its reader for backward compatibility, then that reason
    // lapsed: the plugin has never been distributed, so no older build will ever read a
    // file this one writes. Projects and .mss files that still carry it load fine, since
    // nothing looks for it, and `editorState` covers the same addresses anyway.
    // `.syx` export never had it — that is pure XG SysEx with no XML wrapper.

    // Host sample rate — the rate the DAW/device is currently running at.
    std::atomic<double> hostSampleRate { 44100.0 };

    // The rate MAME is always launched with (-samplerate). This is FIXED, deliberately,
    // and must equal the SWP30's stream rate: swp30.cpp allocates both of its streams at
    // a hardcoded 44100 (`stream_alloc(..., 44100, STREAM_SYNCHRONOUS)`), and MAME inserts
    // a resampler on a route only where the rates DISAGREE (sound_stream::create_resamplers
    // — it stores nullptr when source rate == target rate). So matching it keeps MAME's
    // own converter out of the emulation path entirely.
    //
    // Launching MAME at the host rate instead (what this used to do) put MAME's default
    // resampler — RESAMPLER_LOFI, a 4-point cubic with no real anti-imaging filter — on
    // every sample of a 48 kHz session. Pinning moves that conversion to the drain below,
    // where juce::LagrangeInterpolator (5-point) does it, and leaves the emulation running
    // an identical path at every host rate, which matters because most of the open bugs
    // here are subtle-artifact hunts and the hardware reference captures are all at 44100.
    // At a 44100 host the ratio is exactly 1.0 and processBlock takes its sample-for-sample
    // fast path, so that case is bit-identical to having no resampler at all.
    static constexpr double kMameSampleRate = 44100.0;

    // The rate MAME was actually launched with (-samplerate), pinned in
    // bootMameIfNeeded() and read by VstOsdInterface to configure the MAME audio node.
    // MAME fixes its output rate at machine start — machine().sample_rate() is what
    // builds the speakers, the effects and the record buffer, and its OSD generation
    // mechanism reconfigures ROUTING, not that rate — so it cannot follow a later host
    // rate change, and rebooting to match would throw the whole patch away. The ring is
    // resampled to the host rate on the way out instead. 0 = MAME not launched yet.
    std::atomic<double> mameSampleRate { 0.0 };
    double effectiveMameRate() const
    {
        // The pre-boot fallback is the CONSTANT, not the host rate: prepareToPlay sizes
        // the resampler scratch from this ratio *before* bootMameIfNeeded() pins it, and a
        // host running below kMameSampleRate gives a ratio > 1 — i.e. more ring samples
        // consumed than output samples produced. Falling back to the host rate would size
        // that first block's scratch for 1.0 and overrun it.
        const double r = mameSampleRate.load(std::memory_order_relaxed);
        return (r > 0.0) ? r : kMameSampleRate;
    }

    // --- LCD pixel buffer ---
    static constexpr int LCD_BUF_SIZE = 80 * 16;
    std::mutex lcdMutex;
    uint8_t    lcdPixels[LCD_BUF_SIZE] {};
    std::atomic<bool> lcdUpdated { false };

    // --- Button state ---
    std::atomic<uint8_t> buttonPressedP7 { 0 };
    std::atomic<uint8_t> buttonPressedP8 { 0 };

    // Set when a front-panel button is pressed in the Hardware view, cleared by every
    // wholesale model replacement (read-back, File > New / Open, project restore). The
    // emulated device sends nothing back when its own panel edits it, so while this is
    // true the editor's model may no longer describe what the device holds. Lives in the
    // PROCESSOR, not the editor, so it survives the GUI closing — same reasoning as the
    // undo stack. Message-thread only in practice; atomic to match its neighbours.
    std::atomic<bool> hardwarePanelEdited { false };

    // --- Front-panel LED state (mulcd_device::get_leds(), bits 0-5 = Play/Edit/Util/Effect/Mode/EQ) ---
    std::atomic<uint8_t> ledState { 0 };

    //==============================================================================
    // Standalone MIDI routing
    // The MU100 has two physical MIDI IN jacks:
    //   MIDI IN A (port 0) → mdin_a → H8S SCI rx<1> → parts  1–16
    //   MIDI IN B (port 1) → mdin_b → H8S SCI rx<0> → parts 17–32
    // Each host MIDI device can route to port 0, port 1, or both.
    static constexpr uint8_t MIDI_ROUTE_PORT1 = 0x01;  // MIDI IN A (parts 1–16)
    static constexpr uint8_t MIDI_ROUTE_PORT2 = 0x02;  // MIDI IN B (parts 17–32)

    struct MidiDeviceSettings {
        bool    enabled   { false };
        uint8_t routeMask { MIDI_ROUTE_PORT1 };
        // Whether notes from this device may pick the drum key in drum-edit mode
        // (Keyboard module). Standalone only: the VST gates the same selection on the
        // DAW transport instead, which it can see and the standalone cannot. Defaults
        // TRUE so activating a device behaves as it always has — the setting is here to
        // turn OFF the inputs that carry recorded parts, not to arm the feature.
        bool    drumSelect { true };
    };

    // Called from the editor's routing dialog (message thread only)
    bool    isMidiDeviceEnabled(const juce::String& identifier) const;
    uint8_t getMidiDeviceRouteMask(const juce::String& identifier) const;
    void    setMidiDeviceEnabled(const juce::String& identifier, bool enable);
    void    setMidiDeviceRouteMask(const juce::String& identifier, uint8_t mask);
    bool    isMidiDeviceDrumSelect(const juce::String& identifier) const;
    void    setMidiDeviceDrumSelect(const juce::String& identifier, bool enable);
    void    reconnectMidiDevices();   // close + re-open all enabled MIDI inputs (recovery)

    // MIDI OUTPUT routing (standalone only). The plugin has one logical MIDI output —
    // the editor's XG SysEx echo (see sendSysEx / the "MIDI-OUT SysEx echo" section) —
    // which the standalone can send to any number of host MIDI output devices, e.g. a
    // virtual loopMIDI port a DAW records. Mirrors the input-device model but with no
    // Port A/B distinction (there is a single output). Message thread only.
    bool    isMidiOutputDeviceEnabled(const juce::String& identifier) const;
    void    setMidiOutputDeviceEnabled(const juce::String& identifier, bool enable);

private:
    std::thread mameThread;

    // --- Audio ring buffers ---
    static constexpr int RING_BUFFER_SIZE = 65536;
    static_assert(RING_BUFFER_SIZE >= 2 * kMaxBufferThreshold,
                  "main ring must hold the largest selectable backlog");
    float ringBufferL[RING_BUFFER_SIZE] {};
    float ringBufferR[RING_BUFFER_SIZE] {};

    // --- Output DC blocker ---
    // One-pole HPF on each output channel to remove the DC bias inherent to
    // some SWP30 DPCM samples (some piano key-groups encode loop content
    // ~80% toward the positive rail; without DC removal, the persistent
    // offset combines with hard clipping to produce audible buzz).
    //
    // The corner stands in for the hardware's output coupling caps, so it belongs
    // well below the audio band. It was 20 Hz until 2026-09-22, which is -3 dB at
    // 20 Hz, -2.1 dB at A0 and -1 dB at 40 Hz -- a bass cut, not just a DC block.
    // At 5 Hz: -0.26 dB at 20 Hz, -0.07 dB at 40 Hz; a DC step now settles in
    // ~32 ms instead of ~8 ms, still far below hearing.
    static constexpr double kDcBlockerHz = 5.0;
    struct DCBlocker {
        float x1 = 0.0f, y1 = 0.0f, a = 0.997f;
        void prepare(double sampleRate, double cutoffHz);
        void reset() { x1 = 0.0f; y1 = 0.0f; }
        float process(float x) {
            const float y = x - x1 + a * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };
    DCBlocker dcBlockerL, dcBlockerR;

    // --- Per-part aux output buses (see setPartOutputsEnabled) ---
    // Written by pushAudioFromMame at the SAME writePos as ringBufferL/R and read
    // in processBlock off the SAME totalRead cursor, which is what keeps the aux
    // buses sample-aligned with the main output. Interleaved as
    // auxRing[(part * 2 + lr) * AUX_RING_SIZE + index].
    //
    // Sized against kMaxBufferThreshold, NOT against the default threshold: the
    // backlog is bounded by whatever the user picked, so the aux ring has to hold the
    // largest of those or a big-buffer session silently reads frames the writer has
    // already lapped. 2x the bound mirrors the main ring's own margin.
    //
    // ~16 MB, but ALLOCATED LAZILY in setPartOutputsEnabled and only when the per-part
    // buses are switched on — a default instance pays nothing for this.
    //
    // Growing it does NOT worsen the writer's cost (its channel stride touches 64
    // cache lines per frame): that cost is 64 lines and 64
    // TLB entries per frame whatever the stride, since even the old 64 KB stride put
    // every channel on its own page. The lever named there — shrinking this — would not
    // have helped, and shrinking it now would break the assert below.
    static constexpr int kNumAuxChannels = kNumPartBuses * 2;   // 64
    static constexpr int AUX_RING_SIZE   = 65536;               // ~16 MB, lazily allocated
    static_assert(AUX_RING_SIZE >= 2 * kMaxBufferThreshold,
                  "aux ring must hold the largest selectable backlog, or the per-part "
                  "buses read frames the writer has already overwritten");

    // Full-scale reference for the captured mixer-bus samples, so a part routed to its
    // own output sits at the level it contributed to the main mix.
    //
    // NOT the spectrum probe's 1/(1<<23): that is the full scale of a voice at the END
    // of awm2_step, one stage EARLIER than this tap, and using it left every part bus
    // 21 dB quiet. Measured on the real chain, a part's dry bus value v reaches the
    // main output as (v * 2.828) >> 4 against a 1<<17 full scale, so matching that
    // level takes 2.828/16*64 = 2^3.5 more gain than the probe's reference gives.
    //
    // The 2.828 (= 2*sqrt(2)) is the MEG's dry-path gain, and it was measured constant
    // across master volume, part volume and a change of effect program — master and
    // part volume both scale this tap too, since they are applied upstream of it. The
    // one thing that does move it is the MASTER EQ, which lives inside the MEG: with
    // it engaged the buses no longer match the main output. That is deliberate and
    // consistent with the rest of the feature — the per-part buses are pre-MEG, so a
    // master-bus process does not belong on them any more than the reverb does.
    //
    // Like the probe's reference, this does not promise |sample| <= 1: a part with
    // several voices stacked can exceed full scale, as it can in the hardware mix.
    static constexpr float kPartBusScale = 11.3137085f / float(1 << 23);   // 2^-19.5 total
    std::atomic<bool> partOutputsEnabled { false };   // the user's setting
    std::atomic<bool> partOutputsDirty   { false };   // re-push to swp30 after (re)boot
    std::atomic<bool> partOutputsRemoveFromMain { false };
    // Which aux buses the host has enabled, published by processBlock (audio thread)
    // and consumed by the MAME thread. Deliberately NOT read straight from getBus()
    // on the MAME thread, and deliberately not pushed to swp30 from processBlock:
    // the audio thread must not take the engine-lifetime lock.
    std::atomic<uint32_t> auxBusMask       { 0 };
    // MAME thread: the mask last handed to swp30, and whether it is still known to be
    // what the chip holds. The validity MUST be a separate flag, not a magic mask value:
    // all 32 buses enabled is 0xffffffff, a perfectly legal mask, so a sentinel of
    // 0xffffffff read as "already pushed" and silently skipped the push in exactly the
    // case where every part should have left the main mix.
    uint32_t              auxExcludePushed = 0;
    bool                  auxExcludeValid  = false;
    // Per-part mute/solo. Message thread writes, MAME thread pushes; the same
    // dirty/valid shape as the exclusion above, and for the same reason — a fresh
    // swp30 comes up with everything audible, so a (re)boot must re-push. Note the
    // validity is again its OWN bool: 0xffffffff ("everything audible") is both the
    // default and a perfectly legal mask, so it cannot double as "unknown".
    std::atomic<uint32_t> partMuteMask { 0 };
    std::atomic<uint32_t> partSoloMask { 0 };
    // The one writer of the pair, and the one place their mutual exclusion lives.
    void setMuteSoloMasks(uint32_t mute, uint32_t solo);   // message thread
    std::atomic<bool>     partAudibleDirty { false };
    uint32_t              partAudiblePushed = 0xffffffffu;
    bool                  partAudibleValid  = false;
    // Allocated once, on first enable, and then NEVER freed: the MAME thread may be
    // mid-write when the option is switched off, and a 4 MB buffer is not worth a
    // lifetime protocol. auxRingLive gates use of it and is the only thing toggled.
    std::unique_ptr<float[]> auxRing;
    std::atomic<bool> auxRingLive { false };
    std::vector<int32_t> partFrameScratch;            // MAME thread: swp30 -> ring
    // Short-read canary. A count, not a one-shot flag: the first block after capture
    // is enabled is legitimately short (the FIFO only starts filling then), so "it
    // happened once" and "it happens every block" must be distinguishable — they mean
    // completely different things about whether the buses are aligned.
    bool     auxShortReadLogged = false;              // first occurrence logged
    uint32_t auxShortReadCount  = 0;                  // blocks short since last report
    uint64_t auxShortReadFrames = 0;                  // frames missing since last report
    uint64_t auxShortReadNextLog = 0;                 // totalWritten gate, ~1 s apart
    // MU100_PARTOUT_TRACE: per-part peak-hold of the captured frames, so the buses can
    // be verified outside a DAW (the Standalone build cannot play them).
    int32_t  auxTracePeak[kNumAuxChannels] {};
    DCBlocker auxDcBlocker[kNumAuxChannels];
    juce::LagrangeInterpolator auxResampler[kNumAuxChannels];
    std::vector<float> auxResampleScratch;            // kNumAuxChannels x need

    // --- Dynamic output bus names (BusNameProvider) ---
    // The host may call getBusInfo() from a thread of its choosing, and the names are
    // derived from editorState (a std::map owned by the message thread), so the two are
    // decoupled by this snapshot: refreshBusNames() composes on the message thread under
    // the lock, getDynamicOutputBusName() only copies out.
    static constexpr int kMaxBusNameLen = 128;        // VST3 String128
    juce::CriticalSection busNameLock;
    // [0] = main out, [1..32] = the per-part aux buses. Empty = decline, i.e. leave the
    // static construction-time name ("Output" / "Part n") in place.
    juce::String       busNameCache[1 + kNumPartBuses];
    std::atomic<bool>  busNamesDirty { false };

    std::atomic<uint64_t> totalWritten { 0 };
    uint64_t midiDiagLastRead = 0;   // DIAG: once-a-second midi-clock log

    // --- DIAG: the audio path's own witness (open "goes silent" bug) ---
    // The MIDI totals answer "did input arrive"; these answer the next question
    // down, "did anything come OUT". processBlock keeps a one-second peak window
    // on both ends of the ring — what MAME produced and what the host received —
    // and stamps a packed record here when the output crosses between silent and
    // active. The OSD update() (MAME thread) drains and logs it: the audio thread
    // must never open a file, and MameDiag reopens per line.
public:
    // MIDI input counters for the engine log's "midi totals" line (and MU100_MIDIDIAG):
    // they tell "no MIDI ever arrived" apart from "it arrived, so a silence is
    // downstream". One relaxed add per byte, which is nothing at MIDI rates.
    std::atomic<uint32_t> midiDiagRawIn   { 0 };  // OS callbacks, pre-boot-gate
    std::atomic<uint32_t> midiDiagInBytes { 0 };  // bytes in from the OS callback
    std::atomic<uint32_t> midiDiagToMame  { 0 };  // bytes handed to MAME's SCI
private:
    std::atomic<uint64_t> totalRead    { 0 };

    // Ring → host-rate conversion, used ONLY while the two rates differ. A ratio of
    // exactly 1.0 keeps the original sample-for-sample drain, so the ordinary case is
    // bit-identical to having no resampler at all.
    juce::LagrangeInterpolator resamplerL, resamplerR;
    std::vector<float> resampleScratchL, resampleScratchR;   // sized in prepareToPlay

    // --- Timestamped MIDI ring buffers (one per MAME MIDI IN port) ---
    //
    // MULTI-producer, single-consumer. THREE different threads inject bytes: the
    // Windows MIDI callback threads (one per open input device — the notes the user
    // plays), the message thread (every editor edit, file load and read-back request)
    // and, on the VST3 path, the audio thread. The consumer is the MAME thread, via
    // pollMidiData()/readMidiByte().
    //
    // This used to be a plain SPSC ring — load writePos, write the slot, store the
    // advanced writePos — with no lock and no CAS, which lost bytes when two producers
    // claimed the same slot and, worse, let a 3-byte note-on land INSIDE a 9-byte
    // parameter change. The emulated H8's SCI is mid-SysEx there, so it eats the note
    // as payload and every following byte shifts: a wrong value at a wrong XG address.
    //
    // The fix is a whole-message reservation, not a lock. Producers claim all of a
    // message's slots in one CAS on `reserved`, fill them, then publish each slot with
    // a release store to `ready`; the consumer only advances past a slot whose `ready`
    // flag is set. A producer caught mid-fill therefore delays delivery by a few
    // microseconds instead of corrupting the stream, and no producer ever blocks —
    // which is the constraint a mutex could not meet, since the audio thread would
    // have waited on the message thread holding it mid-bulk-dump.
    struct TimestampedMidi { uint8_t data; double targetMameTime; };
    static constexpr uint32_t MIDI_BUFFER_SIZE = 524288;   // power of two
    static constexpr uint32_t MIDI_BUFFER_MASK = MIDI_BUFFER_SIZE - 1;

    struct MidiRing
    {
        TimestampedMidi      slot [MIDI_BUFFER_SIZE];
        std::atomic<uint8_t> ready[MIDI_BUFFER_SIZE] {};   // 0 = free/consumed, 1 = published

        // Free-running counters (not indices) — masked at use. MIDI_BUFFER_SIZE is a
        // power of two that divides 2^32, so unsigned wraparound is exact.
        std::atomic<uint32_t> reserved { 0 };   // producers hand out slots from here
        std::atomic<uint32_t> readPos  { 0 };   // consumer only

        // "Stop reading" (read-back cancel) drops the queued Dump-Request backlog.
        // The request is raised on the message thread; the consumer honours it by
        // discarding up to `flushTo`. Both index fields stay consumer-thread-only.
        std::atomic<bool>     flushRequested { false };
        uint32_t              flushTo { 0 };    // MAME thread only

        // One log line per ring per run when the head-of-line watchdog fires. MAME
        // thread only. See midiHeadDue().
        bool watchdogLogged { false };
    };

    MidiRing midiRing;    // MIDI IN A (port 0, midiin1)
    MidiRing midiRing2;   // MIDI IN B (port 1, midiin2)

    void serviceMidiFlush(MidiRing& ring);   // MAME thread only

    // The furthest ahead of the published clock a MIDI byte may legitimately be
    // scheduled. The read-back paces one continuous request stream — ~16 global
    // blocks, 32 parts x 5 chunks, then 79 notes x 4 sub-blocks per drum setup — which
    // reaches ~25 s of emulated time with all four setups in use (see
    // EditorView::requestReadBack). 120 s is several times that, while still being
    // astronomically below a garbage timestamp.
    //
    // It does double duty: pushMidiMessageToPort() clamps to it, and midiHeadDue()
    // treats a head beyond it as impossible. Because every push is clamped at push
    // time and the clock only ever advances, `target > now + kMaxMidiLead` can only
    // mean the value is corrupt — so the watchdog needs no timer and no state, and
    // can never fire on legitimately-paced traffic.
    static constexpr double kMaxMidiLeadSeconds = 120.0;

    // True when the ring's head slot may be handed to MAME. MAME thread only.
    bool midiHeadDue(MidiRing& ring, uint32_t idx, double now, int port);

    // --- Received-SysEx queue (see drainIncomingSysEx in the public section) ---
    // Multi-producer by the same reservation discipline as MidiRing: a producer claims
    // 4 + len bytes in ONE CAS, then publishes the payload and the trailing three header
    // bytes before the FIRST header byte, so a header byte whose ready flag is set
    // guarantees (through its release) that the whole message behind it is complete.
    static constexpr uint32_t SYSEX_IN_SIZE = 1u << 18;   // 256 KB, power of two
    static constexpr uint32_t SYSEX_IN_MASK = SYSEX_IN_SIZE - 1;

    struct SysExInRing
    {
        uint8_t              raw  [SYSEX_IN_SIZE];
        std::atomic<uint8_t> ready[SYSEX_IN_SIZE] {};
        std::atomic<uint32_t> reserved { 0 };
        std::atomic<uint32_t> readPos  { 0 };
    };
    SysExInRing sysexIn;
    void queueIncomingSysEx(const uint8_t* data, size_t len);   // any producer thread

    // Runs drainIncomingSysEx() on the message thread. A plain member rather than a base
    // class so the processor's own lifetime controls it.
    struct SysExInDrain : juce::Timer
    {
        explicit SysExInDrain(YamahaMU100AudioProcessor& p) : owner(p) {}
        void timerCallback() override
        {
            owner.drainIncomingSysEx();
            owner.drainIncomingChannelMsgs();   // the MIDI-override shadow
            owner.serviceRestorePatchPush();   // disarmed: one atomic read
        }
        YamahaMU100AudioProcessor& owner;
    };
    SysExInDrain sysexInDrain { *this };

    // --- Received CHANNEL messages -> the MIDI-override shadow ------------------
    // Program change, bank select and the controllers that map to a stored MULTI PART
    // parameter. Queued from the callback thread and translated on the message thread
    // by drainIncomingChannelMsgs(), because the translation has to read the model:
    // channel -> part is not 1:1 (two ports, 32 parts, several parts may share a
    // channel, and Rcv CHANNEL is editable).
    //
    // One 32-bit word per message -- these are all 2- or 3-byte messages, so unlike
    // the SysEx ring there is no variable-length payload and no reservation dance
    // beyond claiming one slot.
    static constexpr uint32_t CHAN_IN_SIZE = 1u << 10;    // 1024 messages
    static constexpr uint32_t CHAN_IN_MASK = CHAN_IN_SIZE - 1;

    struct ChanInRing
    {
        uint32_t              raw  [CHAN_IN_SIZE] {};   // port<<24 | status<<16 | d1<<8 | d2
        std::atomic<uint8_t>  ready[CHAN_IN_SIZE] {};
        std::atomic<uint32_t> reserved { 0 };
        std::atomic<uint32_t> readPos  { 0 };
    };
    ChanInRing chanIn;
    void queueIncomingChannelMsg(int port, uint8_t status, uint8_t d1, uint8_t d2);
    bool drainIncomingChannelMsgs();                            // message thread
    void applyIncomingChannelMsg(int port, uint8_t status, uint8_t d1, uint8_t d2);

    // Bank select arrives as two CCs and takes effect only when a PROGRAM CHANGE
    // follows -- MEASURED, arm 2 of MU100_DIAG_MIDIPC: the CCs alone changed nothing
    // on the device. So they are held per (port, channel) until the program change
    // that consumes them. -1 = nothing pending.
    int pendingBankMsb[2][16];
    int pendingBankLsb[2][16];

    // --- The override shadow itself (message thread only) ----------------------
    std::unique_ptr<mu100::DeviceState> midiOverrideState;
    // Which PARAMETERS are overridden, for the count and the summary list. Keyed
    // (paramIndex, instance, instance2) -- the shadow state alone cannot be counted
    // that way, because one parameter may hold several bytes.
    std::set<uint64_t>    midiOverrideParams;
    std::atomic<int>      midiOverrideCount { 0 };
    std::atomic<uint32_t> midiOverrideSeq   { 0 };

    static uint64_t midiOverrideKey(int paramIndex, int instance, int instance2)
    {
        return ((uint64_t) (uint32_t) paramIndex << 32)
             | ((uint64_t) (uint32_t) (instance  & 0xFFFF) << 16)
             |  (uint64_t) (uint32_t) (instance2 & 0xFFFF);
    }
    // Record one parameter's DEVICE value. Drops the entry instead when the device has
    // landed back on what the model already says, so a mark never claims a difference
    // that is not there.
    void noteMidiOverride(int paramIndex, int instance, int instance2,
                          const uint8_t* bytes, int numBytes);
    // A parameter-change message the EDITOR just sent (or a lane drove) puts the device
    // and the model back in step at those addresses, so any override there is stale.
    void clearMidiOverridesForMessage(const std::vector<uint8_t>& msg);
    void clearAllMidiOverrides();       // wholesale model replacement: nothing to shadow
    void bumpMidiOverrideSeq();

    // --- Coalesced parameter changes (see sendParamChangeThrottled) ---
    // Latest pending message per XG address, keyed model<<21 | aH<<14 | aM<<7 | aL, so
    // a drag collapses to one transmission per address per tick.
    //
    // A VECTOR, in INSERTION order — not a map keyed by address. Re-touching an address
    // overwrites its payload in place and leaves its position alone, so a flush emits
    // writes in the order the editor made them. An address-ordered flush would be fine
    // for one dragged knob (a single address) but silently reorders independent writes
    // once several controls are in flight, which is a trap to leave behind for whoever
    // throttles the next panel. N is the number of DISTINCT addresses touched inside one
    // 33 ms tick — realistically 1-3 — so the linear scan is cheaper than a map.
    // Message-thread only; the flush timer runs there too.
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pendingParamChanges;

    // 30 Hz: fast enough that a drag still feels continuous (a released knob's final
    // value goes out within ~33 ms, with no mouse-up hook needed), and slow enough
    // that the worst case — every one of the 16 effect slots dragged at once — is
    // ~4800 bytes/s... still over the wire, but the tail pacing in sendSysEx absorbs
    // that. A realistic single-knob drag is ~300 bytes/s, a tenth of the port.
    static constexpr int kParamFlushHz = 30;

    // Like SysExInDrain, this belongs to the PROCESSOR, not the editor: a pending edit
    // must still be transmitted if the GUI is closed in the same frame it was made.
    struct ParamFlush : juce::Timer
    {
        explicit ParamFlush(YamahaMU100AudioProcessor& p) : owner(p) {}
        void timerCallback() override { owner.flushPendingParamChanges(); }
        YamahaMU100AudioProcessor& owner;
    };
    ParamFlush paramFlush { *this };


    // --- Undo / Redo state (see EditUndo.h and the public API above) ---
    // Message-thread only, and that is the whole threading story: editorState is a
    // std::map owned by the message thread, every send funnel runs there, and
    // processBlock provably touches neither. So this needs no lock, no atomic, and no
    // new rule — which is why the feature can be added with zero audio-thread cost.
    //
    // undoPos is the number of steps currently APPLIED: undo walks it down, redo walks
    // it up, and a fresh edit truncates everything above it (the discarded redo branch).
    std::deque<mu100undo::Step> undoStack;
    int undoPos = 0;
    int undoSuppressDepth = 0;
    int undoApplies = 0;   // bumped by every undo()/redo(); see undoApplyCount()
    // How many steps the kDepth cap has discarded off the FRONT since the last
    // undoClear(). Only the History window reads it, and only to decide whether entry
    // 0 may be called "Initial State": once anything has been dropped, the oldest
    // reachable state is no longer the one the session (or the last load) started in.
    // Reset by undoClear, because a wholesale load re-baselines the history — the
    // loaded patch IS the initial state from that moment on.
    int undoDropped = 0;
    // Label for the UndoScope currently open, if any; empty otherwise.
    juce::String undoScopeLabel;
    int          undoScopeDepth = 0;
    // The open scope's gesture key, 0 = "this scope never merges with another".
    uint32_t     undoScopeMergeKey = 0;
    // Has the open scope created its step yet? Without this, the scope's first write
    // would fold into whatever unrelated step happened to be on top when it opened —
    // silently welding the previous edit onto this one.
    bool         undoScopeHasStep = false;

    // Capture hooks, called from the two send funnels before they touch the model.
    void undoPush(const std::vector<uint8_t>& msg);
    void undoPushDrumChange(int buffer, std::vector<mu100undo::ByteDelta>&& edits);
    // The one body behind pastePart / resetPart: replay a part block as ordinary
    // parameter edits inside ONE undo step. `in` holds the block addressed at part 0
    // (what collectPartBlock produces); `drumXml` carries the buffer's per-key layer, or
    // is null to let the buffer fall back to the incoming kit's defaults.
    // One part's worth of AddressMap parameters, grouped the way the send order needs
    // them (see the .cpp). Derived from the map, never a list of names.
    struct PartParamPlan
    {
        int                mode      = -1;              // PART MODE
        int                voice[3]  { -1, -1, -1 };    // bank MSB, bank LSB, PROGRAM
        int                portaTime = -1;
        std::vector<int>   ordinary;                    // the rest, in address order
        std::set<uint32_t> covered;                     // part-0 addresses the map owns
    };
    static void buildPartParamPlan(PartParamPlan& plan);
    bool applyPartBlockUndoable(const mu100::DeviceState& in, int part, int targetBuffer,
                                const PartPluginExtras& extras,
                                const juce::XmlElement* drumXml, const juce::String& label);
    std::unique_ptr<juce::XmlElement> partCopyXml;   // a .mps tree, held in memory
    int partCopySrc { -1 };
    void collectDrumBufferEdits(int buffer, int oldMsb, int oldLsb, int oldPgm,
                                int newMsb, int newLsb, int newPgm,
                                std::vector<mu100undo::ByteDelta>& out) const;
    void applyDrumEdits(const mu100undo::Step& s, bool useOld);
    void undoPushPlugin(const mu100undo::PluginDelta& d);
    // Common tail: truncate the redo branch, merge-or-append, cap the depth.
    void undoCommit(mu100undo::Step&& step, uint32_t mergeKey);
    // Rebuild a step's messages carrying the OLD (or NEW) byte values.
    std::vector<std::vector<uint8_t>> undoBuildMessages(const mu100undo::Step& s, bool useOld);
    void undoApplyPlugin(const mu100undo::PluginDelta& d, bool useOld);
    juce::String undoDescribe(const mu100undo::Step& s, bool forward) const;
    // "Drum COWBELL " for an undo label, or "Drum buf 1 key 71 " when the voice
    // cannot be named. Panel-independent by necessity — a tooltip is read with the
    // GUI possibly closed, so this resolves the kit from the model itself.
    juce::String undoDrumWhere(int buffer, int key) const;
    // Effect parameter -> (layout, slot) via the type the model currently holds, so
    // its value can be formatted the way the Effect Parameters panel shows it.
    bool undoEffectSlot(const std::vector<uint8_t>& msg,
                        int& layoutOut, int& slotOut, bool& isVarOut) const;
    // Snapshot a drum buffer's non-default plugin-only values into `into`, so undoing
    // the kit assignment that wiped them brings them back.
    void undoSnapshotDrumBuffer(int buffer, std::vector<mu100undo::PluginDelta>& into);

    // --- DAW automation (see AutomationParams.h) ---
    // Held by pointer so PluginProcessor.h stays free of the codec headers; created
    // in the constructor because VST3 parameter identity and ORDER are part of the
    // plugin's public contract and must not depend on run-time state.
    std::unique_ptr<AutomationParams> automation;

    // --- Outgoing MIDI pacing (see sendSysEx) ---
    // The emulated time at which the last byte we scheduled for the device finishes.
    // ONE cursor covers both MIDI IN ports. The ports are independent wires, so this
    // over-paces a port-B send that follows a port-A one — deliberately: the editor's
    // combined output is a small fraction of one port's capacity, and a single cursor
    // cannot get the two out of step with each other.
    // Each BULK send starts at max(now + 0.05, this), so consecutive bulk messages are
    // laid out back-to-back at the wire rate instead of all restarting at "now" and
    // being handed to MAME in one lump. loadSysExFile / requestDeviceDump build their
    // own timelines and push the tail forward to their end, so one bulk operation is
    // scheduled after another rather than into the middle of it.
    //
    // SMALL sends no longer touch this cursor — they are stamped at the clock (see
    // smallSendStartTime), because scheduling ahead of the clock barriers the host's
    // notes. Ordering against a bulk stream survives that: the ring is consumed in
    // reservation order regardless of timestamps.
    // Message-thread only.
    double sendTailMameTime { 0.0 };

    // Latched so the "edit not transmitted" notice costs one log line per run, not one
    // per edit — the condition persists for as long as the engine is down.
    bool loggedUnsentParamChange { false };

    // Claim the next `numBytes` byte-slots on MIDI IN A's outgoing timeline and return
    // the time the first of them should be stamped with, so no two bulk streams can be
    // scheduled on top of each other.
    // Pacing cursor for BULK streams only (loadSysExFile, requestDeviceDump, sendPanic)
    // — hundreds to thousands of messages that genuinely must be spread over time.
    // Small sends must NOT use this: it schedules into the future, and a future-stamped
    // message is a head-of-line barrier in front of the host's notes. See
    // smallSendStartTime.
    double reserveSendWindow(size_t numBytes);
    // Start time for a small message-thread send: the emulated clock, never ahead of it.
    // The rule that keeps editor edits and DAW automation from scattering note timing.
    double smallSendStartTime() const;
    // Copy a well-formed F0…F7 message onto the plugin's MIDI OUT (host bus / open
    // standalone devices). Shared by sendSysEx and sendSysExAtClock so the two paths
    // cannot drift in what they echo for DAW recording.
    void   teeSysExToMidiOut(const std::vector<uint8_t>& msg);

    // How far ahead of the published clock the cursor may run before reserveSendWindow
    // resets it to "now" and pacing restarts. A safety valve, not an operating point:
    // at the coalesced rate the cursor never gets more than a few ms ahead. It catches
    // a stale cursor after a MAME reboot and bounds worst-case editor latency to ~2 s.
    static constexpr double kMaxSendTailLeadSeconds = 2.0;

    // VST3 only: MIDI IN B events handed over by the patched wrapper via
    // receivePortBMidi(), consumed in processBlock(). Audio-thread only.
    juce::MidiBuffer pendingPortBMidi;

    // --- MIDI-OUT echo for DAW recording ---
    // Editor param/voice edits funnel through sendSysEx() on the message thread; a copy
    // of each one is queued here and drained in processBlock() into the plugin's MIDI
    // output bus, so a host can record the editor's edits. What the copy IS depends on
    // midiOutFormat: the XG SysEx itself (the byte format XGEdit produced), or the
    // channel controller that does the same job (see ccForParamChange). So an entry is
    // either a whole F0..F7 message or a 2-3 byte channel message, told apart by its
    // first byte. Guarded by outSysExLock (message thread pushes, audio thread drains;
    // not the real-time-critical audio path itself).
    juce::CriticalSection             outSysExLock;
    std::vector<std::vector<uint8_t>> pendingOutSysEx;

    // Queue one channel message for the MIDI OUT (host bus + standalone devices).
    void teeChannelToMidiOut(const uint8_t* bytes, int n);
    // The CC a one-byte MULTI PART parameter change is equivalent to, on the part's own
    // Rcv channel, with the value converted where the device needs it. False when the
    // parameter has no CC or the part receives on no channel (Rcv OFF).
    bool ccForParamChange(const std::vector<uint8_t>& msg, uint8_t out[3]) const;

    // Most-recent incoming note-on, for the editor's drum-key selection (see
    // observeNoteOn / takeLatestNoteOn). seq bumps on each note-on so the editor
    // can tell a new note from a repeat of the same number.
    std::atomic<int>      lastNoteOnNote { -1 };
    std::atomic<uint32_t> lastNoteOnSeq  { 0 };

    // Held-note set per raw receive channel (see heldNotesFor). [chan][note / 32].
    std::atomic<uint32_t> heldNotes[32][4] {};
    void setHeldNote(int rawChannel, int note, bool on)
    {
        if (rawChannel < 0 || rawChannel > 31 || note < 0 || note > 127) return;
        auto& w = heldNotes[rawChannel][note >> 5];
        const uint32_t m = 1u << (note & 31);
        if (on) w.fetch_or (  m, std::memory_order_release);
        else    w.fetch_and(~ m, std::memory_order_release);
    }

    // Incoming-MIDI activity accumulators for the Voice module's per-part
    // indicators (see observeMidiActivity / takeMidiChanActivity / takeMidiPartActivity).
    std::atomic<uint32_t> midiChanActivity { 0 };   // bit port*16 + (ch-1)
    std::atomic<uint32_t> midiPartActivity { 0 };   // bit p = MULTI PART p (SysEx)
    std::atomic<uint32_t> midiSysActivity  { 0 };   // SYSACT_* bits (no-part messages)

    // Parts assigned to each DRUMS buffer, as a bitmask — the inverse of
    // partDrumBuffer[], maintained by setPartDrumBuffer. Lets observeMidiActivity
    // (audio / MIDI-callback thread) resolve a DRUM SETUP message to its parts with
    // a single atomic load instead of racing on the plain partDrumBuffer array.
    std::atomic<uint32_t> drumBufferParts[4] { {0}, {0}, {0}, {0} };

    // --- MIDI OUT bulk-dump capture (see pushMidiOutByte / pollCompletedDump) ---
    // dumpBuffer accumulates the concatenated bulk-dump SysEx of the current burst.
    // dumpCurrentSysex assembles the message currently arriving; only completed
    // bulk-dump messages are appended to dumpBuffer. All guarded by dumpLock
    // (MAME thread writes, editor message thread drains — not the audio path).
    juce::CriticalSection dumpLock;
    std::vector<uint8_t>  dumpBuffer;
    std::vector<uint8_t>  dumpCurrentSysex;
    bool                  dumpInSysex { false };
    std::atomic<uint32_t> lastDumpByteMs { 0 };
    uint32_t              lastDumpDeliverMs { 0 };   // last eager hand-over (read-back)
    // Idle gap that marks end-of-burst. Must exceed the MU100's max BULK OUT
    // INTERVAL TIME (300 ms, MU100 SYSTEM table 2-2) so a multi-block dump is
    // never split into separate files.
    static constexpr int  kDumpFinalizeMs = 700;
    // During a read-back, hand over accumulated complete messages at least this often
    // so the editor consumes the stream incrementally (well under kReadBackBurstGapMs).
    static constexpr int  kDumpEagerMs = 250;

    // MAME↔DAW time sync anchor
    std::atomic<bool>     needAnchorSync  { true  };
    std::atomic<double>   anchorMameTime  { 0.0   };
    std::atomic<uint64_t> anchorDawSample { 0     };

    // **Never call running_machine::time() from another thread.** device_scheduler::
    // time() (schedule.cpp) reads m_callback_timer and m_executing_device — plain
    // pointers the MAME thread mutates continuously — and then computes from
    // *m_icountptr mid-instruction. Off-thread it can return an arbitrary value, and a
    // sender that stamps a MIDI byte with one parks an unreachable targetMameTime at
    // the head of the ring, where the head-only delivery gate blocks every byte behind
    // it for the rest of the session. That was the 2026-07-30 port-A freeze: notes
    // stuck on, PANIC swallowed, engine/LCD/GUI all healthy.
    //
    // **MameLease does NOT protect against this.** It guarantees the machine is not
    // destroyed mid-call — a lifetime guarantee, not an atomicity one. Senders still
    // take a lease (it is the "is the engine up?" guard); they just must not read the
    // clock through it.
    //
    // Granularity is one OSD update (a frame or so), which is irrelevant against the
    // +0.05 s lead the senders already add.
    //
    // Declared with the other OSD-written engine state (publishedMameTime, next to
    // mameIsFullyBooted) because VstOsdInterface writes it; only the accessor lives here.
    double senderMameTime() const { return publishedMameTime.load(std::memory_order_acquire); }

    // XG Reset SysEx injected once at t=3.0s after boot
    std::atomic<bool> xgResetSent { false };

    // --- Standalone MIDI device management ---
    // Processor opens host MIDI devices directly (independent of JUCE's AudioDeviceManager),
    // routing each device's messages to the configured MAME MIDI IN port(s).
    class MidiDeviceCallback;
    struct OpenDevice {
        std::unique_ptr<juce::MidiInput>    input;
        std::unique_ptr<MidiDeviceCallback> callback;
    };
    mutable juce::CriticalSection              midiDeviceLock;
    std::map<juce::String, MidiDeviceSettings> midiDeviceSettings; // keyed by device identifier
    std::map<juce::String, OpenDevice>         openMidiDevices;
    bool midiDevicesInitialized { false };

    // MIDI OUTPUT devices (standalone only), keyed by identifier. midiOutputEnabled is
    // the persisted enable state; openMidiOutputs holds the live handles. sendSysEx()
    // forwards each editor edit to every open output. Guarded by midiDeviceLock.
    std::map<juce::String, bool>                             midiOutputEnabled;
    std::map<juce::String, std::unique_ptr<juce::MidiOutput>> openMidiOutputs;
    bool midiOutputDevicesInitialized { false };

    void initMidiDevices();        // opens saved-enabled input devices on first prepareToPlay
    void initMidiOutputDevices();  // opens saved-enabled output devices on first prepareToPlay

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(YamahaMU100AudioProcessor)
};
