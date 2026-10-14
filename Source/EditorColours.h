// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "ChangedMarks.h"

//==============================================================================
// EditorColours — the editor view's palette, in TWO themes.
//
// Every colour the XGEdit-style editor draws with lives here as a mutable global,
// and `edcol::setLightTheme(bool)` rewrites the whole set in one go. Panels read
// these at paint time (and, for juce::Label text colours, at construction time),
// so a theme change is applied by REBUILDING the EditorView after calling
// setLightTheme() — see YamahaMU100AudioProcessorEditor::applyEditorTheme().
// Rebuilding rather than walking the tree is deliberate: many colours are set once
// in a constructor, and the editor already has a supported "recreate and re-apply
// the model" path (applyDeviceState()), so a rebuild costs nothing but a repaint
// and cannot leave a stale colour behind.
//
// Naming is SEMANTIC, not literal ("kFieldBg", not "kNearBlackGreen"). A token
// says what the pixel is FOR; the two theme blocks below say what colour that is.
// Adding a colour means adding a token here and giving it a value in BOTH blocks —
// never a hex literal at the call site, or the light theme silently keeps a dark
// pixel that nothing will find again.
//
// Two groups deliberately do NOT change with the theme:
//   * the four DRUM BUFFER colours (DrumBuffer.h) — they are an identity code, and
//     recolouring them would break the one thing they exist to do;
//   * the piano keyboard's own white/black keys and its green/red lamps.
//
// The ATTENTION colours (the yellow "changed" marker, the orange MIDI/axis/marker
// cues) were in that list until 2026-08-09 and are NOT any more — see the block
// below for why. Their HUE is preserved across themes; their lightness is not, and
// cannot be.
//
// kChanged is the single source of the editor's "changed / highlighted" marker:
// GraphicDisplayPanel's kHi aliases it (the bars and EG handles), and the parameter
// fields / voice names reuse it too, so "changed = one colour" is one rule across
// the whole editor.
//==============================================================================
namespace edcol
{
    //==========================================================================
    // Attention colours — the "look here" marks: changed-from-default, MIDI
    // activity, axis zeros, automation markers.
    //
    // These were originally frozen across both themes on the theory that an alarm
    // colour must not move. That was wrong, and measurably so: on the light theme's
    // module grey the dark theme's yellow scores a **1.08:1** contrast ratio and its
    // orange 1.57:1, against the 3:1 that small or thin marks need. The gutter tabs
    // (3.5 px), the MIDI indicator tabs (4 px, and alpha-faded on top of that) and
    // the graphic display's node letters were effectively invisible.
    //
    // A colour cannot be "the same" on both a near-black and a near-white ground —
    // holding the RGB fixed holds the appearance fixed only against a fixed
    // background. What actually has to survive the theme switch is the SIGNAL:
    // same hue family, same relative ordering, same role. So the hues carry over
    // and the lightness inverts, exactly like the text and field colours do.
    //
    // Note "slightly darker" does not get there: #e08000 is still only 2.23:1. The
    // light values below are the first ones in each hue that clear 3:1 on the module
    // grey (~3.5:1) and comfortably clear it on the near-white plot (~4.3:1).
    //==========================================================================

    // Changed-from-default / highlight marker — the one colour the whole editor
    // uses for FILLS (GraphicDisplayPanel's kHi aliases this, and it is the colour
    // of the changed tab drawn by drawChangedTab below).
    inline juce::Colour kChanged { 0xffe7df3c };

    // The same "changed" signal as TEXT. Split from kChanged because the dark theme
    // needs them different — a bright yellow fill wants darker glyphs. In light the
    // two coincide, and that is fine: the split exists so the themes CAN differ, not
    // because they must.
    inline juce::Colour kChangedText { 0xffe7df3c };

    // Ink drawn ON TOP of a kChanged fill: the letter inside a filled EQ node.
    // This one MUST track kChanged — it was theme-independent on the reasoning that
    // "the fill under it does not change", and that reasoning expired the moment the
    // fill did. Dark ink on the light theme's dark-gold fill would score 1.9:1.
    inline juce::Colour kOnChanged { 0xff202020 };

    // The 1 px outline around a filled bar / handle. Distinct from kOnChanged even
    // though both sit on a kChanged fill, because they do opposite jobs: the letter
    // must CONTRAST with the fill, the outline must DEFINE the fill against the plot
    // behind it. Sharing one token forced a light outline on the light theme's dark
    // fill, which read as a halo eating into the bar.
    inline juce::Colour kChangedEdge { 0xff202020 };

    // Graphic-display "0" orange — axis markers, the MIDI activity indicators
    // (per-part tabs + the SYS/FX/BULK lamps) and XgKnob's automation triangle.
    inline juce::Colour kOrange { 0xffffa019 };

    // MIDI OVERRIDE — where incoming MIDI has left the DEVICE, when that differs
    // from the value the user set. Never a fill the user can edit: it marks a
    // second value the editor is only reporting.
    //
    // Cyan is what was left. Yellow/violet is "changed from default", orange is
    // MIDI activity AND the automation triangle, green is the knob dot, red is solo.
    //
    // ON DISCRIMINABILITY, which the light theme does not win on hue alone: the
    // light theme's changed marker is violet #7b3fa0 and this is teal #0e7490 --
    // both blue-dominant and within 1.3:1 of each other in luminance, so a viewer
    // with a red-green deficiency cannot rely on the hue. They are told apart by
    // POSITION and SHAPE instead, which is the rule that has held elsewhere in this
    // editor: the changed tab is a FULL-HEIGHT bar in the value box's LEFT gutter,
    // the override is a PART-HEIGHT level bar in the RIGHT one. See the
    // "state cue: area, not glyph colour" note.
    inline juce::Colour kMidiOverride { 0xff3cd8e7 };

    //==========================================================================
    // Structure — the view background and the module boxes drawn on it.
    //==========================================================================
    inline juce::Colour kViewBg       { 0xff141414 };  // EditorView's backdrop
    inline juce::Colour kHostBg       { 0xff111111 };  // the host editor / bottom bar
    inline juce::Colour kModuleBg     { 0xff202020 };  // every module's rounded box
    inline juce::Colour kModuleBorder { 0xff3a3a3a };  // ...and its 1px outline
    inline juce::Colour kRule         { 0xff3a3a3a };  // separators INSIDE a module
    inline juce::Colour kRowRule      { 0xff2a2a2a };  // the fainter rule between list rows
    inline juce::Colour kBracket      { 0xff9a9a9a };  // XGEdit group brackets

