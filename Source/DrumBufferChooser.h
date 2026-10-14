// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "EditorColours.h"
#include <functional>
#include "DrumBuffer.h"
#include "EditorTextScale.h"

//==============================================================================
// DrumBufferChooser — "which DRUMS buffer should this load into?"
//
// Shown before a .mds (per-key data alone) or a .mps whose part sits on a DRUMS
// buffer. Both write a whole drum edit buffer, and a buffer is SHARED by every
// part whose PART MODE names it — so the write reaches parts the user never
// selected. Rather than warn after the fact, the choice is made up front and the
// consequence is spelled out next to it.
//
// Five options: DRUMS1..4, each with the same coloured square outline the voice
// browser uses for its buffer swatches (and which outlines a part's voice name in
// the Voice list), plus an unswatched last row that keeps whatever the SOURCE used —
// "Buffer File Was Saved With" for a file, "Buffer It Was Copied From" for the part
// clipboard, since the same component now serves both. Clicking one applies
// immediately — a separate OK would add a click every time and buy nothing, since the
// click IS the decision. Cancel backs out.
//
// Each row also names the parts currently on that buffer, so "what will this
// disturb" is answered per option rather than in the abstract.
//
// A child of the plugin editor (NOT of EditorView), like the load/read-back
// overlays: a file can be opened with the Hardware photo showing, and the chooser
// has to appear either way.
//==============================================================================
class DrumBufferChooser final : public juce::Component
{
public:
    // buffer 0..3, or -1 for "as saved in the file".
    std::function<void(int buffer)> onChoose;
    std::function<void()>           onCancel;

    DrumBufferChooser()
    {
        title.setJustificationType(juce::Justification::centred);
        title.setColour(juce::Label::textColourId, edcol::kTextTitle);
        title.setBorderSize({});
        addAndMakeVisible(title);

        note.setJustificationType(juce::Justification::centredLeft);
        note.setColour(juce::Label::textColourId, edcol::kTextDim);
        note.setBorderSize({});
        // Deliberately verb-NEUTRAL: this component is shared by a file load and by a
        // part paste, and "writing" is the thing both of them do.
        note.setText("Writing a drum buffer also changes the drums of any other "
                     "part using that buffer.", juce::dontSendNotification);
        addAndMakeVisible(note);

        for (int i = 0; i < kNumOptions; ++i)
        {
            auto& b = btn[i];
            // No keyboard operation here, deliberately: a focused juce::Button clicks
            // on ANY return press (Button::keyPressed ignores the modifiers), and each
            // of these commits a load that overwrites a drum buffer.
            b.setWantsKeyboardFocus(false);
            b.setColour(juce::TextButton::buttonColourId,   edcol::kButtonBgAlt);
            b.setColour(juce::TextButton::textColourOffId,  edcol::kButtonText);
            b.onClick = [this, i]
            {
                // -1 for the last row ("as saved"); 0..3 for a named buffer.
                if (onChoose) onChoose(i < drumbuf::kNumBuffers ? i : -1);
            };
            addAndMakeVisible(b);
        }
        btn[kNumOptions - 1].setButtonText("Buffer File Was Saved With");

        cancel.setButtonText("Cancel");
        cancel.setWantsKeyboardFocus(false);
        cancel.setColour(juce::TextButton::buttonColourId,  edcol::kButtonBg);
        cancel.setColour(juce::TextButton::textColourOffId, edcol::kTextDim);
        cancel.onClick = [this] { if (onCancel) onCancel(); };
        addAndMakeVisible(cancel);
    }

