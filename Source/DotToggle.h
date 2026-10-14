// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "Accessibility.h"
#include "EditorColours.h"
#include <functional>

//==============================================================================
// DotToggle — a small circular on/off control: an empty ring when off, a filled
// green dot when on. Click toggles. Used for the Portamento switch (PartPanel)
// and the effect tempo-SYNC switch (EffectParametersPanel); reusable for any
// future on/off green-dot toggle.
//==============================================================================
class DotToggle : public juce::Component,
                  public juce::SettableTooltipClient
{
public:
    // Accessibility pillar 2: an accessible ELEMENT is focusable (see ValueBox's
    // constructor for the rule). A screen reader reaches a control by moving
    // KEYBOARD focus to it, so a handler on a component that refuses focus can
    // never be read.
    DotToggle() { setWantsKeyboardFocus(true); }

    std::function<void(bool)> onChange;

    void setState(bool on, bool notify = false)
    {
        state = on;
        repaint();
        if (notify && onChange) onChange(state);
    }
    bool getState() const { return state; }

    // When true, the ring fills the component so the visible circle's diameter
    // tracks the component size (callers size these to ~the value-box height).
    // Default false keeps the small-LED look (e.g. the Controller resend LED).
    void setFillCell(bool shouldFill) { fillCell = shouldFill; repaint(); }

    // ATTENTION variant: the lit dot is drawn in the palette's alert amber rather
    // than its value green. For the one kind of toggle whose ON state is the state
    // that is NOT normal -- today the Part module's EQ BYPASS lamp, where lit means
    // the EQ you can see is not the EQ you are hearing. Green is this editor's
    // "working" colour and lighting it to say "switched off" would be a lie a user
    // has to learn their way past.
    //
    // A FLAG, not a stored colour: a theme change rebuilds the editor and the palette
    // values move with it, so the colour has to be read at paint time.
    void setAlert(bool a) { if (alert != a) { alert = a; repaint(); } }

    // Grayed / uneditable: the ring and the "on" dot both drop to the inert
    // colours and clicks are ignored. The STATE is untouched — a dimmed toggle
    // still shows the value the device holds (see PartPanel's drum-kit gating).
    void setDimmed(bool d) { if (dimmed != d) { dimmed = d; repaint(); } }
    bool isDimmed() const { return dimmed; }

    // MIDI OVERRIDE: incoming MIDI has left the DEVICE in the other state. The RING
    // goes cyan and thickens; the dot keeps showing YOUR value, as everywhere else.
    //
    // A ring rather than a second dot, and no level bar: this is a two-state control,
    // so "the device differs" and "the device is at the opposite state" are the same
    // sentence -- there is no third value a position could encode. Weight as well as
    // colour, because a 1.5 px ring is exactly the thin mark that cannot carry a
    // state on hue alone.
    void setMidiOverride(bool present)
    {
        if (overridden == present) return;
        overridden = present;
        invalidateAccessibilityHandler();   // the spoken value gains a half
        repaint();
    }
    bool hasMidiOverride() const { return overridden; }

    void mouseDown(const juce::MouseEvent&) override { toggleFromUser(); }

    //== keyboard control (see KeyNav.h) =======================================
    // A toggle is a 0/1 parameter -- the device stores it as an OFF/ON byte -- so
    // "up increases" holds: the up arrow switches it on, the down arrow off. Enter
    // flips it, which is what a click does. Both respect `dimmed`, as the click does.
    void toggleFromUser()   { if (! dimmed) setState(! state, true); }

    // See ValueBox::setTooltip -- an accessible name must not vanish mid-edit.
    void setTooltip(const juce::String& t) override { rawTip = t; juce::SettableTooltipClient::setTooltip(t); }

    // Accessibility (pillar 1): a checkable toggle, so a screen reader says "on" /
    // "off" rather than reading a number. No value interface -- checked IS the value.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        // A toggle has no VALUE interface -- checked IS its value -- so an override
        // cannot ride the spoken value the way a value box's does. It goes into the
        // NAME instead, which a screen reader reads when it lands on the control.
        // Announced on focus rather than the instant it arrives, which is the honest
        // limit of doing it this way and is worth more than nothing.
        auto tip = rawTip;
        if (overridden)
        {
            const auto d = mu100a11y::descOf(rawTip);
            tip = mu100a11y::nameOf(rawTip)
                + ", MIDI has the device at the other setting"
                + (d.isEmpty() ? juce::String() : " - " + d);
        }
        return std::make_unique<mu100a11y::ToggleHandler>(
            *this, tip,
            [this] { return state; },
            [this] { toggleFromUser(); });
    }
    // No-op when it is already there: setState notifies unconditionally, and a
    // redundant onChange is a wasted SysEx message and a spurious undo record.
    void nudge(int dir)
    {
        if (dimmed || dir == 0 || state == (dir > 0)) return;
        setState(dir > 0, true);
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float cell = juce::jmin(b.getWidth(), b.getHeight()) - 2.f;

        const bool  ovr   = overridden && ! dimmed;
        const float ringW = ovr ? 2.5f : 1.5f;
        const juce::Colour ringCol = dimmed ? edcol::kFieldBorder
                                   : (ovr ? edcol::kMidiOverride : edcol::kTextDim);
        const juce::Colour dotCol  = dimmed ? edcol::kValueOff
                                            : (alert ? edcol::kAlert : edcol::kValue);

        if (fillCell)
        {
            // Ring fills the cell; the visible circle ≈ the component size. The
            // filled green "on" dot sits inside the ring.
            const auto ring = juce::Rectangle<float>(cell, cell).withCentre(b.getCentre());
            g.setColour(ringCol);
            g.drawEllipse(ring, ringW);
            if (state)
            {
                const float dotD = cell * 0.58f;
                g.setColour(dotCol);
                g.fillEllipse(juce::Rectangle<float>(dotD, dotD).withCentre(b.getCentre()));
            }
            return;
        }

        // Small-LED look: the filled green "on" dot keeps its established scale
        // (~40% of the cell). The outline ring hugs that dot — just barely larger
        // — instead of filling the whole cell, so it stays proportionate as the
        // editor scales up.
        const float dotD = cell * 0.40f;
        const auto  dot  = juce::Rectangle<float>(dotD, dotD).withCentre(b.getCentre());
        const float ringPad = juce::jmax(2.0f, dotD * 0.14f);
        const auto  ring = dot.expanded(ringPad);

        g.setColour(ringCol);
        g.drawEllipse(ring, ringW);
        if (state)
        {
            g.setColour(dotCol);
            g.fillEllipse(dot);
        }
    }

private:
    bool state { false };
    bool fillCell { false };
    bool dimmed { false };
    bool overridden { false };
    bool alert { false };
    juce::String rawTip;   // see setTooltip
};
