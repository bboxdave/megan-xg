// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <functional>
#include <vector>

#include "XgKnob.h"
#include "ValueBox.h"
#include "XgSelector.h"
#include "DotToggle.h"
#include "ControllerWheel.h"

//==============================================================================
// keynav — keyboard navigation of the editor's controls.
//
// A "stop" is one control the keyboard cursor can sit on. Tab moves between
// MODULES, the left/right arrows between stops inside one, up/down change the
// value, Enter acts on it and Backspace resets it. CONTRIBUTING.md, "Working on
// the editor", has the rule for adding a control.
//
// THE CURSOR IS OURS, NOT JUCE'S — no setWantsKeyboardFocus anywhere in here.
// The plugin editor keeps the keyboard and stays the single place keys are read,
// which is what keeps this from disturbing the Voice List, an open type-in editor,
// or the host-focus machinery that took three Cubase rounds to settle (a focused
// juce::Button swallowing Enter is the failure that work is a monument to). It
// also means the traversal ORDER is AUTHORED — reading order within a module —
// rather than whatever JUCE's focus traverser derives from screen positions.
//
// Every key routes through the SAME method the equivalent mouse gesture uses
// (ValueBox::nudge is literally the wheel's body, XgKnob::beginTypeIn is what a
// click does), so a shortcut cannot grow a second behaviour — and the undo merge,
// the send throttle and the automation mirror all come along for free.
//==============================================================================
namespace keynav
{

//==============================================================================
// One stop.
//
// For an ordinary control `comp` IS the control and the three actions are left
// null — dispatch() below works them out from its type. For a PAINTED cell (the
// Voice module draws its Mute/Solo cells and voice names; the Keyboard module
// draws its DRUM KEY box) `comp` is the PANEL, `cell` says where the ring goes in
// that panel's coordinates, and the lambdas say what the keys do.
struct Item
{
    // How the cursor is drawn. NOT stored per item -- derived from the mark's own size
    // by cueFor() below, so there is one rule and nothing to keep in step.
    //   Brackets - the two ENDS of an outline plus a stub of the top and bottom edges
    //              at each corner, leaving the middles of the long edges undrawn. The
    //              DEFAULT, because almost every stop in this editor is a cell with
    //              TEXT in it (a value, a name, or both) and a closed outline runs a
    //              line parallel to the glyphs a couple of pixels away. In the Voice
    //              module that line is the SAME white the name is drawn in, which is
    //              what made it unreadable -- measured, not guessed.
    //   Ring     - a closed outline, for a mark too small to HAVE a middle: the LED
    //              toggles (PORTA, SYNC, the controller resend lamp) and the Voice
    //              module's Mute/Solo cells. Brackets there would read as four
    //              disconnected ticks rather than as a frame.
    enum class Cue { Ring, Brackets };

    juce::Component*     comp { nullptr };
    // Where the mark goes, in comp's coords. Empty = comp's whole bounds. Set for a
    // painted cell, and for a control whose visible extent is smaller than its bounds
    // (a dial-less XgKnob, which still reserves the dial's space).
    juce::Rectangle<int> cell;
    // True only for a cell with no component of its own, whose keys the lambdas below
    // answer. Explicit rather than inferred from `cell`, so a REAL control can carry a
    // custom mark rect without being mistaken for a painted one.
    bool                 painted { false };
    // Accessibility pillar 3. A painted cell has no component to give the KEYBOARD
    // FOCUS to, and a screen reader can only read what is focused, so each one carries
    // an invisible mouse-transparent stand-in laid over `cell` (mu100a11y::CellProxy).
    // The CELL stays the source of truth for where the stop is -- the proxy is moved
    // to it. Null for a real control, where `comp` is the control and holds focus
    // itself.
    juce::Component*     a11yProxy { nullptr };
    std::function<void(int /*dir*/, bool /*fine*/)> adjust;
    // A stop that is TWO-DIMENSIONAL sets this as well: up/down drive `adjust`, and
    // the horizontal keys (`,` and `.` by default) drive this. Only the Graphic
    // Display's plot handles have one - a PITCH EG point is a TIME and a LEVEL, an EQ
    // node is a GAIN and a FREQUENCY, and in neither case is either one "the" value.
    //
    // Left null everywhere else ON PURPOSE, and that is what keeps the two new keys
    // off the host: with no adjustX the editor declines them and `,` / `.` go to the
    // DAW exactly as they did before.
    std::function<void(int /*dir*/, bool /*fine*/)> adjustX;
    // And a THIRD axis (`[` / `]` by default), for the stop that carries three
    // parameters: an EQ mid-band node, whose Q is neither its gain nor its frequency
    // but is edited from the same handle (the mouse spells it as an Alt-drag). Null
    // everywhere else -- including the EQ SHELF nodes, which have no Q -- so those two
    // keys reach the host from every other stop in the editor.
    std::function<void(int /*dir*/, bool /*fine*/)> adjustQ;
    std::function<void()> activate;
    // The SECONDARY action, on Ctrl+Enter. Set only where a stop genuinely has two
    // things to do and typing is the more frequent one: today the Graphic Display's
    // bar columns, where Enter types a value and this selects the part the column
    // belongs to. Null everywhere else, so Ctrl+Enter reaches the host from every
    // other stop -- the same rule the `.`/`,` and `[`/`]` axes follow.
    std::function<void()> activateAlt;
    std::function<void()> reset;

