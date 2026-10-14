// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EditorView.h"
#include "EditorColours.h"
#include "EditorTextScale.h"
#include "UndoArrowButton.h"
#include "HistoryPanel.h"
#include "DrumBufferChooser.h"
#include "KeyboardShortcuts.h"

//==============================================================================
// FilePathDisplay — bottom-strip label showing the path/name of the .syx file the
// editor model was last loaded-from or saved-to, with a trailing '*' (in the editor's
// "changed" yellow) when the model has unsaved edits.
//
// It doubles as the **bottom-bar tooltip readout** (Options > Tooltips > "Bottom
// bar"): while a tooltip is showing it replaces the filename, so the two never
// compete for the strip. Both are LEFT-justified from the same x — the point of the
// bottom-bar mode is that the eye always starts reading in one spot — which is why
// this component is positioned a couple of button widths right of the undo/redo trio
// rather than filling the strip (see PluginEditor::resized).
//
// The two texts ellipsize in OPPOSITE directions, and that is deliberate: a path
// keeps its TAIL (the filename is what identifies it), a tooltip keeps its HEAD (the
// sentence is what identifies it). The full path is also available as this
// component's own tooltip.
//
// The font rides editorTextLabelsPct (edtext::Label) like the editor's control
// labels, so the strip scales with the rest of the text. Base height is picked so
// 100% renders the tuned size (13 px + the 1 px this feature added = 14).
//==============================================================================
class FilePathDisplay : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    FilePathDisplay() = default;

    // 14 px at editorTextLabelsPct = 100 (edtext applies a 1.30 baseline).
    static constexpr float kBaseFontPx = 14.0f / 1.30f;

    void setState(const juce::String& fullPath, bool dirty)
    {
        if (fullPath == path && dirty == isDirty)
            return;
        path    = fullPath;
        isDirty = dirty;
        setTooltip(path.isEmpty() ? juce::String()
                                  : path + (isDirty ? juce::String(" (unsaved changes)")
                                                    : juce::String()));
        repaint();
    }

    // The bottom-bar tooltip text, or empty for "show the filename again". Driven by
    // PluginEditor::serviceBottomBarTooltip(); empty in every other tooltip mode.
    void setTooltipOverride(const juce::String& text)
    {
        if (text == tipText)
            return;
        tipText = text;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const juce::Font font(edtext::font(edtext::Label, kBaseFontPx));
        g.setFont(font);
        const juce::String ell(juce::CharPointer_UTF8("\xe2\x80\xa6"));   // horizontal ellipsis

        // Tooltip: left-justified, RIGHT-ellipsized (keep the start of the sentence).
        if (tipText.isNotEmpty())
        {
            juce::String text = tipText;
            if (juce::GlyphArrangement::getStringWidthInt(font, text) > getWidth()
                && text.length() > 1)
            {
                while (text.length() > 1
                       && juce::GlyphArrangement::getStringWidthInt(font, text + ell) > getWidth())
                    text = text.dropLastCharacters(1);
                text = text + ell;
            }
            g.setColour(edcol::kTextLabel);
            g.drawText(text, 0, 0, getWidth(), getHeight(), juce::Justification::centredLeft);
            return;
        }

        const juce::String shown = path.isNotEmpty() ? path
                                                     : juce::String("(no file loaded)");
        const juce::String star  = isDirty ? juce::String(" *") : juce::String();
        const int starW    = juce::GlyphArrangement::getStringWidthInt(font, star);
        const int pathAvail = juce::jmax(0, getWidth() - starW);

        // Left-ellipsize the path so the filename stays visible when it overflows.
        juce::String text = shown;
        if (juce::GlyphArrangement::getStringWidthInt(font, text) > pathAvail
            && text.length() > 1)
        {
            while (text.length() > 1
                   && juce::GlyphArrangement::getStringWidthInt(font, ell + text) > pathAvail)
                text = text.substring(1);
            text = ell + text;
        }

        // Path in a dim grey; the '*' in the editor's "changed" yellow so it reads as the
        // same "modified" language as the panel change markers. The leading space in
        // " *" holds the gap between the name and the star. The star follows the text
        // rather than sitting at the right edge, now that the text is left-justified.
        const int textW = juce::GlyphArrangement::getStringWidthInt(font, text);
        g.setColour(path.isNotEmpty() ? edcol::kTextDim : edcol::kTextFaint);
        g.drawText(text, 0, 0, pathAvail, getHeight(), juce::Justification::centredLeft);
        if (isDirty)
        {
            g.setColour(edcol::kChanged);
            g.drawText(star, juce::jmin(textW, pathAvail), 0, starW, getHeight(),
                       juce::Justification::centredLeft);
        }
    }

