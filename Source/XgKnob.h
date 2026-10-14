// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <functional>
#include "ValueBox.h"
#include "DialAction.h"
#include "EditUndo.h"
#include "MidiOverrideMarks.h"

//==============================================================================
// KnobSlider — a rotary juce::Slider with two XGEdit behaviours added:
//   * right-click  -> reset to default (fires onReset)
//   * shift+drag   -> fine adjustment (lower drag sensitivity)
//
//   * shift+wheel  -> fine adjustment (exactly one step per detent)
//
// Optionally (coarseStep > 0) the dial quantises drag + wheel to a coarse step,
// or to a fine step when Shift is held — e.g. the TEMPO knob steps by 1 BPM, 0.1
// with Shift. Off by default, so every other knob keeps its plain interval feel:
// JUCE's proportional rotary wheel unshifted, one range interval with Shift.
//==============================================================================
class KnobSlider : public juce::Slider
{
public:
    // Pixels of vertical drag that traverse the WHOLE range. JUCE computes
    // `proportion(valueOnMouseDown) + dy / pixelsForFullDragExtent` for
    // RotaryVerticalDrag, and XgKnob sets no skew, so these are literally the
    // full-range drag distances. Public because XgKnob's value box borrows
    // kNormalSensitivity to match the dial's travel (setValueBoxFullRangeDrag).
    static constexpr int kNormalSensitivity = 250;
    static constexpr int kFineSensitivity   = 2200;  // ~9x slower for exact values

    std::function<void()> onReset;
    double coarseStep { 0.0 };   // 0 = disabled (plain interval stepping)
    double fineStep   { 0.0 };   // used when Shift is held (falls back to coarse if 0)

    // Where the DEVICE sits, 0..1 along the dial, when incoming MIDI has left it
    // somewhere other than this knob's value. Negative = no override. Read by
    // drawRotarySlider, which paints a second dot -- or, in the arc style, an arc from
    // `overrideOrigin` to here. The knob's own dot never moves for incoming MIDI.
    double overridePos { -1.0 };

    // The point the ARC grows out of, 0..1: the parameter's rest position, which is
    // the minimum for an ordinary control and the centre for a bipolar one (see
    // ValueBox::setBarOrigin -- this is the same number, and comes from there).
    double overrideOrigin { 0.0 };

    // Whether this dial can EVER carry an override mark (see XgKnob::
    // setOverrideMarkable). The arc style reserves a band outside the disc, which
    // costs radius, so a knob nothing can mark must not pay it -- most of the editor's
    // knobs are in modules no override reaches.
    bool overrideCapable { false };

    void mouseDown(const juce::MouseEvent& e) override
    {
        tipSuppressed = true;   // editing begins: keep the tooltip down until re-hover
        if (e.mods.isRightButtonDown())
        {
            if (onReset) onReset();
            return;
        }
        dragStart = e.position;   // origin to mirror the drag around when reversed
        setMouseDragSensitivity(e.mods.isShiftDown() ? kFineSensitivity
                                                     : kNormalSensitivity);
        juce::Slider::mouseDown(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        mu100undo::requestSeal();   // end the undo step on release, not 700 ms later
        juce::Slider::mouseUp(e);
    }

    void mouseExit(const juce::MouseEvent& e) override
    {
        tipSuppressed = false;   // cursor left: allow the tooltip again on next hover
        juce::Slider::mouseExit(e);
    }

    // "" while an edit is in progress on this dial, until the cursor re-enters.
    juce::String getTooltip() override
    {
        return tipSuppressed ? juce::String() : juce::Slider::getTooltip();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.mods.isRightButtonDown()) return;   // never drag on right button

        // "Invert knob/dial direction" (option, off by default): mirror the cursor around
        // the mouse-down point so the vertical (and horizontal) drag delta the base
        // Slider derives flips sign. mouseDownPosition is unchanged, so the drag
        // threshold / sensitivity are preserved.
        if (dialaction::reversed())
        {
            juce::Slider::mouseDrag(e.withNewPosition(juce::Point<float>(
                2.0f * dragStart.x - e.position.x,
                2.0f * dragStart.y - e.position.y)));
            return;
        }
        juce::Slider::mouseDrag(e);
    }

