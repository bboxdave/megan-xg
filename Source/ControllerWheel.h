// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "Accessibility.h"
#include "EditorColours.h"
#include <cmath>
#include <functional>

//==============================================================================
// ControllerWheel — a vertical synth-style controller wheel (XGEdit manual
// §8.4.2). Dragging it sets a normalised position 0..1 and fires onMove, which
// the Controller module turns into the appropriate continuous-controller MIDI
// message for the currently-selected source.
//
//   * spring-loaded mode (pitch bend): the wheel self-centres to 0.5 on release.
//   * bipolar mode draws a centre line; otherwise the rest position is the bottom.
//
// The mouse wheel moves it in the 0..127 grid the non-bend sources actually send:
// 4 per detent, 1 with Shift held (the same "Shift = one step" law the knobs and
// value boxes use). In spring mode the wheel is ignored — a detent would spring
// straight back to centre on the next mouse event, so it would do nothing useful.
//
// The position is set programmatically (per source) via setValueQuiet().
//==============================================================================
class ControllerWheel : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    // Accessibility pillar 2: an accessible ELEMENT is focusable (see ValueBox's
    // constructor for the rule). A screen reader reaches a control by moving
    // KEYBOARD focus to it, so a handler on a component that refuses focus can
    // never be read.
    ControllerWheel() { setWantsKeyboardFocus(true); }

    std::function<void(double)> onMove;   // normalised 0..1, during drag / spring-return

    void setSpring(bool s)  { spring  = s; }
    void setBipolar(bool b) { bipolar = b; repaint(); }

    void setValueQuiet(double norm)
    {
        pos = juce::jlimit(0.0, 1.0, norm);
        repaint();
    }
    double getValue() const { return pos; }

    void mouseDown(const juce::MouseEvent& e) override
    {
        // Right-click resets to the default rest position, like the other controls.
        // Spring (pitch-bend) mode self-centres on release, so it's left alone.
        if (e.mods.isRightButtonDown())
        {
            if (! spring)
            {
                pos = kRestPos;            // non-bend wheels rest at the bottom
                repaint();
                if (onMove) onMove(pos);
            }
            return;
        }

        dragStartPos = pos;
        dragStartY   = e.position.y;
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.mods.isRightButtonDown()) return;   // right-click is reset, not a drag
        const float h = juce::jmax(1.f, (float) getHeight() - 8.f);
        pos = juce::jlimit(0.0, 1.0, dragStartPos + (double) (dragStartY - e.position.y) / h);
        repaint();
        if (onMove) onMove(pos);
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        nudge((w.deltaY != 0.0f) ? (w.deltaY > 0 ? 1 : -1) : (w.deltaX > 0 ? 1 : -1),
              e.mods.isShiftDown());
    }

    //== keyboard control (see KeyNav.h) =======================================
    // One detent's worth, shared by the wheel above and the arrow keys.
    //
    // Ignored in SPRING (pitch bend) mode, which is also why the keyboard cursor skips
    // this control entirely there -- see isSpring(). The mode's whole behaviour is that
    // it returns to centre when you let go, and a key press has no "let go": arrows
    // would leave the part bent, a state no mouse gesture can produce and one that
    // PANIC deliberately does not clear (it sends no CC121). Note right-click already
    // declines here for the same reason, so in that mode there is nothing for any key
    // to do.
    void nudge(int dir, bool fine)
    {
        if (spring || dir == 0) return;

        // Step on the 7-bit grid the source is transmitted on, so one press is a whole
        // number of controller values rather than a fraction that rounds away.
        const int step  = fine ? 1 : kWheelStep;
        const int v     = juce::jlimit(0, 127, (int) std::lround(pos * 127.0) + dir * step);
        const double np = (double) v / 127.0;
        if (np == pos) return;
        pos = np;
        repaint();
        if (onMove) onMove(pos);
    }

    // What a right-click does: back to the rest position. Declines in spring mode,
    // which has no resting value of its own to go to.
    void resetToRest()
    {
        if (spring || pos == kRestPos) return;
        pos = kRestPos;
        repaint();
        if (onMove) onMove(pos);
    }

    bool isSpring() const { return spring; }

    // See ValueBox::setTooltip -- an accessible name must not vanish mid-edit.
    void setTooltip(const juce::String& t) override { rawTip = t; juce::SettableTooltipClient::setTooltip(t); }

    // Accessibility (pillar 1). The value spoken is the 0..127 the source actually
    // transmits, which is the number the manual and the depth knobs talk in.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return mu100a11y::makeValueHandler(
            *this, rawTip, juce::AccessibilityRole::slider,
            [this] { return juce::String((int) std::lround(pos * 127.0)); },
            [this](const juce::String& t)
            {
                const double np = juce::jlimit(0, 127, t.getIntValue()) / 127.0;
                if (np == pos) return;
                pos = np; repaint();
                if (onMove) onMove(pos);
            },
            [this] { return spring; });   // pitch bend has no value to hold; see nudge()
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (spring)
        {
            pos = 0.5;
            repaint();
            if (onMove) onMove(pos);
        }
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced(2.f);

        // Wheel body — a dark rounded ribbon with a subtle vertical gradient so it
        // reads as a cylinder.
        juce::ColourGradient grad(edcol::kWheelNear, b.getX(), 0,
                                  edcol::kWheelFar, b.getRight(), 0, false);
        grad.addColour(0.5, edcol::kWheelMid);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(b, 3.f);
        g.setColour(edcol::kWheelEdge);
        g.drawRoundedRectangle(b.reduced(0.5f), 3.f, 1.f);

        // Ribs to suggest a rotating wheel surface.
        g.setColour(juce::Colour(0x30000000));
        for (float y = b.getY() + 4.f; y < b.getBottom() - 2.f; y += 4.f)
            g.drawHorizontalLine((int) y, b.getX() + 1.f, b.getRight() - 1.f);

        // Centre reference for bipolar (pitch-bend) mode.
        const float travelTop = b.getY() + 3.f, travelBot = b.getBottom() - 3.f;
        if (bipolar)
        {
            const float cy = (travelTop + travelBot) * 0.5f;
            g.setColour(juce::Colour(0x60ffffff));
            g.drawHorizontalLine((int) cy, b.getX(), b.getRight());
        }

        // Indicator notch at the current position (top = 1.0).
        const float y = travelBot - (float) pos * (travelBot - travelTop);
        g.setColour(edcol::kWheelMark);
        g.fillRect(b.getX(), y - 1.5f, b.getWidth(), 3.f);
    }

private:
    static constexpr double kRestPos { 0.0 };   // non-bend wheel default (bottom)
    static constexpr int    kWheelStep { 4 };   // controller values per wheel detent
    double pos { 0.0 };
    bool   spring { false }, bipolar { false };
    juce::String rawTip;   // see setTooltip
    double dragStartPos { 0.0 };
    float  dragStartY { 0.f };
};
