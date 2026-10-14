// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "KeyboardPanel.h"
#include "EditorTextScale.h"
#include "EditorColours.h"
#include "MidiIndicator.h"
#include "ValueBox.h"
#include "PluginProcessor.h"
#include "codec/MU100AddressMap.h"
#include "codec/MU100State.h"
#include "codec/MU100Drums.h"
#include "codec/MU100Tooltips.h"
#include <cmath>

namespace
{
    // The no-part MIDI-in lamps, in row order. `mask` is the processor activity
    // bit the lamp watches; `tip` its tooltip key. Label font matches LO/HI.
    struct LampSpec { const char* label; uint32_t mask; const char* tip; };
    const LampSpec kLamps[] = {
        { "SYS",  YamahaMU100AudioProcessor::SYSACT_SYS,  "KBD|SYS LAMP"  },
        { "FX",   YamahaMU100AudioProcessor::SYSACT_FX,   "KBD|FX LAMP"   },
        { "BULK", YamahaMU100AudioProcessor::SYSACT_BULK, "KBD|BULK LAMP" },
    };
    constexpr int kLampLabelGap = 4;   // label -> lamp frame
    constexpr int kLampH        = 11;  // lamp height (matches a Voice-module row tab)
    constexpr int kLampBorder   = 1;   // frame thickness — same as the LO/HI value boxes
    const juce::Colour& kLampFrame = edcol::kFieldBorder;   // ...and the same colour

    juce::Font lampFont() { return edtext::font(edtext::Label, 10.f, juce::Font::bold); }
}

//==============================================================================
bool KeyboardPanel::isBlackNote(int note)
{
    switch (note % 12)
    {
        case 1: case 3: case 6: case 8: case 10: return true;
        default:                                 return false;
    }
}

KeyboardPanel::KeyboardPanel(YamahaMU100AudioProcessor& proc) : audioProcessor(proc)
{
    using mu100::AddressMap;
    noteLowParam  = AddressMap::find("NOTE LIMIT LOW",      "MULTI PART");
    noteHighParam = AddressMap::find("NOTE LIMIT HIGH",     "MULTI PART");
    velLowParam   = AddressMap::find("VELOCITY LIMIT LOW",  "MULTI PART");
    velHighParam  = AddressMap::find("VELOCITY LIMIT HIGH", "MULTI PART");
    // Which channel the current part listens on, for the played-note display. Read
    // live from the model rather than mirrored here: it is PartListPanel's box that
    // owns the value, and a second copy is a second thing to keep in step.
    chParam       = AddressMap::find("Rcv CHANNEL",         "MULTI PART");

    // Precompute each note's white-key column (black keys -> -1).
    int wi = 0;
    for (int n = 0; n < 128; ++n)
        whiteIndex[(size_t) n] = isBlackNote(n) ? -1 : wi++;
    numWhite = wi;

    auto makeVelBox = [this](int paramIx)
    {
        auto b = std::make_unique<ValueBox>();
        b->setRange(1.0, 127.0, 1.0);
        b->setDefaultValue(paramIx == velLowParam ? 1 : 127);
        b->setValueQuiet(paramIx == velLowParam ? 1 : 127);
        if (paramIx >= 0)
            b->onChange = [this, paramIx](double v)
            {
                audioProcessor.sendParamChange(mu100::AddressMap::makeParamChange(
                    mu100::AddressMap::param(paramIx), currentPart, (uint32_t) v));
            };
        addAndMakeVisible(*b);
        return b;
    };
    velLoBox = makeVelBox(velLowParam);
    velHiBox = makeVelBox(velHighParam);
    velLoBox->setTooltip(mu100::tooltips::forKey("KBD|VEL LOW"));
    velHiBox->setTooltip(mu100::tooltips::forKey("KBD|VEL HIGH"));

    static_assert((int) (sizeof(kLamps) / sizeof(kLamps[0])) == kNumLamps,
                  "kLamps and kNumLamps must agree");

    // The timer now runs continuously: it drives the SYS/FX/BULK lamp envelopes
    // as well as (in drum-edit mode only) the played-note key selection.
    startTimer(midiind::kTickMs);
}

juce::String KeyboardPanel::getTooltip()
{
    const auto p = getMouseXYRelative();
    // Drum-edit-key box: only meaningful (clickable) for a buffered drum part.
    if (drumPart && drumBoxRect.contains(p))
        return mu100::tooltips::forKey("KBD|DRUM EDIT KEY");
    // The marker strip: what the two marks under the mouse actually say for THIS key,
    // and then the strip's own name. The counts are the point -- a tab tells you
    // something happened here, and the only follow-up question is how much.
    //
    // ONE LINE, KEY FIRST. The tooltip may be rendered in the bottom status bar,
    // which shows a single line and clips the rest: a paragraph with the counts at
    // the bottom would put the only per-key part of it out of sight.
    if (const int sn = stripNoteAt(p); sn >= 0)
    {
        int nCh = 0, nOv = 0;
        audioProcessor.drumKeyMarkDetail(drumBuffer, sn, nCh, nOv);
        const char* nm = mu100::drumVoiceName(kitMsb, kitLsb, kitPrg0 + 1, sn);

        juce::String tip;
        tip << rangeNoteName(sn) << " "
            << (nm != nullptr ? juce::String(nm).toUpperCase() : juce::String("NO DRUM KEY"))
            << " - " << (nCh == 0 ? juce::String("no edited values")
                                  : juce::String(nCh) + (nCh == 1 ? " edited value"
                                                                  : " edited values"));
        if (nOv > 0)
            tip << ", " << juce::String(nOv)
                << (nOv == 1 ? " MIDI override" : " MIDI overrides");

        juce::String name = mu100::tooltips::forKey("KBD|DRUM MARK STRIP");
        if (name.isEmpty())
            name = "Drum keys with edited values (upper row) or MIDI overrides "
                   "(lower row) - click a slot to edit that key";
        return tip + ". " + name;
    }
    if (keysArea().contains(p))
        return mu100::tooltips::forKey(drumEditActive ? "KBD|KEYBOARD DRUM" : "KBD|KEYBOARD");
    // SYS/FX/BULK lamps — the whole label+lamp cell is the hover target, not just
    // the 4px tab.
    for (int i = 0; i < kNumLamps; ++i)
        if (lampGroup[(size_t) i].contains(p))
            return mu100::tooltips::forKey(kLamps[i].tip);
    return {};
}

