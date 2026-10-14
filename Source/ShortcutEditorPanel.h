// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <memory>

#include "KeyboardShortcuts.h"
#include "EditorColours.h"

//==============================================================================
// ShortcutEditorComponent — the contents of the "Keyboard Shortcut Editor"
// window, laid out after Surge XT's: one scrolling row per command, each row
//
//     [x]  Action name                 Ctrl + Z        [ Learn ]  [ Reset ]
//
// with OK / Cancel / Reset All along the bottom.
//
// Differences from Surge's, both deliberate:
//  * The list carries CATEGORY HEADINGS. Surge's is one flat run, which is
//    readable at its length; ours groups 30-odd commands that fall into obvious
//    families (File, Parts, Mixer...), and finding "Solo part" in a flat list
//    means reading all of it.
//  * Committing a key that already belongs to another command asks first, and
//    says which command it would be taken from. A silent steal is a shortcut
//    that stops working with nothing having said so.
//
// Edits are made on a COPY and only handed back on OK, which is what makes
// Cancel mean anything. The copy is also why Learn can freely stomp bindings
// while the user experiments.
//==============================================================================
class ShortcutEditorComponent : public juce::Component
{
public:
    // `onApply` receives the edited map when OK is pressed. `initial` is the map
    // currently in force.
    ShortcutEditorComponent(const kbsc::Bindings& initial,
                            std::function<void(const kbsc::Bindings&)> applyFn)
        : bindings(initial), onApply(std::move(applyFn))
    {
        addAndMakeVisible(viewport);
        viewport.setViewedComponent(&list, false);
        viewport.setScrollBarsShown(true, false);

        buildRows();

        auto styleButton = [this](juce::TextButton& b)
        {
            b.setColour(juce::TextButton::buttonColourId, edcol::kButtonBg);
            b.setColour(juce::TextButton::textColourOffId, edcol::kButtonText);
            addAndMakeVisible(b);
        };
        styleButton(okButton);
        styleButton(cancelButton);
        styleButton(resetAllButton);

        okButton.onClick = [this]
        {
            if (onApply) onApply(bindings);
            closeWindow();
        };
        cancelButton.onClick  = [this] { closeWindow(); };
        resetAllButton.onClick = [this]
        {
            bindings.resetAll();
            refreshAllRows();
        };

        setWantsKeyboardFocus(true);
        setSize(660, 520);
    }

    //== layout ================================================================
    void paint(juce::Graphics& g) override
    {
        g.fillAll(edcol::kDialogBg);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(kPad);

        auto bottom = b.removeFromBottom(kButtonH);
        okButton      .setBounds(bottom.removeFromLeft(90));
        bottom.removeFromLeft(8);
        cancelButton  .setBounds(bottom.removeFromLeft(90));
        resetAllButton.setBounds(bottom.removeFromRight(100));

        b.removeFromBottom(kPad);
        viewport.setBounds(b);

        const int w = viewport.getWidth() - viewport.getScrollBarThickness();
        list.setSize(juce::jmax(360, w), listHeight());
        layoutRows();
    }

    //== key capture ===========================================================
    // Only ever reached while a row is learning: the window has no other keyboard
    // duty, and outside learn mode we hand every key back so the dialog's own
    // Return/Escape handling still works.
    bool keyPressed(const juce::KeyPress& k) override
    {
        if (learningRow < 0)
            return false;

        if (k.isKeyCode(juce::KeyPress::escapeKey))
        {
            endLearn();
            return true;
        }

        commitLearn(k);
        return true;
    }

    // Losing focus mid-learn (a click elsewhere, the window deactivating) must not
    // leave a row sitting on "Press a key..." forever.
    void focusLost(FocusChangeType) override
    {
        if (learningRow >= 0)
            endLearn();
    }

private:
    //==========================================================================
    // One list entry: either a category heading or a command row. Headings are
    // entries rather than a separate list so the scroll geometry is one array.
    struct Entry
    {
        bool isHeading { false };
        juce::String heading;
        int  cmd { -1 };
    };

    static constexpr int kPad        = 12;
    static constexpr int kRowH       = 30;
    static constexpr int kHeadingH   = 26;
    static constexpr int kButtonH    = 26;
    static constexpr int kSmallBtnW  = 62;
    static constexpr int kCheckW     = 34;
    static constexpr int kKeyColW    = 170;

