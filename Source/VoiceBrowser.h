// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "EditorColours.h"
#include "codec/MU100Voices.h"
#include "VoiceNaming.h"          // full vs 8-character voice names
#include "VoiceIndicators.h"
#include "EditorTextScale.h"
#include "DrumBuffer.h"
#include "KeyboardShortcuts.h"   // the region keys reuse the Controls bindings
#include <vector>
#include <functional>

//==============================================================================
// A minimal ListBoxModel driven by lambdas, so one VoiceBrowser can own three
// independent columns without three model subclasses. Only user clicks navigate
// or assign (listBoxItemClicked), so programmatic selection never misfires.
//==============================================================================
class LambdaListModel : public juce::ListBoxModel
{
public:
    std::function<int()>             numRows;
    std::function<juce::String(int)> textForRow;
    // If set and it returns true (filling model/elements), a fixed indicator
    // gutter is reserved + drawn before the name (so names stay left-aligned).
    std::function<bool(int, int&, int&)> voiceForRow;
    std::function<void(int)>         onClick;
    // ListBox::keyPressed CONSUMES the return key whenever a row is selected (it
    // calls returnKeyPressed and returns true), so it never bubbles up to the
    // browser. Forward it explicitly or the shortcut dies in whichever column
    // last took focus.
    std::function<void()>            onReturnKey;

    int getNumRows() override { return numRows ? numRows() : 0; }

    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (! textForRow) return;
        if (sel) { g.setColour(edcol::kListSelBg); g.fillRect(0, 0, w, h); }
        int textX = 5, model = 0, elements = 1;
        // Rows that resolve to an actual voice/kit get the indicator gutter. All
        // browser text (group / category / voice rows, i.e. every column) uses the
        // VoiceList size so the three columns match.
        if (voiceForRow && voiceForRow(row, model, elements))
        {
            voiceind::draw(g, juce::Rectangle<int>(4, 0, voiceind::kGutter, h), model, elements);
            textX = 4 + voiceind::kGutter + 2;
        }
        g.setColour(sel ? edcol::kListSelText : edcol::kListText);
        g.setFont(edtext::font(edtext::VoiceList, 12.f));
        g.drawText(textForRow(row), textX, 0, w - textX - 3, h, juce::Justification::centredLeft);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent&) override
    {
        if (onClick) onClick(row);
    }

    void returnKeyPressed(int) override
    {
        if (onReturnKey) onReturnKey();
    }
};

//==============================================================================
// A ListBox that reports KEYBOARD navigation, so arrowing to a row does what
// clicking it does. `selectedRowsChanged` cannot be used for this: it fires on
// programmatic selection too (showFor, selectGroup, highlightBufferKit), which is
// exactly why the model above only acts on listBoxItemClicked. Asking the base
// class whether it consumed a navigation key isolates the user's own keystrokes.
//==============================================================================
class NavListBox : public juce::ListBox
{
public:
    std::function<void(int)> onKeyboardNav;

    bool keyPressed(const juce::KeyPress& k) override
    {
        const bool isNav = k.isKeyCode(juce::KeyPress::upKey)
                        || k.isKeyCode(juce::KeyPress::downKey)
                        || k.isKeyCode(juce::KeyPress::pageUpKey)
                        || k.isKeyCode(juce::KeyPress::pageDownKey)
                        || k.isKeyCode(juce::KeyPress::homeKey)
                        || k.isKeyCode(juce::KeyPress::endKey);
        const int before = getSelectedRow();
        const bool used  = juce::ListBox::keyPressed(k);
        // Only on a real MOVE: at either end of the list the key is still consumed,
        // and re-firing there would re-send the same voice on every extra press.
        if (used && isNav && onKeyboardNav)
        {
            const int now = getSelectedRow();
            if (now >= 0 && now != before) onKeyboardNav(now);
        }
        return used;
    }
};

