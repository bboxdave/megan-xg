// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// Included BEFORE PluginEditor.h, which pulls in the MAME headers -- see the note in
// PluginFocusWin.h about NOMINMAX having to be set ahead of windows.h. The guard is
// the compiler's _WIN32, not JUCE_WINDOWS: JuceHeader.h has not been seen yet here,
// so JUCE_WINDOWS is not defined and the include would be silently skipped.
#if defined(_WIN32)
 #include "PluginFocusWin.h"
#endif

#include "PluginEditor.h"
#include "BusyOverlay.h"
#include "codec/MU100State.h"
#include "DialAction.h"
#include "ChangedMarks.h"
#include "VoiceNaming.h"
#include "StandaloneCloseGuard.h"
#include "ShortcutEditorPanel.h"
#include "HardwarePanelArt.h"

// For JucePlugin_VersionString in the About box. JuceHeader.h does not pull this
// in, and it is generated into JuceLibraryCode/ for both the VST3 and Standalone
// targets, so including it directly is safe.
#include <JucePluginDefines.h>

// Pull in StandalonePluginHolder so the editor can reach the shared AudioDeviceManager
// (for the embedded device selector). This header is normally only included by the
// standalone wrapper TU; it is header-only (inline classes, no app entry point) so
// including it here is safe for both the Standalone and VST3 targets.
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

// Where the manual lives when no local copy has been installed. See openUserManual().
// The RENDERED manual, published by .github/workflows/manual.yml -- not the repo
// root: a user who lost the local copy needs the manual, not a source tree.
static const char* const kManualUrl = "https://bboxdave.github.io/megan-xg/";

//==============================================================================
// ACCESSIBILITY: name a control from the label laid out beside it.
//
// The option windows position their labels by hand rather than through
// juce::Label::attachToComponent, which is what would otherwise tell JUCE the two
// belong together -- so a screen reader in these windows announces "combo box,
// 1024, 43 Hz bins" with nothing to say WHICH setting that is. Taken from the label
// rather than typed again here so a reworded row cannot leave a stale spoken name
// behind, which is the same rule the control tooltips already follow.
static void a11yNameFromLabel(juce::Component& c, const juce::Label& l)
{
    auto t = l.getText().trim();
    if (t.endsWithChar(':')) t = t.dropLastCharacters(1).trim();
    c.setTitle(t);
}

//==============================================================================
// MidiRoutingComponent — shown in the MIDI Routing dialog (standalone only).
// Lets the user activate host MIDI input devices and choose which of the MU100's
// two physical MIDI IN jacks each device feeds:
//   Port A (MIDI IN A) → XG parts 1–16
//   Port B (MIDI IN B) → XG parts 17–32
// The fourth column says which devices may pick the drum key in drum-edit mode. The
// VST has no equivalent — the host owns its MIDI inputs, so it gates the same selection
// on the transport instead (see midiDrumSelectDuringPlayback).
//==============================================================================
class MidiRoutingComponent : public juce::Component
{
public:
    explicit MidiRoutingComponent(YamahaMU100AudioProcessor& p) : proc(p)
    {
        // Header row labels (two lines each; headerH must be tall enough to show both)
        // Every font in this window is built through edtext::OptionsWin, and every
        // measurement it has to fit through edtext::optPx, so the whole thing grows
        // together when editorTextOptionsPct is raised. The checkboxes' own "Active"
        // text is drawn by the LookAndFeel from the button height and is left alone.
        colDevice.setText("MIDI Input Device", juce::dontSendNotification);
        colDevice.setFont(edtext::font(edtext::OptionsWin, 14.f, juce::Font::bold));
        colPort1.setText("Port A\n(parts 1-16)", juce::dontSendNotification);
        colPort1.setFont(edtext::font(edtext::OptionsWin, 14.f, juce::Font::bold));
        colPort1.setJustificationType(juce::Justification::centred);
        colPort2.setText("Port B\n(parts 17-32)", juce::dontSendNotification);
        colPort2.setFont(edtext::font(edtext::OptionsWin, 14.f, juce::Font::bold));
        colPort2.setJustificationType(juce::Justification::centred);
        colDrum.setText("Drum key\n(drum-edit mode)", juce::dontSendNotification);
        colDrum.setFont(edtext::font(edtext::OptionsWin, 14.f, juce::Font::bold));
        colDrum.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(colDevice);
        addAndMakeVisible(colPort1);
        addAndMakeVisible(colPort2);
        addAndMakeVisible(colDrum);

        // Note label
        noteLabel.setText(
            "Active devices are opened directly by this plugin.\n"
            "You must make a device active before selecting a port. "
            "At least one port must be selected for an active device.\n"
            "Drum key: while editing a drum kit, notes from these devices select the key "
            "to edit. Tick your keyboard; untick inputs carrying recorded parts.",
            juce::dontSendNotification);
        noteLabel.setFont(edtext::font(edtext::OptionsWin, 13.f));
        noteLabel.setColour(juce::Label::textColourId, edcol::kTextDim);
        noteLabel.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(noteLabel);

        // Build one row per available MIDI input device
        const auto devices = juce::MidiInput::getAvailableDevices();
        for (const auto& dev : devices)
        {
            auto row = std::make_unique<DeviceRow>();
            row->deviceId = dev.identifier;

            const bool    enabled = proc.isMidiDeviceEnabled(dev.identifier);
            const uint8_t mask    = proc.getMidiDeviceRouteMask(dev.identifier);

            row->nameLabel.setText(dev.name, juce::dontSendNotification);
            row->nameLabel.setFont(edtext::font(edtext::OptionsWin, 14.f));

            row->activeToggle.setButtonText("Active");
            row->activeToggle.setToggleState(enabled, juce::dontSendNotification);

            // ACCESSIBILITY: the three checkboxes to the right have NO button text --
            // their meaning is the column heading above them, which is a separate
            // label a screen reader has no way to connect to them. Unnamed, they read
            // as three identical "check box, not checked" on every row, and the drum
            // column added a third in 2026-09-04. Each gets the device name too: the
            // row's name label is the only thing that says which device this is, and
            // it is as unconnected as the headings are.
            row->activeToggle.setTitle(dev.name + " active");
            row->port1Toggle .setTitle(dev.name + " to Port A, parts 1 to 16");
            row->port2Toggle .setTitle(dev.name + " to Port B, parts 17 to 32");
            row->drumToggle  .setTitle(dev.name + " may select the drum key");
            row->drumToggle  .setDescription(
                "While editing a drum kit, notes from this device select the key to edit.");

            // Port checkboxes: enabled only while the device is active; a device may feed
            // both ports at once, but at least one must stay selected while active.
            row->port1Toggle.setButtonText({});
            row->port1Toggle.setToggleState(
                enabled && !!(mask & YamahaMU100AudioProcessor::MIDI_ROUTE_PORT1),
                juce::dontSendNotification);
            row->port1Toggle.setEnabled(enabled);

            row->port2Toggle.setButtonText({});
            row->port2Toggle.setToggleState(
                enabled && !!(mask & YamahaMU100AudioProcessor::MIDI_ROUTE_PORT2),
                juce::dontSendNotification);
            row->port2Toggle.setEnabled(enabled);

            // Independent of the ports: it gates the SELECTION, not the sound. Ticked
            // for any device the plugin has never seen, so activating one behaves as it
            // did before this column existed.
            row->drumToggle.setButtonText({});
            row->drumToggle.setToggleState(enabled && proc.isMidiDeviceDrumSelect(dev.identifier),
                                           juce::dontSendNotification);
            row->drumToggle.setEnabled(enabled);

            // Capture raw pointers — safe because row members outlive these lambdas
            auto* rowPtr = row.get();

            // A small helper: push the two checkbox states to the processor as a mask.
            auto pushMask = [this, rowPtr]
            {
                uint8_t mask = 0;
                if (rowPtr->port1Toggle.getToggleState()) mask |= YamahaMU100AudioProcessor::MIDI_ROUTE_PORT1;
                if (rowPtr->port2Toggle.getToggleState()) mask |= YamahaMU100AudioProcessor::MIDI_ROUTE_PORT2;
                proc.setMidiDeviceRouteMask(rowPtr->deviceId, mask);
            };

            row->activeToggle.onClick = [this, rowPtr]
            {
                const bool on = rowPtr->activeToggle.getToggleState(); // state after click
                if (on)
                {
                    // Enable first so the setToggleState repaint fires on an enabled
                    // button — otherwise the disabled-state paint can win the coalesce
                    // and the checkmark never appears. Port A is selected by default so
                    // the "at least one port while active" rule is satisfied immediately.
                    rowPtr->port1Toggle.setEnabled(true);
                    rowPtr->port2Toggle.setEnabled(true);
                    rowPtr->drumToggle .setEnabled(true);
                    rowPtr->port1Toggle.setToggleState(true,  juce::dontSendNotification);
                    rowPtr->port2Toggle.setToggleState(false, juce::dontSendNotification);
                    // Its own stored value, not a blanket true: a device the user has
                    // already unticked must not re-arm itself by being switched off and
                    // on again. A device never seen before reads back true.
                    rowPtr->drumToggle.setToggleState(proc.isMidiDeviceDrumSelect(rowPtr->deviceId),
                                                      juce::dontSendNotification);
                    proc.setMidiDeviceEnabled(rowPtr->deviceId, true);
                    proc.setMidiDeviceRouteMask(rowPtr->deviceId,
                                                YamahaMU100AudioProcessor::MIDI_ROUTE_PORT1);
                }
                else
                {
                    rowPtr->port1Toggle.setToggleState(false, juce::dontSendNotification);
                    rowPtr->port2Toggle.setToggleState(false, juce::dontSendNotification);
                    // Cleared for show only — an inactive device delivers nothing, and the
                    // stored flag is left alone so it returns as the user set it.
                    rowPtr->drumToggle.setToggleState(false, juce::dontSendNotification);
                    rowPtr->port1Toggle.setEnabled(false);
                    rowPtr->port2Toggle.setEnabled(false);
                    rowPtr->drumToggle .setEnabled(false);
                    proc.setMidiDeviceEnabled(rowPtr->deviceId, false);
                }
            };

            // Ports are now independent checkboxes (a device may feed both ports), with
            // one rule: an active device must keep at least one port selected. clicking-
            // toggles-state already flipped the clicked box; if that cleared the last
            // remaining port, veto the click by restoring it.
            row->port1Toggle.onClick = [rowPtr, pushMask]
            {
                if (! rowPtr->port1Toggle.getToggleState() && ! rowPtr->port2Toggle.getToggleState())
                    rowPtr->port1Toggle.setToggleState(true, juce::dontSendNotification);   // can't clear the last port
                pushMask();
            };

            row->port2Toggle.onClick = [rowPtr, pushMask]
            {
                if (! rowPtr->port1Toggle.getToggleState() && ! rowPtr->port2Toggle.getToggleState())
                    rowPtr->port2Toggle.setToggleState(true, juce::dontSendNotification);   // can't clear the last port
                pushMask();
            };

            row->drumToggle.onClick = [this, rowPtr]
            {
                proc.setMidiDeviceDrumSelect(rowPtr->deviceId, rowPtr->drumToggle.getToggleState());
            };

            addAndMakeVisible(row->nameLabel);
            addAndMakeVisible(row->activeToggle);
            addAndMakeVisible(row->port1Toggle);
            addAndMakeVisible(row->port2Toggle);
            addAndMakeVisible(row->drumToggle);
            rows.push_back(std::move(row));
        }

        if (rows.empty())
        {
            emptyLabel.setText("No MIDI input devices found.", juce::dontSendNotification);
            emptyLabel.setFont(edtext::font(edtext::OptionsWin, 15.f));
            emptyLabel.setJustificationType(juce::Justification::centred);
            addAndMakeVisible(emptyLabel);
        }

        const int totalH = headerH() + (int) rows.size() * rowH() + noteH() + edtext::optPx(10);
        setSize(rightBlockW() + edtext::optPx(400),
                juce::jmax(edtext::optPx(160), totalH));
    }

    void resized() override
    {
        const int w = getWidth();

        // Column widths. The tick columns sit on one PITCH, so another could be added
        // the same way; the last header is wider than the others because its label is,
        // and it has the window's right margin to grow into.
        const int nameW   = w - rightBlockW();
        const int activeW = edtext::optPx(76);
        const int gapX    = nameW;

        // Header — give labels the full headerH so both lines are visible
        colDevice.setBounds(8,                      2, nameW - 8,  headerH() - 4);
        colPort1 .setBounds(gapX,                   2, portW(),    headerH() - 4);
        colPort2 .setBounds(gapX + colPitch(),      2, portW(),    headerH() - 4);
        colDrum  .setBounds(gapX + 2 * colPitch(),  2, drumHdrW(), headerH() - 4);

        int y = headerH();
        for (auto& row : rows)
        {
            // Each tick is centred under its own header: the box is drawn 4 px in from
            // the button's left edge and is about 16 px wide, so its centre lands ~12 px
            // in. Derived from that rather than the old fixed +20 / +45, which centred
            // only at the column widths those numbers were written for.
            const int tickOff = edtext::optPx(12);
            row->nameLabel   .setBounds(8, y + 4, nameW - activeW - edtext::optPx(14), rowH() - 8);
            row->activeToggle.setBounds(nameW - activeW, y + 4, activeW, rowH() - 8);
            row->port1Toggle .setBounds(gapX + portW() / 2 - tickOff, y + 4, portW(), rowH() - 8);
            row->port2Toggle .setBounds(gapX + colPitch() + portW() / 2 - tickOff,
                                        y + 4, portW(), rowH() - 8);
            row->drumToggle  .setBounds(gapX + 2 * colPitch() + drumHdrW() / 2 - tickOff,
                                        y + 4, portW(), rowH() - 8);
            y += rowH();
        }

        if (rows.empty())
            emptyLabel.setBounds(0, headerH(), w, rowH() + 10);

        noteLabel.setBounds(8, y + 6, w - 16, noteH() - edtext::optPx(2));
    }

private:
    // Geometry, all of it in one place and all of it scaled by the OptionsWin
    // percentage: at 100% these are the sizes the +2 pt fonts are laid out for.
    // noteH covers five lines, which is what both rule notes wrap to.
    static int rowH()        { return edtext::optPx(30); }
    static int headerH()     { return edtext::optPx(52); }   // two-line port labels
    static int noteH()       { return edtext::optPx(78); }
    // 106 is what "(parts 17-32)" needs to stay on two lines at 14 pt bold; at 92 it
    // broke to three and the last line was clipped by the header's height.
    static int portW()       { return edtext::optPx(106); }
    static int colPitch()    { return portW() + edtext::optPx(16); }
    static int drumHdrW()    { return edtext::optPx(130); }
    static int rightBlockW() { return 2 * colPitch() + drumHdrW() + edtext::optPx(16); }

    YamahaMU100AudioProcessor& proc;

    struct DeviceRow {
        juce::String     deviceId;
        juce::Label      nameLabel;
        juce::ToggleButton activeToggle;
        juce::ToggleButton port1Toggle;
        juce::ToggleButton port2Toggle;
        juce::ToggleButton drumToggle;
    };

    std::vector<std::unique_ptr<DeviceRow>> rows;

    juce::Label colDevice, colPort1, colPort2, colDrum;
    juce::Label emptyLabel;
    juce::Label noteLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiRoutingComponent)
};

//==============================================================================
// MidiOutputRoutingComponent — shown in the MIDI Output dialog (standalone only).
// Lets the user activate host MIDI output devices; the plugin's single MIDI output
// (the editor's XG SysEx echo) is sent to every active device. Two columns only —
// device name + Active — since there is no Port A/B distinction on output.
//==============================================================================
class MidiOutputRoutingComponent : public juce::Component
{
public:
    explicit MidiOutputRoutingComponent(YamahaMU100AudioProcessor& p) : proc(p)
    {
        // Sized like the MIDI Input window: OptionsWin fonts, optPx geometry.
        colDevice.setText("MIDI Output Device", juce::dontSendNotification);
        colDevice.setFont(edtext::font(edtext::OptionsWin, 14.f, juce::Font::bold));
        colActive.setText("Active", juce::dontSendNotification);
        colActive.setFont(edtext::font(edtext::OptionsWin, 14.f, juce::Font::bold));
        colActive.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(colDevice);
        addAndMakeVisible(colActive);

        noteLabel.setText(
            "Active devices receive this plugin's MIDI output (editor edits as XG SysEx).\n"
            "Route a virtual port (e.g. loopMIDI) into a DAW to record automation.",
            juce::dontSendNotification);
        noteLabel.setFont(edtext::font(edtext::OptionsWin, 13.f));
        noteLabel.setColour(juce::Label::textColourId, edcol::kTextDim);
        noteLabel.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(noteLabel);

        const auto devices = juce::MidiOutput::getAvailableDevices();
        for (const auto& dev : devices)
        {
            auto row = std::make_unique<DeviceRow>();
            row->deviceId = dev.identifier;

            row->nameLabel.setText(dev.name, juce::dontSendNotification);
            row->nameLabel.setFont(edtext::font(edtext::OptionsWin, 14.f));

            row->activeToggle.setButtonText("Active");
            row->activeToggle.setToggleState(proc.isMidiOutputDeviceEnabled(dev.identifier),
                                             juce::dontSendNotification);
            // Named for the same reason as the input window's: every row's checkbox
            // says only "Active", and the device name beside it is a label a screen
            // reader cannot connect to the box.
            row->activeToggle.setTitle(dev.name + " active");

            auto* rowPtr = row.get();
            row->activeToggle.onClick = [this, rowPtr]
            {
                proc.setMidiOutputDeviceEnabled(rowPtr->deviceId,
                                                rowPtr->activeToggle.getToggleState());
            };

            addAndMakeVisible(row->nameLabel);
            addAndMakeVisible(row->activeToggle);
            rows.push_back(std::move(row));
        }

        if (rows.empty())
        {
            emptyLabel.setText("No MIDI output devices found.", juce::dontSendNotification);
            emptyLabel.setFont(edtext::font(edtext::OptionsWin, 15.f));
            emptyLabel.setJustificationType(juce::Justification::centred);
            addAndMakeVisible(emptyLabel);
        }

        const int totalH = headerH() + (int) rows.size() * rowH() + noteH() + edtext::optPx(10);
        setSize(edtext::optPx(620), juce::jmax(edtext::optPx(140), totalH));
    }

    void resized() override
    {
        const int w       = getWidth();
        const int activeW = edtext::optPx(76);

        colDevice.setBounds(8, 2, w - activeW - edtext::optPx(40), headerH() - 4);
        colActive.setBounds(w - activeW - edtext::optPx(16), 2, activeW, headerH() - 4);

        int y = headerH();
        for (auto& row : rows)
        {
            row->nameLabel   .setBounds(8, y + 4, w - activeW - edtext::optPx(40), rowH() - 8);
            row->activeToggle.setBounds(w - activeW - 8, y + 4, activeW, rowH() - 8);
            y += rowH();
        }

        if (rows.empty())
            emptyLabel.setBounds(0, headerH(), w, rowH() + 10);

        noteLabel.setBounds(8, y + 6, w - 16, noteH() - edtext::optPx(2));
    }

private:
    // Same scaled geometry as the input window; noteH covers this window's two lines.
    static int rowH()    { return edtext::optPx(30); }
    static int headerH() { return edtext::optPx(52); }
    static int noteH()   { return edtext::optPx(42); }

    YamahaMU100AudioProcessor& proc;

    struct DeviceRow {
        juce::String       deviceId;
        juce::Label        nameLabel;
        juce::ToggleButton activeToggle;
    };

    std::vector<std::unique_ptr<DeviceRow>> rows;

    juce::Label colDevice, colActive;
    juce::Label emptyLabel;
    juce::Label noteLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiOutputRoutingComponent)
};

