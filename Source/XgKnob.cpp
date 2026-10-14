// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "XgKnob.h"
#include "Accessibility.h"
#include "EditorColours.h"
#include "EditorTextScale.h"
#include <cmath>

//==============================================================================
// KnobLookAndFeel — a compact dark rotary knob with a light indicator dot,
// approximating the XGEdit knob look. One shared instance for all XgKnobs.
//==============================================================================
namespace
{
    class KnobLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                              float pos, float startAngle, float endAngle,
                              juce::Slider& slider) override
        {
            const auto bounds = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(2.f);
            const float rOuter = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
            const auto  c  = bounds.getCentre();
            const float angle = startAngle + pos * (endAngle - startAngle);

            // In the ARC style the override rides a band just OUTSIDE the disc, so the
            // disc gives up kArcBand + kArcGap of radius to make room. Nothing else
            // moves: the knob's COMPONENT bounds are untouched, so no panel re-lays out
            // and the label and value box below stay exactly where they were. At the
            // Part module's 44 px dial that is r 20 -> 16.5, which still carries its
            // dot clearly. In the other two styles r is unchanged, to the pixel.
            const auto* ks   = dynamic_cast<const KnobSlider*>(&slider);
            const bool  arcs = midimarks::dialArc()
                            && ks != nullptr && ks->overrideCapable;
            const float r    = arcs ? juce::jmax(6.0f, rOuter - (kArcBand + kArcGap))
                                    : rOuter;

            g.setColour(edcol::kKnobDisc);
            g.fillEllipse(c.x - r, c.y - r, r * 2.f, r * 2.f);
            g.setColour(edcol::kKnobRim);
            g.drawEllipse(c.x - r, c.y - r, r * 2.f, r * 2.f, 1.2f);

            // 0.185, up from the long-standing 0.16 (2026-09-09, the user's call after
            // a Cubase session): both dots read a touch small on this dial. The override
            // ring below is sized FROM this one, so raising it moves the PAIR together
            // and keeps them proportionate to each other rather than drifting apart.
            const float dotR = juce::jmax(1.9f, r * 0.185f);
            const float dist = r - dotR - 2.0f;
            const float dx = c.x + dist * std::sin(angle);
            const float dy = c.y - dist * std::cos(angle);
            // Active knobs get the light-green XGEdit dot; inert/inactive ones
            // (disabled via setBare) get a dim gray near the disc colour so the
            // slot reads as "not in use".
            g.setColour(slider.isEnabled() ? edcol::kKnobDotOn
                                           : edcol::kKnobDotOff);
            g.fillEllipse(dx - dotR, dy - dotR, dotR * 2.f, dotR * 2.f);

            // The MIDI-override mark: where the DEVICE is. Which FORM it takes is the
            // Display Animation style (see MidiOverrideMarks.h) -- an arc sweeping out
            // from the parameter's rest point, or the original hollow ring.
            if (ks != nullptr && ks->overridePos >= 0.0 && slider.isEnabled())
            {
                    const float oa = startAngle
                                   + (float) ks->overridePos * (endAngle - startAngle);
                    if (arcs)
                    {
                        // An ARC from the rest point to the device's position, on the
                        // band the disc just gave up. It says HOW FAR MIDI has moved
                        // the value, not merely where it landed -- which is the whole
                        // reason this style exists: at a small override the arc is a
                        // short stroke at a known radius, where a ring a dot's width
                        // from the value dot reads as one smudged mark.
                        const float ob = startAngle
                                       + (float) ks->overrideOrigin * (endAngle - startAngle);
                        const float ar = r + kArcGap + kArcBand * 0.5f;
                        float a0 = juce::jmin(ob, oa), a1 = juce::jmax(ob, oa);

                        // A FLOOR on the sweep, which is the same rule the two bars
                        // take in edcol::overrideSpan and for the same reason: an
                        // override that lands ON the rest point is a zero-length arc,
                        // and an invisible mark says "no override" -- the one thing it
                        // must never say. A round cap was relied on for this and does
                        // not deliver it: JUCE strokes nothing at all for a degenerate
                        // subpath, so a pan automation returning to C, or any bipolar
                        // parameter the song brings back to centre, drew no arc while
                        // the badge and the key strip both reported the override.
                        // USER-REPORTED 2026-09-18.
                        //
                        // Grown AWAY from the origin so the stub still says which side
                        // of the rest point the device is on; exactly at the rest point
                        // the side is arbitrary and this takes the positive one, as
                        // overrideSpan does. Clamped to the dial's own sweep, so the
                        // floor cannot push the mark off the end of the scale.
                        const float minSweep = kArcMinPx / juce::jmax(1.0f, ar);
                        if (a1 - a0 < minSweep)
                        {
                            if (oa >= ob) a1 = a0 + minSweep;
                            else          a0 = a1 - minSweep;
                            if (a1 > endAngle)   { a1 = endAngle;   a0 = a1 - minSweep; }
                            if (a0 < startAngle) { a0 = startAngle; a1 = a0 + minSweep; }
                        }

                        juce::Path arc;
                        arc.addCentredArc(c.x, c.y, ar, ar, 0.0f, a0, a1, true);
                        g.setColour(edcol::kMidiOverride);
                        // A round cap so a short arc reads as a mark and not a hairline.
                        g.strokePath(arc, juce::PathStrokeType(
                            kArcBand, juce::PathStrokeType::curved,
                            juce::PathStrokeType::rounded));
                    }
                    else if (midimarks::dialDot())
                    {
                        // A RING, not a disc, and smaller than the value dot, so the
                        // two can never be confused for each other at a glance -- the
                        // filled one is always yours.
                        //
                        // Drawn on the same radius, so the pair reads as two positions
                        // on one dial rather than as two rings. Painted LAST: where the
                        // device and the value coincide to within a dot the ring would
                        // otherwise be swallowed, and an override hidden is worse than
                        // one overlapping.
                        const float ox = c.x + dist * std::sin(oa);
                        const float oy = c.y - dist * std::cos(oa);
                        // 0.92 of the value dot, up from 0.85: a HOLLOW ring reads
                        // smaller than a filled disc of the same radius, so matching by
                        // radius alone leaves the ring the weaker-looking of the two.
                        // Still under 1.0, which keeps the filled one recognisable.
                        const float orr = juce::jmax(1.7f, dotR * 0.92f);
                        g.setColour(edcol::kMidiOverride);
                        g.drawEllipse(ox - orr, oy - orr, orr * 2.f, orr * 2.f,
                                      juce::jmax(1.2f, orr * 0.55f));
                    }
                    // The third style draws nothing on the dial: the bar under the
                    // value box carries the mark there.
            }
        }

        // The override arc's stroke width and its clearance from the disc rim, design
        // px. Named because the DISC's radius is derived from them above -- the band
        // and the space it needs are one decision, made once.
        static constexpr float kArcBand = 2.6f;
        static constexpr float kArcGap  = 1.2f;
        // The arc's minimum LENGTH along its own band, in px -- the angular form of
        // the 1.5 px floor edcol::overrideSpan puts on the bars. Measured in px
        // rather than radians so the mark keeps the same size on every dial.
        static constexpr float kArcMinPx = 1.5f;
    };

    KnobLookAndFeel& knobLaf()
    {
        static KnobLookAndFeel laf;
        return laf;
    }
}

