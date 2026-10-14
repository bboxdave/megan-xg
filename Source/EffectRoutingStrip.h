// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "Accessibility.h"
#include "EditorColours.h"
#include "EditorTextScale.h"
#include <vector>

//==============================================================================
// EffectRoutingStrip -- an always-visible readout of WHO OWNS THE INSERTION
// SLOTS, drawn in the Effect module's top strip:
//
//     VAR SYS  .  INS1 OFF  .  INS2 P12
//
// The two insertion slots are the scarcest routing resource on the machine, and
// the only way to find out whether one was free was to point the Effect module's
// unit picker at it and read the PART knob -- twice, losing the unit you were
// editing. This says it without a click, on every screen.
//
// THE ORDER IS THE UNIT PICKER'S, not the scarcity order: Variation, Insertion 1,
// Insertion 2, the tail of the box sitting a few pixels to its left. Reading the
// strip and reading the picker are the same act, so they have to agree.
//
// The strip is a READOUT, not a control: it has no value of its own, everything
// it shows is edited by the PART knob and the CON box a few pixels below it, and
// it is deliberately NOT a keynav stop (keynav::detail::isStandardControl does
// not recognise it, which is the right answer for something with nothing to
// adjust). It does publish itself to the accessibility tree as static text, whose
// spoken value is the same three readings in words.
//
// THE STRIP AND ITS TOOLTIP ARE ONE SENTENCE IN TWO HALVES: the strip says WHERE
// each of the three units is routed, the tooltip says WHAT is loaded in it. Neither
// restates the other -- a tooltip that repeated the readings an inch below the
// cursor would explain nothing -- and between them they answer the two questions
// that otherwise cost a trip through the unit picker for each unit.
//
// THE LABELS ARE ALWAYS "V / I1 / I2", never spelled out. The first version
// measured the full "VAR / INS1 / INS2" form against the space available and fell
// back to the short one when it did not fit -- which sounds adaptive and was not.
//
// WINDOW SIZE HAS NOTHING TO DO WITH IT, and getting that wrong is the trap worth
// recording: this module lives on the design canvas, so its cell widths AND its
// font heights are both design pixels and the single canvas transform scales them
// together. Resizing the window cannot change which form fits. The only thing that
// moves the ratio is `editorTextLabelsPct`, which scales fonts but not cells -- so
// the long form (~237 design px against 177 available) would have appeared ONLY
// below about 75% label text scale, and never at any window size at all.
//
// So the "two widths" bought a second appearance almost nobody would see, at the
// price of a readout that could rename itself under you. A fixed label can be
// learned once (user's call, 2026-09-04). At an extreme text scale it simply clips
// at the module's own edge, as any other cell in this editor does.
//==============================================================================
class EffectRoutingStrip : public juce::Component,
                           public juce::SettableTooltipClient
{
public:
    // Part numbers are 1..32, 0 = OFF -- the PART knob's own display encoding, so
    // the strip and the knob cannot disagree about what "off" looks like. Declared
    // in the order they are drawn. The names are the TOOLTIP's half of the sentence
    // and are never painted; Reverb and Chorus carry none, because neither appears
    // on the strip and a tooltip that named units the strip does not show would be
    // describing something else.
    struct Model
    {
        int  varPart     = 0;
        int  ins1Part    = 0;
        int  ins2Part    = 0;
        bool varIsSystem = false;
        juce::String varName, ins1Name, ins2Name;

        bool operator== (const Model& o) const
        {
            return varPart  == o.varPart  && ins1Part == o.ins1Part
                && ins2Part == o.ins2Part && varIsSystem == o.varIsSystem
                && varName  == o.varName  && ins1Name == o.ins1Name
                && ins2Name == o.ins2Name;
        }
        bool operator!= (const Model& o) const { return ! (*this == o); }
    };

    EffectRoutingStrip()
    {
        // A short explicit title beats the tooltip-derived one (mu100a11y::Handler):
        // the tooltip is a three-unit report and a NAME is spoken on every move.
        setTitle("Effect routing");
        refreshText();
    }

    void setModel(const Model& m)
    {
        if (m == model) return;
        model = m;
        refreshText();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto f = font();
        const auto runs = buildRuns();

        g.setFont(f);
        float x = 0.0f;
        for (const auto& r : runs)
        {
            const auto w = (float) juce::GlyphArrangement::getStringWidthInt(f, r.text);
            g.setColour(r.colour);
            // +1: getStringWidthInt rounds down, and drawText clips rather than
            // overhanging -- the same allowance PartPanel makes for its fitted labels.
            g.drawText(r.text, juce::Rectangle<float>(x, 0.0f, w + 1.0f, (float) getHeight()),
                       juce::Justification::centredLeft, false);
            x += w;
        }
    }

private:
    struct Run { juce::String text; juce::Colour colour; };

    static juce::Font font() { return edtext::font(edtext::Label, 11.0f, juce::Font::bold); }

    // "P12" / "OFF" / "SYS". The state is carried by the WORD, not by the colour
    // (the colours only rank it) -- an assigned slot and a free one have to stay
    // distinguishable in both themes and to a reader who cannot separate the two.
    static juce::String partText(int part) { return part > 0 ? "P" + juce::String(part) : juce::String("OFF"); }

    // NOT kValueOff, which is what "an inert value" means everywhere else in the
    // editor -- and which is wrong HERE for the reason the light-theme attention
    // colours were: those greens are tuned to sit in a value FIELD, and this strip
    // draws straight onto kModuleBg. MEASURED against each theme's own module
    // ground: kValueOff scores 2.79:1 dark and 1.98:1 light, both under the 3:1 a
    // small mark needs, and OFF is the state the strip spends most of its life in.
    //   label     kTextLabel  10.56 / 8.52
    //   assigned  kValue      11.86 / 12.78
    //   off, sys  kTextDim     5.79 / 4.11
    // The separator keeps kTextFaint and is allowed to fail: it is a spacer, not a
    // reading. Re-measure these if either theme's kModuleBg moves.
    static juce::Colour partColour(int part) { return part > 0 ? edcol::kValue : edcol::kTextDim; }

    std::vector<Run> buildRuns() const
    {
        static const juce::String sep = juce::String::fromUTF8("  \xc2\xb7  ");

        std::vector<Run> runs;
        auto token = [&] (const char* label, const juce::String& value, juce::Colour valueColour)
        {
            if (! runs.empty()) runs.push_back({ sep, edcol::kTextFaint });
            runs.push_back({ juce::String(label) + " ", edcol::kTextLabel });
            runs.push_back({ value, valueColour });
        };

        // The Variation leads because the unit picker lists it there -- and it earns a
        // place at all only because it can BE an insertion: with CON = INS it takes a
        // part like the other two, and someone hunting for a free insertion needs to
        // see that. When it is a system effect it holds no part, which is worth saying
        // out loud -- a token that vanished would read as "I forgot to look", not as
        // "it is not on a part".
        if (model.varIsSystem) token("V", "SYS", edcol::kTextDim);
        else                   token("V", partText(model.varPart), partColour(model.varPart));

        token("I1", partText(model.ins1Part), partColour(model.ins1Part));
        token("I2", partText(model.ins2Part), partColour(model.ins2Part));

        return runs;
    }

    // ONE LINE -- because the editor's default tooltip mode is the BOTTOM BAR
    // (YamahaMU100AudioProcessor::TooltipBottomBar), not a pop-up: the text is
    // printed into a single-line strip that ellipsizes what does not fit, and
    // newlines would render as garbage there. In pop-up mode JUCE lays the same
    // string out against the look-and-feel's max tooltip width and wraps it.
    //
    // IT FITS, MEASURED ON SCREEN. The bottom-bar readout runs from x=528 to 18 px
    // short of the window's right edge, less 158 more while the mute/solo badge is
    // up -- so 525 px at the DEFAULT 1229 px window with the badge showing, which is
    // the tight case. The default tooltip renders 59 characters in 376 px there,
    // i.e. 6.37 px/char at the fixed 14 px font (100% label scale). Worst case is
    // the longest VARIATION name (14 chars: WAH+DISTORTION / TALK MODULATOR) plus
    // twice the longest INSERTION name (13: AURAL EXCITER) = 70 characters = ~446
    // px, leaving ~78 px spare. It starts ellipsizing below a ~1150 px window with
    // the badge up. Re-measure if the bottom bar gains another button, or if any
    // effect table gains a longer name.
    //
    // "Effects:", NOT "Routings:" -- WAH+DISTORTION is a TYPE, and a line reading
    // "Routings of Var: WAH+DISTORTION" says the routing IS that effect. The strip
    // beside it is what carries the routing. Same rule as the DAMP tooltip, whose
    // wording sent a user hunting for a bug that was not there.
    juce::String buildTooltip() const
    {
        auto named = [] (const juce::String& n) { return n.isNotEmpty() ? n : juce::String("?"); };

        juce::String s;
        s << "Effects: Var: " << named(model.varName)
          << "; Ins1: "       << named(model.ins1Name)
          << "; Ins2: "       << named(model.ins2Name);
        return s;
    }

    // What a screen reader says. Deliberately the SAME facts as the painted strip,
    // in the same order and in words: someone reading "VAR SYS" over another's
    // shoulder and the reader speaking it have to be describing one thing.
    juce::String buildSpoken() const
    {
        auto slot = [] (int part) { return part > 0 ? "part " + juce::String(part) : juce::String("off"); };

        juce::String s;
        s << "Variation "     << (model.varIsSystem ? juce::String("system") : slot(model.varPart))
          << ", insertion 1 " << slot(model.ins1Part)
          << ", insertion 2 " << slot(model.ins2Part);
        return s;
    }

    void refreshText()
    {
        setTooltip(buildTooltip());
        spoken = buildSpoken();
        // The handler captures its tooltip by value for the help text, so it has to be
        // rebuilt when that moves -- rare (only when a type or a routing changes) and
        // cheap. The spoken VALUE needs no invalidation: its getter reads `spoken`.
        invalidateAccessibilityHandler();
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return mu100a11y::makeValueHandler(*this, getTooltip(),
                                           juce::AccessibilityRole::staticText,
                                           [this] { return spoken; },
                                           nullptr);
    }

    Model        model;
    juce::String spoken;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EffectRoutingStrip)
};