//==============================================================================
// DisplayAnimationComponent — the "Display Animation" controls: the per-part
// MIDI activity indicators plus the EQ screen's animated spectrum analyser.
//
// ONE definition serves both hosts — since the Options menu was split up (see
// showSettingsMenu) they both open it the same way, as its own dialog from
// Options -> "Display Animation...". It used to be embedded in the VST's single
// consolidated Options window instead, which is why the MIDI input indicators
// toggle lives HERE rather than in AudioSettingsComponent: it is the same control
// in both places, and duplicating it invites exactly the kind of divergence this
// component exists to prevent. `withHeading` is what the embed needed; both
// callers now pass false, and it is kept for the next embedder.
//==============================================================================
class DisplayAnimationComponent : public juce::Component,
                                 private juce::Timer
{
public:
    // Row geometry, scaled by the option-window text percentage so the rows open up
    // as the labels grow (see edtext::OptionsWin). At 100% these are the numbers this
    // window has always used — 24 / 28 / 26 / 8.
    static int rowH()      { return edtext::optPx(24); }
    static int rowGap()    { return edtext::optPx(28); }
    static int headH()     { return edtext::optPx(26); }
    static int bottomPad() { return edtext::optPx(8);  }

    // The label column of the six combo rows, and where the combos start after it.
    // Widened from 140 for the +2 pt labels ("Smoothing decay:" is the longest).
    static int labelIndent() { return edtext::optPx(26); }
    static int labelW()      { return edtext::optPx(152); }
    static int comboX()      { return labelIndent() + labelW() + edtext::optPx(6); }
    static int comboW()      { return edtext::optPx(230); }

    // Height the embedder must reserve. The last row needs rowH, not rowGap — using
    // the gap for it is what left a band of dead space under the standalone dialog.
    static int rowsH()  { return 11 * rowGap() + rowH() + bottomPad(); }   // the 12 control rows
    static int heightFor(bool withHeading)
    {
        return (withHeading ? headH() : 0) + rowsH();
    }

    // The window's own width: whatever the widest row needs, never less than the
    // 430 this dialog has always opened at.
    static int widthFor()
    {
        return juce::jmax(430, comboX() + comboW() + edtext::optPx(16));
    }

    explicit DisplayAnimationComponent(YamahaMU100AudioProcessor& p, bool withHeading)
        : proc(p), heading(withHeading)
    {
        if (heading)
        {
            headingLabel.setText("Display Animation", juce::dontSendNotification);
            headingLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
            headingLabel.setFont(edtext::font(edtext::OptionsWin, 15.f, juce::Font::bold));
            addAndMakeVisible(headingLabel);
        }

        // MIDI input indicators — the Voice module's per-part orange activity tabs
        // and the Keyboard module's SYS/FX/BULK lamps (both read the flag live).
        midiIndicatorsToggle.setButtonText("MIDI input indicators");
        midiIndicatorsToggle.setColour(juce::ToggleButton::textColourId, edcol::kTextLabel);
        midiIndicatorsToggle.setToggleState(proc.midiInputIndicators, juce::dontSendNotification);
        midiIndicatorsToggle.onClick = [this]
        {
            proc.midiInputIndicators = midiIndicatorsToggle.getToggleState();
            proc.saveSettings();
        };
        addAndMakeVisible(midiIndicatorsToggle);

        // Played notes on the keyboard - the Keyboard module lights every key held by
        // incoming MIDI on the selected part's Rcv channel. Read live by that panel's
        // timer (which runs regardless, for the SYS/FX/BULK lamps), so unlike the two
        // animations below it needs no nudge in the editor's timer.
        playedNotesToggle.setButtonText("Played notes on the keyboard");
        playedNotesToggle.setColour(juce::ToggleButton::textColourId, edcol::kTextLabel);
        playedNotesToggle.setTooltip(
            "In the Keyboard module, light every key currently held by incoming MIDI on "
            "the selected part's Rcv channel, in the same green a right-drag audition "
            "uses. Several keys light at once, so a chord - or a kick, snare and hi-hat "
            "together - reads as one picture.\n\n"
            "It follows the part you have selected, not the whole instrument, and it "
            "stays on in drum-key edit mode: the red marker there says which drum tone "
            "you are editing, the green keys say what is sounding.");
        playedNotesToggle.setToggleState(proc.keyboardPlayedNotes, juce::dontSendNotification);
        playedNotesToggle.onClick = [this]
        {
            proc.keyboardPlayedNotes = playedNotesToggle.getToggleState();
            proc.saveSettings();
        };
        addAndMakeVisible(playedNotesToggle);

        // Show MIDI overrides -- the cyan marks that say where the DEVICE is when
        // something arriving over MIDI has moved it away from the value you set.
        // Read live by the panels, so nothing has to be nudged when it changes: the
        // editor's timer watches the override sequence and re-marks either way.
        // Named to match Options -> "Follow MIDI changes to the device", because it is
        // the same data seen from the other end: that item decides whether the changes
        // are TRACKED, this one whether they are DRAWN. It was "Show MIDI overrides",
        // which named the same thing with a second noun.
        overridesToggle.setButtonText("Show MIDI changes to the device");
        overridesToggle.setColour(juce::ToggleButton::textColourId, edcol::kTextLabel);
        overridesToggle.setTooltip(
            "Mark any control whose value on the device has been changed by incoming "
            "MIDI - a program change, a controller, or a SysEx parameter change. The "
            "control keeps YOUR value and a cyan mark shows where the device is; the "
            "row below chooses what that mark looks like."
            "\n\n"
            "Nothing arriving over MIDI ever changes your setup, so what you save is "
            "always what the panels show. These marks are how you can tell when the "
            "device has been taken somewhere else. They are the same changes Options > "
            "Clear MIDI overrides and Keep MIDI overrides act on."
            "\n\n"
            "Switch this off and the changes are still tracked, just not drawn. To stop "
            "tracking them altogether, untick Options > Follow MIDI changes to the "
            "device."
            "\n\n"
            "DAW automation is not marked: those lanes are part of your project and "
            "do move your values, exactly as in any other plugin.");
        overridesToggle.setToggleState(proc.showMidiOverrides, juce::dontSendNotification);
        overridesToggle.onClick = [this]
        {
            proc.showMidiOverrides = overridesToggle.getToggleState();
            proc.saveSettings();
            updateEnablement();
        };
        addAndMakeVisible(overridesToggle);

        // What the mark LOOKS like. A combo row under the toggle, indented like the
        // other multi-choice rows in this window, rather than three more checkboxes:
        // these are one setting with three values, and the window has never spelled
        // such a thing as a radio group built out of ticks.
        //
        // There is deliberately NO "no marks" entry -- the toggle above is the off
        // switch, and a second way to turn one thing off is exactly the confusion the
        // wording here is trying to avoid.
        markLabel.setText("Mark them with:", juce::dontSendNotification);
        markLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
        markLabel.setFont(edtext::font(edtext::OptionsWin, 15.f));
        addAndMakeVisible(markLabel);

        // Order runs from the most compact to the most visible, and each item names
        // both halves of what it does, because a parameter with a knob and one without
        // are marked differently in the middle choice.
        markCombo.addItem("Horizontal bars",                   midimarks::HorizontalBars      + 1);
        markCombo.addItem("Horizontal bars and knob arcs",     midimarks::BarsAndArcs         + 1);
        markCombo.addItem("Vertical bars and knob dots",       midimarks::VerticalBarsAndDots + 1);
        markCombo.setSelectedId(juce::jlimit(0, midimarks::kStyleCount - 1,
                                             proc.midiOverrideMarkStyle) + 1,
                                juce::dontSendNotification);
        markCombo.setTooltip(
            "Horizontal bars puts a bar under every value box, whether or not the "
            "parameter has a knob."
            "\n\n"
            "Horizontal bars and knob arcs puts the bar under the boxes that have no "
            "knob, and draws an arc around the knobs that do. An arc shows how far "
            "MIDI has moved the value, which a small mark beside the knob's own "
            "pointer cannot."
            "\n\n"
            "Vertical bars and knob dots is the most compact: a short bar inside the "
            "right edge of the value box, and a ring on the dial.");
        markCombo.onChange = [this]
        {
            const int id = markCombo.getSelectedId();
            if (id >= 1 && id <= midimarks::kStyleCount)
            {
                proc.midiOverrideMarkStyle = id - 1;
                proc.saveSettings();   // the editor's timer mirror re-lays the panels out
            }
        };
        a11yNameFromLabel(markCombo, markLabel);
        addAndMakeVisible(markCombo);

        // EQ animated spectrum master on/off. The editor's timer notices the flag
        // change and nudges the Graphic Display (see YamahaMU100AudioProcessorEditor
        // ::timerCallback) — the analyser can re-read it itself only while running,
        // so the off->on direction needs that nudge.
        spectrumToggle.setButtonText("EQ animated spectrum");
        spectrumToggle.setColour(juce::ToggleButton::textColourId, edcol::kTextLabel);
        spectrumToggle.setToggleState(proc.eqSpectrumEnabled, juce::dontSendNotification);
        spectrumToggle.onClick = [this]
        {
            proc.eqSpectrumEnabled = spectrumToggle.getToggleState();
            proc.saveSettings();
            updateEnablement();
        };
        addAndMakeVisible(spectrumToggle);

        // Resolution = FFT size. The bin width is shown because that, not the point
        // count, is what decides whether the low end is readable: at 1024 the bins
        // are 43 Hz apart and HPF2's whole 20-1000 Hz range is a handful of them.
        resLabel.setText("Resolution:", juce::dontSendNotification);
        resLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
        // The label rows ride edtext::OptionsWin, so settings.xml's
        // editorTextOptionsPct sizes them (and the layout follows — see labelW()).
        resLabel.setFont(edtext::font(edtext::OptionsWin, 15.f));
        addAndMakeVisible(resLabel);

        resCombo.addItem("1024   (43 Hz bins, fastest)", 1);
        resCombo.addItem("2048   (22 Hz bins)",          2);
        resCombo.addItem("4096   (11 Hz bins)",          3);
        resCombo.addItem("8192   (5 Hz bins, finest)",   4);
        resCombo.setSelectedId(
            juce::jlimit(0, 3, proc.eqSpectrumFftOrder
                                   - YamahaMU100AudioProcessor::kSpectrumFftOrderMin) + 1,
            juce::dontSendNotification);
        resCombo.onChange = [this]
        {
            const int id = resCombo.getSelectedId();
            if (id >= 1 && id <= 4)
            {
                proc.eqSpectrumFftOrder = YamahaMU100AudioProcessor::kSpectrumFftOrderMin + id - 1;
                proc.saveSettings();   // read live each analyser frame; no nudge needed
            }
        };
        a11yNameFromLabel(resCombo, resLabel);
        addAndMakeVisible(resCombo);

        // Smoothing decay = how fast a peak falls back. Attack is always instant.
        decayLabel.setText("Smoothing decay:", juce::dontSendNotification);
        decayLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
        decayLabel.setFont(edtext::font(edtext::OptionsWin, 15.f));
        addAndMakeVisible(decayLabel);

        decayCombo.addItemList({ "Fast", "Medium", "Slow", "Very slow" }, 1);
        decayCombo.setSelectedId(
            juce::jlimit(0, YamahaMU100AudioProcessor::kSpectrumDecayCount - 1,
                         proc.eqSpectrumDecayIdx) + 1,
            juce::dontSendNotification);
        decayCombo.onChange = [this]
        {
            const int id = decayCombo.getSelectedId();
            if (id >= 1 && id <= YamahaMU100AudioProcessor::kSpectrumDecayCount)
            {
                proc.eqSpectrumDecayIdx = id - 1;
                proc.saveSettings();   // read live each analyser frame
            }
        };
        a11yNameFromLabel(decayCombo, decayLabel);
        addAndMakeVisible(decayCombo);

        // Animated volume level meters — the VOLUME screen's bars. Default ON, and
        // unlike the analyser above this is not purely a GUI cost: it switches on a
        // per-part capture inside the emulated mixer, plus a peak pass over the
        // plugin's own output for the master ("S") bar. Same nudge as the analyser:
        // the panel can re-read the flag itself only while its tick is running.
        meterToggle.setButtonText("Animated volume level meters");
        meterToggle.setColour(juce::ToggleButton::textColourId, edcol::kTextLabel);
        meterToggle.setTooltip(
            "On the Graphic Display's VOLUME screen, show each part's volume setting as "
            "a thin dash and animate the live output level below it.\n\n"
            "The level is the part's dry signal with its volume and pan applied, so it "
            "does not include the reverb, chorus, variation or insertion effects. The "
            "leftmost \"S\" bar meters the plugin's own stereo output.");
        meterToggle.setToggleState(proc.volumeMetersEnabled, juce::dontSendNotification);
        meterToggle.onClick = [this]
        {
            proc.volumeMetersEnabled = meterToggle.getToggleState();
            proc.saveSettings();
            updateEnablement();
        };
        addAndMakeVisible(meterToggle);

        // The same four choices the analyser offers, deliberately: it is the same
        // question asked of a second display, and one table means the two cannot
        // drift apart in wording or timing.
        meterDecayLabel.setText("Meter smoothing:", juce::dontSendNotification);
        meterDecayLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
        meterDecayLabel.setFont(edtext::font(edtext::OptionsWin, 15.f));
        addAndMakeVisible(meterDecayLabel);

        meterDecayCombo.addItemList({ "Fast", "Medium", "Slow", "Very slow" }, 1);
        meterDecayCombo.setSelectedId(
            juce::jlimit(0, YamahaMU100AudioProcessor::kSpectrumDecayCount - 1,
                         proc.volumeMeterDecayIdx) + 1,
            juce::dontSendNotification);
        meterDecayCombo.onChange = [this]
        {
            const int id = meterDecayCombo.getSelectedId();
            if (id >= 1 && id <= YamahaMU100AudioProcessor::kSpectrumDecayCount)
            {
                proc.volumeMeterDecayIdx = id - 1;
                proc.saveSettings();   // read live each meter frame; no nudge needed
            }
        };
        a11yNameFromLabel(meterDecayCombo, meterDecayLabel);
        addAndMakeVisible(meterDecayCombo);

        // Meter colours — the one user-choosable palette in the editor. Each ramp
        // carries its own per-theme values (edcol::meterRamps), so a choice here
        // cannot leave either theme with an illegible meter; the names come from
        // that table rather than being retyped, so the two cannot drift.
        rampLabel.setText("Meter colors:", juce::dontSendNotification);
        rampLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
        rampLabel.setFont(edtext::font(edtext::OptionsWin, 15.f));
        addAndMakeVisible(rampLabel);

        for (int i = 0; i < edcol::kMeterRampCount; ++i)
        {
            const auto& r = edcol::meterRamps()[i];
            juce::String item (r.name);
            if (juce::String(r.hint).isNotEmpty())
                item += "   (" + juce::String(r.hint) + ")";
            rampCombo.addItem(item, i + 1);
        }
        rampCombo.setSelectedId(
            juce::jlimit(0, edcol::kMeterRampCount - 1, proc.volumeMeterRampIdx) + 1,
            juce::dontSendNotification);
        rampCombo.onChange = [this]
        {
            const int id = rampCombo.getSelectedId();
            if (id >= 1 && id <= edcol::kMeterRampCount)
            {
                proc.volumeMeterRampIdx = id - 1;
                proc.saveSettings();
                // Push it straight into the palette so the change is instant. Unlike
                // a THEME change this needs no EditorView rebuild — the four meter
                // tokens are read at paint time and no juce::Label holds one — but
                // the screen does have to be told to repaint, which the editor's
                // timer mirror does (volumeMeterRampApplied).
                edcol::setMeterRamp(proc.volumeMeterRampIdx);
            }
        };
        a11yNameFromLabel(rampCombo, rampLabel);
        addAndMakeVisible(rampCombo);

        // Colour spread — how far down the bar the ramp's stops sit. Labelled with the
        // dB the primary colour ends at, not with the internal index: the same reason
        // the Resolution row above is labelled by bin width rather than point count.
        // "Sensitivity" would be the wrong word — the meter's HEIGHT is unchanged, so
        // it does not respond to quieter signals; only the colour arrives earlier.
        spreadLabel.setText("Color spread:", juce::dontSendNotification);
        spreadLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
        spreadLabel.setFont(edtext::font(edtext::OptionsWin, 15.f));
        addAndMakeVisible(spreadLabel);

        for (int i = 0; i < edcol::kMeterSpreadCount; ++i)
        {
            const auto& sp = edcol::meterSpreads()[i];
            spreadCombo.addItem(juce::String(sp.name) + "   (color from "
                                    + juce::String((int) sp.knee[0]) + " dB)",
                                i + 1);
        }
        spreadCombo.setSelectedId(
            juce::jlimit(0, edcol::kMeterSpreadCount - 1, proc.volumeMeterSpreadIdx) + 1,
            juce::dontSendNotification);
        spreadCombo.onChange = [this]
        {
            const int id = spreadCombo.getSelectedId();
            if (id >= 1 && id <= edcol::kMeterSpreadCount)
            {
                proc.volumeMeterSpreadIdx = id - 1;
                proc.saveSettings();   // read live by paintMeterBar; no nudge needed
            }
        };
        a11yNameFromLabel(spreadCombo, spreadLabel);
        addAndMakeVisible(spreadCombo);

        // Meter fill — cosmetic only. Solid is the meter as it has always been;
        // LED cuts the same column into segments the size of the setting dash above
        // it, with the display background showing between them. Named from the
        // edcol::meterFills table rather than retyped, same as the ramps.
        fillLabel.setText("Meter fill:", juce::dontSendNotification);
        fillLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
        fillLabel.setFont(edtext::font(edtext::OptionsWin, 15.f));
        addAndMakeVisible(fillLabel);

        for (int i = 0; i < edcol::kMeterFillCount; ++i)
        {
            const auto& f = edcol::meterFills()[i];
            juce::String item (f.name);
            if (juce::String(f.hint).isNotEmpty())
                item += "   (" + juce::String(f.hint) + ")";
            fillCombo.addItem(item, i + 1);
        }
        fillCombo.setSelectedId(
            juce::jlimit(0, edcol::kMeterFillCount - 1, proc.volumeMeterFillIdx) + 1,
            juce::dontSendNotification);
        fillCombo.setTooltip(
            "Solid draws the live level as one continuous bar. LED draws it as lit "
            "segments the same size as the volume-setting dash, separated by gaps in "
            "the display background.\n\n"
            "LED lights a segment only once the level reaches it, so it can read up "
            "to one segment low - as a hardware meter bridge does.");
        fillCombo.onChange = [this]
        {
            const int id = fillCombo.getSelectedId();
            if (id >= 1 && id <= edcol::kMeterFillCount)
            {
                proc.volumeMeterFillIdx = id - 1;
                proc.saveSettings();   // read live by paintMeterBar; no nudge needed
            }
        };
        a11yNameFromLabel(fillCombo, fillLabel);
        addAndMakeVisible(fillCombo);

        updateEnablement();
        startTimerHz(4);   // watch the follow lock (see timerCallback)
        setSize(juce::jmax(470, widthFor()), heightFor(heading));
    }

    void resized() override
    {
        const int lx = labelIndent(), lw = labelW();
        const int cx = comboX(),      cw = comboW();
        const int narrowW = edtext::optPx(140);   // the two four-item decay combos
        const int h = rowH();

        int y = 0;
        if (heading) { headingLabel.setBounds(10, y, getWidth() - 20, h); y += headH(); }
        midiIndicatorsToggle.setBounds(10, y, getWidth() - 20, h);  y += rowGap();
        playedNotesToggle   .setBounds(10, y, getWidth() - 20, h);  y += rowGap();
        overridesToggle     .setBounds(10, y, getWidth() - 20, h);  y += rowGap();
        markLabel.setBounds(lx, y, lw, h);
        markCombo.setBounds(cx, y, cw, h);                         y += rowGap();
        spectrumToggle      .setBounds(10, y, getWidth() - 20, h);  y += rowGap();
        resLabel  .setBounds(lx, y, lw, h);
        resCombo  .setBounds(cx, y, cw, h);                        y += rowGap();
        decayLabel.setBounds(lx, y, lw, h);
        decayCombo.setBounds(cx, y, narrowW, h);                   y += rowGap();
        meterToggle.setBounds(10, y, getWidth() - 20, h);           y += rowGap();
        meterDecayLabel.setBounds(lx, y, lw, h);
        meterDecayCombo.setBounds(cx, y, narrowW, h);              y += rowGap();
        rampLabel      .setBounds(lx, y, lw, h);
        rampCombo      .setBounds(cx, y, cw, h);                   y += rowGap();
        spreadLabel    .setBounds(lx, y, lw, h);
        spreadCombo    .setBounds(cx, y, cw, h);                   y += rowGap();
        fillLabel      .setBounds(lx, y, lw, h);
        fillCombo      .setBounds(cx, y, cw, h);
    }

private:
    // Options -> "Follow MIDI changes to the device" can be switched off from a menu
    // while this window is open, and nothing tells us. One bool compare per tick.
    void timerCallback() override
    {
        if (followApplied != proc.midiFollowEnabled)
            updateEnablement();
    }

    // The two spectrum settings only mean anything while the analyser is on, so grey
    // them with the toggle rather than leaving dead controls live.
    void updateEnablement()
    {
        // With following switched off there is nothing to draw and nothing being
        // collected, so the toggle and the style row are greyed -- but their stored
        // values are LEFT ALONE, so they come back as the user left them when
        // following is switched on again. Gate the control, never the value.
        followApplied = proc.midiFollowEnabled;
        overridesToggle.setEnabled(followApplied);
        const bool marks = followApplied && proc.showMidiOverrides;
        markLabel.setEnabled(marks);  markCombo.setEnabled(marks);

        const bool on = proc.eqSpectrumEnabled;
        resLabel  .setEnabled(on);  resCombo  .setEnabled(on);
        decayLabel.setEnabled(on);  decayCombo.setEnabled(on);

        const bool mon = proc.volumeMetersEnabled;
        meterDecayLabel.setEnabled(mon);  meterDecayCombo.setEnabled(mon);
        rampLabel      .setEnabled(mon);  rampCombo      .setEnabled(mon);
        spreadLabel    .setEnabled(mon);  spreadCombo    .setEnabled(mon);
        fillLabel      .setEnabled(mon);  fillCombo      .setEnabled(mon);
    }

    YamahaMU100AudioProcessor& proc;
    bool heading { false };

    juce::Label        headingLabel;
    juce::ToggleButton midiIndicatorsToggle;
    juce::ToggleButton playedNotesToggle;
    juce::ToggleButton overridesToggle;
    juce::Label        markLabel;
    juce::ComboBox     markCombo;
    bool               followApplied { true };
    juce::ToggleButton spectrumToggle;
    juce::Label        resLabel;
    juce::ComboBox     resCombo;
    juce::Label        decayLabel;
    juce::ComboBox     decayCombo;
    juce::ToggleButton meterToggle;
    juce::Label        meterDecayLabel;
    juce::ComboBox     meterDecayCombo;
    juce::Label        rampLabel;
    juce::ComboBox     rampCombo;
    juce::Label        spreadLabel;
    juce::ComboBox     spreadCombo;
    juce::Label        fillLabel;
    juce::ComboBox     fillCombo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DisplayAnimationComponent)
};

//==============================================================================
// AudioSettingsComponent — the Options ▸ "Audio…" window, both hosts. Everything
// here is about how audio LEAVES the plugin: the MAME buffer threshold, and — VST
// only — the PDC trim and the per-part output buses. Every editor/display preference
// that used to share this window with them now has its own menu entry — see
// showSettingsMenu().
//==============================================================================
class AudioSettingsComponent : public juce::Component
{
public:
    // `hosted` adds the two VST-only groups: the PDC trim row and the per-part
    // stereo output pair. Same answer, DIFFERENT reasons, and both are worth
    // keeping in mind before either is moved:
    //   - the per-part buses cannot be OPENED in the standalone at all (JUCE opens
    //     the main bus only), so the checkbox would promise something impossible;
    //   - the PDC trim is not impossible, it is INERT. Its only effect anywhere is
    //     setLatencySamples(), and the standalone runs through AudioProcessorPlayer,
    //     which never reads getLatencySamples() (only AudioProcessorGraph and the
    //     plugin-format wrappers do). So the control worked, persisted and changed
    //     nothing audible — the worst kind of dead control.
    // The value itself is NOT gated: pdcTrimIndex still lives in the shared
    // settings.xml and still round-trips, so a trim set in the VST survives a
    // standalone session untouched. Only the way in is hidden.
    explicit AudioSettingsComponent(YamahaMU100AudioProcessor& p,
                                    bool hosted = false)
        : proc(p), partOutputs(hosted), pdcTrim(hosted)
    {
        // Standalone: embed JUCE's audio device selector at the top so device /
        // sample-rate / buffer-size selection lives in the same window as our
        // buffer-threshold control. MIDI options are turned OFF here — MIDI
        // routing has its own window, and the stock "Active MIDI inputs" box is dead
        // in standalone (the plugin opens MIDI devices itself).
        if (juce::JUCEApplicationBase::isStandaloneApp())
        {
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
            {
                deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent>(
                    holder->deviceManager,
                    0, 0,        // no audio inputs
                    0, 2,        // stereo output
                    false,       // showMidiInputOptions  (removes redundant Active-inputs box)
                    false,       // showMidiOutputSelector
                    true,        // showChannelsAsStereoPairs
                    false);      // hideAdvancedOptionsWithButton
                addAndMakeVisible(*deviceSelector);
            }
        }

        bufferLabel.setText("Buffer threshold:", juce::dontSendNotification);
        bufferLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
        // Both row labels ride edtext::OptionsWin (settings.xml editorTextOptionsPct),
        // and the label column below is sized from the same scale.
        bufferLabel.setFont(edtext::font(edtext::OptionsWin, 15.f));
        addAndMakeVisible(bufferLabel);

        bufferCombo.addItem("128 samples   (~3ms)",   1);
        bufferCombo.addItem("256 samples   (~6ms)",   2);
        bufferCombo.addItem("512 samples   (~12ms)",  3);
        bufferCombo.addItem("1024 samples  (~23ms)",  4);
        bufferCombo.addItem("2048 samples  (~46ms)",  5);
        bufferCombo.addItem("4096 samples  (~93ms)",  6);
        bufferCombo.addItem("8192 samples  (~186ms)", 7);
        // The two large options were added chasing dropouts that turned out NOT to be
        // ours at all — an external 129 s context-switch storm
        // (DellClientManagementService firing WMI) that cost ~0.18 s of audio per
        // event, which 8192's 186 ms did not reliably cover. That cause is fixed at
        // the machine, so nothing needs these for it any more; Cubase runs two
        // instances clean at 1024. They stay because the lesson generalises:
        // bufferThreshold is a CEILING on how far ahead MAME may run, NOT a guarantee
        // of ring occupancy when something stalls, so surviving a stall can take more
        // cushion than the average-deficit arithmetic predicts. The number to judge it
        // by is ring occupancy, not this setting.
        // 32768 is kMaxBufferThreshold; both ring buffers are static_asserted against it.
        bufferCombo.addItem("16384 samples (~372ms)", 8);
        bufferCombo.addItem("32768 samples (~743ms)", 9);

        // Set combo to match current threshold
        auto thresh = proc.mameBufferThreshold.load();
        if      (thresh <= 128)  bufferCombo.setSelectedId(1, juce::dontSendNotification);
        else if (thresh <= 256)  bufferCombo.setSelectedId(2, juce::dontSendNotification);
        else if (thresh <= 512)  bufferCombo.setSelectedId(3, juce::dontSendNotification);
        else if (thresh <= 1024) bufferCombo.setSelectedId(4, juce::dontSendNotification);
        else if (thresh <= 2048) bufferCombo.setSelectedId(5, juce::dontSendNotification);
        else if (thresh <= 4096) bufferCombo.setSelectedId(6, juce::dontSendNotification);
        else if (thresh <= 8192) bufferCombo.setSelectedId(7, juce::dontSendNotification);
        else if (thresh <= 16384) bufferCombo.setSelectedId(8, juce::dontSendNotification);
        else                      bufferCombo.setSelectedId(9, juce::dontSendNotification);

        bufferCombo.onChange = [this]
        {
            static const int kThresholds[] = { 128, 256, 512, 1024, 2048,
                                               4096, 8192, 16384, 32768 };
            int id = bufferCombo.getSelectedId();
            if (id >= 1 && id <= (int) std::size(kThresholds))
                proc.setBufferThreshold(kThresholds[id - 1]);
        };
        a11yNameFromLabel(bufferCombo, bufferLabel);
        addAndMakeVisible(bufferCombo);

        // PDC Trim — compensates for plugin pipeline lag (DAW shifts events earlier).
        // VST only; see the ctor comment for why the standalone gets no row.
        if (pdcTrim)
        {
            trimLabel.setText("PDC Trim:", juce::dontSendNotification);
            trimLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
            trimLabel.setFont(edtext::font(edtext::OptionsWin, 15.f));
            addAndMakeVisible(trimLabel);

            trimCombo.addItemList({ "0 ms", "5 ms", "10 ms", "15 ms", "20 ms",
                                     "25 ms", "30 ms", "40 ms", "50 ms" }, 1);
            trimCombo.setSelectedId(
                juce::jlimit(0, 8, proc.pdcTrimIndex.load()) + 1,
                juce::dontSendNotification);
            trimCombo.onChange = [this]
            {
                const int id = trimCombo.getSelectedId();
                if (id >= 1 && id <= 9)
                    proc.setPdcTrimIndex(id - 1);
            };
            a11yNameFromLabel(trimCombo, trimLabel);
            addAndMakeVisible(trimCombo);
        }

        // Per-part stereo outputs — 32 extra stereo buses, one per part, so each
        // part can be processed on its own DAW track. Unlike everything the Options
        // menu carries as a checkmark, this is not a display preference: it turns on
        // a capture inside the emulated mixer, so it defaults off and stays off until
        // asked for. That is also why it belongs in this window and not on the menu.
        if (partOutputs)
        {
            partOutputsToggle.setButtonText("Separate stereo output per part");
            partOutputsToggle.setTooltip(
                "Give each of the 32 parts its own stereo output, so they can be routed "
                "to separate tracks in your DAW and processed with its plugins. You must "
                "also enable the extra outputs in the DAW.\n\n"
                "Each part's output is dry: pan and volume are applied, but the MU100's "
                "own reverb, chorus, variation and insertion effects are not.");
            partOutputsToggle.setColour(juce::ToggleButton::textColourId, edcol::kTextLabel);
            partOutputsToggle.setToggleState(proc.getPartOutputsEnabled(),
                                             juce::dontSendNotification);
            partOutputsToggle.onClick = [this]
            {
                proc.setPartOutputsEnabled(partOutputsToggle.getToggleState());
                partOutputsRemoveToggle.setEnabled(partOutputsToggle.getToggleState());
                proc.saveSettings();
            };
            addAndMakeVisible(partOutputsToggle);

            // ...and whether those parts should then come OUT of the main output.
            // Off by default: on its own the feature is additive, which is the
            // behaviour someone who just wants a part on its own track expects.
            partOutputsRemoveToggle.setButtonText("...and remove them from the main output");
            partOutputsRemoveToggle.setTooltip(
                "Take the parts you have routed to their own outputs out of the main "
                "stereo output, so they are not heard twice. Only parts whose output "
                "you have actually enabled in the DAW are removed.\n\n"
                "Their reverb and chorus tails still remain in the main output: the "
                "MU100 produces one shared effects return, which cannot be split up "
                "per part. Turn a part's own send levels down if you need it gone "
                "entirely.");
            partOutputsRemoveToggle.setColour(juce::ToggleButton::textColourId,
                                              edcol::kTextLabel);
            partOutputsRemoveToggle.setToggleState(proc.getPartOutputsRemoveFromMain(),
                                                   juce::dontSendNotification);
            partOutputsRemoveToggle.setEnabled(proc.getPartOutputsEnabled());
            partOutputsRemoveToggle.onClick = [this]
            {
                proc.setPartOutputsRemoveFromMain(partOutputsRemoveToggle.getToggleState());
                proc.saveSettings();
            };
            addAndMakeVisible(partOutputsRemoveToggle);
        }

        // Reserve space at the top for the device selector when present. The audio
        // rows are COUNTED rather than assumed, so dropping the trim row shortens
        // the window instead of leaving a hole where it used to be.
        selectorHeight = (deviceSelector != nullptr) ? 360 : 0;
        // Wide enough for the label column plus the widest combo, whatever the option
        // text scale is; never narrower than the window has always been.
        const int width  = juce::jmax(deviceSelector != nullptr ? 500 : 420,
                                      comboX() + edtext::optPx(220) + edtext::optPx(18));
        const int extra  = partOutputs ? kPartOutputsH : 0;
        const int rows   = 1 + (pdcTrim ? 1 : 0);
        setSize(width, selectorHeight + kRowsTop + rows * kRowPitch + kRowsPad + extra);
    }

    void resized() override
    {
        if (deviceSelector != nullptr)
            deviceSelector->setBounds(0, 0, getWidth(), selectorHeight);

        // A running cursor, not fixed offsets: the trim row is optional, and the
        // part-output rows must close up behind it when it is absent.
        int y = selectorHeight + kRowsTop;

        bufferLabel.setBounds(10, y + 1, labelW(), 24);
        bufferCombo.setBounds(comboX(), y, edtext::optPx(220), 24);
        y += kRowPitch;

        if (pdcTrim)
        {
            trimLabel.setBounds(10, y + 1, labelW(), 24);
            trimCombo.setBounds(comboX(), y, edtext::optPx(120), 24);
            y += kRowPitch;
        }

        if (partOutputs)
        {
            partOutputsToggle.setBounds(10, y + 1, getWidth() - 20, 24);
            // Indented: it is a qualifier on the row above, not a peer of it.
            partOutputsRemoveToggle.setBounds(30, y + 29, getWidth() - 40, 24);
        }
    }

private:
    // Audio-row geometry. kRowsTop is the first row's inset below the selector,
    // kRowPitch the spacing between rows, kRowsPad the breathing room under the
    // last one. Hosted (2 rows) reproduces the previous fixed 90 exactly.
    static constexpr int kRowPitch = 36;
    static constexpr int kRowsTop  = 9;
    static constexpr int kRowsPad  = 9;

    // One label column for the whole window, sized to "Buffer threshold:" and scaled
    // with the option-window text (edtext::OptionsWin). 170 is the old 150 plus the
    // room the +2 pt label needs.
    static int labelW() { return edtext::optPx(170); }
    static int comboX() { return labelW() + edtext::optPx(12); }

    // The two part-output rows plus the gap above and below them.
    static constexpr int kPartOutputsH = 64;

    YamahaMU100AudioProcessor& proc;
    bool partOutputs { false };
    bool pdcTrim     { false };

    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    int selectorHeight { 0 };

    juce::Label    bufferLabel;
    juce::ComboBox bufferCombo;
    juce::Label    trimLabel;
    juce::ComboBox trimCombo;

    // Per-part stereo outputs (only when partOutputs == true, i.e. the VST).
    juce::ToggleButton partOutputsToggle;
    juce::ToggleButton partOutputsRemoveToggle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioSettingsComponent)
};