KeyboardPanel::~KeyboardPanel()
{
    // The gesture lives in the PROCESSOR, which outlives this panel: closing the
    // editor mid-drag would leave its depth stuck above zero and fold every later
    // edit of the session into one undo step.
    endRangeUndo();
}

//==============================================================================
int KeyboardPanel::rightPanelW() const
{
    // Slightly narrower than before (was getWidth()/6) so the keys keep their
    // size once a one-key margin is added on each side (see keyMargin/keysArea).
    return juce::jlimit(110, 220, juce::roundToInt((float) getWidth() * 0.15f));
}

int KeyboardPanel::keyMargin() const
{
    // One white-key width of blank space on each side of the keys: the left margin
    // keeps the "C-2" octave label on-screen; the right margin matches it. Sizing
    // the margin as a (numWhite + 2) split makes it exactly one white key wide and
    // preserves the key size (the right panel absorbs the difference).
    const int inner = getLocalBounds().withTrimmedRight(rightPanelW()).reduced(2, 0).getWidth();
    return numWhite > 0 ? inner / (numWhite + 2) : 16;
}

juce::Rectangle<int> KeyboardPanel::keysArea() const
{
    const int m = keyMargin();
    return getLocalBounds()
        .withTrimmedRight(rightPanelW())
        .withTrimmedTop(6 + kMarkStripH)   // red range bar, then the per-key marks
        .withTrimmedBottom(18)    // octave labels (taller font)
        .reduced(2, 0)
        .withTrimmedLeft(m)       // left margin: keeps the "C-2" label readable
        .withTrimmedRight(m);     // matching right margin before the velocity panel
}

juce::Rectangle<float> KeyboardPanel::noteRect(int note, bool& isBlack) const
{
    const auto area = keysArea().toFloat();
    const float ww  = numWhite > 0 ? area.getWidth() / (float) numWhite : 0.f;
    isBlack = isBlackNote(note);
    if (! isBlack)
    {
        const float x = area.getX() + whiteIndex[(size_t) note] * ww;
        return { x, area.getY(), ww, area.getHeight() };
    }
    // A black key straddles the boundary between the white key below it and the
    // next white key.
    const int wiBelow = whiteIndex[(size_t) (note - 1)];
    const float bw = ww * 0.62f;
    const float cx = area.getX() + (wiBelow + 1) * ww;
    return { cx - bw * 0.5f, area.getY(), bw, area.getHeight() * 0.62f };
}

//==============================================================================
// The per-key marker strip.
//
// WHY A STRIP AND NOT MARKS ON THE KEYS. The information is per note and has to be
// read as a ROW -- "where in this kit has anything happened" -- and the keys are the
// wrong ground for that: a black key is 62% of a white key's height and 62% of its
// width, so marks drawn on the keys themselves sit at two heights and two widths and
// the eye cannot sweep them. A strip is one straight line at one size, and each slot
// still sits over the key it belongs to. It costs kMarkStripH of key height, taken
// unconditionally so the keyboard does not resize when the selected part changes.
int KeyboardPanel::markStripTop() const
{
    return keysArea().getY() - kMarkStripH + kMarkTopGap;
}

// The band the strip paints over: the KIT'S key span (13..91), not the whole
// keyboard. Ending it where the kit ends is free and says something true -- there
// are no drum tones outside it to have marks.
juce::Rectangle<int> KeyboardPanel::markStripBounds() const
{
    bool b = false;
    const auto lo = noteRect(mu100::kDrumFirstNote, b);
    const auto hi = noteRect(mu100::kDrumLastNote,  b);
    return { (int) std::floor(lo.getX()), markStripTop(),
             (int) std::ceil(hi.getRight() - lo.getX()),
             2 * kMarkRowH + kMarkRowGap };
}

// One note's tab in row 0 (changed) or row 1 (overridden). Centred on the note's own
// centre, so a tab sits over its key whether that key is black or white. Adjacent
// note centres are half a white key apart, so the width is capped just under that --
// tabs that touched would read as one long bar rather than as a set of keys.
juce::Rectangle<float> KeyboardPanel::markSlot(int note, int row) const
{
    bool black = false;
    const auto r = noteRect(note, black);
    const auto area = keysArea().toFloat();
    const float ww = numWhite > 0 ? area.getWidth() / (float) numWhite : 0.f;
    const float w  = juce::jmax(3.0f, ww * 0.5f - 1.0f);
    const float y  = (float) markStripTop() + (float) (row * (kMarkRowH + kMarkRowGap));
    return { r.getCentreX() - w * 0.5f, y, w, (float) kMarkRowH };
}

// Which note's slot the point is in, or -1. Nearest-centre rather than a hit test on
// the tab rects: the gaps between tabs are dead space the mouse should not fall into,
// and the whole band is one click target per key.
int KeyboardPanel::stripNoteAt(juce::Point<int> p) const
{
    if (! drumPart || drumBuffer < 0) return -1;
    const auto band = markStripBounds();
    // A couple of pixels of slop above and below: the band is 11 px tall and the
    // rows are what the eye aims at, not the band edge.
    if (! band.expanded(0, 2).contains(p)) return -1;

    int best = -1;
    float bestD = 1.0e9f;
    bool black = false;
    for (int n = mu100::kDrumFirstNote; n <= mu100::kDrumLastNote; ++n)
    {
        const float d = std::abs(noteRect(n, black).getCentreX() - (float) p.x);
        if (d < bestD) { bestD = d; best = n; }
    }
    return best;
}

