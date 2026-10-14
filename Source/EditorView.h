// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include <memory>
#include <functional>
#include <vector>

#include "Accessibility.h"
#include "KeyNav.h"
#include <set>

class YamahaMU100AudioProcessor;
class PartPanel;
class PartListPanel;
class VoiceBrowser;
class GraphicDisplayPanel;
class EffectPanel;
class EffectParametersPanel;
class MultiEqPanel;
class ControllersPanel;
class SystemPanel;
class KeyboardPanel;
class BusyOverlay;

//==============================================================================
// EditorView — the XGEdit-style parameter editor surface (the "Editor" view of
// the swappable single-window editor; the other view is the photo "Hardware"
// panel painted by the AudioProcessorEditor).
//
// It approximates XGEdit's single dense screen: a fixed design canvas
// (1480x1063, matching reference/xgedit gui enhanced.png) scaled uniformly to
// fit the window. Panels are positioned by design-coordinate rectangles so they
// keep XGEdit's relative layout at any size. Built out one module at a time;
// regions not yet implemented are drawn as labelled scaffold placeholders.
//==============================================================================
class EditorView : public juce::Component,
                   private juce::FocusChangeListener
{
public:
    explicit EditorView(YamahaMU100AudioProcessor& proc);
    ~EditorView() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    //==========================================================================
    // Phase 5 device-state read-back.
    // requestReadBack() asks the device for its current state; the captured dump
    // is fed back through consumeDump() (the host editor's dump-poll routes it
    // here while a read-back is pending). pollReadBackTimeout() is called each
    // editor-timer tick so a request that gets no reply eventually gives up.
    // `fileDrumSetups` (optional) names drum setups known to be in use from a just-
    // loaded .syx (its PART MODE writes) so their contents read back in one pass even
    // before the part buffers have been refreshed. If a read-back is already in
    // flight the request is queued and re-issued when it ends (so a load triggered
    // during the open read-back isn't lost).
    void requestReadBack(const std::vector<int>& fileDrumSetups = {});
    void consumeDump(const std::vector<uint8_t>& bytes);
    void pollReadBackTimeout();
    bool isAwaitingDump() const { return readBackPending; }
    // Abort an in-flight read-back (and drop any queued re-read), e.g. before
    // streaming a .syx so the file isn't lost in the read-back's request flood on
    // MIDI IN A. Opens the trailing-reply grace window so the aborted run's replies
    // don't pop a spurious "Save dump" dialog.
    void cancelReadBack();
    // True for a short grace period after a read-back ends: bulk dumps arriving in
    // this window are trailing read-back replies (not a user dumpout), so the host
    // editor discards them instead of popping a spurious "Save dump" dialog.
    bool inReadBackGrace() const;

    // Push the processor's already-captured device-state model into every panel
    // WITHOUT contacting the device. Used on GUI reopen to refresh the panels
    // cheaply instead of re-running the full (MIDI-flooding) read-back.
    void applyDeviceState();
    // Re-read the MIDI-override shadow onto the panels. Cheap and independent of
    // applyDeviceState: the shadow moves while the model stands still.
    void refreshMidiOverrides();
    // The Voice module's yellow row roll-ups. Separate from the override refresh:
    // that one rides an event, this one has to be polled (see the .cpp).
    void refreshRowChanged();

    // The host editor calls this when it swaps between the Hardware photo and this
    // view. JUCE's setVisible() notifies only the component itself, never its
    // descendants, so the Graphic Display cannot see the swap on its own — and it
    // has to, because its EQ analyser must not keep running behind the photo.
    // (The analyser also re-tests isShowing() every frame, which covers the HIDE
    // direction by itself; this call is what restarts it on the way back.)
    void notifyViewShown(bool shown);

    // Display Animation options changed (the analyser's on/off).
    void refreshSpectrumOptions();

    // Push the processor's "Disable plugin-only parameters" flag into the two panels
    // that host such parameters (Part module + Graphic Display). Called from the ctor
    // and whenever either host's Options toggle flips it.
    void refreshPluginOnlyLock();
    void refreshRcvRowVisible();   // "Show part MIDI receive switches" option
    void refreshMuteSoloCols();    // "Show Mute and Solo columns" option
    void refreshMidiIndicators();  // "MIDI input indicators" option (starts/stops a timer)
    void refreshMuteSolo();        // a mute/solo bit changed (repaint only)
    void refreshEqBypass();        // an EQ-bypass bit changed (lamp + EQ screen)
    void refreshVoiceNames();      // "Voice names" option (full / 8-character)
    void refreshChangedMarks();    // "Show changed markers" option (paint only)

    // Push the processor's "Mark DAW-automatable params" choice into the two panels
    // that hold host-automatable controls (System + Part). Called from the ctor and
    // whenever the VST Options dropdown changes it. VST-only — this is where the
    // standalone is forced to "None", since it has no host to automate from.
    void refreshAutomationMarkers();
    void refreshNoteLabels();     // Options > Label notes as (global + title + repaint)

    // Push the processor's MIDI-override mark STYLE into the live global the value
    // boxes and knobs read, then re-lay-out the one module that draws those marks.
    // It is a LAYOUT change, not a repaint: the horizontal bar sits in a strip the
    // value boxes are built taller for.
    void refreshMidiMarkStyle();

    // --- What the partial setup files (.mps / .mds) are scoped to ---
    // The part the Part module is editing, and its DRUMS edit buffer (0..3, or -1 when
    // the part is melodic or holds a kit that was never allocated to a buffer — the
    // case a Drum Setup save has to refuse). The buffer comes from the Voice module,
    // which derives it from PART MODE; see PartListPanel::getPartBuffer.
    int getCurrentPart() const;
    int getCurrentPartDrumBuffer() const;

    // --- Keyboard-shortcut entry points ------------------------------------------
    // Each is the exact path the equivalent click takes, so a shortcut cannot grow a
    // second behaviour: selectPart goes through the same onSelectPart destination the
    // Voice rows and the bar axis use, and the mute/solo pair reaches PartListPanel's
    // own cell handler. They are no-ops when the panel they need is not built.
    void selectPart(int part0to31);
    void stepSelectedPart(int delta);          // wraps at both ends of 0..31
    void setDisplayMode(int mode);             // GraphicDisplayPanel::Mode
    void toggleMuteSolo(bool solo, bool exclusive);   // acts on the selected part
    void openVoiceListForCurrentPart();

    // --- Keyboard control of the value boxes (KeyNav.h) ---------------------------
    // The cursor is DRAWN here (the ring) and MOVED here, but since accessibility
    // pillar 2 it IS juce keyboard focus: placing the cursor grabs focus for that
    // control, and focus arriving from anywhere else -- a click, Tab traversal while
    // our command layer is off, or a screen reader calling
    // AccessibilityHandler::grabFocus -- moves the cursor to match. One cursor, one
    // focus, one ring; see navFocusCursor / globalFocusChanged.
    //
    // The plugin editor is still the single READER of keys: these controls decline
    // every key (juce::Slider's own arrow handling is out of reach because the dial
    // is not the focus stop -- the XgKnob wrapper is), so a key bubbles from the
    // focused control up to the host editor exactly as it did before.
    //
    // Each returns false when there is nothing to do, which the host editor turns
    // into "hand the key to the DAW" rather than swallowing it.
    bool navStepGroup(int delta);              // Tab / Shift+Tab
    bool navStepItem(int delta);               // cursor left / right
    bool navAdjust(int dir, bool fine);        // cursor up / down (+ Shift)
    bool navAdjustX(int dir, bool fine);       // `.` / `,` (+ Shift) -- two-axis stops only
    bool navAdjustQ(int dir, bool fine);       // `]` / `[` (+ Shift) -- three-axis stops only (EQ Q)
    bool navActivate();                        // Enter
    // Ctrl+Enter. Returns FALSE where the stop has no secondary action, which is
    // almost everywhere -- that is what hands the key back to the host.
    bool navActivateAlt();
    bool navReset();                           // Backspace
    // Drop the cursor (and its ring) — e.g. when the view is hidden.
    void navClear();

    // Options ▸ Show the arrow-key mark. `always` draws it whenever the cursor is on
    // something; `keyboard` hides it again on the next mouse press, so a mouse-only
    // user never meets it. The CURSOR is unaffected either way -- this is paint, and
    // nothing here may touch focus or the accessibility tree.
    void setFocusMarkMode(int mode);

    // Options ▸ Part module title shows ▸ (a PartPanel::TitleNames bitmask). The names
    // themselves are pushed by pushPartTitleNames(), which this calls.
    void setPartTitleNames(int mask);

    // THERE IS ONE ACTIVE CONTROL, NOT TWO. A mouse press or wheel anywhere in the
    // canvas moves the keyboard cursor onto whatever it landed on, and clears it if
    // that was not a navigable control — so the white ring always marks the thing the
    // next keystroke will act on, and can never sit on one control while the mouse is
    // editing another. EditorView listens to the whole canvas subtree for this, which
    // is also why no control needed a callback of its own.
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // True when keyboard focus is on the control the cursor is on. The host editor
    // asks before running the accessibility key floor -- the handful of keys that must
    // operate a FOCUSED control even when the shortcut layer is switched off, because
    // accessibility is not an option.
    bool navHasFocusedStop() const;

    // Called whenever an in-canvas overlay that took the keyboard gives it up again --
    // today just the Voice List. Hiding a focused component leaves JUCE with NO focused
    // component at all, and a key event with nothing focused is delivered to the PEER's
    // top component and walks UP from there, so it never reaches the plugin editor and
    // the host gets it instead. The host editor uses this to take the keyboard back.
    std::function<void()> onKeyboardFocusReturn;
    // A part menu was asked for -- by right-clicking a Voice row or the bar screens'
    // part ruler, or by the Part menu shortcut. The menu itself lives in the plugin
    // editor: it opens dialogs and the drum-buffer chooser, neither of which this view
    // owns. The part is already selected by the time this fires.
    std::function<void(int part)> onPartContextMenu;
    // Open it on whatever part is selected -- the keyboard route (Shift+F10).
    void requestPartContextMenu();

private:
    // --- keyboard cursor ---------------------------------------------------------
    // The modules that offer stops, in canvas reading order. Rebuilt on demand (it is
    // nine pointer tests) so a panel that has not been constructed yet, or a view
    // rebuilt by a theme change, cannot leave a dangling entry behind.
    std::vector<keynav::Group*> navGroups() const;
    // Move the cursor to the first usable stop at or after `item` in `group`, trying
    // successive groups in `dir` when that group has none. Returns false when nothing
    // in the whole canvas can take a cursor.
    bool navPlace(int group, int item, int dir);
    // The stops of the group the cursor is in, and the component they live in.
    std::vector<keynav::Item> navCurrentItems() const;
    juce::Component* navGroupComponent(int group) const;
    void navUpdateRing();
    // True while the Voice List is up: it is an overlay that takes the keyboard for
    // itself, so the editor's own keynav must not act behind it. See navBlocked().
    bool navBlocked() const;
    // Put the cursor on whatever `e` landed on (see mouseDown above).
    void navPointAt(const juce::MouseEvent& e);
    // Give the control the cursor is on the keyboard focus, so a screen reader reads
    // it. Painted cells have no component to focus (pillar 3 turns them into real
    // ones); until then the cursor moves and focus stays put.
    void navFocusCursor();
    // The reverse direction: focus moved somewhere else, so bring the cursor to it.
    void globalFocusChanged(juce::Component* focused) override;
    // The stop that owns `c` (it, or an ancestor of it), or false. Shared by the
    // focus listener and navHasFocusedStop.
    bool navFindStopFor(juce::Component* c, int& group, int& item) const;

    int navGroup { -1 }, navItem { -1 };
    // Options ▸ Show the arrow-key mark (YamahaMU100AudioProcessor::FocusMarkMode).
    int  navMarkMode { 0 };
    // In `keyboard` mode: has the computer keyboard been used since the last mouse
    // press? Set by anything that MOVES OR ACTS ON the mark -- the nav keys and the
    // part-step keys -- which is the honest rule: a key that moves the mark shows it.
    bool navMarkShown { true };
    // Hide on a mouse PRESS, never on mouse MOVEMENT: a keyboard user who nudges the
    // mouse out of the way must not lose the mark mid-edit.
    void navNoteKeyboardUse();
    bool navMarkVisible() const;

    // Accessibility pillar 4. The same process-wide queue the plugin editor uses -- a
    // SharedResourcePointer, so this needs no plumbing through the host editor and two
    // instances cannot talk over each other. Everything it is told is a change the user
    // cannot see: which part is selected, what mode the Keyboard module is in, that a
    // read-back has started. Silent unless a screen reader is listening.
    mu100a11y::announce::Shared announcer;
    // Set while navFocusCursor is grabbing focus, so the focus notification it causes
    // does not come straight back in and re-place the cursor.
    bool navMovingFocus { false };

    // The focus ring: a transparent child of the canvas that draws a 2px outline over
    // the control the cursor is on. A component rather than a paintOverChildren hook
    // because the panels are canvas children and the ring has to sit ON TOP of them
    // without any of them knowing it exists.
    struct NavRing : public juce::Component
    {
        NavRing() { setInterceptsMouseClicks(false, false); }
        void paint(juce::Graphics&) override;
        keynav::Item::Cue cue { keynav::Item::Cue::Ring };
    };
    NavRing navRing;

    // Tell the Keyboard whether `part` is an editable-drum part (kit on a DRUMS
    // buffer) and which kit, so it shows/updates the DRUM KEY NAME box.
    void refreshDrumContext(int part);
    void pushPartTitleNames();     // hand PartPanel the voice / drum-tone names
    // Push each part's DRUMS-buffer index (-1 = melodic) to the processor, so the
    // engine can route drum voices to the right per-(buffer,key) plugin EQ/HPF.
    void pushDrumBufferMap();
    // Design canvas -> on-screen transform (uniform scale + centring letterbox).
    struct Xform { float scale; float ox; float oy; };
    Xform transform() const;

    YamahaMU100AudioProcessor& audioProcessor;

    // All editor modules live inside this fixed-size design canvas (kDesignW x
    // kDesignH). Rather than scaling each panel's BOUNDS to the window (which left
    // the fonts + value boxes inside at fixed pixel sizes), the canvas is laid out
    // once at design coordinates and the whole thing is drawn through a single
    // AffineTransform (uniform scale + letterbox), so fonts, boxes and lines all
    // scale together and stay crisp at any window size. Mouse events are
    // inverse-mapped by JUCE. The read-back overlay stays a direct child of the
    // view (unscaled) so it covers the letterbox margins too; the voice browser is
    // a canvas child so it rides the same scale.
    juce::Component canvas;

    std::unique_ptr<PartPanel>           partPanel;
    std::unique_ptr<PartListPanel>       partListPanel;   // the "Voice" module
    std::unique_ptr<GraphicDisplayPanel> graphicDisplay;  // XGEdit "Display Panel"
    std::unique_ptr<EffectPanel>           effectPanel;       // XGEdit "Effect" module
    std::unique_ptr<EffectParametersPanel> effectParamsPanel; // "Effect Parameters"
    std::unique_ptr<MultiEqPanel>          multiEqPanel;      // XGEdit "Multi EQ" module
    std::unique_ptr<ControllersPanel>      controllersPanel;  // XGEdit "Controller" section
    std::unique_ptr<SystemPanel>           systemPanel;       // XGEdit "System" module
    std::unique_ptr<KeyboardPanel>         keyboardPanel;     // XGEdit "Keyboard" module
    std::unique_ptr<VoiceBrowser>        voiceBrowser;     // full-editor overlay
    int browserPart { -1 };

    // Read-back round-trip state (the device-state model itself lives in the
    // processor as the shared source of truth — see getEditorState()).
    bool          readBackPending   { false };
    bool          readBackFirstBurst { false };  // clear the model on the 1st burst only
    juce::uint32  readBackRequestMs { 0 };
    // A full drum read-back can stream for several seconds before it goes idle and
    // a burst is delivered, so wait a long time for the FIRST burst; once bursts
    // are arriving, a short gap with no further burst means the read-back is done.
    static constexpr int kReadBackInitialMs = 20000;
    // Idle gap (in delivered replies) that marks the read-back done. Generous so a
    // brief lull in the continuous request stream — an unanswered request stretch, or
    // MAME briefly running below real-time after a reset — isn't mistaken for the end.
    static constexpr int kReadBackBurstGapMs = 2500;
    // The whole read-back streams as one burst delivered only after it goes idle, so
    // the first-burst window must outlast the entire (now part-chunk + drum) request
    // run — which can take much longer than wall-clock when MAME is still warming up
    // after a reset. Sized per read-back from its expected duration (+ a slow-MAME
    // margin) so we never give up before the reply arrives and strand a stray dump.
    int           readBackFirstBurstLimitMs { kReadBackInitialMs };
    static constexpr int kReadBackGraceMs = 5000;
    juce::uint32  readBackEndedMs { 0 };       // when the last read-back ended (0 = none)
    // True from a user "Stop reading" until the device's trailing reply stream (from
    // requests already sent before the flush) has gone quiet. While set, inReadBackGrace()
    // swallows any finalized dump so the aborted read-back's tail can't pop a Save dialog.
    bool          readBackCancelled { false };
    bool          reReadPending   { false };   // a read-back was requested while one ran
    std::set<int> pendingFileSetups;           // file-derived drum setups for the next run

    // "Reading from device" overlay (an in-editor child component, NOT an OS popup
    // — see BusyOverlay.h). Shown while a read-back is in flight; its progress bar
    // is driven from a time estimate (the drum scan that dominates the duration is
    // paced deterministically by requestDeviceDump).
    std::unique_ptr<BusyOverlay> readBackOverlay;
    juce::uint32  readBackStartMs    { 0 };
    int           readBackExpectedMs { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorView)
};