//==============================================================================
XgKnob::XgKnob()
{
    knob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    knob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    knob.setRotaryParameters(juce::MathConstants<float>::pi * 1.2f,
                             juce::MathConstants<float>::pi * 2.8f, true);
    knob.setRange(0.0, 127.0, 1.0);
    knob.setLookAndFeel(&knobLaf());
    knob.onValueChange = [this]
    {
        valueBox.setValueQuiet(knob.getValue());
        if (onChange) onChange(knob.getValue());
    };
    knob.onReset = [this] { resetToDefault(); };
    addAndMakeVisible(knob);

    valueBox.setRange(0.0, 127.0, 1.0);
    valueBox.setTypingEnabled(true);
    // Tell the box it is a dial's readout: in the arc style the DIAL carries the
    // override and the box draws nothing (see MidiOverrideMarks.h).
    valueBox.setDialOwned(true);
    valueBox.onChange = [this](double v)
    {
        // Drive the knob (which fires knob.onValueChange -> our onChange).
        knob.setValue(v, juce::sendNotificationSync);
    };
    addAndMakeVisible(valueBox);

    // The XgKnob is the single accessible element for this parameter (see
    // createAccessibilityHandler); its parts must not be announced separately, and
    // by the same rule none of them is a focus stop -- the wrapper is (pillar 2).
    // juce::Slider's own constructor already declines focus, so `knob` needs no line
    // here; the value box is focusable in its own right and must be turned off.
    knob.setAccessible(false);
    valueBox.setAccessible(false);
    nameLabel.setAccessible(false);
    valueBox.setWantsKeyboardFocus(false);
    setWantsKeyboardFocus(true);

    nameLabel.setJustificationType(juce::Justification::centred);
    nameLabel.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
    nameLabel.setColour(juce::Label::textColourId, edcol::kTextLabel);
    addAndMakeVisible(nameLabel);
}