void KeyboardPanel::paintDrumMarkStrip(juce::Graphics& g)
{
    if (! drumPart || drumBuffer < 0) return;

    const auto band = markStripBounds();

    // A dark ground under both rows, so an EMPTY slot reads as "nothing here" rather
    // than as module background that happens to be blank. It is also what makes the
    // strip legible as a single object at a glance.
    g.setColour(edcol::kDrumStripBg);
    g.fillRect(band);
    g.setColour(edcol::kDrumStripBorder);
    g.drawHorizontalLine(band.getY() - 1, (float) band.getX(), (float) band.getRight());

    // The selected key's stem, continuing the red marker above the strip down to the
    // keys so the whole column reads as one axis. UNDER the tabs deliberately: when
    // the key carries a mark the tab is the thing to see and the red tab above is
    // already saying which key is selected, and when it carries none the stem shows
    // through and is the only locator there is.
    if (drumEditActive && drumEditKey >= 0)
    {
        bool bb = false;
        const auto kr = noteRect(drumEditKey, bb);
        g.setColour(juce::Colour(0xffe03030));
        g.fillRect(juce::Rectangle<float>(kr.getCentreX() - 1.f, (float) band.getY(),
                                          2.f, (float) band.getHeight()));
    }

    // Row 0 = changed from THIS KIT'S defaults, in the editor's one "changed" colour
    // (violet in the light theme, not yellow -- an attention colour cannot be frozen
    // across themes). Row 1 = a MIDI override, in the same cyan every override mark
    // in the editor uses.
    for (int row = 0; row < 2; ++row)
    {
        // Row 0 is a CHANGED marker like any other, so Options -> "Show changed markers"
        // hides it with the rest of the class. The STRIP itself stays -- its ground and
        // its height are unconditional (a drum part must not resize the keyboard), and
        // the override row is a different question with its own switches.
        if (row == 0 && ! changedmarks::show()) continue;

        const auto& mask = (row == 0) ? markChanged : markOverridden;
        g.setColour(row == 0 ? edcol::kChanged : edcol::kMidiOverride);
        for (int n = mu100::kDrumFirstNote; n <= mu100::kDrumLastNote; ++n)
            if ((mask[(size_t) (n >> 5)] & (1u << (n & 31))) != 0)
                g.fillRect(markSlot(n, row));
    }
}

// Recompute both masks. Cheap enough to run on the editor's 4 Hz divider (one
// ordered walk of the buffer's block plus the override set), and it repaints only
// when a mask actually moved -- the keyboard is 1526 px wide and this runs whether
// or not anything is happening.
void KeyboardPanel::refreshDrumMarks()
{
    std::array<uint32_t, 4> ch {}, ov {};
    if (drumPart && drumBuffer >= 0)
        audioProcessor.drumKeyMarks(drumBuffer, ch, ov);
    if (ch == markChanged && ov == markOverridden) return;
    markChanged = ch;
    markOverridden = ov;
    repaint(markStripBounds().expanded(2));
}

int KeyboardPanel::noteAt(juce::Point<int> p) const
{
    const auto fp = p.toFloat();
    bool black = false;
    // Black keys sit on top, so test them first.
    for (int n = kFirstNote; n <= kLastNote; ++n)
        if (isBlackNote(n) && noteRect(n, black).contains(fp)) return n;
    for (int n = kFirstNote; n <= kLastNote; ++n)
        if (! isBlackNote(n) && noteRect(n, black).contains(fp)) return n;
    return -1;
}

//==============================================================================
void KeyboardPanel::setCurrentPart(int part0to31)
{
    currentPart = juce::jlimit(0, 31, part0to31);
    // The lit keys belong to the part we are leaving; keeping them would show the
    // new part holding notes it never received until the next timer tick.
    heldMask.fill(0);
    loadFromState(audioProcessor.getEditorState());
}

void KeyboardPanel::setDrumContext(bool isDrumPart, int msb, int lsb, int prg0,
                                   int buffer)
{
    // Nothing to do when the context is the one we already hold. This test is
    // load-bearing, not an optimisation: the drum-edit exit below must fire on a
    // genuine CONTEXT CHANGE (a different part, a different kit) and not merely
    // because someone refreshed the panels.
    //
    // Without it, every caller of EditorView::applyDeviceState() dropped the drum
    // key — and that is a *value* refresh with seven callers, including the 30 Hz
    // editorStateExternalSeq poll. So sitting in drum-edit mode and playing back a
    // MIDI track carrying any XG parameter SysEx, or moving any DAW automation
    // lane, silently deselected the key and snapped the Part module back to part
    // mode. Undo/Redo did not introduce that; it just put it on a button, at a
    // moment the user is watching, which is how it finally got noticed.
    if (drumPart == isDrumPart && kitMsb == msb && kitLsb == lsb && kitPrg0 == prg0
        && drumBuffer == buffer)
    {
        repaint();
        return;
    }

    drumPart = isDrumPart;
    kitMsb = msb; kitLsb = lsb; kitPrg0 = prg0;
    drumBuffer = (buffer >= 0 && buffer < 4) ? buffer : -1;
    // The strip is keyed by the buffer, so a context change invalidates it outright
    // -- including the melodic case, where it must go blank rather than keep the
    // previous part's kit on screen.
    refreshDrumMarks();
    refreshCellProxies();   // the DRUM KEY stop exists only on a kit part
    if (drumEditActive || drumEditKey >= 0)
    {
        drumEditActive = false;     // a context change exits drum-edit mode
        drumEditKey = -1;           // (the timer keeps running — it drives the lamps)
        if (onDrumEditChanged) onDrumEditChanged(false, -1);
    }
    repaint();
}

// True only when a key is selected AND it maps to a named drum voice in the
// current kit. Many notes inherit / are unmapped, so a selected key can still be
// "NO DRUM KEY" — there is then nothing to edit.
bool KeyboardPanel::drumKeyHasVoice() const
{
    if (drumEditKey < 0) return false;
    return mu100::drumVoiceName(kitMsb, kitLsb, kitPrg0 + 1, drumEditKey) != nullptr;
}

// The drum key the editor modules should act on: the selected key if it has a
// voice, else -1. Routing the callback through this makes the Part / Graphic
// Display modules treat a "NO DRUM KEY" note exactly like no selection (their
// drum-only controls gray out and sends stay inert), while this keyboard keeps
// the key highlighted and shows "NO DRUM KEY".
int KeyboardPanel::effectiveDrumKey() const
{
    return drumKeyHasVoice() ? drumEditKey : -1;
}

juce::String KeyboardPanel::drumKeyText() const
{
    if (! drumPart) return {};
    if (! drumKeyHasVoice()) return "NO DRUM KEY";
    return juce::String(mu100::drumVoiceName(kitMsb, kitLsb, kitPrg0 + 1, drumEditKey)).toUpperCase();
}