    //==========================================================================
    // Text. Four weights, darkest to faintest. Titles are a step stronger than
    // labels in BOTH themes (in light they go near-black against a mid-grey label)
    // so the module headings read as headings at a glance — the editor already
    // sizes them separately (edtext::Title), this gives them the matching contrast.
    //==========================================================================
    inline juce::Colour kTextTitle    { 0xffffffff };  // module titles
    inline juce::Colour kTextStrong   { 0xffe0e0e0 };  // emphasised / selected rows
    inline juce::Colour kTextLabel    { 0xffd0d0d0 };  // parameter + control labels
    inline juce::Colour kTextDim      { 0xff9a9a9a };  // secondary / annotation
    inline juce::Colour kTextFaint    { 0xff6a6a6a };  // placeholder / inert

    //==========================================================================
    // Parameter value fields (ValueBox, XgKnob readouts, the voice program boxes).
    // The light theme is the one the user asked for by name: near-black text on a
    // much lighter field.
    //==========================================================================
    inline juce::Colour kFieldBg      { 0xff101810 };
    inline juce::Colour kFieldBorder  { 0xff3a3a3a };
    // ALERT amber: a lit control whose ON state is the one that is NOT normal -- the
    // Part module's EQ BYPASS lamp today. Deliberately not green (this palette's
    // "working" colour), not kChanged yellow (already means "changed from default"),
    // and not kSoloOnBg red (already means solo). An ATTENTION colour, so it follows
    // the rule at the top of this file: the hue carries across the themes, the
    // lightness does not. MEASURED against each theme's kModuleBg, which is the only
    // ground it is drawn on: 7.99:1 dark, 4.59:1 light.
    inline juce::Colour kAlert        { 0xffffa021 };

    inline juce::Colour kValue        { 0xffc8e6a0 };  // normal value text
    inline juce::Colour kValueOff     { 0xff5a6a4a };  // disabled / inert value text
    inline juce::Colour kValueDim     { 0xff70806a };  // out-of-range synced note

    // "This is the selected one" green — the Graphic Display's mode buttons
    // (VOLUME / PAN / AMP EG ...), its EQ RANGE options, and the History window's
    // current entry. Single source: GraphicDisplayPanel's kBtnTextOn aliases this.
    inline juce::Colour kSelectedText { 0xff5fe05f };

    // XgKnob's dial: the disc, its rim, and the pointer dot live/inert.
    //
    // kKnobDotOn is NOT kValue, even though it was in the dark theme where the two
    // happened to be the same green. The dot sits on the DISC, not on a field, and
    // the disc stays dark in both themes (a light pointer needs something to read
    // against) -- so tying the dot to the field's text colour made the light theme's
    // near-black value colour vanish into the disc, while the INERT dot stayed
    // visible. Exactly backwards. The dot belongs to the disc's contrast, not the
    // readout's.
    inline juce::Colour kKnobDisc   { 0xff2a2a2a };
    inline juce::Colour kKnobRim    { 0xff555555 };
    inline juce::Colour kKnobDotOn  { 0xffc8e6a0 };
    inline juce::Colour kKnobDotOff { 0xff3a3a3a };

    //==========================================================================
    // Drop-down selectors (XgSelector) — the closed box and its popup list.
    //==========================================================================
    inline juce::Colour kSelBg        { 0xff0d1c0d };
    inline juce::Colour kSelBorder    { 0xff3c5a3c };
    inline juce::Colour kSelText      { 0xffaef0ae };
    inline juce::Colour kPopupBg      { 0xff14140f };
    inline juce::Colour kPopupBorder  { 0xff5a5a3c };
    inline juce::Colour kPopupHiBg    { 0xff2f5a2f };
    inline juce::Colour kPopupHiText  { 0xffffffff };
    inline juce::Colour kPopupText    { 0xffbfe6bf };
    inline juce::Colour kPopupTextOff { 0xff5a6a5a };  // grayed / non-selectable

    //==========================================================================
    // Buttons, dialogs and floating windows (voice browser, history, overlays).
    //==========================================================================
    inline juce::Colour kButtonBg     { 0xff2a2a2a };
    inline juce::Colour kButtonBgAlt  { 0xff333333 };  // the lighter of the two
    inline juce::Colour kButtonText   { 0xffd8d8d8 };
    inline juce::Colour kPanicBg      { 0xff5a1f1f };
    inline juce::Colour kPanicText    { 0xffffffff };
    inline juce::Colour kDialogBg     { 0xff282828 };
    inline juce::Colour kWindowBg     { 0xff262626 };  // browser / overlay body
    inline juce::Colour kWindowBorder { 0xff5a5a5a };
    inline juce::Colour kTitleBarBg   { 0xff3a3a3a };  // drag handle strip
    inline juce::Colour kListBg       { 0xff1b1b1b };  // scrolling list background
    inline juce::Colour kListSelBg    { 0xff3a6ea5 };  // ...and its selected row
    inline juce::Colour kListSelText  { 0xffffffff };

