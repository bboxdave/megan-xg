// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>

//==============================================================================
// hwpanel — the Hardware view's front panel, drawn in code.
//
// This replaces megan-xg-hardware-view.png, which could not ship: it was derived
// from a photograph whose copyright survives the edits. Everything below is drawn in
// the ORIGINAL picture's 2000 px coordinate space ("art space"), which is also the
// space of the editor's k_buttons / LCD / All Macro tables, so the two cannot drift.
// The image itself is that space cropped: it starts kArtX0 in, which trims the left
// margin (empty once the knobs and jacks went) to match the one right of the button
// column. Art x -> image x is `x - kArtX0`; the editor applies the same offset.
// Anything drawn here that has a hit target must stay where that table says it is.
//
// Deliberately left out of the picture (purely decorative on the original): the
// knobs, jacks and their labels left of the LCD, and the GENERAL MIDI / XG logos.
//
// Text is placed by its INK box, not by a font size: each label is a glyph path
// stretched to fill the measured box of the original lettering. That keeps the
// layout identical whatever font the platform substitutes for Arial.
//==============================================================================
namespace hwpanel
{
// Equal margins: 428 - kArtX0 on the left of the beige panel, and kWidth minus the
// navigation keys' right edge (art x 1956, i.e. 1671 in the image) on the right.
static constexpr int kArtX0  = 285;
static constexpr int kWidth  = 1814;
static constexpr int kHeight = 458;

namespace detail
{
    inline juce::Colour grey(int v) { return juce::Colour((juce::uint8) v, (juce::uint8) v, (juce::uint8) v); }

    // Panel palette, sampled from the original art.
    inline const juce::Colour kBody       = grey(58);
    inline const juce::Colour kBeige      { 192, 182, 158 };
    inline const juce::Colour kBeigeTop   { 201, 193, 173 };
    inline const juce::Colour kBeigeLow   { 188, 178, 153 };
    inline const juce::Colour kInkDark    {  64,  61,  53 };   // labels on the beige panel
    inline const juce::Colour kInkTitle   {  68,  68,  66 };   // "MEGAN XG"
    inline const juce::Colour kInkLight   = grey(236);         // labels on the dark body
    inline const juce::Colour kRed        { 203,   1,  34 };

    // Fill the ink box [x0,y0]-[x1,y1] (inclusive pixel bounds, as measured) with `text`.
    // `weight` > 0 strokes the outline as well, to thicken a regular face toward
    // the medium weight the panel lettering uses.
    inline void label(juce::Graphics& g, const juce::String& text,
                      float x0, float y0, float x1, float y1, bool bold = false,
                      float weight = 0.f)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText(juce::Font(juce::FontOptions("Arial", 100.f,
                                                      bold ? juce::Font::bold : juce::Font::plain)),
                         text, 0.f, 0.f);
        juce::Path p;
        ga.createPath(p);
        const juce::Rectangle<float> box(x0, y0, x1 - x0 + 1.f, y1 - y0 + 1.f);
        p.applyTransform(p.getTransformToScaleToFit(box.reduced(weight * 0.5f), false));
        g.fillPath(p);
        if (weight > 0.f)
            g.strokePath(p, juce::PathStrokeType(weight));
    }

    // The round white badge either side of PART / SELECT / VALUE, with its symbol
    // knocked out in the body colour: '-' minus, '+' plus, '<' / '>' a triangle.
    inline void badge(juce::Graphics& g, float cx, float cy, char kind)
    {
        const float r = 9.5f;
        g.setColour(kInkLight);
        g.fillEllipse(cx - r, cy - r, 2.f * r, 2.f * r);
        g.setColour(kBody);
        const float arm = 5.5f, half = 1.4f;
        if (kind == '-' || kind == '+')
            g.fillRect(cx - arm, cy - half, 2.f * arm, 2.f * half);
        if (kind == '+')
            g.fillRect(cx - half, cy - arm, 2.f * half, 2.f * arm);
        if (kind == '<' || kind == '>')
        {
            const float d = (kind == '<') ? -1.f : 1.f;
            juce::Path t;
            t.addTriangle(cx + d * 5.5f, cy, cx - d * 3.5f, cy - 5.5f, cx - d * 3.5f, cy + 5.5f);
            g.fillPath(t);
        }
    }

    inline void dropShadow(juce::Graphics& g, float cx, float cy, float r, float alpha)
    {
        juce::ColourGradient sh(juce::Colours::black.withAlpha(alpha), cx, cy + 4.f,
                                juce::Colours::transparentBlack, cx, cy + 4.f + r + 6.f, true);
        g.setGradientFill(sh);
        g.fillEllipse(cx - r - 6.f, cy + 4.f - r - 6.f, 2.f * (r + 6.f), 2.f * (r + 6.f));
    }

