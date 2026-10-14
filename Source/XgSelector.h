// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "Accessibility.h"
#include "EditUndo.h"
#include <functional>
#include "EditorTextScale.h"
#include "EditorColours.h"

//==============================================================================
// XgSelector — an XGEdit-style selection box with NO dropdown arrow.
//
//   * shows the current item's text in a compact green LCD-style field
//   * mouse-wheel over the box steps through items (no popup needed)
//   * left-click opens a borderless popup listing every item; for long lists the
//     items are laid out in `numColumns` columns (1 column = a plain list,
//     3 columns = the wide Effect-Name / Controller pickers) so all options are
//     visible at once with no scrolling
//
// The popup is a juce::CallOutBox (it parents to the editor window and dismisses
// itself on an outside click), so it is DAW-safe and self-managing. Reused for
// the Effect Type box, the Effect Name box, and later the Multi-EQ Program /
// Controller boxes.
//==============================================================================
class XgSelector : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    // Accessibility pillar 2: an accessible ELEMENT is focusable (see ValueBox's
    // constructor for the rule). A screen reader reaches a control by moving
    // KEYBOARD focus to it, so a handler on a component that refuses focus can
    // never be read.
    XgSelector() { setWantsKeyboardFocus(true); }

    std::function<void(int)> onSelect;   // fired with the chosen item index

    void setItems(const juce::StringArray& newItems, int columns = 1)
    {
        items = newItems;
        numColumns = juce::jmax(1, columns);
        if (selected >= items.size()) selected = items.isEmpty() ? -1 : 0;
        repaint();
    }

    void setSelectedIndex(int index, bool notify)
    {
        index = juce::jlimit(items.isEmpty() ? -1 : 0, items.size() - 1, index);
        if (index == selected) { repaint(); return; }
        selected = index;
        repaint();
        if (notify && onSelect && selected >= 0) onSelect(selected);
    }
    int getSelectedIndex() const { return selected; }

    // Grayed-out (non-selectable) items. Disabled items are dimmed in the pop-up,
    // ignored by clicks, and skipped by wheel stepping. setSelectedIndex is NOT
    // blocked, so a disabled item the device is already on still displays (e.g. a
    // patch that already uses an audio-input-only Variation effect). Pass a fresh
    // set each rebuild; the meaning of an index changes when the item list does.
    void setDisabledItems(const juce::SortedSet<int>& d) { disabled = d; repaint(); }
    bool isDisabled(int i) const { return disabled.contains(i); }

    // Override the pop-up's per-column width (design px). Default 132; widen for
    // selectors whose items are long enough to clip at the larger text size.
    void setPopupColumnWidth(int w) { popupColW = juce::jmax(40, w); }

    // Changed-from-default marker. setDefaultIndex records the power-on value;
    // setShowChangedTab(true) enables the yellow gutter tab shown when the current
    // selection differs from it. Off by default (navigation selectors like the
    // Effect-type / Controller-source boxes never mark "changed").
    void setDefaultIndex(int i)     { defaultIndex = i; repaint(); }
    void setShowChangedTab(bool s)  { showChangedTab = s; repaint(); }

    // The MIDI-override mark (see MidiOverrideMarks.h for the shadow this reports).
    // A selector holds a NAME, not a position in a range, so it takes a FULL-HEIGHT
    // tab and nothing else -- there is no height that would mean anything, which is
    // the same judgement the Voice module's row tab already makes.
    //
    // `deviceText` is what the device is on, and it has nowhere to go on screen: the
    // box can show one name and that name is the user's. So it goes to the tooltip
    // and to the spoken value, which is the whole of the answer to "overridden to
    // WHAT". Pass it empty where the device's value cannot be named, and the tab
    // still appears -- an honest "something is different" beats a guess.
    //
    // The three mark STYLES do not apply here: they choose the FORM of a mark on a
    // positional control, and this control has no position. Same rule the bar
    // screens, the EQ nodes and DotToggle already follow.
    void setMidiOverride(bool present, const juce::String& deviceText)
    {
        if (overridden == present && overrideText == deviceText) return;
        overridden   = present;
        overrideText = deviceText;
        repaint();   // the spoken value reads both live, so no handler rebuild
    }
    bool hasMidiOverride() const { return overridden; }

    juce::String currentText() const
    {
        return (selected >= 0 && selected < items.size()) ? items[selected] : juce::String();
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(edcol::kSelBg);
        g.fillRoundedRectangle(b, 2.f);
        g.setColour(edcol::kSelBorder);
        g.drawRoundedRectangle(b.reduced(0.5f), 2.f, 1.f);
        g.setColour(edcol::kSelText);
        g.setFont(edtext::font(edtext::Value, (float) juce::jmin(13, getHeight() - 4)));
        // Text is inset an extra 4px on the left so it clears the changed-marker
        // gutter tab. Kept even on untabbed selectors so all six align identically.
        auto textArea = b.reduced(4, 0);
        textArea.removeFromLeft(4.f);
        // ...and the same clearance on the RIGHT, for the MIDI-override tab below.
        // Reserved UNCONDITIONALLY, exactly like the left gutter: a name must not
        // reflow the moment a program change arrives, and every selector stays
        // aligned with every other whether or not it can be overridden.
        textArea.removeFromRight(edcol::kChangedTabW);
        g.drawText(currentText(), textArea, juce::Justification::centredLeft, true);

        // Changed-from-default marker: same yellow gutter tab as the value fields.
        if (showChangedTab && defaultIndex >= 0 && selected != defaultIndex)
            edcol::drawChangedTab(g, b);

        // ...and the override tab at the OTHER end, which is the same left-versus-
        // right pairing the value boxes use: yellow says this differs from the
        // factory default, cyan says the DEVICE differs from this.
        if (overridden)
            edcol::drawMidiOverrideBar(g, b, 1.0f, 0.0f, edcol::kChangedTabW, 1.5f);
    }

    void mouseDown(const juce::MouseEvent&) override { openPopup(); }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        step((w.deltaY > 0 || w.deltaX > 0) ? -1 : 1);   // wheel up = previous
    }

    //== keyboard control (see KeyNav.h) =======================================
    // One item along, skipping disabled ones. Shared by the wheel above and the
    // arrow keys, so a selector cannot step two different ways.
    void step(int dir)
    {
        if (items.isEmpty() || dir == 0) return;
        int next = selected + dir;
        while (next >= 0 && next < items.size() && isDisabled(next)) next += dir;
        if (next >= 0 && next < items.size()) setSelectedIndex(next, true);
    }
    // What a click does: open the pop-up list.
    void openPopupFromKeyboard() { openPopup(); }

    // See ValueBox::setTooltip -- an accessible name must not vanish mid-edit.
    void setTooltip(const juce::String& t) override { rawTip = t; juce::SettableTooltipClient::setTooltip(t); }

    // Hovering an overridden selector NAMES what the device is on. That is the only
    // place the answer can appear -- and in the default tooltip mode it lands in the
    // bottom bar, at a fixed spot, obscuring nothing. Led with, ahead of the standing
    // tooltip, because it is the transient half; the wording follows the Voice
    // module's row tooltip so one habit reads both.
    juce::String getTooltip() override
    {
        if (overridden && overrideText.isNotEmpty())
            return "Incoming MIDI has the device on " + overrideText
                 + ". The name shown is your setup, and is what will be saved."
                 + (rawTip.isEmpty() ? juce::String() : "\n\n" + rawTip);
        return rawTip;
    }

    // Accessibility (pillar 1): a combo box whose value is the item TEXT.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return mu100a11y::makeValueHandler(
            *this, rawTip, juce::AccessibilityRole::comboBox,
            // The override rides the VALUE, as it does on every other control: a tab
            // says nothing without sight, and read live it is announced arriving
            // under the user rather than only when the cursor next lands here.
            [this]
            {
                auto v = currentText();
                if (overridden && overrideText.isNotEmpty())
                    v += ", MIDI has the device on " + overrideText;
                return v;
            },
            [this](const juce::String& t)
            {
                const int i = items.indexOf(t);
                if (i >= 0 && ! isDisabled(i)) setSelectedIndex(i, true);
            },
            [this] { return ! isEnabled(); });
    }

    // Set by EditorView whenever the keyboard cursor lands on this selector: the
    // pop-up calls it for left/right, so those keys close the list and carry on to the
    // next control instead of dead-ending in a modal window. Left unset elsewhere, in
    // which case left/right simply close.
    std::function<void(int)> onNavigateAway;
    // -1 when this selector records no default (the navigation selectors do not).
    int getDefaultIndex() const { return defaultIndex; }

