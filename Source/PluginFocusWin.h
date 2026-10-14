// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

//==============================================================================
// PluginFocusWin — the Win32 half of "keep the keyboard in the plugin window".
//
// JUCE-level focus is not enough for one case, and it is the case a DAW user hits
// most: clicking the plugin window's TITLE BAR. In a plugin that bar belongs to the
// HOST's window frame, and our editor lives in a CHILD window inside it. Activating
// the frame does not move Win32 keyboard focus into that child, so WM_KEYDOWN never
// reaches JUCE at all — no amount of grabKeyboardFocus() on the component tree can
// help, because there is no key event to route.
//
// So this asks the only question that can be answered here — does the window the user
// just activated contain us, and is anything inside it claiming the keyboard? — and,
// if not, calls SetFocus on our own window.
//
// THE GUARD THAT MAKES THIS SAFE is that an ANCESTOR holding focus counts as "nothing
// is claiming it" while a SIBLING does not. A host's plugin frame typically owns
// controls of its own (Cubase puts a preset field and a bypass button in its header);
// those are siblings of our window, not ancestors, so clicking one leaves focus with
// the host and we keep our hands off it. Only the frame itself — what a title-bar
// click leaves behind — is treated as free.
//
// Windows-only by construction. Every caller is inside #if JUCE_WINDOWS.
//==============================================================================

// See MameDiag.h: NOMINMAX must be set before windows.h or the min/max macros break
// every later std::numeric_limits<...>::max() in the MAME headers this TU pulls in.
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mu100focus
{

// The active window of this thread's queue, as an opaque handle (0 = none). Used by
// the caller only to notice that the user has activated a DIFFERENT window, which is
// what bounds how often a claim is retried.
inline void* activeWindow()
{
    return (void*) ::GetActiveWindow();
}

// True when `ourHandle` is inside the active window and nothing in that window holds
// the keyboard for itself. See the header note for why an ancestor and a sibling are
// treated differently.
inline bool canClaimFocus(void* ourHandle)
{
    auto ours = (HWND) ourHandle;
    if (ours == nullptr)
        return false;

    auto active = ::GetActiveWindow();
    if (active == nullptr)
        return false;

    // The user is somewhere else entirely — another plugin, the project window, another
    // application. Never touch focus then.
    if (! (active == ours || ::IsChild (active, ours)))
        return false;

    auto focused = ::GetFocus();

    // Already ours (or a child window of ours): nothing to do.
    if (focused == ours || (focused != nullptr && ::IsChild (ours, focused)))
        return false;

    // Free: nobody has it, or an ancestor frame does.
    return focused == nullptr || ::IsChild (focused, ours);
}

inline void claimFocus(void* ourHandle)
{
    if (auto ours = (HWND) ourHandle)
        ::SetFocus (ours);
}

} // namespace mu100focus
