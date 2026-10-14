// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "EditorColours.h"
#include "EditorTextScale.h"

//==============================================================================
// xgbracket — the XGEdit-style parameter-group bracket: a thin horizontal line
// spanning [left, right] with a centred label breaking its middle, and short
// downward ticks at each end pointing at the value boxes underneath. Used to mark
// that a run of adjacent parameters belong to one group (LFO, EQ, VIBRATO,
// FILTER, EFFECT). `lineY` is the y of the horizontal line; ticks drop below it.
//
// The label is deliberately a touch lighter and grayer than the parameter labels
// so it reads as a grouping annotation rather than a control name.
//==============================================================================
namespace xgbracket
{
    inline void draw(juce::Graphics& g, int left, int right, int lineY,
                     const juce::String& label)
    {
        const juce::Colour col = edcol::kBracket;             // grayer than the edcol::kTextLabel param labels
        const juce::Font   font (edtext::font (edtext::Label, 12.f));   // lighter (not bold) than the 11px bold labels
        const int cx    = (left + right) / 2;
        const int textW = juce::GlyphArrangement::getStringWidthInt (font, label);
        const int pad   = 4;

        // Raise the label + horizontal line for a little clearance above the value
        // boxes, and lengthen the end ticks by the same amount so they still
        // terminate at their original position.
        const int nudge      = 6;
        const int tickBottom = lineY + 4;      // unchanged tick end
        const int y          = lineY - nudge;  // raised line + label

        const int gapL = cx - textW / 2 - pad;
        const int gapR = cx + textW / 2 + pad;

        g.setColour (col);
        if (gapL > left)  g.drawHorizontalLine (y, (float) left, (float) gapL);
        if (right > gapR) g.drawHorizontalLine (y, (float) gapR, (float) right);
        g.drawVerticalLine (left,      (float) y, (float) tickBottom);
        g.drawVerticalLine (right - 1, (float) y, (float) tickBottom);

        g.setFont (font);
        g.drawText (label, gapL, y - 8, gapR - gapL, 16, juce::Justification::centred);
    }
}
