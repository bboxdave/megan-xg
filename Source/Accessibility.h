// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include <cstdlib>
#include <functional>

//==============================================================================
// mu100a11y — the pieces that make the editor's controls readable by a screen
// reader: a NAME, a ROLE, and a VALUE SPOKEN AS THE CONTROL SHOWS IT.
//
// This is pillar 1 of the accessibility work. It is
// deliberately independent of keyboard focus: a handler describes a control to the
// accessibility tree whether or not anything is focused, so this lands with no
// change to the keyboard-cursor design and nothing to re-test in a host.
//
// THE NAME COMES FROM THE CONTROL'S OWN TOOLTIP. `mu100editor/data/tooltips.csv`
// already carries 244 authored, human-readable names — "Master Volume", "Multi EQ
// Band Center Frequency" — wired to every control class, so there is nothing to
// author again and, more importantly, nothing that can drift: a control that gains
// a tooltip gains a name, and a rewording reaches both at once.
//==============================================================================
namespace mu100a11y
{

//==============================================================================
// A tooltip is usually just a name ("Master Tune", median length 24 characters),
// but a few carry a name plus an explanation: "Part 12 Mute - click to toggle,
// Ctrl+click to mute this part alone". Split at the first " - " or ": " so the
// NAME stays short enough to be spoken on every move and the rest becomes the
// description, which a screen reader reads only when asked.
inline juce::String nameOf(const juce::String& tip)
{
    for (auto* sep : { " - ", ": " })
    {
        const auto i = tip.indexOf(juce::String(sep));
        if (i > 0) return tip.substring(0, i).trim();
    }
    return tip.trim();
}

inline juce::String descOf(const juce::String& tip)
{
    for (auto* sep : { " - ", ": " })
    {
        const auto i = tip.indexOf(juce::String(sep));
        if (i > 0) return tip.substring(i + (int) juce::String(sep).length()).trim();
    }
    return {};
}

//==============================================================================
// THE ONE WORDING for "your value, and where incoming MIDI has left the device".
//
// Every surface that can carry a MIDI-override mark folds it into the SPOKEN VALUE
// rather than announcing it separately, because the override moves no value of ours:
// FocusValueWatcher polls the focused element's spoken string, so a mark that is only
// a colour and a bar would arrive in total silence. Folding it in means the watcher
// says it the moment it appears, and a reader landing on the control hears both
// halves. Written as a sentence, not a pair of numbers -- it is read aloud.
//
// One function because the sentence is the same on a knob, a value box, an envelope
// handle, a mixer bar, an EQ node and a voice row, and six copies of it would drift.
inline juce::String withOverride(const juce::String& own, const juce::String& device)
{
    return device.isEmpty() ? own : own + ", MIDI has the device at " + device;
}

//==============================================================================
// One control's value, as TEXT.
//
// THE RANGE IS DELIBERATELY LEFT INVALID, and that is the whole point of using this
// rather than juce::AccessibilityRangedNumericValueInterface. Windows offers two UIA
// patterns here (juce_AccessibilityElement_windows.cpp): ValuePattern whenever there
// is a value interface, and RangeValuePattern *as well* when the range is valid — and
// a screen reader given a range on a slider reads the NUMBER. The ranged helper makes
// that unavoidable: its getCurrentValueAsString() is `final` and returns
// String(getCurrentValue()), i.e. the raw byte.
//
// Raw bytes are exactly what must not be spoken here. This device stores pan as
// 0..127, filter cutoff centred on 64 and EQ frequency as a table index; the editor
// already renders those as "L63", "-20" and "1.4k" for the display and the undo
// labels, and that is what a user needs to hear. Offering only ValuePattern is what
// gets the formatted string spoken.
class TextValue : public juce::AccessibilityValueInterface
{
public:
    using Getter   = std::function<juce::String()>;
    using Setter   = std::function<void(const juce::String&)>;
    using ReadOnly = std::function<bool()>;

    TextValue(Getter g, Setter s, ReadOnly ro)
        : get(std::move(g)), set(std::move(s)), readOnly(std::move(ro)) {}