void KeyboardPanel::setDrumKey(int note)
{
    const int k = juce::jlimit(mu100::kDrumFirstNote, mu100::kDrumLastNote, note);
    if (k == drumEditKey) return;
    drumEditKey = k;
    if (onDrumEditChanged) onDrumEditChanged(drumEditActive, effectiveDrumKey());
    repaint();
}

void KeyboardPanel::applyState(const mu100::DeviceState& st)
{
    loadFromState(st);
}

void KeyboardPanel::loadFromState(const mu100::DeviceState& st)
{
    using mu100::AddressMap;
    auto get = [&st, this](int paramIx, int fallback) -> int
    {
        if (paramIx < 0) return fallback;
        const auto v = AddressMap::get(st, AddressMap::param(paramIx), currentPart);
        return v.present ? v.combined : fallback;
    };
    rangeLow  = get(noteLowParam,  0);
    rangeHigh = get(noteHighParam, 127);
    velLoBox->setValueQuiet((double) get(velLowParam,  1));
    velHiBox->setValueQuiet((double) get(velHighParam, 127));
    repaint();
}

// The current part's raw receive channel (port*16 + ch-1), or -1 when it receives on
// none. Read from the model rather than mirrored, because the value belongs to the Voice
// module's own box and a second copy is a second thing to keep in step.
// Asked of the processor, which is the one place that knows the rule -- and which
// answers with the FACTORY channel where the model holds no byte for the part. This
// used to decide it here and report "no channel" in that case, so the played-note
// display went dark on a part whose Rcv CHANNEL had never been written.
int KeyboardPanel::currentRcvChannel() const
{
    return audioProcessor.partRcvChannelRaw(currentPart);
}

// Poll the processor's held-note set for the current part. All-zero covers every "nothing
// to draw" case -- option off, no receive channel, nothing held -- so the option going off
// clears the keys through the same path that a note-off does.
void KeyboardPanel::updatePlayedNotes()
{
    std::array<uint32_t, 4> now {};
    if (audioProcessor.keyboardPlayedNotes)
    {
        const int raw = currentRcvChannel();
        if (raw >= 0) audioProcessor.heldNotesFor(raw, now.data());
    }
    if (now == heldMask) return;
    heldMask = now;
    repaint(keysArea());
}

void KeyboardPanel::timerCallback()
{
    updateLamps();
    // Deliberately BEFORE the drum-edit return below: the played-note display runs in
    // both modes. In drum-edit mode the two cues then overlap by design -- the green
    // fills say what is sounding, the red marker says which tone is being edited.
    updatePlayedNotes();

    // The rest only applies in drum-edit mode: a played MIDI note picks the key.
    if (! drumEditActive) return;
    const int note = audioProcessor.takeLatestNoteOn(noteOnSeq);
    if (note < 0) return;

    // Auto-suppress MIDI-triggered selection while the DAW transport is playing so
    // edits stay on the chosen key while a track auditions them — unless the user
    // opted to keep it on (sparse-playback case). takeLatestNoteOn() above still
    // consumed the note, so the seq stays current and playback notes don't queue
    // up to jump the selection the instant playback stops. Left-click selection
    // (mouseDown/mouseDrag) never comes through here, so it always works.
    if (audioProcessor.dawIsPlaying.load(std::memory_order_relaxed)
        && ! audioProcessor.midiDrumSelectDuringPlayback)
        return;

    setDrumKey(note);   // clamped to the drum-key range
}

// Drain the processor's no-part activity mask, (re)arm each lamp's hold window,
// advance its fade envelope, and repaint just the lamp row if anything moved.
// Mirrors PartListPanel::timerCallback (shared timing/envelope via midiind).
void KeyboardPanel::updateLamps()
{
    const uint32_t mask = audioProcessor.takeMidiSysActivity();

    if (! audioProcessor.midiInputIndicators)
    {
        // Feature off: make sure the lamps are dark, then idle. (Draining above
        // keeps stale bits from flashing when the option is later re-enabled.)
        bool any = false;
        for (int i = 0; i < kNumLamps; ++i)
        {
            if (lampLevel[(size_t) i] != 0.f) { lampLevel[(size_t) i] = 0.f; any = true; }
            lampHoldUntil[(size_t) i] = 0.0;
        }
        if (any) repaintLamps();
        return;
    }

    const double now = juce::Time::getMillisecondCounterHiRes();
    bool changed = false;
    for (int i = 0; i < kNumLamps; ++i)
    {
        if ((mask & kLamps[i].mask) != 0)
            lampHoldUntil[(size_t) i] = now + midiind::kHoldMs;
        changed |= midiind::advance(lampLevel[(size_t) i],
                                    now < lampHoldUntil[(size_t) i]);
    }
    if (changed) repaintLamps();
}

void KeyboardPanel::repaintLamps()
{
    for (int i = 0; i < kNumLamps; ++i)
        repaint(lampOutline[(size_t) i]);
}

void KeyboardPanel::sendRange()
{
    using mu100::AddressMap;
    if (noteLowParam >= 0)
        audioProcessor.sendParamChange(AddressMap::makeParamChange(
            AddressMap::param(noteLowParam), currentPart, (uint32_t) rangeLow));
    if (noteHighParam >= 0)
        audioProcessor.sendParamChange(AddressMap::makeParamChange(
            AddressMap::param(noteHighParam), currentPart, (uint32_t) rangeHigh));
}

void KeyboardPanel::startAudition(int note)
{
    if (note < 0) return;
    auditionNote = note;
    const int vel = juce::jlimit(1, 127, (int) velHiBox->getValue());
    audioProcessor.sendChannelMessage(currentPart, 0x90, (uint8_t) note, vel);
    repaint();
}

void KeyboardPanel::stopAudition()
{
    if (auditionNote < 0) return;
    audioProcessor.sendChannelMessage(currentPart, 0x80, (uint8_t) auditionNote, 0);
    auditionNote = -1;
    repaint();
}