    // Snap a drag to the coarse/fine grid (drag-only; setValue keeps the range interval).
    double snapValue(double attempted, juce::Slider::DragMode) override
    {
        if (coarseStep <= 0.0) return attempted;
        const bool shift = juce::ModifierKeys::getCurrentModifiers().isShiftDown();
        const double step = (shift && fineStep > 0.0) ? fineStep : coarseStep;
        const double lo = getMinimum();
        return lo + std::round((attempted - lo) / step) * step;
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wIn) override
    {
        tipSuppressed = true;   // wheel-editing suppresses the tooltip until re-hover

        // "Invert knob/dial direction" (option): negate the wheel deltas up front so both
        // the plain-Slider path and the coarse-step path below reverse together.
        juce::MouseWheelDetails w = wIn;
        if (dialaction::reversed()) { w.deltaX = -w.deltaX; w.deltaY = -w.deltaY; }

        const double dir = (w.deltaY != 0.0f) ? (w.deltaY > 0 ? 1.0 : -1.0)
                                              : (w.deltaX > 0 ? 1.0 : -1.0);

        // Shift = fine, ONE step per detent — the same law Shift already has on the
        // drag and on the value box. Without it a plain rotary detent moves a
        // PROPORTION of the range (juce_Slider's `wheelAmount * 0.15`, ~3.75 % per
        // detent on Windows, i.e. ~5 of 127), which is quick to travel with but can
        // never land on a chosen value. "One step" is the finest the parameter has:
        // the configured fine step where there is one, otherwise the range interval.
        if (e.mods.isShiftDown() && dir != 0.0)
        {
            setValue(juce::jlimit(getMinimum(), getMaximum(), getValue() + dir * oneStep()),
                     juce::sendNotificationSync);
            return;
        }

        if (coarseStep <= 0.0) { juce::Slider::mouseWheelMove(e, w); return; }
        if (dir == 0.0) return;
        setValue(juce::jlimit(getMinimum(), getMaximum(), getValue() + dir * coarseStep),
                 juce::sendNotificationSync);
    }

    //== keyboard control (see KeyNav.h) =======================================
    // The finest move this dial has: the configured fine step, else the coarse one,
    // else the range interval. Same ladder the Shift branch above walks.
    double oneStep() const
    {
        return fineStep   > 0.0 ? fineStep
             : coarseStep > 0.0 ? coarseStep
             : (getInterval() > 0.0 ? getInterval() : 1.0);
    }

    // What ONE unshifted wheel detent moves this dial. `coarseStep` where a knob set
    // one; otherwise juce_Slider's rotary law REPRODUCED, because the real one is
    // only reachable through a MouseWheelDetails: getMouseWheelDelta() takes
    // `wheelAmount * 0.15` as a PROPORTION of the range, JUCE's Windows peer reports
    // one notch as wheelAmount 0.25 -- so a detent is 3.75% of the range -- and the
    // handler then floors the move at the interval. Reproduced rather than shared
    // because juce::Slider exposes neither half; JUCE is pinned at 8.0.13 (see
    // BUILDING.md), so re-check this if that ever moves.
    double oneDetent()   // NOT const: juce::Slider's two proportion helpers are not
    {
        if (coarseStep > 0.0) return coarseStep;
        const double v  = getValue();
        const double iv = getInterval() > 0.0 ? getInterval() : 1.0;
        const double p  = juce::jlimit(0.0, 1.0, valueToProportionOfLength(v) + 0.25 * 0.15);
        return juce::jmax(iv, std::abs(proportionOfLengthToValue(p) - v));
    }