//==============================================================================
// AboutComponent — shown in the About dialog (both standalone and VST).
//==============================================================================
class AboutComponent : public juce::Component
{
public:
    AboutComponent()
    {
        // The version comes from the .jucer (JucePlugin_VersionString) so there is only
        // one place to bump it; the build date is what makes a user's bug report
        // traceable to an actual binary.
        text.setText(
            "MEGAN XG v" JucePlugin_VersionString "  (build " __DATE__ ")\n\n"
            "Runs on the MAME Yamaha MU100 emulator engine by MAMEDev and contributors, "
            "ported from MAME to JUCE with extensive bug fixing and adaptation by "
            "Dave Wheitner and AI. Borrows from the SD-1/32 MAME to JUCE port by "
            "Sojus Records. Editor views follow the layout of XGEdit95 by Gary Gregson, "
            "rebuilt in JUCE, updated and expanded by Dave Wheitner and AI. "
            // Credit for guidance FOLLOWED, not code taken -- the same distinction the
            // XGEdit95 sentence above makes. The caveat is not modesty: naming someone
            // beside the word "accessibility" implies they delivered a working accessible
            // plugin, and the shortfall here is ours. Other UI influences (Surge XT,
            // Pigments, Bitwig, Cubase) are deliberately NOT listed -- a partial list
            // reads as a judgement about who deserved credit; the full one is in
            // PROVENANCE.md section 8, which has room for it.
            "Accessibility support follows guidance published by joeloftusdev; it is "
            "groundwork only and has not been tested with a screen reader.\n\n"
            "Includes MAME(R) emulator components and is built with JUCE. "
            "MEGAN XG is licensed under the GNU AGPL v3.\n\n"
            "Provided as-is, as a hobby project. No support or future updates are promised.\n"
            "All trademarks are the property of their respective owners. "
            "Not affiliated with or endorsed by Yamaha Corporation.",
            juce::dontSendNotification);
        text.setFont(juce::FontOptions(13.f));
        text.setColour(juce::Label::textColourId, edcol::kTextTitle);
        text.setJustificationType(juce::Justification::topLeft);
        text.setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(text);

        // 225 -> 260 for the accessibility sentence above (~2 lines at 13px in a
        // 456px-wide label). VERIFY A CHANGE HERE BY CAPTURING THE DIALOG, never by
        // estimating from the character count -- the label does not scroll, so text
        // past the bottom is silently gone rather than clipped visibly mid-glyph.
        setSize(480, 260);
    }

    void resized() override
    {
        text.setBounds(12, 12, getWidth() - 24, getHeight() - 24);
    }

private:
    juce::Label text;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutComponent)
};

//==============================================================================
// Button definitions — image coordinates are from the original full-resolution
// image (before scaling). Port/mask come from INPUT_PORTS_START(mu100) in ymmu100.cpp.
//==============================================================================
struct ButtonDef
{
    const char* label;
    const char* port;   // ":P7" or ":P8"
    uint8_t     mask;   // bit mask within that port
    float       cx, cy; // center in full-resolution image coordinates
    float       radius; // hit radius in full-resolution image coordinates
    int         ledBit; // bit index into mulcd_device::get_leds(), or -1 if no LED
};

// LED bit assignments from mulcd_device::get_leds() (see ymmu100.cpp pf_w / set_leds):
// bit0=Play bit1=Edit bit2=Util bit3=Effect bit4=Mode bit5=EQ
static constexpr int kLedBitPlay   = 0;
static constexpr int kLedBitEdit   = 1;
static constexpr int kLedBitUtil   = 2;
static constexpr int kLedBitEffect = 3;
static constexpr int kLedBitMode   = 4;
static constexpr int kLedBitEq     = 5;
static constexpr int kLedBitNone   = -1;

static constexpr ButtonDef k_buttons[] =
{
    // Port P7: Enter=0x01 Effect=0x02 Util=0x04 Part+=0x08 Part-=0x10 Mute/Solo=0x20 Edit=0x40 Play=0x80
    { "Play",      ":P7", 0x80, 1502.f, 125.f, 20.f, kLedBitPlay   },
    { "Edit",      ":P7", 0x40, 1594.f, 125.f, 20.f, kLedBitEdit   },
    { "Util",      ":P7", 0x04, 1502.f, 229.f, 20.f, kLedBitUtil   },
    { "Effect",    ":P7", 0x02, 1594.f, 229.f, 20.f, kLedBitEffect },
    { "Mute/Solo", ":P7", 0x20, 1737.f, 122.f, 26.f, kLedBitNone   },
    { "Part -",    ":P7", 0x10, 1833.f, 122.f, 26.f, kLedBitNone   },
    { "Part +",    ":P7", 0x08, 1926.f, 122.f, 26.f, kLedBitNone   },
    { "Enter",     ":P7", 0x01, 1737.f, 230.f, 26.f, kLedBitNone   },
    // Port P8: Mode=0x01 Eq=0x02 Exit=0x04 Sel>=0x08 Sel<=0x10 Val+=0x20 Val-=0x40
    { "Mode",      ":P8", 0x01, 1502.f, 335.f, 20.f, kLedBitMode   },
    { "EQ",        ":P8", 0x02, 1594.f, 335.f, 20.f, kLedBitEq     },
    { "Exit",      ":P8", 0x04, 1737.f, 340.f, 26.f, kLedBitNone   },
    { "Sel <",     ":P8", 0x10, 1833.f, 230.f, 26.f, kLedBitNone   },
    { "Sel >",     ":P8", 0x08, 1926.f, 230.f, 26.f, kLedBitNone   },
    { "Val -",     ":P8", 0x40, 1833.f, 340.f, 26.f, kLedBitNone   },
    { "Val +",     ":P8", 0x20, 1926.f, 340.f, 26.f, kLedBitNone   },
};
static constexpr int k_numButtons = (int)(sizeof(k_buttons) / sizeof(k_buttons[0]));

// "All Macro" trigger — pressing Part- and Part+ simultaneously. Its region is
// hwpanel::kAllButton, the drawn ALL button itself, so the two cannot drift.
static constexpr uint8_t kAllMacroMaskP7 = 0x10 | 0x08; // Part- | Part+

// LCD region in full-resolution image coordinates
static constexpr float kLcdLeft   = 499.f;
static constexpr float kLcdTop    = 80.f;
static constexpr float kLcdRight  = 1338.f;
static constexpr float kLcdBottom = 367.f;

// LCD pixel grid dimensions
// Physical display is 4×16. mu_screen_update reads exactly 64 chars (16×4).
// 2-line HD44780: VR0=buf[0..15], VR2=buf[16..31], VR1=buf[40..55], VR3=buf[56..71]
static constexpr int kLcdCols  = 80;  // 16 chars × 5 px/char
static constexpr int kLcdRows  = 32;  // 4 lines × 8 px/line
static constexpr int kLcdChars = 17;
static constexpr int kLcdLines = 4;

//==============================================================================
YamahaMU100AudioProcessorEditor::YamahaMU100AudioProcessorEditor(YamahaMU100AudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    // The front panel is drawn in code (HardwarePanelArt.h). The tables below —
    // k_buttons, the LCD rectangle, the All Macro box — are in the old
    // megan-xg-hardware-view.png's coordinates, which the image is a crop of: subtract
    // hwpanel::kArtX0 from an x before scaling it. The PNG is no longer read: it derived
    // from a photograph and could not be redistributed.
    bgImage = hwpanel::render();

    // Scale to fit 1200px width
    const float targetW = 1200.f;
    imgScale = bgImage.isValid() ? targetW / (float)bgImage.getWidth() : 1.f;
    const int editorW = (int)targetW;
    const int editorH = bgImage.isValid() ? (int)(bgImage.getHeight() * imgScale) : 420;

    hwW = editorW;
    hwH = editorH + 34;               // +34 for control strip at bottom
    // Kept because hwW/hwH become "the size the user left it at" the moment anything
    // resizes; this pair is what "reset window size" goes back to.
    defaultHwW = hwW;
    defaultHwH = hwH;
    hwAspect = (float)hwW / (float)hwH;
    setSize(hwW, hwH);

    setResizable(true, true);
    if (bgImage.isValid())
    {
        getConstrainer()->setFixedAspectRatio(hwAspect);
        setResizeLimits(600, juce::roundToInt(600.f / hwAspect),
                        2400, juce::roundToInt(2400.f / hwAspect));
    }

    // Seed the palette + LookAndFeel BEFORE the view is built, so a session that
    // opens straight into the Light editor constructs its labels with the light
    // colours rather than building dark and rebuilding a moment later. The theme is
    // only live while the editor view is the one on screen (see wantLightTheme()),
    // so the Hardware default keeps the dark chrome the photo is framed in.
    edcol::setLightTheme(audioProcessor.defaultEditorView.equalsIgnoreCase("Editor")
                         && audioProcessor.editorTheme.equalsIgnoreCase("Light"));
    lookAndFeel.setColourScheme(edcol::isLight()
                                    ? juce::LookAndFeel_V4::getLightColourScheme()
                                    : juce::LookAndFeel_V4::getDarkColourScheme());
    setLookAndFeel(&lookAndFeel);

    // Editor view (the XGEdit-style parameter editor). Hidden until the user
    // toggles to it (or it is the saved default view).
    editorView = std::make_unique<EditorView>(audioProcessor);
    editorView->onKeyboardFocusReturn = [this] { restoreKeyboardFocus(false); };
    editorView->onPartContextMenu     = [this](int p) { showPartContextMenu(p); };
    editorView->setVisible(false);
    addChildComponent(*editorView);

    // Bottom bar buttons: File, Options, About. The Hardware/Editor toggle that used
    // to sit here is gone — the view is picked from Options -> View, which is also
    // what the plugin reopens in, so one control now does both jobs.
    // setWantsKeyboardFocus(false) on every one of these -- see the note on their
    // declarations. It also means clicking one leaves focus on the editor itself,
    // which is what keeps the shortcuts alive after a trip through a menu.
    fileButton.setWantsKeyboardFocus(false);
    fileButton.onClick = [this] { showFileMenu(); };
    addAndMakeVisible(fileButton);

    settingsButton.setWantsKeyboardFocus(false);
    settingsButton.onClick = [this] { showSettingsMenu(); };
    addAndMakeVisible(settingsButton);

    helpButton.setWantsKeyboardFocus(false);
    helpButton.onClick = [this] { showHelpMenu(); };
    addAndMakeVisible(helpButton);

    // PANIC — all-sound-off / all-notes-off + sustain & sostenuto released (both ports).
    // Moved from the System module so it is reachable from either view.
    panicButton.setWantsKeyboardFocus(false);
    panicButton.setColour(juce::TextButton::buttonColourId, edcol::kPanicBg);
    panicButton.setColour(juce::TextButton::textColourOffId, edcol::kPanicText);
    panicButton.onClick = [this] { panicClicked(); };
    addAndMakeVisible(panicButton);

    // Standing mute/solo indicator. Hidden until something is engaged; refreshMuteSoloIndicator
    // owns its text, colour and visibility.
    muteSoloIndicator.setWantsKeyboardFocus(false);
    muteSoloIndicator.onClick = [this]
    {
        audioProcessor.setPartSoloMask(0);
        audioProcessor.setPartMuteMask(0);
        audioProcessor.setPartEqBypassMask(0);
    };
    muteSoloIndicator.setVisible(false);
    addChildComponent(muteSoloIndicator);

    // The MIDI-override badge. Clicking it CLEARS -- puts the device back on what
    // the panels show -- rather than keeping: clearing is the reversible one (the
    // song can move the device again at any time), and keeping changes the setup
    // you are about to save, which should cost a trip to the menu.
    overrideIndicator.setWantsKeyboardFocus(false);
    overrideIndicator.onClick = [this]
    {
        const int n = audioProcessor.midiOverrideTotal();
        audioProcessor.clearMidiOverrides(true);
        refreshOverrideIndicator();
        sayOverrideAction(juce::String(n) + (n == 1 ? " MIDI override cleared"
                                                    : " MIDI overrides cleared")
                          + ", the device is back on your values");
    };
    overrideIndicator.setVisible(false);
    addChildComponent(overrideIndicator);

    midiOutIndicator.setWantsKeyboardFocus(false);
    midiOutIndicator.onClick = [this]
    {
        audioProcessor.midiOutFormat = YamahaMU100AudioProcessor::MidiOutSysEx;
        audioProcessor.saveSettings();
        applyAutomationMarkers();   // the CC marks go, and so does this badge
    };
    midiOutIndicator.setVisible(false);
    addChildComponent(midiOutIndicator);
    // The editor is what redraws when a bit moves, wherever it moved from -- a cell
    // click, this button, or a second editor on the same processor.
    audioProcessor.onPartMuteSoloChanged = [this]
    {
        refreshMuteSoloIndicator();
        if (editorView != nullptr) editorView->refreshMuteSolo();
        // Worth announcing because the EXCLUSIVE forms change up to 31 parts the user
        // never touched, and because the state is otherwise only visible. The counts
        // are what matters, not which parts -- naming 31 of them would be unusable.
        say(mu100a11y::announce::Edit, muteSoloSummary());
    };

    // EQ bypass, the same shape: one place redraws, whichever surface moved the bit.
    audioProcessor.onPartEqBypassChanged = [this]
    {
        refreshMuteSoloIndicator();      // the badge carries the bypass count too
        if (editorView != nullptr) editorView->refreshEqBypass();
        // Announced for the same reason mute/solo is: the all-parts form changes 31
        // parts the user never touched, and a read-back RELEASES the bypass without
        // being asked -- a state change a screen-reader user would otherwise not learn
        // about at all.
        say(mu100a11y::announce::Edit, eqBypassSummary());
    };

    // Undo / Redo. The processor bumps editorStateExternalSeq, which the timer below
    // already watches — but that path is throttled to ~7.5 Hz, and a button should
    // repaint the panels on the click, so refresh directly here and adopt the new
    // sequence number so the timer doesn't then do it a second time.
    undoButton   .setWantsKeyboardFocus(false);
    historyButton.setWantsKeyboardFocus(false);
    redoButton   .setWantsKeyboardFocus(false);
    undoButton.onClick    = [this] { audioProcessor.undo(); afterUndoApply(); };
    redoButton.onClick    = [this] { audioProcessor.redo(); afterUndoApply(); };
    // The hamburger between the arrows opens the HISTORY window (see HistoryPanel.h).
    historyButton.onClick = [this] { toggleHistoryPanel(); };
    // Added HIDDEN: they are Editor-view only (see setView — nothing done on the front
    // panel goes through the undo stack). The setView() call below reveals them when the
    // plugin opens straight into an Editor view.
    addChildComponent(undoButton);
    addChildComponent(historyButton);
    addChildComponent(redoButton);
    refreshUndoButtons();

    // Keyboard shortcuts. The editor is the LAST stop in JUCE's focus chain, so it has
    // to be able to HOLD focus for a key to reach it at all when nothing inside has been
    // clicked. It never GRABS focus -- that stays the host's decision.
    setWantsKeyboardFocus(true);
    refreshShortcutTooltips();

    // Loaded-file display, right side of the bottom strip. Non-clicking (no controls),
    // just shows the path/name + unsaved-changes marker; the timer keeps it current.
    addAndMakeVisible(filePathDisplay);

    // "Loading file" overlay — added last so it sits above both the Editor view and
    // the bottom button strip, covering whichever view is on screen. Hidden until a
    // .syx load is in flight.
    loadOverlay = std::make_unique<BusyOverlay>("Loading file.",
                                                "Please don't close this window");
    addChildComponent(*loadOverlay);

    // Restore the Editor-view size the user left last session (clamped to the
    // editor's resize limits). Seeded before setView() so opening directly in the
    // Editor view uses the persisted size.
    edW = juce::jlimit(560, 3000, audioProcessor.editorViewW);
    edH = juce::jlimit(360, 2000, audioProcessor.editorViewH);

    viewsReady = true;

    // Seed the analyser-flag mirror from the persisted setting, so a session that
    // starts with the spectrum switched off doesn't read as an off->on edge on the
    // first timer tick.
    eqSpectrumApplied = audioProcessor.eqSpectrumEnabled;
    volumeMetersApplied = audioProcessor.volumeMetersEnabled;
    volumeMeterRampApplied = audioProcessor.volumeMeterRampIdx;
    // Same for the plugin-only lock (EditorView's ctor applies it to the panels).
    pluginOnlyApplied = audioProcessor.enablePluginOnlyParams;
    // ...and the receive-switch row (EditorView's ctor likewise applies it itself).
    rcvSwitchesApplied = audioProcessor.showPartRcvSwitches;
    muteSoloColsApplied = audioProcessor.showMuteSoloColumns;
    // ...and the MIDI-input indicators (PartListPanel's ctor starts or stops its own
    // fade timer from the flag, so this only seeds the edge detector).
    midiIndicatorsApplied = audioProcessor.midiInputIndicators;
    // ...and the voice-name form. The live global is already set (the PROCESSOR ctor
    // does it, since bus names need it with no editor around), so this only seeds the
    // edge detector.
    voiceNamesApplied = audioProcessor.shortVoiceNames;
    // ...and the changed markers. Same shape: the PROCESSOR ctor already set the live
    // global (a panel built before this editor exists must read the right value), so
    // this only seeds the edge detector.
    changedMarksApplied = audioProcessor.showChangedMarks;
    // ...and the DAW-automatable markers (ditto), plus the automation switch they
    // follow -- seeded together, since applyAutomationMarkers() pushes both.
    automationMarkerApplied = markerSettingsSig();
    dawAutomationApplied    = audioProcessor.dawAutomationEnabled;
    midiMarkStyleApplied    = audioProcessor.midiOverrideMarkStyle;

    // Open in the saved default view.
    if (audioProcessor.defaultEditorView.equalsIgnoreCase("Editor"))
        setView(true);

    // Drop any MIDI-OUT dump residue the device may have transmitted after a
    // previous editor was closed mid-read-back. Otherwise the timer would finalize
    // that stale burst (no read-back pending now) and pop a spurious Save dialog.
    audioProcessor.clearDumpCapture();

    applyTooltipEnabled();   // honour the persisted Show-tooltips setting

    // From here a setView() call is the user navigating, not the window opening — see
    // the stale-model warning at the end of setView().
    viewInitDone = true;

    // The override marks and badge, ONCE, before the timer takes over. The timer only
    // acts on a CHANGE of the override sequence, and reopening the GUI on a processor
    // that already carries overrides is not a change -- without this the marks would
    // not appear until the next message happened to arrive.
    lastMidiOverrideSeq = audioProcessor.midiOverrideSequence();
    if (editorView != nullptr) editorView->refreshMidiOverrides();
    refreshOverrideIndicator();
    refreshMidiOutBadge();   // the mirror is seeded above, so it would never fire for the opening state
    // ...and adopt the count as already announced. Reopening the GUI on a processor
    // that carries overrides is not news arriving; the marks and the badge are simply
    // there, and a reader landing on a control hears its own.
    ovAnnouncedCount = audioProcessor.showMidiOverrides
                     ? audioProcessor.midiOverrideTotal() : 0;

    startTimerHz(30);
}

void YamahaMU100AudioProcessorEditor::applyTooltipEnabled()
{
    const int mode = audioProcessor.tooltipMode;
    // Only the "pop-ups near cursor" mode uses the TooltipWindow. A huge delay =
    // effectively off (there is no hide-all API); 700ms = the normal delay.
    const bool popups = (mode == YamahaMU100AudioProcessor::TooltipPopup);
    tooltipWindow.setMillisecondsBeforeTipAppears(popups ? 700 : 24 * 60 * 60 * 1000);
    tooltipModeApplied = mode;

    // Leaving bottom-bar mode must hand the strip back to the filename immediately,
    // not on the next hover.
    if (mode != YamahaMU100AudioProcessor::TooltipBottomBar)
    {
        tipShownInBar = false;
        tipLastComp   = nullptr;
        tipLastText   = {};
        filePathDisplay.setTooltipOverride({});
    }
}

//==============================================================================
// Bottom-bar tooltip mode. Mirrors TooltipWindow::timerCallback's logic — find the
// TooltipClient under the mouse, restart the delay whenever the tip changes or the
// mouse moves quickly, then show it — but writes the text into the bottom strip
// instead of a pop-up window.
void YamahaMU100AudioProcessorEditor::serviceBottomBarTooltip()
{
    if (audioProcessor.tooltipMode != YamahaMU100AudioProcessor::TooltipBottomBar)
        return;   // applyTooltipEnabled() already cleared the strip

    const auto mouseSource = juce::Desktop::getInstance().getMainMouseSource();
    auto* comp = mouseSource.isTouch() ? nullptr : mouseSource.getComponentUnderMouse();

    // Only our own window's controls, and never the readout itself: the strip already
    // shows the path, so echoing its tooltip there would just re-print it in place.
    if (comp != nullptr && comp != this && ! isParentOf(comp))
        comp = nullptr;
    if (comp == &filePathDisplay)
        comp = nullptr;

    juce::String tip;
    if (comp != nullptr
        && ! juce::ModifierKeys::getCurrentModifiers().isAnyMouseButtonDown()
        && ! comp->isCurrentlyBlockedByAnotherModalComponent())
    {
        if (auto* client = dynamic_cast<juce::TooltipClient*>(comp))
            tip = client->getTooltip();
    }

    const auto pos          = mouseSource.getScreenPosition().roundToInt();
    const bool movedQuickly = pos.getDistanceFrom(tipLastMousePos) > 12;
    tipLastMousePos = pos;

    const auto now        = juce::Time::getApproximateMillisecondCounter();
    const bool tipChanged = (tip != tipLastText || comp != tipLastComp.getComponent());
    tipLastComp = comp;
    tipLastText = tip;

    // A quick move only defers a tip that has not appeared yet — once it is up,
    // moving on within the same control must not make it flicker.
    if (tipChanged || (! tipShownInBar && movedQuickly))
        tipChangeMs = now;

    if (tip.isEmpty())
        tipShownInBar = false;
    else if (tipChanged)
        tipShownInBar = false;
    else if (! tipShownInBar && now >= tipChangeMs + (juce::uint32) kBottomBarTipDelayMs)
        tipShownInBar = true;

    filePathDisplay.setTooltipOverride(tipShownInBar ? tip : juce::String());
}

//==============================================================================
// Editor theme — Options -> View -> Dark Editor / Light Editor.
//
// The palette is a set of globals (edcol), so installing a theme is two steps:
// rewrite them, then rebuild anything that already READ them. Almost every colour
// in the editor is read at paint time and would only need a repaint — but each
// juce::Label's text colour is set once in a constructor, and those are most of the
// editor's text. Rebuilding EditorView is the honest way to catch both: the view
// has a supported "construct, then push the model in" path (applyDeviceState), so a
// rebuild costs a repaint and nothing else. The undo stack lives in the PROCESSOR,
// not here, so it survives.
//
// The LookAndFeel switch matters as much as the palette: everything the editor does
// NOT colour explicitly — popup menus, the ValueBox's inline text editor, list
// scrollbars, dialog chrome — comes from it, and a light editor popping dark menus
// would read as a bug rather than a theme.
// A top-level PopupMenu or DialogWindow is NOT a child of this editor, so it never
// inherits the LookAndFeel set on it — JUCE falls back to the process-wide default,
// which is dark. That is why a light editor still popped dark menus until each one
// was handed the editor's own LookAndFeel explicitly. Setting the process default
// instead would be the one-line fix and the wrong one: it is shared with the host
// and with every other instance of this plugin, which may be on the other theme.
//
// Returns its argument so a launch site can stay one statement.
juce::DialogWindow* YamahaMU100AudioProcessorEditor::themeDialog(juce::DialogWindow* w)
{
    if (w != nullptr)
    {
        w->setLookAndFeel(&lookAndFeel);
        // Every dialog this editor opens passes through here, which makes it the one
        // place that can notice one CLOSING -- there is no close callback on an
        // async DialogWindow, and a SafePointer going null is the only signal. The
        // timer watches it and hands the keyboard back (see serviceKeyboardFocus).
        openDialog    = w;
        dialogWasOpen = true;
    }
    return w;
}

bool YamahaMU100AudioProcessorEditor::wantLightTheme() const
{
    // Gated on the editor being shown: the Hardware view is a photo of a front
    // panel, so it has no light variant and keeps the dark chrome around it.
    return showingEditor && audioProcessor.editorTheme.equalsIgnoreCase("Light");
}

void YamahaMU100AudioProcessorEditor::applyEditorTheme()
{
    const bool light = wantLightTheme();
    if (light == edcol::isLight() && editorView != nullptr)
        return;                       // already installed; don't rebuild for nothing

    edcol::setLightTheme(light);
    lookAndFeel.setColourScheme(light ? juce::LookAndFeel_V4::getLightColourScheme()
                                      : juce::LookAndFeel_V4::getDarkColourScheme());
    setLookAndFeel(&lookAndFeel);

    // Colours the editor's own chrome sets on concrete components (rather than
    // drawing itself) have to be re-pushed — the LookAndFeel does not own them.
    panicButton.setColour(juce::TextButton::buttonColourId,  edcol::kPanicBg);
    panicButton.setColour(juce::TextButton::textColourOffId, edcol::kPanicText);
    refreshMuteSoloIndicator();   // same reason: its fill is an edcol token, not a LookAndFeel one
    refreshOverrideIndicator();   // likewise: kMidiOverride is a theme token
    refreshMidiOutBadge();        // ...and kOrange

    if (editorView != nullptr)
    {
        // Rebuild in place, then push the model straight back in so the new panels
        // open showing the current patch rather than the factory defaults.
        const bool wasVisible = editorView->isVisible();
        editorView.reset();
        editorView = std::make_unique<EditorView>(audioProcessor);
        editorView->onKeyboardFocusReturn = [this] { restoreKeyboardFocus(false); };
    editorView->onPartContextMenu     = [this](int p) { showPartContextMenu(p); };
        addChildComponent(*editorView);
        editorView->setBounds(0, 0, getWidth(), juce::jmax(0, getHeight() - 34));
        editorView->setVisible(wasVisible);
        editorView->notifyViewShown(wasVisible);
        editorView->applyDeviceState();

        // The overlays must stay ABOVE the view that was just re-added on top of them.
        if (loadOverlay  != nullptr) loadOverlay->toFront(false);
        if (historyPanel != nullptr && historyPanel->isVisible()) historyPanel->toFront(false);
    }

    repaint();
}