    bool isPainted() const { return painted; }
    // What the keyboard focus goes to for this stop, or null if nothing can take it.
    juce::Component* focusTarget() const { return painted ? a11yProxy : comp; }
};

// A mark this small in BOTH directions has no middle to leave out.
inline constexpr int kSmallCue = 22;   // design px

inline Item::Cue cueFor(juce::Rectangle<int> mark)
{
    return (mark.getWidth() <= kSmallCue && mark.getHeight() <= kSmallCue)
             ? Item::Cue::Ring : Item::Cue::Brackets;
}

//==============================================================================
// A module that offers keyboard stops. A panel that is just a bag of standard
// controls implements this as `return keynav::collect(*this);` and is done.
struct Group
{
    virtual ~Group() = default;
    virtual std::vector<Item> navItems() = 0;

    // The module's name, for the manual and any future spoken readout.
    virtual juce::String navGroupName() const = 0;
};

//==============================================================================
// Type dispatch for the standard controls.
namespace detail
{
    inline bool isStandardControl(juce::Component* c)
    {
        return dynamic_cast<XgKnob*>(c)          != nullptr
            || dynamic_cast<ValueBox*>(c)        != nullptr
            || dynamic_cast<XgSelector*>(c)      != nullptr
            || dynamic_cast<DotToggle*>(c)       != nullptr
            || dynamic_cast<ControllerWheel*>(c) != nullptr
            || dynamic_cast<juce::Button*>(c)    != nullptr;
    }

