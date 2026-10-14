// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

// HistoryPanel.h — the Undo/Redo HISTORY window (the hamburger between the arrows).
//
// A step-by-step account of the session's editor edits, oldest at the top, with the
// state you are currently in marked in the Graphic Display's selected-green. Clicking
// an entry returns the plugin to the state it was in right after that edit; clicking
// the top entry returns it to before any edit was made. Modelled on Arturia Pigments,
// at the user's request.
//
// THE LIST IS THE UNDO STACK, not a copy of it. Entry 0 is the state before the oldest
// retained step and entry i the state just after step i, so `undoPosition()` IS the
// index of the current entry and there is nothing to keep in sync — a history that can
// disagree with the stack it describes is worse than none.
//
// Two consequences of the 20-step cap are visible here and both are deliberate:
//   - Entry 0 is only called "Initial State" while `undoHasInitialState()`. Once the
//     cap has discarded the oldest steps it is merely the oldest state still reachable,
//     and the honest label is "Earlier State" — same principle as the undo labels
//     falling back to a raw number rather than printing a plausible wrong unit.
//   - Entries BELOW the current one are the redo branch. They are drawn dimmed because
//     the next edit discards them, which is the one thing about them worth signalling.
//
// A child of the plugin editor (like DrumBufferChooser, and for the same reason): the
// strip it hangs off shows over both the Hardware photo and the Editor view.
//
// The backdrop is transparent, not dimmed. This is a casual navigation pop-up, not a
// decision that must be made before anything else proceeds, so a click anywhere else
// simply closes it — the behaviour of the menus in the same strip.
//
// The COLOURS are this panel's own (see the k*Bg / k*Text constants), no longer the
// LookAndFeel's PopupMenu ids. Borrowing those made the window consistent with the
// File/Options menus but painted the hover band DARKER than the panel, so it barely
// read. Visibility wins here, at the user's request — and Pigments' own history
// window departs from its menus for the same reason.

#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <functional>
#include "EditorColours.h"
#include "EditorTextScale.h"

class HistoryPanel final : public juce::Component
{
public:
    // Pull the list from the processor. `count` = number of steps, `pos` = the current
    // entry (0..count), `initial` = may entry 0 be called "Initial State".
    std::function<int(void)>                 getStepCount;
    std::function<int(void)>                 getPosition;
    std::function<bool(void)>                hasInitialState;
    std::function<juce::String(int)>         getStepLabel;   // 0-based step -> "what it did"
    std::function<void(int)>                 onGoTo;         // entry index 0..count
    std::function<void()>                    onDismiss;

    // It DOES take keyboard focus: the arrow keys walk the list and Return/Escape
    // close it (see keyPressed). Focus is grabbed in openAt and released when the
    // panel hides, at which point the editor's own focus service takes it back.
    HistoryPanel() { setWantsKeyboardFocus(true); }

    // `anchor` is the hamburger button's bounds in the parent's coordinates; the panel
    // opens ABOVE it, left-aligned with it, and slides inside the window if it would
    // hang off an edge.
    void openAt(juce::Rectangle<int> anchor)
    {
        anchorRect = anchor;
        resized();          // the anchor arrives AFTER setBounds, so re-lay-out on it
        rebuild();
        // Open showing the current entry, scrolled to the BOTTOM of the list when it
        // overflows — the newest edits are what a history is usually consulted about,
        // and it matches where the arrows leave you.
        scrollTo(entries.size() - kVisibleRows);
        ensureVisible(position);
        setVisible(true);
        toFront(true);
        grabKeyboardFocus();
    }

    // Cheap enough to call from the editor's timer: rebuilds only when the stack has
    // actually moved (the labels are formatted lazily, so a rebuild is real work).
    void refreshIfChanged()
    {
        if (! isVisible()) return;
        if (getStepCount == nullptr || getPosition == nullptr) return;
        const bool init = (hasInitialState != nullptr) ? hasInitialState() : builtInitial;
        bool same = (getStepCount() == builtCount && getPosition() == position
                     && init == builtInitial);

        // The count can stay put while the NEWEST entry's text moves: a drag merges
        // into the open step on every value change, so its "64 -> 100" keeps growing.
        // Re-read that one label — one format per tick, against the ~21 a full rebuild
        // would cost.
        if (same && builtCount > 0 && getStepLabel != nullptr)
            same = (getStepLabel(builtCount - 1) == entries[builtCount]);

        if (same) return;
        rebuild();
        ensureVisible(position);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto r = panel.toFloat();

        // The panel is painted in three tiers of darkness, deepest under the green
        // (see kHeaderBg / kListBg / kCurrentBg). Header first over the whole
        // rounded rect, then the list block over the lower part of it — squared off
        // at the top so only the panel's own bottom corners stay rounded.
        g.setColour(kHeaderBg);
        g.fillRoundedRectangle(r, 4.0f);

        auto lower = juce::Rectangle<float>((float) panel.getX(), (float) listRect.getY(),
                                            (float) panel.getWidth(),
                                            (float) (panel.getBottom() - listRect.getY()));
        g.setColour(kListBg);
        g.fillRoundedRectangle(lower, 4.0f);
        g.fillRect(lower.withHeight(6.0f));

        g.setColour(edcol::kWindowBorder);
        g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);

