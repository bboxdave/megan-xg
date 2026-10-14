// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

//==============================================================================
// DialAction — a single global flag for "invert knob/dial direction".
//
// When enabled, the direction that mouse DRAG and mouse WHEEL change a value is
// reversed on the two editable numeric controls of the XGEdit-style editor:
//   * XgKnob's rotary dial (KnobSlider)
//   * ValueBox (the compact LCD value fields, incl. the knob value readouts and
//     the stand-alone enum/stepped boxes)
//
// It deliberately does NOT touch other draggable/wheelable elements — the control
// wheel (ControllerWheel), the Graphic Display node/bar editors, the
// dropdown/popup selection boxes (XgSelector) or the pop-up voice browser — none
// of which route their mouse handling through ValueBox / KnobSlider, so leaving
// the flag consulted only by those two classes scopes it exactly right.
//
// Message-thread only (loaded from settings.xml in the processor ctor, written by
// the VST options checkbox / the standalone menu item). Off by default.
//==============================================================================
namespace dialaction
{
    // One instance across all TUs (inline variable). Message thread only.
    inline bool gReverse { false };

    inline void setReverse(bool b) { gReverse = b; }
    inline bool reversed()         { return gReverse; }
}