    // The KEYBOARD FOCUS RING (KeyNav.h) — a 2px outline round the control the
    // arrow keys are pointing at. Deliberately NEUTRAL, not an accent: focus is a
    // cursor, not a state of the parameter, and every accent in this editor already
    // means something (yellow/violet = changed, orange = MIDI + axis zeros, blue =
    // selected row, cyan/red = mute/solo, green = a value). A near-white in dark and
    // a near-black in light cannot be mistaken for any of them.
    //
    // It has to read on FOUR grounds, so all four are measured: the module grey, a
    // value field, the graphic-display plot, and — the one that is easy to miss —
    // the SELECTED Voice row it sits on top of, which is where the cursor spends
    // much of its time. Dark 16.3:1 module / 5.3:1 on kListSelBg; light 14.0:1
    // module / 11.1:1 on kListSelBg.
    inline juce::Colour kNavFocus     { 0xffffffff };
    inline juce::Colour kListText     { 0xffd0d0d0 };
    //==========================================================================
    // Per-part MUTE / SOLO cells in the Voice list.
    //
    // Shaped after Cubase's mixer -- dark cell with off-white lettering when off,
    // saturated fill with black lettering when on -- because that is the mixer most
    // of this plugin's users already have open next to it.
    //
    // Solo keeps Cubase's red. Mute does NOT keep Cubase's yellow: kChanged is
    // already yellow, already means "changed from default", and is already painted
    // in the SAME ROW as the changed tab, so a yellow M cell would put two meanings
    // on one colour a few pixels apart. Cyan is unclaimed in this palette, reads as
    // "off / cold" rather than "look here", and stays discriminable against the blue
    // kListSelBg fill of the selected row -- which a blue mute would not, on exactly
    // the row the user is working on.
    //
    // The ON colours are ATTENTION colours and so follow the rule set out at the top
    // of this file: hue carries across the themes, lightness does not. Each lit cell
    // must clear 3:1 against BOTH grounds it can sit on -- the module background and
    // the selected-row fill -- because a selected part can also be muted or soloed.
    inline juce::Colour kMsOffBg    { 0xff2a2a2a };  // cell, not engaged
    inline juce::Colour kMsOffText  { 0xffc8c8c8 };
    inline juce::Colour kMsBorder   { 0xff484848 };  // 1px cell outline
    inline juce::Colour kMuteOnBg   { 0xff2fb3bd };
    inline juce::Colour kSoloOnBg   { 0xffe0402c };
    inline juce::Colour kMsOnText   { 0xff101010 };  // lettering over either fill

    inline juce::Colour kArrow        { 0xffffffff };  // undo/redo arrow glyphs
    inline juce::Colour kArrowShadow  { 0xff000000 };

    // History window (its own small palette — five surfaces on one list).
    inline juce::Colour kHistHeaderBg   { 0xff1a1f22 };
    inline juce::Colour kHistListBg     { 0xff20262a };
    inline juce::Colour kHistCurrentBg  { 0xff101810 };
    inline juce::Colour kHistCurrentHov { 0xff18241a };
    inline juce::Colour kHistRowHover   { 0xff333c41 };
    inline juce::Colour kHistCurrentTxt { 0xffaef0ae };
    inline juce::Colour kHistRowText    { 0xffb0b8bc };

    //==========================================================================
    // Graphic Display — the plot surface and everything drawn on it. In light this
    // becomes a paper-white plot with dark ink, the same inversion as the fields.
    //==========================================================================
    inline juce::Colour kLcdBg      { 0xff1b1b1b };
    inline juce::Colour kLcdBorder  { 0xff3a3a3a };
    inline juce::Colour kBar        { 0xff6f6f6f };  // value bars
    inline juce::Colour kBarInert   { 0xff343434 };  // ...when inert
    inline juce::Colour kBarEdge    { 0xff202020 };  // the 1px outline around a bar
    inline juce::Colour kDash       { 0xff4a4a4a };  // dashed guides
    inline juce::Colour kCorner     { 0xffc0c0c0 };  // corner letters / bar outline
    inline juce::Colour kAxis       { 0xff8c8c8c };
    inline juce::Colour kEgLine     { 0xffb8b8b8 };
    inline juce::Colour kEgFixed    { 0xff9a9a9a };
    inline juce::Colour kEgEdit     { 0xffcfcfcf };
    inline juce::Colour kReadout    { 0xffd6d6d6 };
    inline juce::Colour kSpecLine   { 0xff787878 };  // EQ spectrum trace
    // VOLUME screen level meters — a four-stop ramp up the bar, fixed to the bar's
    // span so a colour always means the same LEVEL rather than moving with the fill.
    // These four are the LIVE ramp; which one they hold comes from the user's choice
    // in Options -> Display Animation (see kMeterRamps / setMeterRamp below), and
    // they are rewritten by BOTH that and the theme.
    inline juce::Colour kMeterLow   { 0xff3ccf5a };  // quiet  (bottom of the bar)
    inline juce::Colour kMeterMid   { 0xffe8dc3c };
    inline juce::Colour kMeterHigh  { 0xffffa019 };
    inline juce::Colour kMeterPeak  { 0xffe0402c };  // at/near full scale (the top)
    inline juce::Colour kBtnTextOff { 0xffc8c8c8 };  // mode-button text, unselected
    inline juce::Colour kTipBg      { 0xff000000 };  // hover readout bubble
    inline juce::Colour kTipBorder  { 0xff707070 };
    inline juce::Colour kTipText    { 0xffd6d6d6 };

    //==========================================================================
    // Keyboard / drum strip. The keys and lamps themselves are theme-independent
    // (a piano is white-and-black in either theme, and the green/red lamps are
    // status); only the strip they sit in and its lettering follow the theme.
    //==========================================================================
    inline juce::Colour kDrumStripBg     { 0xff141414 };
    inline juce::Colour kDrumStripActive { 0xff55551f };  // drum-edit armed
    inline juce::Colour kDrumStripBorder { 0xff5a5a5a };
    inline juce::Colour kDrumKeyText     { 0xff35c24a };  // key name, drum part idle
    inline juce::Colour kDrumKeyArmed    { 0xffffff00 };  // ...and while drum-edit is armed
    inline juce::Colour kMeterFill       { 0xff8a8a8a };  // over-level cell fill
    inline juce::Colour kMeterText       { 0xff101010 };

    //==========================================================================
    // The mod / pitch-bend wheels. A three-stop gradient across the ribbon (edge,
    // highlight, far edge) plus its outline and position mark — enough to read as
    // a cylinder in either theme, which one flat colour cannot do.
    //==========================================================================
    inline juce::Colour kWheelNear { 0xff3a3a3a };
    inline juce::Colour kWheelMid  { 0xff5a5a5a };   // the highlight down the middle
    inline juce::Colour kWheelFar  { 0xff141414 };
    inline juce::Colour kWheelEdge { 0xff000000 };
    inline juce::Colour kWheelMark { 0xffc8e6a0 };   // the position notch