        g.setColour(edcol::kTextTitle);
        g.setFont(headerFont());
        g.drawText("HISTORY", headerRect.reduced(kPad, 0), juce::Justification::centredLeft);
        g.setColour(edcol::kRule);
        g.fillRect(headerRect.getX() + kPad, headerRect.getBottom() - 1,
                   headerRect.getWidth() - 2 * kPad, 1);

        g.setFont(rowFont());
        g.reduceClipRegion(listRect);

        for (int i = 0; i < entries.size(); ++i)
        {
            const auto row = rowBounds(i);
            if (! row.intersects(listRect)) continue;

            const bool current = (i == position);
            const bool hovered = (i == hoverRow);

            // The current entry keeps its near-black band whether hovered or not —
            // it is the one thing on the panel that must pop — and only lifts a
            // shade under the cursor so it still answers the mouse.
            if (current)         { g.setColour(hovered ? kCurrentHover : kCurrentBg); g.fillRect(row); }
            else if (hovered)    { g.setColour(kRowHover);                            g.fillRect(row); }

            // Current = the value-box green, so the row reads like a lit parameter
            // cell. Everything else is a mid grey that lifts to white under the
            // cursor. The redo branch — everything below the current entry — is
            // dimmed, because the next edit throws it away.
            juce::Colour c = current ? kCurrentText
                                     : (hovered ? edcol::kTextTitle : kRowText);
            if (! current && i > position)
                c = c.withMultipliedAlpha(0.6f);

            g.setColour(c);
            g.drawText(entries[i], row.reduced(kPad + 2, 0),
                       juce::Justification::centredLeft, true);
        }