//==============================================================================
// VoiceBrowser — XGEdit's three-column patch browser, shown as a NON-MODAL,
// draggable floating panel (the rest of the editor stays live underneath, like
// XGEdit; single window, so it's DAW-safe). Drag the title bar to move it; HIDE
// closes it. Column 1 picks a high-level group; Column 2 and Column 3 narrow
// down to a specific voice or kit:
//
//   Melody         -> Col2 = the 16 instrument categories, Col3 = its voices
//   SFX            -> Col2 = the SFX voices (bank MSB 64),   Col3 = blank
//   MU100 Exclusive-> Col2 = the MU100 voices (bank MSB 48), Col3 = blank
//   SFX Kit        -> Col2 = the SFX drum kits (MSB 126),    Col3 = blank
//   Drum Kit       -> Col2 = the drum kits (MSB 127),        Col3 = blank
//
// Clicking a leaf (a voice in Col3, or a voice/kit in Col2 for the non-Melody
// groups) fires onPick with its bank/program; the browser stays open (HIDE or
// RETURN closes it) so several parts can be auditioned in turn. ARROW KEYS do the
// same as a click, row by row, so a list can be auditioned from the keyboard and
// whatever it landed on is simply the voice in force when the panel closes.
//==============================================================================
class VoiceBrowser : public juce::Component
{
public:
    // onPick(bankMSB, bankLSB, program0based, displayName, drumBuffer, fromKeyboard)
    // drumBuffer is the DRUMS1..DRUMS4 index (0..3) the kit was allocated to, or
    // -1 for a plain voice/kit pick (no editable drum buffer). fromKeyboard says the
    // pick came from arrow navigation rather than a click, so a run of them folds
    // into ONE undo step (see EditorView) instead of burying the stack.
    std::function<void(int, int, int, juce::String, int, bool)> onPick;
    std::function<void()> onHide;
    // What command a key is bound to, or -1. Supplied by EditorView so the browser
    // reads the SAME kbsc::Bindings the rest of the editor does: rebinding "Next
    // module" or "Next control" moves the regions here too, and the shortcut editor
    // needs no rows of its own to stay truthful. With the shortcut layer switched OFF
    // it falls back to the DEFAULT bindings for the four region commands only -- the
    // panel's half of the accessibility key floor, without which the regions could not
    // be walked at all and every area but the opening column was unreachable. See the
    // long note at the assignment in EditorView, including why Tab is in this floor
    // when it is deliberately left out of the editor's.
    std::function<int(const juce::KeyPress&)> commandForKey;
    // Options ▸ Show the arrow-key mark. Supplied by EditorView so ONE setting governs
    // this frame and the editor's ring alike -- two marks that disagreed about when to
    // appear would be worse than either rule on its own.
    std::function<bool()> markVisible;
    std::function<void()> onKeyboardUsed;
    // What kit a DRUMS buffer currently holds, or false if it holds none. Supplied by
    // EditorView, which reads it from PartListPanel -- the AUTHORITY on which part is
    // on which buffer, since that is what a file load, a device read-back and a project
    // restore all write (PART MODE 2..5). The browser used to cache this itself, which
    // could only ever know about picks made through the browser in this session, and
    // was seeded with a hardcoded guess for DRUMS1/DRUMS3 that went stale the moment
    // anything moved. Derive it; do not keep a second copy.
    std::function<bool(int, int&, int&, int&)> kitForBuffer;

    enum Group { Melody = 0, SFX, MU100Exclusive, SFXKit, DrumKit, kNumGroups };

    VoiceBrowser()
    {
        // RETURN closes the browser (see keyPressed). The panel itself takes focus
        // on open so the shortcut works before anything has been clicked.
        setWantsKeyboardFocus(true);

        for (auto* l : { &col1, &col2, &col3 })
        {
            l->setRowHeight(18);
            l->setColour(juce::ListBox::backgroundColourId, edcol::kListBg);
            addAndMakeVisible(*l);
        }

        col1Model.numRows    = [] { return (int) kNumGroups; };
        col1Model.textForRow = [](int r) { return juce::String(kGroupNames[r]); };
        col1Model.onClick  = [this](int r) { noteFocusRegion(RegCol1); chooseGroup(r); };
        col1.onKeyboardNav = [this](int r) { if (onKeyboardUsed) onKeyboardUsed(); chooseGroup(r); };
        col1.setModel(&col1Model);

        col2Model.numRows    = [this] { return (int) col2rows.size(); };
        col2Model.textForRow = [this](int r) { return col2Text(r); };
        col2Model.voiceForRow = [this](int r, int& m, int& e) { return col2Voice(r, m, e); };
        col2Model.onClick  = [this](int r) { noteFocusRegion(RegCol2); pickCol2(r, false); };
        col2.onKeyboardNav = [this](int r) { if (onKeyboardUsed) onKeyboardUsed(); pickCol2(r, true); };
        col2.setModel(&col2Model);

        col3Model.numRows    = [this] { return (int) col3rows.size(); };
        col3Model.textForRow = [this](int r)
        { return juce::String(voicenames::displayName(mu100::kVoices[col3rows[(size_t) r]])); };
        col3Model.voiceForRow = [this](int r, int& m, int& e)
        {
            if (r < 0 || r >= (int) col3rows.size()) return false;
            const auto& v = mu100::kVoices[col3rows[(size_t) r]];
            m = v.model; e = v.elements; return true;
        };
        col3Model.onClick  = [this](int r) { noteFocusRegion(RegCol3); pickCol3(r, false); };
        col3.onKeyboardNav = [this](int r) { if (onKeyboardUsed) onKeyboardUsed(); pickCol3(r, true); };
        col3.setModel(&col3Model);

        for (auto* m : { &col1Model, &col2Model, &col3Model })
            m->onReturnKey = [this] { close(); };

        titleLabel.setText("Voice List", juce::dontSendNotification);
        titleLabel.setFont(edtext::font(edtext::Title, 14.f, juce::Font::bold));
        titleLabel.setColour(juce::Label::textColourId, edcol::kTextTitle);
        titleLabel.setInterceptsMouseClicks(false, false);   // let the title bar drag
        addAndMakeVisible(titleLabel);

        hideButton.setButtonText("HIDE");
        hideButton.onClick = [this] { close(); };
        // Buttons must not take keyboard focus: a focused JUCE button swallows
        // RETURN as its own click, which would make the close shortcut depend on
        // what was last pressed.
        hideButton.setWantsKeyboardFocus(false);
        addAndMakeVisible(hideButton);

        // Drum edit buffers (below column 1). DRUMS1 and DRUMS3 hold the Standard
        // Kit by default (matching the MU100's default part 10 / part 26 drums).
        for (int i = 0; i < drumbuf::kNumBuffers; ++i)
        {
            drumBtn[i].setButtonText(drumbuf::name(i));
            drumBtn[i].onClick = [this, i]
            {
                // A click also moves the KEYBOARD's region to the buffer box, so the cue
                // can never disagree with what the arrows would move next. The buttons
                // still take no focus of their own -- a focused juce::Button eats Return,
                // which is the whole reason this box is driven from the panel instead.
                drumFocus = i;
                selectDrumBuffer(i);
                setFocusRegion(RegDrums);
            };
            drumBtn[i].setWantsKeyboardFocus(false);
            addAndMakeVisible(drumBtn[i]);
        }
        updateDrumButtons();

        selectGroup(Melody);
    }

