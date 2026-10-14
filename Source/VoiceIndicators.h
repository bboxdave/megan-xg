// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "EditorColours.h"
#include "EditorTextScale.h"

//==============================================================================
// Shared rendering for the per-voice model + element indicators used in the
// Voice browser and the Part-list "Voice" column, modelled on XGEdit:
//   * a small coloured rectangle = the model the voice was added in
//       0 XG Standard  -> blank (just reserved space, names stay left-aligned)
//       1 MU80         -> light green (a touch lighter than XGEdit's, so it
//                          reads clearly apart from the MU90 blue)
//       2 MU90         -> light blue
//       3 MU100        -> pink
//   * a small orange circle after it if the voice uses 2 elements (XGEdit draws
//     a black square; an orange circle reads better on our dark panels and is
//     clearly distinct from the rectangular model swatches).
// A fixed-width gutter is always reserved so names line up whether or not a
// voice has any indicator.
//==============================================================================
namespace voiceind
{
    constexpr int kGutter = 23;   // reserved width before the name

    inline juce::Colour modelColour(int model)
    {
        switch (model)
        {
            case 1:  return juce::Colour(0xff8fe38a);   // MU80  light green
            case 2:  return juce::Colour(0xff86c5ff);   // MU90  light blue
            case 3:  return juce::Colour(0xffff86c9);   // MU100 pink
            default: return juce::Colour(0x00000000);   // XG    blank
        }
    }

    inline const juce::Colour elementColour() { return juce::Colour(0xffffa500); }  // orange

    // Draw the indicators within `gutter` (a kGutter-wide slot at the row's left).
    inline void draw(juce::Graphics& g, juce::Rectangle<int> gutter, int model, int elements)
    {
        const int h = gutter.getHeight();
        if (model > 0)
        {
            const juce::Rectangle<int> r(gutter.getX() + 1, gutter.getY() + (h - 10) / 2, 10, 10);
            g.setColour(modelColour(model));
            g.fillRect(r);
            g.setColour(juce::Colour(0x60000000));
            g.drawRect(r, 1);
        }
        if (elements >= 2)
        {
            const float d = 6.f;   // a touch smaller than the model swatches
            g.setColour(elementColour());
            g.fillEllipse((float) gutter.getX() + 13.5f, (float) gutter.getY() + (h - d) * 0.5f, d, d);
        }
    }

    // A compact legend (model swatches + a 2-element square), laid out left to
    // right within `area`.
    inline void drawLegend(juce::Graphics& g, juce::Rectangle<int> area)
    {
        struct Item { bool circle; juce::Colour colour; const char* label; };
        const Item items[] = {
            { false, modelColour(1), "MU80 Addition" },
            { false, modelColour(2), "MU90 Addition" },
            { false, modelColour(3), "MU100 Addition" },
            { true,  elementColour(), "2 Elements" },
        };
        const int cy = area.getCentreY();
        int x = area.getX();
        const int labelW = 116;   // room for "MU100 Addition" at the larger font
        g.setFont(edtext::font(edtext::Label, 12.f));   // +20% vs the old 10px
        for (const auto& it : items)
        {
            if (it.circle)
            {
                g.setColour(it.colour);
                g.fillEllipse((float) x + 1.f, (float) cy - 3.f, 6.f, 6.f);
            }
            else
            {
                g.setColour(it.colour);
                g.fillRect(juce::Rectangle<int>(x, cy - 5, 10, 10));
                g.setColour(juce::Colour(0x60000000));
                g.drawRect(juce::Rectangle<int>(x, cy - 5, 10, 10), 1);
            }
            x += 14;
            g.setColour(edcol::kTextLabel);
            g.drawText(it.label, x, area.getY(), labelW, area.getHeight(),
                       juce::Justification::centredLeft);
            x += labelW;
        }
    }
}