    //==========================================================================
    // Which theme is live. Read it only for a genuine either/or that a token
    // cannot express (e.g. picking a light-vs-dark shadow direction) — a colour
    // choice belongs in the table below, not in an `if (isLight())` at a call site.
    //==========================================================================
    inline bool gLight = false;
    inline bool isLight() { return gLight; }

    //==========================================================================
    // VOLUME meter ramps — the ONE user-choosable palette in the editor.
    //
    // Every other colour here is a design decision; this one is deliberately not,
    // because the meters are eye candy and the ramp is the part of them a user has
    // an opinion about. Options -> Display Animation -> "Meter colours".
    //
    // Each ramp carries a value per theme, and that is not optional: a single RGB
    // cannot read the same on a near-black plot and on a paper-white one, which is
    // exactly the mistake the attention colours made in 2026-08-09. Every value
    // below is MEASURED at >= 3:1 against its own theme's kLcdBg (the 4-6 px fills
    // these draw are thin marks, so 3:1 is the floor); the weakest is Ocean's light
    // amber at 3.61 and Classic's light burnt orange at 3.88.
    //
    // The stops sit at fixed FRACTIONS of the bar (see paintMeterBar): low at the
    // floor, mid at -18 dB, high at -9 dB, peak at -3 dB and above. So a ramp whose
    // first three stops are one hue (Mono) spends colour only on near-clipping,
    // while one that changes at every stop reads as a traditional meter bridge.
    //==========================================================================
    struct MeterRamp
    {
        const char*  name;
        const char*  hint;          // shown after the name in the dropdown (US spelling:
                                    // every string here reaches the UI, and the
                                    // plugin's user-facing text is American English —
                                    // "gray", "color", "center". Code COMMENTS in this
                                    // repo stay British; only the strings switch.)
        juce::uint32 dark[4];       // low, mid, high, peak
        juce::uint32 light[4];
    };

    inline constexpr int kMeterRampCount = 5;

    inline const MeterRamp* meterRamps()
    {
        static const MeterRamp ramps[kMeterRampCount] = {
            // The console meter-bridge everyone reads without thinking. Light keeps
            // the hues and drops the lightness: forest green and dark gold, because
            // the dark theme's highlighter green fights a palette whose modules
            // separate by tone rather than contrast.
            { "Classic", "green to red",
              { 0xff3ccf5a, 0xffe8dc3c, 0xffffa019, 0xffe0402c },
              { 0xff2e7d4a, 0xff8a6d00, 0xffc2620a, 0xffb3241a } },
            // Only the primary changes. Cooler on paper, and further from the
            // editor's own value greens than Classic's green is.
            { "Teal", "teal to red",
              { 0xff2fd0bb, 0xffe8dc3c, 0xffffa019, 0xffe0402c },
              { 0xff1f7a70, 0xff8a6d00, 0xffc2620a, 0xffb3241a } },
            // The one that belongs to the LIGHT palette: it already carries a blue
            // for list selection, so a blue-rooted ramp reads as part of the design
            // rather than as an instrument bolted onto it.
            { "Ocean", "blue to red",
              { 0xff3d8fe0, 0xff2fd0bb, 0xffffc23c, 0xffe0402c },
              { 0xff2b5fa8, 0xff1f7a70, 0xffa8791a, 0xffb3241a } },
            // No green at all — a heat ramp, kin to the orange 10/20/30 axis
            // markers in both themes.
            { "Amber", "no green",
              { 0xffb06a0e, 0xffe8a83c, 0xffff7a19, 0xffe0402c },
              { 0xff8a5a08, 0xffa8791a, 0xffc2620a, 0xffb3241a } },
            // The theme's own tonal language, with colour spent only on the one
            // thing worth alarming about. Note the light values get DARKER as the
            // level rises: on paper, ink is what "more" looks like — the same
            // inversion the bars themselves already make.
            { "Mono", "gray, red peak",
              { 0xff8a8a8a, 0xffb4b4b4, 0xffd8d8d8, 0xffe0402c },
              { 0xff7a7a7a, 0xff4e4e4e, 0xff2a2a2a, 0xffb3241a } },
        };
        return ramps;
    }

    //==========================================================================
    // Meter COLOUR SPREAD — where up the bar the ramp's stops sit, in dB.
    //
    // Lives beside the ramps because it is the other half of the same picture: the
    // ramp says WHICH colours, this says WHERE they arrive. Anchored to dB and not
    // to a fraction of the bar, because dB is the only honest way to label it (the
    // dropdown reads "colour from -18 dB") and because the fractions fall out of
    // whatever floor the meter uses -- see GraphicDisplayPanel::kMeterFloorDb, which
    // is what converts these to bar positions.
    //
    // Standard is a meter bridge's own convention, and it means green owns the bottom
    // 70% of the bar: music sits around -20 dB, so in ordinary use you see the primary
    // colour and almost nothing else. Wide and Full are the SAME ramp stretched
    // downward -- each keeps the mid stop halfway up the span above the knee and the
    // high stop ~83% up it -- so they change when a colour arrives, never what the
    // ordering means. What they trade away is the convention that yellow says "getting
    // loud"; at Full it says "audible". That is a fair trade for eye candy and a poor
    // one for a default, which is exactly why it is a setting.
    //
    // NOTE this moves the COLOURS only. The bar's HEIGHT is untouched, deliberately:
    // it is read against the setting dash and clamped to it, so lifting the meter's
    // floor instead (the thing "sensitivity" would normally mean) would make a quiet
    // part look like it is running at its setting.
    struct MeterSpread
    {
        const char* name;
        float       knee[3];   // dB at which low->mid, mid->high, high->peak land
    };

    inline constexpr int kMeterSpreadCount = 3;

    inline const MeterSpread* meterSpreads()
    {
        static const MeterSpread spreads[kMeterSpreadCount] = {
            { "Standard", { -18.0f,  -9.0f, -3.0f } },
            { "Wide",     { -30.0f, -15.0f, -5.0f } },
            { "Full",     { -42.0f, -21.0f, -7.0f } },
        };
        return spreads;
    }