    // An arrow key. Plain travels (one detent), Shift lands (one step) -- the law the
    // wheel and the drag already follow. Deliberately ignores dialaction::reversed():
    // that option says which way to push a MOUSE, while "up increases" is what the
    // key MEANS, and inverting it would leave no way to say "more".
    void nudge(int dir, bool fine)
    {
        if (dir == 0 || ! isEnabled()) return;
        const double step = fine ? oneStep() : oneDetent();
        setValue(juce::jlimit(getMinimum(), getMaximum(), getValue() + dir * step),
                 juce::sendNotificationSync);
    }

private:
    bool tipSuppressed { false };
    juce::Point<float> dragStart;   // mouse-down origin, for reversed-drag mirroring
};

//==============================================================================
// XgKnob — the reusable XGEdit-style control: a ValueBox on top, a rotary knob
// in the middle, and a name label below. The ValueBox and knob stay in sync, so
// the value can be changed by:
//   * dragging the knob (Shift = fine) or right-clicking it to reset
//   * dragging the value box up/down, mouse-wheel, type-in, or right-click reset
//
// Display/parse can be customised (e.g. pan "L63/C/R63", note "+/-24") via
// setTextFromValue / setValueFromText.
//==============================================================================
class XgKnob : public juce::Component
{
public:
    XgKnob();
    ~XgKnob() override;

    void setLabel(const juce::String& name);

    // Set the hover tooltip, forwarded to the interactive children (dial + value
    // box) so it shows wherever the mouse actually lands on the control. Empty =
    // no tooltip.
    void setTooltip(const juce::String& tip);

    void setRange(double lo, double hi, double interval = 1.0);
    void setDefaultValue(double v);

    // "Bare" = an unused knob position: just the disc, no value box or label and
    // inert to the mouse (XGEdit draws these for the blank slots in an effect's
    // 16-knob grid). Call setBare(false) to restore a normal knob.
    void setBare(bool bare);
    bool isBare() const { return bare; }

    // Hide the rotary dial, leaving just the value box with the name label
    // directly beneath it (a compact value-only control that still keeps the
    // XgKnob range/formatting/wiring — the value box drives the hidden slider).
    void setShowDial(bool show);

    // Cap the rotary dial's diameter (design px). By default the dial fills the
    // available cell; a cap makes it smaller (centred, with the value box + label
    // unchanged) so modules with roomy cells can show a more compact knob. 0 =
    // uncapped.
    void setMaxDialSize(int px);

    // Set the displayed value without firing onChange (for sync from device state).
    void setValueQuiet(double v);
    // See the .cpp: marks where the DEVICE is when incoming MIDI moved it away from
    // this control's value. Never moves the control.
    void setMidiOverride(double v, bool present);

    // The value an override mark grows out of -- the bar under the box and the arc on
    // the dial alike. Forwarded to the value box, which owns the number; the dial reads
    // it back from there. See ValueBox::setBarOrigin.
    void setBarOrigin(double raw);
    // ...and drop it again, for a knob re-skinned to a parameter that has no rest
    // point (the Part module's main row swaps between MULTI PART and DRUM SETUP).
    void clearBarOrigin();

    // This control can carry a MIDI-override mark, and its layout has granted the
    // room: the bar strip under the value box in one style, the arc band around the
    // dial in another. Opt-in and false by default, so the modules that can never
    // carry a mark keep both their geometry and their dial size.
    void setOverrideMarkable(bool m);

    // Re-lay-out for a change of mark style. Needed even though nothing about our
    // BOUNDS changes: a parent's resized() that hands us the same rectangle does not
    // call ours, so the value box and the dial would keep the outgoing style's split.
    void refreshMidiMarkStyle();
    double getValue() const { return knob.getValue(); }

    // Grey the value readout without disabling the knob (still interactive) — used
    // by tempo-sync knobs for a note that is out of range at the current tempo.
    void setValueDimmed(bool d) { valueBox.setDimmed(d); }