    // Open, pre-selecting the group/category/voice matching the part's voice.
    // `prg0` is the 0-based MIDI program (matching the editor's model); the
    // voice/kit tables store 1-based program, so convert here.
    void showFor(int msb, int lsb, int prg0)
    {
        disarmBuffer();          // (re)opening or retargeting to a new part disarms
        const int pgm = prg0 + 1;
        const Group g = msb == 64  ? SFX
                      : msb == 48  ? MU100Exclusive
                      : msb == 126 ? SFXKit
                      : msb == 127 ? DrumKit
                                   : Melody;
        selectGroup(g);
        col1.selectRow(g);

        if (g == Melody)
        {
            const int vi = findVoice(msb, lsb, pgm);
            if (vi >= 0)
            {
                const int cat = mu100::kVoices[vi].category;
                selectMelodyCategory(cat);
                for (int r = 0; r < (int) col2rows.size(); ++r)
                    if (col2rows[(size_t) r] == cat) { col2.selectRow(r); break; }
                for (int r = 0; r < (int) col3rows.size(); ++r)
                    if (col3rows[(size_t) r] == vi)
                    { col3.selectRow(r); col3.scrollToEnsureRowIsOnscreen(r); break; }
            }
            else { col2.deselectAllRows(); col3.deselectAllRows(); }
        }
        else   // SFX / MU100 Exclusive / SFX Kit / Drum Kit: col2rows = voice ids
        {
            const int vi = findVoice(msb, lsb, pgm);
            for (int r = 0; r < (int) col2rows.size(); ++r)
                if (col2rows[(size_t) r] == vi)
                { col2.selectRow(r); col2.scrollToEnsureRowIsOnscreen(r); break; }
        }
        setVisible(true);
        toFront(true);
        focusBrowsingColumn();
    }

    // The DRUMS buffer the next kit pick will be allocated to (-1 = none).
    int getSelectedBuffer() const { return selectedBuffer; }