    //==========================================================================
    // Meter FILL — whether the animated level is one continuous column
    // or a stack of lit segments with the background showing between them.
    //
    // Purely cosmetic: the level, the ramp and the clamp to the setting dash are the
    // same either way. LED quantises the TOP of the fill DOWN to the segment grid,
    // which is the only behavioural difference — a segment lights when the level has
    // actually reached it, so the meter can read up to one segment low, exactly as a
    // hardware meter bridge does. Quantising down (never up) also keeps the promise
    // that the level never claims more than the setting allows.
    //
    // The segment height is the setting dash's own height (GraphicDisplayPanel::
    // meterDashH), so the lit blocks and the indicator above them are the same size —
    // that is the whole visual idea, and tying them together means retuning one
    // retunes the other. The gap is a fraction of that, floored at 1 px so it cannot
    // vanish on a small editor, and it is PAINTED in the LCD background colour rather
    // than left unpainted: the centre guide line runs behind the bars, and a gap that
    // merely skipped painting would show it.
    struct MeterFill
    {
        const char* name;
        const char* hint;
    };

    inline constexpr int kMeterFillCount = 2;

    inline const MeterFill* meterFills()
    {
        static const MeterFill fills[kMeterFillCount] = {
            { "Solid", "one continuous bar" },
            { "LED",   "lit segments" },
        };
        return fills;
    }

    // Gap between LED segments, as a fraction of the segment height.
    inline constexpr float kMeterLedGapFrac = 0.4f;

    inline int gMeterRamp = 0;

    // Rewrite the four live meter tokens from the current (ramp, theme) pair. Called
    // by BOTH setMeterRamp and setLightTheme, because either can change the answer —
    // a theme switch that forgot this would silently reinstate Classic's colours.
    inline void applyMeterRamp()
    {
        const auto& r = meterRamps()[juce::jlimit(0, kMeterRampCount - 1, gMeterRamp)];
        const juce::uint32* v = gLight ? r.light : r.dark;
        kMeterLow  = juce::Colour (v[0]);
        kMeterMid  = juce::Colour (v[1]);
        kMeterHigh = juce::Colour (v[2]);
        kMeterPeak = juce::Colour (v[3]);
    }

    // Message thread. Unlike a theme change this needs NO EditorView rebuild: the
    // meter colours are read at paint time only (no juce::Label holds one), so the
    // caller just repaints.
    inline void setMeterRamp(int index)
    {
        gMeterRamp = juce::jlimit(0, kMeterRampCount - 1, index);
        applyMeterRamp();
    }