    // Suppress the value readout entirely (frame still paints) — see
    // ValueBox::setBlank. The rotary dot needs no companion call: it already
    // greys to the disc colour whenever the slider is disabled, which is what
    // setInteractive(false) does.
    void setValueBlank(bool b) { valueBox.setBlank(b); }

    // Read-only-but-visible: ignore mouse/wheel/type-in yet keep painting the
    // value (greyed via the disabled look). Used for the hosted TEMPO knob, whose
    // value is driven by the DAW and must not be user-edited.
    void setInteractive(bool on);

    // Mark this knob's parameter as DAW-automatable (Options ▸ "Mark DAW-automatable
    // params"). The marker is drawn by XgKnob itself, next to / under the NAME label:
    //   0 = none, 1 = a small orange right-pointing triangle before the label,
    //   2 = an underline beneath the label in the label's own colour.
    // These values are NOT the processor's AutomationMarker values: 0 here must mean
    // "no marker" so an unconfigured control draws none, while the setting's 0 must
    // be its default (Triangles). EditorView::refreshAutomationMarkers translates,
    // and is also where the standalone is forced to MarkerNone.
    //
    // SINCE 2026-10-04 THE TWO GLYPHS MEAN DIFFERENT THINGS and are set separately: the
    // TRIANGLE is the DAW mark (a host lane exists), the UNDERLINE is the MIDI CC mark
    // (setCcMarker -- this edit records as a controller in MIDI CC mode). They used to
    // be two STYLES of the one DAW mark. A label can carry both: the triangle sits to
    // its left, the underline beneath, and neither moves the other.
    enum AutoMarker { MarkerNone = 0, MarkerTriangle = 1, MarkerUnderline = 2 };
    void setAutomationMarker(int marker);
    void setCcMarker(bool on);

    // Draw the same marker beside/under an ORDINARY label — for the automatable
    // controls that are not XgKnobs. The Part module's HPF2 is a bare ValueBox whose
    // name lives in the EQ grid's band-label row, so it has no knob to ask; it calls
    // this from its panel's paint() with that label. Static and label-driven so the
    // two surfaces cannot drift apart: same geometry, same orange, same font-derived
    // sizing. `label` must be positioned already (the geometry is measured from its
    // live bounds and font) and a child of the component doing the painting.
    static void paintAutomationMarker(juce::Graphics&, const juce::Label& label, int marker);

    // The same marker against text that is NOT in a Label: the Graphic Display's
    // screen buttons, whose text the LookAndFeel draws. The caller says where the
    // glyphs are (`textLeft`, `textW`, and the vertical centre of the font's line
    // box `lineCentreY`); `underlineColour` is the text's own colour. The Label
    // overload above is this plus the Label's geometry, so the two cannot differ.
    static void paintAutomationMarkerAt(juce::Graphics&, const juce::Font& font,
                                        float textLeft, float textW, float lineCentreY,
                                        juce::Colour underlineColour, int marker);

    // Grey the NAME label. Deliberately separate from setInteractive: a knob can be
    // read-only for a session reason (the hosted TEMPO knob) without the parameter
    // itself being inapplicable, and only the latter should dim its name. Used with
    // setInteractive(false) to mark a parameter the device ignores on the current
    // part (the drum-part exclusions — see PartPanel::updateDrumKitGating).
    void setLabelDimmed(bool d);

    // Fired when the user changes the value (drag / wheel / typed entry / reset).
    std::function<void(double)> onChange;

    // Optional custom display / parse (forwarded to the value box).
    void setTextFromValue(std::function<juce::String(double)> fn);
    void setValueFromText(std::function<double(const juce::String&)> fn);

    // Quantise drag + wheel to `coarse`, or to `fine` when Shift is held — applied to
    // BOTH the value box and the rotary dial (e.g. TEMPO: 1 BPM, 0.1 with Shift). The
    // range interval should be <= fine so the fine values survive (see SystemPanel).
    void setCoarseFineStep(double coarse, double fine);