    //== layout / paint ========================================================
    void resized() override
    {
        auto b = getLocalBounds().reduced(8);
        titleBar = b.removeFromTop(22);
        titleLabel.setBounds(titleBar);
        auto footer = b.removeFromBottom(28);
        hideButton.setBounds(footer.withSizeKeepingCentre(90, 24));
        b.removeFromBottom(10);                         // ~half a line below the legend (above HIDE)
        legendArea = b.removeFromBottom(22);            // symbol legend, above HIDE
        b.removeFromBottom(10);                         // ~half a line above the legend (below columns)
        b.removeFromTop(4);
        const int gap = 6;
        // Columns 2 and 3 (categories / voice names) get extra width beyond the base
        // third at the larger voice-name text size (~8px per character): column 2
        // ~2 chars, column 3 ~5 chars (widened to fit the longest voice names).
        // Column 1 keeps its base third-width; the pop-up's first-open width is
        // enlarged to match both extras — see EditorView.
        const int extra2 = 16;   // column 2: ~2 chars
        const int extra3 = 40;   // column 3: ~5 chars (2 base + 3 added)
        const int cw  = (b.getWidth() - 2 * gap - extra2 - extra3) / 3;
        auto c1 = b.removeFromLeft(cw);          b.removeFromLeft(gap);
        auto c2 = b.removeFromLeft(cw + extra2); b.removeFromLeft(gap);
        auto c3 = b;   // remainder = cw + extra3

        // The DRUM BUFFER box sits below column 1 (column 1's list shrinks to fit).
        // Taller rows -> 20%-taller buttons (which also enlarges their DRUMS1-4
        // label text ~20% via the button's height-scaled font).
        const int rowH = 24;
        const int boxH = rowH * drumbuf::kNumBuffers + 18 /*label*/ + 4;
        auto drumBox = c1.removeFromBottom(boxH);
        c1.removeFromBottom(4);
        col1.setBounds(c1);

        drumLabelArea = drumBox.removeFromBottom(16);
        drumBox.removeFromBottom(2);
        for (int i = 0; i < drumbuf::kNumBuffers; ++i)
        {
            auto row = drumBox.removeFromTop(rowH).reduced(0, 1);
            drumRow[i] = row;               // swatch + button: what the focus cue frames
            // A slightly larger SQUARE swatch (drawn as a colour outline in paint()).
            auto swCell = row.removeFromLeft(22);
            const int sw = 16;
            drumSwatch[i] = juce::Rectangle<int>(0, 0, sw, sw).withCentre(swCell.getCentre());
            row.removeFromLeft(3);
            drumBtn[i].setBounds(row);
        }

        col2.setBounds(c2);
        col3.setBounds(c3);
    }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour(edcol::kWindowBg);
        g.fillRoundedRectangle(r, 6.f);
        g.setColour(edcol::kTitleBarBg);                 // title-bar strip (drag handle)
        g.fillRect(titleBar.expanded(8, 4).withTrimmedTop(4));
        g.setColour(edcol::kWindowBorder);
        g.drawRoundedRectangle(r.reduced(0.5f), 6.f, 1.f);
        voiceind::drawLegend(g, legendArea);