    //==========================================================================
    // Install a theme. Message thread only; the caller rebuilds the editor view.
    //==========================================================================
    inline void setLightTheme(bool light)
    {
        gLight = light;

        if (! light)
        {
            //------------------------------------------------------------------
            // DARK — the original XGEdit-derived palette. These are the values the
            // declarations above already carry; repeated here so the two themes
            // read as one table and neither can drift from the other.
            //------------------------------------------------------------------
            kChanged     = juce::Colour (0xffe7df3c);
            kChangedText = juce::Colour (0xffe7df3c);
            kOnChanged   = juce::Colour (0xff202020);
            kChangedEdge = juce::Colour (0xff202020);
            kOrange      = juce::Colour (0xffffa019);
            // 9.45:1 on the module grey. The dark theme has room, so this is the
            // saturated cyan rather than a muted one -- the mark is thin.
            kMidiOverride = juce::Colour (0xff3cd8e7);

            kViewBg       = juce::Colour (0xff141414);
            kHostBg       = juce::Colour (0xff111111);
            kModuleBg     = juce::Colour (0xff202020);
            kModuleBorder = juce::Colour (0xff3a3a3a);
            kRule         = juce::Colour (0xff3a3a3a);
            kRowRule      = juce::Colour (0xff2a2a2a);
            kBracket      = juce::Colour (0xff9a9a9a);

            kTextTitle  = juce::Colour (0xffffffff);
            kTextStrong = juce::Colour (0xffe0e0e0);
            kTextLabel  = juce::Colour (0xffd0d0d0);
            kTextDim    = juce::Colour (0xff9a9a9a);
            kTextFaint  = juce::Colour (0xff6a6a6a);

            kFieldBg     = juce::Colour (0xff101810);
            kFieldBorder = juce::Colour (0xff3a3a3a);
            kAlert       = juce::Colour (0xffffa021);
            kValue       = juce::Colour (0xffc8e6a0);
            kValueOff    = juce::Colour (0xff5a6a4a);
            kValueDim    = juce::Colour (0xff70806a);
            kSelectedText= juce::Colour (0xff5fe05f);
            kKnobDisc    = juce::Colour (0xff2a2a2a);
            kKnobRim     = juce::Colour (0xff555555);
            kKnobDotOn   = juce::Colour (0xffc8e6a0);
            kKnobDotOff  = juce::Colour (0xff3a3a3a);

            kSelBg        = juce::Colour (0xff0d1c0d);
            kSelBorder    = juce::Colour (0xff3c5a3c);
            kSelText      = juce::Colour (0xffaef0ae);
            kPopupBg      = juce::Colour (0xff14140f);
            kPopupBorder  = juce::Colour (0xff5a5a3c);
            kPopupHiBg    = juce::Colour (0xff2f5a2f);
            kPopupHiText  = juce::Colour (0xffffffff);
            kPopupText    = juce::Colour (0xffbfe6bf);
            kPopupTextOff = juce::Colour (0xff5a6a5a);

            kButtonBg     = juce::Colour (0xff2a2a2a);
            kButtonBgAlt  = juce::Colour (0xff333333);
            kButtonText   = juce::Colour (0xffd8d8d8);
            kPanicBg      = juce::Colour (0xff5a1f1f);
            kPanicText    = juce::Colour (0xffffffff);
            kDialogBg     = juce::Colour (0xff282828);
            kWindowBg     = juce::Colour (0xff262626);
            kWindowBorder = juce::Colour (0xff5a5a5a);
            kTitleBarBg   = juce::Colour (0xff3a3a3a);
            kListBg       = juce::Colour (0xff1b1b1b);
            kListSelBg    = juce::Colour (0xff3a6ea5);
            kListSelText  = juce::Colour (0xffffffff);
            kNavFocus     = juce::Colour (0xffffffff);
            kListText     = juce::Colour (0xffd0d0d0);
            kArrow        = juce::Colour (0xffffffff);
            kArrowShadow  = juce::Colour (0xff000000);

            kHistHeaderBg   = juce::Colour (0xff1a1f22);
            kHistListBg     = juce::Colour (0xff20262a);
            kHistCurrentBg  = juce::Colour (0xff101810);
            kHistCurrentHov = juce::Colour (0xff18241a);
            kHistRowHover   = juce::Colour (0xff333c41);
            kHistCurrentTxt = juce::Colour (0xffaef0ae);
            kHistRowText    = juce::Colour (0xffb0b8bc);

            kLcdBg     = juce::Colour (0xff1b1b1b);
            kLcdBorder = juce::Colour (0xff3a3a3a);
            kBar       = juce::Colour (0xff6f6f6f);
            kBarInert  = juce::Colour (0xff343434);
            kBarEdge   = juce::Colour (0xff202020);
            kDash      = juce::Colour (0xff4a4a4a);
            kCorner    = juce::Colour (0xffc0c0c0);
            kAxis      = juce::Colour (0xff8c8c8c);
            kEgLine    = juce::Colour (0xffb8b8b8);
            kEgFixed   = juce::Colour (0xff9a9a9a);
            kEgEdit    = juce::Colour (0xffcfcfcf);
            kReadout   = juce::Colour (0xffd6d6d6);
            kSpecLine   = juce::Colour (0xff787878);
            kBtnTextOff = juce::Colour (0xffc8c8c8);

            kMsOffBg   = juce::Colour (0xff2a2a2a);
            kMsOffText = juce::Colour (0xffc8c8c8);
            kMsBorder  = juce::Colour (0xff484848);
            kMuteOnBg  = juce::Colour (0xff2fb3bd);
            kSoloOnBg  = juce::Colour (0xffe0402c);
            kMsOnText  = juce::Colour (0xff101010);
            kTipBg     = juce::Colour (0xff000000);
            kTipBorder = juce::Colour (0xff707070);
            kTipText   = juce::Colour (0xffd6d6d6);

            kDrumStripBg     = juce::Colour (0xff141414);
            kDrumStripActive = juce::Colour (0xff55551f);
            kDrumStripBorder = juce::Colour (0xff5a5a5a);
            kDrumKeyText     = juce::Colour (0xff35c24a);
            kDrumKeyArmed    = juce::Colour (0xffffff00);
            kMeterFill       = juce::Colour (0xff8a8a8a);
            kMeterText       = juce::Colour (0xff101010);

            kWheelNear = juce::Colour (0xff3a3a3a);
            kWheelMid  = juce::Colour (0xff5a5a5a);
            kWheelFar  = juce::Colour (0xff141414);
            kWheelEdge = juce::Colour (0xff000000);
            kWheelMark = juce::Colour (0xffc8e6a0);
        }
        else
        {
            //------------------------------------------------------------------
            // LIGHT — modelled on Arturia Pigments' light theme: a mid-grey page
            // with slightly lighter module boxes on it, hairline borders a shade
            // darker than the page, and near-black ink. The greys are deliberately
            // close together (0xffc9 page / 0xffe2 module / 0xffb4 border): the
            // modules are separated by TONE, not by contrast, which is what stops a
            // light UI turning into a grid of glaring white cards.
            //------------------------------------------------------------------
            // "Changed" is the one mark that does NOT keep its dark-theme hue: at the
            // user's choice (2026-08-09) it goes VIOLET rather than to the dark gold
            // that a legible yellow becomes. Two reasons it earns the hue break:
            //   * nothing else in either theme is purple, so it cannot be confused
            //     with the value greens, the list-selection blue, or the orange MIDI
            //     and axis cues -- which the gold sat right next to;
            //   * it is far stronger where the mark actually lives: 5.30:1 on the
            //     module grey and 6.39:1 on the fields and the plot, against the
            //     gold's 3.80 and 4.58 (and the raw yellow's 1.08 and 1.30).
            // The cost, accepted knowingly: the two themes no longer name "changed"
            // with the same hue. Fill and text land on one value -- a light ground
            // wants one dark mark, not a bright fill with dark glyphs on it.
            kChanged     = juce::Colour (0xff7b3fa0);
            kChangedText = juce::Colour (0xff7b3fa0);
            // Ink on that violet fill goes LIGHT (5.9:1); the outline goes the other
            // way, to a deeper violet that defines the fill against the plot.
            kOnChanged   = juce::Colour (0xfff3ebfa);
            kChangedEdge = juce::Colour (0xff4e2769);
            // Teal: 4.06:1 on the light module grey and 5.00:1 on the fields, where
            // the mark actually lives. The dark theme's cyan would score 1.4 there.
            kMidiOverride = juce::Colour (0xff0e7490);
            // Burnt orange: 3.21:1 on the module grey, 3.88:1 on the plot, against
            // #ffa019's 1.57 and 1.90. This is the LIGHTEST value in the hue that
            // still clears 3:1 on the grey -- one step further (#ca6d0c) drops to
            // 2.84 there. Picked over the darker #b85c08 (3.55) because that read
            // heavy for the theme; the widened tabs and the bold axis zeros buy back
            // the legibility that the extra lightness costs.
            kOrange      = juce::Colour (0xffc2620a);

            kViewBg       = juce::Colour (0xffc9c9c9);
            kHostBg       = juce::Colour (0xffbfbfbf);
            kModuleBg     = juce::Colour (0xffe2e2e2);
            kModuleBorder = juce::Colour (0xffb4b4b4);
            kRule         = juce::Colour (0xffbcbcbc);
            kRowRule      = juce::Colour (0xffcecece);
            kBracket      = juce::Colour (0xff8a8a8a);

            // Titles near-black, labels a clear step lighter — the "titles differ
            // from labels" the theme was asked for, carried by colour as well as
            // by the size edtext::Title already gives them.
            kTextTitle  = juce::Colour (0xff141414);
            kTextStrong = juce::Colour (0xff222222);
            kTextLabel  = juce::Colour (0xff3c3c3c);
            kTextDim    = juce::Colour (0xff6b6b6b);
            kTextFaint  = juce::Colour (0xff949494);

            // The voice / parameter fields: near-black on near-white, with the
            // faintest green cast kept so the editor still reads as the same
            // instrument rather than as a generic grey form.
            kFieldBg     = juce::Colour (0xfff5f8ef);
            kFieldBorder = juce::Colour (0xffb0b6a6);
            kAlert       = juce::Colour (0xff9c4f00);
            kValue       = juce::Colour (0xff16220c);
            kValueOff    = juce::Colour (0xff9ba593);
            kValueDim    = juce::Colour (0xff70806a);
            kSelectedText= juce::Colour (0xff147a14);
            // A light-theme knob keeps a DARK disc (Pigments does the same): the
            // pointer has to read against something, and a light disc on a light
            // module leaves the dot nothing to sit on.
            kKnobDisc    = juce::Colour (0xff585858);
            kKnobRim     = juce::Colour (0xff989898);
            kKnobDotOn   = juce::Colour (0xffcdeb9f);   // still light: the disc is dark
            kKnobDotOff  = juce::Colour (0xff7e7e7e);

            kSelBg        = juce::Colour (0xffeef4ea);
            kSelBorder    = juce::Colour (0xff9fb89f);
            kSelText      = juce::Colour (0xff123c12);
            kPopupBg      = juce::Colour (0xfff6f6ee);
            kPopupBorder  = juce::Colour (0xffa8a88c);
            kPopupHiBg    = juce::Colour (0xff2f6a2f);   // inverted row, like a menu
            kPopupHiText  = juce::Colour (0xffffffff);
            kPopupText    = juce::Colour (0xff1c3a1c);
            kPopupTextOff = juce::Colour (0xffa0aca0);

            kButtonBg     = juce::Colour (0xffd2d2d2);
            kButtonBgAlt  = juce::Colour (0xffdcdcdc);
            kButtonText   = juce::Colour (0xff262626);
            kPanicBg      = juce::Colour (0xffd98686);
            kPanicText    = juce::Colour (0xff2a0c0c);
            kDialogBg     = juce::Colour (0xffe6e6e6);
            kWindowBg     = juce::Colour (0xffe4e4e4);
            kWindowBorder = juce::Colour (0xffa0a0a0);
            kTitleBarBg   = juce::Colour (0xffcacaca);
            kListBg       = juce::Colour (0xfff6f6f6);
            // The selected row is LIGHT blue with dark text, not the dark theme's
            // inverted slab. It was the slab until the changed-marker went violet,
            // and that is what exposed the bug: PartListPanel draws the row-changed
            // tab ON TOP of the selection fill, so the marker's real background on
            // the row you are most likely to be editing is this colour, not the
            // module grey. On the dark slab the violet scored 1.29:1 and the gold
            // before it 1.08:1 -- i.e. the marker was invisible on the selected row
            // in BOTH light palettes, and had been since the light theme landed.
            // Lightening the selection fixes it (4.09:1) and, as a bonus, stops the
            // voice list being the one dark block left in a light UI.
            kListSelBg    = juce::Colour (0xffaecbe8);
            kListSelText  = juce::Colour (0xff10243a);
            kNavFocus     = juce::Colour (0xff1a1a1a);
            kListText     = juce::Colour (0xff2c2c2c);
            kArrow        = juce::Colour (0xff2a2a2a);
            kArrowShadow  = juce::Colour (0xffffffff);

            kHistHeaderBg   = juce::Colour (0xffd6dade);
            kHistListBg     = juce::Colour (0xffeef1f3);
            kHistCurrentBg  = juce::Colour (0xffdcf0dc);
            kHistCurrentHov = juce::Colour (0xffcae8ca);
            kHistRowHover   = juce::Colour (0xffdde3e7);
            kHistCurrentTxt = juce::Colour (0xff10500f);
            kHistRowText    = juce::Colour (0xff33393d);

            // The plot goes to paper: white ground, dark ink, and bars that are
            // DARKER than their background rather than lighter, so a filled bar
            // still reads as "more" at a glance.
            kLcdBg     = juce::Colour (0xfff7f7f4);
            kLcdBorder = juce::Colour (0xffb0b0b0);
            kBar       = juce::Colour (0xff7a7a7a);
            kBarInert  = juce::Colour (0xffd2d2d2);
            kBarEdge   = juce::Colour (0xff9a9a9a);
            kDash      = juce::Colour (0xffc4c4c4);
            kCorner    = juce::Colour (0xff4a4a4a);
            kAxis      = juce::Colour (0xff8a8a8a);
            kEgLine    = juce::Colour (0xff4e4e4e);
            kEgFixed   = juce::Colour (0xff7c7c7c);
            kEgEdit    = juce::Colour (0xff2e2e2e);
            kReadout   = juce::Colour (0xff2a2a2a);
            kSpecLine   = juce::Colour (0xff8f8f8f);
            kBtnTextOff = juce::Colour (0xff3c3c3c);

            // Darkened from the dark theme's values, not reused: on the light module
            // grey the dark theme's cyan scores 2.02:1, well under the 3:1 a small
            // filled cue needs, and it also has to stay separable from the light
            // theme's much paler selected-row fill. These measure 4.23:1 (mute) and
            // 5.14:1 (solo) against the module grey.
            //
            // The LETTERING flips with them -- black on the dark theme's brighter
            // fills, off-white on these darker ones -- which is not a broken promise
            // to match Cubase: the state cue here is the filled AREA, and the letter
            // only has to stay readable on top of it. The two fills were then picked
            // to be close in LIGHTNESS as well as legible, because one ink has to
            // serve both: 4.94:1 (mute) and 5.14:1 (solo) with this off-white. The
            // obvious lighter cyan fails exactly here -- it wants black ink while
            // the red wants white, and no single kMsOnText can satisfy both.
            kMsOffBg   = juce::Colour (0xffd2d2d2);
            kMsOffText = juce::Colour (0xff3c3c3c);
            kMsBorder  = juce::Colour (0xff9a9a9a);
            kMuteOnBg  = juce::Colour (0xff17747c);
            kSoloOnBg  = juce::Colour (0xffc22f1d);
            kMsOnText  = juce::Colour (0xfff4f4f4);
            kTipBg     = juce::Colour (0xff2a2a2a);      // stays inverted: a tip is
            kTipBorder = juce::Colour (0xff707070);      // meant to sit ON TOP of
            kTipText   = juce::Colour (0xfff0f0f0);      // the plot, not blend in

            kDrumStripBg     = juce::Colour (0xffdcdcdc);
            kDrumStripActive = juce::Colour (0xfff0e8a0);
            kDrumStripBorder = juce::Colour (0xffa8a8a8);
            kDrumKeyText     = juce::Colour (0xff17772a);
            // Yellow-on-yellow would vanish: the armed strip IS the yellow cue, so
            // the text on it goes to a dark amber of the same hue.
            kDrumKeyArmed    = juce::Colour (0xff5e4d00);
            kMeterFill       = juce::Colour (0xff707070);
            kMeterText       = juce::Colour (0xfff0f0f0);

            // A light wheel needs its gradient INVERTED — the highlight stays the
            // lightest stop, so the ribbon still bulges toward the viewer.
            kWheelNear = juce::Colour (0xffbcbcbc);
            kWheelMid  = juce::Colour (0xffefefef);
            kWheelFar  = juce::Colour (0xff9a9a9a);
            kWheelEdge = juce::Colour (0xff707070);
            kWheelMark = juce::Colour (0xff2c4416);
        }

        // The meter ramp is chosen by the USER, not by the theme, so it is not in
        // either block above — but its VALUES are per theme, so the live tokens have
        // to be recomputed here or a theme switch would leave the meters holding the
        // outgoing theme's version of whichever ramp is selected.
        applyMeterRamp();
    }