    bool isReadOnly() const override  { return readOnly ? readOnly() : (set == nullptr); }

    juce::String getCurrentValueAsString() const override { return get ? get() : juce::String(); }
    // Only used where a client insists on a number; the string above is the real answer.
    double getCurrentValue() const override { return getCurrentValueAsString().getDoubleValue(); }

    // Writes go through the SAME parse the type-in box uses, so the undo capture, the
    // send throttle and the DAW automation mirror all apply — a screen reader cannot
    // reach the device by a route the editor does not police.
    void setValueAsString(const juce::String& v) override { if (set) set(v); }
    void setValue(double v) override                      { setValueAsString(juce::String(v)); }

    juce::AccessibilityValueInterface::AccessibleValueRange getRange() const override
    {
        return {};   // invalid ON PURPOSE — see the note above
    }

private:
    Getter   get;
    Setter   set;
    ReadOnly readOnly;
};

//==============================================================================
// A handler that names itself from a tooltip. `tip` is captured by value at
// construction; controls whose tooltip changes with mode (the Part module's EQ cells
// retarget between part and drum key) rebuild their handler through
// invalidateAccessibilityHandler() when they re-key it.
//
// An explicit Component::setTitle() still wins, for the controls that have no tooltip
// to borrow from.
class Handler : public juce::AccessibilityHandler
{
public:
    Handler(juce::Component& c,
            juce::AccessibilityRole role,
            juce::String tip,
            juce::AccessibilityActions actions = {},
            Interfaces interfaces = {})
        : juce::AccessibilityHandler(c, role, std::move(actions), std::move(interfaces)),
          tooltip(std::move(tip)) {}

    juce::String getTitle() const override
    {
        auto t = juce::AccessibilityHandler::getTitle();
        return t.isNotEmpty() ? t : nameOf(tooltip);
    }

    juce::String getDescription() const override
    {
        auto d = juce::AccessibilityHandler::getDescription();
        return d.isNotEmpty() ? d : descOf(tooltip);
    }

    juce::String getHelp() const override
    {
        auto h = juce::AccessibilityHandler::getHelp();
        return h.isNotEmpty() ? h : tooltip;
    }

private:
    juce::String tooltip;
};

//==============================================================================
// Convenience: a value-bearing control (a knob, a value box, a selector, the
// controller wheel). Role `slider` is JUCE's own choice for a continuous control.
inline std::unique_ptr<juce::AccessibilityHandler>
makeValueHandler(juce::Component& c,
                 const juce::String& tip,
                 juce::AccessibilityRole role,
                 TextValue::Getter get,
                 TextValue::Setter set,
                 TextValue::ReadOnly readOnly = nullptr)
{
    return std::make_unique<Handler>(
        c, role, tip, juce::AccessibilityActions{},
        juce::AccessibilityHandler::Interfaces{
            std::make_unique<TextValue>(std::move(get), std::move(set), std::move(readOnly)) });
}

//==============================================================================
// A two-state control (the LED toggles). Modelled on JUCE's own
// detail::ButtonAccessibilityHandler: role toggleButton, a checkable/checked state,
// and a toggle action — no value interface, because "checked" IS the value.
class ToggleHandler : public Handler
{
public:
    ToggleHandler(juce::Component& c, juce::String tip,
                  std::function<bool()> isOn, std::function<void()> toggle)
        : Handler(c, juce::AccessibilityRole::toggleButton, std::move(tip),
                  juce::AccessibilityActions().addAction(
                      juce::AccessibilityActionType::toggle,
                      [t = std::move(toggle)] { if (t) t(); })),
          on(std::move(isOn)) {}