//==============================================================================
void KeyboardPanel::resized()
{
    auto right = getLocalBounds().removeFromRight(rightPanelW()).reduced(4);
    // Centre the drum-key box + velocity panel between the keys' right edge and the
    // module's right edge (the keys leave a one-key margin on their right, so the
    // panel would otherwise sit hard against the module edge).
    right.translate(-(keyMargin() / 2 + 1), 0);
    // Top: DRUM KEY NAME box. Bottom: velocity-limit panel.
    drumBoxRect = right.removeFromTop(juce::jmin(26, right.getHeight() / 3));
    right.removeFromTop(4);

    // Velocity-limit panel: the two value boxes with LO/HI labels just below them,
    // nudged up so the box tops sit near the old label midline. The VELOCITY LIMIT
    // caption follows a short gap under the labels (it used to sit midway down the
    // panel; the space it vacated now holds the SYS/FX/BULK lamp row).
    auto vel = right;
    const int boxH   = 18;
    const int labelH = 12;
    const int boxTop = vel.getY() + 6;

    auto boxRow = juce::Rectangle<int>(vel.getX(), boxTop, vel.getWidth(), boxH);
    const int halfB = boxRow.getWidth() / 2;
    velLoBox->setBounds(boxRow.removeFromLeft(halfB).reduced(2, 0));
    velHiBox->setBounds(boxRow.reduced(2, 0));

    auto labelRow = juce::Rectangle<int>(vel.getX(), boxTop + boxH, vel.getWidth(), labelH);
    const int halfL = labelRow.getWidth() / 2;
    velLoLabel = labelRow.removeFromLeft(halfL);
    velHiLabel = labelRow;

    const int labelsBottom = boxTop + boxH + labelH;
    const int capH   = 14;
    const int capGap = 3;
    velCaption = juce::Rectangle<int>(vel.getX(), labelsBottom + capGap, vel.getWidth(), capH);

    layOutLamps(vel);
    refreshCellProxies();   // the DRUM KEY box's stop moves with the layout
}

// SYS / FX / BULK lamp row: [LABEL][gap][framed lamp] per group, bottom-aligned
// with the bottom of the keys. The row spans from the velocity panel's left edge
// to the DRUM KEY box's right edge — BULK's frame lines up with that box — with
// equal gaps between the three groups. Label widths are MEASURED from the real
// font: edtext scales fonts but not cells, so a hardcoded width would collide as
// editorTextLabelsPct rises.
void KeyboardPanel::layOutLamps(juce::Rectangle<int> vel)
{
    const juce::Font f (lampFont());
    const int frameW = midiind::kW + 2 * kLampBorder;   // lamp + its frame
    const int frameH = kLampH      + 2 * kLampBorder;
    const int rowH   = juce::jmax(frameH, (int) std::ceil(f.getHeight()));
    const int bottom = keysArea().getBottom();
    const int top    = juce::jmax(vel.getY(), bottom - rowH);

    std::array<int, kNumLamps> groupW {};
    int sumW = 0;
    for (int i = 0; i < kNumLamps; ++i)
    {
        groupW[(size_t) i] = juce::GlyphArrangement::getStringWidthInt(f, kLamps[i].label)
                           + kLampLabelGap + frameW;
        sumW += groupW[(size_t) i];
    }

    // Spread the leftover evenly between the groups. The odd remainder pixel goes
    // to the leftmost gap(s), so the last group still ends exactly on `right` and
    // BULK's frame can't drift off the DRUM KEY box by a rounding pixel.
    const int left    = vel.getX();
    const int right   = drumBoxRect.getRight();
    const int gaps    = kNumLamps - 1;
    const int slack   = (right - left) - sumW;
    const int spacing = juce::jmax(2, gaps > 0 ? slack / gaps : 0);
    const int extra   = juce::jmax(0, slack - spacing * gaps);

    int x = left;
    for (int i = 0; i < kNumLamps; ++i)
    {
        lampGroup  [(size_t) i] = { x, top, groupW[(size_t) i], rowH };
        lampOutline[(size_t) i] = { x + groupW[(size_t) i] - frameW,
                                    top + (rowH - frameH) / 2, frameW, frameH };
        lampRect   [(size_t) i] = lampOutline[(size_t) i].reduced(kLampBorder);
        x += groupW[(size_t) i] + spacing + (i < extra ? 1 : 0);
    }
}