    //==========================================================================
    // Width of the changed tab below, in the field's own pixels. Named so that a
    // layout sizing a field around its value text can keep the text clear of the
    // tab (see the EQ grid's box width in PartPanel::resized).
    inline constexpr float kChangedTabW = 4.5f;   // 3.5 -> 4.5, 2026-08-09

    // Option B marker: a thin yellow tab down the left inside edge of a field,
    // signalling "changed from default" without touching the text colour. `field`
    // is the field's bounds; the tab is inset vertically so it reads as a tab
    // rather than a full border.
    // Options -> "Show changed markers" is asked HERE rather than at each call site:
    // every tab in the editor comes through this one helper (value boxes, selection
    // boxes, the Voice module's row roll-up), so the option cannot reach some of them
    // and miss the rest. The two surfaces that state "changed" WITHOUT a tab -- the
    // Graphic Display's highlighted bars and handles, and the Keyboard's drum marker
    // strip -- ask changedmarks::show() themselves. No layout is involved either way:
    // the gutter is reserved unconditionally, so nothing moves when this goes off.
    inline void drawChangedTab(juce::Graphics& g, juce::Rectangle<float> field,
                               float width = kChangedTabW, float vInset = 1.5f)
    {
        if (! changedmarks::show()) return;
        g.setColour(kChanged);
        g.fillRect(field.removeFromLeft(width).reduced(0.0f, vInset));
    }