    // A domed key lit from above: `top` at the crown shading to `bottom`, with a
    // thin dark rim and a soft shadow under it.
    inline void key(juce::Graphics& g, float cx, float cy, float r,
                    juce::Colour top, juce::Colour bottom, juce::Colour rim, float shadow)
    {
        dropShadow(g, cx, cy, r, shadow);
        g.setColour(rim);
        g.fillEllipse(cx - r - 2.5f, cy - r - 2.5f, 2.f * r + 5.f, 2.f * r + 5.f);
        juce::ColourGradient face(top, cx, cy - r, bottom, cx, cy + r, false);
        g.setGradientFill(face);
        g.fillEllipse(cx - r, cy - r, 2.f * r, 2.f * r);
        // Specular crescent along the upper edge.
        juce::Path hl;
        hl.addCentredArc(cx, cy, r - 1.5f, r - 1.5f, 0.f,
                         -juce::MathConstants<float>::pi * 0.38f,
                          juce::MathConstants<float>::pi * 0.38f, true);
        g.setColour(juce::Colours::white.withAlpha(0.28f));
        g.strokePath(hl, juce::PathStrokeType(2.f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    }
}

// The ALL button: Part- and Part+ pressed together, which a mouse cannot do. It is
// drawn as a key-cap-coloured button so it reads as clickable; on the hardware it is
// only a label with a bracket to the two keys. This rect is also the editor's hit
// target, so the drawing and the click area cannot drift apart.
static const juce::Rectangle<float> kAllButton { 1849.f, 58.f, 62.f, 26.f };

// `pressed` inverts the gradient; the editor paints that over the static image while
// the mouse is held.
inline void allButton(juce::Graphics& g, bool pressed)
{
    using namespace detail;
    const auto r = kAllButton;
    g.setColour(grey(12));
    g.fillRoundedRectangle(r.expanded(1.2f), 6.f);
    const juce::Colour hi(232, 222, 203), lo(186, 173, 147);
    const juce::Colour top = pressed ? juce::Colour(170, 158, 132) : hi;
    const juce::Colour bot = pressed ? juce::Colour(205, 194, 170) : lo;
    g.setGradientFill(juce::ColourGradient(top, 0.f, r.getY(), bot, 0.f, r.getBottom(), false));
    g.fillRoundedRectangle(r, 5.f);
    g.setColour(kInkDark);
    label(g, "ALL", r.getCentreX() - 17.f, r.getY() + 6.f,
                    r.getCentreX() + 17.f, r.getY() + 20.f, true, 0.6f);
}

inline juce::Image render()
{
    using namespace detail;
    juce::Image img(juce::Image::ARGB, kWidth, kHeight, true);
    juce::Graphics g(img);

    // --- Body, with a bevelled edge: a light outer line, a dark groove, a raised ridge.
    g.fillAll(kBody);
    static const int kEdge[] = { 160, 62, 22, 24, 30, 74, 116, 134, 106, 68 };
    for (int i = 0; i < (int) std::size(kEdge); ++i)
    {
        g.setColour(grey(kEdge[i]));
        g.drawRect(i, i, kWidth - 2 * i, kHeight - 2 * i, 1);
    }

    // Everything else is drawn in art space.
    g.setOrigin(-kArtX0, 0);

    // --- Beige centre panel: dark rim, top-lit face, a highlight line under the top edge.
    const juce::Rectangle<float> panel(428.f, 20.f, 1232.f, 417.f);
    g.setColour(grey(6));
    g.fillRoundedRectangle(panel, 14.f);
    const auto face = panel.reduced(2.f);
    g.setGradientFill(juce::ColourGradient(kBeigeTop, 0.f, face.getY(),
                                           kBeigeLow, 0.f, face.getBottom(), false));
    g.fillRoundedRectangle(face, 12.f);
    g.setColour(juce::Colour(222, 214, 195));
    g.drawLine(face.getX() + 10.f, face.getY() + 2.5f, face.getRight() - 10.f, face.getY() + 2.5f, 1.6f);
    g.setColour(juce::Colours::black.withAlpha(0.18f));
    g.drawLine(face.getX() + 10.f, face.getBottom() - 2.f, face.getRight() - 10.f, face.getBottom() - 2.f, 3.f);

    // --- Title and the EMULATOR banner.
    g.setColour(kInkTitle);
    label(g, "MEGAN XG", 498, 36, 742, 67, true);
    g.setColour(kRed);
    g.fillRect(766.f, 38.f, 563.f, 30.f);
    g.setColour(kBeige);
    label(g, "EMULATOR", 783, 42, 960, 62, true);

    // --- LCD window. The editor paints the live display over exactly
    // [499,80]-[1338,367]; this is the recess around it and the unlit glass that
    // shows before the engine has booted.
    const juce::Rectangle<float> lcd(499.f, 80.f, 839.f, 287.f);
    g.setColour(juce::Colour(160, 152, 132));
    g.fillRect(lcd.expanded(2.f));
    g.setColour(juce::Colour(120, 114, 99));
    g.fillRect(lcd.expanded(1.f));
    g.setColour(juce::Colour(118, 134, 92));
    g.fillRect(lcd);

    // --- Labels on the beige panel.
    g.setColour(kInkDark);
    label(g, "XG",      1349, 262, 1373, 274, false, 0.9f);
    label(g, "TG300B",  1348, 291, 1424, 305, false, 0.9f);
    label(g, "C/M",     1348, 313, 1387, 328, false, 0.9f);
    label(g, "PERFORM", 1349, 337, 1442, 352, false, 0.9f);

    struct Word { const char* t; float x0, x1; };
    static const Word kBottom[] = {
        { "PART", 524, 582 }, { "MIDI", 644, 689 }, { "BANK/PGM#", 739, 864 },
        { "VOL", 882, 926 },  { "EXP", 945, 988 },  { "PAN", 1005, 1047 },
        { "REV", 1067, 1111 },{ "CHO", 1126, 1175 },{ "VAR", 1190, 1232 },
        { "KEY", 1254, 1298 },
    };
    for (const auto& w : kBottom)
        label(g, w.t, w.x0, 385, w.x1, 402, false, 0.9f);

    label(g, "PLAY",   1471,  69, 1531,  87, false, 0.9f);
    label(g, "EDIT",   1566,  69, 1618,  87, false, 0.9f);
    label(g, "UTIL",   1474, 172, 1525, 190, false, 0.9f);
    label(g, "EFFECT", 1547, 171, 1638, 190, false, 0.9f);
    label(g, "MODE",   1466, 276, 1537, 295, false, 0.9f);
    label(g, "EQ",     1577, 276, 1608, 295, false, 0.9f);

    // --- Mode keys (black, on the beige panel). Centres match k_buttons; the
    // editor lights these by filling a disc of 0.9 x the 20 px hit radius.
    static const juce::Point<float> kBlack[] = {
        { 1502.f, 126.f }, { 1594.f, 126.f }, { 1502.f, 230.f },
        { 1594.f, 230.f }, { 1502.f, 336.f }, { 1594.f, 336.f },
    };
    for (auto c : kBlack)
        key(g, c.x, c.y, 23.f, grey(62), grey(14), grey(4), 0.45f);

    // --- Navigation keys (beige, on the dark body).
    static const juce::Point<float> kBeigeKeys[] = {
        { 1737.f, 123.f }, { 1833.f, 123.f }, { 1926.f, 123.f },
        { 1737.f, 231.f }, { 1833.f, 231.f }, { 1926.f, 231.f },
        { 1737.f, 341.f }, { 1833.f, 341.f }, { 1926.f, 341.f },
    };
    for (auto c : kBeigeKeys)
        key(g, c.x, c.y, 27.f, juce::Colour(230, 220, 201), juce::Colour(172, 159, 133),
            grey(12), 0.7f);

    // --- Labels on the dark body.
    g.setColour(kInkLight);
    // The top row sits 5 px higher than the original's, to give the ALL button room
    // between PART and the keys; MUTE/SOLO moves with it so the two columns align.
    label(g, "MUTE/",  1697,  34, 1774,  51, true);
    label(g, "SOLO",   1703,  60, 1767,  78, true);
    label(g, "PART",   1855,  34, 1913,  51, true);
    label(g, "ENTER",  1696, 172, 1773, 189, true);
    label(g, "SELECT", 1831, 171, 1923, 189, true);
    label(g, "EXIT",   1710, 281, 1761, 298, true);
    label(g, "VALUE",  1844, 281, 1917, 298, true);

    badge(g, 1835.f,  42.5f, '-');
    badge(g, 1932.5f, 42.5f, '+');
    badge(g, 1813.f, 180.5f, '<');
    badge(g, 1941.f, 180.5f, '>');
    badge(g, 1826.f, 289.5f, '-');
    badge(g, 1936.5f, 289.5f, '+');

    allButton(g, false);

    return img;
}
} // namespace hwpanel