YamahaMU100AudioProcessorEditor::~YamahaMU100AudioProcessorEditor()
{
    stopTimer();

    // The processor outlives this editor and holds a `this`-capturing lambda.
    audioProcessor.onPartMuteSoloChanged = nullptr;

    // Clear mute and solo with the window. They are not persisted anywhere, and with
    // no editor there is no M/S column and no bottom-bar indicator either -- so a
    // part left muted here would be a plugin that is silent with literally nothing on
    // screen to say why, in the one case (the host destroys the editor on window
    // close while the audio keeps running) where the user cannot look. Losing an
    // audition across a window close is the far cheaper mistake.
    audioProcessor.setPartSoloMask(0);
    audioProcessor.setPartMuteMask(0);

    // Before `lookAndFeel` (a member, destroyed after this body runs) goes away.
    setLookAndFeel(nullptr);

    // Persist the Editor-view window size so it reopens where the user left it.
    // resized() keeps audioProcessor.editorViewW/H current with the last ON-SCREEN
    // size (it deliberately ignores construction/teardown transients), so just
    // flush them to disk here — don't copy edW/edH, which may have been clamped to
    // the minimum during teardown.
    audioProcessor.saveSettings();
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(edcol::kHostBg);

    // Editor view: the EditorView child paints the area above the control strip;
    // we only need the dark fill behind the strip here.
    if (showingEditor)
        return;

    if (bgImage.isValid())
    {
        g.drawImage(bgImage, 0, 0, (int)(bgImage.getWidth() * imgScale),
                    (int)(bgImage.getHeight() * imgScale),
                    0, 0, bgImage.getWidth(), bgImage.getHeight());
    }

    // Status overlay if not booted or ROM missing
    if (audioProcessor.isRomMissing.load())
    {
        g.setColour(juce::Colour(0xcc000000));
        g.fillRect(0, 0, getWidth(), getHeight() - 34);
        g.setColour(juce::Colours::orangered);
        g.setFont(juce::FontOptions(18.f, juce::Font::bold));
        g.drawText("ROM missing — place mu100.zip in:\n" + audioProcessor.romPath,
                   10, 10, getWidth() - 20, 60, juce::Justification::centredLeft);
        return;
    }

    if (!audioProcessor.mameIsFullyBooted.load())
    {
        // A crashed engine also leaves mameIsFullyBooted false, and it never comes
        // back — so say so rather than showing "Booting..." forever. The SEH filter
        // around MAME swallows the fault, and this is the only place it surfaces.
        const auto fault = audioProcessor.mameFaultCode.load(std::memory_order_acquire);

        g.setColour(juce::Colour(0x88000000));
        g.fillRect(0, 0, getWidth(), getHeight() - 34);
        g.setColour(fault != 0 ? juce::Colours::orangered : juce::Colours::white);
        g.setFont(juce::FontOptions(20.f, juce::Font::bold));
        g.drawText(fault != 0
                       ? "Emulation engine stopped (0x"
                             + juce::String::toHexString((int) fault) + ") — reopen the plugin"
                       : juce::String("Booting..."),
                   0, 0, getWidth(), getHeight() - 34, juce::Justification::centred);
        return;
    }

    // Draw LCD
    const float lcdLeft   = (kLcdLeft - hwpanel::kArtX0) * imgScale;
    const float lcdTop    = kLcdTop    * imgScale;
    const float lcdWidth  = (kLcdRight  - kLcdLeft) * imgScale;
    const float lcdHeight = (kLcdBottom - kLcdTop)  * imgScale;

    // LCD background (#b2ff33)
    g.setColour(juce::Colour(0xffb2ff33));
    g.fillRect(lcdLeft, lcdTop, lcdWidth, lcdHeight);

    // Gap grid: 16 chars × 6 units (5 pixel + 1 gap col), 4 lines × 9 units (8 pixel + 1 gap row)
    // +1 in denominator adds a 1-cell margin on left and top (matching the natural gap margin on right/bottom)
    const int kTotalCols = kLcdChars * 6;   // 96
    const int kTotalRows = kLcdLines * 9;   // 36
    const float cellW = lcdWidth  / (float)(kTotalCols + 1);
    const float cellH = lcdHeight / (float)(kTotalRows + 1);
    const float dotW  = cellW * 0.82f;
    const float dotH  = cellH * 0.82f;
    const float dxOff = (cellW - dotW) * 0.5f;
    const float dyOff = (cellH - dotH) * 0.5f;

    // VR2 indicator clusters and VR3 icon chars use 2/3-scale rendering (matching MAME SVG pixel size)
    const float compScale = 2.f / 3.f;
    const float compCW    = cellW * compScale;
    const float compCH    = cellH * compScale;
    const float compDW    = compCW * 0.82f;
    const float compDH    = compCH * 0.82f;
    const float compDxOff = (compCW - compDW) * 0.5f;
    const float compDyOff = (compCH - compDH) * 0.5f;

    g.setColour(juce::Colour(0xff182206));
    for (int row = 0; row < kTotalRows; ++row)
    {
        const int rowInChar = row % 9;
        if (rowInChar == 8) continue;   // line gap row — background shows
        const int lineY = row / 9;
        for (int col = 0; col < kTotalCols; ++col)
        {
            const int bitZ = col % 6;
            if (bitZ == 5) continue;    // char gap column — background shows
            const int charX   = col / 6;
            // Physical slot layout (from mulcd.svg):
            //  VR0 (y≈87.5): slots 0–16; 17 chars
            //  VR1 (y≈99.3): slots 40–56; 17 chars
            //  VR2 indicators (y≈120.3, small px): slots 20–36; draw +5 chars right to align over VR3 icons
            //  VR3 (y≈122.0, normal px): slots 17–18 at positions 0–1, then slots 57–71 at 2–16
            static const int kBufRow[4] = { 0, 40, 20, 55 };
            const int idx = (lineY == 3 && charX < 2)
                ? 17 + charX          // VR3 positions 0-1: '01' part-number prefix
                : kBufRow[lineY] + charX;
            const int byteOff = idx * 16 + rowInChar;
            if (byteOff >= YamahaMU100AudioProcessor::LCD_BUF_SIZE) continue;
            if (!((lcdSnapshot[byteOff] >> (4 - bitZ)) & 1)) continue;
            // VR2 and VR3 icon chars (charX>=6) use compressed 2/3-scale rendering;
            // VR2 is also shifted 5 char-widths right to sit above the VR3 icon cluster.
            float px, py, pw, ph;
            if (lineY == 2) {
                const float cx = (5 * 6 + 1) + charX * 6.f * compScale;
                px = lcdLeft + cx * cellW + bitZ * compCW + compDxOff;
                // Bottom-align within the row so clusters sit close to VR3 icons below them
                py = lcdTop  + (lineY * 9 + 1) * cellH + 8.f * cellH * (1.f - compScale)
                             + rowInChar * compCH + compDyOff;
                pw = compDW;  ph = compDH;
            } else if (lineY == 3 && charX >= 5) {
                // Icons start at charX=5; anchor at position 5 to align over VR2 clusters
                const float cx = (5 * 6 + 1) + (charX - 5) * 6.f * compScale;
                px = lcdLeft + cx * cellW + bitZ * compCW + compDxOff;
                py = lcdTop  + (lineY * 9 + 1) * cellH + rowInChar * compCH + compDyOff;
                pw = compDW;  ph = compDH;
            } else {
                px = lcdLeft + (col + 1) * cellW + dxOff;
                py = lcdTop  + (row + 1) * cellH + dyOff;
                pw = dotW;    ph = dotH;
            }
            g.fillRect(px, py, pw, ph);
        }
    }

    // Light up Play/Edit/Util/Effect/Mode/EQ buttons whose front-panel LED is lit,
    // using the same green as the LCD background.
    const uint8_t leds = audioProcessor.ledState.load(std::memory_order_relaxed);
    g.setColour(juce::Colour(0xccb2ff33));
    for (const auto& b : k_buttons)
    {
        if (b.ledBit < 0 || !((leds >> b.ledBit) & 1))
            continue;
        const float cx = (b.cx - hwpanel::kArtX0) * imgScale;
        const float cy = b.cy * imgScale;
        const float r  = b.radius * imgScale * 0.9f;
        g.fillEllipse(cx - r, cy - r, r * 2.f, r * 2.f);
    }

    // Highlight the active (pressed) button with a semi-transparent circle
    if (activeButtonIdx >= 0 && activeButtonIdx < k_numButtons)
    {
        const auto& b = k_buttons[activeButtonIdx];
        g.setColour(juce::Colour(0x66ffffff));
        const float cx = (b.cx - hwpanel::kArtX0) * imgScale;
        const float cy = b.cy * imgScale;
        const float r  = b.radius * imgScale;
        g.fillEllipse(cx - r, cy - r, r * 2.f, r * 2.f);
    }

    // The ALL button's pressed state, drawn in art space over the static image.
    if (allMacroActive)
    {
        juce::Graphics::ScopedSaveState save(g);
        g.addTransform(juce::AffineTransform::translation(-(float) hwpanel::kArtX0, 0.f)
                           .scaled(imgScale));
        hwpanel::allButton(g, true);
    }
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::resized()
{
    imgScale = bgImage.isValid() ? (float)getWidth() / (float)bgImage.getWidth() : 1.f;
    const int stripY = getHeight() - 34;
    const int btnW = 90;
    const int btnH = 24;
    const int gap  = 8;
    fileButton      .setBounds(8,                       stripY + 5, btnW, btnH);
    settingsButton  .setBounds(8 + (btnW + gap),        stripY + 5, btnW, btnH);
    helpButton      .setBounds(8 + 2 * (btnW + gap),    stripY + 5, btnW, btnH);
    // PANIC took the slot the Hardware/Editor toggle used to hold; everything to its
    // right moved one slot left with it, so the file display gained 98 px.
    panicButton     .setBounds(8 + 3 * (btnW + gap),    stripY + 5, btnW, btnH);

    // Loaded-file display: right-justified near the window's right edge (clear of the
    // corner resize tab) and bottom-aligned with the buttons. Its left edge starts one
    // slot past PANIC (the fifth button), keeping the previous gap before the display.
    // Undo / History / Redo: three squares in the slot that used to start the file
    // display. They cost it 80 px, which it can afford — it left-ellipsizes and carries
    // a full-path tooltip, and its width is already clamped at 0 for the narrowest
    // windows. HISTORY sits BETWEEN the arrows, as in Pigments: it is about the same
    // stack they walk, and the arrows stay a symmetric pair either side of it.
    const int sq    = btnH;
    const int undoX = 8 + 4 * (btnW + gap);
    undoButton   .setBounds(undoX,                   stripY + 5, sq, sq);
    historyButton.setBounds(undoX +     (sq + 4),    stripY + 5, sq, sq);
    redoButton   .setBounds(undoX + 2 * (sq + 4),    stripY + 5, sq, sq);

    // The file display / bottom-bar tooltip readout is LEFT-justified from here, so
    // the eye always starts reading in the same spot whichever of the two is showing.
    // The gap after the trio is two undo buttons wide — enough that the text reads as
    // its own thing rather than a label on the redo button.
    const int rightMargin = 18;                            // clearance before the resize corner
    const int fileLeft    = undoX + 3 * sq + 2 * 4 + 2 * sq;
    // The mute/solo indicator takes the far-right end of the strip, and only while it
    // is showing — which is why refreshMuteSoloIndicator re-runs resized() when its
    // presence changes rather than every time a bit moves.
    // Two badges may stand at once. The override badge takes the OUTERMOST slot and
    // mute/solo shuffles left of it, so the one that is always about audibility keeps
    // its place relative to the file display and only the newcomer moves the edge.
    const int ovW  = ovIndicatorShown ? kOvIndicatorW : 0;
    const int ovX  = getWidth() - rightMargin - ovW;
    if (ovIndicatorShown)
        overrideIndicator.setBounds(ovX, stripY + 5, ovW, btnH);
    const int msW  = msIndicatorShown ? kMsIndicatorW : 0;
    const int msX  = ovX - (ovIndicatorShown ? gap : 0) - msW;
    if (msIndicatorShown)
        muteSoloIndicator.setBounds(msX, stripY + 5, msW, btnH);
    // The MIDI OUT badge goes innermost: it is a standing mode, not something
    // counted, so the two badges that come and go keep the slots they always had.
    const int moW  = moIndicatorShown ? kMoIndicatorW : 0;
    const int moX  = (msIndicatorShown ? msX : ovX) - ((msIndicatorShown || ovIndicatorShown) ? gap : 0) - moW;
    if (moIndicatorShown)
        midiOutIndicator.setBounds(moX, stripY + 5, moW, btnH);
    const int fileW       = juce::jmax(0, getWidth() - fileLeft - rightMargin
                                            - (msIndicatorShown ? msW + gap : 0)
                                            - (ovIndicatorShown ? ovW + gap : 0)
                                            - (moIndicatorShown ? moW + gap : 0));
    filePathDisplay.setBounds(fileLeft, stripY + 5, fileW, btnH);

    if (editorView != nullptr)
        editorView->setBounds(0, 0, getWidth(), juce::jmax(0, getHeight() - 34));

    // The History window follows its button: it covers the whole editor (it has to
    // catch the click-outside that dismisses it) and positions its panel from the
    // anchor it is given.
    if (historyPanel != nullptr && historyPanel->isVisible())
    {
        historyPanel->setBounds(getLocalBounds());
        historyPanel->openAt(historyButton.getBounds());
    }

    // The loading overlay covers the whole editor (both views + the button strip).
    if (loadOverlay != nullptr)
        loadOverlay->setBounds(getLocalBounds());

    // Remember each view's size so toggling restores its last dimensions. The
    // persisted Editor size is captured separately, debounced, in timerCallback()
    // — resized() also fires for construction/teardown transients (the component
    // briefly clamps to its 360 minimum height), which must NOT be persisted.
    //
    // NOT during a view swap: mid-swap the component still holds the OUTGOING view's
    // bounds while `showingEditor` already names the incoming one, so recording here
    // files one view's size under the other's name. setView() records the settled size
    // itself once the swap is complete. See rememberViewSize().
    if (! swappingView)
        rememberViewSize();
}

// File the component's current size under whichever view is showing. Split out of
// resized() so the view swap can suppress it for the duration and then call it once at
// the end — see the `swappingView` guard above and its use in setView().
void YamahaMU100AudioProcessorEditor::rememberViewSize()
{
    if (! viewsReady)
        return;

    if (showingEditor) { edW = getWidth(); edH = getHeight(); }
    else               { hwW = getWidth(); hwH = getHeight(); }
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::setView(bool showEditor)
{
    // `viewInitDone` (not `viewsReady`, which is already true by then) keeps the ctor's
    // own "open straight into the Editor view" call from reading as a navigation: that
    // is the window opening, not the user leaving the photo.
    const bool wasHardware = viewInitDone && ! showingEditor;

    // Hold off the per-view size bookkeeping until the swap has settled. Between here
    // and the setSize() below, the component still has the OUTGOING view's bounds while
    // showingEditor already names the incoming one — and setResizeLimits() applies the
    // incoming view's constraint to those bounds RIGHT THEN (JUCE's implementation ends
    // in setBoundsConstrained(getBounds())), so it fires resized() at exactly the wrong
    // moment. That is what used to overwrite each view's remembered size with the
    // other's: leaving a 1329-wide Editor filed 1329 as the Hardware width, and coming
    // back filed the Hardware view's height as the Editor's, which the 360 minimum then
    // clamped. Invisible whenever both views happened to be the same width, which is
    // why it survived so long.
    swappingView = true;

    showingEditor = showEditor;

    // The tick's repaint is gated on this class's own drawing having changed (see
    // timerCallback). A swap changes all of it at once and none of the comparators
    // can see that, so say so here.
    hwRepaintPending = true;

    // Undo / Redo / History belong to the EDITOR's parameter edits — nothing done on
    // the front panel is captured by them (the emulated device transmits nothing back,
    // so the stack has no record of it and the arrows would revert editor edits made
    // before the user ever switched views). Offering them over the photo advertised an
    // undo for what the panel was doing. The strip's layout does NOT reflow: the trio's
    // slot is left empty rather than sliding the file display left, so nothing on the
    // bar moves when the view changes.
    undoButton   .setVisible(showEditor);
    historyButton.setVisible(showEditor);
    redoButton   .setVisible(showEditor);
    // The History window hangs over either view, so it has to be dismissed with them —
    // otherwise it floats over the photo with no button left to close it.
    if (! showEditor && historyPanel != nullptr)
        historyPanel->setVisible(false);

    // The palette is gated on which view is up (the Hardware photo is never themed),
    // so the theme has to be settled BEFORE the view is shown — otherwise the light
    // editor would appear for one frame in dark and then rebuild under the user.
    // No-ops when the live palette already matches.
    applyEditorTheme();

    if (editorView != nullptr)
    {
        editorView->setVisible(showEditor);
        // setVisible notifies only editorView, not its children, so the Graphic
        // Display's EQ analyser would otherwise keep running behind the photo.
        editorView->notifyViewShown(showEditor);
    }

    // The view IS the setting: Options -> View is the only way to change it, so the
    // plugin reopens in whatever was last chosen. `viewsReady` keeps the ctor's own
    // restore call from writing settings.xml back with the value it just read.
    if (viewsReady)
    {
        const juce::String want = showEditor ? "Editor" : "Hardware";
        if (audioProcessor.defaultEditorView != want)
        {
            audioProcessor.defaultEditorView = want;
            audioProcessor.saveSettings();
        }
    }

    if (auto* c = getConstrainer())
    {
        if (showEditor)
        {
            // Editor view: free resize.
            c->setFixedAspectRatio(0.0);
            setResizeLimits(560, 360, 3000, 2000);
            setSize(edW, edH);
        }
        else
        {
            // Hardware view: keep the photo's aspect ratio.
            if (bgImage.isValid())
            {
                c->setFixedAspectRatio(hwAspect);
                setResizeLimits(600, juce::roundToInt(600.f / hwAspect),
                                2400, juce::roundToInt(2400.f / hwAspect));
            }
            // Guarded because the swap no longer records a size on its way through: if
            // the Hardware view has never been laid out, keep the current size rather
            // than collapsing the window to nothing.
            if (hwW > 0 && hwH > 0)
                setSize(hwW, hwH);
        }
    }

    resized();
    repaint();

    // The swap is settled: whatever size the view ended up at (the constrainer may have
    // adjusted the one asked for) is that view's size from here on. Record it once, then
    // hand the bookkeeping back to resized().
    swappingView = false;
    rememberViewSize();

    // Coming back from the Hardware view after the front panel was touched, the editor's
    // model may no longer describe the device — nothing transmits a panel edit back, so
    // the panels keep showing the patch as it was, and a .mss / project save stores THAT
    // rather than what the panel just changed. Say so once per arming, not on every
    // switch: a dialog that appears when nothing happened is one the user learns to
    // dismiss unread. The read-back's own caveats are in the text because both bite
    // afterwards — it drops the undo history, and it only reads the blocks the editor
    // knows, so a front-panel edit outside them is still not captured.
    if (wasHardware && showEditor && audioProcessor.hardwarePanelEdited.exchange(false))
    {
        // The em dash is built from its code point, never a source literal — same rule
        // as GraphicDisplayPanel::pm() / dash(): a bare non-ASCII byte in the source
        // would hinge on the compiler's source charset.
        const juce::String emDash = juce::String::charToString((juce::juce_wchar) 0x2014);

        // Leads with the consequence, not the remedy: the saved-file half is the part
        // that costs something, and it is the half a user has no way to notice. A
        // message box has no emphasis, so the second clause names what gets saved
        // ("what the panels show") rather than relying on a stressed word.
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon,
            "Hardware view edits",
            "Edits made in Hardware view do not reach the Editor. Until you use "
            "File > Read from Device, the panels keep showing the old values "
            + emDash + " and a saved setup or DAW project stores what the panels "
            "show, not the edits you just made.\n\n"
            "Read from Device clears the Undo/Redo buffer, and reads back only the "
            "parameters the Editor covers.",
            "Close", this);
    }

    // Which view is showing. Said only for a real navigation, never for the ctor's own
    // opening call -- viewInitDone is the same flag the stale-model warning uses.
    if (viewInitDone)
        say(mu100a11y::announce::Context,
            showEditor ? "Editor view" : "Hardware view, front panel");
}

// Push the "Disable plugin-only parameters" flag into the editor view. Safe to call
// with no editor view built yet — EditorView's ctor reads the flag itself, so a view
// created later still opens in the right state.
void YamahaMU100AudioProcessorEditor::applyPluginOnlyLock()
{
    pluginOnlyApplied = audioProcessor.enablePluginOnlyParams;
    if (editorView != nullptr)
        editorView->refreshPluginOnlyLock();
}

// Same, for "Show part MIDI receive switches" (the Part module's bottom LED row).
void YamahaMU100AudioProcessorEditor::applyRcvRowVisible()
{
    rcvSwitchesApplied = audioProcessor.showPartRcvSwitches;
    if (editorView != nullptr)
        editorView->refreshRcvRowVisible();
}

// Same again, for "Show Mute and Solo columns" (the Voice list's M and S cells).
void YamahaMU100AudioProcessorEditor::applyMuteSoloCols()
{
    muteSoloColsApplied = audioProcessor.showMuteSoloColumns;
    if (editorView != nullptr)
        editorView->refreshMuteSoloCols();
}

// ...and for "MIDI input indicators" (the Voice list's per-part activity tabs).
void YamahaMU100AudioProcessorEditor::applyMidiIndicators()
{
    midiIndicatorsApplied = audioProcessor.midiInputIndicators;
    if (editorView != nullptr)
        editorView->refreshMidiIndicators();
}

// The bottom strip's standing mute/solo indicator: label, fill colour and whether it
// is there at all. See the member's declaration for why it exists.
//
// It names the PART when exactly one is engaged and counts them otherwise, because
// "SOLO 05" answers the question the user is actually asking ("which one am I
// hearing?") and a count is the only honest answer once there is more than one.
void YamahaMU100AudioProcessorEditor::refreshMuteSoloIndicator()
{
    const uint32_t solo = audioProcessor.getPartSoloMask();
    const uint32_t mute = audioProcessor.getPartMuteMask();

    auto describe = [](const char* what, uint32_t mask) -> juce::String
    {
        int n = 0, first = 0;
        for (int i = 0; i < 32; ++i)
            if ((mask >> i) & 1u)
            {
                if (n == 0) first = i;
                ++n;
            }
        if (n == 0) return {};
        if (n == 1) return juce::String(what) + " "
                         + juce::String(first + 1).paddedLeft('0', 2);
        return juce::String(what) + " x" + juce::String(n);
    };

    juce::StringArray parts;
    if (solo) parts.add(describe("SOLO", solo));
    if (mute) parts.add(describe("MUTE", mute));
    // EQ bypass rides the same badge: it is the same KIND of state -- live, invisible
    // from the hardware view, and the sort you can leave engaged and then spend twenty
    // minutes wondering about. Clearing them all is one click here, as it is for the
    // other two.
    const uint32_t byp = audioProcessor.getPartEqBypassMask();
    if (byp) parts.add(describe("EQ BYP", byp));
    const bool show = ! parts.isEmpty();

    if (show)
    {
        muteSoloIndicator.setButtonText(parts.joinIntoString("  "));
        // Solo takes the fill when both are engaged: it is the stronger statement
        // (everything not soloed is silent, muted or not), so it is the one that has
        // to be recognised from across the room.
        // Solo takes the fill over mute, and both take it over an EQ bypass: the
        // order is how loud the statement is. A bypass changes a part's TONE; the
        // other two change what you can hear at all.
        muteSoloIndicator.setColour(juce::TextButton::buttonColourId,
                                    solo ? edcol::kSoloOnBg
                                         : (mute ? edcol::kMuteOnBg : edcol::kAlert));
        muteSoloIndicator.setColour(juce::TextButton::textColourOffId, edcol::kMsOnText);
        muteSoloIndicator.setColour(juce::TextButton::textColourOnId,  edcol::kMsOnText);
        muteSoloIndicator.setTooltip("Parts are muted, soloed or EQ-bypassed - click to clear all");
    }

    muteSoloIndicator.setVisible(show);
    if (show != msIndicatorShown)
    {
        // Only a change of PRESENCE moves anything: the file display gives up the
        // indicator's width while it is there and takes it back when it goes.
        msIndicatorShown = show;
        resized();
    }
    muteSoloIndicator.repaint();
}

// Same shape as the badge above. The count is of PARAMETERS, not bytes or messages:
// a two-byte effect parameter moved once is one thing to know about, and a sweep
// that sends a controller a hundred times is still one.
void YamahaMU100AudioProcessorEditor::refreshOverrideIndicator()
{
    const int n = audioProcessor.midiOverrideTotal();
    // Hidden when the marks are switched off, not just when the count is zero: a
    // badge pointing at marks nobody can see explains nothing.
    const bool show = n > 0 && audioProcessor.showMidiOverrides;

    if (show)
    {
        overrideIndicator.setButtonText(juce::String(n) + (n == 1 ? " MIDI OVERRIDE"
                                                                 : " MIDI OVERRIDES"));
        overrideIndicator.setColour(juce::TextButton::buttonColourId, edcol::kMidiOverride);
        overrideIndicator.setColour(juce::TextButton::textColourOffId, edcol::kOnChanged);
        overrideIndicator.setColour(juce::TextButton::textColourOnId,  edcol::kOnChanged);
        // ...and WHERE they are, on its own line. The marks answer this for whatever is
        // on screen; this is the only place that can answer it for what is NOT -- above
        // all a drum buffer no part on screen is using, which has no knob and no key
        // strip of its own and was visible only as this count going up.
        //
        // IT GOES FIRST, and that is not a style choice. The DEFAULT tooltip mode is
        // the BOTTOM BAR, one line that keeps the HEAD and drops the rest -- measured:
        // with this line third, the strip showed the opening sentence and the "where"
        // never appeared at all in the mode most users are in. The standing sentences
        // below are the ones a reader can afford to lose, having read them once.
        const juce::String where = audioProcessor.midiOverrideWhere();
        overrideIndicator.setTooltip(
            (where.isEmpty() ? juce::String() : where + "\n\n")
            + "Incoming MIDI has moved " + juce::String(n)
            + (n == 1 ? " setting" : " settings")
            + " on the device away from the values you set. Your setup is unchanged "
              "and is what will be saved.\n\n"
              "Click to put the device back on your values. To keep what arrived "
              "instead, use Options > Keep MIDI overrides.");
    }

    overrideIndicator.setVisible(show);
    if (show != ovIndicatorShown)
    {
        ovIndicatorShown = show;
        resized();
    }
    overrideIndicator.repaint();
}

// Same, for "Voice names". Unlike the options above, the flag has a live global
// behind it (voicenames::gShort) that the PROCESSOR also reads, so this pushes there
// first and only then asks the two name-showing surfaces to relabel.
void YamahaMU100AudioProcessorEditor::applyVoiceNames()
{
    voiceNamesApplied = audioProcessor.shortVoiceNames;
    voicenames::setShort(audioProcessor.shortVoiceNames);
    if (editorView != nullptr)
        editorView->refreshVoiceNames();
}

// Same, for "Show changed markers". Like the voice names above it, this has a live
// global behind it (changedmarks::gShow) that a panel reads at paint time, so the
// global moves first and the view is then asked to repaint. Pushed even with no editor
// view built: the global is what a view constructed later will read.
void YamahaMU100AudioProcessorEditor::applyChangedMarks()
{
    changedMarksApplied = audioProcessor.showChangedMarks;
    changedmarks::setShow(audioProcessor.showChangedMarks);
    if (editorView != nullptr)
        editorView->refreshChangedMarks();
}

// Same, for "Mark DAW-automatable params" (the VST Options dropdown). EditorView
// decides what the panels actually draw — including forcing it off in standalone.
void YamahaMU100AudioProcessorEditor::applyAutomationMarkers()
{
    automationMarkerApplied = markerSettingsSig();
    dawAutomationApplied    = audioProcessor.dawAutomationEnabled;
    if (editorView != nullptr)
        editorView->refreshAutomationMarkers();
    // The signature includes CC mode, so the badge rides the same mirror: a second
    // instance switching modes is picked up here too.
    refreshMidiOutBadge();
}

void YamahaMU100AudioProcessorEditor::refreshMidiOutBadge()
{
    const bool show = audioProcessor.midiOutFormat == YamahaMU100AudioProcessor::MidiOutCC;
    if (show)
    {
        midiOutIndicator.setButtonText("MIDI OUT: CC");
        // Orange: the editor's MIDI colour (the per-part input tabs, the SYS/FX/BULK
        // lamps). Ink is kOnChanged, as on the override badge's cyan fill.
        midiOutIndicator.setColour(juce::TextButton::buttonColourId, edcol::kOrange);
        midiOutIndicator.setColour(juce::TextButton::textColourOffId, edcol::kOnChanged);
        midiOutIndicator.setColour(juce::TextButton::textColourOnId,  edcol::kOnChanged);
        midiOutIndicator.setTooltip("MIDI Out is sending edits as MIDI CC: only the controls "
                                    "that have a controller record, everything else sends "
                                    "nothing - click to switch back to XG SysEx");
    }
    midiOutIndicator.setVisible(show);
    if (show != moIndicatorShown)
    {
        moIndicatorShown = show;
        resized();
    }
    midiOutIndicator.repaint();
}

// Same, for the MIDI-override mark style (Display Animation -> "Mark them with").
// The global has to move even with no editor view built, because it is what a panel
// constructed later will read; EditorView's own ctor calls the same refresh.
void YamahaMU100AudioProcessorEditor::applyMidiMarkStyle()
{
    midiMarkStyleApplied = audioProcessor.midiOverrideMarkStyle;
    midimarks::setStyle(midiMarkStyleApplied);
    if (editorView != nullptr)
        editorView->refreshMidiMarkStyle();
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::timerCallback()
{
    // Pick up a tooltip-mode change made from the VST options window (which only
    // writes the processor flag; the live TooltipWindow lives here).
    if (tooltipModeApplied != audioProcessor.tooltipMode)
        applyTooltipEnabled();

    // ...and the arrow-key mark, for a second instance sharing this settings.xml.
    if (focusMarkApplied != audioProcessor.focusMarkMode)
        applyFocusMarkMode();

    // ...and the Part module title naming, for the same reason.
    if (partTitleNamesApplied != audioProcessor.partTitleNames)
        applyPartTitleNames();

    // ...and the note-label form, for the same reason.
    if (noteLabelsApplied != audioProcessor.noteLabelMode)
        applyNoteLabels();

    // Bottom-bar tooltips do their own hover detection (no-op in the other modes).
    serviceBottomBarTooltip();

    // Keep the keyboard inside this editor whenever our window holds it, so a click on
    // the host's title bar or a dialog closing does not leave the shortcuts dead.
    serviceKeyboardFocus();

    // Accessibility pillar 4: say the focused control's value when something else moves
    // it -- a DAW automation lane, incoming SysEx, a file load, Undo. Polled here rather
    // than hooked into the ~50 places that can write a value, which is also what
    // coalesces a swept lane down to the poll rate. One bool when nobody is listening.
    valueWatcher.poll(*announcer);

    // Same for the EQ animated spectrum: either options window just writes the flag.
    // Turning it OFF the analyser would notice by itself (it re-tests every frame),
    // but turning it back ON it cannot — its timer isn't running to look. Nudging on
    // both edges keeps that asymmetry out of the option's behaviour.
    // The plugin WINDOW appearing is the other edge, and nothing reports it either:
    // isShowing() is false throughout this editor's constructor (the component has no
    // peer yet), so the setView() call in there tells the Graphic Display "you are
    // hidden" and never revisits it. Anything keyed on isShowing() therefore stayed
    // off until some unrelated event re-decided — the VOLUME meters never started at
    // all (their screen is the DEFAULT mode, so no setMode ever fires), and the EQ
    // analyser was dead on any launch that opened straight onto the EQ screen.
    // Watching the flag here costs one call per tick and covers every path that can
    // reveal the window, including ones nobody enumerated.
    const bool editorShowing = (editorView != nullptr) && editorView->isShowing();
    if (editorShowing != editorViewShowing)
    {
        editorViewShowing = editorShowing;
        if (editorView != nullptr)
            editorView->notifyViewShown(editorShowing);
    }

    if (eqSpectrumApplied != audioProcessor.eqSpectrumEnabled
        || volumeMetersApplied != audioProcessor.volumeMetersEnabled
        || volumeMeterRampApplied != audioProcessor.volumeMeterRampIdx)
    {
        eqSpectrumApplied     = audioProcessor.eqSpectrumEnabled;
        volumeMetersApplied   = audioProcessor.volumeMetersEnabled;
        // The ramp is a repaint, not a restart — but it rides the same mirror so
        // that a change made anywhere (including a settings.xml edited between
        // runs) reaches the palette, not just the one made through the dropdown.
        volumeMeterRampApplied = audioProcessor.volumeMeterRampIdx;
        edcol::setMeterRamp(volumeMeterRampApplied);
        if (editorView != nullptr)
            editorView->refreshSpectrumOptions();   // covers both animations
    }

    // And for "Disable plugin-only parameters" — the VST options window only writes
    // the flag; the panels it greys out live under editorView.
    if (pluginOnlyApplied != audioProcessor.enablePluginOnlyParams)
        applyPluginOnlyLock();

    // ...and for "Show part MIDI receive switches", same reason: the VST options
    // window only writes the flag, and the row it hides lives under editorView.
    if (rcvSwitchesApplied != audioProcessor.showPartRcvSwitches)
        applyRcvRowVisible();

    // ...and for "Show Mute and Solo columns": a SECOND plugin instance sharing this
    // settings.xml is what this catches, since the menu handler pushes it directly.
    if (muteSoloColsApplied != audioProcessor.showMuteSoloColumns)
        applyMuteSoloCols();

    // ...and for "MIDI input indicators". This one is REQUIRED, not a convenience:
    // the Display Animation window writes only the flag, and PartListPanel's fade
    // timer is stopped while the feature is off, so nothing over there is running to
    // see it come back on.
    if (midiIndicatorsApplied != audioProcessor.midiInputIndicators)
        applyMidiIndicators();

    // ...and for "Mark DAW-automatable params" — the VST options window writes only
    // the flag, and the knobs it marks live under editorView. The automation SWITCH
    // rides the same mirror because the markers follow it: the menu pushes it directly,
    // but a host can also deliver project state while this editor is open, which no
    // menu handler sees.
    if (automationMarkerApplied != markerSettingsSig()
        || dawAutomationApplied != audioProcessor.dawAutomationEnabled)
        applyAutomationMarkers();

    // ...and for the MIDI-override mark style. The Display Animation window writes
    // only the flag, and the Part module it re-lays-out lives under editorView.
    if (midiMarkStyleApplied != audioProcessor.midiOverrideMarkStyle)
        applyMidiMarkStyle();

    // ...and for "Voice names". A second plugin instance sharing this settings.xml
    // is the case this catches; the menu handler pushes it directly.
    if (voiceNamesApplied != audioProcessor.shortVoiceNames)
        applyVoiceNames();

    // ...and for "Show changed markers", same reason.
    if (changedMarksApplied != audioProcessor.showChangedMarks)
        applyChangedMarks();

    // Pull LCD pixels from processor. The flag is also the only notice we get that
    // the hardware view's dot matrix changed, so it doubles as a repaint trigger --
    // see the gating at the end of this function.
    if (audioProcessor.lcdUpdated.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> lock(audioProcessor.lcdMutex);
        memcpy(lcdSnapshot, audioProcessor.lcdPixels, YamahaMU100AudioProcessor::LCD_BUF_SIZE);
        audioProcessor.lcdUpdated.store(false, std::memory_order_release);
        hwRepaintPending = true;
    }

    checkForCompletedDump();

    // Perform a load that was deferred because the device wasn't ready yet (loaded
    // right after a reset, before the boot XG-reset). Do it now that it's past boot.
    if (pendingLoadFile != juce::File() && audioProcessor.isReadyForEditing())
    {
        const juce::File f = pendingLoadFile;
        const bool isSetup = pendingLoadIsSetup;
        pendingLoadFile = juce::File();
        pendingLoadIsSetup = false;
        isSetup ? doSetupLoad(f) : doSysExLoad(f);
    }

    // "Loading file" overlay progress (time-driven, same model as EditorView's
    // read-back overlay). When the streaming + settle window elapses, hide it and —
    // on the Editor view only — kick off a device read-back so the panels reflect
    // the just-loaded patch.
    if (loadingActive)
    {
        const juce::uint32 nowMs = juce::Time::getMillisecondCounter();
        if (loadOverlay && loadExpectedMs > 0)
            loadOverlay->setProgress(juce::jlimit(0.0, 0.99,
                (double)(nowMs - loadStartMs) / (double) loadExpectedMs));
        if ((int)(nowMs - loadStartMs) >= loadExpectedMs)
            finishSysExLoad();
    }

    // Editor-view window-size restore/capture. Two phases, both gated on the
    // Editor view being the one on screen (the Hardware view's size is derived
    // from the photo aspect, never persisted).
    if (viewsReady && showingEditor)
    {
        const int w = getWidth(), h = getHeight();
        const int tgtW = juce::jlimit(560, 3000, audioProcessor.editorViewW);
        const int tgtH = juce::jlimit(360, 2000, audioProcessor.editorViewH);

        if (! restoreDone)
        {
            // RESTORE: re-apply the persisted size every tick until it sticks.
            // A one-shot restore loses to hosts/standalone that resize the editor
            // to their own size shortly after attach; reading the persisted target
            // (not edW/edH, which the host's resize clobbers via resized()) and
            // retrying makes us win that race. Stop once the size has held at the
            // target for a few ticks, or give up if the host keeps refusing it.
            if (w == tgtW && h == tgtH)
            {
                if (++restoreOkTicks >= 3) { restoreDone = true; edW = w; edH = h; }
            }
            else
            {
                restoreOkTicks = 0;
                setSize(tgtW, tgtH);
                if (++restoreAttempts >= 60) { restoreDone = true; }  // ~2 s cap
            }
        }
        else
        {
            // CAPTURE: persist a genuine user resize, debounced. Recording only a
            // size that has held steady for a few ticks excludes the construction/
            // teardown transients and momentary host resizes that would otherwise
            // persist a bogus size (e.g. the 360 min height).
            if (w >= 560 && h >= 360)
            {
                if (w == capW && h == capH)
                {
                    if (capTicks < 3) ++capTicks;
                    // Commit + flush exactly once per steady size (capTicks hits 3),
                    // and only if it actually changed. Flushing to settings.xml here
                    // (not just in the dtor) means a SECOND instance opened later in
                    // the same session adopts this size without the first instance's
                    // editor having to be closed first.
                    if (capTicks == 3
                        && (w != audioProcessor.editorViewW || h != audioProcessor.editorViewH))
                    {
                        audioProcessor.editorViewW = w;
                        audioProcessor.editorViewH = h;
                        edW = w; edH = h;
                        audioProcessor.saveSettings();
                    }
                }
                else { capW = w; capH = h; capTicks = 0; }
            }
        }
    }

    // Phase 5: once the Editor view is up and the device is READY FOR EDITING,
    // populate the panels with the live patch. The FIRST time (per plugin instance)
    // this needs a full device read-back. On later GUI reopens the processor still
    // holds the captured model, so we just re-apply it — re-running the read-back
    // every reopen floods MIDI IN A with dump requests/replies and starves note
    // handling for seconds. Use File→"Read from Device" for an explicit re-sync.
    //
    // Gate on isReadyForEditing() (past the t=3.0s boot XG-reset), NOT the bare
    // mameIsFullyBooted (which flips on the first frame). On a fresh instance —
    // standalone relaunch or a host reloading a saved project — the device boots
    // from restored work RAM but isn't reliably answering MIDI until boot settles;
    // firing the read-back during that window drops the early-part requests and
    // captures only the later parts (the file's last few edited parts read back,
    // the rest show defaults). The file-load path already defers to this same gate.
    if (! autoReadBackDone && showingEditor && editorView
        && audioProcessor.isReadyForEditing())
    {
        autoReadBackDone = true;
        if (! audioProcessor.editorStatePopulated)
        {
            // Fresh instance / after reset: the device is at factory defaults, so seed
            // the editor model from the embedded defaults (complete + instant) rather
            // than a slow, lossy device read-back — whose late replies could also race
            // a subsequent file load and re-stamp a few parts. A restored project sets
            // editorStatePopulated in setStateInformation, so it skips this and shows
            // the restored patch. "Read from Device" remains for an explicit re-sync.
            audioProcessor.seedEditorDefaults();
            audioProcessor.editorStatePopulated = true;
        }
        editorView->applyDeviceState();   // reflect the seeded / restored model
        // Adopt the current sequence rather than replaying anything the processor's
        // drain folded in before the panels existed — applyDeviceState() has just shown
        // the whole model, external edits included.
        lastExternalStateSeq = audioProcessor.editorStateExternalSeq.load(std::memory_order_acquire);
    }

    // Received SysEx (a DAW track, an external controller) is folded into the model by
    // the processor's own drain timer; re-apply the panels whenever it changed something,
    // or the editor keeps showing its own last selection while the device has moved on.
    // Skipped mid-read-back: consumeDump() owns the model until its bursts finish and
    // calls applyDeviceState() itself at every step.
    if (editorView != nullptr && autoReadBackDone && ! editorView->isAwaitingDump())
    {
        const uint32_t seq = audioProcessor.editorStateExternalSeq.load(std::memory_order_acquire);
        if (seq != lastExternalStateSeq)
        {
            lastExternalStateSeq = seq;
            editorView->applyDeviceState();
        }
    }

    // The MIDI-override marks ride their own sequence, not editorStateExternalSeq:
    // an incoming controller changes the SHADOW and leaves the model alone, so the
    // watch above would never fire for it. Unlike that one this is not gated on the
    // read-back, because it writes no values -- it only moves marks.
    {
        const uint32_t oseq = audioProcessor.midiOverrideSequence();
        if (oseq != lastMidiOverrideSeq)
        {
            lastMidiOverrideSeq = oseq;
            // The marks live in the editor view; the badge and the announcement do
            // not -- the bottom bar is shared with the Hardware view, where the count
            // is the ONLY form of this news there is.
            if (editorView != nullptr) editorView->refreshMidiOverrides();
            refreshOverrideIndicator();
            ovSettleTicks = 0;   // restart the settle window: more is probably coming
        }

        // ...and speak it once it has stopped moving. A second of quiet, not a fixed
        // interval: a sequencer's program changes at the top of a take arrive as one
        // burst and are one piece of news.
        if (ovSettleTicks >= 0 && ++ovSettleTicks >= 30)   // ~1 s at the 30 Hz timer
        {
            ovSettleTicks = -1;
            // Gated on the marks being shown, like the badge: with them off the user
            // has asked not to be told, and with the follow lock off the count is
            // frozen at zero anyway.
            const int n = audioProcessor.showMidiOverrides
                        ? audioProcessor.midiOverrideTotal() : 0;
            if (n != ovAnnouncedCount)
            {
                const bool cleared = (n == 0);
                ovAnnouncedCount = n;
                say(mu100a11y::announce::Edit,
                    cleared ? juce::String("No MIDI overrides")
                            : juce::String(n) + (n == 1 ? " MIDI override"
                                                        : " MIDI overrides"));
            }
        }
    }

    // Refresh the loaded-file strip (path + unsaved-changes '*'). Throttled: the dirty
    // check is a bulk-dump + MD5, so recomputing it every tick is wasteful and a save
    // marker needn't be instant. setState() no-ops when nothing changed.
    if (++dirtyPollTick >= 8)   // ~4 Hz at the 30 Hz timer
    {
        dirtyPollTick = 0;
        filePathDisplay.setState(audioProcessor.loadedFilePath,
                                 audioProcessor.isEditorStateDirty());
        // The Voice module's yellow row roll-ups ride the same divider, and for the
        // same reason: both answer "has anything changed" against the model, and
        // neither has an event to hang on.
        if (editorView != nullptr) editorView->refreshRowChanged();
    }

    // Undo/redo availability + tooltips, on the same divider. The label is formatted
    // here rather than when the edit happens, which is the point: a drag merges into
    // its step on every value change, and building a String 200 times a second to
    // service a tooltip nobody is reading would be the only real cost in the feature.
    if (++undoPollTick >= 8)    // ~4 Hz
    {
        undoPollTick = 0;
        refreshUndoButtons();
        // Keep an open History window current with edits made behind it (and with the
        // arrows). It rebuilds only when the stack has actually moved — the labels are
        // formatted lazily for exactly this reason.
        if (historyPanel != nullptr) historyPanel->refreshIfChanged();
    }

    // Repaint only what this class actually draws, and only when it moved.
    //
    // In the EDITOR view paint() returns immediately after the background fill, so a
    // repaint here buys nothing and costs a full invalidation of EditorView and every
    // panel beneath it. Those panels drive their own repaints and always have.
    //
    // In the HARDWARE view paint() draws the photo, the two status overlays, the LCD
    // dot matrix and the front-panel LEDs. The photo and the pressed-button highlight
    // are already covered elsewhere (resized(), and mouseDown/mouseUp repaint
    // directly), so what is left to poll is the LCD -- flagged above -- plus the LED
    // byte and the three status atomics, none of which reports a change on its own.
    if (! showingEditor)
    {
        const uint8_t  leds  = audioProcessor.ledState.load(std::memory_order_relaxed);
        const bool     rom   = audioProcessor.isRomMissing.load();
        const bool     boot  = audioProcessor.mameIsFullyBooted.load();
        const unsigned long fault =
            audioProcessor.mameFaultCode.load(std::memory_order_acquire);

        if (leds != hwLedsSeen || rom != hwRomMissingSeen
            || boot != hwBootedSeen || fault != hwFaultSeen)
        {
            hwLedsSeen       = leds;
            hwRomMissingSeen = rom;
            hwBootedSeen     = boot;
            hwFaultSeen      = fault;
            hwRepaintPending = true;
        }

        if (hwRepaintPending)
        {
            hwRepaintPending = false;
            repaint();
        }
    }
}

// The shared tail of Undo, Redo and a History click. The processor bumps
// editorStateExternalSeq, which the timer already watches — but that path is throttled
// to ~7.5 Hz and a click should repaint the panels immediately, so refresh directly and
// adopt the new sequence number so the timer doesn't then do it a second time.
// PANIC, from the button or the shortcut. An Alert because it INTERRUPTS: it is the
// key you press when something is wrong, and a confirmation that arrives after the
// queue has drained is no confirmation.
void YamahaMU100AudioProcessorEditor::panicClicked()
{
    audioProcessor.sendPanic();
    announcer->sayNow(mu100a11y::announce::Alert, "Panic, all notes off");
}

// See the header: adopt the count as announced, so the settle watcher in the timer
// does not say "no MIDI overrides" a second after this has said what happened.
void YamahaMU100AudioProcessorEditor::sayOverrideAction(const juce::String& text)
{
    ovAnnouncedCount = audioProcessor.midiOverrideTotal();
    ovSettleTicks    = -1;
    say(mu100a11y::announce::Edit, text);
}

void YamahaMU100AudioProcessorEditor::afterUndoApply()
{
    if (editorView != nullptr)
    {
        editorView->applyDeviceState();
        lastExternalStateSeq = audioProcessor.editorStateExternalSeq.load(std::memory_order_acquire);
    }
    refreshUndoButtons();
    if (historyPanel != nullptr) historyPanel->refreshIfChanged();

    // Undo is the one action whose whole point is that something you cannot see has
    // changed back. The step it lands ON is the one now in force, and undoStepLabel
    // already reads the way the control reads ("Part 3 CUTOFF -12 -> +4").
    const int pos = audioProcessor.undoPosition();
    say(mu100a11y::announce::Edit,
        pos > 0 ? "Undo to " + audioProcessor.undoStepLabel(pos - 1)
                : juce::String("Undo to initial state"));
}

// "3 muted, 1 soloed" -- or "no mutes or solos" when the last one clears, because
// silence would be indistinguishable from the announcement not arriving.
juce::String YamahaMU100AudioProcessorEditor::muteSoloSummary() const
{
    const auto mute = audioProcessor.getPartMuteMask();
    const auto solo = audioProcessor.getPartSoloMask();
    int nm = 0, ns = 0;
    for (int i = 0; i < 32; ++i)
    {
        if ((mute >> i) & 1) ++nm;
        if ((solo >> i) & 1) ++ns;
    }
    if (nm == 0 && ns == 0) return "No mutes or solos";

    juce::StringArray parts;
    if (nm > 0) parts.add(juce::String(nm) + (nm == 1 ? " part muted" : " parts muted"));
    if (ns > 0) parts.add(juce::String(ns) + (ns == 1 ? " part soloed" : " parts soloed"));
    return parts.joinIntoString(", ");
}

juce::String YamahaMU100AudioProcessorEditor::eqBypassSummary() const
{
    const auto byp = audioProcessor.getPartEqBypassMask();
    int n = 0, first = 0;
    for (int i = 0; i < 32; ++i)
        if ((byp >> i) & 1) { if (n == 0) first = i; ++n; }

    if (n == 0) return "EQ bypass off";
    if (n == 1) return "Part " + juce::String(first + 1) + " EQ bypassed";
    return juce::String(n) + " parts EQ bypassed";
}

void YamahaMU100AudioProcessorEditor::toggleHistoryPanel()
{
    if (historyPanel != nullptr && historyPanel->isVisible())
    {
        historyPanel->setVisible(false);        // the hamburger is a toggle
        restoreKeyboardFocus(false);
        return;
    }

    if (historyPanel == nullptr)
    {
        historyPanel = std::make_unique<HistoryPanel>();
        // The panel reads the stack through these rather than holding a copy — see the
        // header: a history that can disagree with the stack it describes is worse than
        // none. All message-thread, like the whole undo feature.
        historyPanel->getStepCount    = [this] { return audioProcessor.undoStepCount(); };
        historyPanel->getPosition     = [this] { return audioProcessor.undoPosition(); };
        historyPanel->hasInitialState = [this] { return audioProcessor.undoHasInitialState(); };
        historyPanel->getStepLabel    = [this](int i) { return audioProcessor.undoStepLabel(i); };
        historyPanel->onGoTo          = [this](int entry)
        {
            audioProcessor.undoGoTo(entry);     // entry i = the state after step i
            afterUndoApply();
        };
        historyPanel->onDismiss = [this]
        {
            historyPanel->setVisible(false);
            restoreKeyboardFocus(false);   // it held the keyboard for its arrow keys
        };
        addChildComponent(*historyPanel);
    }

    historyPanel->setBounds(getLocalBounds());
    historyPanel->openAt(historyButton.getBounds());
}

void YamahaMU100AudioProcessorEditor::refreshUndoButtons()
{
    const bool canU = audioProcessor.canUndo();
    const bool canR = audioProcessor.canRedo();
    undoButton.setEnabled(canU);          // no-ops when unchanged
    redoButton.setEnabled(canR);
    undoButton.setDynamicTooltip(withShortcut(canU ? "Undo: " + audioProcessor.undoLabel()
                                                   : juce::String("Nothing to undo"),
                                              kbsc::EditUndo));
    redoButton.setDynamicTooltip(withShortcut(canR ? "Redo: " + audioProcessor.redoLabel()
                                                   : juce::String("Nothing to redo"),
                                              kbsc::EditRedo));
    // Always available: with no edits yet it opens on "Initial State" alone, which
    // answers "where did my history go" better than a disabled button does.
    historyButton.setDynamicTooltip(withShortcut("Edit history", kbsc::EditHistory));
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::checkForCompletedDump()
{
    if (editorView)
        editorView->pollReadBackTimeout();   // give up on an unanswered read-back

    if (dumpDialogOpen)
        return;  // a Save As dialog is already up; wait for the user

    // While a read-back is in flight, drain replies eagerly (incrementally) so a long
    // multi-second stream is consumed as it arrives and keeps the read-back alive,
    // rather than landing as one final burst it might have already given up on.
    const bool eager = editorView && editorView->isAwaitingDump();
    std::vector<uint8_t> bytes;
    if (!audioProcessor.pollCompletedDump(bytes, eager))
        return;  // no completed dump burst pending

    // Phase 5: a pending read-back consumes the dump into the editor's panels
    // instead of prompting a Save As. (A user-initiated front-panel dumpout with
    // no read-back pending still routes to the Save As dialog below.)
    if (editorView && editorView->isAwaitingDump())
    {
        editorView->consumeDump(bytes);
        return;
    }

    // Trailing reply residue just after a read-back ended (e.g. the device kept
    // transmitting past the read-back's give-up point) is NOT a user dumpout — drop
    // it instead of popping a spurious "Save dump" dialog.
    if (editorView && editorView->inReadBackGrace())
    {
        audioProcessor.clearDumpCapture();   // discard any further trailing bytes too
        return;
    }

    pendingDumpBytes = std::move(bytes);

    juce::File defaultDir = audioProcessor.lastDumpDir.isNotEmpty()
                              ? juce::File(audioProcessor.lastDumpDir)
                              : juce::File{};
    if (! defaultDir.isDirectory())
        defaultDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                         .getChildFile("MEGAN XG");

    const juce::String name = YamahaMU100AudioProcessor::makeDumpDefaultFileName(pendingDumpBytes);
    const juce::File suggested = defaultDir.getChildFile(name);

    dumpDialogOpen = true;
    dumpChooser = std::make_unique<juce::FileChooser>(
        "Save XG dump (.syx)", suggested, "*.syx");

    dumpChooser->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file != juce::File{})
        {
            file.replaceWithData(pendingDumpBytes.data(), pendingDumpBytes.size());
            audioProcessor.lastDumpDir = file.getParentDirectory().getFullPathName();
            audioProcessor.saveSettings();
        }
        // file == File{} → user cancelled: the dump is discarded.
        pendingDumpBytes.clear();
        dumpDialogOpen = false;
    });
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
    if (showingEditor) return;   // hardware-panel hit testing only applies to the photo view

    // "All Macro" button — Part- + Part+ simultaneously
    const auto macro = hwpanel::kAllButton
                           .translated(-(float) hwpanel::kArtX0, 0.f)
                           .transformedBy(juce::AffineTransform::scale(imgScale));
    if (macro.contains(e.position))
    {
        allMacroActive = true;
        audioProcessor.hardwarePanelEdited = true;
        audioProcessor.buttonPressedP7.fetch_or(kAllMacroMaskP7);
        repaint();
        return;
    }

    for (int i = 0; i < k_numButtons; ++i)
    {
        const auto& b = k_buttons[i];
        const float cx = (b.cx - hwpanel::kArtX0) * imgScale;
        const float cy = b.cy * imgScale;
        const float r  = b.radius * imgScale;
        if (e.position.getDistanceFrom(juce::Point<float>(cx, cy)) <= r)
        {
            activeButtonIdx = i;
            pressButton(i);
            repaint();
            break;
        }
    }
}

