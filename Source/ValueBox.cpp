// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "ValueBox.h"
#include "EditorTextScale.h"
#include "EditorColours.h"
#include "DialAction.h"
#include "EditUndo.h"
#include "Accessibility.h"
#include <cmath>

ValueBox::ValueBox()
{
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    // Accessibility pillar 2: an accessible ELEMENT is focusable, and a component
    // hidden from the tree is not -- ONE rule, applied here and at every other
    // setAccessible site. A screen reader moves its own reading cursor by moving
    // KEYBOARD focus (AccessibilityHandler::grabFocus does both), so a control that
    // cannot hold focus cannot be reached, whatever its handler says.
    //
    // A ValueBox inside an XgKnob is the exception, and the XgKnob's constructor
    // turns it off there for the same reason it calls setAccessible(false): the
    // wrapper is the one element for that parameter.
    setWantsKeyboardFocus(true);
}

ValueBox::~ValueBox()
{
    // If we're torn down mid-edit, drop the global listener we registered.
    if (editor != nullptr)
        juce::Desktop::getInstance().removeGlobalMouseListener(this);
}

void ValueBox::setRange(double lo, double hi, double step)
{
    minV = lo; maxV = hi; interval = (step > 0.0 ? step : 1.0);
    value = juce::jlimit(minV, maxV, snap(value));
    repaint();
}

void ValueBox::setOptions(const juce::StringArray& opts)
{
    options = opts;
    setRange(0.0, juce::jmax(0, opts.size() - 1), 1.0);
    typingEnabled = false;
    textFromValue = [this](double v) -> juce::String
    {
        const int i = juce::jlimit(0, options.size() - 1, (int) std::lround(v));
        return options.isEmpty() ? juce::String() : options[i];
    };
    repaint();
}

double ValueBox::snap(double v) const
{
    if (snapValue) return snapValue(v);
    if (interval <= 0.0) return v;
    return minV + std::round((v - minV) / interval) * interval;
}

void ValueBox::setValueQuiet(double v)
{
    value = juce::jlimit(minV, maxV, snap(v));
    repaint();
}

void ValueBox::setValueNotify(double v)
{
    const double nv = juce::jlimit(minV, maxV, snap(v));
    if (nv != value)
    {
        value = nv;
        repaint();
        if (onChange) onChange(value);
    }
}

juce::String ValueBox::currentText() const
{
    return textFromValue ? textFromValue(value)
                         : juce::String((int) std::lround(value));
}

void ValueBox::paint(juce::Graphics& g)
{
    if (editor != nullptr) return;   // editor covers the box while typing
    // The field is our bounds LESS the horizontal-bar strip, which the layout has
    // already granted us (overrideStripHeight). In the two styles that draw no
    // horizontal bar the strip is zero and this is the whole component, exactly as
    // it was -- so nothing outside the bar styles moves by a pixel.
    const auto fieldI = fieldBounds();
    auto b = fieldI.toFloat();

    juce::Colour bg   = edcol::kFieldBg;
    juce::Colour text = ! isEnabled() ? edcol::kValueOff
                      : dimmed        ? edcol::kValueDim      // out-of-range synced note
                                      : edcol::kValue;
    if (valueColour) valueColour(value, bg, text);   // client may recolour specific values

    g.setColour(bg);
    g.fillRect(b);
    g.setColour(edcol::kFieldBorder);
    g.drawRect(b, 1.f);
    // A blank box keeps its frame and background — the cell stays visible in the
    // grid — but shows no number, because it has no value to show (see setBlank).
    if (! blank)
    {
        g.setColour(text);
        g.setFont(edtext::font(edtext::Value, 11.f));
        // Keep the value text clear of the changed-tab gutter. The reserve is taken
        // off BOTH sides, which is the whole point: reducing symmetrically leaves the
        // centre exactly where it was, so every value that already fits renders in
        // identical pixels and no grid shifts by a sub-pixel. Only text wide enough to
        // reach the tab is affected, and drawFittedText condenses it to fit rather
        // than letting the tab sit on the first glyph (the effect grid's "THRU(20k)"
        // was touching it at the old 3.5 px, before the tab was widened at all).
        //
        // Unconditional, not gated on isChanged(): reserving only while the tab is up
        // would make the text jump the moment a value is edited away from its default.
        // Same rule XgSelector already follows ("kept even on untabbed selectors").
        // Derived from kChangedTabW, so widening the tab keeps its clearance for free.
        //
        // Condensing, not ellipsis: a squeezed "THRU(20k)" is still readable, whereas
        // "THRU(20..." has lost the value. 0.72 is a floor, not a target — it is only
        // reached by text that would otherwise overflow badly.
        const int gutter = (int) std::ceil(edcol::kChangedTabW);
        g.drawFittedText(currentText(), fieldI.reduced(gutter, 0),
                         juce::Justification::centred, 1, 0.72f);
    }

    // Option B: a changed-from-default value is marked with a yellow gutter tab
    // down the left inside edge (text colour is left untouched). Uses the same
    // graphic-editor yellow as the changed bars there. Suppressed when a parent
    // aggregates several boxes into one row-level marker.
    if (showChangedTab && isEnabled() && ! blank && isChanged())
        edcol::drawChangedTab(g, b);

    // ...and the MIDI-override mark, in whichever form the style asks for (see
    // MidiOverrideMarks.h). Deliberately NOT gated on showChangedTab: that flag
    // exists because a parent aggregates the changed marker into one row-level tab,
    // and there is no such aggregate here -- an overridden cell in an aggregated row
    // still has to say so itself.
    //
    // A box owned by a dial draws nothing in the arc style: there the DIAL carries
    // the mark, and two marks for one parameter would be two claims about it.
    if (overridePresent && isEnabled() && ! blank)
    {
        const float pos = midiOverrideProportion();
        const float org = midiOverrideOriginProportion();
        if (midimarks::verticalBar(dialOwned))
            edcol::drawMidiOverrideBar(g, b, pos, org);
        else if (midimarks::horizontalBar(dialOwned) && markable)
            edcol::drawMidiOverrideBarH(g, getLocalBounds().toFloat()
                                             .withTop((float) fieldI.getBottom()),
                                        pos, org, midimarks::kBarH);
    }
}