    juce::AccessibleState getCurrentState() const override
    {
        auto s = Handler::getCurrentState().withCheckable();
        return (on && on()) ? s.withChecked() : s;
    }

private:
    std::function<bool()> on;
};

//==============================================================================
// FocusableButton — a TextButton that can hold keyboard focus without stealing keys.
//
// juce::Button::keyPressed triggers a click on the RETURN key and does NOT look at
// the modifiers (juce_Button.cpp), so a focused button eats Alt+Return along with
// plain Return -- the Cubase round-1 bug, whose fix was to make every button in this
// window refuse focus outright. That fix is incompatible with pillar 2: a screen
// reader can only read what can be focused.
//
// So the button takes focus and DECLINES EVERY KEY, which leaves the plugin editor
// the single reader of keys exactly as before. Pressing it from the keyboard still
// works -- keynav's Enter (or the accessibility floor's) calls the same triggerClick
// path a mouse click does, through the editor.
class FocusableButton : public juce::TextButton
{
public:
    explicit FocusableButton(const juce::String& name) : juce::TextButton(name)
    {
        setWantsKeyboardFocus(true);
    }

    bool keyPressed(const juce::KeyPress&) override { return false; }
};

//==============================================================================
// CellProxy — accessibility pillar 3, for a control that is PAINTED rather than
// built out of components.
//
// Five stops in this editor are drawn by their panel and have no component of their
// own: the Voice module's Mute, Solo and voice-name cells, and the Keyboard module's
// DRUM KEY box. A screen reader can only reach what can hold KEYBOARD FOCUS, and
// only a Component can, so each of those cells gets one of these laid over it: an
// invisible, MOUSE-TRANSPARENT child that holds the focus and answers the tree in
// the cell's place.
//
// A proxy rather than real controls, deliberately. The Voice module paints 32 rows
// in one pass and the mute/solo semantics were settled over a Cubase round; turning
// those cells into widgets would rewrite painting, hit-testing and layout to gain
// nothing a proxy does not already give. The panel keeps drawing exactly as it did,
// and the KEYNAV CELL RECT stays the single source of truth for where the stop is —
// the proxy is moved to it, never the other way round.
//
// It paints nothing (no paint() override) but is genuinely visible, because an
// invisible component is not in the accessibility tree and cannot take focus. Hide
// it — setVisible(false) — for a cell that is not currently offered, which is
// exactly what keeps it out of both.
class CellProxy : public juce::Component
{
public:
    CellProxy()
    {
        // The panel underneath keeps every click: it owns the hit-testing for these
        // cells, and a proxy that swallowed a press would break Mute, Solo and the
        // Voice List in one go.
        setInterceptsMouseClicks(false, false);
        setWantsKeyboardFocus(true);
    }

    // What the cell is called, and what it currently reads. Both are asked LIVE
    // rather than cached: the row under the cursor changes with the selected part,
    // and the state changes under it.
    std::function<juce::String()> title;
    std::function<juce::String()> value;       // optional
    std::function<juce::String()> description; // optional
    std::function<bool()>         checked;     // set => a checkable toggle
    std::function<void()>         onActivate;  // Enter / a screen reader's "invoke"

    void setRole(juce::AccessibilityRole r) { role = r; invalidateAccessibilityHandler(); }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        juce::AccessibilityActions actions;
        if (onActivate)
        {
            actions = actions.addAction(juce::AccessibilityActionType::press,
                                        [this] { if (onActivate) onActivate(); });
            if (checked)
                actions = actions.addAction(juce::AccessibilityActionType::toggle,
                                            [this] { if (onActivate) onActivate(); });
        }

        juce::AccessibilityHandler::Interfaces interfaces;
        if (value)
            interfaces.value = std::make_unique<TextValue>(
                [this] { return value ? value() : juce::String(); }, nullptr, nullptr);

        return std::make_unique<ProxyHandler>(*this, role, std::move(actions),
                                              std::move(interfaces));
    }

private:
    // The titles here are composed live ("Part 12 Mute"), so this does NOT go through
    // mu100a11y::Handler, which captures one tooltip string at construction.
    struct ProxyHandler : public juce::AccessibilityHandler
    {
        ProxyHandler(CellProxy& p, juce::AccessibilityRole r,
                     juce::AccessibilityActions a, Interfaces i)
            : juce::AccessibilityHandler(p, r, std::move(a), std::move(i)), proxy(p) {}