void YamahaMU100AudioProcessorEditor::mouseUp(const juce::MouseEvent&)
{
    if (showingEditor) return;

    if (allMacroActive)
    {
        audioProcessor.buttonPressedP7.fetch_and((uint8_t)~kAllMacroMaskP7);
        allMacroActive = false;
        repaint();
        return;
    }

    if (activeButtonIdx >= 0)
    {
        releaseButton(activeButtonIdx);
        activeButtonIdx = -1;
        repaint();
    }
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::pressButton(int idx)
{
    // Any front-panel press arms the stale-model warning shown on the way back to an
    // Editor view. Deliberately ANY button, not just the value-changing ones: from
    // here a press is only a port bit, and which of them can alter data depends on
    // the page the firmware happens to be on. A spurious warning costs one click; a
    // missed one leaves the panels quietly describing a patch the device no longer
    // holds, which is the pitfall this exists to remove.
    audioProcessor.hardwarePanelEdited = true;

    const auto& b = k_buttons[idx];
    if (b.port[2] == '7')
        audioProcessor.buttonPressedP7.fetch_or(b.mask);
    else
        audioProcessor.buttonPressedP8.fetch_or(b.mask);
}

void YamahaMU100AudioProcessorEditor::releaseButton(int idx)
{
    const auto& b = k_buttons[idx];
    if (b.port[2] == '7')
        audioProcessor.buttonPressedP7.fetch_and((uint8_t)~b.mask);
    else
        audioProcessor.buttonPressedP8.fetch_and((uint8_t)~b.mask);
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::showFileMenu()
{
    juce::PopupMenu menu;

    // Items that have a shortcut carry it on the right, Surge-style. `item()` is the
    // one place that asks for the chord, so a menu can never show a stale one -- and it
    // returns empty whenever shortcuts are switched off, which is what makes the whole
    // column disappear with the option rather than needing a second code path.
    auto item = [this](int id, const juce::String& text, int cmd, bool enabled = true)
    {
        juce::PopupMenu::Item i;
        i.itemID    = id;
        i.text      = text;
        i.isEnabled = enabled;
        i.shortcutKeyDescription = shortcutText(cmd);
        return i;
    };

    menu.addItem(item(1, "New",           kbsc::FileNew));
    menu.addItem(item(2, "Open File...",  kbsc::FileOpen));

    // Open Recent: the last kMaxRecentFiles files opened or saved, newest first. A
    // submenu for the same reason Save Setup As is one — the strip has room for one
    // button, not a list. Item ids 20.. index audioProcessor.recentFiles positionally,
    // so the list is read ONCE here and the callback re-reads it by index; a load that
    // reorders the list in between cannot happen, because the menu is modal.
    {
        juce::PopupMenu recent;
        const auto& files = audioProcessor.recentFiles;
        for (int i = 0; i < files.size(); ++i)
        {
            // Shown as "name  (folder)": two setups of the same name in different
            // folders are common (a working copy and a backup), and the bare filename
            // would make them indistinguishable. Only the folder's own NAME, not its
            // full path — JUCE sizes a popup to its widest item, and the full path made
            // the submenu wider than the plugin window for no extra information.
            const juce::File f(files[i]);
            recent.addItem(20 + i, f.getFileName() + "   ("
                                   + f.getParentDirectory().getFileName() + ")");
        }
        menu.addSubMenu("Open Recent...", recent, ! files.isEmpty());
    }

    menu.addSeparator();
    menu.addItem(item(3, "Save Setup (.mss)", kbsc::FileSave, editorView != nullptr));

    // Save Setup As is a submenu, one entry per native format. See the header note on
    // saveSetupAsClicked for why the format is chosen here and not in the file dialog.
    // Drum Setup is only offered when the current part actually has a DRUMS edit buffer
    // to save — the file would otherwise have no drum data in it at all.
    {
        const bool haveEditor = editorView != nullptr;
        const bool haveDrumBuf = haveEditor && editorView->getCurrentPartDrumBuffer() >= 0;
        juce::PopupMenu saveAs;
        saveAs.addItem(10, "System Setup (.mss)", haveEditor);
        saveAs.addItem(11, "Part Setup (.mps)",   haveEditor);
        saveAs.addItem(12, "Drum Setup (.mds)",   haveDrumBuf);
        saveAs.addItem(13, "Effect Setup (.mes)", haveEditor);
        // The submenu head carries the chord even though the chord itself picks System
        // Setup directly -- naming the group is what a reader needs; the four formats
        // then say which one it lands on.
        {
            juce::PopupMenu::Item sub;
            sub.itemID    = 0;
            sub.text      = "Save Setup As...";
            sub.subMenu   = std::make_unique<juce::PopupMenu>(saveAs);
            sub.isEnabled = haveEditor;
            sub.shortcutKeyDescription = shortcutText(kbsc::FileSaveAs);
            menu.addItem(std::move(sub));
        }
    }

    menu.addItem(item(5, "Export Multi 32-Part Syx...", kbsc::FileExportSyx,
                      editorView != nullptr));
    menu.addSeparator();
    menu.addItem(item(6, "Read from Device", kbsc::FileReadDevice, editorView != nullptr));
    menu.addSeparator();

    // Name the DAW's per-part output tracks after the voices assigned to those parts.
    // Deliberately an explicit action rather than automatic on voice change: the VST3
    // notification (kIoTitlesChanged) carries no bus index, so the host re-reads ALL
    // names at once and there is no way to rename just the part that changed — a push
    // can overwrite a track the user renamed by hand, including on parts they did not
    // touch. Opt-in makes that honest; automatic would fire it on every audition.
    // Disabled where it cannot do anything: the standalone has no host to notify and
    // cannot open the aux buses, and with the option off there are no part tracks.
    const bool canNameTracks = audioProcessor.getPartOutputsEnabled()
                            && audioProcessor.wrapperType != juce::AudioProcessor::wrapperType_Standalone;
    menu.addItem(7, "Name All Output Tracks from Voices", canNameTracks);

    // Exit, standalone only — a hosted plugin has no application to quit. Placed last,
    // where every other standalone app puts it.
    const bool isStandalone =
        audioProcessor.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
    if (isStandalone)
    {
        menu.addSeparator();
        menu.addItem(item(8, "Exit", kbsc::FileExit));
    }

    menu.setLookAndFeel(&lookAndFeel);   // see themeDialog(): a top-level popup does not inherit
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(fileButton),
                        [this](int result)
    {
        using SetupKind = YamahaMU100AudioProcessor::SetupKind;
        if (result >= 20 && result < 20 + YamahaMU100AudioProcessor::kMaxRecentFiles)
        {
            const int i = result - 20;
            if (i < audioProcessor.recentFiles.size())
                openRecentClicked(juce::File(audioProcessor.recentFiles[i]));
            return;
        }
        if      (result == 1) newSetupClicked();
        else if (result == 2) openFileClicked();
        else if (result == 3) saveSetupClicked();
        else if (result == 5) saveMultiSyxClicked();
        else if (result == 10) saveSetupAsClicked(SetupKind::System);
        else if (result == 11) saveSetupAsClicked(SetupKind::Part);
        else if (result == 12) saveSetupAsClicked(SetupKind::Drum);
        else if (result == 13) saveSetupAsClicked(SetupKind::Effect);
        else if (result == 6 && editorView)
            editorView->requestReadBack();   // pull live state into the editor panels
        else if (result == 7) audioProcessor.refreshBusNames();
        else if (result == 8) exitStandaloneClicked();
    });
}

//==============================================================================
// Run `proceed` now if the model is clean, else after a discard/cancel warning.
void YamahaMU100AudioProcessorEditor::confirmDiscardIfDirty(std::function<void()> proceed)
{
    if (! audioProcessor.isEditorStateDirty())
    {
        proceed();
        return;
    }

    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "Unsaved changes",
        "The current setup has unsaved changes that will be lost. Continue?",
        "Discard", "Cancel", this,
        juce::ModalCallbackFunction::create([proceed](int result)
        {
            if (result == 1)   // 1 = the first (OK / "Discard") button
                proceed();
        }));
}