XgKnob::~XgKnob()
{
    knob.setLookAndFeel(nullptr);
}

void XgKnob::setLabel(const juce::String& name)
{
    nameLabel.setText(name, juce::dontSendNotification);
    // The marker is measured from the label TEXT, so a relabel (part <-> drum-edit
    // mode) moves it. Cheap, and only when a marker is actually drawn.
    if (autoMarker != MarkerNone || ccMarker) { resized(); repaint(); }
}

void XgKnob::setTooltip(const juce::String& tip)
{
    // The mouse lands on the dial or the value box, not the XgKnob itself, so the
    // tooltip must live on those children (both are SettableTooltipClients). The copy
    // kept here is the accessible NAME -- the children are hidden from the
    // accessibility tree, so it has nowhere else to come from.
    rawTip = tip;
    knob.setTooltip(tip);
    valueBox.setTooltip(tip);
    // The tooltip IS the accessible name, and it is re-keyed at runtime (the Part
    // module swaps its EQ cells between part and drum-key wording), so the cached
    // handler has to be rebuilt or the name goes stale.
    invalidateAccessibilityHandler();
}

// Accessibility (pillar 1): one slider per parameter, speaking the value the way the
// box DISPLAYS it. Writes go through the value box, which drives the dial and so the
// onChange funnel -- the same path a typed value takes.
std::unique_ptr<juce::AccessibilityHandler> XgKnob::createAccessibilityHandler()
{
    // A BARE slot is an unused grid position with no parameter behind it -- one of the
    // blank discs in an effect's 16-knob row. Role `ignored` keeps it out of the
    // accessibility tree entirely, the same judgement keynav::usable() makes when it
    // refuses to put a cursor there. Without this a screen reader reads ten nameless
    // "0" sliders across the two effect modules (measured).
    if (bare)
        return std::make_unique<mu100a11y::Handler>(*this, juce::AccessibilityRole::ignored,
                                                    juce::String());

    // spokenText(), NOT displayedText(): the value box is hidden from the tree (the
    // wrapper is the one element for this parameter), so its MIDI-override half has
    // nowhere else to come from. Without this the override is spoken on a bare value
    // box -- the Part module's EQ cells -- and silent on every KNOB, which is most of
    // the editor. It is read live, so the mark arriving is a value change to
    // FocusValueWatcher and is announced under the user.
    return mu100a11y::makeValueHandler(
        *this, rawTip, juce::AccessibilityRole::slider,
        [this] { return valueBox.spokenText(); },
        [this](const juce::String& t)
        {
            valueBox.setValueNotify(valueBox.valueFromText ? valueBox.valueFromText(t)
                                                           : t.getDoubleValue());
        },
        [this] { return bare || ! knob.isEnabled(); });
}

void XgKnob::setRange(double lo, double hi, double interval)
{
    knob.setRange(lo, hi, interval);
    valueBox.setRange(lo, hi, interval);
}

void XgKnob::setDefaultValue(double v)
{
    defaultValue = v;
    valueBox.setDefaultValue(v);
}

void XgKnob::setBare(bool bare)
{
    // Hide the value box and make the knob inert; the name label is left to the
    // caller (blank for an unused Effect-Parameters slot, kept for an inactive
    // Effect-module control). The disc still draws so the grid stays intact.
    this->bare = bare;
    invalidateAccessibilityHandler();   // bare = ignored; see createAccessibilityHandler
    setWantsKeyboardFocus(! bare);      // ...and an ignored element is no focus stop
    valueBox.setVisible(! bare);
    knob.setEnabled(! bare);
    knob.setInterceptsMouseClicks(! bare, ! bare);
    onChange = nullptr;
    resized();
    repaint();
}

void XgKnob::setInteractive(bool on)
{
    // Keep the components visible/painting but block input when read-only. The
    // disabled look (dim green value text, gray dial dot) reads as "not editable".
    knob.setEnabled(on);
    knob.setInterceptsMouseClicks(on, on);
    valueBox.setEnabled(on);
    valueBox.setInterceptsMouseClicks(on, on);
}