        juce::String getTitle() const override
        {
            return proxy.title ? nameOf(proxy.title()) : juce::String();
        }
        juce::String getDescription() const override
        {
            if (proxy.description) return proxy.description();
            return proxy.title ? descOf(proxy.title()) : juce::String();
        }
        juce::AccessibleState getCurrentState() const override
        {
            auto st = juce::AccessibilityHandler::getCurrentState();
            if (! proxy.checked) return st;
            st = st.withCheckable();
            return proxy.checked() ? st.withChecked() : st;
        }

        CellProxy& proxy;
    };

    juce::AccessibilityRole role { juce::AccessibilityRole::button };
};

//==============================================================================
// Pillar 4 - ANNOUNCEMENTS. Telling the user about a change they cannot see.
//
// TWO THINGS ARE TRUE ON WINDOWS AND BOTH SHAPE THIS:
//
//  1. `AccessibilityHandler::notifyAccessibilityEvent(valueChanged)` IS THE WRONG
//     MECHANISM FOR OUR CONTROLS, and using it would undo pillar 1. JUCE's Windows
//     backend branches on the ROLE: for `slider` it sends UIA_RangeValueValuePropertyId
//     carrying `getCurrentValue()` - the NUMBER - and only for other roles does it send
//     UIA_ValueValuePropertyId with the string. Our controls are sliders whose spoken
//     value is deliberately a STRING with an INVALID range (see TextValue), so that
//     event would carry `String("L63").getDoubleValue()` = 0, for a pattern the element
//     does not even advertise. So we never send it, and speak the value ourselves.
//
//  2. `postAnnouncement` does NOT go through the screen reader on Windows - it drives
//     SAPI (`ISpVoice`) directly. So announcements come out in the system voice rather
//     than in NVDA's, at NVDA-independent rate and volume. That is a real limitation of
//     the platform backend, not something to design around; it is also why announcements
//     are kept SHORT and RARE. Its priorities are counter-intuitive: `medium` is the one
//     that queues politely (SPVPRI_NORMAL), `high` interrupts, and `low` speaks OVER the
//     top of whatever is already being said.
//
// Nothing here runs unless a screen reader is actually listening
// (`areAnyAccessibilityClientsActive`), so a sighted user pays one bool per timer tick.
namespace announce
{

// The test hook. This whole feature is invisible without a screen reader, and an
// untested path is not a feature - the same argument as SWP30_REVDPCM_FORCE. Setting
// MU100_A11Y_SPEAK to a file path pretends a client is active and logs every
// announcement to that file instead of relying on speech, so the queue, the coalescing
// and every call site can be exercised and MEASURED with no screen reader present.
inline const char* logPath()
{
    static const char* p = std::getenv("MU100_A11Y_SPEAK");
    return p;
}

// THERE IS NO "is a screen reader listening" TEST AVAILABLE TO US.
// `AccessibilityHandler::areAnyAccessibilityClientsActive()` is exactly that question
// and JUCE keeps it PRIVATE; on Windows it wraps `UiaClientsAreListening`, which would
// mean dragging the Win32 UIA headers into this one. So nothing here is gated on it,
// and that is affordable because the only expensive step already gates itself:
// `postAnnouncement` early-returns on the same test before it touches SAPI. What runs
// unconditionally is a string compare every eighth GUI tick and a 120 ms timer that
// only exists while something is queued.
inline bool active() { return true; }

// What an announcement is about. The channel is the COALESCING key: a second message on
// the same channel REPLACES an unspoken first, which is what keeps a swept automation
// lane or a held Ctrl+arrow from queueing a paragraph. Ordered by urgency - the timer
// emits the most urgent pending channel first.
enum Channel
{
    Value = 0,   // the focused control's value changed under the user
    Context,     // where you are: part, view, mode
    Edit,        // what just happened: undo, mute, a file loaded
    Alert,       // PANIC, the engine stopped -- interrupts
    NumChannels
};

//==============================================================================
// The queue. One per process (SharedResourcePointer): speech is a single shared
// resource, so two plugin instances announcing at once would collide whatever we did.
class Queue : private juce::Timer
{
public:
    ~Queue() override { stopTimer(); }

