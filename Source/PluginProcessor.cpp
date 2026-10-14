// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "codec/MU100State.h"
#include "codec/MU100AddressMap.h"
#include "codec/MU100Display.h"      // raw byte -> the text the control shows
#include "EffectAddressing.h"        // effect param address <-> (unit, slot)
#include "XgEffectFormat.h"          // type-dependent effect value formatting
#include "FreqLadder.h"              // plugin-only Hz controls
#include "codec/MU100FactoryDefault.h"
#include "codec/MU100Voices.h"   // output bus names composed from the assigned voice
#include "codec/MU100Drums.h"    // drum-key names for the undo labels
#include "codec/MU100DrumDefaults.h"   // per-kit per-key DRUM SETUP defaults
#include "EditorTextScale.h"
#include "DialAction.h"
#include "VoiceNaming.h"   // full vs 8-character voice names (Options -> Voice names)
#include "EditorColours.h"
#include "DrumBuffer.h"   // DRUMS1..4 names, for the override "where" line
#include "AutomationParams.h"
#include "MameDiag.h"   // DIAG: engine-lifetime post-mortem log (see MameDiag.h)

#include <sstream>
#include <set>
#include <algorithm>

#ifndef _WIN32
#include <pthread.h>
#endif
#include <stdlib.h>
#include <new>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#endif

// MAME macros must come before MAME includes
#define PTR64 1
#define LSB_FIRST 1
#define NDEBUG 1
#define __STDC_LIMIT_MACROS 1
#define __STDC_FORMAT_MACROS 1
#define __STDC_CONSTANT_MACROS 1

#include "emu.h"
#include "frontend/mame/ui/ui.h"
#include "osd/modules/lib/osdobj_common.h"
#include "emuopts.h"
#include "render.h"
#include "osdepend.h"
#include "frontend/mame/mame.h"
#include "frontend/mame/clifront.h"
#include "frontend/mame/mameopts.h"
#include "drivenum.h"
#include "output.h"
#include "mame/yamaha/mulcd.h"
#include "sound/swp30_ext.h"   // plugin <-> swp30 bridge (per-part clean HPF; plain-types shim)

//==============================================================================
// MidiDeviceCallback — routes incoming MIDI from a host device to ring buffer(s)
//==============================================================================
class YamahaMU100AudioProcessor::MidiDeviceCallback : public juce::MidiInputCallback
{
    YamahaMU100AudioProcessor* proc;
    juce::String deviceId;

public:
    MidiDeviceCallback(YamahaMU100AudioProcessor* p, const juce::String& id)
        : proc(p), deviceId(id) {}

    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& msg) override
    {
        // Counted BEFORE the boot gate, so "the OS delivered nothing" and "we
        // dropped it because MAME was not up yet" stay distinguishable.
        proc->midiDiagRawIn.fetch_add(1, std::memory_order_relaxed);
        if (!proc->mameIsFullyBooted.load(std::memory_order_acquire)) return;

        // Drum-key selection is per DEVICE here (MIDI Input window, "Drum key" column):
        // a port carrying recorded parts would otherwise walk the selection around the
        // kit while it plays. The VST cannot do this — the host owns its inputs — and
        // gates on the transport instead. Note the gate is only on the SELECTION: the
        // message itself still reaches the engine and sounds below.
        if (msg.isNoteOn() && proc->isMidiDeviceDrumSelect(deviceId))
            proc->observeNoteOn(msg.getNoteNumber());

        const uint8_t mask = proc->getMidiDeviceRouteMask(deviceId);
        const auto*   raw  = msg.getRawData();
        const int     len  = msg.getRawDataSize();

        if (mask & YamahaMU100AudioProcessor::MIDI_ROUTE_PORT1)
            proc->observeMidiActivity(0, msg);
        if (mask & YamahaMU100AudioProcessor::MIDI_ROUTE_PORT2)
            proc->observeMidiActivity(1, msg);

        proc->midiDiagInBytes.fetch_add((uint32_t) len, std::memory_order_relaxed);

        // targetMameTime = 0.0 → MAME consumes immediately (real-time input).
        // Pushed as ONE message: this callback runs on the OS MIDI thread, so a
        // byte-at-a-time loop could interleave a note-on into the middle of an editor
        // SysEx being sent from the message thread.
        if (mask & YamahaMU100AudioProcessor::MIDI_ROUTE_PORT1)
            proc->pushMidiMessageToPort(0, raw, len, 0.0, 0.0);
        if (mask & YamahaMU100AudioProcessor::MIDI_ROUTE_PORT2)
            proc->pushMidiMessageToPort(1, raw, len, 0.0, 0.0);
    }
};

//==============================================================================
// VST MIDI PORT — feeds queued MIDI bytes from a specific ring buffer into MAME
//==============================================================================
class VstMidiInputPort : public osd::midi_input_port
{
    YamahaMU100AudioProcessor* processor;
    int portIndex; // 0 = MIDI IN A (midiin1), 1 = MIDI IN B (midiin2)

public:
    VstMidiInputPort(YamahaMU100AudioProcessor* p, int idx) : processor(p), portIndex(idx) {}
    ~VstMidiInputPort() override {}

    bool poll() override { return processor->pollMidiData(portIndex); }

    int read(uint8_t* pOut) override
    {
        if (processor->pollMidiData(portIndex))
        {
            const uint8_t b = static_cast<uint8_t>(processor->readMidiByte(portIndex));
            *pOut = b;
            // For the "midi totals" engine-log line. Counted where MAME's SCI actually
            // takes the byte, so a queue the delivery gate never opens stays
            // distinguishable from one it drains.
            processor->midiDiagToMame.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
        return 0;
    }
};

//==============================================================================
// VST MIDI OUTPUT PORT — receives MAME's transmitted MIDI OUT bytes (mdout) and
// forwards them to the processor's bulk-dump capture buffer.
//==============================================================================
class VstMidiOutputPort : public osd::midi_output_port
{
    YamahaMU100AudioProcessor* processor;

public:
    explicit VstMidiOutputPort(YamahaMU100AudioProcessor* p) : processor(p) {}
    ~VstMidiOutputPort() override {}

    void write(uint8_t data) override { processor->pushMidiOutByte(data); }
};

//==============================================================================
// DIAG — mirrors MAME's own osd_printf_* stream into the engine log. Without
// this, an engine that dies by emu_fatalerror (a clean C++ exit, no SEH) leaves
// no trace at all: cli_frontend::execute() prints the reason and returns an
// exit code, and in a windowed process stderr goes nowhere.
//==============================================================================
class MameOutputCapture : public osd_output
{
public:
    void output_callback(osd_output_channel channel,
                         util::format_argument_pack<char> const& args) override
    {
        // ERROR/WARNING/INFO only. VERBOSE is on in this build and would bury the
        // few lines that matter (and the log) under a per-boot device-start trace;
        // an engine that dies of emu_fatalerror reports on ERROR.
        if (channel != OSD_OUTPUT_CHANNEL_ERROR
            && channel != OSD_OUTPUT_CHANNEL_WARNING
            && channel != OSD_OUTPUT_CHANNEL_INFO)
        {
            chain_output(channel, args);
            return;
        }

        if (!mamediag::takeMameMessageBudget())
        {
            chain_output(channel, args);
            return;
        }

        const char* tag = channel == OSD_OUTPUT_CHANNEL_ERROR   ? "ERROR"
                        : channel == OSD_OUTPUT_CHANNEL_WARNING ? "WARN " : "INFO ";

        std::string text = util::string_format(args);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (!text.empty())
            mamediag::logf("  mame[%s] %s", tag, text.c_str());

        chain_output(channel, args);
    }
};

//==============================================================================
// MULTI-INSTANCE: `osd_output`'s delegate stack is a PROCESS-GLOBAL
// (`osdcore.cpp`: `static osd_output *m_stack[10]; static int m_ptr;`) with no
// lock of any kind, and a VST3 host can hold several plugin instances in one
// process -- two of which boot their engines at the same moment when a project
// containing both is opened.
//
// Every push and pop in this build is OURS: MAME's own ones live in the
// standalone front-end and the validity checker, neither of which runs here.
// Two happen per engine run -- `osd_common_t`'s constructor pushes itself, and
// the capture below -- so serialising these four points closes the race
// completely. Unsynchronised, two threads incrementing `m_ptr` can write the
// same slot; the entry that lost is then never popped, and the first
// `osd_printf_*` after its object is deleted walks the chain into freed memory.
//
// The capture itself is a PROCESS-WIDE SINGLETON, retained by each run rather
// than one per run, for a reason of its own: the stack is a chain, so a second
// instance's capture sees the FIRST one's messages too and the engine log would
// carry every MAME line twice -- and drain the shared message budget at twice
// the rate. It holds no per-instance state, so one is all that is needed.
//==============================================================================

// The lock every push and pop in this build takes. Also held across the OSD's
// construction and destruction, because those push and pop from inside
// osd_common_t.
static std::mutex& osdOutputStackLock()
{
    static std::mutex m;
    return m;
}

// The shared capture, and how many engine runs are holding it. Guarded by the
// lock above; the object itself is a never-destroyed static, so a late MAME
// message on a thread that is still unwinding cannot land on a dead object.
static MameOutputCapture& sharedMameOutputCapture()
{
    static MameOutputCapture capture;
    return capture;
}
static int gMameCaptureRefs = 0;

static void retainMameOutputCapture()
{
    const std::lock_guard<std::mutex> lock(osdOutputStackLock());
    if (gMameCaptureRefs++ == 0)
        osd_output::push(&sharedMameOutputCapture());
}

static void releaseMameOutputCapture()
{
    const std::lock_guard<std::mutex> lock(osdOutputStackLock());
    if (--gMameCaptureRefs == 0)
        osd_output::pop(&sharedMameOutputCapture());
}

//==============================================================================
// HEADLESS OSD — bridges MAME's core to the JUCE ring buffer
//==============================================================================
class VstOsdInterface : public osd_common_t
{
    YamahaMU100AudioProcessor* processor;
    running_machine* mame_machine = nullptr;
    bool loggedExitRequest = false;   // DIAG: keep the exit note one-shot

public:
    VstOsdInterface(YamahaMU100AudioProcessor* p, osd_options& options)
        : osd_common_t(options), processor(p) {}

    ~VstOsdInterface() override {}

    void process_events() override {}
    bool has_focus() const override { return true; }

    void init(running_machine& machine) override
    {
        osd_common_t::init(machine);
        mame_machine = &machine;
        processor->publishMachine(&machine);
        mamediag::logf("osd init: machine=%p phase=%d", (void*) &machine, (int) machine.phase());
    }

    void osd_exit() override
    {
        // Retract BEFORE the machine is torn down. This runs as a
        // MACHINE_NOTIFY_EXIT notifier, i.e. only on MAME's own orderly exit —
        // runMameEngine() retracts again unconditionally for the paths that
        // never reach here (see the note there).
        mamediag::logf("osd_exit (MACHINE_NOTIFY_EXIT): machine=%p", (void*) mame_machine);
        if (processor)
            processor->publishMachine(nullptr);
        mame_machine = nullptr;
        osd_common_t::osd_exit();
    }

    // MAME calls this from its frame loop — but ALSO from inside
    // running_machine::start(), via m_ui->set_startup_text() (machine.cpp:183),
    // which runs BEFORE start_all_devices() and before any device_reset(). So
    // an update() can arrive while every device in the tree is still
    // un-started and un-reset, and NOTHING below the phase gate may run then.
    //
    // This is what made the MAME engine "go away" on ~11% of cold boots: the
    // LCD block called mulcd/hd44780 render() in that window, and hd44780
    // initialises m_display_on / m_num_line / m_char_size in device_reset(),
    // not device_start(). Read before either, they are whatever the fresh heap
    // happened to contain — and `u8 line_size = 80 / m_num_line;`
    // (hd44780.cpp:478) with an indeterminate m_display_on != 0 and
    // m_num_line == 0 is an integer divide by zero, 0xC0000094, on the engine
    // thread. Hence the randomness (it is uninitialised memory, not a race)
    // and hence "booted=0" every time it happened.
    void update(bool /*skip_redraw*/) override
    {
        if (!mame_machine) return;

        const bool running = (mame_machine->phase() == machine_phase::RUNNING);

        // Only advertise the engine once the machine has actually reached the
        // RUNNING phase — see above: an update() is NOT evidence of a started
        // machine.
        if (running && !processor->mameIsFullyBooted.load(std::memory_order_relaxed))
        {
            processor->mameIsFullyBooted.store(true, std::memory_order_release);
            mamediag::logf("machine RUNNING (mameIsFullyBooted=true)");
        }

        // A stop request is honoured in ANY phase, so a plugin torn down mid-boot
        // still stops the engine. schedule_exit() only sets a flag and eats the
        // scheduler's cycles, neither of which touches a device.
        if (!processor->isMameRunning.load())
        {
            if (!loggedExitRequest)   // once, not once per frame until it stops
            {
                loggedExitRequest = true;
                mamediag::logf("update(): isMameRunning=false -> schedule_exit()");
            }
            mame_machine->schedule_exit();
            return;
        }

        if (!running)
            return;   // everything past here touches devices

        // Publish the emulated clock for the message thread. This is the ONLY place
        // it may be read: we are on the MAME thread, so device_scheduler::time() is
        // consistent here and nowhere else. See publishedMameTime.
        processor->publishedMameTime.store(mame_machine->time().as_double(),
                                           std::memory_order_release);

        processor->reassertCleanHpfIfDirty(); // re-push clean-HPF cutoffs after (re)boot
        processor->reassertPartEqIfDirty();   // re-push per-part EQ bands after (re)boot
        processor->reassertDrumFxIfDirty();   // re-push part->buffer map + drum EQ/HPF after (re)boot
        processor->reassertPartOutputsIfDirty(); // re-enable per-part capture after (re)boot

        // Inject button states into MAME's ioport system
        const auto& portmap = mame_machine->ioport().ports();

        auto it7 = portmap.find(std::string(":P7"));
        if (it7 != portmap.end())
        {
            uint8_t p7 = processor->buttonPressedP7.load();
            for (ioport_field& field : it7->second->fields())
            {
                auto fmask = static_cast<uint8_t>(field.mask());
                field.set_value((p7 & fmask) ? 1 : 0);
            }
        }
        auto it8 = portmap.find(std::string(":P8"));
        if (it8 != portmap.end())
        {
            uint8_t p8 = processor->buttonPressedP8.load();
            for (ioport_field& field : it8->second->fields())
            {
                auto fmask = static_cast<uint8_t>(field.mask());
                field.set_value((p8 & fmask) ? 1 : 0);
            }
        }

        // Update LCD pixel buffer
        auto* lcd = mame_machine->device<mulcd_device>(":lcd");
        if (lcd)
        {
            const uint8_t* pixels = lcd->render();
            {
                std::lock_guard<std::mutex> lock(processor->lcdMutex);
                memcpy(processor->lcdPixels, pixels, YamahaMU100AudioProcessor::LCD_BUF_SIZE);
            }
            processor->lcdUpdated.store(true, std::memory_order_release);

            // Front-panel LEDs (Play/Edit/Util/Effect/Mode/EQ) — bits 0-5
            processor->ledState.store(static_cast<uint8_t>(lcd->get_leds() & 0x3f),
                                       std::memory_order_release);
        }
    }

    // --- Audio ---
    bool no_sound() override { return false; }
    bool sound_external_per_channel_volume() override { return false; }
    bool sound_split_streams_per_source() override { return false; }

    osd::audio_info sound_get_information() override
    {
        osd::audio_info info;
        osd::audio_info::node_info node;
        node.m_name = "vst_audio";
        node.m_display_name = "VST Audio Output";
        node.m_id = 1;
        node.m_rate = { static_cast<uint32_t>(processor->effectiveMameRate()) };
        node.m_sinks = 1;
        node.m_sources = 0;
        info.m_nodes.push_back(node);
        info.m_default_sink = 1;
        info.m_generation = 1;
        return info;
    }

    uint32_t sound_stream_sink_open(uint32_t, std::string, uint32_t) override { return 1; }
    void     sound_stream_close(uint32_t) override {}

    void add_audio_to_recording(const int16_t* buffer, int samples_this_frame) override
    {
        if (processor)
            processor->pushAudioFromMame(buffer, samples_this_frame);
    }

    uint32_t sound_stream_source_open(uint32_t, std::string, uint32_t) override { return 0; }
    uint32_t sound_get_generation() override { return 1; }
    void sound_stream_source_update(uint32_t, int16_t*, int) override {}
    void sound_stream_set_volumes(uint32_t, const std::vector<float>&) override {}
    void sound_begin_update() override {}
    void sound_end_update() override {}
    void sound_stream_sink_update(uint32_t, const int16_t*, int) override {}

    // --- MIDI ---
    // Two MIDI input ports mirror the MU100's physical MIDI IN A and MIDI IN B jacks.
    std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view name) override
    {
        int portIdx = (name == "VST MIDI 2") ? 1 : 0;
        return std::make_unique<VstMidiInputPort>(processor, portIdx);
    }

    // Single MIDI output mirrors the MU100's physical MIDI OUT jack (driver `mdout`).
    // Bytes are captured for the bulk-dump-to-file feature.
    std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view) override
    {
        return std::make_unique<VstMidiOutputPort>(processor);
    }

    std::vector<osd::midi_port_info> list_midi_ports() override
    {
        osd::midi_port_info p1, p2, p3;
        p1.name = "VST MIDI 1"; p1.input = true; p1.output = false;
        p1.default_input = true;  p1.default_output = false;
        p2.name = "VST MIDI 2"; p2.input = true; p2.output = false;
        p2.default_input = false; p2.default_output = false;
        p3.name = "VST MIDI OUT"; p3.input = false; p3.output = true;
        p3.default_input = false; p3.default_output = true;
        return { p1, p2, p3 };
    }

    // --- Stub overrides ---
    void input_update(bool) override {}
    void check_osd_inputs() override {}
    void set_verbose(bool) override {}
    void init_debugger() override {}
    void wait_for_debugger(device_t&, bool) override {}
    void customize_input_type_list(std::vector<input_type_entry>& list) override { list.clear(); }
    std::vector<ui::menu_item> get_slider_list() override { return {}; }
    osd_font::ptr font_alloc() override { return nullptr; }
    bool get_font_families(std::string const&, std::vector<std::pair<std::string,std::string>>&) override { return false; }
    bool execute_command(const char*) override { return false; }
    std::unique_ptr<osd::network_device> open_network_device(int, osd::network_handler&) override { return {}; }
    std::vector<osd::network_device_info> list_network_devices() override { return {}; }
};

//==============================================================================
// Builds the bus layout: the main stereo output, plus one stereo aux bus per part.
// The aux buses are declared but NOT enabled by default — VST3 fixes bus count at
// construction, so they cannot be added when the option is switched on; the host
// activates them, and swp30 only captures while the option is on. A default
// instance therefore looks and behaves exactly like the plain stereo plugin.
juce::AudioProcessor::BusesProperties YamahaMU100AudioProcessor::buildBusLayout()
{
    // The main bus is named after the plugin rather than the generic "Output" JUCE
    // suggests, so the instrument is identifiable in a host's routing lists -- where
    // it sits beside every other plugin's output and "Output" names nothing. It also
    // puts the main bus on the same footing as the 32 per-part buses, which have
    // carried real names since the dynamic-name patch.
    //
    // JucePlugin_Name rather than a literal: the .jucer is the ONE place this
    // plugin's identity is set, and a second copy here would be a second thing to
    // remember at the next rename. Bus names are cosmetic -- routing is by index --
    // so changing this breaks no saved project; a host that stamped a track name from
    // the old one keeps that name, because it was copied at track creation.
    auto layout = BusesProperties()
                      .withOutput(JucePlugin_Name, juce::AudioChannelSet::stereo(), true);
    for (int p = 0; p < YamahaMU100AudioProcessor::kNumPartBuses; ++p)
        layout = layout.withOutput("Part " + juce::String(p + 1),
                                   juce::AudioChannelSet::stereo(), false);
    return layout;
}

//==============================================================================
// Dynamic output bus names. See Source/BusNameProvider.h for why this needs a JUCE
// patch at all, and patches/juce/README.md for the three guarded sites.
//
// The names are derived here rather than from PartListPanel::voiceName() because the
// host can query getBusInfo() with the GUI closed, so the panel may not exist. Both
// read the same underlying model; the voice lookup is the same loop over kVoices.
namespace
{
    // Bank MSB/LSB + 0-based program -> display name, in whichever form Options ->
    // Voice names is showing -- so "Name All Output Tracks from Voices" populates the
    // DAW with exactly the names the Editor view has on screen. Mirrors
    // PartListPanel::lookupVoice; "--" when the combination is not a known voice.
    juce::String voiceNameFor(int msb, int lsb, int prg0)
    {
        if (auto* n = voicenames::displayName(msb, lsb, prg0 + 1))  // table is 1-based
            return juce::String(n);
        return "--";
    }
}

bool YamahaMU100AudioProcessor::refreshBusNames()
{
    using mu100::AddressMap;

    // Resolved once per call rather than cached: this runs on an explicit user action,
    // not in a loop, and a regenerated address map cannot leave a stale index behind.
    const int msbIdx = AddressMap::find("BANK SELECT MSB", "MULTI PART");
    const int lsbIdx = AddressMap::find("BANK SELECT LSB", "MULTI PART");
    const int prgIdx = AddressMap::find("PROGRAM NUMBER",  "MULTI PART");

    if (msbIdx < 0 || lsbIdx < 0 || prgIdx < 0)
    {
        jassertfalse;   // address map renamed one of these out from under us
        return false;
    }

    const auto& st = getEditorState();

    auto partByte = [&](int mapIndex, int part, uint8_t fallback) -> uint8_t
    {
        const auto& def = AddressMap::param(mapIndex);
        mu100::ModelIx model = mu100::ModelIx::XG;
        uint32_t addr21 = 0;
        AddressMap::resolve(def, part, model, addr21);

        uint8_t raw = 0;
        return st.getByte(model, addr21, raw) ? raw : fallback;
    };

    bool changed = false;

    // The lock is scoped to the compose loop and released before updateHostDisplay():
    // that call re-enters the host, which may come straight back into getBusInfo() —
    // on a thread of its choosing. Holding the lock across it would make an unrelated
    // host thread wait on us for no reason.
    {
        const juce::ScopedLock sl(busNameLock);

        for (int p = 0; p < kNumPartBuses; ++p)
        {
            const int msb  = (int) partByte(msbIdx, p, 0);
            const int lsb  = (int) partByte(lsbIdx, p, 0);
            const int prg0 = (int) partByte(prgIdx, p, 0);

            // "Pt 01 Concert Grand". Two things about this format are deliberate:
            //   - the part number LEADS, because Cubase's MixConsole strip truncates
            //     hard and the part is what must stay readable when the name is cut;
            //   - it is ZERO-PADDED, so a name sort is also a numeric sort. Unpadded,
            //     "Pt 10" sorts before "Pt 2" in every name-ordered list Cubase offers
            //     (Find Track, track visibility, export channel selection).
            // "Pt" matches the Voice module's own column header, so the mixer and the
            // editor read the same way. An unknown bank/program resolves to "--", the
            // same convention PartListPanel uses.
            const juce::String composed = "Pt " + juce::String(p + 1).paddedLeft('0', 2)
                                        + " " + voiceNameFor(msb, lsb, prg0);

            if (busNameCache[p + 1] != composed)
            {
                busNameCache[p + 1] = composed;
                changed = true;
            }
        }
    }

    if (changed)
    {
        busNamesDirty.store(true, std::memory_order_release);
        // Any updateHostDisplay() reaches the wrapper's audioProcessorChanged(), which
        // polls consumeBusNamesDirty() and ORs in Vst::kIoTitlesChanged.
        updateHostDisplay();
    }

    return changed;
}

bool YamahaMU100AudioProcessor::getDynamicOutputBusName(int busIndex, char* dest, int destBytes) const
{
    if (dest == nullptr || destBytes <= 0)
        return false;

    if (busIndex < 0 || busIndex > kNumPartBuses)
        return false;

    const juce::ScopedLock sl(busNameLock);

    // Empty = never composed (or the main bus, which keeps its static "Output").
    if (busNameCache[busIndex].isEmpty())
        return false;

    dest[0] = 0;
    busNameCache[busIndex].copyToUTF8(dest, (size_t) juce::jmin(destBytes, kMaxBusNameLen));
    return true;
}

namespace
{
    // The compiled-in ROM/asset directory. This is what the plugin should use unless
    // the user has deliberately pointed it somewhere else.
    juce::File defaultRomDir()
    {
        return juce::File::getSpecialLocation(
            juce::File::userDocumentsDirectory).getChildFile("MEGAN XG");
    }

    // Accept a persisted romPath only when it is a real override that still resolves.
    // A stored path equal to the default is redundant, and one that no longer exists
    // (moved Documents folder, a profile synced from another machine, a changed drive
    // letter) would otherwise override the correct compiled-in default forever and send
    // the plugin looking for ROMs where there are none. Both cases fall back.
    // Returns an empty string to mean "use the default".
    juce::String acceptRomPathOverride(const juce::String& stored)
    {
        if (stored.isEmpty())
            return {};

        const juce::File f(stored);
        if (f == defaultRomDir() || ! f.isDirectory())
            return {};

        return stored;
    }
}

YamahaMU100AudioProcessor::YamahaMU100AudioProcessor()
    : AudioProcessor(buildBusLayout())
{
    editorState = std::make_unique<mu100::DeviceState>();
    // The MIDI-override shadow lives beside the model and is never saved with it.
    midiOverrideState = std::make_unique<mu100::DeviceState>();
    for (int port = 0; port < 2; ++port)
        for (int ch = 0; ch < 16; ++ch)
            pendingBankMsb[port][ch] = pendingBankLsb[port][ch] = -1;

    // DAW-automatable parameters. Registered HERE, before anything else can run:
    // a VST3 host reads the parameter list from a freshly constructed instance and
    // matches saved automation by ID, so the set must be identical on every launch.
    automation = std::make_unique<AutomationParams>(*this);
    automation->createParameters();

    // DIAG START: parameter dump 2026-09-18 (PERMANENT, unlike the rest of this file's
    // diag blocks). MU100_DUMP_PARAMS=1 writes the whole parameter tree -- index, ID,
    // both names, default -- to %TEMP%\mu100-params.txt and carries on.
    //
    // It is kept rather than re-added each time because CONTRIBUTING.md asks for this dump
    // after EVERY change to the parameter set, and a check that has to be rewritten
    // first is a check that gets skipped. Costs nothing when the variable is unset,
    // and it is what the ID-uniqueness, hash-collision and index-stability checks are
    // run against.
    if (std::getenv("MU100_DUMP_PARAMS") != nullptr)
    {
        juce::String out;
        int index = 0;
        std::function<void(const juce::AudioProcessorParameterGroup&, int)> walk =
            [&] (const juce::AudioProcessorParameterGroup& g, int depth)
        {
            for (auto* node : g)
            {
                if (auto* sub = node->getGroup())
                {
                    out << "GROUP\t" << depth << "\t" << sub->getID() << "\t" << sub->getName() << "\n";
                    walk (*sub, depth + 1);
                }
                else if (auto* prm = node->getParameter())
                {
                    juce::String id;
                    if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
                        id = withId->paramID;

                    out << "PARAM\t" << index++ << "\t" << id
                        << "\t" << prm->getName (128) << "\t" << prm->getName (8) << "\t" << prm->getDefaultValue()
                        << "\t" << prm->getText (prm->getDefaultValue(), 32) << "\n";
                }
            }
        };
        walk (getParameterTree(), 0);
        juce::File (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("mu100-params.txt")).replaceWithText (out);
    }
    // DIAG END: parameter dump 2026-09-18

    initPartEqDefaults();   // seed per-part EQ raw arrays before any state load
    initDrumFxDefaults();   // seed per-drum-key EQ/HPF arrays + part->buffer map

    juce::File settingsFile = juce::File::getSpecialLocation(
        juce::File::userDocumentsDirectory).getChildFile("MEGAN XG/settings.xml");

    romPath = defaultRomDir().getFullPathName();

    // DIAG: engine-lifetime log, alongside settings.xml.
    {
        juce::File logFile = settingsFile.getSiblingFile("enginelog.txt");
        logFile.getParentDirectory().createDirectory();
        mamediag::init(logFile.getFullPathName().toRawUTF8());
        mamediag::logf("processor ctor  wrapper=%d", (int) wrapperType);
    }

    // Seeded before the file is read so a settings.xml that predates the option (or no
    // file at all) still gets the per-host default rather than a fixed one.
    useKeyboardShortcuts = defaultUseKeyboardShortcuts();

    if (settingsFile.existsAsFile())
    {
        if (auto xml = juce::XmlDocument::parse(settingsFile))
        {
            const auto rp = acceptRomPathOverride(xml->getStringAttribute("romPath", ""));
            if (rp.isNotEmpty())
                romPath = rp;

            lastSyxDir   = xml->getStringAttribute("lastSyxDir", "");
            lastDumpDir  = xml->getStringAttribute("lastDumpDir", "");
            lastSetupDir = xml->getStringAttribute("lastSetupDir", "");

            // File > Open Recent. Order is the document order, newest first; the cap is
            // re-applied on read so shrinking kMaxRecentFiles takes effect immediately.
            recentFiles.clear();
            forEachXmlChildElement(*xml, rf)
            {
                if (! rf->hasTagName("recentFile")) continue;
                const auto p = rf->getStringAttribute("path", "");
                if (p.isNotEmpty() && ! recentFiles.contains(p, /*ignoreCase*/ true))
                    recentFiles.add(p);
            }
            recentFiles.removeRange(kMaxRecentFiles, recentFiles.size());
            defaultEditorView = xml->getStringAttribute("defaultEditorView", "Hardware");
            editorTheme       = xml->getStringAttribute("editorTheme", "Dark");
            midiDrumSelectDuringPlayback =
                xml->getIntAttribute("midiDrumSelectDuringPlayback", 0) != 0;
            // Tooltip placement. Settings written before the three-way choice existed
            // carry only the boolean, so it seeds the default here (on -> the bottom
            // bar, off -> none) and `tooltipMode` overrides it wherever it is present.
            const int legacyTips = (xml->getIntAttribute("showTooltips", 1) != 0)
                                       ? TooltipBottomBar : TooltipNone;
            tooltipMode = juce::jlimit((int) TooltipPopup, (int) TooltipNone,
                                       xml->getIntAttribute("tooltipMode", legacyTips));
            focusMarkMode = juce::jlimit((int) FocusMarkAlways, (int) FocusMarkKeyboard,
                                         xml->getIntAttribute("focusMarkMode", FocusMarkAlways));
            // Masked rather than clamped: an unknown high bit from a newer build is
            // dropped, and the two we know keep their meaning.
            partTitleNames = xml->getIntAttribute("partTitleNames",
                                                 PartTitleNamePart | PartTitleNameDrum)
                               & (PartTitleNamePart | PartTitleNameDrum);
            noteLabelMode = juce::jlimit((int) NoteLabelLetters, (int) NoteLabelNumbers,
                                         xml->getIntAttribute("noteLabels",
                                                              NoteLabelLetters));
            midiOutFormat = juce::jlimit((int) MidiOutSysEx, (int) MidiOutCC,
                                         xml->getIntAttribute("midiOutFormat",
                                                              MidiOutSysEx));
            // 0 triangles / 1 underlines (retired) / 2 none: only 2 means off.
            markDawAutomatable = xml->getIntAttribute("automationMarkers", 0) != 2;
            markMidiCcParams   = xml->getIntAttribute("markMidiCcParams", 1) != 0;
            reverseDialAction = xml->getIntAttribute("reverseDialAction", 0) != 0;
            // Affirmative since 2026-09-07. A settings.xml written before the flip
            // carries only the old inverted attribute, so read that when the new one
            // is absent — otherwise everyone who had the guard ON would silently get
            // it switched off by an upgrade. Both present (impossible today, but a
            // downgrade-then-upgrade would do it) lets the new one win.
            if (xml->hasAttribute("enablePluginOnlyParams"))
                enablePluginOnlyParams =
                    xml->getIntAttribute("enablePluginOnlyParams", 1) != 0;
            else
                enablePluginOnlyParams =
                    xml->getIntAttribute("disablePluginOnlyParams", 0) == 0;
            showPartRcvSwitches = xml->getIntAttribute("showPartRcvSwitches", 1) != 0;
            showMuteSoloColumns = xml->getIntAttribute("showMuteSoloColumns", 1) != 0;
            showChangedMarks = xml->getIntAttribute("showChangedMarks", 1) != 0;
            shortVoiceNames = xml->getIntAttribute("shortVoiceNames", 0) != 0;

            // Keyboard shortcuts. The default differs by host (see the header), so it
            // is computed here rather than in the member initialiser, and a stored
            // value still overrides it.
            useKeyboardShortcuts =
                xml->getIntAttribute("useKeyboardShortcuts",
                                     defaultUseKeyboardShortcuts() ? 1 : 0) != 0;
            keyBindings.readFrom(*xml);
            // NOTE: the per-part output pair is NOT read here. It is project state
            // only, so a new instance always comes up off — the plain stereo plugin.
            // A settings.xml written before that change may still carry partOutputs /
            // partOutputsRemoveFromMain; ignoring them is the point, and saveSettings
            // rewrites the whole file, so they disappear on the next option change.
            midiInputIndicators = xml->getIntAttribute("midiInputIndicators", 1) != 0;
            keyboardPlayedNotes = xml->getIntAttribute("keyboardPlayedNotes", 1) != 0;
            // Each defaults the way an absent attribute should read: the marks are on
            // out of the box, and the editor follows the device. Global, like every
            // Option bar the per-part output pair and the automation switch.
            showMidiOverrides   = xml->getIntAttribute("showMidiOverrides",   1) != 0;
            // The mark STYLE. Clamped, not trusted, like the animation settings below:
            // a hand-edited settings.xml must not hand the widgets a style they have no
            // drawing for. An absent attribute reads as the default (bars + knob arcs),
            // which is what a settings.xml written before the styles existed carries.
            midiOverrideMarkStyle = juce::jlimit(0, midimarks::kStyleCount - 1,
                                                 xml->getIntAttribute("midiOverrideMarks",
                                                                      midimarks::BarsAndArcs));
            // Was stored inverted, as midiOverridesLocked, when the menu item was
            // phrased as what it stops. A settings.xml written before 2026-09-10 still
            // carries that attribute and nothing else, so read it as this one's default
            // -- an existing choice survives the rename, and saveSettings rewrites the
            // whole file, so the old name disappears on the next option change.
            const bool legacyLocked = xml->getIntAttribute("midiOverridesLocked", 0) != 0;
            midiFollowEnabled   = xml->getIntAttribute("midiFollowEnabled",
                                                       legacyLocked ? 0 : 1) != 0;

            // Display Animation options (see the header). Clamped, not trusted: a
            // hand-edited settings.xml must not be able to hand the analyser an FFT
            // order it has no table for.
            eqSpectrumEnabled  = xml->getIntAttribute("eqSpectrum", 1) != 0;
            eqSpectrumFftOrder = juce::jlimit(kSpectrumFftOrderMin, kSpectrumFftOrderMax,
                                              xml->getIntAttribute("eqSpectrumRes", 12));
            eqSpectrumDecayIdx = juce::jlimit(0, kSpectrumDecayCount - 1,
                                              xml->getIntAttribute("eqSpectrumDecay", 2));
            volumeMetersEnabled  = xml->getIntAttribute("volumeMeters", 1) != 0;
            volumeMeterDecayIdx  = juce::jlimit(0, kSpectrumDecayCount - 1,
                                                xml->getIntAttribute("volumeMeterDecay", 1));
            volumeMeterRampIdx   = juce::jlimit(0, edcol::kMeterRampCount - 1,
                                                xml->getIntAttribute("volumeMeterRamp", 0));
            volumeMeterSpreadIdx = juce::jlimit(0, edcol::kMeterSpreadCount - 1,
                                                xml->getIntAttribute("volumeMeterSpread", 0));
            volumeMeterFillIdx   = juce::jlimit(0, edcol::kMeterFillCount - 1,
                                                xml->getIntAttribute("volumeMeterFill", 0));

            editorViewW = xml->getIntAttribute("editorViewW", kDefaultEditorViewW);
            editorViewH = xml->getIntAttribute("editorViewH", kDefaultEditorViewH);

            // Editor text-size overrides (percent; 100 = unchanged). Pushed into
            // the edtext:: globals so every editor font is built at the chosen
            // size. Read before the editor is created, so panels pick them up.
            editorTextValuesPct     = xml->getIntAttribute("editorTextValuesPct", 100);
            editorTextLabelsPct     = xml->getIntAttribute("editorTextLabelsPct", 100);
            editorTextTitlesPct     = xml->getIntAttribute("editorTextTitlesPct", 100);
            editorTextVoiceListPct  = xml->getIntAttribute("editorTextVoiceListPct", 100);
            editorTextGrDisValuePct = xml->getIntAttribute("editorTextGrDisValuePct", 100);
            // The option windows' labels. Same scale, its own category: those
            // windows are plain dialogs, not part of the scaled editor canvas.
            editorTextOptionsPct    = xml->getIntAttribute("editorTextOptionsPct", 100);

            pdcTrimIndex.store(juce::jlimit(0, 8, xml->getIntAttribute("pdcTrimIndex", 0)),
                               std::memory_order_relaxed);
            // Clamped: the ring buffers are sized against kMaxBufferThreshold, and a
            // value past it makes the writer lap its own reader. The floor keeps a 0 or
            // a negative from disabling the throttle entirely.
            mameBufferThreshold.store(
                juce::jlimit(128, kMaxBufferThreshold,
                             xml->getIntAttribute("bufferThreshold", 2048)),
                std::memory_order_relaxed);

            // Load per-device MIDI routing
            forEachXmlChildElement(*xml, child)
            {
                if (child->hasTagName("midiDevice"))
                {
                    juce::String id = child->getStringAttribute("id");
                    if (id.isNotEmpty())
                    {
                        midiDeviceSettings[id].enabled   = child->getIntAttribute("enabled", 0) != 0;
                        midiDeviceSettings[id].routeMask = static_cast<uint8_t>(
                            child->getIntAttribute("routeMask", MIDI_ROUTE_PORT1));
                        // Absent in files written before the column existed — default 1,
                        // which is what those installs were already doing.
                        midiDeviceSettings[id].drumSelect =
                            child->getIntAttribute("drumSelect", 1) != 0;
                    }
                }
                else if (child->hasTagName("midiOutputDevice"))
                {
                    juce::String id = child->getStringAttribute("id");
                    if (id.isNotEmpty())
                        midiOutputEnabled[id] = child->getIntAttribute("enabled", 0) != 0;
                }
            }
        }
    }

    // WHAT THIS INSTANCE BELIEVES IS ON DISK, taken now that the file (if any) has
    // been read into the members. Every later save diffs against this to decide which
    // keys it may write -- see buildSettingsXml and the header. Taken from OUR OWN
    // builder rather than from the parsed file on purpose: for a key the file did not
    // carry, the honest baseline is the default we just used, so an instance that
    // writes that key later is correctly read as having changed it.
    settingsSnapshot = buildSettingsXml();

    // Apply the editor text-size overrides whether or not a settings file existed
    // (a fresh install with no settings.xml still gets the built-in defaults).
    edtext::setPercents(editorTextValuesPct, editorTextLabelsPct,
                        editorTextTitlesPct, editorTextVoiceListPct,
                        editorTextGrDisValuePct, editorTextOptionsPct);

    // Push the "Invert knob/dial direction" flag into the live global read by the editor's
    // knobs + value boxes (whether or not a settings.xml existed).
    dialaction::setReverse(reverseDialAction);

    // ...and the MIDI-override mark STYLE into the global the value boxes and knobs
    // read at paint AND at layout time (the horizontal bar lives in a strip a control
    // has to be built taller for), so a view constructed later opens in the right one.
    midimarks::setStyle(midiOverrideMarkStyle);

    // ...and "Show changed markers" into the global every changed-from-default mark is
    // drawn through, so a view constructed later opens in the state the user left.
    changedmarks::setShow(showChangedMarks);

    // ...and "Label notes as" into the global every note-as-text surface reads. Pushed
    // in the PROCESSOR for the same reason the voice-name choice is: one of those
    // surfaces (the MIDI-override badge's tooltip) is composed here, with no editor
    // necessarily open.
    notelabel::setMode(noteLabelMode);

    // ...and the "Voice names" choice into the global every voice-name lookup reads,
    // the output-bus names included (which is why it is pushed here in the PROCESSOR
    // ctor rather than when an editor opens -- a host can ask for bus names with no
    // editor in existence).
    voicenames::setShort(shortVoiceNames);

    // ...and the chosen VOLUME-meter ramp into the live palette. Safe this early
    // because it only writes four edcol tokens, which are read at paint time; the
    // theme is installed later (applyEditorTheme) and re-applies the ramp itself.
    edcol::setMeterRamp(volumeMeterRampIdx);

    juce::File romZip = juce::File(romPath).getChildFile("mu100.zip");
    isRomMissing.store(!romZip.existsAsFile());

    // Fold received SysEx into the editor model at a steady 20 Hz, GUI open or not (see
    // drainIncomingSysEx). A plugin instance is constructed on the message thread in
    // every host we target, which is what juce::Timer requires.
    sysexInDrain.startTimerHz(20);

    // Reconcile the DAW parameters with the editor model at 30 Hz, GUI open or not
    // — same reasoning as the two timers above: automation must reach the device
    // and an editor edit must reach the host whether or not anyone is looking. It
    // sends nothing until the engine is up (see AutomationParams::timerCallback).
    automation->start();


    // NOTE: MAME is NOT launched here. See bootMameIfNeeded() called from prepareToPlay().
}

YamahaMU100AudioProcessor::~YamahaMU100AudioProcessor()
{
    sysexInDrain.stopTimer();
    automation->stop();   // also closes any open host change-gesture

    // Close all open MIDI devices before shutting down MAME so any in-flight
    // callbacks can still safely push to the ring buffers.
    {
        std::map<juce::String, OpenDevice> devicesToClose;
        {
            juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
            devicesToClose = std::move(openMidiDevices);
        }
        // devicesToClose destroyed here (outside lock): MidiInput::stop() waits for callbacks
    }

    shutdownMame();
    if (mameThread.joinable())
        mameThread.join();

    // Remove the per-instance MAME workspace now that the engine has stopped
    // writing to it. Safe even if creation never ran (e.g. ROM missing path).
    if (mameWorkDir != juce::File{} && mameWorkDir.isDirectory())
        mameWorkDir.deleteRecursively();
}

//==============================================================================
void YamahaMU100AudioProcessor::shutdownMame()
{
    isMameRunning.store(false);
    mameThrottleEvent.signal();
}

// Publish/retract the live running_machine. Taking the lock EXCLUSIVELY is the
// whole point: it waits for every outstanding MameLease to be released, so once
// this returns with nullptr no other thread can still be inside a dereference of
// the machine that is about to be destroyed.
//
// Retracting also clears mameIsFullyBooted. That flag used to latch true for the
// life of the plugin, which meant every "is MAME up?" guard silently degraded
// into a bare pointer test once the first boot had happened.
void YamahaMU100AudioProcessor::publishMachine(running_machine* machine)
{
    std::unique_lock<std::shared_mutex> w(mameLifetimeLock);
    mameMachine = machine;
    if (machine == nullptr)
    {
        mameIsFullyBooted.store(false, std::memory_order_release);
        lastSpectrumTarget = -1;   // a fresh swp30 starts with the probe off
        // Likewise for per-part capture: force a re-push on the next boot, or the
        // aux buses come back silent after a reboot until the option is toggled.
        partOutputsDirty.store(true, std::memory_order_release);
        // ...and for mute/solo. A fresh swp30 comes up with every part audible, so
        // without this a reboot would silently un-mute while the M/S cells still
        // showed lit — the state visible in the UI and the state in the chip would
        // disagree, which is the one thing a monitoring aid must never do.
        partAudibleDirty.store(true, std::memory_order_release);
    }
}

//==============================================================================
// Written PER ENGINE RUN, not to a global: a VST3 host can hold several plugin
// instances in one process, and a global would report one instance's dead engine
// on all of them.
struct SehCrashInfo { DWORD code; void* fault_addr; void* access_addr; };
static int sehFilter(EXCEPTION_POINTERS* ep, SehCrashInfo* out) {
    out->code        = ep->ExceptionRecord->ExceptionCode;
    out->fault_addr  = ep->ExceptionRecord->ExceptionAddress;
    out->access_addr = ep->ExceptionRecord->NumberParameters >= 2
        ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr;
    // DIAG: the filter runs on the FAULTING thread with its frames intact, so
    // this is the only place a real stack for the dying engine can be taken.
    mamediag::logException(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}
static int executeMameSeh(cli_frontend* fe, std::vector<std::string>& args, SehCrashInfo* out)
{
    int exitCode = -1;
    __try {
        exitCode = fe->execute(args);
    } __except (sehFilter(GetExceptionInformation(), out)) {}
    return exitCode;
}

void YamahaMU100AudioProcessor::runMameEngine()
{
    isMameRunning.store(true);
    mamediag::logf("engine thread start (this=%p)", (void*) this);

#ifdef _WIN32
    // Standalone: Pro Audio HIGH. Single-instance MAME has ample headroom
    // at HIGH, and using CRITICAL caused cross-process scheduling-budget
    // contention with a Cubase session running alongside -- visible as
    // periodic peak-meter spikes on the Cubase side during mixdown work.
    // HIGH stays in the Pro Audio class so the OS still treats it
    // favorably; it just yields to a host DAW's CRITICAL audio thread.
    //
    // Hosted instance: Pro Audio NORMAL. The host's own audio thread is
    // already Critical, and putting two Critical threads in the same
    // process makes them contend for the OS's Pro Audio Critical
    // scheduling budget -- visible in Cubase as peak-meter spikes even
    // when the plugin is idle.
    const bool isStandalone = (wrapperType == wrapperType_Standalone);
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (hTask)
        AvSetMmThreadPriority(hTask, isStandalone ? AVRT_PRIORITY_HIGH : AVRT_PRIORITY_NORMAL);
    else if (isStandalone)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif

    // The per-instance MAME workspace was created in bootMameIfNeeded() so any
    // staged save-state blob from setStateInformation could be dropped in first.
    // Just resolve the path; createDirectory is a no-op when it already exists.
    mameWorkDir.createDirectory();
    const std::string workDirStr = mameWorkDir.getFullPathName().toUTF8().getAddress();

    std::vector<std::string> args;
    args.push_back("mame");
    args.push_back("mu100");
    args.push_back("-rompath");
    args.push_back(romPath.toUTF8().getAddress());
    args.push_back("-homepath");
    args.push_back(workDirStr);
    args.push_back("-cfg_directory");
    args.push_back(workDirStr);
    args.push_back("-nvram_directory");
    args.push_back(workDirStr);
    args.push_back("-state_directory");
    args.push_back(workDirStr);
    args.push_back("-noreadconfig");
    args.push_back("-skip_gameinfo");
    args.push_back("-nowindow");
    args.push_back("-video");
    args.push_back("none");
    args.push_back("-sound");
    args.push_back("osd");
    args.push_back("-nothrottle");
    args.push_back("-nosleep");
    args.push_back("-nowaitvsync");
    args.push_back("-nobackground_input");
    // mameSampleRate, not hostSampleRate: the host's rate can change later, MAME's
    // cannot, and the drain's resample ratio is only right if this is the one number
    // both the command line and the OSD node report.
    args.push_back("-samplerate");
    args.push_back(std::to_string(static_cast<int>(effectiveMameRate())));
    mamediag::logf("MAME -samplerate %d", static_cast<int>(effectiveMameRate()));
    args.push_back("-midiin1");
    args.push_back("VST MIDI 1");   // MIDI IN A → parts 1–16
    args.push_back("-midiin2");
    args.push_back("VST MIDI 2");   // MIDI IN B → parts 17–32
    args.push_back("-midiout");
    args.push_back("VST MIDI OUT"); // MIDI OUT (mdout) → bulk-dump capture
    args.push_back("-keyboardprovider");
    args.push_back("none");
    args.push_back("-mouseprovider");
    args.push_back("none");
    args.push_back("-joystickprovider");
    args.push_back("none");

    auto* mameOpts = new osd_options();
    // osd_common_t's constructor PUSHES ITSELF onto the process-global osd_output
    // stack, so this allocation is part of that stack's traffic and takes its lock
    // -- see osdOutputStackLock(). The matching delete below does the same.
    VstOsdInterface* headlessOsd = nullptr;
    {
        const std::lock_guard<std::mutex> lock(osdOutputStackLock());
        headlessOsd = new VstOsdInterface(this, *mameOpts);
    }
    auto* frontend = new cli_frontend(*mameOpts, *headlessOsd);

    // DIAG: capture MAME's own message stream for the duration of the run.
    // The budget is per run, so an engine that reboots gets a fresh allowance
    // rather than staying silent forever after one noisy run.
    mamediag::resetMameMessageBudget();
    retainMameOutputCapture();
    mamediag::logf("engine thread: entering cli_frontend::execute()  workdir=%s", workDirStr.c_str());

    SehCrashInfo sehInfo = {};
    int exitCode = -1;
    try
    {
        exitCode = executeMameSeh(frontend, args, &sehInfo);
    }
    catch (const std::exception& e)
    {
        mamediag::logf("engine thread: C++ exception escaped execute(): %s", e.what());
    }
    catch (...)
    {
        mamediag::logf("engine thread: unknown C++ exception escaped execute()");
    }

    mamediag::logf("engine thread: execute() returned %d  (fault=0x%08lx)  booted=%d",
                   exitCode, (unsigned long) sehInfo.code,
                   (int) mameIsFullyBooted.load(std::memory_order_relaxed));
    // One line per run, always on: it is the difference between "the app never
    // received any MIDI" and "it did, so a silence is downstream of the input".
    // Answering that from a user's log used to need MU100_MIDIDIAG set in
    // advance, i.e. it could only be answered on the NEXT occurrence.
    mamediag::logf("midi totals: callbacks=%u bytes=%u toMame=%u",
                   midiDiagRawIn.load(), midiDiagInBytes.load(), midiDiagToMame.load());
    // The other half of the same question: `written`/`read` prove the audio path ran
    // at all. Both lines are documented for users in the manual's engine-log appendix.
    mamediag::logf("audio totals: written=%llu read=%llu",
                   (unsigned long long) totalWritten.load(),
                   (unsigned long long) totalRead.load());
    releaseMameOutputCapture();

    // MAME has left execute(), so the running_machine — a STACK object in
    // mame_machine_manager::execute()'s frame — no longer exists, and this
    // thread is about to unwind and unmap the memory it lived in. Retract the
    // pointer unconditionally.
    //
    // osd_exit() normally does this, but it is a MACHINE_NOTIFY_EXIT notifier
    // and there are exits that never reach it: the SEH filter above unwinds MAME
    // straight out of execute() (skipping running_machine::run()'s notifier
    // call), as does a C++ exception escaping past MAME's own handlers. Those
    // paths used to leave mameMachine dangling while mameIsFullyBooted stayed
    // true, so the next editor or processBlock() touch dereferenced freed stack
    // memory. Retracting here is unconditional and idempotent.
    publishMachine(nullptr);

    // A swallowed fault must not be silent: without this, MAME dying takes the
    // audio with it and the only visible symptom is that the plugin goes quiet.
    if (sehInfo.code != 0)
    {
        mameFaultCode.store(sehInfo.code, std::memory_order_release);
        DBG("MAME engine terminated by exception 0x" << juce::String::toHexString((int) sehInfo.code)
            << " at " << juce::String::toHexString((juce::pointer_sized_int) sehInfo.fault_addr)
            << " accessing " << juce::String::toHexString((juce::pointer_sized_int) sehInfo.access_addr));
    }

    delete frontend;
    {
        // ...and its destructor pops. Same lock as the push above.
        const std::lock_guard<std::mutex> lock(osdOutputStackLock());
        delete headlessOsd;
    }
    delete mameOpts;
    mamediag::logf("engine thread exit");

#ifdef _WIN32
    if (hTask) AvRevertMmThreadCharacteristics(hTask);
#endif

    isMameRunning.store(false);
}

//==============================================================================
void YamahaMU100AudioProcessor::pushAudioFromMame(const int16_t* pcmBuffer, int numSamples)
{
    // Per-part capture. Drained HERE, on the MAME thread, and written at the same
    // writePos as the main ring below — that shared index is the whole alignment
    // guarantee for the aux buses. Both ends of the swp30 FIFO are this thread, so
    // frame n corresponds to exactly the stereo sample MAME produced for instant n.
    int auxFrames = 0;
    const bool aux = auxRingLive.load(std::memory_order_acquire) && mameMachine != nullptr;
    if (aux)
    {
        if (static_cast<int>(partFrameScratch.size()) < numSamples * kNumAuxChannels)
            partFrameScratch.assign(size_t(numSamples) * kNumAuxChannels, 0);
        auxFrames = swp30_ext::read_part_frames(*mameMachine, partFrameScratch.data(),
                                                numSamples);
        // The canary. Short reads mean the swp30 FIFO and this callback have drifted
        // apart, which is the assumption the aux buses rest on. Silent in the audio
        // (the shortfall is zero-filled below), visible in the log.
        //
        // The FIRST block after capture is enabled is short by construction — the FIFO
        // only began filling when capture came on, while this callback carries samples
        // MAME produced before that. So the count matters, not the fact: one at startup
        // is expected, a steady stream means the two are genuinely out of step.
        if (auxFrames < numSamples)
        {
            auxShortReadCount ++;
            auxShortReadFrames += uint64_t(numSamples - auxFrames);
            if (! auxShortReadLogged)
            {
                auxShortReadLogged = true;
                mamediag::logf("part-outs: first short read %d of %d frames "
                               "(expected once, when capture starts)",
                               auxFrames, numSamples);
            }
        }

        // MU100_PARTOUT_TRACE=1 logs a once-a-second per-part peak of what was actually
        // captured. This is how you confirm a part landed on the RIGHT bus and the
        // right channel (hard-pan a part and watch one side go to zero) without a DAW
        // — the aux buses are not audible in the Standalone build.
        static const bool partOutTrace = std::getenv("MU100_PARTOUT_TRACE") != nullptr;
        if (partOutTrace && auxFrames > 0)
        {
            for (int i = 0; i < auxFrames; ++i)
            {
                const int32_t* f = partFrameScratch.data() + size_t(i) * kNumAuxChannels;
                for (int p = 0; p < kNumPartBuses; ++p)
                {
                    auxTracePeak[p * 2    ] = std::max(auxTracePeak[p * 2    ], std::abs(f[p]));
                    auxTracePeak[p * 2 + 1] = std::max(auxTracePeak[p * 2 + 1],
                                                       std::abs(f[kNumPartBuses + p]));
                }
            }
        }

        // Roughly once a second, and only while it is still happening.
        const uint64_t now = totalWritten.load(std::memory_order_relaxed);
        if (partOutTrace && now >= auxShortReadNextLog)
        {
            juce::String line;
            for (int p = 0; p < kNumPartBuses; ++p)
                if (auxTracePeak[p * 2] || auxTracePeak[p * 2 + 1])
                    line << " p" << (p + 1) << "=" << auxTracePeak[p * 2]
                         << "/"  << auxTracePeak[p * 2 + 1];
            mamediag::logf("part-outs peaks (L/R):%s",
                           line.isEmpty() ? " (all silent)" : line.toRawUTF8());
            std::fill(std::begin(auxTracePeak), std::end(auxTracePeak), 0);
        }
        if (now >= auxShortReadNextLog)
        {
            if (auxShortReadNextLog != 0 && auxShortReadCount > 0)
                mamediag::logf("part-outs: %u short reads in the last second, %llu frames "
                               "missing — aux buses ARE misaligned with the main output",
                               auxShortReadCount,
                               (unsigned long long) auxShortReadFrames);
            auxShortReadCount  = 0;
            auxShortReadFrames = 0;
            auxShortReadNextLog = now + 44100;
        }
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const uint64_t writePos = totalWritten.load(std::memory_order_relaxed);
        const int16_t l = pcmBuffer[i * 2    ];
        const int16_t r = pcmBuffer[i * 2 + 1];
        ringBufferL[writePos & (RING_BUFFER_SIZE - 1)] = l / 32768.0f;
        ringBufferR[writePos & (RING_BUFFER_SIZE - 1)] = r / 32768.0f;

        if (aux)
        {
            const size_t idx = size_t(writePos & (AUX_RING_SIZE - 1));
            if (i < auxFrames)
            {
                // swp30 hands us 32 left values then 32 right, per frame, in the
                // MIXER-BUS domain — see kPartBusScale for why that is not the
                // spectrum probe's reference.
                const int32_t* f = partFrameScratch.data() + size_t(i) * kNumAuxChannels;
                for (int p = 0; p < kNumPartBuses; ++p)
                {
                    auxRing[size_t(p * 2    ) * AUX_RING_SIZE + idx] = f[p] * kPartBusScale;
                    auxRing[size_t(p * 2 + 1) * AUX_RING_SIZE + idx] = f[kNumPartBuses + p] * kPartBusScale;
                }
            }
            else
            {
                for (int c = 0; c < kNumAuxChannels; ++c)
                    auxRing[size_t(c) * AUX_RING_SIZE + idx] = 0.0f;
            }
        }

        totalWritten.fetch_add(1, std::memory_order_release);
    }

    while (isMameRunning.load())
    {
        if (!mameIsFullyBooted.load(std::memory_order_relaxed))
            break;
        const int64_t ahead = static_cast<int64_t>(totalWritten.load(std::memory_order_acquire))
                            - static_cast<int64_t>(totalRead.load(std::memory_order_acquire));
        if (ahead <= mameBufferThreshold.load())
            break;
        mameThrottleEvent.wait(5);
        mameThrottleEvent.reset();
    }
}

//==============================================================================
void YamahaMU100AudioProcessor::pushMidiByte(uint8_t data, double targetMameTime)
{
    pushMidiByteToPort(0, data, targetMameTime);
}

void YamahaMU100AudioProcessor::observeNoteOn(int note)
{
    // Writer side of the editor's note-on slot. Store the note, then bump the
    // sequence with release so a reader that sees the new seq also sees the note.
    lastNoteOnNote.store(note, std::memory_order_relaxed);
    lastNoteOnSeq.fetch_add(1, std::memory_order_release);
}

int YamahaMU100AudioProcessor::takeLatestNoteOn(uint32_t& seq) const
{
    const uint32_t s = lastNoteOnSeq.load(std::memory_order_acquire);
    if (s == seq) return -1;
    seq = s;
    return lastNoteOnNote.load(std::memory_order_relaxed);
}

void YamahaMU100AudioProcessor::heldNotesFor(int rawChannel, uint32_t out[4]) const
{
    if (rawChannel < 0 || rawChannel > 31)
    {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    for (int w = 0; w < 4; ++w)
        out[w] = heldNotes[rawChannel][w].load(std::memory_order_acquire);
}

void YamahaMU100AudioProcessor::clearHeldNotes()
{
    for (int c = 0; c < 32; ++c)
        for (int w = 0; w < 4; ++w)
            heldNotes[c][w].store(0, std::memory_order_release);
}

// Classify one SysEx message into the parts it addresses (`parts`, a 32-bit mask)
// and/or the no-part lamps it lights (`sys`, SYSACT_* bits). Both may come back
// empty: a message the device ignores must light nothing at all.
//
// getSysExData() points past the F0 and excludes the trailing F7, so d[0] is the
// manufacturer ID. Yamaha messages are
//   d[0]=43  d[1]=1n (parameter change) | 0n (bulk dump) | 2n (dump request)
//   d[2]=model: 4C = XG, 59 = MU100 native Performance, 49 = MU100 native system
// and for a parameter change d[3..5] are the address hi/mid/lo.
void YamahaMU100AudioProcessor::classifySysEx(const juce::MidiMessage& msg,
                                              uint32_t& parts, uint32_t& sys) const
{
    parts = 0;
    sys   = 0;

    const uint8_t* d = msg.getSysExData();
    const int      n = msg.getSysExDataSize();
    if (n < 1) return;

    // Universal SysEx: 7E non-realtime (GM System On, Identity Request/Reply),
    // 7F realtime (Master Volume/Balance/Tune). Global -> SYS.
    //
    // EXCEPT the transport/timecode family, which rides the same 7F prefix but is
    // addressed at the sequencer, not at this device: the MU100 ignores all of it,
    // and a DAW emits MTC continuously while the transport rolls, which would hold
    // the SYS lamp lit for the whole session. d[1] is the device id, d[2] sub-ID#1.
    if (d[0] == 0x7F && n >= 3)
    {
        switch (d[2])
        {
            case 0x01:      // MIDI Time Code (full frame / user bits)
            case 0x05:      // Real Time MTC Cueing
            case 0x06:      // MIDI Machine Control Command
            case 0x07:      // MIDI Machine Control Response
                return;     // transport -> lights nothing
            default:
                break;
        }
    }
    if (d[0] == 0x7E || d[0] == 0x7F) { sys = SYSACT_SYS; return; }

    if (d[0] != 0x43 || n < 3) return;          // not Yamaha -> the device ignores it

    const uint8_t sub   = d[1] & 0xF0;
    const uint8_t model = d[2];

    // Bulk dump (43 0n) and dump request (43 2n), any model: a whole-instrument
    // load. This is the ONE case that still flashes every part, because it really
    // does rewrite all 32. (A single-part bulk dump would be resolvable from the
    // address that follows the byte count; not worth the parse for a rare case.)
    if (sub == 0x00 || sub == 0x20)
    {
        parts = 0xFFFFFFFFu;
        sys   = SYSACT_BULK;
        return;
    }
    if (sub != 0x10) return;                    // not a parameter change

    if (model == 0x59) { sys = SYSACT_BULK; return; }   // native Performance data
    if (model == 0x49) { sys = SYSACT_SYS;  return; }   // MU100 native system
    if (model != 0x4C || n < 6) return;                 // unknown model -> nothing

    const uint8_t hi = d[3], mid = d[4], lo = d[5];

    // --- Part-addressed: the two MULTI PART address blocks ---------------------
    // 08 nn xx  = the main MULTI PART block.
    // 0A nn 20  = the per-part HPF cutoff, a SEPARATE block. The low byte matters:
    //             0A 00 00..0E is REMOTE SWITCH (front-panel button emulation),
    //             which would otherwise read as "part 0 received data".
    if (hi == 0x08 && mid < 32)                  { parts = 1u << mid; return; }
    if (hi == 0x0A && lo == 0x20 && mid < 32)    { parts = 1u << mid; return; }
    if (hi == 0x0A)                              { return; }   // REMOTE SWITCH -> nothing

    // --- DRUM SETUP (3n rr xx, n = DRUMS buffer 0..3) -------------------------
    // Resolves to whichever parts currently use that buffer (none = light nothing).
    // Under model 4C, 30..33 is unambiguously DRUM SETUP; the performance tables
    // that share the 3n prefix live under model 59, handled above.
    if (hi >= 0x30 && hi <= 0x33)
    {
        parts = drumBufferParts[hi - 0x30].load(std::memory_order_acquire);
        return;
    }

    // --- No-part messages -----------------------------------------------------
    if (hi == 0x00 && mid == 0x00)               { sys = SYSACT_SYS; return; }  // XG SYSTEM
    if (hi == 0x06 || hi == 0x07)                { sys = SYSACT_SYS; return; }  // DISPLAY DATA
    if (hi == 0x02 && (mid == 0x01 || mid == 0x40)) { sys = SYSACT_FX; return; }// reverb/chorus/variation, MULTI EQ
    if (hi == 0x03 && (mid == 0x00 || mid == 0x01)) { sys = SYSACT_FX; return; }// insertion 1/2

    // A/D PART (10 0n) / A/D system (11 00) and PLUGIN BOARD (70/71 tt) have a real
    // target that has no row in the Voice module, so they light nothing for now.
    // Anything else unrecognised likewise stays dark.
}

void YamahaMU100AudioProcessor::observeMidiActivity(int port, const juce::MidiMessage& msg)
{
    if (msg.isSysEx())
    {
        uint32_t parts = 0, sys = 0;
        classifySysEx(msg, parts, sys);
        if (parts != 0) midiPartActivity.fetch_or(parts, std::memory_order_release);
        if (sys   != 0) midiSysActivity .fetch_or(sys,   std::memory_order_release);
        // Fold it into the editor's model too, so the panels track a parameter change
        // that arrived from outside instead of showing the editor's own last selection.
        // Queued here (this is the audio or a MIDI callback thread) and applied by
        // drainIncomingSysEx() on the message thread.
        queueIncomingSysEx(msg.getRawData(), static_cast<size_t>(msg.getRawDataSize()));
        return;
    }

    // Any channel-voice message (note on/off, poly/channel aftertouch, CC, program
    // change, pitch bend) counts as data for the channel's part(s). getChannel()
    // returns 1..16 for those and 0 for system messages, which we ignore.
    const int ch = msg.getChannel();
    if (ch < 1 || ch > 16) return;
    const int bit = (port == 0 ? 0 : 16) + (ch - 1);
    midiChanActivity.fetch_or(1u << bit, std::memory_order_release);

    // Held-note set for the Keyboard module's played-note display, keyed by the same
    // channel bit. isNoteOn() is false for a velocity-0 note-on and isNoteOff() true
    // for it, so the running-status note-off idiom is handled by the pair as written.
    // CC120/123 clear the channel because the device silences on them and nothing
    // else would ever release those keys.
    if      (msg.isNoteOn())  setHeldNote(bit, msg.getNoteNumber(), true);
    else if (msg.isNoteOff()) setHeldNote(bit, msg.getNoteNumber(), false);
    else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        for (int w = 0; w < 4; ++w)
            heldNotes[bit][w].store(0, std::memory_order_release);

    // Program change and the controllers that move a STORED parameter are queued for
    // the MIDI-override shadow. Queued, not applied: this is the audio or a MIDI
    // callback thread, and working out which PARTS receive on this channel means
    // reading Rcv CHANNEL out of the model, which belongs to the message thread.
    // Note data is not queued -- notes change no setting, and at playing rates they
    // would be the whole traffic.
    {
        const uint8_t status = (uint8_t) msg.getRawData()[0];
        const uint8_t type   = status & 0xF0u;
        if (type == 0xB0 || type == 0xC0)
        {
            const int n = msg.getRawDataSize();
            queueIncomingChannelMsg(port, status,
                                    n > 1 ? (uint8_t) msg.getRawData()[1] : 0,
                                    n > 2 ? (uint8_t) msg.getRawData()[2] : 0);
        }
    }
}

void YamahaMU100AudioProcessor::queueIncomingSysEx(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0 || len > SYSEX_IN_SIZE - 4u)
        return;

    const uint32_t n    = static_cast<uint32_t>(len);
    const uint32_t need = 4u + n;

    uint32_t base = sysexIn.reserved.load(std::memory_order_relaxed);
    for (;;)
    {
        const uint32_t used = base - sysexIn.readPos.load(std::memory_order_acquire);
        if (used + need > SYSEX_IN_SIZE)
            return;                     // full — drop this message whole, never truncate
        if (sysexIn.reserved.compare_exchange_weak(base, base + need,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed))
            break;
    }

    for (int b = 0; b < 4; ++b)
        sysexIn.raw[(base + static_cast<uint32_t>(b)) & SYSEX_IN_MASK]
            = static_cast<uint8_t>((n >> (8 * b)) & 0xFF);
    for (uint32_t i = 0; i < n; ++i)
        sysexIn.raw[(base + 4u + i) & SYSEX_IN_MASK] = data[i];

    // Publish everything EXCEPT the first header byte, then that one with a release.
    // The consumer tests only ready[base], so seeing it set means the rest is there.
    for (uint32_t i = 0; i < n; ++i)
        sysexIn.ready[(base + 4u + i) & SYSEX_IN_MASK].store(1, std::memory_order_relaxed);
    for (uint32_t b = 3; b >= 1; --b)
        sysexIn.ready[(base + b) & SYSEX_IN_MASK].store(1, std::memory_order_relaxed);
    sysexIn.ready[base & SYSEX_IN_MASK].store(1, std::memory_order_release);
}

bool YamahaMU100AudioProcessor::drainIncomingSysEx()
{
    bool changed = false;
    std::vector<uint8_t> msg;

    for (;;)
    {
        const uint32_t base = sysexIn.readPos.load(std::memory_order_relaxed);
        if (sysexIn.ready[base & SYSEX_IN_MASK].load(std::memory_order_acquire) == 0)
            break;                      // empty, or a producer is still filling

        uint32_t n = 0;
        for (int b = 0; b < 4; ++b)
            n |= static_cast<uint32_t>(sysexIn.raw[(base + static_cast<uint32_t>(b)) & SYSEX_IN_MASK])
                 << (8 * b);

        msg.resize(n);
        for (uint32_t i = 0; i < n; ++i)
            msg[i] = sysexIn.raw[(base + 4u + i) & SYSEX_IN_MASK];

        const uint32_t total = 4u + n;
        for (uint32_t i = 0; i < total; ++i)
            sysexIn.ready[(base + i) & SYSEX_IN_MASK].store(0, std::memory_order_relaxed);
        sysexIn.readPos.store(base + total, std::memory_order_release);

        // The SysEx door forks HERE, and this is the one change the override feature
        // makes at the model end.
        //
        //   BULK DUMP    -> the MODEL, exactly as before. A dump is a whole block or a
        //                   whole setup arriving at once: whoever sent it meant "this
        //                   is your setup now", the same as opening a file.
        //   PARAM CHANGE -> the OVERRIDE SHADOW. One setting arriving mid-song is the
        //                   song playing your setup, not replacing it, so it must not
        //                   reach anything that gets saved.
        //
        // Splitting on Kind is exact rather than heuristic: the parser already tells
        // the two apart by the third status byte (0x0n = dump, 0x1n = parameter
        // change). XG System On is neither, and stays the no-op marker it always was.
        bool wasDump = false;
        for (const auto& m : mu100::parseStream(msg.data(), msg.size()))
            if (m.kind == mu100::Kind::BulkDump) { wasDump = true; break; }

        if (wasDump)
        {
            loadModelLikeDevice(msg.data(), msg.size());
            changed = true;
            // The dump has just rewritten this ground, so any shadow over it is stale.
            clearMidiOverridesForMessage(msg);
        }
        else
        {
            for (const auto& m : mu100::parseStream(msg.data(), msg.size()))
            {
                if (m.kind != mu100::Kind::ParamChange) continue;
                int inst = 0, inst2 = 0;
                const int paramIndex = mu100::AddressMap::identify(m, inst, inst2);
                if (paramIndex < 0) continue;   // XG System On, a request, or foreign
                noteMidiOverride(paramIndex, inst, inst2,
                                 m.data.data(), (int) m.data.size());
            }
        }
    }

    if (changed)
        editorStateExternalSeq.fetch_add(1, std::memory_order_release);
    return changed;
}

void YamahaMU100AudioProcessor::setCleanHpfHz(int part, float hz)
{
    if (part < 0 || part >= 32) return;
    // The plugin-only parameters have no XG address, so they cannot ride the SysEx
    // funnel's capture — they are recorded here, at their own single funnel. Including
    // them is what keeps the EQ grid coherent: bands 2-4 sit in the same row as the
    // hardware shelves in 1 and 5, and a grid where half the columns undo would read
    // as broken rather than as scoped.
    if (undoSuppressDepth == 0)
        undoPushPlugin({ mu100undo::PluginField::CleanHpf, part, 0, 0, cleanHpfHz[part], hz });
    cleanHpfHz[part] = hz;
    if (MameLease mame { *this })
        swp30_ext::set_part_hpf(*mame, part, hz);
    cleanHpfDirty.store(true, std::memory_order_release); // re-assert after any (re)boot
}

// The system-wide HPF floor. One value, one push -- swp30 folds it into each part's
// effective cutoff (max of the two) on the next sample, so nothing per-part is written
// here and a part whose own HPF2 is already higher is not touched at all.
void YamahaMU100AudioProcessor::setSystemHpfHz(float hz)
{
    if (undoSuppressDepth == 0)
        undoPushPlugin({ mu100undo::PluginField::SystemHpf, 0, 0, 0, systemHpfHz, hz });
    systemHpfHz = hz;
    if (MameLease mame { *this })
        swp30_ext::set_system_hpf(*mame, hz);
    // Rides the clean-HPF dirty flag: the same reassert re-pushes both after a (re)boot,
    // and they can never be re-pushed out of step with each other.
    cleanHpfDirty.store(true, std::memory_order_release);
}

// The three reassert*IfDirty() below and the pollMidiData/readMidiByte pair are
// the only places that may touch mameMachine WITHOUT a MameLease: every one of
// them is called from the MAME thread itself (the OSD update() callback and
// MAME's MIDI port polling), and the machine cannot be destroyed underneath its
// own thread. Anything reached from the message or audio thread must take a
// lease — see the MameLease note in the header.
void YamahaMU100AudioProcessor::reassertCleanHpfIfDirty()
{
    if (mameMachine == nullptr) return;
    if (cleanHpfDirty.exchange(false, std::memory_order_acq_rel))
    {
        // The floor FIRST: a part's coefficients are recomputed from max(part, system)
        // on its next sample, so pushing the parts first would recalc them all twice.
        swp30_ext::set_system_hpf(*mameMachine, systemHpfHz);
        for (int part = 0; part < 32; ++part)
            swp30_ext::set_part_hpf(*mameMachine, part, cleanHpfHz[part]);
    }
}

namespace
{
    // XG "EQ Frequency" table (parameters table#3) as numeric Hz, indexed by the raw
    // 0..60 value. THRU (0/60) -> 0 = bypass. Matches the string table the Part/Multi
    // EQ panels display.
    //
    // LEGACY ONLY: the part/drum mid-band FREQ used to be stored as one of these raw
    // indices. It is now absolute Hz off the finer freqladder, so this is needed just
    // to migrate project state saved before that change (see setStateInformation).
    float eqLegacyRawToHz(int raw)
    {
        static const float kHz[] = {
            0.f, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 70, 80, 90, 100, 110, 125,
            140, 160, 180, 200, 225, 250, 280, 315, 355, 400, 450, 500, 560, 630, 700,
            800, 900, 1000, 1100, 1200, 1400, 1600, 1800, 2000, 2200, 2500, 2800, 3200,
            3600, 4000, 4500, 5000, 5600, 6300, 7000, 8000, 9000, 10000, 11000, 12000,
            14000, 16000, 18000, 0.f
        };
        constexpr int n = (int) (sizeof(kHz) / sizeof(kHz[0]));
        return kHz[juce::jlimit(0, n - 1, raw)];
    }
}

void YamahaMU100AudioProcessor::initPartEqDefaults()
{
    for (int p = 0; p < 32; ++p)
        for (int b = 0; b < 3; ++b)
        {
            partEqGain[p][b] = 64;                      // 0 dB = flat / bypass
            partEqFreq[p][b] = kPartEqFreqDefault[b];
            partEqQ[p][b]    = 7;                        // 0.7
        }
}

// Convert a part+band's stored editor values to physical units (dB / Hz / Q) and
// push them to the SWP30 (no-op until MAME is up; the dirty flag re-pushes after boot).
void YamahaMU100AudioProcessor::pushPartEqBand(int part, int band)
{
    if (! partEqValid(part, band)) return;
    // A DRUMS buffer does NOT mute these: the plugin bands run on a drum part too,
    // on top of its per-key layer. Only the XG shelves die there (partShelfEqInert).
    const float gainDb = (float) (partEqGain[part][band] - 64) * (12.0f / 64.0f);   // raw 0..127, 64 = 0 dB, ±12 dB
    const float freqHz = (float) partEqFreq[part][band];            // already absolute Hz
    const float q      = (float) partEqQ[part][band] / 10.0f;      // raw 1..120 = 0.1..12.0
    if (MameLease mame { *this })
        swp30_ext::set_part_eq(*mame, part, band, gainDb, freqHz, q);
    partEqDirty.store(true, std::memory_order_release);
}

void YamahaMU100AudioProcessor::setPartEqGain(int part, int band, int raw)
{
    if (! partEqValid(part, band)) return;
    if (undoSuppressDepth == 0)
        undoPushPlugin({ mu100undo::PluginField::PartEqGain, part, band, 0,
                         (double) partEqGain[part][band], (double) raw });
    partEqGain[part][band] = raw;
    pushPartEqBand(part, band);
}

void YamahaMU100AudioProcessor::setPartEqFreq(int part, int band, int hz)
{
    if (! partEqValid(part, band)) return;
    if (undoSuppressDepth == 0)
        undoPushPlugin({ mu100undo::PluginField::PartEqFreq, part, band, 0,
                         (double) partEqFreq[part][band], (double) hz });
    partEqFreq[part][band] = hz;
    pushPartEqBand(part, band);
}

void YamahaMU100AudioProcessor::setPartEqQ(int part, int band, int raw)
{
    if (! partEqValid(part, band)) return;
    if (undoSuppressDepth == 0)
        undoPushPlugin({ mu100undo::PluginField::PartEqQ, part, band, 0,
                         (double) partEqQ[part][band], (double) raw });
    partEqQ[part][band] = raw;
    pushPartEqBand(part, band);
}

void YamahaMU100AudioProcessor::reassertPartEqIfDirty()
{
    if (mameMachine == nullptr) return;
    if (partEqDirty.exchange(false, std::memory_order_acq_rel))
        for (int part = 0; part < 32; ++part)
            for (int band = 0; band < 3; ++band)
            {
                const float gainDb = (float) (partEqGain[part][band] - 64) * (12.0f / 64.0f);
                const float freqHz = (float) partEqFreq[part][band];
                const float q      = (float) partEqQ[part][band] / 10.0f;
                swp30_ext::set_part_eq(*mameMachine, part, band, gainDb, freqHz, q);
            }
}

//==============================================================================
// Per-drum-key EQ/HPF (keyed by DRUMS buffer + key; shared across parts on the
// buffer). Same raw->physical conversion as the per-part EQ/HPF.
void YamahaMU100AudioProcessor::initDrumFxDefaults()
{
    for (int p = 0; p < 32; ++p) partDrumBuffer[p] = -1;
    for (int buf = 0; buf < 4; ++buf)
        for (int key = 0; key < 128; ++key)
        {
            drumHpfHz[buf][key] = 0.0f;
            for (int b = 0; b < 3; ++b)
            {
                drumEqGain[buf][key][b] = 64;
                drumEqFreq[buf][key][b] = kPartEqFreqDefault[b];
                drumEqQ[buf][key][b]    = 7;
            }
        }
}

void YamahaMU100AudioProcessor::pushDrumHpf(int buffer, int key)
{
    if (! drumIdx(buffer, key)) return;
    if (MameLease mame { *this })
        swp30_ext::set_drum_hpf(*mame, buffer, key, drumHpfHz[buffer][key]);
    drumFxDirty.store(true, std::memory_order_release);
}

void YamahaMU100AudioProcessor::pushDrumBand(int buffer, int key, int band)
{
    if (! drumBandIdx(buffer, key, band)) return;
    const float gainDb = (float) (drumEqGain[buffer][key][band] - 64) * (12.0f / 64.0f);
    const float freqHz = (float) drumEqFreq[buffer][key][band];      // already absolute Hz
    const float q      = (float) drumEqQ[buffer][key][band] / 10.0f;
    if (MameLease mame { *this })
        swp30_ext::set_drum_eq(*mame, buffer, key, band, gainDb, freqHz, q);
    drumFxDirty.store(true, std::memory_order_release);
}

void YamahaMU100AudioProcessor::setPartDrumBuffer(int part, int buffer)
{
    if (part < 0 || part >= 32) return;
    const int b = (buffer >= 0 && buffer < 4) ? buffer : -1;
    const bool bufferChanged = (b != partDrumBuffer[part]);
    partDrumBuffer[part] = b;
    // Keep the buffer->parts bitmask in step (read by observeMidiActivity to map a
    // DRUM SETUP message onto the parts it actually affects).
    for (int i = 0; i < 4; ++i)
    {
        const uint32_t bit = 1u << part;
        if (i == b) drumBufferParts[i].fetch_or (bit, std::memory_order_release);
        else        drumBufferParts[i].fetch_and(~bit, std::memory_order_release);
    }
    if (MameLease mame { *this })
        swp30_ext::set_part_buffer(*mame, part, b);
    drumFxDirty.store(true, std::memory_order_release);
    // NOTHING to re-push for the part-level plugin EQ/HPF: those run whatever the
    // buffer is. (They used to be forced flat here for a drum part, which is why this
    // function once re-pushed them both on every buffer change.) The two XG shelves
    // are the firmware's and need no push either -- the device simply ignores them.

    // The EQ BYPASS, though, is a function OF membership -- who is affected, and which
    // buffers must have their shared per-key shelves flattened -- so it is re-derived
    // here rather than left holding an answer from before the part moved. Cheap enough
    // to call unconditionally (see applyEqBypassChosen), and `bufferChanged` only keeps
    // it off the path of pushDrumBufferMap's 31 no-op pushes.
    //
    // Called PER PART, so a batch that moves two parts at once reconciles between them.
    // That can cost one extra pair of shelf writes mid-batch; the state it lands in is
    // correct at every step, which is the property worth having.
    if (bufferChanged) applyEqBypassChosen(partEqBypassChosen);
}

void YamahaMU100AudioProcessor::setDrumHpf(int buffer, int key, float hz)
{
    if (! drumIdx(buffer, key)) return;
    if (undoSuppressDepth == 0)
        undoPushPlugin({ mu100undo::PluginField::DrumHpf, buffer, key, 0,
                         drumHpfHz[buffer][key], hz });
    drumHpfHz[buffer][key] = hz;
    pushDrumHpf(buffer, key);
}

void YamahaMU100AudioProcessor::setDrumEqGain(int buffer, int key, int band, int raw)
{
    if (! drumBandIdx(buffer, key, band)) return;
    if (undoSuppressDepth == 0)
        undoPushPlugin({ mu100undo::PluginField::DrumEqGain, buffer, key, band,
                         (double) drumEqGain[buffer][key][band], (double) raw });
    drumEqGain[buffer][key][band] = raw;
    pushDrumBand(buffer, key, band);
}

void YamahaMU100AudioProcessor::setDrumEqFreq(int buffer, int key, int band, int hz)
{
    if (! drumBandIdx(buffer, key, band)) return;
    if (undoSuppressDepth == 0)
        undoPushPlugin({ mu100undo::PluginField::DrumEqFreq, buffer, key, band,
                         (double) drumEqFreq[buffer][key][band], (double) hz });
    drumEqFreq[buffer][key][band] = hz;
    pushDrumBand(buffer, key, band);
}

void YamahaMU100AudioProcessor::setDrumEqQ(int buffer, int key, int band, int raw)
{
    if (! drumBandIdx(buffer, key, band)) return;
    if (undoSuppressDepth == 0)
        undoPushPlugin({ mu100undo::PluginField::DrumEqQ, buffer, key, band,
                         (double) drumEqQ[buffer][key][band], (double) raw });
    drumEqQ[buffer][key][band] = raw;
    pushDrumBand(buffer, key, band);
}

// A new kit assigned to a DRUMS buffer clears that buffer's drum EQ/HPF back to
// neutral (flat / HPF off) — the plugin bands have no kit-provided value, matching
// how the hardware drum params reset on kit reassignment.
void YamahaMU100AudioProcessor::resetDrumBuffer(int buffer, int msb, int lsb, int pgm,
                                                int oldMsb, int oldLsb, int oldPgm)
{
    if (buffer < 0 || buffer >= 4) return;

    // The user's OWN per-key edits: every byte the model holds that the OUTGOING kit's
    // defaults do not account for. They are lifted out before the reseed below
    // overwrites them, because they are the one thing in this block the table cannot
    // regenerate.
    //
    // This is what makes an accidental kit change survivable, and it is not a corner
    // case: the undo history is in-memory only and loadEditorState clears it, so a
    // session that has just REOPENED a project has an empty stack. Without this the
    // reseed would be a one-way door -- the device re-initialises itself the moment
    // the program change lands, so the model was the last place hand-edited drum keys
    // still existed.
    std::vector<mu100undo::ByteDelta> edits;
    if (undoSuppressDepth == 0)
        collectDrumBufferEdits(buffer, oldMsb, oldLsb, oldPgm, msb, lsb, pgm, edits);

    // The HARDWARE half: put the model's 3n block where the device's own re-init just
    // put the buffer, so the two halves of every drum key describe the same kit. Until
    // this existed the model kept the OLD kit's per-key values and a save/reload
    // resurrected them onto the new kit. Recorded for undo as a RESEED plus those
    // edits, not as 2686 ByteDeltas -- see Step::drumReseed.
    if (loadDrumBufferDefaults(buffer, msb, lsb, pgm))
        undoPushDrumChange(buffer, std::move(edits));

    // Record what is about to be wiped. This writes the arrays directly rather than
    // going through setDrum*, so nothing else would capture it — and a voice change
    // that restored the kit but left the per-key EQ flattened would be a half-undo,
    // which is worse than none. Sparse: on an untouched buffer this costs one pass
    // and pushes nothing.
    if (undoSuppressDepth == 0)
    {
        mu100undo::Step wiped;
        undoSnapshotDrumBuffer(buffer, wiped.plugin);
        if (! wiped.plugin.empty())
            undoCommit(std::move(wiped), 0);   // 0 = never merge into this
    }

    for (int key = 0; key < 128; ++key)
    {
        drumHpfHz[buffer][key] = 0.0f;
        for (int b = 0; b < 3; ++b)
        {
            drumEqGain[buffer][key][b] = 64;
            drumEqFreq[buffer][key][b] = kPartEqFreqDefault[b];
            drumEqQ[buffer][key][b]    = 7;
        }
        pushDrumHpf(buffer, key);
        for (int b = 0; b < 3; ++b) pushDrumBand(buffer, key, b);
    }
}

void YamahaMU100AudioProcessor::reassertDrumFxIfDirty()
{
    if (mameMachine == nullptr) return;
    if (! drumFxDirty.exchange(false, std::memory_order_acq_rel)) return;
    for (int part = 0; part < 32; ++part)
        swp30_ext::set_part_buffer(*mameMachine, part, partDrumBuffer[part]);
    for (int buffer = 0; buffer < 4; ++buffer)
        for (int key = 0; key < 128; ++key)
        {
            swp30_ext::set_drum_hpf(*mameMachine, buffer, key, drumHpfHz[buffer][key]);
            for (int b = 0; b < 3; ++b)
            {
                const float gainDb = (float) (drumEqGain[buffer][key][b] - 64) * (12.0f / 64.0f);
                const float freqHz = (float) drumEqFreq[buffer][key][b];
                const float q      = (float) drumEqQ[buffer][key][b] / 10.0f;
                swp30_ext::set_drum_eq(*mameMachine, buffer, key, b, gainDb, freqHz, q);
            }
        }
}

//==============================================================================
// Per-part spectrum probe — see setSpectrumTarget in the header for the contract.
//
// Deliberately NOT given a reassert*IfDirty counterpart like the HPF/EQ values: the
// analyser re-pushes its target on every frame anyway, so a (re)boot self-heals on
// the next tick without any dirty-flag plumbing.
void YamahaMU100AudioProcessor::setSpectrumTarget(bool active, int part, int drumKey)
{
    if (part < 0 || part >= 32)
        active = false;

    const int key    = (drumKey >= 0 && drumKey < 128) ? drumKey : 0xff;
    const int packed = active ? ((1 << 16) | (part << 8) | key) : 0;

    // Invalidate the cache whenever MAME is down. This one line does two jobs: it
    // stops us caching a target that was never actually pushed, AND it forces a
    // re-push after a REBOOT (a fresh swp30 starts with the probe off, so a cache
    // still claiming "enabled" would leave the analyser permanently dead).
    MameLease mame(*this);
    if (! mame)
    {
        lastSpectrumTarget = -1;
        return;
    }

    if (packed == lastSpectrumTarget)
        return;                       // called every frame; only a real change goes through

    lastSpectrumTarget = packed;
    swp30_ext::set_spectrum_probe(*mame, active, part, drumKey);
}

// "Enable DAW automation" — see dawAutomationEnabled in the header. Stops or starts
// the AutomationParams bridge; re-enabling resyncs from the model first, so the host
// parameters re-adopt whatever the device holds now rather than firing a burst of
// device writes from values frozen when the bridge stopped.
void YamahaMU100AudioProcessor::setDawAutomationEnabled(bool on)
{
    if (dawAutomationEnabled == on)
        return;

    dawAutomationEnabled = on;

    if (automation != nullptr)
        automation->setEnabled(on);
}

// Per-part stereo outputs — see setPartOutputsEnabled in the header.
//
// Unlike the spectrum probe this DOES need a reassert counterpart: nothing re-pushes
// it periodically, and a fresh swp30 starts with capture off, so without one a reboot
// would silently leave every aux bus dead until the user toggled the option.
void YamahaMU100AudioProcessor::setPartOutputsEnabled(bool enable)
{
    if (enable && auxRing == nullptr)
    {
        // Allocated once and never freed — see the header. Zeroed so a bus that is
        // enabled before MAME has produced anything outputs silence, not garbage.
        auxRing = std::make_unique<float[]>(size_t(kNumAuxChannels) * AUX_RING_SIZE);
        std::fill_n(auxRing.get(), size_t(kNumAuxChannels) * AUX_RING_SIZE, 0.0f);
        partFrameScratch.assign(size_t(kNumAuxChannels) * 4096, 0);
    }

    if (partOutputsEnabled.exchange(enable, std::memory_order_relaxed) == enable)
        return;

    for (auto& b : auxDcBlocker)
        b.reset();
    auxShortReadLogged = false;

    // Gate the readers around the swp30 call: on the way in the ring must be live
    // before anything can be captured into it, on the way out the readers must stop
    // before the writer does.
    if (enable)
        auxRingLive.store(true, std::memory_order_release);

    if (MameLease mame { *this })
        swp30_ext::set_part_bus_capture(*mame, enable);

    if (! enable)
        auxRingLive.store(false, std::memory_order_release);

    // Sticky, exactly as setCleanHpfHz does it: left set so the MAME thread re-pushes
    // after any (re)boot, since a fresh swp30 comes up with capture off.
    partOutputsDirty.store(true, std::memory_order_release);
}

void YamahaMU100AudioProcessor::setPartOutputsRemoveFromMain(bool remove)
{
    partOutputsRemoveFromMain.store(remove, std::memory_order_relaxed);
    // Force the MAME thread to re-evaluate the mask on its next update, whichever way
    // this went — the push is level-triggered off auxExcludePushed, not edge-triggered.
    partOutputsDirty.store(true, std::memory_order_release);
}

void YamahaMU100AudioProcessor::reassertPartOutputsIfDirty()
{
    if (mameMachine == nullptr) return;

    if (partOutputsDirty.exchange(false, std::memory_order_acq_rel))
    {
        swp30_ext::set_part_bus_capture(*mameMachine,
                                        partOutputsEnabled.load(std::memory_order_relaxed));
        // The chip's copy can no longer be assumed to match ours. On a real (re)boot it
        // was reset with the device; on the other paths that raise the dirty flag (a
        // prepare/release cycle, the remove-from-main option) it was NOT — set_part_bus_capture
        // deliberately leaves the exclude mask alone — so this must only invalidate, and
        // let the comparison below re-push whatever the current mask is.
        auxExcludeValid = false;
    }

    // Only the parts actually going somewhere are taken out of the main mix, and only
    // while both the feature and the option are on.
    const uint32_t want =
        (partOutputsEnabled.load(std::memory_order_relaxed)
         && partOutputsRemoveFromMain.load(std::memory_order_relaxed))
            ? auxBusMask.load(std::memory_order_relaxed)
            : 0u;

    if (! auxExcludeValid || want != auxExcludePushed)
    {
        swp30_ext::set_part_bus_exclude(*mameMachine, want);
        auxExcludePushed = want;
        auxExcludeValid  = true;
    }

    // Mute/solo rides along on the same MAME-thread pass: it is the same push shape
    // (level-triggered against a shadow copy, invalidated rather than cleared) and it
    // must survive a reboot for the same reason.
    if (partAudibleDirty.exchange(false, std::memory_order_acq_rel))
        partAudibleValid = false;

    const uint32_t aud = partAudibleMask();
    if (! partAudibleValid || aud != partAudiblePushed)
    {
        swp30_ext::set_part_audible(*mameMachine, aud);
        partAudiblePushed = aud;
        partAudibleValid  = true;
    }

    // EQ bypass rides along the same way. Only the PLUGIN-owned half of the chain is
    // in this mask; the XG shelves went out over SysEx when the bit moved, and a
    // reboot re-sends those with the rest of the patch, so there is nothing to
    // re-assert for them here.
    if (partEqBypassDirty.exchange(false, std::memory_order_acq_rel))
        partEqBypassValid = false;

    const uint32_t byp = partEqBypassMask.load(std::memory_order_relaxed);
    if (! partEqBypassValid || byp != partEqBypassPushed)
    {
        swp30_ext::set_part_eq_bypass(*mameMachine, byp);
        partEqBypassPushed = byp;
        partEqBypassValid  = true;
    }
}

//==============================================================================
// Per-part mute / solo. Nothing here transmits: the four setters only move bits,
// and the MAME thread picks them up in reassertPartOutputsIfDirty above. See the
// header for why this is neither persisted nor automatable.
void YamahaMU100AudioProcessor::setPartMuted(int part, bool muted)
{
    if (part < 0 || part >= 32) return;
    const uint32_t bit = 1u << part;
    const uint32_t cur = partMuteMask.load(std::memory_order_relaxed);
    setPartMuteMask(muted ? (cur | bit) : (cur & ~bit));
}

void YamahaMU100AudioProcessor::setPartSoloed(int part, bool soloed)
{
    if (part < 0 || part >= 32) return;
    const uint32_t bit = 1u << part;
    const uint32_t cur = partSoloMask.load(std::memory_order_relaxed);
    setPartSoloMask(soloed ? (cur | bit) : (cur & ~bit));
}

// A part is never both muted and soloed: whichever mask is being SET wins for the
// parts it names, and the other gives up exactly those bits. Enforced in this one
// place, so a cell click, a Ctrl+click and the badge's clear-all all obey it without
// each having to know the rule.
//
// It is a real invariant, not a tidy-up. The state it removes was one where the M
// cell sat lit and cyan on a part you could hear — solo used to win — and a control
// that says the opposite of what you hear is worse than either rule for resolving it.
//
// Only the OTHER mask is filtered. Clearing one (mask 0) leaves the other alone,
// since `~0` masks nothing off, which is what the badge's clear-all relies on.
void YamahaMU100AudioProcessor::setMuteSoloMasks(uint32_t mute, uint32_t solo)
{
    const bool moved = partMuteMask.exchange(mute, std::memory_order_relaxed) != mute;
    const bool moved2 = partSoloMask.exchange(solo, std::memory_order_relaxed) != solo;
    // Both are stored before the callback: it repaints from BOTH, so notifying
    // between the two writes would render one frame of the state we just excluded.
    if ((moved || moved2) && onPartMuteSoloChanged)
        onPartMuteSoloChanged();
}

//==============================================================================
// Per-part EQ bypass. See the header for what it does and does not cover.
//==============================================================================
// A MELODIC part is bypassed alone -- always, whatever else shares anything with it,
// because everything the bypass governs there is keyed by PART.
//
// A part with a DRUMS BUFFER is a different animal, and the scope follows the DATA
// rather than the control: it is every part on that buffer, each with its own lamp lit
// and counted in the badge. The kit's per-key BASS/TREBLE are FIRMWARE parameters
// belonging to the shared DRUM SETUP, so the only lever is data every part on the
// buffer reads -- a part-scoped bypass could not switch them off for the part the user
// asked about without either leaving them audible (incomplete) or flattening them for
// the buffer's other parts behind a dark lamp (silent, and a state that corresponds to
// nothing). Widening the whole thing is the answer that stays visible.
//
// The COST, which was not there when this was first written (2026-09-03) and is now:
// a drum-buffer part HAS a live part-level EQ again -- its plugin HPF2 and mid bands
// run on top of the key layer -- so this also switches those off on a sibling part the
// user never touched. Weighed and accepted by the user 2026-09-05: everyone playing one
// kit hears it the same way, and no affected part is left with a dark lamp.
// WHAT IS STORED IS WHAT THE USER PRESSED, and everything else is worked out from it
// every time it could have changed. Storing the affected SET instead (which this did
// until 2026-09-05) means storing an answer that depends on buffer membership, and
// membership moves: assigning a part to a DRUMS buffer whose kit was already bypassed
// left that part dark and uncounted while it silently lost the kit's per-key shelves,
// and moving a bypassed part OFF a buffer left that buffer's shelves flattened on the
// device with nothing anywhere still pointing at it. Both were the stale answer, not a
// wrong rule -- so the fix is to stop keeping the answer.
void YamahaMU100AudioProcessor::setPartEqBypass(int part, bool on)
{
    if (part < 0 || part >= 32) return;
    const uint32_t self = 1u << part;
    // ENGAGING chooses THIS PART only: deriveEqBypassMask widens it to the kit, now and
    // after every later membership change. RELEASING has to clear the whole group's
    // choices -- releasing from a part that is only lit because a SIBLING was chosen
    // must let go of the sibling's choice, or the derivation lights it straight back up.
    const int buf = partDrumBuffer[part];
    const uint32_t group = (buf >= 0 && buf < 4)
                             ? (self | drumBufferParts[buf].load(std::memory_order_acquire))
                             : self;
    applyEqBypassChosen(on ? (partEqBypassChosen | self)
                           : (partEqBypassChosen & ~group));
}

// The mask form: the argument is the CHOSEN set, not the affected set. Both callers pass
// a whole-editor value (0 = release everything, ~0 = every part), where the two are the
// same thing.
void YamahaMU100AudioProcessor::setPartEqBypassMask(uint32_t mask)
{
    applyEqBypassChosen(mask);
}

// Chosen -> affected: every chosen part, plus every part sharing a DRUMS buffer with one.
// A kit's per-key BASS/TREBLE are one shared copy in the firmware, so a bypass on any
// part of a kit is a bypass of the kit (see the SCOPE note in the header).
uint32_t YamahaMU100AudioProcessor::deriveEqBypassMask(uint32_t chosen) const
{
    uint32_t eff = chosen;
    for (int buf = 0; buf < 4; ++buf)
    {
        const uint32_t members = drumBufferParts[buf].load(std::memory_order_acquire);
        if ((chosen & members) != 0) eff |= members;
    }
    return eff;
}

// The single point where the bypass state is (re)established: from the choices, and from
// the membership as it stands RIGHT NOW. Called on every toggle and on every buffer
// change, and cheap to call when nothing moved -- with nothing chosen and no buffer
// flattened it compares two zeroes and returns.
void YamahaMU100AudioProcessor::applyEqBypassChosen(uint32_t chosen)
{
    partEqBypassChosen = chosen;

    const uint32_t want = deriveEqBypassMask(chosen);
    const uint32_t old  = partEqBypassMask.exchange(want, std::memory_order_relaxed);

    if (old != want)
    {
        // The plugin-owned half is a mask the MAME thread picks up; the part's own
        // firmware shelves have to be transmitted, and only for the parts that moved.
        partEqBypassDirty.store(true, std::memory_order_release);
        applyPartShelfBypass(old ^ want, want);
    }

    // The buffers are reconciled whether or not the mask moved: a chosen part LEAVING a
    // buffer changes what that buffer needs without changing a single bit here.
    applyBufferShelfBypass(want);

    if (old != want && onPartEqBypassChanged) onPartEqBypassChanged();
}

// A PART's own XG BASS/TREBLE shelves, which the firmware owns: engaging transmits 0 dB,
// releasing transmits the model's value back.
//
// sendSysEx, NOT sendParamChange: that one folds what it sends into editorState, and
// folding a bypass into the model is the exact thing this feature must not do -- the
// user's values have to survive a save made while bypassed. It also keeps the bypass
// out of undo, where a monitoring control does not belong.
//
// A shelf already at 0 dB is skipped in both directions. That is what keeps the
// all-parts case to a handful of messages rather than 64: on a default patch every
// shelf is flat and this sends nothing at all.
void YamahaMU100AudioProcessor::applyPartShelfBypass(uint32_t changed, uint32_t now)
{
    if (changed == 0 || editorState == nullptr) return;
    using mu100::AddressMap;

    // Resolved once: find() is a linear scan of the whole parameter table.
    static const int kShelfGain[2] = { AddressMap::find("EQ BASS GAIN",   "MULTI PART"),
                                       AddressMap::find("EQ TREBLE GAIN", "MULTI PART") };

    for (int part = 0; part < 32; ++part)
    {
        if (((changed >> part) & 1u) == 0) continue;
        const bool bypassing = ((now >> part) & 1u) != 0;

        // ONLY on a part that has some: the device ignores part EQ on a drum part
        // (owner's manual p.82, and measured), so transmitting these there would be
        // traffic that changes nothing. The part's PLUGIN bands and HPF2 need nothing
        // here on any part -- they ride the mask.
        if (partShelfEqInert(part)) continue;

        for (const int idx : kShelfGain)
        {
            if (idx < 0) continue;
            const auto v = AddressMap::get(*editorState, idx, part);
            const int  stored = v.present ? v.combined : 64;
            if (stored == 64) continue;             // already flat: nothing to bypass
            sendSysEx(AddressMap::makeParamChange(AddressMap::param(idx), part,
                                                  (uint32_t) (bypassing ? 64 : stored)));
        }
    }
}

// A DRUM SETUP's per-key BASS/TREBLE: the ONE buffer-scoped thing here, and not by
// choice. They are drum-key-level data the FIRMWARE owns, shared by every part on the
// buffer, so the only lever is the shared parameter -- any part on that buffer loses
// them too, exactly as it does when the user EDITS one, which is how a DRUM SETUP has
// always worked.
//
// DRIVEN BY THE BUFFER, NOT BY THE PARTS THAT MOVED. What a buffer NEEDS is "does any
// part on it, as things stand right now, want the kit bypassed"; what it HAS is
// bufferShelvesZeroed -- what this function last actually wrote to it. Acting on the
// difference is what makes membership changes come out right. The part-driven version
// this replaces could only act on parts whose BIT moved, so a buffer whose MEMBERSHIP
// moved under a steady mask was never revisited: a part joining a bypassed kit silently
// lost the kit's shelves with a dark lamp, and a bypassed part moving away took the
// buffer's identity with it (the release read partDrumBuffer[part], which by then named
// the NEW buffer) and left the old one flattened for good.
//
// Idempotent and cheap: with nothing bypassed and nothing flattened it is four
// comparisons, which is what lets every buffer change call it unconditionally.
void YamahaMU100AudioProcessor::applyBufferShelfBypass(uint32_t now)
{
    if (editorState == nullptr) return;
    using mu100::AddressMap;

    static const int kDrumShelfGain[2] = { AddressMap::find("EQ BASS GAIN",   "DRUM SETUP"),
                                           AddressMap::find("EQ TREBLE GAIN", "DRUM SETUP") };

    for (int buf = 0; buf < 4; ++buf)
    {
        const uint32_t members = drumBufferParts[buf].load(std::memory_order_acquire);
        const bool wantZero = (now & members) != 0;
        if (wantZero == bufferShelvesZeroed[buf]) continue;
        bufferShelvesZeroed[buf] = wantZero;

        // Only the keys actually edited away from 0 dB, which on any normal kit is a
        // handful and usually none -- the alternative is 79 keys x 2 params of traffic
        // for nothing. kDrumKeyLo/Hi are the XG drum note range (0x0D..0x5B).
        constexpr int kDrumKeyLo = 0x0D, kDrumKeyHi = 0x5B;
        for (int key = kDrumKeyLo; key <= kDrumKeyHi; ++key)
            for (const int idx : kDrumShelfGain)
            {
                if (idx < 0) continue;
                const auto v = AddressMap::get(*editorState, idx, key, buf);
                if (! v.present || v.combined == 64) continue;
                sendSysEx(AddressMap::makeParamChange(AddressMap::param(idx), key,
                                                      (uint32_t) (wantZero ? 64 : v.combined),
                                                      buf));
            }
    }
}

void YamahaMU100AudioProcessor::setPartMuteMask(uint32_t mask)
{
    setMuteSoloMasks(mask, partSoloMask.load(std::memory_order_relaxed) & ~mask);
}

void YamahaMU100AudioProcessor::setPartSoloMask(uint32_t mask)
{
    setMuteSoloMasks(partMuteMask.load(std::memory_order_relaxed) & ~mask, mask);
}

// Per-part level meter — see setPartLevelMeter in the header for the contract.
// Same cache-and-retry shape as setSpectrumTarget above, and for the same reasons:
// it is called every meter frame, it must be a no-op when nothing changed, and MAME
// being down must invalidate the cache so a reboot re-pushes into a fresh swp30.
void YamahaMU100AudioProcessor::setPartLevelMeter(bool active)
{
    // The master ("S") half is ours, not swp30's, so arm it whatever the engine is
    // doing — a booting engine still has an output buffer being written.
    masterMeterArmed.store(active, std::memory_order_relaxed);
    if (! active)
        masterPeak.store(0.0f, std::memory_order_relaxed);

    MameLease mame(*this);
    if (! mame)
    {
        lastPartLevelMeter = -1;
        return;
    }

    const int want = active ? 1 : 0;
    if (want == lastPartLevelMeter)
        return;

    lastPartLevelMeter = want;
    swp30_ext::set_part_level_meter(*mame, active);
}

int YamahaMU100AudioProcessor::readPartLevels(float* dst, int maxParts)
{
    if (dst == nullptr || maxParts <= 0)
        return 0;
    MameLease mame(*this);
    if (! mame)
        return 0;

    uint32_t raw[32] = {};
    const int n = swp30_ext::read_part_levels(*mame, raw, juce::jmin(maxParts, 32));
    for (int p = 0; p < n; ++p)
        dst[p] = (float) raw[p] * kPartBusScale;   // the MIXER-bus reference, not the probe's
    return n;
}

int YamahaMU100AudioProcessor::readSpectrum(float* dst, int numSamples)
{
    if (dst == nullptr || numSamples <= 0)
        return 0;
    MameLease mame(*this);
    if (! mame)
        return 0;
    return swp30_ext::read_spectrum(*mame, dst, numSamples);
}

void YamahaMU100AudioProcessor::pushMidiByteToPort(int port, uint8_t data, double targetMameTime)
{
    pushMidiMessageToPort(port, &data, 1, targetMameTime, 0.0);
}

bool YamahaMU100AudioProcessor::pushMidiMessageToPort(int port, const uint8_t* data, int numBytes,
                                                      double startTime, double timePerByte)
{
    if (data == nullptr || numBytes <= 0)
        return false;

    MidiRing& ring = (port == 1) ? midiRing2 : midiRing;
    const uint32_t want = static_cast<uint32_t>(numBytes);

    // Never let an impossible timestamp into the ring. Delivery gates on the HEAD SLOT
    // ONLY, so a single unreachable target blocks the whole port for the rest of the
    // session — which is exactly how the 2026-07-30 port-A freeze happened. Clamp
    // rather than drop: a message whose timestamp is wrong should still be played,
    // just immediately. The NaN test must be explicit — every comparison against NaN
    // is false, so a bare `>` would wave it straight through.
    {
        const double clockNow = publishedMameTime.load(std::memory_order_acquire);
        if (std::isnan(startTime) || startTime > clockNow + kMaxMidiLeadSeconds)
            startTime = clockNow;
    }

    // Claim the whole message's slots in ONE step. A bare fetch_add would be simpler
    // but a reservation cannot be handed back, so a full ring would be overrun by a
    // claim that could not be honoured; the CAS makes "is there room" and "take it"
    // atomic. Contention is negligible here (a few thousand MIDI bytes a second), so
    // the loop is not a real-time concern on the audio thread.
    uint32_t base = ring.reserved.load(std::memory_order_relaxed);
    for (;;)
    {
        // Unsigned difference: exact across wraparound.
        const uint32_t used = base - ring.readPos.load(std::memory_order_acquire);
        if (used + want > MIDI_BUFFER_SIZE)
            return false;                       // full — drop the message whole
        if (ring.reserved.compare_exchange_weak(base, base + want,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed))
            break;
    }

    // Publish in order, so the consumer always sees a valid prefix of the message.
    for (uint32_t i = 0; i < want; ++i)
    {
        const uint32_t idx = (base + i) & MIDI_BUFFER_MASK;
        ring.slot[idx] = { data[i], startTime + i * timePerByte };
        ring.ready[idx].store(1, std::memory_order_release);
    }
    return true;
}

double YamahaMU100AudioProcessor::pushSysExStream(int port, const uint8_t* data, size_t size,
                                                  double startTime, double timePerByte,
                                                  double bulkGap)
{
    double t = startTime;
    size_t i = 0;
    while (i < size)
    {
        size_t len  = 1;
        bool   bulk = false;

        if (data[i] == 0xF0)
        {
            size_t j = i + 1;
            while (j < size && data[j] != 0xF7) ++j;
            if (j < size) ++j;                  // include the terminating F7
            len = j - i;
            // Yamaha BULK DUMP: F0 43 0n <model> …, model 4C (XG) / 49 (MU native) /
            // 59 (Performance). Parameter changes (43 1n) are small and are handled
            // between bytes, so they need no gap.
            bulk = len >= 10 && data[i + 1] == 0x43
                && (data[i + 2] & 0xF0) == 0x00
                && (data[i + 3] == 0x4C || data[i + 3] == 0x49 || data[i + 3] == 0x59);
        }

        pushMidiMessageToPort(port, data + i, static_cast<int>(len), t, timePerByte);
        t += static_cast<double>(len) * timePerByte;
        if (bulk) t += bulkGap;
        i += len;
    }
    return t;
}

// Scan a .syx byte stream for MULTI PART "PART MODE" writes (XG address 08 nn 07,
// value 2..5 = DRUMS1..4) and record which drum setups (0..3) the file puts in use.
// Handles both XG param-change (43 1n 4C) and XG bulk-dump (43 0n 4C) framings.
static void scanFileDrumSetups(const uint8_t* data, size_t size, std::vector<int>& out)
{
    std::set<int> setups;
    size_t i = 0;
    while (i < size)
    {
        if (data[i] != 0xF0) { ++i; continue; }
        size_t j = i + 1;
        while (j < size && data[j] != 0xF7) ++j;
        // message is data[i..j] (inclusive of F7 if found)
        if (j < size && j - i >= 7 && data[i + 1] == 0x43 && data[i + 3] == 0x4C)
        {
            const uint8_t cmd = data[i + 2] & 0xF0;
            auto noteModeAt = [&](uint8_t aH, uint8_t aMid, uint8_t aL, uint8_t val)
            {
                if (aH == 0x08 && aL == 0x07 && val >= 2 && val <= 5)
                    setups.insert(val - 2);
                juce::ignoreUnused(aMid);
            };
            if (cmd == 0x10)   // param change: addr = [4..6], data [7..j-1] (no checksum)
            {
                for (size_t k = i + 7; k < j; ++k)   // first data byte at i+7, F7 at j
                    noteModeAt(data[i + 4], data[i + 5],
                               (uint8_t)(data[i + 6] + (k - (i + 7))), data[k]);
            }
            else if (cmd == 0x00 && j - i >= 11)   // bulk dump: addr=[6..8], payload [9..j-2]
            {
                const uint8_t aH = data[i + 6], aMid = data[i + 7], aL = data[i + 8];
                for (size_t k = i + 9; k + 1 < j; ++k)   // byte at j-1 is checksum, F7 at j
                    noteModeAt(aH, aMid, (uint8_t)(aL + (k - (i + 9))), data[k]);
            }
        }
        i = (j < size) ? j + 1 : j;
    }
    out.assign(setups.begin(), setups.end());
}

int YamahaMU100AudioProcessor::loadSysExFile(const juce::File& syxFile, std::vector<int>* drumSetupsOut)
{
    MameLease mame(*this);
    if (! mame) return 0;

    juce::MemoryBlock syxData;
    if (syxFile.loadFileAsData(syxData) && syxData.getSize() > 0)
    {
        const uint8_t* data = static_cast<const uint8_t*>(syxData.getData());
        const size_t   size = syxData.getSize();

        if (drumSetupsOut != nullptr)
            scanFileDrumSetups(data, size, *drumSetupsOut);

        // Start a little ahead of MAME's current emulated time so the first byte
        // isn't already "late" by the time the MAME thread polls the queue.
        // A pending coalesced edit must not land in the middle of this stream (or after
        // the reset below, which would survive the load); drain it first, then start
        // past whatever is already queued. See sendTailMameTime.
        flushPendingParamChanges();
        const double timePerByte = 1.0 / (31250.0 / 10.0);   // 31250 baud, 10 bits/byte
        const double startTime   = juce::jmax(senderMameTime() + 0.5, sendTailMameTime);
        double t = startTime;

        // SysEx always targets MIDI IN A (port 0), same as the boot-time XG reset.

        // Lead with an XG System On reset so a load REPLACES the current patch rather
        // than merging onto it. Bulk-dump files (e.g. a saved Multi 32-Part dump)
        // carry no reset of their own, so without this the previous file's non-default
        // values survive. XGEdit files already begin with their own XG System On; a
        // second reset is idempotent and harmless.
        static const uint8_t kXgSystemOn[] =
            { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
        pushMidiMessageToPort(0, kXgSystemOn, (int) sizeof(kXgSystemOn), t, timePerByte);
        t += sizeof(kXgSystemOn) * timePerByte;
        t += 0.10;   // let the reset settle before restoring

        // Pace a gap after each bulk-dump message. The firmware must checksum + write
        // each block before the next arrives; a large bulk stream sent back-to-back at
        // full baud overflows the H8 SCI input and all but the first few blocks are
        // dropped. Param-change messages (XGEdit files) are small and processed between
        // bytes, so they are streamed with no extra gap.
        // NOTE: the editor panels no longer depend on this stream (they load straight
        // from the file), so it is a best-effort AUDIO update: a small gap trades some
        // completeness for a short stream (a full Multi 32-Part dump is ~1400 messages,
        // and a long stream starves live-note handling on MIDI IN A). Tunable.
        constexpr double kBulkMsgGap = 0.005;
        t = pushSysExStream(0, data, size, t, timePerByte, kBulkMsgGap);
        sendTailMameTime = t;   // a later editor edit queues after the load, not into it

        // Estimated wall-clock time until the device has consumed the whole stream
        // (scheduling lead + reset + byte-rate span + bulk-message gaps). MAME runs in
        // real time, so emulated seconds ≈ wall seconds. The caller uses this to size
        // the "Loading file" overlay (it adds its own post-stream settle margin).
        return juce::roundToInt((t - senderMameTime()) * 1000.0);
    }
    return 0;
}

double YamahaMU100AudioProcessor::reserveSendWindow(size_t numBytes)
{
    const double timePerByte = 1.0 / (31250.0 / 10.0);
    const double now         = senderMameTime();

    double startTime = juce::jmax(now + 0.05, sendTailMameTime);

    // Two ways the cursor can end up unusably far ahead, both handled by RESETTING it
    // rather than clamping the start time: clamping alone would leave the cursor beyond
    // the cap, so every later send would clamp too and the lead would latch forever.
    //   - A MAME reboot restarts the emulated clock at 0, stranding the old cursor
    //     seconds into what is now the future.
    //   - Genuine sustained overrun, which we would rather degrade into bunching (the
    //     pre-pacing behaviour) than into unbounded editor latency.
    if (startTime > now + kMaxSendTailLeadSeconds)
        startTime = now + 0.05;

    sendTailMameTime = startTime + (double) numBytes * timePerByte;
    return startTime;
}

double YamahaMU100AudioProcessor::smallSendStartTime() const
{
    // Every SMALL message-thread send — editor param changes, the Controller wheel,
    // NRPN, DAW automation — is stamped at the emulated clock, never ahead of it.
    //
    // WHY THIS IS NOT reserveSendWindow: the MIDI ring is strictly FIFO with
    // head-of-line gating (midiHeadDue hands MAME NOTHING while the head slot is not
    // due), and the host's notes are stamped at "now" by processBlock. A message
    // scheduled in the FUTURE is therefore a barrier in front of every note queued
    // behind it. reserveSendWindow's floor of now+0.05 delayed notes by up to 50 ms,
    // varying with where the sender's tick happened to fall — heard as notes scattered
    // off the beat rather than as uniform lag. The DAW-automation case surfaced it; a
    // knob dragged during playback did exactly the same thing.
    //
    // WHY LOSING THE PACING IS SAFE HERE, though it was added deliberately (e4fa18e):
    //   * The cost it existed to prevent — a lump of bytes overrunning MAME's
    //     midiin_device transmit ring — cannot happen at these sizes. XMIT_RING_SIZE is
    //     131072 bytes, ~42 s of wire, and since dcabc16e an overrun drops the new byte
    //     instead of corrupting queued ones. Filling it needs sustained oversupply, not
    //     a burst.
    //   * The SUSTAINED rate is bounded elsewhere and always was: sendParamChangeThrottled
    //     coalesces per address at 30 Hz, and AutomationParams backs off on ring backlog.
    //   * Pacing never removed the real cost anyway. Notes and editor bytes share ONE
    //     serial wire, so a message delays the next note by its own wire time no matter
    //     when it is stamped: 9 bytes = 2.9 ms. Pacing merely ADDED the 50 ms lead on
    //     top. 2.9 ms is exactly what recorded XG SysEx automation on a MIDI track
    //     costs — the same bytes on the same wire — which is why that path always
    //     sounded clean and this one did not.
    //
    // Ordering is preserved by the RING, not by the timestamps: slots are consumed in
    // reservation order, so a clock-stamped edit queued behind a paced bulk stream
    // still arrives after it (it simply becomes due the moment its turn comes). That is
    // why this needs no "is a bulk transfer in flight" special case.
    //
    // The value is the PUBLISHED clock, which lags real emulated time slightly — so the
    // message is already due and is handed over at the next poll. Bulk streams
    // (loadSysExFile, requestDeviceDump, sendPanic) keep using reserveSendWindow: they
    // are hundreds to thousands of messages and genuinely must be spread over time.
    return senderMameTime();
}

uint32_t YamahaMU100AudioProcessor::midiRingBacklog(int port) const
{
    // Bytes reserved but not yet consumed by MAME. A DIRECT measure of "the wire is
    // behind", which is what a clock-stamped sender must throttle against — it has no
    // queue-tail cursor to consult, and unlike that cursor this also counts the host's
    // own note traffic. Relaxed loads: an off-by-a-few reading of a rate-limit is fine.
    const MidiRing& ring = (port == 1) ? midiRing2 : midiRing;
    return ring.reserved.load(std::memory_order_relaxed)
         - ring.readPos .load(std::memory_order_relaxed);
}

bool YamahaMU100AudioProcessor::sendSysEx(const std::vector<uint8_t>& msg)
{
    MameLease mame(*this);
    if (! mame || msg.empty()) return false;

    // Stamped at the CLOCK (see smallSendStartTime), all to MIDI IN A (port 0).
    const double timePerByte = 1.0 / (31250.0 / 10.0);
    pushMidiMessageToPort(0, msg.data(), (int) msg.size(), smallSendStartTime(), timePerByte);
    teeSysExToMidiOut(msg);
    return true;
}

void YamahaMU100AudioProcessor::teeSysExToMidiOut(const std::vector<uint8_t>& msg)
{
    // Mirror this edit onto the plugin's single MIDI OUT so it can be recorded as XG
    // SysEx (same byte format XGEdit produced). Only well-formed F0…F7 messages (the
    // editor always sends these).
    //   VST3:       queue for processBlock() to place on the host output bus.
    //   Standalone: send now to any user-activated MIDI OUTPUT device (there is no host
    //               bus), e.g. a virtual loopMIDI port a DAW records.
    //
    // In CC mode the copy is the equivalent controller instead, and an edit with no
    // controller sends NOTHING -- deliberately no SysEx fallback (see midiOutFormat).
    if (midiOutFormat == MidiOutCC)
    {
        uint8_t cc[3];
        if (ccForParamChange(msg, cc))
        {
            // A SWITCH is sent as a HOLD POINT plus the new value, back to back in one
            // block. A DAW may store recorded controller data as a curve and RAMP
            // between successive points on playback (Cubase does); CC65 acts at a
            // threshold of 64, so a ramp from 0 to 127 flipped portamento halfway
            // between the two recorded events -- late, by however far apart they were.
            // USER-REPORTED 2026-10-04. Repeating the OLD value at the same instant
            // makes the ramp zero-length, i.e. a jump, with nothing for the user to
            // edit. A switch has only two values, so the old one is the other one.
            if (cc[1] == 65)
            {
                const uint8_t hold[3] = { cc[0], cc[1], (uint8_t) (cc[2] != 0 ? 0 : 127) };
                teeChannelToMidiOut(hold, 3);
            }
            teeChannelToMidiOut(cc, 3);
        }
        return;
    }

    if (msg.front() == 0xF0 && msg.back() == 0xF7)
    {
        {
            const juce::ScopedLock sl(outSysExLock);
            pendingOutSysEx.push_back(msg);
        }

        juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
        if (! openMidiOutputs.empty())
        {
            // createSysExMessage re-wraps the payload in F0…F7, so hand it the inner bytes.
            const auto m = juce::MidiMessage::createSysExMessage(msg.data() + 1, (int) msg.size() - 2);
            for (auto& [id, out] : openMidiOutputs)
                if (out != nullptr)
                    out->sendMessageNow(m);
        }
    }
}

void YamahaMU100AudioProcessor::teeChannelToMidiOut(const uint8_t* bytes, int n)
{
    if (bytes == nullptr || n < 2 || n > 3 || bytes[0] < 0x80 || bytes[0] >= 0xF0) return;
    {
        const juce::ScopedLock sl(outSysExLock);
        pendingOutSysEx.emplace_back(bytes, bytes + n);
    }

    juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
    if (! openMidiOutputs.empty())
    {
        const juce::MidiMessage m(bytes, n);
        for (auto& [id, out] : openMidiOutputs)
            if (out != nullptr)
                out->sendMessageNow(m);
    }
}

void YamahaMU100AudioProcessor::sendNRPN(int part, uint8_t nrpnMSB, uint8_t nrpnLSB, uint8_t value)
{
    MameLease mame(*this);
    if (! mame) return;

    const int     port   = (part < 16) ? 0 : 1;          // parts 1-16 → A, 17-32 → B
    const uint8_t ch     = static_cast<uint8_t>(part & 0x0F);
    const uint8_t status = static_cast<uint8_t>(0xB0 | ch);

    // NRPN MSB/LSB, Data Entry MSB, then RPN Null so the param stays unarmed.
    const uint8_t bytes[] = {
        status, 0x63, static_cast<uint8_t>(nrpnMSB & 0x7F),
        status, 0x62, static_cast<uint8_t>(nrpnLSB & 0x7F),
        status, 0x06, static_cast<uint8_t>(value   & 0x7F),
        status, 0x65, 0x7F,
        status, 0x64, 0x7F,
    };

    const double timePerByte = 1.0 / (31250.0 / 10.0);
    // Clock-stamped like every other small message-thread send (see smallSendStartTime).
    // Still ONE push for the whole NRPN sequence: an unrelated CC landing between the
    // parameter select and the data entry would re-target the write, and a single
    // reservation is what makes that impossible.
    pushMidiMessageToPort(port, bytes, (int) sizeof(bytes), smallSendStartTime(), timePerByte);
}

// The port and channel a part ACTUALLY receives on, as the hardware's own raw index:
// 0..15 = port A channels 1..16, 16..31 = port B, -1 for Rcv OFF or a byte the spec
// does not define.
//
// `Rcv CHANNEL` IS EDITABLE, so "part n is on channel n" is only the FACTORY default --
// which is exactly what an absent model byte means, and the one case where the shortcut
// happens to be right. Message thread only: it reads `editorState`.
int YamahaMU100AudioProcessor::partRcvChannelRaw(int part) const
{
    if (part < 0 || part >= 32 || editorState == nullptr) return -1;
    static const int chParam = mu100::AddressMap::find("Rcv CHANNEL", "MULTI PART");

    int raw = part;                                  // no stored value = factory channel
    if (chParam >= 0)
    {
        const auto v = mu100::AddressMap::get(*editorState, chParam, part);
        if (v.present) raw = (int) v.combined;
    }
    return (raw >= 0 && raw < 32) ? raw : -1;        // 0x7F = OFF: it hears nothing
}

void YamahaMU100AudioProcessor::sendChannelMessage(int part, uint8_t status, uint8_t d1, int d2)
{
    MameLease mame(*this);
    if (! mame) return;

    // Addressed by the channel the part is LISTENING on, not by its number. This used
    // to be `part & 0x0F` on port `part < 16`, i.e. the factory map -- so re-channelling
    // a part left the keyboard's right-drag audition and the Controller module's wheel
    // transmitting to whichever part still sat on the old channel, or to nothing at all.
    // USER-REPORTED 2026-09-18. `applyIncomingChannelMsg` already resolved the INVERSE
    // through the model and says why in the same words; only the outgoing half assumed.
    //
    // The PORT comes from the same byte, because it is in the same byte: a part may be
    // put on MIDI IN B whatever its number is.
    const int raw = partRcvChannelRaw(part);
    if (raw < 0) return;         // Rcv OFF -- no channel on either port reaches this part
    const int     port = raw / 16;
    const uint8_t ch   = static_cast<uint8_t>(raw % 16);
    const uint8_t st   = static_cast<uint8_t>((status & 0xF0) | ch);

    uint8_t bytes[3];
    size_t n = 0;
    bytes[n++] = st;
    bytes[n++] = static_cast<uint8_t>(d1 & 0x7F);
    if (d2 >= 0) bytes[n++] = static_cast<uint8_t>(d2 & 0x7F);

    // Clock-stamped like every other small message-thread send (see smallSendStartTime).
    // The Controller module's wheel is a continuous drag control that can emit as fast
    // as the mouse reports, so it is exactly the case that must not schedule ahead of
    // the clock: at 3 bytes a message its intrinsic cost to note timing is under 1 ms.
    const double timePerByte = 1.0 / (31250.0 / 10.0);
    pushMidiMessageToPort(port, bytes, (int) n, smallSendStartTime(), timePerByte);

    // CC mode copies the Controller module's wheel to the MIDI OUT as well (see
    // midiOutFormat). Only the wheel's message types: the keyboard's note audition comes
    // through here too, and an audition is not an edit to record.
    if (midiOutFormat == MidiOutCC)
    {
        const uint8_t type = st & 0xF0;
        if (type == 0xB0 || type == 0xE0 || type == 0xD0 || type == 0xA0)
            teeChannelToMidiOut(bytes, (int) n);
    }
}

void YamahaMU100AudioProcessor::sendPanic()
{
    MameLease mame(*this);
    if (! mame) return;

    // PANIC sends exactly what STOPS SOUND, and nothing else — for every channel on
    // both ports:
    //   CC120 All Sound Off  — silences sounding notes immediately and unconditionally,
    //                          irrespective of pedal state.
    //   CC123 All Notes Off  — the conventional companion (acts as if note-offs arrived,
    //                          so notes held by sustain survive it; 120 is the hammer).
    //   CC64  Hold      = 0  — clears a stuck sustain pedal, which is the one thing that
    //   CC66  Sostenuto = 0    would otherwise re-hang the NEXT notes played.
    //
    // DELIBERATELY NOT CC121 (Reset All Controllers), though convention includes it.
    // CC121 stops nothing; it resets performance state — per the spec's own list
    // (sound_list.txt "1.3.2 Reset All Controllers") it flattens pitch bend, channel and
    // polyphonic pressure, modulation, expression AND the MULTI PART portamento switch,
    // none of which can hang a note. Its only real contribution to panic is clearing a
    // stuck pedal, which the two targeted pedal writes above do without collateral.
    //
    // That matters here beyond tidiness: portamento is state the EDITOR owns and mirrors,
    // so a CC121 left the device holding portamento off while the panel still showed it
    // on, recoverable only by toggling PORTA by hand. Not sending it is what keeps panic
    // side-effect-free — no re-assert machinery, and no model/device drift to repair.
    // A full controller reset, if ever wanted, belongs on its own action.
    //
    // Bytes are scheduled with strictly increasing MAME timestamps so they stay ordered.
    // PANIC must not be queued behind a drag's coalesced tail — it exists to stop notes
    // NOW. Flush first, then take a window for the whole 128-message burst.
    flushPendingParamChanges();
    const double timePerByte = 1.0 / (31250.0 / 10.0);
    const double startTime   = reserveSendWindow(2 * 16 * 4 * 3);
    double t = startTime;

    auto cc = [&](int port, uint8_t ch, uint8_t num)
    {
        // Every CC here takes value 0 — the two mode messages require it, and the two
        // pedals are being released.
        const uint8_t msg[3] = { static_cast<uint8_t>(0xB0 | ch), num, 0 };
        pushMidiMessageToPort(port, msg, 3, t, timePerByte);
        t += 3 * timePerByte;
    };

    for (int port = 0; port < 2; ++port)
        for (uint8_t ch = 0; ch < 16; ++ch)
            for (uint8_t num : { (uint8_t) 120, (uint8_t) 123,
                                 (uint8_t) 64,  (uint8_t) 66 })
                cc(port, ch, num);

    // These CCs are OUTGOING and so never pass through observeMidiActivity, which is
    // the only thing that releases a held note. Without this, panicking out of a hung
    // chord left the Keyboard module lit for keys that had stopped sounding.
    clearHeldNotes();
}

//==============================================================================
// Tempo-sync support (plugin-only; see the effect tempo-sync editor feature).
double YamahaMU100AudioProcessor::effectiveBpm() const
{
    // Standalone: the user's TEMPO knob is authoritative (no host transport).
    // Hosted: the DAW project tempo. Guard against a zero/garbage host value.
    const bool standalone = juce::JUCEApplicationBase::isStandaloneApp();
    const double bpm = standalone ? syncTempo.load(std::memory_order_relaxed)
                                  : dawBpm.load(std::memory_order_relaxed);
    return (bpm > 0.0) ? bpm : 120.0;
}

void YamahaMU100AudioProcessor::setSyncTempo(double bpm)
{
    syncTempo.store(juce::jlimit(10.0, 250.0, bpm), std::memory_order_relaxed);
}

bool YamahaMU100AudioProcessor::getEffectSync(int unit, int typeIndex, SyncEntry& out) const
{
    const auto it = effectSync.find(unit * 1000 + typeIndex);
    if (it == effectSync.end()) return false;
    out = it->second;
    return true;
}

void YamahaMU100AudioProcessor::setEffectSync(int unit, int typeIndex, const SyncEntry& e)
{
    const int key = unit * 1000 + typeIndex;
    if (e.on) effectSync[key] = e;
    else      effectSync.erase(key);   // off = no need to store
}

// Key a parameter change by the XG address it writes, so successive drag values for
// one control collapse onto one entry. Layout: F0 43 1n MM aH aM aL <data..> F7.
// Returns false for anything that isn't a well-formed parameter change — those must
// never be coalesced (they are not idempotent absolute writes).
static bool paramChangeKey(const std::vector<uint8_t>& msg, uint32_t& keyOut)
{
    if (msg.size() < 9 || msg.front() != 0xF0 || msg.back() != 0xF7) return false;
    if (msg[1] != 0x43 || (msg[2] & 0xF0) != 0x10) return false;   // Yamaha param change
    keyOut = ((uint32_t) msg[3] << 21) | ((uint32_t) msg[4] << 14)
           | ((uint32_t) msg[5] <<  7) |  (uint32_t) msg[6];
    return true;
}

void YamahaMU100AudioProcessor::sendParamChange(const std::vector<uint8_t>& msg)
{
    if (msg.empty()) return;
    // Anything sent immediately supersedes a pending drag value at the same address,
    // and may be order-sensitive against one at a DIFFERENT address (an effect-type
    // change carries its program's defaults). Draining first makes "pending" strictly
    // older than "now" in every case.
    flushPendingParamChanges();
    // Capture BEFORE the model moves — the old value is only readable until then.
    if (undoSuppressDepth == 0) undoPush(msg);
    const bool sent = sendSysEx(msg);            // transmit to the device
    editorState->load(msg.data(), msg.size());  // record into the working model
    // Device and model now agree at these addresses, so any override here has
    // retired. Covers the editor's own controls AND a DAW automation lane, which
    // reaches the device through this same funnel.
    clearMidiOverridesForMessage(msg);

    // The model is recorded even when the transmit was refused (engine down), because
    // it is what the panels, project state and the .mss save read — dropping the edit
    // there would lose the user's work as well as the write. But that leaves the panel
    // showing a value the device never received, which looks EXACTLY like a lost
    // parameter change and would misdirect the next diagnosis. Say so, once per run.
    if (! sent && ! loggedUnsentParamChange)
    {
        loggedUnsentParamChange = true;
        mamediag::logf("  editor param change NOT transmitted (engine down); model and "
                       "device are now out of step until the next read-back");
    }
}

void YamahaMU100AudioProcessor::sendParamChangeThrottled(const std::vector<uint8_t>& msg)
{
    if (msg.empty()) return;

    // Capture BEFORE the model moves — the old value is only readable until then.
    if (undoSuppressDepth == 0) undoPush(msg);

    // The model is updated on EVERY value change regardless of coalescing: it is what
    // the panels, project state and the .mss read, so it must track the control exactly.
    // Only the transmission is rate-limited.
    editorState->load(msg.data(), msg.size());
    clearMidiOverridesForMessage(msg);   // see the note in sendParamChange

    uint32_t key = 0;
    if (! paramChangeKey(msg, key))
    {
        sendSysEx(msg);                          // not a coalescable param change
        return;
    }

    // Last value at this address wins, keeping the address's original queue position.
    for (auto& [addr, pending] : pendingParamChanges)
        if (addr == key)
        {
            pending = msg;
            if (! paramFlush.isTimerRunning())
                paramFlush.startTimerHz(kParamFlushHz);
            return;
        }

    pendingParamChanges.emplace_back(key, msg);
    if (! paramFlush.isTimerRunning())
        paramFlush.startTimerHz(kParamFlushHz);
}

void YamahaMU100AudioProcessor::flushPendingParamChanges()
{
    if (pendingParamChanges.empty())
    {
        paramFlush.stopTimer();                  // idle: stop ticking until the next edit
        return;
    }

    // Move first: sendSysEx must not observe a half-drained map, and the model was
    // already updated when each entry was queued, so nothing here touches editorState.
    auto batch = std::move(pendingParamChanges);
    pendingParamChanges.clear();
    for (const auto& [addr, msg] : batch)
        sendSysEx(msg);
}

void YamahaMU100AudioProcessor::recordParamChange(const std::vector<uint8_t>& msg)
{
    if (msg.empty()) return;
    editorState->load(msg.data(), msg.size());   // model only; do not transmit
}

//==============================================================================
// Undo / Redo — see the public API block in PluginProcessor.h and EditUndo.h.
//
// Message thread throughout. No lock, no atomic: editorState is a std::map owned by
// this thread, both send funnels run here, and processBlock touches neither.

namespace
{
    // Split a param-change message into the addresses its data bytes write.
    // Layout: F0 43 1n MM aH aM aL <data..> F7, so the payload is msg[7 .. size-2] at
    // consecutive addresses from the one the header names.
    bool undoAddressRange(const std::vector<uint8_t>& msg,
                          mu100::ModelIx& ixOut, uint32_t& addr0Out, size_t& firstOut)
    {
        if (msg.size() < 9 || msg.front() != 0xF0 || msg.back() != 0xF7) return false;
        if (msg[1] != 0x43 || (msg[2] & 0xF0) != 0x10) return false;   // Yamaha param change
        if (! mu100::modelToIx(static_cast<mu100::Model>(msg[3]), ixOut)) return false;
        addr0Out = ((uint32_t) msg[4] << 14) | ((uint32_t) msg[5] << 7) | (uint32_t) msg[6];
        firstOut = 7;
        return true;
    }

    // How many model bytes a param-change message carries: F0 43 1n MM aH aM aL <data..> F7.
    // This is the lockstep that lets Step::bytes stay a flat list rather than a
    // per-message one — undoBuildMessages walks the two together in the same order.
    size_t undoPayloadCount(const std::vector<uint8_t>& m)
    {
        return m.size() >= 9 ? m.size() - 8 : 0;
    }

    // Same model and same start address, i.e. the same parameter on the same instance.
    bool undoSameTarget(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
    {
        return a.size() == b.size() && a.size() >= 9
            && a[3] == b[3] && a[4] == b[4] && a[5] == b[5] && a[6] == b[6];
    }

    // Merging a second kit change into the same step: an address already carried keeps
    // its FIRST oldVal, so the step still reverts to where the gesture began -- the
    // same rule mergeBytes follows for ordinary edits.
    void undoMergeDrumEdits(mu100undo::Step& top,
                            const std::vector<mu100undo::ByteDelta>& add)
    {
        for (const auto& d : add)
        {
            auto it = std::find_if(top.drumEdits.begin(), top.drumEdits.end(),
                                   [&](const mu100undo::ByteDelta& h)
                                   { return h.stateKey == d.stateKey; });
            if (it == top.drumEdits.end()) top.drumEdits.push_back(d);
            else                           it->newVal = d.newVal;
        }
    }

    mu100::ModelIx undoIxOf  (uint32_t stateKey) { return static_cast<mu100::ModelIx>(stateKey >> 24); }
    uint32_t       undoAddrOf(uint32_t stateKey) { return stateKey & 0x1FFFFF; }

    // MU100_UNDO_TRACE=1 — the same env-gated pattern as SWP30_SILDIAG: a
    // function-local static, so no init-order question and no
    // per-call getenv. Nothing runs when it is unset.
    bool undoTraceOn()
    {
        static const bool on = [] {
            const char* e = std::getenv("MU100_UNDO_TRACE");
            return e != nullptr && *e != '\0' && *e != '0';
        }();
        return on;
    }
}

void YamahaMU100AudioProcessor::undoCommit(mu100undo::Step&& step, uint32_t mergeKey)
{
    const double now = juce::Time::getMillisecondCounterHiRes();

    // A mouse release since the last commit ends the open step, whatever the coalesce
    // window says. Consumed here rather than acted on at mouse-up time, because the
    // controls that raise it hold no processor reference (see EditUndo.h).
    if (mu100undo::gSealRequest)
    {
        mu100undo::gSealRequest = false;
        undoSealTop();
    }

    // A fresh edit discards the redo branch. Doing this FIRST matters: the merge test
    // below must look at the step the user can currently undo, not at one they undid
    // past and then edited away from.
    while ((int) undoStack.size() > undoPos)
        undoStack.pop_back();

    // The FIRST write of a KEYED scope, when the step on top came from a scope of the
    // same kind on the same target and is still inside the coalesce window: adopt
    // that step instead of opening a new one, so the rest of this gesture folds into
    // it through the same path everything else in a scope takes. This is what turns a
    // wheel-scroll burst through the effect programs — one full type change per
    // detent, seventeen messages each — into a single undo step.
    if (undoScopeDepth > 0 && ! undoScopeHasStep && undoScopeMergeKey != 0
        && ! undoStack.empty() && undoPos == (int) undoStack.size()
        && mu100undo::shouldMerge(undoStack.back(), undoScopeMergeKey, now))
    {
        // The burst reads as its LAST pick, which is what undoing it reverts.
        undoStack.back().explicitLabel = undoScopeLabel.toStdString();
        undoScopeHasStep = true;
        if (undoTraceOn())
            mamediag::logf("  UNDO adopt key=%08x", undoScopeMergeKey);
        // fall through to the scope branch below
    }

    // Inside a scope, everything after the first write folds into the one step the
    // scope opened — note undoScopeHasStep, not merely "a scope is open": the first
    // write must CREATE the step, or it would be appended to whatever unrelated edit
    // happened to be on top when the scope opened.
    if (undoScopeDepth > 0 && undoScopeHasStep
        && ! undoStack.empty() && undoPos == (int) undoStack.size())
    {
        auto& top = undoStack.back();

        // Re-touching an address ALREADY in the step updates it IN PLACE. Two things
        // depend on this. A scope spanning a DRAG sees the same addresses again on
        // every mouse move, so appending would grow one step without bound; and a
        // scope ADOPTED above rewrites the same addresses its predecessor wrote, so
        // appending would make undo replay every program the wheel passed through.
        // Either way the step stays one message per address touched.
        //
        // It is lockstep-safe: the same parameter always yields a message of the same
        // length at the same address, so substituting it leaves Step::bytes aligned
        // with Step::msgs. `oldVal` is left alone, as everywhere else, so the step
        // still reverts to where the gesture started — which across an adopted run
        // means the state before the FIRST pick, exactly what is wanted.
        //
        // Order is preserved by first touch, and that is what keeps a rebuilt step
        // playable: an effect TYPE (or MULTI EQ TYPE) message is the first thing its
        // scope writes, so it holds position 0 however many gestures fold in, and the
        // parameter defaults that must follow it still do.
        if (step.msgs.size() == 1 && step.bytes.size() == undoPayloadCount(step.msgs[0]))
        {
            size_t bi = 0;
            for (size_t mi = 0; mi < top.msgs.size(); ++mi)
            {
                const size_t n = undoPayloadCount(top.msgs[mi]);
                if (undoSameTarget(top.msgs[mi], step.msgs[0]))
                {
                    for (size_t k = 0; k < n && k < step.bytes.size(); ++k)
                        top.bytes[bi + k].newVal = step.bytes[k].newVal;
                    top.msgs[mi]    = std::move(step.msgs[0]);
                    top.lastTouchMs = now;
                    return;
                }
                bi += n;
            }
        }
        else if (step.msgs.empty() && step.plugin.size() == 1)
        {
            const auto& d = step.plugin.front();
            for (auto& have : top.plugin)
                if (have.field == d.field && have.a == d.a && have.b == d.b && have.c == d.c)
                {
                    have.newVal     = d.newVal;
                    top.lastTouchMs = now;
                    return;
                }
        }

        for (auto& b : step.bytes)   top.bytes.push_back(b);
        for (auto& p : step.plugin)  top.plugin.push_back(p);
        for (auto& m : step.msgs)    top.msgs.push_back(std::move(m));
        for (const int r : step.drumReseed)          // idempotent, so dedupe
            if (std::find(top.drumReseed.begin(), top.drumReseed.end(), r) == top.drumReseed.end())
                top.drumReseed.push_back(r);
        undoMergeDrumEdits(top, step.drumEdits);
        top.lastTouchMs = now;
        return;
    }

    if (! undoStack.empty() && undoPos == (int) undoStack.size()
        && mu100undo::shouldMerge(undoStack.back(), mergeKey, now))
    {
        auto& top = undoStack.back();
        mu100undo::mergeBytes(top, step.bytes);
        if (! step.plugin.empty() && ! top.plugin.empty())
            top.plugin.back().newVal = step.plugin.back().newVal;
        if (! step.msgs.empty() && ! top.msgs.empty())
            top.msgs.back() = std::move(step.msgs.back());
        for (const int r : step.drumReseed)
            if (std::find(top.drumReseed.begin(), top.drumReseed.end(), r) == top.drumReseed.end())
                top.drumReseed.push_back(r);
        undoMergeDrumEdits(top, step.drumEdits);
        top.labelNew    = step.labelNew;
        top.lastTouchMs = now;
        if (undoTraceOn())
            mamediag::logf("  UNDO merge key=%08x new=%d", mergeKey, top.labelNew);
        return;
    }

    step.mergeKey    = mergeKey;
    step.lastTouchMs = now;
    if (undoScopeDepth > 0)
    {
        step.explicitLabel = undoScopeLabel.toStdString();
        // A scoped step absorbs only the NEXT scope of the same kind, never a stray
        // parameter edit — gesture keys live in their own bit of the key space, and
        // 0 means "never merge into this" outright.
        step.mergeKey      = undoScopeMergeKey;
        undoScopeHasStep   = true;
    }
    undoStack.push_back(std::move(step));
    while ((int) undoStack.size() > mu100undo::kDepth)
    {
        undoStack.pop_front();
        ++undoDropped;      // the History window's "is entry 0 the Initial State" test
    }
    undoPos = (int) undoStack.size();

    if (undoTraceOn())
        mamediag::logf("  UNDO push key=%08x depth=%d bytes=%d plugin=%d",
                       mergeKey, (int) undoStack.size(),
                       (int) undoStack.back().bytes.size(),
                       (int) undoStack.back().plugin.size());
}

void YamahaMU100AudioProcessor::undoPush(const std::vector<uint8_t>& msg)
{
    mu100::ModelIx ix {};
    uint32_t addr0 = 0;
    size_t   first = 0;
    if (! undoAddressRange(msg, ix, addr0, first)) return;   // not a param change

    mu100undo::Step step;
    step.msgs.push_back(msg);

    // Old values come from the model, which the caller has NOT yet written — that
    // ordering is the whole reason capture lives at the top of the funnels.
    for (size_t i = first; i + 1 < msg.size(); ++i)
    {
        const uint32_t addr = addr0 + (uint32_t) (i - first);
        uint8_t old = 0;
        const bool present = editorState->getByte(ix, addr, old);
        step.bytes.push_back({ mu100::DeviceState::key(ix, addr),
                               present ? (int) old : -1,
                               (int) (msg[i] & 0x7F) });
    }
    if (step.bytes.empty()) return;

    // Identify the parameter for the tooltip. Done once per STEP, never on a merge.
    const auto parsed = mu100::parseStream(msg.data(), msg.size());
    if (! parsed.empty())
        step.mapIndex = mu100::AddressMap::identify(parsed.front(), step.instance, step.instance2);

    // A combined value for the label, decoded the way AddressMap::get decodes it —
    // the encoding is not cosmetic here. Part DETUNE is two NIBBLES, so packing it
    // 7 bits at a time reported 1935 where the control reads +12.7 Hz. Unidentified
    // and BYTEPAIR params keep the 7-bit packing, which is what BYTEPAIR means.
    const int shift = (step.mapIndex >= 0
                       && mu100::AddressMap::param(step.mapIndex).enc == mu100::Enc::NIBBLE)
                          ? 4 : 7;
    const int mask = (1 << shift) - 1;
    for (const auto& b : step.bytes)
    {
        step.labelOld = (step.labelOld << shift) | (b.oldVal < 0 ? 0 : (b.oldVal & mask));
        step.labelNew = (step.labelNew << shift) | (b.newVal & mask);
    }

    uint32_t key = 0;
    undoCommit(std::move(step), paramChangeKey(msg, key) ? key : 0);
}

void YamahaMU100AudioProcessor::undoPushPlugin(const mu100undo::PluginDelta& d)
{
    if (d.oldVal == d.newVal) return;       // a no-op setter call is not an edit
    mu100undo::Step step;
    step.plugin.push_back(d);
    undoCommit(std::move(step), mu100undo::pluginMergeKey(d.field, d.a, d.b, d.c));
}

// Rebuild a step's messages carrying the OLD (or NEW) byte values.
//
// The messages are CLONES of the ones actually sent, with only the data bytes
// substituted — same framing, same address, same length. Rebuilding them through
// AddressMap::makeParamChange instead would mean re-deriving each param's encoding
// (SINGLE / NIBBLE / BYTEPAIR), which is work with nothing to gain: the byte-level
// delta already knows exactly what to write.
std::vector<std::vector<uint8_t>>
YamahaMU100AudioProcessor::undoBuildMessages(const mu100undo::Step& s, bool useOld)
{
    std::vector<std::vector<uint8_t>> out;
    out.reserve(s.msgs.size());

    size_t bi = 0;
    for (const auto& src : s.msgs)
    {
        mu100::ModelIx ix {};
        uint32_t addr0 = 0;
        size_t   first = 0;
        if (! undoAddressRange(src, ix, addr0, first)) { out.push_back(src); continue; }

        auto msg = src;
        for (size_t i = first; i + 1 < msg.size() && bi < s.bytes.size(); ++i, ++bi)
        {
            const auto& d = s.bytes[bi];
            int v = useOld ? d.oldVal : d.newVal;
            if (v < 0)
            {
                // The model held nothing at this address before the edit. Writing a
                // bare 0 would be a guess with audible consequences, so fall back to
                // the factory default — the value the device powers on with.
                uint8_t def = 0;
                v = factoryDefaults().getByte(ix, addr0 + (uint32_t) (i - first), def)
                        ? (int) def : (int) msg[i];
            }
            msg[i] = (uint8_t) (v & 0x7F);
        }
        out.push_back(std::move(msg));
    }
    return out;
}

void YamahaMU100AudioProcessor::undoApplyPlugin(const mu100undo::PluginDelta& d, bool useOld)
{
    const double v = useOld ? d.oldVal : d.newVal;
    switch (d.field)
    {
        case mu100undo::PluginField::CleanHpf:   setCleanHpfHz(d.a, (float) v);            break;
        case mu100undo::PluginField::SystemHpf:  setSystemHpfHz((float) v);                break;
        case mu100undo::PluginField::PartEqGain: setPartEqGain(d.a, d.b, (int) v);         break;
        case mu100undo::PluginField::PartEqFreq: setPartEqFreq(d.a, d.b, (int) v);         break;
        case mu100undo::PluginField::PartEqQ:    setPartEqQ   (d.a, d.b, (int) v);         break;
        case mu100undo::PluginField::DrumHpf:    setDrumHpf   (d.a, d.b, (float) v);       break;
        case mu100undo::PluginField::DrumEqGain: setDrumEqGain(d.a, d.b, d.c, (int) v);    break;
        case mu100undo::PluginField::DrumEqFreq: setDrumEqFreq(d.a, d.b, d.c, (int) v);    break;
        case mu100undo::PluginField::DrumEqQ:    setDrumEqQ   (d.a, d.b, d.c, (int) v);    break;
    }
}

void YamahaMU100AudioProcessor::undo()
{
    if (! canUndo()) return;
    UndoSuppress guard(*this);                 // nothing below re-enters the stack
    const auto& s = undoStack[(size_t) undoPos - 1];

    // Once, up front. A drag value still sitting in the coalescing queue would
    // otherwise flush AFTER the undo and stomp it. sendParamChange does the same for
    // the same reason; doing it here rather than per message avoids 17 redundant
    // drains on an effect-type step.
    flushPendingParamChanges();

    // Model first, by hand rather than through sendParamChange, so it is correct even
    // when sendSysEx refuses (engine down) — the reasoning at sendParamChange's own
    // unsent-message log.
    for (auto it = s.bytes.rbegin(); it != s.bytes.rend(); ++it)
    {
        int v = it->oldVal;
        if (v < 0)
        {
            uint8_t def = 0;
            v = factoryDefaults().getByte(undoIxOf(it->stateKey), undoAddrOf(it->stateKey), def)
                    ? (int) def : 0;
        }
        editorState->setByte(undoIxOf(it->stateKey), undoAddrOf(it->stateKey), (uint8_t) v);
    }

    for (const auto& m : undoBuildMessages(s, /*useOld=*/true))
        sendSysEx(m);

    for (auto it = s.plugin.rbegin(); it != s.plugin.rend(); ++it)
        undoApplyPlugin(*it, /*useOld=*/true);

    // AFTER the bytes and the messages: the bytes have restored the part's bank and
    // program, so "which kit is on this buffer" now answers with the OLD kit, and the
    // messages have made the device re-initialise to that same kit's defaults. Both
    // halves therefore land on the table's row for it.
    for (const int b : s.drumReseed) reseedDrumBufferFromModel(b);
    applyDrumEdits(s, /*useOld=*/true);    // AFTER the reseed, which would erase them

    // Re-baseline the DAW parameters BEFORE anyone can observe the moved model.
    //
    // Without this the next AutomationParams tick sees up to 322 parameters differing
    // from lastSeen and fires beginChangeGesture + setValueNotifyingHost on each — in
    // Cubase with a lane in Write/Touch, that RECORDS the undo as automation data.
    // resyncFromModel pushes the values with no gesture and re-baselines lastSeen.
    // setStateInformation ends with the identical call for the identical reason.
    if (automation) automation->resyncFromModel();

    editorStateExternalSeq.fetch_add(1, std::memory_order_release);
    --undoPos;

    if (undoTraceOn())
        mamediag::logf("  UNDO apply <- pos=%d/%d", undoPos, (int) undoStack.size());
}

void YamahaMU100AudioProcessor::redo()
{
    if (! canRedo()) return;
    UndoSuppress guard(*this);
    const auto& s = undoStack[(size_t) undoPos];

    flushPendingParamChanges();

    for (const auto& d : s.bytes)
        editorState->setByte(undoIxOf(d.stateKey), undoAddrOf(d.stateKey), (uint8_t) (d.newVal & 0x7F));

    for (const auto& m : undoBuildMessages(s, /*useOld=*/false))
        sendSysEx(m);

    for (const auto& d : s.plugin)
        undoApplyPlugin(d, /*useOld=*/false);

    for (const int b : s.drumReseed) reseedDrumBufferFromModel(b);
    applyDrumEdits(s, /*useOld=*/false);

    if (automation) automation->resyncFromModel();

    editorStateExternalSeq.fetch_add(1, std::memory_order_release);
    ++undoPos;

    if (undoTraceOn())
        mamediag::logf("  UNDO apply -> pos=%d/%d", undoPos, (int) undoStack.size());
}

void YamahaMU100AudioProcessor::undoClear()
{
    // undoDropped is reset even when the stack is already empty: a clear re-baselines
    // the history on whatever patch was just loaded, so the next entry 0 IS an initial
    // state again. Cheap, and leaving it stale would mislabel the window forever.
    undoDropped = 0;
    if (undoStack.empty()) return;
    if (undoTraceOn())
        mamediag::logf("  UNDO clear (%d steps discarded)", (int) undoStack.size());
    undoStack.clear();
    undoPos = 0;
}

void YamahaMU100AudioProcessor::undoGoTo(int pos)
{
    pos = juce::jlimit(0, (int) undoStack.size(), pos);
    while (undoPos > pos) undo();
    while (undoPos < pos) redo();

    // SEAL the step we landed on. Without this, an edit made within kCoalesceMs of the
    // jump, to the address that step already holds, would MERGE INTO IT — silently
    // rewriting the entry the user just clicked instead of appending a new one below
    // it. The arrows have the same hazard and get away with it because a click on them
    // is a mouse-up on a control that raises gSealRequest; a click on a History row is
    // not, so it has to say so explicitly.
    undoSealTop();
}

juce::String YamahaMU100AudioProcessor::undoStepLabel(int index) const
{
    if (index < 0 || index >= (int) undoStack.size()) return {};
    // forward=false reads old -> new, i.e. what the edit DID. undoLabel()'s forward=true
    // reads the other way round because it describes what pressing Undo will do.
    return undoDescribe(undoStack[(size_t) index], /*forward=*/false);
}

void YamahaMU100AudioProcessor::undoSealTop()
{
    if (! undoStack.empty() && undoPos == (int) undoStack.size())
        undoStack.back().sealed = true;
}

void YamahaMU100AudioProcessor::undoSnapshotDrumBuffer(int buffer,
                                                       std::vector<mu100undo::PluginDelta>& into)
{
    if (buffer < 0 || buffer >= 4) return;
    using PF = mu100undo::PluginField;
    for (int key = 0; key < 128; ++key)
    {
        // Sparse: only what differs from neutral, so the common case costs nothing.
        const float hpf = getDrumHpf(buffer, key);
        if (hpf != 0.0f)
            into.push_back({ PF::DrumHpf, buffer, key, 0, hpf, 0.0 });
        for (int b = 0; b < 3; ++b)
        {
            const int g = getDrumEqGain(buffer, key, b);
            const int f = getDrumEqFreq(buffer, key, b);
            const int q = getDrumEqQ   (buffer, key, b);
            if (g != 64)                       into.push_back({ PF::DrumEqGain, buffer, key, b, (double) g, 64.0 });
            if (f != kPartEqFreqDefault[b])    into.push_back({ PF::DrumEqFreq, buffer, key, b, (double) f, (double) kPartEqFreqDefault[b] });
            if (q != 7)                        into.push_back({ PF::DrumEqQ,    buffer, key, b, (double) q, 7.0 });
        }
    }
}

// Describe what the top step changed, for the button tooltips. Formatted on demand
// (the editor reads it at ~4 Hz), never during capture — a merge fires on every
// onChange of a drag, and building a String 200 times a second is the one avoidable
// cost in this feature.
// Values are shown the way the control shows them ("L24", "+2.3 dB", "ON", "1.0k"),
// not as the raw XG byte. Two independent formatters cover the two halves of the
// parameter space, and both are the SAME ones the panels use:
//
//   * mu100::display::format(), driven by the Disp rule the address-map generator
//     compiles out of the manual's own Description column. Because it is generated,
//     it cannot drift from the address it describes, and re-running the pipeline
//     updates the tooltips for free — the reason this is not a hand-written list of
//     parameter names in C++.
//   * xgfmt::formatValue() for effect parameters, whose meaning depends on the
//     effect TYPE currently loaded and so cannot live in a static per-address table
//     at all. The type is read back out of editorState here.
//
// Anything neither covers still prints the raw byte. That is the honest fallback:
// a plausible-looking wrong unit would be worse than a bare number.

// Resolve an effect parameter-change address to (layout, slot) using the effect type
// the model currently holds for that unit. Returns false when the address is not an
// effect parameter slot (a type change, a return/pan control, another block).
bool YamahaMU100AudioProcessor::undoEffectSlot(const std::vector<uint8_t>& msg,
                                               int& layoutOut, int& slotOut, bool& isVarOut) const
{
    if (msg.size() < 9 || msg[3] != 0x4C) return false;
    const uint8_t aH = msg[4], aM = msg[5], aL = msg[6];

    int unit = -1;
    if (aH == 0x02 && aM == 0x01)
        unit = (aL < 0x20) ? mu100fx::FX_REVERB
             : (aL < 0x40) ? mu100fx::FX_CHORUS
                           : mu100fx::FX_VARIATION;
    else if (aH == 0x03 && aM == 0x00) unit = mu100fx::FX_INSERTION1;
    else if (aH == 0x03 && aM == 0x01) unit = mu100fx::FX_INSERTION2;
    if (unit < 0) return false;

    // The unit's currently-loaded program decides what its 16 slots MEAN.
    uint8_t tMsb = 0, tLsb = 0;
    const uint32_t typeAddr = ((uint32_t) aH << 14) | ((uint32_t) aM << 7) | fxaddr::typeLowMsb(unit);
    if (! editorState->getByte(mu100::ModelIx::XG, typeAddr,     tMsb)) return false;
    if (! editorState->getByte(mu100::ModelIx::XG, typeAddr + 1, tLsb)) return false;

    int count = 0;
    const auto* types = mu100fx::unitTypes(unit, count);
    const mu100fx::EffectType* found = nullptr;
    for (int i = 0; i < count; ++i)
        if (types[i].msb == tMsb && types[i].lsb == tLsb) { found = &types[i]; break; }
    if (found == nullptr) return false;

    const bool insMsb = fxaddr::insertionRequiresMsb(unit, *found);
    for (int slot = 0; slot < 16; ++slot)
        if (fxaddr::paramLow(unit, slot, insMsb) == (int) aL)
        {
            layoutOut = (int) found->layout;
            slotOut   = slot;
            isVarOut  = (unit == mu100fx::FX_VARIATION);
            return true;
        }
    return false;
}

// Name the drum key an undo step touched: "Drum COWBELL " rather than
// "Drum buf 1 key 71 ". A buffer/key pair is what the RECORD carries (they are what
// address the value), but they are not what the user chose — the DRUM KEY box shows
// the instrument, so that is what the tooltip has to say.
//
// The kit is on the PART, not on the buffer, so this finds a part assigned to the
// buffer and reads its bank/program out of the model. No panel is involved, which is
// the point: a tooltip is composed with the GUI possibly closed, and `partDrumBuffer`
// lives in the processor for the same reason.
//
// Falls back to the numeric form rather than guessing whenever the chain breaks — no
// part on the buffer (the map is only pushed once the editor has opened), an unknown
// kit, or a note that carries no drum in it.
juce::String YamahaMU100AudioProcessor::undoDrumWhere(int buffer, int key) const
{
    using mu100::AddressMap;
    const juce::String numeric = "Drum buf " + juce::String(buffer + 1)
                               + " key " + juce::String(key) + " ";
    if (buffer < 0 || buffer >= 4) return numeric;

    int part = -1;
    for (int p = 0; p < 32; ++p)
        if (partDrumBuffer[p] == buffer) { part = p; break; }
    if (part < 0) return numeric;

    const int msbIdx = AddressMap::find("BANK SELECT MSB", "MULTI PART");
    const int lsbIdx = AddressMap::find("BANK SELECT LSB", "MULTI PART");
    const int prgIdx = AddressMap::find("PROGRAM NUMBER",  "MULTI PART");
    if (msbIdx < 0 || lsbIdx < 0 || prgIdx < 0) return numeric;

    auto partByte = [&](int mapIndex) -> int
    {
        const auto& def = AddressMap::param(mapIndex);
        mu100::ModelIx model = mu100::ModelIx::XG;
        uint32_t addr21 = 0;
        AddressMap::resolve(def, part, model, addr21);
        uint8_t raw = 0;
        return editorState->getByte(model, addr21, raw) ? (int) raw : 0;
    };

    // drumVoiceName takes a 1-BASED program; the model byte is 0-based.
    const char* n = mu100::drumVoiceName(partByte(msbIdx), partByte(lsbIdx),
                                         partByte(prgIdx) + 1, key);
    if (n == nullptr || *n == '\0') return numeric;
    return "Drum " + juce::String(n).toUpperCase() + " ";
}

juce::String YamahaMU100AudioProcessor::undoDescribe(const mu100undo::Step& s, bool forward) const
{
    if (! s.explicitLabel.empty())
        return juce::String(s.explicitLabel);

    if (s.mapIndex < 0 && s.msgs.empty())
    {
        if (! s.plugin.empty())
        {
            const auto& d = s.plugin.front();
            const bool drum = d.field >= mu100undo::PluginField::DrumHpf;
            const int  band = (drum ? d.c : d.b);
            juce::String what;
            auto fmt = [&](double v) -> juce::String
            {
                switch (d.field)
                {
                    case mu100undo::PluginField::CleanHpf:
                    case mu100undo::PluginField::SystemHpf:
                    case mu100undo::PluginField::DrumHpf:
                        return v < freqladder::kMin ? juce::String("Off") : freqladder::text(v);
                    case mu100undo::PluginField::PartEqGain:
                    case mu100undo::PluginField::DrumEqGain:
                        return juce::String((v - 64.0) * (12.0 / 64.0), 1) + "dB";
                    case mu100undo::PluginField::PartEqFreq:
                    case mu100undo::PluginField::DrumEqFreq:
                        return freqladder::text(v);
                    default:
                        return juce::String(v / 10.0, 1);
                }
            };
            switch (d.field)
            {
                // The system HPF has no part, so it names itself and returns early --
                // the "Part n " / drum-key prefix below would be a lie about its scope.
                case mu100undo::PluginField::SystemHpf:
                    return juce::String("System HPF ") + fmt(forward ? d.newVal : d.oldVal)
                         + " -> " + fmt(forward ? d.oldVal : d.newVal);
                case mu100undo::PluginField::CleanHpf:
                case mu100undo::PluginField::DrumHpf:    what = "HPF2";                        break;
                case mu100undo::PluginField::PartEqGain:
                case mu100undo::PluginField::DrumEqGain: what = "EQ " + juce::String(band + 2) + " GAIN"; break;
                case mu100undo::PluginField::PartEqFreq:
                case mu100undo::PluginField::DrumEqFreq: what = "EQ " + juce::String(band + 2) + " FREQ"; break;
                default:                                 what = "EQ " + juce::String(band + 2) + " Q";    break;
            }
            const juce::String where = drum ? undoDrumWhere(d.a, d.b)
                                            : ("Part " + juce::String(d.a + 1) + " ");
            return where + what + " " + fmt(forward ? d.newVal : d.oldVal)
                 + " -> " + fmt(forward ? d.oldVal : d.newVal);
        }
        return "parameter change";
    }

    const int from = forward ? s.labelNew : s.labelOld;
    const int to   = forward ? s.labelOld : s.labelNew;

    // Effect parameters first: their meaning is type-dependent, so the address map's
    // static rule cannot describe them and deliberately says nothing.
    int layout = 0, slot = 0; bool isVar = false;
    if (! s.msgs.empty() && undoEffectSlot(s.msgs.front(), layout, slot, isVar))
    {
        const auto& ly = mu100fx::kLayouts[layout];
        const char* lbl = ly.slots[slot].label;
        juce::String out;
        out << (lbl != nullptr ? lbl : "PARAM") << " "
            << xgfmt::formatValue(layout, slot, (double) from, isVar) << " -> "
            << xgfmt::formatValue(layout, slot, (double) to,   isVar);
        return out;
    }

    if (s.mapIndex < 0) return "parameter change";

    const auto& p = mu100::AddressMap::param(s.mapIndex);
    juce::String out;
    // A templated MID address byte is what makes a parameter per-part; DRUM SETUP
    // additionally templates the HIGH byte with its buffer number.
    if (p.hi.mask != 0)
        out << undoDrumWhere(s.instance2, s.instance);
    else if (p.mid.mask != 0)
        out << "Part " << (s.instance + 1) << " ";

    out << mu100::AddressMap::name(p) << " "
        << juce::String(mu100::display::format(p, from)) << " -> "
        << juce::String(mu100::display::format(p, to));
    return out;
}

juce::String YamahaMU100AudioProcessor::undoLabel() const
{
    if (! canUndo()) return {};
    return undoDescribe(undoStack[(size_t) undoPos - 1], /*forward=*/true);
}

juce::String YamahaMU100AudioProcessor::redoLabel() const
{
    if (! canRedo()) return {};
    return undoDescribe(undoStack[(size_t) undoPos], /*forward=*/false);
}

void YamahaMU100AudioProcessor::undoGestureBegin(const juce::String& label, uint32_t mergeKey)
{
    if (undoScopeDepth++ == 0)
    {
        undoScopeLabel    = label;
        undoScopeMergeKey = mergeKey;
        undoScopeHasStep  = false;
    }
}

void YamahaMU100AudioProcessor::undoGestureEnd()
{
    if (undoScopeDepth <= 0) return;      // unbalanced end (a cancelled drag): no-op
    if (--undoScopeDepth == 0)
    {
        undoScopeLabel.clear();
        // A keyed gesture is deliberately left UNSEALED: sealing is exactly what stops
        // the next gesture of the same kind folding in, and folding is the point of
        // the key. What ends such a run is the coalesce window, or a mouse release
        // (XgSelector raises the seal flag when an item is PICKED from its pop-up, so
        // two deliberate clicks stay two steps while a wheel burst becomes one).
        if (undoScopeMergeKey == 0)
            undoSealTop();
        undoScopeMergeKey = 0;
    }
}

void YamahaMU100AudioProcessor::loadEditorState(const uint8_t* data, size_t len)
{
    undoClear();                    // the history's baseline is being replaced — see undoClear
    hardwarePanelEdited = false;    // the model is authoritative again — see the flag
    clearAllMidiOverrides();        // the ground the shadow measured against is gone
    editorState->clear();
    editorState->load(data, len);
}

void YamahaMU100AudioProcessor::appendEditorState(const uint8_t* data, size_t len)
{
    undoClear();
    hardwarePanelEdited = false;    // a read-back IS the resync the warning asks for
    clearAllMidiOverrides();        // the ground the shadow measured against is gone
    editorState->load(data, len);   // fold in without clearing (multi-burst read-back)
}

const mu100::DeviceState& YamahaMU100AudioProcessor::getEditorState() const
{
    return *editorState;
}

const mu100::DeviceState& YamahaMU100AudioProcessor::factoryDefaults()
{
    // Built once from the embedded raw byte array (MU100 power-on state, no Performance).
    static const mu100::DeviceState defaults = []
    {
        mu100::DeviceState st;
        st.load(mu100::kFactoryDefault, mu100::kFactoryDefaultLen);
        return st;
    }();
    return defaults;
}

void YamahaMU100AudioProcessor::seedEditorDefaults()
{
    // Clear + load the complete factory defaults (same clear+load path a read-back
    // uses), so the model is complete without any device round-trip.
    undoClear();
    hardwarePanelEdited = false;    // File > New replaces both halves — see the flag
    clearAllMidiOverrides();        // the ground the shadow measured against is gone
    editorState->clear();
    editorState->load(mu100::kFactoryDefault, mu100::kFactoryDefaultLen);

    // Reset the loaded-file marker to "untitled, clean": no file is associated and the
    // model matches its (factory-default) baseline, so no unsaved-changes '*' shows.
    // (loadEditorFile re-seeds here first, then the editor calls markStateSyncedToFile
    // with the real path; a restored project sets editorStatePopulated and never reaches
    // this, keeping its persisted filename.)
    loadedFilePath.clear();
    openFileType = OpenFileType::None;
    savedStateSig = editorStateSignature();
}

juce::String YamahaMU100AudioProcessor::editorStateSignature() const
{
    if (editorState == nullptr || editorState->size() == 0)
        return {};
    const mu100::Bytes full = editorState->toBulkDumps();
    if (full.empty())
        return {};
    // 64-bit FNV-1a over the canonical bulk-dump bytes. Not cryptographic — just a
    // compact change signature for the unsaved-changes marker (juce::MD5 lives in the
    // juce_cryptography module, which this plugin deliberately doesn't pull in).
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : full) { h ^= b; h *= 1099511628211ULL; }
    return juce::String::toHexString((juce::int64) h);
}

void YamahaMU100AudioProcessor::markStateSyncedToFile(const juce::String& path)
{
    loadedFilePath = path;
    savedStateSig  = editorStateSignature();
    openFileType   = openFileTypeForPath(path);
}

void YamahaMU100AudioProcessor::loadEditorFile(const uint8_t* data, size_t len)
{
    seedEditorDefaults();           // complete factory defaults first
    loadModelLikeDevice(data, len); // then the file overrides the params it carries
    healEffectParamsOutOfRange();
    resetPluginOnlyState();         // clear plugin-only features the file can't carry
}

//==============================================================================
// Effect TYPE bookkeeping the device does on its own. Writing a unit's TYPE makes the
// firmware load that type's parameter defaults; nothing on the wire says so, so any
// path that folds SysEx into the model has to do it too or the model keeps the OLD
// type's parameters under the new type's name. See loadModelLikeDevice in the header.
namespace
{
    uint32_t fxAddr21(int unit, int low)
    {
        const auto b = fxaddr::block(unit);
        return ((uint32_t) b.aH << 14) | ((uint32_t) b.aM << 7) | (uint32_t) low;
    }

    const mu100fx::EffectType* modelEffectType(const mu100::DeviceState& st, int unit)
    {
        uint8_t msb, lsb;
        const uint32_t a = fxAddr21(unit, fxaddr::typeLowMsb(unit));
        if (! st.getByte(mu100::ModelIx::XG, a, msb) || ! st.getByte(mu100::ModelIx::XG, a + 1, lsb))
            return nullptr;
        int count = 0;
        const auto* types = mu100fx::unitTypes(unit, count);
        for (int i = 0; i < count; ++i)
            if (types[i].msb == msb && types[i].lsb == lsb)
                return &types[i];
        return nullptr;
    }

    // Whether `unit`'s TYPE word overlaps XG addresses [a0, a1). One bulk dump of the
    // 02 01 block carries the reverb, chorus AND variation types, so callers ask per unit.
    bool effectTypeIn(int unit, uint32_t a0, uint32_t a1)
    {
        const uint32_t t = fxAddr21(unit, fxaddr::typeLowMsb(unit));
        return t < a1 && t + 2 > a0;
    }

    // The parameter writes the device makes for itself when `unit` becomes type `t`:
    // every used slot's default -- the same set EffectParametersPanel sends on a pick.
    std::vector<mu100::Bytes> effectTypeDefaultMsgs(int unit, const mu100fx::EffectType& t)
    {
        std::vector<mu100::Bytes> out;
        const auto& ly = mu100fx::kLayouts[t.layout];
        const bool isVar  = (unit == mu100fx::FX_VARIATION);
        const bool insMsb = fxaddr::insertionRequiresMsb(unit, t);
        for (int slot = 0; slot < 16; ++slot)
            if (ly.slots[slot].label != nullptr && xgfmt::slotActive(t.layout, slot, isVar))
                out.push_back(fxaddr::setParam(unit, slot, t.deflt[slot], insMsb));
        return out;
    }

    // The same slots, carrying the MODEL's values: what has to follow a TYPE write for
    // the device to end up holding the model's parameters rather than the defaults.
    std::vector<mu100::Bytes> effectUnitModelMsgs(const mu100::DeviceState& st, int unit)
    {
        std::vector<mu100::Bytes> out;
        const auto* t = modelEffectType(st, unit);
        if (t == nullptr) return out;
        const auto& ly = mu100fx::kLayouts[t->layout];
        const bool isVar  = (unit == mu100fx::FX_VARIATION);
        const bool insMsb = fxaddr::insertionRequiresMsb(unit, *t);
        for (int slot = 0; slot < 16; ++slot)
        {
            if (ly.slots[slot].label == nullptr || ! xgfmt::slotActive(t->layout, slot, isVar))
                continue;
            const int lo = fxaddr::paramLow(unit, slot, insMsb);
            if (lo < 0) continue;
            const uint32_t a = fxAddr21(unit, lo);
            uint8_t b0 = 0, b1 = 0;
            if (! st.getByte(mu100::ModelIx::XG, a, b0)) continue;
            int v = b0;
            if (fxaddr::slotIsPair(unit, slot, insMsb))
            {
                if (! st.getByte(mu100::ModelIx::XG, a + 1, b1)) continue;
                v = (b0 << 7) | b1;
            }
            out.push_back(fxaddr::setParam(unit, slot, v, insMsb));
        }
        return out;
    }
}

void YamahaMU100AudioProcessor::loadModelLikeDevice(const uint8_t* data, size_t len)
{
    for (const auto& m : mu100::parseStream(data, len))
    {
        const bool write = (m.kind == mu100::Kind::ParamChange || m.kind == mu100::Kind::BulkDump)
                        && m.model == mu100::Model::XG;
        const uint32_t a0 = m.address(), a1 = a0 + (uint32_t) m.data.size();
        const mu100fx::EffectType* before[mu100fx::FX_UNIT_COUNT] = {};
        for (int u = 0; write && u < mu100fx::FX_UNIT_COUNT; ++u)
            if (effectTypeIn(u, a0, a1))
                before[u] = modelEffectType(*editorState, u);

        editorState->load(m.raw.data(), m.raw.size());
        if (! write) continue;

        bool reseeded = false;
        for (int u = 0; u < mu100fx::FX_UNIT_COUNT; ++u)
        {
            if (! effectTypeIn(u, a0, a1)) continue;
            const auto* after = modelEffectType(*editorState, u);
            if (after == nullptr || after == before[u]) continue;
            for (const auto& d : effectTypeDefaultMsgs(u, *after))
                editorState->load(d);
            reseeded = true;
        }
        if (reseeded)
            editorState->load(m.raw.data(), m.raw.size());   // the message's own params win
    }
}

int YamahaMU100AudioProcessor::healEffectParamsOutOfRange()
{
    static const char* const kUnit[] = { "REVERB", "CHORUS", "VARIATION", "INSERTION1", "INSERTION2" };
    int healed = 0;
    for (int u = 0; u < mu100fx::FX_UNIT_COUNT; ++u)
    {
        const auto* t = modelEffectType(*editorState, u);
        if (t == nullptr) continue;
        const auto& ly = mu100fx::kLayouts[t->layout];
        const bool isVar  = (u == mu100fx::FX_VARIATION);
        const bool insMsb = fxaddr::insertionRequiresMsb(u, *t);

        // D/W exists only where the unit is an INSERTION (Insertion 1/2, or the
        // Variation with CON = INS); elsewhere the device keeps 0 there and refuses any
        // other value, so "healing" it would CREATE a model/device difference. Same
        // test EffectParametersPanel::dryWetActive uses to hide the knob.
        bool dryWetLive = (u == mu100fx::FX_INSERTION1 || u == mu100fx::FX_INSERTION2);
        if (isVar)
        {
            uint8_t con = 0;   // absent = INS, as the panel defaults it
            editorState->getByte(mu100::ModelIx::XG,
                                 fxAddr21(u, fxaddr::controls(u).connection), con);
            dryWetLive = (con == 0);
        }

        for (int slot = 0; slot < 16; ++slot)
        {
            if (ly.slots[slot].label == nullptr || ! xgfmt::slotActive(t->layout, slot, isVar))
                continue;
            if (ly.slots[slot].disp == 4 && ! dryWetLive)   // format 4 = D/W
                continue;
            const auto rg = xgfmt::rangeFor(t->layout, slot, isVar);
            if (rg.hi <= rg.lo) continue;           // no spec range to judge against
            const int lo = fxaddr::paramLow(u, slot, insMsb);
            if (lo < 0) continue;
            const uint32_t a = fxAddr21(u, lo);
            uint8_t b0 = 0, b1 = 0;
            if (! editorState->getByte(mu100::ModelIx::XG, a, b0)) continue;
            int v = b0;
            if (fxaddr::slotIsPair(u, slot, insMsb))
            {
                if (! editorState->getByte(mu100::ModelIx::XG, a + 1, b1)) continue;
                v = (b0 << 7) | b1;
            }
            if (v >= rg.lo && v <= rg.hi) continue;
            editorState->load(fxaddr::setParam(u, slot, t->deflt[slot], insMsb));
            mamediag::logf("  heal: %s %s P%d %s = %d is outside %d..%d -> default %d",
                           kUnit[u], t->name, slot + 1, ly.slots[slot].label, v,
                           rg.lo, rg.hi, (int) t->deflt[slot]);
            ++healed;
        }
    }
    return healed;
}

// A .syx carries only device (XG) state. The plugin-only features it cannot represent —
// effect tempo-SYNC, the per-part clean HPF2, and the mid-band / per-drum-key peaking EQ —
// are "not addressed by the file", so loading a patch (which replaces the whole sound)
// resets them to their defaults rather than carrying the previous session's values onto
// the new patch. The dirty flags re-push the neutralised values to the SWP30 on the MAME
// thread; the editor's applyDeviceState() (called right after a file load) refreshes the
// panels so HPF2/EQ read their defaults and every SYNC toggle shows off.
void YamahaMU100AudioProcessor::resetPluginOnlyState()
{
    for (int i = 0; i < 32; ++i) cleanHpfHz[i] = 0.0f;   // HPF2 off
    systemHpfHz = 0.0f;                                  // ...and the system floor with them
    cleanHpfDirty.store(true, std::memory_order_release);

    initPartEqDefaults();                                // mid-band EQ flat
    partEqDirty.store(true, std::memory_order_release);

    initDrumFxDefaults();                                // per-drum-key EQ/HPF flat + map reset
    drumFxDirty.store(true, std::memory_order_release);

    effectSync.clear();   // all effect tempo-SYNC off; synced knobs revert to the file's raw ms/Hz

    // ...and release every EQ bypass. This function runs where the EQ VALUES are being
    // replaced wholesale -- File > New and a file load -- and a bypass that outlived
    // them would be auditioning an EQ that no longer exists, with a lit lamp on a
    // brand-new file. It is deliberately NOT reset on a project restore (which does not
    // call this): the mask never travels in project state, so a reopened project starts
    // clear anyway.
    //
    // Ordering note for File > New: the caller seeds the factory defaults FIRST, so the
    // shelf restore this triggers finds every gain already at 0 dB and sends nothing.
    setPartEqBypassMask(0);
}

bool YamahaMU100AudioProcessor::forceRestorePatchPush()
{
    static const bool on = [] { const char* e = std::getenv("MU100_FORCE_PATCH_PUSH");
                                return e != nullptr && *e != '\0' && *e != '0'; }();
    return on;
}

// Fallback for a restored project whose device did NOT come up holding the patch.
// See the restoredModelNeedsPush block in PluginProcessor.h for why this exists.
// Message thread (a juce::Timer callback), which is where sendPatchToDevice belongs.
void YamahaMU100AudioProcessor::serviceRestorePatchPush()
{
    if (! restoredModelNeedsPush.load(std::memory_order_acquire))
        return;

    if (restorePushDeadlineMs == 0)                     // first poll after being armed
        restorePushDeadlineMs = juce::Time::getMillisecondCounter() + 60000;

    // Wait for the boot XG-reset, not merely for a booted machine: that reset wipes
    // whatever preceded it, which is the same race the editor's file-load path defers on.
    if (! isReadyForEditing())
    {
        // The engine can fail to come up at all (a swallowed fault leaves mameFaultCode
        // set and nothing else happens). Disarm rather than poll for the whole session.
        if (juce::Time::getMillisecondCounter() > restorePushDeadlineMs)
        {
            restoredModelNeedsPush.store(false, std::memory_order_release);
            mamediag::logf("  restore push: engine never became ready; patch NOT pushed");
        }
        return;
    }

    restoredModelNeedsPush.store(false, std::memory_order_release);

    const bool healed = restoredModelHealed.exchange(false, std::memory_order_acq_rel);
    if (workRamAppliedAtBoot.load(std::memory_order_acquire) && ! forceRestorePatchPush()
        && ! healed)
        return;                     // the snapshot restored the device; nothing to do

    const int ms = sendPatchToDevice();
    mamediag::logf("  restore push: work RAM did not carry the patch%s -- sent it "
                   "to the device (%d ms of stream)",
                   forceRestorePatchPush() ? " (FORCED by MU100_FORCE_PATCH_PUSH)"
                   : healed ? " (the restored model was HEALED; the snapshot is stale)" : "", ms);
}

int YamahaMU100AudioProcessor::sendPatchToDevice()
{
    MameLease mame(*this);
    if (! mame) return 0;
    using mu100::AddressMap;

    const auto&  defs        = factoryDefaults();
    const double startTime   = senderMameTime() + 0.1;
    const double timePerByte = 1.0 / (31250.0 / 10.0);
    constexpr double kMsgGap = 0.003;
    double t = startTime;

    // Stream one complete message's bytes to MIDI IN A (port 0), then a small gap so
    // the firmware processes each message before the next arrives.
    auto stream = [&](const uint8_t* d, size_t n)
    {
        pushMidiMessageToPort(0, d, (int) n, t, timePerByte);
        t += n * timePerByte + kMsgGap;
    };
    // Split a parameter-change blob into its individual F0..F7 messages and stream each.
    auto streamSysEx = [&](const mu100::Bytes& s)
    {
        size_t msgStart = 0; bool inSysex = false;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == 0xF0) { inSysex = true; msgStart = i; }
            else if (s[i] == 0xF7 && inSysex)
            {
                inSysex = false;
                stream(s.data() + msgStart, i - msgStart + 1);
            }
        }
    };

    const int bankMsbP = AddressMap::find("BANK SELECT MSB", "MULTI PART");
    const int bankLsbP = AddressMap::find("BANK SELECT LSB", "MULTI PART");
    const int progP    = AddressMap::find("PROGRAM NUMBER",  "MULTI PART");

    // 1) Reset the device (XG System On), then set every non-default parameter via a
    //    SysEx parameter-change stream — EXCEPT two families held back for later:
    //    the MULTI PART voice bytes (bank MSB/LSB + program), sent in step 2 so the
    //    program write actually triggers the voice load, and the DRUM SETUP blocks
    //    (3n rr xx), sent in step 4 because those very program writes destroy them.
    //    Until 2026-09-24 the diff went out as one parameter change per contiguous RUN,
    //    and the device applies only the first parameter of such a message: a part with
    //    CHORUS, REVERB and VARIATION SEND all off their defaults kept CHORUS and lost
    //    the other two. The loss is then CARRIED FOREVER by a DAW project, because the
    //    work-RAM snapshot saves the device as it is while the model keeps showing the
    //    value the device never got -- user-reported as a VAR knob that "jumps" on the
    //    first nudge after reopening a project. See the two notes below the diff.
    mu100::DeviceState diff;
    for (const auto& kv : editorState->raw())
    {
        const auto     ix   = static_cast<mu100::ModelIx>((kv.first >> 24) & 0xF);
        const uint32_t addr = kv.first & 0x1FFFFF;
        if (ix == mu100::ModelIx::XG)
        {
            const uint8_t aH = (addr >> 14) & 0x7F, aL = addr & 0x7F;
            if (aH == 0x08 && (aL == 0x01 || aL == 0x02 || aL == 0x03))
                continue;   // voice byte — sent as a separate write in step 2
            if (aH >= 0x30 && aH <= 0x33)
                continue;   // DRUM SETUP buffer — sent whole in step 4
        }
        uint8_t dv;
        if (! (defs.getByte(ix, addr, dv) && dv == kv.second))
            diff.setByte(ix, addr, kv.second);
    }
    // ONE PARAMETER CHANGE PER PARAMETER, framed by the address map. A diff is taken
    // byte by byte, but the device applies a parameter only WHOLE and only as its own
    // message, so each byte is widened to the parameter that owns it and sent in that
    // parameter's own size -- the framing applyPartBlockUndoable already uses. Measured
    // with MU100_DIAG_PATCHPUSH, the two ways this used to go wrong:
    //  * runs as parameter changes (what this was): only the first byte of each run
    //    lands, so a part's REV + VAR SEND pair kept REV and dropped VAR;
    //  * a byte alone: an effect TYPE whose LSB equals the default went out as half a
    //    type word, and the device ignored it (CROSS DELAY stayed DELAY LCR).
    // A byte no parameter covers still goes out alone. Missing bytes of a multi-byte
    // parameter come from the factory defaults, the same baseline the diff was taken on.
    //
    std::map<uint32_t, uint8_t> rebuildTypeAt;   // type addr21 -> neutral type MSB
    static constexpr double kTypeRebuildSettle = 0.2;   // s, see sendParamAt
    // An effect unit whose TYPE changed goes out WHOLE. The diff's baseline is the
    // FACTORY patch, whose effect parameters are the defaults of the FACTORY types --
    // but writing a new TYPE makes the device load THAT type's defaults, so a parameter
    // that happens to equal the factory type's default is exactly one the device no
    // longer holds. Measured: a reverb type change left every such parameter at the new
    // type's default, and the same for the Multi EQ bands after an EQ TYPE change.
    {
        // `neutral` = a type to switch through first (see "REBUILT" below); 0xFF = the
        // EQ, which has no stale-coefficient problem measured and is left as it was.
        struct Unit { uint8_t h, m, lo, hi; const char* type; uint8_t neutral; };
        static const Unit kUnits[] = {
            { 0x02, 0x01, 0x00, 0x1F, "REVERB TYPE",            0x00 },   // NO EFFECT
            { 0x02, 0x01, 0x20, 0x3F, "CHORUS TYPE",            0x00 },
            { 0x02, 0x01, 0x40, 0x7F, "VARIATION TYPE",         0x00 },
            { 0x02, 0x40, 0x00, 0x7F, "EQ TYPE",                0xFF },
            { 0x03, 0x00, 0x00, 0x7F, "INSERTION EFFECT1 TYPE", 0x40 },   // THRU
            { 0x03, 0x01, 0x00, 0x7F, "INSERTION EFFECT2 TYPE", 0x40 } };
        for (const auto& u : kUnits)
        {
            const uint32_t base = ((uint32_t) u.h << 14) | ((uint32_t) u.m << 7);
            uint8_t v;
            const int ti = AddressMap::find(u.type);
            if (ti < 0) continue;
            bool typeChanged = false;
            for (int k = 0; k < AddressMap::param(ti).size && ! typeChanged; ++k)
                typeChanged = diff.getByte(mu100::ModelIx::XG, base + u.lo + (uint32_t) k, v);

            // The five effect units are REBUILT on every push, whatever the diff says.
            // The firmware computes each parameter's DSP coefficient only when a write
            // CHANGES the stored byte, and keeps the coefficient in work RAM beside it --
            // so the two can disagree, and then no re-send of the right value repairs it.
            // Measured 2026-09-25: FB = 5000 sent to CROSS DELAY stored 111 (out of
            // range, refused) but computed the coefficient from 5000; a project saved
            // then boots with FB reading 111 and a runaway feedback coefficient, and a
            // full push writes NOTHING to the MEG. A TYPE change recomputes the whole
            // unit from its defaults, so switch through a neutral type and back.
            uint8_t cur = 0xFF;
            if (u.neutral != 0xFF && editorState->getByte(mu100::ModelIx::XG, base + u.lo, cur)
                && cur != u.neutral)
            {
                rebuildTypeAt[base + u.lo] = u.neutral;
                typeChanged = true;
            }
            if (! typeChanged) continue;
            for (uint32_t lo = u.lo; lo <= u.hi; ++lo)
                if (editorState->getByte(mu100::ModelIx::XG, base + lo, v))
                    diff.setByte(mu100::ModelIx::XG, base + lo, v);
        }
    }
    std::map<uint32_t, std::pair<uint32_t, int>> paramAt;   // addr21 -> (start, size)
    for (int i = 0; i < AddressMap::count(); ++i)
    {
        const auto& p = AddressMap::param(i);
        if (p.multiTemplate || p.model != static_cast<uint8_t>(mu100::ModelIx::XG))
            continue;
        const bool templated = p.hi.mask || p.mid.mask || p.lo.mask;
        for (int inst = 0; inst < (templated ? 32 : 1); ++inst)
        {
            mu100::ModelIx m; uint32_t a;
            AddressMap::resolve(p, inst, m, a);
            for (int k = 0; k < p.size; ++k)
                paramAt.emplace(a + (uint32_t) k, std::make_pair(a, (int) p.size));
        }
    }
    {
        const auto on = mu100::encodeXGSystemOn(0);
        stream(on.data(), on.size());
    }
    std::set<uint64_t> sentParams;
    auto sendParamAt = [&](mu100::ModelIx ix, uint32_t a, int size)
    {
        if (! sentParams.insert(((uint64_t) ix << 32) | a).second)
            return;   // already sent whole
        if (ix == mu100::ModelIx::XG)
            if (auto nt = rebuildTypeAt.find(a); nt != rebuildTypeAt.end())
            {
                const auto nm = mu100::encodeParamChange(mu100::Model::XG, 0,
                                                        (uint8_t) ((a >> 14) & 0x7F),
                                                        (uint8_t) ((a >> 7) & 0x7F),
                                                        (uint8_t) (a & 0x7F),
                                                        { nt->second, 0x00 });
                stream(nm.data(), nm.size());
                // The firmware loads an effect program LATER, not on receipt, and
                // folds two type writes that arrive together into the last one -- which
                // is the type already loaded, so nothing is rebuilt. Give the neutral
                // type time to actually load.
                t += kTypeRebuildSettle;
            }
        mu100::Bytes data;
        for (int k = 0; k < size; ++k)
        {
            uint8_t v = 0;
            if (! editorState->getByte(ix, a + (uint32_t) k, v))
                defs.getByte(ix, a + (uint32_t) k, v);
            data.push_back(v);
        }
        const auto msg = mu100::encodeParamChange(mu100::ixToModel(ix), 0,
                                                  (uint8_t) ((a >> 14) & 0x7F),
                                                  (uint8_t) ((a >> 7) & 0x7F),
                                                  (uint8_t) (a & 0x7F), data);
        stream(msg.data(), msg.size());
    };
    for (const auto& kv : diff.raw())
    {
        const auto ix   = static_cast<mu100::ModelIx>((kv.first >> 24) & 0xF);
        uint32_t   a    = kv.first & 0x1FFFFF;
        int        size = 1;
        if (ix == mu100::ModelIx::XG)
            if (auto it = paramAt.find(a); it != paramAt.end())
            {
                a = it->second.first;
                size = it->second.second;
            }
        sendParamAt(ix, a, size);
    }

    // 2) LOAD each non-default part voice as SEPARATE per-part SysEx writes to bank
    //    MSB (08 nn 01), bank LSB (02), then PROGRAM (03) last. Writing these as
    //    individual parameter changes (as XGEdit does, program last) makes the MU100
    //    actually switch the voice; and because they address the part directly, it
    //    works even when parts share a MIDI receive channel — a channel Program Change
    //    can't target one of two parts on the same channel (the echotech9 case: parts
    //    1 and 2 both on ch1).
    if (bankMsbP >= 0 && bankLsbP >= 0 && progP >= 0)
    {
        auto val = [](const mu100::DeviceState& st, int paramIx, int part) -> int
        {
            const auto v = AddressMap::get(st, AddressMap::param(paramIx), part);
            return v.present ? (int) v.combined : -1;
        };
        auto sendVoiceByte = [&](int paramIx, int part, int value)
        {
            const auto msg = AddressMap::makeParamChange(AddressMap::param(paramIx),
                                                         part, (uint32_t) value);
            stream(msg.data(), msg.size());
        };
        for (int p = 0; p < 32; ++p)
        {
            const int msb = val(*editorState, bankMsbP, p);
            const int lsb = val(*editorState, bankLsbP, p);
            const int prg = val(*editorState, progP,    p);
            if (msb < 0 || lsb < 0 || prg < 0)
                continue;
            // Skip parts already at their factory voice (the XG reset handled those).
            if (msb == val(defs, bankMsbP, p) && lsb == val(defs, bankLsbP, p)
                && prg == val(defs, progP, p))
                continue;
            sendVoiceByte(bankMsbP, p, msb);
            sendVoiceByte(bankLsbP, p, lsb);
            sendVoiceByte(progP,    p, prg);   // program last -> triggers the voice load
        }
    }

    // 3) Portamento-time re-assert. The stream above stores PORTAMENTO TIME (08 nn 68)
    //    but the firmware does not audibly latch it into its portamento engine until the
    //    value is written AGAIN WITH A CHANGE: it acts on a delta, so re-sending the
    //    identical value is a no-op (this is why portamento stayed off after a load until
    //    the user nudged PTIME by hand). Reproduce the verified manual "nudge by 1 and
    //    back": after a settle gap, for each part carrying a non-zero portamento time,
    //    write a kicked value then the real value. (Reference setups were built on
    //    MU128/SW1000XG; the MU100 firmware needs this post-init delta.)
    {
        const int ptimeP = AddressMap::find("PORTAMENTO TIME", "MULTI PART");
        if (ptimeP >= 0)
        {
            t += 0.40;   // let the params + voice loads above settle first
            for (int p = 0; p < 32; ++p)
            {
                const auto pv = AddressMap::get(*editorState, AddressMap::param(ptimeP), p);
                if (! pv.present) continue;
                const int real = (int) pv.combined & 0x7F;
                if (real == 0) continue;   // 0 = no glide; nothing to latch
                const int kick = (real < 0x7F) ? real + 1 : real - 1;
                auto sendPtime = [&](int value)
                {
                    const auto msg = AddressMap::makeParamChange(
                        AddressMap::param(ptimeP), p, (uint32_t) value);
                    stream(msg.data(), msg.size());
                    t += 0.010;   // let the firmware register the delta
                };
                sendPtime(kick);   // change the value (forces a delta)...
                sendPtime(real);   // ...then restore the real one
            }
        }
    }

    // 4) DRUM SETUP last, because step 2's program writes DESTROY it. Measured
    //    2026-08-06 against the device (the DRUMRESET probe, `debug:` commit f66445e):
    //    writing a part's PROGRAM re-initialises the Drum Setup assigned to it — and
    //    it fires even when the program is rewritten UNCHANGED, so there is no delta
    //    test to lean on here (unlike PORTAMENTO TIME in step 3 above). Before this,
    //    step 1 streamed the per-key drum bytes FIRST and every .mss / .syx load
    //    quietly lost them. applySetupPartXml already orders it this way.
    //
    //    Three consequences beyond the ordering, all measured in the same probe:
    //    * The whole block goes out, never a diff. Step 1's diff is against the
    //      FACTORY defaults, but a re-initialised buffer holds the NEW KIT's defaults
    //      — Jazz Kit and Standard Kit differ at the probe's test key — so "the device
    //      already has this byte" is not safe for any byte here.
    //    * It goes as BULK DUMPS, not parameter changes. toParamChanges() packs each
    //      contiguous run into ONE multi-byte parameter change, and the firmware
    //      consumes only the addressed parameter's own size from it: sending a key's
    //      16-byte run this way applied byte 0 and silently dropped the other 15
    //      (measured — a single-byte write to the same address landed immediately
    //      after). Step 1 was once thought to get away with the same encoder because
    //      a sparse diff is MOSTLY runs of length 1 -- it did not (adjacent sends on
    //      one part are a run); it now sends one message per parameter. A block needs
    //      the block-shaped message. toBulkDumps() groups by the same runs and is the
    //      format the device itself emits for these blocks.
    //    * Only buffers a part actually names are sent. A full buffer is 316 messages
    //      ≈ 2 s of wire, and an unreferenced buffer can be neither heard nor read
    //      back (requestDeviceDump skips those too, for the same reason).
    {
        // PART MODE 2..5 = DRUMS1..4. Taken from the model rather than
        // partDrumBuffer[], so this stays a pure model->device push like the rest of
        // this function and does not depend on the editor having pushed its map.
        bool used[4] = { false, false, false, false };
        const int modeP = AddressMap::find("PART MODE", "MULTI PART");
        if (modeP >= 0)
            for (int p = 0; p < 32; ++p)
            {
                const auto v = AddressMap::get(*editorState, AddressMap::param(modeP), p);
                if (v.present && v.combined >= 2 && v.combined <= 5)
                    used[v.combined - 2] = true;
            }

        // DIFF AGAINST THE KIT'S DEFAULTS, which is only possible since
        // MU100DrumDefaults.h. Step 2's program writes have just re-initialised each
        // used buffer to its kit's defaults, and the table says exactly what those
        // are -- so a byte equal to its default is already on the device and does not
        // need sending. That retires the "no delta test is safe here" caveat above
        // for every kit in the table, and it matters more now than it used to: the
        // model holds a FULL 3n block for every buffer that has been assigned a kit
        // (resetDrumBuffer populates it), so without a diff this would stream up to
        // 4 x 316 messages -- ~11 s of wire -- on every patch load, where before it
        // usually sent nothing at all.
        //
        // A kit NOT in the table falls back to sending its whole block, which is the
        // pre-2026-09-08 behaviour and always correct, just slower.
        const uint8_t* defRow[4][mu100::kDrumDefNoteSpan] = {};
        for (int b = 0; b < 4; ++b)
        {
            if (! used[b]) continue;
            int kMsb = 0, kLsb = 0, kPgm = 0;
            if (! drumBufferKitFromModel(b, kMsb, kLsb, kPgm)) continue;
            for (int n = 0; n < mu100::kDrumDefNoteSpan; ++n)
                defRow[b][n] = mu100::drumDefaultsFor(kMsb, kLsb, kPgm,
                                                      mu100::kDrumDefFirstNote + n);
        }

        mu100::DeviceState drums;
        bool any = false;
        int skipped = 0;
        for (const auto& kv : editorState->raw())
        {
            const auto ix = static_cast<mu100::ModelIx>((kv.first >> 24) & 0xF);
            if (ix != mu100::ModelIx::XG) continue;
            const uint32_t addr = kv.first & 0x1FFFFF;
            const uint8_t  aH   = (addr >> 14) & 0x7F;
            if (aH < 0x30 || aH > 0x33 || ! used[aH - 0x30]) continue;

            const int noteIx = (int) ((addr >> 7) & 0x7F) - mu100::kDrumDefFirstNote;
            if (noteIx >= 0 && noteIx < mu100::kDrumDefNoteSpan)
                if (const uint8_t* row = defRow[aH - 0x30][noteIx])
                {
                    const int off = mu100::drumDefaultOffsetForLow((uint8_t) (addr & 0x7F));
                    if (off >= 0 && row[off] == kv.second) { ++skipped; continue; }
                }

            drums.setByte(ix, addr, kv.second);
            any = true;
        }
        if (skipped > 0)
            mamediag::logf("patch: drum blocks - %d byte(s) already at the kit default, not sent",
                           skipped);

        if (any)
        {
            t += 0.20;   // let the voice loads finish before rewriting their setups
            streamSysEx(drums.toBulkDumps());
        }
    }

    // A bulk send has just written every part's REAL shelf gains to the device, so a
    // part that is meant to be EQ-bypassed is no longer bypassed at the hardware end.
    // Re-zero those shelves LAST -- and on this stream's own clock, not through
    // sendSysEx: the patch above is scheduled into the future, so an immediate send
    // would land BEFORE it and be overwritten. Mirrors applyPartShelfBypass /
    // applyBufferShelfBypass; keep the three in step.
    const uint32_t byp = partEqBypassMask.load(std::memory_order_relaxed);
    if (byp != 0)
    {
        static const int kShelfGain[2] = { AddressMap::find("EQ BASS GAIN",   "MULTI PART"),
                                           AddressMap::find("EQ TREBLE GAIN", "MULTI PART") };
        for (int part = 0; part < 32; ++part)
        {
            if (((byp >> part) & 1u) == 0) continue;
            if (partShelfEqInert(part)) continue;
            for (const int idx : kShelfGain)
                if (idx >= 0)
                    streamSysEx(AddressMap::makeParamChange(AddressMap::param(idx), part, 64));
        }
    }

    // The per-key shelves are re-zeroed from bufferShelvesZeroed rather than from the
    // mask, for the same reason applyBufferShelfBypass is: that array is the record of
    // which buffers this plugin has actually flattened, and it stays right across a
    // membership change that the mask alone cannot describe.
    {
        static const int kDrumShelfGain[2] = { AddressMap::find("EQ BASS GAIN",   "DRUM SETUP"),
                                               AddressMap::find("EQ TREBLE GAIN", "DRUM SETUP") };
        for (int buf = 0; buf < 4; ++buf)
        {
            if (! bufferShelvesZeroed[buf]) continue;
            // Only the keys the patch just wrote a non-flat gain to -- the same rule
            // applyBufferShelfBypass uses.
            for (int key = 0x0D; key <= 0x5B; ++key)
                for (const int idx : kDrumShelfGain)
                {
                    if (idx < 0) continue;
                    const auto v = AddressMap::get(*editorState, idx, key, buf);
                    if (! v.present || v.combined == 64) continue;
                    streamSysEx(AddressMap::makeParamChange(AddressMap::param(idx), key, 64, buf));
                }
        }
    }

    if (t <= startTime) return 0;
    return juce::roundToInt((t - senderMameTime()) * 1000.0);
}

void YamahaMU100AudioProcessor::requestDeviceDump(const std::vector<int>& drumSetups)
{
    MameLease mame(*this);
    if (! mame) return;

    // RELEASE THE EQ BYPASS FIRST, and this is not a nicety: while a part is bypassed
    // its XG shelves are sitting at 0 dB on the device, and a read-back would fold
    // those zeroes into the model on top of the values the user actually set --
    // silently destroying them. Releasing here restores the real gains ahead of the
    // dump requests on the same wire, so what comes back is the truth. The lamp goes
    // out with it, which is the honest report of what just happened.
    setPartEqBypassMask(0);

    // XG block start addresses (MU100 MIDI spec Table 1-1). Each becomes a
    // Dump Request F0 43 2n 4C aH aM aL F7 (device 0 → 0x20); the device replies
    // with the matching XG bulk dump. We request the blocks the editor needs to
    // mirror device state. Concatenated into ONE stream so sendSysEx schedules
    // them back-to-back at the MIDI baud rate (one startTime, sequential slots).
    // IMPORTANT: a Dump Request returns ONLY the bulk-dump chunk whose top address
    // matches the request exactly — the device does NOT return a whole logical block
    // from its start address. The Effect-1 and Insertion blocks are each emitted as
    // several separately-addressed chunks (verified against device dumpouts), so we
    // must request EVERY chunk top or the later parameters never read back (this was
    // the cause of reverb params 11-16 / variation type+connection not refreshing).
    static const uint8_t kBlocks[][3] = {
        { 0x00, 0x00, 0x00 },   // XG System (master tune/volume/transpose)
        // Effect 1 — reverb / chorus / variation, six chunks:
        { 0x02, 0x01, 0x00 },   //   reverb type + params 1-10 + return/pan
        { 0x02, 0x01, 0x10 },   //   reverb params 11-16
        { 0x02, 0x01, 0x20 },   //   chorus type + params 1-10 + return/pan/to-rev
        { 0x02, 0x01, 0x30 },   //   chorus params 11-16
        { 0x02, 0x01, 0x40 },   //   variation type + params 1-10 + sends/connection/part
        { 0x02, 0x01, 0x70 },   //   variation params 11-16
        { 0x02, 0x40, 0x00 },   // Multi EQ (single chunk)
        // Insertion Effect 1 — three chunks:
        { 0x03, 0x00, 0x00 },   //   type + params 1-10 + part/control
        { 0x03, 0x00, 0x20 },   //   params 11-16
        { 0x03, 0x00, 0x30 },   //   2-byte delay-MSB params region (delay-family effects)
        // Insertion Effect 2 — three chunks:
        { 0x03, 0x01, 0x00 },
        { 0x03, 0x01, 0x20 },
        { 0x03, 0x01, 0x30 },
        { 0x10, 0x00, 0x00 },   // A/D Part
        { 0x11, 0x00, 0x00 },   // A/D System
    };

    // Pace EVERY request on ONE CONTINUOUS MAME-time timeline (no separate flood).
    // Two reasons: (1) each Dump Request makes the device transmit a reply that
    // drains slower than requests arrive, so a fixed gap keeps replies from
    // overflowing the device's MIDI input; (2) a continuous stream — with no large
    // lead between phases — keeps replies flowing without a long real-time gap. After
    // a reset MAME runs below real-time, so even a 1 s emulated lead becomes several
    // real seconds; the read-back's idle-gap detector would then see that gap right
    // after the first replies, declare the run finished, and orphan the part/drum
    // replies (stale panels + a stray dump → spurious "Save dump"). Start a touch
    // ahead of now so the first byte isn't already late.
    // Drain any coalesced edit and start past the queue tail. Two reasons: an edit
    // flushed mid-stream would be interleaved into the request run, and — because the
    // ring gates on the head slot alone — anything still queued ahead delays the whole
    // run past its own schedule, collapsing the careful pacing below into a flood the
    // device answers only partially.
    const double timePerByte = 1.0 / (31250.0 / 10.0);
    flushPendingParamChanges();
    double t = juce::jmax(senderMameTime() + 0.1, sendTailMameTime);
    auto pacedReq = [this, &t, timePerByte](uint8_t h, uint8_t m, uint8_t l, double gap)
    {
        const uint8_t req[8] = { 0xF0, 0x43, 0x20, 0x4C, h, m, l, 0xF7 };
        pushMidiMessageToPort(0, req, 8, t, timePerByte);
        t += 8 * timePerByte + gap;
    };

    // A read-back READS; it never writes. A staged "restore the saved global blocks"
    // push used to run here ahead of the requests — removed 2026-08-07, see the
    // pendingGlobalRestore note in PluginProcessor.h. Do not reintroduce a write on
    // this path: it fires long after the project load that staged it, so it lands on
    // top of whatever the user has edited since.

    // Global single + effect / EQ / insertion / A/D blocks (small replies).
    for (const auto& b : kBlocks)
        pacedReq(b[0], b[1], b[2], 0.020);

    // Multi Part 1..32 (08 00..1F). Like the effect blocks, each part is dumped as
    // SEPARATE chunks and a Dump Request returns only the chunk at the exact address,
    // so request every chunk top or the later part parameters never read back. Chunk
    // tops from the device's own bulk-dump segmentation:
    //   00 -> 00..28 (reserve/bank/program/channel/mode/pan/sends/filter/EG/MW/bend)
    //   30 -> 30..6E (Rcv switches, scale tuning, controllers, portamento, pitch EG)
    //   70 -> 70..73 and 74 -> 74..7F (velocity limits, Part-EQ gain/freq)
    // Part replies are larger than drum sub-blocks, so use a wider reply window.
    static const uint8_t kPartChunks[] = { 0x00, 0x30, 0x70, 0x74 };
    for (int part = 0; part < 32; ++part)
    {
        for (uint8_t cl : kPartChunks)
            pacedReq(0x08, static_cast<uint8_t>(part), cl, 0.030);

        // Per-part HIGH PASS FILTER CUTOFF lives in a SEPARATE block (0A nn 20), not
        // the 08 block, so it needs its own request or the HPF knob never reads back.
        // Interleave it with each part's 08 chunks (rather than a separate trailing
        // loop) so the HPF replies are spread through the run instead of bunched at
        // the very end, where a slower hosted MAME is likeliest to drop them.
        pacedReq(0x0A, static_cast<uint8_t>(part), 0x20, 0.030);
    }

    // DRUM SETUP read-back. A dump request returns only the addressed sub-block of
    // the addressed note, so ask for EACH note (rr = 0x0D..0x5B) and EACH of its four
    // sub-blocks (spec DRUM SETUP table): 00 = main 16-byte block (pitch/level/pan/
    // sends/assign/rcv/LPF/EG rates), 20 = EQ, 50 = HPF, 60 = velocity-sense. Only for
    // the setups actually in use (passed in). Hundreds of requests; the small replies
    // tolerate a tighter window than the part chunks.
    static const uint8_t kDrumSub[] = { 0x00, 0x20, 0x50, 0x60 };
    for (int setup : drumSetups)
    {
        if (setup < 0 || setup > 3) continue;
        const uint8_t hi = static_cast<uint8_t>(0x30 + setup);
        for (uint8_t note = 0x0D; note <= 0x5B; ++note)
            for (uint8_t sub : kDrumSub)
                pacedReq(hi, note, sub, 0.012);
    }

    // An edit made while the read-back runs queues AFTER the request stream. It would
    // otherwise be scheduled at "now", i.e. ahead of requests already queued for later,
    // and the request they precede would be answered against a changed device.
    sendTailMameTime = t;
}

//==============================================================================
// MIDI OUT bulk-dump capture
//==============================================================================
namespace {
// A Yamaha bulk-dump message is: F0 43 0N <model> SS TT GG MM LL <data…> KK F7
// where 0N = device number (high nibble 0), model ∈ {4C XG, 49 native-1, 59 native-2},
// SS/TT = byte count, GG/MM/LL = address. (MU100 MIDI spec §2.2.1–2.2.3.)
bool isBulkDumpSysex(const std::vector<uint8_t>& m)
{
    return m.size() >= 10
        && m.front() == 0xF0
        && m[1] == 0x43
        && (m[2] & 0xF0) == 0x00
        && (m[3] == 0x4C || m[3] == 0x49 || m[3] == 0x59);
}
} // namespace

void YamahaMU100AudioProcessor::pushMidiOutByte(uint8_t data)
{
    const juce::ScopedLock sl(dumpLock);

    // System real-time bytes (F8–FF) may be interleaved anywhere; never part of a
    // SysEx payload. The MU100 doesn't transmit them, but drop them defensively.
    if (data >= 0xF8)
        return;

    if (data == 0xF0)
    {
        dumpInSysex = true;
        dumpCurrentSysex.clear();
        dumpCurrentSysex.push_back(data);
        return;
    }

    if (!dumpInSysex)
        return;  // ignore non-SysEx traffic (channel msgs, MIDI thru echo, …)

    dumpCurrentSysex.push_back(data);

    if (data == 0xF7)
    {
        dumpInSysex = false;
        if (isBulkDumpSysex(dumpCurrentSysex))
        {
            dumpBuffer.insert(dumpBuffer.end(),
                              dumpCurrentSysex.begin(), dumpCurrentSysex.end());
            lastDumpByteMs.store(juce::Time::getMillisecondCounter(),
                                 std::memory_order_relaxed);
        }
        dumpCurrentSysex.clear();
    }
    else if (data & 0x80)
    {
        // Any other status byte mid-SysEx aborts the (malformed) message.
        dumpInSysex = false;
        dumpCurrentSysex.clear();
    }
}

bool YamahaMU100AudioProcessor::pollCompletedDump(std::vector<uint8_t>& out, bool eager)
{
    const juce::ScopedLock sl(dumpLock);
    if (dumpBuffer.empty())
        return false;

    const uint32_t now  = juce::Time::getMillisecondCounter();
    const uint32_t idle = now - lastDumpByteMs.load(std::memory_order_relaxed);

    // dumpBuffer only ever holds COMPLETE bulk-dump messages (pushMidiOutByte appends
    // a message only at its F7), so it is always safe to hand over as-is.
    if (eager)
    {
        // Read-back: deliver once the burst goes idle (done) OR periodically while it
        // streams, so a long continuous reply run is consumed in chunks rather than
        // one final burst the read-back might time out before receiving.
        if (idle < (uint32_t) kDumpFinalizeMs
            && now - lastDumpDeliverMs < (uint32_t) kDumpEagerMs)
            return false;
    }
    else if (idle < (uint32_t) kDumpFinalizeMs)
    {
        return false;  // user dumpout: wait for the whole burst to go idle
    }

    out = std::move(dumpBuffer);
    dumpBuffer.clear();
    lastDumpDeliverMs = now;
    return true;
}

void YamahaMU100AudioProcessor::clearDumpCapture()
{
    // Discard any captured-but-unconsumed MIDI-OUT bulk-dump bytes. Used to drop
    // read-back residue that the device kept transmitting after the editor was
    // closed mid-read-back: without this, that stale burst finalizes on the next
    // editor open (when no read-back is pending) and pops a spurious "Save dump"
    // dialog. Also called at the start of each read-back so a prior tail can't
    // merge into the fresh dump.
    const juce::ScopedLock sl(dumpLock);
    dumpBuffer.clear();
    dumpCurrentSysex.clear();
    dumpInSysex = false;
}

juce::String YamahaMU100AudioProcessor::makeDumpDefaultFileName(const std::vector<uint8_t>& bytes)
{
    // --- classify by scanning the bulk-dump SysEx headers ---
    // Signatures confirmed against real MU100 captures (2026-06-18):
    //   all       : XG Multi Part (4C aH08/0A) AND native Performance (59 aH30+)
    //   mult-16/32: XG Multi Part only, no Performance; 16 vs 32 = max part index
    //               (address-mid 0x00–0x0F = 16 parts, 0x00–0x1F = 32 parts)
    //   …ad        : the above plus XG A/D Part (4C aH10/11)
    //   perf-all  : native Performance with many distinct performance numbers, no Multi Part
    //   perf-NNN  : native Performance with exactly one performance number
    // NOTE: XG System (4C aH00), XG Drum Setup (4C aH30–33) and native System
    // (49 aH00) appear in EVERY multi/all dump, so they do NOT discriminate and
    // are deliberately ignored. The Model ID separates XG Drum Setup (4C, aH30+)
    // from native Performance (59, aH30+), which share address-high values.
    bool hasMultiPart = false, hasAD = false, hasPerf = false;
    int  maxPartIdx = -1;
    std::set<int> perfNums;   // distinct performance numbers (address mid byte)

    size_t i = 0;
    while (i < bytes.size())
    {
        if (bytes[i] != 0xF0) { ++i; continue; }
        const size_t start = i;
        while (i < bytes.size() && bytes[i] != 0xF7) ++i;
        const size_t end = i;          // index of F7 (or == size if unterminated)
        if (i < bytes.size()) ++i;     // step past F7

        if (end - start < 9) continue;            // too short for a bulk header
        if (bytes[start + 1] != 0x43) continue;   // not Yamaha
        if ((bytes[start + 2] & 0xF0) != 0x00) continue; // not bulk device 0N

        const uint8_t model = bytes[start + 3];
        const uint8_t aH    = bytes[start + 6];   // address high
        const uint8_t aM    = bytes[start + 7];   // address mid

        if (model == 0x4C)                        // XG (table 1-1)
        {
            if      (aH == 0x08 || aH == 0x0A) { hasMultiPart = true;
                                                 maxPartIdx = std::max(maxPartIdx, (int) aM); }
            else if (aH == 0x10 || aH == 0x11)   hasAD = true;
        }
        else if (model == 0x59)                   // native-2 Performance (table 3-1)
        {
            if (aH >= 0x30 && aH <= 0x6F) { hasPerf = true; perfNums.insert(aM); }
        }
    }

    const juce::Time now = juce::Time::getCurrentTime();
    const juce::String stamp = now.formatted("%Y-%m-%d-%H%M");  // yyyy-mm-dd-hhmm (24h)

    auto threeDigit = [](int n) { return juce::String(n).paddedLeft('0', 3); };

    juce::String prefix;

    if (hasMultiPart && hasPerf)
    {
        prefix = "all-";                  // superset: multi setup + all performances
    }
    else if (hasMultiPart)
    {
        const char* n = (maxPartIdx >= 0x10) ? "32" : "16";
        prefix = juce::String("mult-") + n + (hasAD ? "ad-" : "-");
    }
    else if (hasPerf)
    {
        if (perfNums.size() > 1)
            prefix = "perf-all-";
        else
            // nnn = 1-based performance number (address mid 0x00 = preset 001).
            prefix = "perf-" + threeDigit(*perfNums.begin() + 1) + "-";
    }
    else
    {
        prefix = "dumpout-";              // unrecognized → generic fallback
    }

    return prefix + stamp + ".syx";
}

// Discard the queued backlog after a read-back cancel ("Stop reading"), which would
// otherwise keep the device replying for seconds. Discards by CONSUMING rather than by
// jumping readPos up to `reserved`: a producer may still be filling slots it has
// already reserved, and a jump would leave those slots published BEHIND the read cursor,
// to surface a whole ring later as garbage bytes. Stops at the first unfilled slot and
// resumes on the next poll, so it never spins waiting on another thread.
void YamahaMU100AudioProcessor::serviceMidiFlush(MidiRing& ring)
{
    if (ring.flushRequested.exchange(false, std::memory_order_acq_rel))
        ring.flushTo = ring.reserved.load(std::memory_order_acquire);

    while (static_cast<int32_t>(ring.flushTo - ring.readPos.load(std::memory_order_relaxed)) > 0)
    {
        const uint32_t r   = ring.readPos.load(std::memory_order_relaxed);
        const uint32_t idx = r & MIDI_BUFFER_MASK;
        if (ring.ready[idx].load(std::memory_order_acquire) == 0)
            return;                     // producer still filling; pick this up next poll
        ring.ready[idx].store(0, std::memory_order_relaxed);
        ring.readPos.store(r + 1, std::memory_order_release);
    }
}

// Head-of-line watchdog. Delivery gates on the head slot alone, so a head that never
// comes due silently kills the port. Every push is clamped to clock + kMaxMidiLead at
// push time and the clock only advances, so a head beyond that bound NOW is provably
// corrupt — no timer and no state needed, and it cannot fire on the read-back's
// legitimately-paced stream. Deliver the byte late rather than never: a wrong-time
// note is a glitch, a dead port is a dead instrument.
bool YamahaMU100AudioProcessor::midiHeadDue(MidiRing& ring, uint32_t idx, double now, int port)
{
    const double target = ring.slot[idx].targetMameTime;

    if (now >= target)
        return true;

    if (! std::isnan(target) && target <= now + kMaxMidiLeadSeconds)
        return false;                       // legitimately scheduled ahead — wait

    if (! ring.watchdogLogged)
    {
        ring.watchdogLogged = true;
        mamediag::logf("MIDI head-of-line watchdog: port %d target=%.6f now=%.6f "
                       "backlog=%u — impossible timestamp, delivering anyway",
                       port, target, now,
                       ring.reserved.load(std::memory_order_relaxed)
                           - ring.readPos.load(std::memory_order_relaxed));
    }
    return true;
}

bool YamahaMU100AudioProcessor::pollMidiData(int port)
{
    if (!mameMachine) return false;

    MidiRing& ring = (port == 1) ? midiRing2 : midiRing;
    serviceMidiFlush(ring);

    const uint32_t idx = ring.readPos.load(std::memory_order_relaxed) & MIDI_BUFFER_MASK;
    // ready == 0 means either "empty" or "a producer is mid-message"; both mean wait.
    if (ring.ready[idx].load(std::memory_order_acquire) == 0)
        return false;
    return midiHeadDue(ring, idx, mameMachine->time().as_double(), port);
}

int YamahaMU100AudioProcessor::readMidiByte(int port)
{
    if (!mameMachine) return 0;

    MidiRing& ring = (port == 1) ? midiRing2 : midiRing;
    const uint32_t r   = ring.readPos.load(std::memory_order_relaxed);
    const uint32_t idx = r & MIDI_BUFFER_MASK;
    if (ring.ready[idx].load(std::memory_order_acquire) == 0)
        return 0;
    if (! midiHeadDue(ring, idx, mameMachine->time().as_double(), port))
        return 0;

    const uint8_t data = ring.slot[idx].data;
    // Free the slot BEFORE publishing the advanced cursor: the release store below
    // orders this write ahead of it, so a producer that sees the new readPos and
    // reuses the slot cannot race a stale ready flag.
    ring.ready[idx].store(0, std::memory_order_relaxed);
    ring.readPos.store(r + 1, std::memory_order_release);
    return data;
}

// Called by the patched VST3 wrapper (audio thread) just before processBlock(),
// carrying the MU100-B event bus. Stash a copy; processBlock() drains it into
// MAME port 1 with the same timestamp anchoring as port 0.
void YamahaMU100AudioProcessor::receivePortBMidi(const juce::MidiBuffer& portB)
{
    pendingPortBMidi.clear();
    pendingPortBMidi.addEvents(portB, 0, -1, 0);
}

//==============================================================================
// Standalone MIDI device management
//==============================================================================

bool YamahaMU100AudioProcessor::isMidiDeviceEnabled(const juce::String& identifier) const
{
    juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
    auto it = midiDeviceSettings.find(identifier);
    return (it != midiDeviceSettings.end()) && it->second.enabled;
}

uint8_t YamahaMU100AudioProcessor::getMidiDeviceRouteMask(const juce::String& identifier) const
{
    juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
    auto it = midiDeviceSettings.find(identifier);
    if (it != midiDeviceSettings.end())
        return it->second.routeMask;
    return MIDI_ROUTE_PORT1;  // default: route to port 1 (parts 1–16)
}

void YamahaMU100AudioProcessor::setMidiDeviceEnabled(const juce::String& identifier, bool enable)
{
    if (!juce::JUCEApplicationBase::isStandaloneApp()) return;

    OpenDevice toClose;  // destroyed outside the lock so its MidiInput::stop() doesn't deadlock

    {
        juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
        midiDeviceSettings[identifier].enabled = enable;

        if (enable)
        {
            if (openMidiDevices.find(identifier) == openMidiDevices.end())
            {
                auto cb    = std::make_unique<MidiDeviceCallback>(this, identifier);
                auto input = juce::MidiInput::openDevice(identifier, cb.get());
                if (input)
                {
                    input->start();
                    OpenDevice od;
                    od.input    = std::move(input);
                    od.callback = std::move(cb);
                    openMidiDevices[identifier] = std::move(od);
                }
            }
        }
        else
        {
            auto it = openMidiDevices.find(identifier);
            if (it != openMidiDevices.end())
            {
                toClose = std::move(it->second);
                openMidiDevices.erase(it);
            }
        }
    }
    // toClose destroyed here — MidiInput::stop() joins its callback thread safely

    saveSettings();
}

void YamahaMU100AudioProcessor::setMidiDeviceRouteMask(const juce::String& identifier, uint8_t mask)
{
    {
        juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
        midiDeviceSettings[identifier].routeMask = mask;
    }
    saveSettings();
}

// Read on the OS MIDI callback thread, once per note-on, so it takes the same lock the
// route mask does — a map lookup under a CriticalSection, exactly as often.
bool YamahaMU100AudioProcessor::isMidiDeviceDrumSelect(const juce::String& identifier) const
{
    juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
    auto it = midiDeviceSettings.find(identifier);
    return (it == midiDeviceSettings.end()) || it->second.drumSelect;   // unknown = on
}

void YamahaMU100AudioProcessor::setMidiDeviceDrumSelect(const juce::String& identifier, bool enable)
{
    {
        juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
        midiDeviceSettings[identifier].drumSelect = enable;
    }
    saveSettings();
}

// Close and re-open every enabled MIDI input. Windows can leave a process
// receiving NO input callbacks at all while every handle still looks open and
// healthy -- measured here as raw=0 callbacks across all ports for minutes, with
// the emulator running perfectly. Re-opening the devices clears it, so this is
// the recovery that used to require quitting and relaunching the app.
void YamahaMU100AudioProcessor::reconnectMidiDevices()
{
    if (!juce::JUCEApplicationBase::isStandaloneApp()) return;

    {
        juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
        for (auto& [id, dev] : openMidiDevices)
            if (dev.input)
                dev.input->stop();
        openMidiDevices.clear();
    }

    mamediag::logf("midi-in reconnect requested");
    midiDevicesInitialized = false;
    initMidiDevices();
}

void YamahaMU100AudioProcessor::initMidiDevices()
{
    if (!juce::JUCEApplicationBase::isStandaloneApp()) return;
    if (midiDevicesInitialized) return;
    midiDevicesInitialized = true;

    juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
    for (auto& [id, settings] : midiDeviceSettings)
    {
        if (settings.enabled && openMidiDevices.find(id) == openMidiDevices.end())
        {
            auto cb    = std::make_unique<MidiDeviceCallback>(this, id);
            auto input = juce::MidiInput::openDevice(id, cb.get());
            // A failed open used to be swallowed silently, and since
            // midiDevicesInitialized latches there is no retry -- the app then
            // looks perfectly healthy while receiving no MIDI at all.
            mamediag::logf("midi-in open %s : %s",
                           input ? "OK  " : "FAIL", id.toRawUTF8());
            if (input)
            {
                input->start();
                OpenDevice od;
                od.input    = std::move(input);
                od.callback = std::move(cb);
                openMidiDevices[id] = std::move(od);
            }
        }
    }
}

bool YamahaMU100AudioProcessor::isMidiOutputDeviceEnabled(const juce::String& identifier) const
{
    juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
    auto it = midiOutputEnabled.find(identifier);
    return (it != midiOutputEnabled.end()) && it->second;
}

void YamahaMU100AudioProcessor::setMidiOutputDeviceEnabled(const juce::String& identifier, bool enable)
{
    if (!juce::JUCEApplicationBase::isStandaloneApp()) return;

    {
        juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
        midiOutputEnabled[identifier] = enable;

        if (enable)
        {
            if (openMidiOutputs.find(identifier) == openMidiOutputs.end())
            {
                if (auto out = juce::MidiOutput::openDevice(identifier))
                    openMidiOutputs[identifier] = std::move(out);
            }
        }
        else
        {
            openMidiOutputs.erase(identifier);  // ~MidiOutput just closes the handle (no callback thread)
        }
    }

    saveSettings();
}

void YamahaMU100AudioProcessor::initMidiOutputDevices()
{
    if (!juce::JUCEApplicationBase::isStandaloneApp()) return;
    if (midiOutputDevicesInitialized) return;
    midiOutputDevicesInitialized = true;

    juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
    for (auto& [id, enabled] : midiOutputEnabled)
    {
        if (enabled && openMidiOutputs.find(id) == openMidiOutputs.end())
        {
            if (auto out = juce::MidiOutput::openDevice(id))
                openMidiOutputs[id] = std::move(out);
        }
    }
}

//==============================================================================
// File > Open Recent. Newest first; an existing entry MOVES to the head rather than
// being duplicated, so re-opening the same file does not fill the list with one name.
// Comparison ignores case because Windows paths do — otherwise a file reached once
// through the chooser and once through this menu can appear twice.
void YamahaMU100AudioProcessor::addRecentFile(const juce::String& fullPath)
{
    if (fullPath.isEmpty())
        return;

    recentFiles.removeString(fullPath, /*ignoreCase*/ true);
    recentFiles.insert(0, fullPath);
    recentFiles.removeRange(kMaxRecentFiles, recentFiles.size());
    saveSettings();
}

//==============================================================================
// Everything this instance would write, as XML. Split out of saveSettings so the
// same shape can be taken as a SNAPSHOT at construction: the merge below compares
// our view against that snapshot to decide which keys we changed ourselves, and a
// comparison is only sound if both sides are built by the same code.
std::unique_ptr<juce::XmlElement> YamahaMU100AudioProcessor::buildSettingsXml() const
{
    auto rootPtr = std::make_unique<juce::XmlElement>("YamahaMU100Settings");
    juce::XmlElement& root = *rootPtr;

    // Only a deliberate override is persisted; absent means "use the compiled-in
    // default", which is what makes a moved Documents folder self-healing.
    if (juce::File(romPath) != defaultRomDir())
        root.setAttribute("romPath", romPath);
    root.setAttribute("lastSyxDir", lastSyxDir);
    root.setAttribute("lastDumpDir", lastDumpDir);
    root.setAttribute("lastSetupDir", lastSetupDir);
    root.setAttribute("defaultEditorView", defaultEditorView);
    root.setAttribute("editorTheme", editorTheme);
    root.setAttribute("midiDrumSelectDuringPlayback", midiDrumSelectDuringPlayback ? 1 : 0);
    root.setAttribute("tooltipMode", tooltipMode);
    root.setAttribute("focusMarkMode", focusMarkMode);
    root.setAttribute("partTitleNames", partTitleNames);
    root.setAttribute("noteLabels", noteLabelMode);
    root.setAttribute("midiOutFormat", midiOutFormat);
    root.setAttribute("automationMarkers", markDawAutomatable ? 0 : 2);
    root.setAttribute("markMidiCcParams", markMidiCcParams ? 1 : 0);
    root.setAttribute("reverseDialAction", reverseDialAction ? 1 : 0);
    root.setAttribute("enablePluginOnlyParams", enablePluginOnlyParams ? 1 : 0);
    root.setAttribute("showPartRcvSwitches", showPartRcvSwitches ? 1 : 0);
    root.setAttribute("showMuteSoloColumns", showMuteSoloColumns ? 1 : 0);
    root.setAttribute("showChangedMarks", showChangedMarks ? 1 : 0);
    root.setAttribute("shortVoiceNames", shortVoiceNames ? 1 : 0);
    root.setAttribute("useKeyboardShortcuts", useKeyboardShortcuts ? 1 : 0);
    // The per-part output pair is deliberately absent: it is project state only, so
    // there is no global value to write. See setPartOutputsEnabled.
    root.setAttribute("midiInputIndicators", midiInputIndicators ? 1 : 0);
    root.setAttribute("keyboardPlayedNotes", keyboardPlayedNotes ? 1 : 0);
    root.setAttribute("showMidiOverrides",   showMidiOverrides   ? 1 : 0);
    root.setAttribute("midiOverrideMarks",   midiOverrideMarkStyle);
    root.setAttribute("midiFollowEnabled",    midiFollowEnabled   ? 1 : 0);
    root.setAttribute("eqSpectrum",      eqSpectrumEnabled ? 1 : 0);
    root.setAttribute("eqSpectrumRes",   eqSpectrumFftOrder);
    root.setAttribute("eqSpectrumDecay", eqSpectrumDecayIdx);
    root.setAttribute("volumeMeters",     volumeMetersEnabled ? 1 : 0);
    root.setAttribute("volumeMeterDecay", volumeMeterDecayIdx);
    root.setAttribute("volumeMeterRamp",  volumeMeterRampIdx);
    root.setAttribute("volumeMeterSpread", volumeMeterSpreadIdx);
    root.setAttribute("volumeMeterFill",   volumeMeterFillIdx);
    root.setAttribute("editorViewW", editorViewW);
    root.setAttribute("editorViewH", editorViewH);
    root.setAttribute("editorTextValuesPct", editorTextValuesPct);
    root.setAttribute("editorTextLabelsPct", editorTextLabelsPct);
    root.setAttribute("editorTextTitlesPct", editorTextTitlesPct);
    root.setAttribute("editorTextVoiceListPct", editorTextVoiceListPct);
    root.setAttribute("editorTextGrDisValuePct", editorTextGrDisValuePct);
    root.setAttribute("editorTextOptionsPct", editorTextOptionsPct);
    root.setAttribute("pdcTrimIndex",    pdcTrimIndex.load(std::memory_order_relaxed));
    root.setAttribute("bufferThreshold", mameBufferThreshold.load(std::memory_order_relaxed));

    for (const auto& p : recentFiles)
        root.createNewChildElement("recentFile")->setAttribute("path", p);

    // Only the rows that differ from their default are written, so a later change to a
    // default reaches everyone who never touched that row.
    keyBindings.writeTo(root);

    {
        juce::CriticalSection::ScopedLockType lock(midiDeviceLock);
        for (const auto& [id, s] : midiDeviceSettings)
        {
            auto* el = root.createNewChildElement("midiDevice");
            el->setAttribute("id", id);
            el->setAttribute("enabled",   s.enabled ? 1 : 0);
            el->setAttribute("routeMask", static_cast<int>(s.routeMask));
            el->setAttribute("drumSelect", s.drumSelect ? 1 : 0);
        }
        for (const auto& [id, enabled] : midiOutputEnabled)
        {
            auto* el = root.createNewChildElement("midiOutputDevice");
            el->setAttribute("id", id);
            el->setAttribute("enabled", enabled ? 1 : 0);
        }
    }

    return rootPtr;
}

//==============================================================================
// MERGE-ON-WRITE. See the header for why this exists at all.
//
// Three inputs, per key: OURS (what this instance holds now), SNAP (what this
// instance believes is on disk) and DISK (what is actually there). The rule is one
// line: if ours differs from snap, THIS instance changed it, so ours wins; if not,
// the file wins, whatever it says -- another instance changed it, or nobody did.
//
// PRESENCE IS PART OF THE VALUE, which is what makes `romPath` work: we write it
// only when it differs from the compiled default, so "we set it back to the
// default" shows up as present-in-snap, absent-in-ours. Comparing values alone
// would miss that and the file would keep a romPath the user just cleared.
//
// Children are merged as WHOLE GROUPS by tag (`recentFile`, `midiDevice`,
// `midiOutputDevice`, `shortcut`) rather than element by element. Each is a list
// whose meaning is the list -- the recent files in order, the devices that exist --
// so half of one instance's list interleaved with half of another's would be a
// state neither instance ever had.
static void mergeSettingsXml(juce::XmlElement& ours,
                             const juce::XmlElement* snap,
                             const juce::XmlElement& disk)
{
    // ---- attributes ----
    juce::StringArray names;
    for (int i = 0; i < ours.getNumAttributes(); ++i) names.addIfNotAlreadyThere(ours.getAttributeName(i));
    for (int i = 0; i < disk.getNumAttributes(); ++i) names.addIfNotAlreadyThere(disk.getAttributeName(i));
    if (snap != nullptr)
        for (int i = 0; i < snap->getNumAttributes(); ++i) names.addIfNotAlreadyThere(snap->getAttributeName(i));

    for (const auto& name : names)
    {
        const bool haveOurs = ours.hasAttribute(name);
        const bool haveSnap = snap != nullptr && snap->hasAttribute(name);
        const bool weChanged = (haveOurs != haveSnap)
                            || (haveOurs && ours.getStringAttribute(name)
                                            != snap->getStringAttribute(name));
        if (weChanged) continue;                    // ours already stands in `ours`

        if (disk.hasAttribute(name)) ours.setAttribute(name, disk.getStringAttribute(name));
        else                         ours.removeAttribute(name);
    }

    // ---- children, by tag ----
    auto groupText = [](const juce::XmlElement* e, const juce::String& tag)
    {
        juce::String s;
        if (e != nullptr)
            for (auto* c : e->getChildWithTagNameIterator(tag))
                s << c->toString(juce::XmlElement::TextFormat().singleLine());
        return s;
    };

    juce::StringArray tags;
    for (auto* c : ours.getChildIterator()) tags.addIfNotAlreadyThere(c->getTagName());
    for (auto* c : disk.getChildIterator()) tags.addIfNotAlreadyThere(c->getTagName());
    if (snap != nullptr)
        for (auto* c : snap->getChildIterator()) tags.addIfNotAlreadyThere(c->getTagName());

    for (const auto& tag : tags)
    {
        if (groupText(&ours, tag) != groupText(snap, tag)) continue;   // we changed it

        ours.deleteAllChildElementsWithTagName(tag);
        for (auto* c : disk.getChildWithTagNameIterator(tag))
            ours.addChildElement(new juce::XmlElement(*c));
    }
}

void YamahaMU100AudioProcessor::saveSettings()
{
    juce::File settingsFile = juce::File::getSpecialLocation(
        juce::File::userDocumentsDirectory).getChildFile("MEGAN XG/settings.xml");

    auto mine   = buildSettingsXml();
    auto merged = std::make_unique<juce::XmlElement>(*mine);

    // A file that is missing or unparsable leaves ours as the whole answer, which is
    // also what a first run does. Deliberately NOT treated as an error: the next save
    // rewrites it, and refusing to save because another process wrote something we
    // cannot read would lose the user's change for good.
    if (auto disk = juce::XmlDocument::parse(settingsFile))
    {
        mergeSettingsXml(*merged, settingsSnapshot.get(), *disk);

        // THE ONE EXCEPTION TO "keep what the file says". A retired attribute is read
        // once, to migrate an old install, and is then meant to disappear the next
        // time the file is written (see the obsolete rows in the manual's settings.xml
        // appendix). The merge above would preserve it for ever instead -- we never
        // write it, so it always reads as "not changed by us", so the file's copy wins
        // on every save. Named explicitly rather than inferred: an attribute we do not
        // write is otherwise indistinguishable from one a NEWER version wrote, and
        // carrying those through is the point of the merge.
        for (const char* dead : { "showTooltips", "partOutputs", "partOutputsRemoveFromMain",
                                  "disablePluginOnlyParams", "midiOverridesLocked" })
            merged->removeAttribute(dead);
    }

    merged->writeTo(settingsFile);

    // THE SNAPSHOT IS OUR OWN VIEW, NOT WHAT WAS JUST WRITTEN, and getting this wrong
    // defeats the whole merge -- measured 2026-09-11, on the first cut. Adopt the
    // MERGED file and the other instance's value becomes our baseline; our own stale
    // member still holds the old one, so the very next save reads as "we changed it"
    // and writes the old value back. Which is the original bug, one save later: the
    // test that found it was B changing one option and then closing its window, which
    // reverted A's change exactly as before.
    //
    // Snapshotting what WE built keeps the rule honest: our view only moves when this
    // instance changes something, so "ours differs from the snapshot" means precisely
    // that and nothing else.
    settingsSnapshot = std::move(mine);
}

//==============================================================================
void YamahaMU100AudioProcessor::DCBlocker::prepare(double sampleRate, double cutoffHz)
{
    a = (float) std::exp(-2.0 * juce::MathConstants<double>::pi * cutoffHz / sampleRate);
    reset();
}

void YamahaMU100AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    mamediag::logf("prepareToPlay sr=%.1f (was %.1f, mameHasStarted=%d)",
                   sampleRate, hostSampleRate.load(), (int) mameHasStarted.load());
    hostSampleRate.store(sampleRate);
    needAnchorSync.store(true);
    dcBlockerL.prepare(sampleRate, kDcBlockerHz);
    dcBlockerR.prepare(sampleRate, kDcBlockerHz);

    // MAME's output rate is fixed for the life of the machine, so a host that switches
    // to a device running at a different rate is served by resampling the ring on the
    // way out. Without this the emulation kept producing at (say) 44100 while the host
    // consumed 48000, the ring underran, and the plugin fell silent until relaunch.
    // Size the scratch here so the audio thread never allocates; the interpolators
    // carry history, so a rate change must reset them.
    resamplerL.reset();
    resamplerR.reset();
    {
        const double ratio  = (sampleRate > 0.0) ? effectiveMameRate() / sampleRate : 1.0;
        const size_t needed = static_cast<size_t>(
            std::ceil(juce::jmax(1, samplesPerBlock) * juce::jmax(1.0, ratio))) + 8;
        resampleScratchL.assign(needed, 0.0f);
        resampleScratchR.assign(needed, 0.0f);
        auxResampleScratch.assign(needed, 0.0f);
    }

    // Same treatment for the per-part buses: their blockers carry a coefficient at
    // the host rate and their interpolators carry history, so both must follow a rate
    // change. Cheap even when the buses are off — 64 tiny structs.
    for (auto& b : auxDcBlocker) b.prepare(sampleRate, kDcBlockerHz);
    for (auto& r : auxResampler) r.reset();

    // The MAME thread stages one frame per sample into this; size it here so the
    // audio path never allocates. Grown in pushAudioFromMame only if a host hands us
    // a bigger block than it declared.
    partFrameScratch.assign(size_t(juce::jmax(1, samplesPerBlock)) * kNumAuxChannels, 0);

    // releaseResources stops the capture without clearing the user's setting, so a
    // host that stops and restarts us has to get the buses back. The dirty flag makes
    // the MAME thread re-enable capture; a stale FIFO backlog cannot misalign us
    // because read_part_frames keeps only the newest frames it is asked for.
    if (partOutputsEnabled.load(std::memory_order_relaxed))
    {
        auxShortReadLogged = false;
        auxRingLive.store(auxRing != nullptr, std::memory_order_release);
        partOutputsDirty.store(true, std::memory_order_release);
    }

    reportLatencyToHost();
    initMidiDevices();
    initMidiOutputDevices();

    // Boot MAME once, on the first prepareToPlay only. Plugin scanners and
    // GUI-only host instantiations never reach here, so MAME stays dormant.
    bootMameIfNeeded();
}

void YamahaMU100AudioProcessor::bootMameIfNeeded()
{
    if (mameHasStarted.exchange(true))
        return;  // Already booted on a previous prepareToPlay call.

    // Pin the emulation's output rate now, synchronously, so the drain's resample ratio
    // is known before the MAME thread exists and can never disagree with the rate the
    // command line below asks for. This is the FIXED kMameSampleRate, not the host rate —
    // see its declaration for why (it matches the SWP30's stream rate, which keeps MAME's
    // own LoFi resampler out of the emulation path; the drain resamples instead).
    mameSampleRate.store(kMameSampleRate, std::memory_order_release);

    // Skip MAME boot during plugin scanning / validation. Those hosts only
    // probe metadata; running MAME there wastes resources and (worse) touches
    // the per-instance NVRAM workspace under no real user session.
    const juce::String hostPath = juce::PluginHostType().getHostPath().toLowerCase();
    if (hostPath.contains("scanner") || hostPath.contains("validator"))
        return;

    if (isRomMissing.load())
        return;

    // Create the per-instance MAME workspace now so we can stage a save-state
    // file before launching the thread. (It's also recreated inside
    // runMameEngine() if this path was skipped; createDirectory is a no-op
    // when the directory already exists.)
    mameWorkDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("MEGANXG_" + juce::Uuid().toString());
    mameWorkDir.createDirectory();

    // If setStateInformation() staged saved work-RAM bytes, drop them at the
    // location MAME's nvram_device expects: <nvram_directory>/<system>/<tag>.
    // We set -nvram_directory to mameWorkDir, system name is "mu100", and the
    // tag is "workram" (matching NVRAM(config, m_nvram, ...) in ymmu100.cpp).
    // MAME loads this file in nvram_device::device_start, BEFORE the H8 runs
    // any instructions, so the firmware boots up seeing the saved state. No
    // mid-boot RAM injection needed.
    //
    // Record whether that actually happened: it is the only honest answer to "did the
    // device come up holding the saved patch". A restore whose blob never reached this
    // point gets the whole patch pushed instead — see serviceRestorePatchPush().
    const bool haveWorkRam = (pendingWorkRam.getSize() == WORKRAM_BYTES);
    workRamAppliedAtBoot.store(haveWorkRam, std::memory_order_release);
    if (haveWorkRam)
    {
        juce::File nvFile = mameWorkDir.getChildFile("mu100").getChildFile("workram");
        nvFile.getParentDirectory().createDirectory();
        nvFile.replaceWithData(pendingWorkRam.getData(), pendingWorkRam.getSize());
        pendingWorkRam.setSize(0);
    }

    mameThread = std::thread([this]() { runMameEngine(); });
}

//==============================================================================
int YamahaMU100AudioProcessor::trimMsForIndex(int idx)
{
    static constexpr int kMs[] = { 0, 5, 10, 15, 20, 25, 30, 40, 50 };
    return kMs[juce::jlimit(0, 8, idx)];
}

int YamahaMU100AudioProcessor::getTrimSamples() const
{
    const int ms = trimMsForIndex(pdcTrimIndex.load(std::memory_order_relaxed));
    const double sr = hostSampleRate.load(std::memory_order_relaxed);
    return static_cast<int>(std::round(ms * sr / 1000.0));
}

void YamahaMU100AudioProcessor::reportLatencyToHost()
{
    // The buffer threshold is a count of MAME-rate ring samples; the host wants HOST
    // samples, which is a different number once the two rates differ.
    const double hostRate = hostSampleRate.load(std::memory_order_relaxed);
    const double mameRate = effectiveMameRate();
    const int    buffered = static_cast<int>(std::lround(
        mameBufferThreshold.load(std::memory_order_relaxed)
        * ((mameRate > 0.0) ? hostRate / mameRate : 1.0)));
    const int total = buffered + getTrimSamples();
    auto report = [this, total] { setLatencySamples(total); };
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        mm != nullptr && mm->isThisTheMessageThread())
    {
        report();
    }
    else
    {
        juce::MessageManager::callAsync(report);
    }
}

void YamahaMU100AudioProcessor::setPdcTrimIndex(int idx)
{
    idx = juce::jlimit(0, 8, idx);
    if (pdcTrimIndex.exchange(idx, std::memory_order_relaxed) == idx)
        return;
    reportLatencyToHost();
    saveSettings();
}

void YamahaMU100AudioProcessor::setBufferThreshold(int samples)
{
    // Clamped here as well as on load: this is the only other way in, and the ring
    // sizes depend on the bound holding for every path, not just the dropdown's.
    samples = juce::jlimit(128, kMaxBufferThreshold, samples);
    if (mameBufferThreshold.exchange(samples, std::memory_order_relaxed) == samples)
        return;
    reportLatencyToHost();
    saveSettings();
}

void YamahaMU100AudioProcessor::releaseResources()
{
    // Backstop for the spectrum probe. The analyser normally disables it when the EQ
    // screen goes away, but nothing in swp30 turns it off by itself, so a path that
    // tears the editor down without that call would leave the sound thread summing
    // voices for a reader that no longer exists. Cheap to be certain here.
    setSpectrumTarget(false, 0, -1);

    // ...and the same backstop for the per-part level meter, which is disabled by the
    // same rule (the panel owns the primary disable; nothing in swp30 does it).
    setPartLevelMeter(false);

    // Same backstop for the per-part capture: nothing in swp30 switches it off by
    // itself, so a teardown that skipped the option would leave the mixer
    // accumulating 32 stereo buses into a FIFO nobody drains. The user's SETTING is
    // untouched — only the capture stops; reassertPartOutputsIfDirty turns it back on.
    if (partOutputsEnabled.load(std::memory_order_relaxed))
    {
        auxRingLive.store(false, std::memory_order_release);
        if (MameLease mame { *this })
            swp30_ext::set_part_bus_capture(*mame, false);
        partOutputsDirty.store(true, std::memory_order_release);
    }
}

bool YamahaMU100AudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (!layouts.getMainInputChannelSet().isDisabled())
        return false;

    // Every per-part bus must be stereo or switched off. Anything else is rejected
    // rather than quietly accepted: a host that gave us a mono or 4-channel part bus
    // would otherwise get a layout we do not actually fill.
    for (int b = 1; b < layouts.outputBuses.size(); ++b)
    {
        const auto& set = layouts.outputBuses.getReference(b);
        if (!set.isDisabled() && set != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void YamahaMU100AudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                              juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // Sample the DAW transport so the editor can auto-suppress MIDI-triggered
    // drum-key selection during playback (see midiDrumSelectDuringPlayback).
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            dawIsPlaying.store(pos->getIsPlaying(), std::memory_order_relaxed);
            if (auto bpm = pos->getBpm())
                if (*bpm > 0.0)
                    dawBpm.store(*bpm, std::memory_order_relaxed);
        }
    }
    else
        dawIsPlaying.store(false, std::memory_order_relaxed);

    if (!mameIsFullyBooted.load() || isRomMissing.load())
        return;

    // --- Anchor sync: first processBlock after boot ---
    if (needAnchorSync.load())
    {
        const uint64_t currentWrite = totalWritten.load(std::memory_order_acquire);
        totalRead.store(currentWrite, std::memory_order_release);
        anchorDawSample.store(currentWrite);
        needAnchorSync.store(false);
        // No lease needed: this reads a published atomic, not the machine. It used to
        // call mameMachine->time() here, which is unsafe off the MAME thread and could
        // poison anchorMameTime — and through it EVERY audio-thread MIDI timestamp.
        // mameIsFullyBooted was already checked above, so the engine is up.
        anchorMameTime.store(senderMameTime());
        mameThrottleEvent.signal();
    }

    // DIAG (silent-boot repro): once a second, compare MAME's emulated clock
    // with the dead-reckoned estimate the MIDI gate compares against. A byte is
    // only handed to MAME when mameMachine->time() >= its target, so a positive,
    // growing (estimate - real) gap means MIDI is being held back forever.
    {
        static const bool clockDiag = getenv("MU100_MIDIDIAG") != nullptr;
        const uint64_t nowRead = totalRead.load(std::memory_order_acquire);
        if (clockDiag && nowRead - midiDiagLastRead > (uint64_t) hostSampleRate.load())
        {
            midiDiagLastRead = nowRead;
            const double est = anchorMameTime.load()
                + static_cast<double>(nowRead - anchorDawSample.load()) / effectiveMameRate();
            double real = -1.0;
            if (MameLease mame { *this })
                real = senderMameTime();
            mamediag::logf("midi-clock: mame=%.3f est=%.3f gap=%+.3f  raw=%u in=%u ring=%d booted=%d",
                           real, est, est - real,
                           midiDiagRawIn.load(), midiDiagInBytes.load(),
                           (int) (midiRing.reserved.load() - midiRing.readPos.load()),
                           (int) mameIsFullyBooted.load());
        }
    }

    // --- XG Reset SysEx: inject once at t=3.0s (always goes to port 0) ---
    if (!xgResetSent.load())
    {
        // totalRead counts MAME-rate ring samples, so the elapsed emulated time is
        // per MAME's rate, not the host's — the two are not the same number once the
        // host has switched to a device at another rate.
        const double samplesSinceAnchor = static_cast<double>(
            totalRead.load(std::memory_order_acquire) - anchorDawSample.load());
        const double estimatedMameTime = anchorMameTime.load() + samplesSinceAnchor / effectiveMameRate();
        if (estimatedMameTime >= 3.0 && xgResetSent.exchange(true) == false)
        {
            static const uint8_t xgReset[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
            pushMidiMessageToPort(0, xgReset, (int) sizeof(xgReset),
                                  estimatedMameTime, 1.0 / (31250.0 / 10.0));
        }
    }

    // --- MIDI: route bytes from host (VST3 only; standalone uses per-device callbacks) ---
    if (!juce::JUCEApplicationBase::isStandaloneApp())
    {
        const double   mameRate  = effectiveMameRate();
        const double   hostRate  = hostSampleRate.load(std::memory_order_relaxed);
        const uint64_t nowSample = totalRead.load(std::memory_order_acquire);
        const double   elapsed   = anchorMameTime.load()
            + static_cast<double>(nowSample - anchorDawSample.load()) / mameRate;

        // The two terms are in DIFFERENT units and must not share a divisor: the sample
        // count above is MAME-rate ring samples, while samplePosition below is a HOST
        // sample offset inside this block. They only coincide while the rates match.
        auto route = [&](int port, const juce::MidiMessageMetadata& metadata)
        {
            const auto   msg            = metadata.getMessage();
            const double targetMameTime = elapsed + metadata.samplePosition / hostRate;

            if (msg.isNoteOn()) observeNoteOn(msg.getNoteNumber());
            observeMidiActivity(port, msg);
            pushMidiMessageToPort(port, msg.getRawData(), msg.getRawDataSize(),
                                  targetMameTime, 1.0 / (mameRate * 256.0));
        };

        for (const auto metadata : midiMessages)        // MU100-A (bus 0) → MAME port 0
            route(0, metadata);
        for (const auto metadata : pendingPortBMidi)    // MU100-B (bus 1) → MAME port 1
            route(1, metadata);
        pendingPortBMidi.clear();
    }

    // Emit queued editor SysEx on the MIDI OUT bus so the DAW can record it. Clear
    // first — the input above was already fed to MAME, and we don't want to echo
    // incoming notes back out onto the output bus. Stamp at sample 0; block-boundary
    // timing is plenty for parameter automation. createSysExMessage re-wraps the
    // payload in F0…F7, so hand it the inner bytes (msg validated F0…F7 in sendSysEx).
    midiMessages.clear();
    {
        const juce::ScopedLock sl(outSysExLock);
        for (const auto& m : pendingOutSysEx)
        {
            if (m.empty()) continue;
            if (m.front() == 0xF0)
                midiMessages.addEvent(
                    juce::MidiMessage::createSysExMessage(m.data() + 1, (int) m.size() - 2), 0);
            else   // a 2-3 byte channel message (CC mode, see midiOutFormat)
                midiMessages.addEvent(m.data(), (int) m.size(), 0);
        }
        pendingOutSysEx.clear();
    }

    // Signal MAME to generate more audio if ring buffer is running low
    const int64_t available = static_cast<int64_t>(totalWritten.load(std::memory_order_acquire))
                            - static_cast<int64_t>(totalRead.load(std::memory_order_acquire));
    if (available < mameBufferThreshold.load())
        mameThrottleEvent.signal();

    // --- Drain ring buffer into DAW output ---
    const int numSamples = buffer.getNumSamples();
    float* outL = buffer.getWritePointer(0);
    float* outR = buffer.getWritePointer(1);

    // Ring samples consumed per output sample. MAME cannot change its rate after launch
    // (see mameSampleRate), so a host now running at a different rate is served by
    // resampling rather than by rebooting the emulation, which would discard the patch.
    const double hostRate = hostSampleRate.load(std::memory_order_relaxed);
    const double ratio    = (hostRate > 0.0) ? effectiveMameRate() / hostRate : 1.0;

    // Per-part aux buses. Read off the SAME totalRead cursor as the main output —
    // this records where it started so the aux fill can mirror exactly what the main
    // drain consumed, without ever advancing the cursor itself.
    const bool auxOut = auxRingLive.load(std::memory_order_acquire)
                     && getBusCount(false) > 1;
    const uint64_t auxStartRead = totalRead.load(std::memory_order_relaxed);
    // The resampler path zero-fills against the totalWritten it sampled, not against
    // what it consumed. Capture the same value so aux and main treat exactly the same
    // samples as present — re-reading totalWritten here would race the MAME thread.
    uint64_t auxWritten = 0;

    if (std::abs(ratio - 1.0) < 1.0e-9)
    {
        // Rates match — the original sample-for-sample drain, untouched.
        for (int i = 0; i < numSamples; ++i)
        {
            const uint64_t readPos = totalRead.load(std::memory_order_relaxed);
            const uint64_t written = totalWritten.load(std::memory_order_acquire);
            if (readPos < written)
            {
                outL[i] = dcBlockerL.process(ringBufferL[readPos & (RING_BUFFER_SIZE - 1)]);
                outR[i] = dcBlockerR.process(ringBufferR[readPos & (RING_BUFFER_SIZE - 1)]);
                totalRead.fetch_add(1, std::memory_order_release);
            }
            else
            {
                outL[i] = dcBlockerL.process(0.0f);
                outR[i] = dcBlockerR.process(0.0f);
            }
        }
    }
    else
    {
        const int need = static_cast<int>(std::ceil(numSamples * ratio)) + 8;
        // Only reachable if the host hands us a bigger block than it declared in
        // prepareToPlay; allocating here beats running off the end of the scratch.
        if (static_cast<int>(resampleScratchL.size()) < need)
        {
            resampleScratchL.resize(static_cast<size_t>(need), 0.0f);
            resampleScratchR.resize(static_cast<size_t>(need), 0.0f);
        }

        // Copy contiguously out of the ring (the interpolator needs a flat block),
        // zero-filling anything MAME has not produced yet.
        const uint64_t readPos = totalRead.load(std::memory_order_relaxed);
        const uint64_t written = totalWritten.load(std::memory_order_acquire);
        auxWritten = written;          // aux must zero-fill against the same snapshot
        for (int i = 0; i < need; ++i)
        {
            const uint64_t p    = readPos + static_cast<uint64_t>(i);
            const bool     have = p < written;
            resampleScratchL[(size_t) i] = have ? ringBufferL[p & (RING_BUFFER_SIZE - 1)] : 0.0f;
            resampleScratchR[(size_t) i] = have ? ringBufferR[p & (RING_BUFFER_SIZE - 1)] : 0.0f;
        }

        const int used = resamplerL.process(ratio, resampleScratchL.data(), outL, numSamples);
        resamplerR.process(ratio, resampleScratchR.data(), outR, numSamples);

        // Advance by what was actually consumed, but never past what MAME has written —
        // the backlog arithmetic elsewhere assumes readPos <= written.
        const uint64_t avail = (written > readPos) ? (written - readPos) : 0;
        totalRead.store(readPos + std::min<uint64_t>(static_cast<uint64_t>(used), avail),
                        std::memory_order_release);

        // DC-block AFTER conversion: the blocker's coefficient is prepared at the HOST
        // rate, which is the rate these samples are now at.
        for (int i = 0; i < numSamples; ++i)
        {
            outL[i] = dcBlockerL.process(outL[i]);
            outR[i] = dcBlockerR.process(outR[i]);
        }
    }

    // --- Per-part aux buses ---
    // Deliberately AFTER the main drain and driven from auxStartRead + the amount the
    // main path actually consumed, so the two can never disagree about which ring
    // sample this block starts on. Only enabled buses are touched; processBlock's
    // buffer.clear() has already silenced the rest.
    if (auxOut)
    {
        const uint64_t consumed = totalRead.load(std::memory_order_relaxed) - auxStartRead;

        const int numBuses = getBusCount(false);
        uint32_t  mask     = 0;
        for (int p = 0; p < kNumPartBuses && p + 1 < numBuses; ++p)
        {
            const auto* bus = getBus(false, p + 1);
            if (bus == nullptr || ! bus->isEnabled())
                continue;
            // Publish which buses the host has live, for the main-mix exclusion. The
            // MAME thread reads this; it must not call getBus() itself.
            mask |= (1u << p);
            auto busBuf = getBusBuffer(buffer, false, p + 1);
            if (busBuf.getNumChannels() < 2)
                continue;

            for (int lr = 0; lr < 2; ++lr)
            {
                const int    ch  = p * 2 + lr;
                const float* src = auxRing.get() + size_t(ch) * AUX_RING_SIZE;
                float*       dst = busBuf.getWritePointer(lr);

                if (std::abs(ratio - 1.0) < 1.0e-9)
                {
                    // Mirror the main loop exactly, including its behaviour when the
                    // ring runs dry mid-block: it emits DC-blocked silence, so do the
                    // same rather than holding the last sample.
                    for (int i = 0; i < numSamples; ++i)
                    {
                        const float s = (static_cast<uint64_t>(i) < consumed)
                                      ? src[(auxStartRead + i) & (AUX_RING_SIZE - 1)]
                                      : 0.0f;
                        dst[i] = auxDcBlocker[ch].process(s);
                    }
                }
                else
                {
                    const int need = static_cast<int>(std::ceil(numSamples * ratio)) + 8;
                    if (static_cast<int>(auxResampleScratch.size()) < need)
                        auxResampleScratch.resize(static_cast<size_t>(need), 0.0f);

                    const uint64_t avail = (auxWritten > auxStartRead)
                                         ? (auxWritten - auxStartRead) : 0;
                    for (int i = 0; i < need; ++i)
                        auxResampleScratch[(size_t) i] =
                            (static_cast<uint64_t>(i) < avail)
                                ? src[(auxStartRead + i) & (AUX_RING_SIZE - 1)]
                                : 0.0f;

                    auxResampler[ch].process(ratio, auxResampleScratch.data(), dst, numSamples);

                    // DC-block after conversion, at the host rate — same ordering as
                    // the main path.
                    for (int i = 0; i < numSamples; ++i)
                        dst[i] = auxDcBlocker[ch].process(dst[i]);
                }
            }
        }

        auxBusMask.store(mask, std::memory_order_relaxed);
    }
    else
    {
        // Nothing routed out, so nothing may be held back from the main mix.
        auxBusMask.store(0, std::memory_order_relaxed);
    }

    // The "S" bar's meter: peak of the main output. Armed by setPartLevelMeter, so
    // this pass does not run at all unless the VOLUME screen's meters are on screen —
    // an ungated extra pass over the output buffer on the audio thread is a cost every
    // user pays for a display nobody is looking at.
    if (masterMeterArmed.load(std::memory_order_relaxed))
    {
        float pk = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            pk = std::max(pk, std::max(std::abs(outL[i]), std::abs(outR[i])));
        if (pk > masterPeak.load(std::memory_order_relaxed))
            masterPeak.store(pk, std::memory_order_relaxed);
    }

}

//==============================================================================
juce::AudioProcessorEditor* YamahaMU100AudioProcessor::createEditor()
{
    return new YamahaMU100AudioProcessorEditor(*this);
}

bool YamahaMU100AudioProcessor::hasEditor() const { return true; }

const juce::String YamahaMU100AudioProcessor::getName() const { return JucePlugin_Name; }

bool YamahaMU100AudioProcessor::acceptsMidi()  const { return true; }
bool YamahaMU100AudioProcessor::producesMidi() const { return true; }   // editor edits echoed as SysEx on MIDI OUT (for DAW recording)
bool YamahaMU100AudioProcessor::isMidiEffect() const { return false; }
double YamahaMU100AudioProcessor::getTailLengthSeconds() const { return 0.0; }

int YamahaMU100AudioProcessor::getNumPrograms()  { return 1; }
int YamahaMU100AudioProcessor::getCurrentProgram() { return 0; }
void YamahaMU100AudioProcessor::setCurrentProgram(int) {}
const juce::String YamahaMU100AudioProcessor::getProgramName(int) { return {}; }
void YamahaMU100AudioProcessor::changeProgramName(int, const juce::String&) {}

void YamahaMU100AudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // DAW project state = the full tree (session items + work-RAM snapshot).
    auto root = buildStateXml(/*includeSession*/ true, /*includeWorkram*/ true);
    copyXmlToBinary(*root, destData);
}

std::unique_ptr<juce::XmlElement>
YamahaMU100AudioProcessor::buildStateXml(bool includeSession, bool includeWorkram)
{
    auto rootPtr = std::make_unique<juce::XmlElement>("YamahaMU100State");
    juce::XmlElement& root = *rootPtr;
    root.setAttribute("version", 1);

    // Session / per-app items: present in DAW project state, omitted from the portable
    // .mss file (which the user loads onto whatever session they like, so these must not
    // ride along and change on every file load).
    if (includeSession)
    {
        // NOTE what is deliberately NOT here: romPath, pdcTrimIndex, bufferThreshold and
        // the MIDI input routing. Each describes the MACHINE rather than the song, so
        // they are settings.xml only — see saveSettings() in the header for why the
        // project's sample rate does not change the latency pair, and why MIDI input
        // routing in project state was actively harmful. Older projects may still carry
        // these attributes; they are ignored on load.

        // Per-part stereo outputs: the one pair that is per-project rather than global,
        // because the DAW already stores its half (which aux buses are enabled) in the
        // song. Not in settings.xml at all, so this is their only home.
        root.setAttribute("partOutputs",
                          partOutputsEnabled.load(std::memory_order_relaxed) ? 1 : 0);
        root.setAttribute("partOutputsRemoveFromMain",
                          partOutputsRemoveFromMain.load(std::memory_order_relaxed) ? 1 : 0);

        // "Enable DAW automation": project state only, same reasoning as the pair
        // above -- the lanes it governs are already stored in the song. Not in
        // settings.xml at all, so this is its only home.
        root.setAttribute("dawAutomation", dawAutomationEnabled ? 1 : 0);

        // Loaded-file marker (bottom strip): the path the model was last synced to plus the
        // signature of that synced state, so the filename + unsaved-changes '*' survive a
        // project close/reopen. savedStateSig is stored as captured at the last sync; the
        // live signature is recomputed on reopen so the '*' redraws correctly.
        root.setAttribute("loadedFilePath", loadedFilePath);
        root.setAttribute("savedStateSig",  savedStateSig);
    }

    // Per-part clean-HPF cutoffs (plugin-only feature; NOT part of the SysEx
    // state, so kept here in project state rather than the XG bulk stream).
    {
        juce::StringArray vals;
        bool any = false;
        for (int i = 0; i < 32; ++i) { vals.add(juce::String(cleanHpfHz[i], 2)); if (cleanHpfHz[i] > 0.0f) any = true; }
        if (any) root.setAttribute("cleanHpfHz", vals.joinIntoString(","));
        // Its own attribute rather than a 33rd entry above: absent means off, and an
        // older project that has none must not shift every part's cutoff by one column.
        if (systemHpfHz > 0.0f) root.setAttribute("systemHpfHz", juce::String(systemHpfHz, 2));
    }

    // Per-part 3-band peaking EQ (plugin-only, like HPF2 — kept in project state, out
    // of the SysEx stream). Only written when at least one band is non-flat (gain!=64).
    {
        juce::StringArray gains, freqs, qs;
        bool any = false;
        for (int p = 0; p < 32; ++p)
            for (int b = 0; b < 3; ++b)
            {
                gains.add(juce::String(partEqGain[p][b]));
                freqs.add(juce::String(partEqFreq[p][b]));
                qs.add   (juce::String(partEqQ[p][b]));
                if (partEqGain[p][b] != 64) any = true;
            }
        if (any)
        {
            root.setAttribute("partEqGain", gains.joinIntoString(","));
            // "partEqFreqHz" (Hz), not the older "partEqFreq" (XG table index) — a bare
            // number can't tell the two apart (raw 40 = 2 kHz, but 40 Hz is also legal
            // now), so the change of units rides on a change of attribute name.
            root.setAttribute("partEqFreqHz", freqs.joinIntoString(","));
            root.setAttribute("partEqQ",    qs.joinIntoString(","));
        }
    }

    // Per-drum-key EQ/HPF (plugin-only project state). Sparse: only (buffer,key)
    // entries that differ from neutral are written, as ";"-separated 12-field tokens
    // "buf,key,hpf,g0,f0,q0,g1,f1,q1,g2,f2,q2". (The part->buffer map is not saved —
    // it is re-derived from the editor's DRUMS-buffer state on load.)
    // Written as "drumFx2": same token layout, but the fN fields are Hz rather than the
    // XG table indices "drumFx" held. See the partEqFreqHz note above.
    {
        juce::StringArray toks;
        for (int buf = 0; buf < 4; ++buf)
            for (int key = 0; key < 128; ++key)
            {
                bool nondef = drumHpfHz[buf][key] > 0.0f;
                for (int b = 0; b < 3 && ! nondef; ++b) if (drumEqGain[buf][key][b] != 64) nondef = true;
                if (! nondef) continue;
                juce::StringArray f;
                f.add(juce::String(buf)); f.add(juce::String(key));
                f.add(juce::String(drumHpfHz[buf][key], 2));
                for (int b = 0; b < 3; ++b)
                {
                    f.add(juce::String(drumEqGain[buf][key][b]));
                    f.add(juce::String(drumEqFreq[buf][key][b]));
                    f.add(juce::String(drumEqQ[buf][key][b]));
                }
                toks.add(f.joinIntoString(","));
            }
        if (toks.size() > 0) root.setAttribute("drumFx2", toks.joinIntoString(";"));
    }

    // Effect tempo-sync (plugin-only editor feature; NOT part of the SysEx state —
    // the device only ever sees the resulting raw ms/Hz effect params). Standalone
    // TEMPO knob + the sparse per-(unit,type) SYNC selections.
    root.setAttribute("syncTempo", syncTempo.load(std::memory_order_relaxed));
    {
        // Each on-entry: "unit,type,n0,n1,...,n15" (note index per grid slot, -1 = none).
        juce::StringArray toks;
        for (const auto& [key, e] : effectSync)
        {
            if (! e.on) continue;
            juce::StringArray f;
            f.add(juce::String(key / 1000));   // unit
            f.add(juce::String(key % 1000));   // type index
            for (int s = 0; s < 16; ++s) f.add(juce::String((int) e.slotNote[s]));
            toks.add(f.joinIntoString(","));
        }
        if (toks.size() > 0) root.setAttribute("effectSync", toks.joinIntoString(";"));
    }

    // MIDI input device routing is settings.xml only and is NOT written here. It can
    // only act in the standalone (initMidiDevices returns early in a plugin, and the
    // MIDI Input... dialog is gated the same way), and the standalone does not persist
    // project state at all -- so this block could never be read where it mattered, while
    // in a DAW it overwrote the shared midiDeviceSettings map on load and the next
    // saveSettings() flushed a stale song's routing into settings.xml. Device
    // identifiers are machine- and port-scoped strings, so the override also broke
    // silently -- no notes, no error -- on any device or computer change.

    // --- MU100 work-RAM snapshot via memshare ---
    // The nvram_device bound to share("workram") in ymmu100.cpp persists this
    // 128 KB across MAME runs; the bytes carry patch state + voice edits. Omitted from
    // the portable .mss file (it is large and MAME-internal; the patch is fully
    // reconstructed from editorState on load).
    if (includeWorkram && mameHasStarted.load(std::memory_order_acquire))
    {
        MameLease mame(*this);
        if (mame)
        {
            auto* share = mame->root_device().memshare("workram");
            if (share && share->bytes() == WORKRAM_BYTES)
            {
                juce::MemoryBlock blob(share->ptr(), share->bytes());
                root.setAttribute("workram", blob.toBase64Encoding());
            }
        }
    }

    // There was a `globalState` attribute here — the global XG effect/EQ/system blocks
    // (aH 00/02/03) as a separate param-change stream. GONE as of 2026-08-07: it was only
    // ever read to feed a restore push that turned out to be unnecessary AND harmful (see
    // the pendingGlobalRestore note in PluginProcessor.h), and `editorState` below already
    // carries those addresses along with everything else. Dropping the write too is safe
    // here because this plugin has never been distributed, so no older build will ever
    // read a file this one writes; that was the only reason it outlived its reader.
    // Files that still HAVE the attribute load fine — nothing looks for it.
    if (editorState != nullptr && editorState->size() > 0)
    {
        // Full editor model, so a reopened instance restores the exact patch into the
        // editor WITHOUT a device read-back (which is lossy and can race a later load).
        const mu100::Bytes full = editorState->toBulkDumps();
        if (! full.empty())
        {
            juce::MemoryBlock blob(full.data(), full.size());
            root.setAttribute("editorState", blob.toBase64Encoding());
        }
    }

    return rootPtr;
}

// Apply the plugin-only extension attrs from a state tree (project restore OR a native
// .mss file) and raise the re-push dirty flags so the SWP30 gets the values after boot.
// Absent attrs fall back to defaults, so a tree that carries none neutralises them.
void YamahaMU100AudioProcessor::applyPluginExtras(const juce::XmlElement& xml)
{
    // Per-part clean-HPF cutoffs; re-pushed to the SWP30 after boot via the dirty flag.
    for (int i = 0; i < 32; ++i) cleanHpfHz[i] = 0.0f;
    {
        const juce::String s = xml.getStringAttribute("cleanHpfHz", {});
        if (s.isNotEmpty())
        {
            juce::StringArray vals;
            vals.addTokens(s, ",", "");
            for (int i = 0; i < juce::jmin(32, vals.size()); ++i)
                cleanHpfHz[i] = static_cast<float>(vals[i].getDoubleValue());
        }
    }
    systemHpfHz = (float) xml.getDoubleAttribute("systemHpfHz", 0.0);
    cleanHpfDirty.store(true, std::memory_order_release);

    // Per-part 3-band peaking EQ; re-pushed to the SWP30 after boot via the dirty flag.
    initPartEqDefaults();
    {
        const juce::String g = xml.getStringAttribute("partEqGain", {});
        const juce::String q = xml.getStringAttribute("partEqQ",    {});
        // Current projects carry "partEqFreqHz" (Hz). Projects saved while the mid bands
        // still rode the XG table carry "partEqFreq" (raw index) instead — convert those
        // so an existing session keeps the frequencies it was tuned to.
        juce::String       f      = xml.getStringAttribute("partEqFreqHz", {});
        const bool         legacy = f.isEmpty();
        if (legacy) f = xml.getStringAttribute("partEqFreq", {});

        if (g.isNotEmpty() && f.isNotEmpty() && q.isNotEmpty())
        {
            juce::StringArray gains, freqs, qs;
            gains.addTokens(g, ",", ""); freqs.addTokens(f, ",", ""); qs.addTokens(q, ",", "");
            for (int p = 0; p < 32; ++p)
                for (int b = 0; b < 3; ++b)
                {
                    const int i = p * 3 + b;
                    if (i < gains.size()) partEqGain[p][b] = gains[i].getIntValue();
                    if (i < qs.size())    partEqQ[p][b]    = qs[i].getIntValue();
                    if (i < freqs.size())
                    {
                        const int v = freqs[i].getIntValue();
                        partEqFreq[p][b] = legacy ? (int) eqLegacyRawToHz(v) : v;
                    }
                }
        }
    }
    partEqDirty.store(true, std::memory_order_release);

    // Per-drum-key EQ/HPF (sparse tokens; see buildStateXml). partDrumBuffer is
    // re-derived from the editor's DRUMS-buffer state (EditorView pushes it), so it is
    // reset to melodic here and re-established after read-back.
    initDrumFxDefaults();
    {
        // "drumFx2" = Hz freq fields; the older "drumFx" = XG table indices (see the
        // partEqFreqHz note in buildStateXml).
        juce::String       s      = xml.getStringAttribute("drumFx2", {});
        const bool         legacy = s.isEmpty();
        if (legacy) s = xml.getStringAttribute("drumFx", {});

        if (s.isNotEmpty())
        {
            juce::StringArray toks; toks.addTokens(s, ";", "");
            for (const auto& t : toks)
            {
                juce::StringArray f; f.addTokens(t, ",", "");
                if (f.size() < 12) continue;
                const int buf = f[0].getIntValue(), key = f[1].getIntValue();
                if (! drumIdx(buf, key)) continue;
                drumHpfHz[buf][key] = (float) f[2].getDoubleValue();
                for (int b = 0; b < 3; ++b)
                {
                    drumEqGain[buf][key][b] = f[3 + b * 3].getIntValue();
                    drumEqQ[buf][key][b]    = f[5 + b * 3].getIntValue();
                    const int v = f[4 + b * 3].getIntValue();
                    drumEqFreq[buf][key][b] = legacy ? (int) eqLegacyRawToHz(v) : v;
                }
            }
        }
    }
    drumFxDirty.store(true, std::memory_order_release);

    // Effect tempo-sync (plugin-only; see buildStateXml). Standalone TEMPO +
    // the sparse per-(unit,type) SYNC selections. The editor reads these back when
    // it (re)builds the effect grid.
    if (xml.hasAttribute("syncTempo"))
        syncTempo.store(juce::jlimit(10.0, 250.0, xml.getDoubleAttribute("syncTempo", 120.0)),
                        std::memory_order_relaxed);
    effectSync.clear();
    {
        const juce::String s = xml.getStringAttribute("effectSync", {});
        if (s.isNotEmpty())
        {
            juce::StringArray toks; toks.addTokens(s, ";", "");
            for (const auto& t : toks)
            {
                juce::StringArray f; f.addTokens(t, ",", "");
                if (f.size() < 18) continue;   // unit,type + 16 slots
                const int unit = f[0].getIntValue(), type = f[1].getIntValue();
                SyncEntry e; e.on = true;
                for (int sl = 0; sl < 16; ++sl) e.slotNote[sl] = (int8_t) f[2 + sl].getIntValue();
                effectSync[unit * 1000 + type] = e;
            }
        }
    }
}

// Classify a path by its extension for openFileType. Only .mss counts as a native
// complete-setup file (Save Setup overwrites it); anything else that is a loaded/exported
// file is treated as Syx (Save Setup will not overwrite it); empty = untitled.
YamahaMU100AudioProcessor::OpenFileType
YamahaMU100AudioProcessor::openFileTypeForPath(const juce::String& path)
{
    if (path.isEmpty())                     return OpenFileType::None;
    if (path.endsWithIgnoreCase(".mss"))    return OpenFileType::Native;
    return OpenFileType::Syx;
}

juce::String YamahaMU100AudioProcessor::makeSetupDefaultFileName(SetupKind k)
{
    const juce::String stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d-%H%M");
    return "meganxg-" + stamp + "." + setupKindExtension(k);
}

// Serialize the native "complete setup" as text XML (patch + plugin-only extensions,
// no session/work-RAM items). Text keeps the file human-inspectable and version-tolerant.
bool YamahaMU100AudioProcessor::saveSetupToFile(const juce::File& file)
{
    auto root = buildStateXml(/*includeSession*/ false, /*includeWorkram*/ false);
    return file.replaceWithText(root->toString());
}

//==============================================================================
// Partial native setup files (.mps / .mds / .mes). See the SetupKind block in
// PluginProcessor.h for the format and why they share the .mss root tag.
//==============================================================================
namespace
{
    // XG address arithmetic on a DeviceState key's 21-bit address.
    constexpr uint32_t xgAddr(uint8_t aH, uint8_t aM, uint8_t aL)
    {
        return ((uint32_t) aH << 14) | ((uint32_t) aM << 7) | (uint32_t) aL;
    }

    // MULTI PART lows these paths need by name (a saved part block is stored at part 0).
    constexpr uint8_t kPartLowBankMsb = 0x01, kPartLowProgram = 0x03,
                      kPartLowPartMode = 0x07, kPartLowPortaTime = 0x68;

    // Split a parameter-change stream into whole F0…F7 messages.
    template <typename Fn>
    void forEachSysExMessage(const mu100::Bytes& s, Fn&& fn)
    {
        size_t msgStart = 0; bool inSysex = false;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == 0xF0) { inSysex = true; msgStart = i; }
            else if (s[i] == 0xF7 && inSysex)
            {
                inSysex = false;
                fn(s.data() + msgStart, i - msgStart + 1);
            }
        }
    }

    // Walk every XG byte of `src`, handing (aH, aM, aL, value) to `fn`.
    template <typename Fn>
    void forEachXgByte(const mu100::DeviceState& src, Fn&& fn)
    {
        for (const auto& kv : src.raw())
        {
            if (((kv.first >> 24) & 0xF) != (uint32_t) mu100::ModelIx::XG)
                continue;
            const uint32_t a = kv.first & 0x1FFFFF;
            fn((uint8_t) ((a >> 14) & 0x7F), (uint8_t) ((a >> 7) & 0x7F),
               (uint8_t) (a & 0x7F), kv.second);
        }
    }

    // One part's bytes, re-addressed to part 0 so the file is part-agnostic.
    // MULTI PART is TWO blocks: 08 nn xx and the part HPF at 0A nn 20. The `aL >= 0x20`
    // test is load-bearing — 0A 00 00..0E is REMOTE SWITCH (front-panel button
    // emulation), which shares the address of part 0's second block and is not part data.
    void collectPartBlock(const mu100::DeviceState& src, int part, mu100::DeviceState& out)
    {
        forEachXgByte(src, [&](uint8_t aH, uint8_t aM, uint8_t aL, uint8_t v)
        {
            if (aM != (uint8_t) part) return;
            if      (aH == 0x08)                 out.setByte(mu100::ModelIx::XG, xgAddr(0x08, 0, aL), v);
            else if (aH == 0x0A && aL >= 0x20)   out.setByte(mu100::ModelIx::XG, xgAddr(0x0A, 0, aL), v);
        });
    }

    // One DRUM SETUP buffer's bytes (3n rr xx, n = buffer). `destHigh` re-addresses them:
    // 0x30 for a file (buffer-agnostic, like the part block), or 0x30 + buffer to keep
    // them where they are for a re-send to the device.
    void collectDrumBlock(const mu100::DeviceState& src, int buffer, mu100::DeviceState& out,
                          uint8_t destHigh = 0x30)
    {
        const uint8_t hi = (uint8_t) (0x30 + buffer);
        forEachXgByte(src, [&](uint8_t aH, uint8_t aM, uint8_t aL, uint8_t v)
        {
            if (aH == hi) out.setByte(mu100::ModelIx::XG, xgAddr(destHigh, aM, aL), v);
        });
    }

    // Every effect unit plus Multi EQ, at their real addresses (nothing indexes them).
    bool isEffectBlock(uint8_t aH, uint8_t aM)
    {
        return (aH == 0x02 && (aM == 0x01 || aM == 0x40))    // reverb/chorus/variation, Multi EQ
            || (aH == 0x03 && (aM == 0x00 || aM == 0x01));   // insertion 1 / 2
    }

    void collectEffectBlocks(const mu100::DeviceState& src, mu100::DeviceState& out)
    {
        forEachXgByte(src, [&](uint8_t aH, uint8_t aM, uint8_t aL, uint8_t v)
        {
            if (isEffectBlock(aH, aM)) out.setByte(mu100::ModelIx::XG, xgAddr(aH, aM, aL), v);
        });
    }

    // A DeviceState as a base64 parameter-change stream — round-trips through
    // DeviceState::load with no new parser. (This encoding was inherited from the old
    // "globalState" attribute, which is gone; these partial-setup attributes are now
    // its only users, and they are STORAGE — never a device send. See "A whole block
    // goes out as bulk dumps" in CONTRIBUTING.md.)
    juce::String stateToB64(const mu100::DeviceState& st)
    {
        if (st.size() == 0) return {};
        const mu100::Bytes s = st.toParamChanges();
        if (s.empty()) return {};
        return juce::MemoryBlock(s.data(), s.size()).toBase64Encoding();
    }

    bool b64ToState(const juce::String& b64, mu100::DeviceState& out)
    {
        if (b64.isEmpty()) return false;
        juce::MemoryBlock mb;
        if (! mb.fromBase64Encoding(b64) || mb.getSize() == 0) return false;
        out.load(static_cast<const uint8_t*>(mb.getData()), (size_t) mb.getSize());
        return out.size() > 0;
    }
}

YamahaMU100AudioProcessor::SetupKind
YamahaMU100AudioProcessor::setupKindOfXml(const juce::XmlElement& xml)
{
    const juce::String k = xml.getStringAttribute("setupKind", {});
    if (k.equalsIgnoreCase("part"))   return SetupKind::Part;
    if (k.equalsIgnoreCase("drum"))   return SetupKind::Drum;
    if (k.equalsIgnoreCase("effect")) return SetupKind::Effect;
    return SetupKind::System;   // absent = a .mss written before the partial formats
}

const char* YamahaMU100AudioProcessor::setupKindExtension(SetupKind k)
{
    switch (k)
    {
        case SetupKind::Part:   return "mps";
        case SetupKind::Drum:   return "mds";
        case SetupKind::Effect: return "mes";
        default:                return "mss";
    }
}

const char* YamahaMU100AudioProcessor::setupKindLabel(SetupKind k)
{
    switch (k)
    {
        case SetupKind::Part:   return "Part Setup";
        case SetupKind::Drum:   return "Drum Setup";
        case SetupKind::Effect: return "Effect Setup";
        default:                return "System Setup";
    }
}

juce::String YamahaMU100AudioProcessor::nativeSetupFilters()
{
    return "*.mss;*.mps;*.mds;*.mes";
}

bool YamahaMU100AudioProcessor::writeSetupXml(const juce::File& file, const juce::XmlElement& xml)
{
    return file.replaceWithText(xml.toString());
}

//==============================================================================
// Write the DRUM SETUP buffer `buffer` into `root`: the 3n rr xx block re-addressed to
// buffer 0, plus the plugin-only per-key HPF2 / mid-band EQ. Shared by .mds (which is
// only this) and .mps (which adds it when the part sits on a buffer).
bool YamahaMU100AudioProcessor::writeDrumBufferToXml(juce::XmlElement& root, int buffer)
{
    if (buffer < 0 || buffer >= 4 || editorState == nullptr) return false;

    mu100::DeviceState block;
    collectDrumBlock(*editorState, buffer, block);
    const juce::String b64 = stateToB64(block);
    if (b64.isEmpty()) return false;   // the buffer has never been read back / seeded
    root.setAttribute("drumState", b64);
    root.setAttribute("srcDrumBuffer", buffer + 1);   // 1-based, i.e. DRUMS1..4

    // Plugin-only per-(buffer,key) HPF2 + mid-band EQ, sparse like the .mss "drumFx2"
    // but with the buffer field dropped (the file carries exactly one buffer):
    // "key,hpf,g0,f0,q0,g1,f1,q1,g2,f2,q2".
    juce::StringArray toks;
    for (int key = 0; key < 128; ++key)
    {
        bool nondef = drumHpfHz[buffer][key] > 0.0f;
        for (int b = 0; b < 3 && ! nondef; ++b) if (drumEqGain[buffer][key][b] != 64) nondef = true;
        if (! nondef) continue;
        juce::StringArray f;
        f.add(juce::String(key));
        f.add(juce::String(drumHpfHz[buffer][key], 2));
        for (int b = 0; b < 3; ++b)
        {
            f.add(juce::String(drumEqGain[buffer][key][b]));
            f.add(juce::String(drumEqFreq[buffer][key][b]));
            f.add(juce::String(drumEqQ   [buffer][key][b]));
        }
        toks.add(f.joinIntoString(","));
    }
    // Written even when empty, so a load knows to CLEAR the target buffer's bands
    // rather than leave the previous kit's shaping on the new one.
    root.setAttribute("drumFxBuf", toks.joinIntoString(";"));
    return true;
}

// The DRUMS buffer a part sits on, per the model's own PART MODE byte (2..5 =
// DRUMS1..4); -1 when the part is melodic or holds a kit with no buffer allocated.
int YamahaMU100AudioProcessor::partDrumBufferFromModel(int part) const
{
    if (part < 0 || part >= 32 || editorState == nullptr) return -1;
    uint8_t mode = 0;
    if (! editorState->getByte(mu100::ModelIx::XG, xgAddr(0x08, (uint8_t) part, kPartLowPartMode), mode))
        return -1;
    return (mode >= 2 && mode <= 5) ? (int) mode - 2 : -1;
}

std::unique_ptr<juce::XmlElement> YamahaMU100AudioProcessor::buildPartSetupXml(int part)
{
    if (part < 0 || part >= 32 || editorState == nullptr) return nullptr;

    auto rootPtr = std::make_unique<juce::XmlElement>("YamahaMU100State");
    auto& root = *rootPtr;
    root.setAttribute("version", 1);
    root.setAttribute("setupKind", "part");
    root.setAttribute("srcPart", part + 1);   // informational only (1-based, as shown)

    mu100::DeviceState block;
    collectPartBlock(*editorState, part, block);
    const juce::String b64 = stateToB64(block);
    if (b64.isEmpty()) return nullptr;        // nothing to save (model not populated yet)
    root.setAttribute("partState", b64);

    // A part on a DRUMS buffer carries that buffer's per-key data too, because the two
    // CANNOT be separated: the part block holds the kit's bank/program, and writing a
    // PROGRAM re-initialises the assigned Drum Setup. There is no such thing as loading
    // this part without disturbing the buffer, so the file states what the buffer should
    // hold rather than leaving it to whatever the session happened to have.
    // The buffer comes from the MODEL's PART MODE, so it cannot disagree with the
    // PART MODE byte inside partState.
    writeDrumBufferToXml(root, partDrumBufferFromModel(part));

    // The plugin-only per-part extensions, written unconditionally — a Part setup
    // REPLACES the target part, so "HPF2 off / EQ flat" has to travel too, or loading a
    // clean part onto a shaped one would silently leave the shaping behind.
    //
    // Deliberately NOT named partEqGain/partEqFreqHz/partEqQ: those attributes exist in
    // the .mss / project tree holding all 32 parts x 3 bands, and applyPluginExtras reads
    // them that way. Same name, different arity is how a future edit quietly loads one
    // part's bands as if they were part 0's — so these carry their own names.
    root.setAttribute("setupCleanHpfHz", juce::String(cleanHpfHz[part], 2));
    {
        juce::StringArray gains, freqs, qs;
        for (int b = 0; b < 3; ++b)
        {
            gains.add(juce::String(partEqGain[part][b]));
            freqs.add(juce::String(partEqFreq[part][b]));
            qs   .add(juce::String(partEqQ   [part][b]));
        }
        root.setAttribute("setupEqGain",   gains.joinIntoString(","));
        root.setAttribute("setupEqFreqHz", freqs.joinIntoString(","));
        root.setAttribute("setupEqQ",      qs.joinIntoString(","));
    }
    return rootPtr;
}

// A Drum setup is the per-key layer ALONE — no part settings, and in particular no
// bank/program. That is what makes it useful and what makes it distinct from a Part
// setup: carrying no program means loading one fires no program change, so it is the
// only file that can rewrite a buffer's per-key data without the buffer first being
// re-initialised. It can therefore move a drum treatment onto a different kit, or onto
// another part, without dragging the original kit or the part's channel strip along.
std::unique_ptr<juce::XmlElement> YamahaMU100AudioProcessor::buildDrumSetupXml(int buffer)
{
    if (buffer < 0 || buffer >= 4 || editorState == nullptr) return nullptr;

    auto rootPtr = std::make_unique<juce::XmlElement>("YamahaMU100State");
    auto& root = *rootPtr;
    root.setAttribute("version", 1);
    root.setAttribute("setupKind", "drum");

    if (! writeDrumBufferToXml(root, buffer)) return nullptr;
    return rootPtr;
}

std::unique_ptr<juce::XmlElement> YamahaMU100AudioProcessor::buildEffectSetupXml()
{
    if (editorState == nullptr) return nullptr;

    auto rootPtr = std::make_unique<juce::XmlElement>("YamahaMU100State");
    auto& root = *rootPtr;
    root.setAttribute("version", 1);
    root.setAttribute("setupKind", "effect");

    mu100::DeviceState block;
    collectEffectBlocks(*editorState, block);
    const juce::String b64 = stateToB64(block);
    if (b64.isEmpty()) return nullptr;
    root.setAttribute("effectState", b64);

    // Plugin-only tempo-SYNC selections, same sparse encoding buildStateXml uses.
    // syncTempo itself is deliberately NOT carried: the tempo is the session's (the DAW
    // playhead, or the standalone TEMPO knob), not a property of the effect settings.
    {
        juce::StringArray toks;
        for (const auto& [key, e] : effectSync)
        {
            if (! e.on) continue;
            juce::StringArray f;
            f.add(juce::String(key / 1000));
            f.add(juce::String(key % 1000));
            for (int s = 0; s < 16; ++s) f.add(juce::String((int) e.slotNote[s]));
            toks.add(f.joinIntoString(","));
        }
        root.setAttribute("effectSync", toks.joinIntoString(";"));
    }
    return rootPtr;
}

//==============================================================================
// Drum-kit DEFAULTS — keeping the model's 3n block describing the kit that is
// actually loaded.
//
// Writing a part's PROGRAM re-initialises the DRUM SETUP buffer its PART MODE names,
// to the NEW KIT's defaults. Until MU100DrumDefaults.h existed the plugin had no way
// to know those values, so after a kit change the model went on holding the OLD kit's
// per-key bytes -- and a save wrote them to file, a reload streamed them back onto the
// new kit's freshly re-inited buffer, and the two halves of every drum key described
// different kits in between.
//
// The table is harvested off this same emulated device (tools/gen_drum_defaults.py),
// so repopulating from it puts the model exactly where the device just went.
//==============================================================================

// Which kit the MODEL says is loaded on `buffer`, via the first part whose PART MODE
// names it. Reads the model rather than partDrumBuffer[] so it is a pure model query,
// usable from the undo path (where the model has just been rewound and the editor's
// map has not necessarily caught up) and from sendPatchToDevice (which is a
// model->device push and must not depend on GUI state).
bool YamahaMU100AudioProcessor::drumBufferKitFromModel(int buffer, int& msb, int& lsb,
                                                       int& pgm) const
{
    if (buffer < 0 || buffer >= 4 || editorState == nullptr) return false;

    for (int p = 0; p < 32; ++p)
    {
        uint8_t mode = 0;
        if (! editorState->getByte(mu100::ModelIx::XG, xgAddr(0x08, (uint8_t) p, kPartLowPartMode), mode))
            continue;
        if (mode != (uint8_t) (buffer + 2)) continue;   // 2..5 = DRUMS1..4

        uint8_t m = 0, l = 0, g = 0;
        if (! editorState->getByte(mu100::ModelIx::XG, xgAddr(0x08, (uint8_t) p, kPartLowBankMsb), m)
         || ! editorState->getByte(mu100::ModelIx::XG, xgAddr(0x08, (uint8_t) p, 0x02), l)
         || ! editorState->getByte(mu100::ModelIx::XG, xgAddr(0x08, (uint8_t) p, kPartLowProgram), g))
            continue;

        msb = (int) m; lsb = (int) l; pgm = (int) g + 1;   // the table's pgm is 1-based
        return true;
    }
    return false;
}

// Overwrite the model's whole 3n block for `buffer` with `kit`'s defaults. Writes the
// MODEL only: the device has already re-initialised itself, which is the event this
// mirrors, so transmitting would be redundant at best. Returns false, changing
// nothing, if the kit is not in the table -- leaving the stale bytes, which is the
// pre-2026-09-08 behaviour and strictly better than writing a guess over them.
bool YamahaMU100AudioProcessor::loadDrumBufferDefaults(int buffer, int msb, int lsb, int pgm)
{
    if (buffer < 0 || buffer >= 4 || editorState == nullptr) return false;

    for (int note = mu100::kDrumDefFirstNote; note <= mu100::kDrumDefLastNote; ++note)
        if (mu100::drumDefaultsFor(msb, lsb, pgm, note) == nullptr)
            return false;                                  // not a kit we harvested

    for (int note = mu100::kDrumDefFirstNote; note <= mu100::kDrumDefLastNote; ++note)
    {
        const uint8_t* row = mu100::drumDefaultsFor(msb, lsb, pgm, note);
        for (int s = 0; s < mu100::kDrumDefSubCount; ++s)
        {
            const auto& sub = mu100::kDrumDefSubs[s];
            for (int i = 0; i < sub.count; ++i)
                editorState->setByte(mu100::ModelIx::XG,
                                     xgAddr((uint8_t) (0x30 + buffer), (uint8_t) note,
                                            (uint8_t) (sub.lo + i)),
                                     row[sub.offset + i]);
        }
    }
    return true;
}

// The byte this (buffer, key, low address) LOADS WITH under the kit currently on the
// buffer. This is the baseline a "changed from default" marker has to measure against,
// and before MU100DrumDefaults.h there was nothing to measure against: the controls
// used one static per-parameter default from the spec, which is right for a MULTI PART
// value and wrong for a drum key. REVERB SEND alone has TEN distinct defaults across
// the kits and LEVEL has 57, and the static 64 is not even the common one -- so on a
// freshly loaded kit a row of controls sitting at their kit defaults all claimed to be
// edited. Returns -1 when the kit is not in the table, and the caller keeps its static
// default. USER-REPORTED 2026-09-08.
int YamahaMU100AudioProcessor::drumDefaultByte(int buffer, int key, uint8_t lo) const
{
    int msb = 0, lsb = 0, pgm = 0;
    if (! drumBufferKitFromModel(buffer, msb, lsb, pgm)) return -1;
    const uint8_t* row = mu100::drumDefaultsFor(msb, lsb, pgm, key);
    if (row == nullptr) return -1;
    const int off = mu100::drumDefaultOffsetForLow(lo);
    return off >= 0 ? (int) row[off] : -1;
}

// Re-derive a buffer's block from whatever kit the MODEL now says is on it. This is
// what an undo/redo replays -- see Step::drumReseed for why the buffer number is
// recorded and the 2686 bytes are not.
void YamahaMU100AudioProcessor::reseedDrumBufferFromModel(int buffer)
{
    int msb = 0, lsb = 0, pgm = 0;
    if (drumBufferKitFromModel(buffer, msb, lsb, pgm))
        loadDrumBufferDefaults(buffer, msb, lsb, pgm);
}

// The bytes of `buffer` that the OUTGOING kit's defaults do not explain -- the user's
// own per-key edits. `newVal` is what the reseed is about to write, so applying the
// delta forwards (redo) is a no-op on top of the reseed while applying it backwards
// (undo) restores the edit.
//
// Only bytes the model actually HOLDS count: an absent byte is not an edit, and giving
// it an oldVal of -1 would make undo write a factory default over a value nobody set.
//
// WHEN THE OUTGOING KIT IS UNKNOWN (oldPgm <= 0, or a kit not in the table) the
// baseline falls back to the byte about to be written, which captures everything the
// reseed would CHANGE. That is a superset of the edits and always restores the block
// exactly; it is only bigger than the ideal capture, never wrong.
//
// The fallback is not a corner case, and treating "unknown" as "nothing to preserve"
// was a REPORTED BUG (2026-09-08). Between a bufferless pick and the next kit
// assignment NO PART NAMES THE BUFFER, so drumBufferKitFromModel has nothing to answer
// with -- while the block still holds edits the user made seconds earlier. An
// unreferenced buffer is not an abandoned one.
void YamahaMU100AudioProcessor::collectDrumBufferEdits(
        int buffer, int oldMsb, int oldLsb, int oldPgm,
        int newMsb, int newLsb, int newPgm,
        std::vector<mu100undo::ByteDelta>& out) const
{
    if (editorState == nullptr) return;

    for (int note = mu100::kDrumDefFirstNote; note <= mu100::kDrumDefLastNote; ++note)
    {
        const uint8_t* newRow = mu100::drumDefaultsFor(newMsb, newLsb, newPgm, note);
        if (newRow == nullptr) { out.clear(); return; }   // not a kit we harvested
        const uint8_t* oldRow = oldPgm > 0
                              ? mu100::drumDefaultsFor(oldMsb, oldLsb, oldPgm, note)
                              : nullptr;

        for (int sIx = 0; sIx < mu100::kDrumDefSubCount; ++sIx)
        {
            const auto& sb = mu100::kDrumDefSubs[sIx];
            for (int i = 0; i < sb.count; ++i)
            {
                const uint32_t addr = xgAddr((uint8_t) (0x30 + buffer), (uint8_t) note,
                                             (uint8_t) (sb.lo + i));
                uint8_t have = 0;
                if (! editorState->getByte(mu100::ModelIx::XG, addr, have)) continue;
                // The baseline: the outgoing kit's default where that is known, else
                // the value about to be written. Either way, "differs from the
                // baseline" means "would be lost by the reseed".
                const uint8_t base = (oldRow != nullptr ? oldRow : newRow)[sb.offset + i];
                if (have == base) continue;
                out.push_back({ mu100::DeviceState::key(mu100::ModelIx::XG, addr),
                                (int) have, (int) newRow[sb.offset + i] });
            }
        }
    }
}

// A part is LEAVING `buffer` without any kit being assigned to it -- a bufferless pick,
// or a move to a different buffer. Nothing happens to this buffer now, which is exactly
// why it is easy to miss: the damage is done by the UNDO. Undoing the pick puts PART
// MODE back on this buffer and re-sends the part's PROGRAM, and that program write
// re-initialises the buffer on the DEVICE -- wiping the very per-key edits the undo was
// supposed to bring back, while the model keeps them and the two halves split.
//
// So the edits travel with this step too. old == new here, because a pick that only
// moves a part does not change the block: undo re-applies them after the re-init, and
// redo re-applies the same values harmlessly. No reseed is recorded -- the model is
// already right, and only the device needs putting back.
//
// MEASURED 2026-09-08 as CASE C of the DRUMUNDO probe: without this the model kept the
// edits and the device did not.
void YamahaMU100AudioProcessor::preserveDrumBufferOnLeave(int buffer, int msb, int lsb, int pgm)
{
    if (undoSuppressDepth != 0 || buffer < 0 || buffer >= 4 || pgm <= 0) return;

    std::vector<mu100undo::ByteDelta> edits;
    collectDrumBufferEdits(buffer, msb, lsb, pgm, msb, lsb, pgm, edits);
    for (auto& d : edits) d.newVal = d.oldVal;   // this pick changes nothing here
    if (edits.empty()) return;

    mu100undo::Step step;
    step.drumEdits = std::move(edits);
    undoCommit(std::move(step), 0);
}

// Record "re-derive this buffer, then re-apply these edits" into the step the caller's
// UndoScope has open.
void YamahaMU100AudioProcessor::undoPushDrumChange(int buffer,
                                                   std::vector<mu100undo::ByteDelta>&& edits)
{
    if (undoSuppressDepth > 0 || buffer < 0 || buffer >= 4) return;
    mu100undo::Step step;
    step.drumReseed.push_back(buffer);
    step.drumEdits = std::move(edits);
    undoCommit(std::move(step), 0);
}

// Re-apply a step's preserved per-key edits, to the MODEL and the DEVICE, after the
// reseed has put the block back on the restored kit's defaults. One single-byte
// parameter change each -- the message is fully described by the address and the
// value, so nothing has to be stored beside the delta to rebuild it.
void YamahaMU100AudioProcessor::applyDrumEdits(const mu100undo::Step& s, bool useOld)
{
    for (const auto& d : s.drumEdits)
    {
        const int v = useOld ? d.oldVal : d.newVal;
        if (v < 0) continue;
        const auto     ix = static_cast<mu100::ModelIx>(d.stateKey >> 24);
        const uint32_t a  = d.stateKey & 0x1FFFFF;
        editorState->setByte(ix, a, (uint8_t) (v & 0x7F));
        const std::vector<uint8_t> msg {
            0xF0, 0x43, 0x10, 0x4C,
            (uint8_t) ((a >> 14) & 0x7F), (uint8_t) ((a >> 7) & 0x7F), (uint8_t) (a & 0x7F),
            (uint8_t) (v & 0x7F), 0xF7 };
        sendSysEx(msg);
    }
}

//==============================================================================
// Applying a partial setup.
//
// Every apply follows the same three steps: fold the file's bytes into editorState,
// stream them to the device on ONE paced MAME-time timeline, then re-baseline the
// things a wholesale model move would otherwise disturb (undo history, DAW automation).
//
// The whole affected block is transmitted, never a diff against the current model:
// writing a part's PROGRAM re-initialises the Drum Setup assigned to it, and writing an
// effect TYPE loads that program's parameter defaults, so "the device already has this
// byte" is not a safe assumption after either. A full DRUM SETUP buffer measures 316
// messages ~ 2.7 s of wire; a part or the effect blocks are well under half a second.
//==============================================================================
// The DRUMS buffer a file targets by default. A .mds says so outright (srcDrumBuffer);
// a .mps says it through the PART MODE byte inside its own part block, which is the
// authority there — a buffer attribute could drift from it. -1 = no buffer involved.
int YamahaMU100AudioProcessor::drumBufferOfSetupXml(const juce::XmlElement& xml)
{
    if (setupKindOfXml(xml) == SetupKind::Drum)
    {
        const int b = xml.getIntAttribute("srcDrumBuffer", 0) - 1;   // stored 1-based
        return (b >= 0 && b < 4) ? b : -1;
    }

    mu100::DeviceState in;
    if (! b64ToState(xml.getStringAttribute("partState", {}), in)) return -1;
    uint8_t mode = 0;
    if (! in.getByte(mu100::ModelIx::XG, xgAddr(0x08, 0, kPartLowPartMode), mode)) return -1;
    return (mode >= 2 && mode <= 5) ? (int) mode - 2 : -1;   // 2..5 = DRUMS1..4
}

// True when loading this file will write a DRUMS buffer, i.e. when the user should be
// asked which one. A .mds always does; a .mps only when its part sits on a buffer.
bool YamahaMU100AudioProcessor::setupTargetsDrumBuffer(const juce::XmlElement& xml)
{
    const SetupKind k = setupKindOfXml(xml);
    return (k == SetupKind::Drum || k == SetupKind::Part) && drumBufferOfSetupXml(xml) >= 0;
}

// Fold a file's drum block into `buffer` (model + device) and apply its per-key plugin
// data. `startAt` is where on the MAME-time timeline the stream begins; returns where it
// ended, so a caller that has already queued a part stream can chain onto it.
double YamahaMU100AudioProcessor::applyDrumBufferFromXml(const juce::XmlElement& xml,
                                                         int buffer, double startAt)
{
    double t = startAt;
    mu100::DeviceState in;
    if (! drumIdx(buffer, 0) || ! b64ToState(xml.getStringAttribute("drumState", {}), in))
        return t;

    mu100::DeviceState out;
    forEachXgByte(in, [&](uint8_t aH, uint8_t aM, uint8_t aL, uint8_t v)
    {
        juce::ignoreUnused(aH);   // the file stores the block at 0x30 whatever it came from
        const uint32_t addr = xgAddr((uint8_t) (0x30 + buffer), aM, aL);
        editorState->setByte(mu100::ModelIx::XG, addr, v);
        out.setByte(mu100::ModelIx::XG, addr, v);
    });

    MameLease mame(*this);
    if (mame)
    {
        const double timePerByte = 1.0 / (31250.0 / 10.0);
        constexpr double kMsgGap = 0.003;
        // BULK DUMPS, not parameter changes: this is a WHOLE block, so its runs are
        // long, and the firmware takes only the addressed parameter's own size from a
        // multi-byte parameter change — a key's 16-byte run applied byte 0 and dropped
        // the rest. See sendPatchToDevice step 4 for the measurement.
        forEachSysExMessage(out.toBulkDumps(), [&](const uint8_t* d, size_t n)
        {
            pushMidiMessageToPort(0, d, (int) n, t, timePerByte);
            t += (double) n * timePerByte + kMsgGap;
        });
    }

    // Plugin-only per-(buffer,key) HPF2 + mid-band EQ. The target buffer is cleared
    // first: the file's list is sparse, so anything it does not name must go back to
    // neutral or the previous kit's shaping survives onto the loaded one.
    for (int key = 0; key < 128; ++key)
    {
        drumHpfHz[buffer][key] = 0.0f;
        for (int b = 0; b < 3; ++b)
        {
            drumEqGain[buffer][key][b] = 64;
            drumEqFreq[buffer][key][b] = kPartEqFreqDefault[b];
            drumEqQ   [buffer][key][b] = 7;
        }
    }
    {
        juce::StringArray toks;
        toks.addTokens(xml.getStringAttribute("drumFxBuf", {}), ";", "");
        for (const auto& tok : toks)
        {
            juce::StringArray f; f.addTokens(tok, ",", "");
            if (f.size() < 11) continue;                    // key,hpf + 3x(g,f,q)
            const int key = f[0].getIntValue();
            if (! drumIdx(buffer, key)) continue;
            drumHpfHz[buffer][key] = (float) f[1].getDoubleValue();
            for (int b = 0; b < 3; ++b)
            {
                drumEqGain[buffer][key][b] = f[2 + b * 3].getIntValue();
                drumEqFreq[buffer][key][b] = f[3 + b * 3].getIntValue();
                drumEqQ   [buffer][key][b] = f[4 + b * 3].getIntValue();
            }
        }
    }
    drumFxDirty.store(true, std::memory_order_release);
    return t;
}

int YamahaMU100AudioProcessor::applyPartSetup(const juce::XmlElement& xml, int part,
                                              int targetBuffer)
{
    if (part < 0 || part >= 32 || editorState == nullptr) return -1;

    mu100::DeviceState in;
    if (! b64ToState(xml.getStringAttribute("partState", {}), in)) return -1;

    // The DRUMS buffer this part lands on (from the file's own PART MODE), or -1 when
    // the setup is melodic. Read before the model is written, but it reads the FILE, so
    // the order is immaterial — it is the same value drumBufferOfSetupXml returns.
    int buffer = -1;
    {
        uint8_t mode = 0;
        if (in.getByte(mu100::ModelIx::XG, xgAddr(0x08, 0, kPartLowPartMode), mode)
            && mode >= 2 && mode <= 5)
            buffer = (int) mode - 2;
    }

    // Honour a caller's buffer choice by REWRITING the PART MODE byte, so the part points
    // at the chosen buffer and the drum data below lands in the same place. -1 keeps the
    // file's own choice.
    if (buffer >= 0 && targetBuffer >= 0 && targetBuffer < 4 && targetBuffer != buffer)
    {
        buffer = targetBuffer;
        in.setByte(mu100::ModelIx::XG, xgAddr(0x08, 0, kPartLowPartMode),
                   (uint8_t) (buffer + 2));
    }

    // Does the file actually carry that buffer's per-key block? Decided with the same
    // test applyDrumBufferFromXml applies, so the two cannot disagree about it, and
    // read here because the answer decides what has to happen AFTER the stream.
    bool fileHasDrumBlock = false;
    if (buffer >= 0)
    {
        mu100::DeviceState probe;
        fileHasDrumBlock = b64ToState(xml.getStringAttribute("drumState", {}), probe);
    }

    // The kit LEAVING that buffer, read BEFORE the loop below writes the model -- the
    // same rule EditorView's voice pick follows, and for the same reason: afterwards the
    // model no longer agrees with itself about what the buffer held.
    int oldMsb = 0, oldLsb = 0, oldPgm = -1;
    if (buffer >= 0 && ! fileHasDrumBlock)
        drumBufferKitFromModel(buffer, oldMsb, oldLsb, oldPgm);

    // Re-address the file's part-0 block onto the target part, both in the model and in
    // the stream about to go out. The VOICE bytes are held back: written as three
    // separate messages with PROGRAM last, they make the firmware actually load the
    // voice (a run folded in with its neighbours does not) — the same discipline
    // sendPatchToDevice step 2 uses, and for the same reason.
    mu100::DeviceState outParams;
    int voice[3] = { -1, -1, -1 };   // bank MSB, bank LSB, program
    forEachXgByte(in, [&](uint8_t aH, uint8_t aM, uint8_t aL, uint8_t v)
    {
        juce::ignoreUnused(aM);
        const uint32_t addr = xgAddr(aH, (uint8_t) part, aL);
        editorState->setByte(mu100::ModelIx::XG, addr, v);

        if (aH == 0x08 && aL >= kPartLowBankMsb && aL <= kPartLowProgram)
            voice[aL - kPartLowBankMsb] = v;
        else
            outParams.setByte(mu100::ModelIx::XG, addr, v);
    });

    MameLease mame(*this);
    int sendMs = 0;
    if (mame)
    {
        // Drain any coalesced knob edit first: it would otherwise flush into the middle
        // of this stream (see sendParamChange's flush-then-send discipline).
        flushPendingParamChanges();

        const double timePerByte = 1.0 / (31250.0 / 10.0);
        constexpr double kMsgGap = 0.003;
        const double startTime = senderMameTime() + 0.1;
        double t = startTime;
        auto stream = [&](const uint8_t* d, size_t n)
        {
            pushMidiMessageToPort(0, d, (int) n, t, timePerByte);
            t += (double) n * timePerByte + kMsgGap;
        };

        // BULK DUMPS — a whole part block, so the same run rule applies as in
        // applyDrumBufferFromXml above. The voice bytes are already held out of
        // outParams, so the runs simply split around the gap at lows 01..03.
        forEachSysExMessage(outParams.toBulkDumps(), stream);

        using mu100::AddressMap;
        static const char* kVoiceParam[3] = { "BANK SELECT MSB", "BANK SELECT LSB", "PROGRAM NUMBER" };
        for (int i = 0; i < 3; ++i)
        {
            if (voice[i] < 0) continue;
            const int ix = AddressMap::find(kVoiceParam[i], "MULTI PART");
            if (ix < 0) continue;
            const auto msg = AddressMap::makeParamChange(AddressMap::param(ix), part,
                                                         (uint32_t) voice[i]);
            stream(msg.data(), msg.size());
        }

        // PORTAMENTO TIME needs a delta to latch — re-sending the identical value is a
        // no-op in this firmware. Same kick-then-restore sendPatchToDevice step 3 does.
        uint8_t ptime = 0;
        if (in.getByte(mu100::ModelIx::XG, xgAddr(0x08, 0, kPartLowPortaTime), ptime) && ptime != 0)
        {
            const int ix = AddressMap::find("PORTAMENTO TIME", "MULTI PART");
            if (ix >= 0)
            {
                t += 0.20;   // let the params + voice load settle first
                const int real = ptime & 0x7F;
                for (int value : { (real < 0x7F) ? real + 1 : real - 1, real })
                {
                    const auto msg = AddressMap::makeParamChange(AddressMap::param(ix), part,
                                                                 (uint32_t) value);
                    stream(msg.data(), msg.size());
                    t += 0.010;
                }
            }
        }

        // The PROGRAM write above RE-INITIALISES the Drum Setup assigned to the part
        // (spec p46-48, the rule resetDrumBuffer already models), so a part on a DRUMS
        // buffer must have that buffer written AFTERWARDS or it is left holding the
        // kit's defaults. The file carries the data precisely because the two cannot be
        // separated — see buildPartSetupXml.
        if (buffer >= 0)
        {
            t += 0.20;   // let the voice load finish before rewriting its setup
            t = applyDrumBufferFromXml(xml, buffer, t);
        }

        if (t > startTime)
            sendMs = juce::roundToInt((t - senderMameTime()) * 1000.0);
    }
    else if (buffer >= 0)
    {
        applyDrumBufferFromXml(xml, buffer, 0.0);   // engine down: model-only
    }

    // A kit assigned to a DRUMS buffer belongs to the BUFFER, so it re-points every other
    // part already on that buffer — the device does this itself, which is why nothing is
    // transmitted here. Without the model following, those parts' Voice rows kept showing
    // the kit they used to have until a Read from Device corrected them.
    // EditorView's voice-browser pick applies the identical rule.
    if (buffer >= 0)
        propagateDrumKitToBufferPeers(part, buffer);

    // A file with NO drum block leaves the two halves split, and nothing above closes
    // the gap: the PROGRAM write re-initialised the buffer on the DEVICE to the new
    // kit's factory defaults, while the model still holds the OUTGOING kit's bytes.
    // applyDrumBufferFromXml is what normally puts it back, and it returned having
    // written nothing. So repopulate from the same defaults table a voice-browser kit
    // pick uses, and clear the per-key plugin EQ/HPF the same way -- otherwise the
    // editor shows, the markers judge, and a save stores a kit the device is not
    // playing, healing key by key as the user happens to touch controls.
    //
    // Today's builds always write the block, so this fires only for a .mps saved before
    // the table existed (its buffer had never been seeded), one written by hand, or a
    // future producer of part XML. Placed AFTER the peer propagation because the
    // fallback below asks the model which kit the buffer now holds, and the peers are
    // what make that answer unambiguous.
    //
    // No edit preservation, unlike resetDrumBuffer's caller in EditorView: a setup load
    // ends in finishSetupApply -> undoClear(), so there is no step for preserved bytes
    // to travel in -- and the device destroyed them the moment the program change
    // landed. The outgoing kit is still passed so the reseed is described honestly if
    // setup loads ever become undoable.
    if (buffer >= 0 && ! fileHasDrumBlock && voice[2] >= 0)
    {
        // No PROGRAM byte in the file would mean no program change went out, so the
        // device re-initialised nothing and the model is still right -- hence the
        // voice[2] test above. Bank bytes can be filled in from the model, which now
        // names the loaded kit.
        int kitMsb = voice[0], kitLsb = voice[1];
        if (kitMsb < 0 || kitLsb < 0)
        {
            int m = 0, l = 0, g = -1;
            if (drumBufferKitFromModel(buffer, m, l, g))
            {
                if (kitMsb < 0) kitMsb = m;
                if (kitLsb < 0) kitLsb = l;
            }
        }
        if (kitMsb >= 0 && kitLsb >= 0)
            resetDrumBuffer(buffer, kitMsb, kitLsb, voice[2] + 1,   // the table is 1-based
                            oldMsb, oldLsb, oldPgm);
    }

    // Plugin-only per-part extensions. Absent attrs fall back to the neutral defaults,
    // so a file written before these existed still clears the target rather than leaving
    // the previous part's shaping on the loaded one.
    cleanHpfHz[part] = (float) xml.getDoubleAttribute("setupCleanHpfHz", 0.0);
    cleanHpfDirty.store(true, std::memory_order_release);
    {
        juce::StringArray gains, freqs, qs;
        gains.addTokens(xml.getStringAttribute("setupEqGain",   {}), ",", "");
        freqs.addTokens(xml.getStringAttribute("setupEqFreqHz", {}), ",", "");
        qs   .addTokens(xml.getStringAttribute("setupEqQ",      {}), ",", "");
        for (int b = 0; b < 3; ++b)
        {
            partEqGain[part][b] = (b < gains.size()) ? gains[b].getIntValue() : 64;
            partEqFreq[part][b] = (b < freqs.size()) ? freqs[b].getIntValue() : kPartEqFreqDefault[b];
            partEqQ   [part][b] = (b < qs.size())    ? qs[b].getIntValue()    : 7;
        }
    }
    partEqDirty.store(true, std::memory_order_release);

    finishSetupApply();
    return sendMs;
}

// A Drum setup carries the per-key layer ONLY — no part settings and, crucially, no
// bank/program. Nothing here triggers a program change, so the buffer is not
// re-initialised first and the file's values simply replace what is there. That is what
// lets a drum treatment move onto a different kit, or onto another part, without
// dragging the original kit or the part's channel strip with it.
//
// `targetBuffer` (-1 = the buffer the file was saved from) is the user's pick.
int YamahaMU100AudioProcessor::applyDrumSetup(const juce::XmlElement& xml, int targetBuffer)
{
    if (editorState == nullptr) return -1;

    const int saved  = drumBufferOfSetupXml(xml);
    const int buffer = (targetBuffer >= 0 && targetBuffer < 4) ? targetBuffer : saved;
    if (buffer < 0 || ! xml.hasAttribute("drumState")) return -1;

    int sendMs = 0;
    {
        MameLease mame(*this);
        if (mame)
        {
            // Drain any coalesced knob edit first, so it cannot flush into the middle of
            // this stream (sendParamChange's flush-then-send discipline).
            flushPendingParamChanges();
            const double startTime = senderMameTime() + 0.1;
            const double t = applyDrumBufferFromXml(xml, buffer, startTime);
            if (t > startTime)
                sendMs = juce::roundToInt((t - senderMameTime()) * 1000.0);
        }
        else
        {
            applyDrumBufferFromXml(xml, buffer, 0.0);   // engine down: model-only
        }
    }

    finishSetupApply();
    return sendMs;
}

// A kit assigned to a DRUMS buffer belongs to the buffer, not to one part: every part
// whose PART MODE names that buffer plays it. Copy `part`'s bank/program to its peers in
// the MODEL so their Voice rows show what they will actually sound like.
//
// Model only, deliberately — the device already re-points them itself, and transmitting a
// program change per peer would re-initialise the very buffer just written.
void YamahaMU100AudioProcessor::propagateDrumKitToBufferPeers(int part, int buffer)
{
    if (editorState == nullptr || part < 0 || part >= 32 || buffer < 0 || buffer >= 4) return;

    uint8_t voice[3] {};
    for (int i = 0; i < 3; ++i)
        if (! editorState->getByte(mu100::ModelIx::XG,
                                   xgAddr(0x08, (uint8_t) part, (uint8_t) (kPartLowBankMsb + i)),
                                   voice[i]))
            return;   // the part has no complete voice to copy

    for (int q = 0; q < 32; ++q)
    {
        if (q == part || partDrumBufferFromModel(q) != buffer) continue;
        for (int i = 0; i < 3; ++i)
            editorState->setByte(mu100::ModelIx::XG,
                                 xgAddr(0x08, (uint8_t) q, (uint8_t) (kPartLowBankMsb + i)),
                                 voice[i]);
    }
}

int YamahaMU100AudioProcessor::applyEffectSetup(const juce::XmlElement& xml)
{
    if (editorState == nullptr) return -1;

    mu100::DeviceState in;
    if (! b64ToState(xml.getStringAttribute("effectState", {}), in)) return -1;

    mu100::DeviceState out;
    forEachXgByte(in, [&](uint8_t aH, uint8_t aM, uint8_t aL, uint8_t v)
    {
        if (! isEffectBlock(aH, aM)) return;   // ignore anything outside the effect blocks
        const uint32_t addr = xgAddr(aH, aM, aL);
        editorState->setByte(mu100::ModelIx::XG, addr, v);
        out.setByte(mu100::ModelIx::XG, addr, v);
    });

    MameLease mame(*this);
    int sendMs = 0;
    if (mame)
    {
        flushPendingParamChanges();
        const double timePerByte = 1.0 / (31250.0 / 10.0);
        constexpr double kMsgGap = 0.003;
        const double startTime = senderMameTime() + 0.1;
        double t = startTime;
        // Address-ascending order puts each unit's TYPE word — reverb 02 01 00, chorus
        // 20, variation 40, insertion 03 0n 00, Multi EQ 02 40 00 — ahead of that unit's
        // parameters, which is required: writing a type loads its program defaults over
        // whatever the parameters held. toBulkDumps() groups by the same runs in the same
        // map order, so that ordering is unchanged; what changes is that a whole block's
        // long runs now actually land (see applyDrumBufferFromXml above). A type and its
        // parameters can now share one message, which is what a .syx export has always
        // shipped and what the device emits itself, so the defaults load stays ahead of
        // the parameter bytes that follow it.
        forEachSysExMessage(out.toBulkDumps(), [&](const uint8_t* d, size_t n)
        {
            pushMidiMessageToPort(0, d, (int) n, t, timePerByte);
            t += (double) n * timePerByte + kMsgGap;
        });
        if (t > startTime)
            sendMs = juce::roundToInt((t - senderMameTime()) * 1000.0);
    }

    // Plugin-only tempo-SYNC selections replace the current set wholesale (the file
    // covers every unit, so a leftover entry would belong to the previous effects).
    effectSync.clear();
    {
        juce::StringArray toks;
        toks.addTokens(xml.getStringAttribute("effectSync", {}), ";", "");
        for (const auto& tok : toks)
        {
            juce::StringArray f; f.addTokens(tok, ",", "");
            if (f.size() < 18) continue;   // unit,type + 16 slots
            SyncEntry e; e.on = true;
            for (int s = 0; s < 16; ++s) e.slotNote[s] = (int8_t) f[2 + s].getIntValue();
            effectSync[f[0].getIntValue() * 1000 + f[1].getIntValue()] = e;
        }
    }

    finishSetupApply();
    return sendMs;
}

// Shared tail of every partial apply.
void YamahaMU100AudioProcessor::finishSetupApply()
{
    editorStatePopulated = true;

    // An undo record is a byte at an ABSOLUTE address, so a record taken before this
    // load would write the PREVIOUS setup's value into the one just loaded — a
    // well-formed, in-range, undetectable one-byte corruption. Same reasoning (and the
    // same remedy) as the wholesale loads in seedEditorDefaults / loadEditorState.
    undoClear();

    // Re-baseline the DAW parameters BEFORE anyone can observe the moved model, or the
    // next AutomationParams tick reads the load as "the user moved these controls" and a
    // Cubase lane in Write/Touch RECORDS it. See undo() / setStateInformation.
    if (automation) automation->resyncFromModel();

    editorStateExternalSeq.fetch_add(1, std::memory_order_release);
}


//==============================================================================
// PART COPY / PASTE / RESET — the .mps trio without the file.
//
// WHAT THEY CARRY is exactly what a .mps carries, deliberately: the MULTI PART block
// (both halves — 08 nn xx and the part high-pass at 0A nn 20), the plugin-only per-part
// HPF2 and mid-band EQ, and, when the part sits on a DRUMS buffer, that buffer's whole
// per-key layer. The buffer is NOT separable from the part: the part block holds the
// kit's bank/program, and writing a PROGRAM re-initialises the assigned Drum Setup on
// the device — measured, and true even when the program is unchanged. So "paste the
// part but leave the buffer alone" is not a state that exists, and carrying it is what
// makes a paste land where the user is looking.
//
// WHY THIS IS NOT applyPartSetup, although it does the same job. That path streams BULK
// DUMPS on a paced timeline and ends in finishSetupApply() -> undoClear(): right for a
// file load, fatal here, because the whole point of an in-session copy is that it is an
// EDIT and has to undo like one. So this replays the block as ordinary parameter changes
// through sendParamChange inside ONE UndoScope — capture is automatic (it happens at the
// funnel), the framing comes from the AddressMap so a NIBBLE-packed parameter is one
// message rather than two loose bytes, and the step reads "Paste Part 5 to Part 7" in
// the HISTORY window.
//
// THE DRUM HALF CANNOT BE ByteDeltas: 79 keys x 34 bytes is 2686 of them, ~32 KB for one
// step against a 10 KB budget for the entire stack, and an undo that re-transmitted them
// would be ~8 s of wire. It uses the mechanism a kit change already uses — record the
// BUFFER (Step::drumReseed) plus only the bytes the kit's own defaults do not explain
// (Step::drumEdits) — which is smaller AND more correct, because undo re-sends the
// program change and the device re-initialises itself.
//
// The per-key overlay is therefore written with undo SUPPRESSED. Captured as ordinary
// ByteDeltas it would be replayed by undo AFTER the program change had already put the
// device on the outgoing kit's defaults, writing the INCOMING kit's defaults over them —
// wrong for every key the user had left at its default. The edits vector is the single
// description of that half, in both directions.
//==============================================================================
namespace
{
    // Every address that belongs to ONE part's block. The low-byte floor on 0x0A is
    // load-bearing: 0A 00 00..0E is REMOTE SWITCH (front-panel button emulation), not
    // part data, and it shares part 0's addresses. Same pair of tests collectPartBlock
    // and classifySysEx already make.
    bool partBlockAddress(uint8_t aH, uint8_t aL)
    {
        return aH == 0x08 || (aH == 0x0A && aL >= 0x20);
    }
}

// One part's worth of AddressMap parameters, split into the groups the send order needs.
// Built by WALKING THE MAP rather than by naming parameters: one added to the MULTI PART
// table tomorrow is copied with no edit here, and one removed stops being copied — the
// same anti-drift argument the generated Disp rules and isAutomated make.
void YamahaMU100AudioProcessor::buildPartParamPlan(PartParamPlan& plan)
{
    using mu100::AddressMap;
    using mu100::ModelIx;

    for (int i = 0; i < AddressMap::count(); ++i)
    {
        const auto& p = AddressMap::param(i);
        if (p.mid.mask == 0) continue;     // not templated on the part byte

        ModelIx mdl; uint32_t a0 = 0;
        AddressMap::resolve(p, /*instance*/ 0, mdl, a0);
        if (mdl != ModelIx::XG) continue;

        const auto aH = (uint8_t) ((a0 >> 14) & 0x7F);
        const auto aM = (uint8_t) ((a0 >> 7) & 0x7F);
        const auto aL = (uint8_t) (a0 & 0x7F);
        if (aM != 0 || ! partBlockAddress(aH, aL)) continue;

        for (int b = 0; b < (int) p.size; ++b)
            plan.covered.insert(a0 + (uint32_t) b);

        if (aH == 0x08 && aL == kPartLowPartMode)                         plan.mode = i;
        else if (aH == 0x08 && aL >= kPartLowBankMsb && aL <= kPartLowProgram)
            plan.voice[aL - kPartLowBankMsb] = i;
        else
        {
            plan.ordinary.push_back(i);
            if (aH == 0x08 && aL == kPartLowPortaTime) plan.portaTime = i;
        }
    }
}

bool YamahaMU100AudioProcessor::applyPartBlockUndoable(const mu100::DeviceState& in, int part,
                                                       int targetBuffer,
                                                       const PartPluginExtras& extras,
                                                       const juce::XmlElement* drumXml,
                                                       const juce::String& label)
{
    using mu100::AddressMap;
    using mu100::ModelIx;

    if (part < 0 || part >= 32 || editorState == nullptr) return false;

    // The kit LEAVING the destination buffer, read BEFORE anything is written — the rule
    // the voice-browser pick and applyPartSetup both follow, for the same reason: once
    // the part's own bank/program have moved, the model no longer agrees with itself
    // about what the buffer held.
    const int oldBuffer = partDrumBufferFromModel(part);
    int oldMsb = 0, oldLsb = 0, oldPgm = -1;
    if (oldBuffer >= 0) drumBufferKitFromModel(oldBuffer, oldMsb, oldLsb, oldPgm);

    // Where the INCOMING block lands, and the PART MODE byte that says so. A caller's
    // choice is honoured by rewriting that byte, exactly as applyPartSetup does, so the
    // part points at the chosen buffer and the per-key data below follows it there.
    uint8_t modeByte = 0;
    const bool haveMode = in.getByte(ModelIx::XG, xgAddr(0x08, 0, kPartLowPartMode), modeByte);
    int newBuffer = (haveMode && modeByte >= 2 && modeByte <= 5) ? (int) modeByte - 2 : -1;
    if (newBuffer >= 0 && targetBuffer >= 0 && targetBuffer < 4)
    {
        newBuffer = targetBuffer;
        modeByte  = (uint8_t) (newBuffer + 2);
    }

    PartParamPlan plan;
    buildPartParamPlan(plan);

    UndoScope scope(*this, label);

    auto sendMapped = [&](int ix, uint32_t value)
    {
        const auto msg = AddressMap::makeParamChange(AddressMap::param(ix), part, value);
        if (! msg.empty()) sendParamChange(msg);
    };
    auto sendRawByte = [&](uint32_t addr, uint8_t v)
    {
        sendParamChange({ 0xF0, 0x43, 0x10, 0x4C,
                          (uint8_t) ((addr >> 14) & 0x7F), (uint8_t) ((addr >> 7) & 0x7F),
                          (uint8_t) (addr & 0x7F), (uint8_t) (v & 0x7F), 0xF7 });
    };

    // ---- 1. PART MODE FIRST. It is an ARGUMENT to the program change below, not
    // bookkeeping that follows it: the mode in force when the PROGRAM lands decides
    // which buffer the kit loads into. Sending it last is bug 841686f.
    if (plan.mode >= 0 && haveMode) sendMapped(plan.mode, modeByte);

    // ---- 2. Everything else the map knows, in address order.
    for (const int ix : plan.ordinary)
    {
        const auto v = AddressMap::get(in, AddressMap::param(ix), 0);
        if (v.present) sendMapped(ix, (uint32_t) v.combined);
    }

    // ---- 3. Any byte the map does NOT own, one message each. Complete by
    // construction: a model byte with no parameter behind it would otherwise be dropped
    // silently, and a block has to arrive whole or the paste is a partial one.
    forEachXgByte(in, [&](uint8_t aH, uint8_t aM, uint8_t aL, uint8_t v)
    {
        if (aM != 0 || ! partBlockAddress(aH, aL)) return;
        if (plan.covered.count(xgAddr(aH, 0, aL)) != 0) return;
        sendRawByte(xgAddr(aH, (uint8_t) part, aL), v);
    });

    // ---- 4. The VOICE last, three separate messages with PROGRAM at the end: a run
    // folded in with its neighbours does not make the firmware load the voice. Same
    // discipline sendPatchToDevice step 2 and applyPartSetup use.
    int newMsb = -1, newLsb = -1, newPgm = -1;
    for (int k = 0; k < 3; ++k)
    {
        if (plan.voice[k] < 0) continue;
        const auto v = AddressMap::get(in, AddressMap::param(plan.voice[k]), 0);
        if (! v.present) continue;
        sendMapped(plan.voice[k], (uint32_t) v.combined);
        if      (k == 0) newMsb = v.combined;
        else if (k == 1) newLsb = v.combined;
        else             newPgm = v.combined;
    }

    // ---- 5. The DRUMS buffer. The program change above has just re-initialised it ON
    // THE DEVICE to the incoming kit's defaults while the MODEL still holds the outgoing
    // kit's bytes, so the two have split; everything here closes that gap in the model,
    // on the device, and in the undo step, which has to walk it back both ways.
    if (newBuffer >= 0 && newPgm >= 0)
    {
        // What the buffer should end up holding, re-addressed from the clipboard's
        // buffer-agnostic 0x30 to the buffer it is landing on.
        mu100::DeviceState want;
        bool haveWant = false;
        if (drumXml != nullptr)
        {
            mu100::DeviceState file;
            if (b64ToState(drumXml->getStringAttribute("drumState", {}), file))
            {
                haveWant = true;
                forEachXgByte(file, [&](uint8_t aH, uint8_t aM, uint8_t aL, uint8_t v)
                {
                    if (aH == 0x30)
                        want.setByte(ModelIx::XG,
                                     xgAddr((uint8_t) (0x30 + newBuffer), aM, aL), v);
                });
            }
        }

        // The undo record: every byte that EITHER kit's defaults fail to explain, in
        // both directions at once. A byte in only one of those sets takes its other
        // side's kit default, which the reseed supplies — so this is exactly the set
        // that survives a reseed, and nothing else has to be stored.
        std::vector<mu100undo::ByteDelta> edits;
        for (int note = mu100::kDrumDefFirstNote; note <= mu100::kDrumDefLastNote; ++note)
        {
            const uint8_t* newRow = mu100::drumDefaultsFor(newMsb, newLsb, newPgm + 1, note);
            const uint8_t* oldRow = oldPgm > 0
                                  ? mu100::drumDefaultsFor(oldMsb, oldLsb, oldPgm, note)
                                  : nullptr;

            for (int sIx = 0; sIx < mu100::kDrumDefSubCount; ++sIx)
            {
                const auto& sb = mu100::kDrumDefSubs[sIx];
                for (int i = 0; i < sb.count; ++i)
                {
                    const uint32_t addr = xgAddr((uint8_t) (0x30 + newBuffer), (uint8_t) note,
                                                 (uint8_t) (sb.lo + i));
                    uint8_t have = 0;
                    const bool hadByte = editorState->getByte(ModelIx::XG, addr, have);

                    const int newDef = (newRow != nullptr) ? (int) newRow[sb.offset + i] : -1;
                    int wantVal = newDef;
                    uint8_t w = 0;
                    if (haveWant && want.getByte(ModelIx::XG, addr, w)) wantVal = (int) w;

                    // Unknown kit either side: fall back to "the value that is there" /
                    // "the value about to be written", so the capture is a SUPERSET of
                    // the real edits. Bigger than ideal, never wrong — the rule
                    // collectDrumBufferEdits already follows, and for the same reason.
                    const int oldDef = (oldRow != nullptr) ? (int) oldRow[sb.offset + i]
                                                           : (hadByte ? (int) have : -1);
                    const int oldVal = hadByte ? (int) have : -1;

                    const bool oldIsEdit = (oldVal >= 0 && oldVal != oldDef);
                    const bool newIsEdit = (wantVal >= 0 && wantVal != newDef);
                    if (! oldIsEdit && ! newIsEdit) continue;
                    edits.push_back({ mu100::DeviceState::key(ModelIx::XG, addr),
                                      oldVal, wantVal >= 0 ? wantVal : 0 });
                }
            }
        }

        // The MODEL follows the device onto the new kit's defaults, then takes the
        // overlay. UNDO-SUPPRESSED: see the block comment above — captured as ByteDeltas
        // the overlay would be replayed on top of an undo that had already restored the
        // outgoing kit, writing the wrong defaults over every unedited key.
        {
            UndoSuppress guard(*this);
            reseedDrumBufferFromModel(newBuffer);

            if (haveWant)
            {
                forEachXgByte(want, [&](uint8_t aH, uint8_t aM, uint8_t aL, uint8_t v)
                {
                    editorState->setByte(ModelIx::XG, xgAddr(aH, aM, aL), v);
                    // Only what the defaults do not already say needs transmitting: the
                    // device re-initialised itself a moment ago, so this is the same
                    // diff sendPatchToDevice step 4 sends, not a 316-message block.
                    const int def = drumDefaultByte(newBuffer, (int) aM, (int) aL);
                    if (def < 0 || def != (int) v)
                        sendSysEx({ 0xF0, 0x43, 0x10, 0x4C, aH, aM, aL,
                                    (uint8_t) (v & 0x7F), 0xF7 });
                });
            }
        }

        undoPushDrumChange(newBuffer, std::move(edits));

        // A kit on a buffer belongs to the BUFFER, so every other part on it now plays
        // this kit. The device does that itself; this is the model catching up.
        propagateDrumKitToBufferPeers(part, newBuffer);

        // The per-key plugin-only layer. Cleared first, then re-applied from the
        // clipboard: a paste REPLACES the buffer, so "no shaping" has to travel too or
        // the previous kit's filters are silently left on the new one. Each setter
        // early-outs on an unchanged value and records its own PluginDelta, so this
        // folds into the same step and costs nothing on an untouched kit.
        for (int key = 0; key < 128; ++key)
        {
            setDrumHpf(newBuffer, key, 0.0f);
            for (int b = 0; b < 3; ++b)
            {
                setDrumEqGain(newBuffer, key, b, 64);
                setDrumEqFreq(newBuffer, key, b, kPartEqFreqDefault[b]);
                setDrumEqQ   (newBuffer, key, b, 7);
            }
        }
        if (drumXml != nullptr)
        {
            juce::StringArray recs;
            recs.addTokens(drumXml->getStringAttribute("drumFxBuf", {}), ";", "");
            for (const auto& rec : recs)
            {
                juce::StringArray f;
                f.addTokens(rec, ",", "");
                if (f.size() < 11) continue;
                const int key = f[0].getIntValue();
                if (key < 0 || key > 127) continue;
                setDrumHpf(newBuffer, key, (float) f[1].getDoubleValue());
                for (int b = 0; b < 3; ++b)
                {
                    setDrumEqGain(newBuffer, key, b, f[2 + b * 3].getIntValue());
                    setDrumEqFreq(newBuffer, key, b, f[3 + b * 3].getIntValue());
                    setDrumEqQ   (newBuffer, key, b, f[4 + b * 3].getIntValue());
                }
            }
        }
    }
    else if (oldBuffer >= 0)
    {
        // The part has LEFT a buffer without any kit being assigned to it. Nothing
        // happens to that buffer now, which is exactly why it is easy to miss: the
        // damage is done by the UNDO, which puts PART MODE back and re-sends the
        // PROGRAM, re-initialising the buffer under the edits it was restoring.
        preserveDrumBufferOnLeave(oldBuffer, oldMsb, oldLsb, oldPgm);
    }

    // ---- 6. The plugin-only per-part extensions. Written unconditionally, for the
    // reason buildPartSetupXml states: a paste REPLACES the part, so "HPF2 off, EQ flat"
    // has to travel or a clean part landing on a shaped one keeps the shaping.
    setCleanHpfHz(part, extras.hpf2);
    for (int b = 0; b < 3; ++b)
    {
        setPartEqGain(part, b, extras.eqGain[b]);
        setPartEqFreq(part, b, extras.eqFreq[b] > 0 ? extras.eqFreq[b] : kPartEqFreqDefault[b]);
        setPartEqQ   (part, b, extras.eqQ[b]);
    }

    // ---- 7. PORTAMENTO TIME last, and twice. The firmware acts on a DELTA against its
    // own stored byte, so re-sending the value it already holds is a NO-OP — the same
    // quirk applyPartSetup and sendPatchToDevice step 3 both work around. Nudge it one
    // step and put it back; undo-suppressed, because the real value is already in the
    // step from pass 2 and a second delta would make the history lie.
    if (plan.portaTime >= 0)
    {
        const auto v = AddressMap::get(in, AddressMap::param(plan.portaTime), 0);
        if (v.present)
        {
            const int real  = v.combined & 0x7F;
            const int nudge = (real < 0x7F) ? real + 1 : real - 1;
            UndoSuppress guard(*this);
            const auto a = AddressMap::makeParamChange(AddressMap::param(plan.portaTime),
                                                       part, (uint32_t) nudge);
            const auto b = AddressMap::makeParamChange(AddressMap::param(plan.portaTime),
                                                       part, (uint32_t) real);
            if (! a.empty()) sendSysEx(a);
            if (! b.empty()) sendSysEx(b);
        }
    }

    editorStateExternalSeq.fetch_add(1, std::memory_order_release);
    return true;
}

//==============================================================================
bool YamahaMU100AudioProcessor::copyPart(int part)
{
    if (part < 0 || part >= 32) return false;
    auto xml = buildPartSetupXml(part);   // the SAME tree a .mps holds, by design
    if (xml == nullptr) return false;
    partCopyXml = std::move(xml);
    partCopySrc = part;
    return true;
}

bool YamahaMU100AudioProcessor::partCopyTargetsDrumBuffer() const
{
    return partCopyXml != nullptr && setupTargetsDrumBuffer(*partCopyXml);
}

int YamahaMU100AudioProcessor::partCopySavedBuffer() const
{
    return partCopyXml != nullptr ? drumBufferOfSetupXml(*partCopyXml) : -1;
}

std::vector<int> YamahaMU100AudioProcessor::partsOnDrumBuffer(int buffer) const
{
    std::vector<int> out;
    if (buffer < 0 || buffer >= 4) return out;
    for (int p = 0; p < 32; ++p)
        if (partDrumBufferFromModel(p) == buffer) out.push_back(p);
    return out;
}

bool YamahaMU100AudioProcessor::pastePart(int part, int targetBuffer)
{
    if (partCopyXml == nullptr || part < 0 || part >= 32) return false;

    mu100::DeviceState in;
    if (! b64ToState(partCopyXml->getStringAttribute("partState", {}), in)) return false;

    PartPluginExtras ex;
    ex.hpf2 = (float) partCopyXml->getDoubleAttribute("setupCleanHpfHz", 0.0);
    juce::StringArray gains, freqs, qs;
    gains.addTokens(partCopyXml->getStringAttribute("setupEqGain",   {}), ",", "");
    freqs.addTokens(partCopyXml->getStringAttribute("setupEqFreqHz", {}), ",", "");
    qs   .addTokens(partCopyXml->getStringAttribute("setupEqQ",      {}), ",", "");
    for (int b = 0; b < 3; ++b)
    {
        ex.eqGain[b] = (b < gains.size()) ? gains[b].getIntValue() : 64;
        ex.eqFreq[b] = (b < freqs.size()) ? freqs[b].getIntValue() : 0;
        ex.eqQ   [b] = (b < qs.size())    ? qs[b].getIntValue()    : 7;
    }

    return applyPartBlockUndoable(in, part, targetBuffer, ex, partCopyXml.get(),
                                  "Paste Part " + juce::String(partCopySrc + 1)
                                      + " to Part " + juce::String(part + 1));
}

bool YamahaMU100AudioProcessor::resetPart(int part)
{
    if (part < 0 || part >= 32) return false;

    // THE PART'S OWN factory row, not part 0's: the receive channel, and on parts 10 and
    // 26 the kit and its PART MODE, all differ per part.
    mu100::DeviceState in;
    collectPartBlock(factoryDefaults(), part, in);
    if (in.raw().empty()) return false;

    // No drum tree: the buffer falls back to the incoming kit's own defaults, which IS
    // its factory state — the same thing the device does to itself on the program change.
    return applyPartBlockUndoable(in, part, /*targetBuffer*/ -1, PartPluginExtras{},
                                  /*drumXml*/ nullptr,
                                  "Reset Part " + juce::String(part + 1));
}

int YamahaMU100AudioProcessor::partResetDrumBuffer(int part) const
{
    if (part < 0 || part >= 32) return -1;
    // The FACTORY PART MODE, because that is what the reset is about to write — not the
    // part's current one. A part sitting on a buffer whose default is melodic simply
    // leaves it, which reaches nobody else.
    uint8_t mode = 0;
    if (! factoryDefaults().getByte(mu100::ModelIx::XG,
                                    xgAddr(0x08, (uint8_t) part, kPartLowPartMode), mode))
        return -1;
    return (mode >= 2 && mode <= 5) ? (int) mode - 2 : -1;
}

//==============================================================================
void YamahaMU100AudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (sizeInBytes <= 0)
        return;

    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml == nullptr || !xml->hasTagName("YamahaMU100State"))
        return;

    // romPath / pdcTrimIndex / bufferThreshold / midiDevice are NOT read back from
    // project state -- see buildStateXml. A project written by an older build still
    // carries them, and ignoring them is the point: they belong to this machine.

    // Per-part stereo outputs: the project is the only place these live, so it decides
    // unconditionally. Absent -- a project saved before they became project state --
    // reads as off, which is also what a new instance comes up as.
    // Called directly rather than deferred: the host enables the aux buses around this
    // point, and reassertPartOutputsIfDirty is what actually reaches the chip (from the
    // MAME thread), so the setter here only allocates the ring and raises the flag.
    setPartOutputsEnabled(xml->getIntAttribute("partOutputs", 0) != 0);
    setPartOutputsRemoveFromMain(xml->getIntAttribute("partOutputsRemoveFromMain", 0) != 0);

    // "Enable DAW automation". Absent -- a project saved before this existed -- reads
    // as ON, which is both the compiled default and what a new instance comes up as.
    // The default therefore differs from the per-part pair above ON PURPOSE: theirs is
    // off because a surprise there is a silently wrong mix, whereas automation that
    // silently stopped working is the surprise here.
    setDawAutomationEnabled(xml->getIntAttribute("dawAutomation", 1) != 0);

    // Loaded-file marker: restore the last-synced path + signature. editorStatePopulated
    // (set below when the editorState blob restores) keeps seedEditorDefaults from
    // clobbering these on reopen, so the strip shows the saved filename and a '*' only if
    // the restored model differs from savedStateSig.
    loadedFilePath = xml->getStringAttribute("loadedFilePath", {});
    savedStateSig  = xml->getStringAttribute("savedStateSig",  {});
    openFileType   = openFileTypeForPath(loadedFilePath);

    // Plugin-only extensions (HPF2 / part-EQ / drum-EQ / tempo-SYNC), shared with the
    // native-file load path. Re-pushed to the SWP30 after boot via the dirty flags.
    applyPluginExtras(*xml);

    // Stage any saved MU100 work-RAM. bootMameIfNeeded() will write it as
    // mu100/workram in the per-instance nvram dir before launching MAME, so
    // the nvram_device loads it into the share before the H8 starts running.
    const juce::String workRamB64 = xml->getStringAttribute("workram", {});
    if (workRamB64.isNotEmpty())
    {
        pendingWorkRam.fromBase64Encoding(workRamB64);
        if (pendingWorkRam.getSize() != WORKRAM_BYTES)
            pendingWorkRam.setSize(0);
    }

    // NOTE: `globalState` is neither read nor written any more (removed 2026-08-07 — see
    // the pendingGlobalRestore note in PluginProcessor.h). Projects saved before that
    // still carry the attribute; it is simply ignored, and the patch comes back through
    // the editorState blob below, which covers the same addresses.

    // Restore the full editor model so the editor opens on the saved patch directly —
    // no device read-back. editorStatePopulated=true makes the editor apply this
    // instead of seeding defaults or reading the device.
    const juce::String editorB64 = xml->getStringAttribute("editorState", {});
    if (editorB64.isNotEmpty())
    {
        juce::MemoryBlock mb;
        mb.fromBase64Encoding(editorB64);
        if (mb.getSize() > 0)
        {
            loadEditorState(static_cast<const uint8_t*>(mb.getData()), mb.getSize());
            editorStatePopulated = true;

            // A project saved before the effect-type loader existed can carry the
            // previous type's parameters under the current type (see
            // loadModelLikeDevice). Heal the model, and since the work-RAM snapshot was
            // taken from a device that had been SENT those values, it has to be pushed
            // over even when it restores -- serviceRestorePatchPush reads this.
            restoredModelHealed.store(healEffectParamsOutOfRange() > 0, std::memory_order_release);

            // This project carries a patch, so the device has to end up playing it. Arm
            // the fallback; serviceRestorePatchPush decides at ready-time whether the
            // work-RAM snapshot already did the job. Deferred because the boot has
            // usually not happened yet, so the answer is not knowable here.
            restorePushDeadlineMs = 0;      // the poll stamps it on the message thread
            restoredModelNeedsPush.store(true, std::memory_order_release);

            // Establish the unsaved-changes baseline from the restored model when the
            // saved project didn't carry a savedStateSig (e.g. saved before this feature
            // existed). editorStatePopulated=true makes the editor skip the defaults
            // seed — the only other place savedStateSig is set — so without this a
            // restored, no-file instance would have no baseline and edits would never
            // show the '*'. A project saved WITH savedStateSig keeps its value here, so
            // genuine unsaved edits still restore as dirty.
            if (savedStateSig.isEmpty())
                savedStateSig = editorStateSignature();
        }
    }

    // Kick MAME boot early if we have a valid staged blob and the host hasn't
    // already fired prepareToPlay. bootMameIfNeeded() is exchange-gated so it
    // is a no-op if MAME already booted. Plugin scanners do not call
    // setStateInformation, so the GUI-open-reboot regression does not return.
    if (pendingWorkRam.getSize() == WORKRAM_BYTES && ! mameHasStarted.load(std::memory_order_acquire))
        bootMameIfNeeded();

    // Re-baseline the automation parameters from the restored model. The project
    // state carries the COMPLETE patch; the parameters are a subset of it, so the
    // model wins and the host is told the restored values. Without this the host's
    // own restored parameter values would read as "the user just moved 289
    // controls" and be transmitted — a second of wire time, racing the patch push
    // that setStateInformation has already scheduled.
    automation->resyncFromModel();

    reportLatencyToHost();
}

//==============================================================================

//==============================================================================

//==============================================================================


//==============================================================================
// MIDI overrides — the display-only shadow of what incoming MIDI did to the device.
// See the block comment on midiOverrideValue() in PluginProcessor.h for why this is
// kept apart from editorState rather than folded into it.
//==============================================================================
namespace {

// The controllers that map onto a STORED MULTI PART parameter, i.e. the ones that
// leave the device somewhere the editor claims it is not. Deliberately NOT the whole
// XG controller set: expression, the pedals and the modulation wheel are performance
// state with no MULTI PART parameter behind them and no control in the editor, so
// there is nothing for a mark to point at.
//
// EVERY ROW HERE WAS MEASURED ON THE DEVICE, and the spec was wrong twice.
// MU100_DIAG_CCMAP (2026-09-09, 13 trials, one CC at a time with the part's whole
// 08 nn 00 + 08 nn 30 read back after each, so a controller landing on a NEIGHBOUR
// is caught rather than counted as a hit; CC11 expression is the negative control
// and moved nothing):
//
//   * CC75 DECAY TIME is NOT IMPLEMENTED. GM2 and the XG tables both list it, and
//     it moves no byte anywhere in either part chunk. It is deliberately absent
//     below -- an entry for it would mark a control the device never touched.
//   * CC94 VARIATION SEND only does anything while the variation block is wired as
//     a SYSTEM effect. As an INSERTION there is no per-part send for it to move, so
//     the first run read as a failure. It is kept: when it moves nothing, nothing
//     is overridden, which is exactly right.
struct CcParamMap { uint8_t cc; const char* param; };
const CcParamMap kCcParams[] = {
    {  5, "PORTAMENTO TIME"                   },
    {  7, "VOLUME"                            },
    { 10, "PAN"                               },
    { 65, "PORTAMENTO SWITCH"                 },
    { 71, "LOW PASS FILTER RESONANCE"         },
    { 72, "EG RELEASE TIME"                   },
    { 73, "EG ATTACK TIME"                    },
    { 74, "LOW PASS FILTER CUTOFF FREQUENCY"  },
    { 91, "REVERB SEND"                       },
    { 93, "CHORUS SEND"                       },
    { 94, "VARIATION SEND"                    },
};

// Resolved once, on first use: AddressMap::find is a linear scan over ~900 rows and
// this runs per incoming CC.
int ccParamIndex(uint8_t cc)
{
    static const std::map<uint8_t, int> table = []
    {
        std::map<uint8_t, int> m;
        for (const auto& e : kCcParams)
        {
            const int ix = mu100::AddressMap::find(e.param, "MULTI PART");
            if (ix >= 0) m[e.cc] = ix;
        }
        return m;
    }();
    const auto it = table.find(cc);
    return it == table.end() ? -1 : it->second;
}

int multiPartParam(const char* name)
{
    static std::map<std::string, int> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    const int ix = mu100::AddressMap::find(name, "MULTI PART");
    cache[name] = ix;
    return ix;
}

} // namespace

// The OUTGOING half of kCcParams, for "MIDI Out sends edits as > MIDI CC". It inverts
// the same measured table rather than keeping a second list, so a controller can only
// be sent if it is also one the device was measured to obey.
//
// VALUES WERE MEASURED TOO (MU100_DIAG_CCVAL, 2026-10-04, 90 trials, each against a
// SysEx-written sentinel that was read back first, whole part chunk diffed for
// neighbours, CC11 as the negative control): CC value v stores raw byte v over 0..127
// for nine of the eleven. The other two need converting, and getting either wrong
// records a value that plays back as something else:
//   * PAN: CC10 = 0 stores 1 (L63), so raw 0 -- RND -- is unreachable by CC. RND is
//     sent as 1, which plays back as L63, rather than as a value that merely happens
//     to land there.
//   * PORTAMENTO SWITCH: CC65 is a THRESHOLD, 0-63 OFF and 64-127 ON, while the editor
//     stores 0/1. Sending the raw 1 for ON plays back as OFF.
int YamahaMU100AudioProcessor::midiCcForMultiPartParam(const char* paramName)
{
    if (paramName == nullptr) return -1;
    for (const auto& e : kCcParams)
        if (std::strcmp(e.param, paramName) == 0)
            return e.cc;
    return -1;
}

bool YamahaMU100AudioProcessor::ccForParamChange(const std::vector<uint8_t>& msg,
                                                 uint8_t out[3]) const
{
    // A one-byte MULTI PART parameter change only: F0 43 1n 4C 08 pp ll vv F7.
    if (msg.size() != 9 || msg[0] != 0xF0 || msg[1] != 0x43 || (msg[2] & 0xF0) != 0x10
        || msg[3] != 0x4C || msg[4] != 0x08 || msg[8] != 0xF7)
        return false;

    // Low address byte -> controller. The part is in the MID byte, so the low byte
    // alone names the parameter within MULTI PART.
    static const std::map<uint8_t, uint8_t> byLow = []
    {
        std::map<uint8_t, uint8_t> m;
        for (const auto& e : kCcParams)
        {
            const int ix = mu100::AddressMap::find(e.param, "MULTI PART");
            if (ix >= 0) m[mu100::AddressMap::param(ix).lo.base] = e.cc;
        }
        return m;
    }();
    const auto it = byLow.find(msg[6]);
    if (it == byLow.end()) return false;

    // The channel the part LISTENS on, port included -- the same helper the Controller
    // wheel and the incoming direction use. Rcv OFF has no channel to put a CC on.
    const int raw = partRcvChannelRaw((int) msg[5]);
    if (raw < 0) return false;

    const uint8_t cc = it->second;
    uint8_t v = (uint8_t) (msg[7] & 0x7F);
    if (cc == 10 && v == 0) v = 1;                  // PAN RND -> L63
    if (cc == 65)           v = v != 0 ? 127 : 0;  // PORTAMENTO SWITCH threshold

    out[0] = (uint8_t) (0xB0 | (raw % 16));
    out[1] = cc;
    out[2] = v;
    return true;
}

void YamahaMU100AudioProcessor::bumpMidiOverrideSeq()
{
    midiOverrideCount.store((int) midiOverrideParams.size(), std::memory_order_release);
    midiOverrideSeq.fetch_add(1, std::memory_order_release);
}

void YamahaMU100AudioProcessor::noteMidiOverride(int paramIndex, int instance, int instance2,
                                                 const uint8_t* bytes, int numBytes)
{
    if (! midiFollowEnabled) return;                 // the follow lock: shadow nothing
    if (paramIndex < 0 || bytes == nullptr || numBytes <= 0) return;

    using mu100::AddressMap;
    const auto& p = AddressMap::param(paramIndex);

    mu100::ModelIx model {};
    uint32_t addr = 0;
    AddressMap::resolve(p, instance, instance2, model, addr);

    // The device's bytes for this parameter, padded from the MODEL where the message
    // carried fewer than the parameter holds. Without the padding a two-byte parameter
    // written one byte at a time would never read back as "present" and its mark would
    // never appear.
    uint8_t dev[8] = {};
    const int n = juce::jmin((int) p.size, (int) sizeof(dev));
    for (int i = 0; i < n; ++i)
    {
        if (i < numBytes) { dev[i] = bytes[i]; continue; }
        uint8_t mv = 0;
        dev[i] = editorState->getByte(model, addr + (uint32_t) i, mv) ? mv : 0;
    }

    // Does the device now agree with the model? Then there is no override — and this is
    // also how an override RETIRES when a song sweeps a controller back to where the
    // user had it.
    bool same = true;
    for (int i = 0; i < n && same; ++i)
    {
        uint8_t mv = 0;
        same = editorState->getByte(model, addr + (uint32_t) i, mv) && mv == dev[i];
    }

    const uint64_t key = midiOverrideKey(paramIndex, instance, instance2);

    if (same)
    {
        if (midiOverrideParams.erase(key) == 0) return;   // nothing was marked
        for (int i = 0; i < n; ++i)
            midiOverrideState->eraseByte(model, addr + (uint32_t) i);
        bumpMidiOverrideSeq();
        return;
    }

    for (int i = 0; i < n; ++i)
        midiOverrideState->setByte(model, addr + (uint32_t) i, dev[i]);
    midiOverrideParams.insert(key);
    bumpMidiOverrideSeq();
}

int YamahaMU100AudioProcessor::midiOverrideValue(int paramIndex, int instance, int instance2) const
{
    if (paramIndex < 0 || midiOverrideParams.empty()) return -1;
    if (midiOverrideParams.find(midiOverrideKey(paramIndex, instance, instance2))
        == midiOverrideParams.end())
        return -1;
    const auto v = mu100::AddressMap::get(*midiOverrideState, paramIndex, instance, instance2);
    return v.present ? v.combined : -1;
}

// The address-keyed read. No parameter-index lookup and no presence set to consult:
// the shadow's BYTES are set and erased in lockstep with midiOverrideParams, so a
// byte being present in it is exactly "incoming MIDI moved this address".
bool YamahaMU100AudioProcessor::midiOverrideByteAt(mu100::ModelIx model, uint32_t addr21,
                                                   uint8_t& out) const
{
    if (midiOverrideParams.empty()) return false;   // the common case, cheaply
    return midiOverrideState->getByte(model, addr21, out);
}

void YamahaMU100AudioProcessor::clearMidiOverridesForMessage(const std::vector<uint8_t>& msg)
{
    // Cheap exit on the overwhelmingly common case: nothing is overridden, and this
    // runs on every knob tick of every drag.
    if (midiOverrideParams.empty() || msg.empty()) return;

    for (const auto& m : mu100::parseStream(msg.data(), msg.size()))
    {
        if (m.kind != mu100::Kind::ParamChange && m.kind != mu100::Kind::BulkDump) continue;

        mu100::ModelIx ix;
        if (! mu100::modelToIx(m.model, ix)) continue;

        // Erase by ADDRESS, then re-derive which parameters that emptied. Going the
        // other way — identify the parameter and erase its key — misses a bulk dump,
        // which covers many parameters in one message.
        const uint32_t base = m.address();
        bool touched = false;
        for (size_t k = 0; k < m.data.size(); ++k)
            if (midiOverrideState->eraseByte(ix, base + (uint32_t) k))
                touched = true;
        if (! touched) continue;

        for (auto it = midiOverrideParams.begin(); it != midiOverrideParams.end(); )
        {
            const int paramIndex = (int) (*it >> 32);
            const int inst       = (int) ((*it >> 16) & 0xFFFF);
            const int inst2      = (int) (*it & 0xFFFF);
            const auto v = mu100::AddressMap::get(*midiOverrideState, paramIndex, inst, inst2);
            if (! v.present) it = midiOverrideParams.erase(it);
            else             ++it;
        }
        bumpMidiOverrideSeq();
    }
}

void YamahaMU100AudioProcessor::clearAllMidiOverrides()
{
    if (midiOverrideParams.empty() && midiOverrideState->raw().empty()) return;
    midiOverrideParams.clear();
    midiOverrideState->clear();
    for (int port = 0; port < 2; ++port)
        for (int ch = 0; ch < 16; ++ch)
            pendingBankMsb[port][ch] = pendingBankLsb[port][ch] = -1;
    bumpMidiOverrideSeq();
}

void YamahaMU100AudioProcessor::clearMidiOverrides(bool resendToDevice)
{
    if (resendToDevice)
    {
        // Put the device back on what the panels have been showing. One message per
        // overridden parameter, through the ordinary coalescing funnel, and NOT undoable:
        // nothing in the user's setup changes, only the device catches up to it.
        UndoSuppress noUndo(*this);
        for (const uint64_t key : midiOverrideParams)
        {
            const int paramIndex = (int) (key >> 32);
            const int inst       = (int) ((key >> 16) & 0xFFFF);
            const int inst2      = (int) (key & 0xFFFF);
            const auto v = mu100::AddressMap::get(*editorState, paramIndex, inst, inst2);
            if (! v.present) continue;
            const auto msg = mu100::AddressMap::makeParamChange(
                mu100::AddressMap::param(paramIndex), inst, (uint32_t) v.combined, inst2);
            sendParamChangeThrottled(msg);

            // Re-sending an effect TYPE makes the device load that type's DEFAULTS, so
            // the model's parameters for the unit have to follow it, or the device ends
            // up on defaults the panels do not show. The queue flushes in insertion
            // order, so these land after the type.
            for (const auto& m : mu100::parseStream(msg.data(), msg.size()))
                for (int u = 0; u < mu100fx::FX_UNIT_COUNT; ++u)
                    if (m.model == mu100::Model::XG
                        && effectTypeIn(u, m.address(), m.address() + (uint32_t) m.data.size()))
                        for (const auto& p : effectUnitModelMsgs(*editorState, u))
                            sendParamChangeThrottled(p);
        }
    }
    clearAllMidiOverrides();
}

void YamahaMU100AudioProcessor::keepMidiOverrides()
{
    if (midiOverrideParams.empty()) return;

    // ONE undo step for the whole capture: this is one user action ("keep what the song
    // did"), however many parameters it moved. The device already holds these values, so
    // nothing is transmitted — only the model is brought up to it.
    UndoScope scope(*this, "Keep MIDI overrides");
    // Two passes, effect TYPES first. A type the song changed made the device load that
    // type's parameter defaults on its own, so the model has to take them too (see
    // loadModelLikeDevice) -- and BEFORE the song's own parameter overrides, which the
    // defaults would otherwise overwrite. The set is ordered by parameter index, which
    // says nothing about which of the two arrived first.
    for (int pass = 0; pass < 2; ++pass)
    for (const uint64_t key : midiOverrideParams)
    {
        const int paramIndex = (int) (key >> 32);
        const int inst       = (int) ((key >> 16) & 0xFFFF);
        const int inst2      = (int) (key & 0xFFFF);
        const auto v = mu100::AddressMap::get(*midiOverrideState, paramIndex, inst, inst2);
        if (! v.present) continue;

        // Through the ordinary funnel so the step is captured and the DAW parameters
        // re-baseline, but with the transmit suppressed: re-sending a value the device
        // already has would cost wire time during playback for no effect.
        const auto msg = mu100::AddressMap::makeParamChange(
            mu100::AddressMap::param(paramIndex), inst, (uint32_t) v.combined, inst2);
        int unit = -1;
        for (const auto& m : mu100::parseStream(msg.data(), msg.size()))
            for (int u = 0; u < mu100fx::FX_UNIT_COUNT && unit < 0; ++u)
                if (m.model == mu100::Model::XG
                    && effectTypeIn(u, m.address(), m.address() + (uint32_t) m.data.size()))
                    unit = u;
        if ((pass == 0) != (unit >= 0)) continue;

        const auto* before = unit >= 0 ? modelEffectType(*editorState, unit) : nullptr;
        undoPush(msg);
        editorState->load(msg.data(), msg.size());
        if (unit < 0) continue;
        const auto* after = modelEffectType(*editorState, unit);
        if (after == nullptr || after == before) continue;
        for (const auto& d : effectTypeDefaultMsgs(unit, *after))
        {
            undoPush(d);
            editorState->load(d);
        }
    }
    clearAllMidiOverrides();
    editorStateExternalSeq.fetch_add(1, std::memory_order_release);
}

//==============================================================================

//==============================================================================
// Per-part roll-ups for the Voice module's two row tabs.
//
// The Voice module can show all 32 parts at once, so its row tabs are the only place
// in the editor that answers "where has anything happened" without clicking through
// to each part. That is what they are for, and it is why they deliberately EXCLUDE
// the five values the module shows for itself: a tab that lit for the voice sitting
// beside it would be saying something the reader can already see, and would drown out
// the thing they cannot.
//==============================================================================
namespace {

// The addresses the Voice module displays in its own columns: element reserve, both
// bank-select bytes, the program, and the receive channel. Mute and solo are session
// masks and live in no address at all, so there is nothing to exclude for them.
bool voiceListOwnAddress(uint32_t addr21)
{
    const uint8_t hi = (uint8_t) ((addr21 >> 14) & 0x7F);
    const uint8_t lo = (uint8_t) (addr21 & 0x7F);
    return hi == 0x08 && lo <= 0x04;
}

// Does this address belong to `part`? MULTI PART is 08 nn xx; the per-part high-pass
// cutoff lives on its own in a SEPARATE block (0A nn 20) and is easy to forget --
// classifySysEx makes the same two tests for the same reason.
//
// DRUM SETUP (3n rr xx) is deliberately NOT part of a part. That data is keyed by
// BUFFER and shared by every part assigned to it, so counting it here would light the
// tab on parts whose own settings nobody has touched.
bool addressBelongsToPart(uint32_t addr21, int part)
{
    const uint8_t hi  = (uint8_t) ((addr21 >> 14) & 0x7F);
    const uint8_t mid = (uint8_t) ((addr21 >> 7) & 0x7F);
    const uint8_t lo  = (uint8_t) (addr21 & 0x7F);
    if (hi == 0x08 && mid == (uint8_t) part) return true;
    if (hi == 0x0A && mid == (uint8_t) part && lo == 0x20) return true;
    return false;
}

} // namespace

bool YamahaMU100AudioProcessor::partChangedOutsideVoiceList(int part) const
{
    if (part < 0 || part >= 32) return false;

    const auto& model = editorState->raw();
    const auto& deflt = factoryDefaults();

    // Both blocks are contiguous key ranges, so this is two range scans rather than a
    // walk of the whole model -- which matters, because the Voice module asks this for
    // all 32 parts whenever the model moves.
    auto scan = [&](uint8_t hi, uint8_t loFirst, uint8_t loLast) -> bool
    {
        const uint32_t base = ((uint32_t) hi << 14) | ((uint32_t) part << 7);
        const auto from = model.lower_bound(mu100::DeviceState::key(mu100::ModelIx::XG,
                                                                    base | loFirst));
        const auto to   = model.upper_bound(mu100::DeviceState::key(mu100::ModelIx::XG,
                                                                    base | loLast));
        for (auto it = from; it != to; ++it)
        {
            const uint32_t addr = it->first & 0x1FFFFF;
            if (! addressBelongsToPart(addr, part)) continue;   // guards a short block
            if (voiceListOwnAddress(addr)) continue;

            uint8_t d = 0;
            // A byte the factory dump does not carry is something the model gained, so
            // it counts as changed. It cannot be compared away.
            if (! deflt.getByte(mu100::ModelIx::XG, addr, d) || d != it->second)
                return true;
        }
        return false;
    };

    if (scan(0x08, 0x00, 0x7F)) return true;
    if (scan(0x0A, 0x20, 0x20)) return true;

    // The plugin-only per-part values have no address, so they are asked for by name.
    // They ARE parameters of the part and a user sets them like any other, so leaving
    // them out would make the tab quietly wrong for anyone working with HPF2.
    if (cleanHpfHz[part] != 0.0f) return true;
    for (int b = 0; b < 3; ++b)
        if (partEqGain[part][b] != 64
            || partEqQ[part][b] != 7
            || partEqFreq[part][b] != kPartEqFreqDefault[b])
            return true;

    return false;
}

// Where the overrides are, grouped, for the badge's tooltip. See the header.
//
// Three kinds of home, because the editor has three: a PART (the MULTI PART block
// plus the part's own HPF, exactly what the Voice module's row tab counts), a DRUM
// BUFFER (per key, and shared by every part whose PART MODE names it -- which is why
// it is named as a BUFFER here and not as a part), and everything else, which lives
// in the System / Effect / Multi EQ modules and is on screen whatever is selected.
// The last bucket exists so the numbers ADD UP: a "where" line whose counts fall
// short of the badge's own total would raise the exact doubt it is here to settle.
//
// Nothing is dropped and nothing is elided. A tooltip wraps rather than ellipsizes,
// so there is no width to buy back, and the one thing this line must not do is hide
// the entry the reader is hunting for. The only summary is for a buffer carrying more
// keys than anyone reads as a list, which becomes a COUNT of keys -- a complete
// statement of a different shape, not a truncated one.
juce::String YamahaMU100AudioProcessor::midiOverrideWhere() const
{
    if (midiOverrideParams.empty()) return {};

    std::array<int, 32>           partN {};
    std::array<int, 4>            bufN  {};
    std::array<std::set<int>, 4>  bufKeys {};
    int otherN = 0;

    for (const uint64_t k : midiOverrideParams)
    {
        const int paramIndex = (int) (k >> 32);
        const int inst       = (int) ((k >> 16) & 0xFFFF);
        const int inst2      = (int) (k & 0xFFFF);

        mu100::ModelIx m {};
        uint32_t addr = 0;
        mu100::AddressMap::resolve(mu100::AddressMap::param(paramIndex), inst, inst2, m, addr);
        if (m != mu100::ModelIx::XG) { ++otherN; continue; }

        const uint8_t hi = (uint8_t) ((addr >> 14) & 0x7F);
        if (hi >= 0x30 && hi <= 0x33)
        {
            const int b = hi - 0x30;
            ++bufN[(size_t) b];
            bufKeys[(size_t) b].insert((int) ((addr >> 7) & 0x7F));
            continue;
        }

        int part = -1;
        for (int p = 0; p < 32 && part < 0; ++p)
            if (addressBelongsToPart(addr, p)) part = p;

        if (part >= 0) ++partN[(size_t) part];
        else           ++otherN;
    }

    // Beyond this many the names stop being a list and start being a wall; the count
    // says the same thing in one phrase. "keys" appears ONLY here, where it is the
    // noun being counted -- a named list needs no label.
    constexpr int kMaxNamedKeys = 8;

    juce::StringArray groups;
    for (int p = 0; p < 32; ++p)
        if (partN[(size_t) p] > 0)
            groups.add(juce::String(partN[(size_t) p]) + " on Pt " + juce::String(p + 1));

    for (int b = 0; b < 4; ++b)
    {
        if (bufN[(size_t) b] == 0) continue;
        const auto& keys = bufKeys[(size_t) b];
        juce::String where = juce::String(bufN[(size_t) b]) + " on " + drumbuf::name(b) + " (";
        if ((int) keys.size() > kMaxNamedKeys)
        {
            where += juce::String((int) keys.size()) + " keys)";
        }
        else
        {
            juce::StringArray names;
            for (const int n : keys) names.add(notelabel::text(n));
            where += names.joinIntoString(", ") + ")";
        }
        groups.add(where);
    }

    if (otherN > 0)
        groups.add(juce::String(otherN) + " on System / Effects");

    return groups.joinIntoString(", ");
}

bool YamahaMU100AudioProcessor::partOverriddenOutsideVoiceList(int part) const
{
    if (part < 0 || part >= 32 || midiOverrideParams.empty()) return false;

    // Walked over the OVERRIDE SET rather than over the addresses, because that set is
    // small (usually empty) while a part's address space is not.
    for (const uint64_t k : midiOverrideParams)
    {
        const int paramIndex = (int) (k >> 32);
        const int inst       = (int) ((k >> 16) & 0xFFFF);
        const int inst2      = (int) (k & 0xFFFF);

        mu100::ModelIx m {};
        uint32_t addr = 0;
        mu100::AddressMap::resolve(mu100::AddressMap::param(paramIndex), inst, inst2, m, addr);
        if (m != mu100::ModelIx::XG) continue;
        if (! addressBelongsToPart(addr, part)) continue;
        if (voiceListOwnAddress(addr)) continue;
        return true;
    }
    return false;
}

//==============================================================================
// Per-drum-key roll-ups for the Keyboard module's marker strip.
//
// The kit's DRUM SETUP block is the one part of a setup with NO overview anywhere:
// it holds 79 keys x 34 bytes, only one key is on screen at a time, and the Voice
// module's row tabs deliberately exclude it (a buffer is shared by every part
// assigned to it, so counting it there would light parts nobody touched -- see
// addressBelongsToPart). This is that overview, and it is keyed by BUFFER for the
// same reason the exclusion exists.
//==============================================================================
// Both halves also count the plugin-only per-(buffer, key) values -- HPF2 and the
// three peaking mid bands. They are parameters of the drum key like any other and a
// user sets them the same way, so a strip that ignored them would be quietly wrong
// for anyone working with HPF2. Mirrors the tail of partChangedOutsideVoiceList.
void YamahaMU100AudioProcessor::drumKeyMarks(int buffer,
                                             std::array<uint32_t, 4>& changed,
                                             std::array<uint32_t, 4>& overridden) const
{
    changed.fill(0);
    overridden.fill(0);
    if (buffer < 0 || buffer >= 4 || editorState == nullptr) return;

    auto mark = [](std::array<uint32_t, 4>& m, int note)
    {
        if (note >= 0 && note < 128) m[(size_t) (note >> 5)] |= 1u << (note & 31);
    };

    // --- changed: the hardware 3n block, diffed against the kit's own defaults ---
    //
    // The kit is resolved ONCE and the default rows cached per key: drumDefaultByte
    // re-derives the kit from the model on every call, which is a per-part scan, and
    // this walk asks the question 2686 times. Same caching the patch-send diff does.
    int kMsb = 0, kLsb = 0, kPgm = 0;
    const bool haveKit = drumBufferKitFromModel(buffer, kMsb, kLsb, kPgm);
    const uint8_t* defRow[mu100::kDrumDefNoteSpan] = {};
    if (haveKit)
        for (int n = 0; n < mu100::kDrumDefNoteSpan; ++n)
            defRow[n] = mu100::drumDefaultsFor(kMsb, kLsb, kPgm,
                                               mu100::kDrumDefFirstNote + n);

    // One ordered range scan of the buffer's block rather than a lookup per byte:
    // the model is a std::map keyed by (model, address), and 3n xx xx is contiguous
    // in it. A byte the kit's table does not explain counts as changed -- an unknown
    // kit therefore reads as "edited", which is the safe direction for a marker that
    // says "look here".
    const uint8_t hi = (uint8_t) (0x30 + buffer);
    const auto& model = editorState->raw();
    const auto from = model.lower_bound(
        mu100::DeviceState::key(mu100::ModelIx::XG, (uint32_t) hi << 14));
    const auto to = model.upper_bound(
        mu100::DeviceState::key(mu100::ModelIx::XG, ((uint32_t) hi << 14) | 0x3FFF));
    for (auto it = from; it != to; ++it)
    {
        const uint32_t addr = it->first & 0x1FFFFF;
        const int note = (int) ((addr >> 7) & 0x7F);
        const int noteIx = note - mu100::kDrumDefFirstNote;
        if (noteIx < 0 || noteIx >= mu100::kDrumDefNoteSpan) continue;

        const uint8_t* row = defRow[noteIx];
        if (row != nullptr)
        {
            const int off = mu100::drumDefaultOffsetForLow((uint8_t) (addr & 0x7F));
            if (off >= 0 && row[off] == it->second) continue;   // at the kit default
        }
        mark(changed, note);
    }

    // ...and the plugin-only side, which lives in no address at all.
    for (int note = 0; note < 128; ++note)
    {
        if (getDrumHpf(buffer, note) != 0.0f) { mark(changed, note); continue; }
        for (int b = 0; b < 3; ++b)
            if (getDrumEqGain(buffer, note, b) != 64
                || getDrumEqQ  (buffer, note, b) != 7
                || getDrumEqFreq(buffer, note, b) != kPartEqFreqDefault[b])
            { mark(changed, note); break; }
    }

    // --- overridden: walked over the OVERRIDE SET, not over the addresses ---
    // That set is small (usually empty) while the block is 2686 bytes. Same choice
    // partOverriddenOutsideVoiceList makes, for the same reason.
    if (! showMidiOverrides) return;
    for (const uint64_t k : midiOverrideParams)
    {
        const int paramIndex = (int) (k >> 32);
        const int inst       = (int) ((k >> 16) & 0xFFFF);
        const int inst2      = (int) (k & 0xFFFF);

        mu100::ModelIx m {};
        uint32_t addr = 0;
        mu100::AddressMap::resolve(mu100::AddressMap::param(paramIndex), inst, inst2, m, addr);
        if (m != mu100::ModelIx::XG) continue;
        if ((uint8_t) ((addr >> 14) & 0x7F) != hi) continue;
        mark(overridden, (int) ((addr >> 7) & 0x7F));
    }
}

void YamahaMU100AudioProcessor::drumKeyMarkDetail(int buffer, int key,
                                                  int& changedCount,
                                                  int& overrideCount) const
{
    changedCount = overrideCount = 0;
    if (buffer < 0 || buffer >= 4 || key < 0 || key >= 128 || editorState == nullptr) return;

    int kMsb = 0, kLsb = 0, kPgm = 0;
    const uint8_t* row = drumBufferKitFromModel(buffer, kMsb, kLsb, kPgm)
                       ? mu100::drumDefaultsFor(kMsb, kLsb, kPgm, key) : nullptr;

    const uint32_t base = ((uint32_t) (0x30 + buffer) << 14) | ((uint32_t) key << 7);
    const auto& model = editorState->raw();
    const auto from = model.lower_bound(mu100::DeviceState::key(mu100::ModelIx::XG, base));
    const auto to   = model.upper_bound(mu100::DeviceState::key(mu100::ModelIx::XG, base | 0x7F));
    for (auto it = from; it != to; ++it)
    {
        if (row != nullptr)
        {
            const int off = mu100::drumDefaultOffsetForLow((uint8_t) (it->first & 0x7F));
            if (off >= 0 && row[off] == it->second) continue;
        }
        ++changedCount;
    }

    if (getDrumHpf(buffer, key) != 0.0f) ++changedCount;
    for (int b = 0; b < 3; ++b)
    {
        if (getDrumEqGain(buffer, key, b) != 64) ++changedCount;
        if (getDrumEqQ   (buffer, key, b) != 7)  ++changedCount;
        if (getDrumEqFreq(buffer, key, b) != kPartEqFreqDefault[b]) ++changedCount;
    }
    if (! showMidiOverrides) return;
    for (const uint64_t k : midiOverrideParams)
    {
        const int paramIndex = (int) (k >> 32);
        const int inst       = (int) ((k >> 16) & 0xFFFF);
        const int inst2      = (int) (k & 0xFFFF);

        mu100::ModelIx m {};
        uint32_t addr = 0;
        mu100::AddressMap::resolve(mu100::AddressMap::param(paramIndex), inst, inst2, m, addr);
        if (m == mu100::ModelIx::XG && ((addr >> 7) & 0x3FFF) == ((base >> 7) & 0x3FFF))
            ++overrideCount;
    }
}

void YamahaMU100AudioProcessor::queueIncomingChannelMsg(int port, uint8_t status,
                                                        uint8_t d1, uint8_t d2)
{
    const uint32_t word = ((uint32_t) (port & 1) << 24) | ((uint32_t) status << 16)
                        | ((uint32_t) d1 << 8) | (uint32_t) d2;

    uint32_t base = chanIn.reserved.load(std::memory_order_relaxed);
    for (;;)
    {
        // Full: drop the OLDEST-arriving claim rather than blocking a callback thread.
        // A dropped controller costs one stale mark until the next message at that
        // address; a blocked audio thread costs a glitch.
        if (base - chanIn.readPos.load(std::memory_order_acquire) >= CHAN_IN_SIZE)
            return;
        if (chanIn.reserved.compare_exchange_weak(base, base + 1,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed))
            break;
    }

    chanIn.raw[base & CHAN_IN_MASK] = word;
    chanIn.ready[base & CHAN_IN_MASK].store(1, std::memory_order_release);
}

bool YamahaMU100AudioProcessor::drainIncomingChannelMsgs()
{
    bool any = false;
    for (;;)
    {
        const uint32_t base = chanIn.readPos.load(std::memory_order_relaxed);
        if (chanIn.ready[base & CHAN_IN_MASK].load(std::memory_order_acquire) == 0)
            break;                          // empty, or a producer is still filling

        const uint32_t word = chanIn.raw[base & CHAN_IN_MASK];
        chanIn.ready[base & CHAN_IN_MASK].store(0, std::memory_order_relaxed);
        chanIn.readPos.store(base + 1, std::memory_order_release);

        applyIncomingChannelMsg((int) ((word >> 24) & 1),
                                (uint8_t) ((word >> 16) & 0xFF),
                                (uint8_t) ((word >> 8) & 0xFF),
                                (uint8_t) (word & 0xFF));
        any = true;
    }
    return any;
}

void YamahaMU100AudioProcessor::applyIncomingChannelMsg(int port, uint8_t status,
                                                        uint8_t d1, uint8_t d2)
{
    if (! midiFollowEnabled) return;

    const uint8_t type = status & 0xF0u;
    const int     ch   = status & 0x0Fu;
    if (type != 0xB0 && type != 0xC0) return;      // only CC and program change matter

    // Which parts receive on this port + channel? Rcv CHANNEL is editable and its raw
    // encoding is discontiguous (00-1F = A1..A16 then B1..B16, 7F = OFF), so this reads
    // the model rather than assuming part n <- channel n. A part with no stored value
    // is on its factory channel, which IS part n.
    const int wanted = (port == 0 ? 0 : 16) + ch;
    uint32_t parts = 0;
    for (int p = 0; p < 32; ++p)
        if (partRcvChannelRaw(p) == wanted)              // -1 = Rcv OFF, matches nothing
            parts |= 1u << p;
    if (parts == 0) return;

    if (type == 0xB0)
    {
        // Bank select is HELD, not applied: measured, it changes nothing on the device
        // until a program change consumes it (MU100_DIAG_MIDIPC arm 2).
        if (d1 == 0)  { pendingBankMsb[port][ch] = d2; return; }
        if (d1 == 32) { pendingBankLsb[port][ch] = d2; return; }

        const int paramIndex = ccParamIndex(d1);
        if (paramIndex < 0) return;                          // not a stored parameter
        for (int p = 0; p < 32; ++p)
            if ((parts >> p) & 1u)
                noteMidiOverride(paramIndex, p, 0, &d2, 1);
        return;
    }

    // --- Program change --------------------------------------------------------
    // The program lands with whatever bank select preceded it; an unset half keeps the
    // bank the model already holds, which is what the device does with it too.
    static const int bankMsbParam = multiPartParam("BANK SELECT MSB");
    static const int bankLsbParam = multiPartParam("BANK SELECT LSB");
    static const int programParam = multiPartParam("PROGRAM NUMBER");

    const int msb = pendingBankMsb[port][ch];
    const int lsb = pendingBankLsb[port][ch];
    pendingBankMsb[port][ch] = pendingBankLsb[port][ch] = -1;

    for (int p = 0; p < 32; ++p)
    {
        if (((parts >> p) & 1u) == 0) continue;
        if (msb >= 0) { const uint8_t b = (uint8_t) msb; noteMidiOverride(bankMsbParam, p, 0, &b, 1); }
        if (lsb >= 0) { const uint8_t b = (uint8_t) lsb; noteMidiOverride(bankLsbParam, p, 0, &b, 1); }
        noteMidiOverride(programParam, p, 0, &d1, 1);
    }
}


//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new YamahaMU100AudioProcessor();
}