void XgKnob::setAutomationMarker(int marker)
{
    if (autoMarker == marker) return;
    autoMarker = marker;
    resized();    // the triangle shifts the label; the underline does not
    repaint();
}

void XgKnob::setCcMarker(bool on)
{
    if (ccMarker == on) return;
    ccMarker = on;
    repaint();    // an underline moves nothing, so no re-layout
}

// Marker geometry, all derived from the label's live font so it tracks
// editorTextLabelsPct.
//
// The triangle is EQUILATERAL and "the same height as the label text", i.e. the
// cap height of the (always uppercase) labels rather than the font's full line
// height — 0.62 is the usual cap-height-to-line-height ratio and reads right
// against the glyphs.
XgKnob::MarkerGeom XgKnob::markerGeom(const juce::Label& nameLabel, int autoMarker)
{
    MarkerGeom m {};
    m.valid = false;

    const auto text = nameLabel.getText();
    if (autoMarker == MarkerNone || text.isEmpty() || ! nameLabel.isVisible())
        return m;

    const auto  f  = nameLabel.getFont();
    m.triH  = juce::jmax(4.0f, f.getHeight() * 0.62f);
    m.triW  = m.triH * 0.866f;                       // equilateral: height * sqrt(3)/2
    m.gap   = juce::jmax(2.0f, m.triH * 0.28f);
    m.textW = (float) juce::GlyphArrangement::getStringWidthInt(f, text);

    // Where the glyphs start. Read from the label's JUSTIFICATION rather than
    // assumed centred: the knob name labels are centred, but the Part module's EQ
    // row labels ("GAIN") are right-justified against the grid, and a centred
    // assumption would put their marker adrift by half the label's slack. The
    // Label's own border is honoured for the same reason — it is what pushes
    // right-justified text in from the edge (JUCE's default is 5 px each side).
    {
        const auto  lb     = nameLabel.getBounds().toFloat();
        const auto  border = nameLabel.getBorderSize();
        const float x      = lb.getX() + (float) border.getLeft();
        const float w      = lb.getWidth() - (float) (border.getLeft() + border.getRight());
        const auto  just   = nameLabel.getJustificationType();

        m.textLeft = just.testFlags (juce::Justification::right) ? x + w - m.textW
                   : just.testFlags (juce::Justification::left)  ? x
                                                                 : x + (w - m.textW) * 0.5f;
    }

    // Move the label right by half the marker so triangle + text sit optically
    // centred in the cell — but only when the cell actually has room, or the shift
    // would push a wide label (CUTOFF, CHORUS) into being ellipsized by JUCE.
    const float room = (float) nameLabel.getWidth() - m.textW;
    m.shift = (autoMarker == MarkerTriangle && room >= m.triW + m.gap)
                  ? std::floor((m.triW + m.gap) * 0.5f) : 0.0f;
    m.valid = true;
    return m;
}

void XgKnob::paint(juce::Graphics& g)
{
    // Measured from the label's FINAL bounds, which already include the triangle's
    // shift, so the underline sits under the text wherever the triangle put it.
    paintAutomationMarker(g, nameLabel, autoMarker);
    if (ccMarker)
        paintAutomationMarker(g, nameLabel, MarkerUnderline);
}

// The marker itself, for any label. See the declaration in XgKnob.h: the Part
// module's HPF2 has no knob and calls this directly with its band label.
void XgKnob::paintAutomationMarker(juce::Graphics& g, const juce::Label& nameLabel, int autoMarker)
{
    const auto m = markerGeom(nameLabel, autoMarker);
    if (! m.valid) return;

    paintAutomationMarkerAt(g, nameLabel.getFont(), m.textLeft, m.textW,
                            nameLabel.getBounds().toFloat().getCentreY(),
                            nameLabel.findColour(juce::Label::textColourId), autoMarker);
}