        // DRUM BUFFER swatches: a coloured square OUTLINE (the same colour fills a
        // part's voice-name outline in the Voice list once a kit is allocated here)
        // -- drawn SOLID on the armed buffer. That is the one cue here that is
        // identical in both themes (these four colours are deliberately
        // theme-independent) and it says WHICH buffer, not merely that one is armed.
        for (int i = 0; i < drumbuf::kNumBuffers; ++i)
        {
            g.setColour(drumbuf::colour(i));
            if (isArmed(i)) g.fillRect(drumSwatch[i]);
            else            g.drawRect(drumSwatch[i], 2);
        }
        g.setColour(edcol::kTextDim);
        g.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
        g.drawText("DRUM BUFFER", drumLabelArea, juce::Justification::centred);
    }

    // The keyboard's region cue. Drawn over the children because the ListBoxes paint
    // their own background over the panel, and because a 2px frame just outside a
    // column would otherwise be half-covered by its neighbour's opaque bounds.
    //
    // A selected row is NOT a focus cue: every column paints kListSelBg on its
    // selection whether or not it holds the keyboard, so with four regions the
    // selection alone cannot say which one the arrows will move. The frame is the
    // same colour and the same 2 design px as the editor's nav ring, for the same
    // reason it is 2 and not 1 -- the canvas is scaled, and a 1px rule antialiases
    // to grey. In the buffer box it has to read against the ARMED pair as well as
    // against the panel, which kNavFocus does in both themes (near-black on the
    // light theme's pale kDrumStripActive, white on the dark theme's olive).
    void paintOverChildren(juce::Graphics& g) override
    {
        if (markVisible && ! markVisible()) return;
        const auto mark = focusRegionBounds();
        if (mark.isEmpty()) return;
        g.setColour(edcol::kNavFocus);
        g.drawRoundedRectangle(mark.expanded(2).toFloat(), 3.0f, 2.0f);
    }

    // Drag the title bar to reposition (kept within the parent editor). Track a
    // flag rather than re-testing the hit point each drag: the panel moves under
    // the cursor, so a position re-test would leave the title-bar rect and stall.
    void mouseDown(const juce::MouseEvent& e) override
    {
        draggingPanel = titleBar.contains(e.getPosition());
        if (draggingPanel) dragger.startDraggingComponent(this, e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! draggingPanel) return;
        dragger.dragComponent(this, e, nullptr);
        if (auto* p = getParentComponent())
            setBounds(getBounds().constrainedWithin(p->getLocalBounds()));
    }

    void mouseUp(const juce::MouseEvent&) override { draggingPanel = false; }

    // The browser's four areas -- the three columns and the DRUM BUFFER box -- are
    // keynav MODULES, walked with the same keys the editor's own modules use:
    //
    //   Tab / Shift+Tab   (nav.groupNext/Prev)  cycle the regions, wrapping
    //   left / right      (nav.prev/next)       step one region, clamping at the ends
    //   up / down                               move the row / the buffer (see below)
    //   Enter             on a buffer: arm it; anywhere else: close, as it always has
    //   Escape                                  close, from any region
    //
    // Up/down are NOT read from the binding table: inside a column they are the
    // ListBox's own keys and were never rebindable, so the buffer box hardcodes them
    // too rather than making the same movement rebindable in one region and not the
    // other. The REGION keys do come from the table, so a user who rebinds "Next
    // module" gets it here.
    //
    // Enter means two things by region, which is deliberate: nav.activate is "press
    // the thing the cursor is on", and in a list the row under the cursor was already
    // applied by the arrow that landed on it -- so "press" there has nothing left to
    // do but finish. Escape exists so there is one exit that does not depend on which
    // region you are in.
    //
    // Anything this panel recognises is CONSUMED, including an arrow at the end of the
    // ring. A modeless overlay that let the arrows through would scrub the DAW's
    // timeline from under a voice audition; while it is open, the browser owns them.
    bool keyPressed(const juce::KeyPress& k) override
    {
        if (onKeyboardUsed) onKeyboardUsed();   // a key that moves the frame reveals it
        if (k.isKeyCode(juce::KeyPress::escapeKey)) { close(); return true; }

        const int cmd = commandForKey ? commandForKey(k) : -1;
        switch (cmd)
        {
            case kbsc::NavGroupNext: return stepRegion(+1, /*wrap*/ true);
            case kbsc::NavGroupPrev: return stepRegion(-1, /*wrap*/ true);
            case kbsc::NavNext:      return stepRegion(+1, /*wrap*/ false);
            case kbsc::NavPrev:      return stepRegion(-1, /*wrap*/ false);
            default: break;
        }

        if (focusRegion == RegDrums)
        {
            if (k.isKeyCode(juce::KeyPress::upKey))   return stepDrumFocus(-1);
            if (k.isKeyCode(juce::KeyPress::downKey)) return stepDrumFocus(+1);
            if (k.isKeyCode(juce::KeyPress::homeKey)) return stepDrumFocus(-drumbuf::kNumBuffers);
            if (k.isKeyCode(juce::KeyPress::endKey))  return stepDrumFocus(+drumbuf::kNumBuffers);
            if (k.isKeyCode(juce::KeyPress::returnKey)) { armFocusedBuffer(); return true; }
        }

        if (k.isKeyCode(juce::KeyPress::returnKey)) { close(); return true; }
        return false;
    }