    // `partsOnBuffer[b]` lists the 1-based parts currently using buffer b (empty =
    // none), and `savedBuffer` is the file's own target, named on the last row so
    // "as saved" is not a mystery. `what` names the file for the title.
    // `verb` is what the action DOES ("Load" for a file, "Paste" for the part
    // clipboard) -- the chooser is shared, so the title has to say which one is asking.
    void setContext(const juce::String& what,
                    const juce::StringArray partsOnBuffer[drumbuf::kNumBuffers],
                    int savedBuffer,
                    const juce::String& verb = "Load",
                    const juce::String& savedRow = "Buffer File Was Saved With")
    {
        title.setText(verb + " " + what + " into which drum buffer?",
                      juce::dontSendNotification);

        for (int i = 0; i < drumbuf::kNumBuffers; ++i)
        {
            const juce::String users = partsOnBuffer[i].isEmpty()
                                         ? juce::String("not in use")
                                         : "used by part " + partsOnBuffer[i].joinIntoString(", ");
            btn[i].setButtonText(juce::String(drumbuf::name(i)) + "   (" + users + ")");
        }

        btn[kNumOptions - 1].setButtonText(
            savedRow
            + (drumbuf::validBuffer(savedBuffer) ? "   (" + juce::String(drumbuf::name(savedBuffer)) + ")"
                                                 : juce::String()));
        savedIx = savedBuffer;
    }

    void paint(juce::Graphics& g) override
    {
        // Dim the editor behind it: this is a decision that must be made before the
        // load proceeds, so the rest of the UI should read as inactive.
        g.fillAll(juce::Colour(0x90000000));

        auto r = panel.toFloat();
        g.setColour(edcol::kWindowBg);
        g.fillRoundedRectangle(r, 6.f);
        g.setColour(edcol::kWindowBorder);
        g.drawRoundedRectangle(r.reduced(0.5f), 6.f, 1.f);

        // The buffer swatches: a coloured square OUTLINE, matching the voice browser's
        // DRUM BUFFER box and the Voice list's voice-name outlines. The "as saved" row
        // deliberately has none — it names no particular buffer.
        for (int i = 0; i < drumbuf::kNumBuffers; ++i)
        {
            g.setColour(drumbuf::colour(i));
            g.drawRect(swatch[i], 2);
        }
    }

    void resized() override
    {
        const int rowH = 30, gap = 6, pad = 16;
        const int w = juce::jmin(430, juce::jmax(260, getWidth() - 40));
        const int h = pad + 26 /*title*/ + 8
                    + kNumOptions * rowH + (kNumOptions - 1) * gap
                    + 12 + 34 /*note*/ + 10 + 28 /*cancel*/ + pad;
        panel = juce::Rectangle<int>(0, 0, w, h).withCentre(getLocalBounds().getCentre())
                    .constrainedWithin(getLocalBounds());

        auto b = panel.reduced(pad);
        title.setFont(edtext::font(edtext::Label, 13.f, juce::Font::bold));
        title.setBounds(b.removeFromTop(26));
        b.removeFromTop(8);

        for (int i = 0; i < kNumOptions; ++i)
        {
            auto row = b.removeFromTop(rowH);
            if (i < kNumOptions - 1) b.removeFromTop(gap);

            // The swatch column is reserved on EVERY row, including the unswatched
            // last one, so all five buttons share a left edge.
            auto swCell = row.removeFromLeft(22);
            if (i < drumbuf::kNumBuffers)
            {
                const int sw = 16;
                swatch[i] = juce::Rectangle<int>(0, 0, sw, sw).withCentre(swCell.getCentre());
            }
            row.removeFromLeft(3);
            btn[i].setBounds(row);
        }

        b.removeFromTop(12);
        note.setFont(edtext::font(edtext::Label, 11.f));
        note.setBounds(b.removeFromTop(34));
        b.removeFromTop(10);
        cancel.setBounds(b.removeFromTop(28).removeFromRight(90));
    }

    // Swallow clicks on the dimmed backdrop so nothing behind the chooser reacts
    // while the decision is pending. Clicking outside is NOT a cancel: this appears
    // mid-load, and a stray click silently abandoning the file would be worse than
    // making the user say so.
    void mouseDown(const juce::MouseEvent&) override {}

private:
    static constexpr int kNumOptions = drumbuf::kNumBuffers + 1;

    juce::Label      title, note;
    juce::TextButton btn[kNumOptions];
    juce::TextButton cancel;
    juce::Rectangle<int> panel, swatch[drumbuf::kNumBuffers];
    int savedIx { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumBufferChooser)
};