    void buildRows()
    {
        juce::String lastCategory;
        for (int c = 0; c < kbsc::NumCommands; ++c)
        {
            const juce::String cat(kbsc::kCommands[c].category);
            if (cat != lastCategory)
            {
                Entry h; h.isHeading = true; h.heading = cat;
                entries.push_back(h);
                lastCategory = cat;
            }
            Entry e; e.cmd = c;
            entries.push_back(e);
        }

        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (entries[i].isHeading) { toggles.push_back(nullptr);
                                        learnButtons.push_back(nullptr);
                                        resetButtons.push_back(nullptr); continue; }

            const int cmd = entries[i].cmd;

            auto tog = std::make_unique<juce::ToggleButton>();
            tog->setColour(juce::ToggleButton::tickColourId,       edcol::kTextLabel);
            tog->setColour(juce::ToggleButton::tickDisabledColourId, edcol::kTextFaint);
            tog->setToggleState(bindings.isEnabled(cmd), juce::dontSendNotification);
            tog->onClick = [this, cmd, p = tog.get()]
            {
                bindings.set(cmd, bindings.get(cmd), p->getToggleState());
                refreshAllRows();   // an on/off changes what the Reset column shows
            };
            list.addAndMakeVisible(*tog);

            auto learn = std::make_unique<juce::TextButton>("Learn");
            learn->setColour(juce::TextButton::buttonColourId, edcol::kButtonBgAlt);
            learn->setColour(juce::TextButton::textColourOffId, edcol::kButtonText);
            learn->onClick = [this, i] { beginLearn((int) i); };
            list.addAndMakeVisible(*learn);

            auto reset = std::make_unique<juce::TextButton>("Reset");
            reset->setColour(juce::TextButton::buttonColourId, edcol::kButtonBgAlt);
            reset->setColour(juce::TextButton::textColourOffId, edcol::kButtonText);
            reset->onClick = [this, cmd]
            {
                bindings.resetToDefault(cmd);
                refreshAllRows();
            };
            list.addAndMakeVisible(*reset);

            toggles.push_back(std::move(tog));
            learnButtons.push_back(std::move(learn));
            resetButtons.push_back(std::move(reset));
        }

