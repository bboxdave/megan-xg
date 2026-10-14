// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "Accessibility.h"
#include "KeyNav.h"
#include <array>
#include <functional>
#include <memory>

class YamahaMU100AudioProcessor;
class ValueBox;
namespace mu100 { class DeviceState; }

//==============================================================================
// KeyboardPanel — XGEdit's Keyboard module (manual section 11). A full-range
// on-screen keyboard (C-2 .. G8) for the current part:
//
//   * Left-drag selects the part's playable key range (NOTE LIMIT LOW/HIGH);
//     a red bar above the keys shows the current range. Clicking nearer the
//     low end moves the lower bound, nearer the high end the upper bound.
//   * Right-drag auditions notes on the current part (note-on/off, velocity =
//     the velocity-limit HIGH value, per manual 11.2/11.3).
//   * Two value boxes on the right set VELOCITY LIMIT LOW/HIGH.
//   * With Options -> Display Animation -> "Played notes on the keyboard" on,
//     every key held by incoming MIDI on the current part's Rcv channel lights in
//     the same green as a right-drag audition. It coexists with drum-edit mode on
//     purpose: the red drum-key marker is drawn on top of it and means something
//     different ("the tone you are editing" vs "this key is sounding"), which is
//     exactly what makes a kick/snare/hat hit legible while one of them is selected.
//   * Below them, SYS / FX / BULK lamps flash for incoming MIDI that belongs to
//     no single part (see classifySysEx): system-level parameters and universal
//     SysEx, system/insertion effect edits, and bulk/performance loads. Same
//     size, colour and fade as the Voice module's per-part tabs (midiind).
//
// Drum-edit mode (single-key drum-tone selection + the DRUM KEY NAME box) is a
// later addition that builds on this; not yet wired here.
//==============================================================================
class KeyboardPanel : public juce::Component,
                      public juce::SettableTooltipClient,
                      public keynav::Group,
                      private juce::Timer
{
public:

    // --- keyboard navigation (KeyNav.h) ---
    std::vector<keynav::Item> navItems() override;
    juce::String navGroupName() const override { return "Keyboard"; }

    explicit KeyboardPanel(YamahaMU100AudioProcessor& proc);
    ~KeyboardPanel() override;

    void setCurrentPart(int part0to31);

    // Tell the keyboard whether the current part is an editable-drum part (a kit
    // allocated to a DRUMS buffer) and, if so, which kit — so it can show the
    // DRUM KEY NAME box and resolve per-key drum-voice names. Any change exits
    // drum-edit mode (the box returns to an unselected state).
    // `buffer` is the DRUMS buffer 0..3 the kit sits on, or -1 for a melodic part.
    // It is what the per-key marker strip is keyed by: DRUM SETUP data belongs to
    // the BUFFER, not the part, and is shared by every part assigned to it.
    void setDrumContext(bool isDrumPart, int kitMsb, int kitLsb, int kitPrg0,
                        int buffer);

    // The per-key marker strip above the keys. Two separate triggers, matching the
    // two things it summarises:
    //   refreshDrumMarks     -- the MODEL moved (polled at ~4 Hz on the editor's
    //                           dirty-marker divider, like the Voice module's yellow
    //                           row tabs: nothing announces a model write to a panel).
    //   refreshMidiOverrides -- the override SHADOW moved (its own sequence).
    // Both recompute both masks, because the shadow is defined as the difference
    // between the device and the model and either side moving can change it.
    void refreshDrumMarks();
    void refreshMidiOverrides() { refreshDrumMarks(); }

    // Fired when drum-edit mode is entered/left or the selected drum key changes
    // (active, note 13..91 or -1). The host broadcasts it to the Part / Graphic
    // Display modules so they retarget to the drum key.
    std::function<void(bool active, int drumKey)> onDrumEditChanged;

    bool isDrumEditActive() const { return drumEditActive; }

    // drumEditKey, or -1 when nothing is picked OR the picked key carries no drum in
    // this kit. Public alongside drumKeyText() so a caller can tell "there is a tone to
    // name" from "there is not" without matching on the "NO DRUM KEY" string.
    int  effectiveDrumKey() const;
    int  getDrumEditKey()   const { return drumEditKey; }
    juce::String drumKeyText() const;   // "NO DRUM KEY" or the selected drum tone name

    // True when a key is HIGHLIGHTED on the keyboard, whether or not the kit maps a
    // drum tone to it. effectiveDrumKey() collapses both cases to -1 — correctly, as
    // there is nothing to edit either way — so this is the only way to tell them
    // apart, which the Part module needs to say WHICH of the two it is showing.
    bool isDrumKeyHighlighted() const { return drumEditKey >= 0; }

    // Phase 5 read-back: refresh the current part's range + velocity limits.
    void applyState(const mu100::DeviceState& st);

    void resized() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String getTooltip() override;   // keyboard (mode-aware) + drum-edit-key box

private:
    static constexpr int kFirstNote = 0;    // C-2
    static constexpr int kLastNote  = 127;  // G8
    static constexpr int kMiddleC   = 60;   // C3 (Yamaha note 0 = C-2)

    // --- the per-key marker strip (see paintDrumMarkStrip) ---
    // Two rows of tabs between the red range bar and the keys, one slot per note:
    // the upper row says the key has values off its kit's defaults, the lower that
    // incoming MIDI is overriding one of them. Reserved unconditionally so the key
    // height does not jump when the selected part changes; painted only on a part
    // that has a DRUMS buffer, where it is the only overview of a 79-key block.
    static constexpr int kMarkRowH   = 5;    // height of each of the two rows
    static constexpr int kMarkRowGap = 1;    // between them
    static constexpr int kMarkTopGap = 2;    // below the red range bar
    static constexpr int kMarkStripH = kMarkTopGap + 2 * kMarkRowH + kMarkRowGap;

    int  markStripTop() const;                      // y of the strip's first row
    juce::Rectangle<int> markStripBounds() const;   // the whole band, kit span only
    juce::Rectangle<float> markSlot(int note, int row) const;
    int  stripNoteAt(juce::Point<int> p) const;     // -1 outside the strip
    void paintDrumMarkStrip(juce::Graphics& g);

    int  rightPanelW() const;                       // drum-key box + velocity panel
    void layOutLamps(juce::Rectangle<int> vel);     // SYS/FX/BULK row (called from resized)
    int  keyMargin() const;                          // blank space each side of the keys
    juce::Rectangle<int>   keysArea() const;       // white/black key drawing area
    juce::Rectangle<float> noteRect(int note, bool& isBlack) const;
    int  noteAt(juce::Point<int> p) const;         // -1 if outside the keys
    static bool isBlackNote(int note);

    void loadFromState(const mu100::DeviceState& st);
    int  currentRcvChannel() const;     // raw 0..31 (port*16 + ch-1), or -1 if Rcv OFF
    void updatePlayedNotes();           // refresh heldMask from the processor
    void timerCallback() override;                   // lamps + (in drum-edit) played notes
    void updateLamps();                              // drain activity mask, advance envelopes
    void repaintLamps();
    void sendRange();                               // transmit NOTE LIMIT LOW/HIGH
    void setRangeEnd(int which, int note);          // 0 = low, 1 = high; clamped + undo
    void nudgeRange(int which, int dir, bool fine); // an octave, or a semitone with Shift
    juce::String rangeNoteName(int note) const;
    void startAudition(int note);
    void stopAudition();
    bool drumKeyHasVoice() const;                    // selected key maps to a drum voice

    void setDrumKey(int note);                       // select a drum key (clamped)

    YamahaMU100AudioProcessor& audioProcessor;
    int currentPart { 0 };
    int rangeLow  { 0 };
    int rangeHigh { 127 };
    int auditionNote { -1 };   // >= 0 while right-drag auditioning
    int dragBoundary { -1 };   // 0 = low edge, 1 = high edge (left-drag), else -1

    // An undo gesture held open for the length of a range drag. sendRange() writes
    // BOTH note limits on every move, so the address-keyed merge sees the key
    // alternate LOW/HIGH/LOW/HIGH and can never fold two consecutive writes — every
    // single message became its own step. Explicit rather than an UndoScope object
    // because it spans mouseDown..mouseUp; endRangeUndo() is a no-op when none is
    // open, so every path that can drop the drag may just call it.
    bool rangeUndoOpen { false };
    void beginRangeUndo();
    void endRangeUndo();

    std::unique_ptr<ValueBox> velLoBox, velHiBox;
    int noteLowParam { -1 }, noteHighParam { -1 };
    int velLowParam  { -1 }, velHighParam  { -1 };
    int chParam      { -1 };            // MULTI PART "Rcv CHANNEL"

    // Keys currently held on the current part's Rcv channel, as a 128-bit set
    // ([note / 32]). Polled from the processor by the timer; all zero when the
    // option is off, when the part receives on no channel, or when nothing is held.
    std::array<uint32_t, 4> heldMask {};

    // Drum-edit mode. The DRUM KEY NAME box shows only for drum parts; clicking
    // it enters mode (single-key drum-tone selection); the key range bar is then
    // replaced by a single-key marker.
    bool drumPart       { false };
    int  drumBuffer     { -1 };                 // DRUMS buffer 0..3, or -1
    // The strip's two 128-bit masks (mask[note >> 5], bit note & 31), recomputed on
    // the two refresh calls above rather than per paint: the changed side walks the
    // buffer's whole 3n block, which is not a per-repaint cost.
    std::array<uint32_t, 4> markChanged {}, markOverridden {};
    void toggleDrumEditMode();   // the DRUM KEY box's click, shared with the keyboard

    bool drumEditActive { false };
    int  drumEditKey    { -1 };                 // 13..91, or -1 = none selected
    int  kitMsb { 0 }, kitLsb { 0 }, kitPrg0 { 0 };
    juce::Rectangle<int> drumBoxRect;
    // Accessibility pillar 3: the DRUM KEY box is painted, so it carries an invisible
    // stand-in that can hold keyboard focus and be read out. navItems() moves it onto
    // drumBoxRect and hides it on a part that is not playing a kit.
    // See mu100a11y::CellProxy.
    mu100a11y::CellProxy drumProxy;
    // ...and the two ends of the note-range bar, which are stops in their own right.
    // NOTE LIMIT LOW/HIGH are set ONLY by dragging the keys -- they appear on no knob
    // and in no value box anywhere in the editor -- so without these the part's
    // playable range could not be set without a mouse at all. Same class of gap the
    // envelope nodes were.
    mu100a11y::CellProxy rangeProxy[2];   // 0 = low, 1 = high
    // An octave a press, a semitone with Shift. There is no wheel over the keys to
    // match, so the pair is chosen for the instrument instead: the octave is the unit
    // a keyboard is actually laid out in, and it makes the 128-note span crossable.
    static constexpr int kRangeCoarse = 12;
    // See PartListPanel::refreshCellProxies -- navItems() is the one place the DRUM
    // KEY box's stop is described, so the proxy is placed by running it.
    void refreshCellProxies() { (void) navItems(); }
    uint32_t noteOnSeq  { 0 };                  // last seen processor note-on seq

    // White-key layout (size-independent): per note its 0..numWhite-1 column, or
    // -1 for black keys.
    int numWhite { 0 };
    std::array<int, 128> whiteIndex {};

    // Velocity panel label/caption rects (set in resized, drawn in paint).
    juce::Rectangle<int> velLoLabel, velHiLabel, velCaption;

    // SYS / FX / BULK "no-part MIDI in" lamps. lampGroup[i] is the whole
    // label+lamp cell (used for hit-testing the tooltip), lampOutline[i] the
    // always-drawn frame and lampRect[i] the orange tab inside it; lampLevel[i]
    // is its fade envelope and lampHoldUntil[i] the ms timestamp it stays lit
    // until. Indices match kLamps in the .cpp.
    static constexpr int kNumLamps = 3;
    std::array<juce::Rectangle<int>, kNumLamps> lampGroup {}, lampOutline {}, lampRect {};
    std::array<float,  kNumLamps> lampLevel {};
    std::array<double, kNumLamps> lampHoldUntil {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KeyboardPanel)
};