// The drawing itself, from a font and where its glyphs sit. The sizes are the same
// font-derived ones markerGeom uses, recomputed here so a caller without a Label
// (the Graphic Display's screen buttons) gets identical marks.
void XgKnob::paintAutomationMarkerAt(juce::Graphics& g, const juce::Font& f,
                                     float textLeft, float textW, float lineCentreY,
                                     juce::Colour underlineColour, int autoMarker)
{
    // Vertical centre of the CAP-height block, not of the line box: JUCE centres
    // the ascent+descent box, so the visible uppercase glyphs sit slightly above
    // the box centre.
    const float baseline = lineCentreY + (f.getAscent() - f.getDescent()) * 0.5f;

    if (autoMarker == MarkerTriangle)
    {
        const float triH  = juce::jmax(4.0f, f.getHeight() * 0.62f);
        const float triW  = triH * 0.866f;
        const float gap   = juce::jmax(2.0f, triH * 0.28f);
        const float cy    = baseline - triH * 0.5f;
        const float right = textLeft - gap;

        juce::Path tri;
        tri.startNewSubPath(right - triW, cy - triH * 0.5f);
        tri.lineTo(right,                 cy);
        tri.lineTo(right - triW,          cy + triH * 0.5f);
        tri.closeSubPath();

        g.setColour(edcol::kOrange);   // the graphic-display "0" orange
        g.fillPath(tri);
    }
    else if (autoMarker == MarkerUnderline)
    {
        const float y = baseline + juce::jmax(1.0f, f.getDescent() * 0.5f);
        // 2 design px, grown DOWNWARD from the same top edge so the extra weight
        // cannot crowd the glyphs. One px was too faint to read: the editor canvas
        // is scaled to the window (~0.81 at the default size), so a 1 px rule lands
        // on a fraction of a device pixel and antialiases to grey.
        g.setColour(underlineColour);
        g.fillRect(textLeft, y, textW, 2.0f);
    }
}

void XgKnob::setLabelDimmed(bool d)
{
    nameLabel.setColour(juce::Label::textColourId,
                        d ? edcol::kValueOff : edcol::kTextLabel);
}

void XgKnob::setShowDial(bool show)
{
    showDial = show;
    knob.setVisible(show);
    resized();
}

void XgKnob::setMaxDialSize(int px)
{
    maxDialSize = px;
    resized();
}

void XgKnob::setValueQuiet(double v)
{
    knob.setValue(v, juce::dontSendNotification);
    valueBox.setValueQuiet(v);
}

// The MIDI-override mark, in the knob's own value domain. Sets BOTH halves: the
// value box draws the level bar in its right gutter, the dial draws the ring.
// Neither the knob's value nor the box's text changes -- an override is a second
// value being reported, never this control's own.
void XgKnob::setMidiOverride(double v, bool present)
{
    valueBox.setMidiOverride(v, present);
    const double p = present ? (double) valueBox.midiOverrideProportion() : -1.0;
    const double o = (double) valueBox.midiOverrideOriginProportion();
    if (std::abs(knob.overridePos - p) < 1e-9
        && std::abs(knob.overrideOrigin - o) < 1e-9) return;
    knob.overridePos    = p;
    knob.overrideOrigin = o;
    knob.repaint();
}

// The box owns the origin; the dial reads it back through the box, so the bar and the
// arc can never disagree about where a parameter rests.
void XgKnob::setBarOrigin(double raw)
{
    valueBox.setBarOrigin(raw);
    knob.overrideOrigin = (double) valueBox.midiOverrideOriginProportion();
    knob.repaint();
}

void XgKnob::clearBarOrigin()
{
    valueBox.clearBarOrigin();
    knob.overrideOrigin = 0.0;
    knob.repaint();
}

// The box decides whether a bar is drawn at all (it knows the style, and that it is a
// dial's readout); this says the control can be marked and has the room. It reaches
// the DIAL too: only a markable knob gives up radius for the arc band, so a module
// that can never show an arc keeps its dials the size they have always been.
void XgKnob::setOverrideMarkable(bool m)
{
    if (valueBox.isOverrideMarkable() == m) return;
    valueBox.setOverrideMarkable(m);
    knob.overrideCapable = m;
    resized();
    repaint();
}

// See the header: a style change moves our INTERNAL split without moving our bounds,
// so the parent's setBounds() cannot be what triggers it.
void XgKnob::refreshMidiMarkStyle()
{
    resized();
    repaint();
}

void XgKnob::setTextFromValue(std::function<juce::String(double)> fn)
{
    valueBox.textFromValue = std::move(fn);
    valueBox.setValueQuiet(knob.getValue());
}

void XgKnob::setValueFromText(std::function<double(const juce::String&)> fn)
{
    valueBox.valueFromText = std::move(fn);
}

