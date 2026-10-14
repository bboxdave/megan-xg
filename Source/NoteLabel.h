// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "codec/MU100Display.h"

//==============================================================================
// NoteLabel — a single global choice for Options ▸ "Label notes as".
//
// Every place the editor prints a MIDI note as text answers to this: note LETTERS
// (`C3`, `A#2` — the MU100's own convention, middle C = C3 = 60) or the bare MIDI
// NOTE NUMBER (`60`, `58`). One flag, because a user who thinks in note numbers
// thinks in them everywhere, and a menu that switched only one surface would leave
// the others quietly disagreeing with it.
//
// What it covers, which is every note-as-text surface there is:
//   * the octave labels under the Keyboard module's keys (still only under the Cs
//     in either mode — numbering all 128 would be noise, and the C is the landmark
//     you count from whichever way it is labelled);
//   * the Part module title's drum key, ahead of the instrument name;
//   * the drum marker strip's tooltip ("C1 KICK - 2 edited values");
//   * the key-range ends' accessible value, which is what a screen reader reads out
//     for NOTE LIMIT LOW / HIGH;
//   * the MIDI-override badge's tooltip, where it names the keys of a DRUMS buffer.
//
// What it does NOT touch, deliberately:
//   * DAW automation parameter NAMES (`DR1 A#2 58 VOL`). Those carry BOTH forms
//     already, on purpose — the host may not share this plugin's octave convention,
//     so the number is the one that cannot be misread. They are also frozen strings
//     the host caches, and a display option must not rewrite them.
//   * note VALUES anywhere. Display only, exactly like [[changedmarks]].
//
// WHY A NOTE NUMBER IS WORTH OFFERING AT ALL: the octave digit is not standard.
// This plugin, and the hardware it emulates, call middle C `C3`; other DAWs call the
// same note `C4` or `C5`. The letter never disagrees between them, only the digit —
// so a user whose DAW numbers octaves differently can switch the whole editor to the
// one form that is the same everywhere.
//
// Message-thread only (loaded from settings.xml in the processor ctor, written by
// the Options menu item). Letters by default: they are what the hardware shows and
// what most people read a keyboard in.
//==============================================================================
namespace notelabel
{
    enum Mode { Letters = 0, Numbers = 1 };

    // One instance across all TUs (inline variable). Message thread only.
    inline int gMode { Letters };

    inline void setMode (int m) { gMode = (m == Numbers) ? Numbers : Letters; }
    inline int  mode()          { return gMode; }

    // The one place a note becomes text. Note names come from mu100::display so this
    // cannot drift from the codec's convention (C3 = 60) or from the automation
    // parameter names built on the same helper.
    inline juce::String text (int note)
    {
        return gMode == Numbers ? juce::String (note)
                                : juce::String (mu100::display::noteName (note).c_str());
    }
}