    // Where a MIDI-override bar STARTS and ENDS along a track of `len` px. Both bars
    // (vertical and horizontal) and the dial arc come through here, so the three
    // cannot disagree about what a value means.
    //
    // `origin` is the parameter's rest point as a proportion: 0 for an ordinary
    // 0..max parameter, 0.5-ish for a bipolar one (PAN, note shift, detune, tune, the
    // EQ gains), where a bar growing out of the minimum would say a centred device is
    // "low" rather than "at rest". A unipolar parameter that merely DEFAULTS to the
    // middle keeps origin 0 -- its middle is a default, not a zero.
    //
    // A floor of 1.5 px on the extent: with origin 0 and pos 0, an invisible bar
    // would say "no override", which is the one thing it must never say. The device
    // sitting at zero while the user set something else is exactly a case worth
    // seeing -- and with a centre origin the same goes for a device sitting at rest.
    struct BarSpan { float lo, hi; };
    inline BarSpan overrideSpan(float len, float pos, float origin)
    {
        const float a = juce::jlimit(0.0f, 1.0f, origin) * len;
        const float b = juce::jlimit(0.0f, 1.0f, pos)    * len;
        float lo = juce::jmin(a, b), hi = juce::jmax(a, b);
        if (hi - lo < 1.5f)
        {
            // Grow AWAY from the origin, so the stub a tiny override leaves still
            // says which side of the rest point the device is on.
            if (b >= a) hi = lo + 1.5f;
            else        lo = hi - 1.5f;
        }
        if (hi > len) { hi = len; lo = juce::jmax(0.0f, len - 1.5f); }
        if (lo < 0.0f) { lo = 0.0f; hi = juce::jmin(len, 1.5f); }
        return { lo, hi };
    }

    // The MIDI-override level bar: same gutter width as the changed tab, but in the
    // field's RIGHT gutter and only as tall as the device's value is high in the
    // parameter's range. `pos` and `origin` are 0..1 (see overrideSpan).
    //
    // The right gutter costs nothing to use: ValueBox already reserves its text
    // clearance on BOTH sides (symmetrically, so the centre never shifts), and the
    // right half of that reserve has been empty since it was introduced.
    inline void drawMidiOverrideBar(juce::Graphics& g, juce::Rectangle<float> field,
                                    float pos, float origin = 0.0f,
                                    float width = kChangedTabW, float vInset = 1.5f)
    {
        auto gutter = field.removeFromRight(width).reduced(0.0f, vInset);
        const auto s = overrideSpan(gutter.getHeight(), pos, origin);
        g.setColour(kMidiOverride);
        g.fillRect(gutter.withTop(gutter.getBottom() - s.hi)
                         .withBottom(gutter.getBottom() - s.lo));
    }

    // ...and the horizontal form, in a strip BELOW the field (midimarks::kBarStripH
    // tall, granted by the control's own layout). Same span rule, run along x, with
    // the whole width of the box to travel instead of a 13 px gutter -- which is the
    // entire point of the style: a small override is legible at this length and is
    // not at that one.
    inline void drawMidiOverrideBarH(juce::Graphics& g, juce::Rectangle<float> strip,
                                     float pos, float origin = 0.0f,
                                     float height = 3.5f)
    {
        auto track = strip.removeFromBottom(height);
        const auto s = overrideSpan(track.getWidth(), pos, origin);
        g.setColour(kMidiOverride);
        g.fillRect(track.withLeft(track.getX() + s.lo).withRight(track.getX() + s.hi));
    }
}