private:
    void close()
    {
        disarmBuffer();          // hiding the browser disarms any armed buffer
        setVisible(false);
        if (onHide) onHide();
    }

    //== keyboard regions ======================================================
    // The four areas the keyboard can be in. The ORDER is the ring that Tab and the
    // left/right arrows both walk -- authored, like keynav's traversal order, rather
    // than derived from screen positions: the buffer box sits below column 1, but it
    // belongs at the END of the walk, because it is where a kit hunt finishes rather
    // than something to cross on the way from column 1 to column 2.
    enum Region { RegCol1 = 0, RegCol2, RegCol3, RegDrums, kNumRegions };

    // A region with nothing to do is SKIPPED -- the rule keynav::usable applies to a
    // stop. Column 3 is empty outside Melody, and the buffer buttons are disabled
    // outside the kit groups (updateDrumButtons), so entering either would be a dead
    // end with no cue saying why the keys had stopped doing anything.
    bool regionUsable(int r) const
    {
        switch (r)
        {
            case RegCol1:  return true;
            case RegCol2:  return ! col2rows.empty();
            case RegCol3:  return group == Melody && ! col3rows.empty();
            case RegDrums: return isKitGroup(group);
            default:       return false;
        }
    }

    NavListBox* listForRegion(int r)
    {
        return r == RegCol1 ? &col1 : r == RegCol2 ? &col2 : r == RegCol3 ? &col3 : nullptr;
    }
    const NavListBox* listForRegion(int r) const
    {
        return const_cast<VoiceBrowser*>(this)->listForRegion(r);
    }

    juce::Rectangle<int> focusRegionBounds() const
    {
        if (focusRegion == RegDrums)
            return (drumFocus >= 0 && drumFocus < drumbuf::kNumBuffers)
                       ? drumRow[drumFocus] : juce::Rectangle<int>();
        if (auto* l = listForRegion(focusRegion)) return l->getBounds();
        return {};
    }

    void setFocusRegion(int r)
    {
        focusRegion = r;
        // The buffer box has no focusable component of its own (the buttons refuse
        // focus, deliberately), so the PANEL holds the keys while the cursor is
        // there -- which is also what routes up/down to stepDrumFocus rather than
        // into whichever column was focused last.
        if (auto* l = listForRegion(r)) l->grabKeyboardFocus();
        else                            grabKeyboardFocus();
        repaint();
    }

    // A mouse click has already moved JUCE's focus; this only catches the cue up.
    void noteFocusRegion(int r) { focusRegion = r; repaint(); }

    bool stepRegion(int dir, bool wrap)
    {
        int r = focusRegion;
        for (int n = 0; n < kNumRegions; ++n)
        {
            r += dir;
            if (r < 0 || r >= kNumRegions)
            {
                if (! wrap) return true;        // clamped: consumed, but nothing moves
                r = (r < 0) ? kNumRegions - 1 : 0;
            }
            if (regionUsable(r)) { setFocusRegion(r); return true; }
        }
        return true;
    }

    bool stepDrumFocus(int dir)
    {
        drumFocus = juce::jlimit(0, drumbuf::kNumBuffers - 1, drumFocus + dir);
        repaint();
        return true;
    }

    // Enter on a buffer arms it -- and then carries the keyboard to the kit list,
    // because arming exists only to receive the next kit pick, and the alternative
    // is a Tab that every user would have to press every single time. DISARMING
    // stays put: it ends the flow rather than starting one.
    void armFocusedBuffer()
    {
        const bool wasArmed = (selectedBuffer == drumFocus);
        selectDrumBuffer(drumFocus);                    // toggles, and may switch group
        if (! wasArmed && selectedBuffer == drumFocus && regionUsable(RegCol2))
            setFocusRegion(RegCol2);
    }

    // A group change can pull the focused region out from under the keyboard (the
    // buffer box on the way out of a kit group; column 3 on the way out of Melody).
    // Land on the nearest usable region rather than leave the keys pointing at
    // nothing. Only once VISIBLE -- the constructor selects a group before there is
    // any keyboard to hand anywhere.
    void ensureFocusRegionUsable()
    {
        if (! isVisible() || regionUsable(focusRegion)) return;
        for (int r = focusRegion; r >= 0; --r)
            if (regionUsable(r)) { setFocusRegion(r); return; }
        setFocusRegion(RegCol1);
    }

    // Focus the column the current voice lives in, so the arrow keys walk the VOICE
    // list from the moment the panel opens rather than only after a click. RETURN
    // reaches close() from any of them (LambdaListModel forwards it), and from the
    // panel itself if no column has rows to give focus to.
    void focusBrowsingColumn()
    {
        setFocusRegion((group == Melody && ! col3rows.empty()) ? RegCol3
                     : (! col2rows.empty())                    ? RegCol2
                                                               : RegCol1);
    }

    // Column 1: pick a group. Shared by the click and arrow-key paths, which must
    // behave identically -- including the disarm, since arrowing out of the kit
    // groups leaves them just as thoroughly as clicking out of them.
    void chooseGroup(int r)
    {
        if (r < 0 || r >= (int) kNumGroups) return;
        const Group dst = (Group) r;
        // An armed buffer survives navigation BETWEEN the two kit groups
        // (SFX Kit <-> Drum Kit), so a buffer already holding a kit can be
        // re-targeted to the other kit type. Leaving the kit groups entirely
        // (or HIDE) disarms.
        if (! isKitGroup(dst)) disarmBuffer();
        selectGroup(dst);
        col2.scrollToEnsureRowIsOnscreen(0);   // fresh group -> top of column 2
    }

    void selectGroup(Group g)
    {
        group = g;
        col2rows.clear();
        col3rows.clear();
        switch (g)
        {
            case Melody:
                for (int c = 0; c < mu100::kVoiceCategoryCount; ++c) col2rows.push_back(c);
                break;
            case SFX:
                for (int i = 0; i < mu100::kVoiceCount; ++i)
                    if (mu100::kVoices[i].msb == 64) col2rows.push_back(i);
                break;
            case MU100Exclusive:
                for (int i = 0; i < mu100::kVoiceCount; ++i)
                    if (mu100::kVoices[i].msb == 48) col2rows.push_back(i);
                break;
            case SFXKit:
                for (int i = 0; i < mu100::kVoiceCount; ++i)
                    if (mu100::kVoices[i].msb == 126) col2rows.push_back(i);
                break;
            case DrumKit:
                for (int i = 0; i < mu100::kVoiceCount; ++i)
                    if (mu100::kVoices[i].msb == 127) col2rows.push_back(i);
                break;
            default: break;
        }
        col2.deselectAllRows();
        col2.updateContent();
        col3.updateContent();
        updateDrumButtons();   // buffer buttons enable only in the kit groups
        ensureFocusRegionUsable();
        repaint();
    }

    void selectMelodyCategory(int cat)
    {
        col3rows.clear();
        for (int i = 0; i < mu100::kVoiceCount; ++i)
        {
            const auto& v = mu100::kVoices[i];
            if (v.msb == 0 && v.category == cat) col3rows.push_back(i);
        }
        col3.updateContent();
    }

    // Column 2 is categories (Melody) or voices/kits (every other group).
    juce::String col2Text(int r)
    {
        if (r < 0 || r >= (int) col2rows.size()) return {};
        const int id = col2rows[(size_t) r];
        return group == Melody ? juce::String(mu100::kVoiceCategories[id])
                               : juce::String(voicenames::displayName(mu100::kVoices[id]));
    }

    bool col2Voice(int r, int& model, int& elements)
    {
        if (group == Melody || r < 0 || r >= (int) col2rows.size()) return false;
        const auto& v = mu100::kVoices[col2rows[(size_t) r]];
        model = v.model; elements = v.elements;
        return true;
    }

    void pickCol2(int r, bool fromKeyboard)
    {
        if (r < 0 || r >= (int) col2rows.size()) return;
        const int id = col2rows[(size_t) r];
        if (group == Melody)
        {
            selectMelodyCategory(id);
            col3.deselectAllRows();
            col3.scrollToEnsureRowIsOnscreen(0);       // jump to top of the new category
        }
        else
            assignVoice(id, fromKeyboard);              // SFX / Exclusive / kits
    }

    void pickCol3(int r, bool fromKeyboard)
    {
        if (r < 0 || r >= (int) col3rows.size()) return;
        assignVoice(col3rows[(size_t) r], fromKeyboard);
    }

    void assignVoice(int voiceIx, bool fromKeyboard)
    {
        if (voiceIx < 0 || voiceIx >= mu100::kVoiceCount) return;
        const auto& v = mu100::kVoices[voiceIx];

        // A kit picked while a DRUMS buffer is armed is allocated to that buffer
        // (only drum/SFX kits are editable; melodic voices ignore the buffer).
        int buffer = -1;
        const bool isKit = (v.msb == 126 || v.msb == 127);
        if (isKit && selectedBuffer >= 0)
            buffer = selectedBuffer;   // onPick puts it on the buffer; kitForBuffer reads it back
        if (onPick) onPick(v.msb, v.lsb, v.pgm - 1, voicenames::displayName(v), buffer,
                           fromKeyboard);
    }

    // Arm a drum buffer for kit picks (click again to disarm). It stays armed
    // while the user auditions kits across BOTH kit groups (SFX Kit / Drum Kit);
    // navigating out to a non-kit group, hiding the browser, selecting another
    // part, or arming a different buffer disarms it (so a kit isn't assigned by
    // accident on return).
    void selectDrumBuffer(int i)
    {
        selectedBuffer = (selectedBuffer == i) ? -1 : i;
        if (selectedBuffer >= 0)
        {
            // Bring the armed buffer's own kit group into view so its current kit
            // can be highlighted. This is programmatic, so it does NOT count as a
            // user "navigate to a new category" (which would disarm).
            // An EMPTY buffer names no kit group, so there is nothing to bring into
            // view: stay where the user is browsing (arming is only reachable from a
            // kit group). Navigating anyway sent SFX Kit -> Drum Kit, away from the
            // very list the user was about to pick from.
            int bmsb = -1, blsb = 0, bprg = 0;
            const bool have = kitForBuffer && kitForBuffer(selectedBuffer, bmsb, blsb, bprg);
            const Group g = (have && bmsb == 126) ? SFXKit
                          : (have && bmsb == 127) ? DrumKit
                          : isKitGroup(group) ? group : DrumKit;
            if (g != group) { selectGroup(g); col1.selectRow(g); }
            highlightBufferKit();
        }
        updateDrumButtons();
    }

    // Disarm the currently armed buffer (no-op if none).
    void disarmBuffer()
    {
        if (selectedBuffer < 0) return;
        selectedBuffer = -1;
        updateDrumButtons();
    }

    // Show what the ARMED buffer holds. Programmatic selection, so the models act on
    // nothing (only listBoxItemClicked and onKeyboardNav assign) -- see LambdaListModel.
    void highlightBufferKit()
    {
        if (selectedBuffer < 0) return;

        int msb = -1, lsb = 0, prg0 = 0;
        if (! kitForBuffer || ! kitForBuffer(selectedBuffer, msb, lsb, prg0))
        {
            // An EMPTY buffer must show BLANK, not whatever was highlighted before it was
            // armed: leaving that row selected reads as "this buffer holds that kit", which
            // is the same false claim the old hardcoded DRUMS1/DRUMS3 seed made. Blank means
            // nothing is loaded here, and the next pick is what fills it.
            col2.deselectAllRows();
            return;
        }

        const int vi = findVoice(msb, lsb, prg0 + 1);
        for (int r = 0; r < (int) col2rows.size(); ++r)
            if (col2rows[(size_t) r] == vi)
            { col2.selectRow(r); col2.scrollToEnsureRowIsOnscreen(r); break; }
    }

    // Armed = this buffer takes the next kit pick. Arming is confined to the kit
    // groups, so paint() and updateDrumButtons() ask the question here rather than
    // each rebuilding it -- a swatch that disagreed with its button would be worse
    // than either cue alone.
    bool isArmed(int i) const { return isKitGroup(group) && i == selectedBuffer; }

    void updateDrumButtons()
    {
        // Buffer assignment is only meaningful while browsing kits, so the
        // buttons are clickable only in the SFX Kit / Drum Kit groups.
        const bool kitGroup = isKitGroup(group);
        for (int i = 0; i < drumbuf::kNumBuffers; ++i)
        {
            drumBtn[i].setEnabled(kitGroup);
            const bool sel = isArmed(i);
            // The FILL is the cue, not the glyph colour. Text colour alone was the
            // whole signal here and it is the weakest channel the eye has at this
            // size -- badly so in the light theme, where kDrumKeyArmed is tuned as
            // ink sitting ON the armed yellow (see EditorColours.h): the browser had
            // borrowed the ink and left the fill behind, leaving dark amber on plain
            // button grey. Taking the keyboard strip's own armed PAIR restores that,
            // and makes both places say "armed for drum edit" the same way.
            //
            // Unarmed REMOVES the override rather than setting kButtonBg: these
            // buttons and HIDE both take the LookAndFeel's default, and pinning one
            // of them to a token would drift the two apart.
            if (sel) drumBtn[i].setColour   (juce::TextButton::buttonColourId, edcol::kDrumStripActive);
            else     drumBtn[i].removeColour(juce::TextButton::buttonColourId);
            drumBtn[i].setColour(juce::TextButton::textColourOffId,
                                 sel ? edcol::kDrumKeyArmed : edcol::kButtonText);
        }
        repaint();
    }

    // Options -> Voice names changed. The columns read the name at paint time, so
    // this only has to make the ListBoxes ask again -- there is no cached string
    // here, unlike PartListPanel.
