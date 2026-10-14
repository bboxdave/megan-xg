// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "EditorColours.h"
#include <memory>
#include <utility>

//==============================================================================
// BusyOverlay — a dim full-surface scrim with a centred message panel and a
// progress bar. It is an ordinary child Component (NOT an OS-level window or
// dialog), so it is completely DAW-safe: no focus stealing, z-order, or
// host-modality issues. It intercepts mouse input so the user can't make edits
// (or trigger menu actions) while a long device operation is in flight.
//
// Used for both the "Reading from device" read-back overlay (inside EditorView)
// and the "Loading file" overlay (owned by the host editor, where it covers
// whichever view — Hardware or Editor — is currently on screen).
//==============================================================================
class BusyOverlay : public juce::Component
{
public:
    BusyOverlay(juce::String titleText, juce::String subtitleText)
        : title(std::move(titleText)), subtitle(std::move(subtitleText))
    {
        progressBar = std::make_unique<juce::ProgressBar>(progress);
        progressBar->setPercentageDisplay(false);
        addAndMakeVisible(*progressBar);
        setInterceptsMouseClicks(true, true);   // swallow clicks to the surface below
    }

    void setProgress(double p) { progress = juce::jlimit(0.0, 1.0, p); }

    // Optionally add a small button at the bottom of the panel (e.g. "Stop
    // reading"), firing onStop when clicked. Not shown unless this is called, so
    // overlays that have nothing to cancel (the "Loading file" overlay) stay
    // button-free. Adding the button grows the panel to make room for it.
    void setActionButton(const juce::String& label)
    {
        actionButton = std::make_unique<juce::TextButton>(label);
        // Never keyboard-operated: a focused Button clicks on any return key press
        // (modifiers included), and this one aborts a read-back.
        actionButton->setWantsKeyboardFocus(false);
        actionButton->onClick = [this] { if (onStop) onStop(); };
        addAndMakeVisible(*actionButton);
        resized();
    }

    std::function<void()> onStop;   // fired by the action button, if one is set

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xc0000000));                 // dim the surface behind
        const auto panel = panelBounds().toFloat();
        g.setColour(edcol::kWindowBg);
        g.fillRoundedRectangle(panel, 10.f);
        g.setColour(edcol::kWindowBorder);
        g.drawRoundedRectangle(panel, 10.f, 1.5f);

        auto text = panel.reduced(18.f);
        g.setColour(edcol::kTextTitle);
        g.setFont(juce::FontOptions(19.f, juce::Font::bold));
        g.drawText(title, text.removeFromTop(30.f), juce::Justification::centred);
        g.setColour(edcol::kTextLabel);
        g.setFont(juce::FontOptions(14.f));
        g.drawText(subtitle, text.removeFromTop(24.f), juce::Justification::centred);
    }

    void resized() override
    {
        auto panel = panelBounds().reduced(22, 0);
        if (actionButton)
        {
            auto row = panel.removeFromBottom(38);   // bottommost strip: the button
            actionButton->setBounds(row.withTrimmedBottom(10).withSizeKeepingCentre(110, 24));
        }
        auto bar = panel.removeFromBottom(40).withTrimmedBottom(16).withHeight(18);
        progressBar->setBounds(bar);
    }

private:
    juce::Rectangle<int> panelBounds() const
    {
        // Grow the panel to fit the optional action button below the progress bar.
        const int h = actionButton ? 168 : 132;
        return juce::Rectangle<int>(0, 0, 360, h).withCentre(getLocalBounds().getCentre());
    }

    juce::String title, subtitle;
    double progress { 0.0 };
    std::unique_ptr<juce::ProgressBar> progressBar;
    std::unique_ptr<juce::TextButton>  actionButton;   // optional "Stop reading" etc.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BusyOverlay)
};