private:
    juce::String path;
    bool         isDirty { false };
    juce::String tipText;   // non-empty = a bottom-bar tooltip is showing

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FilePathDisplay)
};

//==============================================================================

class YamahaMU100AudioProcessorEditor : public juce::AudioProcessorEditor,
                                        private juce::Timer
{
public:
    explicit YamahaMU100AudioProcessorEditor(YamahaMU100AudioProcessor&);
    ~YamahaMU100AudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

    // Keyboard shortcuts. This is the ONLY key handler in the plugin outside the Voice
    // List, and it is deliberately the LAST thing to see a key: JUCE walks the focused
    // component up its parent chain, so a text field, a ValueBox or the browser has
    // already had its chance by the time a press arrives here.
    //
    // It returns FALSE for anything it does not own, which is the rule the whole feature
    // rests on -- an unhandled key bubbles on out to the host, so the DAW's transport and
    // its own shortcuts keep working while the plugin window has focus.
    bool keyPressed(const juce::KeyPress&) override;
    // The ACCESSIBILITY KEY FLOOR -- the keys that operate a FOCUSED control even
    // when the shortcut layer is switched off. See its definition.
    bool keyForFocusedControl(const juce::KeyPress&);

private:
    void timerCallback() override;
    juce::String muteSoloSummary() const;
    juce::String eqBypassSummary() const;
    void panicClicked();

    // Accessibility pillar 4 -- announcements. The queue is shared per process (speech
    // is one resource however many plugin instances are open); the watcher speaks the
    // FOCUSED control's value when something moves it. Both do nothing at all unless a
    // screen reader is listening. See Accessibility.h.
    mu100a11y::announce::Shared            announcer;
    mu100a11y::announce::FocusValueWatcher valueWatcher;
    void say(mu100a11y::announce::Channel ch, const juce::String& text)
    {
        announcer->say(ch, text);
    }

    void pressButton(int idx);
    void releaseButton(int idx);
    void applyTooltipEnabled();   // push audioProcessor.tooltipMode to tooltipWindow
    void applyFocusMarkMode();    // push audioProcessor.focusMarkMode to editorView
    void applyPartTitleNames();   // push audioProcessor.partTitleNames to editorView
    void applyNoteLabels();       // push audioProcessor.noteLabelMode to the global + view
    int  focusMarkApplied { -1 }; // ...and what was last pushed (see timerCallback)
    int  partTitleNamesApplied { -1 };   // ...same, for the Part module title naming
    int  noteLabelsApplied     { -1 };   // ...same, for Options > Label notes as
    void serviceBottomBarTooltip();  // bottom-bar mode: hover -> filePathDisplay
    void applyPluginOnlyLock();   // push audioProcessor.enablePluginOnlyParams to editorView
    void applyRcvRowVisible();    // push audioProcessor.showPartRcvSwitches to editorView
    void applyMuteSoloCols();
    void applyMidiIndicators();   // start/stop PartListPanel's indicator fade timer     // push audioProcessor.showMuteSoloColumns to editorView
    static constexpr int kMsIndicatorW = 150;   // widest label: "SOLO x2  MUTE x12"
    static constexpr int kOvIndicatorW = 132;   // widest label: "32 MIDI OVERRIDES"
    static constexpr int kMoIndicatorW = 104;   // "MIDI OUT: CC"
    void applyVoiceNames();       // push audioProcessor.shortVoiceNames to voicenames + editorView
    void applyChangedMarks();     // push audioProcessor.showChangedMarks to changedmarks + editorView

    // --- The part menu (copy / paste / reset) ------------------------------------
    // Opened by a right-click on a Voice row or the bar screens' part ruler, and by the
    // Part menu shortcut. See the .cpp for why those two surfaces and no others.
    void showPartContextMenu(int part);
    void copyPartClicked(int part);
    void pastePartClicked(int part);    // asks which buffer first where one is involved
    void doPastePart(int part, int targetBuffer);
    void resetPartClicked(int part);    // warns first where a buffer is re-initialised
    void doResetPart(int part);
    // The drum-buffer chooser, for any action that writes a buffer: a .mps/.mds load or
    // a part paste. The pending action is a CALLBACK, so the two share one component.
    void askDrumBuffer(const juce::String& what, const juce::String& verb, int savedBuffer,
                       std::function<void(int)> onChosen,
                       const juce::String& savedRow = "Buffer File Was Saved With");
    std::function<void(int)> pendingBufferChoice;
    void applyAutomationMarkers();// push the DAW + MIDI CC mark settings to editorView
    void applyMidiMarkStyle();    // push audioProcessor.midiOverrideMarkStyle to editorView
    void refreshUndoButtons();    // undo/redo enable state + their dynamic tooltips
    void rememberViewSize();      // file the current size under the view that is showing

    YamahaMU100AudioProcessor& audioProcessor;

    juce::Image bgImage;
    float imgScale { 1.0f };

    // LCD snapshot (local copy from processor, updated in timer)
    uint8_t lcdSnapshot[YamahaMU100AudioProcessor::LCD_BUF_SIZE] {};

    // Bottom bar buttons.
    //
    // NONE of these takes keyboard focus (set in the ctor). A focused juce::Button
    // triggers a CLICK on the return key -- and Button::keyPressed tests
    // isKeyCode(returnKey) alone, so it fires on Alt+Return as well and the chord
    // never reaches the shortcut handler. Whichever of these was clicked last would
    // then decide what Alt+Return did. Same rule the Voice List already applies to
    // its own HIDE and DRUMS buttons.
    juce::TextButton fileButton       { "File" };
    juce::TextButton settingsButton   { "Options" };
    // "Help" replaced a plain "About" button on 2026-08-31: the manual needed a home,
    // and hanging it off Options put a document under a settings menu. About and the
    // manual are the two Help items, which is also how the shortcut editor groups them.
    juce::TextButton helpButton       { "Help" };
    juce::TextButton panicButton      { "PANIC" };   // MIDI all-notes-off (moved here from System module)

    // Standing mute/solo indicator, at the RIGHT end of the strip. Visible only while
    // something is engaged, and clicking it clears everything.
    //
    // It is not a convenience. Mute and solo are the only two controls in this plugin
    // that make it silent with nothing on screen necessarily saying so: the M/S
    // columns can be switched off in Options, the Voice list can be behind the
    // Hardware view, and a soloed part looks exactly like a working plugin from every
    // other module. This is the one cue that survives all of that -- so it is a filled
    // AREA rather than coloured text, and it lives outside the Voice module on
    // purpose.
    juce::TextButton muteSoloIndicator;
    bool msIndicatorShown { false };          // drives the strip's re-layout
    void refreshMuteSoloIndicator();          // label + colour + visibility
    // Standing count of what incoming MIDI has moved on the device. Same shape as
    // the mute/solo badge above and for the same reason: it is live state you can
    // leave engaged without noticing, and one click clears it.
    juce::TextButton overrideIndicator;
    bool ovIndicatorShown { false };
    void refreshOverrideIndicator();
    // "MIDI OUT: CC" -- standing while Options > MIDI Out sends edits as is on MIDI CC.
    // Same family as the two badges above: a setting you can leave on without
    // noticing, whose cost is invisible -- in CC mode most edits record NOTHING, and
    // nothing else on screen says the mode is on. A filled area, not coloured text.
    // Clicking switches back to XG SysEx, the "click to clear" its neighbours share.
    juce::TextButton midiOutIndicator;
    bool moIndicatorShown { false };
    void refreshMidiOutBadge();
    // The Hardware/Editor toggle that used to sit between About and PANIC is gone:
    // the view is chosen from Options -> View, which also persists it.

    // Undo / Redo for editor parameter edits (see the processor's undo API). Square
    // icon buttons rather than another 90 px text slot: the strip's remaining width
    // belongs to the file display, which is already tight at the minimum window size.
    // Children of THIS editor, like PANIC, so they show over both views — the history
    // lives in the processor and stays valid whichever view is up.
    UndoArrowButton undoButton    { UndoArrowButton::Undo };
    UndoArrowButton historyButton { UndoArrowButton::History };
    UndoArrowButton redoButton    { UndoArrowButton::Redo };
    int undoPollTick { 0 };   // throttle: refresh enable state + tooltips every Nth tick

    // The HISTORY window the hamburger opens. Created lazily, like drumBufferChooser,
    // and a child of THIS editor so it can hang over either view.
    std::unique_ptr<HistoryPanel> historyPanel;
    void toggleHistoryPanel();
    // Shared tail of undo / redo / a History click: re-read the model into the panels
    // and refresh the strip. Named because three call sites is where a copied lambda
    // starts to drift.
    void afterUndoApply();

    // Bottom-strip loaded-file display (right side): path/name of the current .syx +
    // an unsaved-changes '*'. Child of this editor (not EditorView) so it shows over
    // both views. Refreshed (throttled) from the timer via the processor's marker state.
    FilePathDisplay filePathDisplay;
    int  dirtyPollTick { 0 };   // throttle: recompute the signature every Nth timer tick

    // Enables the full-path tooltip on filePathDisplay (parented to this editor so it
    // stays inside the plugin window rather than spawning a desktop window).
    juce::TooltipWindow tooltipWindow { this };
    int tooltipModeApplied { -1 };   // last mode pushed to tooltipWindow (-1 = none yet)

    // "Bottom bar" tooltip mode: we do the hover detection ourselves (TooltipWindow
    // is muted in that mode) and paint the text into filePathDisplay. Same shape as
    // TooltipWindow::timerCallback — component under the mouse, a quick-move reset,
    // then a delay — driven off the editor's existing 30 Hz timer.
    juce::Component::SafePointer<juce::Component> tipLastComp;
    juce::String     tipLastText;
    juce::Point<int> tipLastMousePos;
    juce::uint32     tipChangeMs { 0 };
    bool             tipShownInBar { false };
    // Far shorter than the pop-up's 700 ms: the bar is a fixed spot that obscures
    // nothing, so appearing early costs nothing (a pop-up under the cursor would be
    // in the way). Matches GraphicDisplayPanel's kHoverDelayMs, the editor's other
    // hover readout that lands somewhere the cursor isn't.
    static constexpr int kBottomBarTipDelayMs = 150;
    // Same pattern for the EQ analyser's on/off: the options windows only write the
    // processor flag, and the analyser can re-read it itself ONLY while its timer is
    // running — so the off->on direction needs this nudge. Seeded from the processor
    // so a session starting with it off doesn't fire a spurious refresh.
    bool eqSpectrumApplied { true };
    // ...and for the VOLUME screen's animated level meters, which have exactly the
    // same asymmetry: off the panel notices by itself, on it cannot (its tick is not
    // running to look). Seeded from the processor in the ctor.
    bool volumeMetersApplied { false };
    // ...and the meter colour ramp. An index rather than a bool, but the same job:
    // carry a processor-side choice into the live palette + a repaint.
    int volumeMeterRampApplied { 0 };
    // Whether EditorView was showing at the last tick. JUCE notifies only the
    // component whose OWN visibility moved, and isShowing() also depends on the
    // window having a peer — which it does not during this editor's constructor. See
    // the note at the use site in timerCallback().
    bool editorViewShowing { false };
    // Same pattern again for "Enable plugin-only parameters": both hosts' Options UI
    // writes only the processor flag, so the timer carries it to EditorView's panels.
    // Holds the OPTION's sense (on = available), not the panels' lock; seeded in the ctor.
    bool pluginOnlyApplied { true };
    // ...and for "Show part MIDI receive switches". Seeded in the ctor, like the pair
    // above, so a first timer tick can't mistake the default for a user change.
    bool rcvSwitchesApplied { true };
    // ...and for "Show Mute and Solo columns".
    bool muteSoloColsApplied { true };
    // ...and for "MIDI input indicators". Seeded from the processor default so the
    // first tick does not read the default as a change.
    bool midiIndicatorsApplied { true };
    // ...and for "Voice names" (full vs the hardware's 8-character form).
    bool voiceNamesApplied { false };
    // ...and for "Show changed markers". Seeded in the ctor, so a first tick cannot
    // read the default as a user change.
    bool changedMarksApplied { true };
    // ...and for "Mark DAW-automatable params" (VST Options only; the standalone has
    // no such option and EditorView forces the markers off there anyway).
    int  automationMarkerApplied { -1 };   // -1 = nothing pushed yet
    // What the marks depend on, as one comparable value: the DAW mark's toggle, the
    // MIDI CC mark's toggle, and whether MIDI Out is in CC mode (the CC mark's
    // precondition). Any of the three moving re-runs refreshAutomationMarkers.
    int  markerSettingsSig() const
    {
        return (audioProcessor.markDawAutomatable ? 1 : 0)
             | (audioProcessor.markMidiCcParams   ? 2 : 0)
             | (audioProcessor.midiOutFormat == YamahaMU100AudioProcessor::MidiOutCC ? 4 : 0);
    }
    // ...and for the MIDI-override mark style, which the Display Animation window
    // writes as a bare flag. Required rather than convenient: that window is a
    // separate dialog and the module it re-lays-out lives under editorView.
    int  midiMarkStyleApplied { -1 };      // -1 = nothing pushed yet
    // ...and "Enable DAW automation", which the markers follow (see EditorView's
    // refreshAutomationMarkers). Seeded in the ctor beside the marker mode.
    bool dawAutomationApplied { true };

    // --- Hardware-view repaint gating (see timerCallback) ---
    // The 30 Hz tick used to end in an unconditional repaint(). That dates from
    // 388c15c, when this class WAS the whole GUI and the LCD was the only thing on
    // screen that moved; it survived the Editor view's arrival (42c7318) unexamined,
    // and by then it was invalidating the entire component tree -- EditorView and
    // every panel under it -- thirty times a second to animate a dot-matrix panel
    // that is not even visible in that view.
    //
    // Everything paint() draws now names itself here instead. In the Editor view it
    // draws nothing but the background fill behind the control strip, so there is
    // nothing to poll at all; the panels under EditorView have always repainted
    // themselves (their own timers, value changes, the meter/spectrum frames).
    // hwRepaintPending forces one frame through after a view swap, because the
    // photo's own pixels have to come back whatever these comparators say.
    uint8_t       hwLedsSeen       { 0 };
    bool          hwRomMissingSeen { false };
    bool          hwBootedSeen     { false };
    unsigned long hwFaultSeen      { 0 };   // matches mameFaultCode's type
    bool          hwRepaintPending { true };   // first tick always paints

    // --- Swappable Hardware/Editor views ---
    // The container hosts the photo "Hardware" panel (painted by this class) and
    // an "Editor" view (EditorView), showing one at a time. Each view remembers
    // its own window size, so toggling resizes to that view's last dimensions.
    std::unique_ptr<EditorView> editorView;
    bool  showingEditor { false };
    bool  viewsReady    { false };   // gate size-recording until ctor layout is done
    bool  swappingView  { false };   // true for the duration of setView(): suppresses the
                                     // per-view size recording while the component still
                                     // holds the OTHER view's bounds (see setView)
    bool  viewInitDone  { false };   // set at the END of the ctor: a setView() past this
                                     // point is the user navigating, not the window
                                     // opening (gates the stale-model warning)
    int   hwW { 0 }, hwH { 0 };      // remembered Hardware-view size
    // The compiled-in Hardware size, kept because hwW/hwH become "wherever the user
    // left it" as soon as anything resizes. What "reset window size" returns to.
    int   defaultHwW { 0 }, defaultHwH { 0 };
    int   edW { 1200 }, edH { 862 }; // remembered Editor-view size (≈ XGEdit canvas aspect)
    float hwAspect { 1.0f };

    // Editor-size restore/capture state machine (see timerCallback). On open the
    // persisted size (audioProcessor.editorViewW/H) is re-applied on EVERY tick
    // until it sticks: standalone (JUCE windowState restore) and several DAWs
    // resize the editor to their own size shortly AFTER attach, defeating any
    // one-shot restore. We read the persisted fields directly (not edW/edH, which
    // resized() clobbers to whatever the host just imposed). Once the on-screen
    // size has matched the target for a few ticks we mark restore done and switch
    // to debounced capture of genuine user resizes back into the persisted fields.
    bool  restoreDone    { false };
    int   restoreOkTicks { 0 };   // consecutive ticks already at target
    int   restoreAttempts{ 0 };   // give-up guard if the host refuses our size
    int   capW { -1 }, capH { -1 }, capTicks { 0 };  // debounced capture

    // Switch views. Also PERSISTS the choice (defaultEditorView), because Options ->
    // View is now the only way to change it — the view on screen and the view the
    // plugin reopens in are one setting.
    void setView(bool showEditor);

    // Zoom in / out / back to default. Zoom IS the window size here -- the editor is one
    // canvas scaled to fit -- so these resize the window through the constrainer the
    // current view installed.
    static constexpr float kZoomStep = 1.05f;   // 5% a step (was 10%, too coarse)
    void zoomWindow(float factor);
    void resetWindowSize();

    // --- Editor theme (Options -> View -> Dark Editor / Light Editor) ------------
    // Install audioProcessor.editorTheme into edcol + the LookAndFeel, and REBUILD
    // EditorView so constructor-time colours (every juce::Label's text colour) are
    // taken from the new palette. The rebuild is why this is not just a repaint:
    // walking the tree could only fix the colours drawn in paint(), and would leave
    // every Label wearing the old theme.
    //
    // The Hardware view is a photograph of a front panel and has no palette, so the
    // light theme is only installed while the editor view is on screen; leaving for
    // Hardware restores the dark chrome and coming back re-installs the choice,
    // which is remembered independently of the view (see PluginProcessor::editorTheme).
    void applyEditorTheme();
    bool wantLightTheme() const;   // the chosen theme, gated on the editor being shown
    // Hand a just-launched dialog this editor's LookAndFeel — top-level windows do
    // not inherit one. Returns its argument so a launch site stays one statement.
    juce::DialogWindow* themeDialog(juce::DialogWindow* w);
    // Owned so it can be switched between JUCE's dark and light colour schemes: it
    // supplies every colour the editor does NOT set explicitly (popup menus, combo
    // boxes, scrollbars, dialog chrome, the text editor a ValueBox opens). Without
    // it a light editor would still pop dark menus.
    juce::LookAndFeel_V4 lookAndFeel;

    void showFileMenu();
    // File->New: soft reset editor + device to defaults (untitled). File->Open File:
    // choose a .mss (native complete setup) or .syx (raw SysEx) — the format is sniffed
    // from the file, so one chooser handles both. Both guard unsaved changes first.
    void newSetupClicked();
    void openFileClicked();
    std::unique_ptr<juce::FileChooser> openChooser;
    // The half of an open that follows the choice: sniff the format, remember the folder
    // and the recent entry, then load (or defer until the device is past boot). Shared by
    // the chooser and File->Open Recent; it does NOT guard unsaved changes, because Open
    // Recent has to do that before it commits to the file.
    void beginOpenOfFile(const juce::File& file);
    // File->Open Recent-> an entry. Guards unsaved changes, then beginOpenOfFile; a file
    // that has gone away is dropped from the list instead of failing silently.
    void openRecentClicked(const juce::File& file);
    // File->Exit (standalone only) — defers to the window's own closeButtonPressed(), so
    // it is the title-bar X path exactly, unsaved-changes prompt included.
    void exitStandaloneClicked();
    // Native setup saves. Save Setup overwrites the current .mss in place (or falls
    // through to Save Setup As when untitled / a .syx is loaded); Save Setup As always
    // prompts, for whichever of the four kinds the submenu picked. Neither will overwrite
    // a loaded .syx (see openFileType).
    //
    // The KIND comes from the menu rather than the file dialog because JUCE cannot show a
    // labelled "Save as type" list: its Windows chooser builds one COMDLG_FILTERSPEC whose
    // display name IS the pattern string, so the dropdown can only ever read "*.mss".
    // A submenu names each format in full and costs no extra click.
    void saveSetupClicked();
    void saveSetupAsClicked(YamahaMU100AudioProcessor::SetupKind kind);
    std::unique_ptr<juce::FileChooser> setupSaveChooser;
    // Run `proceed` immediately when the model is clean, else after an OK/Cancel warning
    // that unsaved changes will be lost (used by New and Open File).
    void confirmDiscardIfDirty(std::function<void()> proceed);
    // File->"Export Multi 32-Part Syx": write a 32-part multi bulk dump built from the
    // editor's working device-state model (instant, no device round-trip). Shares
    // lastDumpDir + the default-name classifier with the Hardware-view dump capture.
    void saveMultiSyxClicked();
    std::unique_ptr<juce::FileChooser> saveSyxChooser;
    // Apply a chosen file. doSysExLoad streams a raw .syx; doSetupLoad applies a native
    // .mss (patch + plugin-only extensions). Both deferred via pendingLoadFile until the
    // device is past boot (isReadyForEditing) so the load isn't wiped by the boot XG-reset
    // or lost in still-flooding boot/read-back traffic on MIDI IN A; pendingLoadIsSetup
    // selects which path the deferred load takes.
    void doSysExLoad(const juce::File& file);
    void doSetupLoad(const juce::File& file);
    // The .mps / .mds / .mes branch of doSetupLoad: applies the file into the CURRENT
    // selection (the Part module's part, and for a .mds the DRUMS buffer its own PART
    // MODE names) and leaves everything else in the session untouched.
    void doPartialSetupLoad(const juce::XmlElement& xml,
                            YamahaMU100AudioProcessor::SetupKind kind,
                            const juce::File& file);
    // Second half of doPartialSetupLoad, run once the target DRUMS buffer is known
    // (-1 = the one the file was saved with). Split out because the choice is made
    // asynchronously in DrumBufferChooser.
    void finishPartialSetupLoad(YamahaMU100AudioProcessor::SetupKind kind,
                                const juce::File& file, int targetBuffer);
    void showSetupLoadError(const juce::File& file, const juce::String& message);
    void showSetupSaveError(const juce::String& label, const juce::String& message);

    // "Which drum buffer?" overlay, shown before any load that writes one — a buffer is
    // shared by every part naming it, so the write reaches parts the user did not select.
    // The pending file is parked here across the async choice.
    std::unique_ptr<DrumBufferChooser>     drumBufferChooser;
    juce::File                             pendingSetupFile;
    YamahaMU100AudioProcessor::SetupKind   pendingSetupKind { YamahaMU100AudioProcessor::SetupKind::System };
    void askDrumBufferThenLoad(YamahaMU100AudioProcessor::SetupKind kind,
                               const juce::File& file, int savedBuffer);
    juce::File pendingLoadFile;
    bool       pendingLoadIsSetup { false };

    // "Loading file" overlay — covers whichever view (Hardware or Editor) is on
    // screen while a .syx file streams into the device. It is a child of this
    // editor (not of EditorView) so it applies to both views. Time-driven by the
    // editor timer, same model as EditorView's read-back overlay. On completion an
    // Editor-view load fires an automatic device read-back so the panels refresh.
    std::unique_ptr<BusyOverlay> loadOverlay;
    bool         loadingActive  { false };
    juce::uint32 loadStartMs    { 0 };
    int          loadExpectedMs { 0 };
    std::vector<int> loadDrumSetups;            // DRUMS setups the loaded file uses
    static constexpr int kSysExSettleMs = 500;  // post-stream margin before read-back
    void beginSysExLoad(int loadMs, std::vector<int> drumSetups);
    void finishSysExLoad();

    // Bulk-dump-to-file: the timer polls the processor for a completed MIDI-OUT
    // dump burst, then pops a "Save As" dialog seeded with the detected default
    // name and lastDumpDir. pendingDumpBytes holds the captured bytes until saved.
    void checkForCompletedDump();
    std::unique_ptr<juce::FileChooser> dumpChooser;
    std::vector<uint8_t> pendingDumpBytes;
    bool dumpDialogOpen { false };

    // Phase 5: fire one automatic device-state read-back the first time the
    // Editor view is shown with MAME fully booted, so the panels open in sync.
    bool autoReadBackDone { false };

    // Last value of the processor's editorStateExternalSeq the panels have been shown.
    // A mismatch means received SysEx moved the model and the panels must re-apply it.
    uint32_t lastExternalStateSeq { 0 };
    uint32_t lastMidiOverrideSeq  { 0 };
    // Accessibility: the MIDI-override COUNT, spoken once it has SETTLED. The badge
    // in the bottom bar is the sighted form of this, and nothing else in the plugin
    // says the device has drifted unless you happen to be standing on the control
    // that moved. Announced on a delay rather than per change because one program
    // change is a dozen parameters arriving over several ticks -- speaking each would
    // be a paragraph, and the queue's coalescing cannot help when every message is a
    // different string. -1 = nothing waiting.
    int ovAnnouncedCount { 0 };
    int ovSettleTicks    { -1 };
    // Clear and Keep say what they did, and adopt the count so the watcher above does
    // not repeat it a second later.
    void sayOverrideAction(const juce::String& text);

    void showSettingsMenu();           // the Options menu (one menu, both hosts)
    void showHelpMenu();               // About + the manual
    void showShortcutEditor();         // Options -> Edit keyboard shortcuts...
    void openUserManual();             // the F1 command / Options -> manual item
    // Run one kbsc:: command. Returns false when the command is not available in this
    // host or view, which is what lets keyPressed hand the key back to the DAW instead
    // of swallowing it for something that would have done nothing.
    bool performCommand(int cmd);
    // "Ctrl + Z" for a menu item / tooltip, or empty when shortcuts are off, this row is
    // off, or it is unbound. One helper so a menu can never show a stale chord.
    juce::String shortcutText(int cmd) const;
    // Append " (Ctrl + Alt + P)" to a tooltip, or nothing. Used by the bottom-bar
    // buttons, which have no menu row to carry their shortcut.
    juce::String withShortcut(const juce::String& text, int cmd) const;
    void refreshShortcutTooltips();    // push the above onto the strip buttons
    // The shortcut editor lives in a window of its own, held so a second Alt+B brings
    // the existing one forward instead of opening a duplicate.
    juce::Component::SafePointer<juce::DialogWindow> shortcutWindow;

    // Keep JUCE keyboard focus inside this editor whenever our window has the
    // keyboard, so a title-bar click does not leave the shortcuts dead. Polled from
    // the 30 Hz timer -- see the implementation for why there is no event for it.
    void serviceKeyboardFocus();
    // Take the keyboard back after one of OUR surfaces gives it up: a dialog closing,
    // the Voice List hiding, the History window closing. Not a poll -- with JUCE
    // focused on nothing, key events never reach this editor at all (see the
    // implementation), so nothing here would notice.
    void restoreKeyboardFocus(bool alsoBringWindowToFront);
    // The most recent dialog opened through themeDialog(). An async DialogWindow has
    // no close callback, so a SafePointer going null is the only signal it has gone.
    juce::Component::SafePointer<juce::DialogWindow> openDialog;
    bool dialogWasOpen { false };

    // Win32 focus claiming for the title-bar case (see PluginFocusWin.h). Bounded per
    // activation: `lastActiveWindow` notices the user activating a different window and
    // resets the count, so a host that insists on keeping focus is never fought for more
    // than a few ticks.
    void* lastActiveWindow { nullptr };
    int   focusClaims { 0 };
    static constexpr int kMaxFocusClaims = 4;
    void showAudioSettingsDialog();    // Options -> Audio...
    void showAboutDialog();
    void showMidiRoutingDialog();
    void showMidiOutputDialog();
    void showDisplayAnimationDialog(); // Options -> Display Animation...

    // Currently held button index (-1 = none)
    int activeButtonIdx { -1 };

    // "All Macro" rectangle (Part- + Part+ simultaneously) currently held
    bool allMacroActive { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(YamahaMU100AudioProcessorEditor)
};