//==============================================================================
// File > Exit (standalone only). Routed through the WINDOW's closeButtonPressed() rather
// than calling mu100::confirmStandaloneQuit here, so Exit and the title bar's X are the
// same code path by construction — including JUCE's own savePluginState() step ahead of
// the guard. Duplicating the guard call would work today and drift the first time either
// side gains a step.
//
// Quitting is safe to trigger from inside a menu callback: JUCEApplicationBase::quit()
// posts to the message loop rather than tearing this editor down under our feet.
void YamahaMU100AudioProcessorEditor::exitStandaloneClicked()
{
    if (auto* window = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent()))
        window->closeButtonPressed();
    else
        juce::JUCEApplicationBase::quit();   // no standalone window: quit unguarded
}

//==============================================================================
// Declared in StandaloneCloseGuard.h; called from the guarded patch in JUCE's
// juce_StandaloneFilterWindow.h when the standalone window's close button is pressed.
// The standalone no longer writes its setup anywhere, so closing with unsaved edits
// really does discard them — hence the same prompt File > New and File > Open use.
bool mu100::confirmStandaloneQuit (void (*quitNow)())
{
    auto* holder = juce::StandalonePluginHolder::getInstance();
    auto* proc   = (holder != nullptr)
                     ? dynamic_cast<YamahaMU100AudioProcessor*>(holder->processor.get())
                     : nullptr;

    if (proc == nullptr || ! proc->isEditorStateDirty())
        return false;                       // nothing to lose — let the caller quit now

    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "Unsaved changes",
        "The current setup has unsaved changes.\n\n"
        "The standalone always opens at the factory defaults, so these changes "
        "will be lost. Use File > Save Setup As... first if you want to keep them.",
        "Close anyway", "Cancel", nullptr,
        juce::ModalCallbackFunction::create([quitNow](int result)
        {
            if (result == 1)                // 1 = the first ("Close anyway") button
                quitNow();
        }));
    return true;                            // the confirmation owns the quit from here
}

//==============================================================================
// File->New: soft reset editor + device to defaults, untitled. Same in VST and
// standalone (no MAME reboot — seeding defaults + XG reset returns the running device
// to a clean state, and the plugin-only features are neutralised too).
void YamahaMU100AudioProcessorEditor::newSetupClicked()
{
    confirmDiscardIfDirty([this]
    {
        // Free MIDI IN A and suppress the auto read-back, like a file load.
        if (editorView != nullptr)
            editorView->cancelReadBack();
        autoReadBackDone = true;

        audioProcessor.seedEditorDefaults();     // editorState -> factory; clears filename
        audioProcessor.resetPluginOnlyState();   // HPF2/EQ/tempo-SYNC off (+ dirty flags)
        audioProcessor.editorStatePopulated = true;
        if (editorView != nullptr)
            editorView->applyDeviceState();      // panels reflect the defaults now

        const int sendMs = audioProcessor.sendPatchToDevice();  // XG reset -> device audible
        beginSysExLoad(sendMs, {});
    });
}

//==============================================================================
// File->Open File: pick a native .mss (complete setup) or a raw .syx. One chooser
// filters both; the actual format is sniffed from the bytes (0xF0 = raw SysEx, '<' =
// native XML) so a mislabelled extension still loads correctly.
void YamahaMU100AudioProcessorEditor::openFileClicked()
{
    confirmDiscardIfDirty([this]
    {
        juce::File startDir = audioProcessor.lastSetupDir.isNotEmpty()
                                ? juce::File(audioProcessor.lastSetupDir)
                                : audioProcessor.lastSyxDir.isNotEmpty()
                                    ? juce::File(audioProcessor.lastSyxDir)
                                    : juce::File::getSpecialLocation(juce::File::userHomeDirectory);

        // One chooser for every native setup kind plus raw SysEx. Which KIND a native
        // file is does not come from its extension — doSetupLoad reads the setupKind
        // attribute out of the XML — so a renamed file still loads as what it is.
        openChooser = std::make_unique<juce::FileChooser>(
            "Open Setup (.mss System / .mps Part / .mds Drum / .mes Effect) or SysEx (.syx)",
            startDir,
            YamahaMU100AudioProcessor::nativeSetupFilters() + ";*.syx");

        openChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            beginOpenOfFile(file);
        });
    });
}

//==============================================================================
// Second half of an open, once a file has been chosen — by the chooser above, or by
// File > Open Recent. Split out so both entry points sniff, remember and defer
// identically; the unsaved-changes guard belongs to the CALLER, because Open Recent
// must run it before it commits to the file it is about to load.
void YamahaMU100AudioProcessorEditor::beginOpenOfFile(const juce::File& file)
{
    // Sniff the format: first non-whitespace byte 0xF0 => raw SysEx, else native XML.
    bool isSyx = false;
    {
        juce::MemoryBlock mb;
        if (file.loadFileAsData(mb) && mb.getSize() > 0)
        {
            const auto* p = static_cast<const uint8_t*>(mb.getData());
            size_t i = 0;
            while (i < mb.getSize() && (p[i] == ' ' || p[i] == '\t'
                                     || p[i] == '\r' || p[i] == '\n' || p[i] == 0xEF
                                     || p[i] == 0xBB || p[i] == 0xBF))
                ++i;   // skip leading whitespace / UTF-8 BOM
            isSyx = (i < mb.getSize() && p[i] == 0xF0);
        }
    }

    audioProcessor.lastSetupDir = file.getParentDirectory().getFullPathName();
    if (isSyx)
        audioProcessor.lastSyxDir = file.getParentDirectory().getFullPathName();
    audioProcessor.addRecentFile(file.getFullPathName());   // saves settings itself

    // Defer until the device is past boot (else the streamed file is wiped by the
    // boot XG-reset or lost in the boot/read-back MIDI flood). timerCallback()
    // performs the deferred load once isReadyForEditing() is true.
    if (audioProcessor.isReadyForEditing())
    {
        isSyx ? doSysExLoad(file) : doSetupLoad(file);
    }
    else
    {
        pendingLoadFile    = file;
        pendingLoadIsSetup = ! isSyx;
    }
}

//==============================================================================
// File > Open Recent > <n>. Same guard and same load path as Open File; the only extra
// is the missing-file case, which the chooser cannot produce. A file that has since been
// deleted or moved is dropped from the list rather than silently doing nothing — the
// entry is stale, and leaving it there invites the same dead click again.
void YamahaMU100AudioProcessorEditor::openRecentClicked(const juce::File& file)
{
    if (! file.existsAsFile())
    {
        audioProcessor.recentFiles.removeString(file.getFullPathName(), true);
        audioProcessor.saveSettings();
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "File not found",
            file.getFullPathName() + "\n\nis no longer there, so it has been removed from "
            "the recent list.", "OK", this);
        return;
    }

    confirmDiscardIfDirty([this, file] { beginOpenOfFile(file); });
}

//==============================================================================
// Apply a native setup file. The KIND is read from the XML's setupKind attribute, not
// from the extension, so a renamed file still loads as what it is (and a .mss written
// before the partial formats existed, which carries no attribute, reads as System).
//
// A PARTIAL load deliberately does NOT call markStateSyncedToFile: the model now matches
// no single file, so the bottom strip keeps naming the .mss it was last synced to and the
// unsaved-changes '*' appears — which is exactly the truth.
void YamahaMU100AudioProcessorEditor::doSetupLoad(const juce::File& file)
{
    if (editorView != nullptr)
        editorView->cancelReadBack();
    autoReadBackDone = true;

    auto xml = juce::parseXML(file);
    if (xml == nullptr || ! xml->hasTagName("YamahaMU100State"))
        return;

    using SetupKind = YamahaMU100AudioProcessor::SetupKind;
    const SetupKind kind = YamahaMU100AudioProcessor::setupKindOfXml(*xml);
    if (kind != SetupKind::System)
    {
        doPartialSetupLoad(*xml, kind, file);
        return;
    }

    // Patch model from the editorState blob: seed factory defaults, overlay the file, and
    // (inside loadEditorFile) reset the plugin-only features — which applyPluginExtras then
    // overwrites with the file's own HPF2/EQ/SYNC values.
    juce::MemoryBlock eb;
    eb.fromBase64Encoding(xml->getStringAttribute("editorState", {}));
    if (eb.getSize() > 0)
        audioProcessor.loadEditorFile(static_cast<const uint8_t*>(eb.getData()), (size_t) eb.getSize());
    else
        audioProcessor.seedEditorDefaults();

    audioProcessor.applyPluginExtras(*xml);
    audioProcessor.editorStatePopulated = true;
    audioProcessor.markStateSyncedToFile(file.getFullPathName());   // openFileType = Native
    if (editorView != nullptr)
        editorView->applyDeviceState();

    const int sendMs = audioProcessor.sendPatchToDevice();
    beginSysExLoad(sendMs, {});
}

//==============================================================================
// Apply a Part / Drum / Effect setup file into the CURRENT selection. Only the settings
// the file covers move; everything else in the session is left exactly as it was.
void YamahaMU100AudioProcessorEditor::doPartialSetupLoad(
    const juce::XmlElement& xml, YamahaMU100AudioProcessor::SetupKind kind,
    const juce::File& file)
{
    using SetupKind = YamahaMU100AudioProcessor::SetupKind;

    // A Part setup targets whatever the Part module is editing, so there has to BE an
    // editor. (The menu that opens the chooser is already gated on this; the deferred-
    // load path can reach here without it.) A Drum setup addresses a buffer, not a part,
    // and an Effect setup addresses neither — both load with the GUI in any state.
    if (kind == SetupKind::Part
        && (editorView == nullptr || editorView->getCurrentPart() < 0))
    {
        showSetupLoadError(file, "Open the Editor view and select a part first — a Part "
                                 "Setup loads into the currently selected part.");
        return;
    }

    // Anything that writes a DRUMS buffer asks which one FIRST. The buffer is shared by
    // every part whose PART MODE names it, so the write reaches parts the user never
    // selected — better to choose deliberately than to be told afterwards.
    if (YamahaMU100AudioProcessor::setupTargetsDrumBuffer(xml))
    {
        askDrumBufferThenLoad(kind, file,
                              YamahaMU100AudioProcessor::drumBufferOfSetupXml(xml));
        return;
    }

    finishPartialSetupLoad(kind, file, /*targetBuffer*/ -1);
}


//==============================================================================
// THE PART MENU — copy / paste / reset, on a right-click.
//
// This is the plugin's FIRST context menu, and where it may appear is decided by a
// gesture that was already taken: right-click means "reset this control" on every
// ValueBox, knob, bar, EQ node and EG node in the editor. So the menu lives only where
// right-click previously meant NOTHING — a Voice module row, and the bar screens' part
// ruler, which was explicitly swallowing the button. It is never on a control.
//
// Both surfaces SELECT the part first. Every item acts on the selected part, so a menu
// opened on a row the rest of the editor is not showing would invite an edit aimed at
// one part and applied to another. Shift+F10 opens it on the selected part for the
// keyboard, which is what keeps the feature off the mouse-only list.
//
// The menu NAMES WHAT THE CLIPBOARD HOLDS ("Paste Part 5 settings"), which is the whole
// of the visual feedback for a copy: a copy changes nothing on screen, and a message
// that flashes and goes is one a user can miss. This answers the question at any later
// moment instead.
void YamahaMU100AudioProcessorEditor::showPartContextMenu(int part)
{
    if (part < 0 || part >= 32) return;

    const juce::String pn = "Part " + juce::String(part + 1);

    juce::PopupMenu menu;
    menu.addSectionHeader(pn);
    menu.addItem(1, "Copy " + pn + " settings");
    if (audioProcessor.hasPartCopy())
        menu.addItem(2, "Paste Part " + juce::String(audioProcessor.partCopySource() + 1)
                            + " settings");
    else
        menu.addItem(2, "Paste part settings", false);   // greyed: nothing copied yet
    menu.addSeparator();
    menu.addItem(3, "Reset " + pn + " to defaults");

    // A top-level popup does NOT inherit the editor's LookAndFeel (see themeDialog), so
    // a light-theme editor would pop a dark menu.
    menu.setLookAndFeel(&lookAndFeel);
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
                       [this, part](int result)
    {
        // A popup takes the keyboard; without this the shortcuts are dead afterwards.
        // Same rule the Voice List and the About box learned in the Cubase rounds.
        restoreKeyboardFocus(false);

        if      (result == 1) copyPartClicked(part);
        else if (result == 2) pastePartClicked(part);
        else if (result == 3) resetPartClicked(part);
    });
}

void YamahaMU100AudioProcessorEditor::copyPartClicked(int part)
{
    if (! audioProcessor.copyPart(part))
        return;
    // Speech only -- the menu above is what a sighted user reads it back from.
    say(mu100a11y::announce::Edit, "Part " + juce::String(part + 1) + " copied");
}

// Pasting writes the destination part AND, when the copied part sat on a DRUMS buffer,
// a whole buffer — which is shared by every part whose PART MODE names it. So the same
// chooser a .mps load puts up asks which buffer first, for the same reason: better to
// choose deliberately than to be told afterwards.
void YamahaMU100AudioProcessorEditor::pastePartClicked(int part)
{
    if (! audioProcessor.hasPartCopy()) return;

    if (audioProcessor.partCopyTargetsDrumBuffer())
    {
        askDrumBuffer("Part " + juce::String(audioProcessor.partCopySource() + 1)
                          + " settings",
                      "Paste",
                      audioProcessor.partCopySavedBuffer(),
                      [this, part](int buffer) { doPastePart(part, buffer); },
                      "Buffer It Was Copied From");
        return;
    }

    doPastePart(part, -1);
}

void YamahaMU100AudioProcessorEditor::doPastePart(int part, int targetBuffer)
{
    const int src = audioProcessor.partCopySource();
    if (! audioProcessor.pastePart(part, targetBuffer)) return;

    if (editorView != nullptr) editorView->applyDeviceState();
    refreshUndoButtons();
    say(mu100a11y::announce::Edit,
        "Part " + juce::String(src + 1) + " settings pasted to part "
            + juce::String(part + 1));
}

// Reset warns ONLY when it will re-initialise a DRUMS buffer, which is decided by the
// part's FACTORY PART MODE -- parts 10 and 26 on an untouched device. That is the one
// case where resetting one part changes what OTHER parts play, because a buffer's
// per-key settings are shared by everyone on it.
//
// A part that merely sits on a buffer today needs no warning: its reset writes PART MODE
// 0 first, so the program change that follows re-initialises nothing and its peers are
// untouched.
void YamahaMU100AudioProcessorEditor::resetPartClicked(int part)
{
    const int buf = audioProcessor.partResetDrumBuffer(part);
    if (buf < 0)
    {
        doResetPart(part);
        return;
    }

    const auto users = audioProcessor.partsOnDrumBuffer(buf);
    juce::StringArray others;
    for (const int p : users)
        if (p != part) others.add(juce::String(p + 1));

    juce::String msg;
    msg << "Part " << (part + 1) << " goes back to its factory drum kit, which "
        << "re-initializes the whole of " << drumbuf::name(buf) << " — every drum key's "
        << "settings in that buffer return to the kit's own defaults.";
    if (! others.isEmpty())
    {
        // "5", "5 and 12", "5, 12 and 26" -- JUCE's joinIntoString has no last-separator
        // form, and a bare comma list reads as a truncation at the end of a sentence.
        juce::String list = others[0];
        for (int i = 1; i < others.size(); ++i)
            list << (i == others.size() - 1 ? " and " : ", ") << others[i];

        msg << "\n\n" << (others.size() == 1 ? "Part " : "Parts ")
            << list
            << (others.size() == 1 ? " also uses " : " also use ") << drumbuf::name(buf)
            << ", so " << (others.size() == 1 ? "its" : "their") << " drum sounds change too.";
    }
    msg << "\n\nThis is a single undo step.";

    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "Reset Part " + juce::String(part + 1),
        msg, "Reset", "Cancel", this,
        juce::ModalCallbackFunction::create([this, part](int result)
        {
            restoreKeyboardFocus(false);
            if (result == 1) doResetPart(part);
        }));
}

void YamahaMU100AudioProcessorEditor::doResetPart(int part)
{
    if (! audioProcessor.resetPart(part)) return;

    if (editorView != nullptr) editorView->applyDeviceState();
    refreshUndoButtons();
    say(mu100a11y::announce::Edit, "Part " + juce::String(part + 1) + " reset to defaults");
}

// The drum-buffer chooser, for any action that writes a buffer. It was a .mps/.mds-only
// path holding the pending FILE; the pending action is now a callback, so a paste and a
// file load share one component and one set of rules about what is on each buffer.
void YamahaMU100AudioProcessorEditor::askDrumBuffer(const juce::String& what,
                                                    const juce::String& verb,
                                                    int savedBuffer,
                                                    std::function<void(int)> onChosen,
                                                    const juce::String& savedRow)
{
    // Which parts currently sit on each buffer — read from the MODEL, so it is right
    // even with the Editor view never opened.
    juce::StringArray users[drumbuf::kNumBuffers];
    for (int p = 0; p < 32; ++p)
    {
        const int b = audioProcessor.partDrumBufferFromModel(p);
        if (drumbuf::validBuffer(b)) users[b].add(juce::String(p + 1));
    }

    pendingBufferChoice = std::move(onChosen);

    if (drumBufferChooser == nullptr)
    {
        drumBufferChooser = std::make_unique<DrumBufferChooser>();
        drumBufferChooser->onChoose = [this](int buffer)
        {
            drumBufferChooser->setVisible(false);
            if (pendingBufferChoice) pendingBufferChoice(buffer);
        };
        drumBufferChooser->onCancel = [this]
        {
            drumBufferChooser->setVisible(false);   // nothing applied; model untouched
            pendingBufferChoice = nullptr;
        };
        addChildComponent(*drumBufferChooser);
    }

    drumBufferChooser->setContext(what, users, savedBuffer, verb, savedRow);
    drumBufferChooser->setBounds(getLocalBounds());
    drumBufferChooser->setVisible(true);
    drumBufferChooser->toFront(true);
}

// Park the file and put the chooser up; the choice resumes the load. The XML is re-parsed
// on resume rather than held, so there is one source of truth for what gets applied.
void YamahaMU100AudioProcessorEditor::askDrumBufferThenLoad(
    YamahaMU100AudioProcessor::SetupKind kind, const juce::File& file, int savedBuffer)
{
    pendingSetupKind = kind;
    pendingSetupFile = file;

    askDrumBuffer(YamahaMU100AudioProcessor::setupKindLabel(kind), "Load", savedBuffer,
                  [this](int buffer)
                  {
                      finishPartialSetupLoad(pendingSetupKind, pendingSetupFile, buffer);
                  });
}

void YamahaMU100AudioProcessorEditor::finishPartialSetupLoad(
    YamahaMU100AudioProcessor::SetupKind kind, const juce::File& file, int targetBuffer)
{
    using SetupKind = YamahaMU100AudioProcessor::SetupKind;

    auto xml = juce::parseXML(file);
    if (xml == nullptr || ! xml->hasTagName("YamahaMU100State"))
    {
        showSetupLoadError(file, "The file could not be read.");
        return;
    }

    const int part = (editorView != nullptr) ? editorView->getCurrentPart() : -1;

    int sendMs = -1;
    switch (kind)
    {
        case SetupKind::Part:   sendMs = audioProcessor.applyPartSetup(*xml, part, targetBuffer); break;
        case SetupKind::Drum:   sendMs = audioProcessor.applyDrumSetup(*xml, targetBuffer);       break;
        case SetupKind::Effect: sendMs = audioProcessor.applyEffectSetup(*xml);                   break;
        default: break;
    }

    if (sendMs < 0)
    {
        showSetupLoadError(file, "The file does not contain the data for this setup type.");
        return;
    }

    if (editorView != nullptr)
        editorView->applyDeviceState();   // panels (and the part->buffer map) follow the model

    beginSysExLoad(sendMs, {});
}

void YamahaMU100AudioProcessorEditor::showSetupLoadError(const juce::File& file,
                                                         const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::WarningIcon,
        "Cannot load " + file.getFileName(), message, "OK", this);
}

//==============================================================================
// File->Save Setup (.mss): overwrite the current native file in place. Falls through to
// Save Setup As when there is no native file yet — either untitled, or a raw .syx is the
// loaded file (which must never be overwritten by a setup save). Always the SYSTEM setup:
// the partial kinds are per-selection and have no "the file this session came from".
void YamahaMU100AudioProcessorEditor::saveSetupClicked()
{
    const juce::File current(audioProcessor.loadedFilePath);
    const bool haveNative =
        audioProcessor.openFileType == YamahaMU100AudioProcessor::OpenFileType::Native
        && current.getFullPathName().isNotEmpty();

    if (! haveNative)
    {
        saveSetupAsClicked(YamahaMU100AudioProcessor::SetupKind::System);
        return;
    }

    if (audioProcessor.saveSetupToFile(current))
    {
        audioProcessor.markStateSyncedToFile(current.getFullPathName());
        audioProcessor.addRecentFile(current.getFullPathName());
    }
}