void ValueBox::mouseDown(const juce::MouseEvent& e)
{
    // While the type-in editor is open this is a GLOBAL-listener callback (the
    // editor child covers the box, so direct presses never reach here). A press
    // anywhere outside the editor commits the typed value and dismisses it.
    if (editor != nullptr)
    {
        if (! editor->getScreenBounds().contains(e.getScreenPosition()))
            commitEdit(true);
        return;
    }

    if (! isEnabled()) return;
    tipSuppressed = true;   // editing begins: keep the tooltip down until re-hover
    if (e.mods.isRightButtonDown()) { setValueNotify(defaultValue); return; }

    dragStartValue = value;
    dragStartY     = e.position.roundToInt().y;
    dragged        = false;

    if (dragValue) return;   // custom drag law: no pixels-per-step to work out

    dragStep = interval;
    const double numSteps = juce::jmax(1.0, (maxV - minV) / dragStep);
    double base = juce::jlimit(4.0, 24.0, 256.0 / numSteps);
    if (e.mods.isShiftDown()) base *= 5.0;   // fine feel
    pixelsPerStep = base;
}

void ValueBox::mouseDrag(const juce::MouseEvent& e)
{
    if (editor != nullptr) return;   // ignore global drags while editing
    if (! isEnabled() || e.mods.isRightButtonDown()) return;

    const int dyRaw = dragStartY - e.position.roundToInt().y;   // up = increase
    if (std::abs(dyRaw) > 2) dragged = true;

    // "Invert knob/dial direction" flips the drag direction (option, off by default).
    const int dy = dialaction::reversed() ? -dyRaw : dyRaw;

    if (dragValue)
    {
        setValueNotify(dragValue(dragStartValue, (double) dy, e.mods.isShiftDown()));
        return;
    }

    const double steps = std::round(dy / pixelsPerStep);
    setValueNotify(dragStartValue + steps * dragStep);
}

void ValueBox::mouseUp(const juce::MouseEvent& e)
{
    if (editor != nullptr) return;   // ignore global releases while editing
    if (! isEnabled()) return;
    mu100undo::requestSeal();        // end the undo step on release, not 700 ms later
    // A click without a drag (and not a right-click) opens the type-in editor.
    if (! dragged && typingEnabled && e.mouseWasClicked() && ! e.mods.isPopupMenu())
        startEdit();
}

void ValueBox::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // Global-listener callback while editing: a wheel anywhere outside the editor
    // commits + dismisses (including the wheel over another value box).
    if (editor != nullptr)
    {
        if (! editor->getScreenBounds().contains(e.getScreenPosition()))
            commitEdit(true);
        return;
    }

    if (! isEnabled()) return;
    tipSuppressed = true;   // wheel-editing also suppresses the tooltip until re-hover
    double dir = (w.deltaY != 0.0f) ? (w.deltaY > 0 ? 1.0 : -1.0)
                                    : (w.deltaX > 0 ? 1.0 : -1.0);
    if (dir == 0.0) return;
    if (dialaction::reversed()) dir = -dir;   // "Invert knob/dial direction" option

    nudge((int) dir, e.mods.isShiftDown());
}