void KeyboardPanel::paint(juce::Graphics& g)
{
    g.setColour(edcol::kModuleBg);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.f);
    g.setColour(edcol::kModuleBorder);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.f, 1.f);

    const auto area = keysArea();
    bool black = false;

    // White keys.
    for (int n = kFirstNote; n <= kLastNote; ++n)
    {
        if (isBlackNote(n)) continue;
        const auto r = noteRect(n, black);
        g.setColour(juce::Colour(0xfff2f2ee));
        g.fillRect(r.reduced(0.5f));
        g.setColour(juce::Colour(0xff707070));
        g.drawRect(r, 0.7f);
    }
    // Black keys on top.
    for (int n = kFirstNote; n <= kLastNote; ++n)
    {
        if (! isBlackNote(n)) continue;
        const auto r = noteRect(n, black);
        g.setColour(juce::Colour(0xff1a1a1a));
        g.fillRect(r);
        g.setColour(juce::Colour(0xff000000));
        g.drawRect(r, 0.7f);
    }

    // Middle-C marker (green dot near the bottom of the C3 white key).
    {
        const auto r = noteRect(kMiddleC, black);
        const float d = juce::jmin(7.f, r.getWidth() * 0.6f);
        g.setColour(juce::Colour(0xff35c24a));
        g.fillEllipse(r.getCentreX() - d * 0.5f, r.getBottom() - d - 3.f, d, d);
    }

    // Keys that are SOUNDING: those held by incoming MIDI on this part's Rcv channel
    // ("Played notes on the keyboard"), plus the one a right-drag is auditioning. One
    // routine and one colour for both, because the cue means "this key is sounding" and
    // which of the two started it is not something the eye needs told.
    //
    // Drawn in TWO passes with the black keys re-laid between them, and that is not
    // tidiness. A white key's rect is full width and full height (see noteRect), so it
    // runs UNDER the black keys straddling its edges — fill it in one pass with the black
    // keys and a lit C plus a lit D paint right over the C# between them, which reads as
    // three notes held instead of two. Invisible while only ONE key could ever be lit,
    // which is why it survived until the played-note display made chords possible.
    auto isLit = [this](int note)
    {
        return note == auditionNote
            || (heldMask[(size_t) (note >> 5)] & (1u << (note & 31))) != 0;
    };
    auto litKey = [&](int note)
    {
        bool nb = false;
        const auto nr = noteRect(note, nb);
        g.setColour(juce::Colour(0xff35c24a).withAlpha(nb ? 0.85f : 0.55f));
        g.fillRect(nr.reduced(0.5f));
        g.setColour(juce::Colour(0xff2a8a3a));
        g.drawRect(nr, 1.0f);
    };

    bool anyWhiteLit = false;
    for (int n = kFirstNote; n <= kLastNote; ++n)
        if (! isBlackNote(n) && isLit(n)) { litKey(n); anyWhiteLit = true; }

    if (anyWhiteLit)                       // restore the black keys the fills ran under
        for (int n = kFirstNote; n <= kLastNote; ++n)
        {
            if (! isBlackNote(n)) continue;
            const auto r = noteRect(n, black);
            g.setColour(juce::Colour(0xff1a1a1a));
            g.fillRect(r);
            g.setColour(juce::Colour(0xff000000));
            g.drawRect(r, 0.7f);
        }

    for (int n = kFirstNote; n <= kLastNote; ++n)
        if (isBlackNote(n) && isLit(n)) litKey(n);

    // Above the keys: the play-range bar normally, or a single-key marker while
    // selecting a drum tone in drum-edit mode. Its y is taken from the marker strip
    // rather than from the keys, so the bar keeps its place when the strip's height
    // changes and the two never overlap.
    const float barY = (float) (markStripTop() - kMarkTopGap - 5);
    if (drumEditActive)
    {
        if (drumEditKey >= 0)
        {
            bool bb = false;
            const auto kr = noteRect(drumEditKey, bb);
            g.setColour(juce::Colour(0xffe03030));
            g.fillRect(juce::Rectangle<float>(kr.getX(), barY,
                                              juce::jmax(3.f, kr.getWidth()), 4.f));
            g.fillRect(juce::Rectangle<float>(kr.getCentreX() - 1.f, (float) area.getY(),
                                              2.f, (float) area.getHeight()));
        }
    }
    else
    {
        bool b1 = false, b2 = false;
        const auto lo = noteRect(rangeLow,  b1);
        const auto hi = noteRect(rangeHigh, b2);
        const float x1 = lo.getX();
        const float x2 = hi.getRight();
        g.setColour(juce::Colour(0xffe03030));
        g.fillRect(juce::Rectangle<float>(x1, barY, juce::jmax(2.f, x2 - x1), 4.f));
    }

    // ...and between that bar and the keys, the per-key marker strip.
    paintDrumMarkStrip(g);

    // Octave labels under each C.
    g.setColour(edcol::kTextDim);
    g.setFont(edtext::font(edtext::Label, 13.f));
    for (int n = kFirstNote; n <= kLastNote; ++n)
    {
        if (n % 12 != 0) continue;        // C of each octave
        const auto r = noteRect(n, black);
        // Centre a wider box on the C key so "C-2".."C8" fit at this font size
        // (a single white-key width truncated them to dashes). A note NUMBER is
        // never wider than three digits, so the same box holds both forms.
        //
        // Still only the Cs when the labels are numbers (Options > Label notes as).
        // The C is the landmark you count from whichever way it is written, and
        // numbering all 128 keys would be noise rather than information.
        const int lblW = 46;
        g.drawText(notelabel::text(n),
                   juce::Rectangle<int>((int) r.getCentreX() - lblW / 2, area.getBottom(),
                                        lblW, 17),
                   juce::Justification::centredTop);
    }

    // DRUM KEY NAME box (top-right). The frame is always drawn; the text shows
    // only for an editable-drum part ("NO DRUM KEY" until a key is selected).
    {
        const auto r = drumBoxRect.toFloat();
        g.setColour(drumEditActive ? edcol::kDrumStripActive : edcol::kDrumStripBg);
        g.fillRect(r);
        g.setColour(edcol::kDrumStripBorder);
        g.drawRect(r, 1.f);
        if (drumPart)
        {
            g.setColour(drumEditActive ? edcol::kDrumKeyArmed : edcol::kDrumKeyText);
            g.setFont(edtext::font(edtext::Value, 11.f, juce::Font::bold));
            g.drawText(drumKeyText(), drumBoxRect, juce::Justification::centred);
        }
    }

    // Velocity-limit labels + caption.
    g.setColour(edcol::kTextDim);
    g.setFont(edtext::font(edtext::Label, 10.f, juce::Font::bold));
    g.drawText("LO", velLoLabel, juce::Justification::centred);
    g.drawText("HI", velHiLabel, juce::Justification::centred);
    g.setFont(edtext::font(edtext::Label, 11.f, juce::Font::bold));
    g.drawText("VELOCITY LIMIT", velCaption, juce::Justification::centred);

    // SYS / FX / BULK lamps: label, then an always-drawn frame (same 1px/colour as
    // the LO/HI value boxes) holding an orange tab whose opacity is that lamp's
    // fade level (see timerCallback). Same tab width/colour/envelope as the Voice
    // module's per-part indicators, and the same on/off option.
    g.setFont(lampFont());
    for (int i = 0; i < kNumLamps; ++i)
    {
        auto label = lampGroup[(size_t) i];
        label.setWidth(label.getWidth() - lampOutline[(size_t) i].getWidth() - kLampLabelGap);
        g.setColour(edcol::kTextDim);
        g.drawText(kLamps[i].label, label, juce::Justification::centredRight);

        g.setColour(kLampFrame);
        g.drawRect(lampOutline[(size_t) i], kLampBorder);

        const float lvl = lampLevel[(size_t) i];
        if (audioProcessor.midiInputIndicators && midiind::visible(lvl))
        {
            g.setColour(midiind::colour().withAlpha(juce::jlimit(0.f, 1.f, lvl)));
            g.fillRect(lampRect[(size_t) i]);
        }
    }
}

