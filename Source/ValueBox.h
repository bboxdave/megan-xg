// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include <functional>
#include "MidiOverrideMarks.h"

//==============================================================================
// ValueBox — a compact LCD-style value field with no dropdown arrow, editable by:
//   * left-click + drag up/down   — adjust (like a slider/knob; Shift = fine)
//   * mouse wheel (hover only)     — nudge one step
//   * single click (no drag)       — type a value (if typing enabled)
//   * right-click                  — reset to default
//
// Used both as the value display inside XgKnob (so the box drives the knob) and
// stand-alone for stepped/enum parameters (Mono/Poly, Assign, scale tune).
//==============================================================================
class ValueBox : public juce::Component,
                 public juce::SettableTooltipClient
{
public:
    ValueBox();
    ~ValueBox() override;

    void setRange(double lo, double hi, double interval = 1.0);
    void setOptions(const juce::StringArray& options);   // enum mode (range 0..n-1, no typing)

    void setValueQuiet(double v);                         // no onChange
    double getValue() const { return value; }
    void setDefaultValue(double v) { defaultValue = v; }
    void setTypingEnabled(bool e)  { typingEnabled = e; }

    // True when the current value differs from its default (drives the yellow
    // changed-marker). Exposed so a container can aggregate several boxes into a
    // single row-level marker.
    bool isChanged() const { return std::abs(value - defaultValue) > 1e-9; }

    // When false, the box does not paint its own changed-tab (e.g. because a
    // parent draws a single aggregate marker for the whole row). Default true.
    void setShowChangedTab(bool s) { showChangedTab = s; }

    // The MIDI-override mark: where the DEVICE is, when incoming MIDI has left it
    // somewhere other than this box's value. `present` false clears it.
    //
    // The box's own value NEVER moves for this -- that is the whole point of the
    // feature, and what keeps a save honest. This draws a second value beside it.
    void setMidiOverride(double v, bool present)
    {
        if (present == overridePresent
            && (! present || std::abs(v - overrideValue) < 1e-9)) return;
        overridePresent = present;
        overrideValue   = v;
        invalidateAccessibilityHandler();   // the announced value gains/loses a half
        repaint();
    }
    bool hasMidiOverride() const { return overridePresent; }
    double getMidiOverrideValue() const { return overrideValue; }
    // Where the override sits in this box's range, 0..1, for the bar and the dial dot.
    float midiOverrideProportion() const
    {
        if (maxV <= minV) return 0.0f;
        return (float) juce::jlimit(0.0, 1.0, (overrideValue - minV) / (maxV - minV));
    }

    // The value an override bar GROWS OUT OF: the parameter's rest point, not its
    // minimum, for a bipolar parameter (PAN's C, a signed knob's 64, detune's 128,
    // an EQ gain's 0 dB). Unset = the minimum, which is right for every parameter
    // whose range really does start at nothing -- including one that merely DEFAULTS
    // to mid-range, since its middle is a default and not a zero.
    //
    // Set from the one place that already knows the parameter's shape (PartPanel's
    // Kind table), so the bar, the dial arc and the spoken value cannot disagree.
    void setBarOrigin(double raw) { barOrigin = raw; hasBarOrigin = true; repaint(); }
    void clearBarOrigin()         { hasBarOrigin = false; repaint(); }
    float midiOverrideOriginProportion() const
    {
        if (maxV <= minV || ! hasBarOrigin) return 0.0f;
        return (float) juce::jlimit(0.0, 1.0, (barOrigin - minV) / (maxV - minV));
    }

    //== the mark's FORM (see MidiOverrideMarks.h) ==============================
    // `dialOwned` -- this box is the readout of an XgKnob that is showing its dial,
    // so in the arc style the DIAL carries the mark and the box draws nothing. Set by
    // XgKnob; a stand-alone box leaves it false.
    void setDialOwned(bool d) { if (dialOwned != d) { dialOwned = d; repaint(); } }

    // `markable` -- this control can carry a MIDI-override mark, and its LAYOUT has
    // granted the room one needs. Opt-in, and false by default, so every module that
    // can never carry a mark keeps its geometry and its knobs' size exactly as they
    // were. A box whose row-mates can be marked sets it too, so a grid row stays a row.
    void setOverrideMarkable(bool m) { if (markable != m) { markable = m; repaint(); } }
    bool isOverrideMarkable() const  { return markable; }

    // What the layout must add for the bar strip, and what paint() takes back off the
    // bottom before drawing the field. ONE answer, asked by both.
    int overrideStripHeight() const
    { return markable ? midimarks::stripHeight(dialOwned) : 0; }

    // The field itself: our bounds less the bar strip.
    juce::Rectangle<int> fieldBounds() const
    { return getLocalBounds().withTrimmedBottom(overrideStripHeight()); }

    // Grey the value text without disabling the box (still interactive). Used by
    // the effect tempo-sync knobs to show a note whose target time/freq is out
    // of range at the current tempo ("keep note, clamp raw"). Independent of
    // isEnabled(): a disabled box already greys itself.
    void setDimmed(bool d) { if (dimmed != d) { dimmed = d; repaint(); } }
    bool isDimmed() const { return dimmed; }

    // Paint the frame and background but NO value text. For a box whose value is
    // meaningless in the current state rather than merely uneditable — the Part
    // module's drum-mode cells before a drum key is picked, where the numbers on
    // screen are seeded defaults that belong to no key and would be read as the
    // key's data. Deliberately separate from setDimmed / setEnabled, which say
    // "not editable"; this says "there is no value here to show".
    void setBlank(bool b)
    {
        if (blank == b) return;
        blank = b;
        invalidateAccessibilityHandler();   // a blank cell holds no value to announce
        setWantsKeyboardFocus(! b);         // ...and nothing for a focus stop to say
        repaint();
    }
    bool isBlank() const { return blank; }

    std::function<void(double)> onChange;
    std::function<juce::String(double)> textFromValue;          // default: integer
    std::function<double(const juce::String&)> valueFromText;   // default: getDoubleValue

    // Optional per-value colour override. Both arguments are pre-filled with the
    // colours this box would otherwise paint (background, then text); a client may
    // replace either for particular values. Used by the Voice CH box to draw "OFF"
    // as black-on-grey so an inactive part reads at a glance against the normal
    // green-on-black fields. Leave unset for the default look.
    std::function<void(double, juce::Colour&, juce::Colour&)> valueColour;

    // Optional hooks for a control whose values are NOT a uniform `interval` grid —
    // e.g. the frequency boxes, whose steps get coarser as the value rises and whose
    // drag sweeps logarithmically (see FreqLadder.h). Any hook left unset keeps the
    // plain linear behaviour above. Whatever these return is still clamped to
    // [minV, maxV], and drag/wheel/type-in results all pass through `snapValue`.
    //   snapValue  : any value -> the nearest allowed one
    //   dragValue  : (value at mouse-down, pixels dragged (up = +), Shift held) -> value
    //   wheelValue : (current value, detent direction ±1, Shift held) -> value
    // Unlike the linear path, the modifier is read live, so Shift can be pressed or
    // released part-way through a drag.
    std::function<double(double)>              snapValue;
    std::function<double(double, double, bool)> dragValue;
    std::function<double(double, int, bool)>    wheelValue;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseExit(const juce::MouseEvent&) override;

    // Suppress the tooltip once an edit begins on this box, until the cursor
    // leaves and returns (so it doesn't pop back over a control you're using).
    juce::String getTooltip() override;

    // Set the value and fire onChange (only if it actually changes). Public so the
    // Graphic Display EQ screen can drive a box through PartPanel::setEqFromDisplay.
    void setValueNotify(double v);

    //== keyboard control (see KeyNav.h) =======================================
    // The three things an arrow key, Enter and Backspace do to this box. Each is
    // the SAME code path the equivalent mouse gesture takes, so the keyboard
    // cannot grow a second behaviour:
    //   nudge   — one wheel detent's worth. `fine` = Shift, i.e. one step.
    //   beginTypeIn — what a click without a drag does.
    //   resetToDefaultValue — what a right-click does.
    // `nudge` does NOT consult dialaction::reversed(): that option describes which
    // way to push a MOUSE, and "up increases" on the keyboard is the key's meaning,
    // not a direction of travel.
    void nudge(int dir, bool fine);
    void beginTypeIn();
    // Fired when a type-in closes, committed or cancelled alike. For an OVERLAY box --
    // one shown over something else for the duration of the edit, like the Graphic
    // Display's bar type-in -- which has to put itself away again and hand the keyboard
    // back. An ordinary box never sets it: it was already visible and stays so.
    std::function<void()> onTypeInEnd;

    // The exact string on screen -- what a screen reader must speak (see
    // Accessibility.h), not the raw byte behind it.
    juce::String displayedText() const { return currentText(); }

    // The same string PLUS the device's own value when incoming MIDI has moved it
    // away from this box's (see setMidiOverride). This is what a screen reader is
    // given: the mark is a colour and a bar, so an override is otherwise the one
    // change in the editor that says nothing at all without sight.
    //
    // Public and used by XgKnob as well: a knob hides its value box from the
    // accessibility tree and answers for it, so both must speak the same sentence
    // and neither may own a second copy of the wording.
    juce::String spokenText() const;

    // Keep an unsuppressed copy of the tooltip: getTooltip() deliberately returns ""
    // while an edit is in progress, and an accessible NAME must not come and go.
    void setTooltip(const juce::String& t) override
    {
        rawTip = t;
        juce::SettableTooltipClient::setTooltip(t);
        invalidateAccessibilityHandler();   // the tooltip IS the accessible name
    }
    const juce::String& accessibleName() const { return rawTip; }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    void resetToDefaultValue() { if (isEnabled()) setValueNotify(defaultValue); }
    bool canTypeIn() const { return typingEnabled; }

private:
    juce::String currentText() const;
    double snap(double v) const;
    void   startEdit();
    void   commitEdit(bool apply);

    double value { 0.0 }, defaultValue { 0.0 };
    double minV { 0.0 }, maxV { 127.0 }, interval { 1.0 };
    bool   typingEnabled { true };
    bool   showChangedTab { true };
    bool   overridePresent { false };
    double overrideValue   { 0.0 };
    bool   hasBarOrigin    { false };
    double barOrigin       { 0.0 };
    bool   dialOwned       { false };
    bool   markable        { false };
    bool   dimmed { false };
    bool   blank  { false };   // see setBlank — suppress the value text entirely
    juce::StringArray options;

    // drag state
    double dragStartValue { 0.0 };
    int    dragStartY { 0 };
    bool   dragged { false };
    bool   tipSuppressed { false };   // see getTooltip / mouseExit
    double pixelsPerStep { 4.0 };
    double dragStep { 1.0 };      // step size in effect for the active drag

    std::unique_ptr<juce::TextEditor> editor;
    juce::String rawTip;   // see setTooltip

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ValueBox)
};