// One detent's worth, shared by the wheel above and the keyboard's arrow keys so
// the two cannot drift. With no custom law a box's step IS its interval, which is
// already the finest it has -- so `fine` changes nothing there, and that is right:
// there is nothing finer to ask for.
void ValueBox::nudge(int dir, bool fine)
{
    if (dir == 0 || ! isEnabled() || editor != nullptr) return;

    if (wheelValue) { setValueNotify(wheelValue(value, dir, fine)); return; }
    setValueNotify(value + dir * interval);
}

// Open the type-in editor, exactly as a click without a drag does.
void ValueBox::beginTypeIn()
{
    if (! isEnabled() || ! typingEnabled || blank || editor != nullptr) return;
    startEdit();
}

void ValueBox::startEdit()
{
    editor = std::make_unique<juce::TextEditor>();
    editor->setBounds(fieldBounds());   // the field, not the bar strip below it
    editor->setJustification(juce::Justification::centred);
    editor->setFont(edtext::font(edtext::Value, 11.f));
    editor->setText(currentText(), false);
    editor->selectAll();
    editor->onReturnKey  = [this] { commitEdit(true);  };
    editor->onEscapeKey  = [this] { commitEdit(false); };
    editor->onFocusLost  = [this] { commitEdit(true);  };
    addAndMakeVisible(*editor);
    editor->grabKeyboardFocus();

    // Watch the whole GUI: a press/wheel outside this editor deselects it.
    // (Focus-loss alone doesn't cover surfaces that never take keyboard focus.)
    juce::Desktop::getInstance().addGlobalMouseListener(this);
    repaint();
}

void ValueBox::commitEdit(bool apply)
{
    if (editor == nullptr) return;

    juce::Desktop::getInstance().removeGlobalMouseListener(this);

    const juce::String txt = editor->getText();
    // Detach callbacks and defer destruction so we don't delete the editor from
    // inside its own callback.
    editor->onReturnKey = editor->onEscapeKey = editor->onFocusLost = nullptr;
    std::unique_ptr<juce::TextEditor> dead = std::move(editor);
    juce::MessageManager::callAsync([d = std::move(dead)]() mutable { d.reset(); });

    if (apply)
    {
        const double v = valueFromText ? valueFromText(txt) : txt.getDoubleValue();
        setValueNotify(v);
    }
    repaint();

    // LAST, and after the value has been applied: an overlay box hides itself from
    // here, and hiding before the write would take the box out from under it.
    if (onTypeInEnd) onTypeInEnd();
}

void ValueBox::mouseExit(const juce::MouseEvent&)
{
    tipSuppressed = false;   // cursor left: allow the tooltip again on the next hover
}

juce::String ValueBox::getTooltip()
{
    return tipSuppressed ? juce::String() : juce::SettableTooltipClient::getTooltip();
}

//==============================================================================
// THE SPOKEN VALUE CARRIES THE OVERRIDE -- see the header. Written as a sentence
// rather than a pair of numbers, because it is read aloud: "100, MIDI has the
// device at 32".
juce::String ValueBox::spokenText() const
{
    const auto own = currentText();
    if (! overridePresent || blank) return own;
    return mu100a11y::withOverride(own, textFromValue ? textFromValue(overrideValue)
                                                      : juce::String(overrideValue));
}

//==============================================================================
// Accessibility (pillar 1): name from the tooltip, value as the box DISPLAYS it.
//
// setValueAsString goes through the same parse the type-in editor uses, so a screen
// reader's write lands on the snap grid, in range, and through the funnel that
// records undo and paces the wire.
std::unique_ptr<juce::AccessibilityHandler> ValueBox::createAccessibilityHandler()
{
    // A BLANK box is a cell with no value in it at all (the Part module's drum-mode
    // cells before a key is picked). Nothing to name and nothing to read out.
    if (blank)
        return std::make_unique<mu100a11y::Handler>(*this, juce::AccessibilityRole::ignored,
                                                    juce::String());

    // THE SPOKEN VALUE CARRIES THE OVERRIDE, and that is the whole of pillar 4 for
    // this feature -- no new announcement machinery. The override changes no VALUE,
    // so FocusValueWatcher (which polls the focused element's spoken string) would
    // otherwise never see it: the mark appears and a screen-reader user is told
    // nothing. Folding it into the string means the watcher announces it when it
    // arrives or retires, AND a reader landing on the control reads both halves.
    //
    // Written as a sentence rather than a pair of numbers, because it is read aloud:
    // "100, MIDI has the device at 32".
    //
    // Safe against the WRITE path: the getter and the setter are separate, and the
    // setter is fed user-typed text, never this string.
    return mu100a11y::makeValueHandler(
        *this, rawTip, juce::AccessibilityRole::slider,
        [this] { return spokenText(); },
        [this](const juce::String& t)
        {
            setValueNotify(valueFromText ? valueFromText(t) : t.getDoubleValue());
        },
        [this] { return ! isEnabled() || blank; });
}