//==============================================================================
void KeyboardPanel::mouseDown(const juce::MouseEvent& e)
{
    // The DRUM KEY NAME box toggles drum-edit mode (drum parts only).
    if (drumPart && e.mods.isLeftButtonDown() && drumBoxRect.contains(e.getPosition()))
    {
        toggleDrumEditMode();
        return;
    }

    // A left click on the marker strip picks that drum key, entering drum-edit mode
    // if it is not already on. The strip exists to say "look here", so the obvious
    // gesture has to be the one that takes you there -- and the slot is over its own
    // key, so it reads as clicking the key.
    if (e.mods.isLeftButtonDown())
    {
        const int stripNote = stripNoteAt(e.getPosition());
        if (stripNote >= 0)
        {
            if (! drumEditActive) toggleDrumEditMode();
            setDrumKey(stripNote);
            return;
        }
    }

    const int note = noteAt(e.getPosition());
    if (note < 0) return;

    if (e.mods.isRightButtonDown())
    {
        startAudition(note);                 // audition the current part
        return;
    }

    if (drumEditActive)                      // left click/drag selects the drum key
    {
        setDrumKey(note);
        return;
    }

    // Left: move whichever range edge the click is nearer. One gesture for the whole
    // drag, so reversing direction or pausing part-way through still leaves one step
    // that reverts to the range you started from.
    dragBoundary = (note <= (rangeLow + rangeHigh) / 2) ? 0 : 1;
    beginRangeUndo();
    mouseDrag(e);
}

// See rangeUndoOpen in the header for why a range drag needs this and a knob does not.
void KeyboardPanel::beginRangeUndo()
{
    endRangeUndo();                  // a drag left open by anything else
    audioProcessor.undoGestureBegin("Part " + juce::String(currentPart + 1)
                                    + " note range");
    rangeUndoOpen = true;
}

void KeyboardPanel::endRangeUndo()
{
    if (! rangeUndoOpen) return;
    rangeUndoOpen = false;
    audioProcessor.undoGestureEnd();
}

void KeyboardPanel::mouseDrag(const juce::MouseEvent& e)
{
    const int note = noteAt(e.getPosition());
    if (note < 0) return;

    if (auditionNote >= 0)                   // right-drag glissando
    {
        if (note != auditionNote) { stopAudition(); startAudition(note); }
        return;
    }

    if (drumEditActive)                      // left-drag scrubs the drum key
    {
        setDrumKey(note);
        return;
    }

    if (dragBoundary == 0)                   // lower bound
    {
        const int v = juce::jlimit(0, rangeHigh, note);
        if (v != rangeLow) { rangeLow = v; sendRange(); repaint(); }
    }
    else if (dragBoundary == 1)              // upper bound
    {
        const int v = juce::jlimit(rangeLow, 127, note);
        if (v != rangeHigh) { rangeHigh = v; sendRange(); repaint(); }
    }
}

void KeyboardPanel::mouseUp(const juce::MouseEvent&)
{
    stopAudition();
    endRangeUndo();
    dragBoundary = -1;
}

// Arm / disarm drum-edit mode. Extracted from mouseDown so the click on the DRUM KEY
// box and the keyboard's Enter on the same cell are one code path (see navItems).
void KeyboardPanel::toggleDrumEditMode()
{
    if (! drumPart) return;

    drumEditActive = ! drumEditActive;
    drumEditKey = -1;                    // mode change starts with no key chosen
    // The timer runs continuously for the lamps; entering the mode only needs the
    // pre-existing note discarded so it can't jump the selection.
    if (drumEditActive)
        audioProcessor.takeLatestNoteOn(noteOnSeq);
    if (onDrumEditChanged) onDrumEditChanged(drumEditActive, effectiveDrumKey());
    repaint();
}

