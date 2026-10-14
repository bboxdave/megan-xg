// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <atomic>
#include "codec/MU100Voices.h"

//==============================================================================
// VoiceNaming — which of a voice's two names the plugin shows.
//
// Every voice in kVoices carries both a full display name ("Grand Piano") and the
// 8-character name the MU100's own LCD prints for it ("GrandPno"), so the two are
// a display choice, not two data sets. Options -> Voice names picks between them;
// `shortVoiceNames` in settings.xml persists it, and this flag is the live copy.
//
// It is a global rather than a parameter threaded through the call sites because
// the same question is asked from four unrelated places — the Voice module's part
// list, the Voice List browser, the tooltip that names a part's voice, and the
// PROCESSOR's output-bus names (File -> Name All Output Tracks from Voices, which
// composes with the GUI closed and so cannot ask a panel). One flag means the
// editor and the DAW's track names cannot disagree about which form is showing.
//
// ATOMIC because of that last caller: bus names are composed on the message
// thread but the host may read them back from a thread of its choosing. It is a
// plain relaxed load of a bool — nothing here is ordered against anything else.
//==============================================================================
namespace voicenames
{
    // One instance across all TUs (inline variable). Written on the message
    // thread (settings load, Options menu); read from anywhere.
    inline std::atomic<bool> gShort { false };

    inline void setShort (bool b) { gShort.store (b, std::memory_order_relaxed); }
    inline bool useShort()        { return gShort.load (std::memory_order_relaxed); }

    // The name to show for a voice, in whichever form is current.
    inline const char* displayName (const mu100::VoiceEntry& v)
    {
        return useShort() ? v.short8 : v.name;
    }

    // Bank MSB/LSB + 1-based program -> display name, or nullptr when the
    // combination is not a known voice. Callers supply their own placeholder
    // ("--" is the editor's convention) so this stays free of JUCE.
    inline const char* displayName (int msb, int lsb, int pgm)
    {
        for (int i = 0; i < mu100::kVoiceCount; ++i)
        {
            const auto& v = mu100::kVoices[i];
            if (v.msb == msb && v.lsb == lsb && v.pgm == pgm)
                return displayName (v);
        }
        return nullptr;
    }
}