private:
    //==========================================================================
    // The pop-up list content (placed inside a CallOutBox).
    class Popup : public juce::Component
    {
    public:
        Popup(const juce::StringArray& its, int cols, int current,
              std::function<void(int)> pick, int columnWidth,
              const juce::SortedSet<int>& disabledItems)
            : colW(columnWidth), items(its), disabled(disabledItems),
              numColumns(juce::jmax(1, cols)),
              highlight(current), onPick(std::move(pick))
        {
            rows = juce::jmax(1, (items.size() + numColumns - 1) / numColumns);  // column-major
            setSize(numColumns * colW + 4, rows * rowH + 4);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(edcol::kPopupBg);
            g.setColour(edcol::kPopupBorder);
            g.drawRect(getLocalBounds(), 1);
            g.setFont(edtext::font(edtext::Value, 12.f));
            for (int i = 0; i < items.size(); ++i)
            {
                const auto r = itemRect(i);
                const bool dis = disabled.contains(i);
                if (i == highlight && ! dis) { g.setColour(edcol::kPopupHiBg); g.fillRect(r); }
                g.setColour(dis ? edcol::kPopupTextOff                        // grayed / non-selectable
                                : (i == highlight ? edcol::kPopupHiText
                                                  : edcol::kPopupText));
                g.drawText(items[i], r.reduced(5, 0), juce::Justification::centredLeft, true);
            }
        }

        void mouseMove(const juce::MouseEvent& e) override
        {
            int i = indexAt(e.getPosition());
            if (i >= 0 && disabled.contains(i)) i = -1;   // don't highlight non-selectable rows
            if (i != highlight) { highlight = i; repaint(); }
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            const int i = indexAt(e.getPosition());
            if (i >= 0 && ! disabled.contains(i) && onPick) { mu100undo::requestSeal(); onPick(i); }
            dismiss();
        }

        //======================================================================
        // The pop-up is MODAL (juce::CallOutBox enters a modal state), so while it is
        // up the plugin editor's keyPressed never runs and every key would otherwise
        // dead-end here -- CallOutBox itself handles only Escape. So the list answers
        // the navigation keys itself, and it answers them the way the rest of the
        // editor does.
        //
        // Up/Down STEP THE SELECTION LIVE rather than moving a highlight to be
        // confirmed later. That is the editor's existing law in both places it already
        // appears: the wheel over this same box steps programs live (auditioning them
        // is the point), and the Voice List's arrows assign as they go. So Enter
        // ACCEPTS NOTHING -- the value is already applied -- and is purely a way to
        // close, exactly like the Voice List's. The consequence worth knowing is that
        // Escape does not revert either; Undo does, and a whole run of arrow presses
        // is one undo step because none of them seals (see onPick's comment).
        bool keyPressed(const juce::KeyPress& k) override
        {
            if (k.isKeyCode(juce::KeyPress::upKey))    { stepLive(-1); return true; }
            if (k.isKeyCode(juce::KeyPress::downKey))  { stepLive(+1); return true; }

            if (k.isKeyCode(juce::KeyPress::returnKey) || k.isKeyCode(juce::KeyPress::escapeKey))
            {
                mu100undo::requestSeal();   // a deliberate end, like picking with the mouse
                dismiss();
                return true;
            }

            // Left/Right leave the control entirely: close, then move the keyboard
            // cursor on. Without this the list is a trap you can only click out of.
            if (k.isKeyCode(juce::KeyPress::leftKey) || k.isKeyCode(juce::KeyPress::rightKey))
            {
                const int dir = k.isKeyCode(juce::KeyPress::rightKey) ? +1 : -1;
                mu100undo::requestSeal();
                auto away = onNavigateAway;   // dismiss() can destroy this component
                dismiss();
                if (away) away(dir);
                return true;
            }
            return false;
        }

        std::function<void(int)> onNavigateAway;
        void setHighlight(int i) { highlight = i; repaint(); }

    private:
        // Move the selection one item, skipping disabled rows, and apply it. The
        // owner's onPick does the work, minus the undo seal -- so a run of presses
        // coalesces into one step the way a wheel flick does.
        void stepLive(int dir)
        {
            int next = highlight + dir;
            while (next >= 0 && next < items.size() && disabled.contains(next)) next += dir;
            if (next < 0 || next >= items.size()) return;
            highlight = next;
            repaint();
            if (onPick) onPick(next);
        }

        void dismiss()
        {
            if (auto* cob = findParentComponentOfClass<juce::CallOutBox>()) cob->dismiss();
        }

        juce::Rectangle<int> itemRect(int i) const
        {
            const int col = i / rows, row = i % rows;
            return { 2 + col * colW, 2 + row * rowH, colW, rowH };
        }
        int indexAt(juce::Point<int> p) const
        {
            const int col = (p.x - 2) / colW, row = (p.y - 2) / rowH;
            if (col < 0 || col >= numColumns || row < 0 || row >= rows) return -1;
            const int i = col * rows + row;
            return (i >= 0 && i < items.size()) ? i : -1;
        }

        static constexpr int rowH = 17;
        const int colW;
        juce::StringArray items;
        juce::SortedSet<int> disabled;
        int numColumns, rows = 1, highlight = -1;
        std::function<void(int)> onPick;
    };

    void openPopup()
    {
        if (items.isEmpty()) return;
        juce::Component::SafePointer<XgSelector> self(this);
        auto content = std::make_unique<Popup>(items, numColumns, selected,
            [self](int idx)
            {
                if (self == nullptr) return;
                self->setSelectedIndex(idx, true);
            },
            popupColW, disabled);

        // Picking with the MOUSE is one deliberate act, so it ends the open undo step
        // -- two clicks in quick succession stay two steps. The WHEEL and the pop-up's
        // ARROW KEYS deliberately do not seal: each steps through many items with no
        // pause, and each of those is a full edit (an effect program pick writes 17
        // messages), so they coalesce into one step instead. That is why the seal lives
        // on the mouse path here rather than inside the shared pick lambda.
        auto* raw = content.get();
        static_cast<Popup*>(raw)->onNavigateAway = onNavigateAway;

        auto* parent = getTopLevelComponent();
        const auto areaInParent = parent != nullptr
            ? parent->getLocalArea(this, getLocalBounds())
            : getScreenBounds();
        juce::CallOutBox::launchAsynchronously(std::move(content), areaInParent, parent);

        // The list has to hold the keyboard itself to answer the arrows: it is inside a
        // modal CallOutBox, which routes keys to its own tree and handles only Escape.
        static_cast<Popup*>(raw)->setWantsKeyboardFocus(true);
        static_cast<Popup*>(raw)->grabKeyboardFocus();
    }

    juce::StringArray items;
    juce::SortedSet<int> disabled;   // grayed / non-selectable item indices
    int numColumns { 1 };
    int selected   { -1 };
    int popupColW  { 132 };   // per-column pop-up width (design px); widen for long items
    int  defaultIndex   { -1 };      // power-on selection (-1 = no changed-marker)
    juce::String rawTip;             // see setTooltip
    bool showChangedTab { false };   // draw the yellow tab when selected != default
    bool overridden { false };       // see setMidiOverride
    juce::String overrideText;       // what the DEVICE is on; "" = it cannot be named
};