        // Scrollbar: only when the list overflows. Drawn inside the list rect's right
        // edge, which is also where the drag hit-test lives (see scrollbarRect).
        if (maxScroll() > 0)
        {
            auto track = scrollbarRect();
            g.setColour(juce::Colour(0x30ffffff));
            g.fillRoundedRectangle(track.toFloat(), kScrollW * 0.5f);
            g.setColour(juce::Colour(0x90ffffff));
            g.fillRoundedRectangle(thumbRect().toFloat(), kScrollW * 0.5f);
        }
    }

    void resized() override
    {
        // Width: wide enough for the labels this feature produces ("Drum COWBELL EQ 2
        // GAIN +2.3dB -> 0.0dB"), narrowed to fit a small window. Anything longer
        // ellipsizes rather than widening the panel — a pop-up wider than the plugin
        // window is the trap Open Recent already hit.
        const int w = juce::jlimit(200, juce::jmax(200, getWidth() - 16), 340);
        const int h = headerH() + kVisibleRows * rowH() + 2 * kPad;

        int x = anchorRect.getX();
        int y = anchorRect.getY() - 6 - h;          // above the button, like Pigments
        if (y < 4) y = juce::jmin(anchorRect.getBottom() + 6, juce::jmax(4, getHeight() - h - 4));
        x = juce::jlimit(4, juce::jmax(4, getWidth() - w - 4), x);

        panel      = { x, y, w, h };
        headerRect = { x, y + kPad, w, headerH() };
        listRect   = { x, headerRect.getBottom(), w, kVisibleRows * rowH() };
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (! panel.contains(e.getPosition()))
        {
            if (onDismiss) onDismiss();
            return;
        }

        if (maxScroll() > 0 && scrollbarRect().expanded(3, 0).contains(e.getPosition()))
        {
            dragging   = true;
            dragAnchor = e.position.y;
            dragScroll = scroll;
            return;
        }

        const int row = rowAt(e.getPosition());
        if (row >= 0 && onGoTo)
        {
            onGoTo(row);
            // The panel stays open: stepping back through several entries is the normal
            // way this is used, and closing after each click would make that four
            // round trips through the button.
            refreshIfChanged();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! dragging) return;
        // Track-proportional: dragging the thumb the length of the track scrolls the
        // whole list, which is what makes a 20-entry list reachable in one gesture.
        const float travel = (float) juce::jmax(1, listRect.getHeight() - thumbRect().getHeight());
        setScroll(dragScroll + (int) std::lround((e.position.y - dragAnchor)
                                                 * (float) maxScroll() / travel));
    }

    void mouseUp(const juce::MouseEvent&) override { dragging = false; }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const int row = rowAt(e.getPosition());
        if (row != hoverRow) { hoverRow = row; repaint(); }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hoverRow != -1) { hoverRow = -1; repaint(); }
    }

    // Arrow keys walk the list, and landing on an entry DOES WHAT CLICKING IT DOES --
    // the same law the Voice List follows. It is not merely for consistency: an entry
    // is a STATE, and the only way to know a state is to replay the steps between here
    // and there, so there is no such thing as selecting a row without going to it. A
    // "move the highlight, press Return to accept" design cannot be built cheaply, and
    // would be a different feature rather than a variant of this one.
    //
    // Return therefore ACCEPTS NOTHING; like the Voice List's, it is purely a close
    // shortcut, and so is Escape. Every jump is itself undoable, which is what makes
    // walking the list safe to do by feel.
    bool keyPressed(const juce::KeyPress& k) override
    {
        if (! isVisible()) return false;

        if (k.isKeyCode(juce::KeyPress::returnKey)
            || k.isKeyCode(juce::KeyPress::escapeKey))
        {
            if (onDismiss) onDismiss();
            return true;
        }

        const int last = entries.size() - 1;
        if (last < 0) return false;

        int target = position;
        if      (k.isKeyCode(juce::KeyPress::upKey))       target = position - 1;
        else if (k.isKeyCode(juce::KeyPress::downKey))     target = position + 1;
        else if (k.isKeyCode(juce::KeyPress::pageUpKey))   target = position - kVisibleRows;
        else if (k.isKeyCode(juce::KeyPress::pageDownKey)) target = position + kVisibleRows;
        else if (k.isKeyCode(juce::KeyPress::homeKey))     target = 0;
        else if (k.isKeyCode(juce::KeyPress::endKey))      target = last;
        else return false;   // anything else bubbles on to the editor's shortcuts

        target = juce::jlimit(0, last, target);

        // At either end the key is still CONSUMED but nothing is replayed. Re-issuing
        // the same jump would seal and re-seal the step on every extra press, and it
        // would cost a round trip through undoGoTo for no movement.
        if (target != position && onGoTo)
        {
            onGoTo(target);
            refreshIfChanged();
            ensureVisible(position);
            repaint();
        }
        return true;
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (! panel.contains(e.getPosition())) return;
        if (maxScroll() <= 0) return;
        const float dy = w.deltaY * (w.isReversed ? -1.0f : 1.0f);
        if (dy == 0.0f) return;

        // A NOTCHED wheel must move a row on EVERY click, so it does not go through
        // the accumulator at all. It used to: one Windows notch is deltaY 0.234
        // (`amount / 256` in juce_Windowing_windows.cpp, amount = 0.5 * WHEEL_DELTA),
        // and at the old 3.0 gain that is 0.70 of a row — so the first click moved
        // nothing and, the residue never quite reaching a whole row, roughly every
        // third click after it moved nothing either. Rounding keeps a multi-notch
        // message (some drivers batch them) proportional, and the floor of 1
        // guarantees a click is never swallowed whatever a platform's notch is worth.
        if (! w.isSmooth)
        {
            const int rows = juce::jmax(1, juce::roundToInt(std::abs(dy) * kWheelRowsPerUnit));
            wheelAccum = 0.0f;
            setScroll(scroll - (dy > 0.0f ? rows : -rows));
            return;
        }

        // Smooth (trackpad / precision) wheels deliver many small deltas and DO need
        // the accumulator, or every one of them would be rounded up to a whole row.
        wheelAccum += dy * kWheelRowsPerUnit;
        const int steps = (int) wheelAccum;
        if (steps != 0)
        {
            wheelAccum -= (float) steps;
            setScroll(scroll - steps);
        }
    }

