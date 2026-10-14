// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

//==============================================================================
// MidiOverrideMarks -- the one global that says HOW a MIDI override is drawn on a
// value box or a knob. Same shape as DialAction.h: an inline variable read at paint
// and layout time by the two widget classes, written once from settings.xml.
//
// The setting it carries is Display Animation -> "Mark them with", and it decides
// only the FORM of the mark. Two other switches sit above it and are asked first:
//   * Options -> "Follow MIDI changes to the device"  (midiFollowEnabled) --
//     whether the override shadow is kept AT ALL. Nothing to draw when it is off.
//   * Display Animation -> "Show MIDI changes to the device" (showMidiOverrides) --
//     whether the marks are drawn from it. That is the OFF state, which is why
//     there is deliberately no "none" style here.
//
// The three styles, and which surface carries the mark in each:
//
//   style                 no dial (a bare ValueBox)   with a dial (an XgKnob)
//   --------------------  --------------------------  ------------------------
//   HorizontalBars        bar under the value box     bar under the value box
//   BarsAndArcs (default) bar under the value box     an arc on the dial
//   VerticalBarsAndDots   bar in the box's right      a dot on the dial, AND the
//                         gutter                      bar in the box's gutter
//
// Only the horizontal bar costs LAYOUT: it lives in a strip below the field, so a
// control that can draw one is built kBarStripH taller. `reservesStrip` is the one
// place that decides, so the layout and the paint can never disagree about it.
//==============================================================================
namespace midimarks
{
    enum Style
    {
        HorizontalBars      = 0,   // bars everywhere, nothing on any dial
        BarsAndArcs         = 1,   // bars where there is no dial, arcs where there is
        VerticalBarsAndDots = 2    // the original: right-gutter bars + dial dots
    };
    inline constexpr int kStyleCount = 3;

    // The strip a horizontal bar lives in, design px: the bar plus a hairline of air
    // above it so it reads as sitting UNDER the field rather than welded to its frame.
    inline constexpr int kBarStripH = 5;
    inline constexpr float kBarH    = 3.5f;

    // One instance across all TUs. Message thread only (loaded in the processor ctor,
    // written by the Display Animation combo).
    inline int gStyle { BarsAndArcs };

    inline void setStyle(int s) { gStyle = (s >= 0 && s < kStyleCount) ? s : BarsAndArcs; }
    inline int  style()         { return gStyle; }

    // `hasDial` = this control is an XgKnob showing its rotary. The three predicates
    // are mutually consistent by construction: exactly one of horizontalBar /
    // verticalBar is true for any control, and a dial mark is drawn only by the two
    // styles that have one.
    inline bool horizontalBar(bool hasDial)
    { return gStyle == HorizontalBars || (gStyle == BarsAndArcs && ! hasDial); }

    inline bool verticalBar(bool /*hasDial*/) { return gStyle == VerticalBarsAndDots; }

    inline bool dialArc() { return gStyle == BarsAndArcs; }
    inline bool dialDot() { return gStyle == VerticalBarsAndDots; }

    // Height a control must add to its value field for the bar strip. Zero unless a
    // horizontal bar can appear there, so every other module keeps its exact layout.
    inline int stripHeight(bool hasDial) { return horizontalBar(hasDial) ? kBarStripH : 0; }
}