void XgKnob::setCoarseFineStep(double coarse, double fine)
{
    // Dial (rotary) — quantise its drag + wheel.
    knob.coarseStep = coarse;
    knob.fineStep   = fine;

    // Value box — the number on top: drag ~6 px/step, wheel one step per detent.
    valueBox.dragValue = [coarse, fine](double start, double px, bool shift)
    {
        return start + std::round(px / 6.0) * (shift ? fine : coarse);
    };
    valueBox.wheelValue = [coarse, fine](double v, int dir, bool shift)
    {
        return v + dir * (shift ? fine : coarse);
    };
}

void XgKnob::setValueBoxFullRangeDrag()
{
    valueBox.dragValue = [this](double start, double px, bool shift)
    {
        const double lo   = knob.getMinimum();
        const double hi   = knob.getMaximum();
        const double step = knob.getInterval() > 0.0 ? knob.getInterval() : 1.0;

        if (shift)
        {
            // Shift keeps ValueBox's own fine law, reproduced exactly rather than
            // re-tuned: pixels-per-step = clamp(256/steps, 4, 24), x5 for Shift.
            // (Installing dragValue bypasses that code, so it has to be restated.)
            const double steps  = juce::jmax(1.0, (hi - lo) / step);
            const double finePx = juce::jlimit(4.0, 24.0, 256.0 / steps) * 5.0;
            return start + std::round(px / finePx) * step;
        }

        // Proportional, matching the dial: the full range per kNormalSensitivity px.
        // setValueNotify snaps the result onto the interval grid.
        return start + px * (hi - lo) / (double) KnobSlider::kNormalSensitivity;
    };
}

void XgKnob::setValueBoxWheelStep(double coarse, double fine)
{
    // Only the box's wheel: KnobSlider::mouseWheelMove is untouched, so the dial
    // keeps JUCE's proportional-per-detent rotary wheel (see the header).
    valueBox.wheelValue = [coarse, fine](double v, int dir, bool shift)
    {
        return v + dir * (shift ? fine : coarse);
    };
}

void XgKnob::resetToDefault()
{
    knob.setValue(defaultValue, juce::sendNotificationSync);
}

void XgKnob::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (bare) return;   // inert slot: ignore the wheel entirely

    // Fallback for wheel over the name label / gaps (the knob and value box
    // handle wheel over themselves).
    const double step = knob.getInterval() > 0.0 ? knob.getInterval() : 1.0;
    double dir  = (w.deltaY != 0.0f) ? (w.deltaY > 0 ? 1.0 : -1.0)
                                     : (w.deltaX > 0 ? 1.0 : -1.0);
    if (dir == 0.0) return;
    if (dialaction::reversed()) dir = -dir;   // "Invert knob/dial direction" option
    knob.setValue(juce::jlimit(knob.getMinimum(), knob.getMaximum(),
                               knob.getValue() + dir * step),
                  juce::sendNotificationSync);
}

void XgKnob::resized()
{
    auto b = getLocalBounds();
    // The value field is 16 px as it has always been, PLUS the override bar's strip
    // where this control is allowed one and the style draws one. The room comes off
    // the DIAL (centred in what is left), so the value box, the label and this
    // component's own bounds stay where they were and no panel has to know.
    const int nameH  = 14;
    const int valueH = 16 + valueBox.overrideStripHeight();
    valueBox.setBounds(b.removeFromTop(valueH));

    // A triangle marker shifts the label right so marker + text stay centred; it
    // must be applied AFTER the bounds are set, since markerGeom measures against
    // the label's width. Zero for the underline and for a label with no room.
    auto placeLabel = [this](juce::Rectangle<int> r)
    {
        nameLabel.setBounds(r);
        const auto m = markerGeom();
        if (m.valid && m.shift > 0.0f)
            nameLabel.setBounds(r.translated((int) m.shift, 0));
    };

    // No-dial mode: label sits directly beneath the value box (aligns with the
    // stacked value+label controls next to it), and the rotary is left hidden.
    if (! showDial)
    {
        placeLabel(b.removeFromTop(nameH));
        return;
    }

    placeLabel(b.removeFromBottom(nameH));
    int side = juce::jmin(b.getWidth(), b.getHeight());
    if (maxDialSize > 0) side = juce::jmin(side, maxDialSize);
    knob.setBounds(b.withSizeKeepingCentre(side, side));
}
