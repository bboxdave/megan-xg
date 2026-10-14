// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// UndoArrowButton.h — the lower strip's Undo / History / Redo buttons.
//
// A square rounded-rect button carrying a HOOKED arrow, in the shape Arturia Pigments
// uses: a stem rising from the bottom right, a rounded right-angle turn, a straight
// horizontal shaft running left, and an open chevron head at the end of it. Mirrored
// for redo.
//
// It replaced a sweeping circular arc (the Cubase/Bitwig shape) on a plain legibility
// judgement: at this size the arc's head sits at an odd angle and the whole glyph reads
// as a circle first and an arrow second, whereas the straight shaft points somewhere
// unmistakable and the right angle keeps the two directions distinct at a glance.
//
// The glyph is drawn ONCE and mirrored for redo, rather than described twice. That is
// what makes the pair read as a pair — two hand-drawn arrows are never quite symmetric,
// and the eye notices on a 24 px icon. Everything derives from the bounds, so the
// button can be resized without the arrow drifting off centre.
//
// The HISTORY button — the three-line "hamburger" Pigments puts between the arrows —
// is the same class rather than a second one, for the same reason redo is a mirror
// rather than a second drawing: the three share one rounded rect, one set of state
// colours and one disabled alpha, so the trio cannot drift as any of those is tuned.
// Only the glyph differs.
//
// Colours come from the LookAndFeel's TextButton ids, so these sit beside the plain
// TextButtons in the same strip without introducing a custom LookAndFeel.

#pragma once
#include <JuceHeader.h>
#include "EditorColours.h"

class UndoArrowButton : public juce::Button
{
public:
    enum Glyph { Undo, Redo, History };

    explicit UndoArrowButton(Glyph g)
        : juce::Button(g == Redo ? "Redo" : g == History ? "History" : "Undo"), glyph(g) {}

    // The tooltip names the specific edit ("Undo: Part 5 VOLUME 100 -> 64"), so it
    // changes as the stack moves. juce::Button is already a SettableTooltipClient with
    // a virtual getTooltip(), so overriding is all this takes — the same approach
    // PartListPanel uses for its painted columns.
    void setDynamicTooltip(juce::String s) { tip = std::move(s); }
    juce::String getTooltip() override { return tip; }

protected:
    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced(0.5f);
        auto base = getLookAndFeel().findColour(juce::TextButton::buttonColourId);
        if (down)      base = base.contrasting(0.20f);
        else if (over) base = base.brighter(0.15f);

        g.setColour(isEnabled() ? base : base.withMultipliedAlpha(0.45f));
        g.fillRoundedRectangle(b, 4.0f);
        g.setColour(edcol::kArrowShadow.withAlpha(0.45f));
        g.drawRoundedRectangle(b, 4.0f, 1.0f);

        const float cx = b.getCentreX();
        const float cy = b.getCentreY();
        const float s  = juce::jmin(b.getWidth(), b.getHeight());
        const float box = s * 0.66f;                    // the glyph's own square
        const float t  = juce::jmax(1.6f, s * 0.085f);  // stroke thickness

        // The hamburger: three bars in the same box and at the same stroke weight as
        // the arrows, so the trio reads as one set. The outer bars sit on the box's
        // edges and the middle one on its centre, which is what keeps the gaps equal
        // at any size.
        if (glyph == History)
        {
            const float w = box, x = cx - w * 0.5f;
            g.setColour(edcol::kArrow.withAlpha(isEnabled() ? 0.92f : 0.35f));
            for (int i = 0; i < 3; ++i)
                g.fillRoundedRectangle(x, cy + ((float) i - 1.0f) * (box * 0.5f) - t * 0.5f,
                                       w, t, t * 0.5f);
            return;
        }

        // Unit coordinates within that square, x right / y down. The x values already
        // carry the centring offset: the drawn shape spans x 0.13..0.87 by itself, so
        // laying it out from 0.06 and shifting would put it left of centre. The head is
        // ABOVE the vertical middle, which is where a hooked arrow wants it — the stem
        // is what fills the lower half.
        auto px = [cx, box](float u) { return cx + (u - 0.5f) * box; };
        auto py = [cy, box](float v) { return cy + (v - 0.5f) * box; };

        constexpr float tipX = 0.13f, shaftY = 0.34f;   // arrow tip
        constexpr float turnX = 0.69f, stemX = 0.87f;   // the right-angle turn
        constexpr float stemY = 0.94f;                  // bottom of the stem
        constexpr float barbX = 0.41f;                  // where the chevron's arms end

        juce::Path arrow;
        // The chevron and the shaft are one subpath, drawn head-first: the round join at
        // the tip is what gives the head its point, so the three segments must connect.
        arrow.startNewSubPath(px(barbX), py(shaftY - (barbX - tipX)));   // upper arm, 45 deg
        arrow.lineTo         (px(tipX),  py(shaftY));
        arrow.lineTo         (px(barbX), py(shaftY + (barbX - tipX)));   // lower arm
        arrow.startNewSubPath(px(tipX),  py(shaftY));
        arrow.lineTo         (px(turnX), py(shaftY));
        // Rounded right angle: the control point is the corner the two straights would
        // have met at, so the curve is tangent to both and the radius follows the box.
        arrow.quadraticTo    (px(stemX), py(shaftY), px(stemX), py(shaftY + (stemX - turnX)));
        arrow.lineTo         (px(stemX), py(stemY));

        juce::Path stroked;
        juce::PathStrokeType(t, juce::PathStrokeType::curved,
                                juce::PathStrokeType::rounded)
            .createStrokedPath(stroked, arrow);

        if (glyph == Redo)
            stroked.applyTransform(juce::AffineTransform::scale(-1.0f, 1.0f)
                                       .translated(2.0f * cx, 0.0f));

        g.setColour(edcol::kArrow.withAlpha(isEnabled() ? 0.92f : 0.35f));
        g.fillPath(stroked);
    }

private:
    Glyph        glyph;
    juce::String tip;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UndoArrowButton)
};