    // Make a plain drag on the VALUE BOX cover the full range in the same pixel
    // distance as a plain drag on the dial (KnobSlider::kNormalSensitivity).
    //
    // The generic ValueBox law is pixels-per-STEP, floored at 4 px, so a wide range
    // needs an unusable drag to cross: MASTER TUNE (2048 steps) takes ~8200 px on
    // the box versus 250 px on its dial. This makes the box proportional instead.
    // Shift-drag keeps the generic fine law untouched, and the wheel is not
    // involved at all. Reads the range at drag time, so call order doesn't matter.
    void setValueBoxFullRangeDrag();

    // Give the VALUE BOX its own wheel step, in raw value units. The dial's wheel is
    // left alone, and note it is a different animal: JUCE steps a rotary slider by a
    // PROPORTION of the range per detent (`wheelAmount * 0.15`), not by the interval,
    // so on a wide range the dial moves far more per detent than the box does. Use
    // this where the raw unit is finer than the unit a user thinks in: System TUNE
    // stores 0.1 cent, so the box wheels a whole cent per detent while Shift still
    // reaches the 0.1 grid.
    void setValueBoxWheelStep(double coarse, double fine);

    //== keyboard control (see KeyNav.h) =======================================
    // Arrow keys drive the DIAL, not the value box. The dial's law is the knob's
    // primary one -- plain travels a proportion of the range, Shift lands on a step
    // -- and it is the same pair the mouse gets. (The two differ only on the handful
    // of knobs given setValueBoxWheelStep; there the keyboard follows the dial.)
    void nudge(int dir, bool fine) { knob.nudge(dir, fine); }
    void beginTypeIn()             { valueBox.beginTypeIn(); }
    void resetToDefault();         // what a right-click on the dial does
    bool isInteractive() const     { return knob.isEnabled(); }

    // With the dial hidden an XgKnob IS a value box with a label under it, and its
    // BOUNDS still span the space the dial would have used. The keyboard cursor needs
    // the box: it is what the control looks like, it is what sorts into reading order
    // alongside the plain value boxes beside it (the wrapper's centre sits a row
    // lower), and it is what the focus mark should frame.
    bool isDialShown() const                  { return showDial; }
    juce::Rectangle<int> valueBoxBounds() const { return valueBox.getBounds(); }

    // Accessibility (pillar 1). ONE element per parameter: the XgKnob is it, and its
    // dial, value box and label are marked inaccessible in the constructor -- three
    // elements for one parameter would make a screen reader read every knob three
    // times. Same reason keynav::collect treats an XgKnob as a leaf.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    void resized() override;
    void paint(juce::Graphics&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:

    // Geometry of the automation marker, measured from the label's REAL font (see
    // "Editor text scaling" — a fixed size would drift the moment the label
    // percentage moves). `textW` is the measured label width and `textLeft` where
    // the glyphs actually start (which depends on the label's justification, not
    // just its bounds — the Part module's EQ row labels are right-justified);
    // `shift` is how far right the label is moved so triangle + text stay optically
    // centred, 0 when there is no room for it.
    struct MarkerGeom { float triW, triH, gap, textW, textLeft, shift; bool valid; };
    static MarkerGeom markerGeom(const juce::Label& label, int marker);
    MarkerGeom markerGeom() const { return markerGeom(nameLabel, autoMarker); }

    KnobSlider  knob;
    ValueBox    valueBox;
    juce::String rawTip;   // the name a screen reader speaks; see setTooltip
    juce::Label nameLabel;
    double      defaultValue { 0.0 };
    bool        bare { false };      // inert/grayed slot — see setBare
    int         autoMarker { MarkerNone };   // DAW-automatable marker — see setAutomationMarker
    bool        ccMarker   { false };        // MIDI CC marker (underline) — see setCcMarker
    bool        showDial { true };   // false = value box + label only, no rotary
    int         maxDialSize { 0 };   // dial diameter cap in px (0 = uncapped)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(XgKnob)
};