//==============================================================================
// File->Save Setup As: always prompt, for the kind the submenu picked. The System kind
// defaults to the current native file's name/folder when one is open; every kind
// otherwise gets a fresh "meganxg-<stamp>.<ext>" in the last setup folder (or My
// Documents\MEGAN XG). The single-extension filter + forced extension mean a loaded
// .syx cannot be overwritten here even if the user navigates to it.
//
// Only a System save re-syncs the loaded-file marker. A partial file is a fragment of the
// session, so writing one neither renames the session nor clears its unsaved-changes '*'.
void YamahaMU100AudioProcessorEditor::saveSetupAsClicked(
    YamahaMU100AudioProcessor::SetupKind kind)
{
    using SetupKind = YamahaMU100AudioProcessor::SetupKind;
    const juce::String ext = YamahaMU100AudioProcessor::setupKindExtension(kind);
    const juce::String label = YamahaMU100AudioProcessor::setupKindLabel(kind);

    const int part   = (editorView != nullptr) ? editorView->getCurrentPart() : -1;
    const int buffer = (editorView != nullptr) ? editorView->getCurrentPartDrumBuffer() : -1;

    if (kind != SetupKind::Effect && part < 0)
    {
        showSetupSaveError(label, "No part is selected.");
        return;
    }
    // The abort: a Drum Setup saves the buffer the current part is on, so without one
    // there is nothing to save. The menu item is already disabled in that state; this
    // catches the assignment changing between opening the menu and picking the item.
    if (kind == SetupKind::Drum && buffer < 0)
    {
        showSetupSaveError(label,
            "Part " + juce::String(part + 1) + " is not assigned to a drum edit buffer.\n\n"
            "A Drum Setup saves the per-key data of the buffer the selected part uses, so "
            "that part's kit must be allocated to DRUMS1-4 (set from the Voice list).");
        return;
    }

    const juce::File current(audioProcessor.loadedFilePath);
    const bool haveNative =
        kind == SetupKind::System
        && audioProcessor.openFileType == YamahaMU100AudioProcessor::OpenFileType::Native
        && current.getFullPathName().isNotEmpty();

    juce::File defaultDir = haveNative ? current.getParentDirectory()
                          : audioProcessor.lastSetupDir.isNotEmpty()
                                ? juce::File(audioProcessor.lastSetupDir)
                                : juce::File{};
    if (! defaultDir.isDirectory())
        defaultDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                         .getChildFile("MEGAN XG");

    const juce::String name = haveNative ? current.getFileName()
                                         : YamahaMU100AudioProcessor::makeSetupDefaultFileName(kind);
    const juce::File   suggested = defaultDir.getChildFile(name);

    setupSaveChooser = std::make_unique<juce::FileChooser>(
        "Save " + label + " (." + ext + ")", suggested, "*." + ext);

    setupSaveChooser->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, kind, ext, label, part, buffer](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
            return;
        if (! file.hasFileExtension(ext))
            file = file.withFileExtension(ext);

        bool ok = false;
        if (kind == SetupKind::System)
        {
            ok = audioProcessor.saveSetupToFile(file);
        }
        else
        {
            std::unique_ptr<juce::XmlElement> xml;
            switch (kind)
            {
                case SetupKind::Part:   xml = audioProcessor.buildPartSetupXml(part);   break;
                case SetupKind::Drum:   xml = audioProcessor.buildDrumSetupXml(buffer); break;
                case SetupKind::Effect: xml = audioProcessor.buildEffectSetupXml();     break;
                default: break;
            }
            ok = xml != nullptr && YamahaMU100AudioProcessor::writeSetupXml(file, *xml);
        }

        if (! ok)
        {
            showSetupSaveError(label, "Could not write " + file.getFullPathName() + ".");
            return;
        }

        audioProcessor.lastSetupDir = file.getParentDirectory().getFullPathName();
        // Every native kind joins the recent list, not just System: a .mps / .mds / .mes
        // is opened through the same File > Open, so it is exactly as re-openable.
        audioProcessor.addRecentFile(file.getFullPathName());   // saves settings itself
        if (kind == SetupKind::System)
            audioProcessor.markStateSyncedToFile(file.getFullPathName());
    });
}

void YamahaMU100AudioProcessorEditor::showSetupSaveError(const juce::String& label,
                                                         const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::WarningIcon,
        "Cannot save " + label, message, "OK", this);
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::saveMultiSyxClicked()
{
    // Build a 32-part multi bulk dump from the editor's working device-state model
    // (the same {model,addr}->byte map a device dumpout reduces to, kept live by
    // param edits + read-back). Generating it from the model is instant and avoids
    // a slow, race-prone device round-trip. Pure bulk dumps, no leading XG System
    // On — matching the device's own mult-32 dumpout that this mirrors.
    const std::vector<uint8_t> full = audioProcessor.getEditorState().toBulkDumps(0, false);

    // Drop the A/D part / A/D system blocks (model 4C, address-high 0x10 / 0x11) that
    // the read-back also collects, so the file is a PURE Multi 32-Part dump — matching
    // the device's own "mult-32" dumpout (and so makeDumpDefaultFileName names it
    // "mult-32-", not "mult-32ad-").
    std::vector<uint8_t> bytes;
    bytes.reserve(full.size());
    for (size_t i = 0; i < full.size(); )
    {
        if (full[i] != 0xF0) { ++i; continue; }
        const size_t s = i;
        while (i < full.size() && full[i] != 0xF7) ++i;
        if (i < full.size()) ++i;   // include the F7
        const bool isAD = (i - s) >= 9 && full[s + 1] == 0x43 && (full[s + 2] & 0xF0) == 0x00
                          && full[s + 3] == 0x4C && (full[s + 6] == 0x10 || full[s + 6] == 0x11);
        if (! isAD) bytes.insert(bytes.end(), full.begin() + (long) s, full.begin() + (long) i);
    }
    if (bytes.empty())
        return;

    // Share the dump directory persistence with the Hardware-view capture (the same
    // lastDumpDir field): default to the last-used dump dir, else My Documents\MEGAN XG.
    juce::File defaultDir = audioProcessor.lastDumpDir.isNotEmpty()
                              ? juce::File(audioProcessor.lastDumpDir)
                              : juce::File{};
    if (! defaultDir.isDirectory())
        defaultDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                         .getChildFile("MEGAN XG");

    // Same default-name classifier as the Hardware-view dump -> "mult-32-<stamp>.syx".
    const juce::String name      = YamahaMU100AudioProcessor::makeDumpDefaultFileName(bytes);
    const juce::File    suggested = defaultDir.getChildFile(name);

    saveSyxChooser = std::make_unique<juce::FileChooser>(
        "Save Multi 32-Part SysEx (.syx)", suggested, "*.syx");

    // Keep the generated bytes alive for the async callback.
    auto payload = std::make_shared<std::vector<uint8_t>>(bytes);
    saveSyxChooser->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, payload](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file != juce::File{})
        {
            file.replaceWithData(payload->data(), payload->size());
            audioProcessor.lastDumpDir = file.getParentDirectory().getFullPathName();
            audioProcessor.saveSettings();
            // The model is now saved to this file: show its path, clear the marker.
            audioProcessor.markStateSyncedToFile(file.getFullPathName());
        }
    });
}

void YamahaMU100AudioProcessorEditor::doSysExLoad(const juce::File& file)
{
    // Free MIDI IN A: abort any in-flight read-back (e.g. the post-open auto read-back
    // after a reset) so the streamed file isn't lost in its request flood. Suppress a
    // fresh auto read-back too.
    if (editorView != nullptr)
        editorView->cancelReadBack();
    autoReadBackDone = true;

    // Populate the editor from the FILE, not from a device read-back. The file IS the
    // authoritative state: it is loaded as factory-defaults overlaid with the file, so
    // the model is COMPLETE (every param resolved — omitted ones fall back to default,
    // not a previous file's value) and the codec parses both framings (XGEdit stream /
    // bulk dump) exactly. Panels refresh instantly and save/recall is exact — no slow,
    // lossy MIDI round-trip that dropped parts or corrupted values on the way back.
    juce::MemoryBlock mb;
    if (! (file.loadFileAsData(mb) && mb.getSize() > 0))
        return;

    audioProcessor.loadEditorFile(static_cast<const uint8_t*>(mb.getData()),
                                  (size_t) mb.getSize());
    audioProcessor.editorStatePopulated = true;
    // The model now matches this file: show its path in the bottom strip, marker clear.
    audioProcessor.markStateSyncedToFile(file.getFullPathName());
    if (editorView != nullptr)
        editorView->applyDeviceState();   // panels reflect the loaded patch NOW

    // Push the patch to the device so it AUDIBLY plays it: XG System On + the model's
    // non-default params as a (sparse, reliable) parameter-change stream. No read-back.
    const int sendMs = audioProcessor.sendPatchToDevice();
    beginSysExLoad(sendMs, {});
}

//==============================================================================
// Show the "Loading file" overlay and seed its time-based progress. The overlay
// stays up for the streaming estimate (from loadSysExFile) plus a short settle
// margin so the device has finished applying the dump before any follow-up
// read-back starts. timerCallback() advances the bar and calls finishSysExLoad().
void YamahaMU100AudioProcessorEditor::beginSysExLoad(int loadMs, std::vector<int> drumSetups)
{
    loadingActive  = true;
    loadDrumSetups = std::move(drumSetups);   // fed to the follow-up read-back
    loadStartMs    = juce::Time::getMillisecondCounter();
    loadExpectedMs = juce::jmax(0, loadMs) + kSysExSettleMs;

    if (loadOverlay)
    {
        loadOverlay->setProgress(0.0);
        loadOverlay->setBounds(getLocalBounds());
        loadOverlay->setVisible(true);
        loadOverlay->toFront(false);          // above both views; no focus stealing
    }
}

// Streaming + settle window has elapsed: hide the overlay and refresh the editor.
void YamahaMU100AudioProcessorEditor::finishSysExLoad()
{
    loadingActive = false;
    if (loadOverlay)
    {
        loadOverlay->setProgress(1.0);
        loadOverlay->setVisible(false);
    }

    // No device read-back: the panels were already refreshed from the file in
    // doSysExLoad(), reliably and completely. Reading the loaded patch back from the
    // device was the slow step and could drop/corrupt the very state we already hold.
    // The device stream (for audio) plays out on its own.
    loadDrumSetups.clear();
}

//==============================================================================
// The Options menu — ONE menu for both hosts (2026-08-09). The VST used to open a
// single consolidated Options window instead, which had grown into a stack of
// unrelated settings: audio routing next to display preferences next to view
// choices. Now both hosts get the same menu, and only the things that genuinely
// need a window (Audio, Display Animation) open one.
//
// Order: the two windows, then the plain toggles, then the multi-choice submenus. The
// toggles are alphabetical among themselves, as are the submenus -- so a new one goes
// where its name puts it, not on the end.
// Every item below "Audio..." is identical in both builds except the ones that can
// only mean something in one of them — see the gates inline.
void YamahaMU100AudioProcessorEditor::showSettingsMenu()
{
    const bool standalone = juce::JUCEApplicationBase::isStandaloneApp();

    juce::PopupMenu menu;

    // MIDI routing is standalone-only (the DAW manages routing for the VST).
    if (standalone)
    {
        menu.addItem(1, "MIDI Input...");
        menu.addItem(4, "MIDI Output...");
        // Recovery for a process that has stopped receiving MIDI callbacks
        // entirely while its handles still look open -- see reconnectMidiDevices().
        menu.addItem(12, "Reconnect MIDI Inputs");
        menu.addSeparator();
    }

    menu.addItem(2, "Audio...");
    // Was a "MIDI input indicators" tick-item; that toggle now sits at the top of the
    // Display Animation window, above the EQ analyser settings it belongs with.
    menu.addItem(7, "Display Animation...");
    menu.addSeparator();

    // VST only, like the marker submenu below and for the same reason: the standalone
    // exposes no host parameters, so there is no automation there to enable. It is also
    // the one toggle here that is PROJECT state rather than settings.xml -- see
    // dawAutomationEnabled in the processor -- and the standalone deliberately persists
    // no project state, so an item there would forget itself on every launch.
    if (! standalone)
        menu.addItem(30, "Enable DAW automation", true,
                     audioProcessor.dawAutomationEnabled);
    // Only meaningful with a transport running: it decides whether incoming notes
    // may move the drum-key selection while the DAW is playing.
    if (! standalone)
        menu.addItem(16, "Enable MIDI drum key selection during DAW playback", true,
                     audioProcessor.midiDrumSelectDuringPlayback);
    menu.addItem(8, "Enable plugin-only parameters", true,
                 audioProcessor.enablePluginOnlyParams);
    // The follow lock, in its alphabetical place. Phrased as what it DOES and default
    // ON -- it was "Ignore MIDI changes to the device", default off, which is the same
    // behaviour read backwards; every other toggle here names the thing it switches on,
    // so the odd one out was the phrasing, not the default. Global. Unticking it is for
    // the session where a sequencer is spraying changes and you want the panels to sit
    // still.
    menu.addItem(31, "Follow MIDI changes to the device", true,
                 audioProcessor.midiFollowEnabled);
    menu.addItem(6, "Invert knob/dial direction", true, audioProcessor.reverseDialAction);
    // The two kinds of mark by a control's name, side by side. The DAW mark (triangle)
    // is VST-only: without a host there is nothing to mark. It used to be a submenu of
    // styles; the underline now belongs to the MIDI CC mark, so it is a plain toggle.
    if (! standalone)
        menu.addItem(17, "Mark DAW-automatable parameters", true,
                     audioProcessor.markDawAutomatable);
    // The MIDI CC mark (underline), both hosts. GRAYED, not hidden or reset, while MIDI
    // Out is on XG SysEx: there is nothing to mark then, and the choice must survive a
    // trip through SysEx mode -- gate the control, never the value.
    menu.addItem(39, "Mark parameters that send MIDI CC",
                 audioProcessor.midiOutFormat == YamahaMU100AudioProcessor::MidiOutCC,
                 audioProcessor.markMidiCcParams);
    // Display only, and deliberately alphabetical among the toggles rather than next to
    // the override items below: hiding the marks changes no value and sends nothing.
    menu.addItem(34, "Show changed markers", true,
                 audioProcessor.showChangedMarks);
    menu.addItem(23, "Show Mute and Solo columns", true,
                 audioProcessor.showMuteSoloColumns);
    menu.addItem(9, "Show part MIDI receive switches", true,
                 audioProcessor.showPartRcvSwitches);
    menu.addSeparator();

    // Both act on what MIDI has done since this instance opened, so both are inert
    // -- and shown inert -- when nothing has. The count is in the item text rather
    // than a separate heading: it is the one number that decides whether either is
    // worth picking.
    {
        const int n = audioProcessor.midiOverrideTotal();
        const juce::String suffix = n > 0 ? (" (" + juce::String(n) + ")") : juce::String();
        menu.addItem(32, "Clear MIDI overrides" + suffix, n > 0);
        menu.addItem(33, "Keep MIDI overrides" + suffix, n > 0);
    }
    menu.addSeparator();

    // Which editor controls the DAW can automate natively. VST-only, and none is
    // wanted in the standalone: without a host there is nothing to mark, and those
    // parameters cannot be reached at all from that build. (Every parameter is still
    // automatable as XG SysEx on a MIDI track; the marker distinguishes the two
    // kinds, it does not say what can and cannot be automated at all.)
    // "Label notes as" submenu — note letters or bare MIDI note numbers, everywhere
    // the editor prints a note as text (see NoteLabel.h for the list). First among the
    // multi-choice submenus because they are alphabetical among themselves.
    //
    // The two rows carry an EXAMPLE of the same note rather than a bare word, which is
    // what quietly says this plugin calls middle C `C3` — the fact that makes the
    // option worth having, since other DAWs call it C4 or C5.
    const int nlMode = audioProcessor.noteLabelMode;
    juce::PopupMenu noteLabelMenu;
    noteLabelMenu.addItem(35, "Note letters (C3)", true,
                          nlMode == YamahaMU100AudioProcessor::NoteLabelLetters);
    noteLabelMenu.addItem(36, "MIDI note numbers (60)", true,
                          nlMode == YamahaMU100AudioProcessor::NoteLabelNumbers);
    menu.addSubMenu("Label notes as", noteLabelMenu);

    // "MIDI Out sends edits as" — what the plugin's MIDI OUT carries for each edit, so a
    // DAW records it. Both hosts: the standalone's MIDI Output devices carry it too.
    // The bracketed halves state the trade-off where the choice is made: SysEx covers
    // every XG parameter (the plugin-only ones have no XG address in either mode), CC
    // only those with a controller -- and in CC mode the rest send nothing at all.
    const int moFmt = audioProcessor.midiOutFormat;
    juce::PopupMenu midiOutMenu;
    midiOutMenu.addItem(37, "XG SysEx (every XG parameter)", true,
                        moFmt == YamahaMU100AudioProcessor::MidiOutSysEx);
    midiOutMenu.addItem(38, "MIDI CC (only parameters that have a CC)", true,
                        moFmt == YamahaMU100AudioProcessor::MidiOutCC);
    menu.addSubMenu("MIDI Out sends edits as", midiOutMenu);

    // "Part module title shows" submenu — whether the Part module's title names what
    // it is editing. TWO CHECK ITEMS, not a four-row radio group, because the halves
    // are independent settings (see PartTitleNames in the processor); neither ticked is
    // the default and the behaviour the module has always had. Both hosts: nothing here
    // needs a DAW. Alphabetical among the submenus, so it follows "Mark...".
    const int ptn = audioProcessor.partTitleNames;
    juce::PopupMenu titleNameMenu;
    titleNameMenu.addItem(28, "The part's number and voice", true,
                          (ptn & YamahaMU100AudioProcessor::PartTitleNamePart) != 0);
    titleNameMenu.addItem(29, "The drum key and instrument", true,
                          (ptn & YamahaMU100AudioProcessor::PartTitleNameDrum) != 0);
    menu.addSubMenu("Part module title shows", titleNameMenu);

    // "Show the arrow-key mark" submenu — when the keyboard cursor's mark is drawn.
    // Sits here because the multi-choice submenus are alphabetical among themselves
    // (Mark..., Show..., Tooltips, View, Voice names) and grouped at the bottom, so the
    // single-click toggles stay one uninterrupted block.
    //
    // There is deliberately no "never" -- see FocusMarkMode in the processor.
    const int fmMode = audioProcessor.focusMarkMode;
    juce::PopupMenu markMenu;
    markMenu.addItem(26, "Always", true,
                     fmMode == YamahaMU100AudioProcessor::FocusMarkAlways);
    markMenu.addItem(27, "Only while using the computer keyboard", true,
                     fmMode == YamahaMU100AudioProcessor::FocusMarkKeyboard);
    menu.addSubMenu("Show the arrow-key mark", markMenu);

    // Tooltips submenu — where hover text appears.
    const int tipMode = audioProcessor.tooltipMode;
    juce::PopupMenu tipMenu;
    tipMenu.addItem(13, "Pop-ups near cursor", true,
                    tipMode == YamahaMU100AudioProcessor::TooltipPopup);
    tipMenu.addItem(14, "Bottom bar", true,
                    tipMode == YamahaMU100AudioProcessor::TooltipBottomBar);
    tipMenu.addItem(15, "None", true,
                    tipMode == YamahaMU100AudioProcessor::TooltipNone);
    menu.addSubMenu("Tooltips", tipMenu);

    // View submenu — replaced both the old "Default View" submenu and the bottom
    // bar's Hardware/Editor toggle button. It switches the view on the spot AND is
    // what the plugin reopens in, because the two were never really different
    // settings: `defaultEditorView` now simply records the last view chosen.
    //
    // Three views, three ticks, one of them always on. Dark/Light are the same view
    // in two palettes (edcol), so picking either switches TO the editor as well as
    // setting the theme — which is why the tick is on the theme only while the
    // editor is the view on screen. The theme itself is remembered independently
    // (audioProcessor.editorTheme), so a trip through Hardware and back returns to
    // the palette last chosen rather than resetting to dark.
    const bool lightChosen = audioProcessor.editorTheme.equalsIgnoreCase("Light");
    juce::PopupMenu viewMenu;
    viewMenu.addItem(11, "Dark Editor",  true, showingEditor && ! lightChosen);
    viewMenu.addItem(20, "Light Editor", true, showingEditor &&   lightChosen);
    viewMenu.addItem(10, "Hardware",     true, ! showingEditor);
    menu.addSubMenu("View", viewMenu);

    // Voice names submenu — which of the two names kVoices carries for every voice
    // is shown, everywhere one appears: the Voice module's part rows, the Voice List
    // browser, and drum/SFX kits along with the rest. Display only; nothing is stored
    // or transmitted differently, so switching back loses nothing.
    //
    // It sits below View because it is the other thing that changes what the editor
    // LOOKS like rather than what it does.
    const bool shortNames = audioProcessor.shortVoiceNames;
    juce::PopupMenu nameMenu;
    nameMenu.addItem(21, "Full",                 true, ! shortNames);
    nameMenu.addItem(22, "8 characters (as the hardware shows them)",
                                                 true,   shortNames);
    menu.addSubMenu("Voice names", nameMenu);

    // Keyboard shortcuts. The tick is the master switch -- with it off nothing here
    // takes a key from the host, and every shortcut column in these menus goes blank.
    // The editor stays reachable either way, so a map can be set up before switching on.
    menu.addSeparator();
    menu.addItem(24, "Use keyboard shortcuts", true, audioProcessor.useKeyboardShortcuts);
    {
        juce::PopupMenu::Item i;
        i.itemID = 25;
        i.text   = "Edit keyboard shortcuts...";
        i.shortcutKeyDescription = shortcutText(kbsc::OptionsShortcuts);
        menu.addItem(std::move(i));
    }

    // The manual used to sit here. It moved to the Help menu on 2026-08-31: it is a
    // document, not a setting, and About was already a button of its own with nothing
    // beside it.

    // "Reset to default state" removed — File->New now covers it (soft reset, both hosts).

    menu.setLookAndFeel(&lookAndFeel);   // see themeDialog(): a top-level popup does not inherit
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(settingsButton),
                        [this](int result)
    {
        if (result == 1)
            showMidiRoutingDialog();
        else if (result == 4)
            showMidiOutputDialog();
        else if (result == 2)
            showAudioSettingsDialog();
        else if (result == 26 || result == 27)
        {
            audioProcessor.focusMarkMode = (result == 27)
                                             ? YamahaMU100AudioProcessor::FocusMarkKeyboard
                                             : YamahaMU100AudioProcessor::FocusMarkAlways;
            audioProcessor.saveSettings();
            applyFocusMarkMode();   // instant, rather than a frame late
        }
        else if (result == 28 || result == 29)
        {
            // XOR the one bit the row owns, so ticking either leaves the other alone —
            // which is the whole reason these are checks and not a radio group.
            const int bit = (result == 28) ? YamahaMU100AudioProcessor::PartTitleNamePart
                                           : YamahaMU100AudioProcessor::PartTitleNameDrum;
            audioProcessor.partTitleNames ^= bit;
            audioProcessor.saveSettings();
            applyPartTitleNames();   // instant, rather than a frame late
        }
        else if (result == 35 || result == 36)
        {
            audioProcessor.noteLabelMode = (result == 36)
                                             ? YamahaMU100AudioProcessor::NoteLabelNumbers
                                             : YamahaMU100AudioProcessor::NoteLabelLetters;
            audioProcessor.saveSettings();
            applyNoteLabels();   // instant, rather than a frame late
        }
        else if (result == 37 || result == 38)
        {
            audioProcessor.midiOutFormat = (result == 38)
                                             ? YamahaMU100AudioProcessor::MidiOutCC
                                             : YamahaMU100AudioProcessor::MidiOutSysEx;
            audioProcessor.saveSettings();
            applyAutomationMarkers();   // the CC marks AND the badge exist only in CC mode
        }
        else if (result >= 13 && result <= 15)
        {
            audioProcessor.tooltipMode = result - 13;   // 13/14/15 -> Popup/BottomBar/None
            audioProcessor.saveSettings();
            applyTooltipEnabled();
        }
        else if (result == 17)
        {
            audioProcessor.markDawAutomatable = ! audioProcessor.markDawAutomatable;
            audioProcessor.saveSettings();
            applyAutomationMarkers();   // instant, rather than up to a frame late
        }
        else if (result == 39)
        {
            audioProcessor.markMidiCcParams = ! audioProcessor.markMidiCcParams;
            audioProcessor.saveSettings();
            applyAutomationMarkers();
        }
        else if (result == 6)
        {
            audioProcessor.reverseDialAction = ! audioProcessor.reverseDialAction;
            dialaction::setReverse(audioProcessor.reverseDialAction);
            audioProcessor.saveSettings();
        }
        else if (result == 8)
        {
            audioProcessor.enablePluginOnlyParams = ! audioProcessor.enablePluginOnlyParams;
            audioProcessor.saveSettings();
            // The timer would pick this up too, but pushing it here makes the
            // greying instant instead of up to a frame late.
            applyPluginOnlyLock();
        }
        else if (result == 30)
        {
            audioProcessor.setDawAutomationEnabled(! audioProcessor.dawAutomationEnabled);
            // NOT saveSettings(): this one lives in project state only, so there is no
            // global value to write (see setPartOutputsEnabled for the same shape).
            // Tell the host its project changed, or the switch is silently lost on a
            // close with nothing else edited.
            audioProcessor.updateHostDisplay(
                juce::AudioProcessor::ChangeDetails{}.withNonParameterStateChanged(true));
            // The markers say "the DAW can automate this", so they follow it. Pushed
            // here as well as mirrored by the timer, to make it instant.
            applyAutomationMarkers();
        }
        else if (result == 9)
        {
            audioProcessor.showPartRcvSwitches = ! audioProcessor.showPartRcvSwitches;
            audioProcessor.saveSettings();
            applyRcvRowVisible();   // instant, rather than up to a frame late
        }
        else if (result == 34)
        {
            // Paint only: nothing is edited, nothing is sent, and no gutter is given
            // back -- so this is safe to flip mid-drag and mid-session.
            audioProcessor.showChangedMarks = ! audioProcessor.showChangedMarks;
            audioProcessor.saveSettings();
            applyChangedMarks();   // instant, rather than up to a frame late
        }
        else if (result == 23)
        {
            // Visibility only: any parts already muted or soloed STAY that way, and
            // the bottom-bar indicator is what keeps that visible. Clearing the state
            // here would be worse -- an option about what is DRAWN would silently
            // change what is HEARD.
            audioProcessor.showMuteSoloColumns = ! audioProcessor.showMuteSoloColumns;
            audioProcessor.saveSettings();
            applyMuteSoloCols();
        }
        else if (result == 16)
        {
            audioProcessor.midiDrumSelectDuringPlayback
                = ! audioProcessor.midiDrumSelectDuringPlayback;
            audioProcessor.saveSettings();
        }
        else if (result == 31)
        {
            audioProcessor.midiFollowEnabled = ! audioProcessor.midiFollowEnabled;
            audioProcessor.saveSettings();
            // Switching following OFF drops what is already marked. Leaving the old
            // marks up would be the worst of both: they would freeze at whatever the
            // last message happened to say and go on claiming to describe the device.
            if (! audioProcessor.midiFollowEnabled)
                audioProcessor.clearMidiOverrides(false);
        }
        else if (result == 32)
        {
            // Put the device back on what the panels have been showing all along.
            const int n = audioProcessor.midiOverrideTotal();
            audioProcessor.clearMidiOverrides(true);
            // Both commands leave the marks and the badge gone, so their outcomes are
            // identical in silence and opposite in meaning. Said, with the count.
            sayOverrideAction(juce::String(n) + (n == 1 ? " MIDI override cleared"
                                                        : " MIDI overrides cleared")
                              + ", the device is back on your values");
        }
        else if (result == 33)
        {
            // Make what the song did part of the setup. One undo step, and the
            // panels have to re-read: this is the one path where an override
            // actually changes a value.
            const int n = audioProcessor.midiOverrideTotal();
            audioProcessor.keepMidiOverrides();
            if (editorView) editorView->applyDeviceState();
            sayOverrideAction(juce::String(n) + (n == 1 ? " MIDI override kept"
                                                        : " MIDI overrides kept")
                              + ", your setup now has what MIDI sent");
        }
        else if (result == 21 || result == 22)
        {
            // 21 = full names, 22 = the hardware's 8-character names. Pushed directly
            // as well as saved, so the relabel is instant rather than up to a frame
            // late. Output-bus names are deliberately NOT re-pushed here: renaming the
            // DAW's tracks stays the explicit File menu action (see refreshBusNames —
            // kIoTitlesChanged renames ALL of them, so it must never fire implicitly).
            audioProcessor.shortVoiceNames = (result == 22);
            audioProcessor.saveSettings();
            applyVoiceNames();
        }
        else if (result == 24)
        {
            audioProcessor.useKeyboardShortcuts = ! audioProcessor.useKeyboardShortcuts;
            audioProcessor.saveSettings();
            // The chords in the button tooltips appear/disappear with the option, so
            // push it rather than waiting for the next hover to compose stale text.
            refreshShortcutTooltips();
        }
        else if (result == 25)
            showShortcutEditor();
        else if (result == 12)
            audioProcessor.reconnectMidiDevices();
        else if (result == 7)
            showDisplayAnimationDialog();
        else if (result == 10 || result == 11 || result == 20)
        {
            // 11 = Dark Editor, 20 = Light Editor, 10 = Hardware. The theme is
            // written BEFORE setView so the view switch installs it in one step
            // (setView -> applyEditorTheme) instead of rebuilding the panels twice.
            // Hardware leaves editorTheme alone — it is not a theme, it is the
            // other view, and the choice has to survive the round trip.
            if (result != 10)
            {
                const juce::String want = (result == 20) ? "Light" : "Dark";
                if (audioProcessor.editorTheme != want)
                {
                    audioProcessor.editorTheme = want;
                    audioProcessor.saveSettings();
                }
            }
            setView(result != 10);   // persists the view choice itself
        }
    });
}

