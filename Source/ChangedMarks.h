// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

//==============================================================================
// ChangedMarks — a single global flag for Options ▸ "Show changed markers".
//
// The editor marks every value that differs from its default. That is one SIGNAL
// wearing several shapes, and this flag switches the whole class off together:
//
//   * the yellow tab down the left edge of a value box or a selection box
//     (edcol::drawChangedTab, which is gated HERE so every caller is covered);
//   * the Voice module's per-row roll-up tab (same helper);
//   * the Graphic Display's highlighted bars and envelope handles — the same
//     "not at its default" statement in the one place it is made with colour
//     rather than with a tab;
//   * the Keyboard's drum marker strip, upper row (a kit key that has been edited).
//
// What it does NOT touch, deliberately:
//   * the MIDI-override marks (cyan). A different question — "the DEVICE has been
//     moved off your value" — with its own switches (Options ▸ Follow MIDI changes
//     to the device, Display Animation ▸ Show MIDI changes to the device).
//   * LAYOUT. The gutter a tab lives in is reserved unconditionally everywhere it
//     appears, so switching the marks off never reflows a value or moves a module;
//     the pixels simply go unpainted. That is what makes this safe to toggle mid-
//     session and what keeps every screenshot in the manual valid either way.
//   * anything about the VALUES themselves. Display only.
//
// Message-thread only (loaded from settings.xml in the processor ctor, written by
// the Options menu item). On by default — the marks are how the editor answers
// "what have I touched?", so hiding them is the deliberate choice.
//==============================================================================
namespace changedmarks
{
    // One instance across all TUs (inline variable). Message thread only.
    inline bool gShow { true };

    inline void setShow(bool b) { gShow = b; }
    inline bool show()          { return gShow; }
}