//==============================================================================
// Keyboard navigation (KeyNav.h).
//
// Only two of this module's editable things are components — the velocity limits —
// so the generic collector would find them and silently stop there. The DRUM KEY box
// is painted, and it is the gate on the whole drum-editing half of the editor: with
// no key armed every Part-module edit is dropped, so leaving it unreachable would
// make the keyboard route dead-end exactly where a user needs it most.
//
// The keys themselves (the note range, and picking a drum key by clicking one) stay
// mouse-only for now; they are painted drag surfaces, like the Graphic Display's
// plot handles, and belong with those in a second pass.
std::vector<keynav::Item> KeyboardPanel::navItems()
{
    std::vector<keynav::Item> items;

    // Hidden = out of the accessibility tree and unfocusable, which is what a stop that
    // is not offered on this part needs. Set ONCE, never hidden-then-shown: hiding a
    // focused component makes JUCE give the focus away, and this runs on every focus
    // change (see PartListPanel::navItems).
    if (! drumPart) drumProxy.setVisible(false);

    if (drumPart)
    {
        keynav::Item it;
        it.comp     = this;
        it.painted  = true;
        it.cell     = drumBoxRect;
        // Accessibility pillar 3. The box is a toggle (drum-edit armed or not) whose
        // VALUE is the drum it has landed on -- which is the thing a user needs read
        // back, and the same string the box paints and the Graphic Display's corner
        // label shows.
        if (drumProxy.title == nullptr)
        {
            drumProxy.setRole(juce::AccessibilityRole::toggleButton);
            drumProxy.title      = [] { return juce::String(mu100::tooltips::forKey("KBD|DRUM EDIT KEY")); };
            // The selected key's own override rides the VALUE, as it does on every
            // other control: the key's cyan mark says incoming MIDI has moved THIS
            // drum on the device, and a mark on a painted key says nothing at all
            // without sight.
            drumProxy.value      = [this]
            {
                auto v = drumKeyText();
                if (drumEditKey >= 0 && drumEditKey < 128
                    && (markOverridden[(size_t) (drumEditKey >> 5)] >> (drumEditKey & 31)) & 1u)
                    v += ", MIDI has changed this key on the device";
                return v;
            };
            // THE MARKER STRIP IN WORDS. The strip above the keyboard is the only
            // place the kit's other keys are summarised, and it is pure paint -- so
            // this is the whole of it for a screen reader: how many keys of the kit
            // have been edited, and how many the device has been moved off. Counts,
            // not names: 61 keys read out is not a summary. Description rather than
            // value, so it is there when asked and never spoken on every move.
            drumProxy.description = [this]
            {
                auto count = [](const std::array<uint32_t, 4>& m)
                {
                    int n = 0;
                    for (auto w : m) for (; w != 0; w &= w - 1) ++n;
                    return n;
                };
                const int ch = count(markChanged), ov = count(markOverridden);
                juce::StringArray out;
                if (ch > 0) out.add(juce::String(ch) + (ch == 1 ? " key in this kit is edited"
                                                                : " keys in this kit are edited"));
                if (ov > 0) out.add(juce::String(ov) + (ov == 1 ? " key has been changed by MIDI"
                                                                : " keys have been changed by MIDI"));
                return out.joinIntoString(", ");
            };
            drumProxy.checked    = [this] { return drumEditActive; };
            drumProxy.onActivate = [this] { toggleDrumEditMode(); };
            addChildComponent(drumProxy);
        }
        drumProxy.setBounds(drumBoxRect);
        drumProxy.setVisible(true);
        it.a11yProxy = &drumProxy;
        it.activate = [this] { toggleDrumEditMode(); };
        // Up arms the mode, down disarms it — the toggle law, same as a DotToggle.
        it.adjust   = [this](int dir, bool)
        {
            if (dir != 0 && drumEditActive != (dir > 0)) toggleDrumEditMode();
        };
        it.reset = [] {};
        items.push_back(std::move(it));
    }

    // The two ends of the NOTE RANGE bar. Offered only outside drum-edit mode, which
    // is exactly when the mouse drags them: in drum-edit mode a press on the keys picks
    // a drum key instead, and the bar is not drawn at all.
    const bool rangeStops = ! drumEditActive;
    if (! rangeStops)
        for (auto& px : rangeProxy) px.setVisible(false);

    if (rangeStops)
    {
        const auto area = keysArea();
        bool blk = false;
        const auto loR = noteRect(rangeLow,  blk);
        const auto hiR = noteRect(rangeHigh, blk);

        // THE MARK GOES ON THE BAR, NOT ON THE KEY, and that is a legibility
        // requirement rather than a preference: the piano keys are among the few
        // things in this editor drawn the SAME in both themes, so a white key stays
        // white under the dark theme -- where edcol::kNavFocus is also white. A ring
        // around a white key would be invisible in the default theme. The 6 px strip
        // above the keys (keysArea's withTrimmedTop) is module background, the red
        // range bar already lives there, and a mark on the bar's end says which end
        // the keys will move.
        const int barY = markStripTop() - kMarkTopGap - 6;
        auto endCell = [barY](float x)
        {
            return juce::Rectangle<int>((int) x - 7, barY, 14, 6);
        };

        struct End { int idx; float x; const char* param; };
        const End ends[2] = { { 0, loR.getX(),     "NOTE LIMIT LOW"  },
                              { 1, hiR.getRight(), "NOTE LIMIT HIGH" } };

        for (const auto& en : ends)
        {
            auto& px = rangeProxy[en.idx];
            const int idx = en.idx;

            px.setRole(juce::AccessibilityRole::slider);
            px.title = [param = en.param]
            { return juce::String(mu100::tooltips::forParam("MULTI PART", param)); };
            // Spoken as the NOTE, which is what the control is about and what the
            // octave labels under the keys read in.
            px.value = [this, idx] { return rangeNoteName(idx == 0 ? rangeLow : rangeHigh); };
            px.onActivate = nullptr;
            if (px.getParentComponent() == nullptr) addChildComponent(px);
            px.setBounds(endCell(en.x));
            px.setVisible(true);
            px.invalidateAccessibilityHandler();

            keynav::Item it;
            it.comp      = this;
            it.painted   = true;
            it.cell      = endCell(en.x);
            it.a11yProxy = &px;
            // Up/down AND `.` / `,` both move it: the parameter is a single note, and
            // both readings are natural here -- higher is up, and it is also to the
            // right. Same choice the AMP EG nodes make.
            it.adjust  = [this, idx](int dir, bool fine) { nudgeRange(idx, dir, fine); };
            it.adjustX = [this, idx](int dir, bool fine) { nudgeRange(idx, dir, fine); };
            // Back to the full 0..127 span, which is the factory default for both. The
            // mouse has no equivalent -- a right-drag over the keys AUDITIONS them --
            // so this is the one place the two do not mirror each other.
            it.reset   = [this, idx] { setRangeEnd(idx, idx == 0 ? 0 : 127); };
            it.activate = [] {};
            items.push_back(std::move(it));
        }
    }

    if (velLoBox != nullptr) items.push_back(keynav::Item { velLoBox.get() });
    if (velHiBox != nullptr) items.push_back(keynav::Item { velHiBox.get() });

    return items;
}

// One end of the note range, by note number, clamped against the other end exactly as
// the drag clamps it.
void KeyboardPanel::setRangeEnd(int which, int note)
{
    const int v = (which == 0) ? juce::jlimit(0, rangeHigh, note)
                               : juce::jlimit(rangeLow, 127, note);
    int& target = (which == 0) ? rangeLow : rangeHigh;
    if (v == target) return;

    // ONE undo step per press. sendRange() writes BOTH note limits every time (see its
    // note in the header), so the address-keyed merge cannot fire and two bare sends
    // would be two steps -- which is also why the mouse DRAG opens a gesture.
    YamahaMU100AudioProcessor::UndoScope undoStep(
        audioProcessor, "Part " + juce::String(currentPart + 1) + " note range");
    target = v;
    sendRange();
    repaint();
}

void KeyboardPanel::nudgeRange(int which, int dir, bool fine)
{
    if (dir == 0) return;
    const int cur = (which == 0) ? rangeLow : rangeHigh;
    setRangeEnd(which, cur + dir * (fine ? 1 : kRangeCoarse));
}

// C3 = middle C, the convention the octave labels under the keys already use.
// The key-range ends, as a screen reader reads them and as the drum marker strip's
// tooltip names a key. Through notelabel rather than JUCE's own formatter so it
// follows Options > Label notes as -- and so the octave convention has ONE source
// (mu100::display::noteName), rather than this one asking JUCE for middle-C-is-3 and
// the rest of the editor asking the codec for the same thing.
juce::String KeyboardPanel::rangeNoteName(int note) const
{
    return notelabel::text(note);
}