//==============================================================================
// The "Display Animation..." window, both hosts. It used to be embedded in the
// VST's consolidated Options window (same component, so they could not drift);
// since that window was split up, both hosts open this dialog.
void YamahaMU100AudioProcessorEditor::showDisplayAnimationDialog()
{
    auto* content = new DisplayAnimationComponent(audioProcessor, /*withHeading*/ false);
    // Width from the component, not a literal: the label column grows with
    // editorTextOptionsPct, and a fixed 430 would clip the combos once it does.
    content->setSize(DisplayAnimationComponent::widthFor(),
                     DisplayAnimationComponent::heightFor(false));

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle              = "Display Animation for MEGAN XG";
    opts.dialogBackgroundColour   = edcol::kDialogBg;
    opts.content.setOwned(content);
    opts.componentToCentreAround  = this;
    opts.resizable                = false;
    opts.useNativeTitleBar        = true;
    themeDialog(opts.launchAsync());
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::showAudioSettingsDialog()
{
    // Two groups are VST-only: the per-part output buses (the standalone cannot open
    // them) and the PDC trim (inert without a host). See the component's ctor comment.
    const bool hosted = ! juce::JUCEApplicationBase::isStandaloneApp();
    auto* content = new AudioSettingsComponent(audioProcessor, /*hosted*/ hosted);

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle              = "Audio Settings for MEGAN XG";
    opts.dialogBackgroundColour   = edcol::kDialogBg;
    opts.content.setOwned(content);
    opts.componentToCentreAround  = this;
    opts.resizable                = false;
    opts.useNativeTitleBar        = true;
    themeDialog(opts.launchAsync());
}

//==============================================================================
// Keyboard shortcuts.
//
// The gating rule below is "everything that acts on, or is only visible in, the
// Editor view requires the Editor view". Over the Hardware photo that leaves File,
// PANIC, About, the manual, the shortcut editor and the view toggle -- and, just as
// importantly, it hands Ctrl+Z straight back to the DAW while the photo is showing,
// which is exactly when the undo arrows are hidden and there is nothing to undo.

bool YamahaMU100AudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    // Never act while a modal component (a file chooser, an alert, the drum-buffer
    // chooser's backdrop) is up: those own the keyboard for as long as they are there.
    if (juce::Component::getCurrentlyModalComponent() != nullptr)
        return false;

    // ...nor while text is being typed. A ValueBox opens a real TextEditor, which
    // consumes the printable keys itself, but not every chord -- Alt+S while editing a
    // value must not solo the part.
    //
    // Both tests moved ABOVE the shortcuts switch when the accessibility floor landed:
    // they guard that path too, and a modal dialog owns the keyboard whatever the
    // option says.
    if (auto* focused = juce::Component::getCurrentlyFocusedComponent())
        if (dynamic_cast<juce::TextEditor*>(focused) != nullptr)
            return false;

    if (! audioProcessor.useKeyboardShortcuts)
        return keyForFocusedControl(key);

    const int cmd = audioProcessor.keyBindings.findCommand(key);
    if (cmd < 0)
        return false;

    return performCommand(cmd);
}

// The ACCESSIBILITY KEY FLOOR.
//
// Accessibility is NOT an option here -- but the shortcut
// layer IS one, and it defaults OFF when hosted. Without this, a screen-reader user in
// a DAW could reach every control and hear its name and value, and change none of
// them, until they found and ticked a menu item -- which is the very thing keyboard
// access exists to make reachable. So when a control HAS keyboard focus, the keys that
// operate it work whatever that checkbox says.
//
// Two deliberate limits keep this from being the shortcut layer by the back door:
//
//  * It runs the DEFAULT bindings, never the user's map. With the layer off there is
//    no map in force, and a fixed floor is something the manual can state plainly.
//  * TAB IS NOT IN IT. With our layer off, Tab belongs to JUCE's own focus traverser,
//    which is the standard behaviour a screen-reader user expects from a plugin; the
//    focus it moves brings the cursor with it (EditorView::globalFocusChanged).
//
// Everything else is unchanged: with no control focused this declines, and the key
// goes to the host exactly as it did before.
bool YamahaMU100AudioProcessorEditor::keyForFocusedControl(const juce::KeyPress& key)
{
    if (editorView == nullptr || ! showingEditor)
        return false;
    if (! editorView->navHasFocusedStop())
        return false;

    static constexpr int floorCommands[] =
    {
        kbsc::NavPrev,         kbsc::NavNext,
        kbsc::NavIncrease,     kbsc::NavDecrease,
        kbsc::NavIncreaseFine, kbsc::NavDecreaseFine,
        kbsc::NavIncreaseX,    kbsc::NavDecreaseX,
        kbsc::NavIncreaseXFine, kbsc::NavDecreaseXFine,
        kbsc::NavIncreaseQ,    kbsc::NavDecreaseQ,
        kbsc::NavIncreaseQFine, kbsc::NavDecreaseQFine,
        kbsc::NavActivate,     kbsc::NavReset,
    };

    for (const int cmd : floorCommands)
        if (kbsc::Bindings::defaultFor(cmd) == key)
            return performCommand(cmd);

    return false;
}

bool YamahaMU100AudioProcessorEditor::performCommand(int cmd)
{
    using SetupKind = YamahaMU100AudioProcessor::SetupKind;
    const bool standalone = juce::JUCEApplicationBase::isStandaloneApp();
    const bool haveEditor = editorView != nullptr;
    const bool inEditor   = haveEditor && showingEditor;

    switch (cmd)
    {
        case kbsc::FileNew:        newSetupClicked();  return true;
        case kbsc::FileOpen:       openFileClicked();  return true;
        case kbsc::FileSave:       if (! haveEditor) return false;
                                   saveSetupClicked(); return true;
        case kbsc::FileSaveAs:     if (! haveEditor) return false;
                                   saveSetupAsClicked(SetupKind::System); return true;
        case kbsc::FileExportSyx:  if (! haveEditor) return false;
                                   saveMultiSyxClicked(); return true;
        case kbsc::FileReadDevice: if (! haveEditor) return false;
                                   editorView->requestReadBack(); return true;
        case kbsc::FileExit:       if (! standalone) return false;
                                   exitStandaloneClicked(); return true;

        // Undo/redo decline when there is nothing on the stack, so an exhausted Ctrl+Z
        // reaches the host instead of dead-ending in the plugin.
        case kbsc::EditUndo:
            if (! inEditor || ! audioProcessor.canUndo()) return false;
            audioProcessor.undo(); afterUndoApply(); return true;
        case kbsc::EditRedo:
            if (! inEditor || ! audioProcessor.canRedo()) return false;
            audioProcessor.redo(); afterUndoApply(); return true;
        case kbsc::EditHistory:
            if (! inEditor) return false;
            toggleHistoryPanel(); return true;

        case kbsc::PartPrev:  if (! inEditor) return false;
                              editorView->stepSelectedPart(-1); return true;
        case kbsc::PartNext:  if (! inEditor) return false;
                              editorView->stepSelectedPart(+1); return true;
        case kbsc::PartPrev8: if (! inEditor) return false;
                              editorView->stepSelectedPart(-8); return true;
        case kbsc::PartNext8: if (! inEditor) return false;
                              editorView->stepSelectedPart(+8); return true;
        case kbsc::PartVoiceList:
            if (! inEditor) return false;
            editorView->openVoiceListForCurrentPart(); return true;

        // Copy / paste / reset of the selected part. Paste DECLINES on an empty
        // clipboard, so the chord reaches the host rather than dead-ending here -- the
        // same rule undo/redo follow on an exhausted stack.
        case kbsc::PartCopy:
            if (! inEditor || editorView->getCurrentPart() < 0) return false;
            copyPartClicked(editorView->getCurrentPart()); return true;
        case kbsc::PartPaste:
            if (! inEditor || editorView->getCurrentPart() < 0
                || ! audioProcessor.hasPartCopy()) return false;
            pastePartClicked(editorView->getCurrentPart()); return true;
        case kbsc::PartReset:
            if (! inEditor || editorView->getCurrentPart() < 0) return false;
            resetPartClicked(editorView->getCurrentPart()); return true;
        case kbsc::PartMenu:
            if (! inEditor) return false;
            editorView->requestPartContextMenu(); return true;

        // Keyboard control of the value boxes (KeyNav.h). Each returns false when it
        // has nothing to act on, which hands the key to the DAW rather than eating it
        // -- these are unmodified keys, so declining matters more here than anywhere.
        case kbsc::NavGroupNext:    if (! inEditor) return false;
                                    return editorView->navStepGroup(+1);
        case kbsc::NavGroupPrev:    if (! inEditor) return false;
                                    return editorView->navStepGroup(-1);
        case kbsc::NavPrev:         if (! inEditor) return false;
                                    return editorView->navStepItem(-1);
        case kbsc::NavNext:         if (! inEditor) return false;
                                    return editorView->navStepItem(+1);
        case kbsc::NavIncrease:     if (! inEditor) return false;
                                    return editorView->navAdjust(+1, false);
        case kbsc::NavDecrease:     if (! inEditor) return false;
                                    return editorView->navAdjust(-1, false);
        case kbsc::NavIncreaseFine: if (! inEditor) return false;
                                    return editorView->navAdjust(+1, true);
        case kbsc::NavDecreaseFine: if (! inEditor) return false;
                                    return editorView->navAdjust(-1, true);
        case kbsc::NavIncreaseX:     if (! inEditor) return false;
                                     return editorView->navAdjustX(+1, false);
        case kbsc::NavDecreaseX:     if (! inEditor) return false;
                                     return editorView->navAdjustX(-1, false);
        case kbsc::NavIncreaseXFine: if (! inEditor) return false;
                                     return editorView->navAdjustX(+1, true);
        case kbsc::NavDecreaseXFine: if (! inEditor) return false;
                                     return editorView->navAdjustX(-1, true);
        case kbsc::NavIncreaseQ:     if (! inEditor) return false;
                                     return editorView->navAdjustQ(+1, false);
        case kbsc::NavDecreaseQ:     if (! inEditor) return false;
                                     return editorView->navAdjustQ(-1, false);
        case kbsc::NavIncreaseQFine: if (! inEditor) return false;
                                     return editorView->navAdjustQ(+1, true);
        case kbsc::NavDecreaseQFine: if (! inEditor) return false;
                                     return editorView->navAdjustQ(-1, true);
        case kbsc::NavActivate:     if (! inEditor) return false;
                                    return editorView->navActivate();
        case kbsc::NavActivateAlt:  if (! inEditor) return false;
                                    return editorView->navActivateAlt();
        case kbsc::NavReset:        if (! inEditor) return false;
                                    return editorView->navReset();

        case kbsc::MixMute:          if (! inEditor) return false;
                                     editorView->toggleMuteSolo(false, false); return true;
        case kbsc::MixSolo:          if (! inEditor) return false;
                                     editorView->toggleMuteSolo(true,  false); return true;
        case kbsc::MixMuteExclusive: if (! inEditor) return false;
                                     editorView->toggleMuteSolo(false, true);  return true;
        case kbsc::MixSoloExclusive: if (! inEditor) return false;
                                     editorView->toggleMuteSolo(true,  true);  return true;
        // Toggles: the SELECTED part, or every part at once. The all-parts form asks
        // the same question the badge does -- if anything is bypassed, clear it all;
        // otherwise bypass everything -- so one key both engages and gets you out.
        case kbsc::MixEqBypass:
            if (! inEditor) return false;
            audioProcessor.setPartEqBypass(editorView->getCurrentPart(),
                                           ! audioProcessor.isPartEqBypassed(editorView->getCurrentPart()));
            return true;
        case kbsc::MixEqBypassAll:
            if (! inEditor) return false;
            audioProcessor.setPartEqBypassMask(audioProcessor.getPartEqBypassMask() != 0 ? 0u : 0xffffffffu);
            return true;

        case kbsc::MixClear:
            // Deliberately NOT gated on the view: the bottom-bar badge shows the state
            // over the photo too, and this is the one action that gets you out of it.
            audioProcessor.setPartSoloMask(0);
            audioProcessor.setPartMuteMask(0);
            audioProcessor.setPartEqBypassMask(0);
            return true;

        case kbsc::ViewToggle:  setView(! showingEditor);  return true;
        case kbsc::ViewZoomIn:    zoomWindow(kZoomStep);        return true;
        case kbsc::ViewZoomOut:   zoomWindow(1.0f / kZoomStep); return true;
        case kbsc::ViewZoomReset: resetWindowSize();            return true;
        case kbsc::DevicePanic: panicClicked(); return true;

        case kbsc::OptionsShortcuts: showShortcutEditor(); return true;
        case kbsc::HelpManual:    openUserManual();     return true;
        case kbsc::HelpAbout:     showAboutDialog();    return true;

        default: break;
    }

    // The nine Display* commands are one contiguous run in GraphicDisplayPanel::Mode
    // order -- see the note on the enum in KeyboardShortcuts.h.
    if (cmd >= kbsc::DisplayVolume && cmd <= kbsc::DisplayHPF2)
    {
        if (! inEditor) return false;
        editorView->setDisplayMode(cmd - kbsc::DisplayVolume);
        return true;
    }

    return false;
}

juce::String YamahaMU100AudioProcessorEditor::shortcutText(int cmd) const
{
    return audioProcessor.keyBindings.descriptionFor(cmd, audioProcessor.useKeyboardShortcuts);
}

juce::String YamahaMU100AudioProcessorEditor::withShortcut(const juce::String& text,
                                                           int cmd) const
{
    const auto k = shortcutText(cmd);
    return k.isEmpty() ? text : text + "  (" + k + ")";
}

// The bottom-bar buttons have no menu row to carry a shortcut, so they carry it in
// their tooltip instead. Called from the ctor and whenever the option or the map
// changes; the undo/redo pair is left to refreshUndoButtons, which composes its own
// text from the step being undone.
void YamahaMU100AudioProcessorEditor::applyFocusMarkMode()
{
    focusMarkApplied = audioProcessor.focusMarkMode;
    if (editorView != nullptr) editorView->setFocusMarkMode(focusMarkApplied);
}

void YamahaMU100AudioProcessorEditor::applyPartTitleNames()
{
    partTitleNamesApplied = audioProcessor.partTitleNames;
    if (editorView != nullptr) editorView->setPartTitleNames(partTitleNamesApplied);
}

// Same shape as applyChangedMarks: a global a panel reads at paint time, so the global
// moves first and the view is then asked to refresh. Pushed even with no editor view
// built — the global is what a view constructed later will read, and one surface (the
// override badge's tooltip) is composed in the processor with no view at all.
void YamahaMU100AudioProcessorEditor::applyNoteLabels()
{
    noteLabelsApplied = audioProcessor.noteLabelMode;
    notelabel::setMode(audioProcessor.noteLabelMode);
    if (editorView != nullptr) editorView->refreshNoteLabels();
}

void YamahaMU100AudioProcessorEditor::refreshShortcutTooltips()
{
    // The Help BUTTON has no shortcut of its own; the two items behind it do, and
    // they already show their chords in the menu. Its tooltip names both so the pair
    // is discoverable without opening it -- composed here rather than through
    // withShortcut(), which appends ONE chord and would read as a single ambiguous
    // pair of parentheses with two chords in it.
    {
        const auto a = shortcutText(kbsc::HelpAbout);
        const auto m = shortcutText(kbsc::HelpManual);
        juce::String tip = "About MEGAN XG";
        if (a.isNotEmpty()) tip += " (" + a + ")";
        tip += " and the manual";
        if (m.isNotEmpty()) tip += " (" + m + ")";
        helpButton.setTooltip(tip);
    }
    panicButton.setTooltip(withShortcut("Stop all notes and release the pedals",
                                        kbsc::DevicePanic));
    refreshUndoButtons();   // its tooltips carry the undo/redo chords
}

//==============================================================================
// The Help menu, behind the bottom bar's third button.
//
// Two items, and they are the two the shortcut editor already groups under Help.
// This replaced a bare "About" button: the manual needed somewhere visible to live,
// and Options is a settings menu, not a place to keep a document.
void YamahaMU100AudioProcessorEditor::showHelpMenu()
{
    juce::PopupMenu menu;

    auto item = [this](int id, const juce::String& text, int cmd)
    {
        juce::PopupMenu::Item i;
        i.itemID = id;
        i.text   = text;
        i.shortcutKeyDescription = shortcutText(cmd);
        return i;
    };

    menu.addItem(item(1, "MEGAN XG Manual...", kbsc::HelpManual));
    menu.addSeparator();
    menu.addItem(item(2, "About MEGAN XG",     kbsc::HelpAbout));

    menu.setLookAndFeel(&lookAndFeel);   // a top-level popup does not inherit one
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(helpButton),
                       [this](int result)
    {
        if      (result == 1) openUserManual();
        else if (result == 2) showAboutDialog();
    });
}

//==============================================================================
// Keyboard focus, and why this is a poll rather than an event.
//
// A shortcut only fires if some component in THIS editor's tree holds JUCE keyboard
// focus. Clicking inside the editor does that; clicking the plugin window's TITLE BAR
// does not, because in a DAW that bar belongs to the HOST's window frame -- the OS
// focuses the host window, JUCE never routes anything to us, and the shortcuts look
// dead until you click the GUI. Closing one of our own dialogs has the same shape:
// focus goes back to the host, not to the editor the dialog came from.
//
// There is no single event that covers both. A plugin editor is not a TopLevelWindow,
// so activeWindowStatusChanged() never arrives; broughtToFront() does not fire for a
// host-frame click; and focusGained() cannot fire on a component that is not being
// given focus in the first place. What is reliable is the question itself: does OUR
// PEER have the keyboard, and does nothing inside us hold focus? Asked off the 30 Hz
// timer that is already running, it costs two pointer tests.
//
// The rules that keep this from stealing anything:
//  * Only while shortcuts are switched on. With the option off this never runs, so it
//    cannot change behaviour for anyone who has not asked for shortcuts.
//  * Only when our peer is ALREADY focused. We never take the keyboard from the host
//    or from another window -- we only settle it inside a window that has it.
//  * Never over a modal component, and never when anything in our tree (a ValueBox's
//    TextEditor, the Voice List) already has focus.
//==============================================================================
// Zoom = resize the window, because that is the only thing zoom can mean here: the
// whole editor is ONE canvas scaled to fit, so the window size IS the zoom level. It
// is also the lever that can never clip anything, unlike the text-size percentages in
// settings.xml, which grow the text without growing the boxes it sits in.
//
// setBoundsConstrained applies whatever the current view installed in setView(), so
// the limits and the Hardware view's fixed photo aspect are honoured for free. Landing
// on a rail still counts as handled -- returning false would hand Ctrl+= to the DAW
// and zoom its arrangement instead, which is a strange thing to happen at maximum zoom.
void YamahaMU100AudioProcessorEditor::zoomWindow(float factor)
{
    setBoundsConstrained(getBounds().withSize(juce::roundToInt(getWidth()  * factor),
                                              juce::roundToInt(getHeight() * factor)));
}

// Back to the size a fresh install opens at. The Editor view's pair is the processor's
// own settings default, shared rather than copied so the two cannot drift; the Hardware
// view's is the photo scaled to 1200 px, captured in the constructor.
void YamahaMU100AudioProcessorEditor::resetWindowSize()
{
    if (showingEditor)
        setBoundsConstrained(getBounds().withSize(
            YamahaMU100AudioProcessor::kDefaultEditorViewW,
            YamahaMU100AudioProcessor::kDefaultEditorViewH));
    else if (defaultHwW > 0 && defaultHwH > 0)
        setBoundsConstrained(getBounds().withSize(defaultHwW, defaultHwH));
}

// Take the keyboard back after one of OUR surfaces gives it up -- a dialog closing,
// the Voice List hiding, the History window closing.
//
// This is the half that CANNOT be a poll, and the reason is JUCE's key routing:
// ComponentPeer::getTargetForKeyPress falls back to the PEER'S OWN top component when
// nothing is focused, and handleKeyPress then walks UP the parent chain from there. In
// a plugin the peer's component is the host wrapper, and this editor is BELOW it -- so
// with JUCE focused on nothing, our keyPressed is never called at all and every key
// goes to the DAW. Hiding a focused component leaves exactly that state.
//
// It is safe to grab here, and only here, because each caller is a moment where the
// user just finished with something of ours: the window legitimately holds the
// keyboard. The generic poll below must stay guarded on peer->isFocused(), or it would
// pull Win32 focus out of the host at 30 Hz while someone types in a track name.
void YamahaMU100AudioProcessorEditor::restoreKeyboardFocus(bool alsoBringWindowToFront)
{
    if (! audioProcessor.useKeyboardShortcuts)
        return;
    if (juce::Component::getCurrentlyModalComponent() != nullptr)
        return;

    // Only for a real WINDOW closing. An in-window overlay changes no window ordering,
    // and asking the host to re-front its own plugin window is not free in every host.
    if (alsoBringWindowToFront)
        if (auto* top = getTopLevelComponent())
            top->toFront(true);

    grabKeyboardFocus();
}

void YamahaMU100AudioProcessorEditor::serviceKeyboardFocus()
{
    if (! audioProcessor.useKeyboardShortcuts)
        return;

    // A dialog of ours just closed: put the keyboard back where it came from rather
    // than leaving it with the host. Checked before the peer test below, because the
    // whole point is that our window does NOT have focus at this moment. Every dialog
    // this editor opens goes through themeDialog(), which is what makes ONE test here
    // cover About, Audio, Display Animation, the MIDI windows and the shortcut editor.
    if (dialogWasOpen && openDialog == nullptr)
    {
        dialogWasOpen = false;
        restoreKeyboardFocus(/*alsoBringWindowToFront*/ true);
        return;
    }
    dialogWasOpen = (openDialog != nullptr);

    if (juce::Component::getCurrentlyModalComponent() != nullptr)
        return;

    auto* peer = getPeer();
    if (peer == nullptr)
        return;

   #if JUCE_WINDOWS
    // Clicking the plugin window's TITLE BAR activates the HOST's frame without moving
    // Win32 focus into our child window, so no key event is generated for JUCE to route
    // and nothing below this could help. Claim it at the Win32 level instead; see
    // PluginFocusWin.h for the ancestor-vs-sibling rule that keeps this off the host's
    // own header controls.
    //
    // Bounded to kMaxFocusClaims tries per activation so that a host which insists on
    // keeping focus is argued with for ~130 ms and then left alone, rather than fought
    // 30 times a second for the life of the session.
    {
        void* activeNow = mu100focus::activeWindow();
        if (activeNow != lastActiveWindow)
        {
            lastActiveWindow = activeNow;
            focusClaims      = 0;
        }

        if (peer->isFocused())
            focusClaims = 0;
        else if (focusClaims < kMaxFocusClaims
                 && mu100focus::canClaimFocus(peer->getNativeHandle()))
        {
            ++focusClaims;
            mu100focus::claimFocus(peer->getNativeHandle());
        }
    }
   #endif

    if (! peer->isFocused())
        return;

    if (! hasKeyboardFocus(/*trueIfChildIsFocused*/ true))
        grabKeyboardFocus();
}

//==============================================================================
// Options -> "Edit keyboard shortcuts..." (and the Alt+B command).
void YamahaMU100AudioProcessorEditor::showShortcutEditor()
{
    // A second invocation brings the open window forward rather than stacking another
    // copy -- two editors of one map would each save over the other on OK.
    if (shortcutWindow != nullptr)
    {
        shortcutWindow->toFront(true);
        return;
    }

    auto* content = new ShortcutEditorComponent(
        audioProcessor.keyBindings,
        [this](const kbsc::Bindings& edited)
        {
            audioProcessor.keyBindings = edited;
            audioProcessor.saveSettings();
            refreshShortcutTooltips();   // the chords in the tooltips just moved
        });

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle              = "Keyboard Shortcut Editor";
    opts.dialogBackgroundColour   = edcol::kDialogBg;
    opts.content.setOwned(content);
    opts.componentToCentreAround  = this;
    opts.resizable                = true;
    opts.useNativeTitleBar        = true;
    // Escape must NOT close it: while a row is learning, Escape cancels the learn, and a
    // window that also vanished on the same key would throw away every other edit.
    opts.escapeKeyTriggersCloseButton = false;

    shortcutWindow = themeDialog(opts.launchAsync());
    if (shortcutWindow != nullptr)
        shortcutWindow->setResizeLimits(520, 300, 1200, 1400);
}

//==============================================================================
// F1 / Options -> "MEGAN XG Manual...".
//
// Local copy first, the hosted copy as a fallback. The local one is what a user with no
// network connection has, and it is the copy that matches THIS build; the URL is what an
// installation that never copied the manual across still gets.
void YamahaMU100AudioProcessorEditor::openUserManual()
{
    const juce::File local = juce::File::getSpecialLocation(
        juce::File::userDocumentsDirectory)
            .getChildFile("MEGAN XG/manual/MEGAN-XG-Manual.html");

    if (local.existsAsFile())
    {
        local.startAsProcess();   // opens in the default browser
        return;
    }

    juce::URL(kManualUrl).launchInDefaultBrowser();
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::showAboutDialog()
{
    auto* content = new AboutComponent();

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle              = "About MEGAN XG";
    opts.dialogBackgroundColour   = edcol::kDialogBg;
    opts.content.setOwned(content);
    opts.componentToCentreAround  = this;
    opts.resizable                = false;
    opts.useNativeTitleBar        = true;
    themeDialog(opts.launchAsync());
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::showMidiRoutingDialog()
{
    auto* content = new MidiRoutingComponent(audioProcessor);

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle              = "MIDI Input for MEGAN XG";
    opts.dialogBackgroundColour   = edcol::kDialogBg;
    opts.content.setOwned(content);
    opts.componentToCentreAround  = this;
    opts.resizable                = false;
    opts.useNativeTitleBar        = true;
    themeDialog(opts.launchAsync());
}

//==============================================================================
void YamahaMU100AudioProcessorEditor::showMidiOutputDialog()
{
    auto* content = new MidiOutputRoutingComponent(audioProcessor);

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle              = "MIDI Output for MEGAN XG";
    opts.dialogBackgroundColour   = edcol::kDialogBg;
    opts.content.setOwned(content);
    opts.componentToCentreAround  = this;
    opts.resizable                = false;
    opts.useNativeTitleBar        = true;
    themeDialog(opts.launchAsync());
}