    // Depth-first, but a recognised control is a LEAF: an XgKnob owns a ValueBox and
    // a slider, and descending into it would offer the same parameter twice.
    inline void gather(juce::Component& parent, std::vector<juce::Component*>& out)
    {
        for (auto* c : parent.getChildren())
        {
            if (isStandardControl(c)) out.push_back(c);
            else                      gather(*c, out);
        }
    }
}

// Collect a module's controls in READING ORDER: rows top to bottom, and left to
// right within a row. Rows are banded rather than sorted on raw y, because a row of
// knobs and a row of value boxes do not share a top edge — kBandTol is roughly half
// a value box, comfortably under the gap between two real rows.
//
// Deliberately re-run on every keystroke rather than cached: it is a handful of
// dynamic_casts on the message thread, and a list built once could not see a panel
// that gained a control, hid its receive-switch row, or went into drum-edit mode.
// Same fail-closed reasoning as capturing undo at the funnel.
inline std::vector<Item> collect(juce::Component& parent)
{
    constexpr int kBandTol = 10;   // design px

    std::vector<juce::Component*> found;
    detail::gather(parent, found);

    // Positions in the MODULE's own coordinate space — the controls sit at varying
    // depths (an EQ cell is a grandchild), so their own getY() are not comparable.
    // A dial-less XgKnob is a value box with a label, but its BOUNDS still span the
    // hidden dial -- so both its reading-order position and its focus mark have to come
    // from the box, or it sorts a row late and gets marked a row tall.
    auto markIn = [](juce::Component* c) -> juce::Rectangle<int>
    {
        if (auto* k = dynamic_cast<XgKnob*>(c))
            if (! k->isDialShown()) return k->valueBoxBounds();
        return c->getLocalBounds();
    };
    auto rectIn = [&parent, &markIn](juce::Component* c)
    {
        return parent.getLocalArea(c, markIn(c));
    };

    std::sort(found.begin(), found.end(), [&](juce::Component* a, juce::Component* b)
    {
        const auto ra = rectIn(a), rb = rectIn(b);
        if (ra.getCentreY() != rb.getCentreY()) return ra.getCentreY() < rb.getCentreY();
        return ra.getX() < rb.getX();
    });

    std::vector<Item> items;
    items.reserve(found.size());

    for (size_t i = 0; i < found.size(); )
    {
        const int bandY = rectIn(found[i]).getCentreY();
        size_t j = i;
        while (j < found.size() && std::abs(rectIn(found[j]).getCentreY() - bandY) <= kBandTol)
            ++j;

        std::sort(found.begin() + (long) i, found.begin() + (long) j,
                  [&](juce::Component* a, juce::Component* b)
                  { return rectIn(a).getX() < rectIn(b).getX(); });

        for (size_t k = i; k < j; ++k)
        {
            Item it;
            it.comp = found[k];
            const auto mark = markIn(found[k]);
            if (mark != found[k]->getLocalBounds()) it.cell = mark;
            items.push_back(std::move(it));
        }
        i = j;
    }
    return items;
}

//==============================================================================
// Is this stop worth landing on right now? Asked at NAVIGATION time, never at
// collection time, because the answer moves: drum-edit mode, the plugin-only lock
// and the drum-part exclusions all grey controls while the module still holds them.
inline bool usable(const Item& it, const juce::Component& group)
{
    if (it.comp == nullptr) return false;

    for (auto* c = it.comp; c != nullptr && c != &group; c = c->getParentComponent())
        if (! c->isVisible()) return false;

    if (it.isPainted()) return true;          // a painted cell: its own lambdas decide
    if (! it.comp->isEnabled()) return false;

    // A bare knob is an unused grid position and an inert one is a parameter the
    // device ignores here; a blank box is a cell with no value in it at all. None is
    // somewhere to put a cursor. (setBare / setInteractive disable the inner dial,
    // not the XgKnob itself, so asking the wrapper's isEnabled would miss both.)
    if (auto* k = dynamic_cast<XgKnob*>(it.comp))
        return ! k->isBare() && k->isInteractive();
    if (auto* v = dynamic_cast<ValueBox*>(it.comp))
        return ! v->isBlank();

    // The controller wheel is a live performance control rather than a parameter, but
    // it is the one thing that makes the depth knobs beside it AUDIBLE, so it earns a
    // stop -- except in PITCH BEND mode. That mode is spring-loaded: the mouse returns
    // it to centre on release and a key press has no release, so arrows would leave the
    // part bent, which no mouse gesture can do and PANIC does not undo. Right-click
    // already declines there too, so the mode has nothing for any key to act on.
    if (auto* w = dynamic_cast<ControllerWheel*>(it.comp))
        return ! w->isSpring();

    return true;
}

//==============================================================================
// The three actions, on a standard control or through a painted cell's lambdas.
inline void adjust(const Item& it, int dir, bool fine)
{
    if (it.adjust) { it.adjust(dir, fine); return; }

    if (auto* k = dynamic_cast<XgKnob*>(it.comp))     { k->nudge(dir, fine); return; }
    if (auto* v = dynamic_cast<ValueBox*>(it.comp))   { v->nudge(dir, fine); return; }
    if (auto* s = dynamic_cast<XgSelector*>(it.comp)) { s->step(dir);        return; }
    if (auto* d = dynamic_cast<DotToggle*>(it.comp))  { d->nudge(dir);       return; }
    if (auto* w = dynamic_cast<ControllerWheel*>(it.comp)) { w->nudge(dir, fine); return; }
    // A Button is a Graphic Display screen: there is no more or less of one.
}

inline void activate(const Item& it)
{
    if (it.activate) { it.activate(); return; }

    if (auto* k = dynamic_cast<XgKnob*>(it.comp))     { k->beginTypeIn();          return; }
    if (auto* v = dynamic_cast<ValueBox*>(it.comp))   { v->beginTypeIn();          return; }
    if (auto* s = dynamic_cast<XgSelector*>(it.comp)) { s->openPopupFromKeyboard();return; }
    if (auto* d = dynamic_cast<DotToggle*>(it.comp))  { d->toggleFromUser();       return; }
    // A ControllerWheel deliberately falls through: it has no type-in and no pop-up,
    // and its "press" would be a re-send, which the resend lamp beside it already is.
    if (dynamic_cast<ControllerWheel*>(it.comp) != nullptr) return;
    if (auto* b = dynamic_cast<juce::Button*>(it.comp)) { b->triggerClick();       return; }
}

// No type fallbacks: a secondary action is authored or it does not exist. A control
// that has not been given one declines the key rather than inventing a second meaning.
inline void activateAlt(const Item& it)
{
    if (it.activateAlt) it.activateAlt();
}

inline void reset(const Item& it)
{
    if (it.reset) { it.reset(); return; }

    if (auto* k = dynamic_cast<XgKnob*>(it.comp))   { k->resetToDefault();      return; }
    if (auto* v = dynamic_cast<ValueBox*>(it.comp)) { v->resetToDefaultValue(); return; }
    if (auto* s = dynamic_cast<XgSelector*>(it.comp))
    {
        // Only the selectors that record a power-on value have a default to go to;
        // the navigation ones (effect type, controller source) deliberately do not.
        if (s->getDefaultIndex() >= 0) s->setSelectedIndex(s->getDefaultIndex(), true);
        return;
    }
    if (auto* w = dynamic_cast<ControllerWheel*>(it.comp)) { w->resetToRest(); return; }
    // A toggle records no default and a screen button has none: leave both alone
    // rather than inventing one.
}

//==============================================================================
// Where the focus ring goes, in `space`'s coordinates.
inline juce::Rectangle<int> ringBounds(const Item& it, juce::Component& space)
{
    if (it.comp == nullptr) return {};
    const auto local = it.cell.isEmpty() ? it.comp->getLocalBounds() : it.cell;
    return space.getLocalArea(it.comp, local);
}

} // namespace keynav
