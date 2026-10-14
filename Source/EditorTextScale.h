// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>

#include <cmath>

//==============================================================================
// EditorTextScale — global, message-thread-only text-size multipliers for the
// XGEdit-style editor view.
//
// The editor lays out on a fixed design canvas (1480x962) that is scaled to the
// window by a single AffineTransform. That makes fonts scale with the window,
// but at any window NARROWER than the design canvas (the default 1200x862 gives
// ~0.81x) the fonts render SMALLER than their design pixels — harder to read
// than the pre-transform fixed-pixel fonts were.
//
// These multipliers let the text categories be enlarged INDEPENDENTLY of that
// uniform transform, so parameter values / control labels / module titles / voice
// names can each be nudged up to taste. They are loaded from settings.xml at
// startup (see PluginProcessor: editorTextValuesPct / ...LabelsPct / ...TitlesPct
// / ...VoiceListPct / ...GrDisValuePct).
//
// A built-in BASELINE enlargement (kBaseline) is applied on top of the settings
// percentage, so the tuned default look is what a user sees at the 100% setting
// (settings.xml can list a clean 100 for every category yet the GUI still renders
// ~30-35% larger than the raw design-pixel sizes). The percentage then multiplies
// that baseline: effective height = basePx * kBaseline[cat] * (pct / 100).
//
// NOTE: this scales the FONTS only, not the value boxes / cells that hold them
// (those ride the canvas transform). A large multiplier can therefore overflow a
// box; keep it modest (the settings are clamped 50..400%) and tune per-category.
//==============================================================================
namespace edtext
{
    // VoiceList = the assignable voice NAMES (Voice module rows + the voice
    // browser's list items, all three columns). Split out from Label so the names
    // can be sized independently of other control labels.
    //
    // GrDisValue = the Graphic Display's value readouts ONLY — the numbers drawn
    // next to the EG points, the EQ nodes and the bars (both the always-on EG
    // readouts and the hover/drag popups). Split out from Value because those sit
    // on the LCD amongst the plot rather than inside a value box, so they want
    // their own size: they are drawn a point larger than a box's value by default
    // (call sites pass 11.0f where a box passes 10.0f) and can be scaled without
    // dragging every ValueBox/XgKnob field along with them. The Graphic Display's
    // axis/corner/node LETTERS are Label, not this.
    //
    // OptionsWin = the text in the plain juce::Label rows of the OPTION WINDOWS
    // (Audio Settings, Display Animation, and the standalone's MIDI Input / MIDI
    // Output). Those windows are NOT on the editor's design canvas -- they are
    // ordinary dialogs at 1:1 -- so this category carries NO baseline enlargement
    // (kBaseline 1.0) and 100% means exactly the pixel sizes the call sites pass.
    // It is listed here rather than in its own header so every user-facing font
    // size lives in one place and one settings block.
    //
    // It scales LABELS only. Checkbox text is drawn by juce::LookAndFeel_V4 from
    // the button's height and caps at 15 px, and the combo boxes size their text
    // the same way, so those stay put while the labels grow. Geometry that has to
    // follow the text (label columns, note-block heights, window widths) is scaled
    // by optPx() at the call sites.
    enum Cat { Value = 0, Label, Title, VoiceList, GrDisValue, OptionsWin, kNumCats };

    // Built-in default enlargement per category (the "new 100%"). Values/labels/
    // titles 1.30x, voice names 1.35x — the sizes the editor is tuned for.
    // GrDisValue matches Value's 1.30x; its extra point comes from the larger base
    // height its call sites pass, so 100% here means "the tuned default".
    inline constexpr float kBaseline[kNumCats] { 1.30f, 1.30f, 1.30f, 1.35f, 1.30f, 1.00f };

    // The settings-driven multiplier ON TOP of kBaseline (1.0 = the 100% default).
    // One instance across all TUs (inline variable). Message thread only.
    inline float gScale[kNumCats] { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

    // Set all categories from integer percentages (100 = the tuned default look).
    // Clamped to a sane range so a bad settings.xml value can't make the UI unusable.
    inline void setPercents(int valuePct, int labelPct, int titlePct, int voiceListPct,
                            int grDisValuePct, int optionsPct)
    {
        gScale[Value]      = (float) juce::jlimit(50, 400, valuePct)      / 100.0f;
        gScale[Label]      = (float) juce::jlimit(50, 400, labelPct)      / 100.0f;
        gScale[Title]      = (float) juce::jlimit(50, 400, titlePct)      / 100.0f;
        gScale[VoiceList]  = (float) juce::jlimit(50, 400, voiceListPct)  / 100.0f;
        gScale[GrDisValue] = (float) juce::jlimit(50, 400, grDisValuePct) / 100.0f;
        gScale[OptionsWin] = (float) juce::jlimit(50, 400, optionsPct)     / 100.0f;
    }

    // Build a font of the given base (design-pixel) height for a category. The
    // style flags match juce::FontOptions(height, styleFlags).
    inline juce::FontOptions font(Cat c, float basePx, int styleFlags = juce::Font::plain)
    {
        return juce::FontOptions(basePx * kBaseline[c] * gScale[c], styleFlags);
    }

    // Scale a design pixel measurement by the OptionsWin percentage. The option
    // windows lay out in fixed pixels, so a label column or a wrapped note block
    // sized for the default font clips as soon as the percentage is raised; run
    // every such measurement through this and the whole window grows with the text.
    inline int optPx(float px)
    {
        return (int) std::lround(px * gScale[OptionsWin]);
    }
}