private:
    static constexpr int kVisibleRows = 5;      // the user's spec
    static constexpr int kPad         = 6;

    // The two fonts ride `edtext`, so settings.xml can size them like the rest of
    // the editor's text: HISTORY is a Title (editorTextTitlesPct) and the entries
    // are Labels (editorTextLabelsPct). The base heights are chosen so 100% renders
    // exactly the tuned sizes — 10.0 * kBaseline 1.30 = the 13 px rows, and the
    // header is deliberately ~2 points above them.
    static juce::FontOptions headerFont() { return edtext::font(edtext::Title, 10.77f,
                                                                juce::Font::bold); }
    static juce::FontOptions rowFont()    { return edtext::font(edtext::Label, 10.0f); }

    // MEASURED cell heights, floored at the tuned defaults — `edtext` scales fonts
    // but NOT the cells that hold them, so a raised percentage clips anything given
    // fixed pixels. The padding terms are
    // what the old fixed 22s amounted to at 100%, so the default look is unchanged.
    static int rowH()    { return juce::jmax(22, juce::roundToInt(rowFont().getHeight()) + 9); }
    static int headerH() { return juce::jmax(22, juce::roundToInt(headerFont().getHeight()) + 8); }
    static constexpr int kScrollW     = 5;

    // Rows per unit of MouseWheelDetails::deltaY. 256/60 makes one Windows notch
    // exactly one row; see mouseWheelMove.
    static constexpr float kWheelRowsPerUnit = 256.0f / 60.0f;

    // The palette is the panel's OWN, not the LookAndFeel's PopupMenu ids it used
    // to borrow — at the user's request (2026-08-07), visibility over consistency
    // with the plugin's menus. Pigments' own history window breaks from its menus
    // the same way. Three tiers of darkness, deepest under the green so the current
    // entry pops: header < list, and the current row is kValueBg — the same near-
    // black as a parameter value cell, which is what its green is borrowed from.
    // References, not copies: these are static members initialised at static-init
    // time, before any theme is chosen, so a copy would freeze the declared default.
    static inline const juce::Colour& kHeaderBg     = edcol::kHistHeaderBg;
    static inline const juce::Colour& kListBg       = edcol::kHistListBg;
    static inline const juce::Colour& kCurrentBg    = edcol::kHistCurrentBg;   // = ValueBox's bg
    static inline const juce::Colour& kCurrentHover = edcol::kHistCurrentHov;
    static inline const juce::Colour& kRowHover     = edcol::kHistRowHover;    // a shade ABOVE kListBg
    static inline const juce::Colour& kCurrentText  = edcol::kHistCurrentTxt;  // = XgSelector's green
    static inline const juce::Colour& kRowText      = edcol::kHistRowText;

    void rebuild()
    {
        entries.clear();
        builtCount   = (getStepCount     != nullptr) ? getStepCount()     : 0;
        position     = (getPosition      != nullptr) ? getPosition()      : 0;
        builtInitial = (hasInitialState  != nullptr) ? hasInitialState()  : true;

        entries.add(builtInitial ? "Initial State" : "Earlier State");
        for (int i = 0; i < builtCount; ++i)
        {
            auto s = (getStepLabel != nullptr) ? getStepLabel(i) : juce::String();
            entries.add(s.isNotEmpty() ? s : juce::String("Parameter change"));
        }
        setScroll(scroll);      // re-clamp: the list may have shrunk
    }

    juce::Rectangle<int> rowBounds(int i) const
    {
        return { listRect.getX(), listRect.getY() + (i - scroll) * rowH(),
                 listRect.getWidth(), rowH() };
    }

    int rowAt(juce::Point<int> p) const
    {
        if (! listRect.contains(p)) return -1;
        const int i = scroll + (p.y - listRect.getY()) / rowH();
        return (i >= 0 && i < entries.size()) ? i : -1;
    }

    int  maxScroll() const { return juce::jmax(0, entries.size() - kVisibleRows); }
    void setScroll(int s)  { const int n = juce::jlimit(0, maxScroll(), s);
                             if (n != scroll) { scroll = n; repaint(); } }
    void scrollTo(int s)   { setScroll(s); }

    void ensureVisible(int row)
    {
        if (row < scroll)                        setScroll(row);
        else if (row >= scroll + kVisibleRows)   setScroll(row - kVisibleRows + 1);
    }

    juce::Rectangle<int> scrollbarRect() const
    {
        return { listRect.getRight() - kPad - kScrollW, listRect.getY() + 2,
                 kScrollW, listRect.getHeight() - 4 };
    }

    juce::Rectangle<int> thumbRect() const
    {
        auto track = scrollbarRect();
        const int n = juce::jmax(1, entries.size());
        const int h = juce::jmax(14, track.getHeight() * kVisibleRows / n);
        const int y = track.getY() + (track.getHeight() - h)
                        * scroll / juce::jmax(1, maxScroll());
        return { track.getX(), y, track.getWidth(), h };
    }

    juce::StringArray entries;
    juce::Rectangle<int> anchorRect, panel, headerRect, listRect;
    int   position     { 0 };
    int   builtCount   { -1 };
    bool  builtInitial { true };
    int   scroll       { 0 };
    int   hoverRow     { -1 };
    bool  dragging     { false };
    float dragAnchor   { 0.0f };
    int   dragScroll   { 0 };
    float wheelAccum   { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HistoryPanel)
};
