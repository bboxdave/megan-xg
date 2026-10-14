// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

// Reached from the guarded patch in JUCE's juce_StandaloneFilterWindow.h (see
// patches/juce/README.md). Kept in its own tiny header, like MidiBusSink.h, so the JUCE
// translation unit does not have to include the plugin's own headers.
namespace mu100
{
    /** Called instead of quitting when the standalone window's close button is pressed.

        The standalone deliberately does NOT carry a setup between sessions, so closing
        with unsaved edits really does lose them — hence the same guard File > New and
        File > Open already use.

        Returns true if it has TAKEN OVER the close: a confirmation is now on screen and
        it will call quitNow() itself if the user agrees (and nothing at all if they
        cancel). Returns false when there is nothing to lose and the caller should quit
        immediately.
    */
    bool confirmStandaloneQuit (void (*quitNow)());
}