        list.owner = this;
        refreshAllRows();
    }

    int listHeight() const
    {
        int h = 0;
        for (const auto& e : entries)
            h += e.isHeading ? kHeadingH : kRowH;
        return h;
    }

    void layoutRows()
    {
        int y = 0;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (entries[i].isHeading) { y += kHeadingH; continue; }

            juce::Rectangle<int> r(0, y, list.getWidth(), kRowH);
            r.reduce(6, 3);

            toggles[i]->setBounds(r.removeFromLeft(kCheckW));
            resetButtons[i]->setBounds(r.removeFromRight(kSmallBtnW));
            r.removeFromRight(6);
            learnButtons[i]->setBounds(r.removeFromRight(kSmallBtnW));

            y += kRowH;
        }
    }

    void refreshAllRows()
    {
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (entries[i].isHeading) continue;
            const int cmd = entries[i].cmd;
            toggles[i]->setToggleState(bindings.isEnabled(cmd), juce::dontSendNotification);
            // Surge greys Reset on a row that is already at its default, which is
            // the only cue that a row HAS been changed — keep it.
            resetButtons[i]->setEnabled(! bindings.isDefault(cmd));
            learnButtons[i]->setButtonText((int) i == learningRow ? "..." : "Learn");
        }
        list.repaint();
    }

    void beginLearn(int rowIndex)
    {
        learningRow = rowIndex;
        grabKeyboardFocus();
        refreshAllRows();
    }

    void endLearn()
    {
        learningRow = -1;
        refreshAllRows();
    }

    void commitLearn(const juce::KeyPress& k)
    {
        const int row = learningRow;
        if (row < 0 || row >= (int) entries.size()) { endLearn(); return; }
        const int cmd = entries[(size_t) row].cmd;

        const int clash = bindings.conflictWith(k, cmd);
        if (clash >= 0)
        {
            const juce::String keyText = kbsc::Bindings::describe(k);
            juce::AlertWindow::showOkCancelBox(
                juce::MessageBoxIconType::QuestionIcon,
                "Shortcut already in use",
                keyText + " is assigned to \"" + kbsc::kCommands[clash].name
                        + "\".\n\nAssign it to \"" + kbsc::kCommands[cmd].name
                        + "\" instead? \"" + kbsc::kCommands[clash].name
                        + "\" will be left with no shortcut.",
                "Reassign", "Cancel", this,
                juce::ModalCallbackFunction::create([this, cmd, clash, k](int result)
                {
                    if (result == 1)
                    {
                        bindings.set(clash, juce::KeyPress(), bindings.isEnabled(clash));
                        applyLearned(cmd, k);
                    }
                    endLearn();
                }));
            return;
        }

        applyLearned(cmd, k);
        endLearn();
    }

    void applyLearned(int cmd, const juce::KeyPress& k)
    {
        // Learning a key also switches the row ON: the user just told us what it
        // should do, and leaving it disabled would look like the Learn failed.
        bindings.set(cmd, k, true);
        refreshAllRows();

        // A modifier-less binding is legal but is taken from the host the moment
        // this window has focus, which is exactly the surprise the whole feature
        // is trying to avoid — so say so once, here, rather than let the user
        // discover it when the DAW stops responding to that key.
        if (! k.getModifiers().isAnyModifierKeyDown() && ! isFunctionKey(k))
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                "Shortcut has no modifier",
                kbsc::Bindings::describe(k)
                    + " has no Ctrl, Alt or Shift.\n\nWhile the plugin window has "
                      "keyboard focus this key will be used by MEGAN XG and will not "
                      "reach your DAW.");
    }

    static bool isFunctionKey(const juce::KeyPress& k)
    {
        return k.getKeyCode() >= juce::KeyPress::F1Key
            && k.getKeyCode() <= juce::KeyPress::F16Key;
    }

    void closeWindow()
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->closeButtonPressed();
    }

    //==========================================================================
    // The scrolled contents. Painting lives here (rather than in per-row
    // components) because a row is two pieces of text and a background stripe.
    struct ListBody : public juce::Component
    {
        ShortcutEditorComponent* owner { nullptr };

        void paint(juce::Graphics& g) override
        {
            if (owner == nullptr) return;
            owner->paintList(g, *this);
        }
    };

    void paintList(juce::Graphics& g, juce::Component& body)
    {
        // Plain, fixed sizes: this is a top-level dialog, not part of the editor's
        // scaled design canvas, so it must not ride edtext's percentages.
        const juce::Font nameFont(juce::FontOptions(14.0f));
        const juce::Font headFont(juce::FontOptions(12.0f, juce::Font::bold));

        int y = 0;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];

            if (e.isHeading)
            {
                g.setColour(edcol::kTextDim);
                g.setFont(headFont);
                g.drawText(e.heading.toUpperCase(), 8, y, body.getWidth() - 16, kHeadingH,
                           juce::Justification::centredLeft);
                g.setColour(edcol::kTextFaint.withAlpha(0.5f));
                g.fillRect(8, y + kHeadingH - 1, body.getWidth() - 16, 1);
                y += kHeadingH;
                continue;
            }

            const int cmd = e.cmd;
            juce::Rectangle<int> row(0, y, body.getWidth(), kRowH);

            // Alternating stripe, and the learning row in the list-selection colour
            // so "which row is waiting for a key" is unmistakable.
            if ((int) i == learningRow)
                g.setColour(edcol::kListSelBg);
            else
                g.setColour((i % 2) ? edcol::kButtonBg.withAlpha(0.35f)
                                    : juce::Colours::transparentBlack);
            g.fillRect(row);

            const bool learning = ((int) i == learningRow);
            const bool live     = bindings.isEnabled(cmd);

            g.setFont(nameFont);
            g.setColour(learning ? edcol::kListSelText
                                 : (live ? edcol::kTextLabel : edcol::kTextFaint));
            const int nameX = kCheckW + 8;
            const int keyX  = row.getWidth() - 2 * kSmallBtnW - 18 - kKeyColW;
            g.drawText(kbsc::kCommands[cmd].name, nameX, y,
                       juce::jmax(40, keyX - nameX - 8), kRowH,
                       juce::Justification::centredLeft);

            juce::String keyText = kbsc::Bindings::describe(bindings.get(cmd));
            if (learning) keyText = "Press a key...";

            g.setColour(learning ? edcol::kListSelText
                                 : (live ? edcol::kTextLabel : edcol::kTextFaint));
            g.drawText(keyText, keyX, y, kKeyColW, kRowH, juce::Justification::centredLeft);

            y += kRowH;
        }
    }

    //==========================================================================
    kbsc::Bindings bindings;
    std::function<void(const kbsc::Bindings&)> onApply;

    std::vector<Entry> entries;
    // Parallel to `entries`; null at a heading. Parallel arrays rather than a row
    // struct because the heading entries have no widgets at all.
    std::vector<std::unique_ptr<juce::ToggleButton>> toggles;
    std::vector<std::unique_ptr<juce::TextButton>>   learnButtons;
    std::vector<std::unique_ptr<juce::TextButton>>   resetButtons;

    int learningRow { -1 };

    juce::Viewport   viewport;
    ListBody         list;
    juce::TextButton okButton       { "OK" };
    juce::TextButton cancelButton   { "Cancel" };
    juce::TextButton resetAllButton { "Reset All" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ShortcutEditorComponent)
};