    void say(Channel ch, const juce::String& text)
    {
        if (! active() || text.isEmpty()) return;

        // An exact repeat says nothing new and costs a second of the user's attention.
        if (text == lastSpoken[ch]) return;

        pending[ch] = text;

        // An alert is the one thing worth interrupting for, so it does not wait for the
        // tick -- a PANIC or a dead engine is useless news a second late.
        if (ch == Alert) { emit(Alert); return; }

        // The timer exists only while there is something to say, so an idle plugin --
        // which is every plugin with no screen reader attached -- runs no timer at all.
        if (! isTimerRunning()) startTimer(kTickMs);
    }

    // Say it now, exactly once, ignoring the queue -- for a message that is already
    // rate-limited by the thing that produced it.
    void sayNow(Channel ch, const juce::String& text)
    {
        if (! active() || text.isEmpty()) return;
        pending[ch] = text;
        emit(ch);
    }

private:
    static constexpr int kTickMs = 120;   // ~8 announcements a second, worst case

    void timerCallback() override
    {
        for (int ch = NumChannels; --ch >= 0;)
            if (pending[ch].isNotEmpty())
            {
                emit((Channel) ch);
                return;   // ONE per tick: the gap is what makes speech followable
            }

        stopTimer();   // drained
    }

    void emit(Channel ch)
    {
        const auto text = pending[ch];
        if (text.isEmpty()) return;
        pending[ch].clear();
        lastSpoken[ch] = text;

        if (auto* p = logPath())
        {
            juce::File(juce::String(p))
                .appendText(juce::String((int) juce::Time::getMillisecondCounter())
                            + " [" + juce::String((int) ch) + "] " + text + juce::newLine);
            return;   // under the test hook, LOG instead of speaking
        }

        using AH = juce::AccessibilityHandler;
        AH::postAnnouncement(text, ch == Alert ? AH::AnnouncementPriority::high
                                              : AH::AnnouncementPriority::medium);
    }

    juce::String pending[NumChannels];
    juce::String lastSpoken[NumChannels];
};

using Shared = juce::SharedResourcePointer<Queue>;

//==============================================================================
// FocusValueWatcher - "the value under you changed".
//
// Polled rather than hooked into the ~50 places that can move a value, for the same
// reason undo captures at the funnel: a source added tomorrow is covered with no edit,
// and there is no site to forget. It asks the FOCUSED element what it reads, which
// works for every control class and for the painted cells' proxies alike, and it
// coalesces for free -- a sweeping automation lane is read at the poll rate, not at
// the rate it changes.
//
// It stays SILENT on a focus MOVE. The screen reader announces the name and value of
// whatever it has just focused; speaking again over that is the fastest way to make a
// plugin unbearable.
class FocusValueWatcher
{
public:
    void poll(Queue& q)
    {
        if (! active()) return;

        auto* f = juce::Component::getCurrentlyFocusedComponent();
        const auto v = read(f);

        if (f != last)          // focus moved: the reader is already saying this
        {
            last = f;
            lastValue = v;
            return;
        }

        if (v != lastValue)
        {
            lastValue = v;
            if (v.isNotEmpty()) q.say(Value, v);
        }
    }

private:
    // The spoken value of whatever is focused: the value interface where there is one,
    // and the CHECKED state where there is not (the LED toggles and the Mute/Solo
    // cells, whose value is "on" or "off" -- see ToggleHandler).
    static juce::String read(juce::Component* c)
    {
        if (c == nullptr) return {};
        auto* h = c->getAccessibilityHandler();
        if (h == nullptr) return {};

        if (auto* vi = h->getValueInterface())
            return vi->getCurrentValueAsString();

        const auto st = h->getCurrentState();
        if (st.isCheckable())
            return st.isChecked() ? "on" : "off";

        return {};
    }

    juce::Component* last { nullptr };
    juce::String     lastValue;
};

} // namespace announce

} // namespace mu100a11y