public:
    void refreshVoiceNames()
    {
        col2.updateContent();
        col3.updateContent();
        repaint();
    }

private:
    // The two groups whose kits can be allocated to a DRUMS buffer. Arming and
    // buffer-button enablement are confined to these, and an armed buffer stays
    // armed while navigating between them.
    static bool isKitGroup(Group g) { return g == DrumKit || g == SFXKit; }

    static int findVoice(int msb, int lsb, int pgm)
    {
        for (int i = 0; i < mu100::kVoiceCount; ++i)
        {
            const auto& v = mu100::kVoices[i];
            if (v.msb == msb && v.lsb == lsb && v.pgm == pgm) return i;
        }
        return -1;
    }

    static constexpr const char* kGroupNames[kNumGroups] =
        { "Melody", "SFX", "MU100 Exclusive", "SFX Kit", "Drum Kit" };

    juce::Label    titleLabel;
    juce::TextButton hideButton;
    NavListBox     col1, col2, col3;
    LambdaListModel col1Model, col2Model, col3Model;
    Group          group { Melody };
    std::vector<int> col2rows;   // category / voice / kit ids (per group)
    std::vector<int> col3rows;   // voice ids (Melody only)
    juce::ComponentDragger dragger;
    juce::Rectangle<int>   titleBar;
    juce::Rectangle<int>   legendArea;
    bool                   draggingPanel { false };

    // Drum edit buffers. selectedBuffer is the armed DRUMS button (-1 = none). What
    // each buffer HOLDS is not stored here -- it is asked of kitForBuffer, above.
    juce::TextButton       drumBtn[drumbuf::kNumBuffers];
    juce::Rectangle<int>   drumSwatch[drumbuf::kNumBuffers];
    juce::Rectangle<int>   drumRow[drumbuf::kNumBuffers];
    juce::Rectangle<int>   drumLabelArea;
    int                    selectedBuffer { -1 };

    // Which of the four areas the keyboard is in, and which buffer inside the box.
    // Stored rather than asked of JUCE: the buffer box's focus lives on the PANEL,
    // which cannot be told apart from "the panel itself has focus" any other way.
    int                    focusRegion { RegCol2 };
    int                    drumFocus { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceBrowser)
};
